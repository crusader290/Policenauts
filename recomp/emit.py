"""Emit C source from analyzed segments.

Each MIPS function becomes `void f_<seg>_<ADDR>(CPU *c)`. Registers live in the CPU
struct; memory goes through the runtime's MEM_* accessors (runtime/psx.h).
"""
import os

SIG_WORDS = 8


def cname(seg, addr):
    return f"f_{seg.name}_{addr:08X}"


class Emitter:
    def __init__(self, segments, hooks=None):
        self.segs = segments
        self.resident = [s for s in segments if s.resident]
        self.unresolved = []
        # (segment name, addr) -> {"name": ..., "mode": "before"|"replace"}
        self.hooks = hooks or {}

    # ---- call target resolution ------------------------------------------------
    def resolve(self, seg, t):
        """Return C function name for a direct call target, or None if dynamic."""
        if seg.contains(t):
            return cname(seg, t) if t in seg.funcs else None
        for r in self.resident:
            if r.contains(t):
                return cname(r, t) if t in r.funcs else None
        return None

    def call(self, seg, t, pc):
        fn = self.resolve(seg, t)
        if fn:
            return f"{fn}(c);"
        self.unresolved.append((seg.name, pc, t))
        return f"call_dynamic(c, 0x{t:08X}u);"

    # ---- instruction translation -------------------------------------------------
    @staticmethod
    def R(n):
        return "0u" if n == 0 else f"c->r[{n}]"

    def simple(self, i):
        """Translate a non-control-flow instruction. Returns C statement string."""
        R = self.R
        n, rs, rt, rd, sa = i.name, i.rs, i.rt, i.rd, i.sa
        imm, simm = i.imm, i.simm
        ea = f"({R(rs)} + 0x{simm & 0xFFFFFFFF:X}u)" if simm else f"{R(rs)}"

        def wr(reg, expr):
            if reg == 0:
                return ""
            return f"c->r[{reg}] = {expr};"

        if n == "sll":
            if i.w == 0:
                return ""  # nop
            return wr(rd, f"{R(rt)} << {sa}")
        if n == "srl":
            return wr(rd, f"{R(rt)} >> {sa}")
        if n == "sra":
            return wr(rd, f"(uint32_t)((int32_t){R(rt)} >> {sa})")
        if n == "sllv":
            return wr(rd, f"{R(rt)} << ({R(rs)} & 31)")
        if n == "srlv":
            return wr(rd, f"{R(rt)} >> ({R(rs)} & 31)")
        if n == "srav":
            return wr(rd, f"(uint32_t)((int32_t){R(rt)} >> ({R(rs)} & 31))")
        if n in ("add", "addu"):
            return wr(rd, f"{R(rs)} + {R(rt)}")
        if n in ("sub", "subu"):
            return wr(rd, f"{R(rs)} - {R(rt)}")
        if n == "and":
            return wr(rd, f"{R(rs)} & {R(rt)}")
        if n == "or":
            return wr(rd, f"{R(rs)} | {R(rt)}")
        if n == "xor":
            return wr(rd, f"{R(rs)} ^ {R(rt)}")
        if n == "nor":
            return wr(rd, f"~({R(rs)} | {R(rt)})")
        if n == "slt":
            return wr(rd, f"(int32_t){R(rs)} < (int32_t){R(rt)}")
        if n == "sltu":
            return wr(rd, f"{R(rs)} < {R(rt)}")
        if n in ("addi", "addiu"):
            return wr(rt, f"{R(rs)} + 0x{simm & 0xFFFFFFFF:X}u")
        if n == "slti":
            return wr(rt, f"(int32_t){R(rs)} < {simm}")
        if n == "sltiu":
            return wr(rt, f"{R(rs)} < 0x{simm & 0xFFFFFFFF:X}u")
        if n == "andi":
            return wr(rt, f"{R(rs)} & 0x{imm:X}u")
        if n == "ori":
            return wr(rt, f"{R(rs)} | 0x{imm:X}u")
        if n == "xori":
            return wr(rt, f"{R(rs)} ^ 0x{imm:X}u")
        if n == "lui":
            return wr(rt, f"0x{imm << 16:X}u")
        if n == "mfhi":
            return wr(rd, "c->hi")
        if n == "mflo":
            return wr(rd, "c->lo")
        if n == "mthi":
            return f"c->hi = {R(rs)};"
        if n == "mtlo":
            return f"c->lo = {R(rs)};"
        if n == "mult":
            return f"MULT(c, {R(rs)}, {R(rt)});"
        if n == "multu":
            return f"MULTU(c, {R(rs)}, {R(rt)});"
        if n == "div":
            return f"DIV(c, {R(rs)}, {R(rt)});"
        if n == "divu":
            return f"DIVU(c, {R(rs)}, {R(rt)});"
        loads = {"lb": "(uint32_t)(int32_t)(int8_t)MEM_LB", "lbu": "(uint32_t)MEM_LB",
                 "lh": "(uint32_t)(int32_t)(int16_t)MEM_LH", "lhu": "(uint32_t)MEM_LH",
                 "lw": "MEM_LW"}
        if n in loads:
            if rt == 0:
                return f"(void){loads[n]}({ea});"
            return wr(rt, f"{loads[n]}({ea})")
        if n == "lwl":
            return f"c->r[{rt}] = MEM_LWL({ea}, {R(rt)});" if rt else f"(void)MEM_LW(({ea}) & ~3u);"
        if n == "lwr":
            return f"c->r[{rt}] = MEM_LWR({ea}, {R(rt)});" if rt else f"(void)MEM_LW(({ea}) & ~3u);"
        if n == "sb":
            return f"MEM_SB({ea}, (uint8_t){R(rt)});"
        if n == "sh":
            return f"MEM_SH({ea}, (uint16_t){R(rt)});"
        if n == "sw":
            return f"MEM_SW({ea}, {R(rt)});"
        if n == "swl":
            return f"MEM_SWL({ea}, {R(rt)});"
        if n == "swr":
            return f"MEM_SWR({ea}, {R(rt)});"
        if n == "mfc0":
            return wr(rt, f"cop0_read(c, {rd})")
        if n == "mtc0":
            return f"cop0_write(c, {rd}, {R(rt)});"
        if n == "rfe":
            return "cop0_rfe(c);"
        if n == "mfc2":
            return wr(rt, f"gte_read_data(c, {rd})")
        if n == "cfc2":
            return wr(rt, f"gte_read_ctrl(c, {rd})")
        if n == "mtc2":
            return f"gte_write_data(c, {rd}, {R(rt)});"
        if n == "ctc2":
            return f"gte_write_ctrl(c, {rd}, {R(rt)});"
        if n == "cop2":
            return f"gte_command(c, 0x{i.w & 0x1FFFFFF:07X}u);"
        if n == "lwc2":
            return f"gte_write_data(c, {rt}, MEM_LW({ea}));"
        if n == "swc2":
            return f"MEM_SW({ea}, gte_read_data(c, {rt}));"
        if n == "syscall":
            return f"psx_syscall(c, 0x{(i.w >> 6) & 0xFFFFF:X}u, 0x{i.addr:08X}u);"
        if n == "break":
            return f"psx_break(c, 0x{(i.w >> 6) & 0xFFFFF:X}u, 0x{i.addr:08X}u);"
        raise ValueError(f"unhandled {i}")

    def cond(self, i):
        R = self.R
        n = i.name
        if n == "beq":
            return "1" if i.rs == i.rt else f"{R(i.rs)} == {R(i.rt)}"
        if n == "bne":
            return f"{R(i.rs)} != {R(i.rt)}"
        if n == "blez":
            return f"(int32_t){R(i.rs)} <= 0"
        if n == "bgtz":
            return f"(int32_t){R(i.rs)} > 0"
        if n in ("bltz", "bltzal"):
            return f"(int32_t){R(i.rs)} < 0"
        if n in ("bgez", "bgezal"):
            return f"(int32_t){R(i.rs)} >= 0"
        raise ValueError(n)

    # ---- function emission -------------------------------------------------------
    def leave_to(self, seg, f, t, pc):
        """Control transfer to t that isn't a goto inside f: tail call."""
        if t in f.insns:
            if t <= pc:
                return f"{{ BACKEDGE(c); goto L_{t:08X}; }}"
            return f"goto L_{t:08X};"
        fn = self.resolve(seg, t)
        if fn:
            return f"{{ {fn}(c); return; }}"
        self.unresolved.append((seg.name, pc, t))
        return f"{{ jump_dynamic(c, 0x{t:08X}u); return; }}"

    def function(self, seg, f):
        out = []
        sym = seg.symbols.get(f.start)
        out.append(f"/* {seg.name} {f.start:08X}-{f.end:08X}{'  ' + sym if sym else ''} */")
        hook = self.hooks.get((seg.name, f.start))
        if hook:
            out.append(f"void {hook['name']}(CPU *c);")
            out.append(f"static void {cname(seg, f.start)}_orig(CPU *c);")
            out.append(f"void {cname(seg, f.start)}(CPU *c) {{")
            out.append(f"    {hook['name']}(c);")
            if hook["mode"] == "before":
                out.append(f"    {cname(seg, f.start)}_orig(c);")
            out.append("}")
            out.append(f"static void {cname(seg, f.start)}_orig(CPU *c) {{")
        else:
            out.append(f"void {cname(seg, f.start)}(CPU *c) {{")
        out.append(f"    psx_cur_func = 0x{f.start:08X}u;")
        ins = f.insns
        delay = set()
        for a, i in ins.items():
            if i.has_delay and a + 4 in ins:
                delay.add(a + 4)
        targets = set(t for t in f.labels if t in ins)
        for tbl in f.jumptables.values():
            targets.update(t for t in tbl if t in ins)
        for i in ins.values():
            if i.name == "j" and i.target in ins:
                targets.add(i.target)
        extra = []  # (label, delay-insn, continue addr) blocks for branches into delay slots
        body = []
        order = sorted(ins)
        for idx, a in enumerate(order):
            if a in delay and a not in targets:
                continue
            if a in delay and a in targets:
                # Reached directly: handled by the owning branch's emitted block.
                continue
            i = ins[a]
            if a in targets:
                body.append(f"L_{a:08X}:")
            if not i.has_delay:
                s = self.simple(i)
                if s:
                    body.append("    " + s)
                if a + 4 not in ins and i.name != "break":
                    # fell off the end of the known code: continue into whatever is next
                    body.append("    " + self.leave_to(seg, f, a + 4, a))
                continue
            d = ins.get(a + 4)
            ds = self.simple(d) if d else ""
            ds_label = (a + 4) in targets
            body.extend(self.control(seg, f, i, ds))
            after = a + 8
            if ds_label:
                # someone branches straight into this delay slot: execute it and fall through
                body.append(f"    goto L_{after:08X}_ft;")
                body.append(f"L_{a + 4:08X}:")
                if ds:
                    body.append("    " + ds)
                body.append(f"L_{after:08X}_ft:;")
            if (ds_label or not self._is_terminator(i)) and after not in ins:
                body.append("    " + self.leave_to(seg, f, after, a))
        out.extend(body)
        out.append("}")
        return "\n".join(out)

    @staticmethod
    def _is_terminator(i):
        if i.name in ("j", "jr"):
            return True
        if i.name == "beq" and i.rs == 0 and i.rt == 0:
            return True
        return False

    def control(self, seg, f, i, ds):
        R = self.R
        a = i.addr
        n = i.name
        out = []
        dsl = f"        {ds}" if ds else None
        if i.is_branch:
            link = n in ("bltzal", "bgezal")
            if n == "beq" and i.rs == 0 and i.rt == 0:
                if ds:
                    out.append("    " + ds)
                out.append("    " + self.leave_to(seg, f, i.target, a))
                return out
            out.append(f"    {{ int cond = {self.cond(i)};")
            if link:
                out.append(f"        c->r[31] = 0x{a + 8:08X}u;")
            if dsl:
                out.append(dsl)
            if link:
                out.append(f"        if (cond) {self.call(seg, i.target, a)} }}")
            else:
                out.append(f"        if (cond) {self.leave_to(seg, f, i.target, a)} }}")
            return out
        if n == "jal":
            out.append(f"    c->r[31] = 0x{a + 8:08X}u;")
            if ds:
                out.append("    " + ds)
            out.append("    " + self.call(seg, i.target, a))
            return out
        if n == "j":
            if ds:
                out.append("    " + ds)
            out.append("    " + self.leave_to(seg, f, i.target, a))
            return out
        if n == "jalr":
            prev = f.insns.get(a - 4)
            if i.rs == 10 and prev is not None and (prev.w & 0xFFFFFF00) == 0x240A0000 \
                    and (prev.w & 0xFF) in (0xA0, 0xB0, 0xC0):
                # inline BIOS call: li t2,0xB0 ; jalr t2 ; li t1,N
                if i.rd:
                    out.append(f"    c->r[{i.rd}] = 0x{a + 8:08X}u;")
                if ds:
                    out.append("    " + ds)
                out.append(f"    bios_call(c, 0x{prev.w & 0xFF:X}u);")
                return out
            out.append(f"    {{ uint32_t tgt = {R(i.rs)};")
            if i.rd:
                out.append(f"        c->r[{i.rd}] = 0x{a + 8:08X}u;")
            if dsl:
                out.append(dsl)
            out.append("        call_dynamic(c, tgt); }")
            return out
        if n == "jr":
            if i.rs == 31:
                if ds:
                    out.append("    " + ds)
                out.append("    return;")
                return out
            prev = f.insns.get(a - 4)
            if i.rs == 10 and prev is not None and (prev.w & 0xFFFFFF00) == 0x240A0000 \
                    and (prev.w & 0xFF) in (0xA0, 0xB0, 0xC0):
                if ds:
                    out.append("    " + ds)
                out.append(f"    bios_call(c, 0x{prev.w & 0xFF:X}u); return;")
                return out
            out.append(f"    {{ uint32_t tgt = {R(i.rs)};")
            if dsl:
                out.append(dsl)
            tbl = f.jumptables.get(a)
            if tbl:
                out.append("        switch (tgt) {")
                for t in sorted(set(tbl)):
                    if t in f.insns:
                        out.append(f"        case 0x{t:08X}u: goto L_{t:08X};")
                out.append("        default: break; }")
            out.append("        jump_dynamic(c, tgt); return; }")
            return out
        raise ValueError(n)

    # ---- files -----------------------------------------------------------------
    def write(self, outdir, per_file=250):
        os.makedirs(outdir, exist_ok=True)
        files = []
        hdr = ["/* generated by recomp/emit.py - do not edit */", "#pragma once",
               '#include "psx.h"', ""]
        for seg in self.segs:
            for s in sorted(seg.funcs):
                hdr.append(f"void {cname(seg, s)}(CPU *c);")
        open(os.path.join(outdir, "funcs.h"), "w").write("\n".join(hdr) + "\n")
        for seg in self.segs:
            starts = sorted(seg.funcs)
            for k in range(0, len(starts), per_file):
                fn = f"{seg.name}_{k // per_file:02d}.c"
                files.append(fn)
                with open(os.path.join(outdir, fn), "w") as o:
                    o.write("/* generated by recomp/emit.py - do not edit */\n")
                    o.write('#include "funcs.h"\n\n')
                    for s in starts[k:k + per_file]:
                        o.write(self.function(seg, seg.funcs[s]))
                        o.write("\n\n")
        files.append("lookup.c")
        self.write_lookup(os.path.join(outdir, "lookup.c"))
        return files

    def signature(self, seg):
        """Pick code words that distinguish this overlay from the others in its slot."""
        if seg.resident:
            return []
        others = [s for s in self.segs if s.slot == seg.slot and s is not seg]
        sig = []
        for a in sorted(seg.funcs):
            for off in range(0, 16, 4):
                p = a + off
                if not seg.contains(p):
                    continue
                w = seg.word(p)
                if all(not o.contains(p) or o.word(p) != w for o in others):
                    sig.append((p, w))
                    break
            if len(sig) >= SIG_WORDS:
                break
        return sig

    def write_lookup(self, path):
        o = open(path, "w")
        o.write("/* generated by recomp/emit.py - do not edit */\n")
        o.write('#include "funcs.h"\n\n')
        for seg in self.segs:
            o.write(f"static const FuncEntry funcs_{seg.name}[] = {{\n")
            for s in sorted(seg.funcs):
                o.write(f"    {{ 0x{s:08X}u, {cname(seg, s)} }},\n")
            o.write("};\n")
            sig = self.signature(seg)
            o.write(f"static const SigWord sig_{seg.name}[] = {{\n")
            for p, w in sig:
                o.write(f"    {{ 0x{p:08X}u, 0x{w:08X}u }},\n")
            if not sig:
                o.write("    { 0, 0 },\n")
            o.write("};\n\n")
        o.write("const SegmentInfo psx_segments[] = {\n")
        for seg in self.segs:
            slot = f"'{seg.slot}'" if seg.slot else "0"
            nsig = len(self.signature(seg))
            o.write(f'    {{ "{seg.name}", 0x{seg.base:08X}u, 0x{seg.size:X}u, {slot}, '
                    f"funcs_{seg.name}, {len(seg.funcs)}, sig_{seg.name}, {nsig} }},\n")
        o.write("};\n")
        o.write(f"const int psx_segment_count = {len(self.segs)};\n")
        o.close()
