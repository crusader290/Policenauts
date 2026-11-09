/* Watchdog: crash reports, hang detection and snapshots.
 *
 * Each report is written to Policenauts.log and saved in reports/<date>-<time>-<kind>/
 * along with ram.bin (2 MB of PSX RAM), screen.bmp (the last frame shown), card1.mcd
 * (the memory card at that moment) and a copy of the log. Snapshots and fatals also get
 * vram.bmp.
 *
 *   crash     native exception (access violation, stack overflow, ...) or abort()
 *   fatal     psx_fatal(): something the runtime can't handle (unknown call target,
 *             unmapped read, unimplemented BIOS call, ...)
 *   hang      no new frame for PSX_WATCHDOG seconds (default 10, 0 = off). Not fatal:
 *             the log notes when the game runs again.
 *   snapshot  F12 in the window, for soft-locks where frames keep coming
 *
 * The report is written by the watchdog thread while the failing thread waits, so even a
 * stack overflow gets one. The native call stack is the game's call chain: recompiled
 * functions call each other directly, and their names (f_itp_8006EDF0) are read from the
 * exe's symbol table.
 *
 * Between reports the watchdog logs breadcrumbs: overlay changes and script changes.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bios.h"
#include "cd.h"
#include "display.h"
#include "log.h"

enum { K_CRASH, K_FATAL, K_HANG, K_SNAPSHOT };
static const char *const kind_name[] = { "crash", "fatal", "hang", "snapshot" };

static HANDLE wd_thread, main_thread, req_event, done_event;
static DWORD wd_tid;
static CRITICAL_SECTION req_lock;
static int hang_seconds = 10;

static struct {
    int kind;
    char why[512];
    CONTEXT ctx;
    int have_ctx;
    EXCEPTION_RECORD exc;
    int have_exc;
    DWORD tid;
    char dir[MAX_PATH];
} req;

/* ---- function names from the exe's COFF symbol table ---------------------------------- */
typedef struct { uintptr_t addr; const char *name; } Sym;
static Sym *syms;
static int nsyms;
static INIT_ONCE syms_once = INIT_ONCE_STATIC_INIT;

static int sym_cmp(const void *a, const void *b) {
    uintptr_t x = ((const Sym *)a)->addr, y = ((const Sym *)b)->addr;
    return x < y ? -1 : x > y;
}

static BOOL CALLBACK load_symbols(INIT_ONCE *once, void *param, void **ctx) {
    (void)once; (void)param; (void)ctx;
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    DWORD symoff = nt->FileHeader.PointerToSymbolTable, nsym = nt->FileHeader.NumberOfSymbols;
    if (!symoff || !nsym) return TRUE;                       /* stripped */
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int nsec = nt->FileHeader.NumberOfSections;
    HANDLE f = CreateFileA(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return TRUE;
    LARGE_INTEGER size;
    GetFileSizeEx(f, &size);
    if (size.QuadPart <= symoff + (LONGLONG)nsym * IMAGE_SIZEOF_SYMBOL) { CloseHandle(f); return TRUE; }
    DWORD len = (DWORD)(size.QuadPart - symoff), got = 0;
    uint8_t *tab = malloc(len);
    LARGE_INTEGER off = { .QuadPart = symoff };
    SetFilePointerEx(f, off, NULL, FILE_BEGIN);
    if (!tab || !ReadFile(f, tab, len, &got, NULL) || got != len) { CloseHandle(f); free(tab); return TRUE; }
    CloseHandle(f);
    const char *strtab = (const char *)tab + (size_t)nsym * IMAGE_SIZEOF_SYMBOL;
    DWORD strsize = len - nsym * IMAGE_SIZEOF_SYMBOL;
    syms = malloc(sizeof *syms * nsym);
    char *short_names = malloc((size_t)nsym * 9);
    if (!syms || !short_names) return TRUE;
    for (DWORD i = 0; i < nsym; i++) {
        const IMAGE_SYMBOL *s = (const IMAGE_SYMBOL *)(tab + (size_t)i * IMAGE_SIZEOF_SYMBOL);
        int n = s->SectionNumber;
        /* functions, plus untyped code labels (assembly such as ___chkstk_ms); not the
           section symbols (".text") that carry aux records */
        if (n > 0 && n <= nsec && (sec[n - 1].Characteristics & IMAGE_SCN_CNT_CODE) &&
            ((s->Type & 0x30) == 0x20 || (s->Type == 0 && s->NumberOfAuxSymbols == 0)) &&
            (s->StorageClass == IMAGE_SYM_CLASS_EXTERNAL || s->StorageClass == IMAGE_SYM_CLASS_STATIC)) {
            const char *name;
            if (s->N.Name.Short == 0) {
                if (s->N.Name.Long >= strsize) goto skip;
                name = strtab + s->N.Name.Long;
            } else {
                char *d = short_names + (size_t)nsyms * 9;
                memcpy(d, s->N.ShortName, 8);
                d[8] = 0;
                name = d;
            }
            if (name[0] == '.') goto skip;
            syms[nsyms].addr = (uintptr_t)base + sec[n - 1].VirtualAddress + s->Value;
            syms[nsyms].name = name;
            nsyms++;
        }
    skip:
        i += s->NumberOfAuxSymbols;
    }
    qsort(syms, (size_t)nsyms, sizeof *syms, sym_cmp);
    return TRUE;
}

static const Sym *find_sym(uintptr_t pc) {
    InitOnceExecuteOnce(&syms_once, load_symbols, NULL, NULL);
    int lo = 0, hi = nsyms - 1;
    const Sym *best = NULL;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (syms[mid].addr <= pc) { best = &syms[mid]; lo = mid + 1; } else hi = mid - 1;
    }
    return best && pc - best->addr < 0x100000 ? best : NULL;
}

