#!/bin/sh
# Install the Retro68 PowerPC toolchain in ~/.local/opt/retro68, or in
# $RETRO68 when that is set. ./build.sh, kernel/build.sh and
# tools/make-dist.sh find it there (toolchain/find.sh); CI runs this script
# to build it.
#
# Usage:
#   toolchain/build.sh       fetch, build and install
#   toolchain/build.sh src   fetch only
#
# Fetches Retro68 at the commit below, with its multiversal submodule, into
# toolchain/Retro68, builds in toolchain/build, and installs. Both folders
# are git-ignored and can be removed afterwards. A finished install records
# the commit in REVISION; when that is this commit, the script does
# nothing. To move to another commit, change it here, empty the install
# folder, and run the script.
#
# Builds PowerPC only, with the open-source Multiversal Interfaces:
# Enforcer has no 68k or Carbon programs and needs none of Apple's
# Universal Interfaces.
#
# Needs, on macOS:
#   brew install boost cmake ninja gmp mpfr libmpc bison texinfo
# On Debian or Ubuntu:
#   sudo apt-get install cmake ninja-build g++ libgmp-dev libmpfr-dev \
#       libmpc-dev libboost-all-dev bison flex texinfo ruby
set -eu
commit=a4a389c41e90b1095a40ccf13f28678b6cfbba86
url=https://github.com/autc04/Retro68.git

here=$(cd "$(dirname "$0")" && pwd)
src=$here/Retro68
out=$here/build
prefix=${RETRO68:-$HOME/.local/opt/retro68}

stage=${1:-all}
case "$stage" in
    all|src) ;;
    *) echo "usage: $0 [src]" >&2; exit 2 ;;
esac

if [ "$stage" = all ] && [ -f "$prefix/REVISION" ] &&
   [ "$(cat "$prefix/REVISION")" = "$commit" ]; then
    echo "toolchain: $prefix is already built from Retro68 $commit"
    exit 0
fi

# Fetch the one commit, not Retro68's history.
if [ ! -d "$src/.git" ] || [ "$(git -C "$src" rev-parse HEAD)" != "$commit" ]; then
    rm -rf "$src"
    git init -q "$src"
    git -C "$src" fetch -q --depth 1 "$url" "$commit"
    git -C "$src" checkout -q FETCH_HEAD
fi
git -C "$src" submodule update -q --init --recursive --depth 1
[ "$stage" = src ] && exit 0

mkdir -p "$prefix"
if [ ! -w "$prefix" ]; then
    echo "$prefix is not a folder you can write." >&2
    exit 1
fi
if [ -n "$(ls -A "$prefix")" ]; then
    old=$(cat "$prefix/REVISION" 2>/dev/null || echo "an unknown commit")
    echo "$prefix holds a toolchain built from Retro68 $old." >&2
    echo "To replace it with $commit, empty $prefix first." >&2
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
echo "$commit" >"$prefix/REVISION"
echo "toolchain: $prefix"
