"""R3000A instruction field decoding (self-contained, no external deps)."""

REG_NAMES = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
             "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
             "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
             "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

SPECIAL = {0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav",
           0x08: "jr", 0x09: "jalr", 0x0C: "syscall", 0x0D: "break",
           0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo",
           0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu",
           0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu",
           0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu"}

PRIMARY = {0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz",
           0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi",
           0x0D: "ori", 0x0E: "xori", 0x0F: "lui",
           0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu",
           0x26: "lwr", 0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr",
           0x32: "lwc2", 0x3A: "swc2"}

BRANCHES = {"beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal"}


class Ins:
    __slots__ = ("addr", "w", "op", "rs", "rt", "rd", "sa", "imm", "simm", "target", "name")

    def __init__(self, addr, w):
        self.addr = addr
        self.w = w
        self.op = w >> 26
        self.rs = (w >> 21) & 31
        self.rt = (w >> 16) & 31
        self.rd = (w >> 11) & 31
        self.sa = (w >> 6) & 31
        self.imm = w & 0xFFFF
        self.simm = self.imm - 0x10000 if self.imm & 0x8000 else self.imm
        self.target = None
        self.name = self._decode()

    def _decode(self):
        op, w = self.op, self.w
        if op == 0:
            fn = w & 0x3F
            name = SPECIAL.get(fn)
            if name is None:
                # ASPSX emits a MIPS II trap (0x00001770) for the div INT_MIN/-1 overflow
                # check; on R3000 it is a reserved-instruction trap, so treat it like break.
                if 0x30 <= fn <= 0x36 and self.rs == 0 and self.rt == 0:
                    return "break"
                return None
            # reject encodings with garbage in must-be-zero fields
            if name in ("jr",) and (w & 0x001FFFC0):
                return None
            if name in ("mfhi", "mflo") and (w & 0x03FF07C0):
                return None
            if name in ("mthi", "mtlo") and (w & 0x001FFFC0):
                return None
            if name in ("mult", "multu", "div", "divu") and (w & 0x0000FFC0):
                return None
            # sll/srl/sra ignore rs; hand-written code in the font module leaves it set
            # (e.g. 0x018C6200 = sll $t4,$t4,8 with rs=$t4), so don't reject it.
            if name in ("add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu",
                        "sllv", "srlv", "srav") and self.sa:
                return None
            return name
        if op == 1:
            self.target = (self.addr + 4 + (self.simm << 2)) & 0xFFFFFFFF
            return {0x00: "bltz", 0x01: "bgez", 0x10: "bltzal", 0x11: "bgezal"}.get(self.rt)
        if op in (2, 3):
            self.target = ((self.addr + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            return PRIMARY[op]
        if op == 0x10:  # COP0
            sub = self.rs
            if sub == 0x00:
                return "mfc0"
            if sub == 0x04:
                return "mtc0"
            if sub == 0x10 and (w & 0x3F) == 0x10:
                return "rfe"
            return None
        if op == 0x12:  # COP2 / GTE
            sub = self.rs
            if sub & 0x10:
                return "cop2"
            return {0x00: "mfc2", 0x02: "cfc2", 0x04: "mtc2", 0x06: "ctc2"}.get(sub)
        name = PRIMARY.get(op)
        if name in BRANCHES or name in ("beq", "bne", "blez", "bgtz"):
            self.target = (self.addr + 4 + (self.simm << 2)) & 0xFFFFFFFF
        if name in ("blez", "bgtz") and self.rt:
            return None
        if name == "lui" and self.rs:
            return None
        return name

    @property
    def valid(self):
        return self.name is not None

    @property
    def is_branch(self):
        return self.name in BRANCHES

    @property
    def has_delay(self):
        return self.name in BRANCHES or self.name in ("j", "jal", "jr", "jalr")

    def __repr__(self):
        return f"<{self.addr:08x} {self.name} {self.w:08x}>"
