/*
 * session.c — client connection layer and the interactive main loop (§5.1, §5.6).
 *
 * One process, one poll() loop over stdin and the socket (no threads):
 *   stdin   -> cmd_keys()  (escape commands, or controller input batched 5 ms / 64 bytes)
 *   socket  -> packets     (DATA/EVENT drawn in order, STATUS updates the status bar, ...)
 *   timers  -> flush input, PING every 15 s, 15 s receive timeout, reconnect back-off,
 *              redraw the status bar once a second (idle counters, countdowns)
 *
 * Reconnect (R12): on EOF/error/timeout the bar turns yellow and we retry after 1,2,4,8,16,30,30…
 * seconds. HELLO then carries resume_offset = last offset seen + the server start time we
 * knew, so the server continues seamlessly, reports DROPPED, or (if it restarted) replays.
 */
#include "client.h"
#include "bench_log.h"
#include "bench_net.h"
#include "bench_util.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---------- connection layer (shared with subcommands) ---------- */

int conn_connect(struct conn *c, const struct client_opts *o, uint32_t replay, int timeout_ms)
{
    c->fd = tcp_connect_timeout(o->host, o->port, timeout_ms);
    if (c->fd < 0)
        return -1;
    struct bn_hello h;
    memset(&h, 0, sizeof h);
    bench_strlcpy(h.name, o->name, sizeof h.name);
    h.replay_bytes = htobe32(replay);
    if (c->have_pos && c->have_start) {
        h.has_resume = 1;
        h.resume_offset = htobe64(c->pos);
        h.last_server_start_ns = htobe64(c->server_start_ns);
    }
    c->last_rx_ms = c->last_ping_ms = mono_ms();
    if (bn_send(c->fd, BN_HELLO, &h, sizeof h) < 0) {
        conn_close(c);
        return -1;
    }
    LOGD("connected to %s:%u, HELLO sent (replay %u, resume %s)", o->host, o->port, replay,
         h.has_resume ? "yes" : "no");
    return 0;
}

int conn_recv(struct conn *c, struct bn_hdr *h, uint8_t *buf, size_t cap, int *perr)
{
    int rc = bn_recv(c->fd, h, buf, cap, perr);
    if (rc == 1)
        c->last_rx_ms = mono_ms();
    return rc;
}

int conn_send(struct conn *c, uint8_t type, const void *p, uint32_t len)
{
    return bn_send(c->fd, type, p, len);
}

/* Orderly goodbye: BYE, half-close, drain until the server closes. Closing a socket that still
 * has unread data makes the kernel send RST instead of FIN — we want a clean FIN exchange. */
void conn_bye(struct conn *c)
{
    if (c->fd < 0)
        return;
    bn_send(c->fd, BN_BYE, NULL, 0);
    shutdown(c->fd, SHUT_WR);
    uint64_t end = mono_ms() + 1000;
    char tmp[4096];
    while (mono_ms() < end) {
        struct pollfd p = { .fd = c->fd, .events = POLLIN };
        if (poll(&p, 1, 100) <= 0)
            continue;
        if (read(c->fd, tmp, sizeof tmp) <= 0)
            break;
    }
    conn_close(c);
}

