#!/bin/sh
# Build the pcwatch QEMU plugin: tools/pcwatch/build/libpcwatch.dylib.
#
# It needs glib from Homebrew and the QEMU that qemu/build.sh installs (in
# /opt/qemu-os9, or $QEMU_PREFIX), which has plugins enabled: the plugin is
# built with that QEMU's plugin header and runs only in it.

set -eu
cd "$(dirname "$0")"

header=${QEMU_PREFIX:-/opt/qemu-os9}/include
if [ ! -f "$header/qemu-plugin.h" ]; then
  echo "no $header/qemu-plugin.h: run qemu/build.sh first" >&2
  exit 1
fi

mkdir -p build
# shellcheck disable=SC2046
cc -O2 -g -Wall -std=gnu11 -fPIC -I"$header" $(pkg-config --cflags glib-2.0) \
  -bundle -Wl,-undefined,dynamic_lookup \
  -o build/libpcwatch.dylib pcwatch.c $(pkg-config --libs glib-2.0)
echo "built tools/pcwatch/build/libpcwatch.dylib"
