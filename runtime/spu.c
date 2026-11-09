/* SPU: sound RAM, transfers, IRQ9, 24 ADPCM voices, noise, reverb, CD/XA input.
 *
 * Runs at 44.1 kHz in virtual time (768 CPU cycles per sample): spu_run() catches
 * the SPU up to the clock, and every register access does so first, so writes land
 * at the right sample. Algorithms follow psx-spx (ADPCM filters, pitch counter with
 * 4-point Gaussian interpolation, ADSR/sweep envelope generator, noise LFSR, reverb
 * formula). Reverb runs at 22.05 kHz with a plain 2:1 decimation instead of the FIR.
 *
 * Konami's sound driver keys a voice on a looping silent block and points the SPU
 * IRQ address at it: that IRQ is the driver's tick, so IRQ timing matters more than
 * anything audible here.
 *
 * XA-ADPCM sectors arrive from cd.c (cd_xa_sector), are decoded into a ring of
 * 37.8/18.9 kHz stereo frames, and are resampled linearly into the CD input.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psx.h"
#include "bios.h"
#include "cd.h"
#include "audio.h"
#include "spu.h"
#include "spu_gauss.h"

#define CYCLES_PER_SAMPLE 768ull           /* 33868800 / 44100 */
#define RAM_SIZE 0x80000u

/* register file, indexed by (addr - 0x1F801C00) / 2 */
enum {
    R_MVOLL = 0xC0, R_MVOLR, R_EVOLL, R_EVOLR, R_KON0, R_KON1, R_KOFF0, R_KOFF1,
    R_PMON0, R_PMON1, R_NON0, R_NON1, R_EON0, R_EON1, R_ENDX0, R_ENDX1,
    R_ESA = 0xD1, R_IRQA, R_TSA, R_FIFO, R_ATTR, R_RAMCTL, R_STATX,
    R_AVOLL, R_AVOLR, R_BVOLL, R_BVOLR, R_MVOLXL, R_MVOLXR,
    R_REV = 0xE0, R_VOLX = 0x100,
};
/* voice registers (halfword index within the voice's 8) */
enum { V_VOLL, V_VOLR, V_PITCH, V_SSA, V_ADSR1, V_ADSR2, V_ENVX, V_LSAX };
/* reverb registers (index from R_REV) */
enum {
    dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2,
    mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2, dLSAME, dRSAME,
    mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF,
    mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN,
};

enum { PH_OFF, PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE };

typedef struct { int32_t level; uint32_t counter; } Env;

typedef struct {
    uint32_t addr;                 /* current ADPCM block address (bytes) */
    uint32_t counter;              /* pitch counter: bits 12+ sample, 4-11 gauss index */
    int16_t buf[3 + 28];           /* last 3 samples of the previous block + this block */
    int32_t hist1, hist2;          /* ADPCM filter history */
    uint8_t flags;                 /* current block's flag byte */
    uint8_t has_block, ignore_loop;
    int phase;
    Env adsr, vol[2];
    int32_t out;                   /* last sample after ADSR (pitch modulation source) */
} Voice;

static uint8_t ram[RAM_SIZE];
static uint16_t regs[0x200];
static Voice voice[24];
static uint32_t endx;
static uint32_t xfer_addr;
static uint16_t statx_irq;         /* STATX bit 6 */
static Env mvol[2];
static uint32_t rev_addr;          /* current reverb buffer address (bytes) */
static int32_t rev_in[2], rev_out[2];
static int rev_phase;
static int32_t noise_timer;
static uint16_t noise_level = 1;
static uint64_t next_sample;
static int trace;

#define VR(n, r) regs[(n) * 8 + (r)]
#define ATTR regs[R_ATTR]

static int32_t clamp16(int32_t v) { return v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v; }

/* ---- IRQ9 ----------------------------------------------------------------------- */
static void irq_check(uint32_t addr, uint32_t len) {
    if ((ATTR & 0x8040) != 0x8040 || statx_irq) return;
    uint32_t ia = (uint32_t)regs[R_IRQA] * 8;
    if (ia >= addr && ia < addr + len) {
        statx_irq = 0x40;
        psx_irq_raise(IRQ_SPU);
        if (trace) fprintf(stderr, "[spu] irq at %05X\n", ia);
    }
}

