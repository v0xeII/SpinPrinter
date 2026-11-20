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
import shutil
import socket
import subprocess
import sys
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
VERSION = "0.8.9"
VIDEO_EXT = (".mp4", ".mkv", ".mov", ".avi", ".webm")
SPIN_EXT = ".spin"  # SpinSlicer's one-file print: a stored ZIP with spin.json, video.mp4, preview.png
PRINT_EXT = VIDEO_EXT + (SPIN_EXT,)
SPIN_VERSION = 1  # the newest .spin version this SpinPi reads
API_VERSION = 1
# what this SpinPi can do, in GET /api/printer: a slicer checks before it relies on it
#   confirm  POST /api/print {"confirm": true} waits for a yes on the printer's screen (0.8.0)
#   job_seq  /api/state: job.seq and request.job_seq tell which print is which (0.8.2)
FEATURES = ["confirm", "job_seq"]
ASK_FIRMWARE = (0, 8, 0)  # the first SpinScreen that shows "Print?" (ASK)
ROTATIONS = ("ccw_from_top", "cw_from_top")


def version_tuple(text):
    """"0.8.1" / "0.8.1-test" -> (0, 8, 1); None when there is no version in it."""
    m = re.match(r"\s*v?(\d+)\.(\d+)(?:\.(\d+))?", text or "")
    return (int(m.group(1)), int(m.group(2)), int(m.group(3) or 0)) if m else None


def log(*a):
    """A log line. A character the console cannot show (×, a name in another script) must never stop
    the code that logs it: it is replaced, not raised."""
    try:
        print(time.strftime("%H:%M:%S"), *a, flush=True)
    except UnicodeEncodeError:
        line = " ".join([time.strftime("%H:%M:%S")] + [str(x) for x in a])
        enc = getattr(sys.stdout, "encoding", None) or "ascii"
        print(line.encode(enc, "replace").decode(enc, "replace"), flush=True)


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


PONG_RE = re.compile(r"PONG SpinScreen ([0-9]+\.[0-9]+(?:\.[0-9]+)?(?:[-+][0-9A-Za-z.]+)?)$")
PINS_RE = re.compile(r"SPINPINS ([0-9]+(?:,[0-9]+)+)( nvs)?$")


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
        self.pins = None  # the board's wiring (PINS -> SPINPINS <pins> [nvs])
        self.nvs = False  # the board keeps its wiring in its flash: a release build fits it
        self.pins_asked = None  # for which firmware version PINS was asked
        self.paused = threading.Event()  # set while a new firmware is written: the port is left alone
        self.events = queue.Queue()
        threading.Thread(target=self._run, daemon=True).start()
        threading.Thread(target=self._events, daemon=True).start()

    def _events(self):
        """EV lines one after another, in the order the board sent them. (A thread each let a burst of
        EV OFFSET from a held stick land out of order, the last one applied being an older value.)"""
        while True:
            ev = self.events.get()
            try:
                self.on_event(ev)
            except Exception as e:  # noqa: BLE001 - one bad event must not stop the rest
                log("ESP event failed:", ev, e)

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
            if self.paused.is_set():
                if self.ser is not None:
                    self._drop("paused (firmware update)")
                time.sleep(0.2)
                continue
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
                if time.monotonic() - last_ping > 1:  # not time.time(): NTP moves it after boot
                    self.send("PING")
                    last_ping = time.monotonic()
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
        cb = getattr(self, "on_hello", None)  # what else a freshly (re)started board must hear
        if cb:
            cb()

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
            # Only a whole answer says the version. Boards up to 0.8.5 lose the end of a line now and
            # then (the USB driver), and "PONG SpinScreOK" taken as the version made the update check
            # see no firmware, or ask for the wiring again in the middle of it.
            m = PONG_RE.match(line)
            if m:
                v = m.group(1)
                if v != getattr(self, "version", ""):  # another firmware: its wiring has to be asked again
                    self.pins = self.pins_asked = None
                self.version = v
        elif line.startswith("SPINPINS "):
            m = PINS_RE.match(line)  # a cut line must not become the board's wiring
            if m:
                self.pins = m.group(1)
                self.nvs = bool(m.group(2))
        elif line.startswith("READY"):  # ESP rebooted
            self.hello()
        elif line.startswith("CFG "):  # the answer to GET: handled with the events, in order
            self.events.put(line)
        elif line.startswith("EV "):
            if line == "EV STOP":  # never waits behind a slow event (a list of a big USB stick)
                threading.Thread(target=self.on_event, args=("STOP",), daemon=True).start()
            else:
                self.events.put(line[3:])

    def pause(self, timeout=3.0):
        """Let go of the serial port (esptool needs it). Returns the port the ESP was on."""
        port = self.port
        self.paused.set()
        end = time.monotonic() + timeout
        while self.ser is not None and time.monotonic() < end:
            time.sleep(0.05)
        return port

    def resume(self):
        self.connected = False
        self.paused.clear()

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
    # the centering shift the slicer already put into the frames. video.offset_px since SpinSlicer
    # 0.9; before, the same number sat in printer.projector.offset_px (and SpinPi shifted it again)
    baked = v.get("offset_px")
    if baked is None and isinstance(pr, dict) and isinstance(pr.get("projector"), dict):
        baked = pr["projector"].get("offset_px")
    if baked is not None:
        plan["offset_px"] = baked
    return plan


def resin_id(resin):
    """The resin a print is for: resin.id from SpinSlicer (0.9+), else made from its optics (absorption,
    refractive index), so older files of the same resin share it. None without resin data."""
    if not isinstance(resin, dict):
        return None
    if isinstance(resin.get("id"), str) and resin["id"].strip():
        return resin["id"].strip()[:40]
    a, n = resin.get("alpha_per_mm"), resin.get("n_resin")
    if not (_num(a) and _num(n)):
        return None
    return "r-" + hashlib.sha1(f"{float(a):.4f}/{float(n):.4f}".encode()).hexdigest()[:10]


