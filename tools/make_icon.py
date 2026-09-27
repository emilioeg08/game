"""Draws the provisional application icon (a star, an orbit and a planet) and writes it as a Windows .ico
with PNG-compressed images at 16, 24, 32, 48, 64 and 256 px. Standard library only, deterministic output.

    python tools/make_icon.py Apps/Game/resources/gx_game.ico
"""
import math
import struct
import sys
import zlib

SUPERSAMPLE = 4


def render(size):
    n = size * SUPERSAMPLE
    c = (n - 1) / 2.0
    acc = [[(0.0, 0.0, 0.0, 0.0)] * size for _ in range(size)]
    rows = []
    for y in range(n):
        row = []
        for x in range(n):
            dx, dy = (x - c) / n, (y - c) / n  # -0.5..0.5
            r = math.hypot(dx, dy)
            color = None
            # Dark disc (the background of the icon).
            if r <= 0.48:
                color = (12, 18, 34, 255)
            # Orbit: a tilted ellipse.
            ex, ey = dx * math.cos(0.5) + dy * math.sin(0.5), -dx * math.sin(0.5) + dy * math.cos(0.5)
            e = math.hypot(ex / 0.40, ey / 0.20)
            if abs(e - 1.0) < 0.045 and r <= 0.48:
                color = (90, 150, 230, 255)
            # Star.
            if r <= 0.12:
                t = r / 0.12
                color = (255, int(235 - 60 * t), int(150 - 100 * t), 255)
            # Planet on the orbit.
            px, py = 0.40 * math.cos(-0.9), 0.20 * math.sin(-0.9)
            wx, wy = px * math.cos(0.5) - py * math.sin(0.5), px * math.sin(0.5) + py * math.cos(0.5)
            if math.hypot(dx - wx, dy - wy) <= 0.075:
                color = (70, 200, 140, 255)
            row.append(color or (0, 0, 0, 0))
        rows.append(row)
    pixels = bytearray()
    for y in range(size):
        pixels.append(0)  # PNG filter: none
        for x in range(size):
            r = g = b = a = 0
            for sy in range(SUPERSAMPLE):
                for sx in range(SUPERSAMPLE):
                    pr, pg, pb, pa = rows[y * SUPERSAMPLE + sy][x * SUPERSAMPLE + sx]
                    r += pr * pa
                    g += pg * pa
                    b += pb * pa
                    a += pa
            k = SUPERSAMPLE * SUPERSAMPLE
            if a:
                pixels += bytes((r // a, g // a, b // a, a // k))
            else:
                pixels += bytes((0, 0, 0, 0))
    return bytes(pixels)


def png(size, raw):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main(path):
    sizes = [16, 24, 32, 48, 64, 256]
    images = [png(s, render(s)) for s in sizes]
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for s, image in zip(sizes, images):
        out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    out += b"".join(images)
    with open(path, "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "gx_game.ico")
