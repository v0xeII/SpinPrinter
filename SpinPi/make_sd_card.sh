#!/bin/bash
# SD-карта для принтера: Raspberry Pi OS Lite 64-bit + SpinPi, которая сама ставится при первом
# включении Pi. Запуск на компьютере (Linux), карта в картридере:
#
#     sudo bash SpinPi/make_sd_card.sh
#
# Что делает:
#   1. находит SD-карту (только съёмные диски до 128 ГБ) и спрашивает «точно?»;
#   2. спрашивает твой Wi-Fi (пароль вводишь сам, он нигде не показывается);
#   3. стирает всё старое на карте и записывает Raspberry Pi OS Lite 64-bit
#      (без рабочего стола: ничего не светит на проектор);
#   4. настраивает: имя spinslicer, вход по SSH-ключу этого компьютера (без пароля), Wi-Fi,
#      чёрный экран при загрузке, SpinPi + SpinProj, которые поставятся сами при первом включении.
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
ROOT="$(dirname "$HERE")"
IMG_URL="https://downloads.raspberrypi.com/raspios_lite_arm64/images/raspios_lite_arm64-2026-10-06/2026-10-06-raspios-trixie-arm64-lite.img.xz"
IMG_NAME="$(basename "$IMG_URL")"

say() { printf '\n\033[1;35m== %s\033[0m\n' "$*"; }
die() { printf '\n\033[1;31mОшибка: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "запусти через sudo:  sudo bash SpinPi/make_sd_card.sh"
USER_NAME="${SUDO_USER:-}"
[ -n "$USER_NAME" ] && [ "$USER_NAME" != root ] || die "запусти через sudo от своего пользователя, не из-под root"
USER_HOME="$(getent passwd "$USER_NAME" | cut -d: -f6)"
CACHE="$USER_HOME/.cache/spinprinter"
for t in xz dd lsblk partprobe mount umount wipefs; do command -v $t >/dev/null || die "нет программы $t"; done

# ---------------------------------------------------------------- 1. which disk
say "1/6 Ищу SD-карту"
mapfile -t CANDS < <(lsblk -dbnpo NAME,RM,SIZE,TYPE,TRAN | awk '$2==1 && $4=="disk" && $3>1e9 && $3<128e9 {print $1}')
if [ "${#CANDS[@]}" -eq 0 ]; then
	die "не вижу SD-карту. Вставь её в картридер и запусти снова."
elif [ "${#CANDS[@]}" -eq 1 ]; then
	DEV="${CANDS[0]}"
else
	echo "Нашёл несколько съёмных дисков:"
	for d in "${CANDS[@]}"; do lsblk -dno NAME,SIZE,MODEL "$d"; done
	read -rp "Какой из них SD-карта? (например /dev/sdb): " DEV
	printf '%s\n' "${CANDS[@]}" | grep -qx "$DEV" || die "$DEV не в списке съёмных дисков"
fi
ROOTDEV="$(findmnt -no SOURCE / | sed -E 's/p?[0-9]+$//')"
[ "$DEV" != "$ROOTDEV" ] || die "$DEV — это системный диск компьютера"
echo
lsblk -o NAME,SIZE,MODEL,LABEL,FSTYPE,MOUNTPOINTS "$DEV"
echo
echo "ВСЁ на $DEV будет стёрто: старая система и все данные на ней."
read -rp "Записать принтер на эту карту? Напиши «да»: " OK
[ "$OK" = "да" ] || [ "$OK" = "yes" ] || die "отменено, карта не тронута"

part() { case "$DEV" in *mmcblk*|*nvme*|*loop*) echo "${DEV}p$1" ;; *) echo "${DEV}$1" ;; esac; }
WORK="$(mktemp -d /tmp/spinsd.XXXXXX)"
chmod 700 "$WORK"
cleanup() {
	for m in "$WORK/boot" "$WORK/root"; do mountpoint -q "$m" && umount "$m"; done
	rm -rf "$WORK"
}
trap cleanup EXIT
mkdir -p "$WORK"/{boot,root}

# ---------------------------------------------------------------- 2. your Wi-Fi
say "2/6 Wi-Fi для принтера"
echo "Pi подключится к твоему роутеру. Пароль вводишь только ты: он не показывается и"
echo "записывается только на карту. Без Wi-Fi можно подключить Pi кабелем к роутеру: тогда Enter."
while true; do
	read -rp "Имя Wi-Fi сети (как в телефоне): " SSID
	[ -n "$SSID" ] && break
	read -rp "Без Wi-Fi Pi нужен сетевой кабель от роутера. Будет кабель? (да/нет): " CABLE
	[ "$CABLE" = "да" ] || [ "$CABLE" = "yes" ] && break
