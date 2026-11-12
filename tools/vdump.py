#!/usr/bin/env python3
"""Save ranges of the guest's logical memory to files, over QMP, from one
consistent moment.

Usage: tools/vdump.py ADDR LENGTH OUT.bin [ADDR LENGTH OUT.bin ...]

The monitor's memsave uses the CPU's current address translation, so a read
taken while the CPU is in the NanoKernel (translation off) or another
context fails or returns other memory. This pauses the guest, and if the
CPU is in user mode with translation on (MSR PR, IR and DR set) and lowmem
ApplZone (0x2AA) is not zero, saves every range before resuming it.
Otherwise it resumes and tries again.
"""

import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
QMP = os.path.join(HERE, "qmp.py")
MSR_PR, MSR_IR, MSR_DR = 0x4000, 0x20, 0x10


def hmp(cmd):
    return subprocess.run([QMP, "hmp", cmd], capture_output=True,
                          text=True).stdout


def memsave(addr, length, path):
    if os.path.exists(path):
        os.remove(path)
    hmp('memsave 0x%x %d "%s"' % (addr, length, path))
    try:
        data = open(path, "rb").read()
    except OSError:
        return None
    return data if len(data) == length else None


def snapshot(ranges, tmp):
    """Pause, and if the CPU is in the blue task's user context, read every
    range. Always resumes."""
    hmp("stop")
    try:
        m = re.search(r"MSR ([0-9a-fA-F]+)", hmp("info registers"))
        msr = int(m.group(1), 16) if m else 0
        want = MSR_PR | MSR_IR | MSR_DR
        if msr & want != want:
            return None
        zone = memsave(0x2AA, 4, os.path.join(tmp, "zone.bin"))
        if zone is None or zone == b"\0\0\0\0":
            return None
        out = []
        for i, (addr, length, _) in enumerate(ranges):
            data = memsave(addr, length, os.path.join(tmp, "r%d.bin" % i))
            if data is None:
                return None
            out.append(data)
        return out
    finally:
        hmp("cont")


def main():
    args = sys.argv[1:]
    if not args or len(args) % 3:
        sys.exit(__doc__)
    ranges = [(int(args[i], 0), int(args[i + 1], 0), args[i + 2])
              for i in range(0, len(args), 3)]
    with tempfile.TemporaryDirectory() as tmp:
        for _ in range(100):
            data = snapshot(ranges, tmp)
            if data is None:
                continue
            for (addr, length, path), d in zip(ranges, data):
                open(path, "wb").write(d)
                print("saved %d bytes from %08X to %s" % (length, addr, path))
            return
    sys.exit("the CPU was never in the blue task's user context")


if __name__ == "__main__":
    main()
