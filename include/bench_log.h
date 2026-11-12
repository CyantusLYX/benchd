/*
 * bench_log.h — two-level debug log (§7).
 *
 * Compile time: -DLOG_COMPILE_LEVEL=N. Macros above N expand to a no-op whose arguments sit
 *   inside an unevaluated sizeof(), so the format string is NOT emitted into the binary
 *   (`strings build/bin/benchd | grep <trace msg>` finds nothing) while the compiler still
 *   type-checks the arguments and does not warn about "unused" variables.
 * Run time: the active level lives behind a pointer. The server points it into shared memory
 *   so that `kill -USR1 <master>` changes the level of every process instantly.
 */
#ifndef BENCH_LOG_H
#define BENCH_LOG_H

#include <stdio.h>

/* numeric literals in the #if tests below: the preprocessor cannot see enum values */
enum { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3, LOG_TRACE = 4 };

#ifndef LOG_COMPILE_LEVEL
#define LOG_COMPILE_LEVEL 2
#endif

void bench_log_set_role(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void bench_log_set_level_ptr(volatile int *p);   /* NULL -> back to the private level */
void bench_log_set_level(int lvl);
int  bench_log_get_level(void);
int  bench_log_compile_level(void);
int  bench_log_enabled(int lvl);
int  bench_log_level_from_str(const char *s);   /* "debug" / "3" -> 3, -1 if bad */
const char *bench_log_level_name(int lvl);
int  bench_log_init_level(int verbose_count);    /* INFO, BENCH_LOG env, then +N for -v */
int  bench_log_open_file(const char *path);       /* extra copy of every line */
void bench_log_set_stderr(int on);
void bench_log_write(int lvl, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define BENCH_LOG_AT(lvl, ...) \
    do { if (bench_log_enabled(lvl)) bench_log_write((lvl), __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define BENCH_LOG_OFF(...) do { (void)sizeof(printf(__VA_ARGS__)); } while (0)

#define LOGE(...) BENCH_LOG_AT(LOG_ERROR, __VA_ARGS__)
#if LOG_COMPILE_LEVEL >= 1   /* WARN */
#define LOGW(...) BENCH_LOG_AT(LOG_WARN, __VA_ARGS__)
#else
#define LOGW(...) BENCH_LOG_OFF(__VA_ARGS__)
#endif
#if LOG_COMPILE_LEVEL >= 2   /* INFO */
#define LOGI(...) BENCH_LOG_AT(LOG_INFO, __VA_ARGS__)
#else
#define LOGI(...) BENCH_LOG_OFF(__VA_ARGS__)
#endif
#if LOG_COMPILE_LEVEL >= 3   /* DEBUG */
#define LOGD(...) BENCH_LOG_AT(LOG_DEBUG, __VA_ARGS__)
#else
#define LOGD(...) BENCH_LOG_OFF(__VA_ARGS__)
#endif
#if LOG_COMPILE_LEVEL >= 4   /* TRACE */
#define LOGT(...) BENCH_LOG_AT(LOG_TRACE, __VA_ARGS__)
#else
#define LOGT(...) BENCH_LOG_OFF(__VA_ARGS__)
#endif

#endif
