# SpinProj: the projector shows the frame for the vial's angle

[Русский](README.md) · **English**

A small C program. On every projector refresh (vsync) it puts up the frame that belongs to the
vial's current angle. The ESP32 counts the angle from the motor steps, SpinPi forwards it 20 times a second.

```
ESP32 (motor steps) ──USB/UART──► SpinPi ──"A angle rpm time"──► SpinProj ──HDMI──► projector
```

Why this beats playing a video (OpenCAL, SpinPi 0.4 with mpv):
- the video cannot drift from the motor: the frame is chosen by angle, not by time;
- a missed vsync shifts nothing: the next one shows the right frame again;
- the light goes off after exactly N turns, to one refresh;
- no angle for 0.3 s → the light goes off by itself. If SpinPi dies, it goes off too.

Before a print one turn of frames is unpacked into RAM: 8-bit gray, LZ4-compressed. Any file
ffmpeg reads works: MP4 (OpenCAL's too), lossless MKV (FFV1), PNG sequences, lossless H.264 from SpinSlicer.

## Outputs

| `--out` | where | for |
|---|---|---|
| `drm` | the projector on the Raspberry (DRM/KMS, page flip) | printing |
| `sdl` | a window on a PC | looking at it on a PC |
| `null` | nowhere, an exact virtual 60 Hz | self-tests |

## Build

```bash
make                 # finds SDL2 and libdrm itself; without them only null is built
make NO_SDL=1        # on the Raspberry
```
Needs gcc, make, liblz4 (Raspberry Pi OS: `sudo apt install liblz4-dev libdrm-dev`).

## Measuring on the Raspberry

Stop the SpinPi service, connect a projector or monitor over HDMI:
```bash
sudo spinproj bench --rt --cpu 3                      # synthetic CAL-like frames
sudo spinproj bench --rt --cpu 3 --video print.mp4    # your real video
```
It shows the memory for one turn, how many such frames fit in RAM, the milliseconds to unpack a
frame, how even the vsyncs are and how many were missed. The last line `RESULT …` has all the numbers.

**Numbers from a PC say nothing about the Raspberry.** On the development PC a 1280×800 frame takes
1.5–3 ms; on a Raspberry it will be slower (a Pi 3 more than a Pi 5). Only measuring tells how much.

## Commands (stdin → SpinProj)

| command | does |
|---|---|
| `LOAD <frames per turn> <file>` | unpack one turn into RAM (in the background) |
| `A <angle°> <rpm> <time ns> [round trip ns]` | the angle from the ESP32; time on `CLOCK_MONOTONIC` (Python's `time.monotonic_ns()`); slow answers are trusted less |
| `ARM <turns>` | from the next vsync, light for exactly that many turns |
| `STOP` / `BLACK` | dark |
| `IMAGE <file.pgm>` | a test picture (grid, center…) |
| `OFFSET <px>` | shift the picture sideways (centering on the rotation axis) |
| `PHASE <°>` | the angle that shows frame 0 (default 0; SpinPi sends `frame0_angle_deg` from the `.spin`) |
| `REVERSE <0\|1>` | 1: the video was sliced for the other turning direction, frames go backwards (frame 0 stays at `PHASE`). SpinPi sends it before `LOAD` |
| `PING` / `QUIT` | |

| reply (stdout) | when |
|---|---|
| `READY <output> <width> <height> <Hz>` | started |
| `LOADING i n` · `LOADED n w h raw_MB lz4_MB seconds` | unpacking / ready |
| `LIGHT <angle>` | light on |
| `STAT <degrees done> <frames> <missed vsync> <max ms per frame> <angle jitter ms>` | twice a second while lit |
| `DONE <frames> <missed> <min> <max> <mean> <max ms>` | N turns done; min/max/mean = refreshes each frame stayed up |
| `STOPPED` · `ERR text` · `INFO text` | |

## Even dose

Every frame stays up a whole number of refreshes. If refreshes per frame is not whole (24 fps on
60 Hz = 2.5), frames take turns staying up 2 and 3 refreshes, as with any video player. SpinPi warns
about it; 30 or 60 fps on 60 Hz is exactly even.
