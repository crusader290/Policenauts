/* MDEC macroblock decoder.
 *
 * Input is a word stream (register writes to 0x1F801820 or DMA0): a command word
 * followed by its parameter words. Command 1 decodes run-length coded blocks, 2 loads
 * the quantization tables, 3 loads the IDCT matrix. Decoded pixels go to an output
 * FIFO that DMA1 (or reads of 0x1F801820) drain.
 *
 * Decoding is incremental: each time parameters arrive, every macroblock whose data is
 * complete is decoded, so output is available as soon as the input for it is. The
 * arithmetic follows the hardware description in psx-spx (dequantize, 2-pass IDCT
 * with the uploaded matrix, YUV->RGB with Cr/Cb at half resolution).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psx.h"
#include "mdec.h"

static uint32_t ctrl;
static uint32_t cmd;               /* current command word */
static uint32_t remain;            /* parameter words still expected for `cmd` */
static uint32_t nparams;           /* parameter words received for `cmd` */

static uint16_t *in;               /* command 1 parameters, as halfwords */
static size_t in_len, in_pos, in_cap;
static uint8_t tbl_bytes[128];     /* command 2/3 parameters */

static uint32_t *out;              /* output FIFO */
static size_t out_len, out_pos, out_cap;
static uint32_t out_bytes, out_acc; /* partial word being assembled */

static uint8_t qt_y[64], qt_c[64];
static int32_t scale[64];          /* IDCT matrix, 1.15 fixed point: scale[u*8+x] */
static int zagzig[64];             /* scan index -> natural (row-major) position */
static int cur_block;              /* status bits 18-16 */
static int trace;

static const uint8_t zigzag[64] = {
     0,  1,  5,  6, 14, 15, 27, 28,  2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,  9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63,
};

static int depth(void)  { return (cmd >> 27) & 3; }   /* 0:4bit 1:8bit 2:24bit 3:15bit */
static int is_signed(void) { return (cmd >> 26) & 1; }
static int set_bit15(void) { return (cmd >> 25) & 1; }

/* ---- output FIFO ---------------------------------------------------------------- */
static void out_word(uint32_t w) {
    if (out_pos == out_len) out_pos = out_len = 0;     /* drained: reuse from the start */
    if (out_len == out_cap) {
        out_cap = out_cap ? out_cap * 2 : 0x4000;
        out = realloc(out, out_cap * sizeof *out);
    }
    out[out_len++] = w;
}
static void out_byte(uint8_t b) {
    out_acc |= (uint32_t)b << (8 * out_bytes);
    if (++out_bytes == 4) { out_word(out_acc); out_acc = 0; out_bytes = 0; }
}
static void out_half(uint16_t h) { out_byte((uint8_t)h); out_byte((uint8_t)(h >> 8)); }

size_t mdec_out_avail(void) { return out_len - out_pos; }

static uint32_t out_pop(void) {
    if (out_pos < out_len) return out[out_pos++];
    return 0;
}

/* ---- block decoding ------------------------------------------------------------- */
static int32_t sext10(uint16_t n) { return ((int32_t)(n & 0x3FF) ^ 0x200) - 0x200; }
static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Run-length decode + dequantize one 8x8 block starting at in[*pos]. Returns 0 (and
 * leaves *pos alone) when the block's data hasn't fully arrived yet. */
static int rl_block(size_t *pos, int32_t blk[64], const uint8_t *qt) {
    size_t p = *pos;
    uint16_t n;
    do {
        if (p >= in_len) return 0;
        n = in[p++];
    } while (n == 0xFE00);                              /* padding between blocks */
    memset(blk, 0, 64 * sizeof *blk);
    int q = (n >> 10) & 0x3F, k = 0;
    int32_t val = sext10(n) * qt[0];
    for (;;) {
        if (q == 0) val = sext10(n) * 2;                /* raw mode: no table, no zigzag */
        val = clampi(val, -0x400, 0x3FF);
        if (q) blk[zagzig[k]] = val; else blk[k] = val;
        if (p >= in_len) return 0;
        n = in[p++];
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63) break;                              /* 0xFE00 = end of block */
        val = (sext10(n) * qt[k] * q + 4) / 8;
    }
    *pos = p;
    return 1;
}

/* Two transposing 1-D passes with the uploaded matrix (each pass is M/2 in 1.15). */
static void idct(int32_t blk[64]) {
    int64_t tmp[64];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int64_t s = 0;
            for (int z = 0; z < 8; z++) s += (int64_t)blk[y + z * 8] * scale[x + z * 8];
            tmp[x + y * 8] = s;
        }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int64_t s = 0;
            for (int z = 0; z < 8; z++) s += tmp[y + z * 8] * scale[x + z * 8];
            blk[x + y * 8] = (int32_t)((s + (1ll << 31)) >> 32);
        }
}

static uint8_t to_u8(int32_t v) {                      /* signed sample -> output byte */
    v = clampi(v, -128, 127);
    return (uint8_t)(is_signed() ? v : v ^ 0x80);
}

static void emit_mono(const int32_t y[64]) {
    if (depth() == 1) {
        for (int i = 0; i < 64; i++) out_byte(to_u8(y[i]));
    } else {
        for (int i = 0; i < 64; i += 2)
            out_byte((uint8_t)((to_u8(y[i]) >> 4) | (to_u8(y[i + 1]) & 0xF0)));
    }
}

