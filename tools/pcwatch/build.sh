#!/bin/sh
# Build the pcwatch QEMU plugin: tools/pcwatch/build/libpcwatch.dylib.
#
# It needs the plugin header from the QEMU sources (qemu/build.sh src, or a
# full qemu/build.sh) and glib from Homebrew, and runs only with the local
# QEMU build (qemu/build/qemu-system-ppc), which has plugins enabled.

set -eu
cd "$(dirname "$0")"

header=../../qemu/src/include/plugins
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
