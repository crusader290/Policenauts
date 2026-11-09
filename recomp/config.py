"""Load recomp/segments.toml into Segment objects."""
import os
import tomllib

from analyze import Segment, load_symbols

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_segments(path=None):
    path = path or os.path.join(ROOT, "recomp", "segments.toml")
    cfg = tomllib.load(open(path, "rb"))
    segs = []
    for s in cfg["segment"]:
        data = open(os.path.join(ROOT, s["file"]), "rb").read()[s.get("skip", 0):]
        seg = Segment(s["name"], data, s["base"], slot=s.get("slot"),
                      resident=s.get("resident", False), entry=s.get("entry"))
        load_symbols(seg, os.path.join(ROOT, "config", f"symbol_addrs.{seg.name}.txt"))
        load_symbols(seg, os.path.join(ROOT, "config", "symbol_addrs.txt"))
        segs.append(seg)
    return segs
