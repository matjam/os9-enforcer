#!/usr/bin/env python3
"""Draw the Enforcer icons and write them as Rez source.

The icon is a blue shield with a gold rim and a memory chip on its face:
Enforcer keeps each application's memory safe from the others. The
extension's icon puts the shield in front of a puzzle piece, as Mac OS
does for extensions.

Usage: tools/make-icons.py [-p DIR]

Writes enforcer/icon-panel.r and enforcer/icon-extension.r. Each holds the
icon family at ID 128 in every format Mac OS 9 reads:
  'icns'  32-bit color with 8-bit masks, for Icon Services (Mac OS 8.5 and
          later), with copies of the formats below;
  'icl8', 'ics8'  256 colors (the system palette);
  'icl4', 'ics4'  16 colors;
  'ICN#', 'ics#'  black and white, with the mask.
and the 'icns' again at ID -16455, as the file's custom icon: the Finder
shows bundle icons from the desktop database, which keeps at most 256
colors.
With -p, it also writes preview images into DIR (needs PIL).

Each icon is drawn from shapes given as signed distance functions (negative
inside), sampled 8 by 8 times in each pixel. Edges that fall on pixel
boundaries stay sharp; curves get soft edges in the 32-bit icons.
"""

import argparse
import math
import os
import struct

SAMPLES = 8
LIGHT = (-0.6, -0.8)  # toward the light: up and to the left (y grows down)

# Shapes -----------------------------------------------------------------


def box(x, y, cx, cy, hw, hh, r=0.0):
    qx = abs(x - cx) - hw + r
    qy = abs(y - cy) - hh + r
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


def circle(x, y, cx, cy, r):
    return math.hypot(x - cx, y - cy) - r


def shield(cx, top, w, mid, bottom, corner):
    """A heater shield: straight sides from top to mid, then two arcs that
    meet at a point at (cx, bottom). Each side is a circle swept upward
    from mid, so insets stay even all the way round."""
    r = (w * w + (bottom - mid) ** 2) / (2 * w)
    xl, xr = cx - w + r, cx + w - r
    hh = (bottom + 2 - top) / 2

    def side(x, y, xc):
        if y < mid:
            return abs(x - xc) - r
        return math.hypot(x - xc, y - mid) - r

    def f(x, y):
        return max(box(x, y, cx, top + hh, w, hh, corner),
                   side(x, y, xl), side(x, y, xr))
    return f


def puzzle(x0, y0, s, k, corner):
    """A puzzle piece: a square body from (x0, y0) of side s, knobs of
    radius k on the top and right, holes on the left and bottom."""
    c = x0 + s / 2
    m = y0 + s / 2

    def f(x, y):
        d = min(box(x, y, c, m, s / 2, s / 2, corner),
                circle(x, y, c, y0 - k * 0.55, k),
                circle(x, y, x0 + s + k * 0.55, m, k))
        return max(d, -circle(x, y, x0 + k * 0.55, m, k * 0.85),
                   -circle(x, y, c, y0 + s - k * 0.55, k * 0.85))
    return f


def on_ground(f, cx, cy, b=0.5):
    """f drawn lying flat, seen from the front and above: the plane's
    (u, v) shows at (cx + u, cy + v b). The distance is scaled back to
    pixels."""
    def g(x, y):
        return f(x - cx, (y - cy) / b)

    def h(x, y, e=0.01):
        gx = (g(x + e, y) - g(x - e, y)) / (2 * e)
        gy = (g(x, y + e) - g(x, y - e)) / (2 * e)
        return g(x, y) / (math.hypot(gx, gy) or 1)
    return h


def facing(f, x, y, e=0.01):
    """How much the outward normal of f's boundary near (x, y) faces the
    light, from -1 (away) to 1 (toward it)."""
    gx = f(x + e, y) - f(x - e, y)
    gy = f(x, y + e) - f(x, y - e)
    n = math.hypot(gx, gy) or 1
    return (gx * LIGHT[0] + gy * LIGHT[1]) / n


# Colors -----------------------------------------------------------------


def rgb(hexstr):
    v = int(hexstr, 16)
    return ((v >> 16) / 255, (v >> 8 & 255) / 255, (v & 255) / 255)


def mix(a, b, t):
    t = min(max(t, 0.0), 1.0)
    return tuple(p + (q - p) * t for p, q in zip(a, b))


