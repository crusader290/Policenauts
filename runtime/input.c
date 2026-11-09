/* Controller input: keyboard (when the window has focus) and scripted presses.
 *
 * Keyboard: arrows = D-pad, Enter = Start, Backspace = Select, Z = Cross, X = Circle,
 *           A = Square, S = Triangle, Q/W = L1/R1, 1/2 = L2/R2.
 * Gamepad:  the first XInput controller, buttons by position (A = Cross, B = Circle,
 *           X = Square, Y = Triangle), bumpers/triggers = L1/R1/L2/R2, Back = Select,
 *           left stick = D-pad. Works without window focus; off when headless or with
 *           PSX_NO_PAD=1.
 *
 * PSX_INPUT scripts presses in virtual seconds, for headless runs:
 *   PSX_INPUT="start@95,cross@110-111.5"   (a bare time holds the button for 0.25 s)
 *   PSX_INPUT=@file                         (the same list read from a file; commas,
 *                                            spaces or newlines separate entries)
 *   swap@T                                  swaps discs at T, like F2 in the window
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bios.h"
#include "display.h"
#include "cd.h"

enum {
    B_SELECT = 0, B_L3, B_R3, B_START, B_UP, B_RIGHT, B_DOWN, B_LEFT,
    B_L2, B_R2, B_L1, B_R1, B_TRIANGLE, B_CIRCLE, B_CROSS, B_SQUARE, B_SWAP
};

static const struct { const char *name; int bit; } names[] = {
    { "select", B_SELECT }, { "start", B_START }, { "up", B_UP }, { "right", B_RIGHT },
    { "down", B_DOWN }, { "left", B_LEFT }, { "l2", B_L2 }, { "r2", B_R2 }, { "l1", B_L1 },
    { "r1", B_R1 }, { "triangle", B_TRIANGLE }, { "circle", B_CIRCLE }, { "cross", B_CROSS },
    { "square", B_SQUARE },
    { "swap", B_SWAP },                  /* not a button: swap discs (like F2) */
};

static const struct { int vk, bit; } keymap[] = {
    { VK_UP, B_UP }, { VK_DOWN, B_DOWN }, { VK_LEFT, B_LEFT }, { VK_RIGHT, B_RIGHT },
    { VK_RETURN, B_START }, { VK_BACK, B_SELECT }, { 'Z', B_CROSS }, { 'X', B_CIRCLE },
    { 'A', B_SQUARE }, { 'S', B_TRIANGLE }, { 'Q', B_L1 }, { 'W', B_R1 }, { '1', B_L2 },
    { '2', B_R2 },
};

/* ---- XInput gamepad (first connected controller), loaded at runtime ---------------- */
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XINPUT_STATE *);
static XInputGetState_t xinput_get_state;
static int xinput_tried, xinput_pad = -1;
static uint64_t xinput_next_scan;

static const struct { WORD mask; int bit; } padmap[] = {
    { XINPUT_GAMEPAD_DPAD_UP, B_UP }, { XINPUT_GAMEPAD_DPAD_DOWN, B_DOWN },
    { XINPUT_GAMEPAD_DPAD_LEFT, B_LEFT }, { XINPUT_GAMEPAD_DPAD_RIGHT, B_RIGHT },
    { XINPUT_GAMEPAD_START, B_START }, { XINPUT_GAMEPAD_BACK, B_SELECT },
    { XINPUT_GAMEPAD_A, B_CROSS }, { XINPUT_GAMEPAD_B, B_CIRCLE },      /* by position */
    { XINPUT_GAMEPAD_X, B_SQUARE }, { XINPUT_GAMEPAD_Y, B_TRIANGLE },
    { XINPUT_GAMEPAD_LEFT_SHOULDER, B_L1 }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, B_R1 },
    { XINPUT_GAMEPAD_LEFT_THUMB, B_L3 }, { XINPUT_GAMEPAD_RIGHT_THUMB, B_R3 },
};

