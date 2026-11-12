#!/bin/sh
# Launch the Mac OS 9.2.2 test machine in QEMU (mac99, PowerPC G4).
#
# Usage:
#   ./run.sh install   boot the 9.2.2 install CD, with the hard disk attached
#   ./run.sh           boot from the hard disk
#   ./run.sh debug     boot from the hard disk, paused, gdb stub on localhost:1234
#
# Set PLUGIN=library[,args] to load a QEMU TCG plugin (needs the local
# build, qemu/build), for example tools/storewatch.
# Set HEADLESS=1 to run without a window, SCRATCH=1 to discard disk writes,
# CD=image to start with a disc in the CD drive (it is empty otherwise),
# DISK=image to boot another hard disk image (default disks/os9.qcow2; for
# example disks/os9-novm.qcow2, an overlay with virtual memory off).
# The guest has a network card (sungem) on QEMU's user-mode network: DHCP,
# outbound internet, and the host reachable at 10.0.2.2.
# A QMP control socket is always opened at run/qmp.sock, for screenshots
# and scripted input.
#
# A guest restart hangs QEMU's mac99 firmware on a black screen, so QEMU is
# told to exit on a guest restart instead, and this script starts it again
# from the hard disk.

set -eu
cd "$(dirname "$0")"

DISK=${DISK:-disks/os9.qcow2}
CDROM=media/macos-922-uni.iso
QMP=run/qmp.sock
EVENTS=run/events.sock

# Prefer the local build (qemu/build), which adds integer scaling to the
# Cocoa window; fall back to Homebrew's QEMU.
QEMU=qemu-system-ppc
zoom="zoom-to-fit=on"
if [ -x qemu/build/qemu-system-ppc ]; then
  QEMU=qemu/build/qemu-system-ppc
  zoom="zoom-integer=on"
fi

mkdir -p run disks

if [ ! -f "$DISK" ]; then
  qemu-img create -f qcow2 "$DISK" 8G
fi

mode=${1:-run}
case "$mode" in
  install|run|debug) ;;
  *) echo "usage: $0 [install|run|debug]" >&2; exit 2 ;;
esac

display="-display cocoa,$zoom"
if [ "${HEADLESS:-}" = 1 ]; then
  display="-display none"
fi

# SCRATCH=1 keeps every disk write in a temporary file that QEMU discards on
# exit, so a test run leaves the disk image unchanged.
scratch=""
if [ "${SCRATCH:-}" = 1 ]; then
  scratch="-snapshot"
fi

plugin=""
if [ -n "${PLUGIN:-}" ]; then
  plugin="-plugin $PLUGIN"
fi

qemu_pid=""
trap '[ -n "$qemu_pid" ] && kill "$qemu_pid" 2>/dev/null; exit 130' INT TERM

while :; do
  boot=c
  debug=""
  cd_image=${CD:-}
  case "$mode" in
    install) boot=d; cd_image=$CDROM ;;
    debug) debug="-s -S" ;;
  esac

  # The CD drive starts empty unless CD=image is set (install mode uses the
  # 9.2.2 CD); tools/push.sh inserts discs into it while the guest runs.
  if [ -n "$cd_image" ]; then
    cd_drive="if=ide,index=2,media=cdrom,readonly=on,file=$cd_image"
  else
    cd_drive="if=ide,index=2,media=cdrom"
  fi

  rm -f "$QMP" "$EVENTS"

  # The events socket waits for tools/qmp.py to connect before the guest
  # starts, so the SHUTDOWN event that says why QEMU stopped is not missed.
  # shellcheck disable=SC2086
  "$QEMU" \
    -name "Mac OS 9.2.2" \
    -M mac99,via=pmu \
    -m 512 \
    -boot "$boot" \
    -drive file="$DISK",format=qcow2,media=disk \
    -drive "$cd_drive" \
    -g 1024x768x32 \
    -prom-env 'vga-ndrv?=true' \
    -prom-env 'auto-boot?=true' \
    -nic user,model=sungem \
    -action reboot=shutdown \
    -qmp unix:"$QMP",server=on,wait=off \
    -qmp unix:"$EVENTS",server=on,wait=on \
    $display $debug $scratch $plugin &
  qemu_pid=$!

  reason=$(QMP_SOCK="$EVENTS" tools/qmp.py wait-shutdown) || reason=""
  status=0
  wait "$qemu_pid" || status=$?
  qemu_pid=""

  # guest-reset: Special > Restart in the guest.
  # host-qmp-system-reset: a system_reset sent over QMP.
  case "$reason" in
    guest-reset|host-qmp-system-reset)
      echo "run.sh: machine restarted; starting QEMU again from the hard disk" >&2
      mode=run
      continue
      ;;
  esac
  exit "$status"
done
