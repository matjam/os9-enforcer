#!/usr/bin/env python3
"""Set Finder flags in MacBinary files.

Retro68's Rez writes MacBinary files with every Finder flag clear. The
Finder reads a program's bundle (its 'BNDL' resource, and so its icons)
only when hasBundle is set, and the file's own icon (resources with ID
-16455) only when hasCustomIcon is set.

Usage: tools/finder-flags.py [--bundle] [--custom-icon] FILE.bin...
"""

import argparse
import binascii
import sys

FLAGS_HIGH = 73          # the Finder flags' high byte in a MacBinary header
HAS_BUNDLE = 0x20        # bit 13 of the Finder flags
HAS_CUSTOM_ICON = 0x04   # bit 10
CRC = 124                # MacBinary II: CRC-16 of the header's first 124 bytes


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bundle", action="store_true", help="set hasBundle")
    ap.add_argument("--custom-icon", action="store_true",
                    help="set hasCustomIcon")
    ap.add_argument("files", nargs="+", metavar="FILE.bin")
    args = ap.parse_args()
    flags = (HAS_BUNDLE if args.bundle else 0) | \
        (HAS_CUSTOM_ICON if args.custom_icon else 0)
    for path in args.files:
        with open(path, "r+b") as f:
            header = bytearray(f.read(128))
            if len(header) < 128 or header[0] != 0 or \
                    binascii.crc_hqx(bytes(header[:CRC]), 0) != \
                    int.from_bytes(header[CRC:CRC + 2], "big"):
                sys.exit("%s: not a MacBinary II file" % path)
            header[FLAGS_HIGH] |= flags
            crc = binascii.crc_hqx(bytes(header[:CRC]), 0)
            header[CRC:CRC + 2] = crc.to_bytes(2, "big")
            f.seek(0)
            f.write(header)


if __name__ == "__main__":
    main()