static uint16_t gamepad_buttons(void) {
    if (!xinput_tried) {
        xinput_tried = 1;
        if (getenv("PSX_HEADLESS") || getenv("PSX_NO_PAD")) return 0;
        static const char *dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        for (size_t i = 0; i < sizeof dlls / sizeof dlls[0] && !xinput_get_state; i++) {
            HMODULE m = LoadLibraryA(dlls[i]);
            if (m) xinput_get_state = (XInputGetState_t)(void (*)(void))GetProcAddress(m, "XInputGetState");
        }
    }
    if (!xinput_get_state) return 0;
    XINPUT_STATE st;
    if (xinput_pad < 0) {                        /* polling absent pads is slow: rescan ~1/s */
        uint64_t now = GetTickCount64();
        if (now < xinput_next_scan) return 0;
        xinput_next_scan = now + 1000;
        for (DWORD i = 0; i < 4 && xinput_pad < 0; i++)
            if (xinput_get_state(i, &st) == ERROR_SUCCESS) {
                xinput_pad = (int)i;
                fprintf(stderr, "[input] gamepad %lu connected\n", i);
            }
        if (xinput_pad < 0) return 0;
    }
    if (xinput_get_state((DWORD)xinput_pad, &st) != ERROR_SUCCESS) {
        fprintf(stderr, "[input] gamepad %d disconnected\n", xinput_pad);
        xinput_pad = -1;
        return 0;
    }
    const XINPUT_GAMEPAD *g = &st.Gamepad;
    uint16_t p = 0;
    for (size_t i = 0; i < sizeof padmap / sizeof padmap[0]; i++)
        if (g->wButtons & padmap[i].mask) p |= (uint16_t)(1u << padmap[i].bit);
    if (g->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) p |= 1u << B_L2;
    if (g->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) p |= 1u << B_R2;
    const int dz = 12000;                        /* left stick doubles as the D-pad */
    if (g->sThumbLX < -dz) p |= 1u << B_LEFT;
    if (g->sThumbLX > dz) p |= 1u << B_RIGHT;
    if (g->sThumbLY > dz) p |= 1u << B_UP;
    if (g->sThumbLY < -dz) p |= 1u << B_DOWN;
    return p;
}

#define MAX_SCRIPT 4096
static struct { int bit; double t0, t1; } script[MAX_SCRIPT];
static int nscript = -1;

static void parse_script(void) {
    nscript = 0;
    const char *s = getenv("PSX_INPUT");
    static char text[65536];
    if (s && *s == '@') {
        FILE *f = fopen(s + 1, "rb");
        size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
        if (f) fclose(f);
        text[n] = 0;
        s = text;
    }
    while (s && *s && nscript < MAX_SCRIPT) {
        while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t' || *s == ',') s++;
        if (!*s) break;
        char name[16] = { 0 };
        int n = 0;
        while (*s && *s != '@' && n < 15) name[n++] = (char)(*s++ | 0x20);
        if (*s != '@') break;
        s++;
        char *end;
        double t0 = strtod(s, &end), t1 = t0 + 0.25;
        s = end;
        if (*s == '-') { t1 = strtod(s + 1, &end); s = end; }
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
    for (int i = 0; i < nscript; i++) {
        if (t < script[i].t0 || t >= script[i].t1) continue;
        if (script[i].bit == B_SWAP) {
            cd_swap(cd_disc() == 2 ? 1 : 2, 1.0);
            script[i].t1 = -1;                                  /* once */
        } else {
            pressed |= (uint16_t)(1u << script[i].bit);
        }
    }
    pressed |= gamepad_buttons();
    if (display_has_focus())
        for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
            if (GetAsyncKeyState(keymap[i].vk) & 0x8000) pressed |= (uint16_t)(1u << keymap[i].bit);
    return (uint16_t)~pressed;   /* active low */
}
