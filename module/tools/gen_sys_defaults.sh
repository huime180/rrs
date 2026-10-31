#!/system/bin/sh
# 读取系统默认刷新率配置，生成 /data/adb/RRS/sys_defaults
# 用法: gen_sys_defaults.sh [原厂 device_features xml] [输出文件]
XML="$1"
OUT="${2:-/data/adb/RRS/sys_defaults}"
MODDIR=$(cd "$(dirname "$0")/.." && pwd)
SQL="$MODDIR/tools/sqlite3"
[ -x "$SQL" ] || SQL=/data/adb/RRS/sqlite3
DEV=$(getprop ro.product.device)
[ -n "$XML" ] || XML="/product/etc/device_features/$DEV.xml"

mkdir -p /data/adb/RRS
: > "$OUT"

if [ -f "$XML" ]; then
    LIST=$(sed -n '/name="fpsList"/,/<\/integer-array>/p' "$XML" 2>/dev/null | grep -o '<item>[0-9]*</item>' | sed 's/<[^>]*>//g' | sort -rn -u | tr '\n' ',' | sed 's/,$//')
    MAXF=$(grep -o 'name="support_max_fps">[0-9]*' "$XML" 2>/dev/null | grep -o '[0-9]*$')
    AOD=$(grep -o 'name="support_aod_fullscreen">[a-z]*' "$XML" 2>/dev/null | grep -o '[a-z]*$')
    MAXF=${MAXF:-0}
    case ",$LIST," in
        *",$MAXF,"*) ;;
        *) [ "$MAXF" -gt 0 ] && LIST="$MAXF${LIST:+,$LIST}" ;;
    esac
    printf 'max_fps=%s\n' "$MAXF" >> "$OUT"
    printf 'fps_list=%s\n' "$LIST" >> "$OUT"
    printf 'aod_fs=%s\n' "${AOD:-false}" >> "$OUT"
fi

DB=/data/data/com.miui.powerkeeper/databases/user_configure.db
if [ -x "$SQL" ] && [ -f "$DB" ]; then
    cp -f "$DB" /data/local/tmp/.rrs_uc.db 2>/dev/null
    Q() { "$SQL" /data/local/tmp/.rrs_uc.db "select value from misc where name='$1';" 2>/dev/null; }
    L60=$(Q fps_group | sed -n 's/.*"list60":"\([^"]*\)".*/\1/p' | tr ',' '\n' | sort -u | tr '\n' ',' | sed 's/,$//')
    S60=$(Q fps_smart_group | sed -n 's/.*"list60":"\([^"]*\)".*/\1/p' | tr ',' '\n' | sort -u | tr '\n' ',' | sed 's/,$//')
    D60=$(Q display_fps_group | tr -d ' ')
    PRIV=$(Q key_priv_names)
    TOP=$(Q key_top_names)
    OPT=$(Q key_optimize_game_names)
    printf 'list60=%s\n' "$L60" >> "$OUT"
    printf 'smart60=%s\n' "$S60" >> "$OUT"
    printf 'display60=%s\n' "$D60" >> "$OUT"
    printf 'priv=%s\n' "$PRIV" >> "$OUT"
    printf 'top=%s\n' "$TOP" >> "$OUT"
    printf 'optimize=%s\n' "$OPT" >> "$OUT"
    rm -f /data/local/tmp/.rrs_uc.db
fi
wc -l < "$OUT" 2>/dev/null
