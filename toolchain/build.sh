#!/bin/sh
# Build the Retro68 PowerPC toolchain in toolchain/build/toolchain from the
# Retro68 submodule (toolchain/Retro68). ./build.sh uses it.
#
# Usage: toolchain/build.sh
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

if [ ! -f "$src/build-toolchain.bash" ] ||
   [ ! -f "$src/multiversal/make-multiverse.rb" ]; then
    git -C "$here/.." submodule update --init --recursive
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
bash "$src/build-toolchain.bash" --no-68k --no-carbon --multiversal --ninja
echo "toolchain: $out/toolchain"
