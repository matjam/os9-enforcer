#!/bin/sh
# Build the Retro68 PowerPC toolchain from the Retro68 submodule
# (toolchain/Retro68) and install it in /opt/retro68, or in $RETRO68 when
# that is set. ./build.sh, kernel/build.sh and tools/make-dist.sh find it
# there (toolchain/find.sh).
#
# Usage: toolchain/build.sh
#
# The install folder must be writable and empty. To make /opt/retro68, run
# once:
#   sudo mkdir -p /opt/retro68 && sudo chown "$USER" /opt/retro68
# The build files go into toolchain/build, which can be removed afterwards.
# The installed toolchain records the submodule commit it was built from in
# REVISION; when that is the submodule's commit, the script does nothing.
# To rebuild from another commit, empty the install folder first.
#
# Fetches the submodule if it is missing. Builds PowerPC only, with the
# open-source Multiversal Interfaces: Enforcer has no 68k or Carbon
# programs and needs none of Apple's Universal Interfaces.
#
# Needs, on macOS:
#   brew install boost cmake ninja gmp mpfr libmpc bison texinfo
# On Debian or Ubuntu:
#   sudo apt-get install cmake ninja-build g++ libgmp-dev libmpfr-dev \
#       libmpc-dev libboost-all-dev bison flex texinfo ruby
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src=$here/Retro68
out=$here/build
prefix=${RETRO68:-/opt/retro68}

if [ ! -f "$src/build-toolchain.bash" ] ||
   [ ! -f "$src/multiversal/make-multiverse.rb" ]; then
    git -C "$here/.." submodule update --init --recursive toolchain/Retro68
fi
rev=$(git -C "$src" rev-parse HEAD)

if [ -f "$prefix/REVISION" ] && [ "$(cat "$prefix/REVISION")" = "$rev" ]; then
    echo "toolchain: $prefix is already built from Retro68 $rev"
    exit 0
fi
mkdir -p "$prefix" 2>/dev/null || true
if [ ! -w "$prefix" ]; then
    echo "$prefix is not a folder you can write. To make it, run:" >&2
    echo "  sudo mkdir -p $prefix && sudo chown \"\$USER\" $prefix" >&2
    exit 1
fi
if [ -n "$(ls -A "$prefix")" ]; then
    old=$(cat "$prefix/REVISION" 2>/dev/null || echo "an unknown commit")
    echo "$prefix holds a toolchain built from Retro68 $old;" >&2
    echo "the submodule is at $rev. To replace it, empty $prefix first." >&2
    exit 1
fi

# Homebrew's bison is keg-only, and the bison that comes with macOS is too
# old for Retro68.
if command -v brew >/dev/null 2>&1; then
    bison=$(brew --prefix bison 2>/dev/null || true)
    if [ -n "$bison" ] && [ -x "$bison/bin/bison" ]; then
        PATH=$bison/bin:$PATH
        export PATH
    fi
fi

mkdir -p "$out"
cd "$out"
bash "$src/build-toolchain.bash" --no-68k --no-carbon --multiversal --ninja \
    --prefix="$prefix"
echo "$rev" >"$prefix/REVISION"
echo "toolchain: $prefix"
