"""Function discovery for PSX code segments (recursive descent + pointer scan)."""
import re
import struct
from dataclasses import dataclass, field

from mips import Ins

MAX_GAP_SCAN = 0x40000


@dataclass
class Function:
    start: int
    end: int = 0                  # exclusive, contiguous extent
    insns: dict = field(default_factory=dict)   # addr -> Ins (reachable only)
    labels: set = field(default_factory=set)
    jumptables: dict = field(default_factory=dict)  # jr addr -> [targets]
    name: str = ""


class Segment:
    def __init__(self, name, data, base, slot=None, resident=False, entry=None):
        self.name = name
        self.data = data
        self.base = base
        self.size = len(data) & ~3
        self.slot = slot
        self.resident = resident
        self.entry = entry
        self.funcs = {}           # start -> Function
        self.symbols = {}         # addr -> name
        self.extra_funcs = set()
        self.warnings = []

    def contains(self, a):
        return self.base <= a < self.base + self.size

    def word(self, a):
        return struct.unpack_from("<I", self.data, a - self.base)[0]

    def ins(self, a):
        return Ins(a, self.word(a))

    def is_prologue(self, a):
        if not self.contains(a) or a & 3:
            return False
        w = self.word(a)
        return (w >> 16) == 0x27BD and (w & 0x8000) != 0

    def looks_like_func(self, a):
        """Weaker test for addresses referenced by pointers/constants: a stack frame set
        up within the first few instructions (GCC schedules loads ahead of it), or the
        address directly follows another function's `jr ra` + delay slot."""
        if not self.contains(a) or a & 3:
            return False
        if any(self.is_prologue(a + 4 * k) for k in range(6)):
            return True
        return self.contains(a - 8) and self.word(a - 8) == 0x03E00008


def load_symbols(seg, path):
    """Read splat-style symbol_addrs: `name = 0x80012345; // type:func`."""
    rx = re.compile(r"^\s*(\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*;(.*)$")
    try:
        lines = open(path, encoding="utf-8").read().splitlines()
    except FileNotFoundError:
        return
    for ln in lines:
        m = rx.match(ln)
        if not m:
            continue
        name, addr, rest = m.group(1), int(m.group(2), 16), m.group(3)
        if not seg.contains(addr):
            continue
        seg.symbols[addr] = name
        if "type:func" in rest:
            seg.extra_funcs.add(addr)


def _walk(seg, start, known=frozenset()):
    """Recursive descent within one function.

    Follows branches, jump tables and `j` (as goto) except `j` to a known function
    start, which is a tail call. Overlap with other functions is allowed: duplicated
    code in C is still correct. Returns (insns, labels, jumptables, calls, tails) or None.
    """
    insns, labels, jts = {}, set(), {}
    calls, tails = set(), set()
    work = [start]
    seen = set()
    ds_only = set()          # addresses so far reached only as a delay slot
    while work:
        pc = work.pop()
        if pc in ds_only:
            # branched straight into a delay slot: that instruction is recorded already,
            # execution continues sequentially after it
            ds_only.discard(pc)
            pc += 4
        while True:
            if pc in seen:
                break
            if not seg.contains(pc):
                return None
            seen.add(pc)
            i = seg.ins(pc)
            if not i.valid:
                return None
            insns[pc] = i
            if i.has_delay:
                ds = pc + 4
                if not seg.contains(ds):
                    return None
                d = seg.ins(ds)
                if not d.valid or d.has_delay:
                    return None
                insns[ds] = d
                if ds not in seen:
                    ds_only.add(ds)
                seen.add(ds)
            n = i.name
            if i.is_branch:
                labels.add(i.target)
                if n in ("bltzal", "bgezal"):
                    calls.add(i.target)
                else:
                    work.append(i.target)
                if n == "beq" and i.rs == 0 and i.rt == 0:   # unconditional `b`
                    break
                pc += 8
                continue
            if n == "jal":
                if i.target != pc + 8:          # `jal .+8` only loads the PC into ra
                    calls.add(i.target)
                pc += 8
                continue
            if n == "jalr":
                pc += 8
                continue
            if n == "j":
                if (i.target in known and i.target != start) or not seg.contains(i.target):
                    tails.add(i.target)
                else:
                    labels.add(i.target)
                    work.append(i.target)
                break
            if n == "jr":
                if i.rs != 31:
                    tbl = _jumptable(seg, insns, pc, i.rs)
                    if tbl:
                        jts[pc] = tbl
                        for t in tbl:
                            labels.add(t)
                            work.append(t)
                break
            if n == "break":
                # traps (div-by-zero, crt0 "main returned") never fall through; code after
                # them is only reached via a branch around the trap.
                break
            pc += 4
    return insns, labels, jts, calls, tails