static void describe_pc(uintptr_t pc, char *out, size_t n) {
    HMODULE m = NULL;
    char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pc, &m) && m) {
        GetModuleFileNameA(m, mod, sizeof mod);
        char *slash = strrchr(mod, '\\');
        if (slash) memmove(mod, slash + 1, strlen(slash));
    }
    const Sym *s = m == GetModuleHandleA(NULL) ? find_sym(pc) : NULL;
    if (s) snprintf(out, n, "%s+0x%llx", s->name, (unsigned long long)(pc - s->addr));
    else snprintf(out, n, "%s+0x%llx", mod, (unsigned long long)(pc - (uintptr_t)m));
}

/* ---- native stack walk ------------------------------------------------------------------ */
static int readable(uintptr_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!a || !VirtualQuery((void *)a, &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return a + n <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}

static void print_frame(int i, uintptr_t at, int repeats) {
    char d[300];
    describe_pc(at, d, sizeof d);
    const char *seg = NULL;
    unsigned mips = 0;
    if (!strncmp(d, "f_", 2) && sscanf(d, "f_%*[^_]_%8X", &mips) == 1) seg = psx_segment_of(mips);
    char rep[48] = "";
    if (repeats) snprintf(rep, sizeof rep, "  (and %d more times)", repeats);
    log_printf("  #%-3d %s%s%s%s\n", i, d, seg ? "  <- " : "", seg ? seg : "", rep);
}

#define MAX_FRAMES 4096
static uintptr_t frames[MAX_FRAMES];

static void walk_stack(CONTEXT ctx) {
    int n = 0;
    while (n < MAX_FRAMES && ctx.Rip) {
        uintptr_t at = n ? ctx.Rip - 1 : ctx.Rip;         /* return addresses point past the call */
        frames[n++] = at;
        if (!readable(ctx.Rsp, 512)) break;
        DWORD64 img;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(at, &img, NULL);
        const Sym *s = fe ? NULL : find_sym(at);
        if (fe) {
            void *hd;
            DWORD64 ef;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, at, fe, &ctx, &hd, &ef, NULL);
        } else if (s && !strcmp(s->name, "___chkstk_ms")) {  /* stack probe: pushed rcx, rax */
            ctx.Rip = *(DWORD64 *)(ctx.Rsp + 16);
            ctx.Rsp += 24;
        } else {                                           /* leaf: return address at rsp */
            ctx.Rip = *(DWORD64 *)ctx.Rsp;
            ctx.Rsp += 8;
        }
    }
    /* print with runs of the same frame folded; for very deep stacks (runaway recursion)
       the top and the bottom are what matter */
    int printed = 0;
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && frames[j] == frames[i]) j++;
        if (printed == 40 && n - i > 40) {
            int skip_to = n - 40;
            while (skip_to > i && frames[skip_to - 1] == frames[skip_to]) skip_to--;
            if (skip_to > i) {
                log_printf("  ... %d frames ...\n", skip_to - i);
                i = skip_to;
                continue;
            }
        }
        print_frame(i, frames[i], j - i - 1);
        printed++;
        i = j;
    }
    if (n == MAX_FRAMES) log_printf("  ... (stack deeper than %d frames)\n", MAX_FRAMES);
}

