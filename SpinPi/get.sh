#!/bin/bash
# SpinPrinter on a Raspberry Pi with Raspberry Pi OS Lite (written by Raspberry Pi Imager on
# Windows, Mac or Linux). Run on the Pi (over SSH):
#
#     curl -fsSL https://raw.githubusercontent.com/v0xeII/SpinPrinter/main/SpinPi/get.sh | sudo bash
#
# Downloads SpinPi + SpinProj, installs them as a service, makes the projector stay black and
# frees the UART pins for the ESP32 (wires), then reboots. Safe to run again (updates).
set -euo pipefail
REPO="${SPINPRINTER_REPO:-v0xeII/SpinPrinter}"
BRANCH="${SPINPRINTER_BRANCH:-main}"
SRC=/opt/spinprinter-src

[ "$(id -u)" = 0 ] || { echo "run with sudo:  curl -fsSL … | sudo bash"; exit 1; }
echo "== SpinPrinter: downloading $REPO ($BRANCH)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fsSL "https://github.com/$REPO/archive/refs/heads/$BRANCH.tar.gz" | tar -xz -C "$TMP"
rm -rf "$SRC"
mkdir -p "$SRC"
cp -r "$TMP"/*/SpinPi "$TMP"/*/SpinProj "$SRC/"
bash "$SRC/SpinPi/pi_setup.sh"
bash "$SRC/SpinPi/install.sh"
echo
echo "== Rebooting in 10 s (the projector and UART settings need it). Ctrl+C to stay."
sleep 10
reboot
