/*
 * subcmd.c — non-interactive subcommands for scripts and stress tests (§4.3).
 *
 *   bench info  host           print system info and exit (box on a tty, key=value otherwise)
 *   bench tail  host [-n 16K]  print history and exit
 *   bench watch host [--name N] [--duration S] [--out F]   read-only stream (no raw mode)
 *   bench send  host "reset\n" take -> send -> release; exit 2 if control is refused
 */
#include "client.h"
#include "bench_log.h"
#include "bench_net.h"
#include "bench_util.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint8_t buf[BN_MAX_PAYLOAD];
static int out_bol = 1;   /* did the last byte written to stdout end a line? */

static void out_data(const uint8_t *d, size_t n)
{
    if (!n)
        return;
    fwrite(d, 1, n, stdout);
    out_bol = d[n - 1] == '\n';
}

static int open_session(struct conn *c, struct client_opts *o, uint32_t replay, struct bn_welcome *w)
{
    memset(c, 0, sizeof *c);
    c->fd = -1;
    if (conn_connect(c, o, replay, 3000) < 0) {
        fprintf(stderr, "bench: cannot connect to %s:%u: %s\n", o->host, o->port, strerror(errno));
        return -1;
    }
    int rc = conn_wait_welcome(c, w, 5000);
    if (rc < 0) {
        if (rc == -1)
            fprintf(stderr, "bench: no WELCOME from server\n");
        conn_close(c);
        return -1;
    }
    return 0;
}

/* Receive one packet with a timeout. 1 ok, 0 timeout, -1 closed/error. */
static int recv_timeout(struct conn *c, struct bn_hdr *h, int ms)
{
    struct pollfd p = { .fd = c->fd, .events = POLLIN };
    int r = poll(&p, 1, ms);
    if (r == 0)
        return 0;
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    int perr;
    int rc = conn_recv(c, h, buf, sizeof buf, &perr);
    return rc == 1 ? 1 : -1;
}

static void print_error_pkt(const struct bn_hdr *h)
{
    struct bn_error_hdr e;
    if (h->len < sizeof e)
        return;
    memcpy(&e, buf, sizeof e);
    fprintf(stderr, "bench: server error %s: %.*s\n", bn_err_name(be16toh(e.code)),
            (int)(h->len - sizeof e), (char *)buf + sizeof e);
}

static void print_event(FILE *f, const struct bn_hdr *h, int color)
{
    struct bn_event_hdr e;
    if (h->len < sizeof e)
        return;
    memcpy(&e, buf, sizeof e);
    char hms[16];
    fmt_hms(be64toh(e.ts_ns), hms, sizeof hms);
    if (!out_bol)
        fputc('\n', f);
    out_bol = 1;
    fprintf(f, "%s──── %.*s %s ────%s\n", color ? "\033[2m" : "", (int)(h->len - sizeof e),
            (char *)buf + sizeof e, hms, color ? "\033[0m" : "");
}

int sub_info(struct client_opts *o)
{
    struct conn c;
    struct bn_welcome w;
    if (open_session(&c, o, 0, &w) < 0)
        return 1;
    conn_send(&c, BN_INFO_REQ, NULL, 0);
    struct bn_hdr h;
    uint64_t deadline = mono_ms() + 5000;
    while (mono_ms() < deadline) {
        int rc = recv_timeout(&c, &h, 500);
        if (rc < 0)
            break;
        if (rc == 0 || h.type != BN_INFO)
            continue;
        if (!o->raw && isatty(STDOUT_FILENO)) {
            char lines[64][256];
            int n = info_box_lines((char *)buf, h.len, 80, lines, 64);
            char title[160];
            snprintf(title, sizeof title, "System info: %.100s:%u", o->host, (unsigned)o->port);
            int inner = str_width(title) + 3;           /* box sized to the widest line */
            for (int i = 0; i < n; i++)
                if (str_width(lines[i]) > inner)
                    inner = str_width(lines[i]);
            printf("┌─ %s ", title);
            for (int k = str_width(title) + 3; k < inner + 2; k++)
                fputs("─", stdout);
            puts("┐");
            for (int i = 0; i < n; i++)
                printf("│ %s%*s │\n", lines[i], inner - str_width(lines[i]), "");
            fputs("└", stdout);
            for (int k = 0; k < inner + 2; k++)
                fputs("─", stdout);
            puts("┘");
        } else {
            fwrite(buf, 1, h.len, stdout);
        }
        conn_bye(&c);
        return 0;
    }
    fprintf(stderr, "bench: no INFO reply\n");
    conn_close(&c);
    return 1;
}

int sub_tail(struct client_opts *o)
{
    struct conn c;
    struct bn_welcome w;
    uint64_t n = o->replay_set ? o->replay : 16 * 1024;
    if (open_session(&c, o, (uint32_t)n, &w) < 0)
        return 1;
    struct bn_hdr h;
    int color = isatty(STDOUT_FILENO);
    for (;;) {
        int rc = recv_timeout(&c, &h, 5000);
        if (rc <= 0)
            break;
        if (h.type == BN_DATA && h.len >= sizeof(struct bn_data_hdr))
            out_data(buf + sizeof(struct bn_data_hdr), h.len - sizeof(struct bn_data_hdr));
        else if (h.type == BN_EVENT)
            print_event(stdout, &h, color);
        else if (h.type == BN_REPLAY_END)
            break;
    }
    fflush(stdout);
    conn_bye(&c);
    return 0;
}

