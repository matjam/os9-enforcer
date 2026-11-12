#!/usr/bin/env python3
"""Compare the NanoKernel's count of write faults with the write hooks'.

Usage: tools/faults-read.py

The NanoKernel counts every exception by cause in NanoKernelInfo
(logical 0x68FFEDC0, one word per cause; cause 21 is a write to a
write-protected page), for every task. The write hooks only see faults in
the blue task (Mac OS itself, including its interrupt-level code). A fault
in any other task, such as a Multiprocessing Services task, stops that task
without reaching a hook, so the kernel's count is then higher than the
hooks' total. Run it while the hooks are installed. The kernel counts since
boot, and the hooks' counts start at zero each time they are installed, so
faults taken before then (for example under an earlier Guard or XLog run)
show as a constant difference; a difference that grows means faults in
another task.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vdump  # noqa: E402

NK_INFO = 0x68FFEDC0       # NanoKernelInfo: ExceptionCauseCounts[32] first
CAUSE_WRITE = 21           # ecDataWriteViolation
H68K_DATA = 0x68FFFB80
NCB_SLOT = 0x68FFF44C
DATA_SIZE = 0xC0


def collect(read):
    causes = struct.unpack(">32I", read(NK_INFO, 128))
    h68k = struct.unpack(">2I", read(H68K_DATA, 8))[1]
    ncb = struct.unpack(">I", read(NCB_SLOT, 4))[0]
    hnat = None
    if DATA_SIZE <= ncb < 0xFFC00000:
        hnat = struct.unpack(">2I", read(ncb - DATA_SIZE, 8))[1]
    return causes, h68k, hnat


def main():
    causes, h68k, hnat = vdump.paused(collect)
    print("kernel exception counts by cause:",
          ", ".join("%d: %d" % (i, c) for i, c in enumerate(causes) if c))
    hooks = h68k + (hnat or 0)
    print("write faults: kernel %d, hooks %d (68k %d, native %s)" % (
        causes[CAUSE_WRITE], hooks, h68k,
        "not installed" if hnat is None else hnat))
    missed = causes[CAUSE_WRITE] - hooks
    if missed > 0:
        print("%d write faults did not reach a hook (another task, or "
              "taken before the hooks were installed)" % missed)


if __name__ == "__main__":
    main()
