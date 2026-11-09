#pragma once
#include <stdint.h>

void gpu_init(void);
void gpu_gp0(uint32_t w);
void gpu_gp1(uint32_t w);
uint32_t gpu_read(void);          /* GPUREAD */
uint32_t gpu_stat(int odd_line);  /* GPUSTAT */

/* Display output: fills a 32-bit BGRX buffer (max 640x480) with the visible area.
 * Returns 0 when the display is disabled (buffer cleared to black). */
int gpu_output(uint32_t *dst, int *w, int *h);

void gpu_dump_vram_bmp(const char *path);
const uint16_t *gpu_vram(void);   /* 1024x512 16-bit */
