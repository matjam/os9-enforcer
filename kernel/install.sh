#!/bin/sh
# Put a Mac OS ROM file in the System Folder of a test machine disk, so the
# machine boots its NanoKernel. Shut the machine down first.
#
# Usage: kernel/install.sh [ROM [DISK]]
#   ROM   the Mac OS ROM file (default "kernel/build/Mac OS ROM", from
#         kernel/pack.sh), with its resource fork and Finder info;
#   DISK  a raw disk image (default disks/os9-kernel.img).
#
# A running Mac OS 9 keeps its Mac OS ROM file open, and the file is name
# locked, so the Finder cannot replace it. This script attaches the image
# with hdiutil, mounts its HFS+ volume, replaces the file, removes the
# folders macOS adds to a volume it mounts, and detaches the image.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
rom=${1:-$here/build/Mac OS ROM}
disk=${2:-$root/disks/os9-kernel.img}

for f in "$rom" "$disk"; do
    if [ ! -e "$f" ]; then
        echo "install.sh: missing $f" >&2
        exit 1
    fi
done
case "$disk" in
    *.qcow2)
        echo "install.sh: $disk is qcow2; this needs a raw image" >&2
        exit 1
        ;;
esac
if lsof "$disk" >/dev/null 2>&1; then
    echo "install.sh: $disk is open; shut the machine down first" >&2
    exit 1
fi

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

hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage \
    "$disk" >"$tmp/attach"
dev=$(awk '$2 == "Apple_partition_scheme" {print $1}' "$tmp/attach")
part=$(awk '$2 == "Apple_HFS" {print $1}' "$tmp/attach")
mkdir "$tmp/mnt"
mount -t hfs -o nobrowse "$part" "$tmp/mnt"

dest="$tmp/mnt/System Folder/Mac OS ROM"
rm -f "$dest"
cp "$rom" "$dest"
# macOS also stores a com.apple.provenance attribute on the copy and does
# not let xattr remove it. Mac OS 9 does not read HFS+ extended
# attributes, so it does no harm.
want=$(shasum -a 256 "$rom" | cut -d' ' -f1)
got=$(shasum -a 256 "$dest" | cut -d' ' -f1)
want_rsrc=$(shasum -a 256 "$rom/..namedfork/rsrc" | cut -d' ' -f1)
got_rsrc=$(shasum -a 256 "$dest/..namedfork/rsrc" | cut -d' ' -f1)
if [ "$want" != "$got" ] || [ "$want_rsrc" != "$got_rsrc" ]; then
    echo "install.sh: the installed file does not match $rom" >&2
    exit 1
fi

# Mac OS 9 shows these as ordinary files and folders.
rm -rf "$tmp/mnt/.fseventsd" "$tmp/mnt/.Spotlight-V100" \
    "$tmp/mnt/.Trashes" "$tmp/mnt/.DS_Store" \
    "$tmp/mnt/System Folder/.DS_Store"

umount "$tmp/mnt"
hdiutil detach -quiet "$dev"
dev=

echo "installed:  $rom"
echo "            data fork SHA-256 $got"
echo "            resource fork SHA-256 $got_rsrc"
echo "disk:       $disk"
