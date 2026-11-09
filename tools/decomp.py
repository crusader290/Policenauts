"""Decompile one recompiled function to readable C with m2c (needs tools/m2c).

  python tools/decomp.py <segment> <addr_hex> [--asm]

Uses the recompiler's own function discovery for extent and branch labels, prints
GNU-as style assembly in the form m2c expects, and runs m2c on it.
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "recomp"))

import rabbitizer  # noqa: E402

from analyze import discover  # noqa: E402
from config import load_segments  # noqa: E402

BRANCH_RX = re.compile(r"\. \+ 4 \+ \((-?0x[0-9A-Fa-f]+) << 2\)")


def asm_for(seg, f):
    lines = [".set noat", ".set noreorder", "", f"glabel func_{f.start:08X}"]
    for a in sorted(f.insns):
        if a in f.labels and a != f.start:
            lines.append(f".L{a:08X}:")
        ins = rabbitizer.Instruction(seg.word(a), vram=a)
        text = ins.disassemble()
        m = BRANCH_RX.search(text)
        if m:
            tgt = (a + 4 + (int(m.group(1), 16) << 2)) & 0xFFFFFFFF
            text = BRANCH_RX.sub(f".L{tgt:08X}", text)
            if tgt not in f.insns:
                text = text.replace(f".L{tgt:08X}", f"func_{tgt:08X}")
        if text.startswith("j ") and "func_" in text:
            tgt = int(text.split("func_")[1], 16)
            if tgt in f.insns:
                text = f"j .L{tgt:08X}"
        lines.append(f"/* {a:08X} {seg.word(a):08X} */ {text}")
    # labels used as jump-table / goto targets that are not branch targets
    return "\n".join(lines) + "\n"


def main():
    name, addr = sys.argv[1], int(sys.argv[2], 16)
    segs = load_segments()
    seg = next(s for s in segs if s.name == name)
    discover(seg, segs)
    f = seg.funcs.get(addr)
    if f is None:
        sys.exit(f"no function at {addr:08X} in {name}")
    for tbl in f.jumptables.values():
        f.labels.update(tbl)
    for i in f.insns.values():
        if i.name == "j" and i.target in f.insns:
            f.labels.add(i.target)
    asm = asm_for(seg, f)
    if "--asm" in sys.argv:
        print(asm)
        return
    with tempfile.NamedTemporaryFile("w", suffix=".s", delete=False) as t:
        t.write(asm)
        path = t.name
    m2c = os.path.join(ROOT, "tools", "m2c", "m2c.py")
    r = subprocess.run([sys.executable, m2c, "--target", "mips-gcc-c", path],
                       capture_output=True, text=True)
    os.unlink(path)
    print(r.stdout or r.stderr)


if __name__ == "__main__":
    main()
