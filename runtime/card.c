/* Memory cards and the BIOS "bu" file system, backed by host card images.
 *
 * Each slot is a raw 128 KB image (the common .mcd / .mcr format: 1024 frames of 128
 * bytes, frame 0 the "MC" header, frames 1-15 the directory, blocks 1-15 the data).
 *
 *   PSX_CARD1=path   slot 1 image (default saves/card1.mcd; "none" = no card)
 *   PSX_CARD2=path   slot 2 image (default: no card)
 *
 * A missing image file is a freshly formatted card; the file is written on the first
 * change. Results are reported the way the BIOS does it: SwCARD/HwCARD events
 * (EvSpIOE when done, EvSpTIMOUT when no card is inserted), delivered at once.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include "bios.h"

#define HwCARD     0xF0000011u
#define SwCARD     0xF4000001u
#define EvSpIOE    0x0004u
#define EvSpTIMOUT 0x0100u

#define FRAME   128
#define FRAMES  1024
#define CARD_SIZE (FRAME * FRAMES)
#define BLOCK   (64 * FRAME)

/* directory frame */
#define D_STATE 0x00
#define D_SIZE  0x04
#define D_NEXT  0x08
#define D_NAME  0x0A
#define ST_FREE  0xA0u
#define ST_FIRST 0x51u
#define ST_MID   0x52u
#define ST_LAST  0x53u

/* open() mode bits */
#define FREAD  0x0001u
#define FWRITE 0x0002u
#define FCREAT 0x0200u
#define FASYNC 0x8000u

typedef struct {
    int present, loaded, dirty;
    char path[260];
    uint8_t data[CARD_SIZE];
} Card;

static Card cards[2];
static int trace_card = -1;

#define TRACE(...) do { if (trace_card) fprintf(stderr, "[card] " __VA_ARGS__); } while (0)

static uint8_t *frame(Card *k, int n) { return k->data + n * FRAME; }

