/*
 * main.c — benchd master process.
 *
 * Startup: parse options, build the shared memory (ring + state + robust mutex) and the input
 * pipe BEFORE any fork() so every process inherits them, fork the reader, listen.
 *
 * Main loop (poll on the listening socket, 200 ms tick):
 *   - accept() (retried on EINTR); over --max-clients -> ERROR SERVER_BUSY and close (R9);
 *     otherwise fork() a child that runs session_main().
 *   - SIGCHLD only sets a flag; the loop reaps with waitpid(-1, WNOHANG) (R1). A reaped child
 *     that held control releases it (R6); a dead reader is restarted.
 *   - SIGUSR1 cycles the shared log level INFO -> DEBUG -> TRACE -> ERROR -> WARN -> INFO.
 *   - SIGINT/SIGTERM: write SERVER_SHUTDOWN into the ring, give children up to 1 s to deliver
 *     it, SIGTERM everyone, reap, exit.
 */
#include "server.h"
#include "bench_log.h"
#include "bench_net.h"
#include "bench_util.h"

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct bench_shm *g_shm;
struct server_cfg g_cfg;

static volatile sig_atomic_t got_chld, got_usr1, got_term;
static pid_t reader_pid;
static size_t shm_size;

static void on_chld(int sig) { (void)sig; got_chld = 1; }
static void on_usr1(int sig) { (void)sig; got_usr1 = 1; }
static void on_term(int sig) { got_term = sig; }

static void usage(FILE *f)
{
    fprintf(f,
        "usage: benchd -d <device> [-b 115200] [-p 9000] [--history 64K] [--max-clients 32]\n"
        "              [--idle-release 120] [-v...] [-l <logfile>] [--unsafe <list>]\n"
        "  -d, --device PATH       serial device (e.g. /dev/ttyUSB0 or /tmp/benchsim0)\n"
        "  -b, --baud N            baud rate, default 115200 (8N1)\n"
        "  -p, --port N            TCP port, default 9000\n"
        "      --history SIZE      history ring size (K/M suffix), default 64K\n"
        "      --max-clients N     concurrent clients, default 32\n"
        "      --idle-release SEC  auto-release control after SEC idle seconds, default 120 (0=never)\n"
        "  -v                      more logging (repeatable); env BENCH_LOG=debug also works\n"
        "  -l, --log FILE          also append the log to FILE\n"
        "      --unsafe LIST       disable robustness mechanisms (comma list or 'all'):\n"
        "                          zombie sigpipe validate readn timeout ctrl-release robust-mutex\n"
        "                          dev-reopen max-clients overrun\n"
        "  -h, --help\n"
        "signals: SIGUSR1 cycles the log level at run time; SIGINT/SIGTERM shut down cleanly.\n");
}

static void spawn_reader(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        LOGE("fork reader: %s", strerror(errno));
        exit(1);
    }
    if (pid == 0)
        reader_main();
    reader_pid = pid;
    shm_lock(g_shm);
    g_shm->reader_pid = pid;
    shm_unlock(g_shm);
    LOGI("reader process %d started", (int)pid);
}

static void reap_children(void)
{
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        if (pid == reader_pid) {
            LOGE("reader %d died (%s %d)%s", (int)pid, WIFSIGNALED(st) ? "signal" : "status",
                 WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st), got_term ? "" : " — restarting");
            reader_pid = 0;
            if (!got_term)
                spawn_reader();
            continue;
        }
        struct ctrl_note note = { 0 };
        shm_lock(g_shm);
        if (!UNSAFE(R_CTRL_RELEASE) && g_shm->ctrl_pid == pid)
            ctrl_release_locked(g_shm, "disconnect", &note);   /* controller crashed (R6) */
        for (int i = 0; i < BENCH_MAX_SLOTS; i++)
            if (g_shm->slots[i].pid == pid)
                memset(&g_shm->slots[i], 0, sizeof g_shm->slots[i]);
        slots_recount(g_shm);
        shm_unlock(g_shm);
        ctrl_log_note(&note);
        if (WIFSIGNALED(st))
            LOGW("child %d killed by signal %d (%s)", (int)pid, WTERMSIG(st), strsignal(WTERMSIG(st)));
        else
            LOGD("child %d exited with %d", (int)pid, WEXITSTATUS(st));
    }
}

