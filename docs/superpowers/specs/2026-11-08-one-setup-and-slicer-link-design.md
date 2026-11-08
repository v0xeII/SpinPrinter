# One printer setup, and a closer slicer link

Date: 2026-11-08. Status: approved in chat.

## Goal

SpinPrinter has too many ways to be built. Keep one, the author's own, and make SpinSlicer
work with the printer from its settings and from its last step.

## 1. One hardware setup (SpinPrinter)

The only supported printer:

| part | what |
|---|---|
| board | ESP32-S3 Super Mini |
| screen | ST7789 2.4" 240x320 (e.g. GMT024-08-SPI8P), on wires: SCL 12, SDA 11, RES 8, DC 9, CS 10, BLK 7 |
| controls | KY-023 joystick |
| motor | stepper with a TMC2209 |
| link to the Pi | USB cable only |

Changes:

- Firmware: pins, panel and input are compile-time constants again. `hw.cpp` / `hw.h`, the
  NVS hardware config and the `CONFIG` serial command go. The other panels (170x320,
  240x240, ILI9341, ST7735, GC9A01), buttons, the KY-040 encoder and the breadboard layout go.
  The UI lays out for 320x240 landscape and the joystick only.
- The UART link to the Pi (4 wires) goes: USB only, in the firmware, SpinPi and the docs.
- Flasher: only "Flash". The screen step and `screen.js` go.
- `SpinOS/` (untested Buildroot image) goes. Raspberry Pi OS + one command stays the only way.
- Empty skeleton folders `firmware/`, `pi/`, `system/` go.
- README, INSTALL, SpinScreen README and the wiring picture describe this one setup (en + ru).

## 2. Print started from the slicer, confirmed on the printer

- `POST /api/print` gets `"confirm": true`. The Pi then does not start: it holds a pending job
  and sends the board `ASK <index> <revs> <minutes> <name>`.
- The board shows a card: name, turns, time, "Print?". Joystick right / press = `GO`,
  left = `NO`. The Pi answers `GO` by starting the job, `NO` by dropping it.
- A pending job is dropped after 10 minutes, or when another print starts.
- `/api/state` carries `job`: `{"state": "waiting_confirm" | "printing" | "done" | "cancelled"
  | "stopped", "id", "name", ...}` so the slicer and the web page can follow it.
- `POST /api/print` without `confirm` (the web page) starts as before.

## 3. SpinSlicer

Settings -> Printer, one card:
- found printers, "Connect", an address field for typing one in;
- once connected: name, version, online state; the profile fills the settings, and the fields
  it changed are marked;
- "Write to printer" (`POST /api/printer`): sends projector, vial and calibration values edited
  in the slicer, after showing what will change. The printer stays the source of truth.

Last step "Print": two big buttons instead of the small link.
- "Print": upload with progress, `POST /api/print {"confirm": true}`, then "Confirm on the
  printer's screen", then progress and "Stop". Disabled with a hint when no printer is
  connected.
- "To USB stick": finds removable drives (Windows, macOS, Linux), writes `<name>.spin` to the
  root, asks which one when there are several, offers to eject. No stick: the usual
  "Save as" dialog.

## 4. Prints that stop by themselves (bug)

Reported on the Pi: a print sometimes just stops. Find the cause in SpinPi / SpinScreen and fix
it, with a test that reproduces it.

## Testing

- SpinPi: pending job, GO, NO, timeout, state in `/api/state`.
- Firmware: the confirm card in the simulator; `pio`/arduino build of the firmware.
- Slicer: removable drive search (system calls faked), print flow and profile write against a
  local HTTP server.

## Order

1. Spec. 2. The stop bug. 3. Firmware down to one setup. 4. Confirm flow (Pi + board).
5. Flasher, docs, removals. 6. SpinSlicer (its own repository, branch from `main`).
7. All tests and builds.