def _jumptable(seg, insns, jr_pc, reg):
    """Resolve the GCC switch idiom: lui at,hi; addu at,at,idx; lw reg,lo(at); jr reg."""
    lw = None
    a = jr_pc - 4
    for _ in range(8):
        i = insns.get(a)
        if i is None:
            break
        if i.name == "lw" and i.rt == reg:
            lw = i
            break
        a -= 4
    if lw is None:
        return None
    base_reg, lo = lw.rs, lw.simm
    hi = None
    count = None
    b = lw.addr - 4
    for _ in range(16):
        i = insns.get(b)
        if i is None:
            break
        if hi is None and i.rt == base_reg:
            if i.name == "lui":
                hi = i.imm << 16
            elif i.name == "addiu" and i.rs == base_reg:
                lo += i.simm
        if i.name == "sltiu" and count is None:
            count = i.imm
        b -= 4
    if hi is None:
        return None
    tbl = (hi + lo) & 0xFFFFFFFF
    if not seg.contains(tbl):
        return None
    targets = []
    n = count if count else 512
    for k in range(n):
        t = tbl + 4 * k
        if not seg.contains(t):
            break
        v = seg.word(t)
        if not seg.contains(v) or v & 3:
            break
        targets.append(v)
    return targets or None


def discover(seg, all_segments):
    resident = [s for s in all_segments if s.resident]
    seeds = set(seg.extra_funcs)
    if seg.entry:
        seeds.add(seg.entry)
    # Seed with every jal target inside the segment that lands on a prologue.
    for a in range(seg.base, seg.base + seg.size, 4):
        w = seg.word(a)
        if (w >> 26) == 3:
            t = ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if seg.contains(t):
                seeds.add(t)
    # Cross-segment seeds: any segment may call resident code directly; overlays may call
    # into the other slot (only trusted when the target looks like a prologue here).
    for other in all_segments:
        if other is seg:
            continue
        if other.slot is not None and other.slot == seg.slot:
            continue
        for t in _jal_targets(other):
            if seg.contains(t) and (seg.resident or seg.is_prologue(t)):
                seeds.add(t)
    # Pass 1: collect all function starts (jal targets, entry, symbols, pointer scan).
    starts = set()
    bad = set()
    pending = sorted(seeds)
    walked = {}
    while pending:
        while pending:
            a = pending.pop()
            if a in walked or a in bad:
                continue
            r = _walk(seg, a)
            if r is None:
                bad.add(a)
                continue
            walked[a] = r
            starts.add(a)
            pending.extend(c for c in r[3] if seg.contains(c))
        # pointer scan: data words pointing at prologues are function pointers (callbacks)
        code = set()
        for w in walked.values():
            code.update(w[0].keys())
        for p in range(seg.base, seg.base + seg.size, 4):
            if p in code:
                continue
            v = seg.word(p)
            if seg.contains(v) and v not in walked and v not in bad and seg.looks_like_func(v):
                pending.append(v)
        # lui/addiu address constants in code that point at prologues (callbacks, thread entries)
        for a in sorted(code):
            i = seg.ins(a)
            if i.name != "lui":
                continue
            for b in (a + 4, a + 8, a + 12):
                j = code and seg.contains(b) and seg.ins(b)
                if j and j.name == "addiu" and j.rs == i.rt:
                    v = ((i.imm << 16) + j.simm) & 0xFFFFFFFF
                    if seg.contains(v) and v not in walked and v not in bad and seg.looks_like_func(v):
                        pending.append(v)
                    break
        if not pending:
            # finally: any remaining prologue that decodes cleanly (unreferenced/dead code)
            for p in range(seg.base, seg.base + seg.size, 4):
                if p not in code and p not in walked and p not in bad and seg.is_prologue(p):
                    pending.append(p)
    # Pass 2: re-walk with the final start set so `j` to a function start is a tail call.
    known = frozenset(starts)
    for a in sorted(starts):
        insns, labels, jts, calls, tails = _walk(seg, a, known)
        f = Function(start=a, insns=insns, labels=labels, jumptables=jts)
        f.end = max(insns) + 4
        f.name = seg.symbols.get(a) or f"{seg.name}_{a:08X}"
        seg.funcs[a] = f
    return seg.funcs


_JAL_CACHE = {}


def _jal_targets(seg):
    """Call targets for seeding other segments: every jal, plus j that leaves `seg`
    (a cross-segment tail call)."""
    if seg.name not in _JAL_CACHE:
        ts = set()
        for a in range(seg.base, seg.base + seg.size, 4):
            w = seg.word(a)
            op = w >> 26
            if op in (2, 3):
                t = ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if op == 3 or not seg.contains(t):
                    ts.add(t)
        _JAL_CACHE[seg.name] = ts
    return _JAL_CACHE[seg.name]


def _owner(starts, a):
    import bisect
    k = bisect.bisect_right(starts, a) - 1
    return starts[k] if k >= 0 else None
