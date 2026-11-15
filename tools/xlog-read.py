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

# vdump.py is next to this script.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vdump  # noqa: E402

OWNER = 0x68FFFBEC
HEADER = ["magic", "self", "off_counters", "n_counters", "off_domains",
          "n_domains", "domain_size", "off_writers", "max_writers",
          "writer_size", "off_cur", "off_offenses", "max_offenses",
          "offense_size", "off_mode", "off_stopper", "off_sys", "off_kernel"]
# State from off_kernel, then the kernel's control block (common/kcb.h).
KERNEL = ["kcb", "kcb_err", "owners", "kcb_raw", "owners_raw", "kseen",
          "kevents", "klost", "kdenied"]
# After those: the temporary memory pools (PoolStubs pointer, PoolCounters,
# vm, pm_zone), then the Chunk table.
POOL = ["pool", "news", "passed", "disposed", "chunks_made", "chunks_freed",
        "chunks_dropped", "quits", "gone", "reinstalled", "launched",
        "system", "over_handles", "over_ptrs", "over_none", "vm", "pm_zone",
        "owners_size"]
POOL_STUBS = ["magic", "enabled_inside", "osd_old", "dh_old", "ets_old"]
CHUNK = ["live", "domain", "handle", "block", "lo", "hi", "zone", "handles",
         "keep"]
MAX_CHUNKS = 128
# After the Chunk table: skips, application VSIDs and the Process Manager
# hook.
SPACES = ["skips", "spaces", "vsid_mask", "spaces_on", "pm_site", "pm_stub",
          "pm_mapped", "pm_tried", "pm_tries", "yield", "preempt_on",
          "slice_ms", "size_site", "size_stub", "size_mapped", "size_cap",
          "size_raised", "size_from", "size_to", "mem_on", "want_mem",
          "sparse_want_lo", "sparse_want_hi", "sparse_lo", "sparse_hi",
          "sparse_area", "sparse_used", "sparse_chunks", "sparse_short",
          "pool_grown", "pool_shrunk", "part", "part_made", "part_freed",
          "md_calls"]
# The yield stub's data (common/hyield.h).
YIELD = (["magic", "entry", "vecbr", "check", "wne", "entered", "yielded",
          "lost", "starting", "not_task", "in_call", "other_stack",
          "low_stack", "broken", "safe", "last_sp", "last_pc", "last_lr",
          "last_owner", "last_base", "last_n"] +
         ["chain%d" % i for i in range(11)])
KCB = ["magic", "version", "mode", "current", "entries", "denied_page",
       "seq", "frame", "allowed", "logged", "denied", "bail", "self",
       "skipped", "applzone", "deny_native_page", "deny_native_seq",
       "deny_68k_page", "deny_68k_seq", "tables", "spaces", "rebuilt",
       "vswitch", "lazy", "lazy_pc", "slice", "next", "stub", "yield_pc",
       "preempts", "zone"]
KCB_RING, KCB_RING_N, KCB_EVENTS = 0x300, 488, 12
EVF = [(1, "68k"), (2, "task"), (4, "offense"), (8, "denied"),
       (16, "skipped")]
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
             "info_errors"] + ["app_code_" + s for s in SOURCES] +
            ["switch_pages", "switch_tb", "switch_tb_max", "tb_per_ms",
             "pm_switches"])
