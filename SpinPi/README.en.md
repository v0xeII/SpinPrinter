# SpinPi: the Raspberry Pi side of the SpinSlicer CAL printer

[Русский](README.md) · **English**

Printing without a USB stick: the Pi is on Wi-Fi, the printer screen shows a **QR code**, you
scan it with a phone, the printer page opens, you drop a video from SpinSlicer on it and press
"Print". While printing the page shows the time left, the motor speed and the sync.

```
 phone / PC ──Wi-Fi──► Raspberry Pi ──HDMI──► projector
                             │
                             └──USB cable──► ESP32-S3 (screen, joystick, motor)
```

**Frames by angle (since 0.5).** The ESP32 counts every motor step and knows the vial angle.
SpinPi forwards that angle to [SpinProj](../SpinProj/README.en.md) 20 times a second, and SpinProj
puts up the frame for that angle on every projector refresh. The video cannot drift from the
rotation, and the light goes off after exactly N turns. The old way (mpv plays the video, the motor
is trimmed to follow it) is still there: `--engine mpv`.

> Try everything without hardware on a PC: [`python3 dev/bench.py`](../README.md#the-whole-printer-on-a-pc).

## Install (once)

Step by step with pictures: [docs/INSTALL.md](../docs/INSTALL.md).

**Linux, one command on the computer** (card in the reader, from the `SpinPrinter` folder):
```
sudo bash SpinPi/make_sd_card.sh
```
It finds the card, asks you to type "yes" and your Wi-Fi, wipes the card, writes Raspberry Pi OS Lite
64-bit, sets up SSH login with this computer's key and keeps the screen black. The Pi installs
SpinPi and SpinProj by itself on the first start (10–20 minutes, needs internet).

**Any OS:**
1. **Raspberry Pi Imager** → your Pi model → **Raspberry Pi OS Lite (64-bit)** → settings: hostname
   **spinslicer**, a user and password, your Wi-Fi, **SSH on**. Write, put the card in the Pi, power on, wait 3 minutes.
2. Log in from the computer (Windows: PowerShell): `ssh user@spinslicer.local`
3. One command on the Pi (10–20 minutes, then it reboots; run again to update):
   ```
   curl -fsSL https://raw.githubusercontent.com/v0xeII/SpinPrinter/main/SpinPi/get.sh | sudo bash
   ```
4. Connect the ESP32 (USB cable, see the [wiring diagram](../docs/wiring.svg)) and the
   projector (HDMI). The ESP screen → **Connect** shows the QR code with the address.

## Printing

**From SpinSlicer:** "Send to printer". The slicer finds the printer on the network (mDNS) and
sends a `.spin` file: the one-turn video, a picture of the part and the planned turns in one file.

**From a USB stick (like OpenCAL):** put the `.spin` or MP4 on a stick, plug it into the Pi. On the printer
screen: **Print** → pick the video → check turns, exposure and pause → the pink **PRINT**. The stick
is mounted read-only.

**Over Wi-Fi:** open the printer page (QR or `http://spinslicer.local`), drop the `.spin` or video, press
"Print". Uploaded files show up in the list on the printer screen too.

### The `.spin` file
One file per print: a plain stored (uncompressed) ZIP with `spin.json` (metadata), `video.mp4`,
`preview.png` and `slicer.json` (the format: [docs/ecosystem.ru.md](../docs/ecosystem.ru.md)).
The printer:
- checks `spin.json` (format, version) and the video's SHA-256; a broken file is refused on
  upload and shown with its error when it is on a stick;
- unpacks the video once into `/var/lib/spinpi/spin/` (stick files too) and shows the preview on
  the page and in the print sheet;
- takes the turns and tolerance from `plan` (as before from the MP4 tag or `name.spin.json`), and
  `frames_per_rev`, `frame0_angle_deg` and `rotation` from `video`;
- if the video was sliced for the other turning direction than the vial really turns (printer
  profile → "The vial turns"), it plays the frames backwards: the part comes out right, not mirrored;
- warns when the file was sliced for another printer (`printer.profile_id`).

Plain videos, `name.spin.json` beside them and JSON in the MP4 tag keep working.

### Printer profile and print log
The "Printer profile" card on the page holds what the slicer needs for its maths: network name,
projector irradiance and contrast, vial diameter, wall and glass, motor gear and direction.
Resolution, refresh, centering and vial width are filled in by themselves. Stored in
`settings.json`, read by the slicer with `GET /api/printer`. The printer has a stable `id` (`spinpi-…`).

The print log (`GET /api/history`) keeps the file, the plan, the turns really used, the exposure
correction, the resin and the "how did it come out" answer, so the slicer can tune the resin's dose.

### Exposure without a camera
OpenCAL plays the video until someone sees the part in a camera. Here the time is computed:

1. **SpinSlicer** computes the dose from physics (resin absorption, projector irradiance, gel
   dose) and stores the plan in the `.spin` file (or in the MP4 and in `name.spin.json` beside it).
2. **The printer** turns it into a **whole number of turns**, so every angle gets the same dose.
   The light goes off exactly on the turn boundary.
3. **Resin calibration, once:** SpinSlicer prints a "staircase" of 9 columns with different doses.
   You see which one cured first and enter its number.
4. **Learning after each print:** on "Done!" choose "Under / Good / Over". The printer moves the
   exposure correction by the plan's tolerance. All prints go into the log on the page.

The printer also keeps the dose even:
- **frame by angle:** SpinProj shows the frame for the current vial angle on every refresh, so the
  video never runs ahead of or behind the motor;
- **projector warm-up** (5 minutes after HDMI connects, by default): the print waits in the dark;
- **dark spin-up:** the vial spins in the dark first so the resin settles;
- **frame rate:** if the video fps does not divide the refresh (24 fps on 60 Hz), frames stay up
  2 or 3 refreshes in turn; the printer warns: export 30 or 60 fps;
- **faults:** no angle from the ESP for 0.3 s → SpinProj switches the light off by itself; the ESP
  silent for 1 s or the motor 5 % off speed → the print stops. If the Pi goes away, the ESP stops the motor;
- **dropped frames** (missed vsyncs) are counted and shown.

Videos without a plan (older ones, OpenCAL with `_9rpm` in the name) print too: you set the turns.
Stop a print on the page or on the printer screen (hold OK).

## Projector setup
On the printer screen (Service → Projector) and on the page:
- **Grid, Focus, Circle, White**: test pictures for focusing. **Black** switches the picture off.
- **Center**: a line on the axis. Move it until it matches the vial's rotation axis; the shift
  applies to every print.
- **Vial width**: a white bar N pixels wide. Match it to the vial to learn how many projector pixels
  the vial covers (SpinSlicer needs that number).

Keep resin out of the light!

## Troubleshooting
- Logs: `journalctl -u spinpi -f` · restart: `sudo systemctl restart spinpi`
- Projector resolution: `--projector auto` (default; asks the HDMI) or e.g. `--projector 1280x800` in
  `/etc/systemd/system/spinpi.service`, then `sudo systemctl daemon-reload && sudo systemctl restart spinpi`.
- Can the Pi keep up: `sudo systemctl stop spinpi && sudo /opt/spinpi/spinproj bench --rt --cpu 3`.

## For developers: HTTP API
| | |
|---|---|
| `PUT /api/files/<name>` | upload a `.spin` or a video (body = the file): `curl -T part.spin http://spinslicer.local/api/files/part.spin`. Answer `{"ok":true,"name":…,"info":…}`; a broken `.spin` → 400 `{"ok":false,"error":…}` |
| `GET /api/files` | the library (`id`: `lib/…` uploaded, `usb/…` on a stick): `format` (`spin`/`video`), frames, fps, rpm, `plan`, `plan_revs`, `tolerance_pct`, `rotation`, `frames_per_rev`, `frame0_angle_deg`, `reversed`, `other_printer`, `warnings`, `resin`, `preview`, `error` |
| `GET /api/thumb?id=…` | the picture (PNG from the `.spin` or a JPEG frame of the video) |
| `GET /api/printer` | printer profile: `id`, `name`, `projector` (`width`, `height`, `hz`, `irradiance_mw_cm2`, `contrast`), `vial` (`diameter_mm`, `wall_mm`, `n_glass`), `calibration` (`offset_px`, `center_offset_px`, `vial_width_px`), `motor` (`gear`, `direction`), `software`; unknown = `null` |
| `POST /api/printer` `{"name":"Lab","vial_diameter_mm":25,"motor_dir":"cw_from_top"}` | edit the profile (also `irradiance_mw_cm2`, `contrast`, `vial_wall_mm`, `vial_n`, `motor_gear`; `null` = unknown) |
| `GET /api/history` | `{"printer_id":…,"factor":…,"prints":[…]}`: every print with `file`, `plan`, `plan_revs`, `revs`, `factor`, `exposure_pct`, `resin`, `result` (under / ok / over / null), `factor_after` |
| `POST /api/print` `{"id":"lib/print.mp4","revs":6,"factor":1.05,"delay_s":3}` | start (`revs` optional: plan × correction) |
| `POST /api/print` `{"id":…,"confirm":true}` | what SpinSlicer sends: nothing starts, the printer's screen asks "Print?" and a yes starts it. The answer and `/api/state` carry `request`: `state` = `waiting_confirm` / `started` / `cancelled` / `expired` (10 minutes unanswered) / `replaced` / `error`. No screen connected: 409 |
| `POST /api/plan` `{"id":…,"factor":1.05}` | turns and seconds it would take, dose deviation, warnings |
| `POST /api/result` `{"result":"under"}` | how the last print came out: under / ok / over |
| `POST /api/settings` `{"warmup_min":5}` | projector warm-up, dark spin-up |
| `POST /api/stop` | stop |
| `POST /api/motor` `{"on":true,"rpm":4.0}` | spin the motor outside a print |
| `POST /api/projector` `{"pattern":"grid"}` | black / white / grid / focus / circle / center / vial |
| `POST /api/calibration` `{"offset_px":12,"vial_px":400,"show":"center"}` | centering and vial width |
| `GET /api/state`, `GET /api/events` | state (events = SSE stream, twice a second) |

No password: everyone on your Wi-Fi sees the page. Fine for a home network.

**mDNS:** the printer announces itself as `_spinprinter._tcp` (port 80) with TXT `name`, `id`, `api=1`,
`path=/api/printer`. `spinpi.py` writes the service file `/etc/avahi/services/spinprinter.service`
itself (the installer puts a first copy there). From a PC: `avahi-browse -rt _spinprinter._tcp`.

**Tests without a Pi:** `python -m unittest discover tests` (Windows, Mac, Linux; no ESP, no ffmpeg needed).

`fake_esp.py` pretends to be the ESP32; better is the [virtual ESP32](../SpinScreen_v0.7/sim/virtual_esp.cpp), the real screen UI in a window.
