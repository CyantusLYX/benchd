/*
 * util.c — time, size, string and robustness-switch helpers (see bench_util.h).
 */
#include "bench_util.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

uint32_t bench_unsafe_mask;

uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t mono_ms(void) { return mono_ns() / 1000000ull; }

void sleep_ms(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

char *fmt_clock(uint64_t ns, char *buf, size_t n)
{
    time_t t = (time_t)(ns / 1000000000ull);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, n, "%02d:%02d:%02d.%03u", tm.tm_hour, tm.tm_min, tm.tm_sec,
             (unsigned)((ns / 1000000ull) % 1000));
    return buf;
}

char *fmt_hms(uint64_t ns, char *buf, size_t n)
{
    time_t t = (time_t)(ns / 1000000000ull);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, n, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

int parse_size(const char *s, uint64_t *out)
{
    if (!s || !*s)
        return -1;
    errno = 0;
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || end == s)
        return -1;
    uint64_t mul = 1;
    switch (toupper((unsigned char)*end)) {
    case 0: break;
    case 'B': end++; break;
    case 'K': mul = 1024ull; end++; break;
    case 'M': mul = 1024ull * 1024; end++; break;
    case 'G': mul = 1024ull * 1024 * 1024; end++; break;
    default: return -1;
    }
    if (mul > 1) {                 /* accept 64K, 64KB, 64KiB */
        if (toupper((unsigned char)*end) == 'I')
            end++;
        if (toupper((unsigned char)*end) == 'B')
            end++;
    }
    if (*end)
        return -1;
    *out = (uint64_t)v * mul;
    return 0;
}

char *fmt_size(uint64_t b, char *buf, size_t n)
{
    if (b < 1024)
        snprintf(buf, n, "%lluB", (unsigned long long)b);
    else if (b < 1024ull * 1024)
        snprintf(buf, n, "%.1fK", (double)b / 1024.0);
    else if (b < 1024ull * 1024 * 1024)
        snprintf(buf, n, "%.1fM", (double)b / (1024.0 * 1024.0));
    else
        snprintf(buf, n, "%.1fG", (double)b / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

size_t bench_strlcpy(char *dst, const char *src, size_t n)
{
    size_t len = strlen(src);
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return len;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

size_t unescape(const char *in, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (*in && n < cap) {
        if (*in != '\\' || !in[1]) {
            out[n++] = (uint8_t)*in++;
            continue;
        }
        in++;
        switch (*in) {
        case 'n': out[n++] = '\n'; in++; break;
        case 'r': out[n++] = '\r'; in++; break;
        case 't': out[n++] = '\t'; in++; break;
        case 'e': out[n++] = 0x1b; in++; break;
        case '0': out[n++] = 0; in++; break;
        case '\\': out[n++] = '\\'; in++; break;
        case 'x': {
            int h1 = hexval((unsigned char)in[1]);
            int h2 = h1 >= 0 ? hexval((unsigned char)in[2]) : -1;
            if (h1 < 0) {                    /* "\x" without digits: keep literally */
                out[n++] = 'x';
                in++;
            } else if (h2 < 0) {
                out[n++] = (uint8_t)h1;
                in += 2;
            } else {
                out[n++] = (uint8_t)(h1 * 16 + h2);
                in += 3;
            }
            break;
        }
        default: out[n++] = (uint8_t)*in++; break;
        }
    }
    return n;
}

void sanitize_name(char *s, size_t n)
{
    if (!n)
        return;
    s[n - 1] = 0;
    size_t len = strnlen(s, n);
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] >= 0x7f)
            s[i] = '?';
    /* trim surrounding spaces */
    while (len && s[len - 1] == ' ')
        s[--len] = 0;
    size_t lead = 0;
    while (s[lead] == ' ')
        lead++;
    if (lead)
        memmove(s, s + lead, len - lead + 1);
    if (!s[0])
        bench_strlcpy(s, "anon", n);
}

int parse_hostport(const char *s, char *host, size_t hostsz, uint16_t *port, uint16_t defport)
{
    *port = defport;
    if (!s || !*s)
        return -1;
    const char *colon = NULL;
    if (s[0] == '[') {                       /* [::1]:9000 */
        const char *rb = strchr(s, ']');
        if (!rb)
            return -1;
        size_t l = (size_t)(rb - s - 1);
        if (l + 1 > hostsz)
            return -1;
        memcpy(host, s + 1, l);
        host[l] = 0;
        if (rb[1] == ':')
            colon = rb + 1;
        else if (rb[1])
            return -1;
    } else {
        colon = strrchr(s, ':');
        if (colon && strchr(s, ':') != colon)  /* bare IPv6 without brackets */
            colon = NULL;
        size_t l = colon ? (size_t)(colon - s) : strlen(s);
        if (l + 1 > hostsz || l == 0)
            return -1;
        memcpy(host, s, l);
        host[l] = 0;
    }
    if (colon) {
        char *end;
        long p = strtol(colon + 1, &end, 10);
        if (*end || p <= 0 || p > 65535)
            return -1;
        *port = (uint16_t)p;
    }
    return 0;
}

int hexdump_line(char *out, size_t outsz, const uint8_t *p, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < 16; i++) {
        if (i < n)
            o += (size_t)snprintf(out + o, outsz - o, "%02x ", p[i]);
        else
            o += (size_t)snprintf(out + o, outsz - o, "   ");
        if (i == 7)
            o += (size_t)snprintf(out + o, outsz - o, " ");
        if (o >= outsz)
            return (int)outsz;
    }
    o += (size_t)snprintf(out + o, outsz - o, " |");
    for (size_t i = 0; i < n && o + 2 < outsz; i++)
        out[o++] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '.';
    if (o + 2 < outsz) {
        out[o++] = '|';
        out[o] = 0;
    }
    return (int)o;
}