/* ---- game state ------------------------------------------------------------------------- */
static const char *const regname[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra" };

static void game_state(void) {
    log_printf("virtual time %.2f s, frame %llu, disc %d in the drive\n",
               (double)psx_cycles() / 33868800.0, (unsigned long long)display_frame_count(), cd_disc());
    const char *a = psx_overlay_in_slot('A'), *b = psx_overlay_in_slot('B');
    log_printf("overlays: slot A = %s, slot B = %s\n", a ? a : "-", b ? b : "-");
    if (a && !strcmp(a, "itp"))
        log_printf("script: number %08X, pc %08X, buffer %08X\n",
                   MEM_LW(0x80086154u), MEM_LW(0x8008626Cu), MEM_LW(0x80086284u));
    const char *seg = psx_segment_of(psx_cur_func);
    log_printf("last recompiled function entered: %08X (%s)\n", psx_cur_func, seg ? seg : "?");
    const CPU *c = bios_current_cpu();
    log_printf("MIPS registers (current thread):\n");
    for (int r = 0; r < 32; r += 4)
        log_printf("  %-4s %08X  %-4s %08X  %-4s %08X  %-4s %08X\n", regname[r], c->r[r],
                   regname[r + 1], c->r[r + 1], regname[r + 2], c->r[r + 2], regname[r + 3], c->r[r + 3]);
    log_printf("  hi   %08X  lo   %08X\n", c->hi, c->lo);
    log_printf("kernel threads (* = running):\n");
    bios_describe();
    uint32_t fn[8], cnt[8];
    int n = hw_hot_funcs(fn, cnt, 8);
    if (n) {
        log_printf("busiest loops (samples):\n");
        for (int i = 0; i < n; i++) {
            const char *s = psx_segment_of(fn[i]);
            log_printf("  %08X %-8s %u\n", fn[i], s ? s : "?", cnt[i]);
        }
    }
    uint32_t addr[16], ra[16];
    n = psx_recent_calls(addr, ra, 16);
    if (n) {
        log_printf("recent indirect calls (newest first):\n");
        for (int i = 0; i < n; i++) log_printf("  %08X from ra=%08X\n", addr[i], ra[i]);
    }
}

/* ---- report files ----------------------------------------------------------------------- */
static void save(const char *name, const void *data, size_t n) {
    char p[MAX_PATH + 32];
    snprintf(p, sizeof p, "%s\\%s", req.dir, name);
    HANDLE f = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, data, (DWORD)n, &w, NULL);
    CloseHandle(f);
}

static void save_bmp(const char *name, const uint32_t *px, int w, int h) {
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return;
    static uint8_t buf[54 + 1024 * 1024 * 4];
    uint32_t data = (uint32_t)w * (uint32_t)h * 4, v;
    memset(buf, 0, 54);
    buf[0] = 'B'; buf[1] = 'M';
    v = 54 + data; memcpy(buf + 2, &v, 4);
    v = 54; memcpy(buf + 10, &v, 4);
    v = 40; memcpy(buf + 14, &v, 4);
    int32_t sw = w, sh = -h; memcpy(buf + 18, &sw, 4); memcpy(buf + 22, &sh, 4);
    uint16_t planes = 1, bpp = 32; memcpy(buf + 26, &planes, 2); memcpy(buf + 28, &bpp, 2);
    memcpy(buf + 34, &data, 4);
    memcpy(buf + 54, px, data);
    save(name, buf, 54 + data);
}

static void make_dir(void) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    CreateDirectoryA("reports", NULL);
    snprintf(req.dir, sizeof req.dir, "reports\\%04u%02u%02u-%02u%02u%02u-%s", t.wYear, t.wMonth,
             t.wDay, t.wHour, t.wMinute, t.wSecond, kind_name[req.kind]);
    for (int i = 2; !CreateDirectoryA(req.dir, NULL) && i < 100; i++)
        snprintf(req.dir, sizeof req.dir, "reports\\%04u%02u%02u-%02u%02u%02u-%s-%d", t.wYear,
                 t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, kind_name[req.kind], i);
}

static const char *exc_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned access";
    default: return "exception";
    }
}

