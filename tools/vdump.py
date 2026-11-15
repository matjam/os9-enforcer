#!/usr/bin/env python3
"""Save ranges of the guest's logical memory to files, over QMP, from one
consistent moment.

Usage: tools/vdump.py ADDR LENGTH OUT.bin [ADDR LENGTH OUT.bin ...]

The monitor's memsave uses the CPU's current address translation, so a read
taken while the CPU is in the NanoKernel (translation off) or another
context fails or returns other memory. This pauses the guest, and if the
CPU has translation on (MSR IR and DR set) and lowmem ApplZone (0x2AA)
reads as not zero, saves every range before resuming it. Otherwise it
resumes and tries again.

User mode (MSR PR) is not required: with virtual memory off, an idle guest
spends most of its time in a supervisor-mode power-saving loop that uses
the blue task's translation, and rarely in user mode.
"""

import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
QMP = os.path.join(HERE, "qmp.py")
MSR_IR, MSR_DR = 0x20, 0x10


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


class NotInContext(Exception):
    """A read failed: the CPU left the blue task's address space."""


def paused(fn, tries=100):
    """Return fn(read), called while the guest is paused in the blue task's
    address space. read(addr, length) returns the bytes there; fn can follow
    pointers through several reads from the same moment. Retries with a new
    pause until the CPU is in that context; always resumes the guest."""
    with tempfile.TemporaryDirectory() as tmp:
        n = [0]

        def read(addr, length):
            n[0] += 1
            data = memsave(addr, length, os.path.join(tmp, "r%d.bin" % n[0]))
            if data is None:
                raise NotInContext
            return data

        want = MSR_IR | MSR_DR
        for _ in range(tries):
            hmp("stop")
            try:
                m = re.search(r"MSR ([0-9a-fA-F]+)", hmp("info registers"))
                msr = int(m.group(1), 16) if m else 0
                if msr & want != want or read(0x2AA, 4) == b"\0\0\0\0":
                    continue
                return fn(read)
            except NotInContext:
                continue
            finally:
                hmp("cont")
    sys.exit("the CPU was never in the blue task's address space")


def main():
    args = sys.argv[1:]
    if not args or len(args) % 3:
        sys.exit(__doc__)
    ranges = [(int(args[i], 0), int(args[i + 1], 0), args[i + 2])
              for i in range(0, len(args), 3)]
    data = paused(lambda read: [read(a, n) for a, n, _ in ranges])
    for (addr, length, path), d in zip(ranges, data):
        open(path, "wb").write(d)
        print("saved %d bytes from %08X to %s" % (length, addr, path))


if __name__ == "__main__":
    main()
