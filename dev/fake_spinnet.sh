#!/bin/sh
# Stand-in for SpinOS's /usr/sbin/spinnet in tests: remembers the mode in $SPINNET_STATE.
S=${SPINNET_STATE:-/tmp/fake_spinnet}
case "$1" in
status) echo "mode=$(cat $S 2>/dev/null || echo ap)"; echo "ap_name=SpinSlicer-TEST"; echo "ip=192.168.4.1"
        [ "$(cat $S 2>/dev/null)" = client ] && echo "ssid=$(cat $S.ssid)" ;;
scan) printf -- '-48.00\tДомашняя сеть\n-71.00\tСосед 5G\n' ;;
connect) echo "$2" >$S.ssid; echo client >$S ;;
forget|ap) echo ap >$S ;;
esac