done
PSK=""
if [ -n "$SSID" ]; then
	while true; do
		read -rsp "Пароль от «$SSID»: " PSK
		echo
		read -rsp "Ещё раз пароль: " PSK2
		echo
		[ "$PSK" = "$PSK2" ] && break
		echo "Пароли не совпали, давай ещё раз."
	done
	PSK2=""
	[ -z "$PSK" ] || [ "${#PSK}" -ge 8 ] || die "пароль Wi-Fi короче 8 символов: так не бывает, проверь"
else
	echo "Хорошо: при первом включении подключи Pi сетевым кабелем к роутеру."
fi
for p in $(lsblk -lnpo NAME "$DEV" | tail -n +2); do umount "$p" 2>/dev/null || true; done

# ---------------------------------------------------------------- 3. the image
say "3/6 Raspberry Pi OS Lite 64-bit"
mkdir -p "$CACHE"
if [ ! -s "$CACHE/$IMG_NAME" ]; then
	command -v curl >/dev/null || die "нет curl"
	echo "Скачиваю $IMG_NAME (550 МБ) с downloads.raspberrypi.com"
	curl -fL --progress-bar -o "$CACHE/$IMG_NAME.part" "$IMG_URL"
	mv "$CACHE/$IMG_NAME.part" "$CACHE/$IMG_NAME"
	chown -R "$USER_NAME": "$CACHE"
fi
if curl -fsSL -o "$WORK/img.sha256" "$IMG_URL.sha256" 2>/dev/null; then
	(cd "$CACHE" && sed "s#  .*#  $IMG_NAME#" "$WORK/img.sha256" | sha256sum -c --quiet) || die "образ повреждён, удали $CACHE/$IMG_NAME и запусти снова"
	echo "Образ проверен (sha256)."
fi

# ---------------------------------------------------------------- 4. write
say "4/6 Стираю старое и записываю на $DEV (5–15 минут, зависит от карты)"
# old data (the seller's system and networks) goes: partition table and file system marks
# first, then the whole card is told it is empty (not every card reader supports that)
for p in $(lsblk -lnpo NAME "$DEV" | tail -n +2); do wipefs -aq "$p" 2>/dev/null || true; done
wipefs -aq "$DEV"
blkdiscard -f "$DEV" 2>/dev/null && echo "Карта очищена целиком." || true
xz -dc "$CACHE/$IMG_NAME" | dd of="$DEV" bs=4M conv=fsync status=progress
sync
partprobe "$DEV" 2>/dev/null || blockdev --rereadpt "$DEV" 2>/dev/null || true
for i in $(seq 20); do [ -b "$(part 2)" ] && break; sleep 0.5; done
sleep 1
for p in $(lsblk -lnpo NAME "$DEV" | tail -n +2); do umount "$p" 2>/dev/null || true; done  # the desktop may automount
mount "$(part 1)" "$WORK/boot"
mount "$(part 2)" "$WORK/root"

# ---------------------------------------------------------------- 5. settings
say "5/6 Настраиваю"
# SSH key of this computer: log in to the printer without any password
KEY="$USER_HOME/.ssh/id_ed25519"
if [ ! -s "$KEY.pub" ]; then
	sudo -u "$USER_NAME" mkdir -p "$USER_HOME/.ssh"
	sudo -u "$USER_NAME" ssh-keygen -q -t ed25519 -N "" -C "$USER_NAME@spinprinter" -f "$KEY"
	echo "Сделал SSH-ключ: $KEY"
fi
PUB="$(cat "$KEY.pub")"
cat >"$WORK/boot/user-data" <<EOF
#cloud-config
hostname: spinslicer
timezone: Europe/Moscow
keyboard:
  model: pc105
  layout: "ru"
enable_ssh: true
ssh_pwauth: false
users:
- name: pi
  groups: users,adm,dialout,audio,netdev,video,plugdev,input,gpio,render,sudo
  shell: /bin/bash
  sudo: ALL=(ALL) NOPASSWD:ALL
  lock_passwd: true
  ssh_authorized_keys:
  - $PUB
EOF
if [ -n "$SSID" ]; then  # the same file Raspberry Pi Imager writes; cloud-init applies it on the first start
	q() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
	cat >"$WORK/boot/network-config" <<EOF
network:
  version: 2
  wifis:
    wlan0:
      dhcp4: true
      regulatory-domain: "RU"
      access-points:
        "$(q "$SSID")":
          password: "$(q "$PSK")"
      optional: true
EOF
fi
PSK=""

# the projector stays black: no rainbow, no boot text, no blinking cursor, no login prompt
# console=serial0 goes too: GPIO14/15 are the wires to the ESP32, Linux must not talk there
sed -i -E 's/ ?console=tty1//; s/ ?console=serial0,[0-9]+//; s/ ?splash//; s/ ?quiet//' "$WORK/boot/cmdline.txt"
sed -i -E 's/$/ quiet loglevel=0 logo.nologo vt.global_cursor_default=0 consoleblank=0 isolcpus=3 cfg80211.ieee80211_regdom=RU/' "$WORK/boot/cmdline.txt"
cat >>"$WORK/boot/config.txt" <<'EOF'

