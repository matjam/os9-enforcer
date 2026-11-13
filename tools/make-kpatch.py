#!/usr/bin/env python3
"""Write installer/kpatch.h: the kernel patch, as the bytes that differ
between the stock NanoKernel and ours.

Usage: tools/make-kpatch.py

Reads kernel/stock/NanoKernel-v02.28.bin (kernel/extract.sh) and
kernel/build/NanoKernel-v02.28.bin (kernel/build.sh). The header holds
only the changed bytes, which are all ours: our code past the end of the
stock kernel, where the ROM image has zeros, and the instructions and table
entries we replaced. It also holds the CRC-32 of the stock kernel and of
ours, so the installer can tell a stock ROM image from a patched one.
"""

import os
import zlib

KERNEL = "NanoKernel-v02.28.bin"
GAP = 8  # join runs of changes closer than this (keeps the header short)


def runs(old, new):
    """(offset, length) of each run of bytes where new differs from old
    (old reads as zeros past its end)."""
    old = old + bytes(len(new) - len(old))
    out = []
    i = 0
    while i < len(new):
        if old[i] == new[i]:
            i += 1
            continue
        j = i
        while j < len(new) and (old[j] != new[j] or
                                any(old[k] != new[k]
                                    for k in range(j, min(j + GAP, len(new))))):
            j += 1
        out.append((i, j - i))
        i = j
    return out


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    stock = open(os.path.join(root, "kernel", "stock", KERNEL), "rb").read()
    ours = open(os.path.join(root, "kernel", "build", KERNEL), "rb").read()
    assert len(ours) >= len(stock)
    found = runs(stock, ours)
    data = bytearray()
    lines = []
    for off, n in found:
        lines.append("    {0x%05X, %5d, %5d}," % (off, n, len(data)))
        data += ours[off:off + n]
    hexrows = []
    for k in range(0, len(data), 12):
        hexrows.append("    " + " ".join("0x%02X," % b for b in data[k:k + 12]))
    with open(os.path.join(root, "installer", "kpatch.h"), "w") as f:
        f.write("""/* The NanoKernel patch. Written by tools/make-kpatch.py from
   kernel/build/%s: run it again after kernel/build.sh
   instead of editing this file. */

#define KPATCH_VERSION "2.28"
#define KPATCH_STOCK_SIZE %d
#define KPATCH_STOCK_CRC 0x%08XUL
#define KPATCH_SIZE %d
#define KPATCH_CRC 0x%08XUL

/* Each run: kernel offset, length, offset into kpatch_data. */
static const KPatchRun kpatch_runs[] = {
%s
};

static const unsigned char kpatch_data[%d] = {
%s
};
""" % (KERNEL, len(stock), zlib.crc32(stock), len(ours), zlib.crc32(ours),
       "\n".join(lines), len(data), "\n".join(hexrows)))
    print("%d runs, %d bytes" % (len(found), len(data)))


if __name__ == "__main__":
    main()
