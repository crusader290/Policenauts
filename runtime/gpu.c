/* PS1 GPU: 1 MB VRAM (1024x512x16), GP0 drawing/transfer commands, GP1 display
 * control, and a software rasterizer.
 *
 * Rasterization follows the hardware's rules where it matters for 2D games: integer
 * vertex positions, top-left fill convention (right/bottom edges excluded), quads drawn
 * as triangles (0,1,2) + (1,2,3), 16.16 attribute gradients, texture windows, CLUT
 * textures, semi-transparency modes, mask bits and 4x4 dithering.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu.h"

static uint16_t vram[512][1024];

static struct {
    /* command assembly */
    uint32_t buf[64];
    int n, need;
    int polyline;

    /* VRAM transfers */
    int xfer;                    /* 0 none, 1 CPU->VRAM, 2 VRAM->CPU */
    int tx, ty, tw, th, tcx, tcy;
    uint32_t gpuread;

    /* drawing state */
    uint32_t texpage;            /* E1 bits 0-11 */
    int dither, draw_display, tex_disable, flip_x, flip_y;
    uint32_t tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    int da_x1, da_y1, da_x2, da_y2;
    int off_x, off_y;
    int mask_set, mask_check;
    uint32_t e2, e3, e4, e5;

    /* display */
    int disp_x, disp_y;
    int hr1, hr2, vr1, vr2;
    uint32_t mode;               /* GP1(08) */
    int display_off;
    int dma_dir;
    int irq;
} g;

const uint16_t *gpu_vram(void) { return &vram[0][0]; }

static int sext11(uint32_t v) { return (int)(v << 21) >> 21; }

/* ---- pixel pipeline --------------------------------------------------------------- */
static const int dither_tab[4][4] = {
    { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 },
};

static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* blend 15-bit front over back using the current semi-transparency mode */
static inline uint16_t blend(uint16_t b, uint16_t f, int mode) {
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    int fr = f & 31, fg = (f >> 5) & 31, fb = (f >> 10) & 31;
    int r, gg, bl;
    switch (mode) {
    case 0: r = (br + fr) >> 1; gg = (bg + fg) >> 1; bl = (bb + fb) >> 1; break;
    case 1: r = br + fr; gg = bg + fg; bl = bb + fb; break;
    case 2: r = br - fr; gg = bg - fg; bl = bb - fb; break;
    default: r = br + (fr >> 2); gg = bg + (fg >> 2); bl = bb + (fb >> 2); break;
    }
    r = r < 0 ? 0 : r > 31 ? 31 : r;
    gg = gg < 0 ? 0 : gg > 31 ? 31 : gg;
    bl = bl < 0 ? 0 : bl > 31 ? 31 : bl;
    return (uint16_t)(r | (gg << 5) | (bl << 10));
}

static inline void put(int x, int y, uint16_t c, int semi, int mode) {
    uint16_t *p = &vram[y][x];
    if (g.mask_check && (*p & 0x8000)) return;
    uint16_t m = c & 0x8000;
    if (semi) c = blend(*p, c, mode);
    *p = (uint16_t)((c & 0x7FFF) | m | (g.mask_set ? 0x8000 : 0));
}

static inline int in_area(int x, int y) {
    return x >= g.da_x1 && x <= g.da_x2 && y >= g.da_y1 && y <= g.da_y2;
}

/* 8-bit RGB -> 15-bit, optionally dithered */
static inline uint16_t rgb15(int r, int gg, int b, int x, int y, int dith) {
    if (dith) {
        int d = dither_tab[y & 3][x & 3];
        r = clamp8(r + d); gg = clamp8(gg + d); b = clamp8(b + d);
    }
    return (uint16_t)((r >> 3) | ((gg >> 3) << 5) | ((b >> 3) << 10));
}

typedef struct { int depth, px, py, cx, cy; } Tex;

static inline uint16_t texel(const Tex *t, int u, int v) {
    u &= 0xFF; v &= 0xFF;
    u = (int)((u & ~(g.tw_mask_x * 8)) | ((g.tw_off_x & g.tw_mask_x) * 8));
    v = (int)((v & ~(g.tw_mask_y * 8)) | ((g.tw_off_y & g.tw_mask_y) * 8));
    int y = (t->py + v) & 511;
    switch (t->depth) {
    case 0: {
        uint16_t w = vram[y][(t->px + (u >> 2)) & 1023];
        return vram[t->cy][(t->cx + ((w >> ((u & 3) * 4)) & 0xF)) & 1023];
    }
    case 1: {
        uint16_t w = vram[y][(t->px + (u >> 1)) & 1023];
        return vram[t->cy][(t->cx + ((w >> ((u & 1) * 8)) & 0xFF)) & 1023];
    }
    default:
        return vram[y][(t->px + u) & 1023];
    }
}

