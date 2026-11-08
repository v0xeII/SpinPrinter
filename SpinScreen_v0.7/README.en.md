# SpinScreen: screen and motor firmware for the SpinSlicer CAL printer

[Русский](README.md) · **English**

One set of hardware: an **ESP32-S3 Super Mini** board, a **2.4" ST7789 240×320** screen (e.g.
GMT024-08-SPI8P) and a **KY-023** joystick on wires, a **TMC2209** motor driver, a **USB cable** to the
Raspberry Pi. All of it is set in `config.h`, nothing to set up on the board: flash it and the screen
works. Black background, pink UI, smooth animation, the SpinSlicer logo at boot, a big PRINT card on top
of the main page, menus in Russian and English. Spinny the axolotl is optional.

**Menu:**
- **Print** → video list (USB stick in the Pi and network uploads) → setup: **turns** (from the SpinSlicer
  plan), **exposure correction**, dark spin-up + the pink **PRINT** button → spin-up / warm-up / dark →
  print screen (time, turn k of N, sync) → "Done!" + "Under / Good / Over".
- **Print?** A print sent from SpinSlicer does not start by itself: the screen shows what came (name,
  turns, time) and waits. Right = print, left = cancel.
- **Connect**: QR code of the printer page.
- **Settings**: five groups, each its own page (no "Back" row: tilt left or long press goes back):
  - Spin: test spin, speed, direction (live tachometer on the left).
  - Display: brightness, language, dark screen while printing; **Home**: Card (default: PRINT as a big card
    on top) or Axolotl (Spinny at the side); **Colour** of the axolotl: pink, gold, wild, black, blue, RGB.
  - Projector: Black, White, Grid, Focus, Circle, Center, Vial width.
  - Motor: ramp, microsteps, steps per turn, gear, "exactly 1 turn", zero. Changes made while the motor
    spins are applied when it stops.
  - System: reboot or shut down the Raspberry Pi (with confirmation); About.

**Joystick:** up / down = the list (hold to scroll), right or a press = OK, left or a long press =
back. On the print screen one ← only asks ("← again = stop"); a second ← within 3 s stops. A bump of
the stick or noise from the motor on its wires never ends a print. The stick's centre is taken at
power-up: leave it alone then.

**Motor:** steps come from a 10 MHz hardware timer with a fractional period accumulator, so the
average speed is exact and every step is counted. The Pi asks for the angle (`POS`).

- `SpinScreen/`: the firmware, opens in the Arduino IDE (`SpinScreen.ino`)
- `SpinScreen/config.h`: the pins and hardware (the one setup), motor defaults
- `screens/`: **screenshots of every screen** (drawn by the real UI: `sim/shots.cpp` + `dev/make_shots.py`)
- `tools/flash.sh`: build and flash in one command · `tools/pi_test.py`: pretend to be the Pi from a PC
- `sim/`: the UI on a PC. `make virtual_esp`: a **virtual ESP32** in a window (the real UI, the joystick
  from the keyboard, motor, the link to SpinPi through a virtual serial port); `spinny_stress`: Spinny under lag for 20 minutes
