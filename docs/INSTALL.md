# Installing SpinPrinter

[Русский](INSTALL.ru.md) · **English**

You need: a Raspberry Pi (3, 3B+, 4, 5 or Zero 2 W, see below) with its own power supply, an
8–32 GB microSD card, an ESP32-S3 Super Mini with an ST7789 screen and three buttons, a USB cable
that carries data, a projector with HDMI. And a computer with Windows, Mac or Linux.

## Which Raspberry Pi

Any 64-bit Raspberry Pi with the 40-pin header works: the software and the wiring are the same.

| model | power supply | HDMI to the projector | ESP32 by USB cable |
|---|---|---|---|
| Pi 3 / 3B+ | micro-USB, 5 V 2.5 A | full-size HDMI | any USB-A port |
| Pi 4 / 400 | USB-C, 5 V 3 A | micro-HDMI (cable or adapter) | any USB-A port |
| Pi 5 | USB-C, 5 V 5 A (27 W) | micro-HDMI (cable or adapter) | any USB-A port |
| Pi Zero 2 W | micro-USB, 5 V 2.5 A | mini-HDMI (adapter) | USB OTG adapter, or use the 4 wires |

Not supported: the first Pi, Pi 2 and the first Pi Zero (32-bit only, the Zero has one core).
The faster the Pi, the more frames per turn it can keep up with; `spinproj bench` tells on yours.
The SpinOS image is built for the Pi 3 only so far; everything else runs on Raspberry Pi OS.

## 1. Flash the ESP32-S3

**In the browser (Windows, Mac, Linux, nothing to install):**
1. Plug the ESP32-S3 into the computer with a USB cable **that carries data**.
2. Open **https://v0xeii.github.io/SpinPrinter/flasher/** in Chrome or Edge.
3. "Flash SpinScreen" → pick the port `USB JTAG/serial debug unit` (`COM…` on Windows) → "Install".
4. Half a minute later Spinny the axolotl swims on the screen.

No port in the list: the cable only charges, use another one. Flashing fails: hold **BOOT**,
press and release **RESET**, release BOOT and try again. The board's settings are kept.

**For development (Linux, Arduino IDE 2 with the esp32 core):** `bash SpinScreen_v0.7/tools/flash.sh`

## 2. Set up the Raspberry Pi

### Way A: any OS (Raspberry Pi Imager)
1. Install [Raspberry Pi Imager](https://www.raspberrypi.com/software/).
2. Device: **your Raspberry Pi model**. OS: **Raspberry Pi OS (other) → Raspberry Pi OS Lite (64-bit)**. Storage: your microSD.
3. "Edit settings":
   - hostname: `spinslicer`
   - a user name and password (remember them)
   - Wi-Fi: network name, password, your country
   - "Services": enable SSH (password)
4. "Write". Put the card in the Pi, power it and wait 3 minutes.
5. On the computer open a terminal (Windows: **PowerShell**) and log in to the Pi:
   ```
   ssh user@spinslicer.local
   ```
   Answer `yes` to the "yes/no" question, then type the password (it does not show, that is normal).
6. One command on the Pi (10–20 minutes, the Pi reboots at the end):
   ```bash
   curl -fsSL https://raw.githubusercontent.com/v0xeII/SpinPrinter/main/SpinPi/get.sh | sudo bash
   ```
   It downloads SpinPi and SpinProj, installs them as a service, keeps the projector black until
   a print, and frees the UART pins for the ESP. Run it again any time to update.

### Way B: Linux, one command on the computer
With the card in the reader, from the SpinPrinter folder:
```bash
sudo bash SpinPi/make_sd_card.sh
```
It finds the card and asks you to type "yes", wipes it, writes Raspberry Pi OS Lite, asks for your Wi-Fi
(only you see the password) and sets up SSH login with this computer's key, no password. The Pi
installs SpinPi by itself on the first start: 10–20 minutes, keep it powered meanwhile.

## 3. Wire it up

![wiring](wiring.svg)

- **ESP32 ↔ Pi:** a USB cable (the board's USB-C to any Pi USB port) **or** 4 wires to the Pi header:
  pin 2 → 5V, pin 6 → GND, pin 8 (TX) → RX, pin 10 (RX) → TX. TX and RX cross.
- **Projector:** HDMI to the Pi.
- **Pi power:** the supply for your model (table above). A phone charger with a thin cable often gives "Undervoltage".
- **Motor (later):** TMC2209 driver: STEP → 1, DIR → 2, EN → 13, common GND; the motor has its own 12–24 V supply.

Wire everything with the power off.

## 4. First print

1. Switch the Pi on. When the ESP screen says **"Pi connected"**, it is ready. (The screen speaks English
   and Russian: Service → Display → Language.)
2. Open `http://spinslicer.local` (or scan the QR code: ESP screen → Connect).
3. **Service → Projector** on the ESP screen: "Grid" and "Centering" to focus and find the vial axis.
4. In SpinSlicer: Print → **"to printer"** → "Send to printer". Or drop the video on the printer page,
   or bring it on a USB stick.
5. After the print, answer "Under / Good / Over" on the screen: the printer adjusts the exposure itself.

## Troubleshooting

| you see | do this |
|---|---|
| no "Pi connected" | wait 20 minutes after the first start; check the cable (data) or that TX/RX are crossed |
| "Undervoltage" on a monitor | the right supply for your model (table above), a short thick cable without adapters |
| `spinslicer.local` does not open | are the PC and the Pi on the same network? On Windows without Bonjour use the IP from the ESP screen |
| way B: the Pi did not come up | put the card in a computer: the `bootfs` partition holds a boot diary, `spinprinter-log.txt` |
| service log | `ssh pi@spinslicer.local journalctl -u spinpi -n 50` |
| can the Pi keep up with the frames | `sudo systemctl stop spinpi && sudo /opt/spinpi/spinproj bench --rt --cpu 3` |