def baked_offset_x(plan):
    """The horizontal centering shift already in the frames (spin.json / the MP4 tag), 0 if none."""
    v = plan.get("offset_px") if isinstance(plan, dict) else None
    if isinstance(v, (list, tuple)) and v:
        v = v[0]
    return int(v) if _num(v) and abs(v) <= 4000 else 0


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
        d["resin_id"] = resin_id(d["resin"])
        d["printer_profile_id"] = plan.get("printer_profile_id")
        d["baked_offset_px"] = baked_offset_x(plan)
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
        self.since = time.monotonic()  # unknown at start: assume it was switched on with the Raspberry
        self.connected = None
        self.on_s = 0.0  # seconds the projector was on (HDMI connected) not yet added to the counter
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
            if c:
                self.on_s += 1.0

    def take_on_s(self):
        s, self.on_s = self.on_s, 0.0
        return s

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
        # the vial calibration was really made (on the screen, the page or from the slicer): until
        # then offset 0 / width 400 are only defaults and the profile does not offer them as values
        self.settings.setdefault("calibrated", self.settings["vial_px"] != 400 or self.settings["offset_px"] != 0)
        self.settings.setdefault("prints", 0)
        self.settings.setdefault("projector_s", 0.0)  # how long the projector has been on, all in all
        self.counters_saved = time.monotonic()
        self.fw_offer = None  # a firmware for the board (firmware_info), offered on its screen
        self.fw_state = {"state": "idle", "msg": ""}  # idle | flashing | ok | error
        self.update = None  # what the last "Check for updates" found: {tag, version, pi, fw}
        self.shapes = {}  # print id -> SHAPE lines (or None): the part's silhouette for the screen
        self.upd_state = {"state": "idle", "msg": ""}  # idle | checking | none | available | updating | error
        # an update of the Raspberry ran before this start: say how it went once the board is there
        self.update_done = self.settings.pop("update_pending", None)
        if self.update_done:
            self.save_settings()
        self.history = load_json(self.history_path, [])
        self.on_profile = None  # called after the profile changes (mDNS name)
        self.state = "idle"  # idle | wait | print | done | error
        self.wait = None  # (seconds, kind) while waiting before the light
        self.job = None
        self.request = None  # the last print sent from SpinSlicer that waits for a yes on the screen
        self.board_dir_inv = None  # the board's DIR setting, as it last said (CFG)
        esp.on_hello = self.board_hello
        self.msg = ""
        self.sync_ms = None
        self.drops = 0
        self.stop_flag = threading.Event()
        self.stop_reason = ""  # who stopped the last print: shown on the page and in the log
        self.motor_rpm = 0.0
        self.motor_angle = 0.0
        self.pattern = "black"
        self.esp_list = []
        self.lock = threading.RLock()  # start / ask / answer: one at a time
        self.job_seq = 0  # +1 for every print started: /api/state says which print is which
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

    # The light can only go off on a whole turn. With frames by angle (SpinProj) the speed is free, and
    # the dose of a turn grows with its time: the fraction the slicer asked for (5.35 turns) is made
    # up by turning a little slower (5 turns at 4.00 * 5 / 5.35 = 3.74 rpm), so the dose is the plan's.
    SPEED_TRIM = 0.20          # at most this far from the video's own speed
    RPM_RANGE = (1.0, 30.0)    # what the board's motor does (config.h RPM_MIN / RPM_MAX)

    def speed_for(self, rpm, revs, need):
        """The speed that gives `need` turns' dose in `revs` turns, or None (out of range)."""
        if not self.angle_mode or not need or need <= 0 or revs < 1:
            return None
        r = rpm * revs / need
        if abs(r / rpm - 1.0) > self.SPEED_TRIM + 1e-9 or not (self.RPM_RANGE[0] <= r <= self.RPM_RANGE[1]):
            return None
        return r

    # "How did it come out?" moves a correction. For a file that names its resin it is that resin's
    # (another resin's prints are not moved by it); the general one (the screen's "Correction" row,
    # settings["factor"]) stays the user's. The light is plan x general x resin correction.
    def resin_factor(self, f):
        rid = f.get("resin_id") if isinstance(f, dict) else None
        return float((self.settings.get("resin_factors") or {}).get(rid, 1.0)) if rid else 1.0

    def plan_for(self, f, revs=None, factor=None):
        """revolutions to expose, the speed and time, and how far the dose is from the slicer's plan."""
        factor = float(self.settings["factor"] if factor is None else factor)
        rf = self.resin_factor(f)
        plan = f.get("plan_revs")
        need = plan * factor * rf if plan else None
        rpm = None
        if revs is None and need:
            near = max(1, round(need))  # the nearest whole number first, then its neighbours
            for n in sorted({near, max(1, near - 1), near + 1}, key=lambda n: (abs(n - need), n)):
                rpm = self.speed_for(f["rpm"], n, need)
                if rpm:
                    revs = n
                    break
            if revs is None:
                revs = near
        elif revs is None:
            revs = int(self.settings["default_revs"])
        revs = max(1, int(revs))
        if rpm is None and need:
            rpm = self.speed_for(f["rpm"], revs, need)
        exact = rpm is not None
        rpm = rpm or f["rpm"]
        rev_s = 60.0 / rpm
        d = {"revs": revs, "time_s": revs * rev_s, "rpm": rpm, "rev_s": rev_s, "dose_exact": exact,
             "speed_trim_pct": 100.0 * (rpm / f["rpm"] - 1.0), "factor": factor, "plan_revs": plan,
             "resin_id": f.get("resin_id"), "resin_factor": rf,
             "tolerance_pct": f.get("tolerance_pct"), "cadence_ok": self.cadence_ok(f.get("fps", 0)),
             "reversed": self.reversed(f), "other_printer": self.other_printer(f),
             "warnings": (f.get("warnings") or []) + ([self.size_warning(f)] if self.size_mismatch(f) else [])}
        if plan:
            # dose ~ turns x time of a turn: 0 when the speed made up the fraction
            d["dose_dev_pct"] = 100.0 * (revs * (f["rpm"] / rpm) / need - 1.0)
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

    def size_mismatch(self, f):
        """The video is not the projector's size: it would be scaled, and the part with it."""
        w, h = f.get("width"), f.get("height")
        return bool(w and h and self.pw and self.ph and (int(w), int(h)) != (int(self.pw), int(self.ph)))

    def size_warning(self, f):
        return (f"кадры {f['width']}×{f['height']}, проектор {self.pw}×{self.ph}: картинка растянется, "
                "размер детали и ширина колбы не совпадут. Нарежь под разрешение проектора")

    def offset_for(self, f):
        """The centering shift to apply: the printer's calibration minus what the frames already have."""
        return int(self.settings["offset_px"]) - int(f.get("baked_offset_px") or 0)

    def annotate(self, files):
        for f in files:
            f["reversed"] = self.reversed(f)
            f["other_printer"] = self.other_printer(f)
            f["size_mismatch"] = self.size_mismatch(f)
        return files

    # ---- the printer profile for SpinSlicer
    def profile(self):
        s, p = self.settings, self.settings["profile"]
        hz = getattr(self.proj, "hz", None) or s.get("display_fps")
        return {
            "api": API_VERSION, "spin_version": SPIN_VERSION, "features": FEATURES,
            "id": s["printer_id"], "name": s.get("printer_name") or socket.gethostname(),
            "projector": {"width": self.pw, "height": self.ph, "hz": hz,
                          "irradiance_mw_cm2": p.get("irradiance_mw_cm2"), "contrast": p.get("contrast")},
            "vial": {"diameter_mm": p.get("vial_diameter_mm"), "wall_mm": p.get("vial_wall_mm"),
                     "n_glass": p.get("vial_n")},
            # null until calibrated: a default is not a measurement, the slicer keeps its own
            "calibration": ({"offset_px": s["offset_px"], "offset_x_px": s["offset_px"],
                             "center_offset_px": [s["offset_px"], 0], "vial_width_px": s["vial_px"]}
                            if s.get("calibrated") else
                            {"offset_px": None, "offset_x_px": None, "center_offset_px": None, "vial_width_px": None}),
            "motor": {"gear": p.get("motor_gear"), "direction": self.motor_dir},
            "exposure": {"factor": s["factor"]},
            "software": {"spinpi": VERSION, "spinscreen": self.esp.version or None,
                         "engine": "angle" if self.angle_mode else "mpv"},
            "storage": {"free_gb": free_gb(self.data)},  # SpinSlicer checks there is room before sending
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
                if k == "motor_dir" and self.board_dir_inv is not None:
                    prof["motor_dir_inv"] = self.board_dir_inv
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
             "engine": "angle" if self.angle_mode else "mpv", "firmware": self.firmware_view(),
             "update": self.update_view()}
        if j:
            d["job"] = {k: j.get(k) for k in ("seq", "id", "file", "rpm", "revs", "time_s", "factor", "plan_revs",
                                              "reversed")}
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
        with self.lock:
            return self._ask(fid, revs, factor, delay_s)

    def _ask(self, fid, revs, factor, delay_s):
        err, f, _path, p = self._check(fid, revs, factor)
        if err:
            return err
        if not self.esp.connected:
            return self.NO_SCREEN
        fw = version_tuple(self.esp.version)
        if fw is not None and fw < ASK_FIRMWARE:  # an older board ignores ASK: nobody would ever be asked
            need = ".".join(map(str, ASK_FIRMWARE))
            return f"прошивка платы {self.esp.version} не спрашивает «Печатать?»: прошей плату {need} или новее"
        self.expire_request(replaced=True)
        name = f["name"].encode("utf-8")[:60].decode("utf-8", "ignore")
        lines = [f"ASK {int(p['revs'])} {int(round(p['time_s']))} {name}"]
        lines += [f"ASKW {w}"[:150] for w in self.ask_warnings(f, p)[:2]]
        self.request = {"state": "waiting_confirm", "id": fid, "name": f["name"], "revs": p["revs"],
                        "time_s": p["time_s"], "error": None, "t": time.monotonic(),
                        "args": (fid, revs, factor, delay_s), "lines": lines}
        self.send_shape(fid)
        for line in lines:
            self.esp.send(line)
        log("print request:", f["name"], "waits for a yes on the screen")
        return None

    def answer(self, yes):
        with self.lock:
            r = self.request
            if not r or r["state"] != "waiting_confirm":
                return
            if not yes:
                r["state"] = "cancelled"
                log("print request: cancelled on the screen")
                return
            r["state"] = "starting"
            err = self.start(*r["args"])
            if err is None:
                r["job_seq"] = self.job["seq"]
            r["state"], r["error"] = ("started", None) if err is None else ("error", err)
        if err:
            self.esp.send(f"STATE ERROR {err}")

    def cancel_request(self):
        """The web page's "Cancel" on a print that waits for its yes on the printer's screen."""
        with self.lock:
            r = self.request
            if not r or r["state"] != "waiting_confirm":
                return False
            r["state"] = "cancelled"
            self.esp.send("ASK END")
            log("print request: cancelled on the web page")
            return True

    def expire_request(self, replaced=False):
        """Drop a request nobody answered (or one a newer print replaces): the screen leaves it."""
        r = self.request
        if r and r["state"] == "waiting_confirm" and (replaced or time.monotonic() - r["t"] >= self.ASK_TIMEOUT_S):
            r["state"] = "replaced" if replaced else "expired"
            self.esp.send("ASK END")

    def request_view(self):
        r = self.request
        return {k: v for k, v in r.items() if k not in ("t", "args", "lines")} if r else None

    def ask_warnings(self, f, p):
        """What the screen should say next to "Print?": only SpinPi knows these."""
        out = []
        if f.get("size_mismatch") or self.size_mismatch(f):
            out.append(f"кадры {f['width']}×{f['height']}, проектор {self.pw}×{self.ph}")
        if p.get("reversed"):
            out.append("кадры пойдут в обратном порядке")
        if p.get("other_printer"):
            out.append("файл нарезан для другого принтера")
        tol = p.get("tolerance_pct") or 5.0
        if p.get("dose_dev_pct") is not None and not p.get("dose_exact") and abs(p["dose_dev_pct"]) > tol / 2:
            out.append(f"доза {p['dose_dev_pct']:+.0f}% от расчёта")
        return out

    def ask_again(self):
        """The board (re)started while a print waits for its yes: ask on the screen again. Without this
        the question was gone and the request waited 10 minutes for an answer nobody could give."""
        r = self.request
        if r and r["state"] == "waiting_confirm" and r.get("lines"):
            self.send_shape(r["id"])
            for line in r["lines"]:
                self.esp.send(line)

    # ---- which way the vial turns, and the gear: one place
    # The board's screen sets them (Service → Motor, the wizard). Which way "DIR 0" turns the vial
    # depends on how the motor is wired, so SpinPi keeps the real direction (motor_dir, also set on the
    # web page) and follows every flip made on the screen: EV CFG DIR <0|1> GEAR <g>, and the GET answer.
    def board_hello(self):
        """What a freshly (re)started board has to hear besides the greeting."""
        self.esp.send("GET")  # its direction and gear come back as a CFG line
        self.esp.send(f"MDIR {'ccw' if self.motor_dir == 'ccw_from_top' else 'cw'}")
        self.ask_again()

    def board_cfg(self, dir_inv=None, gear=None):
        prof = dict(self.settings["profile"])
        changed = False
        if dir_inv is not None:
            self.board_dir_inv = dir_inv
            base = prof.get("motor_dir_inv")
            if base is None:  # first time: this is what the direction on record goes with
                prof["motor_dir_inv"] = dir_inv
                changed = True
            elif dir_inv != base:  # flipped on the screen: the vial turns the other way now
                prof["motor_dir"] = "cw_from_top" if self.motor_dir == "ccw_from_top" else "ccw_from_top"
                prof["motor_dir_inv"] = dir_inv
                log("motor direction changed on the printer's screen:", prof["motor_dir"])
                changed = True
        if gear and gear > 0 and abs(float(prof.get("motor_gear") or 0) - gear) > 1e-6:
            prof["motor_gear"] = gear
            changed = True
        if changed:
            self.settings["profile"] = prof
            self.save_settings()
            if self.on_profile:
                self.on_profile()
        self.esp.send(f"MDIR {'ccw' if self.motor_dir == 'ccw_from_top' else 'cw'}")

    def start(self, fid, revs=None, factor=None, delay_s=None):
        # Two starts at once (a yes on the screen and Print on the page) both found the printer idle,
        # because the job thread set "wait" only once it ran: two threads drove one motor. Now the
        # check and "wait" happen together, under the lock.
        with self.lock:
            return self._start(fid, revs, factor, delay_s)

    def _start(self, fid, revs, factor, delay_s):
        err, f, path, p = self._check(fid, revs, factor)
        if err:
            return err
        self.expire_request(replaced=True)  # started some other way: the question on the screen goes
        delay_s = float(self.settings["delay_s"] if delay_s is None else delay_s)
        self.settings.update(factor=p["factor"], delay_s=delay_s, last_id=fid)
        if not f.get("plan_revs"):
            self.settings["default_revs"] = p["revs"]
        self.save_settings()
        self.job = dict(p, id=fid, file=f["name"], path=path, video_rev_s=f["rev_s"], fps=f.get("fps"),
                        delay_s=delay_s, frames_per_rev=f.get("frames_per_rev"), phase=f.get("frame0_angle_deg") or 0.0,
                        format=f.get("format", "video"), plan=f.get("plan") if isinstance(f.get("plan"), dict) else None,
                        resin=f.get("resin"), printer_profile_id=f.get("printer_profile_id"),
                        offset_px=self.offset_for(f), size_mismatch=self.size_mismatch(f))
        self.job_seq += 1
        self.job["seq"] = self.job_seq
        if self.job["size_mismatch"]:  # it still prints, but the screen says why the part may come out wrong
            log("print:", self.size_warning(f))
            self.esp.send(f"MSG Кадры {f['width']}×{f['height']}, проектор {self.pw}×{self.ph}: размер не тот")
        self.state, self.msg = "wait", ""  # busy from this moment, not from when the thread runs
        self.stop_flag.clear()
        self.stop_reason = ""
        threading.Thread(target=self._run_angle if self.angle_mode else self._run, name="spinpi-job",
                         daemon=True).start()
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
                "resin_id": j.get("resin_id"), "resin_factor": j.get("resin_factor", 1.0),
                "plan_revs": j["plan_revs"], "tolerance_pct": j["tolerance_pct"],
                "plan": {k: plan[k] for k in keep if plan.get(k) is not None} or None,
                "resin": j.get("resin"), "printer_profile_id": j.get("printer_profile_id"),
                "printer_id": self.settings["printer_id"], "reversed": j.get("reversed", False),
                "frame0_angle_deg": j.get("phase", 0.0), "engine": engine, "result": None}

    def result(self, verdict):
        """Feedback after a print: under / ok / over. Moves the correction for next time: the resin's
        when the print named one, else the general one. Returns the correction now, in %."""
        if not self.history or verdict not in ("under", "ok", "over"):
            return None
        h = self.history[-1]
        h["result"] = verdict
        h["factor_after"] = None  # filled below: the correction the next print will start from
        tol = h.get("tolerance_pct") or 5.0
        step = min(0.15, max(0.02, tol / 100.0))
        k = 1 + step if verdict == "under" else (1 - step if verdict == "over" else 1.0)
        rid = h.get("resin_id")
        if rid:
            rf = self.settings.setdefault("resin_factors", {})
            rf[rid] = round(max(0.3, min(3.0, float(rf.get(rid, 1.0)) * k)), 4)
            h["factor_after"] = rf[rid]
            pct = 100 * (rf[rid] - 1)
        else:
            self.settings["factor"] = round(max(0.3, min(3.0, self.settings["factor"] * k)), 4)
            h["factor_after"] = self.settings["factor"]
            pct = 100 * (self.settings["factor"] - 1)
        self.save_settings()
        self.save_history()
        return pct

    def reset_resin_factor(self, rid, value=1.0):
        """SpinSlicer took the correction into the resin's dose ("Apply"): it starts again from 1."""
        rf = self.settings.setdefault("resin_factors", {})
        rf[str(rid)[:40]] = round(max(0.3, min(3.0, float(value))), 4)
        self.save_settings()

    # A test picture (white, grid…) left on would cure the resin of the next print, or a vial left in:
    # it goes dark by itself after this long without being touched.
    PATTERN_OFF_S = 300.0

    def show_pattern(self, name):
        if self.busy():
            return "busy"
        self.pattern = name
        self.pattern_t = time.monotonic()
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
        if offset is not None or vial is not None:
            self.settings["calibrated"] = True
        self.save_settings()
        self.esp.send(f"CAL {self.settings['offset_px']} {self.settings['vial_px']}")
        if self.pattern != "black" and not self.busy():
            self.show_pattern(self.pattern)

    def send_list(self):
        every = self.lib.list()
        good = [f for f in every if f.get("rpm") and not f.get("error")]
        bad = [f for f in every if f.get("error")]  # shown greyed with why, not silently left out
        files = (good + bad)[:32]  # SpinScreen keeps 32
        self.esp_list = [f["id"] for f in files]
        s, e = self.settings, self.esp
        e.send(f"FL BEGIN {len(files)} {100 * (s['factor'] - 1):.0f} {s['delay_s']:.0f} {int(s['default_revs'])} "
               f"{1 if self.angle_mode else 0} {len(good) + len(bad)}")  # speed makes up a fraction; all files
        for i, f in enumerate(files):
            if f.get("error"):  # rpm 0 = cannot be printed; the name carries the reason
                why = f"{f['name']}: {f['error']}".encode("utf-8")[:150].decode("utf-8", "ignore")
                e.send(f"FL {i} 0.00 {1 if f['source'] == 'usb' else 0} -1.00 -1.0 0.000 0 {why}")
                continue
            name = f["name"].encode("utf-8")[:60].decode("utf-8", "ignore")
            plan = (f["plan_revs"] * self.resin_factor(f)) if f.get("plan_revs") else -1
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
            # EV PRINT <number in the last list sent> <turns> <correction %> <pause s>. A number this
            # SpinPi never sent (it restarted since the list, or the line came cut) prints nothing:
            # it used to print the last printed file instead, a part nobody chose.
            try:
                i = int(p[1]) if len(p) >= 5 and p[1].isdigit() else -1
                revs, factor, delay = int(float(p[2])), 1 + float(p[3]) / 100, float(p[4])
            except (ValueError, IndexError):
                i = -1
            if 0 <= i < len(self.esp_list):
                err = self.start(self.esp_list[i], revs=revs, factor=factor, delay_s=delay)
            else:
                err = "список файлов устарел: открой «Печать» заново"
            if err:
                self.esp.send(f"STATE ERROR {err}")
        elif p[0] in ("GO", "NO"):
            self.answer(p[0] == "GO")
        elif p[0] == "CFG":  # EV CFG DIR 0 GEAR 1.0000 (a change on the screen) / CFG RPM … DIR … GEAR … (GET)
            kv = dict(zip(p[1::2], p[2::2]))
            try:
                d = int(kv["DIR"]) if "DIR" in kv else None
                g = float(kv["GEAR"]) if "GEAR" in kv else None
            except ValueError:
                d = g = None
            self.board_cfg(d if d in (0, 1) else None, g)
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
        elif p[0] == "FWUPDATE":
            threading.Thread(target=self.firmware_update, daemon=True).start()
        elif p[0] == "UPDCHECK":
            threading.Thread(target=self.check_updates, daemon=True).start()
        elif p[0] == "UPDATE":
            threading.Thread(target=self.run_update, daemon=True).start()
        elif p[0] in ("REBOOT", "POWEROFF"):
            self.stop("перезагрузка" if p[0] == "REBOOT" else "выключение")
            self.proj.black()
            self.esp.send("MSG " + ("Перезагружаюсь…" if p[0] == "REBOOT" else "Выключаюсь… подожди 20 с"))
            if os.geteuid() == 0:
                time.sleep(1)
                subprocess.Popen(["systemctl", "reboot" if p[0] == "REBOOT" else "poweroff"])
            else:
                log("(not root: would", p[0].lower(), ")")

    def send_shape(self, fid, make=None):
        """The part's silhouette to the screen before a print (SHAPE -: none, a plain video)."""
        if fid not in self.shapes:
            png, kind = self.lib.thumb_file(fid)
            self.shapes[fid] = (make or part_shape)(png) if kind == "image/png" else None
        lines = self.shapes[fid]
        for line in lines or ["SHAPE -"]:
            self.esp.send(line)

    # ---- the Raspberry itself: what the System page shows, counters, the board's firmware
    def tick_counters(self):
        """Adds the projector's on-time; saved every 5 minutes (not every 10 s: the SD card)."""
        self.settings["projector_s"] = float(self.settings.get("projector_s") or 0) + self.warm.take_on_s()
        if time.monotonic() - self.counters_saved > 300:
            self.counters_saved = time.monotonic()
            self.save_settings()

    def system_info(self):
        usb = [g for g in (free_gb(d) for d in self.lib.usb_dirs_fn()) if g is not None]
        return {"ip": my_ip(), "wifi": wifi_signal(), "temp": cpu_temp(), "sd_free_gb": free_gb(self.data),
                "usb_free_gb": sum(usb) if usb else None, "prints": int(self.settings.get("prints") or 0),
                "projector_h": float(self.settings.get("projector_s") or 0) / 3600.0,
                "hdmi": getattr(self.warm, "connected", None)}

    def firmware_view(self):
        offer = {k: self.fw_offer[k] for k in ("version", "source")} if self.fw_offer else None
        return dict(self.fw_state, offer=offer)

    def refresh_firmware(self):
        """Looks for a firmware to offer and tells the screen (FW <version> <source> / FW -)."""
        if self.fw_state["state"] == "flashing":
            return
        f = pick_firmware(firmware_files(self.lib.usb_dirs_fn()), self.esp.version,
                          getattr(self.esp, "pins", None), getattr(self.esp, "nvs", False))
        self.fw_offer = f
        self.esp.send(f"FW {f['version']} {f['source']}" if f else "FW -")

    def firmware_update(self, flash=None, f=None):
        """EV FWUPDATE from the screen: writes the offered firmware with esptool, then waits for the board.
        Returns True when it was written."""
        f = f or self.fw_offer
        if self.busy():
            self.esp.send("MSG Сначала дождись конца печати")
            return False
        if not f or self.fw_state["state"] == "flashing":
            return False
        self.fw_state = {"state": "flashing", "msg": f["version"]}
        self.esp.send("FWUP")
        time.sleep(0.5)  # the screen draws "Updating…" before it stops answering
        port = self.esp.pause()
        try:
            ok, msg = (flash or flash_firmware)(f, port) if port else (False, "плата не найдена")
        finally:
            self.esp.resume()
        end = time.monotonic() + 25  # the board restarts with the new firmware
        while not self.esp.connected and time.monotonic() < end:
            time.sleep(0.2)
        self.esp.send("FWUP END")
        if ok:
            self.fw_state = {"state": "ok", "msg": f["version"]}
            self.esp.send(f"MSG Прошивка {self.esp.version or f['version']} установлена")
        else:
            self.fw_state = {"state": "error", "msg": msg}
            self.esp.send(f"MSG Прошивка не записана: {msg}"[:150])
        log("firmware update:", self.fw_state)
        self.fw_offer = None
        return ok

    # ---- updates over the air (Settings → System → "Check for updates")
    def check_updates(self, latest=None, fetch=None):
        """EV UPDCHECK: asks GitHub for the latest release. The Raspberry needs it when it is newer than
        this SpinPi; the board when it is newer than its firmware and the release's firmware fits it
        (downloaded here to check). Tells the screen: UPD AVAIL <version> / UPD NONE / UPD ERR <why>."""
        if self.busy():
            self.esp.send("UPD ERR Сначала дождись конца печати")
            return
        self.upd_state = {"state": "checking", "msg": ""}
        self.esp.send("UPD CHECKING")
        try:
            self.esp.send("UPD STEP Спрашиваю GitHub…")  # the screen shows each step, so it is not silent
            tag = (latest or latest_release)()
            ver = tag.lstrip("v")
            new = version_tuple(ver)
            pi = bool(new and new > (version_tuple(VERSION) or (0,)))
            fw, fw_blocked = None, False
            have = version_tuple(self.esp.version)
            if new and have and new > have:
                os.makedirs(os.path.join(self.data, "fw"), exist_ok=True)
                path = os.path.join(self.data, "fw", f"spinscreen-{tag}-merged.bin")
                if not os.path.exists(path):
                    self.esp.send(f"UPD STEP Скачиваю прошивку {ver}…")
                    (fetch or download)(release_firmware_url(tag), path)
                info = firmware_info(path, "release")
                if not info or not info.get("app_len"):  # a broken file (cut download): fetch it again
                    os.remove(path)
                    self.esp.send(f"UPD STEP Скачиваю прошивку {ver}…")
                    (fetch or download)(release_firmware_url(tag), path)
                    info = firmware_info(path, "release")
                if self.esp.pins is None:  # not known yet: ask now rather than say "does not fit"
                    self.esp.send("PINS")
                    end = time.monotonic() + 3
                    while self.esp.pins is None and time.monotonic() < end:
                        time.sleep(0.1)
                if firmware_fits(info, self.esp.pins, self.esp.nvs):
                    fw = info
                else:
                    fw_blocked = True
        except Exception as e:  # noqa: BLE001 - no internet, GitHub down, a bad file
            log("update check:", e)
            self.upd_state = {"state": "error", "msg": str(e)[:100]}
            self.esp.send("UPD ERR Нет связи с GitHub")
            return
        self.update = {"tag": tag, "version": ver, "pi": pi, "fw": fw}
        if pi or fw:
            self.upd_state = {"state": "available", "msg": ver}
            self.esp.send(f"UPD AVAIL {ver}")
        elif fw_blocked:
            self.upd_state = {"state": "error", "msg": "the release firmware does not fit the board's wiring"}
            self.esp.send("UPD ERR Прошивку платы обнови с компьютера")
        else:
            self.upd_state = {"state": "none", "msg": ver}
            self.esp.send("UPD NONE")

    def run_update(self, flash=None, run=None, watch=True):
        """EV UPDATE: first the board's firmware (SpinPi writes it), then the Raspberry itself (get.sh of
        the release, outside this service: its install restarts SpinPi)."""
        u = self.update
        if not u or self.busy() or self.upd_state["state"] == "updating":
            return
        self.upd_state = {"state": "updating", "msg": u["version"]}
        if u["fw"] and not self.firmware_update(flash=flash, f=u["fw"]):
            if not u["pi"]:
                self.upd_state = {"state": "error", "msg": self.fw_state.get("msg", "")}
                self.esp.send("UPD ERR Прошивка платы не записана")
                return
            # the Raspberry still goes on: a newer SpinPi may write the board where this one could not
            # ("Check for updates" again after it restarts)
            log("board firmware failed, updating the Raspberry anyway")
        if not u["pi"]:
            self.update = None
            self.upd_state = {"state": "none", "msg": u["version"]}
            self.esp.send("UPD IDLE")
            return
        self.update_raspberry(u["tag"], run=run, watch=watch)

    def update_raspberry(self, tag, run=None, watch=True):
        root = hasattr(os, "geteuid") and os.geteuid() == 0
        if not root and not run:
            self.upd_state = {"state": "error", "msg": "not root"}
            self.esp.send("UPD ERR SpinPi запущен не от root")
            return
        self.settings["update_pending"] = tag
        self.save_settings()
        self.esp.send("UPD RUN pi")
        try:
            (run or subprocess.run)(raspberry_update_cmd(tag), check=True, timeout=30)
        except Exception as e:  # noqa: BLE001
            self.settings.pop("update_pending", None)
            self.save_settings()
            self.upd_state = {"state": "error", "msg": str(e)[:100]}
            self.esp.send("UPD ERR Не удалось запустить обновление")
            return
        if watch:
            threading.Thread(target=self._watch_update, daemon=True).start()

    def _watch_update(self):
        """get.sh restarts SpinPi when it succeeds. Still here after it ended = it failed."""
        time.sleep(5)
        while True:
            r = subprocess.run(["systemctl", "is-active", "spinprinter-update"], capture_output=True, text=True)
            if r.stdout.strip() not in ("active", "activating"):
                break
            time.sleep(3)
        time.sleep(20)  # a successful install restarts us within these seconds
        self.settings.pop("update_pending", None)
        self.save_settings()
        try:
            tail = open("/var/log/spinprinter-update.log", encoding="utf-8", errors="replace").read().strip()
            tail = tail.splitlines()[-1][:80] if tail else ""
        except OSError:
            tail = ""
        log("Raspberry update failed:", tail)
        self.upd_state = {"state": "error", "msg": tail}
        self.esp.send("UPD ERR Raspberry не обновилась" + (f": {tail}" if tail else ""))

    def update_view(self):
        u = self.update
        return dict(self.upd_state, available={"version": u["version"], "raspberry": u["pi"],
                                               "board": bool(u["fw"])} if u else None)

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
            self.send_shape(j["id"])
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
            self.proj.play_loop(j["path"], 1.0, j["offset_px"])
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
        """Ask the ESP for the angle 20 times a second and hand it to SpinProj, for the whole job.
        A lost answer is asked again at once (SpinProj keeps the light on from its own prediction
        for up to SpinProj's ANGLE_LOST_NS, but no longer)."""
        while not done.is_set():
            p = self.esp.query_pos()
            if p:
                a, rpm, at_speed, t_mid, rtt = p
                self.last_pos = (a, rpm, at_speed, t_mid, time.monotonic())
                self.motor_angle, self.motor_rpm = a, rpm
                self.proj.angle(a, rpm, int(t_mid * 1e9), int(rtt * 1e9))
                time.sleep(0.05)
            else:
                time.sleep(0.005)  # no answer within 0.15 s: again now, not 50 ms later

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
            frames = j.get("frames_per_rev") or max(1, round(j["video_rev_s"] * (j.get("fps") or 0)))
            if j.get("reversed"):
                log("the video turns the other way than the motor: frames go backwards")
            # unpacks while the motor spins up
            pr.load(j["path"], frames, j["offset_px"], reverse=j.get("reversed"), phase_deg=j.get("phase", 0.0))
            threading.Thread(target=self._feed_angles, args=(done,), daemon=True).start()
            self.send_shape(j["id"])
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

    def pattern_timeout(self):
        if self.pattern != "black" and not self.busy() and \
                time.monotonic() - getattr(self, "pattern_t", time.monotonic()) > self.PATTERN_OFF_S:
            log("test picture left on: dark again")
            self.show_pattern("black")
            self.esp.send("MSG Тестовая картинка погашена")

    def poll_idle(self):
        """Keep motor rpm/angle fresh for the web page when no print is running."""
        while True:
            self.expire_request()
            self.pattern_timeout()
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


