"""Disassemble a range of a configured code segment (debugging aid).

  python tools/disasm.py <segment> <addr_hex> [count]
  python tools/disasm.py kernel 800127B4 40
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "recomp"))

import rabbitizer  # noqa: E402

from config import load_segments  # noqa: E402


def main():
    name, addr = sys.argv[1], int(sys.argv[2], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 32
    seg = next((s for s in load_segments() if s.name == name), None)
    if seg is None:
        sys.exit(f"unknown segment {name}")
    for a in range(addr, addr + 4 * count, 4):
        if not seg.contains(a):
            break
        w = seg.word(a)
        sym = seg.symbols.get(a)
        if sym:
            print(f"{sym}:")
        print(f"  {a:08X}  {w:08X}  {rabbitizer.Instruction(w, vram=a).disassemble()}")


if __name__ == "__main__":
    main()
