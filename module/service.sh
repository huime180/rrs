#!/system/bin/sh
MODDIR=$(cd "$(dirname "$0")" && pwd)
DAEMON="$MODDIR/system/bin/RRS_Daemon"

[ -x "$DAEMON" ] || exit 0

killall -TERM RRS_Daemon 2>/dev/null
for i in 1 2; do
    [ -z "$(pidof RRS_Daemon)" ] && break
    sleep 1
done
killall -9 RRS_Daemon 2>/dev/null
sleep 1

for i in $(seq 1 120); do
    [ "$(getprop sys.boot_completed)" = "1" ] && break
    sleep 1
done
[ -s /data/adb/RRS/sys_defaults ] || sh "$MODDIR/tools/gen_sys_defaults.sh" >/dev/null 2>&1

n=0
while [ "$n" -lt 3 ]; do
    setsid "$DAEMON" </dev/null >/dev/null 2>&1 &
    sleep 6
    [ -n "$(pidof RRS_Daemon)" ] && break
    n=$((n + 1))
done
