#!/usr/bin/env python3
"""Write the version of this build for the programs to carry.

Usage: tools/make-vers.py OUT.r OUT.h

OUT.r gets 'vers' resources 1 and 2, which the Finder shows in Get Info;
OUT.h defines ENFORCER_VERSION, a C string. A file whose contents would not
change is left alone, so nothing is built again for it. The CMake files of
enforcer/ and installer/ run this when they configure (common/version.cmake).

The version is:
- the environment variable ENFORCER_VERSION, when set (the release
  workflow sets it: its checkout has no tags);
- else version.txt, when HEAD is the commit tagged vVERSION;
- else version.txt, a plus sign and the short commit hash, for example
  0.1.0+1a2b3c4: a build made after that release. Without git, "+dev".
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git(*args):
    try:
        r = subprocess.run(["git", "-C", ROOT] + list(args),
                           capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout.strip() if r.returncode == 0 else None


def version():
    env = os.environ.get("ENFORCER_VERSION", "").strip()
    if env:
        return env.lstrip("v")
    base = open(os.path.join(ROOT, "version.txt")).read().strip()
    head = git("rev-parse", "HEAD")
    if head is None:
        return base + "+dev"
    if git("rev-parse", "-q", "--verify", "refs/tags/v%s^{commit}" % base) \
            == head:
        return base
    return "%s+%s" % (base, head[:7])


def bcd(n):
    n = min(int(n), 99)
    return n // 10 << 4 | n % 10


def vers_bytes(ver, long_text):
    """The 'vers' resource: the number in BCD, the stage, the region, and
    the short and long strings."""
    m = re.match(r"(\d+)\.(\d+)(?:\.(\d+))?", ver)
    major, minor, bug = (int(x or 0) for x in m.groups()) if m else (0, 0, 0)
    release = re.fullmatch(r"\d+\.\d+(\.\d+)?", ver) is not None
    short = ver.encode("mac_roman", errors="replace")[:255]
    text = long_text.encode("mac_roman", errors="replace")[:255]
    return (bytes([bcd(major), min(minor, 9) << 4 | min(bug, 9),
                   0x80 if release else 0x20,   # final, or development
                   0, 0, 0]) +                  # no build number; region US
            bytes([len(short)]) + short + bytes([len(text)]) + text)


def rez_data(res_id, data):
    lines = ["data 'vers' (%d) {" % res_id]
    for i in range(0, len(data), 16):
        lines.append('\t$"%s"' % data[i:i + 16].hex().upper())
    lines.append("};")
    return "\n".join(lines) + "\n"


def write_if_changed(path, text):
    try:
        if open(path).read() == text:
            return
    except OSError:
        pass
    with open(path, "w") as f:
        f.write(text)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    ver = version()
    write_if_changed(sys.argv[1],
                     "/* Written by tools/make-vers.py. */\n" +
                     rez_data(1, vers_bytes(ver, "Enforcer " + ver)) +
                     rez_data(2, vers_bytes(ver, "Enforcer " + ver)))
    write_if_changed(sys.argv[2],
                     "/* Written by tools/make-vers.py. */\n"
                     '#define ENFORCER_VERSION "%s"\n' % ver)
    print(ver)


if __name__ == "__main__":
    main()
