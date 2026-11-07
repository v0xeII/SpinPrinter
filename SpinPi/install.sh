#!/bin/bash
# SpinPi installer for Raspberry Pi OS Lite (the quick way to try the printer before SpinOS).
# Copy the whole SpinPrinter folder to the Pi, then:  bash SpinPi/install.sh
set -e
cd "$(dirname "$0")"
echo "== 1/5 packages (ffmpeg, python3-serial, compiler for SpinProj, mpv as the old fallback)"
sudo apt-get update
sudo apt-get install -y ffmpeg python3-serial mpv gcc make pkg-config liblz4-dev libdrm-dev
echo "== 2/5 SpinProj (frames by angle)"
if [ -d ../SpinProj ]; then
  make -C ../SpinProj NO_SDL=1 clean spinproj
else
  echo "нет папки SpinProj рядом с SpinPi: скопируй на Pi всю папку SpinPrinter" && exit 1
fi
echo "== 3/5 files -> /opt/spinpi"
sudo mkdir -p /opt/spinpi /var/lib/spinpi
sudo cp -r spinpi.py web /opt/spinpi/
sudo install -m 755 ../SpinProj/spinproj /opt/spinpi/spinproj
echo "== 4/5 no login text on the projector (tty1)"
sudo systemctl disable --now getty@tty1.service || true
echo "== 5/5 service"
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
echo "Замер проектора (служба должна быть остановлена):"
echo "  sudo systemctl stop spinpi && sudo /opt/spinpi/spinproj bench --rt --cpu 3; sudo systemctl start spinpi"
