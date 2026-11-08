#!/system/bin/sh
MODDIR=${0%/*}
mkdir -p /data/adb/RRS
[ -f /data/adb/RRS/config ] && ! grep -q '^log=1' /data/adb/RRS/config 2>/dev/null \
    && rm -f /data/adb/RRS/rrs.log /data/adb/RRS/rrs.log.1
rm -f /data/adb/RRS/sys_defaults /data/adb/RRS/display_modes
rm -rf /data/adb/RRS/tmp

# Generate the overlay before modules are mounted.
MODDIR="$MODDIR" sh "$MODDIR/tools/rrs_build.sh" >/dev/null 2>&1

[ -d "$MODDIR/odm/firmware" ] || exit 0
chcon u:object_r:vendor_file:s0 "$MODDIR"/odm/firmware/* 2>/dev/null
if ! ls -Z "$MODDIR"/odm/firmware/* 2>/dev/null | grep -q vendor_file; then
    rm -rf "$MODDIR/odm"
fi
