"""Extract the kernel's task launch table and write splat-style symbol files.

Record layout (36 bytes): char task[16]; char file[16]; u32 entry.
Writes config/symbol_addrs.<segment>.txt entries `task_<name> = 0x...; // type:func`.
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KBASE = 0x80010000

FILE_TO_SEG = {"": "kernel", "display.bin": "display", "itp.bin": "itp", "subs1.bin": "subs1",
               "subs2.bin": "subs2", "subs3.bin": "subs3", "subs4.bin": "subs4",
               "masq1.bin": "masq1", "masq2.bin": "masq2", "blood.bin": "blood", "mv.bin": "mv",
               "menu.bin": "menu", "open.bin": "open", "english.bin": "english",
               "roll.bin": "roll"}


def cstr(b):
    return b.split(b"\0")[0].decode("ascii", "replace")


def main():
    k = open(os.path.join(ROOT, "extracted/disc1/NAUTS/SLPS_002.15"), "rb").read()[0x800:]
    anchor = k.find(b"carchase\0")
    # walk backwards/forwards in 36-byte strides from the anchor record
    start = anchor
    while start - 36 >= 0:
        prev = k[start - 36:start]
        name = cstr(prev[:16])
        if not re.fullmatch(r"[A-Za-z0-9_]+", name or "-"):
            break
        start -= 36
    recs = []
    o = start
    while o + 36 <= len(k):
        r = k[o:o + 36]
        name, fname = cstr(r[:16]), cstr(r[16:32])
        entry = struct.unpack_from("<I", r, 32)[0]
        if not re.fullmatch(r"[A-Za-z0-9_]+", name or "-") or not (0x80010000 <= entry < 0x80200000):
            break
        recs.append((name, fname, entry))
        o += 36
    print(f"task table at {KBASE + start:08x}, {len(recs)} records")
    per_seg = {}
    for name, fname, entry in recs:
        seg = FILE_TO_SEG.get(fname.lower(), None)
        print(f"  {name:12s} {fname:14s} {entry:08x} -> {seg}")
        if seg:
            per_seg.setdefault(seg, []).append((name, entry))
    os.makedirs(os.path.join(ROOT, "config"), exist_ok=True)
    for seg, items in per_seg.items():
        path = os.path.join(ROOT, "config", f"symbol_addrs.{seg}.txt")
        existing = open(path).read() if os.path.exists(path) else ""
        with open(path, "a") as f:
            for name, entry in items:
                line = f"task_{name} = 0x{entry:08X}; // type:func"
                if line not in existing:
                    f.write(line + "\n")


if __name__ == "__main__":
    main()
