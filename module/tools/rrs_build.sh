#!/system/bin/sh
# Generate module overlays from the stock partitions.
MODDIR="${MODDIR:-$(cd "$(dirname "$0")/.." && pwd)}"
MODPATH="$MODDIR"
mkdir -p /data/adb/RRS 2>/dev/null
ui_print(){ :; }
set_perm_recursive(){ :; }
ROM_XIAOMI=0

HASH_FILE=/data/adb/RRS/stock_hashes
STOCK_HASH=$(sha256sum /odm/firmware/*thp_config.* /product/etc/device_features/*.xml 2>/dev/null)
if [ -n "$STOCK_HASH" ] && [ -s "$HASH_FILE" ] && [ "$STOCK_HASH" = "$(cat "$HASH_FILE" 2>/dev/null)" ] \
        && { ls "$MODPATH/odm/firmware"/*thp_config.* >/dev/null 2>&1 \
             || ls "$MODPATH/product/etc/device_features"/*.xml >/dev/null 2>&1; }; then
    ui_print "- 原厂文件和配置未变化，跳过后台生成"
    exit 0
fi
BUILD_OK=1
# device_features

BRAND="$(getprop ro.product.brand) $(getprop ro.product.manufacturer)"
if echo "$BRAND" | grep -qiE 'xiaomi|redmi|poco'; then
    ROM_XIAOMI=1
else
    ui_print "- 非小米设备：跳过 device_features 注入"
fi

FEATURE_DIR="/product/etc/device_features"
[ -d "$FEATURE_DIR" ] || FEATURE_DIR="/system/product/etc/device_features"
SRC_DIR="$MODPATH/product/etc/device_features"

ODMFW="/odm/firmware"
ODM_RAW=1

DEVICE=$(getprop ro.product.device)
[ -z "$DEVICE" ] && DEVICE=$(getprop ro.product.name)
[ -z "$DEVICE" ] && DEVICE=$(getprop ro.build.product)

XML=""
if [ "$ROM_XIAOMI" = "1" ]; then
    if [ -n "$DEVICE" ] && [ -f "$FEATURE_DIR/$DEVICE.xml" ]; then
        XML="$FEATURE_DIR/$DEVICE.xml"
    else
        set -- "$FEATURE_DIR"/*.xml
        [ "$#" -eq 1 ] && [ -f "$1" ] && XML="$1"
    fi
fi

if [ -z "$XML" ]; then
    ui_print "- 未找到 device_features xml，跳过注入"
else
    FNAME=$(basename "$XML")
    TOP=$(sed -n 's/.*name="support_max_fps">\([0-9][0-9]*\)<.*/\1/p' "$XML" | head -n1)
    [ -z "$TOP" ] && TOP=0
    if [ "$TOP" -le 0 ]; then
        TOP=$(sed -n '/name="fpsList"/,/<\/integer-array>/p' "$XML" \
            | grep -o '<item>[0-9][0-9]*</item>' | sed 's/<[^>]*>//g' | sort -n | tail -n1)
        [ -z "$TOP" ] && TOP=0
    fi
    LIST=$(sed -n '/name="fpsList"/,/<\/integer-array>/p' "$XML" \
        | grep -o '<item>[0-9][0-9]*</item>' | sed 's/<[^>]*>//g')
    [ "$TOP" -gt 0 ] && LIST="$LIST $TOP"
    LIST=$(printf '%s\n' $LIST 1 | grep -E '^[0-9]+$' | sort -rn -u | tr '\n' ' ')
    START=$(grep -n 'name="fpsList"' "$XML" | head -n1 | cut -d: -f1)
    END=""
    [ -n "$START" ] && END=$(tail -n +"$START" "$XML" | grep -n '</integer-array>' | head -n1 | cut -d: -f1)
    [ -n "$START" ] && [ -n "$END" ] && END=$((START + END - 1))
    if [ -n "$START" ] && [ -n "$END" ]; then
        mkdir -p "$SRC_DIR"
        OUT="$SRC_DIR/$FNAME"
        {
            head -n $((START - 1)) "$XML"
            echo '    <integer-array name="fpsList">'
            for v in $LIST; do echo "        <item>$v</item>"; done
            echo '    </integer-array>'
            tail -n +$((END + 1)) "$XML"
        } > "$OUT.tmp"
        LINES_SRC=$(wc -l < "$XML")
        LINES_OUT=$(wc -l < "$OUT.tmp" 2>/dev/null)
        [ -z "$LINES_OUT" ] && LINES_OUT=0
        if grep -q '</features>' "$OUT.tmp" 2>/dev/null && [ "$LINES_OUT" -ge $((LINES_SRC / 2)) ]; then
            mv -f "$OUT.tmp" "$OUT"
            mkdir -p "$MODPATH/system/product/etc/device_features" 2>/dev/null
            cp -f "$OUT" "$MODPATH/system/product/etc/device_features/$FNAME" 2>/dev/null
            ui_print "- device_features: $FNAME"
        else
            rm -f "$OUT.tmp"
            cp -f "$XML" "$OUT"
            BUILD_OK=0
        fi
    fi
fi

CFG=/data/adb/RRS/config
mkdir -p /data/adb/RRS
if [ ! -f "$CFG" ] && [ -f "$MODPATH/config.example" ]; then
    cp -f "$MODPATH/config.example" "$CFG" 2>/dev/null
    chmod 644 "$CFG" 2>/dev/null
fi

for f in /data/adb/RRS/config /data/adb/RRS/config.bak; do
    [ -f "$f" ] || continue
    if grep -q -E '^sampling(_game)?=' "$f" 2>/dev/null; then
        sed -i -E 's/^sampling_game=(.*)/thp_report_rate_game=\1/; s/^sampling=(.*)/thp_report_rate_normal=\1/' "$f"
    fi
    if grep -q -E '^thp_report_rate_(normal|game)=[0-9]+/[0-9]+' "$f" 2>/dev/null; then
        awk -F= '/^thp_report_rate_(normal|game)=[0-9]+\/[0-9]+/ {split($2,a,"/"); print $1"="a[2]; next} {print}' "$f" > "$f.tmp" && mv -f "$f.tmp" "$f"
    fi
done

if [ -f "$CFG" ] && ! grep -q '^sampling_node=' "$CFG" 2>/dev/null; then
    for n in /sys/class/touch/touch_dev/touch_thp_ic_cmd /sys/class/touch/touch_dev/touch_ic_cmd /sys/class/touch/touch_dev/touch_cmd; do
        if [ -w "$n" ] && printf '8 3007 240 1' > "$n" 2>/dev/null; then
            printf 'sampling_node=%s\n' "$n" >> "$CFG"
            grep -q '^sampling_cmd=' "$CFG" 2>/dev/null || printf 'sampling_cmd=8 3007 %%d 1\n' >> "$CFG"
            ui_print "- 触控写节点：$n"
            break
        fi
    done
fi

SAMP_CFG="$(sed -n 's/^thp_report_rate_normal=\([^#]*\).*/\1/p' /data/adb/RRS/config 2>/dev/null | head -n1 | tr -d ' ')"
case "$SAMP_CFG" in
    ''|off|on|*/*) FW_RATE="" ;;
    *) FW_RATE="$SAMP_CFG" ;;
esac
[ -z "$FW_RATE" ] && FW_RATE=240

GAME_CFG="$(sed -n 's/^thp_report_rate_game=\([^#]*\).*/\1/p' /data/adb/RRS/config 2>/dev/null | head -n1 | tr -d ' ')"
case "$GAME_CFG" in
    ''|off|on|*/*) GAME_RATE="" ;;
    *) GAME_RATE="$GAME_CFG" ;;
esac

MOD_ID="$(sed -n 's/^id=//p' "$MODPATH/module.prop" 2>/dev/null | head -n1)"
[ -z "$MOD_ID" ] && MOD_ID=RRS
MOD_FINAL="/data/adb/modules/$MOD_ID"
MOD_OUT="$MODPATH"
mkdir -p "$MOD_OUT/odm/firmware" /data/adb/RRS
REPACK="$MODPATH/tools/thp_repack"
DUMP="$MODPATH/tools/thp_dump"
[ -f "$REPACK" ] || REPACK="$MOD_OUT/tools/thp_repack"
[ -f "$DUMP" ] || DUMP="$MOD_OUT/tools/thp_dump"
chmod 0755 "$REPACK" "$DUMP" 2>/dev/null

# Stock touch parameters
STOCK=/data/adb/RRS/thp_stock
CAPTURE_STOCK=0
[ "$ODM_RAW" = "1" ] && CAPTURE_STOCK=1
if [ "$CAPTURE_STOCK" != "1" ] && [ ! -s "$STOCK" ] && [ ! -e "$MOD_FINAL/odm/firmware" ]; then
    CAPTURE_STOCK=1
fi
if [ "$CAPTURE_STOCK" = "1" ]; then
    {
        echo "# model=$(getprop ro.product.device)"
        for f in "$ODMFW"/*thp_config.ini; do
            [ -f "$f" ] || continue
            echo "file=$(basename "$f")"
            grep -E '^[[:space:]]*(ic_rate_|rate_)' "$f"
        done
        for f in "$ODMFW"/*thp_config.cfg; do
            [ -f "$f" ] || continue
            echo "file=$(basename "$f")"
            "$DUMP" "$f" 2>/dev/null | sed -n '/^report_rate/,/^[a-z_]/p' | grep -E '^[[:space:]]*(ic_rate_|rate_)'
        done
    } > "$STOCK" 2>/dev/null
    chmod 644 "$STOCK" 2>/dev/null
    ui_print "- 已更新 thp_stock"
else
    ui_print "- 保留现有 thp_stock（原厂 /odm 不可读）"
fi

PATCHED=0
for f in "$ODMFW"/*thp_config.ini; do
    [ -f "$f" ] || continue
    n="$(basename "$f")"
    # IC rate = report rate / timer coefficient
    COEF=1
    C=$(sed -n 's/^[[:space:]]*ic_report_timer_coef[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$f" | head -n1)
    [ -n "$C" ] && [ "$C" -gt 0 ] && COEF=$C
    IC_RATE=$((FW_RATE / COEF))
    [ "$IC_RATE" -lt 1 ] && IC_RATE=1
    awk -v r="$FW_RATE" -v i="$IC_RATE" -v g="$GAME_RATE" '
        /^[[:space:]]*ic_rate_normal=/ { sub(/=.*/, "=" i) }
        /^[[:space:]]*rate_normal=/    { sub(/=.*/, "=" r) }
        /^[[:space:]]*ic_rate_game=/   { if (g != "") sub(/=.*/, "=" g) }
        /^[[:space:]]*rate_game=/      { if (g != "") sub(/=.*/, "=" g) }
        { print }' "$f" > "$MOD_OUT/odm/firmware/$n" 2>/dev/null
    if grep -q "rate_normal=$FW_RATE" "$MOD_OUT/odm/firmware/$n" 2>/dev/null; then
        PATCHED=$((PATCHED + 1))
    else
        rm -f "$MOD_OUT/odm/firmware/$n"
    fi
