"""Generates src/app/app.ico (an isometric cube) using only the Python standard library.

    python tools/make_icon.py
"""

import os
import struct
import zlib

SIZES = [16, 24, 32, 48, 64, 256]

# Unit-square hexagon of an isometric cube.
TOP, UR, LR, BOTTOM, LL, UL, CENTER = (0.5, 0.06), (0.89, 0.285), (0.89, 0.715), (0.5, 0.94), (0.11, 0.715), (0.11, 0.285), (0.5, 0.5)
FACES = [
    ([TOP, UR, CENTER, UL], (126, 200, 255)),     # top
    ([UL, CENTER, BOTTOM, LL], (47, 134, 214)),   # left
    ([CENTER, UR, LR, BOTTOM], (28, 95, 168)),    # right
]


def inside(poly, x, y):
    sign = 0
    for i in range(len(poly)):
        ax, ay = poly[i]
        bx, by = poly[(i + 1) % len(poly)]
        cross = (bx - ax) * (y - ay) - (by - ay) * (x - ax)
        if cross != 0:
            s = 1 if cross > 0 else -1
            if sign and s != sign:
                return False
            sign = s
    return True


def render(size, ss=4):
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    x = (px + (sx + 0.5) / ss) / size
                    y = (py + (sy + 0.5) / ss) / size
                    for poly, color in FACES:
                        if inside(poly, x, y):
                            # subtle vertical shading
                            k = 1.08 - 0.16 * y
                            acc[0] += min(255, color[0] * k)
                            acc[1] += min(255, color[1] * k)
                            acc[2] += min(255, color[2] * k)
                            acc[3] += 255
                            break
            n = ss * ss
            a = acc[3] / n
            if a > 0:
                row += bytes([int(acc[0] / (acc[3] / 255)), int(acc[1] / (acc[3] / 255)), int(acc[2] / (acc[3] / 255)), int(a)])
            else:
                row += bytes([0, 0, 0, 0])
        rows.append(bytes(row))
    return rows


def png(size, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    images = [png(s, render(s)) for s in SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    for s, data in zip(SIZES, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "app", "app.ico")
    with open(out, "wb") as f:
        f.write(header + entries + b"".join(images))
    print("wrote", os.path.normpath(out))


main()
