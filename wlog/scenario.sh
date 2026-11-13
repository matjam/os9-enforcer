#!/bin/sh
# Boot a scratch VM, run WLog, have the Finder draw over it, then stop WLog.
# Usage: wlog/scenario.sh [WLog.bin]
# Runs in the test machine's folder (OS9_PROTECT, default ../os9-protect):
# screenshots go to its run/ws-*.png. Leaves the VM running.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
wlog=${1:-$here/wlog/build/WLog.bin}
case $wlog in /*) ;; *) wlog=$PWD/$wlog ;; esac
cd "${OS9_PROTECT:-$here/../os9-protect}" || exit 1
q=tools/qmp.py
w() { s=$(date +%s); until [ $(( $(date +%s) - s )) -ge "$1" ]; do :; done; }

if ! $q status >/dev/null 2>&1; then
  SCRATCH=1 HEADLESS=1 ./run.sh >run/wlog-vm.log 2>&1 &
  w 5
  start=$(date +%s)
  until [ $(( $(date +%s) - start )) -ge 90 ] && $q pos >/dev/null 2>&1; do sleep 2; done
fi

tools/push.sh -n WLog "$wlog" >/dev/null || exit 1
w 4
$q dclick 590 108; w 3
$q dclick 44 90; w 6
$q screenshot run/ws-0.png
$q key cmd-tab; w 3
$q screenshot run/ws-1.png
$q drag 230 35 330 200 || exit 1; w 1
$q drag 330 200 150 300 || exit 1; w 1
$q drag 150 300 260 120 || exit 1; w 1
$q click 22 9; w 1
$q key esc; w 1
$q screenshot run/ws-2.png
$q key cmd-w; w 2
$q key cmd-tab; w 3
$q key space; w 4
$q screenshot run/ws-3.png
