#!/bin/sh
# Assemble kernel/nanokernel.s (the stock NanoKernel with our patches),
# show what changed against the stock kernel, and pack it into a Mac OS ROM
# file for kernel/install.sh.
#
# Usage: kernel/build.sh
#
# Needs kernel/stock/ (run kernel/extract.sh first) and, in os9-protect
# (OS9_PROTECT, default ../os9-protect), the Retro68 toolchain and tbxi.
# Writes into kernel/build/: NanoKernel-v02.28.bin and .dis (the patched
# kernel and its disassembly), dump/ (the stock dump with that kernel) and
# "Mac OS ROM".
set -eu
here=$(cd "$(dirname "$0")" && pwd)
os9=$(cd "${OS9_PROTECT:-$here/../../os9-protect}" && pwd)
bin=$os9/toolchain/build/toolchain/bin
kernel=NanoKernel-v02.28
stock=$here/stock
out=$here/build

for f in "$bin/powerpc-apple-macos-as" "$stock/$kernel.bin" "$stock/dump"; do
    if [ ! -e "$f" ]; then
        echo "build.sh: missing $f" >&2
        exit 1
    fi
done

mkdir -p "$out"
"$bin/powerpc-apple-macos-as" -mppc -I "$stock" "$here/nanokernel.s" \
    -o "$out/nanokernel.o"
"$bin/powerpc-apple-macos-objcopy" -O binary -j .text "$out/nanokernel.o" \
    "$out/$kernel.bin"
"$bin/powerpc-apple-macos-objdump" -D -b binary -m powerpc:common -EB \
    "$out/$kernel.bin" >"$out/$kernel.dis"

# Every changed or added instruction, as a diff of the two disassemblies
# (their first 7 lines name the files).
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
tail -n +8 "$stock/$kernel.dis" >"$tmp/stock"
tail -n +8 "$out/$kernel.dis" >"$tmp/patched"
echo "Changes against the stock kernel:"
diff "$tmp/stock" "$tmp/patched" || true

rm -rf "$out/dump"
cp -R "$stock/dump" "$out/dump"
cp "$out/$kernel.bin" "$out/dump/Parcels.src/MacROM.src/$kernel"
"$here/pack.sh" "$out/dump" "$out/Mac OS ROM"
echo "kernel:     $out/$kernel.bin ($(wc -c <"$out/$kernel.bin" | tr -d ' ') bytes)"
