#!/bin/sh
# Build the storewatch QEMU plugin: tools/storewatch/build/libstorewatch.dylib.
#
# It needs glib from Homebrew and the QEMU that qemu/build.sh installs (in
# ~/.local/opt/qemu-os9, or $QEMU_PREFIX), which has plugins enabled: the
# plugin is built with that QEMU's plugin header and runs only in it.

set -eu
cd "$(dirname "$0")"

header=${QEMU_PREFIX:-$HOME/.local/opt/qemu-os9}/include
if [ ! -f "$header/qemu-plugin.h" ]; then
  echo "no $header/qemu-plugin.h: run qemu/build.sh first" >&2
  exit 1
fi

mkdir -p build
# shellcheck disable=SC2046
cc -O2 -g -Wall -std=gnu11 -fPIC -I"$header" $(pkg-config --cflags glib-2.0) \
  -bundle -Wl,-undefined,dynamic_lookup \
  -o build/libstorewatch.dylib storewatch.c $(pkg-config --libs glib-2.0)
echo "built tools/storewatch/build/libstorewatch.dylib"
