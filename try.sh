#!/bin/sh
# Boot your own copy of the test machine, with Enforcer installed and on,
# to try things by hand.
#
# Usage: ./try.sh [--vm] [--fresh]
#   --vm     virtual memory on: a copy of disks/os9-pool.qcow2 (990 MB of
#            virtual memory); without it, a copy of disks/os9-vmoff.qcow2
#            (virtual memory off)
#   --fresh  make the copy again, losing what you changed in it
#
# The copy, disks/try.qcow2 (or disks/try-vm.qcow2), is made on first use
# as a whole image, so it does not depend on the test disks, which tests
# change. What you do in it is kept from run to run.
#
# It runs beside a test machine: its QMP socket is run/try.sock (or
# run/try-vm.sock). To drive it from the repository's top folder, set
# QMP_SOCK for tools/qmp.py, tools/push.sh and the tools that read the
# guest:
#   QMP_SOCK=run/try.sock tools/qmp.py screenshot run/try.png
# run.sh's other settings (MEM, HEADLESS, SCRATCH, CD) pass through.
#
# To put the latest build on it: run ./build.sh and tools/make-dist.sh,
# then, while it runs,
#   QMP_SOCK=run/try.sock tools/push.sh -n Enforcer dist/*.bin
# open Enforcer Installer from the CD, click Install, then Restart.

set -eu
cd "$(dirname "$0")"

base=disks/os9-vmoff.qcow2
name=try
fresh=0
for a in "$@"; do
  case "$a" in
    --vm) base=disks/os9-pool.qcow2; name=try-vm ;;
    --fresh) fresh=1 ;;
    *) echo "usage: $0 [--vm] [--fresh]" >&2; exit 2 ;;
  esac
done
disk=disks/$name.qcow2
sock=$PWD/run/$name.sock

if QMP_SOCK=$sock tools/qmp.py status >/dev/null 2>&1; then
  echo "$disk is already running (QMP socket run/$name.sock)" >&2
  exit 1
fi

qemu_img=${QEMU_PREFIX:-$HOME/.local/opt/qemu-os9}/bin/qemu-img
[ -x "$qemu_img" ] || qemu_img=qemu-img

if [ "$fresh" = 1 ] || [ ! -f "$disk" ]; then
  echo "Copying $base to $disk"
  "$qemu_img" convert -O qcow2 "$base" "$disk.part"
  mv "$disk.part" "$disk"
fi

mkdir -p run
DISK=$disk QMP_SOCK=$sock exec ./run.sh
