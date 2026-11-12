#!/bin/sh
# Build the patched QEMU that run.sh prefers (qemu/build/qemu-system-ppc).
#
# Usage:
#   qemu/build.sh        fetch, patch, configure and build
#   qemu/build.sh src    fetch and patch only (qemu/src), for editing patches
#
# Steps:
#   1. Download the QEMU release tarball to qemu/qemu-VERSION.tar.xz, unless
#      it is already there, and check its SHA-256.
#   2. If qemu/src does not exist, unpack the tarball into it and make it a
#      git repository whose one commit is the unmodified release.
#   3. Apply each qemu/patches/*.patch in name order. A patch that is already
#      applied is skipped, so the script can be run again on an existing tree.
#   4. Configure qemu/build if it has not been configured, then build it.
#
# To change a patch, edit the files in qemu/src, then regenerate it (while
# there is only one patch):
#   git -C qemu/src diff > qemu/patches/0001-cocoa-zoom-integer.patch

set -eu
cd "$(dirname "$0")"

VERSION=11.1.3
URL=https://download.qemu.org/qemu-$VERSION.tar.xz
SHA256=5a8289c7940dc77d3873c81a6c29ba3afe1446fc9d36e77fa1658cdbecb84b22
TARBALL=qemu-$VERSION.tar.xz

stage=${1:-all}
case "$stage" in
  all|src) ;;
  *) echo "usage: $0 [src]" >&2; exit 2 ;;
esac

if [ ! -f "$TARBALL" ]; then
  echo "Downloading $URL"
  curl -fL -o "$TARBALL.part" "$URL"
  mv "$TARBALL.part" "$TARBALL"
fi
actual=$(shasum -a 256 "$TARBALL" | cut -d' ' -f1)
if [ "$actual" != "$SHA256" ]; then
  echo "$TARBALL: SHA-256 is $actual, expected $SHA256" >&2
  exit 1
fi

if [ ! -d src ]; then
  echo "Unpacking $TARBALL into qemu/src"
  rm -rf src.part
  mkdir src.part
  tar -xJf "$TARBALL" -C src.part --strip-components=1
  git -C src.part init -q
  git -C src.part add -A
  git -C src.part -c user.name=qemu/build.sh -c user.email=none \
    commit -q -m "QEMU $VERSION release tarball"
  mv src.part src
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
  (cd build && nice -n 19 ../src/configure --target-list=ppc-softmmu \
    --enable-cocoa --disable-docs --disable-werror)
fi
nice -n 19 make -C build -j"$(sysctl -n hw.ncpu)"
echo "Built qemu/build/qemu-system-ppc"