static void tex_from(Tex *t, uint32_t tpage, uint32_t clut) {
    t->px = (int)(tpage & 0xF) * 64;
    t->py = (int)((tpage >> 4) & 1) * 256;
    t->depth = (int)((tpage >> 7) & 3);
    t->cx = (int)(clut & 0x3F) * 16;
    t->cy = (int)(clut >> 6) & 511;
}

/* shade a textured pixel: returns 0 if transparent */
static inline int shade_tex(uint16_t tc, int raw, int r, int gg, int b, int x, int y,
                            int dith, uint16_t *out) {
    if (tc == 0) return 0;
    if (raw) { *out = tc; return 1; }
    int tr = (tc & 31) << 3, tg = ((tc >> 5) & 31) << 3, tb = ((tc >> 10) & 31) << 3;
    uint16_t c = rgb15(clamp8((tr * r) >> 7), clamp8((tg * gg) >> 7), clamp8((tb * b) >> 7),
                       x, y, dith);
    *out = (uint16_t)(c | (tc & 0x8000));
    return 1;
}

/* ---- triangles --------------------------------------------------------------------- */
typedef struct { int x, y, r, g, b, u, v; } Vtx;

typedef struct {
    int gouraud, textured, raw, semi, mode;
    Tex tex;
    uint32_t flat;
} Prim;

static int64_t edge(const Vtx *a, const Vtx *b, int x, int y) {
    return (int64_t)(b->x - a->x) * (y - a->y) - (int64_t)(b->y - a->y) * (x - a->x);
}

static int is_top_left(const Vtx *a, const Vtx *b) {
    /* for counter-clockwise (positive area) winding in screen space with y down */
    int dy = b->y - a->y, dx = b->x - a->x;
    return (dy < 0) || (dy == 0 && dx > 0);
}

static void grad(int64_t area, const Vtx *v0, const Vtx *v1, const Vtx *v2,
                 int a0, int a1, int a2, int64_t *dx, int64_t *dy) {
    int64_t d1 = a1 - a0, d2 = a2 - a0;
    *dx = ((d1 * (v2->y - v0->y) - d2 * (v1->y - v0->y)) * 65536) / area;
    *dy = ((d2 * (v1->x - v0->x) - d1 * (v2->x - v0->x)) * 65536) / area;
}

