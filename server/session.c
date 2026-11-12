/*
 * session.c — one forked child per TCP client (§3.5, §5).
 *
 *   handshake  HELLO must arrive within 5 s (R5); version checked; name sanitised; WELCOME.
 *   replay     REPLAY_BEGIN, records from max(tail, head - replay_bytes) (record aligned),
 *              REPLAY_END — or, for a reconnecting client, resume from its last offset.
 *   live loop  poll(socket, 20 ms): handle complete packets, then copy [my_offset, head) out of
 *              the ring under the lock and send it outside the lock. Fell behind further than
 *              the ring holds? -> EVENT DROPPED and jump to tail (R10). STATUS every second.
 *
 * A slow viewer only blocks its own child in write(): fork() isolates clients from each other.
 */
#include "server.h"
#include "bench_log.h"
#include "bench_net.h"
#include "bench_util.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

struct sess {
    int fd;
    int slot;
    pid_t me;
    char name[BN_NAME_LEN];
    char peer[64];
    uint64_t off;            /* next ring offset to send */
    uint64_t last_rx_ms;     /* last packet from the client */
    uint64_t last_status_ms;
    struct bn_status last_status;
};

static uint8_t copybuf[64 * 1024] __attribute__((aligned(8)));
static uint8_t sendbuf[sizeof(struct bn_event_hdr) + REC_MAX_DATA + 8];
static int pipe_wr = -1;

static void finish(struct sess *s, const char *why) __attribute__((noreturn));

static void violation(struct sess *s, int code)
{
    LOGW("protocol violation from %s (%s): %s — disconnecting", s->peer, s->name, bn_err_name(code));
    bn_send_error(s->fd, (uint16_t)code, "protocol error: %s", bn_err_name(code));
    finish(s, "protocol violation");
}

/* Forward a message to the reader. <= PIPE_BUF bytes, so the write is atomic even with many
 * children writing the same pipe at once. */
static void pipe_send(struct sess *s, uint16_t type, const void *data, uint16_t len)
{
    struct pipe_msg m = { .pid = s->me, .type = type, .len = len };
    if (len)
        memcpy(m.data, data, len);
    if (writen(pipe_wr, &m, PIPE_MSG_HDR + len) < 0)
        LOGE("input pipe write: %s", strerror(errno));
}

static void finish(struct sess *s, const char *why)
{
    shm_lock(g_shm);
    int was_ctrl = (g_shm->ctrl_pid == s->me);
    if (s->slot >= 0 && g_shm->slots[s->slot].pid == s->me)
        memset(&g_shm->slots[s->slot], 0, sizeof g_shm->slots[s->slot]);
    slots_recount(g_shm);
    shm_unlock(g_shm);
    /* release goes through the pipe so it is ordered after any input we already forwarded */
    if (was_ctrl && !UNSAFE(R_CTRL_RELEASE))
        pipe_send(s, PIPE_RELEASE, "disconnect", 10);
    LOGI("client %s (%s) left: %s", s->peer, s->name, why);
    close(s->fd);
    _exit(0);
}

/* ---- sending ring records ---- */

static int send_record(struct sess *s, const struct ring_rec *r, const uint8_t *data, uint64_t end_off)
{
    if (r->type == REC_EVENT) {
        struct bn_event_hdr *e = (struct bn_event_hdr *)sendbuf;
        e->offset = htobe64(end_off);
        e->ts_ns = htobe64(r->ts_ns);
        e->code = htobe16(r->code);
        e->pad = 0;
        memcpy(sendbuf + sizeof *e, data, r->len);
        return bn_send(s->fd, BN_EVENT, sendbuf, (uint32_t)(sizeof *e + r->len));
    }
    /* REC_DATA (or, with --unsafe=overrun, whatever garbage we read) */
    struct bn_data_hdr *d = (struct bn_data_hdr *)sendbuf;
    d->offset = htobe64(end_off);
    d->ts_ns = htobe64(r->ts_ns);
    memcpy(sendbuf + sizeof *d, data, r->len);
    return bn_send(s->fd, BN_DATA, sendbuf, (uint32_t)(sizeof *d + r->len));
}

