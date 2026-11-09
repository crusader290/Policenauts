/* CD-ROM controller (0x1F801800-0x1F801803) backed by a raw MODE2/2352 disc image.
 *
 * Commands produce responses through a small queue of timed interrupts. Like the real
 * controller, the next interrupt is only delivered once the CPU has acknowledged the
 * previous one. Reads deliver one sector per INT1 at 75 or 150 sectors/s of virtual
 * time; XA-ADPCM sectors are routed to the audio path instead of the CPU when the
 * mode asks for it.
 */
#include <stdio.h>
#include <stdlib.h>
#include "bios.h"
#include "cd.h"

#define CPU_HZ        33868800ull
#define SECTOR_RAW    2352
#define T_FIRST       25000ull        /* command -> first response (INT3) */
#define T_SECOND      70000ull        /* typical second response (INT2) */
#define T_SEEK        200000ull
#define T_READTOC     (CPU_HZ / 4)

/* stat bits */
#define ST_ERROR   0x01
#define ST_MOTOR   0x02
#define ST_SHELL   0x10
#define ST_READ    0x20
#define ST_SEEK    0x40
#define ST_PLAY    0x80

typedef struct {
    uint64_t time;
    uint8_t irq;
    uint8_t n;
    uint8_t data[16];
    uint8_t is_sector;
} Pending;

static struct {
    FILE *disc;
    uint32_t disc_sectors;
    int shell_open;

    int index;
    uint8_t ie, iflag, request;
    uint8_t param[16]; int nparam;
    uint8_t resp[16]; int nresp, presp;

    uint8_t mode, stat_extra;
    uint32_t setloc, pos;             /* LBAs */
    int setloc_pending;
    int reading;
    uint64_t next_sector;

    uint8_t sector[SECTOR_RAW];       /* last sector read (raw) */
    int sector_valid;
    uint8_t data[SECTOR_RAW]; int data_len, data_pos;
    uint8_t filter_file, filter_chan;
    uint8_t last_header[8];

    Pending q[16]; int nq;
    int trace;
} cd;

/* ---- helpers ----------------------------------------------------------------- */
static uint8_t bcd(uint32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static uint32_t unbcd(uint8_t b) { return (b >> 4) * 10u + (b & 0xF); }
static uint32_t msf_to_lba(uint8_t m, uint8_t s, uint8_t f) {
    return (unbcd(m) * 60 + unbcd(s)) * 75 + unbcd(f) - 150;
}
static void lba_to_msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f) {
    lba += 150;
    *m = bcd(lba / (60 * 75)); *s = bcd((lba / 75) % 60); *f = bcd(lba % 75);
}

static uint8_t stat_byte(void) {
    uint8_t s = cd.shell_open ? ST_SHELL : ST_MOTOR;
    if (cd.reading) s |= ST_READ;
    return s | cd.stat_extra;
}

static void push(uint64_t delay, uint8_t irq, const uint8_t *d, int n, int is_sector) {
    if (cd.nq == 16) { fprintf(stderr, "[cd] interrupt queue overflow\n"); return; }
    Pending *p = &cd.q[cd.nq++];
    uint64_t base = psx_cycles();
    /* keep queue order: a response never overtakes an earlier one */
    if (cd.nq > 1 && cd.q[cd.nq - 2].time > base) base = cd.q[cd.nq - 2].time;
    p->time = base + delay;
    p->irq = irq;
    p->n = (uint8_t)n;
    p->is_sector = (uint8_t)is_sector;
    memcpy(p->data, d, (size_t)n);
}
static void push_stat(uint64_t delay, uint8_t irq) {
    uint8_t s = stat_byte();
    push(delay, irq, &s, 1, 0);
}
static void push_error(uint8_t code) {
    uint8_t d[2] = { (uint8_t)(stat_byte() | ST_ERROR), code };
    push(T_FIRST, 5, d, 2, 0);
}
static void drop_sector_irqs(void) {
    int j = 0;
    for (int i = 0; i < cd.nq; i++) if (!cd.q[i].is_sector) cd.q[j++] = cd.q[i];
    cd.nq = j;
}