static void triangle(const Prim *p, Vtx a, Vtx b, Vtx c) {
    const Vtx *v0 = &a, *v1 = &b, *v2 = &c;
    int64_t area = edge(v0, v1, v2->x, v2->y);
    if (area == 0) return;
    if (area < 0) { const Vtx *t = v1; v1 = v2; v2 = t; area = -area; }
    int minx = v0->x, maxx = v0->x, miny = v0->y, maxy = v0->y;
    const Vtx *vs[3] = { v0, v1, v2 };
    for (int i = 1; i < 3; i++) {
        if (vs[i]->x < minx) minx = vs[i]->x;
        if (vs[i]->x > maxx) maxx = vs[i]->x;
        if (vs[i]->y < miny) miny = vs[i]->y;
        if (vs[i]->y > maxy) maxy = vs[i]->y;
    }
    if (maxx - minx >= 1024 || maxy - miny >= 512) return;    /* hardware rejects these */
    if (minx < g.da_x1) minx = g.da_x1;
    if (miny < g.da_y1) miny = g.da_y1;
    if (maxx > g.da_x2) maxx = g.da_x2;
    if (maxy > g.da_y2) maxy = g.da_y2;
    if (minx > maxx || miny > maxy) return;

    int64_t drdx = 0, drdy = 0, dgdx = 0, dgdy = 0, dbdx = 0, dbdy = 0;
    int64_t dudx = 0, dudy = 0, dvdx = 0, dvdy = 0;
    if (p->gouraud) {
        grad(area, v0, v1, v2, v0->r, v1->r, v2->r, &drdx, &drdy);
        grad(area, v0, v1, v2, v0->g, v1->g, v2->g, &dgdx, &dgdy);
        grad(area, v0, v1, v2, v0->b, v1->b, v2->b, &dbdx, &dbdy);
    }
    if (p->textured) {
        grad(area, v0, v1, v2, v0->u, v1->u, v2->u, &dudx, &dudy);
        grad(area, v0, v1, v2, v0->v, v1->v, v2->v, &dvdx, &dvdy);
    }
    int b0 = is_top_left(v1, v2) ? 0 : -1;
    int b1 = is_top_left(v2, v0) ? 0 : -1;
    int b2 = is_top_left(v0, v1) ? 0 : -1;
    int dith = g.dither && (p->gouraud || (p->textured && !p->raw));
    int fr = p->flat & 0xFF, fg = (p->flat >> 8) & 0xFF, fb = (p->flat >> 16) & 0xFF;

    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int64_t w0 = edge(v1, v2, x, y), w1 = edge(v2, v0, x, y), w2 = edge(v0, v1, x, y);
            if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
            int dx = x - v0->x, dy = y - v0->y;
            int r = fr, gg = fg, bl = fb;
            if (p->gouraud) {
                r = clamp8((int)(((int64_t)v0->r * 65536 + drdx * dx + drdy * dy + 32768) >> 16));
                gg = clamp8((int)(((int64_t)v0->g * 65536 + dgdx * dx + dgdy * dy + 32768) >> 16));
                bl = clamp8((int)(((int64_t)v0->b * 65536 + dbdx * dx + dbdy * dy + 32768) >> 16));
            }
            uint16_t out;
            if (p->textured) {
                int u = (int)(((int64_t)v0->u * 65536 + dudx * dx + dudy * dy + 32768) >> 16);
                int v = (int)(((int64_t)v0->v * 65536 + dvdx * dx + dvdy * dy + 32768) >> 16);
                if (!shade_tex(texel(&p->tex, u, v), p->raw, r, gg, bl, x, y, dith, &out)) continue;
                put(x, y, out, p->semi && (out & 0x8000), p->mode);
            } else {
                put(x, y, rgb15(r, gg, bl, x, y, dith), p->semi, p->mode);
            }
        }
    }
}

/* ---- primitives -------------------------------------------------------------------- */
static void vtx_xy(Vtx *v, uint32_t w) {
    v->x = sext11(w) + g.off_x;
    v->y = sext11(w >> 16) + g.off_y;
}
static void vtx_rgb(Vtx *v, uint32_t w) {
    v->r = (int)(w & 0xFF); v->g = (int)((w >> 8) & 0xFF); v->b = (int)((w >> 16) & 0xFF);
}

static uint32_t semi_mode_of(uint32_t tpage) { return (tpage >> 5) & 3; }

static void cmd_polygon(void) {
    uint32_t cmd = g.buf[0] >> 24;
    int gouraud = (cmd >> 4) & 1, quad = (cmd >> 3) & 1, tex = (cmd >> 2) & 1;
    Prim p = { gouraud, tex, cmd & 1, (cmd >> 1) & 1, 0, { 0 }, g.buf[0] & 0xFFFFFF };
    int nv = quad ? 4 : 3;
    Vtx v[4];
    int k = 0;
    uint32_t clut = 0, tpage = g.texpage;
    for (int i = 0; i < nv; i++) {
        if (i == 0 || !gouraud) vtx_rgb(&v[i], g.buf[0]);
        else vtx_rgb(&v[i], g.buf[k++]);
        if (i == 0) k = 1;
        vtx_xy(&v[i], g.buf[k++]);
        if (tex) {
            uint32_t w = g.buf[k++];
            v[i].u = (int)(w & 0xFF); v[i].v = (int)((w >> 8) & 0xFF);
            if (i == 0) clut = w >> 16;
            if (i == 1) tpage = w >> 16;
        } else {
            v[i].u = v[i].v = 0;
        }
    }
    if (tex) {
        /* the polygon's tpage also becomes the current draw mode texpage */
        g.texpage = (g.texpage & ~0x9FFu) | (tpage & 0x9FFu);
        tex_from(&p.tex, tpage, clut);
    }
    p.mode = (int)semi_mode_of(tex ? tpage : g.texpage);
    if (!gouraud) p.flat = g.buf[0] & 0xFFFFFF;
    triangle(&p, v[0], v[1], v[2]);
    if (quad) triangle(&p, v[1], v[2], v[3]);
}