def shade(dark, mid, light, t):
    """mid at t = 0, dark at -1, light at 1."""
    return mix(mid, light, t) if t > 0 else mix(mid, dark, -t)


GOLD_DARK, GOLD, GOLD_LIGHT = rgb("8A5A00"), rgb("E0A82E"), rgb("FFF0A0")
GOLD_EDGE = rgb("4A3000")
BLUE_L_TOP, BLUE_L_BOT = rgb("6F9BFF"), rgb("2F55C8")
BLUE_R_TOP, BLUE_R_BOT = rgb("3D66E0"), rgb("16309A")
CHIP_TOP, CHIP_BOT = rgb("4A4A55"), rgb("24242A")
CHIP_LIGHT, CHIP_DARK = rgb("8A8A96"), rgb("0E0E12")
NOTCH = rgb("15151A")
PIN, PIN_DARK = rgb("D8D8E0"), rgb("8C8C98")
PIECE_DARK, PIECE, PIECE_LIGHT = rgb("8890A8"), rgb("C4CADB"), rgb("F2F4FA")
PIECE_SIDE, PIECE_SIDE_DARK = rgb("7A82A0"), rgb("4E5570")
PIECE_EDGE = rgb("2E3346")

# Each scene returns (color, bit, c4) for a point, or None outside the
# icon. bit is the black and white icon's pixel: 0 white, 1 black, or the
# pattern 'l' (light gray, 1 in 4 black). c4 is the 16-color icon's color,
# an index into CLUT4: that palette is too coarse to pick from by nearness.


def pattern(bit, i, j):
    if bit == 'l':
        return 1 if (i % 4 == 0 and j % 2 == 0) or (i % 4 == 2 and j % 2) \
            else 0
    return bit


class Badge:
    """The shield and chip, in a box of `size` pixels (32 or 16) or a
    smaller one at an offset, for the extension."""

    def __init__(self, cx, top, w, mid, bottom, corner, rim, chip):
        self.f = shield(cx, top, w, mid, bottom, corner)
        self.cx, self.top, self.bottom = cx, top, bottom
        self.rim = rim
        self.chip = chip  # (cy, hw, hh, pin_rows, pin_len, notch) or None

    def __call__(self, x, y):
        d = self.f(x, y)
        if d > 0:
            return None
        c = self.chip_at(x, y)
        if c:
            return c
        if d > -1:
            return GOLD_EDGE, 1, 10
        if d > -1 - self.rim:
            t = facing(self.f, x, y)
            inner = self.rim >= 2 and d <= -1 - self.rim / 2
            if inner:
                t = -t * 0.8  # the inner half of the bevel faces inward
            c4 = 1 if t > 0.15 else 10 if t < -0.45 else 11
            return shade(GOLD_DARK, GOLD, GOLD_LIGHT, t * 1.2), int(inner), c4
        v = (y - self.top) / (self.bottom - self.top)
        if x < self.cx:
            return mix(BLUE_L_TOP, BLUE_L_BOT, v), 0, 6
        return mix(BLUE_R_TOP, BLUE_R_BOT, v), 'l', 5

    def chip_at(self, x, y):
        if not self.chip:
            return None
        cy, hw, hh, pin_rows, pin_len, notch = self.chip
        cx = self.cx
        d = box(x, y, cx, cy, hw, hh, 0.6)
        if d <= 0:
            if notch and circle(x, y, cx, cy - hh, notch) <= 0:
                return NOTCH, 1, 15
            if d > -0.8:
                t = facing(lambda u, v: box(u, v, cx, cy, hw, hh, 0.6), x, y)
                if t > 0.3:
                    return CHIP_LIGHT, 1, 13
                if t < -0.3:
                    return CHIP_DARK, 1, 15
            return mix(CHIP_TOP, CHIP_BOT, (y - cy + hh) / (2 * hh)), 1, 14
        for py in pin_rows:
            if abs(y - py) <= 0.5 and hw <= abs(x - cx) <= hw + pin_len:
                if abs(x - cx) > hw + pin_len - 1:
                    return PIN_DARK, 1, 13
                return PIN, 1, 12
        return None


