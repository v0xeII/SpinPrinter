# SpinScreen: screen and motor firmware for the SpinSlicer CAL printer

[Русский](README.md) · **English**

Board: **ESP32-S3 Super Mini** with **any colour SPI screen on the list**: ST7789 1.9" 170×320, 1.3"/1.54"
240×240 and 2.0"/2.4"/2.8" 240×320, ILI9341 320×240, ST7735 1.8" 160×128, the round GC9A01 240×240. One
firmware: the screen, pins and buttons are chosen on the board (the installer or the `CONFIG` command). Black
background, pink UI, smooth animation, the SpinSlicer logo at boot, a big PRINT card on top of the main page, menus in Russian and English. Spinny the axolotl is optional.

**Menu:**
- **Print** → video list (USB stick in the Pi and Wi-Fi uploads) → setup: **turns** (from the SpinSlicer
  plan), **exposure correction**, dark spin-up + the pink **PRINT** button → spin-up / warm-up / dark →
  print screen (time, turn k of N, sync) → "Done!" + "Under / Good / Over".
- **Connect**: QR code of the printer page.
- **Service**:
  - Projector: Black, White, Grid, Focus, Circle, Center, Vial width.
  - Motor: test spin, speed, direction, ramp, microsteps, steps per turn, gear, "exactly 1 turn", zero; a live tachometer.
  - Screen: brightness, language, dark screen while printing.
  - Customize: **Home**: Card (default: PRINT as a big card on top, every page uses the whole width) or Axolotl (Spinny at the side of every page); **Colour** of the axolotl: pink, gold, wild, black, blue, RGB.
  - Raspberry Pi: reboot or shut down (with confirmation).
  - About.

**Motor:** steps come from a 10 MHz hardware timer with a fractional period accumulator, so the
average speed is exact and every step is counted. The Pi asks for the angle (`POS`).

- `SpinScreen/`: the firmware, opens in the Arduino IDE (`SpinScreen.ino`)
- `SpinScreen/config.h`: the defaults (pins, screen, buttons); `CONFIG` changes them on the board
- `SpinScreen/hw.cpp`: the hardware settings at run time: the `CONFIG` line, pin checks, presets
- `screens/`: **screenshots of every layout** (drawn by the real UI: `sim/shots.cpp` + `dev/make_shots.py`)
- `tools/flash.sh`: build and flash in one command · `tools/pi_test.py`: pretend to be the Pi from a PC
- `sim/`: the UI on a PC. `make virtual_esp`: a **virtual ESP32** in a window (the real UI, buttons,
  motor, the link to SpinPi through a virtual serial port); `spinny_stress`: Spinny under lag for 20 minutes
