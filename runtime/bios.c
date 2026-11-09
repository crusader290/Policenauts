/* High-level emulation of the PS1 BIOS (A0/B0/C0 tables, syscalls) for recompiled code.
 *
 * Threads
 * -------
 * The Konami kernel manipulates BIOS TCBs directly: it reads the process control block
 * (0x108) and TCB array (0x110), copies whole 0xC0-byte TCB images in and out of the
 * BIOS slots and rewrites the "current TCB" pointer itself. So TCBs live in emulated RAM
 * with the real layout and ChangeTh does what the BIOS does: save the registers into
 * whatever TCB is current, switch the pointer, load the target image.
 *
 * Execution state that isn't in registers (the recompiled call stack) lives in a host
 * fiber. Each saved image is tagged in two unused TCB words with the fiber that owns it
 * and that fiber's suspend count, so the tag travels with the image when the kernel
 * copies it around. Loading a tagged image resumes its fiber; an untagged image (fresh
 * OpenTh, or one the kernel built itself) starts a new fiber at the image's EPC.
 *
 * Interrupts
 * ----------
 * Psy-Q libetc installs its IRQ entry with SetCustomExitFromException(jmp_buf). On an
 * interrupt the BIOS longjmps there; the code after that setjmp calls the dispatcher.
 * A recompiled function can't be resumed mid-way, so the runtime maps the resume PC to
 * the dispatcher function (see irq_resume_table) and calls it on a separate CPU context
 * built from the jmp_buf. ReturnFromException unwinds back to the interrupted point.
 */
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef OpenEvent
#include "psx.h"
#include "bios.h"

/* ---- low-memory layout (BIOS kernel area, unused by the game) ------------------- */
#define A_PCB        0x8000E000u
#define A_TCB        0x8000E100u
#define TCB_SIZE     0xC0u
#define N_TCB        4u               /* SYSTEM.CNF: TCB = 4 */
#define A_EVCB       0x8000E500u
#define EVCB_SIZE    0x1Cu
#define N_EVCB       24u              /* SYSTEM.CNF: EVENT = 16 (+ BIOS internal) */
#define EXC_STACK    0x8000FFF0u      /* event callbacks run on this stack */
/* Function tables handed out by GetB0Table/GetC0Table (at their real BIOS addresses).
 * Libraries patch the code these point at (e.g. Psy-Q's exception-entry fix-up); under
 * HLE that code never runs, so every entry points at its own scratch slot. */
#define A_C0TBL      0x80000674u
#define A_B0TBL      0x80000874u
#define A_PATCHAREA  0x8000A000u      /* 0x80 bytes per table entry */

/* TCB fields */
#define TCB_STATUS   0x00
#define TCB_REGS     0x08
#define TCB_EPC      0x88
#define TCB_HI       0x8C
#define TCB_LO       0x90
#define TCB_SR       0x94
#define TCB_CAUSE    0x98
#define TCB_TAGMAGIC 0xB8
#define TCB_TAG      0xBC
#define TCB_FREE     0x1000u
#define TCB_USED     0x4000u
#define FIBER_MAGIC  0x52424946u      /* "FIBR" */

/* EvCB fields */
#define EV_CLASS 0x00
#define EV_STATUS 0x04
#define EV_SPEC 0x08
#define EV_MODE 0x0C
#define EV_FUNC 0x10
#define EvStUNUSED  0x0000u
#define EvStWAIT    0x1000u
#define EvStACTIVE  0x2000u
#define EvStALREADY 0x4000u
#define EvMdINTR    0x1000u
#define EvMdNOINTR  0x2000u

static int trace_bios;

static inline uint32_t R32(uint32_t a) { return MEM_LW(a); }
static inline void W32(uint32_t a, uint32_t v) { MEM_SW(a, v); }

/* ---- COP0 ------------------------------------------------------------------------ */
static uint32_t cop0_sr = 0x00000000u, cop0_cause, cop0_epc;