class Extension:
    """The badge standing on a puzzle piece that lies flat (f, drawn with
    on_ground), with `depth` pixels of thickness below its top face."""

    def __init__(self, badge, piece, depth):
        self.badge = badge
        self.f = piece
        self.steps = [depth * (k + 1) / 8 for k in range(8)]

    def __call__(self, x, y):
        c = self.badge(x, y)
        if c:
            return c
        d = self.f(x, y)
        side = min(self.f(x, y - s) for s in self.steps)
        if min(d, side) > 0:
            return None
        if min(d, side) > -1:
            return PIECE_EDGE, 1, 15
        if d > 0:
            v = min(max(-side, 0) / 2, 1)
            return mix(PIECE_SIDE_DARK, PIECE_SIDE, v), 1, 13
        if d > -1:
            # the top face's edge: a highlight where it faces the light
            t = facing(self.f, x, y)
            c4 = 0 if t > 0.2 else 12
            return shade(PIECE_DARK, PIECE, PIECE_LIGHT, t * 1.3), 0, c4
        color = mix(PIECE_LIGHT, PIECE, 0.6)
        # The badge's shadow, cast down and to the right.
        if self.badge.f(x - 1.5, y - 1) <= 0:
            return mix(color, PIECE_SIDE_DARK, 0.45), 0, 13
        return color, 0, 12


ICONS = {
    "panel": {
        32: Badge(16, 2, 13, 15, 31, 2, 2, (16, 5, 6, [11.5, 14.5, 17.5, 20.5],
                                            2, 1.5)),
        16: Badge(8, 1, 7, 7, 15.6, 1, 1, (7.5, 2, 2.5, [6.5, 8.5], 1, 0)),
    },
    "extension": {
        32: Extension(
            Badge(15, 1, 9, 10, 26, 1.5, 2, (10, 3, 4, [7.5, 10.5, 13.5],
                                             2, 1)),
            on_ground(puzzle(-10, -10, 20, 3.4, 2), 15, 23.5), 3),
        16: Extension(
            Badge(7, 0, 4, 5, 13, 0.5, 1, (5, 1, 2, [3.5, 5.5], 1, 0)),
            on_ground(puzzle(-5, -5, 10, 1.8, 1), 7, 12), 1.5),
    },
}

# The 256-color icons use these colors of the system palette (CLUT8): the
# nearest of all 256 puts stray purples and greens into the gradients.
PAL8 = """
    FFFFFF EEEEEE DDDDDD CCCCCC BBBBBB AAAAAA 999999 888888 777777 666666
    555555 444444 333333 222222 111111 000000
    99CCFF 6699FF 3366FF 3366CC 0033CC 003399 000099
    FFFFCC FFFF99 FFCC66 FFCC33 CC9933 CC9900 996600 663300 333300
    CCCCFF 9999CC 666699 333366
""".split()

# Rendering --------------------------------------------------------------


def render(scene, size):
    """Returns rows of (r, g, b, a) floats, rows of 1-bit values and rows
    of 16-color indices (None where the pixel's center is outside)."""
    pixels, bits, c4s = [], [], []
    n = SAMPLES * SAMPLES
    for j in range(size):
        row, brow, crow = [], [], []
        for i in range(size):
            acc = [0.0, 0.0, 0.0]
            hits = 0
            for sj in range(SAMPLES):
                for si in range(SAMPLES):
                    c = scene(i + (si + 0.5) / SAMPLES, j + (sj + 0.5) / SAMPLES)
                    if c:
                        hits += 1
                        for k in range(3):
                            acc[k] += c[0][k]
            if hits:
                row.append((acc[0] / hits, acc[1] / hits, acc[2] / hits,
                            hits / n))
            else:
                row.append((0.0, 0.0, 0.0, 0.0))
            c = scene(i + 0.5, j + 0.5)
            brow.append(pattern(c[1], i, j) if c else 0)
            crow.append(c[2] if c else None)
        pixels.append(row)
        bits.append(brow)
        c4s.append(crow)
    return pixels, bits, c4s


def clut8():
    """The Mac OS system palette for 256 colors."""
    levels = [0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00]
    clut = [(r, g, b) for r in levels for g in levels for b in levels][:215]
    ramp = [0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11]
    clut += [(v, 0, 0) for v in ramp]
    clut += [(0, v, 0) for v in ramp]
    clut += [(0, 0, v) for v in ramp]
    clut += [(v, v, v) for v in ramp]
    clut.append((0, 0, 0))
    return clut