- `SpinScreen/qrcodegen.*`: QR codes, Project Nayuki library (MIT)
- `gen/make_fonts.py`: Cyrillic fonts from [Onest](https://fonts.google.com/specimen/Onest) (SIL OFL)

## New in 0.8.0
- **One firmware for every colour SPI screen:** ST7789 (170×320, 240×240, 240×320), ILI9341 320×240,
  ST7735 160×128, the round GC9A01 240×240. As fast as before: DMA, only what changed goes out, and the
  self-healing band against garbled pixels.
- **All the hardware is set without flashing:** screen type, rotation, SPI clock, inversion, colour order,
  offset, screen pins, the controls (buttons / KY-040 encoder / KY-023 joystick) and their pins, motor pins and
  the wires to the Pi. Stored on the board; until something is stored, `config.h`'s values apply, so boards
  that already work keep working.
- **The installer** asks "which screen, and how is it wired", draws the wiring for that choice, stores the
  settings over Web Serial and checks the board comes back with them.
- **Layouts by size:** wide (320×170, 320×240), square (240×240), compact (160×128) and round: the menu as a
  wheel (the chosen row in the middle, the others turn away round the circle, dots on the right edge show where
  you are), print progress as a ring round the glass, nothing important in the clipped corners. The main page
  is still yours to choose: Customize → Home → Card or Axolotl.
- **KY-023 joystick** and the breadboard layout (screen and joystick plugged in next to the board): `CONFIG preset=breadboard`.
- If the screen does not start (wrong type or pins), the board still answers over USB: `CONFIG?` shows what is
  stored, `CONFIG …` or `CONFIG RESET` fix it.
- About shows which screen is set up.

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

ILI9341 and ST7735 name their pins differently: SCK/CLK = SCL, SDI/MOSI = SDA, RESET/RST = RES, A0 = DC,
LED/BL = BLK; SDO (MISO) is not needed. Other pins: the installer or `CONFIG scl=… sda=…`.

Buttons (one leg on the pin, the other on GND, no resistors): UP → 4, DOWN → 5, OK → 6. A long
OK press = back / stop. A KY-040 rotary encoder works instead: CLK → 4, DT → 5, SW → 6
(`CONFIG input=encoder clk=4 dt=5 sw=6`). Or a KY-023 joystick: GND → GND, **+5V → 3V3** (not 5V),
VRx → 4, VRy → 5, SW → 6 (`CONFIG input=joystick vrx=4 vry=5 sw=6`): up/down = the list, right or press
= OK, left or long press = back; the centre is taken at power-up, so leave the stick alone then; directions
swapped → `joy_swap=1`, `joy_invy=1`, `joy_invx=1`; VRx and VRy need analog pins (GPIO 1..20); `JOY` shows
what the board reads. Motor driver (later): STEP → 1, DIR → 2, EN → 13, common GND.

**On a breadboard** (`CONFIG preset=breadboard`): a 2.4" 240×320 screen plugs in on the TX RX 1..4 side
(SCL → TX/43, SDA → RX/44, RES 1, DC 2, CS 3, BLK 4, plus 2 wires for GND and 3V3), the joystick on the
5V GND 3V3 side (GND → GND, +5V → 3V3, VRx 13, VRy 11, SW 12), motor STEP 5, DIR 6, EN 7. The Pi on the
USB cable only (TX and RX are the screen's).
The Raspberry Pi: USB cable, or wires as in the [wiring diagram](../docs/wiring.svg).

## Flashing

**Easiest, in the browser** (Windows, Mac, Linux): https://v0xeii.github.io/SpinPrinter/flasher/ in Chrome or Edge.

**One command** (board on USB, Arduino IDE 2 with the esp32 core installed): `bash SpinScreen_v0.7/tools/flash.sh`

**Arduino IDE:** Boards Manager → **esp32** (Espressif) → Install; Tools → Board → **ESP32S3 Dev Module**;
Tools → **USB CDC On Boot: Enabled**; open `SpinScreen/SpinScreen.ino`, Upload. Board not found:
hold BOOT, press and release RESET, release BOOT, Upload again.

## Which screen

Easiest: step 2 of the [installer](https://v0xeii.github.io/SpinPrinter/flasher/#screen): pick the screen
and the controls, look at the wiring, press Save. Or over USB (any terminal at 115200):

```
CONFIG panel=ili9341
CONFIG panel=st7735 spi=27 input=encoder clk=4 dt=5 sw=6
CONFIG preset=breadboard
CONFIG?            what is stored now
CONFIG RESET       forget it, config.h's values again
```

The board answers `CONFIG SAVED …` and reboots with the new settings (one key is enough, the rest stay).
A mistake (unknown key, a pin used twice, GPIO 19/20 = USB or 26..32 = flash) answers `ERR CONFIG …` and
changes nothing. `CONFIG TRY …` only checks.

| key | what | values |
|---|---|---|
| `panel` | screen | `st7789_170x320`, `st7789_240x240`, `st7789_240x320`, `ili9341`, `st7735`, `gc9a01` |
| `rot` | rotation | rectangular: 1 or 3 (180°); square and round: 0..3 |
| `spi`, `mode` | SPI clock in MHz, SPI mode | 40 (ST7735: 27), 0 or 3 |
| `invert`, `bgr` | inversion, colour order | 0 / 1 / `auto` |
| `offx`, `offy` | where the picture sits in the controller | a number / `auto` |
| `scl` `sda` `res` `dc` `cs` `blk` | screen pins | GPIO, -1 = none (RES, CS, BLK) |
| `input` | controls | `buttons` (`up` `down` `ok`), `encoder` (`clk` `dt` `sw`, `enc_detent`), `joystick` (`vrx` `vry` `sw`, `joy_gnd` `joy_vcc` `joy_swap` `joy_invx` `joy_invy`) |
| `step` `dir` `en` | motor | GPIO, `en=-1`: the driver's EN on GND |
| `pi_rx` `pi_tx` | wires to the Pi | GPIO, or both -1 (USB only) |
| `preset` | a ready set of pins | `wires` (the table above), `breadboard` |

The UI rearranges itself by size ([screenshots](screens/)): **wide** 320×170 / 320×240 (Spinny and the
tachometer left, the menu right, or the PRINT card on top), **square** 240×240 (Spinny on top, the menu
below), **compact** 160×128 (the same, smaller), **round** GC9A01 (the menu as a wheel, progress as a ring).

Odd picture: nothing / white → the screen type and pins (`CONFIG?`), BLK, 3V3 power; colours inverted →
`CONFIG invert=0` (or 1); red and blue swapped → `bgr=1` (or 0); upside down → `rot=3` (or 1); shifted or a
garbage stripe → `offx=… offy=…` (0, 35 or 80; ST7735 sometimes 1 or 2); no CS pin → `cs=-1 mode=3`;
garbage or stripes → `spi=20`.

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
| `PING` | answers `PONG SpinScreen 0.8.0`; the Pi sends it every second |
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
| `CAL offset vial` | centering and vial width (Projector menu) |
| `CONFIG?` · `CONFIG key=value …` · `CONFIG TRY …` · `CONFIG RESET` | hardware settings (see Which screen) |
| `JOY` | what the joystick reads (wiring check) |

At power-up the board prints `READY SpinScreen 0.8.0 <screen> <w>x<h> <controls>` (and `noscreen` if the screen did not start).

| ESP → Pi | when |
|---|---|
| `EV LIST` · `EV PRINT i turns correction% pause` | list opened · PRINT pressed |
| `EV RESULT under/ok/over` | answer to "How did it come out?" |
| `EV PATTERN name` · `EV OFFSET px` · `EV VIALW px` | Service → Projector |
| `EV REBOOT` / `EV POWEROFF` · `EV STOP` | Service → Raspberry Pi · OK held on the print screen |
| `EV SPIN 1/0`, `EV RPM 9.5` | motor or speed changed on the screen |
