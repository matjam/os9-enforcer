#!/usr/bin/env python3
"""Summarize a storewatch dump: stores into low memory and the system heap,
which globals they hit, and which code made them.

Usage: tools/storewatch/report.py PREFIX [TOP]

Reads PREFIX.sites and PREFIX.words (see storewatch.c). Low memory globals
are named from the LMGet accessors in the 68k Multiversal Interfaces
(toolchain/build/toolchain/m68k-apple-macos/include/Multiverse.h), which
give each global's address.
"""

import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
MULTIVERSE = os.path.join(HERE, "..", "..", "toolchain", "build", "toolchain",
                          "m68k-apple-macos", "include", "Multiverse.h")
LOW_END = 0x2800


def lowmem_names():
    names = {}
    try:
        text = open(MULTIVERSE, encoding="latin-1").read()
    except OSError:
        return names
    for m in re.finditer(r"#define LMGet(\w+)\(\) \(\*\([^)]*\*\)(0x[0-9A-Fa-f]+)\)",
                         text):
        names.setdefault(int(m.group(2), 16), m.group(1))
    return names


def namer(names):
    addrs = sorted(names)

    def name(a):
        best = None
        for x in addrs:
            if x > a:
                break
            best = x
        if best is None or a - best >= 0x10:
            return "?"
        return names[best] + ("" if a == best else "+%X" % (a - best))
    return name


def code_kind(pc, sys_end):
    if pc >= 0xFFC00000:
        return "ROM"
    if pc < sys_end:
        return "system heap"
    if 0x68000000 <= pc < 0x69000000:
        return "emulator/kernel"
    return "other"


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    prefix = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    name = namer(lowmem_names())

    header = {}
    sites = []
    for line in open(prefix + ".sites"):
        if line.startswith("# insns") or line.startswith("# stores"):
            f = line[2:].split()
            header = {f[i]: f[i + 1] for i in range(0, len(f) - 1, 2)}
            continue
        if line.startswith("#"):
            continue
        f = line.split()
        sites.append({
            "region": f[0], "kind": f[1], "pc": int(f[2], 16),
            "total": int(f[3]), "app": int(f[4]), "first": int(f[5], 16),
            "last": int(f[6], 16), "zone": int(f[7], 16),
            "epc": int(f[8], 16) if len(f) > 8 else 0})
    words = []
    for line in open(prefix + ".words"):
        if line.startswith("#"):
            continue
        a, t, p = line.split()
        words.append((int(a, 16), int(t), int(p)))

    sys_end = int(header.get("sys_end", "0"), 16)
    insns = int(header.get("insns", "0"))
    low, sysh = int(header["low"]), int(header["sys"])
    low_app, sys_app = int(header["low_app"]), int(header["sys_app"])
    per = (lambda n: "%.0f per million instructions" % (n * 1e6 / insns)) \
        if insns else (lambda n: "")
    print("%s: %s instructions, %s stores" % (prefix, header.get("insns", "?"),
                                              header["stores"]))
    print("  low memory:  %9d stores, %s; by app code %d" %
          (low, per(low), low_app))
    print("  system heap: %9d stores, %s; by app code %d (heap ends %08X)" %
          (sysh, per(sysh), sys_app, sys_end))
    print("  with translation off: %s" % header["real_mode"])

    print("\nlow memory words, most stores first:")
    for a, t, p in sorted(words, key=lambda w: -w[1])[:top]:
        print("  %04X %-22s %9d%s" % (a, name(a), t,
                                     "  (app code %d)" % p if p else ""))
    # Mac OS uses vectors 0-63; system globals start at 0100 (MonkeyLives).
    vec = [(a, t, p) for a, t, p in words if a < 0x100]
    print("\n68k exception vectors (0000-00FF): %s" %
          (", ".join("%04X x%d" % (a, t) for a, t, _ in vec) or "no stores"))
    app_words = [(a, t, p) for a, t, p in words if p]
    print("\nlow memory words written by app code:")
    for a, t, p in app_words:
        print("  %04X %-22s app %d of %d" % (a, name(a), p, t))
    if not app_words:
        print("  none")

    for region, label in (("low", "low memory"), ("sys", "system heap")):
        rs = [s for s in sites if s["region"] == region]
        print("\n%s, code that stores most (68k: the instruction after the "
              "store):" % label)
        for s in sorted(rs, key=lambda s: -s["total"])[:top]:
            extra = name(s["first"]) if region == "low" else "%08X" % s["first"]
            print("  %-6s %08X %-15s %9d  e.g. %s" % (
                s["kind"], s["pc"], code_kind(s["pc"], sys_end), s["total"],
                extra))
        apps = [s for s in rs if s["app"]]
        print("%s, app code (%d sites):" % (label, len(apps)))
        for s in sorted(apps, key=lambda s: -s["app"])[:top]:
            print("  %-6s %08X app %7d of %7d  %s-%s  zone %08X%s" % (
                s["kind"], s["pc"], s["app"], s["total"],
                "%08X" % s["first"], "%08X" % s["last"], s["zone"],
                "  emulator %08X" % s["epc"] if s["epc"] else ""))
        by_target = defaultdict(int)
        by_epc = defaultdict(int)
        for s in apps:
            by_target["%08X" % (s["first"] & ~3)] += s["app"]
            if s["epc"]:
                by_epc["%08X" % s["epc"]] += s["app"]
        if apps:
            print("  by first address: " + ", ".join(
                "%s x%d" % kv for kv in sorted(by_target.items(),
                                               key=lambda kv: -kv[1])[:8]))
        if by_epc:
            print("  68k stores by emulator instruction: " + ", ".join(
                "%s x%d" % kv for kv in sorted(by_epc.items(),
                                               key=lambda kv: -kv[1])[:8]))


if __name__ == "__main__":
    main()