static int read_raw(uint32_t lba, uint8_t *buf) {
    if (!cd.disc || lba >= cd.disc_sectors) return 0;
    if (_fseeki64(cd.disc, (int64_t)lba * SECTOR_RAW, SEEK_SET) != 0) return 0;
    return fread(buf, 1, SECTOR_RAW, cd.disc) == SECTOR_RAW;
}

static uint64_t sector_period(void) { return CPU_HZ / ((cd.mode & 0x80) ? 150 : 75); }

/* ---- commands ------------------------------------------------------------------ */
static void start_read(void) {
    if (cd.setloc_pending) { cd.pos = cd.setloc; cd.setloc_pending = 0; }
    cd.reading = 1;
    drop_sector_irqs();
    cd.next_sector = psx_cycles() + T_SEEK / 2 + sector_period();
}

static void command(uint8_t cmd) {
    uint8_t *p = cd.param;
    int np = cd.nparam;
    cd.nparam = 0;
    if (cd.trace) {
        fprintf(stderr, "[cd] cmd %02X", cmd);
        for (int i = 0; i < np; i++) fprintf(stderr, " %02X", p[i]);
        fprintf(stderr, "\n");
    }
    switch (cmd) {
    case 0x01: push_stat(T_FIRST, 3); break;                       /* Getstat */
    case 0x02:                                                     /* Setloc */
        if (np < 3) { push_error(0x20); break; }
        cd.setloc = msf_to_lba(p[0], p[1], p[2]);
        cd.setloc_pending = 1;
        push_stat(T_FIRST, 3);
        break;
    case 0x03:                                                     /* Play (CD-DA) */
        push_stat(T_FIRST, 3);
        break;
    case 0x06: case 0x1B:                                          /* ReadN / ReadS */
        push_stat(T_FIRST, 3);
        start_read();
        break;
    case 0x07: push_stat(T_FIRST, 3); push_stat(T_SECOND, 2); break; /* MotorOn */
    case 0x08: case 0x09:                                          /* Stop / Pause */
        push_stat(T_FIRST, 3);
        cd.reading = 0;
        drop_sector_irqs();
        push_stat(T_SECOND, 2);
        break;
    case 0x0A:                                                     /* Init */
        cd.mode = 0; cd.reading = 0; drop_sector_irqs();
        push_stat(T_FIRST, 3); push_stat(T_SECOND, 2);
        break;
    case 0x0B: case 0x0C: push_stat(T_FIRST, 3); break;            /* Mute / Demute */
    case 0x0D:                                                     /* Setfilter */
        cd.filter_file = np > 0 ? p[0] : 0; cd.filter_chan = np > 1 ? p[1] : 0;
        push_stat(T_FIRST, 3);
        break;
    case 0x0E: cd.mode = np ? p[0] : 0; push_stat(T_FIRST, 3); break; /* Setmode */
    case 0x0F: {                                                   /* Getparam */
        uint8_t d[5] = { stat_byte(), cd.mode, 0, cd.filter_file, cd.filter_chan };
        push(T_FIRST, 3, d, 5, 0);
        break;
    }
    case 0x10:                                                     /* GetlocL */
        push(T_FIRST, 3, cd.last_header, 8, 0);
        break;
    case 0x11: {                                                   /* GetlocP */
        uint8_t d[8] = { 0x01, 0x01 };                             /* track 1, index 1 */
        uint32_t rel = cd.pos;                                     /* track 1 starts at LBA 0 */
        d[2] = bcd(rel / (60 * 75)); d[3] = bcd((rel / 75) % 60); d[4] = bcd(rel % 75);
        lba_to_msf(cd.pos, &d[5], &d[6], &d[7]);
        push(T_FIRST, 3, d, 8, 0);
        break;
    }
    case 0x13: { uint8_t d[3] = { stat_byte(), 0x01, 0x01 }; push(T_FIRST, 3, d, 3, 0); break; } /* GetTN */
    case 0x14: {                                                   /* GetTD */
        uint8_t d[3] = { stat_byte(), 0, 0 };
        if (np && unbcd(p[0]) == 0) {                              /* track 0 = disc end */
            uint8_t f;
            lba_to_msf(cd.disc_sectors, &d[1], &d[2], &f);
        } else {
            d[1] = 0x00; d[2] = 0x02;                              /* track 1 at 00:02 */
        }
        push(T_FIRST, 3, d, 3, 0);
        break;
    }
    case 0x15: case 0x16:                                          /* SeekL / SeekP */
        cd.reading = 0;
        drop_sector_irqs();
        if (cd.setloc_pending) { cd.pos = cd.setloc; cd.setloc_pending = 0; }
        push_stat(T_FIRST, 3);
        push_stat(T_SEEK, 2);
        break;
    case 0x19:                                                     /* Test */
        if (np && p[0] == 0x20) {
            static const uint8_t ver[4] = { 0x94, 0x09, 0x19, 0xC0 };
            push(T_FIRST, 3, ver, 4, 0);
        } else {
            push_error(0x10);
        }
        break;
    case 0x1A: {                                                   /* GetID */
        push_stat(T_FIRST, 3);
        if (!cd.disc) { uint8_t d[8] = { 0x08, 0x40 }; push(T_SECOND, 5, d, 8, 0); break; }
        uint8_t d[8] = { stat_byte(), 0x00, 0x20, 0x00, 'S', 'C', 'E', 'I' };   /* licensed, NTSC-J */
        push(T_SECOND, 2, d, 8, 0);
        break;
    }
    case 0x1E: push_stat(T_FIRST, 3); push_stat(T_READTOC, 2); break; /* ReadTOC */
    default:
        fprintf(stderr, "[cd] unsupported command %02X\n", cmd);
        push_error(0x40);
    }
}

