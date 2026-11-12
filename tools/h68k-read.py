#!/usr/bin/env python3
"""Print the 68k write hook's data block (common/h68k.s) from the running
guest, over QMP.

Usage: tools/h68k-read.py

Reads the emulator data page (logical 0x68FFF000) with the monitor's
memsave. A logical read only works while the CPU is in a context that maps
the page, so it retries until the emulator context block's handler word
reads as either the emulator's entry or the hook.
"""

import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
QMP = os.path.join(HERE, "qmp.py")

PAGE = 0x68FFF000
SLOT = 0x14C          # ECB.ExceptionHandler, page offset
DATA = 0xB80          # hook data, page offset
CODE = 0xD00          # hook code, page offset
EMULATOR_ENTRY = 0x6806D440
HOOK_ENTRY = PAGE + CODE


def read_page(path):
    for _ in range(60):
        if os.path.exists(path):
            os.remove(path)
        subprocess.run([QMP, "hmp", 'memsave 0x%x 4096 "%s"' % (PAGE, path)],
                       capture_output=True)
        try:
            page = open(path, "rb").read()
        except OSError:
            continue
        if len(page) == 4096 and struct.unpack(">I", page[SLOT:SLOT + 4])[0] in (
                EMULATOR_ENTRY, HOOK_ENTRY):
            return page
    sys.exit("could not read the emulator data page")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        page = read_page(os.path.join(tmp, "edp.bin"))

    def w(off):
        return struct.unpack(">I", page[off:off + 4])[0]

    print("ECB handler %08X (%s)" % (w(SLOT), "hook" if w(SLOT) == HOOK_ENTRY
                                     else "emulator"))
    names = ["magic", "count_all", "count_handled", "lo", "hi", "area", "orig",
             "kstatus", "ring_idx"]
    print(", ".join("%s %08X" % (n, w(DATA + 4 * i)) for i, n in enumerate(names)))
    print("count_passed %08X" % w(DATA + 0x50))
    print("spill CR r0 r5 r6 r7:",
          " ".join("%08X" % w(DATA + 0x24 + 4 * i) for i in range(5)))
    print("saved ECB 40 44 84 8C 94 9C:",
          " ".join("%08X" % w(DATA + 0x38 + 4 * i) for i in range(6)))
    idx = w(DATA + 0x20) & 15
    for i in range(16):
        e = DATA + 0x80 + 16 * ((idx + i) & 15)
        if w(e + 4):
            print("  emu pc %08X addr %08X r24 %08X lr %08X"
                  % (w(e), w(e + 4), w(e + 8), w(e + 12)))
    words = [w(off) for off in range(CODE, 0xF00, 4)]
    used = [i for i, x in enumerate(words) if x]
    if used:
        print("code: %d bytes, last word %08X" % (4 * (used[-1] + 1),
                                                 words[used[-1]]))
    else:
        print("code: none")


if __name__ == "__main__":
    main()
