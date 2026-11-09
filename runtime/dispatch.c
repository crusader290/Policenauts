/* Indirect call/jump dispatch for recompiled code.
 *
 * Resident segments (kernel, display) are always valid. Overlay slots hold whichever
 * overlay the kernel last loaded; we identify it by comparing signature words (code
 * words unique to that overlay) against current RAM contents.
 */
#include <stdio.h>
#include <stdlib.h>
#include "psx.h"
#include "log.h"

static const SegmentInfo *slot_cache[256];

static int sig_matches(const SegmentInfo *s) {
    for (int i = 0; i < s->nsig; i++)
        if (MEM_LW(s->sig[i].addr) != s->sig[i].word) return 0;
    return s->nsig > 0;
}

static RecompFunc find_in(const SegmentInfo *s, uint32_t addr) {
    int lo = 0, hi = s->nfuncs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s->funcs[mid].addr == addr) return s->funcs[mid].fn;
        if (s->funcs[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* Segments can overlap at their edges (ITP.BIN runs 12 bytes into slot B), so every
 * segment that covers the address and is currently loaded gets a chance. */
static RecompFunc lookup(uint32_t addr) {
    addr = (addr & 0x1FFFFFFFu) | 0x80000000u;
    RecompFunc f;
    for (int i = 0; i < psx_segment_count; i++) {
        const SegmentInfo *s = &psx_segments[i];
        if (s->slot == 0 && addr >= s->base && addr < s->base + s->size && (f = find_in(s, addr)))
            return f;
    }
    for (int i = 0; i < psx_segment_count; i++) {
        const SegmentInfo *s = &psx_segments[i];
        if (s->slot == 0 || addr < s->base || addr >= s->base + s->size) continue;
        const SegmentInfo *c = slot_cache[(uint8_t)s->slot];
        if (c && addr >= c->base && addr < c->base + c->size && sig_matches(c) && (f = find_in(c, addr)))
            return f;
        if (s != c && sig_matches(s) && (f = find_in(s, addr))) {
            slot_cache[(uint8_t)s->slot] = s;
            return f;
        }
    }
    return NULL;
}

/* ---- diagnostics (watchdog.c) ------------------------------------------------------ */
#define NRECENT 32
static struct { uint32_t addr, ra; } recent[NRECENT];
static uint32_t recent_n;

int psx_recent_calls(uint32_t *addr, uint32_t *ra, int max) {   /* newest first */
    int n = 0;
    for (uint32_t i = recent_n; i > 0 && n < max && n < NRECENT; i--, n++) {
        addr[n] = recent[(i - 1) % NRECENT].addr;
        ra[n] = recent[(i - 1) % NRECENT].ra;
    }
    return n;
}

const char *psx_overlay_in_slot(char slot) {        /* NULL: nothing recognisable */
    for (int i = 0; i < psx_segment_count; i++)
        if (psx_segments[i].slot == slot && sig_matches(&psx_segments[i])) return psx_segments[i].name;
    return NULL;
}

const char *psx_segment_of(uint32_t addr) {         /* segment whose code is at addr now */
    addr = (addr & 0x1FFFFFFFu) | 0x80000000u;
    for (int i = 0; i < psx_segment_count; i++) {
        const SegmentInfo *s = &psx_segments[i];
        if (addr >= s->base && addr < s->base + s->size && (s->slot == 0 || sig_matches(s)))
            return s->name;
    }
    return NULL;
}

void call_dynamic(CPU *c, uint32_t addr) {
    recent[recent_n % NRECENT].addr = addr;
    recent[recent_n % NRECENT].ra = c->r[31];
    recent_n++;
    RecompFunc f = lookup(addr);
    if (!f) psx_fatal("call_dynamic: no function at %08X (ra=%08X)", addr, c->r[31]);
    f(c);
}

void jump_dynamic(CPU *c, uint32_t addr) {
    /* A tail jump behaves like a call whose callee returns to our caller. */
    call_dynamic(c, addr);
}