def wifi_signal():
    """Wi-Fi link quality in % (0..100), None without Wi-Fi. /proc/net/wireless: 'wlan0: 0000   52.  -58. ...'"""
    try:
        lines = open("/proc/net/wireless").read().splitlines()[2:]
    except OSError:
        return None
    for line in lines:
        p = line.split()
        if len(p) >= 3:
            try:
                return max(0, min(100, round(float(p[2].rstrip(".")) * 100 / 70)))
            except ValueError:
                continue
    return None


def cpu_temp():
    """CPU temperature, °C, or None (not a Raspberry)."""
    try:
        return int(open("/sys/class/thermal/thermal_zone0/temp").read().strip()) / 1000.0
    except (OSError, ValueError):
        return None


def free_gb(path):
    try:
        return shutil.disk_usage(path).free / 1e9
    except OSError:
        return None


def sys_line(info):
    """The SYS line for the printer's screen (System page). '-' = not known."""
    def f(v, fmt):
        return "-" if v is None else fmt.format(v)
    return (f"SYS ip={info.get('ip') or '-'} wifi={f(info.get('wifi'), '{:d}')} temp={f(info.get('temp'), '{:.0f}')} "
            f"sd={f(info.get('sd_free_gb'), '{:.1f}')} usb={f(info.get('usb_free_gb'), '{:.1f}')} "
            f"prints={f(info.get('prints'), '{:d}')} proj={f(info.get('projector_h'), '{:.1f}')} "
            f"hdmi={f(None if info.get('hdmi') is None else int(bool(info['hdmi'])), '{:d}')}")