/* ---- envelopes (ADSR and volume sweep share the generator) ---------------------- */
static void env_tick(Env *e, int shift, int stepv, int exp, int dec, int neg, int frozen) {
    if (frozen) return;                        /* all-ones rate: never steps */
    int32_t step = 7 - stepv;
    if (dec ^ neg) step = ~step;
    if (shift < 11) step <<= 11 - shift;
    uint32_t inc = shift > 11 ? 0x8000u >> (shift - 11) : 0x8000u;
    if (exp && !dec && e->level > 0x6000) {
        if (shift < 10) step >>= 2;
        else if (shift >= 11) inc >>= 2;
        else { step >>= 1; inc >>= 1; }
    } else if (exp && dec) {
        step = (step * e->level) >> 15;
    }
    if (!inc) inc = 1;
    e->counter += inc;
    if (!(e->counter & 0x8000)) return;
    e->counter = 0;
    int32_t l = e->level + step;
    if (!dec) l = clamp16(l);
    else if (neg) l = l < -0x8000 ? -0x8000 : l > 0 ? 0 : l;
    else if (l < 0) l = 0;
    e->level = l;
}

/* volume register: direct (bit15=0, value is volume/2) or sweep (bit15=1) */
static void vol_set(Env *e, uint16_t v) {
    if (!(v & 0x8000)) e->level = (int16_t)(v << 1);
    e->counter = 0;
}
static void vol_tick(Env *e, uint16_t v) {
    if (!(v & 0x8000)) return;
    env_tick(e, (v >> 2) & 0x1F, v & 3, (v >> 14) & 1, (v >> 13) & 1, (v >> 12) & 1,
             (v & 0x7F) == 0x7F);
}

static void adsr_set_phase(Voice *v, int ph) { v->phase = ph; v->adsr.counter = 0; }

static void adsr_tick(Voice *v, int n) {
    uint16_t lo = VR(n, V_ADSR1), hi = VR(n, V_ADSR2);
    switch (v->phase) {
    case PH_ATTACK:
        env_tick(&v->adsr, (lo >> 10) & 0x1F, (lo >> 8) & 3, lo >> 15, 0, 0,
                 ((lo >> 8) & 0x7F) == 0x7F);
        if (v->adsr.level >= 0x7FFF) adsr_set_phase(v, PH_DECAY);
        break;
    case PH_DECAY:
        env_tick(&v->adsr, (lo >> 4) & 0xF, 0, 1, 1, 0, 0);
        if (v->adsr.level <= (int32_t)((lo & 0xF) + 1) * 0x800) adsr_set_phase(v, PH_SUSTAIN);
        break;
    case PH_SUSTAIN:
        env_tick(&v->adsr, (hi >> 8) & 0x1F, (hi >> 6) & 3, hi >> 15, (hi >> 14) & 1, 0,
                 ((hi >> 6) & 0x7F) == 0x7F);
        break;
    case PH_RELEASE:
        env_tick(&v->adsr, hi & 0x1F, 0, (hi >> 5) & 1, 1, 0, (hi & 0x1F) == 0x1F);
        if (v->adsr.level <= 0) { v->adsr.level = 0; adsr_set_phase(v, PH_OFF); }
        break;
    }
}

/* ---- ADPCM ---------------------------------------------------------------------- */
static const int32_t K0[5] = { 0, 60, 115, 98, 122 };
static const int32_t K1[5] = { 0, 0, -52, -55, -60 };

static void decode_block(Voice *v, int n) {
    uint32_t a = v->addr & (RAM_SIZE - 16);
    irq_check(a, 16);
    uint8_t hdr = ram[a], flags = ram[a + 1];
    if ((flags & 4) && !v->ignore_loop) VR(n, V_LSAX) = (uint16_t)(a >> 3);
    int shift = hdr & 0xF, filt = (hdr >> 4) & 7;
    if (shift > 12) shift = 9;
    if (filt > 4) filt = 4;
    memcpy(v->buf, v->buf + 28, 3 * sizeof v->buf[0]);
    for (int i = 0; i < 28; i++) {
        int nib = (ram[a + 2 + i / 2] >> ((i & 1) * 4)) & 0xF;
        int32_t s = ((int16_t)(nib << 12)) >> shift;
        s += (v->hist1 * K0[filt] + v->hist2 * K1[filt] + 32) >> 6;
        s = clamp16(s);
        v->hist2 = v->hist1;
        v->hist1 = s;
        v->buf[3 + i] = (int16_t)s;
    }
    v->flags = flags;
    v->has_block = 1;
}

