/* MDEC: macroblock decoder (0x1F801820 command/data, 0x1F801824 status/control). */
#ifndef MDEC_H
#define MDEC_H
#include <stdint.h>
#include <stddef.h>

void mdec_init(void);
uint32_t mdec_read(int reg);                    /* reg 0: data out, 1: status */
void mdec_write(int reg, uint32_t v);           /* reg 0: command/param, 1: control */
void mdec_dma_in(uint32_t madr, uint32_t words);    /* DMA0: RAM -> MDEC */
size_t mdec_out_avail(void);                    /* words waiting in the output FIFO */
void mdec_dma_out(uint32_t madr, uint32_t words);   /* DMA1: MDEC -> RAM */

#endif