done
for f in "$ODMFW"/*thp_config.cfg; do
    [ -f "$f" ] || continue
    n="$(basename "$f")"
    if "$REPACK" "$f" "$MOD_OUT/odm/firmware/$n" "$FW_RATE" >/dev/null 2>&1 \
            && [ -s "$MOD_OUT/odm/firmware/$n" ] \
            && "$DUMP" "$MOD_OUT/odm/firmware/$n" 2>/dev/null | grep -qE 'rate_(normal|game)='; then
        PATCHED=$((PATCHED + 1))
    else
        rm -f "$MOD_OUT/odm/firmware/$n"
    fi
done
SRC_FW=$(ls "$ODMFW"/*thp_config.ini "$ODMFW"/*thp_config.cfg 2>/dev/null | wc -l)
[ "$SRC_FW" -gt 0 ] && [ "$PATCHED" -lt "$SRC_FW" ] && BUILD_OK=0
if [ "$PATCHED" -gt 0 ] && [ "$MOD_OUT" != "$MOD_FINAL" ] && [ -d "$MOD_FINAL" ]; then
    mkdir -p "$MOD_FINAL/odm/firmware"
    cp -f "$MOD_OUT"/odm/firmware/* "$MOD_FINAL/odm/firmware/" 2>/dev/null
fi
for d in "$MOD_OUT" "$MOD_FINAL"; do
    [ -d "$d/odm/firmware" ] || continue
    chcon u:object_r:vendor_file:s0 "$d/odm/firmware/"* 2>/dev/null
    if ! ls -Z "$d"/odm/firmware/* 2>/dev/null | grep -q vendor_file; then
        rm -rf "$d/odm"
    fi
done
if [ "$PATCHED" -gt 0 ]; then
    ui_print "- 触摸固件覆盖: $FW_RATE（$PATCHED 个）"
else
    rm -rf "$MOD_OUT/odm" "$MOD_FINAL/odm"
    ui_print "- 触摸固件: 无可改写配置"
fi
rm -f /data/adb/RRS/thp_rate_max 2>/dev/null
if [ "$BUILD_OK" = "1" ] && [ -n "$STOCK_HASH" ]; then
    printf '%s\n' "$STOCK_HASH" > "$HASH_FILE" 2>/dev/null
else
    ui_print "- 生成未完整成功，保留旧 stock_hashes，下次开机重试"
fi
