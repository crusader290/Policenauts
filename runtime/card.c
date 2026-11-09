/* Memory card and BIOS file I/O. For now: no card inserted in either slot.
 * Card results are reported the way the BIOS does it, by delivering SwCARD/HwCARD
 * events; "no card" is a timeout. */
#include <stdio.h>
#include "bios.h"

#define HwCARD   0xF0000011u
#define SwCARD   0xF4000001u
#define EvSpIOE  0x0004u
#define EvSpERROR 0x8000u
#define EvSpTIMOUT 0x0100u
#define EvSpNEW  0x2000u

static void no_card(CPU *c) {
    bios_deliver_event(c, SwCARD, EvSpTIMOUT);
    bios_deliver_event(c, HwCARD, EvSpTIMOUT);
}

void psx_card_info(CPU *c, uint32_t port) { (void)port; no_card(c); }
void psx_card_load(CPU *c, uint32_t port) { (void)port; no_card(c); }
uint32_t psx_card_format(CPU *c, uint32_t name) { (void)c; (void)name; return 0; }
uint32_t psx_card_read(CPU *c, uint32_t port, uint32_t sector, uint32_t buf) {
    (void)port; (void)sector; (void)buf; no_card(c); return 1;
}
uint32_t psx_card_write(CPU *c, uint32_t port, uint32_t sector, uint32_t buf) {
    (void)port; (void)sector; (void)buf; no_card(c); return 1;
}

static void log_name(const char *op, uint32_t name) {
    char s[64];
    int i = 0;
    for (; i < 63; i++) { s[i] = (char)MEM_LB(name + i); if (!s[i]) break; }
    s[i] = 0;
    fprintf(stderr, "[card] %s(\"%s\") -> no card\n", op, s);
}

uint32_t psx_file_open(CPU *c, uint32_t name, uint32_t mode) {
    (void)c; (void)mode; log_name("open", name); return 0xFFFFFFFFu;
}
uint32_t psx_file_lseek(CPU *c, uint32_t fd, uint32_t off, uint32_t whence) {
    (void)c; (void)fd; (void)off; (void)whence; return 0xFFFFFFFFu;
}
uint32_t psx_file_read(CPU *c, uint32_t fd, uint32_t buf, uint32_t len) {
    (void)c; (void)fd; (void)buf; (void)len; return 0xFFFFFFFFu;
}
uint32_t psx_file_write(CPU *c, uint32_t fd, uint32_t buf, uint32_t len) {
    (void)c; (void)fd; (void)buf; (void)len; return 0xFFFFFFFFu;
}
uint32_t psx_file_close(CPU *c, uint32_t fd) { (void)c; (void)fd; return 0xFFFFFFFFu; }
uint32_t psx_file_firstfile(CPU *c, uint32_t name, uint32_t dirent) {
    (void)c; (void)dirent; log_name("firstfile", name); return 0;
}
uint32_t psx_file_nextfile(CPU *c, uint32_t dirent) { (void)c; (void)dirent; return 0; }
uint32_t psx_file_erase(CPU *c, uint32_t name) { (void)c; log_name("erase", name); return 0; }
