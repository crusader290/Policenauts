"""Extract everything the recompiler needs from a Policenauts disc 1 image.

  python tools/extract_all.py <disc1.bin> <out_dir>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import dpk  # noqa: E402
import psxiso  # noqa: E402

WANT = ("SYSTEM.CNF", "SLPS_002", "KERNEL.SC", "BIN.DPK", "FONT.DPK")


def main():
    disc_path, out = sys.argv[1], sys.argv[2]
    disc = psxiso.Disc(disc_path)
    for name, lba, size, isdir in psxiso.walk(disc):
        if isdir or not any(w in name.upper() for w in WANT):
            continue
        rel = name.split(";")[0].lstrip("/")
        dst = os.path.join(out, "disc1", rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(disc.read_file(lba, size))
    for pack, sub in (("BIN.DPK", "bin"), ("FONT.DPK", "font")):
        data = open(os.path.join(out, "disc1", "NAUTS", pack), "rb").read()
        dst = os.path.join(out, "disc1", sub)
        os.makedirs(dst, exist_ok=True)
        for name, off, size, _ in dpk.entries(data):
            with open(os.path.join(dst, name), "wb") as f:
                f.write(data[off:off + size])
    open(os.path.join(out, ".stamp"), "w").close()


if __name__ == "__main__":
    main()