static void key_on(int n) {
    Voice *v = &voice[n];
    v->addr = (uint32_t)VR(n, V_SSA) * 8;
    v->counter = 0;
    v->hist1 = v->hist2 = 0;
    memset(v->buf, 0, sizeof v->buf);
    v->has_block = 0;
    v->ignore_loop = 0;
    v->adsr.level = 0;
    adsr_set_phase(v, PH_ATTACK);
    endx &= ~(1u << n);
}

static void key_off(int n) {
    if (voice[n].phase != PH_OFF) adsr_set_phase(&voice[n], PH_RELEASE);
}

static void noise_tick(void) {
    int shift = (ATTR >> 10) & 0xF, step = ((ATTR >> 8) & 3) + 4;
    noise_timer -= step;
    int parity = ((noise_level >> 15) ^ (noise_level >> 12) ^ (noise_level >> 11) ^
                  (noise_level >> 10) ^ 1) & 1;
    if (noise_timer < 0) noise_level = (uint16_t)(noise_level * 2 + parity);
    if (noise_timer < 0) noise_timer += 0x20000 >> shift;
    if (noise_timer < 0) noise_timer += 0x20000 >> shift;
}

static int32_t voice_sample(int n, uint32_t pmon, uint32_t non) {
    Voice *v = &voice[n];
    if (!v->has_block) decode_block(v, n);
    int32_t s;
    if (non & (1u << n)) {
        s = (int16_t)noise_level;
    } else {
        int i = (v->counter >> 4) & 0xFF, k = (int)(v->counter >> 12);
        s  = (gauss[0x0FF - i] * v->buf[k]) >> 15;
        s += (gauss[0x1FF - i] * v->buf[k + 1]) >> 15;
        s += (gauss[0x100 + i] * v->buf[k + 2]) >> 15;
        s += (gauss[0x000 + i] * v->buf[k + 3]) >> 15;
    }
    int32_t out = (s * v->adsr.level) >> 15;
    v->out = out;
    adsr_tick(v, n);

    uint32_t step = VR(n, V_PITCH);
    if (n > 0 && (pmon & (1u << n))) {
        int32_t factor = voice[n - 1].out + 0x8000;
        step = (uint32_t)(((int32_t)(int16_t)step * factor) >> 15) & 0xFFFF;
    }
    if (step > 0x3FFF) step = 0x4000;
    v->counter += step;
    if ((v->counter >> 12) >= 28) {
        v->counter -= 28u << 12;
        if (v->flags & 1) {                         /* loop end */
            endx |= 1u << n;
            v->addr = (uint32_t)VR(n, V_LSAX) * 8;
            if (!(v->flags & 2)) {                  /* end without repeat: mute */
                v->adsr.level = 0;
                adsr_set_phase(v, PH_OFF);
            }
        } else {
            v->addr = (v->addr + 16) & (RAM_SIZE - 1);
        }
        v->has_block = 0;
    }
    return out;
}

/* ---- reverb --------------------------------------------------------------------- */
static uint32_t rev_at(int32_t off_bytes) {
    uint32_t base = (uint32_t)regs[R_ESA] * 8, size = RAM_SIZE - base;
    if (!size) return base;
    int64_t rel = ((int64_t)rev_addr - base + off_bytes) % (int64_t)size;
    if (rel < 0) rel += size;
    return (base + (uint32_t)rel) & (RAM_SIZE - 2);
}
#define RV(k) ((int32_t)(int16_t)regs[R_REV + (k)])
#define RA(k) ((int32_t)regs[R_REV + (k)] * 8)
static int32_t rd(int32_t off) { uint32_t a = rev_at(off); return (int16_t)(ram[a] | ram[a + 1] << 8); }
static void wr(int32_t off, int32_t v) {
    if (!(ATTR & 0x80)) return;
    uint32_t a = rev_at(off);
    irq_check(a, 2);
    v = clamp16(v);
    ram[a] = (uint8_t)v;
    ram[a + 1] = (uint8_t)(v >> 8);
}
static int32_t mul(int32_t a, int32_t b) { return (a * b) >> 15; }

