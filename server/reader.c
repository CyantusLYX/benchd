/*
 * reader.c — the device reader process: the ONLY process that holds the serial fd (§3.2).
 *
 * Main loop (poll on tty + input pipe, 100 ms tick):
 *   tty readable   -> read() up to 1024 bytes -> append one DATA record to the ring.
 *   read() = 0/EIO -> device unplugged: close, DEV_LOST event, retry open() every 500 ms (R8).
 *   pipe readable  -> {pid, type, data} from a child. Re-check pid == ctrl_pid (control may have
 *                     been released since the child forwarded it) and only then write the tty.
 *                     BAUD and RELEASE travel the same pipe so they stay ordered with INPUT.
 *   every second   -> release control if the controller has been idle too long.
 *
 * Why one process: two processes reading the same tty would steal bytes from each other.
 */
#include "server.h"
#include "bench_log.h"
#include "bench_net.h"
#include "bench_util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <termios.h>
#include <unistd.h>

static int tty_fd = -1;
static struct termios orig_tio;
static int have_orig;
static volatile sig_atomic_t reader_stop;

/* SIGTERM: finish the loop and restore the tty. If we are stuck (e.g. blocked on a lock whose
 * owner died with --unsafe=robust-mutex) SIGALRM's default action ends us 2 s later. */
static void on_term(int sig)
{
    (void)sig;
    reader_stop = 1;
    alarm(2);
}

static const struct { uint32_t baud; speed_t sp; } baud_table[] = {
    { 1200, B1200 }, { 2400, B2400 }, { 4800, B4800 }, { 9600, B9600 }, { 19200, B19200 },
    { 38400, B38400 }, { 57600, B57600 }, { 115200, B115200 }, { 230400, B230400 },
    { 460800, B460800 }, { 921600, B921600 }, { 1500000, B1500000 }, { 3000000, B3000000 },
};

static speed_t speed_of(uint32_t baud)
{
    for (size_t i = 0; i < sizeof baud_table / sizeof baud_table[0]; i++)
        if (baud_table[i].baud == baud)
            return baud_table[i].sp;
    return 0;
}

int baud_supported(uint32_t baud) { return speed_of(baud) != 0; }

static int apply_termios(int fd, uint32_t baud)
{
    struct termios t;
    if (tcgetattr(fd, &t) < 0)
        return -1;
    if (!have_orig) {
        orig_tio = t;
        have_orig = 1;
    }
    cfmakeraw(&t);                                    /* raw, 8 data bits */
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(tcflag_t)(CSTOPB | PARENB | CRTSCTS);  /* 8N1, no flow control */
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    speed_t sp = speed_of(baud);
    if (sp) {
        cfsetispeed(&t, sp);
        cfsetospeed(&t, sp);
    }
    return tcsetattr(fd, TCSANOW, &t);
}

