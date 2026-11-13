# Find the Retro68 PowerPC toolchain and set RETRO68 to its folder (the one
# with bin/ and powerpc-apple-macos/). Sourced by ./build.sh,
# tools/make-dist.sh and kernel/build.sh, with ROOT set to the repository's
# top folder. In order:
#   $RETRO68, if set;
#   toolchain/build/toolchain, built by toolchain/build.sh;
#   the test machine's, in os9-protect (OS9_PROTECT, default ../os9-protect).
# Exits if there is none.

if [ -z "${RETRO68:-}" ]; then
    for d in "$ROOT/toolchain/build/toolchain" \
             "${OS9_PROTECT:-$ROOT/../os9-protect}/toolchain/build/toolchain"; do
        if [ -f "$d/powerpc-apple-macos/cmake/retroppc.toolchain.cmake" ]; then
            RETRO68=$(cd "$d" && pwd)
            break
        fi
    done
fi
if [ -z "${RETRO68:-}" ] ||
   [ ! -f "$RETRO68/powerpc-apple-macos/cmake/retroppc.toolchain.cmake" ]; then
    echo "No Retro68 PowerPC toolchain found. To build one, run" >&2
    echo "toolchain/build.sh. To use one you have, set RETRO68 to its folder." >&2
    exit 1
fi
