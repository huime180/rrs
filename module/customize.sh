#!/system/bin/sh

command -v abort >/dev/null 2>&1 || abort() { ui_print "! $*"; exit 1; }

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

BRAND="$(getprop ro.product.brand) $(getprop ro.product.manufacturer)"
if ! echo "$BRAND" | grep -qiE 'xiaomi|redmi|poco'; then
    ui_print "! 本模块仅支持 Xiaomi / Redmi / POCO 设备"
    abort "当前设备品牌：$BRAND"
fi

if [ "$KSU" = "true" ] || [ -d /data/adb/ksu ]; then
    META=0
    for d in /data/adb/modules/*; do
        [ -d "$d" ] || continue
        case "$(basename "$d")" in
            hybrid_mount|meta-*|meta_*) META=1 ;;
        esac
    done
    if [ "$META" = "0" ]; then
        ui_print "! 缺少元模块，无法挂载 /odm 与 /product"
        ui_print "请先安装元模块，推荐hybrid_mount VFS模式"
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
        if [ -e "$n" ] && [ -w "$n" ]; then
            printf 'sampling_node=%s\n' "$n" >> "$CFG"
            grep -q '^sampling_cmd=' "$CFG" 2>/dev/null || printf 'sampling_cmd=8 3007 %%d 1\n' >> "$CFG"
            ui_print "- 触控写节点：$n"
            break
        fi
    done
fi

rm -f /data/adb/RRS/thp_rate_max 2>/dev/null
set_perm_recursive $MODPATH 0 0 0777 0755
