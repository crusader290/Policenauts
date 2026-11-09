/* Placeholders for subsystems not written yet. Each moves to its own file when built. */
#include <stdio.h>
#include <stdlib.h>
#include "psx.h"

uint8_t psx_ram[PSX_RAM_SIZE];
uint8_t psx_scratch[0x400];

static uint32_t gte_data[32], gte_ctrl[32];
uint32_t gte_read_data(CPU *c, int reg) { (void)c; return gte_data[reg]; }
uint32_t gte_read_ctrl(CPU *c, int reg) { (void)c; return gte_ctrl[reg]; }
void gte_write_data(CPU *c, int reg, uint32_t v) { (void)c; gte_data[reg] = v; }
void gte_write_ctrl(CPU *c, int reg, uint32_t v) { (void)c; gte_ctrl[reg] = v; }
void gte_command(CPU *c, uint32_t cmd) {
    (void)c;
    fprintf(stderr, "fatal: gte_command %07X unimplemented\n", cmd);
    exit(1);
}
