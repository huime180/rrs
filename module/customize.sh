#!/system/bin/sh

AUTOMOUNT=true
SKIPMOUNT=true
PROPFILE=true
POSTFSDATA=true
LATESTARTSERVICE=true

device="`getprop ro.product.odm.marketname`"
version="`getprop ro.build.version.release`"
HyperOS_Version="`getprop ro.mi.os.version.incremental`"
B="`grep_prop author $TMPDIR/module.prop`"
C="`grep_prop name $TMPDIR/module.prop`"
D="`grep_prop description $TMPDIR/module.prop`"
ui_print "- *******************************"
ui_print "- 您的设备: $device"
ui_print "- 系统版本: $version"
ui_print "- HyperOS版本: $HyperOS_Version"
ui_print "- $C    "
ui_print "- 作者：$B"
ui_print "- $D    "
ui_print "- *******************************"

# 自动适配：读取本机原厂 device_features xml，只注入刷新率上限、fpsList 档位
# 与 support_aod_fullscreen；原厂已有内容与档位全部保留。

BRAND="$(getprop ro.product.brand) $(getprop ro.product.manufacturer)"
ROM_XIAOMI=1
if echo "$BRAND" | grep -qiE 'oppo|oneplus|realme|oplus'; then
    ROM_XIAOMI=0
    ui_print "- 检测到 ColorOS/Realme 设备：跳过 device_features 与小米属性注入"
fi

STOCK_TMP="/data/local/tmp/.rrs_stock"
FEATURE_DIR=""
PRODDEV=""
[ "$ROM_XIAOMI" = "1" ] && PRODDEV="$(mount 2>/dev/null | grep ' /product ' | head -n1 | awk '{print $1}')"
if [ -n "$PRODDEV" ]; then
    rm -rf "$STOCK_TMP" 2>/dev/null
    mkdir -p "$STOCK_TMP/product" 2>/dev/null
    if mount -t erofs -o ro "$PRODDEV" "$STOCK_TMP/product" 2>/dev/null \
            || mount -o ro "$PRODDEV" "$STOCK_TMP/product" 2>/dev/null; then
        FEATURE_DIR="$STOCK_TMP/product/etc/device_features"
        ui_print "- 原厂 /product 只读挂载成功"
    fi
fi
[ -z "$FEATURE_DIR" ] && [ "$ROM_XIAOMI" = "1" ] && FEATURE_DIR="/system/product/etc/device_features"
SRC_DIR="$MODPATH/product/etc/device_features"

DEVICE=$(getprop ro.product.device)
[ -z "$DEVICE" ] && DEVICE=$(getprop ro.product.name)
[ -z "$DEVICE" ] && DEVICE=$(getprop ro.build.product)

XML=""
if [ -n "$DEVICE" ] && [ -f "$FEATURE_DIR/$DEVICE.xml" ]; then
    XML="$FEATURE_DIR/$DEVICE.xml"
