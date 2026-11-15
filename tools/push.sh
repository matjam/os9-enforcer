#!/bin/sh
# Copy files from the host into the running guest.
#
# Usage: tools/push.sh [-n "Volume Name"] file...
#
# Builds an HFS volume containing the files and inserts it as the guest's CD,
# where Mac OS mounts it on the desktop. hcopy's automatic mode decides how
# each file is copied: .bin files are decoded as MacBinary (keeping resource
# forks, type and creator), .hqx as BinHex, .txt as text with Mac line
# endings, and anything else as raw data.

set -eu
here=$(cd "$(dirname "$0")/.." && pwd)

name=Transfer
if [ "${1:-}" = -n ]; then
  name=$2
  shift 2
fi
if [ $# -eq 0 ]; then
  echo "usage: $0 [-n \"Volume Name\"] file..." >&2
  exit 2
fi

mkdir -p "$here/run"
# The machine's QMP socket (QMP_SOCK, as for tools/qmp.py) names this
# script's files in run/, so two machines' discs do not mix.
tag=push
if [ -n "${QMP_SOCK:-}" ]; then
  tag=push-$(basename "$QMP_SOCK" .sock)
fi
# A new file name each time, so the guest sees a different disc.
image="$here/run/$tag-$(date +%Y%m%d-%H%M%S).img"

# Size: the files plus a quarter for HFS overhead, at least 2 MB, rounded up
# to whole megabytes (a whole number of 2048-byte CD sectors).
bytes=$(cat "$@" | wc -c)
mb=$(( (bytes + bytes / 4) / 1048576 + 2 ))
dd if=/dev/zero of="$image" bs=1048576 count="$mb" 2>/dev/null

hformat -l "$name" "$image" >/dev/null
for f in "$@"; do
  hcopy -a "$f" :
done
hls -l
humount

# Mac OS locks the tray while a disc is mounted and does not notice a disc
# swapped underneath it, so a mounted disc is ejected from the guest first.
qmp="$here/tools/qmp.py"
last="$here/run/$tag-last-name"
case "$("$qmp" cd-state)" in
  locked*)
    "$qmp" guest-eject "$(cat "$last" 2>/dev/null || echo Transfer)"
    ;;
esac

"$qmp" change-cd "$image"
printf '%s\n' "$name" > "$last"
for old in "$here"/run/"$tag"-[0-9]*.img; do
  [ "$old" = "$image" ] || rm -f "$old"
done
echo "pushed $# file(s) as \"$name\" ($image)"