static int send_dropped(struct sess *s, uint64_t dropped, uint64_t new_off)
{
    char text[96], sz[32];
    int n = snprintf(text, sizeof text, "dropped %s", fmt_size(dropped, sz, sizeof sz));
    struct bn_event_hdr *e = (struct bn_event_hdr *)sendbuf;
    e->offset = htobe64(new_off);
    e->ts_ns = htobe64(now_ns());
    e->code = htobe16(EV_DROPPED);
    e->pad = 0;
    memcpy(sendbuf + sizeof *e, text, (size_t)n);
    LOGW("client %s (%s) fell behind: %llu bytes overwritten", s->peer, s->name, (unsigned long long)dropped);
    return bn_send(s->fd, BN_EVENT, sendbuf, (uint32_t)(sizeof *e + (size_t)n));
}

static void fill_status(struct sess *s, struct bn_status *st)
{
    uint64_t now = now_ns();
    memset(st, 0, sizeof *st);
    st->dev_state = (uint8_t)g_shm->dev_state;
    st->you_are_ctrl = g_shm->ctrl_pid == s->me;
    st->baud = htobe32(g_shm->baud);
    st->viewers = htobe32(g_shm->viewers);
    st->ctrl_idle_sec = htobe32(ctrl_idle_sec(g_shm, now));
    st->rx_bytes = htobe64(g_shm->rx_bytes);
    if (g_shm->ctrl_pid)
        bench_strlcpy(st->ctrl_name, g_shm->ctrl_name, sizeof st->ctrl_name);
}

/*
 * Stream ring records [s->off, limit) to the client. The lock is held only while memcpy'ing a
 * chunk out of the ring; network writes happen with the lock released.
 */
static int pump(struct sess *s, uint64_t limit, int want_status)
{
    for (;;) {
        uint64_t dropped = 0;
        struct bn_status st;
        shm_lock(g_shm);
        if (!UNSAFE(R_OVERRUN) && s->off < g_shm->tail) {   /* R10: we were lapped */
            dropped = g_shm->tail - s->off;
            s->off = g_shm->tail;
        }
        if (s->off > g_shm->head)
            s->off = g_shm->head;
        uint64_t end = g_shm->head < limit ? g_shm->head : limit;
        uint64_t start = s->off;
        size_t n = ring_copy(g_shm, &s->off, end, copybuf, sizeof copybuf);
        if (s->slot >= 0)
            g_shm->slots[s->slot].offset = start;
        if (want_status)
            fill_status(s, &st);
        shm_unlock(g_shm);

        if (dropped && send_dropped(s, dropped, start) < 0)
            return -1;

        uint64_t o = start;
        for (size_t i = 0; i < n;) {
            const struct ring_rec *r = (const struct ring_rec *)(copybuf + i);
            uint64_t rs = REC_SIZE(r->len);
            o += rs;
            if (send_record(s, r, copybuf + i + sizeof *r, o) < 0)
                return -1;
            i += (size_t)rs;
        }

        if (want_status) {
            uint64_t now = mono_ms();
            if (now - s->last_status_ms >= 1000 || memcmp(&st, &s->last_status, sizeof st) != 0) {
                /* rx_bytes and idle seconds change constantly: only those -> once a second */
                struct bn_status a = st, b = s->last_status;
                a.rx_bytes = b.rx_bytes = 0;
                a.ctrl_idle_sec = b.ctrl_idle_sec = 0;
                if (now - s->last_status_ms >= 1000 || memcmp(&a, &b, sizeof a) != 0) {
                    if (bn_send(s->fd, BN_STATUS, &st, sizeof st) < 0)
                        return -1;
                    s->last_status = st;
                    s->last_status_ms = now;
                }
            }
            want_status = 0;
        }
        if (n == 0 || s->off >= end)
            return 0;
    }
}

/* ---- handshake ---- */

