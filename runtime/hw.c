/* Hardware register space (0x1F801000-0x1F802FFF) and virtual time.
 *
 * Time is virtual: it advances when recompiled code touches hardware, runs loop
 * back-edges, or idles - never with the host clock - so runs are deterministic.
 * Subsystems (GPU, CD, SPU, MDEC) attach here as they're written; until then
 * unknown registers read back what was last written.
 */
#include <stdio.h>
#include <stdlib.h>
#include "bios.h"
#include "cd.h"

#define CPU_HZ         33868800ull
#define LINE_CYCLES    2152ull          /* NTSC: 263 lines at ~59.8 Hz */
#define FRAME_LINES    263ull
#define VBLANK_LINE    240ull
#define FRAME_CYCLES   (LINE_CYCLES * FRAME_LINES)
#define DOT_CYCLES     5ull             /* ~dot clock at 320px width */
#define POLL_COST      64ull            /* cycles charged per hardware register access */

static uint64_t now;
static uint64_t next_vblank = LINE_CYCLES * VBLANK_LINE;
static uint32_t i_stat, i_mask;
static uint8_t io[0x2000];              /* backing store for unimplemented registers */
static int trace_hw, trace_dma;

uint64_t psx_cycles(void) { return now; }
void psx_irq_raise(int line) { i_stat |= 1u << line; }
uint32_t psx_irq_pending(void) { return i_stat & i_mask; }
void psx_irq_ack(int line) { i_stat &= ~(1u << line); }

#define IO32(p) (*(uint32_t *)&io[(p) - 0x1F801000u])

/* ---- root counters ------------------------------------------------------------- */
typedef struct {
    uint32_t mode, target;
    uint64_t base_time;     /* time of last write/reset */
    uint32_t base_val;      /* counter value at base_time */
    uint64_t last_check;    /* IRQ hit detection watermark */
} Timer;
static Timer tmr[3];

static uint64_t timer_unit(int n) {
    uint32_t src = (tmr[n].mode >> 8) & 3;
    if (n == 0) return (src & 1) ? DOT_CYCLES : 1;
    if (n == 1) return (src & 1) ? LINE_CYCLES : 1;
    return (src & 2) ? 8 : 1;
}
static uint64_t timer_period(int n) {
    return (tmr[n].mode & 0x08) ? (uint64_t)tmr[n].target + 1 : 0x10000ull;
}
/* absolute (unwrapped) tick count at time t */
static uint64_t timer_abs(int n, uint64_t t) {
    return tmr[n].base_val + (t - tmr[n].base_time) / timer_unit(n);
}
static uint32_t timer_value(int n) {
    return (uint32_t)(timer_abs(n, now) % timer_period(n));
}
/* number of times the counter equals v in (a, b] (absolute ticks) */
static uint64_t hits(uint64_t a, uint64_t b, uint64_t v, uint64_t period) {
    if (b <= a) return 0;
    uint64_t fa = (a + period - v) / period, fb = (b + period - v) / period;
    return fb - fa;
}
static void timer_check(int n) {
    Timer *t = &tmr[n];
    uint64_t a = timer_abs(n, t->last_check), b = timer_abs(n, now);
    t->last_check = now;
    if (b == a) return;
    uint64_t p = timer_period(n);
    int fire = 0;
    if ((t->mode & 0x10) && hits(a, b, t->target % p, p)) { fire = 1; t->mode |= 0x0800; }
    if ((t->mode & 0x20) && p == 0x10000ull && hits(a, b, 0xFFFF, p)) { fire = 1; t->mode |= 0x1000; }
    if (fire) psx_irq_raise(IRQ_TMR0 + n);
}
/* earliest time a timer IRQ can fire, or UINT64_MAX */
static uint64_t timer_next_irq(int n) {
    Timer *t = &tmr[n];
    if (!(t->mode & 0x30)) return UINT64_MAX;
    uint64_t u = timer_unit(n), p = timer_period(n), v = timer_abs(n, now);
    uint64_t want = (t->mode & 0x10) ? t->target % p : 0xFFFF;
    uint64_t ticks = (want + p - (v % p)) % p;
    if (!ticks) ticks = p;
    return now - (now - t->base_time) % u + ticks * u;
}
static void timer_write(int n, int reg, uint32_t v) {
    Timer *t = &tmr[n];
    uint32_t cur = timer_value(n);
    switch (reg) {
    case 0: t->base_val = v & 0xFFFF; break;
    case 4: t->mode = v & 0x3FF; t->base_val = 0; break;   /* writing mode resets */
    case 8: t->target = v & 0xFFFF; t->base_val = cur; break;
    }
    t->base_time = t->last_check = now;
}
static uint32_t timer_read(int n, int reg) {
    switch (reg) {
    case 0: return timer_value(n);
    case 4: { uint32_t m = tmr[n].mode; tmr[n].mode &= ~0x1800u; return m | 0x0400; }
    case 8: return tmr[n].target;
    }
    return 0;
}

