#!/bin/bash
# Build the firmware and put it, with its manifest, next to the browser flasher (flasher/fw/).
# GitHub Pages then serves it at https://v0xeii.github.io/SpinPrinter/flasher/ . Run it after
# changing the firmware, before committing:   bash SpinScreen_v0.7/tools/update_flasher.sh
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.."
CLI="$(command -v arduino-cli || true)"
[ -n "$CLI" ] || CLI="$(ls /var/lib/flatpak/app/cc.arduino.IDE2/x86_64/stable/*/files/arduino-ide/resources/app/lib/backend/resources/arduino-cli 2>/dev/null | head -1 || true)"
[ -n "$CLI" ] || { echo "Нет arduino-cli: поставь Arduino IDE 2 и ядро esp32"; exit 1; }
CFG=()
[ -f "$HOME/.arduinoIDE/arduino-cli.yaml" ] && CFG=(--config-file "$HOME/.arduinoIDE/arduino-cli.yaml")
B="${TMPDIR:-/tmp}/spinscreen-build"
# no paths of this computer (user name, folders) may end up inside the published firmware:
# build a copy from a neutral folder and map the home folder away
SRC="${TMPDIR:-/tmp}/spinscreen-src"
rm -rf "$B" "$SRC" && mkdir -p "$SRC" && cp -r SpinScreen_v0.7/SpinScreen "$SRC/"
MAP="-ffile-prefix-map=$HOME=/build -ffile-prefix-map=$SRC=/src"
"$CLI" "${CFG[@]}" compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --build-path "$B" \
	--build-property "compiler.c.extra_flags=$MAP" --build-property "compiler.cpp.extra_flags=$MAP" \
	"$SRC/SpinScreen"
if strings -n 6 "$B/SpinScreen.ino.bin" | grep -qE "$HOME|$(whoami)"; then
	echo "the firmware still contains paths of this computer, not copying it"; exit 1
fi
VER=$(sed -n 's/#define FW_VERSION "\(.*\)"/\1/p' SpinScreen_v0.7/SpinScreen/config.h)
mkdir -p flasher/fw
cp "$B/SpinScreen.ino.bootloader.bin" flasher/fw/bootloader.bin
cp "$B/SpinScreen.ino.partitions.bin" flasher/fw/partitions.bin
cp "$B/SpinScreen.ino.bin" flasher/fw/spinscreen.bin
cp "$(find "$HOME/.arduino15/packages/esp32/hardware/esp32" -name boot_app0.bin | head -1)" flasher/fw/boot_app0.bin
cp docs/wiring.svg docs/wiring.ru.svg flasher/
# separate parts, not one merged image: the settings partition (nvs, 0x9000) is left alone
cat >flasher/manifest.json <<JSON
{
  "name": "SpinScreen",
  "version": "$VER",
  "new_install_prompt_erase": false,
  "builds": [{
    "chipFamily": "ESP32-S3",
    "parts": [
      {"path": "fw/bootloader.bin", "offset": 0},
      {"path": "fw/partitions.bin", "offset": 32768},
      {"path": "fw/boot_app0.bin", "offset": 57344},
      {"path": "fw/spinscreen.bin", "offset": 65536}
    ]
  }]
}
JSON
echo "flasher/: SpinScreen $VER"