# The Mac OS system palette for 16 colors.
CLUT4 = [(0xFF, 0xFF, 0xFF), (0xFC, 0xF3, 0x05), (0xFF, 0x64, 0x02),
         (0xDD, 0x08, 0x06), (0xF2, 0x08, 0x84), (0x46, 0x00, 0xA5),
         (0x00, 0x00, 0xD4), (0x02, 0xAB, 0xEA), (0x1F, 0xB7, 0x14),
         (0x00, 0x64, 0x11), (0x56, 0x2C, 0x05), (0x90, 0x71, 0x3A),
         (0xC0, 0xC0, 0xC0), (0x80, 0x80, 0x80), (0x40, 0x40, 0x40),
         (0x00, 0x00, 0x00)]
CLUT8 = clut8()
USE8 = [CLUT8.index(tuple(bytes.fromhex(h))) for h in PAL8]


def nearest(clut, c, use=None):
    r, g, b = (v * 255 for v in c)

    def dist(k):
        p = clut[k]
        rm = (r + p[0]) / 2
        dr, dg, db = r - p[0], g - p[1], b - p[2]
        return ((2 + rm / 256) * dr * dr + 4 * dg * dg +
                (2 + (255 - rm) / 256) * db * db)
    return min(use or range(len(clut)), key=dist)