uint32_t cop0_read(CPU *c, int reg) {
    (void)c;
    switch (reg) {
    case 12: return cop0_sr;
    case 13: return cop0_cause;
    case 14: return cop0_epc;
    case 15: return 0x00000002u;   /* PRId: R3000A */
    default: return 0;
    }
}
void cop0_write(CPU *c, int reg, uint32_t v) {
    (void)c;
    if (reg == 12) cop0_sr = v;
    else if (reg == 13) cop0_cause = (cop0_cause & ~0x300u) | (v & 0x300u);
}
void cop0_rfe(CPU *c) { (void)c; cop0_sr = (cop0_sr & ~0xFu) | ((cop0_sr >> 2) & 0xFu); }

int psx_irq_enabled(void) { return (cop0_sr & 0x401u) == 0x401u; }

/* ---- fibers ---------------------------------------------------------------------- */
typedef struct Fiber {
    void *handle;
    CPU cpu;
    CPU *active;           /* register struct in use where the fiber was suspended */
    uint32_t entry, entry_ra;
    uint32_t seq;          /* bumped each time the fiber is suspended */
    int alive;
} Fiber;

#define MAX_FIBERS 64
static Fiber fibers[MAX_FIBERS];
static int cur_fiber;

static void tag_image(uint32_t tcb, int fid) {
    W32(tcb + TCB_TAGMAGIC, FIBER_MAGIC);
    W32(tcb + TCB_TAG, ((uint32_t)fid << 16) | (fibers[fid].seq & 0xFFFFu));
}

static void save_image(const CPU *c, uint32_t tcb, uint32_t resume_pc) {
    for (int r = 0; r < 32; r++) W32(tcb + TCB_REGS + 4 * r, r ? c->r[r] : 0);
    W32(tcb + TCB_EPC, resume_pc);
    W32(tcb + TCB_HI, c->hi);
    W32(tcb + TCB_LO, c->lo);
    /* stored as the exception handler would see it (IEc pushed into IEp) */
    W32(tcb + TCB_SR, (cop0_sr & ~0x3Fu) | ((cop0_sr & 0xFu) << 2));
}

static void load_image(CPU *c, uint32_t tcb) {
    for (int r = 1; r < 32; r++) c->r[r] = R32(tcb + TCB_REGS + 4 * r);
    c->r[0] = 0;
    c->hi = R32(tcb + TCB_HI);
    c->lo = R32(tcb + TCB_LO);
    uint32_t sr = R32(tcb + TCB_SR);
    cop0_sr = (sr & ~0xFu) | ((sr >> 2) & 0xFu);   /* ReturnFromException's rfe */
}

static void WINAPI fiber_main(void *param) {
    Fiber *f = param;
    int id = (int)(f - fibers);
    if (trace_bios) fprintf(stderr, "[fiber %d] start pc=%08X sp=%08X\n", id, f->entry, f->cpu.r[29]);
    call_dynamic(&f->cpu, f->entry);
    /* entry function returned: real hardware jumps to the ra the thread started with */
    if (f->entry_ra) call_dynamic(&f->cpu, f->entry_ra);
    fprintf(stderr, "fatal: thread on fiber %d (entry %08X) returned to ra=%08X\n",
            id, f->entry, f->entry_ra);
    exit(1);
}

static int alloc_fiber(void) {
    for (int i = 1; i < MAX_FIBERS; i++)
        if (!fibers[i].alive) return i;
    return -1;
}

/* Delete fibers whose images no longer exist anywhere in RAM. */
static void collect_fibers(void) {
    uint8_t live[MAX_FIBERS] = {0};
    live[cur_fiber] = 1;
    for (uint32_t off = 0; off + 8 <= PSX_RAM_SIZE; off += 4) {
        uint32_t m;
        memcpy(&m, &psx_ram[off], 4);
        if (m != FIBER_MAGIC) continue;
        uint32_t tag;
        memcpy(&tag, &psx_ram[off + 4], 4);
        uint32_t id = tag >> 16;
        if (id < MAX_FIBERS && fibers[id].alive && (tag & 0xFFFFu) == (fibers[id].seq & 0xFFFFu))
            live[id] = 1;
    }
    for (int i = 1; i < MAX_FIBERS; i++) {
        if (fibers[i].alive && !live[i]) {
            if (trace_bios) fprintf(stderr, "[fiber %d] collected\n", i);
            DeleteFiber(fibers[i].handle);
            fibers[i].handle = NULL;
            fibers[i].alive = 0;
        }
    }
}

