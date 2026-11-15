#!/bin/sh
# Pack a tbxi dump into a Mac OS ROM file for kernel/install.sh.
#
# Usage: kernel/pack.sh [DUMP [OUT]]
#   DUMP  a tbxi dump folder (default kernel/stock/dump, unmodified);
#   OUT   the file to write (default "kernel/build/Mac OS ROM").
#
# tbxi builds both forks. The resource fork must come from tbxi, not from
# the stock file: its 'cfrg' resource gives the data fork offsets of the
# System Enabler's code fragments, which follow the compressed ROM image,
# so they move whenever the image's compressed size changes. Mac OS then
# loads garbage as code ("unimplemented trap" during startup). The type,
# creator and Finder flags come from the stock file, kernel/stock/Mac OS
# ROM (run kernel/extract.sh first). tbxi is refs/tbxi-venv (see "Reference
# sources" in CLAUDE.md).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
tbxi=$root/refs/tbxi-venv/bin/tbxi
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

# tbxi writes the resource fork as Rez text (rom.rdump); macresources, which
# tbxi installs, turns it into a resource fork.
"$root/refs/tbxi-venv/bin/python" -I -c '
import sys, macresources
res = macresources.parse_rez_code(open(sys.argv[1], "rb").read())
open(sys.argv[2], "wb").write(macresources.make_file(res))
' "$tmp/rom.rdump" "$tmp/rsrc"

# cp brings the Finder info; writing through the shell then replaces both
# forks and leaves it in place.
mkdir -p "$(dirname "$out")"
rm -f "$out"
cp "$stock" "$out"
cat "$tmp/rom" >"$out"
cat "$tmp/rsrc" >"$out/..namedfork/rsrc"

echo "Mac OS ROM: data fork SHA-256 $(shasum -a 256 "$out" | cut -d' ' -f1)"
echo "            resource fork SHA-256 $(shasum -a 256 "$out/..namedfork/rsrc" | cut -d' ' -f1)"
echo "            $out"
