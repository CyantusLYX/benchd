/*
 * log.c — the runtime half of the two-level log (§7).
 *
 * Line format:
 *   [20:44:13.123] [INFO ] [child:4321] server/session.c:88  client 127.0.0.1:51234 (alice) joined
 * Each line is formatted into one buffer and emitted with a single write(), so lines from
 * different processes sharing stderr / the log file do not interleave mid-line.
 */
#include "bench_log.h"
#include "bench_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static int private_level = LOG_INFO;
static volatile int *level_ptr = &private_level;
static char role[32] = "main";
static int file_fd = -1;
static int to_stderr = 1;

static const char *names[] = { "ERROR", "WARN", "INFO", "DEBUG", "TRACE" };

void bench_log_set_role(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(role, sizeof role, fmt, ap);
    va_end(ap);
}

void bench_log_set_level_ptr(volatile int *p)
{
    if (p) {
        *p = *level_ptr;
        level_ptr = p;
    } else {
        private_level = *level_ptr;
        level_ptr = &private_level;
    }
}

void bench_log_set_level(int lvl)
{
    if (lvl < LOG_ERROR)
        lvl = LOG_ERROR;
    if (lvl > LOG_TRACE)
        lvl = LOG_TRACE;
    *level_ptr = lvl;
}

int bench_log_get_level(void) { return *level_ptr; }
int bench_log_compile_level(void) { return LOG_COMPILE_LEVEL; }
int bench_log_enabled(int lvl) { return lvl <= *level_ptr; }

const char *bench_log_level_name(int lvl)
{
    return (lvl >= 0 && lvl <= LOG_TRACE) ? names[lvl] : "?";
}

int bench_log_level_from_str(const char *s)
{
    if (!s || !*s)
        return -1;
    if (s[0] >= '0' && s[0] <= '4' && s[1] == 0)
        return s[0] - '0';
    for (int i = 0; i <= LOG_TRACE; i++)
        if (strcasecmp(s, names[i]) == 0)
            return i;
    if (strcasecmp(s, "warning") == 0)
        return LOG_WARN;
    return -1;
}

int bench_log_init_level(int verbose_count)
{
    int lvl = LOG_INFO;
    int env = bench_log_level_from_str(getenv("BENCH_LOG"));
    if (env >= 0)
        lvl = env;
    lvl += verbose_count;
    bench_log_set_level(lvl);
    return *level_ptr;
}

int bench_log_open_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    if (file_fd >= 0)
        close(file_fd);
    file_fd = fd;
    return 0;
}

void bench_log_set_stderr(int on) { to_stderr = on; }

void bench_log_write(int lvl, const char *file, int line, const char *fmt, ...)
{
    int saved = errno;
    char buf[2048], ts[32];
    fmt_clock(now_ns(), ts, sizeof ts);
    int n = snprintf(buf, sizeof buf, "[%s] [%-5s] [%s] %s:%d  ", ts,
                     bench_log_level_name(lvl), role, file, line);
    if (n < 0)
        n = 0;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (m < 0)
        m = 0;
    size_t len = (size_t)n + (size_t)m;
    if (len > sizeof buf - 2)
        len = sizeof buf - 2;
    buf[len++] = '\n';
    if (to_stderr) {
        ssize_t w = write(STDERR_FILENO, buf, len);
        (void)w;
    }
    if (file_fd >= 0) {
        ssize_t w = write(file_fd, buf, len);
        (void)w;
    }
    errno = saved;
}
