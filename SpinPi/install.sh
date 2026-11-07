#!/bin/bash
# SpinPi installer for Raspberry Pi OS Lite (the quick way to try the printer before SpinOS).
# Copy the whole SpinPrinter folder to the Pi, then:  bash SpinPi/install.sh
set -e
cd "$(dirname "$0")"
echo "== 1/5 packages (ffmpeg, python3-serial, the compiler for SpinProj)"
sudo apt-get update
sudo apt-get install -y --no-install-recommends ffmpeg python3-serial gcc make pkg-config libc6-dev liblz4-dev libdrm-dev avahi-daemon
# the old video player (spinpi.py --engine mpv) is not needed any more:  sudo apt install mpv
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
# mDNS: SpinSlicer finds the printer as _spinprinter._tcp (spinpi.py fills in its own name and id)
sudo mkdir -p /etc/avahi/services
[ -f /etc/avahi/services/spinprinter.service ] || sudo install -m 644 spinprinter.avahi.service /etc/avahi/services/spinprinter.service
sudo systemctl enable --now avahi-daemon || true
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
