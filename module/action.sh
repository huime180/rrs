#!/system/bin/sh
# KernelSU 动作按钮：重启 daemon 并输出状态
MOD=${0%/*}
[ -z "$MOD" ] || [ ! -d "$MOD" ] && MOD=/data/adb/modules/RRS
DAEMON="$MOD/system/bin/RRS_Daemon"

killall -TERM RRS_Daemon 2>/dev/null
for i in 1 2 3 4; do
    [ -z "$(pidof RRS_Daemon)" ] && break
    sleep 1
done
killall -9 RRS_Daemon 2>/dev/null
sleep 1
setsid "$DAEMON" </dev/null >/dev/null 2>&1 &
sleep 2

echo "RRS daemon: pid=$(pidof RRS_Daemon)"
"$DAEMON" --probe 2>/dev/null | grep -E '^(active|tiers|is_smart_fps|user_refresh_rate|gamebooster)'
