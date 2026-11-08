#!/usr/bin/env python3
"""SpinPi: the Raspberry Pi side of a SpinSlicer CAL printer.

* web page on http://<pi>/ : drop a SpinSlicer `.spin` file (or a plain video) from a phone or PC, press Print
* SpinSlicer finds the printer by mDNS (_spinprinter._tcp), reads its profile (GET /api/printer),
  uploads `.spin` files (PUT /api/files/<name>.spin) and reads the print log (GET /api/history)
* USB stick: plug it into the Pi, its videos appear on the printer screen and the web page
* the projector shows the frame that belongs to the vial's angle (SpinProj, C program): the ESP
  counts every motor step, SpinPi forwards the angle 20 times a second, SpinProj puts up the
  right frame on every vsync. Nothing can drift. (--engine mpv: the old way, video by time,
  mpv's speed nudged to follow the motor)
* talks to the SpinScreen ESP32 over USB: video list, print settings, progress, the QR code
* projector calibration: centering offset (applied to every print) and vial width

Only needs: python3, python3-serial, ffmpeg (ffprobe), spinproj (or mpv). See install.sh.
"""
import argparse
import glob
import hashlib
import json
import os
import queue
import re
import socket
import subprocess
import threading
import time
import urllib.parse
import uuid
import zipfile
from xml.sax.saxutils import escape as xml_escape
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import serial  # python3-serial
except ImportError:  # still serve the web page without the ESP
    serial = None

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = "0.6.0"
VIDEO_EXT = (".mp4", ".mkv", ".mov", ".avi", ".webm")
SPIN_EXT = ".spin"  # SpinSlicer's one-file print: a stored ZIP with spin.json, video.mp4, preview.png
PRINT_EXT = VIDEO_EXT + (SPIN_EXT,)
SPIN_VERSION = 1  # the newest .spin version this SpinPi reads
API_VERSION = 1
ROTATIONS = ("ccw_from_top", "cw_from_top")


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def load_json(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def save_json(path, obj, **kw):
    """Write to a temp file, fsync, rename: the power can go at any moment and the old file stays whole."""
    tmp = f"{path}.{threading.get_ident()}.tmp"  # one per thread: two writers never share it
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, **kw)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


# =============================================================================== ESP32 link
class Lines:
    """Bytes from the serial port -> whole lines. A read can stop in the middle of a line (USB
    hands over what it has); the rest waits here for its newline instead of being parsed as two
    broken lines. A line that never ends (noise) is dropped rather than growing forever."""

    MAX = 1024

    def __init__(self):
        self.buf = bytearray()
        self.junk = False

    def feed(self, data):
        out = []
        for b in data:
            if b == 0x0A:
                if not self.junk:
                    line = self.buf.decode("utf-8", "replace").strip()
                    if line:
                        out.append(line)
                self.buf.clear()
                self.junk = False
            elif len(self.buf) < self.MAX:
                self.buf.append(b)
            else:
                self.junk = True
        return out


class Esp:
    """Line protocol over the USB cable (see SpinScreen README). Every ESP port is tried in turn
    until one answers PING, and if the ESP goes quiet the search starts again."""

    SILENT_S = 3.0  # no PONG for this long: not (or no longer) the ESP's port

    def __init__(self, port_hint, on_event):
        self.port_hint = port_hint
        self.on_event = on_event
        self.ser = None
        self.lock = threading.Lock()
        self.pos_cv = threading.Condition()
        self.pos = None
        self.connected = False
        self.version = ""
        self.greeting = []  # lines re-sent whenever the ESP (re)connects
        self.port = ""
        self._next = 0  # which candidate to try next
        self.last_pong = 0.0
        threading.Thread(target=self._run, daemon=True).start()

    def _candidates(self):
        if self.port_hint:  # --serial: one port, or several separated by commas
            return [p for p in self.port_hint.split(",") if p]
        # USB only: the Pi's UART (/dev/serial0) never has the ESP on it, and trying it after a
        # quiet moment used to cost a running print 3 s of silence
        usb = sorted(glob.glob("/dev/serial/by-id/*Espressif*"))
        real = {os.path.realpath(p) for p in usb}
        return usb + [p for p in sorted(glob.glob("/dev/ttyACM*")) if os.path.realpath(p) not in real]

    def _find_port(self):
        cands = self._candidates()
        if not cands:
            return None
        port = cands[self._next % len(cands)]
        self._next += 1
        return port

    def _drop(self, why):
        log("ESP:", why)
        try:
            self.ser.close()
        except Exception:  # noqa: BLE001
            pass
        self.ser = None
        self.connected = False

    def _run(self):
        last_ping = 0
        while True:
            if self.ser is None:
                port = self._find_port() if serial else None
                if not port:
                    self.connected = False
                    time.sleep(2)
                    continue
                try:
                    self.ser = serial.Serial(port, 115200, timeout=0.05)
                    self.ser.reset_input_buffer()
                    lines = Lines()
                    self.port, opened = port, time.monotonic()
                    self.last_pong = 0.0
                    self.hello()
                except Exception as e:  # noqa: BLE001
                    log("ESP: cannot open", port, e)
                    self.ser = None
                    time.sleep(1)
                    continue
            try:
                if time.time() - last_ping > 1:
                    self.send("PING")
                    last_ping = time.time()
                for line in lines.feed(self.ser.read(max(1, self.ser.in_waiting))):
                    self.on_line(line)
                quiet = time.monotonic() - max(self.last_pong, opened)
                if quiet > self.SILENT_S:  # nobody answers PING here: try the next port
                    if self.connected:
                        self._drop(f"silent on {self.port}, looking again")
                    else:
                        self._drop_quiet()
            except Exception as e:  # noqa: BLE001
                self._drop(f"lost ({e})")
                time.sleep(1)

    def _drop_quiet(self):
        try:
            self.ser.close()
        except Exception:  # noqa: BLE001
            pass
        self.ser = None
        if len(self._candidates()) <= 1:
            time.sleep(1)  # the only port: keep knocking, but not in a tight loop

    def hello(self):
        for line in list(self.greeting):
            self.send(line)

    def on_line(self, line):
        """One whole line. A line that does not parse is logged and skipped: the link stays up."""
        try:
            self._handle(line)
        except (ValueError, IndexError) as e:
            log("ESP: bad line", repr(line[:80]), e)

    def _handle(self, line):
        if line.startswith("POS "):
            p = line.split()
            with self.pos_cv:
                self.pos = (float(p[1]), float(p[2]), p[3] == "1", time.monotonic())
                self.pos_cv.notify_all()
        elif line.startswith("PONG"):
            if not self.connected:
                log("ESP:", line, "on", self.port)
                self.hello()  # the greeting may have gone to a port that was not listening yet
            self.connected = True
            self.last_pong = time.monotonic()
            self.version = line.split()[-1]
        elif line.startswith("READY"):  # ESP rebooted
            self.hello()
        elif line.startswith("EV "):
            threading.Thread(target=self.on_event, args=(line[3:],), daemon=True).start()

    def send(self, line):
        s = self.ser
        if s is None:
            return False
        with self.lock:
            try:
                s.write((line + "\n").encode("utf-8"))
                return True
            except Exception:  # noqa: BLE001
                return False

    def query_pos(self, timeout=0.15):
        """(angle_deg, rpm, at_speed, t_mid, rtt) where t_mid = middle of the round trip."""
        with self.pos_cv:
            self.pos = None
            t0 = time.monotonic()
            if not self.send("POS"):
                return None
            self.pos_cv.wait_for(lambda: self.pos is not None, timeout)
            if self.pos is None:
                return None
            a, r, ok, t1 = self.pos
            return a, r, ok, (t0 + t1) / 2, t1 - t0


# =============================================================================== mpv (projector)
class Projector:
    """One mpv instance for the whole life of the service: idle = black full screen."""

    engine = "mpv"
    # tried in order until mpv stays up (Raspberry Pi OS Lite has no desktop: mpv draws via DRM)
    FALLBACKS = [[], ["--vo=gpu", "--gpu-context=drm"], ["--vo=drm"]]

    def __init__(self, sock_path, extra_args, width):
        self.sock_path = sock_path
        self.extra = extra_args
        self.width = width
        self.proc = None
        self.lock = threading.Lock()
        self.req = 0
        self.ok = False
        self.err = ""
        self.start()

    def start(self):
        for extra in ([[]] if self.extra else self.FALLBACKS):
            if self._try(extra):
                return

    def _try(self, fallback):
        try:
            os.unlink(self.sock_path)
        except FileNotFoundError:
            pass
        cmd = ["mpv", "--idle=yes", "--force-window=yes", "--fs", "--no-audio", "--no-osc", "--osd-level=0",
               "--no-input-default-bindings", "--cursor-autohide=always", "--keep-open=no", "--hwdec=auto-safe",
               f"--input-ipc-server={self.sock_path}"] + fallback + self.extra
        try:
            self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        except FileNotFoundError:
            self.err = "mpv is not installed"
            log("projector:", self.err)
            return False
        for _ in range(60):
            if os.path.exists(self.sock_path):
                time.sleep(0.6)  # still alive after opening the output?
                if self.proc.poll() is None:
                    self.ok, self.err = True, ""
                    log("projector: mpv running", " ".join(fallback + self.extra))
                    return True
                break
            if self.proc.poll() is not None:
                break
            time.sleep(0.05)
        self.err = ((self.proc.stderr.read() or b"").decode(errors="replace")[-400:]
                    if self.proc.poll() is not None else "mpv did not start")
        log("projector: mpv failed", fallback, self.err.strip()[-200:])
        try:
            self.proc.kill()
        except Exception:  # noqa: BLE001
            pass
        return False

    def cmd(self, *args, timeout=1.0):
        with self.lock:
            if self.proc is None or self.proc.poll() is not None:
                self.ok = False
                self.start()
            self.req += 1
            rid = self.req
            msg = json.dumps({"command": list(args), "request_id": rid}) + "\n"
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(timeout)
                s.connect(self.sock_path)
                s.sendall(msg.encode())
                buf = b""
                while True:
                    chunk = s.recv(65536)
                    if not chunk:
                        return None
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        try:
                            d = json.loads(line)
                        except ValueError:
                            continue
                        if d.get("request_id") == rid:
                            s.close()
                            return d
            except OSError:
                return None

    def get(self, prop):
        d = self.cmd("get_property", prop)
        return d.get("data") if d and d.get("error") == "success" else None

    def set(self, prop, value):
        return self.cmd("set_property", prop, value)

    def play_loop(self, path, speed, offset_px):
        self.cmd("set", "loop-file", "inf")
        self.set("video-pan-x", offset_px / float(self.width))
        self.set("speed", speed)
        self.set("pause", False)
        return self.cmd("loadfile", path, "replace")

    def show_image(self, path):
        self.cmd("set", "loop-file", "no")
        self.cmd("set", "image-display-duration", "inf")  # "set" parses the string like a command-line option
        self.set("video-pan-x", 0.0)
        self.cmd("loadfile", path, "replace")

    def black(self):
        self.cmd("stop")