static void do_report(void) {
    log_flush();
    make_dir();
    log_printf("\n======== %s report ========\n", kind_name[req.kind]);
    if (req.have_exc) {
        char at[300];
        describe_pc((uintptr_t)req.exc.ExceptionAddress, at, sizeof at);
        snprintf(req.why, sizeof req.why, "%s (%08lX) at %s", exc_name(req.exc.ExceptionCode),
                 req.exc.ExceptionCode, at);
        if (req.exc.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && req.exc.NumberParameters >= 2) {
            size_t l = strlen(req.why);
            ULONG_PTR op = req.exc.ExceptionInformation[0];
            snprintf(req.why + l, sizeof req.why - l, ", %s address %llX",
                     op == 0 ? "reading" : op == 1 ? "writing" : "executing",
                     (unsigned long long)req.exc.ExceptionInformation[1]);
        }
    }
    log_printf("%s\n", req.why);
    if (req.have_ctx) {
        log_printf("native call stack (thread %lu):\n", req.tid);
        walk_stack(req.ctx);
    }
    game_state();
    save("ram.bin", psx_ram, PSX_RAM_SIZE);
    int w, h;
    const uint32_t *px = display_last_frame(&w, &h);
    save_bmp("screen.bmp", px, w, h);
    if (req.kind == K_SNAPSHOT || req.kind == K_FATAL) {
        char p[MAX_PATH + 16];
        snprintf(p, sizeof p, "%s\\vram.bmp", req.dir);
        display_save_vram(p);
    }
    const char *card = getenv("PSX_CARD1");
    if (!card) card = "saves/card1.mcd";
    char p[MAX_PATH + 32];
    snprintf(p, sizeof p, "%s\\card1.mcd", req.dir);
    CopyFileA(card, p, FALSE);
    log_printf("report saved in %s\n", req.dir);
    log_printf("======== end of %s report ========\n\n", kind_name[req.kind]);
    snprintf(p, sizeof p, "%s\\Policenauts.log", req.dir);
    log_copy_to(p);
}

static void tell_user(void) {
    if (display_headless() || req.kind == K_HANG) return;
    char msg[1200];
    if (req.kind == K_SNAPSHOT)
        snprintf(msg, sizeof msg, "Snapshot saved in %s", req.dir);
    else
        snprintf(msg, sizeof msg,
                 "Policenauts has stopped.\n\n%s\n\nA report was saved in %s\\ "
                 "(the details are also in %s).", req.why, req.dir, log_path() ? log_path() : "the console");
    MessageBoxA(NULL, msg, "Policenauts", MB_OK | (req.kind == K_SNAPSHOT ? MB_ICONINFORMATION : MB_ICONERROR) |
                MB_TOPMOST | MB_SETFOREGROUND);
}

/* Fill req, then have the watchdog thread write the report while this thread waits. */
static void submit(void) {
    if (!wd_thread || GetCurrentThreadId() == wd_tid) {
        do_report();
        tell_user();
        return;
    }
    SetEvent(req_event);
    WaitForSingleObject(done_event, 120000);
}

static void request(int kind, const char *why, const CONTEXT *ctx) {
    if (req_event) EnterCriticalSection(&req_lock);
    req.kind = kind;
    snprintf(req.why, sizeof req.why, "%s", why);
    req.have_ctx = ctx != NULL;
    if (ctx) req.ctx = *ctx;
    req.have_exc = 0;
    req.tid = GetCurrentThreadId();
    submit();
    if (req_event) LeaveCriticalSection(&req_lock);
}