/* Make `tcb` the running context. Returns once the calling fiber is resumed. */
static void switch_to_image(CPU *c, uint32_t tcb) {
    uint32_t magic = R32(tcb + TCB_TAGMAGIC), tag = R32(tcb + TCB_TAG);
    uint32_t id = tag >> 16;
    int target;
    if (magic == FIBER_MAGIC && id < MAX_FIBERS && fibers[id].alive &&
        (tag & 0xFFFFu) == (fibers[id].seq & 0xFFFFu)) {
        target = (int)id;
        if (target == cur_fiber) {     /* switched to ourselves */
            load_image(c, tcb);
            return;
        }
        load_image(fibers[target].active, tcb);
    } else {
        if (magic == FIBER_MAGIC)
            fprintf(stderr, "warning: stale TCB image at %08X (fiber %u seq %u) - starting fresh\n",
                    tcb, id, tag & 0xFFFFu);
        target = alloc_fiber();
        if (target < 0) { fprintf(stderr, "fatal: out of fibers\n"); exit(1); }
        Fiber *f = &fibers[target];
        memset(&f->cpu, 0, sizeof f->cpu);
        load_image(&f->cpu, tcb);
        f->entry = R32(tcb + TCB_EPC);
        f->entry_ra = f->cpu.r[31];
        f->seq = 0;
        f->alive = 1;
        f->active = &f->cpu;
        f->handle = CreateFiber(256 * 1024, fiber_main, f);
        if (!f->handle) { fprintf(stderr, "fatal: CreateFiber failed\n"); exit(1); }
    }
    if (trace_bios)
        fprintf(stderr, "[ChangeTh] fiber %d -> %d (tcb %08X)\n", cur_fiber, target, tcb);
    fibers[cur_fiber].active = c;
    cur_fiber = target;
    SwitchToFiber(fibers[target].handle);
}

/* Save the running context into the current TCB (as the BIOS does on ChangeTh and on
 * exception entry). Returns the TCB address. */
static uint32_t suspend_current(CPU *c, uint32_t resume_pc) {
    uint32_t tcb = R32(A_PCB);
    fibers[cur_fiber].seq++;
    save_image(c, tcb, resume_pc);
    tag_image(tcb, cur_fiber);
    return tcb;
}

/* ---- threads (B0:0E/0F/10) ------------------------------------------------------- */
static uint32_t OpenTh(uint32_t pc, uint32_t sp, uint32_t gp) {
    collect_fibers();
    for (uint32_t i = 0; i < N_TCB; i++) {
        uint32_t t = A_TCB + i * TCB_SIZE;
        if (R32(t + TCB_STATUS) == TCB_USED) continue;
        for (uint32_t o = 0; o < TCB_SIZE; o += 4) W32(t + o, 0);
        W32(t + TCB_STATUS, TCB_USED);
        W32(t + TCB_REGS + 4 * 28, gp);
        W32(t + TCB_REGS + 4 * 29, sp);
        W32(t + TCB_REGS + 4 * 30, sp);
        W32(t + TCB_EPC, pc);
        W32(t + TCB_SR, 0x0404u);
        return 0xFF000000u | i;
    }
    return 0xFFFFFFFFu;
}

static uint32_t CloseTh(uint32_t h) {
    uint32_t i = h & 0xFFFFu;
    if (i < N_TCB) W32(A_TCB + i * TCB_SIZE + TCB_STATUS, TCB_FREE);
    return 1;
}

static void ChangeTh(CPU *c, uint32_t h) {
    uint32_t i = h & 0xFFFFu;
    if (i >= N_TCB) { c->r[2] = 0; return; }
    c->r[2] = 1;                       /* value seen when this context resumes */
    suspend_current(c, c->r[31]);
    uint32_t target = A_TCB + i * TCB_SIZE;
    W32(A_PCB, target);
    switch_to_image(c, target);
}

