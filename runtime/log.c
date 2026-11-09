/* Policenauts.log: everything the runtime writes to stdout/stderr also goes to a log
 * file, each line stamped with wall-clock time and virtual (PSX) seconds. The previous
 * run's log is kept as Policenauts.prev.log.
 *
 *   PSX_LOG=path    log somewhere else
 *   PSX_LOG=none    no log file
 *
 * How: stderr and stdout are redirected into a pipe; a reader thread copies the pipe to
 * the original console (unchanged) and to the log file (stamped). Crash reports use
 * log_write(), which writes directly and never touches the CRT's stream locks.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bios.h"
#include "log.h"
#if __has_include("version.h")
#include "version.h"                 /* build/version.h, written by configure.py */
#endif
#ifndef PSX_VERSION
#define PSX_VERSION "unknown"
#endif

static HANDLE file = INVALID_HANDLE_VALUE, console = INVALID_HANDLE_VALUE, pipe_rd;
static CRITICAL_SECTION lock;
static int lock_ready, at_line_start = 1;
static volatile LONG reader_busy;
static char path[MAX_PATH];

static void put(HANDLE h, const char *s, size_t n) {
    DWORD w;
    if (h != INVALID_HANDLE_VALUE && h && n) WriteFile(h, s, (DWORD)n, &w, NULL);
}

/* To the file with a stamp at each line start; to the console as is. */
static void emit(const char *s, size_t n) {
    if (lock_ready) EnterCriticalSection(&lock);
    for (size_t i = 0; i < n && file != INVALID_HANDLE_VALUE;) {
        if (at_line_start) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            char st[48];
            int k = snprintf(st, sizeof st, "%02u:%02u:%02u.%03u %9.2f  ", t.wHour, t.wMinute,
                             t.wSecond, t.wMilliseconds, (double)psx_cycles() / 33868800.0);
            put(file, st, (size_t)k);
            at_line_start = 0;
        }
        const char *nl = memchr(s + i, '\n', n - i);
        size_t len = nl ? (size_t)(nl - (s + i)) + 1 : n - i;
        put(file, s + i, len);
        i += len;
        if (nl) at_line_start = 1;
    }
    if (lock_ready) LeaveCriticalSection(&lock);
    put(console, s, n);   /* outside the lock: a console paused by a selection can't block the file */
}

void log_write(const char *s, uint32_t n) { emit(s, n); }

void log_printf(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    emit(buf, (size_t)n);
}

/* The read end is overlapped: on a synchronous handle, PeekNamedPipe in log_flush would
 * queue behind the reader's pending ReadFile and block. */
static OVERLAPPED rd_ov;

static DWORD WINAPI reader_main(void *arg) {
    (void)arg;
    char buf[4096];
    DWORD n;
    for (;;) {
        if (!ReadFile(pipe_rd, buf, sizeof buf, NULL, &rd_ov) && GetLastError() != ERROR_IO_PENDING) return 0;
        if (!GetOverlappedResult(pipe_rd, &rd_ov, &n, TRUE) || !n) return 0;
        InterlockedExchange(&reader_busy, 1);
        emit(buf, n);
        InterlockedExchange(&reader_busy, 0);
    }
}

void log_flush(void) {
    if (!pipe_rd) return;
    fflush(stdout);
    fflush(stderr);
    for (int i = 0; i < 500; i++) {                  /* up to ~0.5 s */
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe_rd, NULL, 0, NULL, &avail, NULL)) return;
        /* done when nothing is queued, no read has completed unhandled, nothing is mid-write */
        if (!avail && WaitForSingleObject(rd_ov.hEvent, 0) != WAIT_OBJECT_0 && !reader_busy) return;
        Sleep(1);
    }
}

const char *log_path(void) { return file != INVALID_HANDLE_VALUE ? path : NULL; }

int log_copy_to(const char *dst) {
    if (file == INVALID_HANDLE_VALUE) return 0;
    log_flush();
    HANDLE in = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
    if (in == INVALID_HANDLE_VALUE) return 0;
    HANDLE out = CreateFileA(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return 0; }
    char buf[65536];
    DWORD n;
    while (ReadFile(in, buf, sizeof buf, &n, NULL) && n) put(out, buf, n);
    CloseHandle(in);
    CloseHandle(out);
    return 1;
}

static void log_exit(void) {
    log_printf("[log] exit\n");
    log_flush();
}

static void header(int argc, char **argv) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    log_printf("Policenauts recomp %s (built %s %s), started %04u-%02u-%02u %02u:%02u:%02u\n",
               PSX_VERSION, __DATE__, __TIME__, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond);
    typedef LONG (WINAPI *RtlGetVersion_t)(OSVERSIONINFOW *);
    RtlGetVersion_t rgv = (RtlGetVersion_t)(void (*)(void))GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = { sizeof v };
    if (rgv && rgv(&v) == 0)
        log_printf("Windows %lu.%lu build %lu\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(sizeof cwd, cwd);
    log_printf("directory: %s\ncommand:", cwd);
    for (int i = 0; i < argc; i++) log_printf(" %s", argv[i]);
    log_printf("\n");
    char *env = GetEnvironmentStringsA();             /* the PSX_* switches in effect */
    for (char *e = env; e && *e; e += strlen(e) + 1)
        if (!strncmp(e, "PSX_", 4)) log_printf("env: %s\n", e);
    if (env) FreeEnvironmentStringsA(env);
}

void log_init(int argc, char **argv) {
    InitializeCriticalSection(&lock);
    lock_ready = 1;
    console = GetStdHandle(STD_ERROR_HANDLE);
    const char *p = getenv("PSX_LOG");
    if (!p) p = "Policenauts.log";
    if (strcmp(p, "none") != 0) {
        snprintf(path, sizeof path, "%s", p);
        char prev[MAX_PATH + 16];
        size_t n = strlen(path);
        if (n > 4 && !_stricmp(path + n - 4, ".log"))
            snprintf(prev, sizeof prev, "%.*s.prev.log", (int)(n - 4), path);
        else
            snprintf(prev, sizeof prev, "%s.prev", path);
        MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
        file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) fprintf(stderr, "warning: cannot write %s\n", path);
    }
    if (file != INVALID_HANDLE_VALUE) {
        /* keep a private handle to the real console, then point fds 1 and 2 at the pipe */
        HANDLE con = (HANDLE)_get_osfhandle(2), dup = INVALID_HANDLE_VALUE;
        if (con != INVALID_HANDLE_VALUE && con)
            DuplicateHandle(GetCurrentProcess(), con, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
        console = dup;
        char name[64];
        snprintf(name, sizeof name, "\\\\.\\pipe\\policenauts-log-%lu", GetCurrentProcessId());
        pipe_rd = CreateNamedPipeA(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   1, 0, 1 << 16, 0, NULL);
        HANDLE wr = pipe_rd == INVALID_HANDLE_VALUE ? INVALID_HANDLE_VALUE
                  : CreateFileA(name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        rd_ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (wr == INVALID_HANDLE_VALUE) pipe_rd = NULL;
        else {
            int fd = _open_osfhandle((intptr_t)wr, _O_WRONLY | _O_BINARY);
            fflush(stdout);
            fflush(stderr);
            _dup2(fd, 1);
            _dup2(fd, 2);
            _close(fd);
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);
            SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(1));
            SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(2));
            CloseHandle(CreateThread(NULL, 0, reader_main, NULL, 0, NULL));
        }
    }
    header(argc, argv);
    atexit(log_exit);
}
