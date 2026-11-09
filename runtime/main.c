/* Boot: load the PS-X EXE image into RAM and start the BIOS on the recompiled kernel. */
#include <stdio.h>
#include <stdlib.h>
#include "bios.h"
#include "cd.h"
#include "log.h"

void hw_init(void);

static int load_exe(const char *path, uint32_t *pc, uint32_t *gp, uint32_t *sp) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    uint8_t hdr[0x800];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, "PS-X EXE", 8) != 0) {
        fprintf(stderr, "%s: not a PS-X EXE\n", path);
        fclose(f);
        return 0;
    }
    uint32_t t_addr, t_size;
    memcpy(pc, hdr + 0x10, 4);
    memcpy(gp, hdr + 0x14, 4);
    memcpy(&t_addr, hdr + 0x18, 4);
    memcpy(&t_size, hdr + 0x1C, 4);
    memcpy(sp, hdr + 0x30, 4);
    size_t n = fread(&psx_ram[t_addr & (PSX_RAM_SIZE - 1)], 1, t_size, f);
    fclose(f);
    fprintf(stderr, "loaded %s: %zu bytes at %08X, pc=%08X sp=%08X\n", path, n, t_addr, *pc, *sp);
    return n == t_size;
}

static char disc_paths[2][1024];

/* build/discs.txt (written by configure.py): one disc image path per line */
static void load_disc_list(void) {
    FILE *f = fopen("build/discs.txt", "r");
    if (!f) return;
    for (int i = 0; i < 2 && fgets(disc_paths[i], sizeof disc_paths[i], f); i++)
        disc_paths[i][strcspn(disc_paths[i], "\r\n")] = 0;
    fclose(f);
}

const char *psx_disc_path(int n) { return (n >= 0 && n < 2 && disc_paths[n][0]) ? disc_paths[n] : NULL; }

int main(int argc, char **argv) {
    log_init(argc, argv);                                 /* Policenauts.log */
    const char *exe = "extracted/disc1/NAUTS/SLPS_002.15";
    load_disc_list();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--disc1") && i + 1 < argc) snprintf(disc_paths[0], sizeof disc_paths[0], "%s", argv[++i]);
        else if (!strcmp(argv[i], "--disc2") && i + 1 < argc) snprintf(disc_paths[1], sizeof disc_paths[1], "%s", argv[++i]);
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else { fprintf(stderr, "usage: %s [--disc1 bin] [--disc2 bin] [--exe SLPS]\n", argv[0]); return 2; }
    }
    uint32_t pc, gp, sp;
    if (!load_exe(exe, &pc, &gp, &sp)) return 1;
    hw_init();
    watchdog_init();
    for (int i = 0; i < 2; i++)
        fprintf(stderr, "disc %d: %s\n", i + 1, psx_disc_path(i) ? psx_disc_path(i) : "(none)");
    const char *boot = getenv("PSX_BOOT_DISC");          /* start with disc 2 in the drive */
    int disc = boot && atoi(boot) == 2 ? 2 : 1;
    if (!cd_insert_disc(disc)) fprintf(stderr, "warning: no disc %d image (run configure.py or pass --disc%d)\n", disc, disc);
    bios_boot(pc, gp, sp ? sp : 0x801FFF00u);   /* SYSTEM.CNF STACK = 801fff00 */
    return 0;
}
