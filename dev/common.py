"""Shared bits of the PC test bench: building, test videos, starting the programs."""
import json
import os
import socket
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, "dev", "work")
SPINPROJ = os.path.join(ROOT, "SpinProj", "spinproj")
VESP = os.path.join(ROOT, "SpinScreen_v0.7", "sim", "virtual_esp")
SPINPI = os.path.join(ROOT, "SpinPi", "spinpi.py")


def say(*a):
    print("·", *a, flush=True)


def build():
    """Compile SpinProj and the virtual ESP32 (make only rebuilds what changed)."""
    for d, target in (("SpinProj", "spinproj"), (os.path.join("SpinScreen_v0.7", "sim"), "virtual_esp")):
        r = subprocess.run(["make", "-s", "-C", os.path.join(ROOT, d), target], capture_output=True, text=True)
        if r.returncode:
            print(r.stdout + r.stderr)
            sys.exit(f"не собралось: {d}. Нужны: gcc, make, liblz4, SDL2 (Manjaro: sudo pacman -S lz4 sdl2 libdrm)")


def free_port(start=8080):
    for p in range(start, start + 50):
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", p))
            return p
        except OSError:
            continue
        finally:
            s.close()
    return 0


# A test "print" you can check by eye: an off-axis rod (moves left-right as the vial turns) and,
# in the corner, a dial whose hand shows the angle the frame belongs to. The dial on the
# projector window must point the same way as the dial on the virtual ESP32.
def _geq(n):
    th = f"(2*PI*N/{n})"
    rod = f"between(X,W/2+0.3*H*cos({th})-0.04*H,W/2+0.3*H*cos({th})+0.04*H)*between(Y,0.2*H,0.8*H)"
    cx, cy, r = "(W-0.14*H)", "(0.14*H)", "(0.1*H)"
    dist = f"hypot(X-{cx},Y-{cy})"
    ring = f"between({dist},{r}-2,{r}+1)"
    hand = f"lt(abs(mod(atan2(Y-{cy},X-{cx})-{th}+PI/2+3*PI,2*PI)-PI),0.05)*lt({dist},{r}-6)"
    soft = f"0.35*exp(-pow((X-W/2-0.15*H*sin(2*{th}))/(0.12*H),2)-pow((Y-H/2)/(0.25*H),2))"
    return f"255*min(1,{rod}+{ring}+{hand}+{soft})"


VIDEOS = [
    # name, size, frames per turn, fps, planned turns (SpinSlicer sidecar) or None
    ("spin_test_4rpm.mp4", "1280x800", 360, 24, 2.0),       # SpinSlicer style: one video = one turn (15 s)
    ("opencal_style_9rpm.mp4", "1280x800", 200, 30, None),  # OpenCAL style: speed in the name
    ("quick_12rpm.mp4", "640x400", 120, 24, 2.0),           # small and fast, for the self-test
]


def make_videos(folder, only=None):
    os.makedirs(folder, exist_ok=True)
    for name, size, n, fps, plan in VIDEOS:
        if only and name not in only:
            continue
        path = os.path.join(folder, name)
        if not os.path.exists(path):
            say("делаю тестовое видео", name, "(один раз, до минуты)")
            w, h = size.split("x")
            vf = f"format=gray,geq=lum='{_geq(n)}'"
            r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", f"color=black:s={w}x{h}:r={fps}",
                                "-frames:v", str(n), "-vf", vf, "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "10",
                                path + ".tmp.mp4"], capture_output=True, text=True)
            if r.returncode:
                sys.exit("ffmpeg: " + r.stderr[-300:])
            os.replace(path + ".tmp.mp4", path)
        side = os.path.splitext(path)[0] + ".spin.json"
        if plan and not os.path.exists(side):
            json.dump({"format": "spinslicer-print-1", "revolutions": plan, "tolerance_pct": 5.0,
                       "note": "test file made by dev/common.py"}, open(side, "w"), indent=1)


def data_dir(path, **settings):
    """A data folder for SpinPi with test-friendly settings (no 5 minute projector warm-up)."""
    os.makedirs(path, exist_ok=True)
    sp = os.path.join(path, "settings.json")
    try:
        s = json.load(open(sp))
    except (OSError, ValueError):
        s = {}
    s.update(settings)
    json.dump(s, open(sp, "w"), indent=1)
    return path


def start_esp(extra=(), log=None):
    p = subprocess.Popen([VESP] + list(extra), stdout=subprocess.PIPE, stderr=log or subprocess.DEVNULL, text=True)
    tty = p.stdout.readline().strip()
    if not tty.startswith("/dev/"):
        p.kill()
        sys.exit("виртуальный ESP32 не запустился")
    return p, tty


def start_pi(tty, port, data, usb, out="sdl", projector="1280x800", log=None):
    cmd = [sys.executable, SPINPI, "--serial", tty, "--port", str(port), "--data", data, "--usb-dir", usb,
           "--hdmi", "ignore", "--projector", projector, "--spinproj", SPINPROJ, f"--spinproj-arg=--out={out}"]
    return subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