static int dev_open(void)
{
    int fd = open(g_cfg.device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    shm_lock(g_shm);
    uint32_t baud = g_shm->baud;
    shm_unlock(g_shm);
    if (apply_termios(fd, baud) < 0)
        LOGW("tcsetattr %s: %s (not a tty?)", g_cfg.device, strerror(errno));
    return fd;
}

static void dev_lost(const char *why)
{
    close(tty_fd);
    tty_fd = -1;
    shm_lock(g_shm);
    g_shm->dev_state = DEV_LOST;
    ring_event(g_shm, EV_DEVICE_LOST, "device lost");
    shm_unlock(g_shm);
    LOGW("device %s lost (%s); retrying every 500 ms", g_cfg.device, why);
}

static void dev_back(int fd)
{
    tty_fd = fd;
    shm_lock(g_shm);
    g_shm->dev_state = DEV_UP;
    g_shm->dev_reconnects++;
    ring_event(g_shm, EV_DEVICE_BACK, "device reconnected");
    shm_unlock(g_shm);
    LOGI("device %s is back", g_cfg.device);
}

/* Write to the (non-blocking) tty, waiting briefly when its output queue is full. */
static ssize_t tty_write_all(const uint8_t *p, size_t n)
{
    size_t done = 0;
    int spins = 0;
    while (done < n) {
        ssize_t w = write(tty_fd, p + done, n - done);
        if (w > 0) {
            done += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && spins++ < 20) {
            struct pollfd pf = { .fd = tty_fd, .events = POLLOUT };
            poll(&pf, 1, 50);
            continue;
        }
        return done ? (ssize_t)done : -1;
    }
    return (ssize_t)done;
}

static void handle_pipe_msg(struct pipe_msg *m)
{
    struct ctrl_note note = { 0 };
    shm_lock(g_shm);
    int is_ctrl = (m->pid == g_shm->ctrl_pid);   /* second check: control may have moved */
    char who[BN_NAME_LEN];
    bench_strlcpy(who, g_shm->ctrl_name, sizeof who);
    shm_unlock(g_shm);

    switch (m->type) {
    case PIPE_INPUT:
        if (!is_ctrl) {
            LOGD("dropping %u stale input bytes from pid %d (not controller)", m->len, (int)m->pid);
            return;
        }
        if (tty_fd < 0) {
            LOGD("dropping %u input bytes: device lost", m->len);
            return;
        }
        {
            ssize_t w = tty_write_all(m->data, m->len);
            if (w > 0) {
                shm_lock(g_shm);
                g_shm->tx_bytes += (uint64_t)w;
                shm_unlock(g_shm);
            }
            LOGT("tty <- %zd bytes from %s", w, who);
        }
        return;
    case PIPE_BAUD: {
        uint32_t nb;
        if (!is_ctrl || m->len != sizeof nb)
            return;
        memcpy(&nb, m->data, sizeof nb);
        if (tty_fd >= 0 && apply_termios(tty_fd, nb) < 0) {
            LOGW("set baud %u failed: %s", nb, strerror(errno));
            return;
        }
        shm_lock(g_shm);
        uint32_t old = g_shm->baud;
        g_shm->baud = nb;
        ring_event(g_shm, EV_BAUD_CHANGED, "baud %u -> %u by %s", old, nb, who);
        shm_unlock(g_shm);
        LOGI("baud %u -> %u by %s", old, nb, who);
        return;
    }
    case PIPE_RELEASE: {
        char reason[32] = "released";
        if (m->len && m->len < sizeof reason) {
            memcpy(reason, m->data, m->len);
            reason[m->len] = 0;
        }
        shm_lock(g_shm);
        if (m->pid == g_shm->ctrl_pid)
            ctrl_release_locked(g_shm, reason, &note);
        shm_unlock(g_shm);
        ctrl_log_note(&note);
        return;
    }
    default:
        LOGW("unknown pipe message type %u", m->type);
    }
}

void reader_main(void)
{
    bench_log_set_role("reader");
    prctl(PR_SET_NAME, "benchd-reader");
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != g_shm->master_pid)
        _exit(0);
    signal(SIGINT, SIG_IGN);           /* terminal Ctrl-C is the master's business */
    signal(SIGUSR1, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    close(g_cfg.listen_fd);
    close(g_cfg.input_pipe[1]);
    int pipe_rd = g_cfg.input_pipe[0];

    tty_fd = dev_open();
    if (tty_fd < 0) {
        LOGW("cannot open %s: %s — waiting for the device", g_cfg.device, strerror(errno));
        shm_lock(g_shm);
        g_shm->dev_state = DEV_LOST;
        shm_unlock(g_shm);
    } else {
        shm_lock(g_shm);
        g_shm->dev_state = DEV_UP;
        shm_unlock(g_shm);
        LOGI("opened %s", g_cfg.device);
    }

    uint64_t next_reopen = 0, next_idle = mono_ms() + 1000;
    uint8_t buf[REC_MAX_DATA];

    while (!reader_stop) {
        struct pollfd p[2] = { { .fd = pipe_rd, .events = POLLIN }, { .fd = tty_fd, .events = POLLIN } };
        int n = poll(p, tty_fd >= 0 ? 2 : 1, 100);
        if (n < 0 && errno != EINTR) {
            LOGE("poll: %s", strerror(errno));
            break;
        }

        if (n > 0 && tty_fd >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t r = read(tty_fd, buf, sizeof buf);
            if (r > 0) {
                shm_lock(g_shm);
                ring_append(g_shm, REC_DATA, 0, buf, (uint32_t)r);
                g_shm->rx_bytes += (uint64_t)r;
                g_shm->last_rx_ns = now_ns();
                shm_unlock(g_shm);
                LOGT("ring append %zd bytes from tty", r);
            } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                if (UNSAFE(R_DEV_REOPEN)) {
                    /* naive: "nothing to read, try again" — spins at 100% CPU forever */
                    continue;
                }
                dev_lost(r == 0 ? "EOF" : strerror(errno));
                next_reopen = mono_ms() + 500;
            }
        }

        if (n > 0 && (p[0].revents & POLLIN)) {
            struct pipe_msg m;
            ssize_t r = readn(pipe_rd, &m, PIPE_MSG_HDR);
            if (r == PIPE_MSG_HDR && m.len <= BN_MAX_INPUT &&
                (m.len == 0 || readn(pipe_rd, m.data, m.len) == m.len))
                handle_pipe_msg(&m);
            else if (r <= 0)
                LOGE("input pipe closed");
        }

        uint64_t now = mono_ms();
        if (tty_fd < 0 && !UNSAFE(R_DEV_REOPEN) && now >= next_reopen) {
            int fd = dev_open();
            if (fd >= 0)
                dev_back(fd);
            next_reopen = now + 500;
        }
        if (now >= next_idle) {
            struct ctrl_note note = { 0 };
            shm_lock(g_shm);
            ctrl_check_idle_locked(g_shm, now_ns(), &note);
            shm_unlock(g_shm);
            ctrl_log_note(&note);
            next_idle = now + 1000;
        }
    }

    if (tty_fd >= 0 && have_orig)
        tcsetattr(tty_fd, TCSANOW, &orig_tio);   /* leave the port as we found it */
    LOGI("reader exiting (tty settings restored)");
    _exit(0);
}
