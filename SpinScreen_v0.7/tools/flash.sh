#!/bin/bash
# Прошить ESP32-S3 одной командой (Arduino IDE открывать не нужно):
#     bash SpinScreen_v0.7/tools/flash.sh
# Плата подключена к компьютеру USB-кабелем. Берёт arduino-cli из Arduino IDE (обычной или flatpak).
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
CLI="$(command -v arduino-cli || true)"
[ -n "$CLI" ] || CLI="$(ls /var/lib/flatpak/app/cc.arduino.IDE2/x86_64/stable/*/files/arduino-ide/resources/app/lib/backend/resources/arduino-cli 2>/dev/null | head -1 || true)"
[ -n "$CLI" ] || { echo "Нет arduino-cli: поставь Arduino IDE 2 и в нём ядро esp32 (Boards Manager)"; exit 1; }
CFG=()
[ -f "$HOME/.arduinoIDE/arduino-cli.yaml" ] && CFG=(--config-file "$HOME/.arduinoIDE/arduino-cli.yaml")
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc"
PORT="${1:-$(ls /dev/serial/by-id/*Espressif* /dev/ttyACM* 2>/dev/null | head -1 || true)}"
[ -n "$PORT" ] || { echo "Плата не найдена. Подключи ESP32 USB-кабелем (с данными, не только зарядка)."; exit 1; }
echo "== собираю прошивку"
"$CLI" "${CFG[@]}" compile --fqbn "$FQBN" --build-path "${TMPDIR:-/tmp}/spinscreen-build" SpinScreen
echo "== прошиваю $PORT"
if ! "$CLI" "${CFG[@]}" upload --fqbn "$FQBN" --input-dir "${TMPDIR:-/tmp}/spinscreen-build" -p "$PORT"; then
	echo
	echo "Не прошилось. Зажми BOOT, нажми и отпусти RESET, отпусти BOOT и запусти снова."
	echo "Нет доступа к порту? Manjaro/Arch:  sudo usermod -aG uucp \$USER   Ubuntu:  sudo usermod -aG dialout \$USER  (и перезайти)"
	exit 1
fi
echo "Готово! На экране должен появиться Spinny."