static void handshake(struct sess *s, int conn_seq)
{
    /* fixed-size stack buffer: with --unsafe=validate a lying header length overflows it (R3) */
    char buf[4096];
    struct bn_hdr h;
    int perr = 0;

    if (!UNSAFE(R_TIMEOUT)) {
        struct pollfd p = { .fd = s->fd, .events = POLLIN };
        uint64_t deadline = mono_ms() + HELLO_TIMEOUT_MS;
        int r;
        do {
            int64_t left = (int64_t)(deadline - mono_ms());
            r = poll(&p, 1, left > 0 ? (int)left : 0);
        } while (r < 0 && errno == EINTR);
        if (r == 0) {
            LOGW("client %s sent no HELLO within %d s — disconnecting", s->peer, HELLO_TIMEOUT_MS / 1000);
            bn_send_error(s->fd, BN_ERR_HANDSHAKE_TIMEOUT, "no HELLO within %d s", HELLO_TIMEOUT_MS / 1000);
            finish(s, "handshake timeout");
        }
    }
    int rc = bn_recv(s->fd, &h, buf, sizeof buf, &perr);
    if (rc == 0)
        finish(s, "closed before HELLO");
    if (rc == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            LOGW("client %s: HELLO incomplete after %d s — disconnecting", s->peer, HELLO_TIMEOUT_MS / 1000);
            bn_send_error(s->fd, BN_ERR_HANDSHAKE_TIMEOUT, "HELLO incomplete");
        }
        finish(s, strerror(errno));
    }
    if (rc == -2)
        violation(s, perr);
    if (h.type != BN_HELLO || h.len < sizeof(struct bn_hello)) {
        if (!UNSAFE(R_VALIDATE))
            violation(s, h.type != BN_HELLO ? BN_ERR_BAD_TYPE : BN_ERR_BAD_LENGTH);
    }
    struct bn_hello hello;
    memcpy(&hello, buf, sizeof hello);
    memcpy(s->name, hello.name, sizeof s->name);
    sanitize_name(s->name, sizeof s->name);
    uint32_t replay = be32toh(hello.replay_bytes);
    uint64_t resume = be64toh(hello.resume_offset);
    uint64_t client_start = be64toh(hello.last_server_start_ns);

    /* handshake done: switch from the 5 s HELLO deadline to the 60 s idle timeout */
    if (!UNSAFE(R_TIMEOUT))
        set_io_timeouts(s->fd, IDLE_TIMEOUT_MS / 1000, IDLE_TIMEOUT_MS / 1000);

    struct bn_welcome w;
    memset(&w, 0, sizeof w);
    uint64_t from, dropped = 0, snap_head;
    int resume_ok = 0;

    shm_lock(g_shm);
    if (s->slot >= 0) {
        struct bench_slot *sl = &g_shm->slots[s->slot];
        sl->pid = s->me;
        sl->joined = 1;
        bench_strlcpy(sl->name, s->name, sizeof sl->name);
        bench_strlcpy(sl->peer, s->peer, sizeof sl->peer);
        sl->joined_ns = now_ns();
    }
    slots_recount(g_shm);
    w.proto_ver = BN_VERSION;
    w.dev_state = (uint8_t)g_shm->dev_state;
    w.baud = htobe32(g_shm->baud);
    w.viewers = htobe32(g_shm->viewers);
    w.history_cap = htobe32((uint32_t)g_shm->cap);
    w.head = htobe64(g_shm->head);
    w.server_start_ns = htobe64(g_shm->server_start_ns);
    bench_strlcpy(w.dev_path, g_shm->dev_path, sizeof w.dev_path);
    if (g_shm->ctrl_pid)
        bench_strlcpy(w.ctrl_name, g_shm->ctrl_name, sizeof w.ctrl_name);
    bench_strlcpy(w.server_version, BENCH_VERSION, sizeof w.server_version);

    snap_head = g_shm->head;
    if (hello.has_resume && client_start == g_shm->server_start_ns && resume <= g_shm->head) {
        resume_ok = 1;
        if (resume < g_shm->tail) {
            dropped = g_shm->tail - resume;
            from = g_shm->tail;
        } else {
            from = ring_seek(g_shm, resume);
        }
    } else {
        uint64_t want = g_shm->head > replay ? g_shm->head - replay : 0;
        if (want < g_shm->tail) {
            dropped = g_shm->tail - want;
            want = g_shm->tail;
        }
        from = ring_seek(g_shm, want);
    }
    uint32_t viewers = g_shm->viewers;
    shm_unlock(g_shm);

    LOGI("client %s (%s) joined; %u viewer(s); %s", s->peer, s->name, viewers,
         resume_ok ? "resuming" : "replaying history");
    if (bn_send(s->fd, BN_WELCOME, &w, sizeof w) < 0)
        finish(s, "send failed");

