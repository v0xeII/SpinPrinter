# SpinScreen: screen and motor firmware for the SpinSlicer CAL printer

[Русский](README.md) · **English**

Board: **ESP32-S3 Super Mini**, screen: **ST7789 1.9" 170×320** or **1.3"/1.54" 240×240**. Black
background, pink UI, smooth animation, Spinny the axolotl swimming at the side, menus in Russian and English.

**Menu:**
- **Print** → video list (USB stick in the Pi and Wi-Fi uploads) → setup: **turns** (from the SpinSlicer
  plan), **exposure correction**, dark spin-up + the pink **PRINT** button → spin-up / warm-up / dark →
  print screen (time, turn k of N, sync) → "Done!" + "Under / Good / Over".
- **Connect**: QR code of the printer page.
- **Service**:
  - Projector: Black, White, Grid, Focus, Circle, Center, Vial width.
  - Motor: test spin, speed, direction, ramp, microsteps, steps per turn, gear, "exactly 1 turn", zero; a live tachometer.
  - Screen: brightness, language, dark screen while printing.
  - Axolotl: show or hide (the menu then uses the whole screen), colour: pink, gold, wild, black, blue, RGB.
  - Raspberry Pi: reboot or shut down (with confirmation).
  - About.

**Motor:** steps come from a 10 MHz hardware timer with a fractional period accumulator, so the
average speed is exact and every step is counted. The Pi asks for the angle (`POS`).

- `SpinScreen/`: the firmware, opens in the Arduino IDE (`SpinScreen.ino`)
- `SpinScreen/config.h`: **all pins and settings in one place**
- `tools/flash.sh`: build and flash in one command · `tools/pi_test.py`: pretend to be the Pi from a PC
- `sim/`: the UI on a PC. `make virtual_esp`: a **virtual ESP32** in a window (the real UI, buttons,
  motor, the link to SpinPi through a virtual serial port); `spinny_stress`: Spinny under lag for 20 minutes
