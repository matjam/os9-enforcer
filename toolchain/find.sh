# Find the Retro68 PowerPC toolchain and set RETRO68 to its folder (the one
# with bin/ and powerpc-apple-macos/). Sourced by ./build.sh,
# tools/make-dist.sh, kernel/build.sh and kernel/extract.sh. RETRO68
# defaults to ~/.local/opt/retro68, where toolchain/build.sh installs it.
# Exits if there is no toolchain there.

RETRO68=${RETRO68:-$HOME/.local/opt/retro68}
if [ ! -f "$RETRO68/powerpc-apple-macos/cmake/retroppc.toolchain.cmake" ]; then
    echo "No Retro68 PowerPC toolchain in $RETRO68. To install one, run" >&2
    echo "toolchain/build.sh. To use another, set RETRO68 to its folder." >&2
    exit 1
fi
