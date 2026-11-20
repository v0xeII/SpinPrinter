#!/usr/bin/env python3
"""The strobe test print (docs/sync-report.md, 6.3): the projector as a stroboscope.

    python dev/make_strobe.py [OUT.spin] [--size 1280x800] [--frames 360] [--fps 30]

One turn of the colba = FRAMES frames, all black except four (0, 90, 180, 270 degrees), which show a
narrow white vertical bar in the middle. A paper disc with one radial line on the colba, a print of
30 minutes, and the line has to stand still in four places:
  * it creeps         -> the phase drifts: note the angle after 30 minutes;
  * it is blurred      -> jitter: blur width minus the flash width is the error in degrees;
  * it jumps           -> dropped frames or skipped steps.
Needs ffmpeg on the PATH (the Raspberry has it). Writes a .spin that SpinPi prints like any other.
"""
import argparse
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import zipfile


def frames(w, h, n, bar_px):
    black = bytes(w * h)
    row = bytearray(w)
    x0 = w // 2 - bar_px // 2
    row[x0:x0 + bar_px] = b"\xff" * bar_px
    lit = bytes(row) * h
    marks = {0, n // 4, n // 2, 3 * n // 4}
    for i in range(n):
        yield lit if i in marks else black


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", default="strobe.spin")
    ap.add_argument("--size", default="1280x800")
    ap.add_argument("--frames", type=int, default=360)
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--bar", type=int, default=4, help="bar width, pixels")
    a = ap.parse_args()
    w, h = (int(v) for v in a.size.lower().split("x"))
    with tempfile.TemporaryDirectory() as tmp:
        mp4 = os.path.join(tmp, "video.mp4")
        cmd = ["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "gray", "-s", f"{w}x{h}",
               "-r", str(a.fps), "-i", "-", "-c:v", "libx264", "-preset", "veryfast", "-crf", "0",
               "-g", "1", "-pix_fmt", "yuv420p", mp4]
        try:
            p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
        except FileNotFoundError:
            sys.exit("ffmpeg is needed (sudo apt install ffmpeg)")
        for f in frames(w, h, a.frames, a.bar):
            p.stdin.write(f)
        p.stdin.close()
        if p.wait() != 0:
            sys.exit("ffmpeg failed")
        video = open(mp4, "rb").read()
    rev_s = a.frames / a.fps
    meta = {
        "format": "spin", "version": 1, "generator": "SpinPrinter dev/make_strobe.py", "name": "strobe",
        "video": {"file": "video.mp4", "sha256": hashlib.sha256(video).hexdigest(), "frames": a.frames,
                  "fps": a.fps, "width": w, "height": h, "frames_per_rev": a.frames,
                  "rotation": "ccw_from_top", "frame0_angle_deg": 0.0, "offset_px": [0, 0]},
        # no dose plan: the turns are chosen on the printer (enough for 30 minutes)
        "plan": {"revolutions": None, "revolution_s": rev_s, "rpm": 60.0 / rev_s,
                 "warnings": ["strobe test: not a part, no resin"]},
        "resin": {}, "printer": {},
    }
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
        z.writestr("spin.json", json.dumps(meta, ensure_ascii=False, indent=1))
        z.writestr("video.mp4", video)
    with open(a.out, "wb") as f:
        f.write(buf.getvalue())
    turns_30min = int(1800 / rev_s + 0.5)
    print(f"{a.out}: {a.frames} frames, {w}x{h}, {a.fps:g} fps = {60 / rev_s:.2f} rpm; "
          f"about {turns_30min} turns for 30 minutes")


if __name__ == "__main__":
    main()