- `SpinScreen/qrcodegen.*`: QR codes, Project Nayuki library (MIT)
- `gen/make_fonts.py`: Cyrillic fonts from [Onest](https://fonts.google.com/specimen/Onest) (SIL OFL)

## New in 0.7.2
- **The Pi can be on wires:** RX (GPIO44) ← Pi pin 8, TX (GPIO43) → Pi pin 10, GND, 5V. The USB
  cable works too, at the same time: the firmware answers where the Pi's PING came from. Noise from
  a loose wire is ignored (pull-up on RX, lines with control bytes are dropped).

## New in 0.7.1
- **Spinny no longer swims into the menu.** His corner (122 px) is shorter than two of his bodies,
  and on a U-turn at the edge the tail poked into the list. Now the body meets the corner wall and
  turns towards you, like a real axolotl at the aquarium glass.
- **Spinny no longer twitches back and forth.** A goal in the middle of the corner (food, a place to
  sleep) made him re-decide the direction every frame. The side is now chosen with a margin, and a
  small step back is paddled, without a U-turn.
- **No more freezes from USB.** With the board on a computer that is not reading the port, every
  line written to USB waited ~100 ms and the UI froze. Lines now go through a separate task.
- Measured with `sim/spinny_stress.cpp` (the real UI for 20 minutes with lag, 30/60 fps, keys,
  prints, errors): out-of-corner frames 4–14 thousand → 0–3 (only while he swims in at boot),
  back-and-forth bursts 30–45 → 0–2.

## Wiring

| Screen | ESP32-S3 |
|---|---|
| GND | GND |
| VCC | **3V3** (not 5V) |
| SCL | 12 |
| SDA | 11 |
| RES | 8 |
| DC | 9 |
| CS | 10 |
| BLK | 7 |

Buttons (one leg on the pin, the other on GND, no resistors): UP → 4, DOWN → 5, OK → 6. A long
OK press = back / stop. A KY-040 rotary encoder works instead: CLK → 4, DT → 5, SW → 6 and
`INPUT_MODE INPUT_ENCODER` in `config.h`. Motor driver (later): STEP → 1, DIR → 2, EN → 13, common GND.
The Raspberry Pi: USB cable, or wires as in the [wiring diagram](../docs/wiring.svg).

## Flashing

**Easiest, in the browser** (Windows, Mac, Linux): https://v0xeii.github.io/SpinPrinter/ in Chrome or Edge.

**One command** (board on USB, Arduino IDE 2 with the esp32 core installed): `bash SpinScreen_v0.7/tools/flash.sh`

**Arduino IDE:** Boards Manager → **esp32** (Espressif) → Install; Tools → Board → **ESP32S3 Dev Module**;
Tools → **USB CDC On Boot: Enabled**; open `SpinScreen/SpinScreen.ino`, Upload. Board not found:
hold BOOT, press and release RESET, release BOOT, Upload again.

## Which screen

One line in `config.h`: `#define TFT_PANEL PANEL_170x320` (1.9" wide) or `PANEL_240x240`
(1.3" / 1.54" square). The UI rearranges itself. Odd picture: colours inverted → `TFT_INVERT 0`;
upside down → another `TFT_ROTATION`; shifted or a garbage stripe → `TFT_OFFSET_X` / `TFT_OFFSET_Y`
(0, 35 or 80); square module without CS → `PIN_TFT_CS -1` and `TFT_SPI_MODE 3`.

## Smoothness

The UI keeps a **steady 60 fps** on an exact microsecond grid, and animation follows the frame
time, so there is no 16/17 ms shimmer. It drops to 30 fps only when everything is still, or settles
on a steady 30 if the board cannot draw 60. Drawing is cheap: fast inline math instead of library
calls, span-skipping shape fills, Spinny's body as one shape, `-O2` for the drawing code, and only
changed areas go to the screen (DMA). Measured in the Espressif QEMU: ~1.7–1.9 million instructions
per frame (v0.6: ~9.7–11.9 million). Real frame time on your board: Service → About.

## Spinny

A flexible chain of 15 joints: the head swims to a goal and the body follows, so he bends and turns
round through himself, facing you. He folds his legs at speed and paddles when still, his gills
breathe. He swims about his corner, follows the selected row, comes up for air, eats food, sinks
to the bottom and sleeps after a minute without keys, dashes about on "Done!" and startles on errors.

## Protocol with the Raspberry Pi (USB or TX/RX wires, 115200, text lines)

| Pi → ESP | does |
|---|---|
| `PING` | answers `PONG SpinScreen 0.7.2`; the Pi sends it every second |
| `RPM 9.5` · `SPIN 1/0` | speed in rpm · motor on/off (with a ramp) |
| `STATE PRINT/DONE/IDLE/ERROR text` · `PROG 42 80` | print state · progress 42 %, 80 s left |
| `MSG text` | Spinny says it in a bubble |
| `GET` | current settings |
| `POS` | answers `POS <angle°> <rpm> <at speed 0/1>` within ~1 ms |
| `ZERO` · `SETANGLE 12.5` | angle := 0 · angle := 12.5° |
| `TURNS 6` · `WAIT 30 warm` · `DROPS 2` | turns · waiting before light (spin/warm/dark) · dropped frames |
| `URL …` / `HOST …` / `NET …` · `FILE name` · `SYNC 12` | QR address, host, Wi-Fi · file · sync error ms |
| `FL BEGIN …` · `FL i …` · `FL END` | video list for the Print screen |
| `CAL offset vial` | centering and vial width (Projector menu) |

| ESP → Pi | when |
|---|---|
| `EV LIST` · `EV PRINT i turns correction% pause` | list opened · PRINT pressed |
| `EV RESULT under/ok/over` | answer to "How did it come out?" |
| `EV PATTERN name` · `EV OFFSET px` · `EV VIALW px` | Service → Projector |
| `EV REBOOT` / `EV POWEROFF` · `EV STOP` | Service → Raspberry Pi · OK held on the print screen |
| `EV SPIN 1/0`, `EV RPM 9.5` | motor or speed changed on the screen |