static void reverb_side(int right, int32_t in) {
    int32_t Lin = mul(RV(right ? vRIN : vLIN), in);
    int same = right ? mRSAME : mLSAME, dsame = right ? dRSAME : dLSAME;
    int diff = right ? mRDIFF : mLDIFF, ddiff = right ? dLDIFF : dRDIFF;
    int32_t s2 = rd(RA(same) - 2), d2 = rd(RA(diff) - 2);
    wr(RA(same), mul(Lin + mul(rd(RA(dsame)), RV(vWALL)) - s2, RV(vIIR)) + s2);
    wr(RA(diff), mul(Lin + mul(rd(RA(ddiff)), RV(vWALL)) - d2, RV(vIIR)) + d2);
    int c1 = right ? mRCOMB1 : mLCOMB1, c2 = right ? mRCOMB2 : mLCOMB2;
    int c3 = right ? mRCOMB3 : mLCOMB3, c4 = right ? mRCOMB4 : mLCOMB4;
    int32_t out = mul(RV(vCOMB1), rd(RA(c1))) + mul(RV(vCOMB2), rd(RA(c2))) +
                  mul(RV(vCOMB3), rd(RA(c3))) + mul(RV(vCOMB4), rd(RA(c4)));
    int a1 = right ? mRAPF1 : mLAPF1, a2 = right ? mRAPF2 : mLAPF2;
    int32_t t = rd(RA(a1) - RA(dAPF1));
    out = clamp16(out - mul(RV(vAPF1), t));
    wr(RA(a1), out);
    out = clamp16(mul(out, RV(vAPF1)) + t);
    t = rd(RA(a2) - RA(dAPF2));
    out = clamp16(out - mul(RV(vAPF2), t));
    wr(RA(a2), out);
    out = clamp16(mul(out, RV(vAPF2)) + t);
    rev_out[right] = mul(out, (int16_t)regs[right ? R_EVOLR : R_EVOLL]);
}

static void reverb(int32_t inl, int32_t inr) {
    rev_in[0] += inl;
    rev_in[1] += inr;
    if (++rev_phase < 2) return;               /* reverb runs at 22.05 kHz */
    rev_phase = 0;
    reverb_side(0, clamp16(rev_in[0] / 2));
    reverb_side(1, clamp16(rev_in[1] / 2));
    rev_in[0] = rev_in[1] = 0;
    rev_addr = rev_at(2);
}

/* ---- CD input: XA-ADPCM --------------------------------------------------------- */
#define XA_RING 0x10000u
static int16_t xa_ring[XA_RING][2];
static uint32_t xa_head, xa_tail;               /* free-running indices */
static uint32_t xa_rate = 37800, xa_frac;       /* frac: 16.16 position between frames */
static int32_t xa_hist[2][2];

static void xa_put(int16_t l, int16_t r) {
    if (xa_head - xa_tail >= XA_RING) xa_tail++;       /* full: drop the oldest */
    xa_ring[xa_head % XA_RING][0] = l;
    xa_ring[xa_head % XA_RING][1] = r;
    xa_head++;
}

void cd_xa_sector(const uint8_t *raw) {
    uint8_t coding = raw[19];
    int stereo = (coding & 3) == 1, half = ((coding >> 2) & 3) == 1, bits8 = ((coding >> 4) & 3) == 1;
    xa_rate = half ? 18900 : 37800;
    const uint8_t *data = raw + 24;
    int units = bits8 ? 4 : 8;
    for (int g = 0; g < 18; g++) {
        const uint8_t *grp = data + g * 128;
        int16_t pcm[8][28];
        for (int u = 0; u < units; u++) {
            uint8_t hdr = grp[4 + u];
            int shift = hdr & 0xF, filt = (hdr >> 4) & 3;
            if (shift > 12) shift = 9;
            int32_t *h = xa_hist[stereo ? (u & 1) : 0];
            for (int j = 0; j < 28; j++) {
                int32_t s;
                if (bits8) {
                    s = ((int16_t)(grp[16 + j * 4 + u] << 8)) >> shift;
                } else {
                    int nib = (grp[16 + j * 4 + (u >> 1)] >> ((u & 1) * 4)) & 0xF;
                    s = ((int16_t)(nib << 12)) >> shift;
                }
                s += (h[0] * K0[filt] + h[1] * K1[filt] + 32) >> 6;
                s = clamp16(s);
                h[1] = h[0];
                h[0] = s;
                pcm[u][j] = (int16_t)s;
            }
        }
        if (stereo) {
            for (int u = 0; u < units; u += 2)
                for (int j = 0; j < 28; j++) xa_put(pcm[u][j], pcm[u + 1][j]);
        } else {
            for (int u = 0; u < units; u++)
                for (int j = 0; j < 28; j++) xa_put(pcm[u][j], pcm[u][j]);
        }
    }
}

