#pragma once

void display_init(void);
void display_vblank(void);       /* present a frame, pump window messages, pace to 60 Hz */
void display_dump_final(void);   /* write the last frame + VRAM if PSX_DUMP_DIR is set */
int display_has_focus(void);
