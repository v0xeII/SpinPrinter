#!/bin/bash
# Pi's boot diary on the FAT boot partition, readable on any computer even if the Pi hangs:
# spinprinter-log.txt next to config.txt. Every 20 s for the first 40 minutes after power-on.
OUT=/boot/firmware/spinprinter-log.txt
for i in $(seq 120); do
	{
		echo "=== $(date '+%F %T')  up $(cut -d' ' -f1 /proc/uptime) s"
		echo "power: $(vcgencmd get_throttled 2>/dev/null)   (0x0 = ok; 0x50005 etc = undervoltage: weak power supply)"
		echo "temp:  $(vcgencmd measure_temp 2>/dev/null)"
		echo "--- network"
		nmcli -t -f DEVICE,STATE,CONNECTION device 2>/dev/null
		ip -4 -brief addr 2>/dev/null
		echo "--- services"
		systemctl is-active cloud-init-main cloud-final spinprinter-firstboot spinpi 2>/dev/null | paste -sd' '
		echo "--- install (last lines)"
		tail -n 15 /var/log/spinprinter-firstboot.log 2>/dev/null
		echo "--- spinpi (last lines)"
		journalctl -u spinpi -n 15 --no-pager 2>/dev/null
		echo "--- problems this boot"
		journalctl -b -p warning -n 40 --no-pager 2>/dev/null
	} >"$OUT.tmp" 2>&1
	mv "$OUT.tmp" "$OUT"
	sync
	sleep 20
done