void hexdump(FILE *fp, const void *data, size_t len, uint64_t base)
{
    const uint8_t *p = data;
    char line[96];
    for (size_t i = 0; i < len; i += 16) {
        hexdump_line(line, sizeof line, p + i, len - i < 16 ? len - i : 16);
        fprintf(fp, "%08llx  %s\n", (unsigned long long)(base + i), line);
    }
}

static const char *robust_names[R_COUNT] = {
    "zombie", "sigpipe", "validate", "readn", "timeout", "ctrl-release", "robust-mutex",
    "dev-reopen", "max-clients", "overrun", "term-restore", "reconnect",
};

const char *bench_robust_name(int r)
{
    return (r >= 0 && r < R_COUNT) ? robust_names[r] : "?";
}

int bench_unsafe_parse(const char *list, uint32_t *mask)
{
    char buf[256];
    bench_strlcpy(buf, list, sizeof buf);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
        if (strcasecmp(tok, "all") == 0) {
            *mask |= (1u << R_COUNT) - 1;
            continue;
        }
        if (strcasecmp(tok, "none") == 0)
            continue;
        int found = -1;
        for (int i = 0; i < R_COUNT; i++) {
            char rn[8];
            snprintf(rn, sizeof rn, "R%d", i + 1);
            if (strcasecmp(tok, robust_names[i]) == 0 || strcasecmp(tok, rn) == 0)
                found = i;
        }
        if (found < 0)
            return -1;
        *mask |= 1u << found;
    }
    return 0;
}

void bench_unsafe_format(uint32_t mask, char *buf, size_t n)
{
    size_t o = 0;
    buf[0] = 0;
    for (int i = 0; i < R_COUNT; i++) {
        if (!(mask & (1u << i)))
            continue;
        int w = snprintf(buf + o, n - o, "%s%s", o ? "," : "", robust_names[i]);
        if (w < 0 || (size_t)w >= n - o)
            break;
        o += (size_t)w;
    }
    if (!o)
        snprintf(buf, n, "(none)");
}