static void line(const Vtx *a, const Vtx *b, int gouraud, int semi) {
    int dx = b->x - a->x, dy = b->y - a->y;
    if (abs(dx) >= 1024 || abs(dy) >= 512) return;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    int mode = (int)semi_mode_of(g.texpage);
    for (int i = 0; i <= steps; i++) {
        int x = a->x + (steps ? (dx * i + (dx >= 0 ? steps / 2 : -steps / 2)) / steps : 0);
        int y = a->y + (steps ? (dy * i + (dy >= 0 ? steps / 2 : -steps / 2)) / steps : 0);
        if (!in_area(x, y)) continue;
        int r = a->r, gg = a->g, bl = a->b;
        if (gouraud && steps) {
            r = a->r + (b->r - a->r) * i / steps;
            gg = a->g + (b->g - a->g) * i / steps;
            bl = a->b + (b->b - a->b) * i / steps;
        }
        put(x, y, rgb15(r, gg, bl, x, y, g.dither && gouraud), semi, mode);
    }
}

static void cmd_line(void) {
    uint32_t cmd = g.buf[0] >> 24;
    int gouraud = (cmd >> 4) & 1, semi = (cmd >> 1) & 1;
    Vtx prev, cur;
    vtx_rgb(&prev, g.buf[0]);
    vtx_xy(&prev, g.buf[1]);
    int k = 2;
    while (k < g.n) {
        if (gouraud) vtx_rgb(&cur, g.buf[k++]); else vtx_rgb(&cur, g.buf[0]);
        if (k >= g.n) break;
        vtx_xy(&cur, g.buf[k++]);
        line(&prev, &cur, gouraud, semi);
        prev = cur;
    }
}

static void cmd_rect(void) {
    uint32_t cmd = g.buf[0] >> 24;
    int tex = (cmd >> 2) & 1, raw = cmd & 1, semi = (cmd >> 1) & 1, size = (cmd >> 3) & 3;
    int x0 = sext11(g.buf[1]) + g.off_x, y0 = sext11(g.buf[1] >> 16) + g.off_y;
    int k = 2, u0 = 0, v0 = 0;
    Tex t = { 0 };
    if (tex) {
        uint32_t w = g.buf[k++];
        u0 = (int)(w & 0xFF); v0 = (int)((w >> 8) & 0xFF);
        tex_from(&t, g.texpage, w >> 16);
    }
    int w, h;
    switch (size) {
    case 0: w = (int)(g.buf[k] & 0x3FF); h = (int)((g.buf[k] >> 16) & 0x1FF); break;
    case 1: w = h = 1; break;
    case 2: w = h = 8; break;
    default: w = h = 16; break;
    }
    int r = g.buf[0] & 0xFF, gg = (g.buf[0] >> 8) & 0xFF, bl = (g.buf[0] >> 16) & 0xFF;
    int mode = (int)semi_mode_of(g.texpage);
    for (int yy = 0; yy < h; yy++) {
        int y = y0 + yy;
        if (y < g.da_y1 || y > g.da_y2) continue;
        for (int xx = 0; xx < w; xx++) {
            int x = x0 + xx;
            if (x < g.da_x1 || x > g.da_x2) continue;
            if (tex) {
                int u = g.flip_x ? u0 - xx : u0 + xx;
                int v = g.flip_y ? v0 - yy : v0 + yy;
                uint16_t out;
                if (!shade_tex(texel(&t, u, v), raw, r, gg, bl, x, y, 0, &out)) continue;
                put(x, y, out, semi && (out & 0x8000), mode);
            } else {
                put(x, y, rgb15(r, gg, bl, x, y, 0), semi, mode);
            }
        }
    }
}

static void cmd_fill(void) {
    uint32_t c = g.buf[0];
    uint16_t col = (uint16_t)(((c >> 3) & 31) | (((c >> 11) & 31) << 5) | (((c >> 19) & 31) << 10));
    int x0 = (int)(g.buf[1] & 0x3F0), y0 = (int)((g.buf[1] >> 16) & 0x1FF);
    int w = (int)(((g.buf[2] & 0x3FF) + 15) & ~15u), h = (int)((g.buf[2] >> 16) & 0x1FF);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) vram[(y0 + y) & 511][(x0 + x) & 1023] = col;
}

