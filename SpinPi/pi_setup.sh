#!/bin/bash
# Raspberry Pi OS settings for the printer (idempotent; used by get.sh, needs a reboot):
#   * the projector stays black: no rainbow splash, no boot text, no blinking cursor
#   * GPIO14/15 (pins 8, 10) are the three-wire link to the ESP32: the PL011 UART goes there,
#     Bluetooth is off, no Linux console or login on it
#   * CPU 3 is kept free for SpinProj (isolcpus=3)
set -euo pipefail
BOOT=/boot/firmware
[ -d "$BOOT" ] || BOOT=/boot
CMD="$BOOT/cmdline.txt"
CFG="$BOOT/config.txt"
cp -n "$CMD" "$CMD.before-spinprinter" 2>/dev/null || true
cp -n "$CFG" "$CFG.before-spinprinter" 2>/dev/null || true

line="$(head -n1 "$CMD")"
for w in console=tty1 console=serial0,115200 console=ttyAMA0,115200 console=ttyS0,115200 splash; do
	line="$(echo " $line " | sed "s/ $w / /g")"
done
for w in quiet loglevel=0 logo.nologo vt.global_cursor_default=0 consoleblank=0 isolcpus=3; do
	echo " $line " | grep -q " ${w%%=*}[= ]" || line="$line $w"
done
echo "$line" | tr -s ' ' | sed 's/^ //; s/ $//' >"$CMD"

if ! grep -q "^# SpinPrinter" "$CFG"; then
	cat >>"$CFG" <<'CFGEOF'

[all]
# SpinPrinter: the projector stays black from power-on; GPIO14/15 are the link to the ESP32
disable_splash=1
boot_delay=0
dtparam=audio=off
enable_uart=1
dtoverlay=disable-bt
CFGEOF
fi
for unit in serial-getty@ttyAMA0 serial-getty@serial0 serial-getty@ttyS0 hciuart; do
	systemctl mask "$unit.service" >/dev/null 2>&1 || true
done
echo "pi_setup: done (reboot to apply)"
