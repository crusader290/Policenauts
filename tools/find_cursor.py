"""Locate the white mouse pointer in BMP frame dumps (debug aid for scripted play).

  python tools/find_cursor.py frame_*.bmp

Prints the top-left of the bright-white blob nearest the frame's pointer shape.
"""
import sys
sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from contact_sheet import read_bmp


def find(path):
    w, h, rows = read_bmp(path)
    pts = [(x, y) for y in range(h) for x in range(w)
           if min(rows[y][x]) >= 240]
    if not pts:
        return None
    # the pointer is a small cluster; take the densest 12x16 window
    best, bc = None, 0
    for (x, y) in pts:
        n = sum(1 for (a, b) in pts if 0 <= a - x < 10 and 0 <= b - y < 14)
        if n > bc:
            best, bc = (x, y), n
    return best, bc, w, h


for p in sys.argv[1:]:
    print(p, find(p))
