/* Native hooks attached to recompiled functions (see recomp/hooks.toml). */
#include <stdio.h>
#include <stdlib.h>
#include "psx.h"

static int klog = -1;

/* kernel debug log line (a0 = NUL-terminated text, usually ending in '\n') */
void hook_kernel_log(CPU *c) {
    if (klog < 0) klog = getenv("PSX_QUIET") == NULL;
    if (!klog) return;
    uint32_t p = c->r[4];
    fputs("[K] ", stderr);
    int ch, last = 0;
    for (int i = 0; i < 512 && (ch = MEM_LB(p + (uint32_t)i)) != 0; i++) { fputc(ch, stderr); last = ch; }
    if (last != '\n') fputc('\n', stderr);
}

/* ---- setjmp/longjmp ---------------------------------------------------------------- */
/* Host jump buffers, keyed by the game's jmp_buf address. The recompiler wraps each call
 * of the game's setjmp in __builtin_setjmp(psx_jmpbuf(a0)); the game's setjmp then runs
 * as usual and saves the MIPS registers. */
#define MAX_JMPBUF 16
static struct { uint32_t addr; void *buf[5]; } jmpbufs[MAX_JMPBUF];

void **psx_jmpbuf(uint32_t mips_buf) {
    for (int i = 0; i < MAX_JMPBUF; i++)
        if (jmpbufs[i].addr == mips_buf) return jmpbufs[i].buf;
    for (int i = 0; i < MAX_JMPBUF; i++)
        if (!jmpbufs[i].addr) { jmpbufs[i].addr = mips_buf; return jmpbufs[i].buf; }
    fprintf(stderr, "psx_jmpbuf: too many setjmp buffers\n");
    abort();
}

/* longjmp(a0 = jmp_buf, a1 = value): restore what setjmp saved, return a1 from setjmp */
void hook_longjmp(CPU *c) {
    uint32_t b = c->r[4];
    int i = 0;
    while (i < MAX_JMPBUF && jmpbufs[i].addr != b) i++;
    if (i == MAX_JMPBUF) {
        fprintf(stderr, "longjmp to %08X without a recorded setjmp\n", b);
        abort();
    }
    c->r[31] = MEM_LW(b + 0x00);
    c->r[29] = MEM_LW(b + 0x04);
    c->r[30] = MEM_LW(b + 0x08);
    for (int r = 0; r < 8; r++) c->r[16 + r] = MEM_LW(b + 0x0C + 4 * (uint32_t)r);
    c->r[28] = MEM_LW(b + 0x2C);
    c->r[2] = c->r[5];
    __builtin_longjmp(jmpbufs[i].buf, 1);
}

/* ---- disc swap ------------------------------------------------------------------- */
#include "cd.h"

void hook_disc_check(CPU *c) {
    int want = (int)c->r[4];
    static int warned;
    if ((want != 1 && want != 2) || cd_disc() == want) return;
    if (psx_disc_path(want - 1)) cd_swap(want, 0.0);   /* lid closes before the check runs */
    else if (!warned++) fprintf(stderr, "the game wants disc %d, but no image is configured (--disc%d)\n", want, want);
}

void hook_sprintf(CPU *c) {
    static const char fmt[] = "Insert Disc %d";
    uint32_t f = c->r[5];
    for (int i = 0; i < (int)sizeof fmt; i++)
        if (MEM_LB(f + (uint32_t)i) != (uint8_t)fmt[i]) return;
    int want = (int)c->r[6];
    if ((want == 1 || want == 2) && cd_disc() != want && psx_disc_path(want - 1)) cd_swap(want, 1.0);
}