/* next CD-input frame at 44.1 kHz, after the CD controller's volume matrix */
static void cd_input(int32_t *l, int32_t *r) {
    *l = *r = 0;
    if (xa_head - xa_tail < 2) return;
    const int16_t *a = xa_ring[xa_tail % XA_RING], *b = xa_ring[(xa_tail + 1) % XA_RING];
    int32_t f = (int32_t)(xa_frac >> 4);                  /* 12-bit fraction */
    int32_t sl = a[0] + (((b[0] - a[0]) * f) >> 12);
    int32_t sr = a[1] + (((b[1] - a[1]) * f) >> 12);
    xa_frac += (uint32_t)(((uint64_t)xa_rate << 16) / 44100);
    xa_tail += xa_frac >> 16;
    xa_frac &= 0xFFFF;
    int m[4], mute;
    cd_audio_matrix(m, &mute);
    if (mute) return;
    *l = clamp16((sl * m[0] + sr * m[3]) >> 7);
    *r = clamp16((sr * m[2] + sl * m[1]) >> 7);
}

/* ---- mixer ---------------------------------------------------------------------- */
static void tick(void) {
    uint32_t pmon = regs[R_PMON0] | (uint32_t)regs[R_PMON1] << 16;
    uint32_t non = regs[R_NON0] | (uint32_t)regs[R_NON1] << 16;
    uint32_t eon = regs[R_EON0] | (uint32_t)regs[R_EON1] << 16;
    int32_t l = 0, r = 0, rl = 0, rr = 0;
    noise_tick();
    for (int n = 0; n < 24; n++) {
        Voice *v = &voice[n];
        vol_tick(&v->vol[0], VR(n, V_VOLL));
        vol_tick(&v->vol[1], VR(n, V_VOLR));
        if (v->phase == PH_OFF && !(ATTR & 0x40)) { v->out = 0; continue; }
        int32_t s = voice_sample(n, pmon, non);
        int32_t vl = mul(s, v->vol[0].level), vr = mul(s, v->vol[1].level);
        regs[R_VOLX + 2 * n] = (uint16_t)vl;
        regs[R_VOLX + 2 * n + 1] = (uint16_t)vr;
        l += vl;
        r += vr;
        if (eon & (1u << n)) { rl += vl; rr += vr; }
    }
    if (!(ATTR & 0x8000) || !(ATTR & 0x4000)) l = r = rl = rr = 0;   /* off / muted */
    int32_t cl, cr;
    cd_input(&cl, &cr);
    if (ATTR & 1) {
        cl = mul(cl, (int16_t)regs[R_AVOLL]);
        cr = mul(cr, (int16_t)regs[R_AVOLR]);
        l += cl;
        r += cr;
        if (ATTR & 4) { rl += cl; rr += cr; }
    }
    reverb(clamp16(rl), clamp16(rr));
    l += rev_out[0];
    r += rev_out[1];
    vol_tick(&mvol[0], regs[R_MVOLL]);
    vol_tick(&mvol[1], regs[R_MVOLR]);
    regs[R_MVOLXL] = (uint16_t)mvol[0].level;
    regs[R_MVOLXR] = (uint16_t)mvol[1].level;
    audio_push((int16_t)clamp16(mul(clamp16(l), mvol[0].level)),
               (int16_t)clamp16(mul(clamp16(r), mvol[1].level)));
}

void spu_run(void) {
    uint64_t now = psx_cycles();
    while (next_sample <= now) {
        tick();
        next_sample += CYCLES_PER_SAMPLE;
    }
}

uint64_t spu_next_event(void) {
    /* with IRQ9 armed, don't let idle skipping overshoot it by more than ~0.7 ms */
    if ((ATTR & 0x8040) == 0x8040 && !statx_irq) return next_sample + 31 * CYCLES_PER_SAMPLE;
    return UINT64_MAX;
}

