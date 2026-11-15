#!/usr/bin/env python3
"""Measure how much of each application's heap is free, and how much of
that is whole 4 KB pages that could be given back while the application
runs.

Usage: tools/heap-pages.py [ZONE...]

Without ZONE, takes each application's partition from tools/xlog-read.py
(the engine must be running; virtual memory off, as for tools/lmem.py).
The application zone starts at the partition's start.

For each zone it walks the heap blocks. Mac OS 9's Memory Manager gives
each block a 16-byte header: the previous block, flags, the block's size
with the header, and for a relocatable block its master pointer's offset
(for a nonrelocatable one the zone). The flags' first byte is 6B for a
nonrelocatable block, AB for a relocatable one, 00 or 10 for a free one;
the free blocks add up to the zone's zcbFree (zone + 12).

Columns:
  heap       the zone's blocks, in KB
  free       free blocks, in KB and as a share of the heap
  pages now  whole pages inside free blocks (after the block's first 32
             bytes, which hold its header and free-list links)
  compacted  whole pages if all free space were one block (an upper
             bound: nonrelocatable and locked blocks stay in the way)
  rest       from the heap's end to the partition's end: the stack and
             the A5 world, used or not
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lmem  # noqa: E402

PAGE = 0x1000
KEEP = 32


def domains():
    out = subprocess.run([os.path.join(HERE, "xlog-read.py")],
                         capture_output=True, text=True).stdout
    found = []
    for m in re.finditer(r"\[\s*\d+\] (.{15}) [0-9A-F]{8}-[0-9A-F]{8} "
                         r"partition ([0-9A-F]{8})\+([0-9A-F]{8})", out):
        found.append((m.group(1).strip(), int(m.group(2), 16),
                      int(m.group(3), 16)))
    return found


def walk(m, zone):
    lim = m.word(zone)
    a = zone + 0x30
    prev = 0
    heap = free = pages = 0
    while a < lim - 16:
        back, flags, size = m.word(a), m.word(a + 4), m.word(a + 8)
        if size < 16 or (prev and back != prev):
            raise ValueError("not a block at %08X" % a)
        heap += size
        if flags >> 24 in (0x00, 0x10):
            free += size
            lo = (a + KEEP + PAGE - 1) & ~(PAGE - 1)
            hi = (a + size) & ~(PAGE - 1)
            if hi > lo:
                pages += (hi - lo) // PAGE
        prev = a
        a += size
    return lim, heap, free, pages, m.word(zone + 12)


def main(argv):
    zones = ([("%08X" % int(z, 0), int(z, 0), 0) for z in argv]
             or domains())
    if not zones:
        sys.exit("no zones: is the engine running?")
    tmp = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
    tmp.close()
    try:
        lmem.save(tmp.name)
        m = lmem.Memory(tmp.name)
        print("%-16s %8s %14s %10s %10s %8s" % (
            "application", "heap KB", "free KB", "pages now", "compacted",
            "rest KB"))
        t = [0, 0, 0, 0]
        for name, zone, psize in zones:
            try:
                lim, heap, free, pages, zcb = walk(m, zone)
            except ValueError as e:
                print("%-16s %s" % (name, e))
                continue
            note = "" if free == zcb else "  (zcbFree %d KB)" % (zcb // 1024)
            rest = (zone + psize - lim) // 1024 if psize else 0
            print("%-16s %8d %8d (%2d%%) %10d %10d %8d%s" % (
                name, heap // 1024, free // 1024,
                100 * free // heap if heap else 0, pages, free // PAGE,
                rest, note))
            for i, v in enumerate((heap, free, pages, free // PAGE)):
                t[i] += v
        if len(zones) > 1 and t[0]:
            print("%-16s %8d %8d (%2d%%) %10d %10d" % (
                "all", t[0] // 1024, t[1] // 1024, 100 * t[1] // t[0],
                t[2], t[3]))
    finally:
        os.unlink(tmp.name)


if __name__ == "__main__":
    main(sys.argv[1:])