/* ---- events (B0:07..0D, 20) ------------------------------------------------------ */
static void run_callback(CPU *c, uint32_t func) {
    CPU cb = *c;
    cb.r[29] = EXC_STACK;
    cb.r[31] = 0;
    call_dynamic(&cb, func);
}

static uint32_t OpenEvent(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func) {
    for (uint32_t i = 0; i < N_EVCB; i++) {
        uint32_t e = A_EVCB + i * EVCB_SIZE;
        if (R32(e + EV_STATUS) != EvStUNUSED) continue;
        W32(e + EV_CLASS, cls);
        W32(e + EV_STATUS, EvStWAIT);
        W32(e + EV_SPEC, spec);
        W32(e + EV_MODE, mode);
        W32(e + EV_FUNC, func);
        return 0xF1000000u | i;
    }
    return 0xFFFFFFFFu;
}

static uint32_t ev_addr(uint32_t h) {
    uint32_t i = h & 0xFFFFu;
    return i < N_EVCB ? A_EVCB + i * EVCB_SIZE : 0;
}

void bios_deliver_event(CPU *c, uint32_t cls, uint32_t spec) {
    for (uint32_t i = 0; i < N_EVCB; i++) {
        uint32_t e = A_EVCB + i * EVCB_SIZE;
        if (R32(e + EV_STATUS) != EvStACTIVE || R32(e + EV_CLASS) != cls ||
            R32(e + EV_SPEC) != spec)
            continue;
        if (R32(e + EV_MODE) == EvMdINTR) {
            uint32_t f = R32(e + EV_FUNC);
            if (f) run_callback(c, f);
        } else {
            W32(e + EV_STATUS, EvStALREADY);
        }
    }
}

static uint32_t TestEvent(uint32_t h) {
    uint32_t e = ev_addr(h);
    if (e && R32(e + EV_STATUS) == EvStALREADY) {
        W32(e + EV_STATUS, EvStACTIVE);
        return 1;
    }
    return 0;
}

static uint32_t WaitEvent(CPU *c, uint32_t h) {
    uint32_t e = ev_addr(h);
    if (!e) return 0;
    uint32_t st = R32(e + EV_STATUS);
    if (st != EvStACTIVE && st != EvStALREADY) return 0;
    while (R32(e + EV_STATUS) != EvStALREADY) psx_idle(c);
    W32(e + EV_STATUS, EvStACTIVE);
    return 1;
}

/* ---- exception hook (B0:17/18/19) ------------------------------------------------ */
static uint32_t custom_exit;        /* jmp_buf address from SetCustomExitFromException */

/* PCs right after `setjmp(hook_buf)` in libetc, mapped to the dispatcher it then calls. */
static const struct { uint32_t resume_pc, handler; } irq_resume_table[] = {
    { 0x80027490u, 0x80027504u },   /* kernel: ResetCallback -> intr dispatcher */
};

static void *irq_return_buf[5];
static int in_exception;

static uint32_t rcnt_clear[4] = { 1, 1, 1, 1 };   /* ChangeClearRCnt: BIOS acks the IRQ */