/* ---- transfers ------------------------------------------------------------------ */
static void xfer_write(uint16_t h) {
    irq_check(xfer_addr, 2);
    ram[xfer_addr] = (uint8_t)h;
    ram[xfer_addr + 1] = (uint8_t)(h >> 8);
    xfer_addr = (xfer_addr + 2) & (RAM_SIZE - 1);
}
static uint16_t xfer_read(void) {
    irq_check(xfer_addr, 2);
    uint16_t h = (uint16_t)(ram[xfer_addr] | ram[xfer_addr + 1] << 8);
    xfer_addr = (xfer_addr + 2) & (RAM_SIZE - 1);
    return h;
}

void spu_dma_write(uint32_t madr, uint32_t words) {
    spu_run();
    if (trace) fprintf(stderr, "[spu] dma write %u words -> %05X\n", words, xfer_addr);
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = MEM_LW(0x80000000u | ((madr + 4 * i) & 0x1FFFFC));
        xfer_write((uint16_t)w);
        xfer_write((uint16_t)(w >> 16));
    }
}

void spu_dma_read(uint32_t madr, uint32_t words) {
    spu_run();
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = xfer_read();
        w |= (uint32_t)xfer_read() << 16;
        MEM_SW(0x80000000u | ((madr + 4 * i) & 0x1FFFFC), w);
    }
}

/* ---- registers ------------------------------------------------------------------ */
uint16_t spu_read16(uint32_t addr) {
    spu_run();
    uint32_t i = ((addr & 0x3FF) >> 1);
    if (i < 0xC0) {
        int n = (int)(i >> 3);
        if ((i & 7) == V_ENVX) return (uint16_t)voice[n].adsr.level;
        return regs[i];
    }
    switch (i) {
    case R_ENDX0: return (uint16_t)endx;
    case R_ENDX1: return (uint16_t)(endx >> 16);
    case R_STATX: return (uint16_t)((ATTR & 0x3F) | statx_irq | ((ATTR & 0x20) << 2));
    }
    return regs[i];
}

void spu_write16(uint32_t addr, uint16_t v) {
    spu_run();
    uint32_t i = ((addr & 0x3FF) >> 1);
    if (trace > 1) fprintf(stderr, "[spu] w %03X <- %04X\n", i * 2, v);
    if (i < 0xC0) {
        int n = (int)(i >> 3);
        regs[i] = v;
        switch (i & 7) {
        case V_VOLL: vol_set(&voice[n].vol[0], v); break;
        case V_VOLR: vol_set(&voice[n].vol[1], v); break;
        case V_ENVX: voice[n].adsr.level = (int16_t)v; break;
        case V_LSAX: voice[n].ignore_loop |= voice[n].phase != PH_OFF; break;
        }
        return;
    }
    switch (i) {
    case R_MVOLL: regs[i] = v; vol_set(&mvol[0], v); return;
    case R_MVOLR: regs[i] = v; vol_set(&mvol[1], v); return;
    case R_KON0: case R_KON1:
        regs[i] = v;
        for (int b = 0; b < 16; b++)
            if (v & (1u << b) && (i == R_KON0 || b < 8)) key_on(b + (i == R_KON1) * 16);
        if (trace && v) fprintf(stderr, "[spu] kon%d %04X\n", i - R_KON0, v);
        return;
    case R_KOFF0: case R_KOFF1:
        regs[i] = v;
        for (int b = 0; b < 16; b++)
            if (v & (1u << b) && (i == R_KOFF0 || b < 8)) key_off(b + (i == R_KOFF1) * 16);
        return;
    case R_ENDX0: case R_ENDX1: case R_STATX: return;     /* read-only */
    case R_ESA: regs[i] = v; rev_addr = (uint32_t)v * 8; return;
    case R_TSA: regs[i] = v; xfer_addr = ((uint32_t)v * 8) & (RAM_SIZE - 1); return;
    case R_FIFO: xfer_write(v); return;               /* written through immediately */
    case R_ATTR:
        regs[i] = v;
        if (!(v & 0x40)) statx_irq = 0;                /* IRQ9 acknowledge */
        return;
    }
    regs[i] = v;
}

void spu_init(void) {
    const char *t = getenv("PSX_TRACE_SPU");
    trace = t ? (atoi(t) ? atoi(t) : 1) : 0;
    next_sample = psx_cycles();
    audio_init();
}
