# SpinOS: a dedicated system image for the printer (Raspberry Pi 3 for now)

[Русский](README.md) · **English**

Write one file to a card, put it in the Pi, switch the printer on. No config files, no SSH.

> **Status: built to the drawing, not yet run on hardware.** Buildroot accepts every setting;
> whether a Pi boots this image, the first card will tell. Until then use Raspberry Pi OS
> with [`get.sh` or `make_sd_card.sh`](../docs/INSTALL.md).

## Inside

- **Raspberry Pi Linux 6.12 with PREEMPT_RT** (real time), built with Buildroot 2026.02.3.
- **Read-only system** (squashfs): pull the power any time. Everything that changes (videos,
  settings, log, Wi-Fi) lives on `/data`, and SpinPi writes so a power cut leaves the old file whole.
- **The projector is black from power-on:** no rainbow square (`disable_splash=1`), no text console
  and no penguin on HDMI (switched off in the kernel).
- **ESP32 over USB or wires** (GPIO14/15): no console and no Bluetooth on the UART pins, they are the ESP's.
- **SpinProj on its own CPU core:** core 3 is taken out of normal scheduling (`isolcpus=3`), SpinProj
  runs there with real-time priority. Constant CPU clock.
- **Wi-Fi without config files:**
  1. First start: the printer runs its own network **SpinSlicer-XXXX**, and the ESP32 screen shows a
     QR code for it. Point the phone, join, the printer page opens by itself.
  2. On the page: "Find networks" → pick the home one → password → "Connect".
  3. From then on the printer joins your home network, page: `http://spinslicer.local`. If the
     network is gone or the password is wrong, SpinSlicer-XXXX comes back within a minute.
- **SSH off.** It switches on when you put your key in `/data/ssh/authorized_keys`. No passwords at all.

## Card partitions

| | | |
|---|---|---|
| 1 | `SPINBOOT` (FAT, 48 MB) | Pi firmware, kernel, `config.txt`, `cmdline.txt` |
| 2 | root (squashfs, read only) | the system + SpinPi + SpinProj |
| 3 | `SPINDATA` (ext4) | `/data`; grows to the whole card on the first start |

## Getting the image

**GitHub Actions:** **Actions** → **spinos** → **Run workflow** (1–2 hours); `spinos-rpi3.img.xz` is in
the run's **Artifacts**. A tag `v0.1.0` also puts it in **Releases**.

**On your computer** (~15 GB free, 1–2 hours; the folder path must have no spaces):
```bash
wget https://buildroot.org/downloads/buildroot-2026.02.3.tar.xz
tar xf buildroot-2026.02.3.tar.xz
make -C buildroot-2026.02.3 BR2_EXTERNAL=$PWD/SpinOS O=$PWD/out spinos_rpi3_defconfig
make -C buildroot-2026.02.3 O=$PWD/out
```
Image: `out/images/sdcard.img`. **Write it:** Raspberry Pi Imager → "Choose OS" → "Use custom".
Imager's settings (name, Wi-Fi, SSH) are not used by SpinOS.

## What to check on the first card

1. The projector (or a monitor) stays **dark** from power-on until a print: no rainbow, no text.
2. How many seconds until the page opens (the goal is about 10 s).
3. The SpinSlicer-XXXX network, the QR code on the ESP32 screen, the page opening by itself.
4. Joining the home network, `spinslicer.local` opens.
5. `/data` grew to the whole card (`df -h /data` over SSH).
6. `spinproj bench --rt --cpu 3`: vsync and frame preparation numbers (needs the SSH key, see above).
7. Pull the power while a video uploads: after power-on everything is in place.

SpinPi's log is `/tmp/spinpi.log` (over SSH). There is no console on the UART: those pins are the ESP32's.

## Files

| | |
|---|---|
| `configs/spinos_rpi3_defconfig` | what goes into the system |
| `board/spinos/config.txt`, `cmdline.txt` | Pi boot: black screen, KMS, core 3 for the projector |
| `board/spinos/linux.fragment` | kernel: PREEMPT_RT, no console or logo, constant clock |
| `board/spinos/rootfs_overlay/usr/sbin/spinnet` | Wi-Fi: home network or its own access point |
| `board/spinos/rootfs_overlay/etc/init.d/S01data` | `/data`, grown on the first start |
| `board/spinos/post-build.sh` | starts SpinPi (and restarts it if it ever stops) |
| `package/spinpi`, `package/spinproj` | how Buildroot installs our programs |
