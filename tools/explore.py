"""Generate a PSX_INPUT script that visits screen points and acts on each.

  python tools/explore.py <out.txt> <start_s> x,y[,n] ...

Starts with the autosave load (title -> Continue -> Autosave File) unless --no-load.
For each point: park the pointer in the top-left corner, move to (x, y) (frame dump
coordinates), press circle (verb menu), circle again (first verb), then n more circle
presses 2 s apart to advance dialogue (default 5), then cross twice to back out.
Prints the virtual time at which each point's verb is chosen.
"""
import sys

CORNER = (16, 9)


def hold_for(d):
    """seconds of D-pad hold for d pixels (accelerating pointer, measured)"""
    if d <= 0:
        return 0
    if d < 48:
        return 0.5 * (d / 48) ** 0.5
    return 0.5 + (d - 48) / 120


def main():
    args = sys.argv[1:]
    load = True
    if "--no-load" in args:
        args.remove("--no-load")
        load = False
    out, t = args[0], float(args[1])
    ev = ["start@30", "circle@38", "circle@42", "circle@46"] if load else []
    for spec in args[2:]:
        p = [int(v) for v in spec.split(",")]
        x, y = p[0], p[1]
        n = p[2] if len(p) > 2 else 5
        ev += [f"left@{t:.2f}-{t + 3.2:.2f}", f"up@{t:.2f}-{t + 3.2:.2f}"]
        t += 3.5
        hx, hy = hold_for(x - CORNER[0]), hold_for(y - CORNER[1])
        if hx:                                   # one axis at a time (calibrated that way)
            ev.append(f"right@{t:.2f}-{t + hx:.2f}")
            t += hx + 0.3
        if hy:
            ev.append(f"down@{t:.2f}-{t + hy:.2f}")
            t += hy + 0.3
        print(f"  pointer settled at {t:.1f}s")
        t += 0.2
        ev.append(f"circle@{t:.2f}")
        t += 1.2
        ev.append(f"circle@{t:.2f}")
        print(f"{x},{y}: verb at {t:.1f}s")
        for _ in range(n):
            t += 2.0
            ev.append(f"circle@{t:.2f}")
        t += 1.5
        ev += [f"cross@{t:.2f}", f"cross@{t + 1:.2f}"]
        t += 2.5
    open(out, "w").write("\n".join(ev) + "\n")
    print(f"end {t:.1f}s")


main()
