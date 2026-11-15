#!/usr/bin/env python3
"""Read the guest's logical memory from a physical dump.

Usage:
  tools/lmem.py dump ADDR [LENGTH]     print words at logical ADDR
  tools/lmem.py find VALUE [LO HI]     print where a word holds VALUE
  tools/lmem.py save OUT.bin           only save the physical dump

QEMU's memsave reads logical memory through the hashed page table, so a
page whose PTE is not in it (a protected page, or one not used lately)
does not read. With virtual memory off the blue task's logical memory maps
to physical memory in a few linear runs, so this script takes one physical
dump (pmemsave, 512 MB) and reads it through that map. The map below was
measured on the test machine (512 MB) by matching the pages memsave could
read with the physical dump; check it again after changing the RAM size.

Set LMEM_DUMP to a dump saved earlier to read that instead.
"""

import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
QMP = os.path.join(HERE, "qmp.py")
RAM = 0x20000000

# (logical start, logical end, physical - logical)
MAP = [(0x00000000, 0x00BFC000, 0x004000),
       (0x00BFC000, 0x00CB6000, 0x25C000),
       (0x00CB6000, 0x1F000000, 0x35D000)]


def save(path):
    cmd = ('{"execute":"pmemsave","arguments":{"val":0,"size":%d,'
           '"filename":"%s"}}' % (RAM, path))
    subprocess.run([QMP, "raw", cmd], check=True, capture_output=True)


class Memory:
    def __init__(self, path):
        self.p = open(path, "rb").read()

    def word(self, a):
        for lo, hi, off in MAP:
            if lo <= a < hi:
                return struct.unpack(">I", self.p[a + off:a + off + 4])[0]
        raise ValueError("%08X is outside the map" % a)

    def find(self, value, lo, hi):
        pat = struct.pack(">I", value)
        for mlo, mhi, off in MAP:
            a0, a1 = max(lo, mlo), min(hi, mhi)
            if a0 >= a1:
                continue
            d = self.p[a0 + off:a1 + off]
            i = d.find(pat)
            while i >= 0:
                if i % 4 == 0:
                    yield a0 + i
                i = d.find(pat, i + 1)


def row(m, a):
    return "%08X: %s" % (a, " ".join("%08X" % m.word(a + 4 * i)
                                     for i in range(8)))


def main(argv):
    if not argv:
        sys.exit(__doc__)
    if argv[0] == "save" and len(argv) == 2:
        save(argv[1])
        return
    path = os.environ.get("LMEM_DUMP")
    tmp = None
    if not path:
        tmp = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        tmp.close()
        path = tmp.name
        save(path)
    try:
        m = Memory(path)
        if argv[0] == "dump" and len(argv) in (2, 3):
            a = int(argv[1], 0) & ~3
            n = int(argv[2], 0) if len(argv) == 3 else 0x80
            for x in range(a, a + n, 32):
                print(row(m, x))
        elif argv[0] == "find" and len(argv) in (2, 4):
            v = int(argv[1], 0)
            lo, hi = ((int(argv[2], 0), int(argv[3], 0)) if len(argv) == 4
                      else (0x1000, MAP[-1][1]))
            for a in m.find(v, lo, hi):
                print("at %08X, from %s" % (a, row(m, a - 16)))
        else:
            sys.exit(__doc__)
    finally:
        if tmp:
            os.unlink(path)


if __name__ == "__main__":
    main(sys.argv[1:])
