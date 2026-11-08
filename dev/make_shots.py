#!/usr/bin/env python3
"""Screenshots of every SpinScreen screen, drawn by the real UI (SpinScreen_v0.7/sim/shots.cpp).

    python dev/make_shots.py SHOTS_EXE OUT_DIR

The 320x240 screen (ST7789 2.4", KY-023 joystick), with the card and with Spinny on the main page,
on one sheet: OUT_DIR/screens_320x240.png (and the English one: screens_320x240_en.png). Needs Pillow.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

LAYOUTS = [  # name, extra args, title
    ("screens_320x240", [], "SpinScreen 320x240 · ST7789 2.4\" · KY-023"),
    ("screens_320x240_en", ["en"], "SpinScreen 320x240 · ST7789 2.4\" · KY-023 (English)"),
]
SCALE = 2
BG = (10, 8, 11)


def shots(exe, extra, tmp):
    a, b = tmp / "card", tmp / "axo"
    a.mkdir(parents=True)
    b.mkdir(parents=True)
    subprocess.run([exe, *extra, str(a)], check=True)
    subprocess.run([exe, *extra, "axo", str(b)], check=True)
    files = sorted(a.glob("*.ppm"))
    files[2:2] = sorted(b.glob("*.ppm"))[1:]  # Spinny's main page and service list after the card's
    return files


def sheet(files, title, cols=4):
    ims = []
    for f in files:
        im = Image.open(f).convert("RGB")
        ims.append((f.stem.split("_", 1)[1], im.resize((im.width * SCALE, im.height * SCALE), Image.NEAREST)))
    w, h = ims[0][1].size
    pad, lab = 16, 22
    rows = (len(ims) + cols - 1) // cols
    out = Image.new("RGB", (cols * (w + pad) + pad, 40 + rows * (h + pad + lab) + pad), BG)
    d = ImageDraw.Draw(out)
    try:
        font = ImageFont.truetype("arial.ttf", 15)
        big = ImageFont.truetype("arialbd.ttf", 20)
    except OSError:
        font = big = ImageFont.load_default()
    d.text((pad, 10), title, fill=(255, 141, 181), font=big)
    for i, (name, im) in enumerate(ims):
        x = pad + (i % cols) * (w + pad)
        y = 40 + (i // cols) * (h + pad + lab)
        d.text((x, y), name.replace("_", " "), fill=(168, 143, 155), font=font)
        out.paste(im, (x, y + lab))
    return out


def main():
    exe, outdir = sys.argv[1], Path(sys.argv[2])
    outdir.mkdir(parents=True, exist_ok=True)
    for name, extra, title in LAYOUTS:
        with tempfile.TemporaryDirectory() as t:
            sheet(shots(exe, extra, Path(t)), title).save(outdir / f"{name}.png", optimize=True)
        print(outdir / f"{name}.png")


if __name__ == "__main__":
    main()
