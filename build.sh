#!/bin/sh
# Build the programs with the Retro68 PowerPC toolchain (see
# toolchain/find.sh for where it looks; toolchain/build.sh builds one).
#
# Usage: ./build.sh [PROGRAM...]
# PROGRAM is a directory: enforcer, installer, xlog, scribble, tmem, h68k,
# wlog or probe. Without one, it builds all of them. Each builds in
# PROGRAM/build.
set -eu
cd "$(dirname "$0")"
ROOT=$(pwd)
. "$ROOT/toolchain/find.sh"
toolchain=$RETRO68/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
if [ $# -eq 0 ]; then
    set -- enforcer installer xlog scribble tmem h68k wlog probe
fi
for p in "$@"; do
    # A build folder configured for another toolchain keeps that
    # toolchain's tools in its cache, so start it again.
    cache=$p/build/CMakeCache.txt
    if [ -f "$cache" ] && ! grep -qxF "RETRO68_ROOT:PATH=$RETRO68" "$cache"; then
        echo "$p: configured for another toolchain; starting $p/build again"
        rm -rf "$p/build"
    fi
    cmake -S "$p" -B "$p/build" -DCMAKE_TOOLCHAIN_FILE="$toolchain" >/dev/null
    cmake --build "$p/build"
done
