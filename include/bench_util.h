/*
 * bench_util.h — small helpers shared by every benchd executable (part of libbench.so).
 *
 *   - time helpers (wall clock / monotonic, formatting)
 *   - size strings ("64K", "1M") parse/format
 *   - C-style escape parsing for `bench send "reset\n"`
 *   - hexdump, safe string copy, name sanitising, host:port parsing
 *   - the robustness ("--unsafe") switch table shared by client and server (§9)
 */
#ifndef BENCH_UTIL_H
#define BENCH_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---- time ---- */
uint64_t now_ns(void);          /* CLOCK_REALTIME in ns  */
uint64_t mono_ns(void);         /* CLOCK_MONOTONIC in ns */
uint64_t mono_ms(void);
void     sleep_ms(unsigned ms);
char    *fmt_clock(uint64_t realtime_ns, char *buf, size_t n);  /* "20:44:13.123" */
char    *fmt_hms(uint64_t realtime_ns, char *buf, size_t n);    /* "20:44:13"     */

/* ---- sizes ---- */
int   parse_size(const char *s, uint64_t *out);       /* "64K" -> 65536; 0 ok, -1 bad */
char *fmt_size(uint64_t bytes, char *buf, size_t n);   /* 12620 -> "12.3K" */

/* ---- strings ---- */
size_t bench_strlcpy(char *dst, const char *src, size_t n);
size_t unescape(const char *in, uint8_t *out, size_t cap);   /* \n \r \t \\ \xHH \e \0 */
void   sanitize_name(char *s, size_t n);   /* force printable ASCII, NUL terminate, non-empty */
int    parse_hostport(const char *s, char *host, size_t hostsz, uint16_t *port, uint16_t defport);
void   hexdump(FILE *fp, const void *data, size_t len, uint64_t base);
int    hexdump_line(char *out, size_t outsz, const uint8_t *p, size_t n);

/* ---- robustness switches (§9) ----
 * Each mechanism has a name; `--unsafe=a,b` or `--unsafe=all` turns it OFF at run time so the
 * same binary can show the with/without behaviour. Code is written as
 *     if (!UNSAFE(R_SIGPIPE)) signal(SIGPIPE, SIG_IGN);
 */
enum bench_robust {
    R_ZOMBIE = 0,       /* R1  */
    R_SIGPIPE,          /* R2  */
    R_VALIDATE,         /* R3  */
    R_READN,            /* R4  */
    R_TIMEOUT,          /* R5  */
    R_CTRL_RELEASE,     /* R6  */
    R_ROBUST_MUTEX,     /* R7  */
    R_DEV_REOPEN,       /* R8  */
    R_MAX_CLIENTS,      /* R9  */
    R_OVERRUN,          /* R10 */
    R_TERM_RESTORE,     /* R11 */
    R_RECONNECT,        /* R12 */
    R_COUNT
};

extern uint32_t bench_unsafe_mask;
#define UNSAFE(r) ((bench_unsafe_mask >> (r)) & 1u)

const char *bench_robust_name(int r);
int  bench_unsafe_parse(const char *list, uint32_t *mask);   /* -1 on unknown name */
void bench_unsafe_format(uint32_t mask, char *buf, size_t n); /* "(none)" or "a,b" */

#endif