/* ---- sector delivery ----------------------------------------------------------- */
static void deliver_sector(void) {
    uint32_t lba = cd.pos++;
    uint8_t raw[SECTOR_RAW];
    if (!read_raw(lba, raw)) {
        cd.reading = 0;
        uint8_t d[2] = { (uint8_t)(stat_byte() | ST_ERROR), 0x04 };
        push(0, 5, d, 2, 1);
        return;
    }
    memcpy(cd.last_header, raw + 12, 8);
    uint8_t file = raw[16], chan = raw[17], submode = raw[18];
    int is_audio = (submode & 0x04) && (submode & 0x20);         /* realtime form2 audio */
    if ((cd.mode & 0x40) && is_audio) {
        if (!(cd.mode & 0x08) || (file == cd.filter_file && chan == cd.filter_chan))
            cd_xa_sector(raw);
        return;                                                   /* never reaches the CPU */
    }
    if ((cd.mode & 0x08) && (submode & 0x04)) return;             /* filtered audio: skipped */
    memcpy(cd.sector, raw, SECTOR_RAW);
    cd.sector_valid = 1;
    uint8_t s = stat_byte();
    push(0, 1, &s, 1, 1);
}

void cd_update(void) {
    uint64_t now = psx_cycles();
    while (cd.reading && now >= cd.next_sector) {
        cd.next_sector += sector_period();
        /* the controller holds at most one undelivered sector: drop if the CPU is behind */
        int queued = 0;
        for (int i = 0; i < cd.nq; i++) queued += cd.q[i].is_sector;
        if (queued < 2) deliver_sector();
    }
    if (cd.iflag == 0 && cd.nq && now >= cd.q[0].time) {
        Pending p = cd.q[0];
        memmove(cd.q, cd.q + 1, sizeof(Pending) * (size_t)--cd.nq);
        memcpy(cd.resp, p.data, p.n);
        cd.nresp = p.n;
        cd.presp = 0;
        cd.iflag = p.irq;
        if (cd.trace) fprintf(stderr, "[cd] INT%d (%d bytes, stat %02X)\n", p.irq, p.n, p.data[0]);
        if (cd.iflag & cd.ie & 0x1F) psx_irq_raise(IRQ_CDROM);
    }
}