# =============================================================================== the part's silhouette
# The printer's screen draws the part inside the progress ring (SHAPE lines). It comes from the .spin
# preview (preview.png): its background goes (transparent pixels, or the colour of its corners), the
# empty edges are cut and what is left is fitted into SHAPE_SIZE x SHAPE_SIZE pixels, 1 bit each.
SHAPE_SIZE = 56
SHAPE_SRC = 128  # ffmpeg scales the preview to this first (kept in proportion, padded)


def shape_mask(rgba, w, h):
    """Which pixels belong to the part: list of rows of bools."""
    px = [rgba[i:i + 4] for i in range(0, w * h * 4, 4)]
    if any(p[3] < 250 for p in px):  # a transparent background
        return [[px[y * w + x][3] > 128 for x in range(w)] for y in range(h)]
    corners = [px[0], px[w - 1], px[(h - 1) * w], px[h * w - 1]]
    bg = [sum(c[i] for c in corners) / 4 for i in range(3)]
    return [[sum(abs(px[y * w + x][i] - bg[i]) for i in range(3)) > 60 for x in range(w)] for y in range(h)]


def shape_lines(mask, size=SHAPE_SIZE):
    """The SHAPE lines for the screen, or None (nothing, or everything, is the part)."""
    h, w = len(mask), len(mask[0]) if mask else 0
    ys = [y for y in range(h) if any(mask[y])]
    xs = [x for x in range(w) if any(mask[y][x] for y in range(h))]
    if not ys or not xs:
        return None
    y0, y1, x0, x1 = ys[0], ys[-1] + 1, xs[0], xs[-1] + 1
    bw, bh = x1 - x0, y1 - y0
    if bw * bh >= w * h * 0.98 and bw == w:  # no background found: not a silhouette
        return None
    k = max(bw, bh) / size  # source pixels per screen pixel, the same both ways
    ow, oh = max(1, round(bw / k)), max(1, round(bh / k))
    ox, oy = (size - ow) // 2, (size - oh) // 2
    bpr = (size + 7) // 8
    rows = [bytearray(bpr) for _ in range(size)]
    for ty in range(oh):
        for tx in range(ow):
            sx0, sy0 = x0 + int(tx * k), y0 + int(ty * k)
            sx1, sy1 = max(sx0 + 1, x0 + int((tx + 1) * k)), max(sy0 + 1, y0 + int((ty + 1) * k))
            cells = [mask[y][x] for y in range(sy0, min(sy1, y1)) for x in range(sx0, min(sx1, x1))]
            if cells and sum(cells) >= 0.35 * len(cells):
                X, Y = ox + tx, oy + ty
                rows[Y][X >> 3] |= 0x80 >> (X & 7)
    out = [f"SHAPE BEGIN {size} {size}"]
    for y in range(0, size, 8):
        out.append(f"SHAPE {y} " + "".join(rows[r].hex().upper() for r in range(y, min(y + 8, size))))
    out.append("SHAPE END")
    return out


