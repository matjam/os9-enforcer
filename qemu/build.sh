#!/bin/sh
# Install the patched QEMU that run.sh prefers in ~/.local/opt/qemu-os9, or
# in $QEMU_PREFIX when that is set.
#
# Usage:
#   qemu/build.sh        fetch, patch, configure, build and install
#   qemu/build.sh src    fetch and patch only (qemu/src), for editing patches
#
# Steps:
#   1. Fetch QEMU at the release below into qemu/src (a shallow clone of
#      its tag), unless it is there.
#   2. Apply each qemu/patches/*.patch in name order. A patch that is
#      already applied is skipped, so the script can be run again.
#   3. Configure qemu/build for the install folder if it has not been
#      configured for it (qemu/build/.prefix), build it, and install it.
#      Configuring fetches QEMU's meson subprojects into qemu/src.
# qemu/src and qemu/build are git-ignored and can be removed afterwards.
# A finished install records the release and the patches' SHA-1 in
# REVISION; when that matches and qemu/build is gone, the script does
# nothing.
#
# To change a patch, run qemu/build.sh src, edit the files in qemu/src,
# then regenerate it (while there is only one patch):
#   git -C qemu/src diff > qemu/patches/0001-cocoa-zoom-integer.patch
# To go back to the unpatched release: git -C qemu/src checkout -- .

set -eu
version=v11.1.3
url=https://github.com/qemu/qemu.git

cd "$(dirname "$0")"
prefix=${QEMU_PREFIX:-$HOME/.local/opt/qemu-os9}
revision="$version $(cat patches/*.patch 2>/dev/null | shasum | cut -d' ' -f1)"

stage=${1:-all}
case "$stage" in
  all|src) ;;
  *) echo "usage: $0 [src]" >&2; exit 2 ;;
esac

if [ "$stage" = all ] && [ ! -d build ] && [ -f "$prefix/REVISION" ] &&
   [ "$(cat "$prefix/REVISION")" = "$revision" ]; then
  echo "qemu: $prefix is already built from $version with these patches"
  exit 0
fi

if [ ! -d src/.git ] ||
   [ "$(git -C src describe --tags --exact-match 2>/dev/null)" != "$version" ]; then
  rm -rf src build
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$version" \
    "$url" src 2>&1 | grep -v 'is not a commit' >&2 || true
  [ -f src/configure ] || { echo "could not fetch QEMU $version" >&2; exit 1; }
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

if [ ! -f build/build.ninja ] ||
   [ "$(cat build/.prefix 2>/dev/null)" != "$prefix" ]; then
  mkdir -p build
  (cd build && nice -n 19 ../src/configure --prefix="$prefix" \
    --target-list=ppc-softmmu --enable-cocoa --enable-plugins \
    --disable-docs --disable-werror)
  echo "$prefix" >build/.prefix
fi
nice -n 19 make -C build -j"$(sysctl -n hw.ncpu)"

mkdir -p "$prefix"
make -C build install >/dev/null
echo "$revision" >"$prefix/REVISION"
echo "Installed $prefix/bin/qemu-system-ppc"