static void set_checksum(uint8_t *f) {
    uint8_t x = 0;
    for (int i = 0; i < FRAME - 1; i++) x ^= f[i];
    f[FRAME - 1] = x;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void format(Card *k) {
    memset(k->data, 0, CARD_SIZE);
    uint8_t *h = frame(k, 0);
    h[0] = 'M'; h[1] = 'C';
    set_checksum(h);
    for (int i = 1; i < 16; i++) {
        uint8_t *d = frame(k, i);
        wr32(d + D_STATE, ST_FREE);
        wr16(d + D_NEXT, 0xFFFF);
        set_checksum(d);
    }
    for (int i = 16; i < 36; i++) {             /* broken-sector list: none */
        uint8_t *d = frame(k, i);
        wr32(d, 0xFFFFFFFFu);
        wr16(d + 8, 0xFFFF);
        set_checksum(d);
    }
    memcpy(frame(k, 63), h, FRAME);              /* write-test frame */
    k->dirty = 1;
}

static void flush(Card *k) {
    if (!k->dirty) return;
    char dir[260];
    strcpy(dir, k->path);
    char *s = strrchr(dir, '/'), *b = strrchr(dir, '\\');
    if (b > s) s = b;
    if (s) { *s = 0; _mkdir(dir); }
    FILE *f = fopen(k->path, "wb");
    if (!f) { fprintf(stderr, "[card] cannot write %s\n", k->path); return; }
    fwrite(k->data, 1, CARD_SIZE, f);
    fclose(f);
    k->dirty = 0;
    TRACE("saved %s\n", k->path);
}

static void flush_all(void) { flush(&cards[0]); flush(&cards[1]); }

static Card *card(uint32_t port) {
    if (trace_card < 0) trace_card = getenv("PSX_TRACE_CARD") != NULL;
    if (port & 0x0F) return NULL;               /* multitap sub-slots: nothing there */
    int n = (port >> 4) & 1;
    Card *k = &cards[n];
    if (!k->loaded) {
        k->loaded = 1;
        const char *p = getenv(n ? "PSX_CARD2" : "PSX_CARD1");
        if (!p) p = n ? "none" : "saves/card1.mcd";
        if (strcmp(p, "none") != 0) {
            k->present = 1;
            snprintf(k->path, sizeof k->path, "%s", p);
            FILE *f = fopen(p, "rb");
            if (f && fread(k->data, 1, CARD_SIZE, f) == CARD_SIZE && k->data[0] == 'M' && k->data[1] == 'C') {
                TRACE("slot %d: %s\n", n + 1, p);
            } else {
                TRACE("slot %d: %s (new, formatted)\n", n + 1, p);
                format(k);
                k->dirty = 0;                    /* written on the first real change */
            }
            if (f) fclose(f);
            static int hooked;
            if (!hooked) { hooked = 1; atexit(flush_all); }
        }
    }
    return k->present ? k : NULL;
}

static void report(CPU *c, Card *k) {
    uint32_t spec = k ? EvSpIOE : EvSpTIMOUT;
    bios_deliver_event(c, SwCARD, spec);
    bios_deliver_event(c, HwCARD, spec);
}

/* ---- low level (sector) access --------------------------------------------------- */

void psx_card_info(CPU *c, uint32_t port) { report(c, card(port)); }
void psx_card_load(CPU *c, uint32_t port) { report(c, card(port)); }

uint32_t psx_card_read(CPU *c, uint32_t port, uint32_t sector, uint32_t buf) {
    Card *k = card(port);
    if (k && sector < FRAMES)
        for (int i = 0; i < FRAME; i++) MEM_SB(buf + i, frame(k, (int)sector)[i]);
    report(c, k);
    return 1;
}

uint32_t psx_card_write(CPU *c, uint32_t port, uint32_t sector, uint32_t buf) {
    Card *k = card(port);
    if (k && sector < FRAMES) {
        for (int i = 0; i < FRAME; i++) frame(k, (int)sector)[i] = MEM_LB(buf + i);
        k->dirty = 1;
        flush(k);
    }
    report(c, k);
    return 1;
}

/* ---- file system ----------------------------------------------------------------- */

static void get_str(uint32_t a, char *s, int max) {
    int i = 0;
    for (; i < max - 1; i++) { s[i] = (char)MEM_LB(a + i); if (!s[i]) break; }
    s[i] = 0;
}

/* "bu00:NAME" -> port (0x00/0x10) and the file name part; -1 if not a card device */
static int parse(uint32_t name, char *file, int max) {
    char s[64];
    get_str(name, s, sizeof s);
    if ((s[0] | 0x20) != 'b' || (s[1] | 0x20) != 'u' || s[2] < '0' || s[2] > '1') return -1;
    if (s[3] != '0') return -1;                   /* multitap sub-slot */
    const char *colon = strchr(s, ':');
    if (!colon) return -1;
    snprintf(file, (size_t)max, "%s", colon + 1);
    return (s[2] - '0') << 4;
}

static int match(const char *pat, const char *s) {
    for (; *pat; pat++, s++) {
        if (*pat == '*') return 1;
        if (!*s) return 0;
        if (*pat != '?' && *pat != *s) return 0;
    }
    return *s == 0;
}

static int find(Card *k, const char *name) {
    for (int i = 1; i < 16; i++) {
        uint8_t *d = frame(k, i);
        if (rd32(d + D_STATE) == ST_FIRST && !strncmp((char *)d + D_NAME, name, 20)) return i;
    }
    return 0;
}

static int nblocks(Card *k, int first) {
    int n = 0;
    for (int b = first; b >= 1 && b < 16 && n < 15; n++) {
        uint16_t next = rd16(frame(k, b) + D_NEXT);
        b = next == 0xFFFF ? 0 : next + 1;
    }
    return n;
}

/* byte offset within the file -> card offset */
static int file_off(Card *k, int first, uint32_t pos) {
    int b = first;
    for (uint32_t i = 0; i < pos / BLOCK; i++) {
        uint16_t next = rd16(frame(k, b) + D_NEXT);
        if (next == 0xFFFF) return -1;
        b = next + 1;
    }
    return b * BLOCK + (int)(pos % BLOCK);
}

static void erase_chain(Card *k, int first) {
    for (int b = first, n = 0; b >= 1 && b < 16 && n < 15; n++) {
        uint8_t *d = frame(k, b);
        uint16_t next = rd16(d + D_NEXT);
        memset(d, 0, FRAME);
        wr32(d + D_STATE, ST_FREE);
        wr16(d + D_NEXT, 0xFFFF);
        set_checksum(d);
        b = next == 0xFFFF ? 0 : next + 1;
    }
    k->dirty = 1;
}

static int create(Card *k, const char *name, int blocks) {
    int free_[15], nf = 0;
    for (int i = 1; i < 16; i++)
        if ((rd32(frame(k, i) + D_STATE) & 0xF0) == 0xA0) free_[nf++] = i;
    if (blocks < 1) blocks = 1;
    if (nf < blocks) return 0;
    for (int j = 0; j < blocks; j++) {
        uint8_t *d = frame(k, free_[j]);
        memset(d, 0, FRAME);
        wr32(d + D_STATE, j == 0 ? ST_FIRST : j == blocks - 1 ? ST_LAST : ST_MID);
        if (j == 0) {
            wr32(d + D_SIZE, (uint32_t)blocks * BLOCK);
            memcpy(d + D_NAME, name, strnlen(name, 20));
        }
        wr16(d + D_NEXT, j == blocks - 1 ? 0xFFFF : (uint16_t)(free_[j + 1] - 1));
        set_checksum(d);
        memset(k->data + free_[j] * BLOCK, 0, BLOCK);
    }
    k->dirty = 1;
    return free_[0];
}

#define MAX_FD 8
static struct { int used, port, first, async; uint32_t pos; } fds[MAX_FD];
#define FD_BASE 2

uint32_t psx_file_open(CPU *c, uint32_t name, uint32_t mode) {
    (void)c;
    char file[32];
    int port = parse(name, file, sizeof file);
    Card *k = port < 0 ? NULL : card((uint32_t)port);
    if (!k) { TRACE("open(\"%s\") -> no card\n", file); return 0xFFFFFFFFu; }
    int first = find(k, file);
    if (mode & FCREAT) {
        if (first) { TRACE("open(\"%s\", create) -> exists\n", file); return 0xFFFFFFFFu; }
        first = create(k, file, (int)(mode >> 16));
        flush(k);
    }
    if (!first) { TRACE("open(\"%s\", %X) -> not found\n", file, mode); return 0xFFFFFFFFu; }
    for (int i = 0; i < MAX_FD; i++)
        if (!fds[i].used) {
            fds[i].used = 1; fds[i].port = port; fds[i].first = first; fds[i].pos = 0;
            fds[i].async = (mode & FASYNC) != 0;
            TRACE("open(\"%s\", %X) -> fd %d (block %d)\n", file, mode, i + FD_BASE, first);
            return (uint32_t)(i + FD_BASE);
        }
    return 0xFFFFFFFFu;
}

static int fd_index(uint32_t fd) {
    int i = (int)fd - FD_BASE;
    return i >= 0 && i < MAX_FD && fds[i].used ? i : -1;
}

uint32_t psx_file_lseek(CPU *c, uint32_t fd, uint32_t off, uint32_t whence) {
    (void)c;
    int i = fd_index(fd);
    if (i < 0) return 0xFFFFFFFFu;
    if (whence == 0) fds[i].pos = off;
    else if (whence == 1) fds[i].pos += off;
    else return 0xFFFFFFFFu;
    return fds[i].pos;
}

static uint32_t transfer(CPU *c, uint32_t fd, uint32_t buf, uint32_t len, int write) {
    int i = fd_index(fd);
    if (i < 0) return 0xFFFFFFFFu;
    Card *k = card((uint32_t)fds[i].port);
    if (!k) return 0xFFFFFFFFu;
    uint32_t size = (uint32_t)nblocks(k, fds[i].first) * BLOCK, done = 0;
    while (done < len && fds[i].pos < size) {
        int o = file_off(k, fds[i].first, fds[i].pos);
        if (o < 0) break;
        if (write) k->data[o] = MEM_LB(buf + done);
        else MEM_SB(buf + done, k->data[o]);
        done++; fds[i].pos++;
    }
    TRACE("%s(fd %u, %u) -> %u\n", write ? "write" : "read", fd, len, done);
    if (write && done) { k->dirty = 1; flush(k); }
    if (fds[i].async) {
        /* The BIOS signals each 128-byte frame on HwCARD (the kernel counts these down
         * to know when an async read has finished), then the whole transfer on SwCARD. */
        for (uint32_t f = 0; f < (done + FRAME - 1) / FRAME; f++)
            bios_deliver_event(c, HwCARD, EvSpIOE);
        bios_deliver_event(c, SwCARD, EvSpIOE);
    }
    return done;
}

uint32_t psx_file_read(CPU *c, uint32_t fd, uint32_t buf, uint32_t len) { return transfer(c, fd, buf, len, 0); }
uint32_t psx_file_write(CPU *c, uint32_t fd, uint32_t buf, uint32_t len) { return transfer(c, fd, buf, len, 1); }

uint32_t psx_file_close(CPU *c, uint32_t fd) {
    (void)c;
    int i = fd_index(fd);
    if (i < 0) return 0xFFFFFFFFu;
    fds[i].used = 0;
    return fd;
}

/* firstfile/nextfile: the BIOS DIRENTRY is name[20], attr, size, next, head, system[4] */
static struct { int port, slot; char pat[32]; } search;

static uint32_t next_match(uint32_t dirent) {
    Card *k = card((uint32_t)search.port);
    if (!k) return 0;
    for (int i = search.slot; i < 16; i++) {
        uint8_t *d = frame(k, i);
        const char *n = (const char *)d + D_NAME;
        if (rd32(d + D_STATE) != ST_FIRST || !match(search.pat, n)) continue;
        search.slot = i + 1;
        /* a 20-character name fills the field; attr (0 here) terminates it */
        for (int j = 0; j < 20; j++) MEM_SB(dirent + j, (uint8_t)n[j]);
        MEM_SW(dirent + 20, 0);
        MEM_SW(dirent + 24, (uint32_t)nblocks(k, i) * BLOCK);
        MEM_SW(dirent + 28, 0);
        MEM_SW(dirent + 32, (uint32_t)i);
        TRACE("found \"%.20s\" (%d blocks)\n", n, nblocks(k, i));
        return dirent;
    }
    search.slot = 16;
    return 0;
}

uint32_t psx_file_firstfile(CPU *c, uint32_t name, uint32_t dirent) {
    (void)c;
    int port = parse(name, search.pat, sizeof search.pat);
    TRACE("firstfile(\"%s\")\n", search.pat);
    if (port < 0) return 0;
    if (!search.pat[0]) strcpy(search.pat, "*");
    search.port = port;
    search.slot = 1;
    return next_match(dirent);
}

uint32_t psx_file_nextfile(CPU *c, uint32_t dirent) { (void)c; return next_match(dirent); }

uint32_t psx_file_erase(CPU *c, uint32_t name) {
    (void)c;
    char file[32];
    int port = parse(name, file, sizeof file);
    Card *k = port < 0 ? NULL : card((uint32_t)port);
    int first = k ? find(k, file) : 0;
    TRACE("erase(\"%s\") -> %s\n", file, first ? "ok" : "not found");
    if (!first) return 0;
    erase_chain(k, first);
    flush(k);
    return 1;
}

uint32_t psx_card_format(CPU *c, uint32_t name) {
    (void)c;
    char file[32];
    int port = parse(name, file, sizeof file);
    Card *k = port < 0 ? NULL : card((uint32_t)port);
    if (!k) return 0;
    format(k);
    flush(k);
    TRACE("format slot %d\n", (port >> 4) + 1);
    return 1;
}