else
    set -- "$FEATURE_DIR"/*.xml
    [ "$#" -eq 1 ] && [ -f "$1" ] && XML="$1"
fi

if [ -z "$XML" ]; then
    ui_print "- 未找到 device_features xml，跳过注入"
else
    FNAME=$(basename "$XML")
    ui_print "- 机型标识: $DEVICE"
    ui_print "- 特征文件: $FNAME"

    # 更新安装时，旧版本模块可能已经 bind 了这个文件；先卸载才能读到原厂内容
    if mount 2>/dev/null | grep -q " on $XML "; then
        umount "$XML" 2>/dev/null
        if mount 2>/dev/null | grep -q " on $XML "; then
            ui_print "- 警告: 该文件仍被旧模块覆盖，读到的可能不是原厂值（建议先卸载旧模块再刷）"
        fi
    fi

    # 刷新率上限：优先原厂 support_max_fps，缺失才用原厂 fpsList 最高档
    TOP=$(awk '
        /name="support_max_fps"/ {
            line = $0
            gsub(/[^0-9]+/, " ", line)
            n = split(line, a, " ")
            for (i = 1; i <= n; i++)
                if (a[i] + 0 > m) m = a[i] + 0
        }
        END { printf "%d", m + 0 }
    ' "$XML")
    [ -z "$TOP" ] && TOP=0
    TOP_SRC="support_max_fps"

    if [ "$TOP" -le 0 ]; then
        TOP=$(awk '
            /name="fpsList"/ { inb = 1 }
            inb {
                line = $0
                gsub(/[^0-9]+/, " ", line)
                n = split(line, a, " ")
                for (i = 1; i <= n; i++)
                    if (a[i] + 0 > m) m = a[i] + 0
                if ($0 ~ /<\/integer-array>/) inb = 0
            }
            END { printf "%d", m + 0 }
        ' "$XML")
        [ -z "$TOP" ] && TOP=0
        TOP_SRC="fpsList 最高档"
    fi

    if [ "$TOP" -gt 0 ]; then
        ui_print "- 刷新率上限: ${TOP}Hz（取自原厂 $TOP_SRC）"
    else
        ui_print "- 原厂 xml 无 support_max_fps / fpsList，跳过刷新率注入，仅处理全屏AOD"
    fi

    # ---- 面板实际支持的模式（用于补档） ----
    MODES=$(dumpsys display 2>/dev/null | grep -oE 'fps=[0-9.]+' | cut -d= -f2 \
        | awk '{printf "%d\n", $1}' | sort -rn -u | tr '\n' ' ')
    [ -z "$MODES" ] && MODES="120 90 60"
    ALLLIST=$(echo "$MODES" | tr ' ' '\n' | grep -E '^[0-9]+$' | sort -rn -u | tr '\n' ' ')
    ALLLIST="$ALLLIST 1"   # 息屏 AOD 1Hz：作为档位一起生成（去重由 awk 处理）
    mkdir -p "$SRC_DIR"
    OUT="$SRC_DIR/$FNAME"
    AWK="$TMPDIR/rrs_inject.awk"

    cat > "$AWK" <<'AWKEOF'
BEGIN { nw = split(list, WANT, " ") }
{ L[NR] = $0 }
END {
    # ---- 定位原厂 fpsList 块 ----
    fs = 0; fe = 0
    for (i = 1; i <= NR; i++) {
        if (fs == 0 && L[i] ~ /name="fpsList"/) fs = i
        if (fs > 0 && L[i] ~ /<\/integer-array>/) { fe = i; break }
    }
    if (fs > 0 && fe == 0) fs = 0

    # ---- 档位集合：原厂全部保留 + 上限内补齐标准档 ----
    cnt = 0
    if (fs > 0) {
        for (i = fs; i <= fe; i++) {
            s = L[i]
            gsub(/[^0-9]+/, " ", s)
            n = split(s, a, " ")
            for (j = 1; j <= n; j++) {
                v = a[j] + 0
                if (v <= 0) continue
                dup = 0
                for (k = 1; k <= cnt; k++) if (A[k] == v) dup = 1
                if (!dup) { cnt++; A[cnt] = v }
            }
        }
        for (k = 1; k <= nw; k++) {
            v = WANT[k] + 0
            dup = 0
            for (j = 1; j <= cnt; j++) if (A[j] == v) dup = 1
            if (!dup) { cnt++; A[cnt] = v }
        }
        # 上限本身不是标准档（如 240）时也补进列表
        if (cap > 0) {
            dup = 0
            for (j = 1; j <= cnt; j++) if (A[j] == cap) dup = 1
            if (!dup) { cnt++; A[cnt] = cap }
        }
        # 降序输出
        for (i = 1; i <= cnt; i++) {
            mi = i
            for (j = i + 1; j <= cnt; j++) if (A[j] > A[mi]) mi = j
            t = A[i]; A[i] = A[mi]; A[mi] = t
        }
    }

    has_aodfs = 0; has_maxfps = 0
    for (i = 1; i <= NR; i++) {
        if (L[i] ~ /name="support_aod_fullscreen"/) has_aodfs = 1
        if (L[i] ~ /name="support_max_fps"/) has_maxfps = 1
    }

    written_max = 0; written_aod = 0
    i = 1
    while (i <= NR) {
        l = L[i]

        # fpsList 整块重写（保留原厂值 + 补齐）
        if (fs > 0 && i == fs) {
            print "    <integer-array name=\"fpsList\">"
            for (j = 1; j <= cnt; j++) printf "        <item>%d</item>\n", A[j]
            print "    </integer-array>"
            i = fe + 1
            continue
        }

        # support_max_fps：已存在时按上限比较，低了就替换（不新增重复节点）
        if (l ~ /name="support_max_fps"/ && written_max == 0) {
            v = 0
            if (match(l, />[0-9]+</)) v = substr(l, RSTART + 1, RLENGTH - 2) + 0
            if (cap > 0 && v < cap)
                printf "    <integer name=\"support_max_fps\">%d</integer>\n", cap
            else
                print l
            written_max = 1
            i++
            continue
        }

        # 收尾：缺的节点在 </features> 之前补上，保证 XML 结构合法
        if (l ~ /<\/features>/) {
            if (cap > 0 && written_max == 0) {
                printf "    <integer name=\"support_max_fps\">%d</integer>\n", cap
                written_max = 1
            }
            if (has_aodfs == 0 && written_aod == 0) {
                print "    <!--whether the device supports aod fullscreen mode-->"
                print "    <bool name=\"support_aod_fullscreen\">true</bool>"
                written_aod = 1
            }
        }

        print l
        i++

        if (l ~ /name="support_smart_fps"/ && cap > 0 && has_maxfps == 0 && written_max == 0) {
            printf "    <integer name=\"support_max_fps\">%d</integer>\n", cap
            written_max = 1
        }

        if (l ~ /name="support_aod"/ && has_aodfs == 0 && written_aod == 0) {
            print "    <!--whether the device supports aod fullscreen mode-->"
            print "    <bool name=\"support_aod_fullscreen\">true</bool>"
            written_aod = 1
        }
    }

    if (cap > 0 && written_max == 0) printf "    <integer name=\"support_max_fps\">%d</integer>\n", cap
    if (has_aodfs == 0 && written_aod == 0) {
        print "    <!--whether the device supports aod fullscreen mode-->"
        print "    <bool name=\"support_aod_fullscreen\">true</bool>"
    }
}
AWKEOF

    awk -v cap="$TOP" -v list="$ALLLIST" -f "$AWK" "$XML" > "$OUT.tmp" 2>/dev/null

    # 校验：必须含 </features>，且行数不少于原文件的一半，否则回退为原文件
    LINES_SRC=$(wc -l < "$XML")
    LINES_OUT=$(wc -l < "$OUT.tmp" 2>/dev/null)
    [ -z "$LINES_OUT" ] && LINES_OUT=0
    if grep -q '</features>' "$OUT.tmp" 2>/dev/null && [ "$LINES_OUT" -ge $((LINES_SRC / 2)) ]; then
        mv -f "$OUT.tmp" "$OUT"
        mkdir -p "$MODPATH/system/product/etc/device_features" 2>/dev/null
        cp -f "$OUT" "$MODPATH/system/product/etc/device_features/$FNAME" 2>/dev/null
        if [ "$TOP" -gt 0 ]; then
            HAS_TOP=$(grep -c "<item>$TOP</item>" "$OUT")
        else
            HAS_TOP=1
        fi
        HAS_AOD=$(grep -c 'name="support_aod_fullscreen"' "$OUT")
        HAS_MAX=$(grep -c 'name="support_max_fps"' "$OUT")
        ITEMS=$(sed -n '/name="fpsList"/,/<\/integer-array>/p' "$OUT" | grep -o '<item>[0-9][0-9]*</item>' | sed 's/<[^>]*>//g' | tr '\n' '/' | sed 's#/$##')
        if [ "$TOP" -gt 0 ]; then
            ui_print "- 注入完成: ${TOP}Hz 上限, 档位=${ITEMS}, aod_fullscreen=$HAS_AOD, max_fps=$HAS_MAX"
        else
            ui_print "- 注入完成: 未改刷新率, aod_fullscreen=$HAS_AOD"
        fi
        if { [ "$TOP" -gt 0 ] && [ "$HAS_TOP" -eq 0 ]; } || [ "$HAS_AOD" -eq 0 ]; then
            ui_print "- 警告: 部分项目未注入，系统可能不识别该文件的对应节点"
        fi
    else
        rm -f "$OUT.tmp"
        cp -f "$XML" "$OUT"
        ui_print "- 注入失败，已回退为本机原厂文件（不影响开机）"
    fi
fi

# 读取系统默认策略（原厂 device_features + powerkeeper 默认清单）
if [ -x "$MODPATH/tools/gen_sys_defaults.sh" ]; then
    [ -n "$XML" ] && sh "$MODPATH/tools/gen_sys_defaults.sh" "$XML" /data/adb/RRS/sys_defaults >/dev/null 2>&1
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
PATCHED=0
for f in /odm/firmware/*thp_config.ini; do
    [ -f "$f" ] || continue
    n="$(basename "$f")"
    awk -v v="$FW_RATE" '
        /^[[:space:]]*ic_rate_normal=/ { sub(/=.*/, "=" v) }
        /^[[:space:]]*rate_normal=/ { sub(/=.*/, "=" v) }
        { print }' "$f" > "$MOD_OUT/odm/firmware/$n" 2>/dev/null
    if grep -q "rate_normal=$FW_RATE" "$MOD_OUT/odm/firmware/$n" 2>/dev/null; then
        PATCHED=$((PATCHED + 1))
    else
        rm -f "$MOD_OUT/odm/firmware/$n"
    fi
