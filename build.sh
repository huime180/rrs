#!/bin/sh
# Build the RRS module: compile the daemon and package dist/rrs-<version>-ksu.zip
#   ./build.sh            build daemon + package
#   ./build.sh --daemon   build the daemon only
# Env: ZIG points to zig; LEGACY=0 disables RRS_LEGACY_1035 (enabled by default)
set -eu
cd "$(dirname "$0")"

SRC=src/rrs_daemon_adaptive.c
DAEMON=module/system/bin/RRS_Daemon
V=$(sed -n 's/^version=//p' module/module.prop | head -n1)
[ -n "$V" ] || { echo "missing version in module/module.prop" >&2; exit 1; }

find_zig() {
    if [ -n "${ZIG:-}" ]; then
        printf '%s\n' "$ZIG"
        return
    fi
    command -v zig 2>/dev/null || true
}

ZIG_BIN=$(find_zig)
[ -n "$ZIG_BIN" ] || { echo "zig not found: set ZIG or install zig" >&2; exit 1; }

build_daemon() {
    DEF=""
    [ "${LEGACY:-1}" = "1" ] && DEF="-DRRS_LEGACY_1035"
    mkdir -p "$(dirname "$DAEMON")"
    "$ZIG_BIN" cc -target aarch64-linux-musl -static -O2 -pthread -s \
        $DEF -o "$DAEMON" "$SRC"
    echo "daemon: $DAEMON ($(wc -c < "$DAEMON") bytes)"
}

pack() {
    python3 - "$1" "$2" <<'PY'
import os, sys, zipfile

src, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(src):
        for f in sorted(files):
            if f == ".DS_Store":
                continue
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, src))
print(out, os.path.getsize(out), "bytes")
PY
}

build_daemon
[ "${1:-}" = "--daemon" ] && exit 0

chmod 755 module/*.sh "$DAEMON" module/tools/*.sh \
    module/tools/thp_repack module/tools/thp_dump module/tools/sqlite3 2>/dev/null || true
chmod 644 module/module.prop module/config.example module/system.prop \
    module/webroot/config.json module/webroot/index.html \
    module/tools/libthp_dump.so 2>/dev/null || true
mkdir -p dist
OUT="dist/rrs-${V}-ksu.zip"
rm -f "$OUT"
pack module "$OUT"
echo "built $OUT"