/* ---- entry points ----------------------------------------------------------------------- */
_Noreturn void psx_fatal(const char *fmt, ...) {
    char why[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    log_flush();
    log_printf("fatal: %s\n", why);
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    request(K_FATAL, why, &ctx);
    exit(1);
}

void watchdog_snapshot(const char *why) {
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    request(K_SNAPSHOT, why, &ctx);
}

/* Runs on the faulting thread, possibly with almost no stack left: no formatting here. */
static LONG WINAPI on_exception(EXCEPTION_POINTERS *ep) {
    static volatile LONG entered;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (InterlockedExchange(&entered, 1)) TerminateProcess(GetCurrentProcess(), code);
    if (req_event) EnterCriticalSection(&req_lock);
    req.kind = K_CRASH;
    req.ctx = *ep->ContextRecord;
    req.have_ctx = 1;
    req.exc = *ep->ExceptionRecord;
    req.have_exc = 1;
    req.tid = GetCurrentThreadId();
    submit();
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void on_abort(int sig) {
    (void)sig;
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    request(K_CRASH, "abort() was called", &ctx);
    TerminateProcess(GetCurrentProcess(), 3);
}

static void report_hang(double seconds) {
    CONTEXT ctx = { .ContextFlags = CONTEXT_FULL };
    int suspended = SuspendThread(main_thread) != (DWORD)-1;
    EnterCriticalSection(&req_lock);
    req.kind = K_HANG;
    snprintf(req.why, sizeof req.why, "no new frame for %.0f seconds (the game is not running)", seconds);
    req.have_ctx = suspended && GetThreadContext(main_thread, &ctx);
    if (req.have_ctx) req.ctx = ctx;
    req.have_exc = 0;
    req.tid = GetThreadId(main_thread);
    do_report();
    LeaveCriticalSection(&req_lock);
    if (suspended) ResumeThread(main_thread);
}

static DWORD WINAPI wd_main(void *arg) {
    (void)arg;
    InitOnceExecuteOnce(&syms_once, load_symbols, NULL, NULL);
    uint64_t last_frames = 0, t_last = GetTickCount64(), t_prev = t_last;
    int hung = 0;
    const char *ov_a = NULL, *ov_b = NULL;
    uint32_t script = 0xFFFFFFFFu;
    for (;;) {
        if (WaitForSingleObject(req_event, 500) == WAIT_OBJECT_0) {
            do_report();
            tell_user();
            SetEvent(done_event);
            continue;
        }
        uint64_t t = GetTickCount64(), f = display_frame_count();
        if (t - t_prev > 5000) t_last = t;              /* the PC was asleep, not the game */
        t_prev = t;
        if (f != last_frames || display_paused() || IsDebuggerPresent()) {
            if (hung) log_printf("[watchdog] running again after %.1f s\n", (double)(t - t_last) / 1000.0);
            hung = 0;
            last_frames = f;
            t_last = t;
        } else if (hang_seconds > 0 && f > 0 && !hung && t - t_last >= (uint64_t)hang_seconds * 1000) {
            hung = 1;
            report_hang((double)(t - t_last) / 1000.0);
        }
        if (hung) continue;
        /* breadcrumbs: where in the game we are */
        const char *a = psx_overlay_in_slot('A'), *b = psx_overlay_in_slot('B');
        if ((a && a != ov_a) || (b && b != ov_b)) {
            if (a) ov_a = a;
            if (b) ov_b = b;
            log_printf("[watchdog] overlays: slot A = %s, slot B = %s\n", a ? a : "-", b ? b : "-");
        }
        if (a && !strcmp(a, "itp")) {
            uint32_t s = MEM_LW(0x80086154u);
            if (s != script) {
                script = s;
                log_printf("[watchdog] script %08X\n", s);
            }
        }
    }
    return 0;
}

/* PSX_TEST_FAULT=kind@seconds: fail on purpose (game thread) to check the reports.
 * kind: segv, stack, abort, fatal, hang, snapshot */
static volatile int sink;
static int recurse(int n) { volatile char pad[4096]; pad[0] = (char)n; return recurse(n + 1) + pad[0]; }

void watchdog_frame(void) {
    static int parsed;
    static char kind[16];
    static double when = -1;
    if (!parsed) {
        parsed = 1;
        const char *e = getenv("PSX_TEST_FAULT");
        if (e) sscanf(e, "%15[^@]@%lf", kind, &when);
    }
    if (when < 0 || (double)psx_cycles() / 33868800.0 < when) return;
    when = -1;
    log_printf("[watchdog] PSX_TEST_FAULT: %s\n", kind);
    if (!strcmp(kind, "segv")) *(volatile int *)(uintptr_t)sink = 1;
    else if (!strcmp(kind, "stack")) sink = recurse(0);
    else if (!strcmp(kind, "abort")) abort();
    else if (!strcmp(kind, "fatal")) psx_fatal("test fault (PSX_TEST_FAULT)");
    else if (!strcmp(kind, "hang")) Sleep((DWORD)(hang_seconds + 3) * 1000);
    else if (!strcmp(kind, "snapshot")) watchdog_snapshot("PSX_TEST_FAULT snapshot");
}

void watchdog_init(void) {
    const char *e = getenv("PSX_WATCHDOG");
    if (e) hang_seconds = atoi(e);
    InitializeCriticalSection(&req_lock);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread,
                    0, FALSE, DUPLICATE_SAME_ACCESS);
    req_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    done_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    wd_thread = CreateThread(NULL, 256 * 1024, wd_main, NULL, 0, &wd_tid);
    SetUnhandledExceptionFilter(on_exception);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, on_abort);
    log_printf("[watchdog] on: crash reports in reports\\, hang check after %d s%s, F12 = snapshot\n",
               hang_seconds, hang_seconds > 0 ? "" : " (off)");
}