static void emit_color(const int32_t *cr, const int32_t *cb, int32_t (*ys)[64]) {
    for (int py = 0; py < 16; py++)
        for (int px = 0; px < 16; px++) {
            int c = (px >> 1) + (py >> 1) * 8;
            int32_t Y = ys[(py >> 3) * 2 + (px >> 3)][(px & 7) + (py & 7) * 8];
            int32_t R = (cr[c] * 1436 + 512) >> 10;                     /*  1.402  Cr */
            int32_t G = (cb[c] * -352 + cr[c] * -731 + 512) >> 10;      /* -.3437 Cb -.7143 Cr */
            int32_t B = (cb[c] * 1815 + 512) >> 10;                     /*  1.772  Cb */
            uint8_t r = to_u8(Y + R), g = to_u8(Y + G), b = to_u8(Y + B);
            if (depth() == 2) {
                out_byte(r); out_byte(g); out_byte(b);
            } else {
                out_half((uint16_t)((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10 |
                                    (set_bit15() ? 0x8000 : 0)));
            }
        }
}

/* Decode every macroblock whose input is complete. */
static void decode_available(void) {
    int32_t blk[6][64];
    for (;;) {
        size_t p = in_pos;
        if (depth() <= 1) {
            if (!rl_block(&p, blk[0], qt_y)) return;
            idct(blk[0]);
            cur_block = 4;
            emit_mono(blk[0]);
        } else {
            for (int b = 0; b < 6; b++) {                /* Cr, Cb, Y1..Y4 */
                if (!rl_block(&p, blk[b], b < 2 ? qt_c : qt_y)) return;
            }
            for (int b = 0; b < 6; b++) idct(blk[b]);
            emit_color(blk[0], blk[1], &blk[2]);
        }
        in_pos = p;
    }
}

/* ---- command stream ------------------------------------------------------------- */
static void finish_command(void) {
    int op = cmd >> 29;
    if (op == 2) {
        memcpy(qt_y, tbl_bytes, 64);
        if (cmd & 1) memcpy(qt_c, tbl_bytes + 64, 64);
    } else if (op == 3) {
        for (int i = 0; i < 64; i++)
            scale[i] = (int16_t)(tbl_bytes[2 * i] | tbl_bytes[2 * i + 1] << 8);
    } else if (op == 1) {
        /* leftover halfwords that never formed a whole block are dropped; a 24-bit
           stream always ends on a word boundary in the FIFO anyway */
        in_len = in_pos = 0;
    }
    if (trace) fprintf(stderr, "[mdec] cmd %08X done, out=%zu words\n", cmd, mdec_out_avail());
}

static void feed(uint32_t w) {
    if (!remain) {                                      /* new command */
        cmd = w;
        nparams = 0;
        int op = w >> 29;
        switch (op) {
        case 1: remain = w & 0xFFFF; in_len = in_pos = 0; break;
        case 2: remain = (w & 1) ? 32 : 16; break;
        case 3: remain = 32; break;
        default: remain = w & 0xFFFF; break;           /* no function: params ignored */
        }
        if (trace) fprintf(stderr, "[mdec] cmd %08X (%u params)\n", w, remain);
        if (!remain) finish_command();
        return;
    }
    int op = cmd >> 29;
    if (op == 1) {
        if (in_len + 2 > in_cap) {
            in_cap = in_cap ? in_cap * 2 : 0x8000;
            in = realloc(in, in_cap * sizeof *in);
        }
        in[in_len++] = (uint16_t)w;
        in[in_len++] = (uint16_t)(w >> 16);
    } else if ((op == 2 || op == 3) && nparams < 32) {
        memcpy(&tbl_bytes[nparams * 4], &w, 4);
    }
    nparams++;
    if (--remain == 0) {
        if (op == 1) decode_available();
        finish_command();
    }
}

void mdec_dma_in(uint32_t madr, uint32_t words) {
    for (uint32_t i = 0; i < words; i++)
        feed(MEM_LW(0x80000000u | ((madr + 4 * i) & 0x1FFFFC)));
    if ((cmd >> 29) == 1 && remain) decode_available();
}

void mdec_dma_out(uint32_t madr, uint32_t words) {
    for (uint32_t i = 0; i < words; i++)
        MEM_SW(0x80000000u | ((madr + 4 * i) & 0x1FFFFC), out_pop());
}

/* ---- registers ------------------------------------------------------------------ */
static void reset(void) {
    cmd = remain = nparams = 0;
    in_len = in_pos = 0;
    out_len = out_pos = 0;
    out_bytes = out_acc = 0;
    cur_block = 4;
}

uint32_t mdec_read(int reg) {
    if (reg == 0) return out_pop();
    uint32_t s = 0;
    if (!mdec_out_avail()) s |= 1u << 31;                       /* data-out FIFO empty */
    if (remain) s |= 1u << 29;                                  /* command busy */
    if ((ctrl & (1u << 30)) && remain) s |= 1u << 28;           /* data-in request */
    if ((ctrl & (1u << 29)) && mdec_out_avail()) s |= 1u << 27; /* data-out request */
    s |= (cmd >> 2) & 0x07800000u;                              /* depth/signed/bit15 */
    s |= (uint32_t)cur_block << 16;
    s |= (remain - 1) & 0xFFFF;
    return s;
}

void mdec_write(int reg, uint32_t v) {
    if (reg == 0) {
        feed(v);
        if ((cmd >> 29) == 1 && remain) decode_available();
        return;
    }
    if (v & (1u << 31)) reset();
    ctrl = v;
}

void mdec_init(void) {
    trace = getenv("PSX_TRACE_MDEC") != NULL;
    for (int i = 0; i < 64; i++) zagzig[zigzag[i]] = i;
    reset();
}