void bios_raise_exception(CPU *c) {
    if (in_exception) return;
    uint32_t handler = 0, jb = custom_exit;
    if (jb) {
        uint32_t pc = R32(jb + 0);
        for (size_t i = 0; i < sizeof irq_resume_table / sizeof irq_resume_table[0]; i++)
            if (irq_resume_table[i].resume_pc == pc) handler = irq_resume_table[i].handler;
        if (!handler) {
            fprintf(stderr, "fatal: exception hook resumes at unknown pc %08X\n", pc);
            exit(1);
        }
    }
    /* BIOS exception entry: the interrupted context is saved into the current TCB */
    uint32_t tcb = suspend_current(c, c->r[31]);
    cop0_sr = (cop0_sr & ~0x3Fu) | ((cop0_sr & 0xFu) << 2);   /* exception entry push */
    in_exception = 1;

    /* BIOS interrupt chain: root counter events (RCnt0-2 = IRQ4-6, RCnt3 = VBlank) are
     * delivered before the custom exit runs. */
    static const int rcnt_line[4] = { IRQ_TMR0, IRQ_TMR1, IRQ_TMR2, IRQ_VBLANK };
    for (int n = 0; n < 4; n++) {
        if (!(psx_irq_pending() & (1u << rcnt_line[n]))) continue;
        bios_deliver_event(c, 0xF2000000u | (uint32_t)n, 0x0002u);   /* EvSpINT */
        if (rcnt_clear[n]) psx_irq_ack(rcnt_line[n]);
    }

    if (!handler) {
        in_exception = 0;
        uint32_t next = R32(A_PCB);
        if (next == tcb) load_image(c, tcb);
        else switch_to_image(c, next);
        return;
    }
    CPU ex = *c;
    ex.r[31] = R32(jb + 0);
    ex.r[29] = R32(jb + 0x04);
    ex.r[30] = R32(jb + 0x08);
    for (int i = 0; i < 8; i++) ex.r[16 + i] = R32(jb + 0x0C + 4 * i);
    ex.r[28] = R32(jb + 0x2C);
    ex.r[2] = 1;
    if (__builtin_setjmp(irq_return_buf) == 0)
        call_dynamic(&ex, handler);
    in_exception = 0;
    /* ReturnFromException resumes whatever TCB is current now - the kernel preempts
     * tasks by repointing it from inside its VBlank handler. */
    uint32_t next = R32(A_PCB);
    if (next == tcb) load_image(c, tcb);
    else switch_to_image(c, next);
}

static void ReturnFromException(CPU *c) {
    (void)c;
    if (!in_exception) {
        fprintf(stderr, "fatal: ReturnFromException outside exception\n");
        exit(1);
    }
    __builtin_longjmp(irq_return_buf, 1);
}

/* ---- heap (A0:33/34/39) ---------------------------------------------------------- */
static uint32_t heap_base, heap_size, heap_top;

/* ---- pads (B0:12..16) ------------------------------------------------------------ */
static uint32_t pad_buf[2], pad_len[2];
static int pad_started;

void bios_vblank_pads(void) {
    if (!pad_started) return;
    for (int i = 0; i < 2; i++) {
        if (!pad_buf[i]) continue;
        uint16_t buttons = i == 0 ? psx_pad_buttons() : 0xFFFFu;
        MEM_SB(pad_buf[i] + 0, i == 0 ? 0x00 : 0xFF);   /* 0 = connected */
        MEM_SB(pad_buf[i] + 1, 0x41);                   /* digital pad, 1 halfword */
        MEM_SH(pad_buf[i] + 2, buttons);
    }
}

/* ---- console --------------------------------------------------------------------- */
static void con_write(uint32_t buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) fputc(MEM_LB(buf + i), stdout);
    fflush(stdout);
}

/* ---- dispatch ---------------------------------------------------------------------- */
static void unimplemented(CPU *c, uint32_t vec) {
    fprintf(stderr, "fatal: BIOS %02X:%02X unimplemented (a0=%08X a1=%08X a2=%08X ra=%08X)\n",
            vec, c->r[9], c->r[4], c->r[5], c->r[6], c->r[31]);
    exit(1);
}

static void bios_a0(CPU *c) {
    uint32_t a0 = c->r[4], a1 = c->r[5];
    switch (c->r[9]) {
    case 0x33: {                                         /* malloc */
        uint32_t n = (a0 + 7) & ~7u;
        if (heap_top + n > heap_base + heap_size) { c->r[2] = 0; break; }
        c->r[2] = heap_top; heap_top += n; break;
    }
    case 0x34: c->r[2] = 0; break;                       /* free (bump allocator) */
    case 0x39: heap_base = heap_top = a0; heap_size = a1; c->r[2] = 0; break; /* InitHeap */
    case 0x3F: case 0x3E: {                              /* printf / puts (string only) */
        uint32_t p = a0; int ch;
        while ((ch = MEM_LB(p++)) != 0) fputc(ch, stdout);
        if (c->r[9] == 0x3E) fputc('\n', stdout);
        fflush(stdout);
        c->r[2] = 0; break;
    }
    case 0x44: c->r[2] = 0; break;                       /* FlushCache */
    case 0x49: hw_write(0x1F801810u, a0, 4); c->r[2] = 0; break; /* GPU_cw */
    case 0x70: c->r[2] = 1; break;                       /* _bu_init */
    case 0x72: c->r[2] = 0; break;                       /* CdRemove */
    case 0xAB: c->r[2] = 1; psx_card_info(c, a0); break; /* _card_info */
    case 0xAC: c->r[2] = 1; psx_card_load(c, a0); break; /* _card_load */
    default: unimplemented(c, 0xA0);
    }
}