def part_shape(png, size=SHAPE_SIZE):
    """SHAPE lines from a preview image (ffmpeg decodes it), or None."""
    if not png or not os.path.exists(png):
        return None
    vf = (f"scale={SHAPE_SRC}:{SHAPE_SRC}:force_original_aspect_ratio=decrease,"
          f"pad={SHAPE_SRC}:{SHAPE_SRC}:(ow-iw)/2:(oh-ih)/2:color=black@0,format=rgba")
    try:
        r = subprocess.run(["ffmpeg", "-v", "error", "-i", png, "-vf", vf, "-frames:v", "1", "-f", "rawvideo",
                            "-pix_fmt", "rgba", "-"], capture_output=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if r.returncode != 0 or len(r.stdout) < SHAPE_SRC * SHAPE_SRC * 4:
        return None
    return shape_lines(shape_mask(r.stdout[:SHAPE_SRC * SHAPE_SRC * 4], SHAPE_SRC, SHAPE_SRC), size)


# =============================================================================== the board's firmware
# A firmware for the board comes from a USB stick (any spinscreen*.bin: your own build) or from a
# GitHub release (Settings → System → "Check for updates" downloads it). It goes on only if it fits
# the board's wiring:
#  * a release build (SPINBUILD release, 0.8.5 on) takes the wiring the board keeps in its flash, so
#    it fits every board that keeps one (its PINS answer ends with "nvs");
#  * any other build has its pins compiled in: it must have exactly the board's pins.
# Only the app part is written (at 0x10000): the board's flash keeps its settings and its wiring.
REPO = os.environ.get("SPINPRINTER_REPO", "v0xeII/SpinPrinter")
APP_OFFSET = 0x10000


def esp_image_len(data):
    """Length of an ESP32 app image at the start of data (header, segments, checksum, SHA-256), or None."""
    if len(data) < 24 or data[0] != 0xE9:
        return None
    off = 24
    for _ in range(data[1]):
        if off + 8 > len(data):
            return None
        seg_len = int.from_bytes(data[off + 4:off + 8], "little")
        off += 8 + seg_len
    off += 16 - off % 16  # padding, the checksum in the last byte of the 16-byte block
    if data[23] == 1:  # hash appended
        off += 32
    return off if off <= len(data) else None


def firmware_info(path, source):
    """What a firmware file says about itself: version, wiring, build kind, and its app part."""
    try:
        with open(path, "rb") as fh:
            data = fh.read(16 << 20)
    except OSError:
        return None
    m = re.search(rb"PONG SpinScreen ([0-9][0-9A-Za-z.+\-]*)", data)
    if not m:
        return None  # not a SpinScreen firmware
    pins = re.search(rb"SPINPINS ([0-9,]+)\x00", data)  # the compiled-in pins (not "SPINPINS %s nvs")
    build = re.search(rb"SPINBUILD (\w+)\x00", data)
    merged = len(data) > APP_OFFSET + 24 and data[0x8000:0x8002] == b"\xaa\x50"  # a partition table at 0x8000
    app = data[APP_OFFSET:] if merged else data
    n = esp_image_len(app)
    return {"path": path, "source": source, "version": m.group(1).decode(), "merged": merged,
            "pins": pins.group(1).decode().strip() if pins else None,
            "build": build.group(1).decode() if build else "local",
            "app_offset": APP_OFFSET if merged else 0, "app_len": n, "size": len(data)}


def firmware_fits(f, esp_pins, esp_nvs):
    """True when f can go onto a board with this wiring (see above)."""
    if not f or not esp_pins:
        return False
    if f.get("build") == "release" and esp_nvs:
        return True
    return f.get("pins") == esp_pins


def firmware_files(usb_dirs):
    """The SpinScreen firmwares on the USB sticks (top folder, named spinscreen*.bin)."""
    out = []
    for d in usb_dirs:
        for p in sorted(glob.glob(os.path.join(d, "*.bin"))):
            if "spinscreen" in os.path.basename(p).lower():
                out.append(firmware_info(p, "usb"))
    return [i for i in out if i]


def pick_firmware(files, esp_version, esp_pins, esp_nvs=False):
    """The stick firmware to offer on the screen, or None: one that fits the board and is another
    version than the board's (you put it there on purpose)."""
    if not esp_version:
        return None
    for f in files:
        if firmware_fits(f, esp_pins, esp_nvs) and f["version"] != esp_version:
            return f
    return None


def esptool_cmd():
    for exe in ("esptool", "esptool.py"):
        p = shutil.which(exe)
        if p:
            return [p]
    try:
        import importlib.util
        if importlib.util.find_spec("esptool"):
            return [sys.executable, "-m", "esptool"]
    except (ImportError, ValueError):
        pass
    return None


def flash_firmware(fw, port, timeout=240, run=None):
    """Write the app part of fw (firmware_info) at 0x10000 of the ESP32-S3 on port. Returns (ok, message).
    Debian's esptool (apt install esptool) comes without its flasher stubs ("No such file ...
    stub_flasher/stub_flasher_32s3.json"): then it is done again with --no-stub, through the chip's
    own ROM loader (slower, the same result)."""
    run = run or subprocess.run
    cmd = esptool_cmd()
    if not cmd:
        return False, "нет esptool: sudo apt install esptool"
    if not fw.get("app_len"):
        return False, "файл не похож на прошивку ESP32"
    with open(fw["path"], "rb") as fh:
        fh.seek(fw["app_offset"])
        app = fh.read(fw["app_len"])
    import tempfile
    tmp = os.path.join(tempfile.gettempdir(), f"spinscreen-app-{os.getpid()}.bin")
    with open(tmp, "wb") as fh:
        fh.write(app)
    try:
        for extra in ([], ["--no-stub"]):
            args = cmd + ["--chip", "esp32s3", "--port", port, "--baud", "460800", "--before", "default_reset",
                          "--after", "hard_reset"] + extra + ["write_flash", hex(APP_OFFSET), tmp]
            log("firmware:", " ".join(args))
            try:
                r = run(args, capture_output=True, text=True, timeout=timeout)
            except (OSError, subprocess.TimeoutExpired) as e:
                return False, f"esptool: {e}"
            out = (r.stdout + r.stderr).strip()
            log("esptool:", out[-600:])
            if r.returncode == 0:
                return True, "ok"
            if "stub_flasher" not in out:  # failed for another reason: --no-stub would not help
                break
        return False, (out.splitlines() or ["esptool failed"])[-1][:120]
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass


# ---- updates over the air: what GitHub has
def latest_release(timeout=15):
    """The tag of the latest release (github.com/<repo>/releases/latest redirects to .../tag/<tag>)."""
    import urllib.request
    req = urllib.request.Request(f"https://github.com/{REPO}/releases/latest", method="HEAD",
                                 headers={"User-Agent": "SpinPi"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        url = r.geturl()
    if "/releases/tag/" not in url:
        raise OSError("на GitHub нет релиза")
    return urllib.parse.unquote(url.rsplit("/releases/tag/", 1)[1])


def download(url, dst, timeout=60):
    import urllib.request
    tmp = dst + ".part"
    req = urllib.request.Request(url, headers={"User-Agent": "SpinPi"})
    with urllib.request.urlopen(req, timeout=timeout) as r, open(tmp, "wb") as fh:
        shutil.copyfileobj(r, fh)
    os.replace(tmp, dst)
    return dst


def release_firmware_url(tag):
    return f"https://github.com/{REPO}/releases/download/{tag}/spinscreen-{tag}-merged.bin"


def raspberry_update_cmd(tag):
    """Runs the release's get.sh outside the spinpi service (its install restarts spinpi), no reboot."""
    script = (f"curl -fsSL https://raw.githubusercontent.com/{REPO}/{tag}/SpinPi/get.sh | "
              f"SPINPRINTER_REF={tag} SPINPRINTER_NO_REBOOT=1 bash >/var/log/spinprinter-update.log 2>&1")
    return ["systemd-run", "--unit=spinprinter-update", "--collect", "--quiet", "/bin/bash", "-c", script]


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
            os.makedirs(os.path.realpath(self.folder), exist_ok=True)
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

    PRINTER_HOST = "spinprinter.local"

    def alias(self, ip, hostname):
        """The printer's own name, spinprinter.local, also on a Raspberry set up under another host name
        (before 0.8.9 the SD card named it spinslicer): a line in avahi's hosts file, kept on its address."""
        if not self.folder or not ip or ip.startswith("127.") or hostname.lower() in ("spinprinter", ""):
            return False
        path = os.path.join(os.path.dirname(self.folder.rstrip("/")), "hosts")  # /etc/avahi/hosts
        if not os.path.isdir(os.path.dirname(path)):
            return False
        want = f"{ip} {self.PRINTER_HOST}"
        try:
            with open(path, encoding="utf-8") as f:
                lines = f.read().splitlines()
        except OSError:
            lines = []
        keep = [ln for ln in lines if not (ln.split() and ln.split()[-1] == self.PRINTER_HOST)]
        if want in lines and len(keep) == len(lines) - 1:
            return False  # already there, once
        try:
            with open(path + ".tmp", "w", encoding="utf-8") as f:
                f.write("\n".join(keep + [want]) + "\n")
            os.replace(path + ".tmp", path)
            log("mDNS: also", self.PRINTER_HOST, "->", ip)
            subprocess.run(["avahi-daemon", "--reload"], capture_output=True, timeout=5)
        except (OSError, subprocess.TimeoutExpired) as e:
            if not self.warned:
                log("mDNS: cannot write", path, e)
                self.warned = True
            return False
        return True


# =============================================================================== web server
def error_code(err):
    """A short code for a refusal, next to its text: SpinSlicer shows its own words for it, in its language."""
    if not err:
        return None
    e = str(err)
    for start, code in (("busy", "busy"), ("no such video", "no_file"), ("projector not connected", "no_hdmi"),
                        ("видео для другого направления", "reversed_mpv"), ("прошивка платы", "old_board"),
                        ("видео в .spin повреждено", "bad_file"), ("unknown host name", "foreign"),
                        ("request from another site", "foreign"), ("Content-Type", "foreign")):
        if e.startswith(start):
            return code
    if e == Printer.NO_SCREEN:
        return "no_screen"
    return "other"


class BadRequest(ValueError):
    """A request that does not make sense: 400 with this text, never a dropped connection."""


def host_ok(host, own_names=()):
    """The Host header names this printer the way a person or SpinSlicer does: an IP address, a .local
    name, localhost or the machine's own name. Anything else is a page from the internet that made
    its domain point at this printer (DNS rebinding) and must not drive it."""
    h = (host or "").strip().lower()
    if h.startswith("["):  # [IPv6]:port
        h = h[1:h.find("]")] if "]" in h else h
    elif h.count(":") == 1:
        h = h.rsplit(":", 1)[0]
    if not h:
        return True  # HTTP/1.0 without Host: a local tool
    try:
        import ipaddress
        ipaddress.ip_address(h)
        return True
    except ValueError:
        pass
    return h == "localhost" or h.endswith(".local") or h in {n.lower() for n in own_names if n}


def foreign_request(method, headers, own_names=()):
    """Why a request that changes something must be refused (None = it may go on).

    A page open in a browser on the same network could otherwise start a print or turn the light on:
    a cross-site fetch() with a "simple" body needs no permission. The printer's own page and
    SpinSlicer send JSON from the same origin (or no Origin at all), so they pass."""
    if not host_ok(headers.get("Host"), own_names):
        return "unknown host name"
    site = (headers.get("Sec-Fetch-Site") or "").lower()
    if site in ("cross-site", "same-site"):
        return "request from another site"
    origin = headers.get("Origin")
    if origin == "null":
        return "request from another site"
    if origin:
        o = urllib.parse.urlparse(origin)
        if o.netloc.lower() != (headers.get("Host") or "").lower():
            return "request from another site"
    if method == "POST":
        ctype = (headers.get("Content-Type") or "").split(";")[0].strip().lower()
        if ctype != "application/json":
            return "Content-Type must be application/json"
    return None


def num(b, key, default=None, lo=None, hi=None, kind=float):
    """b[key] as a number (or default when absent / null); BadRequest on text, lists and the like."""
    v = b.get(key, default)
    if v is None:
        return None
    if isinstance(v, bool) or not isinstance(v, (int, float, str)):
        raise BadRequest(f"{key}: нужно число")
    try:
        x = float(v)
    except (TypeError, ValueError):
        raise BadRequest(f"{key}: нужно число")
    if x != x or x in (float("inf"), float("-inf")):
        raise BadRequest(f"{key}: нужно число")
    if lo is not None:
        x = max(lo, x)
    if hi is not None:
        x = min(hi, x)
    return int(round(x)) if kind is int else x


def text(b, key, default=""):
    v = b.get(key, default)
    if v is None:
        return default
    if not isinstance(v, str):
        raise BadRequest(f"{key}: нужна строка")
    return v


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

    def _length(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            raise BadRequest("bad Content-Length")
        if n < 0:
            raise BadRequest("bad Content-Length")
        return n

    def _body(self):
        """The JSON object of a POST. Not JSON, or not an object ([1, 2], "x") = BadRequest."""
        n = self._length()
        if n > 1 << 20:
            raise BadRequest("body too large")
        raw = self.rfile.read(n) if n else b""
        try:
            b = json.loads(raw or b"{}")
        except ValueError:
            raise BadRequest("body is not JSON")
        if not isinstance(b, dict):
            raise BadRequest("body must be a JSON object")
        return b

    def _guarded(self, method, fn):
        """Refuse requests from other sites; answer a bad request with 400 instead of a dropped line."""
        why = foreign_request(method, self.headers, (socket.gethostname(), socket.gethostname() + ".local"))
        if why:
            log("refused", method, self.path, "-", why, self.headers.get("Origin") or "")
            self.close_connection = True  # the body (if any) is not read
            return self._json({"ok": False, "error": why, "code": "foreign"},
                              415 if why.startswith("Content-Type") else 403)
        try:
            return fn()
        except BadRequest as e:
            self.close_connection = True
            return self._json({"ok": False, "error": str(e), "code": "bad_request"}, 400)

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
                               "factor": p.settings["factor"], "resin_factors": p.settings.get("resin_factors") or {},
                               "prints": p.history})
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
        self._json({"error": "not found"}, 404)

    def do_PUT(self):
        return self._guarded("PUT", self._put)

    def do_DELETE(self):
        return self._guarded("DELETE", self._delete)

    def do_POST(self):
        return self._guarded("POST", self._post)

    def _put(self):  # upload: PUT /api/files/<name>, body = the video or the .spin file
        a = self.app
        u = urllib.parse.urlparse(self.path)
        if not u.path.startswith("/api/files/"):
            return self._json({"error": "not found"}, 404)
        n = self._length()
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

    def _delete(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/api/files":
            ok = self.app.lib.remove(q.get("id", [""])[0])
            return self._json({"ok": ok})
        self._json({"error": "not found"}, 404)

    def _post(self):
        a = self.app
        p = a.printer
        u = urllib.parse.urlparse(self.path)
        b = self._body()
        if u.path == "/api/print":  # "confirm": true = SpinSlicer: wait for a yes on the printer's screen
            args = (text(b, "id"), num(b, "revs", lo=1, hi=100000, kind=int), num(b, "factor", lo=0.05, hi=20),
                    num(b, "delay_s", lo=0, hi=600))
            err = p.ask(*args) if b.get("confirm") else p.start(*args)
            return self._json({"ok": err is None, "error": err, "code": error_code(err), "request": p.request_view()},
                              409 if err == p.NO_SCREEN else 200)
        if u.path == "/api/plan":  # what a print would be with these settings (for the web form)
            f, _ = a.lib.resolve(text(b, "id"))
            if not f or not f.get("rpm"):
                return self._json({"error": "no such video"}, 404)
            return self._json(p.plan_for(f, num(b, "revs", lo=1, hi=100000, kind=int), num(b, "factor", lo=0.05, hi=20)))
        if u.path == "/api/result":
            pct = p.result(text(b, "result"))
            return self._json({"ok": pct is not None, "factor_pct": pct})
        if u.path == "/api/settings":
            for k in ("warmup_min", "delay_s"):
                if b.get(k) is not None:
                    p.settings[k] = num(b, k, lo=0.0, hi=60.0)
            if b.get("factor") is not None:  # the general exposure correction (projector settings card)
                p.settings["factor"] = round(num(b, "factor", lo=0.3, hi=3.0), 4)
            rf = b.get("resin_factor")  # {"resin_id": …, "value": 1.0}: SpinSlicer applied it to the dose
            if rf is not None:
                if not isinstance(rf, dict) or not text(rf, "resin_id"):
                    raise BadRequest("resin_factor: {resin_id, value}")
                p.reset_resin_factor(text(rf, "resin_id"), num(rf, "value", 1.0, lo=0.3, hi=3.0))
            p.save_settings()
            return self._json({"ok": True, "settings": p.settings})
        if u.path == "/api/printer":
            err = p.set_profile(b)
            return self._json({"ok": err is None, "error": err, "printer": p.profile()}, 200 if err is None else 400)
        if u.path == "/api/stop":
            p.stop()
            return self._json({"ok": True})
        if u.path == "/api/request/cancel":
            return self._json({"ok": p.cancel_request()})
        if u.path == "/api/motor":
            err = p.motor(bool(b.get("on")), num(b, "rpm", lo=0, hi=60))
            return self._json({"ok": err is None, "error": err})
        if u.path == "/api/revs":
            a.lib.set_revs(text(b, "id"), num(b, "revs", 1, lo=0.01, hi=100000))
            return self._json({"ok": True})
        if u.path == "/api/projector":
            err = p.show_pattern(text(b, "pattern", "black"))
            return self._json({"ok": err is None, "error": err})
        if u.path == "/api/calibration":
            p.calibrate(num(b, "offset_px", lo=-400, hi=400, kind=int), num(b, "vial_px", lo=20, hi=4000, kind=int))
            if b.get("show"):
                p.show_pattern(text(b, "show"))
            return self._json({"ok": True, "settings": p.settings})
        self._json({"error": "not found"}, 404)


class App:
    """What the request handler needs: library, printer, ESP, projector (set in main())."""


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
            g = [f"URL {url}", f"HOST {host}.local", f"NET {wifi_name()}", f"CAL {s['offset_px']} {s['vial_px']}",
                 "UPD IDLE"]  # a fresh SpinPi: the screen's "Updating the Raspberry…" is over
            if g != app.esp.greeting:
                app.esp.greeting = g
                app.esp.hello()
                log("address:", url, f"({host}.local)")
            app.mdns.alias(ip, host)  # spinprinter.local too, whatever the host is called
            publish_mdns()  # the name follows the host name until it is set on the web page
            time.sleep(15)

    threading.Thread(target=refresh_greeting, daemon=True).start()

    def system_loop():
        """Every 10 s: counters, the System page's facts (SYS) and the firmware on offer (FW)."""
        while True:
            pr = app.printer
            try:
                pr.tick_counters()
                if app.esp.connected and pr.update_done:  # how the update before this start went
                    ok = version_tuple(pr.update_done) == version_tuple(VERSION)
                    app.esp.send(f"MSG Raspberry обновлена: {VERSION}" if ok else "MSG Raspberry не обновилась")
                    pr.update_done = None
                if app.esp.connected and not pr.busy():  # nothing extra on the wire during a print
                    if app.esp.pins is None and app.esp.version:
                        app.esp.pins_asked = app.esp.version
                        app.esp.send("PINS")  # until it answers; firmware before 0.8.5: "ERR unknown command"
                    app.esp.send(sys_line(pr.system_info()))
                    pr.refresh_firmware()
            except Exception as e:  # noqa: BLE001 - a broken sensor must not stop the rest
                log("system info:", e)
            time.sleep(10)

    threading.Thread(target=system_loop, daemon=True).start()
    Handler.app = app
    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    srv.daemon_threads = True
    log(f"SpinPi {VERSION} on port {args.port}, data in {args.data}, projector: {args.engine}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
