"""Policenauts static recompiler driver.

  python recomp/recomp.py [--out generated]

Reads recomp/segments.toml + config/symbol_addrs*.txt, discovers functions and
writes C into the output directory (default: generated/).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from analyze import discover  # noqa: E402
from config import ROOT, load_segments  # noqa: E402
from emit import Emitter  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "generated"))
    args = ap.parse_args()

    segs = load_segments()
    for s in segs:
        discover(s, segs)
        print(f"{s.name:8s} {len(s.funcs):5d} functions")
        for w in s.warnings:
            print("   warn:", w)
    hooks = {}
    hooks_path = os.path.join(ROOT, "recomp", "hooks.toml")
    if os.path.exists(hooks_path):
        import tomllib
        for h in tomllib.load(open(hooks_path, "rb")).get("hook", []):
            seg = next((s for s in segs if s.name == h["segment"]), None)
            if seg is None or h["addr"] not in seg.funcs:
                sys.exit(f"hook {h['name']}: no function {h['addr']:08X} in {h['segment']}")
            hooks[(h["segment"], h["addr"])] = {"name": h["name"], "mode": h.get("mode", "replace")}
    em = Emitter(segs, hooks)
    files = em.write(args.out)
    with open(os.path.join(args.out, "unresolved.txt"), "w") as f:
        for seg, pc, t in sorted(set(em.unresolved)):
            f.write(f"{seg} {pc:08X} -> {t:08X}\n")
    with open(os.path.join(args.out, "sources.txt"), "w", newline="\n") as f:
        f.write("\n".join(files) + "\n")
    print(f"wrote {len(files)} files, {len(set(em.unresolved))} dynamic call sites")


if __name__ == "__main__":
    main()