static void bios_b0(CPU *c) {
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
    switch (c->r[9]) {
    case 0x07: bios_deliver_event(c, a0, a1); c->r[2] = 1; break;  /* DeliverEvent */
    case 0x08: c->r[2] = OpenEvent(a0, a1, a2, a3); break;
    case 0x09: { uint32_t e = ev_addr(a0); if (e) W32(e + EV_STATUS, EvStUNUSED); c->r[2] = 1; break; }
    case 0x0A: c->r[2] = WaitEvent(c, a0); break;
    case 0x0B: c->r[2] = TestEvent(a0); break;
    case 0x0C: { uint32_t e = ev_addr(a0); if (e) W32(e + EV_STATUS, EvStACTIVE); c->r[2] = 1; break; }
    case 0x0D: { uint32_t e = ev_addr(a0); if (e) W32(e + EV_STATUS, EvStWAIT); c->r[2] = 1; break; }
    case 0x0E: c->r[2] = OpenTh(a0, a1, a2); break;
    case 0x0F: c->r[2] = CloseTh(a0); break;
    case 0x10: ChangeTh(c, a0); break;
    case 0x12: pad_buf[0] = a0; pad_len[0] = a1; pad_buf[1] = a2; pad_len[1] = a3;
               c->r[2] = 1; break;                                   /* InitPAD2 */
    case 0x13: pad_started = 1; c->r[2] = 1; break;                  /* StartPAD2 */
    case 0x14: pad_started = 0; c->r[2] = 1; break;                  /* StopPAD2 */
    case 0x15: pad_buf[0] = a0; pad_buf[1] = a1; pad_started = 1; c->r[2] = 1; break;
    case 0x16: c->r[2] = psx_pad_buttons() ^ 0xFFFFu; break;          /* GetButtons */
    case 0x17: ReturnFromException(c); break;
    case 0x18: custom_exit = 0; c->r[2] = 0; break;                   /* SetDefaultExit */
    case 0x19: custom_exit = a0; c->r[2] = 0; break;                  /* SetCustomExit */
    case 0x20: { /* UnDeliverEvent */
        for (uint32_t i = 0; i < N_EVCB; i++) {
            uint32_t e = A_EVCB + i * EVCB_SIZE;
            if (R32(e + EV_CLASS) == a0 && R32(e + EV_SPEC) == a1 &&
                R32(e + EV_STATUS) == EvStALREADY && R32(e + EV_MODE) == EvMdNOINTR)
                W32(e + EV_STATUS, EvStACTIVE);
        }
        c->r[2] = 1; break;
    }
    case 0x32: c->r[2] = psx_file_open(c, a0, a1); break;
    case 0x33: c->r[2] = psx_file_lseek(c, a0, a1, a2); break;
    case 0x34: c->r[2] = psx_file_read(c, a0, a1, a2); break;
    case 0x35:
        if (a0 == 1) { con_write(a1, a2); c->r[2] = a2; }
        else c->r[2] = psx_file_write(c, a0, a1, a2);
        break;
    case 0x36: c->r[2] = psx_file_close(c, a0); break;
    case 0x41: c->r[2] = psx_card_format(c, a0); break;
    case 0x42: c->r[2] = psx_file_firstfile(c, a0, a1); break;
    case 0x43: c->r[2] = psx_file_nextfile(c, a0); break;
    case 0x45: c->r[2] = psx_file_erase(c, a0); break;
    case 0x4A: case 0x4B: case 0x4C: c->r[2] = 1; break;            /* Init/Start/StopCARD */
    case 0x4E: c->r[2] = psx_card_write(c, a0, a1, a2); break;
    case 0x4F: c->r[2] = psx_card_read(c, a0, a1, a2); break;
    case 0x50: c->r[2] = 1; break;                                    /* _new_card */
    case 0x56: c->r[2] = A_C0TBL; break;                              /* GetC0Table */
    case 0x57: c->r[2] = A_B0TBL; break;                              /* GetB0Table */
    case 0x5B: c->r[2] = 0; break;                                    /* ChangeClearPad */
    default: unimplemented(c, 0xB0);
    }
}

