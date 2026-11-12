/*
 * sysinfo.c — builds the INFO reply (§6.6): "key=value\n" lines from uname(), /proc and the
 * shared state. The shared state is snapshotted under the lock first; file I/O happens after.
 */
#include "server.h"
#include "bench_log.h"
#include "bench_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

static size_t add(char *buf, size_t cap, size_t o, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static size_t add(char *buf, size_t cap, size_t o, const char *fmt, ...)
{
    if (o >= cap)
        return o;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + o, cap - o, fmt, ap);
    va_end(ap);
    if (n < 0)
        return o;
    return o + (size_t)n >= cap ? cap - 1 : o + (size_t)n;
}

static int read_first_line(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(out, (int)n, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return 0;
}

static long meminfo(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    long v = -1;
    size_t kl = strlen(key);
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, key, kl) == 0 && line[kl] == ':') {
            sscanf(line + kl + 1, "%ld", &v);
            break;
        }
    fclose(f);
    return v;
}

static void cpu_model(char *out, size_t n)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[512];
    bench_strlcpy(out, "unknown", n);
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "model name", 10) == 0 || strncmp(line, "Hardware", 8) == 0) {
            char *c = strchr(line, ':');
            if (c) {
                c++;
                while (*c == ' ' || *c == '\t')
                    c++;
                c[strcspn(c, "\n")] = 0;
                bench_strlcpy(out, c, n);
                break;
            }
        }
    }
    fclose(f);
}

size_t sysinfo_build(char *buf, size_t cap)
{
    /* 1. snapshot shared state */
    struct {
        int dev_state; char dev_path[128]; uint32_t baud; uint64_t rx, tx; uint32_t recon;
        uint64_t last_rx_ns; pid_t ctrl_pid; char ctrl_name[BN_NAME_LEN]; uint64_t ctrl_last;
        uint32_t idle_release, viewers, max_clients; uint64_t start_ns, cap, head, tail;
        int log_level; uint32_t unsafe; uint32_t recov; pid_t master;
    } st;
    shm_lock(g_shm);
    st.dev_state = g_shm->dev_state;
    memcpy(st.dev_path, g_shm->dev_path, sizeof st.dev_path);
    st.baud = g_shm->baud;
    st.rx = g_shm->rx_bytes;
    st.tx = g_shm->tx_bytes;
    st.recon = g_shm->dev_reconnects;
    st.last_rx_ns = g_shm->last_rx_ns;
    st.ctrl_pid = g_shm->ctrl_pid;
    memcpy(st.ctrl_name, g_shm->ctrl_name, sizeof st.ctrl_name);
    st.ctrl_last = g_shm->ctrl_last_input_ns;
    st.idle_release = g_shm->idle_release_sec;
    st.viewers = slots_recount(g_shm);
    st.max_clients = g_shm->max_clients;
    st.start_ns = g_shm->server_start_ns;
    st.cap = g_shm->cap;
    st.head = g_shm->head;
    st.tail = g_shm->tail;
    st.log_level = g_shm->log_level;
    st.unsafe = g_shm->unsafe_mask;
    st.recov = g_shm->lock_recoveries;
    st.master = g_shm->master_pid;
    shm_unlock(g_shm);

    /* 2. host facts */
    struct utsname u;
    uname(&u);
    char up[64] = "0", load[64] = "?", cpu[128];
    read_first_line("/proc/uptime", up, sizeof up);
    read_first_line("/proc/loadavg", load, sizeof load);
    double upf = 0;
    sscanf(up, "%lf", &upf);
    /* keep the first three loadavg numbers */
    {
        int sp = 0;
        for (char *p = load; *p; p++)
            if (*p == ' ' && ++sp == 3) {
                *p = 0;
                break;
            }
    }
    cpu_model(cpu, sizeof cpu);
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    uint64_t now = now_ns();
    char unsafe[256];
    bench_unsafe_format(st.unsafe, unsafe, sizeof unsafe);

    size_t o = 0;
    o = add(buf, cap, o, "host.name=%s\n", u.nodename);
    o = add(buf, cap, o, "host.kernel=%s %s %s\n", u.sysname, u.release, u.machine);
    o = add(buf, cap, o, "host.uptime_sec=%.0f\n", upf);
    o = add(buf, cap, o, "host.loadavg=%s\n", load);
    o = add(buf, cap, o, "host.cpu_model=%s\n", cpu);
    o = add(buf, cap, o, "host.cpu_count=%ld\n", ncpu);
    o = add(buf, cap, o, "host.mem_total_kb=%ld\n", meminfo("MemTotal"));
    o = add(buf, cap, o, "host.mem_avail_kb=%ld\n", meminfo("MemAvailable"));
    o = add(buf, cap, o, "server.pid=%d\n", (int)st.master);
    o = add(buf, cap, o, "server.uptime_sec=%llu\n", (unsigned long long)((now - st.start_ns) / 1000000000ull));
    o = add(buf, cap, o, "server.version=%s\n", BENCH_VERSION);
    o = add(buf, cap, o, "server.clients=%u\n", st.viewers);
    o = add(buf, cap, o, "server.max_clients=%u\n", st.max_clients);
    o = add(buf, cap, o, "server.log_level=%s\n", bench_log_level_name(st.log_level));
    o = add(buf, cap, o, "server.unsafe=%s\n", unsafe);
    o = add(buf, cap, o, "server.lock_recoveries=%u\n", st.recov);
    o = add(buf, cap, o, "dev.path=%s\n", st.dev_path);
    o = add(buf, cap, o, "dev.state=%s\n", st.dev_state == DEV_UP ? "UP" : "LOST");
    o = add(buf, cap, o, "dev.baud=%u 8N1\n", st.baud);
    o = add(buf, cap, o, "dev.rx_bytes=%llu\n", (unsigned long long)st.rx);
    o = add(buf, cap, o, "dev.tx_bytes=%llu\n", (unsigned long long)st.tx);
    o = add(buf, cap, o, "dev.reconnects=%u\n", st.recon);
    if (st.last_rx_ns)
        o = add(buf, cap, o, "dev.last_rx_ago_ms=%llu\n", (unsigned long long)((now - st.last_rx_ns) / 1000000ull));
    else
        o = add(buf, cap, o, "dev.last_rx_ago_ms=-1\n");
    o = add(buf, cap, o, "hist.cap=%llu\n", (unsigned long long)st.cap);
    o = add(buf, cap, o, "hist.used=%llu\n", (unsigned long long)(st.head - st.tail));
    o = add(buf, cap, o, "hist.head=%llu\n", (unsigned long long)st.head);
    o = add(buf, cap, o, "ctrl.holder=%s\n", st.ctrl_pid ? st.ctrl_name : "(none)");
    o = add(buf, cap, o, "ctrl.idle_sec=%llu\n",
            st.ctrl_pid && now > st.ctrl_last ? (unsigned long long)((now - st.ctrl_last) / 1000000000ull) : 0ull);
    o = add(buf, cap, o, "ctrl.idle_release_sec=%u\n", st.idle_release);
    return o;
}
