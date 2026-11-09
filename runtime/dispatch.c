/* Indirect call/jump dispatch for recompiled code.
 *
 * Resident segments (kernel, display) are always valid. Overlay slots hold whichever
 * overlay the kernel last loaded; we identify it by comparing signature words (code
 * words unique to that overlay) against current RAM contents.
 */
#include <stdio.h>
#include <stdlib.h>
#include "psx.h"

static const SegmentInfo *slot_cache[256];

static int sig_matches(const SegmentInfo *s) {
    for (int i = 0; i < s->nsig; i++)
        if (MEM_LW(s->sig[i].addr) != s->sig[i].word) return 0;
    return s->nsig > 0;
}

static const SegmentInfo *segment_for(uint32_t addr) {
    addr = (addr & 0x1FFFFFFFu) | 0x80000000u;
    for (int i = 0; i < psx_segment_count; i++) {
        const SegmentInfo *s = &psx_segments[i];
        if (s->slot == 0 && addr >= s->base && addr < s->base + s->size) return s;
    }
    for (int i = 0; i < psx_segment_count; i++) {
        const SegmentInfo *s = &psx_segments[i];
        if (s->slot == 0 || addr < s->base || addr >= s->base + s->size) continue;
        const SegmentInfo *c = slot_cache[(uint8_t)s->slot];
        if (c && addr >= c->base && addr < c->base + c->size && sig_matches(c)) return c;
        if (sig_matches(s)) { slot_cache[(uint8_t)s->slot] = s; return s; }
    }
    return NULL;
}

static RecompFunc lookup(uint32_t addr) {
    const SegmentInfo *s = segment_for(addr);
    if (!s) return NULL;
    addr = (addr & 0x1FFFFFFFu) | 0x80000000u;
    int lo = 0, hi = s->nfuncs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s->funcs[mid].addr == addr) return s->funcs[mid].fn;
        if (s->funcs[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

void call_dynamic(CPU *c, uint32_t addr) {
    RecompFunc f = lookup(addr);
    if (!f) {
        fprintf(stderr, "call_dynamic: no function at %08X (ra=%08X)\n", addr, c->r[31]);
        FILE *d = fopen("build/ram_crash.bin", "wb");
        if (d) { fwrite(psx_ram, 1, PSX_RAM_SIZE, d); fclose(d); fprintf(stderr, "RAM dumped to build/ram_crash.bin\n"); }
        abort();
    }
    f(c);
}

void jump_dynamic(CPU *c, uint32_t addr) {
    /* A tail jump behaves like a call whose callee returns to our caller. */
    call_dynamic(c, addr);
}
