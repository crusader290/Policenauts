/* Runtime interface for recompiled Policenauts code. */
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct CPU {
    uint32_t r[32];
    uint32_t hi, lo;
} CPU;

typedef void (*RecompFunc)(CPU *c);

typedef struct { uint32_t addr; RecompFunc fn; } FuncEntry;
typedef struct { uint32_t addr; uint32_t word; } SigWord;
typedef struct {
    const char *name;
    uint32_t base, size;
    char slot;                 /* 0 = resident, otherwise overlay slot id */
    const FuncEntry *funcs; int nfuncs;
    const SigWord *sig; int nsig;
} SegmentInfo;

extern const SegmentInfo psx_segments[];
extern const int psx_segment_count;

/* ---- memory ---------------------------------------------------------------- */
#define PSX_RAM_SIZE   0x200000u
#define PSX_SCRATCH    0x1F800000u
extern uint8_t psx_ram[PSX_RAM_SIZE];
extern uint8_t psx_scratch[0x400];

uint32_t hw_read(uint32_t addr, int size);
void hw_write(uint32_t addr, uint32_t val, int size);

static inline uint8_t *mem_ptr(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (p < 0x800000u) return &psx_ram[p & (PSX_RAM_SIZE - 1)];   /* 2MB mirrored x4 */
    if ((p & 0xFFFFFC00u) == PSX_SCRATCH) return &psx_scratch[p & 0x3FF];
    return 0;
}

static inline uint32_t MEM_LW(uint32_t a) {
    uint8_t *p = mem_ptr(a); uint32_t v;
    if (p) { memcpy(&v, p, 4); return v; }
    return hw_read(a, 4);
}
static inline uint16_t MEM_LH(uint32_t a) {
    uint8_t *p = mem_ptr(a); uint16_t v;
    if (p) { memcpy(&v, p, 2); return v; }
    return (uint16_t)hw_read(a, 2);
}
static inline uint8_t MEM_LB(uint32_t a) {
    uint8_t *p = mem_ptr(a);
    return p ? *p : (uint8_t)hw_read(a, 1);
}
static inline void MEM_SW(uint32_t a, uint32_t v) {
    uint8_t *p = mem_ptr(a);
    if (p) memcpy(p, &v, 4); else hw_write(a, v, 4);
}
static inline void MEM_SH(uint32_t a, uint16_t v) {
    uint8_t *p = mem_ptr(a);
    if (p) memcpy(p, &v, 2); else hw_write(a, v, 2);
}
static inline void MEM_SB(uint32_t a, uint8_t v) {
    uint8_t *p = mem_ptr(a);
    if (p) *p = v; else hw_write(a, v, 1);
}

/* Unaligned access, little-endian R3000A semantics. */
static inline uint32_t MEM_LWL(uint32_t a, uint32_t old) {
    uint32_t w = MEM_LW(a & ~3u), sh = (a & 3) * 8;
    return (old & (0x00FFFFFFu >> sh)) | (w << (24 - sh));
}
static inline uint32_t MEM_LWR(uint32_t a, uint32_t old) {
    uint32_t w = MEM_LW(a & ~3u), sh = (a & 3) * 8;
    return (old & ~(0xFFFFFFFFu >> sh)) | (w >> sh);
}
static inline void MEM_SWL(uint32_t a, uint32_t v) {
    uint32_t w = MEM_LW(a & ~3u), sh = (a & 3) * 8;
    MEM_SW(a & ~3u, (w & ~(0xFFFFFFFFu >> (24 - sh))) | (v >> (24 - sh)));
}
static inline void MEM_SWR(uint32_t a, uint32_t v) {
    uint32_t w = MEM_LW(a & ~3u), sh = (a & 3) * 8;
    MEM_SW(a & ~3u, (w & ~(0xFFFFFFFFu << sh)) | (v << sh));
}

/* ---- arithmetic -------------------------------------------------------------- */
static inline void MULT(CPU *c, uint32_t a, uint32_t b) {
    int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    c->lo = (uint32_t)p; c->hi = (uint32_t)((uint64_t)p >> 32);
}
static inline void MULTU(CPU *c, uint32_t a, uint32_t b) {
    uint64_t p = (uint64_t)a * b;
    c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32);
}
static inline void DIV(CPU *c, uint32_t a, uint32_t b) {
    int32_t n = (int32_t)a, d = (int32_t)b;
    if (d == 0) { c->hi = a; c->lo = n >= 0 ? 0xFFFFFFFFu : 1u; }
    else if (a == 0x80000000u && d == -1) { c->hi = 0; c->lo = a; }
    else { c->lo = (uint32_t)(n / d); c->hi = (uint32_t)(n % d); }
}
static inline void DIVU(CPU *c, uint32_t a, uint32_t b) {
    if (b == 0) { c->hi = a; c->lo = 0xFFFFFFFFu; }
    else { c->lo = a / b; c->hi = a % b; }
}

/* ---- time ------------------------------------------------------------------------ */
/* Emitted on every backward branch so polling loops let virtual time (and interrupts)
 * advance even when they only read RAM. */
extern uint32_t psx_cur_func;   /* last recompiled function entered (diagnostics) */
int psx_recent_calls(uint32_t *addr, uint32_t *ra, int max);   /* dispatch.c: newest first */
const char *psx_overlay_in_slot(char slot);                    /* 'A'/'B' -> name or NULL */
const char *psx_segment_of(uint32_t addr);
#define BACKEDGE_BATCH 256
extern int32_t psx_backedge_budget;
void psx_backedge_slow(CPU *c);
static inline void BACKEDGE(CPU *c) {
    if (--psx_backedge_budget <= 0) psx_backedge_slow(c);
}

/* ---- control flow / system ---------------------------------------------------- */
void call_dynamic(CPU *c, uint32_t addr);   /* jalr, or calls into another overlay slot */
void jump_dynamic(CPU *c, uint32_t addr);   /* unresolved jr / tail jump */
void **psx_jmpbuf(uint32_t mips_buf);       /* host jmp_buf paired with a game setjmp buffer */
void bios_call(CPU *c, uint32_t vector);    /* A0/B0/C0 table call, function number in t1 */
void psx_syscall(CPU *c, uint32_t code, uint32_t pc);
void psx_break(CPU *c, uint32_t code, uint32_t pc);

uint32_t cop0_read(CPU *c, int reg);
void cop0_write(CPU *c, int reg, uint32_t v);
void cop0_rfe(CPU *c);

uint32_t gte_read_data(CPU *c, int reg);
uint32_t gte_read_ctrl(CPU *c, int reg);
void gte_write_data(CPU *c, int reg, uint32_t v);
void gte_write_ctrl(CPU *c, int reg, uint32_t v);
void gte_command(CPU *c, uint32_t cmd);
