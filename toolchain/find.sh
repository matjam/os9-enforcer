# Find the Retro68 PowerPC toolchain and set RETRO68 to its folder (the one
# with bin/ and powerpc-apple-macos/). Sourced by ./build.sh,
# tools/make-dist.sh, kernel/build.sh and kernel/extract.sh, with ROOT set
# to the repository's top folder. RETRO68 defaults to /opt/retro68, where
# toolchain/build.sh installs it. Exits if there is none there.

RETRO68=${RETRO68:-/opt/retro68}
if [ ! -f "$RETRO68/powerpc-apple-macos/cmake/retroppc.toolchain.cmake" ]; then
    echo "No Retro68 PowerPC toolchain in $RETRO68. To build one, run" >&2
    echo "toolchain/build.sh. To use another, set RETRO68 to its folder." >&2
    exit 1
fi

# The toolchain is shared with other projects: say so when it was built
# from another Retro68 commit than the one this repository pins.
if [ -f "$RETRO68/REVISION" ]; then
    pinned=$(git -C "$ROOT" rev-parse HEAD:toolchain/Retro68 2>/dev/null || true)
    built=$(cat "$RETRO68/REVISION")
    if [ -n "$pinned" ] && [ "$pinned" != "$built" ]; then
        echo "warning: $RETRO68 is built from Retro68 $built;" >&2
        echo "this repository pins $pinned (toolchain/build.sh)." >&2
    fi
fi