#ifdef BENCH_FAULT_INJECTION
    if (g_cfg.fault_die_in_lock_at && conn_seq == g_cfg.fault_die_in_lock_at) {
        struct rlimit no_core = { 0, 0 };
        setrlimit(RLIMIT_CORE, &no_core);
        LOGW("BENCH_FAULT=die_in_lock: connection #%d aborts while holding the shared lock", conn_seq);
        shm_lock(g_shm);
        abort();
    }
#else
    (void)conn_seq;
#endif

    s->off = from;
    if (resume_ok) {
        if (dropped && send_dropped(s, dropped, from) < 0)
            finish(s, "send failed");
    } else {
        struct bn_replay_begin rb = { .from_offset = htobe64(from), .dropped_bytes = htobe64(dropped) };
        if (bn_send(s->fd, BN_REPLAY_BEGIN, &rb, sizeof rb) < 0)
            finish(s, "send failed");
        if (pump(s, snap_head, 0) < 0)
            finish(s, "send failed");
        struct bn_replay_end re = { .head = htobe64(s->off) };
        if (bn_send(s->fd, BN_REPLAY_END, &re, sizeof re) < 0)
            finish(s, "send failed");
    }
}

/* ---- commands ---- */

/* Returns 0 to keep going, -1 to end the session. */
static int handle_packet(struct sess *s)
{
    char buf[4096];   /* see handshake(): R3 overflow target when --unsafe=validate */
    struct bn_hdr h;
    int perr = 0;
    int rc = bn_recv(s->fd, &h, buf, sizeof buf, &perr);
    if (rc == 0)
        finish(s, "connection closed");
    if (rc == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            bn_send_error(s->fd, BN_ERR_IDLE_TIMEOUT, "idle timeout");
            finish(s, "receive timeout");
        }
        finish(s, strerror(errno));
    }
    if (rc == -2)
        violation(s, perr);
    s->last_rx_ms = mono_ms();

    switch (h.type) {
    case BN_INPUT: {
        if (h.len > BN_MAX_INPUT) {
            if (!UNSAFE(R_VALIDATE))
                violation(s, BN_ERR_BAD_LENGTH);
            h.len = BN_MAX_INPUT;
        }
        shm_lock(g_shm);
        int is_ctrl = g_shm->ctrl_pid == s->me;
        int dev_up = g_shm->dev_state == DEV_UP;
        if (is_ctrl && dev_up)
            g_shm->ctrl_last_input_ns = now_ns();
        shm_unlock(g_shm);
        if (!is_ctrl)
            return bn_send_error(s->fd, BN_ERR_NOT_CONTROLLER, "you do not hold control");
        if (!dev_up)
            return bn_send_error(s->fd, BN_ERR_DEVICE_LOST, "device is disconnected; input dropped");
        pipe_send(s, PIPE_INPUT, buf, (uint16_t)h.len);
        LOGT("forwarded %u input bytes", h.len);
        return 0;
    }
    case BN_TAKE: {
        struct bn_ctrl_result res;
        shm_lock(g_shm);
        int ok = ctrl_take_locked(g_shm, s->me, s->name, &res);
        shm_unlock(g_shm);
        if (ok)
            LOGI("control granted to %s (%s)", s->name, s->peer);
        else
            LOGI("control denied to %s: held by %s", s->name, res.holder);
        return bn_send(s->fd, BN_CTRL_RESULT, &res, sizeof res);
    }
    case BN_RELEASE: {
        shm_lock(g_shm);
        int is_ctrl = g_shm->ctrl_pid == s->me;
        shm_unlock(g_shm);
        if (is_ctrl)
            pipe_send(s, PIPE_RELEASE, "released", 8);
        return 0;
    }
    case BN_INFO_REQ: {
        static char info[8192];
        size_t n = sysinfo_build(info, sizeof info);
        return bn_send(s->fd, BN_INFO, info, (uint32_t)n);
    }
    case BN_BAUD: {
        uint32_t nb;
        if (h.len != sizeof nb) {
            if (!UNSAFE(R_VALIDATE))
                violation(s, BN_ERR_BAD_LENGTH);
            return 0;
        }
        memcpy(&nb, buf, sizeof nb);
        nb = be32toh(nb);
        shm_lock(g_shm);
        int is_ctrl = g_shm->ctrl_pid == s->me;
        if (is_ctrl)
            g_shm->ctrl_last_input_ns = now_ns();
        shm_unlock(g_shm);
        if (!is_ctrl)
            return bn_send_error(s->fd, BN_ERR_NOT_CONTROLLER, "only the controller may change baud");
        if (!baud_supported(nb))
            return bn_send_error(s->fd, BN_ERR_BAD_REQUEST, "unsupported baud rate %u", nb);
        pipe_send(s, PIPE_BAUD, &nb, sizeof nb);
        return 0;
    }
    case BN_PING: {
        struct bn_pong p = { 0 };
        if (h.len >= sizeof(struct bn_ping))
            memcpy(&p.client_ts_ns, buf, 8);
        p.server_ts_ns = htobe64(now_ns());
        return bn_send(s->fd, BN_PONG, &p, sizeof p);
    }
    case BN_BYE:
        finish(s, "bye");
    default:
        if (!UNSAFE(R_VALIDATE))
            violation(s, BN_ERR_BAD_TYPE);
        LOGD("ignoring unknown packet type 0x%02x", h.type);
        return 0;
    }
}

