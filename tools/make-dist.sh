#!/bin/sh
# Collect Enforcer's three programs in dist/: Enforcer Installer, the
# Enforcer Extension and the Enforcer control panel, as MacBinary .bin
# files and on an HFS disk image, Enforcer.dsk, for a Mac or an emulator.
# The installer copies the other two from its own folder, so they stay
# together.
#
# Usage: tools/make-dist.sh
#
# Run ./build.sh enforcer installer first. Uses hfsutils from the Retro68
# toolchain (see toolchain/find.sh).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/toolchain/find.sh"
dist=$ROOT/dist
image=$dist/Enforcer.dsk

set -- "$ROOT/installer/build/Enforcer Installer.bin" \
       "$ROOT/enforcer/build/Enforcer Extension.bin" \
       "$ROOT/enforcer/build/Enforcer.bin"
for f in "$@"; do
    if [ ! -f "$f" ]; then
        echo "make-dist.sh: missing $f (run ./build.sh enforcer installer)" >&2
        exit 1
    fi
done

rm -rf "$dist"
mkdir -p "$dist"
cp "$@" "$dist/"

# Size: the files plus a quarter for HFS overhead, at least 2 MB, in whole
# megabytes.
bytes=$(cat "$@" | wc -c)
mb=$(( (bytes + bytes / 4) / 1048576 + 2 ))
dd if=/dev/zero of="$image" bs=1048576 count="$mb" 2>/dev/null

# hcopy -m decodes each MacBinary file, keeping its resource fork, type,
# creator and Finder flags. hfsutils keeps its current volume in ~/.hcd.
bin=$RETRO68/bin
"$bin/hformat" -l Enforcer "$image" >/dev/null
for f in "$@"; do
    "$bin/hcopy" -m "$f" :
done
"$bin/hls" -l
"$bin/humount"
echo "dist: $dist"