# =============================================================================== SpinProj (projector by angle)
class AngleProjector:
    """SpinProj runs for the whole life of the service; we talk to it over its stdin/stdout.

    Black whenever nothing else is asked for. If SpinPi dies, SpinProj sees its stdin close and
    goes black; if the angle stops coming for 0.3 s while the light is on, it goes black by itself.
    """

    engine = "angle"

    def __init__(self, exe, args):
        self.cmdline = [exe] + list(args)
        self.proc = None
        self.lock = threading.Lock()
        self.ok = False
        self.err = ""
        self.hz = None
        self.size = None  # (w, h) of the projector, from READY
        self.events = queue.Queue()  # (word, [fields]) for the print thread
        self.stat = None
        self.start()

    def start(self):
        try:
            self.proc = subprocess.Popen(self.cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                         bufsize=1, encoding="utf-8", errors="replace")
        except OSError as e:
            self.ok, self.err = False, f"spinproj: {e}"
            log("projector:", self.err)
            return
        ready = threading.Event()
        threading.Thread(target=self._read, args=(self.proc, ready), daemon=True).start()
        if not ready.wait(5):
            log("projector: spinproj did not say READY", self.err)

    def _read(self, proc, ready):
        for line in proc.stdout:
            line = line.strip()
            word, _, rest = line.partition(" ")
            if word == "READY":
                p = rest.split()
                self.ok, self.err, self.hz = True, "", float(p[3])
                self.size = (int(p[1]), int(p[2]))
                log("projector: spinproj", rest)
                ready.set()
            elif word == "STAT":
                self.stat = [float(x) for x in rest.split()]
            elif word in ("ERR", "INFO"):
                log("projector:", line)
                if word == "ERR":
                    self.err = rest
            if word not in ("READY", "PONG"):
                self.events.put((word, rest))
        self.ok = False
        self.err = self.err or "spinproj stopped"
        log("projector: spinproj exited", proc.wait())
        ready.set()
        time.sleep(3)  # e.g. no projector on HDMI yet: try again
        with self.lock:
            if self.proc is proc:
                self.start()

    def send(self, line):
        with self.lock:
            if self.proc is None or self.proc.poll() is not None:
                self.start()
            try:
                self.proc.stdin.write(line + "\n")
                self.proc.stdin.flush()
                return True
            except (OSError, ValueError, AttributeError):
                return False

    def drain(self):
        while not self.events.empty():
            self.events.get_nowait()

    def black(self):
        self.send("BLACK")

    def show_image(self, path):
        self.send("IMAGE " + path)

    def load(self, path, frames_per_turn, offset_px, reverse=False, phase_deg=0.0):
        """reverse: the video was made for the other turning direction; phase_deg: angle of frame 0."""
        self.drain()
        self.send(f"OFFSET {int(offset_px)}")
        self.send(f"REVERSE {1 if reverse else 0}")
        self.send(f"PHASE {float(phase_deg):.4f}")
        self.send(f"LOAD {int(frames_per_turn)} {path}")

    def angle(self, deg, rpm, t_ns, rtt_ns=0):
        self.send(f"A {deg:.4f} {rpm:.4f} {t_ns} {rtt_ns}")

    def arm(self, turns):
        self.send(f"ARM {turns}")

    def stop(self):
        self.send("STOP")


# =============================================================================== projector test images
def write_pgm(path, w, h, row_fn):
    """row_fn(y) -> bytes of length w. Rows repeat a lot, so this is fast even in pure Python."""
    with open(path, "wb") as f:
        f.write(f"P5 {w} {h} 255\n".encode())
        for y in range(h):
            f.write(row_fn(y))


