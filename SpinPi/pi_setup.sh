#!/bin/bash
# Raspberry Pi OS settings for the printer (idempotent; used by get.sh, needs a reboot):
#   * the projector stays black: no rainbow splash, no boot text, no blinking cursor
#   * CPU 3 is kept free for SpinProj (isolcpus=3)
set -euo pipefail
BOOT=/boot/firmware
[ -d "$BOOT" ] || BOOT=/boot
CMD="$BOOT/cmdline.txt"
CFG="$BOOT/config.txt"
cp -n "$CMD" "$CMD.before-spinprinter" 2>/dev/null || true
cp -n "$CFG" "$CFG.before-spinprinter" 2>/dev/null || true

line="$(head -n1 "$CMD")"
for w in console=tty1 splash; do
	line="$(echo " $line " | sed "s/ $w / /g")"
done
for w in quiet loglevel=0 logo.nologo vt.global_cursor_default=0 consoleblank=0 isolcpus=3; do
	echo " $line " | grep -q " ${w%%=*}[= ]" || line="$line $w"
done
echo "$line" | tr -s ' ' | sed 's/^ //; s/ $//' >"$CMD"

if ! grep -q "^# SpinPrinter" "$CFG"; then
	cat >>"$CFG" <<'CFGEOF'

[all]
# SpinPrinter: the projector stays black from power-on (the ESP32 is on the USB cable)
disable_splash=1
boot_delay=0
dtparam=audio=off
CFGEOF
fi
echo "pi_setup: done (reboot to apply)"