void conn_close(struct conn *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

int conn_wait_welcome(struct conn *c, struct bn_welcome *w, int timeout_ms)
{
    static uint8_t buf[BN_MAX_PAYLOAD];
    struct pollfd p = { .fd = c->fd, .events = POLLIN };
    if (poll(&p, 1, timeout_ms) <= 0)
        return -1;
    struct bn_hdr h;
    int perr;
    if (conn_recv(c, &h, buf, sizeof buf, &perr) != 1)
        return -1;
    if (h.type == BN_ERROR && h.len >= sizeof(struct bn_error_hdr)) {
        struct bn_error_hdr e;
        memcpy(&e, buf, sizeof e);
        fprintf(stderr, "bench: server error %s: %.*s\n", bn_err_name(be16toh(e.code)),
                (int)(h.len - sizeof e), (char *)buf + sizeof e);
        errno = ECONNREFUSED;
        return -2;
    }
    if (h.type != BN_WELCOME || h.len < sizeof *w)
        return -1;
    memcpy(w, buf, sizeof *w);
    c->server_start_ns = be64toh(w->server_start_ns);
    c->have_start = 1;
    return 0;
}

/* ---------- interactive ---------- */

static const unsigned backoff_s[] = { 1, 2, 4, 8, 16, 30 };
static volatile sig_atomic_t got_winch;
static void on_winch(int sig) { (void)sig; got_winch = 1; }

static void app_disconnect(struct app *a, const char *why)
{
    int was_live = a->st == ST_LIVE;
    conn_close(&a->c);
    a->st = ST_DISCONNECTED;
    a->is_ctrl = 0;
    a->ilen = 0;
    LOGI("disconnected: %s", why);
    if (!a->o.reconnect || UNSAFE(R_RECONNECT)) {
        /* naive client (R12 unsafe) or --no-reconnect: just give up */
        a->quit = 2;
        ui_note(a, "connection lost: %s", why);
        return;
    }
    unsigned d = backoff_s[a->backoff_idx < 5 ? a->backoff_idx : 5];
    if (a->backoff_idx < 100)
        a->backoff_idx++;
    a->retry_at = mono_ms() + d * 1000ull;
    if (was_live)
        ui_flash(a, FL_WARN, "connection lost (%s) — reconnecting", why);
    ui_status(a);
}

int app_send(struct app *a, uint8_t type, const void *p, uint32_t len)
{
    if (a->st == ST_DISCONNECTED)
        return -1;
    if (conn_send(&a->c, type, p, len) < 0) {
        app_disconnect(a, strerror(errno));
        return -1;
    }
    return 0;
}

void app_flush_input(struct app *a)
{
    if (!a->ilen)
        return;
    if (a->st == ST_LIVE)
        app_send(a, BN_INPUT, a->ibuf, (uint32_t)a->ilen);
    a->ilen = 0;
}

static void try_connect(struct app *a)
{
    ui_status(a);
    ob_flush();
    if (conn_connect(&a->c, &a->o, (uint32_t)a->o.replay, 2000) < 0) {
        int e = errno;
        if (!a->ever_welcomed && !a->o.reconnect) {
            a->quit = 2;
            fprintf(stderr, "bench: cannot connect to %s:%u: %s\n", a->o.host, a->o.port, strerror(e));
            return;
        }
        a->st = ST_HANDSHAKE;   /* so app_disconnect schedules the back-off */
        app_disconnect(a, strerror(e));
        return;
    }
    a->st = ST_HANDSHAKE;
}

static void handle_welcome(struct app *a, const uint8_t *buf, uint32_t len)
{
    struct bn_welcome w;
    if (len < sizeof w)
        return;
    memcpy(&w, buf, sizeof w);
    uint64_t start = be64toh(w.server_start_ns);
    int restarted = a->ever_welcomed && a->c.have_start && start != a->c.server_start_ns;
    if (a->ever_welcomed)
        ui_separator(a, restarted ? "server restarted" : "reconnected");
    a->c.server_start_ns = start;
    a->c.have_start = 1;
    if (restarted)
        a->c.have_pos = 0;
    a->dev_state = w.dev_state;
    a->baud = be32toh(w.baud);
    a->viewers = be32toh(w.viewers);
    bench_strlcpy(a->dev_path, w.dev_path, sizeof a->dev_path);
    bench_strlcpy(a->ctrl_name, w.ctrl_name, sizeof a->ctrl_name);
    a->st = ST_LIVE;
    a->backoff_idx = 0;
    a->ever_welcomed = 1;
    LOGI("welcome: server %s, device %s @ %u, %u viewers", w.server_version, w.dev_path, a->baud, a->viewers);
}

static void handle_packet(struct app *a, const struct bn_hdr *h, const uint8_t *buf)
{
    switch (h->type) {
    case BN_WELCOME:
        handle_welcome(a, buf, h->len);
        break;
    case BN_REPLAY_BEGIN:
        a->in_replay = 1;
        if (h->len >= sizeof(struct bn_replay_begin)) {
            struct bn_replay_begin rb;
            memcpy(&rb, buf, sizeof rb);
            a->c.pos = be64toh(rb.from_offset);
            a->c.have_pos = 1;
        }
        break;
    case BN_REPLAY_END:
        a->in_replay = 0;
        if (h->len >= sizeof(struct bn_replay_end)) {
            struct bn_replay_end re;
            memcpy(&re, buf, sizeof re);
            a->c.pos = be64toh(re.head);
            a->c.have_pos = 1;
        }
        ui_separator(a, "replay end · live");
        break;
    case BN_DATA: {
        if (h->len < sizeof(struct bn_data_hdr))
            break;
        struct bn_data_hdr d;
        memcpy(&d, buf, sizeof d);
        a->c.pos = be64toh(d.offset);
        a->c.have_pos = 1;
        const uint8_t *data = buf + sizeof d;
        size_t n = h->len - sizeof d;
        ui_data(a, data, n, be64toh(d.ts_ns));
        if (a->logf)
            fwrite(data, 1, n, a->logf);
        break;
    }
    case BN_EVENT: {
        if (h->len < sizeof(struct bn_event_hdr))
            break;
        struct bn_event_hdr e;
        memcpy(&e, buf, sizeof e);
        a->c.pos = be64toh(e.offset);
        a->c.have_pos = 1;
        char text[300];
        size_t tl = h->len - sizeof e;
        if (tl >= sizeof text)
            tl = sizeof text - 1;
        memcpy(text, buf + sizeof e, tl);
        text[tl] = 0;
        int code = be16toh(e.code);
        ui_event(a, code, text, be64toh(e.ts_ns));
        if (code == EV_DEVICE_LOST)
            a->dev_state = DEV_LOST;
        else if (code == EV_DEVICE_BACK)
            a->dev_state = DEV_UP;
        break;
    }
    case BN_CTRL_RESULT: {
        struct bn_ctrl_result r;
        if (h->len < sizeof r)
            break;
        memcpy(&r, buf, sizeof r);
        char holder[33];
        bench_strlcpy(holder, r.holder, sizeof holder);
        if (r.granted) {
            a->is_ctrl = 1;
            bench_strlcpy(a->ctrl_name, holder, sizeof a->ctrl_name);
            ui_flash(a, FL_OK, "you have control — Ctrl-] r to release");
        } else {
            uint32_t idle = be32toh(r.holder_idle_sec), in = be32toh(r.auto_release_in_sec);
            if (in == UINT32_MAX)
                ui_flash(a, FL_ERR, "held by %s (idle %us, no auto-release)", holder, idle);
            else
                ui_flash(a, FL_ERR, "held by %s (idle %us, auto-release in %us)", holder, idle, in);
        }
        break;
    }
    case BN_INFO:
        if (a->info_pending)
            ui_info_box(a, (const char *)buf, h->len);
        a->info_pending = 0;
        break;
    case BN_STATUS: {
        struct bn_status s;
        if (h->len < sizeof s)
            break;
        memcpy(&s, buf, sizeof s);
        a->dev_state = s.dev_state;
        int was_ctrl = a->is_ctrl;
        a->is_ctrl = s.you_are_ctrl;
        a->baud = be32toh(s.baud);
        a->viewers = be32toh(s.viewers);
        a->ctrl_idle = be32toh(s.ctrl_idle_sec);
        a->rx_bytes = be64toh(s.rx_bytes);
        bench_strlcpy(a->ctrl_name, s.ctrl_name, sizeof a->ctrl_name);
        if (was_ctrl && !a->is_ctrl)
            a->ilen = 0;
        ui_status(a);
        break;
    }
    case BN_ERROR: {
        struct bn_error_hdr e;
        if (h->len < sizeof e)
            break;
        memcpy(&e, buf, sizeof e);
        int code = be16toh(e.code);
        ui_flash(a, code == BN_ERR_NOT_CONTROLLER ? FL_WARN : FL_ERR, "server: %.*s",
                 (int)(h->len - sizeof e), (const char *)buf + sizeof e);
        LOGW("server error %s", bn_err_name(code));
        break;
    }
    case BN_PONG:
        break;
    default:
        LOGD("ignoring packet type 0x%02x", h->type);
    }
}

int app_run(struct app *a)
{
    static uint8_t buf[BN_MAX_PAYLOAD];
    a->c.fd = -1;
    a->st = ST_DISCONNECTED;
    a->retry_at = 0;
    a->dev_state = DEV_UP;
    bench_strlcpy(a->dev_path, "?", sizeof a->dev_path);

    if (term_init() < 0) {
        fprintf(stderr, "bench: stdin is not a terminal (use `bench watch` for scripts)\n");
        return 1;
    }
    struct sigaction sa = { .sa_handler = on_winch, .sa_flags = SA_RESTART };
    sigaction(SIGWINCH, &sa, NULL);
    ui_init(a);
    ob_flush();

    while (!a->quit) {
        uint64_t now = mono_ms();
        if (a->st == ST_DISCONNECTED && now >= a->retry_at) {
            try_connect(a);
            if (a->quit)
                break;
            now = mono_ms();
        }

        int timeout = 250;
        if (a->ilen) {
            int64_t d = (int64_t)(a->iflush_at - now);
            timeout = d < 0 ? 0 : (int)d;
        }
        if (a->st == ST_DISCONNECTED) {
            int64_t d = (int64_t)(a->retry_at - now);
            if (d < timeout)
                timeout = d < 0 ? 0 : (int)d;
        }
        struct pollfd p[2] = { { .fd = STDIN_FILENO, .events = POLLIN },
                               { .fd = a->c.fd, .events = POLLIN } };
        int nfds = a->st != ST_DISCONNECTED ? 2 : 1;
        int r = poll(p, (nfds_t)nfds, timeout);
        if (r < 0 && errno != EINTR)
            break;

        if (got_winch) {
            got_winch = 0;
            ui_resize(a);
        }
        if (r > 0 && (p[0].revents & (POLLIN | POLLHUP))) {
            uint8_t k[256];
            ssize_t n = read(STDIN_FILENO, k, sizeof k);
            if (n <= 0)
                break;
            cmd_keys(a, k, (size_t)n);
        }
        if (r > 0 && nfds == 2 && a->st != ST_DISCONNECTED && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            for (int i = 0; i < 256 && a->st != ST_DISCONNECTED; i++) {
                struct bn_hdr h;
                int perr = 0;
                int rc = conn_recv(&a->c, &h, buf, sizeof buf, &perr);
                if (rc == 1) {
                    handle_packet(a, &h, buf);
                } else {
                    app_disconnect(a, rc == 0 ? "server closed the connection" :
                                      rc == -2 ? bn_err_name(perr) : strerror(errno));
                    break;
                }
                struct pollfd q = { .fd = a->c.fd, .events = POLLIN };
                if (poll(&q, 1, 0) <= 0)
                    break;
            }
        }

        now = mono_ms();
        if (a->ilen && now >= a->iflush_at)
            app_flush_input(a);
        if (a->st != ST_DISCONNECTED) {
            if (now - a->c.last_rx_ms > 15000)
                app_disconnect(a, "no data from server for 15 s");
            else if (now - a->c.last_ping_ms >= 15000) {
                struct bn_ping pg = { .client_ts_ns = htobe64(now_ns()) };
                a->c.last_ping_ms = now;
                app_send(a, BN_PING, &pg, sizeof pg);
            }
        }
        if (now - a->last_status_draw >= 1000 || (a->flash[0] && now >= a->flash_until)) {
            if (a->flash[0] && now >= a->flash_until)
                a->flash[0] = 0;
            ui_status(a);
        }
        ob_flush();
    }

    if (a->st != ST_DISCONNECTED) {
        app_flush_input(a);
        conn_bye(&a->c);
    }
    if (a->logf)
        fclose(a->logf);
    term_restore();
    return a->quit == 2 ? 1 : 0;
}