/* ---- GPU status ------------------------------------------------------------------ */
static uint32_t gpustat(void) {
    uint64_t line = (now / LINE_CYCLES) % FRAME_LINES;
    uint32_t odd = (line < VBLANK_LINE) ? (uint32_t)(line & 1) : 0;
    return 0x1C000000u | (odd << 31);
}

/* ---- DMA ------------------------------------------------------------------------- */
#define DPCR 0x1F8010F0u
#define DICR 0x1F8010F4u

static void dma_irq(int ch) {
    uint32_t dicr = IO32(DICR);
    if (dicr & (1u << (16 + ch))) {
        dicr |= 1u << (24 + ch);
        IO32(DICR) = dicr;
        if (dicr & 0x00800000u) psx_irq_raise(IRQ_DMA);
    }
}

static void dma_run(int ch) {
    uint32_t base = 0x1F801080u + 0x10u * ch;
    uint32_t madr = IO32(base) & 0x1FFFFC, bcr = IO32(base + 4), chcr = IO32(base + 8);
    switch (ch) {
    case 6: {                              /* OTC: build reverse-linked empty ordering table */
        uint32_t n = bcr & 0xFFFF ? bcr & 0xFFFF : 0x10000;
        uint32_t a = madr;
        for (uint32_t i = 1; i < n; i++, a -= 4) MEM_SW(0x80000000u | a, (a - 4) & 0x1FFFFF);
        MEM_SW(0x80000000u | a, 0x00FFFFFFu);
        break;
    }
    case 3: {                              /* CD-ROM data FIFO -> RAM */
        uint32_t words = (bcr & 0xFFFF) ? (bcr & 0xFFFF) : 0x10000;
        if ((bcr >> 16) > 1) words *= bcr >> 16;
        cd_dma_read(madr, words);
        break;
    }
    case 2:                                /* GPU: linked list or block - consumed, not drawn yet */
        if (((chcr >> 9) & 3) == 2) {
            uint32_t a = madr, guard = 0;
            while (a != 0xFFFFFF && guard++ < 0x40000) a = MEM_LW(0x80000000u | a) & 0xFFFFFF;
        }
        break;
    default:
        if (trace_hw) fprintf(stderr, "[dma] ch%d madr=%08X bcr=%08X chcr=%08X (ignored)\n",
                              ch, madr, bcr, chcr);
        break;
    }
    IO32(base + 8) = chcr & ~0x11000000u;  /* clear start/busy + trigger */
    if (trace_dma)
        fprintf(stderr, "[dma] ch%d madr=%06X bcr=%08X chcr=%08X dicr=%08X dpcr=%08X\n",
                ch, madr, bcr, chcr, IO32(DICR), IO32(DPCR));
    dma_irq(ch);
}

/* ---- events and time ------------------------------------------------------------- */
static void run_events(CPU *c) {
    (void)c;
    while (now >= next_vblank) {
        next_vblank += FRAME_CYCLES;
        psx_irq_raise(IRQ_VBLANK);
        bios_vblank_pads();
    }
    for (int n = 0; n < 3; n++) timer_check(n);
    cd_update();
}

void psx_poll(CPU *c) {
    run_events(c);
    if ((i_stat & i_mask) && psx_irq_enabled())
        bios_raise_exception(c);
}

void psx_idle(CPU *c) {
    uint64_t t = next_vblank;
    for (int n = 0; n < 3; n++)
        if (i_mask & (1u << (IRQ_TMR0 + n))) {
            uint64_t x = timer_next_irq(n);
            if (x < t) t = x;
        }
    uint64_t x = cd_next_event();
    if (x < t) t = x;
    if (t > now) now = t;
    psx_poll(c);
}

static void advance(CPU *c, uint64_t cyc) {
    now += cyc;
    psx_poll(c);
}

#define BACKEDGE_CYCLES 24ull     /* rough cost of one loop iteration */
int32_t psx_backedge_budget = BACKEDGE_BATCH;

/* ---- diagnostics: sample which function loops are spinning in ------------------- */
uint32_t psx_cur_func;
static uint64_t max_cycles = UINT64_MAX;
#define NSAMPLE 64
static struct { uint32_t func; uint32_t n; } samples[NSAMPLE];

static void sample(uint32_t f) {
    int free_slot = -1, min_slot = 0;
    for (int i = 0; i < NSAMPLE; i++) {
        if (samples[i].func == f) { samples[i].n++; return; }
        if (!samples[i].n && free_slot < 0) free_slot = i;
        if (samples[i].n < samples[min_slot].n) min_slot = i;
    }
    int s = free_slot >= 0 ? free_slot : min_slot;
    samples[s].func = f;
    samples[s].n = 1;
}

