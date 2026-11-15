#!/bin/sh
# Build the patched QEMU that run.sh prefers and install it in
# /opt/qemu-os9, or in $QEMU_PREFIX when that is set.
#
# Usage:
#   qemu/build.sh        fetch, patch, configure, build and install
#   qemu/build.sh src    fetch and patch only (qemu/src), for editing patches
#
# Steps:
#   1. Fetch the submodule qemu/src (QEMU's git, at the release tag the
#      superproject pins) if it is missing.
#   2. Apply each qemu/patches/*.patch in name order to its checkout (a
#      detached HEAD; .gitmodules ignores the changes). A patch that is
#      already applied is skipped, so the script can be run again.
#   3. Configure qemu/build for the install folder if it has not been
#      configured, build it, and install it. Configuring fetches QEMU's
#      meson subprojects into qemu/src. To change the install folder,
#      remove qemu/build first.
#
# The install folder must be writable. To make /opt/qemu-os9, run once:
#   sudo mkdir -p /opt/qemu-os9 && sudo chown "$USER" /opt/qemu-os9
#
# To change a patch, edit the files in qemu/src, then regenerate it (while
# there is only one patch):
#   git -C qemu/src diff > qemu/patches/0001-cocoa-zoom-integer.patch
# To go back to the unpatched release: git -C qemu/src checkout -- .

set -eu
cd "$(dirname "$0")"
prefix=${QEMU_PREFIX:-/opt/qemu-os9}

stage=${1:-all}
case "$stage" in
  all|src) ;;
  *) echo "usage: $0 [src]" >&2; exit 2 ;;
esac

if [ ! -f src/configure ]; then
  git -C .. submodule update --init --depth 1 qemu/src
fi

for p in patches/*.patch; do
  [ -e "$p" ] || continue
  name=$(basename "$p")
  if git -C src apply --check -R "../$p" 2>/dev/null; then
    echo "$name: already applied"
  elif git -C src apply --check "../$p"; then
    git -C src apply "../$p"
    echo "$name: applied"
  else
    echo "$name: does not apply to qemu/src" >&2
    exit 1
  fi
done

[ "$stage" = src ] && exit 0

if [ ! -f build/build.ninja ]; then
  mkdir -p build
  (cd build && nice -n 19 ../src/configure --prefix="$prefix" \
    --target-list=ppc-softmmu --enable-cocoa --enable-plugins \
    --disable-docs --disable-werror)
fi
nice -n 19 make -C build -j"$(sysctl -n hw.ncpu)"

mkdir -p "$prefix" 2>/dev/null || true
if [ ! -w "$prefix" ]; then
  echo "Built qemu/build, but $prefix is not a folder you can write." >&2
  echo "To make it, run:" >&2
  echo "  sudo mkdir -p $prefix && sudo chown \"\$USER\" $prefix" >&2
  echo "then run qemu/build.sh again to install." >&2
  exit 1
fi
make -C build install >/dev/null
git -C src describe --tags --always >"$prefix/REVISION"
echo "Installed $prefix/bin/qemu-system-ppc"