/* Server full. Peek at the client's HELLO (if it arrives within 100 ms) so the log can name who
 * was turned away, then send ERROR SERVER_BUSY.
 * R2: if that client already reset the connection, our recv() reports ECONNRESET and the
 * following write() raises SIGPIPE — which kills the whole master unless SIGPIPE is ignored. */
static void reject_busy(int fd, const char *peer, int in_use)
{
    char name[BN_NAME_LEN] = "?";
    struct pollfd p = { .fd = fd, .events = POLLIN };
    if (poll(&p, 1, 100) > 0) {
        uint8_t tmp[BN_HDR_LEN + sizeof(struct bn_hello)];
        ssize_t n = recv(fd, tmp, sizeof tmp, MSG_DONTWAIT);
        if (n >= (ssize_t)(BN_HDR_LEN + BN_NAME_LEN)) {
            memcpy(name, tmp + BN_HDR_LEN, BN_NAME_LEN);
            sanitize_name(name, sizeof name);
        } else if (n < 0) {
            LOGD("busy-reject %s: recv: %s", peer, strerror(errno));
        }
    }
    if (bn_send_error(fd, BN_ERR_SERVER_BUSY, "server busy (%d/%u clients)", in_use, g_cfg.max_clients) < 0)
        LOGW("busy reply to %s failed: %s (ignored)", peer, strerror(errno));
    LOGW("rejected %s (%s): server full (%d/%u)", peer, name, in_use, g_cfg.max_clients);
    close(fd);
}

static void handle_accept(int *conn_seq)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    int fd;
    do {
        fd = accept(g_cfg.listen_fd, (struct sockaddr *)&ss, &sl);   /* EINTR: just retry */
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        if (errno != EAGAIN && errno != ECONNABORTED)
            LOGW("accept: %s", strerror(errno));
        if (errno == EMFILE || errno == ENFILE)
            sleep_ms(100);
        return;
    }
    char peer[64];
    sock_peer_str(fd, peer, sizeof peer);

    shm_lock(g_shm);
    int in_use = slots_in_use(g_shm);
    int slot = -1;
    for (int i = 0; i < BENCH_MAX_SLOTS; i++)
        if (!g_shm->slots[i].pid) {
            slot = i;
            break;
        }
    if (slot >= 0)
        g_shm->slots[slot].pid = -1;          /* reserved until fork() returns */
    shm_unlock(g_shm);

    if (!UNSAFE(R_MAX_CLIENTS) && in_use >= (int)g_cfg.max_clients) {
        if (slot >= 0) {
            shm_lock(g_shm);
            g_shm->slots[slot].pid = 0;
            shm_unlock(g_shm);
        }
        reject_busy(fd, peer, in_use);
        return;
    }

    int seq = ++*conn_seq;
    pid_t pid = fork();
    if (pid < 0) {
        LOGE("fork for %s: %s", peer, strerror(errno));
        bn_send_error(fd, BN_ERR_SERVER_BUSY, "server cannot fork: %s", strerror(errno));
        close(fd);
        if (slot >= 0) {
            shm_lock(g_shm);
            g_shm->slots[slot].pid = 0;
            shm_unlock(g_shm);
        }
        return;
    }
    if (pid == 0)
        session_main(fd, slot, seq, peer);
    close(fd);
    if (slot >= 0) {
        shm_lock(g_shm);
        if (g_shm->slots[slot].pid == -1)
            g_shm->slots[slot].pid = pid;
        shm_unlock(g_shm);
    }
    LOGD("accepted %s -> child %d (slot %d, %d in use)", peer, (int)pid, slot, in_use + 1);
}

