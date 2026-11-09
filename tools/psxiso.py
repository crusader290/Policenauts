"""Minimal ISO9660 reader for raw MODE2/2352 PlayStation disc images.

Usage:
  python tools/psxiso.py list    <disc.bin>
  python tools/psxiso.py extract <disc.bin> <out_dir> [path_filter...]
"""
import os
import struct
import sys

SECTOR = 2352
DATA_OFF = 24  # 12 sync + 4 header + 8 subheader (Mode 2 Form 1)


class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        self.nsectors = self.f.tell() // SECTOR

    def raw(self, lba):
        self.f.seek(lba * SECTOR)
        return self.f.read(SECTOR)

    def data(self, lba, count=1):
        out = bytearray()
        for i in range(count):
            out += self.raw(lba + i)[DATA_OFF:DATA_OFF + 2048]
        return bytes(out)

    def read_file(self, lba, size):
        n = (size + 2047) // 2048
        return self.data(lba, n)[:size]

    def pvd(self):
        d = self.data(16)
        assert d[1:6] == b"CD001", "no ISO9660 PVD"
        root = d[156:156 + 34]
        return d, root


def parse_dir(disc, lba, size, prefix=""):
    buf = disc.data(lba, (size + 2047) // 2048)
    pos = 0
    entries = []
    while pos < len(buf):
        ln = buf[pos]
        if ln == 0:
            pos = (pos // 2048 + 1) * 2048
            continue
        rec = buf[pos:pos + ln]
        ext = struct.unpack_from("<I", rec, 2)[0]
        dsz = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        pos += ln
        if name in (b"\x00", b"\x01"):
            continue
        name = name.decode("ascii", "replace")
        full = prefix + "/" + name
        if flags & 2:
            entries.append((full + "/", ext, dsz, True))
            entries += parse_dir(disc, ext, dsz, full)
        else:
            entries.append((full, ext, dsz, False))
    return entries


def walk(disc):
    d, root = disc.pvd()
    ext = struct.unpack_from("<I", root, 2)[0]
    dsz = struct.unpack_from("<I", root, 10)[0]
    return parse_dir(disc, ext, dsz)


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    disc = Disc(path)
    ents = walk(disc)
    if cmd == "list":
        print(f"# {disc.nsectors} sectors")
        for name, lba, size, isdir in ents:
            print(f"{lba:8d} {size:10d}  {name}")
    elif cmd == "extract":
        out = sys.argv[3]
        filt = [s.upper() for s in sys.argv[4:]]
        for name, lba, size, isdir in ents:
            if isdir:
                continue
            if filt and not any(f in name.upper() for f in filt):
                continue
            clean = name.split(";")[0].lstrip("/")
            dst = os.path.join(out, clean)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as o:
                o.write(disc.read_file(lba, size))
            print("extracted", clean, size)


if __name__ == "__main__":
    main()