DOMAIN_SIZE, WRITER_SIZE, OFFENSE_SIZE = 84, 64, 56
CODE_STATES = ["not looked for", "in the partition", "owned", "failed"]
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
                   h["off_kernel"] + 4 * (len(KERNEL) + len(POOL)) +
                   MAX_CHUNKS * 4 * len(CHUNK) + 4 * len(SPACES))
        d = read(a, size)
        o = h["off_kernel"]
        kernel = dict(zip(KERNEL, struct.unpack(
            ">%dI" % len(KERNEL), d[o:o + 4 * len(KERNEL)])))
        o += 4 * len(KERNEL)
        kernel.update(zip(POOL, struct.unpack(">%dI" % len(POOL),
                                              d[o:o + 4 * len(POOL)])))
        o += 4 * len(POOL)
        kernel["chunks"] = []
        for i in range(MAX_CHUNKS):
            c = dict(zip(CHUNK, struct.unpack(">%dI" % len(CHUNK), d[
                o + 4 * len(CHUNK) * i:o + 4 * len(CHUNK) * (i + 1)])))
            if c["live"]:
                c["slot"] = i
                kernel["chunks"].append(c)
        o += MAX_CHUNKS * 4 * len(CHUNK)
        kernel.update(zip(SPACES, struct.unpack(">%dI" % len(SPACES),
                                                d[o:o + 4 * len(SPACES)])))
        if kernel["yield"]:
            kernel.update(("y_" + n, v) for n, v in zip(
                YIELD, struct.unpack(">%dI" % len(YIELD),
                                     read(kernel["yield"], 4 * len(YIELD)))))
        if kernel["pool"]:
            kernel.update(("stub_" + n, v) for n, v in zip(
                POOL_STUBS, struct.unpack(">%dI" % len(POOL_STUBS),
                                          read(kernel["pool"],
                                               4 * len(POOL_STUBS)))))
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
    print("  kernel: allowed %d, offenses allowed %d, denied %d, dropped %d, "
          "left to the hooks %d, own (current application) %d, into the "
          "tables (dropped) %d; events %d" % (
              k["cb_allowed"], k["cb_logged"], k["cb_denied"],
              k["cb_skipped"], k["cb_bail"], k["cb_self"], k["cb_tables"],
              k["cb_seq"]))
    print("  engine: taken %d (last %d), lost %d, denied %d" % (
        k["kevents"], k["kseen"], k["klost"], k["kdenied"]))
    print("  application VSIDs: %s (kernel %d), segments %02X; switches %d "
          "(%d made at a store, the last at %08X), PTEs built again %d; "
          "Process Manager hook %s" % (
              "on" if k["spaces_on"] else
              "possible" if k["spaces"] else "no", k["cb_spaces"],
              k["vsid_mask"], k["cb_vswitch"], k["cb_lazy"], k["cb_lazy_pc"],
              k["cb_rebuilt"],
              "at %08X%s, stub %08X" % (
                  k["pm_site"], " (file-mapped)" if k["pm_mapped"] else "",
                  k["pm_stub"]) if k["pm_site"] else
              "not installed (%d searches, the last from %08X)" % (
                  k["pm_tries"], k["pm_tried"])))
    print("  memory sizes %s; launch sizes: hook %s; cap %d KB; raised %d, "
          "the last from %d KB to %d KB" % (
              "on" if k["mem_on"] else "off",
              "at %08X%s" % (k["size_site"],
                             " (file-mapped)" if k["size_mapped"] else "")
              if k["size_site"] else "not installed",
              k["size_cap"] >> 10, k["size_raised"], k["size_from"] >> 10,
              k["size_to"] >> 10))
    if k["sparse_area"]:
        print("  sparse memory: %08X-%08X (area %08X), pieces in use %08X; "
              "chunks made %d, requests refused %d; pool blocks added %d, "
              "freed %d" % (
                  k["sparse_lo"], k["sparse_hi"], k["sparse_area"],
                  k["sparse_used"], k["sparse_chunks"], k["sparse_short"],
                  k["pool_grown"], k["pool_shrunk"]))
        print("    sparse partitions: hooks %s; made %d, given back %d; "
              "MemoryDispatch calls for the range %d" % (
                  "at %08X" % k["part"] if k["part"] else "not installed",
                  k["part_made"], k["part_freed"], k["md_calls"]))
    else:
        why = k["sparse_short"]
        print("  sparse memory: none (planned %08X-%08X%s)" % (
            k["sparse_want_lo"], k["sparse_want_hi"],
            "; no system heap" if why == 1 else
            "; not locked" if why == 2 else
            "; MPCreateArea error %d" % (why - (1 << 32)) if why else ""))
    if k["yield"]:
        print("  preemption: %s, slice %d ms (kernel %d ticks), stub %08X; "
              "redirects %d (pending %08X); stub entries %d, WaitNextEvent "
              "calls %d, lost %d" % (
                  "on" if k["preempt_on"] else "off", k["slice_ms"],
                  k["cb_slice"], k["yield"] + k["y_entry"],
                  k["cb_preempts"], k["cb_yield_pc"], k["y_entered"],
                  k["y_yielded"], k["y_lost"]))
        n = k["y_last_n"]
        print("    checks: starting %d, not task %d, in a call %d, other "
              "stack %d, low stack %d, chain not ending at the base %d, safe "
              "%d; the last: sp %08X pc %08X lr %08X owner %d stack base "
              "%08X, %d frames: %s%s" % (
                  k["y_starting"], k["y_not_task"], k["y_in_call"],
                  k["y_other_stack"], k["y_low_stack"], k["y_broken"],
                  k["y_safe"], k["y_last_sp"], k["y_last_pc"],
                  k["y_last_lr"], k["y_last_owner"], k["y_last_base"], n,
                  " ".join("%08X" % k["y_chain%d" % i]
                           for i in range(min(11, n))),
                  ", ended at %08X" % k["y_chain%d" % n] if n < 11 else ""))
    else:
        print("  preemption: no yield stub")
    if k["events"]:
        print("  last events (owner/code/current: 1 system heap, 3+ domain "
              "slot + 3):")
    for seq, pc, addr, r24, lr, sp, info, task in k["events"]:
        flags = ",".join(n for b, n in EVF if info & b) or "-"
        print("  #%d %08X by %08X (r24 %08X lr %08X sp %08X) owner %d code "
              "%d current %d %s task %08X" % (
                  seq, addr, pc, r24, lr, sp, info >> 24, (info >> 16) & 0xFF,
                  (info >> 8) & 0xFF, flags, task))


