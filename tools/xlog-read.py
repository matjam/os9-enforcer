#!/usr/bin/env python3
"""Print the protection engine's state from the running guest (XLog, Guard,
SysLog, SysGuard or the Enforcer Extension): mode, counters, the system
heap's page watch, domains, cross-domain writers and stores by an
application's own code.

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

# vdump.py is in the test machine's tools: OS9_PROTECT, default
# ../os9-protect next to this repository.
sys.path.insert(0, os.path.join(os.environ.get("OS9_PROTECT", os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "os9-protect")),
    "tools"))
import vdump  # noqa: E402

OWNER = 0x68FFFBEC
HEADER = ["magic", "self", "off_counters", "n_counters", "off_domains",
          "n_domains", "domain_size", "off_writers", "max_writers",
          "writer_size", "off_cur", "off_offenses", "max_offenses",
          "offense_size", "off_mode", "off_stopper", "off_sys", "off_kernel"]
# State from off_kernel, then the kernel's control block (common/kcb.h).
KERNEL = ["kcb", "kcb_err", "owners", "kcb_raw", "owners_raw", "kseen",
          "kevents", "klost", "kdenied"]
KCB = ["magic", "version", "mode", "current", "entries", "denied_page",
       "seq", "frame", "allowed", "logged", "denied", "bail", "self"]
KCB_RING, KCB_RING_N, KCB_EVENTS = 0x200, 496, 12
EVF = [(1, "68k"), (2, "task"), (4, "offense"), (8, "denied")]
STOPPER = ["magic", "old_vector", "zone"] + ["stops_" + s for s in
                                             ["native", "68k", "fallback"]]
# State from off_sys, then the SysPages header and its SysCounters.
SYS_STATE = ["sys_dom", "sys_on", "want_sys", "deny_from", "sys"]
SYS_PAGES = (["magic", "cap", "lo", "n", "tick", "n_hot"] +
             ["traps", "app_traps", "hot", "hot_full", "cooled", "reheated",
              "excluded", "not_rw", "stacks", "resweeps", "grown", "shrunk"])
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
            return a, None, None, None, None, None
        n = len(HEADER)
        h = dict(zip(HEADER, struct.unpack(">%dI" % n, read(a, 4 * n))))
        if h["magic"] != struct.unpack(">I", b"ENF4")[0] or h["self"] != a:
            return a, h, None, None, None, None
        size = max(h["off_writers"] + h["max_writers"] * h["writer_size"],
                   h["off_offenses"] + 4 +
                   h["max_offenses"] * h["offense_size"],
                   h["off_stopper"] + 4, h["off_sys"] + 4 * len(SYS_STATE),
                   h["off_kernel"] + 4 * len(KERNEL))
        d = read(a, size)
        o = h["off_kernel"]
        kernel = dict(zip(KERNEL, struct.unpack(
            ">%dI" % len(KERNEL), d[o:o + 4 * len(KERNEL)])))
        if kernel["kcb"]:
            k = kernel["kcb"]
            kernel.update(("cb_" + n, v) for n, v in zip(KCB, struct.unpack(
                ">%dI" % len(KCB), read(k, 4 * len(KCB)))))
            last = kernel["cb_seq"]
            events = []
            for seq in range(max(1, last - KCB_EVENTS + 1), last + 1):
                events.append(struct.unpack(">8I", read(
                    k + KCB_RING + 32 * ((seq - 1) % KCB_RING_N), 32)))
            kernel["events"] = events
        o = h["off_stopper"]
        st = struct.unpack(">I", d[o:o + 4])[0]
        stopper = None
        if st:
            stopper = dict(zip(STOPPER, struct.unpack(
                ">%dI" % len(STOPPER), read(st, 4 * len(STOPPER)))))
            stopper["addr"] = st
        o = h["off_sys"]
        sys_state = dict(zip(SYS_STATE, struct.unpack(
            ">%dI" % len(SYS_STATE), d[o:o + 4 * len(SYS_STATE)])))
        if sys_state["sys"]:
            sys_state.update(("p_" + k, v) for k, v in zip(SYS_PAGES, struct.unpack(
                ">%dI" % len(SYS_PAGES),
                read(sys_state["sys"], 4 * len(SYS_PAGES)))))
        return a, h, d, stopper, sys_state, kernel
    return fn


def cstr(b):
    return b.split(b"\0", 1)[0].decode("mac_roman")


def print_kernel(k):
    """The kernel's owner check: its counts and its last events."""
    if not k["kcb"]:
        err = struct.unpack(">i", struct.pack(">I", k["kcb_err"]))[0]
        print("kernel owner check: not registered (%d%s)" % (
            err, ": no patch 4 in the kernel" if err == -4 else ""))
        return
    mode = k["cb_mode"]
    print("kernel owner check at %08X: mode %s, current owner %d, table "
          "%08X (%d pages), frame %08X" % (
              k["kcb"], ["pass", "log", "enforce"][mode] if mode < 3 else mode,
              k["cb_current"], k["owners"], k["cb_entries"], k["cb_frame"]))
    print("  kernel: allowed %d, offenses allowed %d, denied %d, left to the "
          "hooks %d, own (current application) %d; events %d" % (
              k["cb_allowed"], k["cb_logged"], k["cb_denied"], k["cb_bail"],
              k["cb_self"], k["cb_seq"]))
    print("  engine: taken %d (last %d), lost %d, denied %d" % (
        k["kevents"], k["kseen"], k["klost"], k["kdenied"]))
    if k["events"]:
        print("  last events (owner/code/current: 1 system heap, 3+ domain "
              "slot + 3):")
    for seq, pc, addr, r24, lr, sp, info, task in k["events"]:
        flags = ",".join(n for b, n in EVF if info & b) or "-"
        print("  #%d %08X by %08X (r24 %08X lr %08X sp %08X) owner %d code "
              "%d current %d %s task %08X" % (
                  seq, addr, pc, r24, lr, sp, info >> 24, (info >> 16) & 0xFF,
                  (info >> 8) & 0xFF, flags, task))


