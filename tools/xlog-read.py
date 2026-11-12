#!/usr/bin/env python3
"""Print XLog's state from the running guest: counters, domains and
cross-domain writers.

Usage: tools/xlog-read.py [STATE_ADDR]

Without an address, it takes the state pointer XLog leaves in the 68k write
hook's data block (the `owner` word at 0x68FFFBEC). The state starts with a
header that gives the offsets and sizes of its parts; the field order inside
Counters, Domain and Writer must match xlog/xlog.c.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vdump  # noqa: E402

OWNER = 0x68FFFBEC
HEADER = ["magic", "self", "off_counters", "n_counters", "off_domains",
          "n_domains", "domain_size", "off_writers", "max_writers",
          "writer_size", "off_cur"]
SOURCES = ["native", "68k", "fallback"]
COUNTERS = (["own_" + s for s in SOURCES] + ["cross_" + s for s in SOURCES] +
            ["lost_" + s for s in SOURCES] +
            ["outside", "switches", "timer_runs", "kernel_errors",
             "refreshes", "added", "removed", "dropped", "nwriters",
             "info_errors"])
DOMAIN_SIZE, WRITER_SIZE = 60, 64


def collect(addr):
    def fn(read):
        a = addr
        if a is None:
            a = struct.unpack(">I", read(OWNER, 4))[0]
        if not a:
            return a, None, None
        h = dict(zip(HEADER, struct.unpack(">11I", read(a, 44))))
        if h["magic"] != struct.unpack(">I", b"XLG2")[0] or h["self"] != a:
            return a, h, None
        size = h["off_writers"] + h["max_writers"] * h["writer_size"]
        return a, h, read(a, size)
    return fn


def cstr(b):
    return b.split(b"\0", 1)[0].decode("mac_roman")


def main():
    addr = int(sys.argv[1], 0) if len(sys.argv) > 1 else None
    addr, h, d = vdump.paused(collect(addr))
    if not addr:
        sys.exit("XLog is not running (no owner in the 68k hook)")
    if d is None:
        sys.exit("no XLog v2 state at %08X" % addr)
    if (h["n_counters"] != len(COUNTERS) or h["domain_size"] != DOMAIN_SIZE
            or h["writer_size"] != WRITER_SIZE):
        sys.exit("layout differs from this script: %s" % h)

    def w(o):
        return struct.unpack(">I", d[o:o + 4])[0]

    o = h["off_cur"]
    cur = struct.unpack(">i", d[o:o + 4])[0]
    print("state %08X: current domain %d (zone %08X), active %d, area %08X, "
          "mine %08X-%08X" % (addr, cur, w(o + 4), w(o + 8), w(o + 12),
                              w(o + 16), w(o + 20)))
    c = {n: w(h["off_counters"] + 4 * i) for i, n in enumerate(COUNTERS)}
    print(", ".join("%s %d" % (n, c[n]) for n in COUNTERS))
    print("domains:")
    for i in range(h["n_domains"]):
        o = h["off_domains"] + i * DOMAIN_SIZE
        if w(o):
            print("  [%2d] %-15s %08X-%08X partition %08X+%08X dying %d own %d "
                  "written by others %d" % (
                      i, cstr(d[o + 36:o + 52]), w(o + 12), w(o + 16),
                      w(o + 20), w(o + 24), w(o + 4), w(o + 52), w(o + 56)))
    rows = []
    for i in range(min(c["nwriters"], h["max_writers"])):
        o = h["off_writers"] + i * WRITER_SIZE
        rows.append((w(o + 24), w(o), w(o + 4), w(o + 28),
                     cstr(d[o + 32:o + 48]), cstr(d[o + 48:o + 64]),
                     w(o + 16), w(o + 8)))
    rows.sort(reverse=True)
    print("cross-domain writers (68k: PC is the instruction after the store):")
    print(" count PC       LR       writer          target          addr     "
          "writer zone")
    for count, pc, lr, src, writer, target, first, zone in rows:
        lrs = " (68k)  " if src == 1 else "%08X" % lr
        print("%6d %08X %s %-15s %-15s %08X %08X%s" % (
            count, pc, lrs, writer, target, first, zone,
            " (fallback)" if src == 2 else ""))


if __name__ == "__main__":
    main()
