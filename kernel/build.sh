#!/bin/sh
# Assemble kernel/nanokernel.s (the stock NanoKernel with our patches),
# show what changed against the stock kernel, and pack it into a Mac OS ROM
# file for kernel/install.sh.
#
# Usage: kernel/build.sh
#
# Needs kernel/stock/ (run kernel/extract.sh first), the Retro68 toolchain
# (see toolchain/find.sh) and, for kernel/pack.sh, tbxi (refs/tbxi-venv; see
# "Reference sources" in CLAUDE.md). Writes into kernel/build/:
# NanoKernel-v02.28.bin and .dis (the patched kernel and its disassembly),
# dump/ (the stock dump with that kernel) and "Mac OS ROM".
set -eu
here=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$here/.." && pwd)
. "$ROOT/toolchain/find.sh"
bin=$RETRO68/bin
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
# The 68k emulator starts at offset 0x50000 from the kernel in the ROM image.
size=$(wc -c <"$out/$kernel.bin" | tr -d ' ')
if [ "$size" -gt $((0x50000)) ]; then
    echo "build.sh: the kernel ($size bytes) runs into the 68k emulator" >&2
    exit 1
fi
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
echo "kernel:     $out/$kernel.bin ($size bytes)"

# Where the added code keeps its data, as a kernel offset and a physical
# address (the test machine's kernel is at physical 0x00F10000).
"$bin/powerpc-apple-macos-nm" "$out/nanokernel.o" | while read -r addr type name; do
    case $name in
    task_log|enf_cb)
        printf '%-11s offset 0x%X, physical 0x%08X\n' "$name:" \
            "0x$addr" $((0x$addr + 0xF10000)) ;;
    esac
done