int sub_watch(struct client_opts *o)
{
    struct conn c;
    struct bn_welcome w;
    uint64_t n = o->replay_set ? o->replay : 0;
    if (open_session(&c, o, (uint32_t)n, &w) < 0)
        return 1;
    FILE *out = NULL;
    if (o->out) {
        out = fopen(o->out, "wb");
        if (!out) {
            fprintf(stderr, "bench: cannot open %s: %s\n", o->out, strerror(errno));
            return 1;
        }
    }
    int color = isatty(STDOUT_FILENO);
    uint64_t start = mono_ms(), end = o->duration > 0 ? start + (uint64_t)(o->duration * 1000) : UINT64_MAX;
    uint64_t first_off = UINT64_MAX, last_off = 0, bytes = 0, records = 0, dropped = 0, last_ping = start;
    int status = 0;
    struct bn_hdr h;
    for (;;) {
        uint64_t now = mono_ms();
        if (now >= end)
            break;
        if (now - last_ping >= 15000) {
            struct bn_ping pg = { .client_ts_ns = htobe64(now_ns()) };
            conn_send(&c, BN_PING, &pg, sizeof pg);
            last_ping = now;
        }
        uint64_t left = end - now;
        int rc = recv_timeout(&c, &h, left > 1000 ? 1000 : (int)left);
        if (rc < 0) {
            fprintf(stderr, "bench: connection closed by server\n");
            status = 1;
            break;
        }
        if (rc == 0) {
            if (mono_ms() - c.last_rx_ms > 15000) {
                fprintf(stderr, "bench: no data from server for 15 s\n");
                status = 1;
                break;
            }
            continue;
        }
        if (h.type == BN_DATA && h.len >= sizeof(struct bn_data_hdr)) {
            struct bn_data_hdr d;
            memcpy(&d, buf, sizeof d);
            size_t dl = h.len - sizeof d;
            uint64_t off = be64toh(d.offset);
            if (first_off == UINT64_MAX)
                first_off = off;
            last_off = off;
            bytes += dl;
            records++;
            out_data(buf + sizeof d, dl);
            if (out) {
                fwrite(buf + sizeof d, 1, dl, out);
                fflush(out);
            }
        } else if (h.type == BN_EVENT) {
            struct bn_event_hdr e;
            if (h.len >= sizeof e) {
                memcpy(&e, buf, sizeof e);
                if (be16toh(e.code) == EV_DROPPED)
                    dropped++;
                if (first_off == UINT64_MAX)
                    first_off = be64toh(e.offset);
                last_off = be64toh(e.offset);
            }
            print_event(stdout, &h, color);
        } else if (h.type == BN_ERROR) {
            print_error_pkt(&h);
        }
        fflush(stdout);
    }
    conn_bye(&c);
    if (out)
        fclose(out);
    fprintf(stderr, "watch: name=%s records=%llu bytes=%llu first_offset=%llu last_offset=%llu dropped_events=%llu\n",
            o->name, (unsigned long long)records, (unsigned long long)bytes,
            (unsigned long long)(first_off == UINT64_MAX ? 0 : first_off), (unsigned long long)last_off,
            (unsigned long long)dropped);
    return status;
}

int sub_send(struct client_opts *o, const char *text)
{
    uint8_t data[4096];
    size_t n = unescape(text, data, sizeof data);
    struct conn c;
    struct bn_welcome w;
    if (open_session(&c, o, 0, &w) < 0)
        return 1;
    conn_send(&c, BN_TAKE, NULL, 0);
    struct bn_hdr h;
    uint64_t deadline = mono_ms() + 5000;
    int granted = -1;
    while (granted < 0 && mono_ms() < deadline) {
        int rc = recv_timeout(&c, &h, 500);
        if (rc < 0)
            break;
        if (rc == 1 && h.type == BN_CTRL_RESULT && h.len >= sizeof(struct bn_ctrl_result)) {
            struct bn_ctrl_result r;
            memcpy(&r, buf, sizeof r);
            granted = r.granted;
            if (!granted) {
                char holder[33];
                bench_strlcpy(holder, r.holder, sizeof holder);
                uint32_t in = be32toh(r.auto_release_in_sec);
                if (in == UINT32_MAX)
                    fprintf(stderr, "bench: control held by %s (idle %us, no auto-release)\n", holder,
                            be32toh(r.holder_idle_sec));
                else
                    fprintf(stderr, "bench: control held by %s (idle %us, auto-release in %us)\n", holder,
                            be32toh(r.holder_idle_sec), in);
            }
        }
    }
    if (granted != 1) {
        if (granted < 0)
            fprintf(stderr, "bench: no answer to TAKE\n");
        conn_bye(&c);
        return 2;
    }
    for (size_t off = 0; off < n; off += BN_MAX_INPUT) {
        size_t chunk = n - off < BN_MAX_INPUT ? n - off : BN_MAX_INPUT;
        conn_send(&c, BN_INPUT, data + off, (uint32_t)chunk);
    }
    conn_send(&c, BN_RELEASE, NULL, 0);
    int status = 0;
    if (o->wait_ms) {
        uint64_t end = mono_ms() + o->wait_ms;
        while (mono_ms() < end) {
            int rc = recv_timeout(&c, &h, (int)(end - mono_ms()));
            if (rc < 0)
                break;
            if (rc == 1 && h.type == BN_DATA && h.len >= sizeof(struct bn_data_hdr))
                fwrite(buf + sizeof(struct bn_data_hdr), 1, h.len - sizeof(struct bn_data_hdr), stdout);
            else if (rc == 1 && h.type == BN_ERROR) {
                print_error_pkt(&h);
                status = 1;
            }
        }
        fflush(stdout);
    }
    conn_bye(&c);
    fprintf(stderr, "bench: sent %zu bytes\n", n);
    return status;
}