static void cycle_log_level(void)
{
    static const int next[] = { LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_TRACE, LOG_ERROR };
    int cur = g_shm->log_level;
    int nl = next[cur < 0 || cur > LOG_TRACE ? LOG_INFO : cur];
    g_shm->log_level = nl;
    if (nl > bench_log_compile_level())
        LOGW("log level -> %s (note: this build only contains messages up to %s)",
             bench_log_level_name(nl), bench_log_level_name(bench_log_compile_level()));
    else
        LOGW("log level -> %s", bench_log_level_name(nl));
}

static void shutdown_server(void)
{
    LOGI("signal %d: shutting down", (int)got_term);
    close(g_cfg.listen_fd);

    shm_lock(g_shm);
    ring_event(g_shm, EV_SERVER_SHUTDOWN, "server shutdown");
    uint64_t target = g_shm->head;
    shm_unlock(g_shm);

    /* give children up to 1 s to deliver the shutdown event */
    uint64_t deadline = mono_ms() + 1000;
    while (mono_ms() < deadline) {
        int pending = 0;
        shm_lock(g_shm);
        for (int i = 0; i < BENCH_MAX_SLOTS; i++)
            if (g_shm->slots[i].pid > 0 && g_shm->slots[i].joined && g_shm->slots[i].offset < target)
                pending++;
        shm_unlock(g_shm);
        if (!pending)
            break;
        sleep_ms(20);
    }

    /* SIGTERM every child we know about (explicit list instead of kill(0, ...): when benchd is
     * started from a script it shares the script's process group). */
    shm_lock(g_shm);
    pid_t pids[BENCH_MAX_SLOTS];
    int np = 0;
    for (int i = 0; i < BENCH_MAX_SLOTS; i++)
        if (g_shm->slots[i].pid > 0)
            pids[np++] = g_shm->slots[i].pid;
    shm_unlock(g_shm);
    for (int i = 0; i < np; i++)
        kill(pids[i], SIGTERM);
    if (reader_pid)
        kill(reader_pid, SIGTERM);

    deadline = mono_ms() + 2000;
    for (;;) {
        pid_t p = waitpid(-1, NULL, WNOHANG);
        if (p < 0 && errno == ECHILD)
            break;
        if (p <= 0) {
            if (mono_ms() > deadline) {
                for (int i = 0; i < np; i++)
                    kill(pids[i], SIGKILL);
                if (reader_pid)
                    kill(reader_pid, SIGKILL);
                deadline = UINT64_MAX;
            }
            sleep_ms(10);
        }
    }
    LOGI("bye");
    munmap(g_shm, shm_size);
}

