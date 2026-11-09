"""Combine BMP frame dumps into one PNG contact sheet (stdlib only).

  python tools/contact_sheet.py <out.png> <cols> <a.bmp> [b.bmp ...]

Each tile is scaled (nearest) to 320x240; 1024x512 VRAM dumps are scaled to 512x256.
"""
import struct
import sys
import zlib


def read_bmp(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    flip = h > 0
    h = abs(h)
    bpr = (w * (bpp // 8) + 3) & ~3
    rows = []
    for y in range(h):
        sy = h - 1 - y if flip else y
        row = d[off + sy * bpr: off + sy * bpr + w * (bpp // 8)]
        px = []
        step = bpp // 8
        for x in range(w):
            b, g, r = row[x * step], row[x * step + 1], row[x * step + 2]
            px.append((r, g, b))
        rows.append(px)
    return w, h, rows


def scale(w, h, rows, tw, th):
    return [[rows[y * h // th][x * w // tw] for x in range(tw)] for y in range(th)]


def write_png(path, w, h, rows):
    raw = bytearray()
    for r in rows:
        raw.append(0)
        for p in r:
            raw += bytes(p)

    def chunk(t, data):
        c = struct.pack(">I", len(data)) + t + data
        return c + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    out, cols, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    tiles = []
    for f in files:
        w, h, rows = read_bmp(f)
        tw, th = (512, 256) if w == 1024 else (320, 240)
        tiles.append(scale(w, h, rows, tw, th))
    tw = max(len(t[0]) for t in tiles)
    th = max(len(t) for t in tiles)
    rows_n = (len(tiles) + cols - 1) // cols
    W, H = cols * (tw + 4), rows_n * (th + 4)
    canvas = [[(40, 40, 40)] * W for _ in range(H)]
    for i, t in enumerate(tiles):
        ox, oy = (i % cols) * (tw + 4), (i // cols) * (th + 4)
        for y, r in enumerate(t):
            canvas[oy + y][ox:ox + len(r)] = r
    write_png(out, W, H, canvas)


if __name__ == "__main__":
    main()
