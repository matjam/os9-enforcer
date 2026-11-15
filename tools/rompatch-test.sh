#!/bin/sh
# Test the installer's ROM patching on the host: patch the stock Mac OS ROM
# file (kernel/stock, from kernel/extract.sh) and compare the result with
# the one kernel/build.sh made with tbxi, data fork and 'cfrg' resource.
#
# Usage: tools/rompatch-test.sh
#
# Needs a C compiler, and tbxi's Python environment (refs/tbxi-venv; see
# "Reference sources" in CLAUDE.md) for reading resource forks.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
python=$here/refs/tbxi-venv/bin/python
stock="$here/kernel/stock/Mac OS ROM"
built="$here/kernel/build/Mac OS ROM"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cc -O2 -Wall -I "$here/installer" "$here/tools/rompatch-test.c" \
    "$here/installer/rompatch.c" "$here/installer/lzss.c" -o "$tmp/test"

# The 'cfrg' resources of the stock file and of the tbxi-built one.
cfrg() {
    "$python" -I -c '
import sys, macresources
for r in macresources.parse_file(open(sys.argv[1], "rb").read()):
    if r.type == b"cfrg":
        open(sys.argv[2], "wb").write(r.data)
' "$1/..namedfork/rsrc" "$2"
}
cfrg "$stock" "$tmp/cfrg"
cfrg "$built" "$tmp/cfrg-built"

"$tmp/test" "$stock" "$tmp/out" "$built" "$tmp/cfrg" "$tmp/cfrg-out"
if cmp -s "$tmp/cfrg-out" "$tmp/cfrg-built"; then
    echo "cfrg: the same as the tbxi-built file's"
else
    echo "cfrg: DIFFERENT from the tbxi-built file's"
    exit 1
fi

# The two data forks, unpacked by tbxi, must match. (cat copies only the
# data fork. The raw Parcels file holds the padding, so it is left out.)
cat "$built" >"$tmp/built"
"$here/refs/tbxi-venv/bin/tbxi" dump "$tmp/out" -o "$tmp/dump-out" >/dev/null 2>&1
"$here/refs/tbxi-venv/bin/tbxi" dump "$tmp/built" -o "$tmp/dump-built" >/dev/null 2>&1
if diff -r -x Parcels "$tmp/dump-out" "$tmp/dump-built" >/dev/null; then
    echo "tbxi dump: the same as the tbxi-built file's"
else
    echo "tbxi dump: DIFFERENT from the tbxi-built file's"
    diff -r -x Parcels "$tmp/dump-out" "$tmp/dump-built" | head -20
    exit 1
fi
