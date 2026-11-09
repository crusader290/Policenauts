"""Unpacker for Policenauts PSX 'FRID' .DPK archives.

Header (little endian):
  0x00 'FRID'
  0x04 u32 flags (0xE0000000 observed)
  0x08 u32 data alignment / first data offset
  0x0C u32 entry count
  0x10 u32 ?
  0x14 u32 entry size (0x18)
Entries at 0x20, 0x18 bytes each: char name[12], u32 offset, u32 size, u32 checksum.

Usage:
  python tools/dpk.py list    <file.DPK>
  python tools/dpk.py extract <file.DPK> <out_dir>
"""
import os
import struct
import sys


def entries(buf):
    assert buf[:4] == b"FRID", "not a FRID archive"
    count = struct.unpack_from("<I", buf, 0x0C)[0]
    esz = struct.unpack_from("<I", buf, 0x14)[0]
    out = []
    for i in range(count):
        o = 0x20 + i * esz
        name = buf[o:o + 12].split(b"\0")[0].decode("ascii", "replace")
        off, size, csum = struct.unpack_from("<III", buf, o + 12)
        out.append((name, off, size, csum))
    return out


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    buf = open(path, "rb").read()
    ents = entries(buf)
    if cmd == "list":
        for name, off, size, csum in ents:
            print(f"{off:08x} {size:8d} {csum:08x}  {name}")
    elif cmd == "extract":
        out = sys.argv[3]
        os.makedirs(out, exist_ok=True)
        for name, off, size, csum in ents:
            with open(os.path.join(out, name), "wb") as o:
                o.write(buf[off:off + size])


if __name__ == "__main__":
    main()
