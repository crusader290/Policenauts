/* Controller input: keyboard (when the window has focus) and scripted presses.
 *
 * Keyboard: arrows = D-pad, Enter = Start, Backspace = Select, Z = Cross, X = Circle,
 *           A = Square, S = Triangle, Q/W = L1/R1, 1/2 = L2/R2.
 *
 * PSX_INPUT scripts presses in virtual seconds, for headless runs:
 *   PSX_INPUT="start@95,cross@110-111.5"   (a bare time holds the button for 0.25 s)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bios.h"
#include "display.h"

enum {
    B_SELECT = 0, B_L3, B_R3, B_START, B_UP, B_RIGHT, B_DOWN, B_LEFT,
    B_L2, B_R2, B_L1, B_R1, B_TRIANGLE, B_CIRCLE, B_CROSS, B_SQUARE
};

static const struct { const char *name; int bit; } names[] = {
    { "select", B_SELECT }, { "start", B_START }, { "up", B_UP }, { "right", B_RIGHT },
    { "down", B_DOWN }, { "left", B_LEFT }, { "l2", B_L2 }, { "r2", B_R2 }, { "l1", B_L1 },
    { "r1", B_R1 }, { "triangle", B_TRIANGLE }, { "circle", B_CIRCLE }, { "cross", B_CROSS },
    { "square", B_SQUARE },
};

static const struct { int vk, bit; } keymap[] = {
    { VK_UP, B_UP }, { VK_DOWN, B_DOWN }, { VK_LEFT, B_LEFT }, { VK_RIGHT, B_RIGHT },
    { VK_RETURN, B_START }, { VK_BACK, B_SELECT }, { 'Z', B_CROSS }, { 'X', B_CIRCLE },
    { 'A', B_SQUARE }, { 'S', B_TRIANGLE }, { 'Q', B_L1 }, { 'W', B_R1 }, { '1', B_L2 },
    { '2', B_R2 },
};

#define MAX_SCRIPT 64
static struct { int bit; double t0, t1; } script[MAX_SCRIPT];
static int nscript = -1;

static void parse_script(void) {
    nscript = 0;
    const char *s = getenv("PSX_INPUT");
    while (s && *s && nscript < MAX_SCRIPT) {
        char name[16] = { 0 };
        int n = 0;
        while (*s && *s != '@' && n < 15) name[n++] = (char)(*s++ | 0x20);
        if (*s != '@') break;
        s++;
        char *end;
        double t0 = strtod(s, &end), t1 = t0 + 0.25;
        s = end;
        if (*s == '-') { t1 = strtod(s + 1, &end); s = end; }
        if (*s == ',') s++;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            if (!strcmp(name, names[i].name)) {
                script[nscript].bit = names[i].bit;
                script[nscript].t0 = t0;
                script[nscript].t1 = t1;
                nscript++;
            }
    }
}

uint16_t psx_pad_buttons(void) {
    if (nscript < 0) parse_script();
    uint16_t pressed = 0;
    double t = (double)psx_cycles() / 33868800.0;
    for (int i = 0; i < nscript; i++)
        if (t >= script[i].t0 && t < script[i].t1) pressed |= (uint16_t)(1u << script[i].bit);
    if (display_has_focus())
        for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
            if (GetAsyncKeyState(keymap[i].vk) & 0x8000) pressed |= (uint16_t)(1u << keymap[i].bit);
    return (uint16_t)~pressed;   /* active low */
}