void session_main(int fd, int slot, int conn_seq, const char *peer)
{
    struct sess s = { .fd = fd, .slot = slot, .me = getpid() };
    bench_strlcpy(s.peer, peer, sizeof s.peer);
    bench_strlcpy(s.name, "?", sizeof s.name);

    bench_log_set_role("child:%d", (int)s.me);
    prctl(PR_SET_NAME, "benchd-child");
    prctl(PR_SET_PDEATHSIG, SIGTERM);          /* master gone -> we go too */
    if (getppid() != g_shm->master_pid)
        _exit(0);
    signal(SIGINT, SIG_IGN);                   /* shutdown is coordinated by the master */
    signal(SIGUSR1, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    close(g_cfg.listen_fd);
    close(g_cfg.input_pipe[0]);
    pipe_wr = g_cfg.input_pipe[1];

    if (!UNSAFE(R_TIMEOUT)) {
        set_keepalive(fd, 30, 10, 3);
        set_io_timeouts(fd, HELLO_TIMEOUT_MS / 1000, IDLE_TIMEOUT_MS / 1000);
    }
    LOGD("child for %s (slot %d, conn #%d)", peer, slot, conn_seq);

    handshake(&s, conn_seq);
    s.last_rx_ms = mono_ms();

    for (;;) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, 20);
        if (r < 0 && errno != EINTR)
            finish(&s, strerror(errno));
        /* handle every complete packet that is waiting (bounded so streaming keeps going) */
        for (int i = 0; r > 0 && i < 32; i++) {
            if (handle_packet(&s) < 0)
                finish(&s, "send failed");
            r = poll(&p, 1, 0);
        }
        if (!UNSAFE(R_TIMEOUT) && mono_ms() - s.last_rx_ms > IDLE_TIMEOUT_MS) {
            bn_send_error(fd, BN_ERR_IDLE_TIMEOUT, "no packet for %d s", IDLE_TIMEOUT_MS / 1000);
            LOGW("client %s (%s) idle for %d s — disconnecting", s.peer, s.name, IDLE_TIMEOUT_MS / 1000);
            finish(&s, "idle timeout");
        }
        if (pump(&s, UINT64_MAX, 1) < 0)
            finish(&s, errno == EAGAIN ? "send timeout (client not reading)" : strerror(errno));
    }
}