static void cmd_copy(void) {
    int sx = (int)(g.buf[1] & 0x3FF), sy = (int)((g.buf[1] >> 16) & 0x1FF);
    int dx = (int)(g.buf[2] & 0x3FF), dy = (int)((g.buf[2] >> 16) & 0x1FF);
    int w = (int)(((g.buf[3] & 0xFFFF) - 1) & 0x3FF) + 1, h = (int)((((g.buf[3] >> 16) - 1) & 0x1FF)) + 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t c = vram[(sy + y) & 511][(sx + x) & 1023];
            uint16_t *p = &vram[(dy + y) & 511][(dx + x) & 1023];
            if (g.mask_check && (*p & 0x8000)) continue;
            *p = (uint16_t)(c | (g.mask_set ? 0x8000 : 0));
        }
}

static void begin_xfer(int dir) {
    g.tx = (int)(g.buf[1] & 0x3FF); g.ty = (int)((g.buf[1] >> 16) & 0x1FF);
    g.tw = (int)(((g.buf[2] & 0xFFFF) - 1) & 0x3FF) + 1;
    g.th = (int)((((g.buf[2] >> 16) - 1) & 0x1FF)) + 1;
    g.tcx = g.tcy = 0;
    g.xfer = dir;
}

static void xfer_pixel(uint16_t c) {
    if (g.tcy >= g.th) return;
    uint16_t *p = &vram[(g.ty + g.tcy) & 511][(g.tx + g.tcx) & 1023];
    if (!(g.mask_check && (*p & 0x8000))) *p = (uint16_t)(c | (g.mask_set ? 0x8000 : 0));
    if (++g.tcx == g.tw) { g.tcx = 0; g.tcy++; }
}

static uint16_t xfer_read_pixel(void) {
    if (g.tcy >= g.th) return 0;
    uint16_t c = vram[(g.ty + g.tcy) & 511][(g.tx + g.tcx) & 1023];
    if (++g.tcx == g.tw) { g.tcx = 0; g.tcy++; }
    return c;
}

/* ---- GP0 ----------------------------------------------------------------------------- */
static int cmd_length(uint32_t cmd) {
    if (cmd >= 0x20 && cmd < 0x40) {
        int gouraud = (cmd >> 4) & 1, nv = (cmd & 8) ? 4 : 3, tex = (cmd >> 2) & 1;
        return 1 + nv * (1 + tex) + (gouraud ? nv - 1 : 0);
    }
    if (cmd >= 0x40 && cmd < 0x60) {
        if (cmd & 8) return -1;                              /* polyline: until terminator */
        return (cmd & 0x10) ? 4 : 3;
    }
    if (cmd >= 0x60 && cmd < 0x80) return 2 + ((cmd >> 2) & 1) + (((cmd >> 3) & 3) == 0);
    if (cmd >= 0x80 && cmd < 0xA0) return 4;
    if (cmd >= 0xA0 && cmd < 0xE0) return 3;
    if (cmd == 0x02) return 3;
    return 1;
}

static int trace_gpu;

static void execute(void) {
    uint32_t cmd = g.buf[0] >> 24;
    if (trace_gpu > 1) {
        fprintf(stderr, "[gpu] GP0");
        for (int i = 0; i < g.n && i < 12; i++) fprintf(stderr, " %08X", g.buf[i]);
        fprintf(stderr, "\n");
    }
    if (cmd >= 0x20 && cmd < 0x40) cmd_polygon();
    else if (cmd >= 0x40 && cmd < 0x60) cmd_line();
    else if (cmd >= 0x60 && cmd < 0x80) cmd_rect();
    else if (cmd >= 0x80 && cmd < 0xA0) cmd_copy();
    else if (cmd >= 0xA0 && cmd < 0xC0) begin_xfer(1);
    else if (cmd >= 0xC0 && cmd < 0xE0) begin_xfer(2);
    else switch (cmd) {
    case 0x02: cmd_fill(); break;
    case 0x1F: g.irq = 1; break;
    case 0xE1:
        g.texpage = g.buf[0] & 0x9FF;
        g.dither = (g.buf[0] >> 9) & 1;
        g.draw_display = (g.buf[0] >> 10) & 1;
        g.tex_disable = (g.buf[0] >> 11) & 1;
        g.flip_x = (g.buf[0] >> 12) & 1;
        g.flip_y = (g.buf[0] >> 13) & 1;
        break;
    case 0xE2:
        g.e2 = g.buf[0] & 0xFFFFF;
        g.tw_mask_x = g.buf[0] & 31; g.tw_mask_y = (g.buf[0] >> 5) & 31;
        g.tw_off_x = (g.buf[0] >> 10) & 31; g.tw_off_y = (g.buf[0] >> 15) & 31;
        break;
    case 0xE3: g.e3 = g.buf[0] & 0xFFFFF; g.da_x1 = (int)(g.buf[0] & 0x3FF); g.da_y1 = (int)((g.buf[0] >> 10) & 0x1FF); break;
    case 0xE4: g.e4 = g.buf[0] & 0xFFFFF; g.da_x2 = (int)(g.buf[0] & 0x3FF); g.da_y2 = (int)((g.buf[0] >> 10) & 0x1FF); break;
    case 0xE5: g.e5 = g.buf[0] & 0x3FFFFF; g.off_x = sext11(g.buf[0]); g.off_y = sext11(g.buf[0] >> 11); break;
    case 0xE6: g.mask_set = g.buf[0] & 1; g.mask_check = (g.buf[0] >> 1) & 1; break;
    default: break;                                          /* nop / cache clear */
    }
}

