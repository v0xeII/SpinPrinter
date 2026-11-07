#!/usr/bin/env python3
"""The README animation: the real SpinScreen UI (sim/sim, the same ui.cpp as the firmware) next to
what the projector shows, frame by the vial angle.

    python3 dev/make_demo.py            -> docs/demo.gif (English UI)
    python3 dev/make_demo.py ru         -> docs/demo.ru.gif

Needs: make, a C++ compiler, ffmpeg, Pillow.
"""
import math
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(ROOT, "SpinScreen_v0.7", "sim")
W, H = 320, 170          # the ESP screen
FPS_IN, STEP = 60, 4     # sim frames per second, keep every 4th: 15 fps
UNTIL_MS = 24500         # menu -> print -> done -> "how did it come out?"
BG, PANEL, LINE = (10, 8, 11), (21, 16, 22), (42, 32, 41)
PINK, HOT, TEXT, DIM = (255, 141, 181), (255, 79, 154), (247, 227, 236), (168, 143, 155)


def font(size, bold=False):
    for name in (("NotoSans-Bold.ttf" if bold else "NotoSans-Regular.ttf"),
                 ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf")):
        for d in ("/usr/share/fonts/noto", "/usr/share/fonts/TTF", "/usr/share/fonts/truetype/dejavu"):
            p = os.path.join(d, name)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def projector(draw, x0, y0, pw, ph, angle, lit, f_small):
    """The test print as SpinProj would show it: an off-axis rod and a dial at the frame's angle."""
    draw.rounded_rectangle((x0 - 3, y0 - 3, x0 + pw + 3, y0 + ph + 3), 10, fill=(0, 0, 0), outline=LINE, width=2)
    if not lit:
        draw.text((x0 + pw / 2, y0 + ph / 2), "dark", fill=(60, 50, 56), font=f_small, anchor="mm")
        return
    th = math.radians(angle)
    cx = x0 + pw / 2 + 0.3 * ph * math.cos(th)
    for k in range(18, 0, -1):  # soft glow around the rod
        g = int(255 * 0.35 * math.exp(-(k / 9) ** 2))
        draw.rounded_rectangle((cx - 0.04 * ph - k, y0 + 0.2 * ph - k, cx + 0.04 * ph + k, y0 + 0.8 * ph + k), 6,
                               fill=(g, g, g))
    draw.rectangle((cx - 0.04 * ph, y0 + 0.2 * ph, cx + 0.04 * ph, y0 + 0.8 * ph), fill=(255, 255, 255))
    dx, dy, r = x0 + pw - 0.14 * ph, y0 + 0.14 * ph, 0.1 * ph
    draw.ellipse((dx - r, dy - r, dx + r, dy + r), outline=(255, 255, 255), width=2)
    a = th - math.pi / 2
    draw.line((dx, dy, dx + math.cos(a) * (r - 5), dy + math.sin(a) * (r - 5)), fill=(255, 255, 255), width=3)


def main():
    lang = "ru" if "ru" in sys.argv[1:] else "en"
    out = os.path.join(ROOT, "docs", "demo.ru.gif" if lang == "ru" else "demo.gif")
    subprocess.run(["make", "-s", "-C", SIM, "sim"], check=True)
    work = tempfile.mkdtemp(prefix="spindemo")
    track = os.path.join(work, "track.txt")
    args = [os.path.join(SIM, "sim"), f"--track={track}", f"--until={UNTIL_MS}"] + (["en"] if lang == "en" else [])
    raw = subprocess.run(args, capture_output=True, check=True).stdout
    rows = [ln.split() for ln in open(track)]
    n = min(len(rows), len(raw) // (W * H * 3))
    f_title, f_cap, f_small = font(26, True), font(17, True), font(15)
    cap_esp = "ESP32-S3 screen (the real firmware UI)" if lang == "en" else "экран ESP32-S3 (настоящий интерфейс)"
    cap_proj = "projector: the frame for the vial angle" if lang == "en" else "проектор: кадр по углу флакона"
    CW, CH = 1200, 470
    sx, sy, sc = 40, 70, 2          # screen x2
    px, py, pw, ph = 760, 92, 400, 250
    k = 0
    for i in range(0, n, STEP):
        ms, angle, lit = int(rows[i][0]), float(rows[i][1]), rows[i][2] == "1"
        frame = Image.frombytes("RGB", (W, H), raw[i * W * H * 3:(i + 1) * W * H * 3]).resize((W * sc, H * sc), Image.NEAREST)
        img = Image.new("RGB", (CW, CH), BG)
        d = ImageDraw.Draw(img)
        d.text((40, 22), "SpinPrinter", fill=PINK, font=f_title)
        d.rounded_rectangle((sx - 16, sy - 16, sx + W * sc + 16, sy + H * sc + 16), 22, fill=PANEL, outline=LINE, width=2)
        img.paste(frame, (sx, sy))
        d.text((sx, sy + H * sc + 30), cap_esp, fill=DIM, font=f_cap)
        projector(d, px, py, pw, ph, angle, lit, f_small)
        d.text((px, py + ph + 18), cap_proj, fill=DIM, font=f_cap)
        state = ("light on" if lit else "dark") if lang == "en" else ("свет" if lit else "темно")
        d.text((px, py + ph + 44), f"vial {angle % 360:5.1f}°  ·  {state}" if lang == "en"
               else f"флакон {angle % 360:5.1f}°  ·  {state}", fill=HOT if lit else DIM, font=f_small)
        img.save(os.path.join(work, f"f{k:04d}.png"))
        k += 1
    fps = FPS_IN // STEP
    pal = os.path.join(work, "pal.png")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-framerate", str(fps), "-i", os.path.join(work, "f%04d.png"),
                    "-vf", "palettegen=max_colors=96:stats_mode=diff", pal], check=True)
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-framerate", str(fps), "-i", os.path.join(work, "f%04d.png"), "-i", pal,
                    "-lavfi", "paletteuse=dither=none:diff_mode=rectangle", "-loop", "0", out], check=True)
    print(f"{out}: {k} frames, {os.path.getsize(out) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
