/* Native hooks attached to recompiled functions (see recomp/hooks.toml). */
#include <stdio.h>
#include <stdlib.h>
#include "psx.h"

static int klog = -1;

/* kernel debug log line (a0 = NUL-terminated text, usually ending in '\n') */
void hook_kernel_log(CPU *c) {
    if (klog < 0) klog = getenv("PSX_QUIET") == NULL;
    if (!klog) return;
    uint32_t p = c->r[4];
    fputs("[K] ", stderr);
    int ch, last = 0;
    for (int i = 0; i < 512 && (ch = MEM_LB(p + (uint32_t)i)) != 0; i++) { fputc(ch, stderr); last = ch; }
    if (last != '\n') fputc('\n', stderr);
}
