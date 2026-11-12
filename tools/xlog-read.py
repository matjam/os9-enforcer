#!/usr/bin/env python3
"""Print XLog's state from the running guest: counters, domains, open pages
and cross-domain writers.

Usage: tools/xlog-read.py [STATE_ADDR]

Without an address, it takes the state pointer XLog leaves in the 68k write
hook's data block (the `owner` word at 0x68FFFBEC). The layout must match
`State` in xlog/xlog.c.
"""

import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
VDUMP = os.path.join(HERE, "vdump.py")

MAX_DOMAINS, MAX_OPEN, MAX_WRITERS = 32, 256, 512
DOMAIN_SIZE = 60      # live dying seen lo hi location size psn[2] name[16] self foreign
WRITER_SIZE = 64      # pc lr writer_zone target_lo first last count is68k writer[16] target[16]
OFF_DOMAINS = 32
OFF_CUR = OFF_DOMAINS + MAX_DOMAINS * DOMAIN_SIZE
OFF_OPEN = OFF_CUR + 12
OFF_COUNTERS = OFF_OPEN + MAX_OPEN * 8
COUNTERS = ["bitmap", "h68k_seen", "h68k_lost", "native_self", "native_foreign",
            "m68k_self", "m68k_foreign", "switches", "timer_runs",
            "kernel_errors", "open_overflows", "refreshes", "added", "removed",
            "chained", "dropped", "nwriters"]
OFF_WRITERS = OFF_COUNTERS + 4 * len(COUNTERS)
STATE_SIZE = OFF_WRITERS + MAX_WRITERS * WRITER_SIZE


def dump(ranges):
    with tempfile.TemporaryDirectory() as tmp:
        args, paths = [], []
        for i, (addr, length) in enumerate(ranges):
            p = os.path.join(tmp, "%d.bin" % i)
            args += ["0x%x" % addr, str(length), p]
            paths.append(p)
        subprocess.run([VDUMP] + args, check=True, capture_output=True)
        return [open(p, "rb").read() for p in paths]


def cstr(b):
    return b.split(b"\0", 1)[0].decode("mac_roman")


def main():
    if len(sys.argv) > 1:
        addr = int(sys.argv[1], 0)
    else:
        (owner,) = dump([(0x68FFFBEC, 4)])
        addr = struct.unpack(">I", owner)[0]
    (d,) = dump([(addr, STATE_SIZE)])
    w = lambda o: struct.unpack(">I", d[o:o + 4])[0]
    if d[:4] != b"XLG!":
        sys.exit("no XLog state at %08X" % addr)
    print("state %08X: active %d busy %d area %08X mine %08X-%08X zone %08X"
          % (addr, w(8), w(12), w(16), w(20), w(24), w(28)))
    cur = struct.unpack(">i", d[OFF_CUR:OFF_CUR + 4])[0]
    print("cur %d cur_zone %08X nopen %d" % (cur, w(OFF_CUR + 4), w(OFF_CUR + 8)))
    c = {n: w(OFF_COUNTERS + 4 * i) for i, n in enumerate(COUNTERS)}
    print(", ".join("%s %d" % (n, c[n]) for n in COUNTERS[1:]))
    print("domains:")
    for i in range(MAX_DOMAINS):
        o = OFF_DOMAINS + i * DOMAIN_SIZE
        if w(o):
            print("  [%2d] %-15s %08X-%08X loc %08X size %08X dying %d self %d "
                  "written by others %d" % (i, cstr(d[o + 36:o + 52]), w(o + 12),
                                            w(o + 16), w(o + 20), w(o + 24),
                                            w(o + 4), w(o + 52), w(o + 56)))
    rows = []
    for i in range(min(c["nwriters"], MAX_WRITERS)):
        o = OFF_WRITERS + i * WRITER_SIZE
        rows.append((w(o + 24), w(o), w(o + 4), w(o + 28), cstr(d[o + 32:o + 48]),
                     cstr(d[o + 48:o + 64]), w(o + 16), w(o + 8)))
    rows.sort(reverse=True)
    print("cross-domain writers (68k: PC is the instruction after the store):")
    print(" count PC       LR       writer          target          addr     "
          "writer zone")
    for count, pc, lr, is68k, writer, target, first, zone in rows:
        print("%6d %08X %s %-15s %-15s %08X %08X" % (
            count, pc, " (68k)  " if is68k else "%08X" % lr, writer, target,
            first, zone))


if __name__ == "__main__":
    main()
