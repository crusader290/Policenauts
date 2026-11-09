/* Win32 window showing the GPU's display area, paced to the NTSC frame rate.
 *
 * Environment:
 *   PSX_HEADLESS=1            no window (automated runs)
 *   PSX_FAST=1                don't pace to real time
 *   PSX_DUMP_DIR=dir          write frames as BMP into dir
 *   PSX_DUMP_EVERY=N          ...every N frames (default 60)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "display.h"
#include "cd.h"
#include "gpu.h"
#include "log.h"

/* One virtual frame (263 lines x 2152 cycles at 33.8688 MHz, ~59.84 Hz). Pacing to
   exactly this keeps the SPU's 44.1 kHz output in step with the audio device. */
#define FRAME_SECONDS (565976.0 / 33868800.0)

static HWND hwnd;
static int headless, fast, dump_every = 60;
static const char *dump_dir;
static uint32_t frame_buf[640 * 480];
static int frame_w = 320, frame_h = 240;
static uint64_t frame_no;
static LARGE_INTEGER qpf, t0;

static volatile LONG in_modal;      /* dragging/resizing: no frames, but not a hang */

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE: exit(0);
    case WM_ENTERSIZEMOVE: case WM_ENTERMENULOOP: in_modal = 1; break;
    case WM_EXITSIZEMOVE: case WM_EXITMENULOOP: in_modal = 0; break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_KEYDOWN:
        if (wp == VK_F2 && !(lp & (1 << 30))) cd_swap(cd_disc() == 2 ? 1 : 2, 1.0);   /* swap discs */
        if (wp == VK_F12 && !(lp & (1 << 30))) watchdog_snapshot("F12 pressed");   /* report */
        break;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static void write_bmp(const char *path, const uint32_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t data = (uint32_t)w * h * 4, fsize = 54 + data, v;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &fsize, 4);
    v = 54; memcpy(hdr + 10, &v, 4);
    v = 40; memcpy(hdr + 14, &v, 4);
    int32_t sw = w, sh = -h; memcpy(hdr + 18, &sw, 4); memcpy(hdr + 22, &sh, 4);
    uint16_t planes = 1, bpp = 32; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &data, 4);
    fwrite(hdr, 1, 54, f);
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
}

void display_init(void) {
    headless = getenv("PSX_HEADLESS") != NULL;
    fast = getenv("PSX_FAST") != NULL;
    dump_dir = getenv("PSX_DUMP_DIR");
    if (getenv("PSX_DUMP_EVERY")) dump_every = atoi(getenv("PSX_DUMP_EVERY"));
    if (dump_every < 1) dump_every = 1;
    if (dump_dir) CreateDirectoryA(dump_dir, NULL);
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&t0);
    timeBeginPeriod(1);
    if (headless) return;

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "PolicenautsRecomp";
    RegisterClassA(&wc);
    RECT r = { 0, 0, 960, 720 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA(wc.lpszClassName, "Policenauts", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                         NULL, NULL, wc.hInstance, NULL);
}

static void present(void) {
    HDC dc = GetDC(hwnd);
    RECT cr;
    GetClientRect(hwnd, &cr);
    int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    /* 4:3 letterbox */
    int dw = cw, dh = cw * 3 / 4;
    if (dh > ch) { dh = ch; dw = ch * 4 / 3; }
    int dx = (cw - dw) / 2, dy = (ch - dh) / 2;
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = frame_w;
    bi.bmiHeader.biHeight = -frame_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    if (dx > 0) { PatBlt(dc, 0, 0, dx, ch, BLACKNESS); PatBlt(dc, dx + dw, 0, cw - dx - dw, ch, BLACKNESS); }
    if (dy > 0) { PatBlt(dc, 0, 0, cw, dy, BLACKNESS); PatBlt(dc, 0, dy + dh, cw, ch - dy - dh, BLACKNESS); }
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, frame_w, frame_h, frame_buf, &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(hwnd, dc);
}

void display_vblank(void) {
    frame_no++;
    watchdog_frame();
    gpu_output(frame_buf, &frame_w, &frame_h);
    if (dump_dir && frame_no % (uint64_t)dump_every == 0) {
        char path[512];
        snprintf(path, sizeof path, "%s/frame_%06llu.bmp", dump_dir, (unsigned long long)frame_no);
        write_bmp(path, frame_buf, frame_w, frame_h);
    }
    if (!headless) {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        present();
    }
    audio_pump();
    if (!fast) {
        LARGE_INTEGER t;
        double target = (double)frame_no * FRAME_SECONDS;
        for (;;) {
            QueryPerformanceCounter(&t);
            double el = (double)(t.QuadPart - t0.QuadPart) / (double)qpf.QuadPart;
            double left = target - el;
            if (left <= 0) {
                if (left < -0.25) {           /* fell far behind: don't try to catch up */
                    t0.QuadPart = t.QuadPart - (LONGLONG)(target * (double)qpf.QuadPart);
                }
                break;
            }
            if (left > 0.002) Sleep((DWORD)((left - 0.001) * 1000.0));
        }
    }
}

int display_has_focus(void) { return hwnd && GetForegroundWindow() == hwnd; }

/* ---- for the watchdog (read from its thread) ----------------------------------------- */
uint64_t display_frame_count(void) { return frame_no; }
int display_paused(void) { return in_modal; }
int display_headless(void) { return headless; }
const uint32_t *display_last_frame(int *w, int *h) { *w = frame_w; *h = frame_h; return frame_buf; }
void display_save_vram(const char *path) { gpu_dump_vram_bmp(path); }

void display_dump_final(void) {
    if (!dump_dir) return;
    char path[512];
    gpu_output(frame_buf, &frame_w, &frame_h);
    snprintf(path, sizeof path, "%s/final.bmp", dump_dir);
    write_bmp(path, frame_buf, frame_w, frame_h);
    snprintf(path, sizeof path, "%s/vram.bmp", dump_dir);
    gpu_dump_vram_bmp(path);
    fprintf(stderr, "dumped %s/final.bmp and vram.bmp (frame %llu)\n", dump_dir,
            (unsigned long long)frame_no);
}