static void bios_c0(CPU *c) {
    switch (c->r[9]) {
    case 0x0A: {                                                      /* ChangeClearRCnt */
        uint32_t t = c->r[4] & 3;
        c->r[2] = rcnt_clear[t]; rcnt_clear[t] = c->r[5]; break;
    }
    default: unimplemented(c, 0xC0);
    }
}

void bios_call(CPU *c, uint32_t vector) {
    if (trace_bios)
        fprintf(stderr, "[bios] %02X:%02X a0=%08X a1=%08X a2=%08X a3=%08X ra=%08X\n",
                vector, c->r[9], c->r[4], c->r[5], c->r[6], c->r[7], c->r[31]);
    switch (vector) {
    case 0xA0: bios_a0(c); break;
    case 0xB0: bios_b0(c); break;
    case 0xC0: bios_c0(c); break;
    default: unimplemented(c, vector);
    }
}

void psx_syscall(CPU *c, uint32_t code, uint32_t pc) {
    (void)code;
    switch (c->r[4]) {
    case 0: c->r[2] = 0; break;                                       /* NoFunction */
    case 1: {                                                         /* EnterCriticalSection */
        c->r[2] = psx_irq_enabled();
        cop0_sr &= ~0x401u;
        break;
    }
    case 2: cop0_sr |= 0x401u; break;                                 /* ExitCriticalSection */
    default:
        fprintf(stderr, "fatal: syscall a0=%X at %08X\n", c->r[4], pc);
        exit(1);
    }
}

void psx_break(CPU *c, uint32_t code, uint32_t pc) {
    fprintf(stderr, "fatal: break %X at %08X (ra=%08X)\n", code, pc, c->r[31]);
    exit(1);
}

/* ---- boot ------------------------------------------------------------------------ */
void bios_boot(uint32_t pc, uint32_t gp, uint32_t sp) {
    trace_bios = getenv("PSX_TRACE_BIOS") != NULL;
    /* table of tables */
    W32(0x80000108u, A_PCB);
    W32(0x8000010Cu, 4);
    W32(0x80000110u, A_TCB);
    W32(0x80000114u, N_TCB * TCB_SIZE);
    W32(0x80000120u, A_EVCB);
    W32(0x80000124u, N_EVCB * EVCB_SIZE);
    for (uint32_t i = 0; i < 0x20; i++) W32(A_C0TBL + 4 * i, A_PATCHAREA + 0x80 * i);
    for (uint32_t i = 0; i < 0x60; i++) W32(A_B0TBL + 4 * i, A_PATCHAREA + 0x1000 + 0x80 * i);
    for (uint32_t i = 0; i < N_TCB; i++) W32(A_TCB + i * TCB_SIZE + TCB_STATUS, TCB_FREE);
    W32(A_TCB + TCB_STATUS, TCB_USED);      /* thread 0 = boot thread */
    W32(A_PCB, A_TCB);
    cop0_sr = 0x00000401u;

    Fiber *f = &fibers[0];
    f->handle = ConvertThreadToFiber(NULL);
    f->alive = 1;
    f->active = &f->cpu;
    f->entry = pc;
    f->cpu.r[28] = gp;
    f->cpu.r[29] = sp;
    f->cpu.r[30] = sp;
    cur_fiber = 0;
    call_dynamic(&f->cpu, pc);
    fprintf(stderr, "boot thread returned\n");
}

CPU *bios_current_cpu(void) { return &fibers[cur_fiber].cpu; }