[all]
# SpinPrinter: the projector stays black from power-on
disable_splash=1
boot_delay=0
dtparam=audio=off
# the ESP32 can also be on three wires: GPIO14 TX (pin 8), GPIO15 RX (pin 10), GND (pin 6).
# The good UART (PL011, /dev/serial0) goes to those pins; Bluetooth is not used.
enable_uart=1
dtoverlay=disable-bt
[pi5]
# Pi 5: GPIO14/15 are UART0 (/dev/ttyAMA0); /dev/serial0 there is the debug connector
dtparam=uart0=on
[all]
EOF
# nothing on the wires to the ESP32. NOT getty@tty1: Raspberry Pi OS switches it back on while
# cloud-init creates the user, and with it masked the user was never made (the "Please enter new
# username" wizard came up instead). install.sh turns the screen login off once SpinPi is there.
for unit in serial-getty@ttyAMA0 serial-getty@serial0 serial-getty@ttyS0 hciuart; do
	ln -sf /dev/null "$WORK/root/etc/systemd/system/$unit.service"
done

# SpinPi + SpinProj: copied now, installed by the Pi itself on the first start (needs internet)
mkdir -p "$WORK/root/opt/spinprinter-src"
cp -r "$ROOT/SpinPi" "$ROOT/SpinProj" "$WORK/root/opt/spinprinter-src/"
rm -f "$WORK/root/opt/spinprinter-src/SpinProj/spinproj"
cat >"$WORK/root/etc/systemd/system/spinprinter-firstboot.service" <<'EOF'
[Unit]
Description=SpinPrinter: install SpinPi + SpinProj on the first start
# not After=cloud-init.target: that one comes after multi-user.target, systemd saw a cycle and
# dropped this job. firstboot.sh waits for the internet itself.
After=network-online.target
Wants=network-online.target
ConditionPathExists=!/var/lib/spinprinter/installed

[Service]
Type=oneshot
ExecStart=/bin/bash /opt/spinprinter-src/SpinPi/firstboot.sh
StandardOutput=append:/var/log/spinprinter-firstboot.log
StandardError=append:/var/log/spinprinter-firstboot.log
TimeoutStartSec=infinity

[Install]
WantedBy=multi-user.target
EOF
mkdir -p "$WORK/root/etc/systemd/system/multi-user.target.wants"
ln -sf /etc/systemd/system/spinprinter-firstboot.service \
	"$WORK/root/etc/systemd/system/multi-user.target.wants/spinprinter-firstboot.service"

# boot diary on the boot partition (readable on any computer, even if the Pi hangs)
cat >"$WORK/root/etc/systemd/system/spinprinter-bootlog.service" <<'EOF'
[Unit]
Description=SpinPrinter: boot diary on the boot partition (spinprinter-log.txt)
After=local-fs.target

[Service]
Type=simple
ExecStart=/bin/bash /opt/spinprinter-src/SpinPi/bootlog.sh

[Install]
WantedBy=multi-user.target
EOF
ln -sf /etc/systemd/system/spinprinter-bootlog.service \
	"$WORK/root/etc/systemd/system/multi-user.target.wants/spinprinter-bootlog.service"
echo "Записано: $(date '+%F %T'). Если Pi не загрузится, здесь появится spinprinter-log.txt." >"$WORK/boot/spinprinter-log.txt"

# ---------------------------------------------------------------- 6. done
say "6/6 Завершаю запись"
sync
umount "$WORK/boot" "$WORK/root"
echo
echo "Готово! Карта для принтера записана."
echo
echo "  1. Вставь карту в Raspberry Pi."
echo "  2. ESP32 с экраном подключи к Pi USB-кабелем (любой USB-порт Pi)."
[ -n "$SSID" ] || echo "     И сетевой кабель от роутера в Pi: без Wi-Fi ей нужен интернет по кабелю."
echo "  3. Включи питание Pi (свой блок: Pi 3 и Zero 2 W — 5 В 2,5 А, Pi 4 — 3 А, Pi 5 — 5 А)."
echo "     Первый запуск: 10–20 минут"
echo "     (Pi сама скачает и поставит программы). Не выключай её всё это время."
echo "  4. Когда на экране ESP появится «Pi на связи», открой  http://spinslicer.local"
echo
echo "  Зайти на Pi (без пароля):  ssh pi@spinslicer.local"
echo "  Как идёт установка:        ssh pi@spinslicer.local tail -f /var/log/spinprinter-firstboot.log"
echo "  Не работает? Вынь карту, вставь в компьютер: в разделе bootfs файл spinprinter-log.txt."