uint64_t cd_next_event(void) {
    uint64_t t = UINT64_MAX;
    if (cd.reading) t = cd.next_sector;
    if (cd.iflag == 0 && cd.nq && cd.q[0].time < t) t = cd.q[0].time;
    return t;
}

/* ---- registers ------------------------------------------------------------------ */
uint8_t cd_read(uint32_t reg) {
    switch (reg) {
    case 0: {
        uint8_t s = (uint8_t)cd.index;
        if (cd.nparam == 0) s |= 0x08;                 /* param FIFO empty */
        if (cd.nparam < 16) s |= 0x10;                 /* param FIFO not full */
        if (cd.presp < cd.nresp) s |= 0x20;            /* response FIFO not empty */
        if (cd.data_pos < cd.data_len) s |= 0x40;      /* data FIFO not empty */
        return s;
    }
    case 1: return cd.presp < cd.nresp ? cd.resp[cd.presp++] : 0;
    case 2: return cd.data_pos < cd.data_len ? cd.data[cd.data_pos++] : 0;
    case 3: return (cd.index & 1) ? (uint8_t)(0xE0 | cd.iflag) : (uint8_t)(0xE0 | cd.ie);
    }
    return 0;
}

static void request(uint8_t v) {
    if (cd.trace > 1) fprintf(stderr, "[cd] request %02X (valid=%d fifo %d/%d)\n", v, cd.sector_valid, cd.data_pos, cd.data_len);
    cd.request = v;
    if (v & 0x80) {
        if (cd.data_pos >= cd.data_len && cd.sector_valid) {
            if (cd.mode & 0x20) { memcpy(cd.data, cd.sector + 12, 0x924); cd.data_len = 0x924; }
            else { memcpy(cd.data, cd.sector + 24, 0x800); cd.data_len = 0x800; }
            cd.data_pos = 0;
        }
    } else {
        cd.data_len = cd.data_pos = 0;
    }
}

void cd_write(uint32_t reg, uint8_t v) {
    int idx = cd.index;
    if (reg == 0) { cd.index = v & 3; return; }
    switch (idx * 4 + (int)reg) {
    case 0 * 4 + 1: command(v); break;
    case 0 * 4 + 2: if (cd.nparam < 16) cd.param[cd.nparam++] = v; break;
    case 0 * 4 + 3: request(v); break;
    case 1 * 4 + 2: cd.ie = v & 0x1F; break;
    case 1 * 4 + 3:
        cd.iflag &= (uint8_t)~(v & 0x1F);
        if (v & 0x40) cd.nparam = 0;
        if (cd.iflag == 0 && cd.nq && cd.q[0].time < psx_cycles())
            cd.q[0].time = psx_cycles() + 1000;        /* next response after a short gap */
        break;
    default: break;                                    /* audio volume registers */
    }
}

/* DMA channel 3: data FIFO -> RAM */
void cd_dma_read(uint32_t addr, uint32_t words) {
    if (cd.trace > 1) fprintf(stderr, "[cd] dma %u words -> %08X (fifo %d/%d)\n", words, addr, cd.data_pos, cd.data_len);
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = 0;
        for (int b = 0; b < 4; b++)
            w |= (uint32_t)(cd.data_pos < cd.data_len ? cd.data[cd.data_pos++] : 0) << (8 * b);
        MEM_SW(0x80000000u | ((addr + 4 * i) & 0x1FFFFC), w);
    }
}

/* ---- disc ----------------------------------------------------------------------- */
int cd_insert(const char *path) {
    if (cd.disc) fclose(cd.disc);
    cd.disc = path ? fopen(path, "rb") : NULL;
    if (!cd.disc) { cd.disc_sectors = 0; return 0; }
    _fseeki64(cd.disc, 0, SEEK_END);
    cd.disc_sectors = (uint32_t)(_ftelli64(cd.disc) / SECTOR_RAW);
    fprintf(stderr, "[cd] inserted %s (%u sectors)\n", path, cd.disc_sectors);
    return 1;
}

void cd_init(void) {
    const char *t = getenv("PSX_TRACE_CD");
    cd.trace = t ? atoi(t) ? atoi(t) : 1 : 0;
}