- `SpinScreen/qrcodegen.*`: QR codes, Project Nayuki library (MIT)
- `gen/make_fonts.py`: Cyrillic fonts from [Onest](https://fonts.google.com/specimen/Onest) (SIL OFL)

## Wiring

| ST7789 2.4" screen | ESP32-S3 |
|---|---|
| GND | GND |
| VCC | **3V3** (not 5V) |
| SCL | 43 (TX) |
| SDA | 44 (RX) |
| RES | 1 |
| DC | 2 |
| CS | 3 |
| BLK | 4 |

| KY-023 joystick | ESP32-S3 |
|---|---|
| GND | GND |
| +5V | **3V3** (not 5V: that would put more than 3.3 V on the ESP's inputs) |
| VRx | 13 |
| VRy | 11 |
| SW | 12 |

TMC2209 motor driver: STEP → 5, DIR → 6, EN → 7, plus a common GND with the ESP32; the motor has its own 12–24 V supply.

To the Raspberry Pi: a USB cable that carries data (the board's USB-C to any Pi USB port).

Joystick directions swapped? In `config.h`: `JOY_SWAP_XY`, `JOY_INVERT_Y`, `JOY_INVERT_X`.
The `JOY` command over USB shows what the board reads.

## Flashing

**Easiest: from the browser** (Windows, Mac, Linux): https://v0xeii.github.io/SpinPrinter/flasher/ in Chrome or Edge.

**One command** (board on USB, Arduino IDE 2 with the esp32 core installed): `bash SpinScreen_v0.7/tools/flash.sh`

Or in the Arduino IDE: Boards Manager → **esp32** (Espressif) → Install; Board: **ESP32S3 Dev Module**;
**USB CDC On Boot: Enabled**; open `SpinScreen/SpinScreen.ino`, Upload. Not found: hold BOOT, press and
release RESET, release BOOT, Upload again.

## If the screen looks wrong

All of it is in `config.h`, then flash again:
- Nothing / white: the screen's wires, the BLK backlight, 3V3 power.
- Colours like a negative: `TFT_INVERT 0`.
- Red and blue swapped: `TFT_BGR 1`.
- Upside down: `TFT_ROTATION 3`.
- Garbage or stripes on long wires: `TFT_SPI_MHZ 20`.

## Smoothness

The UI keeps a **steady 60 fps** on an exact microsecond grid, and animation follows the frame
time, so there is no 16/17 ms shimmer. It drops to 30 fps only when everything is still, or settles
on a steady 30 if the board cannot draw 60. Drawing is cheap: fast inline math instead of library
calls, span-skipping shape fills, Spinny's body as one shape, `-O2` for the drawing code, and only
changed areas go to the screen (DMA). Measured in the Espressif QEMU: ~1.7–1.9 million instructions
per frame (v0.6: ~9.7–11.9 million). Real frame time on your board: Settings → About.

## Spinny

A flexible chain of 15 joints: the head swims to a goal and the body follows, so he bends and turns
round through himself, facing you. He folds his legs at speed and paddles when still, his gills
breathe. He swims about his corner, follows the selected row, comes up for air, eats food, sinks
to the bottom and sleeps after a minute without keys, dashes about on "Done!" and startles on errors.

## Protocol with the Raspberry Pi (USB, 115200, text lines)

| Pi → ESP | does |
|---|---|
| `PING` | answers `PONG SpinScreen 0.8.4`; the Pi sends it every second |
| `RPM 9.5` · `SPIN 1/0` | speed in rpm · motor on/off (with a ramp) |
| `STATE PRINT/DONE/IDLE/ERROR text` · `PROG 42 80` | print state · progress 42 %, 80 s left |
| `MSG text` | Spinny says it in a bubble |
| `STATS` | for tuning: replies `STATS <fps> fps <work> ms <KB> KB draw <draw> ms scr <screen> page <page> sel <item>` |
| `KEY u` / `d` / `o` / `b` | press up / down / OK / back (tests from a PC) |
| `GET` | current settings |
| `POS` | answers `POS <angle°> <rpm> <at speed 0/1>` within ~1 ms |
| `ZERO` · `SETANGLE 12.5` | angle := 0 · angle := 12.5° |
| `TURNS 6` · `WAIT 30 warm` · `DROPS 2` | turns · waiting before light (spin/warm/dark) · dropped frames |
| `URL …` / `HOST …` / `NET …` · `FILE name` · `SYNC 12` | QR address, host, Wi-Fi · file · sync error ms |
| `FL BEGIN …` · `FL i …` · `FL END` | video list for the Print screen |
| `ASK turns seconds name` · `ASK END` | a print from SpinSlicer waits for a yes on the screen · the question is gone (expired or replaced) |
| `CAL offset vial` | centering and vial width (Projector menu) |
| `JOY` | what the joystick reads (wiring check) |

At power-up the board prints `READY SpinScreen 0.8.4 st7789_240x320 320x240 joystick` (and `noscreen` if the screen did not start).

| ESP → Pi | when |
|---|---|
| `EV LIST` · `EV PRINT i turns correction% pause` | list opened · PRINT pressed |
| `EV RESULT under/ok/over` | answer to "How did it come out?" |
| `EV PATTERN name` · `EV OFFSET px` · `EV VIALW px` | Settings → Projector |
| `EV REBOOT` / `EV POWEROFF` · `EV STOP` | Settings → Raspberry Pi · ← twice within 3 s on the print screen |
| `EV GO` / `EV NO` | the answer to "Print?" |
| `EV SPIN 1/0`, `EV RPM 9.5` | motor or speed changed on the screen |
