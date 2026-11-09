#pragma once
#include <stdint.h>

/* log.c - Policenauts.log: a copy of everything written to stdout/stderr */
void log_init(int argc, char **argv);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_write(const char *s, uint32_t n);   /* raw text, no CRT stream locks */
void log_flush(void);                        /* wait until stderr output reached the log */
const char *log_path(void);                  /* NULL when logging to a file is off */
int log_copy_to(const char *dst);            /* copy the log so far to dst */

/* watchdog.c - crash reports, hang detection, snapshots */
void watchdog_init(void);
void watchdog_snapshot(const char *why);     /* write a report without stopping (F12) */
void watchdog_frame(void);                   /* once per frame (PSX_TEST_FAULT) */
_Noreturn void psx_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
