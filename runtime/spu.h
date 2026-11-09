/* SPU: 24 ADPCM voices, reverb, CD (XA-ADPCM) input, IRQ9. 0x1F801C00-0x1F801FFF. */
#pragma once
#include <stdint.h>

void spu_init(void);
void spu_run(void);                              /* produce samples up to the current time */
uint64_t spu_next_event(void);                   /* for idle skipping; UINT64_MAX if none */
uint16_t spu_read16(uint32_t addr);
void spu_write16(uint32_t addr, uint16_t v);
void spu_dma_write(uint32_t madr, uint32_t words);   /* DMA4: RAM -> SPU */
void spu_dma_read(uint32_t madr, uint32_t words);    /* DMA4: SPU -> RAM */
