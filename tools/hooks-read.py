#!/usr/bin/env python3
"""Print both write hooks' data blocks and rings from the running guest,
over QMP.

Usage: tools/hooks-read.py [N]

The 68k hook (common/h68k.s) keeps its data in the emulator data page, at
logical 0x68FFFB80. The native hook (common/hnat.s) lives in a system heap
block: the native context block's handler word (0x68FFF44C) points at its
code, and its data is the 0xC0 bytes before the code. Each data block holds
a pointer to its ring of 256 entries in the system heap. The newest N ring
entries of each hook are printed (default 64).

Everything is read in one pause of the guest (tools/vdump.py), so the
counters and rings agree with each other.
"""

import os
import struct
import sys

# vdump.py is in the test machine's tools: OS9_PROTECT, default
# ../os9-protect next to this repository.
sys.path.insert(0, os.path.join(os.environ.get("OS9_PROTECT", os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "os9-protect")),
    "tools"))
import vdump  # noqa: E402

ECB_SLOT = 0x68FFF14C      # ECB.ExceptionHandler
NCB_SLOT = 0x68FFF44C      # NCB.ExceptionHandler
EMULATOR_ENTRY = 0x6806D440
EXCEPTION_MANAGER = 0xFFCF0F28
H68K_DATA = 0x68FFFB80
H68K_CODE = 0x68FFFC40
DATA_SIZE = 0xC0
RING_N = 256
EVENT_SIZE = 32


def words(b):
    return struct.unpack(">%dI" % (len(b) // 4), b)


def fourcc(v):
    s = struct.pack(">I", v)
    return s.decode("latin-1") if all(32 <= c < 127 for c in s) else "%08X" % v


def collect(read):
    out = {"ecb": words(read(ECB_SLOT, 4))[0],
           "ncb": words(read(NCB_SLOT, 4))[0]}
    hooks = []
    if out["ecb"] == H68K_CODE:
        hooks.append(("68k hook", H68K_DATA))
    if out["ncb"] < 0xFFC00000 and out["ncb"] >= DATA_SIZE:
        hooks.append(("native hook", out["ncb"] - DATA_SIZE))
    out["hooks"] = []
    for name, addr in hooks:
        d = words(read(addr, DATA_SIZE))
        ring = d[0x78 // 4]
        events = words(read(ring, RING_N * EVENT_SIZE)) if ring else ()
        out["hooks"].append((name, addr, d, events))
    return out


def show(name, addr, d, events, newest):
    def f(off):
        return d[off // 4]

    print("%s: data %08X, magic %s, area %08X, original handler %08X" %
          (name, addr, fourcc(f(0)), f(0x14), f(0x18)))
    print("  entered %d, recorded %d, own %d, passed on %d, kernel status %d"
          % (f(0x04), f(0x08), f(0x7C), f(0x50),
             struct.unpack(">i", struct.pack(">I", f(0x1C)))[0]))
    print("  range %08X-%08X, bitmap %08X, current %08X-%08X, ring %08X" %
          (f(0x0C), f(0x10), f(0x68), f(0x70), f(0x74), f(0x78)))
    deny_from = f(0x88)
    print("  deny from %s, denied %d%s" % (
        "nowhere" if deny_from == 0xFFFFFFFF else "%08X" % deny_from, f(0x8C),
        ", emulator frame code %08X" % f(0xA8) if f(0xA8) else ""))
    native = name.startswith("native")
    for label, count, off in (("passed on", 0x50, 0x54),
                              ("denied", 0x8C, 0x90)):
        if f(count):
            p = [f(off + 4 * i) for i in range(5)]
            print("  last %s: pc %08X addr %08X %s %08X zone %08X sp %08X"
                  % (label, p[0], p[1], "r3" if native else "next 68k",
                     p[2] if native else p[2] - 2, p[3], p[4]))
    # Oldest first: ring_idx is the next slot the hook writes. Bit 0 of pc
    # marks a store by the current application's own code.
    idx = f(0x20) % RING_N
    for i in range(RING_N - min(newest, RING_N), RING_N) if events else ():
        e = events[8 * ((idx + i) % RING_N):][:8]
        if e[0]:
            print("  #%d pc %08X addr %08X %s %08X lr %08X zone %08X r4 %08X "
                  "sp %08X%s" % (e[0], e[1] & ~3, e[2], "r3" if native else
                                 "next 68k", e[3] if native else e[3] - 2,
                                 e[4], e[5], e[6], e[7],
                                 " (app code)" if e[1] & 1 else ""))


def main():
    newest = int(sys.argv[1]) if len(sys.argv) > 1 else 64
    out = vdump.paused(collect)
    print("ECB handler %08X (%s), NCB handler %08X (%s)" % (
        out["ecb"], "68k hook" if out["ecb"] == H68K_CODE else
        "emulator" if out["ecb"] == EMULATOR_ENTRY else "other",
        out["ncb"], "Exception Manager" if out["ncb"] == EXCEPTION_MANAGER
        else "native hook" if out["ncb"] < 0xFFC00000 else "other"))
    for hook in out["hooks"]:
        show(*hook, newest)


if __name__ == "__main__":
    main()
