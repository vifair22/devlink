#!/usr/bin/env python3
"""Convert an X11 window dump to a PNG.

Xvfb writes its framebuffer as an XWD file when given -fbdir, which is the
only way to photograph the windowed agent on a machine with no screen. The
conversion is here rather than left to ImageMagick so that looking at the
agent's window needs nothing beyond Xvfb, Wine and the Python that is already
required to check stack usage.
"""

import struct
import sys
import zlib

HEADER_FIELDS = 25
COLOR_ENTRY = 12  # XWDColor: pixel, red, green, blue, flags, pad


def read_xwd(path):
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < HEADER_FIELDS * 4:
        raise SystemExit("%s: too short to be an XWD file" % path)
    h = struct.unpack(">%dI" % HEADER_FIELDS, raw[: HEADER_FIELDS * 4])
    header_size, version, fmt = h[0], h[1], h[2]
    if version != 7:
        raise SystemExit("%s: XWD version %d, expected 7" % (path, version))
    if fmt != 2:
        raise SystemExit("%s: not ZPixmap" % path)
    width, height = h[4], h[5]
    bpp, stride = h[11], h[12]
    if bpp not in (24, 32):
        raise SystemExit("%s: %d bits per pixel is not handled" % (path, bpp))
    masks = (h[14], h[15], h[16])
    start = header_size + h[19] * COLOR_ENTRY
    return width, height, bpp, stride, masks, raw[start:]


def shift_of(mask):
    s = 0
    while mask and not mask & 1:
        mask >>= 1
        s += 1
    return s


def to_png(width, height, bpp, stride, masks, pixels):
    """Truecolour pixels to 8-bit RGB scanlines, then a minimal PNG."""
    rs, gs, bs = (shift_of(m) for m in masks)
    step = bpp // 8
    rows = bytearray()
    for y in range(height):
        base = y * stride
        rows.append(0)  # filter: none
        for x in range(width):
            off = base + x * step
            if off + step > len(pixels):
                rows.extend(b"\0\0\0")
                continue
            v = int.from_bytes(pixels[off : off + step], "little")
            rows.append((v & masks[0]) >> rs)
            rows.append((v & masks[1]) >> gs)
            rows.append((v & masks[2]) >> bs)

    def chunk(kind, body):
        return (
            struct.pack(">I", len(body))
            + kind
            + body
            + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
        )

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(rows), 6))
        + chunk(b"IEND", b"")
    )


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: xwd2png.py IN.xwd OUT.png")
    width, height, bpp, stride, masks, pixels = read_xwd(sys.argv[1])
    with open(sys.argv[2], "wb") as f:
        f.write(to_png(width, height, bpp, stride, masks, pixels))
    print("%s: %dx%d" % (sys.argv[2], width, height))


if __name__ == "__main__":
    main()