class Icon:
    def __init__(self, scene, size):
        self.size = size
        self.pixels, self.bits, c4s = render(scene, size)
        self.mask = [[1 if p[3] >= 0.5 else 0 for p in row]
                     for row in self.pixels]
        self.idx8 = [[nearest(CLUT8, p[:3], USE8) if m else 0
                      for p, m in zip(row, mrow)]
                     for row, mrow in zip(self.pixels, self.mask)]
        self.idx4 = [[(c if c is not None else nearest(CLUT4, p[:3]))
                      if m else 0
                      for p, m, c in zip(row, mrow, crow)]
                     for row, mrow, crow in zip(self.pixels, self.mask, c4s)]
        # The black and white icon keeps a solid outline: black on every
        # masked pixel next to one outside the mask.
        s = size
        for j in range(s):
            for i in range(s):
                if self.mask[j][i] and any(
                        not (0 <= i + di < s and 0 <= j + dj < s) or
                        not self.mask[j + dj][i + di]
                        for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                    self.bits[j][i] = 1
                if not self.mask[j][i]:
                    self.bits[j][i] = 0

    # Mac OS formats

    def bitmap(self, rows):
        out = bytearray()
        for row in rows:
            for k in range(0, self.size, 8):
                v = 0
                for b in row[k:k + 8]:
                    v = v << 1 | b
                out.append(v)
        return bytes(out)

    def icn(self):  # 'ICN#' or 'ics#': the icon, then the mask
        return self.bitmap(self.bits) + self.bitmap(self.mask)

    def ic8(self):
        return bytes(v for row in self.idx8 for v in row)

    def ic4(self):
        out = bytearray()
        for row in self.idx4:
            for k in range(0, self.size, 2):
                out.append(row[k] << 4 | row[k + 1])
        return bytes(out)

    def channel(self, k):
        return bytes(round(p[k] * 255) if p[3] > 0 else 0
                     for row in self.pixels for p in row)

    def rgb32(self):  # 'il32' or 'is32': each channel compressed
        return b"".join(rle(self.channel(k)) for k in range(3))

    def mask8(self):  # 'l8mk' or 's8mk'
        return bytes(round(p[3] * 255) for row in self.pixels for p in row)


def rle(data):
    """The run-length coding of 32-bit icon data: a byte n below 0x80 is
    followed by n + 1 literal bytes, one of 0x80 or more by one byte to
    repeat n - 0x80 + 3 times."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        j = i
        while j < n and j - i < 130 and data[j] == data[i]:
            j += 1
        if j - i >= 3:
            out += bytes((0x80 + j - i - 3, data[i]))
            i = j
            continue
        j = i
        while j < n and j - i < 128:
            if j + 2 < n and data[j] == data[j + 1] == data[j + 2]:
                break
            j += 1
        out.append(j - i - 1)
        out += data[i:j]
        i = j
    return bytes(out)


def icns(large, small):
    parts = [(b"ICN#", large.icn()), (b"icl4", large.ic4()),
             (b"icl8", large.ic8()), (b"il32", large.rgb32()),
             (b"l8mk", large.mask8()), (b"ics#", small.icn()),
             (b"ics4", small.ic4()), (b"ics8", small.ic8()),
             (b"is32", small.rgb32()), (b"s8mk", small.mask8())]
    body = b"".join(t + struct.pack(">I", len(d) + 8) + d for t, d in parts)
    return b"icns" + struct.pack(">I", len(body) + 8) + body


def rez_data(rtype, rid, data):
    lines = ["data '%s' (%d) {" % (rtype, rid)]
    for k in range(0, len(data), 32):
        lines.append('\t$"%s"' % data[k:k + 32].hex().upper())
    lines.append("};")
    return "\n".join(lines)


def write_rez(path, name, large, small):
    family = icns(large, small)
    resources = [("ICN#", 128, large.icn()), ("icl4", 128, large.ic4()),
                 ("icl8", 128, large.ic8()), ("ics#", 128, small.icn()),
                 ("ics4", 128, small.ic4()), ("ics8", 128, small.ic8()),
                 ("icns", 128, family), ("icns", -16455, family)]
    with open(path, "w") as f:
        f.write(
            "/* The %s's icons. Written by tools/make-icons.py: change that "
            "and run\n   it again instead of editing this file.\n\n"
            "   ID 128 is the icon family that the bundle names. The desktop "
            "database\n   keeps only up to 256 colors and a 1-bit mask of it, "
            "so the same 'icns'\n   is also the file's custom icon (ID -16455, "
            "with the hasCustomIcon flag\n   set by tools/finder-flags.py), "
            "which Icon Services draws in 32-bit\n   color. */\n\n" % name)
        for rtype, rid, data in resources:
            f.write(rez_data(rtype, rid, data) + "\n\n")


# Preview ----------------------------------------------------------------


def preview(icons, path):
    from PIL import Image
    desk = (0x66, 0x66, 0x99)
    zoom = 8
    names = list(icons)
    cols = 4  # 32-bit, 8-bit, 4-bit, 1-bit
    w = cols * (32 * zoom + 16) + cols * (16 * zoom + 16) + 200
    h = len(names) * (32 * zoom + 24) + 24
    sheet = Image.new("RGB", (w, h), desk)

    def draw(icon, kind, x0, y0, z, bg):
        for j in range(icon.size):
            for i in range(icon.size):
                p = icon.pixels[j][i]
                m = icon.mask[j][i]
                if kind == 32:
                    a = p[3]
                    c = tuple(round(p[k] * 255 * a + bg[k] * (1 - a))
                              for k in range(3))
                elif not m:
                    c = bg
                elif kind == 8:
                    c = CLUT8[icon.idx8[j][i]]
                elif kind == 4:
                    c = CLUT4[icon.idx4[j][i]]
                else:
                    c = (0, 0, 0) if icon.bits[j][i] else (255, 255, 255)
                for v in range(z):
                    for u in range(z):
                        sheet.putpixel((x0 + i * z + u, y0 + j * z + v), c)

    y = 12
    for name in names:
        large, small = icons[name]
        x = 12
        for kind in (32, 8, 4, 1):
            draw(large, kind, x, y, zoom, desk)
            x += 32 * zoom + 16
        for kind in (32, 8, 4, 1):
            draw(small, kind, x, y, zoom, desk)
            x += 16 * zoom + 16
        # actual size, on the desktop and on white
        draw(large, 32, x, y, 1, desk)
        draw(small, 32, x + 40, y, 1, desk)
        draw(large, 8, x, y + 40, 1, desk)
        draw(small, 8, x + 40, y + 40, 1, desk)
        for k in range(3):
            for v in range(80):
                for u in range(80):
                    sheet.putpixel((x + 80 + u, y + v), (255, 255, 255))
        draw(large, 32, x + 84, y + 4, 1, (255, 255, 255))
        draw(small, 32, x + 124, y + 4, 1, (255, 255, 255))
        draw(large, 1, x + 84, y + 44, 1, (255, 255, 255))
        draw(small, 1, x + 124, y + 44, 1, (255, 255, 255))
        y += 32 * zoom + 24
    sheet.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-p", metavar="DIR", help="also write preview images")
    args = ap.parse_args()
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    icons = {}
    for name, scenes in ICONS.items():
        large, small = Icon(scenes[32], 32), Icon(scenes[16], 16)
        icons[name] = (large, small)
        title = "control panel" if name == "panel" else "extension"
        write_rez(os.path.join(root, "enforcer", "icon-%s.r" % name), title,
                  large, small)
    if args.p:
        os.makedirs(args.p, exist_ok=True)
        preview(icons, os.path.join(args.p, "icons.png"))


if __name__ == "__main__":
    main()
