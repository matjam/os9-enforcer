#!/bin/sh
# Pack a tbxi dump into a Mac OS ROM file for kernel/install.sh.
#
# Usage: kernel/pack.sh [DUMP [OUT]]
#   DUMP  a tbxi dump folder (default kernel/stock/dump, unmodified);
#   OUT   the file to write (default "kernel/build/Mac OS ROM").
#
# tbxi builds the data fork. The resource fork, type, creator and Finder
# flags come from the stock file, kernel/stock/Mac OS ROM (run
# kernel/extract.sh first). tbxi comes from os9-protect (OS9_PROTECT,
# default ../os9-protect).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
os9=$(cd "${OS9_PROTECT:-$here/../../os9-protect}" && pwd)
tbxi=$os9/refs/tbxi-venv/bin/tbxi
stock="$here/stock/Mac OS ROM"
dump=${1:-$here/stock/dump}
out=${2:-$here/build/Mac OS ROM}

for f in "$tbxi" "$stock" "$dump"; do
    if [ ! -e "$f" ]; then
        echo "pack.sh: missing $f" >&2
        exit 1
    fi
done

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"$tbxi" build "$dump" -o "$tmp/rom" >/dev/null

# cp copies the resource fork and Finder info; writing through the shell
# then replaces the data fork and leaves both in place.
mkdir -p "$(dirname "$out")"
rm -f "$out"
cp "$stock" "$out"
cat "$tmp/rom" >"$out"

echo "Mac OS ROM: data fork SHA-256 $(shasum -a 256 "$out" | cut -d' ' -f1)"
echo "            $out"