static void dump_and_exit(CPU *c) {
    fprintf(stderr, "\n=== stopped at %.2f virtual seconds ===\n", (double)now / CPU_HZ);
    fprintf(stderr, "i_stat=%03X i_mask=%03X irq_enabled=%d cur_func=%08X\n",
            i_stat, i_mask, psx_irq_enabled(), psx_cur_func);
    fprintf(stderr, "ra=%08X sp=%08X v0=%08X a0=%08X a1=%08X\n",
            c->r[31], c->r[29], c->r[2], c->r[4], c->r[5]);
    fprintf(stderr, "hottest loop functions:\n");
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < NSAMPLE; i++)
            if (samples[i].n && (best < 0 || samples[i].n > samples[best].n)) best = i;
        if (best < 0) break;
        fprintf(stderr, "  %08X  %u\n", samples[best].func, samples[best].n);
        samples[best].n = 0;
    }
    exit(3);
}

void psx_backedge_slow(CPU *c) {
    psx_backedge_budget = BACKEDGE_BATCH;
    sample(psx_cur_func);
    advance(c, BACKEDGE_BATCH * BACKEDGE_CYCLES);
    if (now > max_cycles) dump_and_exit(c);
}

/* ---- register access -------------------------------------------------------------- */
uint32_t hw_read(uint32_t addr, int size) {
    uint32_t p = addr & 0x1FFFFFFFu;
    advance(bios_current_cpu(), POLL_COST);
    uint32_t v;
    if (p == 0x1F801070u) v = i_stat;
    else if (p == 0x1F801074u) v = i_mask;
    else if (p >= 0x1F801100u && p < 0x1F801130u) v = timer_read((p >> 4) & 3, p & 0xC);
    else if (p == 0x1F801814u) v = gpustat();
    else if (p >= 0x1F801800u && p < 0x1F801804u) {
        v = cd_read(p & 3);
        if (size > 1) fprintf(stderr, "warning: %d-bit CD register read %08X\n", size * 8, addr);
    }
    else if (p >= 0x1F801000u && p < 0x1F803000u) {
        v = 0;
        memcpy(&v, &io[p - 0x1F801000u], (size_t)size);
    } else if (p >= 0x1FC00000u && p < 0x1FC80000u) {
        v = 0;                                    /* BIOS ROM: not present */
    } else {
        fprintf(stderr, "fatal: read%d from unmapped %08X\n", size * 8, addr);
        exit(1);
    }
    if (size == 1) v &= 0xFF;
    else if (size == 2) v &= 0xFFFF;
    if (trace_hw) fprintf(stderr, "[hw] r%d %08X -> %08X\n", size * 8, addr, v);
    return v;
}

void hw_write(uint32_t addr, uint32_t val, int size) {
    uint32_t p = addr & 0x1FFFFFFFu;
    if (trace_hw) fprintf(stderr, "[hw] w%d %08X <- %08X\n", size * 8, addr, val);
    if (p == 0x1F801070u) { i_stat &= val; return; }          /* write 0 to acknowledge */
    if (p == 0x1F801074u) { i_mask = val & 0x7FF; return; }
    if (p >= 0x1F801100u && p < 0x1F801130u) { timer_write((p >> 4) & 3, p & 0xC, val); return; }
    if (p >= 0x1F801800u && p < 0x1F801804u) { cd_write(p & 3, (uint8_t)val); return; }
    if (p == DICR) {                                          /* flags are write-1-to-clear */
        uint32_t d = IO32(DICR);
        d = (d & 0xFF000000u & ~(val & 0x7F000000u)) | (val & 0x00FFFFFFu);
        IO32(DICR) = d;
        return;
    }
    if (p >= 0x1F801000u && p < 0x1F803000u) {
        memcpy(&io[p - 0x1F801000u], &val, (size_t)size);
        if (p >= 0x1F801088u && p < 0x1F8010F0u && (p & 0xF) == 8 && (val & 0x01000000u))
            dma_run((int)((p - 0x1F801080u) >> 4));
        return;
    }
    if ((p & 0xFFFFFFF0u) == 0x1FFE0130u || (addr & 0xFFFFFFF0u) == 0xFFFE0130u) return;
    fprintf(stderr, "warning: write%d to unmapped %08X = %08X\n", size * 8, addr, val);
}

uint16_t psx_pad_buttons(void) { return 0xFFFFu; }

void hw_init(void) {
    trace_hw = getenv("PSX_TRACE_HW") != NULL;
    trace_dma = getenv("PSX_TRACE_DMA") != NULL;
    const char *ms = getenv("PSX_MAX_SECONDS");
    if (ms) max_cycles = (uint64_t)(atof(ms) * (double)CPU_HZ);
    cd_init();
}
