#!/bin/sh
# Build the programs with the Retro68 PowerPC toolchain built in the test
# machine's folder (OS9_PROTECT, default ../os9-protect next to this one).
#
# Usage: ./build.sh [PROGRAM...]
# PROGRAM is a directory: enforcer, xlog, scribble, h68k, wlog or probe.
# Without one, it builds all of them. Each builds in PROGRAM/build.
set -eu
cd "$(dirname "$0")"
os9=${OS9_PROTECT:-../os9-protect}
toolchain=$(cd "$os9" && pwd)/toolchain/build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
if [ ! -f "$toolchain" ]; then
    echo "build.sh: no toolchain at $toolchain" >&2
    echo "build.sh: build it in os9-protect (see its README), or set OS9_PROTECT" >&2
    exit 1
fi
if [ $# -eq 0 ]; then
    set -- enforcer xlog scribble h68k wlog probe
fi
for p in "$@"; do
    cmake -S "$p" -B "$p/build" -DCMAKE_TOOLCHAIN_FILE="$toolchain" >/dev/null
    cmake --build "$p/build"
done
