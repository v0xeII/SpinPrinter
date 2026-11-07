#!/bin/bash
# Runs once, on the Pi's first start (spinprinter-firstboot.service, made by make_sd_card.sh):
# waits for the internet, installs SpinPi + SpinProj, then switches itself off.
set -u
echo "=== $(date '+%F %T') SpinPrinter first start"
for i in $(seq 120); do  # up to an hour without internet: keep trying, the Wi-Fi may come late
	if apt-get update -q; then break; fi
	echo "нет интернета, жду ($i)…"
	sleep 30
done
if bash /opt/spinprinter-src/SpinPi/install.sh; then
	mkdir -p /var/lib/spinprinter
	touch /var/lib/spinprinter/installed
	systemctl disable spinprinter-firstboot.service
	echo "=== $(date '+%F %T') готово"
else
	echo "=== $(date '+%F %T') установка не удалась, попробую при следующем включении"
	exit 1
fi
