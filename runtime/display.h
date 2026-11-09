#pragma once

void display_init(void);
void display_vblank(void);       /* present a frame, pump window messages, pace to 60 Hz */
void display_dump_final(void);   /* write the last frame + VRAM if PSX_DUMP_DIR is set */
int display_has_focus(void);
#include <stdint.h>
uint64_t display_frame_count(void);
int display_paused(void);                                 /* window being dragged/resized */
int display_headless(void);
const uint32_t *display_last_frame(int *w, int *h);      /* last presented frame, XRGB */
void display_save_vram(const char *path);
