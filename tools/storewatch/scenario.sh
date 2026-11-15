#!/bin/sh
# Measure stores into low memory and the system heap in two phases, with the
# storewatch plugin loaded:
#   idle  20 seconds at the desktop;
#   use   open and close the System Folder window, type a sum into the
#         Calculator, open Stickies and type, switch applications three
#         times and drag a note.
# Each phase is saved as OUT-idle.* and OUT-use.* (sites, words, and pages
# with the plugin's epoch= option) for tools/storewatch/report.py.
#
# Usage: tools/storewatch/scenario.sh [OUT]   (default run/sw)
#
# Start the guest first, from the Enforcer test disk so that Enforcer's 68k
# hook says which application is current:
#   SCRATCH=1 DISK=disks/os9-enforcer.qcow2 \
#     PLUGIN=tools/storewatch/build/libstorewatch.dylib,out=run/sw ./run.sh &
# The clicks are for that disk's desktop at 640x480 guest pixels: the OS
# window open at the bottom left, the Apple menu as installed.

set -eu
cd "$(dirname "$0")/../.."
out=${1:-run/sw}
q=tools/qmp.py

# The desktop is up once Enforcer reports a current application.
for _ in $(seq 1 200); do
  if [ -f "$out.rate" ] && tail -1 "$out.rate" | grep -qv "cur 00000000-"; then
    break
  fi
  sleep 3
done
sleep 30

# Save the counts since the last reset as OUT-NAME.*, then reset.
phase() {
  touch "$out.dump"
  while [ -f "$out.dump" ]; do sleep 1; done
  sleep 1
  cp "$out.sites" "$out-$1.sites"
  cp "$out.words" "$out-$1.words"
  if [ -f "$out.pages" ]; then cp "$out.pages" "$out-$1.pages"; fi
  touch "$out.reset"
  while [ -f "$out.reset" ]; do sleep 1; done
}

touch "$out.reset"
while [ -f "$out.reset" ]; do sleep 1; done
sleep 20
phase idle

$q dclick 60 305; sleep 4      # System Folder
$q key cmd-w; sleep 2
$q click 22 9; sleep 3         # Apple menu
$q click 80 104; sleep 8       # Calculator
$q type '12+34='; sleep 2
$q click 22 9; sleep 3
$q click 72 302; sleep 10      # Stickies
$q type 'Measuring stores'; sleep 2
for _ in 1 2 3; do $q key cmd-tab; sleep 3; done
$q drag 300 254 330 200; sleep 2
phase use
echo "saved $out-idle.* and $out-use.*"
