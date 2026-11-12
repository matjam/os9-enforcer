#!/usr/bin/env python3
"""Dump the emulator data page (logical 0x68FFF000) repeatedly from the
running guest and report which words were ever nonzero, and whether the
space the 68k write hook uses (page offsets 0xB80-0xEFF) stayed zero.

Usage: tools/edp-scan.py [COUNT [GAP_SECONDS]]    (default 40 dumps, 0.5 s)

Use the guest while it runs, so the dumps cover real activity. Run it
before installing the hook: once installed, the hook fills that space.
"""

import os
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
QMP = os.path.join(HERE, "qmp.py")

PAGE = 0x68FFF000
SLOT = 0x14C
GOOD_SLOT = (0x6806D440, 0x68FFFD00)
LO, HI = 0xB80, 0xF00


def dump(path):
    for _ in range(40):
        if os.path.exists(path):
            os.remove(path)
        subprocess.run([QMP, "hmp", 'memsave 0x%x 4096 "%s"' % (PAGE, path)],
                       capture_output=True)
        try:
            data = open(path, "rb").read()
        except OSError:
            continue
        if len(data) == 4096 and struct.unpack(">I", data[SLOT:SLOT + 4])[0] in GOOD_SLOT:
            return data
    return None


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 40
    gap = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
    ever = set()
    good = 0
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "edp.bin")
        for i in range(count):
            data = dump(path)
            if data is None:
                print("dump %d: no good read" % i)
                continue
            good += 1
            for off in range(0, 4096, 4):
                if struct.unpack(">I", data[off:off + 4])[0]:
                    ever.add(off)
            time.sleep(gap)

    print("good dumps:", good)
    runs = []
    for off in sorted(ever):
        if runs and runs[-1][1] == off - 4:
            runs[-1][1] = off
        else:
            runs.append([off, off])
    print("ever nonzero:", ", ".join("%03X-%03X" % (a, b + 3) for a, b in runs))
    inside = sorted(o for o in ever if LO <= o < HI)
    print("nonzero in %03X-%03X:" % (LO, HI - 1),
          ", ".join("%03X" % o for o in inside) or "none")


if __name__ == "__main__":
    main()
