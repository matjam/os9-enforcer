#!/bin/sh
# Copy the Mac OS ROM file out of the test machine's installed system,
# check that it is the known file, unpack it with tbxi and disassemble its
# NanoKernel. Everything goes into kernel/stock/, which git ignores (the
# files are Apple's).
#
# Usage: kernel/extract.sh
#
# Needs, in this repository:
#   disks/os9.qcow2            the installed system (read only);
#   refs/tbxi-venv/bin/tbxi    tbxi (see "Reference sources" in CLAUDE.md);
# and the Retro68 toolchain (toolchain/find.sh) for powerpc-apple-macos-objdump.
# The disk is converted to a raw image in a temporary folder (about 330 MB
# on disk, 8 GB sparse), attached read only with hdiutil, and detached
# again.
set -eu

# SHA-256 of the Mac OS ROM file's data fork in the 9.2.2 install
# (dated 2002-04-17), as extracted on 2026-11-12.
known=1cddc81677f7cfff75cc6a341452e62e83cc26bdadb3b9a81118689a5499e085
kernel=NanoKernel-v02.28

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
ROOT=$root
. "$root/toolchain/find.sh"
tbxi=$root/refs/tbxi-venv/bin/tbxi
objdump=$RETRO68/bin/powerpc-apple-macos-objdump
out=$here/stock

for f in "$root/disks/os9.qcow2" "$tbxi" "$objdump"; do
    if [ ! -e "$f" ]; then
        echo "extract.sh: missing $f" >&2
        exit 1
    fi
done

tmp=$(mktemp -d)
dev=
cleanup() {
    if [ -n "$dev" ]; then
        umount "$tmp/mnt" 2>/dev/null || true
        hdiutil detach -quiet "$dev" || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT

qemu-img convert -O raw "$root/disks/os9.qcow2" "$tmp/os9.raw"
hdiutil attach -readonly -nomount -imagekey diskimage-class=CRawDiskImage \
    "$tmp/os9.raw" >"$tmp/attach"
dev=$(awk '$2 == "Apple_partition_scheme" {print $1}' "$tmp/attach")
part=$(awk '$2 == "Apple_HFS" {print $1}' "$tmp/attach")
mkdir "$tmp/mnt"
mount -t hfs -o rdonly "$part" "$tmp/mnt"

mkdir -p "$out"
cp "$tmp/mnt/System Folder/Mac OS ROM" "$out/Mac OS ROM"
sum=$(shasum -a 256 "$out/Mac OS ROM" | cut -d' ' -f1)
if [ "$sum" != "$known" ]; then
    echo "extract.sh: Mac OS ROM is not the known file (SHA-256 $sum)" >&2
    exit 1
fi

rm -rf "$out/dump"
"$tbxi" dump "$out/Mac OS ROM" -o "$out/dump" >/dev/null
cp "$out/dump/Parcels.src/MacROM.src/$kernel" "$out/$kernel.bin"
"$objdump" -D -b binary -m powerpc:common -EB "$out/$kernel.bin" \
    >"$out/$kernel.dis"

echo "Mac OS ROM: SHA-256 $sum (known file)"
echo "kernel:     $out/$kernel.bin ($(wc -c <"$out/$kernel.bin" | tr -d ' ') bytes)"
echo "listing:    $out/$kernel.dis (addresses are offsets in the kernel)"
