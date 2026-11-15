#!/bin/sh
# Make tbxi's Python environment, refs/tbxi-venv, from the refs/tbxi
# submodule. kernel/extract.sh, kernel/pack.sh and tools/rompatch-test.sh
# use it.
#
# Usage: tools/tbxi-setup.sh
#
# tbxi's setup leaves out its C compressor (fast_lzss) when it fails to
# build, and then packs a ROM file in pure Python, which takes minutes
# instead of a second. With a pyenv Python on macOS the link fails (the
# linker cannot read the SDK's library stubs), so when the compressor is
# missing this script compiles it with Xcode's clang.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
venv=$root/refs/tbxi-venv
py=$venv/bin/python

if [ ! -f "$root/refs/tbxi/setup.py" ]; then
    git -C "$root" submodule update --init refs/tbxi
fi
[ -x "$py" ] || python3 -m venv "$venv"
"$venv/bin/pip" install --quiet "$root/refs/tbxi"

if ! "$py" -I -c 'import tbxi.fast_lzss' 2>/dev/null; then
    inc=$("$py" -I -c 'import sysconfig; print(sysconfig.get_paths()["include"])')
    ext=$("$py" -I -c 'import sysconfig; print(sysconfig.get_config_var("EXT_SUFFIX"))')
    # tbxi has no __init__.py: it is a namespace package, with no __file__.
    pkg=$("$py" -I -c 'import tbxi; print(list(tbxi.__path__)[0])')
    cc=cc
    command -v xcrun >/dev/null 2>&1 && cc="xcrun clang"
    $cc -O3 -bundle -undefined dynamic_lookup -I "$inc" \
        "$root/refs/tbxi/speedups/fast_lzss.c" -o "$pkg/fast_lzss$ext"
    "$py" -I -c 'import tbxi.fast_lzss'
fi
echo "tbxi: $venv/bin/tbxi, with fast_lzss"
