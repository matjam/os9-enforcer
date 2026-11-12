#!/usr/bin/env python3
"""Print the protection engine's state from the running guest (XLog, Guard
or the Enforcer Extension): mode, counters, domains, cross-domain writers
and stores by an application's own code.

Usage: tools/xlog-read.py [STATE_ADDR]

Without an address, it takes the state pointer the engine leaves in the 68k
write hook's data block (the `owner` word at 0x68FFFBEC). The state starts
with a header that gives the offsets and sizes of its parts; the field
order inside Counters, Domain, Writer and Offense must match
common/engine.h.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vdump  # noqa: E402

OWNER = 0x68FFFBEC
HEADER = ["magic", "self", "off_counters", "n_counters", "off_domains",
          "n_domains", "domain_size", "off_writers", "max_writers",
          "writer_size", "off_cur", "off_offenses", "max_offenses",
          "offense_size", "off_mode"]
MODES = ["off", "log only", "enforce"]
SOURCES = ["native", "68k", "fallback"]
COUNTERS = (["own_" + s for s in SOURCES] + ["cross_" + s for s in SOURCES] +
            ["lost_" + s for s in SOURCES] +
            ["outside", "switches", "timer_runs", "kernel_errors",
             "refreshes", "added", "removed", "dropped", "nwriters",
             "info_errors"] + ["app_code_" + s for s in SOURCES])
DOMAIN_SIZE, WRITER_SIZE, OFFENSE_SIZE = 60, 64, 56
SRC_MASK, APP_CODE = 0xFF, 0x100


def collect(addr):
    def fn(read):
        a = addr
        if a is None:
            a = struct.unpack(">I", read(OWNER, 4))[0]
        if not a:
            return a, None, None
        n = len(HEADER)
        h = dict(zip(HEADER, struct.unpack(">%dI" % n, read(a, 4 * n))))
        if h["magic"] != struct.unpack(">I", b"ENF1")[0] or h["self"] != a:
            return a, h, None
        size = max(h["off_writers"] + h["max_writers"] * h["writer_size"],
                   h["off_offenses"] + 4 +
                   h["max_offenses"] * h["offense_size"])
        return a, h, read(a, size)
    return fn


def cstr(b):
    return b.split(b"\0", 1)[0].decode("mac_roman")


def main():
    addr = int(sys.argv[1], 0) if len(sys.argv) > 1 else None
    addr, h, d = vdump.paused(collect(addr))
    if not addr:
        sys.exit("no engine is running (no owner in the 68k hook)")
    if d is None:
        sys.exit("no engine state (ENF1) at %08X" % addr)
    if (h["n_counters"] != len(COUNTERS) or h["domain_size"] != DOMAIN_SIZE
            or h["writer_size"] != WRITER_SIZE
            or h["offense_size"] != OFFENSE_SIZE):
        sys.exit("layout differs from this script: %s" % h)

    def w(o):
        return struct.unpack(">I", d[o:o + 4])[0]

    m = h["off_mode"]
    mode, want, running = w(m), w(m + 4), w(m + 8)
    print("%s: mode %s%s%s" % (
        cstr(d[m + 12:m + 28]), MODES[mode] if mode < 3 else mode,
        "" if want == mode else " (requested %s)" % MODES[want],
        "" if running else ", stopped"))

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
        lrs = " (68k)  " if src & SRC_MASK == 1 else "%08X" % lr
        print("%6d %08X %s %-15s %-15s %08X %08X%s%s" % (
            count, pc, lrs, writer, target, first, zone,
            " (fallback)" if src & SRC_MASK == 2 else "",
            " (app code)" if src & APP_CODE else ""))

    o = h["off_offenses"]
    n = w(o)
    print("stores by an application's own code: %d" % n)
    for seq in range(max(1, n - h["max_offenses"] + 1), n + 1):
        e = o + 4 + ((seq - 1) % h["max_offenses"]) * OFFENSE_SIZE
        t = w(e + 4) % 86400  # seconds since 1904, local time
        print("  #%d %02d:%02d:%02d %s: %s, %s code at %08X, into %s at %08X"
              % (w(e), t // 3600, t // 60 % 60, t % 60,
                 "denied" if w(e + 20) else "would deny",
                 cstr(d[e + 24:e + 40]), SOURCES[w(e + 16) & SRC_MASK],
                 w(e + 8), cstr(d[e + 40:e + 56]), w(e + 12)))


if __name__ == "__main__":
    main()