void gpu_gp0(uint32_t w) {
    if (g.xfer == 1) {
        xfer_pixel((uint16_t)w);
        xfer_pixel((uint16_t)(w >> 16));
        if (g.tcy >= g.th) g.xfer = 0;
        return;
    }
    if (g.n == 0) {
        g.need = cmd_length(w >> 24);
        g.polyline = g.need < 0;
    }
    if (g.n < (int)(sizeof g.buf / sizeof g.buf[0])) g.buf[g.n++] = w;
    if (g.polyline) {
        /* The terminator (0x5xxx5xxx) can only appear where a new vertex would start,
         * once at least two vertices are in. m counts words after the command word:
         * flat:    xy0, xy1, xy2...         -> vertex starts at m = 1, 2, 3...
         * gouraud: xy0, c1, xy1, c2, xy2... -> vertex starts at m = 1, 2, 4, 6... */
        int gouraud = ((g.buf[0] >> 24) & 0x10) != 0;
        int m = g.n - 1;
        int at_vertex = gouraud ? (m >= 4 && (m & 1) == 0) : (m >= 3);
        if (at_vertex && (w & 0xF000F000u) == 0x50005000u) {
            g.n--;                               /* drop terminator */
            execute();
            g.n = 0;
        } else if (g.n >= (int)(sizeof g.buf / sizeof g.buf[0])) {
            execute();
            g.n = 0;
        }
        return;
    }
    if (g.n >= g.need) {
        execute();
        g.n = 0;
    }
}

/* ---- GP1 ----------------------------------------------------------------------------- */
static void reset(void) {
    memset(&g, 0, sizeof g);
    g.display_off = 1;
    g.hr1 = 0x200; g.hr2 = 0x200 + 2560;
    g.vr1 = 0x10; g.vr2 = 0x10 + 240;
    g.da_x2 = 1023; g.da_y2 = 511;
}

void gpu_gp1(uint32_t w) {
    if (trace_gpu && (w >> 24) != 0x10) fprintf(stderr, "[gpu] GP1 %08X\n", w);
    switch (w >> 24) {
    case 0x00: reset(); break;
    case 0x01: g.n = 0; g.xfer = 0; break;
    case 0x02: g.irq = 0; break;
    case 0x03: g.display_off = w & 1; break;
    case 0x04: g.dma_dir = w & 3; break;
    case 0x05: g.disp_x = (int)(w & 0x3FE); g.disp_y = (int)((w >> 10) & 0x1FF); break;
    case 0x06: g.hr1 = (int)(w & 0xFFF); g.hr2 = (int)((w >> 12) & 0xFFF); break;
    case 0x07: g.vr1 = (int)(w & 0x3FF); g.vr2 = (int)((w >> 10) & 0x3FF); break;
    case 0x08: g.mode = w & 0xFF; break;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        switch (w & 7) {
        case 2: g.gpuread = g.e2; break;
        case 3: g.gpuread = g.e3; break;
        case 4: g.gpuread = g.e4; break;
        case 5: g.gpuread = g.e5; break;
        case 7: g.gpuread = 2; break;
        default: break;
        }
        break;
    default: break;
    }
}

uint32_t gpu_read(void) {
    if (g.xfer == 2) {
        uint32_t lo = xfer_read_pixel(), hi = xfer_read_pixel();
        if (g.tcy >= g.th) g.xfer = 0;
        return lo | (hi << 16);
    }
    return g.gpuread;
}

