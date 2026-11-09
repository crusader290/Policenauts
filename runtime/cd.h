#pragma once
#include <stdint.h>

void cd_init(void);
int cd_insert(const char *path);          /* raw MODE2/2352 image; NULL = empty drive */
uint8_t cd_read(uint32_t reg);
void cd_write(uint32_t reg, uint8_t v);
void cd_update(void);                     /* run due events (called from the clock) */
uint64_t cd_next_event(void);
void cd_dma_read(uint32_t addr, uint32_t words);
void cd_xa_sector(const uint8_t *raw);    /* XA-ADPCM sector for the audio path (spu.c) */
void cd_audio_matrix(int m[4], int *mute); /* LL, LR, RR, RL volumes (0x80 = 1.0) */
