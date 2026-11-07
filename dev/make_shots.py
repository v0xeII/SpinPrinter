#!/usr/bin/env python3
"""Screenshots of every SpinScreen layout, drawn by the real UI (SpinScreen_v0.7/sim/shots.cpp).

    python dev/make_shots.py SHOTS_EXE OUT_DIR

Runs the shots tool for every screen size (wide 320x170 and 320x240, square 240x240, compact
160x128, round 240x240) and puts each layout's screens on one sheet: OUT_DIR/<layout>.png.
The round layout is shown through a round mask, as the glass shows it. OUT_DIR/controls_320x240.png:
the screens with hints for buttons, the KY-040 encoder and the KY-023 joystick. Needs Pillow.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

LAYOUTS = [  # name, size, extra args, title
    ("wide_320x170", "320x170", [], "ST7789 1.9\" 320x170"),
    ("wide_320x240", "320x240", [], "ST7789 / ILI9341 320x240"),
    ("square_240x240", "240x240", [], "ST7789 240x240"),
    ("compact_160x128", "160x128", [], "ST7735 160x128"),
    ("round_240x240", "240x240", ["round"], "GC9A01 round 240x240"),
]
SCALE = 2
BG = (10, 8, 11)


def shots(exe, size, extra, tmp):
    a, b = tmp / "card", tmp / "axo"
    a.mkdir(parents=True)
    b.mkdir(parents=True)
    subprocess.run([exe, size, *extra, str(a)], check=True)
    subprocess.run([exe, size, *extra, "axo", str(b)], check=True)
    files = sorted(a.glob("*.ppm"))
    files[2:2] = sorted(b.glob("*.ppm"))[1:]  # Spinny's main page and service list after the card's
    return files


def sheet(files, round_, title, cols=4, label=None):
    ims = []
    for f in files:
        im = Image.open(f).convert("RGB")
        if round_:
            w, h = im.size
            mask = Image.new("L", (w * 4, h * 4), 0)
            ImageDraw.Draw(mask).ellipse((0, 0, w * 4 - 1, h * 4 - 1), fill=255)
            mask = mask.resize((w, h), Image.LANCZOS)
            back = Image.new("RGB", (w, h), (40, 36, 42))
            im = Image.composite(im, back, mask)
        ims.append(((label or (lambda f: f.stem.split("_", 1)[1]))(f), im.resize((im.width * SCALE, im.height * SCALE), Image.NEAREST)))
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


INPUT_SCREENS = ["setup", "edit", "edit_speed", "print", "about", "confirm"]


def inputs_sheet(exe, tmp):
    """The same screens with each kind of controls: the hints follow the device."""
    files = []
    for inp in ("buttons", "encoder", "joystick"):
        d = tmp / inp
        d.mkdir(parents=True)
        subprocess.run([exe, "320x240", *([] if inp == "buttons" else [inp]), str(d)], check=True)
        by = {f.stem.split("_", 1)[1]: f for f in d.glob("*.ppm")}
        files += [by[n] for n in INPUT_SCREENS]
    return sheet(files, False, "Controls: buttons / KY-040 encoder / KY-023 joystick (320x240)", cols=len(INPUT_SCREENS),
                 label=lambda f: f.parent.name + " · " + f.stem.split("_", 1)[1].replace("_", " "))


def main():
    exe, outdir = sys.argv[1], Path(sys.argv[2])
    outdir.mkdir(parents=True, exist_ok=True)
    for name, size, extra, title in LAYOUTS:
        with tempfile.TemporaryDirectory() as t:
            files = shots(exe, size, extra, Path(t))
            sheet(files, "round" in extra, title).save(outdir / f"{name}.png", optimize=True)
        print(outdir / f"{name}.png")
    with tempfile.TemporaryDirectory() as t:
        inputs_sheet(exe, Path(t)).save(outdir / "controls_320x240.png", optimize=True)
    print(outdir / "controls_320x240.png")


if __name__ == "__main__":
    main()