def main():
    addr = int(sys.argv[1], 0) if len(sys.argv) > 1 else None
    addr, h, d, stopper, sy, kernel = vdump.paused(collect(addr))
    if not addr:
        sys.exit("no engine is running (no owner in the 68k hook)")
    if d is None:
        sys.exit("no engine state (ENF4) at %08X" % addr)
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
    if stopper:
        print("clean stop at %08X (stub %08X, bus error vector was %08X): "
              "zone %08X, stops native %d, 68k %d" % (
                  stopper["addr"], stopper["addr"] + 4 * len(STOPPER),
                  stopper["old_vector"], stopper["zone"],
                  stopper["stops_native"], stopper["stops_68k"]))
    else:
        print("clean stop not installed")
    deny = sy["deny_from"]
    print("deny from %s" % ("nowhere" if deny == 0xFFFFFFFF else
                            "%08X" % deny))
    print_kernel(kernel)
    if sy["sys"]:
        ticks = sy["p_tick"]
        print("system heap %s (domain %d%s): %08X-%08X, %d pages of %d; "
              "%d ticks" % (
                  "watched" if sy["sys_on"] else "not watched",
                  struct.unpack(">i", struct.pack(">I", sy["sys_dom"]))[0],
                  "" if sy["want_sys"] == sy["sys_on"] else ", change requested",
                  sy["p_lo"], sy["p_lo"] + sy["p_n"] * 4096, sy["p_n"],
                  sy["p_cap"], ticks))
        print("  traps %d (%.1f a tick), by app code %d; hot now %d, made "
              "hot %d, cooled %d, reheated %d, hot list full %d" % (
                  sy["p_traps"], sy["p_traps"] / ticks if ticks else 0,
                  sy["p_app_traps"], sy["p_n_hot"], sy["p_hot"],
                  sy["p_cooled"], sy["p_reheated"], sy["p_hot_full"]))
        print("  never protected: engine's own %d, not read/write %d, "
              "stacks %d; resweeps %d, grown %d, shrunk %d" % (
                  sy["p_excluded"], sy["p_not_rw"], sy["p_stacks"],
                  sy["p_resweeps"], sy["p_grown"], sy["p_shrunk"]))
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
