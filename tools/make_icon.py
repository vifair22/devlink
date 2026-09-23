#!/usr/bin/env python3
"""Draw the agent's icon.

Generated rather than stored so the repository keeps no binary that nobody can
read or review in a diff. It is two connectors with a cable between them, which
is what the program is; the shape has to survive being 16 pixels wide, so it is
drawn from rectangles and nothing else.

24-bit colour with a 1-bit transparency mask, which every Windows from 95
onwards understands. usage: make_icon.py OUT.ico
"""

import struct
import sys

BODY = (0x2F, 0x4F, 0x73)   # connector, dark slate blue (BGR order below)
CABLE = (0xC8, 0x8A, 0x2A)  # cable, amber
SIZES = (16, 32, 48)


def draw(size):
    """A pixel grid, None where the icon is transparent."""
    px = [[None] * size for _ in range(size)]

    def box(x0, y0, x1, y1, colour):
        for y in range(max(0, y0), min(size, y1)):
            for x in range(max(0, x0), min(size, x1)):
                px[y][x] = colour

    unit = size / 16.0
    mid = size // 2
    thick = max(1, int(round(unit * 2)))
    # The cable first, so the connectors sit on top of it.
    box(int(unit * 3), mid - thick // 2, int(unit * 13), mid - thick // 2 + thick, CABLE)
    # A connector at each end, taller than the cable is thick.
    tall = max(3, int(round(unit * 8)))
    wide = max(2, int(round(unit * 3)))
    box(int(unit * 1), mid - tall // 2, int(unit * 1) + wide, mid - tall // 2 + tall, BODY)
    box(size - int(unit * 1) - wide, mid - tall // 2, size - int(unit * 1),
        mid - tall // 2 + tall, BODY)
    return px


def image(size):
    """One icon image: a header, the colours bottom-up, then the mask."""
    px = draw(size)
    row_pad = (4 - (size * 3) % 4) % 4
    colours = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            r, g, b = px[y][x] or (0, 0, 0)
            colours += bytes((b, g, r))
        colours += b"\0" * row_pad

    # The mask is 1 where the pixel is transparent, packed into whole bytes and
    # padded to four per row like any other Windows bitmap.
    mask_row = (size + 31) // 32 * 4
    mask = bytearray()
    for y in range(size - 1, -1, -1):
        bits = bytearray(mask_row)
        for x in range(size):
            if px[y][x] is None:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits

    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 24, 0,
                         len(colours) + len(mask), 0, 0, 0, 0)
    return header + bytes(colours) + bytes(mask)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: make_icon.py OUT.ico")
    images = [image(s) for s in SIZES]
    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, data in zip(SIZES, images):
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 24, len(data), offset)
        offset += len(data)
    for data in images:
        out += data
    with open(sys.argv[1], "wb") as f:
        f.write(bytes(out))
    print("%s: %s" % (sys.argv[1], ", ".join("%dx%d" % (s, s) for s in SIZES)))


if __name__ == "__main__":
    main()
