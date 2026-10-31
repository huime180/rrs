#!/system/bin/sh
# 先让 daemon 正常退出（恢复它改过的 peak/min），再清理配置与运行时文件。

killall -TERM RRS_Daemon 2>/dev/null
for i in 1 2 3 4; do
    [ -z "$(pidof RRS_Daemon)" ] && break
    sleep 1
done
killall -9 RRS_Daemon 2>/dev/null

rm -rf /data/adb/RRS
rm -f /sdcard/Android/Game/RRS_GameList 2>/dev/null
rm -f /storage/emulated/0/Android/Game/RRS_GameList 2>/dev/null
rmdir /sdcard/Android/Game 2>/dev/null
rmdir /storage/emulated/0/Android/Game 2>/dev/null
rm -f /data/local/tmp/RRS_Daemon 2>/dev/null

exit 0