int main(int argc, char **argv)
{
    static const struct option lo[] = {
        { "device", required_argument, 0, 'd' }, { "baud", required_argument, 0, 'b' },
        { "port", required_argument, 0, 'p' }, { "history", required_argument, 0, 1 },
        { "max-clients", required_argument, 0, 2 }, { "idle-release", required_argument, 0, 3 },
        { "log", required_argument, 0, 'l' }, { "unsafe", required_argument, 0, 4 },
        { "help", no_argument, 0, 'h' }, { 0, 0, 0, 0 } };
    const char *logfile = NULL;
    int verbose = 0, c;
    uint32_t unsafe = 0;

    g_cfg.baud = 115200;
    g_cfg.port = BN_DEFAULT_PORT;
    g_cfg.history = 64 * 1024;
    g_cfg.max_clients = 32;
    g_cfg.idle_release = 120;

    while ((c = getopt_long(argc, argv, "d:b:p:l:vh", lo, NULL)) != -1) {
        switch (c) {
        case 'd': g_cfg.device = optarg; break;
        case 'b': g_cfg.baud = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'p': g_cfg.port = (uint16_t)strtoul(optarg, NULL, 10); break;
        case 1:
            if (parse_size(optarg, &g_cfg.history) < 0) {
                fprintf(stderr, "benchd: bad --history size '%s'\n", optarg);
                return 1;
            }
            break;
        case 2: g_cfg.max_clients = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 3: g_cfg.idle_release = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'l': logfile = optarg; break;
        case 4:
            if (bench_unsafe_parse(optarg, &unsafe) < 0) {
                fprintf(stderr, "benchd: unknown name in --unsafe '%s'\n", optarg);
                return 1;
            }
            break;
        case 'v': verbose++; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 1;
        }
    }
    if (!g_cfg.device) {
        usage(stderr);
        return 1;
    }
    if (!baud_supported(g_cfg.baud)) {
        fprintf(stderr, "benchd: unsupported baud rate %u\n", g_cfg.baud);
        return 1;
    }
    if (g_cfg.history < 4096)
        g_cfg.history = 4096;
    g_cfg.history = (g_cfg.history + 7) & ~7ull;
    if (g_cfg.max_clients < 1)
        g_cfg.max_clients = 1;
    if (g_cfg.max_clients > BENCH_MAX_SLOTS)
        g_cfg.max_clients = BENCH_MAX_SLOTS;

    bench_unsafe_mask = unsafe;        /* inherited by every fork()ed process */
    bench_log_set_role("master");
    bench_log_init_level(verbose);
    if (logfile && bench_log_open_file(logfile) < 0) {
        fprintf(stderr, "benchd: cannot open log file %s\n", logfile);
        return 1;
    }
    print_unsafe_banner();

    if (!UNSAFE(R_SIGPIPE))
        signal(SIGPIPE, SIG_IGN);      /* R2: a write to a dead peer must return EPIPE, not kill us */
    else
        signal(SIGPIPE, SIG_DFL);      /* unsafe: default action even if the launcher ignored SIGPIPE */

    /* ---- shared memory: everything children need, created before any fork ---- */
    shm_size = sizeof(struct bench_shm) + g_cfg.history;
    g_shm = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_shm == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    shm_mutex_init(&g_shm->lock);
    ring_init(g_shm, g_cfg.history);
    g_shm->dev_state = DEV_LOST;
    bench_strlcpy(g_shm->dev_path, g_cfg.device, sizeof g_shm->dev_path);
    g_shm->baud = g_cfg.baud;
    g_shm->idle_release_sec = g_cfg.idle_release;
    g_shm->max_clients = g_cfg.max_clients;
    g_shm->server_start_ns = now_ns();
    g_shm->unsafe_mask = unsafe;
    g_shm->master_pid = getpid();
    bench_log_set_level_ptr(&g_shm->log_level);   /* one level for all processes */

    if (pipe(g_cfg.input_pipe) < 0) {
        perror("pipe");
        return 1;
    }

#ifdef BENCH_FAULT_INJECTION
    {
        const char *f = getenv("BENCH_FAULT");
        if (f && strncmp(f, "die_in_lock", 11) == 0) {
            g_cfg.fault_die_in_lock_at = f[11] == ':' ? atoi(f + 12) : 1;
            LOGW("fault injection armed: connection #%d will abort() inside the lock",
                 g_cfg.fault_die_in_lock_at);
        }
    }
#endif

    g_cfg.listen_fd = tcp_listen(g_cfg.port, 128);
    if (g_cfg.listen_fd < 0) {
        LOGE("listen on port %u: %s", g_cfg.port, strerror(errno));
        return 1;
    }

    struct sigaction sa = { 0 };
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_term;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_usr1;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    if (!UNSAFE(R_ZOMBIE)) {
        /* R1: the handler only sets a flag; reaping happens in the main loop */
        sa.sa_handler = on_chld;
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa, NULL);
    }

    spawn_reader();
    char hist[32];
    LOGI("benchd %s listening on :%u — device %s @ %u 8N1, history %s, max %u clients, idle-release %us",
         BENCH_VERSION, g_cfg.port, g_cfg.device, g_cfg.baud, fmt_size(g_cfg.history, hist, sizeof hist),
         g_cfg.max_clients, g_cfg.idle_release);

    int conn_seq = 0;
    while (!got_term) {
        struct pollfd p = { .fd = g_cfg.listen_fd, .events = POLLIN };
        int r = poll(&p, 1, 200);
        if (got_chld) {
            got_chld = 0;
            reap_children();
        }
        if (got_usr1) {
            got_usr1 = 0;
            cycle_log_level();
        }
        if (r > 0 && !got_term)
            handle_accept(&conn_seq);
    }
    shutdown_server();
    return 0;
}
