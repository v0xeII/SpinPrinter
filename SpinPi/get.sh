#!/bin/bash
# SpinPrinter on a Raspberry Pi with Raspberry Pi OS Lite (written by Raspberry Pi Imager on
# Windows, Mac or Linux). Run on the Pi (over SSH):
#
#     curl -fsSL https://raw.githubusercontent.com/v0xeII/SpinPrinter/main/SpinPi/get.sh | sudo bash
#
# Downloads SpinPi + SpinProj of the LATEST RELEASE (the same version the flasher puts on the
# board), installs them as a service, makes the projector stay black, then reboots. Safe to run
# again: that is how you update.
#
# Another version:  ... | sudo SPINPRINTER_REF=v0.8.1 bash    (a tag, or a branch such as main)
set -euo pipefail
REPO="${SPINPRINTER_REPO:-v0xeII/SpinPrinter}"
REF="${SPINPRINTER_REF:-${SPINPRINTER_BRANCH:-}}"
SRC=/opt/spinprinter-src

[ "$(id -u)" = 0 ] || { echo "run with sudo:  curl -fsSL … | sudo bash"; exit 1; }

if [ -z "$REF" ]; then
	# github.com/<repo>/releases/latest redirects to .../releases/tag/<the latest tag>
	LATEST="$(curl -fsSLI -o /dev/null -w '%{url_effective}' "https://github.com/$REPO/releases/latest" || true)"
	REF="${LATEST##*/releases/tag/}"
	if [ -z "$LATEST" ] || [ "$REF" = "$LATEST" ]; then
		echo "!! could not find the latest release of $REPO (no internet, or no release yet)"
		echo "!! to install the development version anyway:  ... | sudo SPINPRINTER_REF=main bash"
		exit 1
	fi
fi
case "$REF" in
	v[0-9]*) URL="https://github.com/$REPO/archive/refs/tags/$REF.tar.gz" ;;
	*) URL="https://github.com/$REPO/archive/refs/heads/$REF.tar.gz"
	   echo "!! $REF is a branch, not a release: the board's firmware may not match it" ;;
esac

echo "== SpinPrinter $REF: downloading $REPO"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fsSL "$URL" | tar -xz -C "$TMP"
rm -rf "$SRC"
mkdir -p "$SRC"
cp -r "$TMP"/*/SpinPi "$TMP"/*/SpinProj "$SRC/"
echo "$REF" >"$SRC/REF"
bash "$SRC/SpinPi/pi_setup.sh"
bash "$SRC/SpinPi/install.sh"
echo
echo "== SpinPrinter $REF installed. The board needs the same version: the flasher page flashes the latest release."
# the update from the printer's screen (Settings → System) runs this with SPINPRINTER_NO_REBOOT=1:
# install.sh has restarted SpinPi already, a reboot is not needed for an update
if [ -n "${SPINPRINTER_NO_REBOOT:-}" ]; then exit 0; fi
echo "== Rebooting in 10 s (the projector settings need it). Ctrl+C to stay."
sleep 10
reboot