def print_pool(k):
    """The temporary memory pools: stubs, counts and chunks."""
    print("virtual memory %s, Process Manager zone %08X" % (
        "on" if k["vm"] else "off", k["pm_zone"]))
    if not k["pool"]:
        print("temporary memory pools: not installed")
    else:
        print("temporary memory pools at %08X: enabled %d, inside %d; "
              "traps were _OSDispatch %08X, DisposeHandle %08X, ExitToShell "
              "%08X" % (k["pool"], k["stub_enabled_inside"] >> 16,
                        k["stub_enabled_inside"] & 0xFFFF, k["stub_osd_old"],
                        k["stub_dh_old"], k["stub_ets_old"]))
    print("  taken %d, passed on %d, passed on in system mode %d, disposed "
          "%d; chunks made %d, freed %d, dropped %d; quits %d, gone %d; "
          "stubs put back %d; added at an event call %d" % (
              k["news"], k["passed"], k["system"], k["disposed"],
              k["chunks_made"], k["chunks_freed"], k["chunks_dropped"],
              k["quits"], k["gone"], k["reinstalled"], k["launched"]))
    print("  overflow: failed NewHandle calls served %d, NewPtr %d, not "
          "served %d" % (k["over_handles"], k["over_ptrs"], k["over_none"]))
    for c in k["chunks"]:
        print("  chunk [%3d] domain %d: %08X-%08X zone %08X, handle %08X -> "
              "%08X, %d handles%s" % (
                  c["slot"], c["domain"], c["lo"], c["hi"], c["zone"],
                  c["handle"], c["block"], c["handles"],
                  ", kept" if c["keep"] else ""))


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
    if c["switches"] and c["tb_per_ms"]:
        us = 1000.0 / c["tb_per_ms"]
        print("switch: %.0f us and %.0f pages protected a switch, %.2f us a "
              "page, longest %.0f us" % (
                  c["switch_tb"] * us / c["switches"],
                  c["switch_pages"] / c["switches"],
                  c["switch_tb"] * us / max(c["switch_pages"], 1),
                  c["switch_tb_max"] * us))
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
    print_pool(kernel)
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
        if w(o) or w(o + 64):
            print("  [%2d] %-15s %08X-%08X partition %08X+%08X (master "
                  "pointer %08X) dying %d%s own %d written by others %d" % (
                      i, cstr(d[o + 36:o + 52]), w(o + 12), w(o + 16),
                      w(o + 20), w(o + 24), w(o + 60), w(o + 4),
                      ", quit" if w(o + 64) else "", w(o + 52), w(o + 56)))
            st = w(o + 76)
            what = (CODE_STATES[st] if st < 3 else
                    "failed at step %d" % (st - 3))
            if st == 2:
                what += " %08X-%08X" % (w(o + 68), w(o + 72))
            print("       native code outside the partition: %s" % what)
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
        target = cstr(d[e + 40:e + 56])
        # The system heap is log only: a store into it is never denied.
        if w(e + 20) == 2:
            what = "blocked (store dropped)"
        elif w(e + 20):
            what = "denied"
        elif target == "System heap":
            what = "logged"
        else:
            what = "would deny"
        print("  #%d %02d:%02d:%02d %s: %s, %s code at %08X, into %s at %08X"
              % (w(e), t // 3600, t // 60 % 60, t % 60, what,
                 cstr(d[e + 24:e + 40]), SOURCES[w(e + 16) & SRC_MASK],
                 w(e + 8), target, w(e + 12)))


if __name__ == "__main__":
    main()
