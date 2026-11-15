#!/usr/bin/env python3
"""Make the release file: dist/Enforcer-VERSION.sit.hqx.

Usage: tools/make-release.py [VERSION]

Run tools/make-dist.sh first. VERSION defaults to the contents of
version.txt.

The file is a StuffIt archive in BinHex 4.0. StuffIt Expander (part of a
Mac OS 9 installation) expands it to the folder "Enforcer VERSION", which
holds Enforcer Installer, Enforcer Extension, the Enforcer control panel,
Read Me and License. BinHex is text, so the file survives any download
or transfer that does not keep resource forks.

The archive is in the StuffIt 1.5.1 format with nothing compressed: an
archive header, then a 112-byte header and the two forks for each file,
with the folder's files between a start-folder and an end-folder header.
The programs come from the MacBinary files in dist/; Read Me is README.md
as plain text, and License is LICENSE, both as SimpleText documents (Mac
Roman, CR line ends).
"""

import binascii
import os
import re
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")
PROGRAMS = ["Enforcer Installer.bin", "Enforcer Extension.bin", "Enforcer.bin"]

MAC_EPOCH = 2082844800  # seconds from 1904 to 1970
K_INITED = 0x0100       # Finder flag: the Finder has seen the file
SIT_FOLDER, SIT_END_FOLDER = 32, 33


def crc_arc(data):
    """CRC-16/ARC, which StuffIt uses for forks and headers."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def read_macbinary(path):
    """(name, type, creator, flags, data fork, resource fork)."""
    raw = open(path, "rb").read()
    name = raw[2:2 + raw[1]]
    ftype, creator = raw[65:69], raw[69:73]
    flags = raw[73] << 8 | raw[101]
    dlen, rlen = struct.unpack(">II", raw[83:91])
    data = raw[128:128 + dlen]
    roff = 128 + (dlen + 127) // 128 * 128
    rsrc = raw[roff:roff + rlen]
    if len(data) != dlen or len(rsrc) != rlen:
        sys.exit("%s: not a whole MacBinary file" % path)
    return name, ftype, creator, flags & ~K_INITED, data, rsrc


def text_file(name, text):
    """A SimpleText document."""
    body = text.replace("\r\n", "\n").replace("\n", "\r")
    return (name, b"TEXT", b"ttxt", 0,
            body.encode("mac_roman", errors="replace"), b"")


def plain(markdown):
    """README.md without the Markdown marks that only add noise as text."""
    out = []
    for line in markdown.split("\n"):
        if line.startswith("```") or line.startswith("[!["):  # code, badges
            continue
        line = re.sub(r"^#+ ", "", line)
        line = line.replace("**", "").replace("`", "")
        out.append(line)
    return "\n".join(out)


def sit_header(rmethod, dmethod, name, ftype, creator, flags, when,
               data, rsrc):
    h = struct.pack(">BBB63s4s4sHIIIIIIHH6x", rmethod, dmethod, len(name),
                    name, ftype, creator, flags, when, when,
                    len(rsrc), len(data), len(rsrc), len(data),
                    crc_arc(rsrc), crc_arc(data))
    return h + struct.pack(">H", crc_arc(h))


def sit_archive(folder, files, when):
    body = sit_header(SIT_FOLDER, SIT_FOLDER, folder, b"\0" * 4, b"\0" * 4,
                      0, when, b"", b"")
    for name, ftype, creator, flags, data, rsrc in files:
        body += sit_header(0, 0, name, ftype, creator, flags, when,
                           data, rsrc)
        body += rsrc + data
    body += sit_header(SIT_END_FOLDER, SIT_END_FOLDER, folder, b"\0" * 4,
                       b"\0" * 4, 0, when, b"", b"")
    # One entry at the top: the folder.
    head = struct.pack(">4sHI4sB7x", b"SIT!", 1, 22 + len(body), b"rLau", 1)
    return head + body


HQX_CHARS = (b"!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijkl"
             b"mpqr")


def binhex(name, ftype, creator, flags, data, rsrc):
    """BinHex 4.0: header, data fork and resource fork, each followed by
    its CRC; run-length coded (0x90 marks a run), then six bits a
    character, 64 characters a line."""
    head = (bytes([len(name)]) + name + b"\0" + ftype + creator +
            struct.pack(">HII", flags, len(data), len(rsrc)))
    raw = b""
    for part in (head, data, rsrc):
        raw += part + struct.pack(">H", binascii.crc_hqx(part, 0))
    packed = bytearray()
    i = 0
    while i < len(raw):
        b = raw[i]
        n = 1
        while n < 255 and i + n < len(raw) and raw[i + n] == b:
            n += 1
        if b == 0x90:
            packed += b"\x90\x00"
            n = 1
        elif n >= 3:
            packed += bytes([b, 0x90, n])
        else:
            packed += bytes([b]) * n
        i += n
    packed += b"\0" * (-len(packed) % 3)
    chars = bytearray(b":")
    for i in range(0, len(packed), 3):
        v = packed[i] << 16 | packed[i + 1] << 8 | packed[i + 2]
        chars += bytes(HQX_CHARS[v >> s & 63] for s in (18, 12, 6, 0))
    chars += b":"
    lines = [bytes(chars[i:i + 64]) for i in range(0, len(chars), 64)]
    return (b"(This file must be converted with BinHex 4.0)\n" +
            b"\n".join(lines) + b"\n")


def main():
    if len(sys.argv) > 1:
        version = sys.argv[1]
    else:
        version = open(os.path.join(ROOT, "version.txt")).read().strip()
    files = []
    for p in PROGRAMS:
        path = os.path.join(DIST, p)
        if not os.path.exists(path):
            sys.exit("make-release.py: missing %s (run tools/make-dist.sh)"
                     % path)
        files.append(read_macbinary(path))
    readme = open(os.path.join(ROOT, "README.md"), encoding="utf-8").read()
    files.append(text_file(b"Read Me", plain(readme)))
    license_text = open(os.path.join(ROOT, "LICENSE"),
                        encoding="utf-8").read()
    files.append(text_file(b"License", license_text))

    folder = ("Enforcer %s" % version).encode("mac_roman")
    when = int(time.time()) + MAC_EPOCH
    archive = sit_archive(folder, files, when)
    out = os.path.join(DIST, "Enforcer-%s.sit.hqx" % version)
    with open(out, "wb") as f:
        # Creator SITx is StuffIt Expander: a double-click opens it there.
        # (With StuffIt's own creator, SIT!, the Finder asks for DropStuff.)
        f.write(binhex(folder + b".sit", b"SIT!", b"SITx", 0, archive, b""))
    print("%s: %d bytes; folder \"%s\" with %s" % (
        out, os.path.getsize(out), folder.decode("mac_roman"),
        ", ".join(f[0].decode("mac_roman") for f in files)))


if __name__ == "__main__":
    main()