done
for f in /odm/firmware/*thp_config.cfg; do
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
THP_MAX=0
for f in /odm/firmware/*thp_config.ini; do
    [ -f "$f" ] || continue
    v=$(grep -oE '^[[:space:]]*(ic_rate_normal|rate_normal|ic_rate_game|rate_game|rate_game_super|ic_rate_game_super)=[0-9]+' "$f" 2>/dev/null | grep -oE '[0-9]+$' | sort -n | tail -1)
    [ -n "$v" ] && [ "$v" -gt "$THP_MAX" ] && THP_MAX="$v"
done
for f in /odm/firmware/*thp_config.cfg; do
    [ -f "$f" ] || continue
    v=$("$DUMP" "$f" 2>/dev/null | grep -oE '^[[:space:]]*(ic_rate_normal|rate_normal|ic_rate_game|rate_game|rate_game_super|ic_rate_game_super)=[0-9]+' | grep -oE '[0-9]+$' | sort -n | tail -1)
    [ -n "$v" ] && [ "$v" -gt "$THP_MAX" ] && THP_MAX="$v"
done
echo "$THP_MAX" > /data/adb/RRS/thp_rate_max 2>/dev/null
chmod 644 /data/adb/RRS/thp_rate_max 2>/dev/null

# 卸载安装期间临时挂载的原厂分区
for d in product odm; do
    if [ -d "$STOCK_TMP/$d" ]; then
        umount "$STOCK_TMP/$d" 2>/dev/null || umount -l "$STOCK_TMP/$d" 2>/dev/null
    fi
done
rm -rf "$STOCK_TMP" 2>/dev/null

set_perm_recursive $MODPATH 0 0 0777 0755