def pattern(root, name, w, h, offset=0, vial=400):
    d = os.path.join(root, "patterns")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, f"{name}_{w}x{h}_{offset}_{vial}.pgm")
    if os.path.exists(p):
        return p
    cx, cy = w // 2 + offset, h // 2

    def band(x0, x1, val=255, base=0):
        r = bytearray([base]) * w
        x0, x1 = max(0, x0), min(w, x1)
        if x1 > x0:
            r[x0:x1] = bytes([val]) * (x1 - x0)
        return r

    if name == "white":
        row = bytes([255]) * w
        fn = lambda y: row
    elif name == "grid":
        lines = bytearray(w)
        for x in range(w):
            if (x - cx) % 64 in (0, 1) or abs(x - cx) < 2:
                lines[x] = 255
        full = bytes([255]) * w
        fn = lambda y: full if (y - cy) % 64 in (0, 1) else bytes(lines)
    elif name == "focus":
        half = h // 3
        a = bytearray(w)
        b = bytearray(w)
        for x in range(max(0, cx - half), min(w, cx + half)):
            a[x] = 255 if (x // 2) % 2 == 0 else 0
            b[x] = 0 if (x // 2) % 2 == 0 else 255
        z = bytes(w)
        fn = lambda y: z if abs(y - cy) >= half else (bytes(a) if (y // 2) % 2 == 0 else bytes(b))
    elif name == "circle":
        R = h * 0.4

        def fn(y):
            r = bytearray(w)
            dy = y - cy
            if abs(dy) <= R + 2:
                for rr in (R - 1, R, R + 1):
                    if abs(dy) <= rr:
                        dx = int((rr * rr - dy * dy) ** 0.5)
                        for x in (cx - dx, cx + dx):
                            if 0 <= x < w:
                                r[x] = 255
            return bytes(r)
    elif name == "center":  # rotation axis: a vertical line + short horizontal ticks
        line = band(cx - 1, cx + 2)
        tick = band(cx - 40, cx + 41)
        fn = lambda y: bytes(tick) if abs(y - cy) < 2 or abs(y - cy - h // 4) < 2 or abs(y - cy + h // 4) < 2 else bytes(line)
    elif name == "vial":  # a white bar as wide as the vial should be on the projector
        bar = band(cx - vial // 2, cx + vial // 2 + 1)
        fn = lambda y: bytes(bar)
    else:
        return None
    write_pgm(p, w, h, fn)
    return p


# =============================================================================== .spin files
class SpinError(ValueError):
    """A .spin file that cannot be printed: the message is for the user (Russian, like the screen)."""


def _num(v, positive=False):
    ok = isinstance(v, (int, float)) and not isinstance(v, bool) and v == v and abs(v) != float("inf")
    return ok and (not positive or v > 0)


def check_spin_meta(meta, names=None):
    """Raise SpinError unless spin.json describes a print this SpinPi can do. Unknown fields are fine."""
    if not isinstance(meta, dict) or meta.get("format") != "spin":
        raise SpinError("это не файл .spin (нет format: spin)")
    v = meta.get("version")
    if not isinstance(v, int) or isinstance(v, bool) or v < 1:
        raise SpinError("в spin.json нет версии формата")
    if v > SPIN_VERSION:
        raise SpinError(f"файл .spin версии {v}, этот SpinPi читает до {SPIN_VERSION}: обнови SpinPi")
    video = meta.get("video")
    if not isinstance(video, dict):
        raise SpinError("в spin.json нет раздела video")
    vf = video.get("file") or "video.mp4"
    if not isinstance(vf, str) or (names is not None and vf not in names):
        raise SpinError(f"в файле нет видео {vf}")
    sha = video.get("sha256")
    if sha is not None and not (isinstance(sha, str) and re.fullmatch(r"[0-9a-fA-F]{64}", sha)):
        raise SpinError("video.sha256 не похож на SHA-256")
    rot = video.get("rotation")
    if rot is not None and rot not in ROTATIONS:
        raise SpinError(f"непонятное направление вращения: {rot}")
    for k in ("frames", "frames_per_rev", "width", "height"):
        if video.get(k) is not None and not (isinstance(video[k], int) and not isinstance(video[k], bool) and video[k] > 0):
            raise SpinError(f"video.{k} должно быть целым > 0")
    if video.get("fps") is not None and not _num(video["fps"], positive=True):
        raise SpinError("video.fps должно быть > 0")
    if video.get("frame0_angle_deg") is not None and not _num(video["frame0_angle_deg"]):
        raise SpinError("video.frame0_angle_deg должно быть числом")
    plan = meta.get("plan")
    if plan is not None:
        if not isinstance(plan, dict):
            raise SpinError("plan должен быть объектом")
        for k in ("revolutions", "tolerance_pct", "revolution_s", "print_time_s", "rpm"):
            if plan.get(k) is not None and not _num(plan[k], positive=k != "tolerance_pct"):
                raise SpinError(f"plan.{k} должно быть числом > 0")
    return meta


def read_spin(path):
    """spin.json of a .spin file, checked. Raises SpinError."""
    try:
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            if "spin.json" not in names:
                raise SpinError("в файле нет spin.json")
            try:
                meta = json.loads(z.read("spin.json").decode("utf-8"))
            except (ValueError, UnicodeDecodeError):
                raise SpinError("spin.json не читается (это не JSON)") from None
            return check_spin_meta(meta, names)
    except (zipfile.BadZipFile, zipfile.LargeZipFile, OSError, EOFError, RuntimeError, NotImplementedError) as e:
        raise SpinError(f"файл .spin повреждён ({e})") from None


def extract_spin(path, dst):
    """Unpack video.mp4 (and preview.png, slicer.json) of a .spin into the folder dst, checking the
    video's SHA-256 on the way. Returns spin.json. Raises SpinError; a half-written folder is removed."""
    meta = read_spin(path)
    os.makedirs(dst, exist_ok=True)
    vf = meta["video"].get("file") or "video.mp4"
    want = (meta["video"].get("sha256") or "").lower()
    out = os.path.join(dst, "video.mp4")
    try:
        with zipfile.ZipFile(path) as z:
            h = hashlib.sha256()
            with z.open(vf) as src, open(out + ".part", "wb") as f:
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    h.update(chunk)
                    f.write(chunk)
            if want and h.hexdigest() != want:
                os.unlink(out + ".part")
                raise SpinError("видео в .spin повреждено: SHA-256 не совпадает (перепиши файл)")
            os.replace(out + ".part", out)
            names = z.namelist()
            for extra in ("preview.png", "slicer.json"):
                if extra in names and z.getinfo(extra).file_size < 8 << 20:
                    data = z.read(extra)
                    if extra == "preview.png" and not data.startswith(b"\x89PNG\r\n\x1a\n"):
                        continue
                    with open(os.path.join(dst, extra), "wb") as f:
                        f.write(data)
    except (zipfile.BadZipFile, OSError, EOFError, RuntimeError, NotImplementedError) as e:
        for n in ("video.mp4.part",):
            try:
                os.unlink(os.path.join(dst, n))
            except OSError:
                pass
        raise SpinError(f"файл .spin повреждён ({e})") from None
    return meta


def spin_plan(meta):
    """spin.json -> the flat plan SpinPi already knows from the MP4 tag and <name>.spin.json."""
    v = meta.get("video") or {}
    plan = dict(meta.get("plan") or {})
    plan["format"] = "spin"
    plan["version"] = meta.get("version")
    for k in ("frames", "fps", "width", "height", "frames_per_rev", "rotation", "frame0_angle_deg", "sha256"):
        if v.get(k) is not None:
            plan[k] = v[k]
    for k in ("name", "generator", "created", "resin"):
        if meta.get(k) is not None:
            plan[k] = meta[k]
    pr = meta.get("printer")
    if isinstance(pr, dict) and pr.get("profile_id"):
        plan["printer_profile_id"] = pr["profile_id"]
    return plan


# =============================================================================== library
SAFE =re.compile(r"[^A-Za-z0-9._()\- Ѐ-ӿ]+")
RPM_IN_NAME = re.compile(r"_(\d+(?:\.\d+)?)rpm", re.IGNORECASE)  # OpenCAL convention: part_9rpm.mp4


def safe_name(name):
    name = os.path.basename(urllib.parse.unquote(name)).strip()
    name = SAFE.sub("_", name)[:120]
    if not name.lower().endswith(PRINT_EXT):
        name += ".mp4"
    return name


def probe(path):
    """frames, fps, width, height of a video (ffprobe)."""
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_packets", "-show_entries",
             "stream=width,height,r_frame_rate,nb_read_packets:format_tags=comment", "-of", "json", path],
            capture_output=True, text=True, timeout=120).stdout
        d = json.loads(out)
        st = d["streams"][0]
        num, den = st["r_frame_rate"].split("/")
        fps = float(num) / float(den)
        info = {"frames": int(st["nb_read_packets"]), "fps": fps, "width": st["width"], "height": st["height"]}
        comment = (d.get("format", {}).get("tags") or {}).get("comment", "")
        try:
            plan = json.loads(comment) if comment.startswith("{") else None
        except ValueError:
            plan = None
        if isinstance(plan, dict) and str(plan.get("format", "")).startswith("spinslicer-print"):
            info["plan"] = plan
        return info
    except Exception as e:  # noqa: BLE001
        log("probe failed", path, e)
        return None


class Library:
    """Prints uploaded over Wi-Fi ("lib/<name>") + prints on USB sticks ("usb/<stick>/<path>").

    A print is a plain video or a .spin file. The video inside a .spin is unpacked once into
    spin/<hash>/ (SpinProj and ffprobe need a plain file), also for .spin files on a USB stick."""

    def __init__(self, root, usb_dirs_fn):
        self.root = os.path.join(root, "prints")
        self.thumbs = os.path.join(root, "thumbs")
        self.spin_root = os.path.join(root, "spin")
        os.makedirs(self.root, exist_ok=True)
        os.makedirs(self.thumbs, exist_ok=True)
        os.makedirs(self.spin_root, exist_ok=True)
        self.meta_path = os.path.join(root, "library.json")
        self.meta = load_json(self.meta_path, {})
        self.usb_dirs_fn = usb_dirs_fn
        self.lock = threading.Lock()

    def _save(self):
        save_json(self.meta_path, self.meta, indent=1)

    def lib_path(self, name):
        return os.path.join(self.root, safe_name(name))

    def thumb_path(self, fid):
        return os.path.join(self.thumbs, hashlib.sha1(fid.encode()).hexdigest()[:16] + ".jpg")

    def thumb_file(self, fid):
        """(path, content type): the .spin preview (PNG) or a frame grabbed by ffmpeg (JPEG)."""
        jpg = self.thumb_path(fid)
        png = jpg[:-4] + ".png"
        return (png, "image/png") if os.path.exists(png) else (jpg, "image/jpeg")

    def spin_dir(self, fid):
        return os.path.join(self.spin_root, hashlib.sha1(fid.encode()).hexdigest()[:16])

    @staticmethod
    def _thumb_from_video(video, out):
        try:
            subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-i", video, "-vf", "scale=320:-2", "-frames:v", "1", out],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError:  # no ffmpeg (a PC test): no picture
            pass

    def _spin_info(self, fid, path):
        """Unpack and check a .spin; metadata from spin.json (ffprobe only for what it leaves out)."""
        d = self.spin_dir(fid)
        try:
            meta = extract_spin(path, d)
        except SpinError as e:
            log("bad .spin", path, e)
            return {"error": str(e), "format": "spin"}
        video = os.path.join(d, "video.mp4")
        plan = spin_plan(meta)
        v = meta["video"]
        info = {k: v[k] for k in ("frames", "fps", "width", "height") if v.get(k) is not None}
        if len(info) < 4:
            info = dict(probe(video) or {}, **info)
            info.pop("plan", None)
        info.update(plan=plan, video=video, format="spin", title=meta.get("name"))
        thumb = self.thumb_path(fid)
        png = thumb[:-4] + ".png"
        preview = os.path.join(d, "preview.png")
        for p in (png, thumb):
            try:
                os.unlink(p)
            except OSError:
                pass
        if os.path.exists(preview):
            with open(preview, "rb") as src, open(png, "wb") as dst:
                dst.write(src.read())
            info["preview"] = True
        else:
            self._thumb_from_video(video, thumb)
        return info

    def _info(self, fid, path):
        """Cached metadata; re-probed (a .spin: unpacked again) when the file changes."""
        st = os.stat(path)
        key = f"{st.st_size}:{int(st.st_mtime)}"
        with self.lock:
            m = self.meta.get(fid)
        if m is not None and m.get("video") and not os.path.exists(m["video"]):
            m = None  # the unpacked video is gone: unpack again
        if m is None or m.get("key") != key:
            if path.lower().endswith(SPIN_EXT):
                info = self._spin_info(fid, path)
            else:
                info = probe(path) or {}
                self._thumb_from_video(path, self.thumb_path(fid))
            old = m or self.meta.get(fid) or {}
            m = dict(info, key=key, size=st.st_size, added=old.get("added", time.time()), revs=old.get("revs"))
            with self.lock:
                self.meta[fid] = m
                self._save()
        return m

    @staticmethod
    def _sidecar(path):
        """SpinSlicer writes <name>.spin.json beside the video (the same plan as the MP4 tag)."""
        p = os.path.splitext(path)[0] + ".spin.json"
        try:
            with open(p, encoding="utf-8") as f:
                plan = json.load(f)
            return plan if isinstance(plan, dict) else None
        except (OSError, ValueError):
            return None

    def _entry(self, fid, path, source):
        m = self._info(fid, path)
        name = os.path.basename(path)
        d = {k: v for k, v in m.items() if k not in ("key", "video")}
        d.update(id=fid, name=name, source=source)
        d.setdefault("format", "video")
        plan = m.get("plan") if d["format"] == "spin" else (self._sidecar(path) or m.get("plan"))
        d["plan"] = plan
        plan = plan if isinstance(plan, dict) else {}
        if _num(plan.get("revolutions"), positive=True):
            d["plan_revs"] = float(plan["revolutions"])
            d["tolerance_pct"] = float(plan["tolerance_pct"]) if _num(plan.get("tolerance_pct"), True) else None
        # how the frames sit on the vial's angle (SpinSlicer writes these; old videos: one turn, unknown way)
        rot = plan.get("rotation")
        d["rotation"] = rot if rot in ROTATIONS else None
        d["frame0_angle_deg"] = float(plan["frame0_angle_deg"]) if _num(plan.get("frame0_angle_deg")) else 0.0
        fpr = plan.get("frames_per_rev")
        fpr = int(fpr) if _num(fpr, positive=True) else None
        d["frames_per_rev"] = fpr
        w = plan.get("warnings")
        d["warnings"] = [str(x) for x in w if x] if isinstance(w, list) else []
        d["resin"] = plan.get("resin") if isinstance(plan.get("resin"), dict) else None
        d["printer_profile_id"] = plan.get("printer_profile_id")
        if m.get("frames") and m.get("fps"):
            dur = m["frames"] / m["fps"]
            hint = RPM_IN_NAME.search(name)
            if m.get("revs"):  # set by hand on the web page
                rev_s = dur / m["revs"]
            elif fpr:  # the slicer says how many frames make one turn
                rev_s = fpr / m["fps"]
            elif hint and d["format"] != "spin":  # OpenCAL-style file name
                rev_s = 60.0 / float(hint.group(1))
            else:  # SpinSlicer: one video = one turn
                rev_s = dur
            d["rev_s"] = rev_s
            d["rpm"] = 60.0 / rev_s
            d["revs"] = dur / rev_s
        return d

    def list(self):
        out = []
        for mount in self.usb_dirs_fn():
            label = os.path.basename(mount.rstrip("/"))
            for dirpath, dirs, names in os.walk(mount):
                dirs[:] = [x for x in dirs if not x.startswith(".") and x != "System Volume Information"]
                if dirpath.count(os.sep) - mount.count(os.sep) > 2:
                    dirs[:] = []
                for n in sorted(names):
                    if n.lower().endswith(PRINT_EXT) and not n.startswith(".") and "recording" not in n.lower():
                        p = os.path.join(dirpath, n)
                        rel = os.path.relpath(p, mount)
                        try:
                            out.append(self._entry(f"usb/{label}/{rel}", p, "usb"))
                        except OSError:
                            pass
        lib = []
        for n in os.listdir(self.root):
            p = os.path.join(self.root, n)
            if os.path.isfile(p) and not n.startswith(".") and not n.endswith(".part"):
                lib.append(self._entry("lib/" + n, p, "lib"))
        lib.sort(key=lambda f: -f.get("added", 0))
        return out + lib

    def resolve(self, fid):
        """id -> (entry, path of the video to play) or (None, None). A .spin plays its unpacked video."""
        for f in self.list():
            if f["id"] == fid:
                if f.get("format") == "spin":
                    with self.lock:
                        m = self.meta.get(fid) or {}
                    return f, m.get("video")
                if fid.startswith("lib/"):
                    return f, self.lib_path(fid[4:])
                _, label, rel = fid.split("/", 2)
                for mount in self.usb_dirs_fn():
                    if os.path.basename(mount.rstrip("/")) == label:
                        return f, os.path.join(mount, rel)
        return None, None

    def added(self, name):
        return self._entry("lib/" + name, self.lib_path(name), "lib")

    def remove(self, fid):
        if not fid.startswith("lib/"):
            return False
        for p in (self.lib_path(fid[4:]), self.thumb_path(fid), self.thumb_path(fid)[:-4] + ".png"):
            try:
                os.unlink(p)
            except FileNotFoundError:
                pass
        d = self.spin_dir(fid)
        for n in ("video.mp4", "video.mp4.part", "preview.png", "slicer.json"):
            try:
                os.unlink(os.path.join(d, n))
            except FileNotFoundError:
                pass
        try:
            os.rmdir(d)
        except OSError:
            pass
        with self.lock:
            self.meta.pop(fid, None)
            self._save()
        return True

    def forget(self, fid):
        with self.lock:
            if self.meta.pop(fid, None) is not None:
                self._save()

    def set_revs(self, fid, revs):
        with self.lock:
            if fid in self.meta:
                self.meta[fid]["revs"] = max(0.01, float(revs))
                self._save()


# =============================================================================== USB sticks
class UsbWatcher:
    """Mounts USB sticks read-only under /mnt/spinpi-usb/<dev> (Pi OS Lite does not automount)."""

    def __init__(self, base="/mnt/spinpi-usb", extra_dirs=()):
        self.base = base
        self.extra = list(extra_dirs)
        self.mounted = {}  # dev -> mountpoint
        self.can_mount = os.geteuid() == 0 if hasattr(os, "geteuid") else False
        if self.can_mount:
            threading.Thread(target=self._run, daemon=True).start()

    def dirs(self):
        ds = list(self.mounted.values()) + [d for d in self.extra if os.path.isdir(d)]
        for d in glob.glob("/media/*/*"):  # desktop automount, if any
            if os.path.ismount(d):
                ds.append(d)
        return ds

    def _parts(self):
        out = []
        for disk in glob.glob("/sys/block/sd*"):
            name = os.path.basename(disk)
            parts = sorted(os.path.basename(p) for p in glob.glob(f"{disk}/{name}[0-9]*"))
            out += parts or [name]
        return out

    def _run(self):
        os.makedirs(self.base, exist_ok=True)
        while True:
            present = set(self._parts())
            for dev in present - set(self.mounted):
                mp = os.path.join(self.base, dev)
                os.makedirs(mp, exist_ok=True)
                r = subprocess.run(["mount", "-o", "ro,noatime", f"/dev/{dev}", mp], capture_output=True, text=True)
                if r.returncode == 0:
                    self.mounted[dev] = mp
                    log("USB: mounted", dev)
                else:
                    try:
                        os.rmdir(mp)
                    except OSError:
                        pass
            for dev in set(self.mounted) - present:
                mp = self.mounted.pop(dev)
                subprocess.run(["umount", "-l", mp], capture_output=True)
                try:
                    os.rmdir(mp)
                except OSError:
                    pass
                log("USB: removed", dev)
            time.sleep(2)


# =============================================================================== projector warm-up
class Warmup:
    """Lamp/LED output drifts for the first minutes. Count from the moment HDMI saw the projector."""

    def __init__(self, watch=True):
        self.since = time.monotonic()  # unknown at start: assume it was switched on with the Pi
        self.connected = None
        if watch:  # --hdmi ignore (PC tests): never say "not connected"
            threading.Thread(target=self._run, daemon=True).start()

    @staticmethod
    def _status():
        st = None
        for p in glob.glob("/sys/class/drm/card*-HDMI-A-*/status"):
            try:
                v = open(p).read().strip()
            except OSError:
                continue
            st = st or v == "connected"
        return st  # None = cannot tell (not a Pi / no DRM)

    def _run(self):
        while True:
            c = self._status()
            if c and self.connected is False:
                self.since = time.monotonic()  # projector just plugged in / switched on
                log("projector: HDMI connected")
            self.connected = c
            time.sleep(1)

    def left(self, minutes):
        if self.connected is False:
            return None  # not connected at all
        return max(0.0, minutes * 60 - (time.monotonic() - self.since))


# =============================================================================== the print job
class PrintError(Exception):
    pass


# The printer profile (GET /api/printer): what SpinSlicer needs for its maths, edited on the web
# page. key in settings["profile"] -> (kind, min, max); None = not known yet (the slicer keeps its own)
PROFILE_FIELDS = {
    "irradiance_mw_cm2": (float, 0.0, 1000.0),  # projector, on the axis, at full white
    "contrast": (float, 1.0, 1e7),
    "vial_diameter_mm": (float, 1.0, 500.0),
    "vial_wall_mm": (float, 0.0, 50.0),
    "vial_n": (float, 1.0, 3.0),  # refractive index of the vial glass
    "motor_gear": (float, 0.01, 1000.0),
    "motor_dir": (str, None, None),  # ccw_from_top | cw_from_top: how the vial really turns
}
PROFILE_DEFAULTS = {"irradiance_mw_cm2": None, "contrast": None, "vial_diameter_mm": None, "vial_wall_mm": None,
                    "vial_n": None, "motor_gear": 1.0, "motor_dir": "ccw_from_top"}


class Printer:
    """Exposure = a whole number of turns, ended exactly on the turn boundary.

    The video plays at its own frame rate (every frame the same time on screen) and the
    motor is the slave: SpinPi trims the motor speed so the vial angle follows the video.
    Faults (ESP silent, motor off speed, video stalled) switch the light off at once.
    """

    def __init__(self, esp, proj, lib, data, proj_w, proj_h, warm):
        self.esp, self.proj, self.lib, self.warm = esp, proj, lib, warm
        self.data, self.pw, self.ph = data, proj_w, proj_h
        self.settings_path = os.path.join(data, "settings.json")
        self.save_lock = threading.RLock()
        self.history_path = os.path.join(data, "history.json")
        self.settings = load_json(self.settings_path, {})
        defaults = {"factor": 1.0, "delay_s": 3, "default_revs": 5, "warmup_min": 5, "last_id": "",
                    "offset_px": 0, "vial_px": 400, "display_fps": 60.0}
        for k, v in defaults.items():
            self.settings.setdefault(k, v)
        prof = self.settings.get("profile")
        prof = prof if isinstance(prof, dict) else {}
        for k, v in PROFILE_DEFAULTS.items():
            prof.setdefault(k, v)
        self.settings["profile"] = prof
        if not self.settings.get("printer_id"):  # made once, stays for the life of the SD card
            self.settings["printer_id"] = "spinpi-" + uuid.uuid4().hex[:8]
            self.save_settings()
        self.settings.setdefault("printer_name", "")
        self.settings.setdefault("prints", 0)
        self.history = load_json(self.history_path, [])
        self.on_profile = None  # called after the profile changes (mDNS name)
        self.state = "idle"  # idle | wait | print | done | error
        self.wait = None  # (seconds, kind) while waiting before the light
        self.job = None
        self.request = None  # the last print sent from SpinSlicer that waits for a yes on the screen
        self.msg = ""
        self.sync_ms = None
        self.drops = 0
        self.stop_flag = threading.Event()
        self.stop_reason = ""  # who stopped the last print: shown on the page and in the log
        self.motor_rpm = 0.0
        self.motor_angle = 0.0
        self.pattern = "black"
        self.esp_list = []
        self.angle_mode = getattr(proj, "engine", "mpv") == "angle"
        self.last_pos = None  # (angle, rpm, at_speed, t_mid, received) from the job's angle feeder

    # ---- persistence
    def save_settings(self):
        with self.save_lock:  # the print thread and a web request may save at the same moment
            save_json(self.settings_path, self.settings, indent=1)

    def save_history(self):
        with self.save_lock:
            self.history = self.history[-200:]
            save_json(self.history_path, self.history, indent=1, ensure_ascii=False)

    def busy(self):
        return self.state in ("wait", "print")

    # ---- exposure maths
    def cadence_ok(self, fps):
        # the same for both engines: refreshes per frame = display Hz / video fps. Not a whole number
        # (24 fps on 60 Hz) = frames take turns staying up 2 and 3 refreshes
        r = self.settings["display_fps"] / fps if fps else 0
        return r > 0 and abs(r - round(r)) < 0.02

    def plan_for(self, f, revs=None, factor=None):
        """revolutions to expose, their time and how far that is from the slicer's plan."""
        factor = float(self.settings["factor"] if factor is None else factor)
        plan = f.get("plan_revs")
        if revs is None:
            revs = max(1, round(plan * factor)) if plan else int(self.settings["default_revs"])
        revs = max(1, int(revs))
        d = {"revs": revs, "time_s": revs * f["rev_s"], "factor": factor, "plan_revs": plan,
             "tolerance_pct": f.get("tolerance_pct"), "cadence_ok": self.cadence_ok(f.get("fps", 0)),
             "reversed": self.reversed(f), "other_printer": self.other_printer(f), "warnings": f.get("warnings") or []}
        if plan:
            d["dose_dev_pct"] = 100.0 * (revs / (plan * factor) - 1.0)
        return d

    # ---- how the video's frames sit on the vial
    @property
    def motor_dir(self):
        d = self.settings["profile"].get("motor_dir")
        return d if d in ROTATIONS else "ccw_from_top"

    def reversed(self, f):
        """The slicer made the frames for the other turning direction: play them backwards."""
        return bool(f.get("rotation")) and f["rotation"] != self.motor_dir

    def other_printer(self, f):
        pid = f.get("printer_profile_id")
        return bool(pid) and pid != self.settings["printer_id"]

    def annotate(self, files):
        for f in files:
            f["reversed"] = self.reversed(f)
            f["other_printer"] = self.other_printer(f)
        return files

    # ---- the printer profile for SpinSlicer
    def profile(self):
        s, p = self.settings, self.settings["profile"]
        hz = getattr(self.proj, "hz", None) or s.get("display_fps")
        return {
            "api": API_VERSION, "spin_version": SPIN_VERSION,
            "id": s["printer_id"], "name": s.get("printer_name") or socket.gethostname(),
            "projector": {"width": self.pw, "height": self.ph, "hz": hz,
                          "irradiance_mw_cm2": p.get("irradiance_mw_cm2"), "contrast": p.get("contrast")},
            "vial": {"diameter_mm": p.get("vial_diameter_mm"), "wall_mm": p.get("vial_wall_mm"),
                     "n_glass": p.get("vial_n")},
            "calibration": {"offset_px": s["offset_px"], "center_offset_px": [s["offset_px"], 0],
                            "vial_width_px": s["vial_px"]},
            "motor": {"gear": p.get("motor_gear"), "direction": self.motor_dir},
            "exposure": {"factor": s["factor"]},
            "software": {"spinpi": VERSION, "spinscreen": self.esp.version or None,
                         "engine": "angle" if self.angle_mode else "mpv"},
        }

    def set_profile(self, b):
        """Edit from the web page: {"name": …, "irradiance_mw_cm2": …, …}; empty / null = not known."""
        if not isinstance(b, dict):
            return "нужен JSON-объект"
        if "name" in b:
            self.settings["printer_name"] = SAFE.sub(" ", str(b["name"] or "")).strip()[:40]
        prof = dict(self.settings["profile"])
        for k, (kind, lo, hi) in PROFILE_FIELDS.items():
            if k not in b:
                continue
            v = b[k]
            if kind is str:
                if v not in ROTATIONS:
                    return f"{k}: {' / '.join(ROTATIONS)}"
                prof[k] = v
            elif v is None or v == "":
                prof[k] = PROFILE_DEFAULTS[k]
            else:
                try:
                    v = float(v)
                except (TypeError, ValueError):
                    return f"{k}: нужно число"
                if not (lo <= v <= hi):
                    return f"{k}: от {lo:g} до {hi:g}"
                prof[k] = v
        self.settings["profile"] = prof
        self.save_settings()
        if self.on_profile:
            self.on_profile()
        return None

    def status(self):
        j = self.job
        left = self.warm.left(self.settings["warmup_min"])
        d = {"state": self.state, "msg": self.msg, "esp": self.esp.connected, "esp_version": self.esp.version,
             "projector": self.proj.ok, "projector_err": self.proj.err[-200:] if not self.proj.ok else "",
             "hdmi": self.warm.connected, "warmup_left_s": left, "wait": self.wait,
             "sync_ms": self.sync_ms, "drops": self.drops, "motor_rpm": self.motor_rpm,
             "motor_angle": self.motor_angle % 360, "settings": self.settings, "pattern": self.pattern,
             "history": self.history[-8:], "version": VERSION, "request": self.request_view(),
             "engine": "angle" if self.angle_mode else "mpv"}
        if j:
            d["job"] = {k: j.get(k) for k in ("id", "file", "rpm", "revs", "time_s", "factor", "plan_revs", "reversed")}
            d["job"].update(turn=j.get("turn", 0), left_s=j.get("left_s", j["time_s"]),
                            progress=j.get("progress", 0.0))
        return d

    # ---- control
    def _check(self, fid, revs=None, factor=None):
        """(error, file, path, plan): what start() needs, and why it cannot start (None = it can)."""
        if self.busy():
            return "busy", None, None, None
        f, path = self.lib.resolve(fid)
        if f and f.get("error"):
            return f["error"], None, None, None
        if not f or not f.get("rpm") or not path:
            return "no such video", None, None, None
        if self.warm.connected is False:
            return "projector not connected (HDMI)", None, None, None
        p = self.plan_for(f, revs, factor)
        if p["reversed"] and not self.angle_mode:
            return "видео для другого направления вращения: нужен движок angle (SpinProj)", None, None, None
        return None, f, path, p

    # A print sent from SpinSlicer does not start by itself: the vial has to be in place, so the
    # printer's screen asks (ASK) and a yes there (EV GO) starts it. Unanswered, it goes after 10 min.
    ASK_TIMEOUT_S = 600.0
    NO_SCREEN = "экран принтера не подключён"

    def ask(self, fid, revs=None, factor=None, delay_s=None):
        err, f, _path, p = self._check(fid, revs, factor)
        if err:
            return err
        if not self.esp.connected:
            return self.NO_SCREEN
        self.expire_request(replaced=True)
        self.request = {"state": "waiting_confirm", "id": fid, "name": f["name"], "revs": p["revs"],
                        "time_s": p["time_s"], "error": None, "t": time.monotonic(),
                        "args": (fid, revs, factor, delay_s)}
        name = f["name"].encode("utf-8")[:60].decode("utf-8", "ignore")
        self.esp.send(f"ASK {int(p['revs'])} {int(round(p['time_s']))} {name}")
        log("print request:", f["name"], "waits for a yes on the screen")
        return None

    def answer(self, yes):
        r = self.request
        if not r or r["state"] != "waiting_confirm":
            return
        if not yes:
            r["state"] = "cancelled"
            log("print request: cancelled on the screen")
            return
        r["state"] = "starting"
        err = self.start(*r["args"])
        r["state"], r["error"] = ("started", None) if err is None else ("error", err)
        if err:
            self.esp.send(f"STATE ERROR {err}")

    def expire_request(self, replaced=False):
        """Drop a request nobody answered (or one a newer print replaces): the screen leaves it."""
        r = self.request
        if r and r["state"] == "waiting_confirm" and (replaced or time.monotonic() - r["t"] >= self.ASK_TIMEOUT_S):
            r["state"] = "replaced" if replaced else "expired"
            self.esp.send("ASK END")

    def request_view(self):
        r = self.request
        return {k: v for k, v in r.items() if k not in ("t", "args")} if r else None

    def start(self, fid, revs=None, factor=None, delay_s=None):
        err, f, path, p = self._check(fid, revs, factor)
        if err:
            return err
        self.expire_request(replaced=True)  # started some other way: the question on the screen goes
        delay_s = float(self.settings["delay_s"] if delay_s is None else delay_s)
        self.settings.update(factor=p["factor"], delay_s=delay_s, last_id=fid)
        if not f.get("plan_revs"):
            self.settings["default_revs"] = p["revs"]
        self.save_settings()
        self.job = dict(p, id=fid, file=f["name"], path=path, rpm=f["rpm"], rev_s=f["rev_s"], fps=f.get("fps"),
                        delay_s=delay_s, frames_per_rev=f.get("frames_per_rev"), phase=f.get("frame0_angle_deg") or 0.0,
                        format=f.get("format", "video"), plan=f.get("plan") if isinstance(f.get("plan"), dict) else None,
                        resin=f.get("resin"), printer_profile_id=f.get("printer_profile_id"))
        self.stop_flag.clear()
        self.stop_reason = ""
        threading.Thread(target=self._run_angle if self.angle_mode else self._run, daemon=True).start()
        return None

    def stop(self, reason="со страницы принтера"):
        if self.busy() and not self.stop_flag.is_set():
            log("print stop:", reason)
        self.stop_reason = reason
        self.stop_flag.set()

    def motor(self, on, rpm=None):
        if self.busy():
            return "busy"
        if rpm:
            self.esp.send(f"RPM {float(rpm):.4f}")
        self.esp.send(f"SPIN {1 if on else 0}")
        return None

    def history_entry(self, j, engine):
        """One line of the print log: also what SpinSlicer reads back (GET /api/history) to tune the resin."""
        self.settings["prints"] = int(self.settings.get("prints") or 0) + 1
        plan = j.get("plan") or {}
        keep = ("revolutions", "tolerance_pct", "revolution_s", "print_time_s", "rpm", "exposure_factor", "brightness",
                "generator", "created", "sha256")
        return {"n": self.settings["prints"], "t": time.strftime("%Y-%m-%d %H:%M"),
                "time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "id": j["id"], "file": j["file"], "format": j.get("format", "video"),
                "revs": j["revs"], "time_s": j["time_s"], "rpm": j["rpm"],
                "factor": j["factor"], "exposure_pct": round(100 * (j["factor"] - 1), 2),
                "plan_revs": j["plan_revs"], "tolerance_pct": j["tolerance_pct"],
                "plan": {k: plan[k] for k in keep if plan.get(k) is not None} or None,
                "resin": j.get("resin"), "printer_profile_id": j.get("printer_profile_id"),
                "printer_id": self.settings["printer_id"], "reversed": j.get("reversed", False),
                "frame0_angle_deg": j.get("phase", 0.0), "engine": engine, "result": None}

    def result(self, verdict):
        """Feedback after a print: under / ok / over. Moves the exposure factor for next time."""
        if not self.history or verdict not in ("under", "ok", "over"):
            return None
        h = self.history[-1]
        h["result"] = verdict
        h["factor_after"] = None  # filled below: the correction the next print will start from
        tol = h.get("tolerance_pct") or 5.0
        step = min(0.15, max(0.02, tol / 100.0))
        if verdict == "under":
            self.settings["factor"] *= 1 + step
        elif verdict == "over":
            self.settings["factor"] *= 1 - step
        self.settings["factor"] = round(max(0.3, min(3.0, self.settings["factor"])), 4)
        h["factor_after"] = self.settings["factor"]
        self.save_settings()
        self.save_history()
        pct = 100 * (self.settings["factor"] - 1)
        return pct

    def show_pattern(self, name):
        if self.busy():
            return "busy"
        self.pattern = name
        if name == "black":
            self.proj.black()
            return None
        p = pattern(self.data, name, self.pw, self.ph, int(self.settings["offset_px"]), int(self.settings["vial_px"]))
        if not p:
            return "unknown pattern"
        self.proj.show_image(p)
        return None

    def calibrate(self, offset=None, vial=None):
        if offset is not None:
            self.settings["offset_px"] = max(-400, min(400, int(offset)))
        if vial is not None:
            self.settings["vial_px"] = max(20, min(4000, int(vial)))
        self.save_settings()
        self.esp.send(f"CAL {self.settings['offset_px']} {self.settings['vial_px']}")
        if self.pattern != "black" and not self.busy():
            self.show_pattern(self.pattern)

    def send_list(self):
        files = [f for f in self.lib.list() if f.get("rpm") and not f.get("error")][:16]
        self.esp_list = [f["id"] for f in files]
        s, e = self.settings, self.esp
        e.send(f"FL BEGIN {len(files)} {100 * (s['factor'] - 1):.0f} {s['delay_s']:.0f} {int(s['default_revs'])}")
        for i, f in enumerate(files):
            name = f["name"].encode("utf-8")[:60].decode("utf-8", "ignore")
            plan = f.get("plan_revs") or -1
            tol = f.get("tolerance_pct") or -1
            e.send(f"FL {i} {f['rpm']:.2f} {1 if f['source'] == 'usb' else 0} {plan:.2f} {tol:.1f} "
                   f"{f['rev_s']:.3f} {1 if self.cadence_ok(f.get('fps', 0)) else 0} {name}")
        e.send("FL END")

    def on_esp_event(self, ev):
        log("ESP event:", ev)
        p = ev.split()
        if p[0] == "LIST":
            self.send_list()
        elif p[0] == "PRINT":
            if len(p) >= 5 and p[1].isdigit() and int(p[1]) < len(self.esp_list):
                err = self.start(self.esp_list[int(p[1])], revs=int(float(p[2])), factor=1 + float(p[3]) / 100,
                                 delay_s=float(p[4]))
            else:
                err = self.start(self.settings.get("last_id") or "")
            if err:
                self.esp.send(f"STATE ERROR {err}")
        elif p[0] in ("GO", "NO"):
            self.answer(p[0] == "GO")
        elif p[0] == "STOP":
            self.stop("с экрана принтера")
        elif p[0] == "RESULT" and len(p) > 1:
            pct = self.result(p[1])
            if pct is not None:
                self.esp.send(f"MSG Поправка экспозиции: {pct:+.0f}%")
        elif p[0] == "PATTERN" and len(p) > 1:
            self.show_pattern(p[1])
        elif p[0] == "OFFSET" and len(p) > 1:
            self.calibrate(offset=int(p[1]))
        elif p[0] == "VIALW" and len(p) > 1:
            self.calibrate(vial=int(p[1]))
        elif p[0] in ("REBOOT", "POWEROFF"):
            self.stop("перезагрузка" if p[0] == "REBOOT" else "выключение")
            self.proj.black()
            self.esp.send("MSG " + ("Перезагружаюсь…" if p[0] == "REBOOT" else "Выключаюсь… подожди 20 с"))
            if os.geteuid() == 0:
                time.sleep(1)
                subprocess.Popen(["systemctl", "reboot" if p[0] == "REBOOT" else "poweroff"])
            else:
                log("(not root: would", p[0].lower(), ")")

    # ---- the job thread
    def _wait(self, seconds_fn, kind):
        """Dark waiting with a countdown on the screens. seconds_fn() -> seconds left (or 0)."""
        last = 0
        while not self.stop_flag.is_set():
            left = seconds_fn()
            if left <= 0:
                self.wait = None
                return
            self.wait = (left, kind)
            if time.monotonic() - last > 0.5:
                last = time.monotonic()
                self.esp.send(f"WAIT {int(left + 0.99)} {kind}")
            time.sleep(0.1)
        raise InterruptedError

    def _run(self):
        j = self.job
        e = self.esp
        entry = self.history_entry(j, "mpv")
        try:
            self.state, self.msg, self.sync_ms, self.drops, self.pattern = "wait", "", None, 0, "black"
            self.proj.black()
            e.send("FILE " + j["file"].encode("utf-8")[:60].decode("utf-8", "ignore"))
            e.send(f"TURNS {j['revs']}")
            e.send(f"RPM {j['rpm']:.4f}")
            e.send("SPIN 1")
            e.send("STATE PRINT")
            # 1) motor up to speed
            t0 = time.monotonic()

            def spin_left():
                p = e.query_pos()
                if p:
                    self.motor_angle, self.motor_rpm = p[0], p[1]
                    if p[2]:
                        return 0
                if time.monotonic() - t0 > 90:
                    raise PrintError("мотор не набрал скорость")
                return max(0.5, (j["rpm"] - (p[1] if p else 0)) / 6.0 + 0.5)

            self._wait(spin_left, "spin")
            # 2) projector warm-up, 3) dark spin-up for the resin
            self._wait(lambda: self.warm.left(self.settings["warmup_min"]) or 0, "warm")
            t_dark = time.monotonic() + j["delay_s"]
            self._wait(lambda: t_dark - time.monotonic(), "dark")
            # 4) light: video at its own clock, the motor follows
            self.proj.play_loop(j["path"], 1.0, int(self.settings["offset_px"]))
            for _ in range(100):
                tpos = self.proj.get("time-pos")
                if tpos is not None:
                    break
                time.sleep(0.05)
            else:
                raise PrintError("видео не запустилось")
            # the vial angle := where the video is right now (frame 0 sits at frame0_angle_deg), so they
            # start in phase
            j["angle0"] = ((tpos % j["rev_s"]) / j["rev_s"] * 360.0 + j.get("phase", 0.0)) % 360.0
            e.send(f"SETANGLE {j['angle0']:.4f}")
            entry["angle0"] = round(j["angle0"], 4)
            dfps = self.proj.get("display-fps")
            if dfps:
                self.settings["display_fps"] = round(float(dfps), 3)
            drops0 = (self.proj.get("frame-drop-count") or 0) + (self.proj.get("decoder-frame-drop-count") or 0)
            self.state = "print"
            self._expose(j, drops0)
            self.proj.black()
            e.send("SPIN 0")
            e.send("STATE DONE")
            self.state = "done"
            entry["drops"] = self.drops
            entry["sync_ms"] = self.sync_ms
            self.history.append(entry)
            self.save_history()
        except InterruptedError:
            self.proj.black()
            e.send("SPIN 0")
            e.send("STATE IDLE")
            self.state, self.msg = "idle", "остановлено " + (self.stop_reason or "")
        except Exception as ex:  # noqa: BLE001
            log("print error:", ex, "| turn", j.get("turn"), "of", j.get("revs"))
            self.proj.black()
            e.send("SPIN 0")
            e.send(f"STATE ERROR {str(ex)[:40]}")
            self.state, self.msg = "error", str(ex)
        finally:
            self.wait = None
            self.save_settings()

    # ---- the job thread, projector by angle (SpinProj)
    def _feed_angles(self, done):
        """Ask the ESP for the angle 20 times a second and hand it to SpinProj, for the whole job."""
        while not done.is_set():
            p = self.esp.query_pos()
            if p:
                a, rpm, at_speed, t_mid, rtt = p
                self.last_pos = (a, rpm, at_speed, t_mid, time.monotonic())
                self.motor_angle, self.motor_rpm = a, rpm
                self.proj.angle(a, rpm, int(t_mid * 1e9), int(rtt * 1e9))
            time.sleep(0.05)

    def _proj_event(self, timeout):
        try:
            return self.proj.events.get(timeout=timeout)
        except queue.Empty:
            return None, ""

    def _run_angle(self):
        j = self.job
        e, pr = self.esp, self.proj
        entry = self.history_entry(j, "angle")
        done = threading.Event()
        try:
            self.state, self.msg, self.sync_ms, self.drops, self.pattern = "wait", "", None, 0, "black"
            self.last_pos = None
            pr.black()
            frames = j.get("frames_per_rev") or max(1, round(j["rev_s"] * (j.get("fps") or 0)))
            if j.get("reversed"):
                log("the video turns the other way than the motor: frames go backwards")
            # unpacks while the motor spins up
            pr.load(j["path"], frames, self.settings["offset_px"], reverse=j.get("reversed"), phase_deg=j.get("phase", 0.0))
            threading.Thread(target=self._feed_angles, args=(done,), daemon=True).start()
            e.send("FILE " + j["file"].encode("utf-8")[:60].decode("utf-8", "ignore"))
            e.send(f"TURNS {j['revs']}")
            e.send(f"RPM {j['rpm']:.4f}")
            e.send("SPIN 1")
            e.send("STATE PRINT")
            t0 = time.monotonic()
            loaded = {}

            def events_until_loaded():
                while True:
                    w, rest = self._proj_event(0)
                    if w is None:
                        return
                    if w == "LOADED":
                        loaded["info"] = rest
                        log("frames ready:", rest)
                    elif w == "ERR":
                        raise PrintError("проектор: " + rest[:60])

            # 1) motor up to speed (frames are being unpacked meanwhile)
            def spin_left():
                events_until_loaded()
                p = self.last_pos
                if p and p[2] and time.monotonic() - p[4] < 0.5:
                    return 0
                if time.monotonic() - t0 > 90:
                    raise PrintError("мотор не набрал скорость")
                return max(0.5, (j["rpm"] - (p[1] if p else 0)) / 6.0 + 0.5)

            self._wait(spin_left, "spin")
            # 2) projector warm-up, 3) the last frames, 4) dark spin for the resin
            self._wait(lambda: (events_until_loaded(), self.warm.left(self.settings["warmup_min"]) or 0)[1], "warm")
            said = False
            while "info" not in loaded:
                if not said:
                    e.send("MSG Готовлю кадры…")
                    said = True
                if time.monotonic() - t0 > 600:
                    raise PrintError("кадры не готовы за 10 минут")
                self._wait(lambda: (events_until_loaded(), 0 if "info" in loaded else 1)[1], "spin")
            t_dark = time.monotonic() + j["delay_s"]
            self._wait(lambda: t_dark - time.monotonic(), "dark")
            # 5) light: SpinProj shows frames by angle for exactly N turns, then goes black itself
            pr.arm(j["revs"])
            t_arm = time.monotonic()
            w, rest = None, ""
            while w not in ("LIGHT", "ERR") and time.monotonic() - t_arm < 2.0:
                w, rest = self._proj_event(0.1)
            if w != "LIGHT":
                raise PrintError("проектор не включил свет: " + (rest or "нет ответа")[:50])
            try:
                j["angle0"] = float(rest.split()[0])  # the angle the light came on at
                entry["angle0"] = round(j["angle0"], 4)
            except (ValueError, IndexError):
                pass
            self.state = "print"
            if pr.hz:
                self.settings["display_fps"] = round(pr.hz, 3)
            result = self._watch_light(j, done)
            entry.update(result)
            pr.black()
            e.send("SPIN 0")
            e.send("STATE DONE")
            self.state = "done"
            entry["drops"] = self.drops
            entry["sync_ms"] = self.sync_ms
            self.history.append(entry)
            self.save_history()
        except InterruptedError:
            pr.stop()
            pr.black()
            e.send("SPIN 0")
            e.send("STATE IDLE")
            self.state, self.msg = "idle", "остановлено " + (self.stop_reason or "")
        except Exception as ex:  # noqa: BLE001
            log("print error:", ex, "| turn", j.get("turn"), "of", j.get("revs"))
            pr.stop()
            pr.black()
            e.send("SPIN 0")
            e.send(f"STATE ERROR {str(ex)[:40]}")
            self.state, self.msg = "error", str(ex)
        finally:
            done.set()
            self.wait = None
            self.save_settings()

    def _watch_light(self, j, done):
        """While the light is on: faults, progress for the screens. Returns dose numbers for the log."""
        e, pr = self.esp, self.proj
        total = j["revs"] * 360.0
        last_msg = 0
        while True:
            if self.stop_flag.is_set():
                raise InterruptedError
            w, rest = self._proj_event(0.1)
            if w == "DONE":
                f = rest.split()
                self.drops = int(f[1])
                j.update(turn=j["revs"], left_s=0, progress=1.0)
                e.send("PROG 100 0")
                return {"flips": int(f[0]), "missed_vsync": int(f[1]), "dose_min": int(f[2]), "dose_max": int(f[3]),
                        "dose_mean": float(f[4]), "fill_ms_max": float(f[5])}
            if w == "ERR":
                raise PrintError("проектор: " + rest[:60])
            if w == "STOPPED":
                raise InterruptedError
            now = time.monotonic()
            p = self.last_pos
            if p is None or now - p[4] > 1.0:
                raise PrintError("ESP не отвечает")
            if abs(p[1] - j["rpm"]) > 0.05 * j["rpm"]:
                raise PrintError(f"мотор: {p[1]:.2f} вместо {j['rpm']:.2f} об/мин")
            st = pr.stat
            if st:
                deg, _flips, missed, _fill, resid = st[:5]
                prog = max(0.0, min(1.0, deg / total))
                left = (total - deg) / (j["rpm"] * 6.0)
                self.drops, self.sync_ms = int(missed), resid
                j.update(turn=min(j["revs"], int(prog * j["revs"]) + 1), left_s=left, progress=prog)
                if now - last_msg > 0.5:
                    last_msg = now
                    e.send(f"PROG {100 * prog:.1f} {int(left + 0.99)}")
                    e.send(f"SYNC {resid:.0f}")
                    if self.drops:
                        e.send(f"DROPS {self.drops}")

    def _expose(self, j, drops0):
        e = self.esp
        total_deg = j.get("angle0", 0.0) + j["revs"] * 360.0  # light ends exactly N turns after it started
        T = j["rev_s"]
        err_f = None
        last_ok = time.monotonic()
        last_tpos, tpos_t = None, time.monotonic()
        last_msg = 0
        rpm_cmd = j["rpm"]
        while True:
            if self.stop_flag.is_set():
                raise InterruptedError
            p = e.query_pos()
            tpos = self.proj.get("time-pos")
            now = time.monotonic()
            # -- faults
            if p is None:
                if now - last_ok > 1.0:
                    raise PrintError("ESP не отвечает")
                continue
            last_ok = now
            angle, rpm, _, t_m = p[:4]
            self.motor_angle, self.motor_rpm = angle, rpm
            if abs(rpm - j["rpm"]) > 0.05 * j["rpm"]:
                raise PrintError(f"мотор: {rpm:.2f} вместо {j['rpm']:.2f} об/мин")
            if tpos is not None and tpos != last_tpos:
                last_tpos, tpos_t = tpos, now
            elif now - tpos_t > 1.0:
                raise PrintError("видео зависло")
            # -- end exactly on the turn boundary
            angle_now = angle + rpm * 6.0 * (now - t_m)
            remaining = total_deg - angle_now
            if remaining <= rpm * 6.0 * 0.4:
                time.sleep(max(0.0, remaining / (rpm * 6.0)))
                self.proj.black()
                j.update(turn=j["revs"], left_s=0, progress=1.0)
                e.send(f"PROG 100 0")
                return
            # -- phase lock: motor follows the video
            if tpos is not None:
                motor_phase = (angle_now / 360.0) % 1.0
                video_phase = (tpos % T) / T
                err = (video_phase - motor_phase + 0.5) % 1.0 - 0.5  # turns, + = motor behind
                err_s = err * T
                err_f = err_s if err_f is None else err_f * 0.6 + err_s * 0.4
                trim = max(-0.02, min(0.02, err_f / 2.0))  # close the gap in ~2 s, never more than 2 %
                new_cmd = j["rpm"] * (1 + trim)
                if abs(new_cmd - rpm_cmd) > 1e-4:
                    rpm_cmd = new_cmd
                    e.send(f"RPM {rpm_cmd:.4f}")
                self.sync_ms = abs(err_f) * 1000
            drops = (self.proj.get("frame-drop-count") or 0) + (self.proj.get("decoder-frame-drop-count") or 0)
            self.drops = max(0, drops - drops0)
            prog = max(0.0, min(1.0, (angle_now - j.get("angle0", 0.0)) / (j["revs"] * 360.0)))
            left = remaining / (j["rpm"] * 6.0)
            j.update(turn=min(j["revs"], int(prog * j["revs"]) + 1), left_s=left, progress=prog)
            if now - last_msg > 0.5:
                last_msg = now
                e.send(f"PROG {100 * prog:.1f} {int(left + 0.99)}")
                if self.sync_ms is not None:
                    e.send(f"SYNC {self.sync_ms:.0f}")
                if self.drops:
                    e.send(f"DROPS {self.drops}")
            time.sleep(0.1 if remaining > rpm * 6.0 * 1.0 else 0.02)

    def poll_idle(self):
        """Keep motor rpm/angle fresh for the web page when no print is running."""
        while True:
            self.expire_request()
            if not self.busy():
                p = self.esp.query_pos()
                if p:
                    self.motor_angle, self.motor_rpm = p[0], p[1]
            time.sleep(0.25)


# =============================================================================== network info
def my_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def wifi_name():
    for cmd in (["iwgetid", "-r"], ["nmcli", "-t", "-f", "active,ssid", "dev", "wifi"]):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=3).stdout
        except (OSError, subprocess.TimeoutExpired):
            continue
        for line in out.splitlines():
            if cmd[0] == "nmcli":
                if line.startswith("yes:"):
                    return line[4:]
            elif line.strip():
                return line.strip()
    return ""


# =============================================================================== mDNS (avahi)
AVAHI_DIR = "/etc/avahi/services"


def avahi_service(name, printer_id, port):
    """avahi static service: SpinSlicer browses _spinprinter._tcp and shows TXT name / id."""
    txt = "".join(f"\n    <txt-record>{xml_escape(t)}</txt-record>" for t in
                  (f"name={name}", f"id={printer_id}", f"api={API_VERSION}", "path=/api/printer"))
    return ("<?xml version=\"1.0\" standalone='no'?>\n"
            "<!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">\n"
            "<!-- written by SpinPi (spinpi.py) on every start: edits here are lost -->\n"
            "<service-group>\n"
            f"  <name replace-wildcards=\"yes\">{xml_escape(name)}</name>\n"
            "  <service>\n"
            "    <type>_spinprinter._tcp</type>\n"
            f"    <port>{int(port)}</port>{txt}\n"
            "  </service>\n"
            "</service-group>\n")


class Mdns:
    """Keeps /etc/avahi/services/spinprinter.service in step with the printer's name and id.
    avahi-daemon notices the change by itself; nothing happens where there is no avahi (a PC)."""

    def __init__(self, port, folder=AVAHI_DIR):
        self.port, self.folder = port, folder
        self.text = None
        self.warned = False

    def publish(self, name, printer_id):
        text = avahi_service(name, printer_id, self.port)
        if text == self.text or not self.folder or not os.path.isdir(os.path.dirname(self.folder.rstrip("/")) or "/"):
            return False
        path = os.path.join(self.folder, "spinprinter.service")
        try:
            os.makedirs(os.path.realpath(self.folder), exist_ok=True)  # SpinOS: a link into /run
            try:
                with open(path, encoding="utf-8") as f:
                    same = f.read() == text
            except OSError:
                same = False
            if not same:
                with open(path + ".tmp", "w", encoding="utf-8") as f:
                    f.write(text)
                os.replace(path + ".tmp", path)
                log("mDNS: _spinprinter._tcp", repr(name), printer_id, "port", self.port)
                try:
                    subprocess.run(["avahi-daemon", "--reload"], capture_output=True, timeout=5)
                except (OSError, subprocess.TimeoutExpired):
                    pass
        except OSError as e:
            if not self.warned:
                log("mDNS: cannot write", path, e)
                self.warned = True
            return False
        self.text = text
        return True


# =============================================================================== Wi-Fi (SpinOS)
class Net:
    """Home Wi-Fi or the printer's own access point. The work is done by the SpinOS script
    `spinnet`; on a PC or on Raspberry Pi OS there is no such script and this does nothing."""

    def __init__(self):
        self.exe = next((p for p in ("/usr/sbin/spinnet", os.environ.get("SPINNET", "")) if p and os.path.exists(p)), "")

    def _run(self, *args, timeout=20):
        if not self.exe:
            return ""
        try:
            return subprocess.run([self.exe, *args], capture_output=True, text=True, timeout=timeout).stdout
        except (OSError, subprocess.TimeoutExpired):
            return ""

    def status(self):
        """{"mode": "client"|"ap"|"none", "ssid": …, "ip": …, "ap_name": …}"""
        d = {"available": bool(self.exe), "mode": "none"}
        for line in self._run("status", timeout=5).splitlines():
            k, _, v = line.partition("=")
            if k:
                d[k.strip()] = v.strip()
        return d

    def scan(self):
        nets = []
        for line in self._run("scan").splitlines():
            sig, _, ssid = line.partition("\t")
            if ssid and not any(n["ssid"] == ssid for n in nets):
                nets.append({"ssid": ssid, "signal": float(sig or -100)})
        return sorted(nets, key=lambda n: -n["signal"])

    def connect(self, ssid, psk):
        if not self.exe:
            return "нет spinnet (это не SpinOS)"
        if not ssid or len(ssid) > 32 or (psk and not 8 <= len(psk) <= 63):
            return "имя сети до 32 символов, пароль 8–63 символа"
        # in the background: the phone that asked is about to lose the access point
        subprocess.Popen([self.exe, "connect", ssid, psk or ""], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
        return None

    def forget(self):
        if self.exe:
            subprocess.Popen([self.exe, "forget"], start_new_session=True)


# =============================================================================== web server
class Handler(BaseHTTPRequestHandler):
    app = None  # set in main()
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _json(self, obj, code=200):
        b = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(b)

    def _file(self, path, ctype):
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            return self._json({"error": "not found"}, 404)
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n > 1 << 20:
            return {}
        try:
            return json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return {}

    def do_GET(self):
        a = self.app
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path in ("/", "/index.html"):
            return self._file(os.path.join(HERE, "web", "index.html"), "text/html; charset=utf-8")
        if u.path == "/api/state":
            return self._json(a.printer.status())
        if u.path == "/api/files":
            return self._json(a.printer.annotate(a.lib.list()))
        if u.path == "/api/thumb":
            return self._file(*a.lib.thumb_file(q.get("id", [""])[0]))
        if u.path == "/api/printer":  # the printer profile for SpinSlicer
            return self._json(a.printer.profile())
        if u.path == "/api/history":  # every print with its plan, correction and "how did it come out"
            p = a.printer
            return self._json({"api": API_VERSION, "printer_id": p.settings["printer_id"],
                               "factor": p.settings["factor"], "prints": p.history})
        if u.path == "/api/wifi":
            return self._json(dict(a.net.status(), networks=a.net.scan() if q.get("scan") else []))
        if u.path == "/api/events":  # Server-Sent Events: live state twice a second
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            try:
                while True:
                    self.wfile.write(("data: " + json.dumps(a.printer.status(), ensure_ascii=False) + "\n\n").encode())
                    self.wfile.flush()
                    time.sleep(0.5)
            except OSError:
                return
        if a.ap_mode:  # captive portal: phones probe some URL on joining the printer's own Wi-Fi
            self.send_response(302)
            self.send_header("Location", "http://192.168.4.1/")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self._json({"error": "not found"}, 404)

    def do_PUT(self):  # upload: PUT /api/files/<name>, body = the video or the .spin file
        a = self.app
        u = urllib.parse.urlparse(self.path)
        if not u.path.startswith("/api/files/"):
            return self._json({"error": "not found"}, 404)
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n > 4 << 30:
            return self._json({"error": "bad size"}, 400)
        name = safe_name(u.path[len("/api/files/"):])
        dst = a.lib.lib_path(name)
        tmp = dst + ".part"
        left = n
        with open(tmp, "wb") as f:
            while left > 0:
                chunk = self.rfile.read(min(1 << 20, left))
                if not chunk:
                    break
                f.write(chunk)
                left -= len(chunk)
            f.flush()
            os.fsync(f.fileno())
        if left:
            os.unlink(tmp)
            return self._json({"ok": False, "error": "upload interrupted"}, 400)
        spin = name.lower().endswith(SPIN_EXT)
        if spin:
            try:
                read_spin(tmp)
            except SpinError as e:
                os.unlink(tmp)
                log("rejected", name, e)
                return self._json({"ok": False, "error": str(e)}, 400)
        os.replace(tmp, dst)
        a.lib.forget("lib/" + name)  # a new file under an old name: unpack it again
        info = a.lib.added(name)
        if info.get("error"):  # e.g. the video's SHA-256 does not match
            a.lib.remove("lib/" + name)
            log("rejected", name, info["error"])
            return self._json({"ok": False, "error": info["error"]}, 400)
        log("uploaded", name)
        self._json({"ok": True, "name": name, "info": a.printer.annotate([info])[0]})

    def do_DELETE(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/api/files":
            ok = self.app.lib.remove(q.get("id", [""])[0])
            return self._json({"ok": ok})
        self._json({"error": "not found"}, 404)

    def do_POST(self):
        a = self.app
        p = a.printer
        u = urllib.parse.urlparse(self.path)
        b = self._body()
        if u.path == "/api/print":  # "confirm": true = SpinSlicer: wait for a yes on the printer's screen
            args = (b.get("id", ""), b.get("revs"), b.get("factor"), b.get("delay_s"))
            err = p.ask(*args) if b.get("confirm") else p.start(*args)
            return self._json({"ok": err is None, "error": err, "request": p.request_view()},
                              409 if err == p.NO_SCREEN else 200)
        if u.path == "/api/plan":  # what a print would be with these settings (for the web form)
            f, _ = a.lib.resolve(b.get("id", ""))
            if not f or not f.get("rpm"):
                return self._json({"error": "no such video"}, 404)
            return self._json(p.plan_for(f, b.get("revs"), b.get("factor")))
        if u.path == "/api/result":
            pct = p.result(b.get("result", ""))
            return self._json({"ok": pct is not None, "factor_pct": pct})
        if u.path == "/api/settings":
            for k in ("warmup_min", "delay_s"):
                if k in b:
                    p.settings[k] = max(0.0, min(60.0, float(b[k])))
            p.save_settings()
            return self._json({"ok": True, "settings": p.settings})
        if u.path == "/api/printer":
            err = p.set_profile(b)
            return self._json({"ok": err is None, "error": err, "printer": p.profile()}, 200 if err is None else 400)
        if u.path == "/api/stop":
            p.stop()
            return self._json({"ok": True})
        if u.path == "/api/motor":
            err = p.motor(bool(b.get("on")), b.get("rpm"))
            return self._json({"ok": err is None, "error": err})
        if u.path == "/api/revs":
            a.lib.set_revs(b.get("id", ""), b.get("revs", 1))
            return self._json({"ok": True})
        if u.path == "/api/projector":
            err = p.show_pattern(b.get("pattern", "black"))
            return self._json({"ok": err is None, "error": err})
        if u.path == "/api/wifi":
            if b.get("forget"):
                a.net.forget()
                return self._json({"ok": True})
            err = a.net.connect(str(b.get("ssid", "")), str(b.get("psk", "")))
            return self._json({"ok": err is None, "error": err})
        if u.path == "/api/calibration":
            p.calibrate(b.get("offset_px"), b.get("vial_px"))
            if b.get("show"):
                p.show_pattern(b["show"])
            return self._json({"ok": True, "settings": p.settings})
        self._json({"error": "not found"}, 404)


class App:
    ap_mode = False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--data", default="/var/lib/spinpi")
    ap.add_argument("--serial", default="", help="ESP serial port (default: auto)")
    ap.add_argument("--projector", default="1280x800", help="projector resolution, or auto (SpinProj asks the HDMI)")
    ap.add_argument("--usb-dir", action="append", default=[], help="extra folder treated like a USB stick (tests)")
    ap.add_argument("--mpv-arg", action="append", default=[], help="extra mpv option, e.g. --mpv-arg=--vo=drm")
    ap.add_argument("--engine", default="angle", choices=["angle", "mpv"],
                    help="angle: SpinProj shows frames by the vial angle (default); mpv: the old video player")
    ap.add_argument("--spinproj", default="", help="path to spinproj (default: next to spinpi.py, then $PATH)")
    ap.add_argument("--spinproj-arg", action="append", default=[], help="extra spinproj option, e.g. --spinproj-arg=--rt")
    ap.add_argument("--hdmi", default="watch", choices=["watch", "ignore"],
                    help="ignore: do not look at the HDMI connector (tests on a PC)")
    ap.add_argument("--avahi-dir", default=AVAHI_DIR, help="where to put the mDNS service file ('' = do not)")
    args = ap.parse_args()
    os.makedirs(args.data, exist_ok=True)
    pw, ph = (1280, 800) if args.projector == "auto" else (int(v) for v in args.projector.lower().split("x"))

    app = App()
    app.usb = UsbWatcher(extra_dirs=args.usb_dir)
    app.lib = Library(args.data, app.usb.dirs)
    if args.engine == "angle":
        exe = args.spinproj or next((p for p in (os.path.join(HERE, "spinproj"), "/usr/bin/spinproj")
                                     if os.path.exists(p)), "spinproj")
        app.proj = AngleProjector(exe, ["--size", args.projector] + args.spinproj_arg)
        if args.projector == "auto" and app.proj.size:
            pw, ph = app.proj.size
    else:
        app.proj = Projector(os.path.join("/tmp", f"spinpi-mpv-{os.getpid()}.sock"), args.mpv_arg, pw)
    ref = {}
    app.esp = Esp(args.serial, lambda ev: ref["p"].on_esp_event(ev))
    app.warm = Warmup(watch=args.hdmi == "watch")
    app.net = Net()
    app.printer = Printer(app.esp, app.proj, app.lib, args.data, pw, ph, app.warm)
    ref["p"] = app.printer
    threading.Thread(target=app.printer.poll_idle, daemon=True).start()
    app.mdns = Mdns(args.port, args.avahi_dir)

    def publish_mdns():
        p = app.printer.profile()
        app.mdns.publish(p["name"], p["id"])

    app.printer.on_profile = publish_mdns

    def refresh_greeting():
        while True:
            ip = my_ip()
            host = socket.gethostname()
            url = f"http://{ip}" if args.port == 80 else f"http://{ip}:{args.port}"
            s = app.printer.settings
            st = app.net.status() if app.net.exe else {}
            app.ap_mode = st.get("mode") == "ap"
            if app.ap_mode:  # first start: the QR code joins the phone to the printer's own Wi-Fi
                name = st.get("ap_name", "SpinSlicer")
                g = [f"URL WIFI:T:nopass;S:{name};;", f"HOST 192.168.4.1", f"NET {name}",
                     f"CAL {s['offset_px']} {s['vial_px']}", f"MSG Wi-Fi: {name}"]
            else:
                g = [f"URL {url}", f"HOST {host}.local", f"NET {st.get('ssid') or wifi_name()}",
                     f"CAL {s['offset_px']} {s['vial_px']}"]
            if g != app.esp.greeting:
                app.esp.greeting = g
                app.esp.hello()
                log("address:", url, f"({host}.local)")
            publish_mdns()  # the name follows the host name until it is set on the web page
            time.sleep(15)

    threading.Thread(target=refresh_greeting, daemon=True).start()
    Handler.app = app
    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    srv.daemon_threads = True
    log(f"SpinPi {VERSION} on port {args.port}, data in {args.data}, projector: {args.engine}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
