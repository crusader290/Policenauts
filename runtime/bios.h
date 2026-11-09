/* Runtime-internal interfaces between BIOS HLE and the hardware layer. */
#pragma once
#include "psx.h"

/* bios.c */
void bios_boot(uint32_t pc, uint32_t gp, uint32_t sp);
void bios_deliver_event(CPU *c, uint32_t cls, uint32_t spec);
void bios_raise_exception(CPU *c);
void bios_vblank_pads(void);
int psx_irq_enabled(void);
CPU *bios_current_cpu(void);
void bios_describe(void);              /* kernel threads, to the log */
int hw_hot_funcs(uint32_t *func, uint32_t *count, int max);   /* hw.c: busiest loops */

/* hw.c - interrupts and time */
enum { IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3, IRQ_TMR0 = 4, IRQ_TMR1 = 5,
       IRQ_TMR2 = 6, IRQ_SIO = 7, IRQ_SPU = 9 };
void psx_irq_raise(int line);
uint32_t psx_irq_pending(void);        /* I_STAT & I_MASK */
void psx_irq_ack(int line);
void psx_poll(CPU *c);   /* deliver pending interrupts if enabled */
void psx_idle(CPU *c);   /* nothing to do until the next event: advance time to it */
uint64_t psx_cycles(void);

/* input */
uint16_t psx_pad_buttons(void);   /* active-low PSX button mask */

/* card.c - memory card + BIOS file I/O ("bu00:") */
void psx_card_info(CPU *c, uint32_t port);
void psx_card_load(CPU *c, uint32_t port);
uint32_t psx_card_format(CPU *c, uint32_t name);
uint32_t psx_card_read(CPU *c, uint32_t port, uint32_t sector, uint32_t buf);
uint32_t psx_card_write(CPU *c, uint32_t port, uint32_t sector, uint32_t buf);
uint32_t psx_file_open(CPU *c, uint32_t name, uint32_t mode);
uint32_t psx_file_lseek(CPU *c, uint32_t fd, uint32_t off, uint32_t whence);
uint32_t psx_file_read(CPU *c, uint32_t fd, uint32_t buf, uint32_t len);
uint32_t psx_file_write(CPU *c, uint32_t fd, uint32_t buf, uint32_t len);
uint32_t psx_file_close(CPU *c, uint32_t fd);
uint32_t psx_file_firstfile(CPU *c, uint32_t name, uint32_t dirent);
uint32_t psx_file_nextfile(CPU *c, uint32_t dirent);
uint32_t psx_file_erase(CPU *c, uint32_t name);
