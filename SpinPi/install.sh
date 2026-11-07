#!/bin/bash
# SpinPi installer: run on the Raspberry Pi from this folder:  bash install.sh
set -e
cd "$(dirname "$0")"
echo "== 1/4 packages (mpv, ffmpeg, python3-serial)"
sudo apt-get update
sudo apt-get install -y mpv ffmpeg python3-serial
echo "== 2/4 files -> /opt/spinpi"
sudo mkdir -p /opt/spinpi /var/lib/spinpi
sudo cp -r spinpi.py web /opt/spinpi/
echo "== 3/4 no login text on the projector (tty1)"
sudo systemctl disable --now getty@tty1.service || true
echo "== 4/4 service"
sudo cp spinpi.service /etc/systemd/system/spinpi.service
sudo systemctl daemon-reload
sudo systemctl enable spinpi
sudo systemctl restart spinpi
sleep 2
systemctl --no-pager --lines=5 status spinpi || true
IP=$(hostname -I | awk '{print $1}')
echo
echo "Готово! Открой на телефоне или компьютере:  http://$(hostname).local   или   http://$IP"
echo "Логи:  journalctl -u spinpi -f"
