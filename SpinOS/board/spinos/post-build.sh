#!/bin/sh
# SpinOS: final touches to the root file system (it becomes a read-only squashfs after this)
set -eu
T="$TARGET_DIR"

mkdir -p "$T/data"
# things that must survive a reboot live on /data
ln -snf /data/ssh "$T/root/.ssh"

# dnsmasq only runs as the access point's DHCP/DNS, started by spinnet
rm -f "$T/etc/init.d/S80dnsmasq"

# the printer service, restarted by init if it ever stops. SpinProj runs on CPU 3, which the
# kernel keeps free (isolcpus=3), with real-time priority.
sed -i '/spinpi.py/d' "$T/etc/inittab"
cat >>"$T/etc/inittab" <<'TAB'
# SpinPi: web page, ESP32, projector (logs in /tmp/spinpi.log)
::respawn:/bin/sh -c 'exec /usr/bin/python3 /opt/spinpi/spinpi.py --data /data/spinpi --port 80 --projector auto --spinproj-arg=--rt --spinproj-arg=--cpu=3 >>/tmp/spinpi.log 2>&1'
TAB

# image version for the "О принтере" screen and bug reports
echo "SpinOS $(date -u +%Y-%m-%d) $(cd "$BR2_EXTERNAL_SPINOS_PATH" && git describe --always --dirty 2>/dev/null || echo dev)" \
	>"$T/etc/spinos-version"