uint32_t gpu_stat(int odd_line) {
    uint32_t s = g.texpage & 0x1FF;
    s |= (uint32_t)g.dither << 9;
    s |= (uint32_t)g.draw_display << 10;
    s |= (uint32_t)g.mask_set << 11;
    s |= (uint32_t)g.mask_check << 12;
    s |= (uint32_t)g.tex_disable << 15;
    s |= ((g.mode >> 6) & 1) << 16;            /* 368 */
    s |= (g.mode & 3) << 17;                   /* width */
    s |= ((g.mode >> 2) & 1) << 19;            /* height */
    s |= ((g.mode >> 3) & 1) << 20;            /* PAL */
    s |= ((g.mode >> 4) & 1) << 21;            /* 24-bit */
    s |= ((g.mode >> 5) & 1) << 22;            /* interlace */
    s |= (uint32_t)g.display_off << 23;
    s |= (uint32_t)g.irq << 24;
    s |= 1u << 26;                             /* ready for command */
    s |= (g.xfer == 2 ? 1u : 0u) << 27;        /* ready to send VRAM to CPU */
    s |= 1u << 28;                             /* ready for DMA block */
    s |= (uint32_t)g.dma_dir << 29;
    switch (g.dma_dir) {                       /* DMA request bit */
    case 1: case 2: s |= 1u << 25; break;
    case 3: s |= (g.xfer == 2 ? 1u : 0u) << 25; break;
    }
    s |= (uint32_t)(odd_line & 1) << 31;
    return s;
}

/* ---- display ------------------------------------------------------------------------ */
int gpu_output(uint32_t *dst, int *pw, int *ph) {
    static const int widths[4] = { 256, 320, 512, 640 };
    int w = (g.mode & 0x40) ? 368 : widths[g.mode & 3];
    int h = ((g.mode & 0x24) == 0x24) ? 480 : 240;
    int lines = g.vr2 - g.vr1;
    if ((g.mode & 0x24) == 0x24) lines *= 2;
    if (lines > 0 && lines < h) h = lines;
    *pw = w; *ph = h;
    if (g.display_off) { memset(dst, 0, (size_t)w * h * 4); return 0; }
    for (int y = 0; y < h; y++) {
        int vy = (g.disp_y + y) & 511;
        uint32_t *o = dst + (size_t)y * w;
        if (g.mode & 0x10) {                   /* 24-bit (FMV) */
            const uint8_t *row = (const uint8_t *)vram[vy];
            for (int x = 0; x < w; x++) {
                int b = (g.disp_x * 2 + x * 3) & 2047;
                uint32_t r = row[b], gg = row[(b + 1) & 2047], bl = row[(b + 2) & 2047];
                o[x] = (r << 16) | (gg << 8) | bl;
            }
        } else {
            for (int x = 0; x < w; x++) {
                uint16_t c = vram[vy][(g.disp_x + x) & 1023];
                uint32_t r = c & 31, gg = (c >> 5) & 31, bl = (c >> 10) & 31;
                o[x] = (((r << 3) | (r >> 2)) << 16) | (((gg << 3) | (gg >> 2)) << 8) | ((bl << 3) | (bl >> 2));
            }
        }
    }
    return 1;
}

/* ---- debug dump --------------------------------------------------------------------- */
void gpu_dump_vram_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const int W = 1024, H = 512;
    uint32_t data = (uint32_t)W * H * 3, fsize = 54 + data;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &fsize, 4);
    uint32_t v = 54; memcpy(hdr + 10, &v, 4);
    v = 40; memcpy(hdr + 14, &v, 4);
    int32_t sw = W, sh = -H; memcpy(hdr + 18, &sw, 4); memcpy(hdr + 22, &sh, 4);
    uint16_t planes = 1, bpp = 24; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &data, 4);
    fwrite(hdr, 1, 54, f);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint16_t c = vram[y][x];
            uint8_t px[3] = { (uint8_t)(((c >> 10) & 31) << 3), (uint8_t)(((c >> 5) & 31) << 3),
                              (uint8_t)((c & 31) << 3) };
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

void gpu_init(void) {
    reset();
    const char *t = getenv("PSX_TRACE_GPU");
    trace_gpu = t ? (atoi(t) > 0 ? atoi(t) : 1) : 0;
}
