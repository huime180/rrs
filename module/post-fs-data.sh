#!/system/bin/sh
MODDIR=$(cd "$(dirname "$0")" && pwd)

[ -d "$MODDIR/odm/firmware" ] || exit 0
chcon u:object_r:vendor_file:s0 "$MODDIR"/odm/firmware/* 2>/dev/null
if ! ls -Z "$MODDIR"/odm/firmware/* 2>/dev/null | grep -q vendor_file; then
    rm -rf "$MODDIR/odm"
fi
