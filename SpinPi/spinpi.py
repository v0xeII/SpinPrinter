#!/usr/bin/env python3
"""SpinPi: the Raspberry Pi side of a SpinSlicer CAL printer.

* web page on http://<pi>/ : drop a SpinSlicer video from a phone or PC, press Print
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
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import serial  # python3-serial
except ImportError:  # still serve the web page without the ESP
    serial = None

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = "0.5.0"
VIDEO_EXT = (".mp4", ".mkv", ".mov", ".avi", ".webm")


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def save_json(path, obj, **kw):
    """Write to a temp file, fsync, rename: the power can go at any moment and the old file stays whole."""
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, **kw)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


# =============================================================================== ESP32 link
class Esp:
    """Line protocol over serial (see SpinScreen README): the USB cable or three wires to the Pi's
    UART (GPIO14/15 = /dev/serial0). Every candidate port is tried in turn until one answers PING,
    and if the ESP goes quiet the search starts again, so moving from wires to cable just works."""

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
        usb = sorted(glob.glob("/dev/serial/by-id/*Espressif*"))
        real = {os.path.realpath(p) for p in usb}
        usb += [p for p in sorted(glob.glob("/dev/ttyACM*")) if os.path.realpath(p) not in real]
        # Raspberry Pi GPIO14/15: /dev/serial0 on Raspberry Pi OS, ttyAMA0 itself on SpinOS
        wires = next(([p] for p in ("/dev/serial0", "/dev/ttyAMA0") if os.path.exists(p)), [])
        return usb + wires

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
                raw = self.ser.readline()
                if raw:
                    self._handle(raw.decode("utf-8", "replace").strip())
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

    def load(self, path, frames_per_turn, offset_px):
        self.drain()
        self.send(f"OFFSET {int(offset_px)}")
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


# =============================================================================== library
SAFE = re.compile(r"[^A-Za-z0-9._()\- Ѐ-ӿ]+")
RPM_IN_NAME = re.compile(r"_(\d+(?:\.\d+)?)rpm", re.IGNORECASE)  # OpenCAL convention: part_9rpm.mp4


def safe_name(name):
    name = os.path.basename(urllib.parse.unquote(name)).strip()
    name = SAFE.sub("_", name)[:120]
    if not name.lower().endswith(VIDEO_EXT):
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
    """Videos uploaded over Wi-Fi ("lib/<name>") + videos on USB sticks ("usb/<stick>/<path>")."""

    def __init__(self, root, usb_dirs_fn):
        self.root = os.path.join(root, "prints")
        self.thumbs = os.path.join(root, "thumbs")
        os.makedirs(self.root, exist_ok=True)
        os.makedirs(self.thumbs, exist_ok=True)
        self.meta_path = os.path.join(root, "library.json")
        try:
            self.meta = json.load(open(self.meta_path))
        except (OSError, ValueError):
            self.meta = {}
        self.usb_dirs_fn = usb_dirs_fn
        self.lock = threading.Lock()

    def _save(self):
        save_json(self.meta_path, self.meta, indent=1)

    def lib_path(self, name):
        return os.path.join(self.root, safe_name(name))

    def thumb_path(self, fid):
        return os.path.join(self.thumbs, hashlib.sha1(fid.encode()).hexdigest()[:16] + ".jpg")

    def _info(self, fid, path):
        """Cached metadata; re-probed when the file changes."""
        st = os.stat(path)
        key = f"{st.st_size}:{int(st.st_mtime)}"
        with self.lock:
            m = self.meta.get(fid)
        if m is None or m.get("key") != key:
            info = probe(path) or {}
            old = m or {}
            m = dict(info, key=key, size=st.st_size, added=old.get("added", time.time()), revs=old.get("revs"))
            with self.lock:
                self.meta[fid] = m
                self._save()
            subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-i", path, "-vf", "scale=320:-2", "-frames:v", "1",
                              self.thumb_path(fid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return m

    @staticmethod
    def _sidecar(path):
        """SpinSlicer writes <name>.spin.json beside the video (the same plan as the MP4 tag)."""
        p = os.path.splitext(path)[0] + ".spin.json"
        try:
            plan = json.load(open(p, encoding="utf-8"))
            return plan if isinstance(plan, dict) else None
        except (OSError, ValueError):
            return None

    def _entry(self, fid, path, source):
        m = self._info(fid, path)
        name = os.path.basename(path)
        d = {k: v for k, v in m.items() if k != "key"}
        d.update(id=fid, name=name, source=source)
        plan = self._sidecar(path) or m.get("plan")
        d["plan"] = plan
        if plan and plan.get("revolutions"):
            d["plan_revs"] = float(plan["revolutions"])
            d["tolerance_pct"] = float(plan.get("tolerance_pct") or 0) or None
        if m.get("frames") and m.get("fps"):
            dur = m["frames"] / m["fps"]
            hint = RPM_IN_NAME.search(name)
            if m.get("revs"):  # set by hand on the web page
                rev_s = dur / m["revs"]
            elif hint:  # OpenCAL-style file name
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
                    if n.lower().endswith(VIDEO_EXT) and not n.startswith(".") and "recording" not in n.lower():
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
        """id -> (entry, path) or (None, None)"""
        for f in self.list():
            if f["id"] == fid:
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
        for p in (self.lib_path(fid[4:]), self.thumb_path(fid)):
            try:
                os.unlink(p)
            except FileNotFoundError:
                pass
        with self.lock:
            self.meta.pop(fid, None)
            self._save()
        return True

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
        self.history_path = os.path.join(data, "history.json")
        try:
            self.settings = json.load(open(self.settings_path))
        except (OSError, ValueError):
            self.settings = {}
        defaults = {"factor": 1.0, "delay_s": 3, "default_revs": 5, "warmup_min": 5, "last_id": "",
                    "offset_px": 0, "vial_px": 400, "display_fps": 60.0}
        for k, v in defaults.items():
            self.settings.setdefault(k, v)
        try:
            self.history = json.load(open(self.history_path))
        except (OSError, ValueError):
            self.history = []
        self.state = "idle"  # idle | wait | print | done | error
        self.wait = None  # (seconds, kind) while waiting before the light
        self.job = None
        self.msg = ""
        self.sync_ms = None
        self.drops = 0
        self.stop_flag = threading.Event()
        self.motor_rpm = 0.0
        self.motor_angle = 0.0
        self.pattern = "black"
        self.esp_list = []
        self.angle_mode = getattr(proj, "engine", "mpv") == "angle"
        self.last_pos = None  # (angle, rpm, at_speed, t_mid, received) from the job's angle feeder

    # ---- persistence
    def save_settings(self):
        save_json(self.settings_path, self.settings, indent=1)

    def save_history(self):
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
             "tolerance_pct": f.get("tolerance_pct"), "cadence_ok": self.cadence_ok(f.get("fps", 0))}
        if plan:
            d["dose_dev_pct"] = 100.0 * (revs / (plan * factor) - 1.0)
        return d

    def status(self):
        j = self.job
        left = self.warm.left(self.settings["warmup_min"])
        d = {"state": self.state, "msg": self.msg, "esp": self.esp.connected, "esp_version": self.esp.version,
             "projector": self.proj.ok, "projector_err": self.proj.err[-200:] if not self.proj.ok else "",
             "hdmi": self.warm.connected, "warmup_left_s": left, "wait": self.wait,
             "sync_ms": self.sync_ms, "drops": self.drops, "motor_rpm": self.motor_rpm,
             "motor_angle": self.motor_angle % 360, "settings": self.settings, "pattern": self.pattern,
             "history": self.history[-8:], "version": VERSION,
             "engine": "angle" if self.angle_mode else "mpv"}
        if j:
            d["job"] = {k: j[k] for k in ("id", "file", "rpm", "revs", "time_s", "factor", "plan_revs")}
            d["job"].update(turn=j.get("turn", 0), left_s=j.get("left_s", j["time_s"]),
                            progress=j.get("progress", 0.0))
        return d

    # ---- control
    def start(self, fid, revs=None, factor=None, delay_s=None):
        if self.busy():
            return "busy"
        f, path = self.lib.resolve(fid)
        if not f or not f.get("rpm"):
            return "no such video"
        if self.warm.connected is False:
            return "projector not connected (HDMI)"
        p = self.plan_for(f, revs, factor)
        delay_s = float(self.settings["delay_s"] if delay_s is None else delay_s)
        self.settings.update(factor=p["factor"], delay_s=delay_s, last_id=fid)
        if not f.get("plan_revs"):
            self.settings["default_revs"] = p["revs"]
        self.save_settings()
        self.job = dict(p, id=fid, file=f["name"], path=path, rpm=f["rpm"], rev_s=f["rev_s"], fps=f.get("fps"),
                        delay_s=delay_s)
        self.stop_flag.clear()
        threading.Thread(target=self._run_angle if self.angle_mode else self._run, daemon=True).start()
        return None

    def stop(self):
        self.stop_flag.set()

    def motor(self, on, rpm=None):
        if self.busy():
            return "busy"
        if rpm:
            self.esp.send(f"RPM {float(rpm):.4f}")
        self.esp.send(f"SPIN {1 if on else 0}")
        return None

    def result(self, verdict):
        """Feedback after a print: under / ok / over. Moves the exposure factor for next time."""
        if not self.history or verdict not in ("under", "ok", "over"):
            return None
        h = self.history[-1]
        h["result"] = verdict
        tol = h.get("tolerance_pct") or 5.0
        step = min(0.15, max(0.02, tol / 100.0))
        if verdict == "under":
            self.settings["factor"] *= 1 + step
        elif verdict == "over":
            self.settings["factor"] *= 1 - step
        self.settings["factor"] = round(max(0.3, min(3.0, self.settings["factor"])), 4)
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
        files = [f for f in self.lib.list() if f.get("rpm")][:16]
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
        elif p[0] == "STOP":
            self.stop()
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
            self.stop()
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
        entry = {"t": time.strftime("%Y-%m-%d %H:%M"), "file": j["file"], "revs": j["revs"], "time_s": j["time_s"],
                 "factor": j["factor"], "plan_revs": j["plan_revs"], "tolerance_pct": j["tolerance_pct"],
                 "result": None}
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
            # the vial angle := where the video is right now, so they start in phase
            j["angle0"] = (tpos % j["rev_s"]) / j["rev_s"] * 360.0
            e.send(f"SETANGLE {j['angle0']:.4f}")
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
            self.state = "idle"
        except Exception as ex:  # noqa: BLE001
            log("print error:", ex)
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
        entry = {"t": time.strftime("%Y-%m-%d %H:%M"), "file": j["file"], "revs": j["revs"], "time_s": j["time_s"],
                 "factor": j["factor"], "plan_revs": j["plan_revs"], "tolerance_pct": j["tolerance_pct"],
                 "result": None, "engine": "angle"}
        done = threading.Event()
        try:
            self.state, self.msg, self.sync_ms, self.drops, self.pattern = "wait", "", None, 0, "black"
            self.last_pos = None
            pr.black()
            frames = max(1, round(j["rev_s"] * (j.get("fps") or 0)))
            pr.load(j["path"], frames, self.settings["offset_px"])  # unpacks while the motor spins up
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
            self.state = "idle"
        except Exception as ex:  # noqa: BLE001
            log("print error:", ex)
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
            data = open(path, "rb").read()
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
            return self._json(a.lib.list())
        if u.path == "/api/thumb":
            return self._file(a.lib.thumb_path(q.get("id", [""])[0]), "image/jpeg")
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

    def do_PUT(self):  # upload: PUT /api/files/<name>, body = the video
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
            return self._json({"error": "upload interrupted"}, 400)
        os.replace(tmp, dst)
        info = a.lib.added(name)
        log("uploaded", name)
        self._json({"ok": True, "name": name, "info": info})

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
        if u.path == "/api/print":
            err = p.start(b.get("id", ""), b.get("revs"), b.get("factor"), b.get("delay_s"))
            return self._json({"ok": err is None, "error": err})
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
            time.sleep(15)

    threading.Thread(target=refresh_greeting, daemon=True).start()
    Handler.app = app
    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    srv.daemon_threads = True
    log(f"SpinPi {VERSION} on port {args.port}, data in {args.data}, projector: {args.engine}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
