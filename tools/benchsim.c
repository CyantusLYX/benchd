/*
 * benchsim.c — a simulated serial device for demos without hardware (§13).
 *
 * Creates a pseudo terminal, publishes the slave as a stable symlink (default /tmp/benchsim0)
 * and replays a boot log at the real UART pace (10 bits per byte at the given baud rate).
 * Lines "#delay <ms>" in the log pause the output (boot stages). When playback ends (or any key
 * arrives during playback — like stopping autoboot) it becomes a tiny fake U-Boot prompt:
 * version / printenv / reset / help.
 *
 * Killing benchsim closes the pty master, so benchd's reader gets EIO — that is how the
 * "device unplugged" scenario (R8) is demonstrated.
 */
#include "bench_log.h"
#include "bench_util.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* ---------- built-in content (generic, NOT the Onkyo capture) ---------- */
static const char builtin_log[] =
    "#delay 200\n"
    "U-Boot SPL 2014.07 (benchsim built-in generic log)\n"
    "DRAM:  256 MiB\n"
    "Trying to boot from MMC1\n"
    "#delay 400\n"
    "\n"
    "\n"
    "U-Boot 2014.07 (benchsim built-in generic log)\n"
    "\n"
    "I2C:   ready\n"
    "DRAM:  256 MiB\n"
    "MMC:   mmc@0: 0, mmc@1: 1\n"
    "#delay 300\n"
    "Using default environment\n"
    "\n"
    "In:    serial\n"
    "Out:   serial\n"
    "Err:   serial\n"
    "Net:   Net Initialization Skipped\n"
    "No ethernet found.\n"
    "Hit any key to stop autoboot:  1 \n"
    "#delay 1000\n"
    "Hit any key to stop autoboot:  0 \n"
    "switch to partitions #0, OK\n"
    "mmc0 is current device\n"
    "reading uEnv.txt\n"
    "#delay 200\n"
    "** Unable to read file uEnv.txt **\n"
    "reading zImage\n"
    "#delay 800\n"
    "4194304 bytes read in 412 ms (9.7 MiB/s)\n"
    "reading board.dtb\n"
    "#delay 100\n"
    "32768 bytes read in 12 ms (2.6 MiB/s)\n"
    "Kernel image @ 0x82000000 [ 0x000000 - 0x400000 ]\n"
    "## Flattened Device Tree blob at 88000000\n"
    "   Booting using the fdt blob at 0x88000000\n"
    "   Loading Device Tree to 8fff5000, end 8ffff7ff ... OK\n"
    "\n"
    "Starting kernel ...\n"
    "\n"
    "#delay 600\n"
    "[    0.000000] Booting Linux on physical CPU 0x0\n"
    "[    0.000000] Linux version 3.10.0 (builder@benchsim) (gcc version 4.8.3) #1 SMP PREEMPT\n"
    "[    0.000000] CPU: ARMv7 Processor [413fc090] revision 0 (ARMv7), cr=10c5387d\n"
    "[    0.000000] Machine: Generic DT based system\n"
    "[    0.000000] Memory policy: ECC disabled, Data cache writealloc\n"
    "[    0.000000] Kernel command line: console=ttyS0,115200n8 root=/dev/mmcblk0p2 rw rootwait\n"
    "[    0.000000] PID hash table entries: 1024 (order: 0, 4096 bytes)\n"
    "[    0.000000] Memory: 256MB = 256MB total\n"
    "#delay 300\n"
    "[    0.120000] Calibrating delay loop... 996.14 BogoMIPS (lpj=4980736)\n"
    "[    0.180000] pid_max: default: 32768 minimum: 301\n"
    "[    0.190000] Mount-cache hash table entries: 512\n"
    "[    0.210000] CPU: Testing write buffer coherency: ok\n"
    "[    0.230000] Setting up static identity map for 0x80400000 - 0x80400058\n"
    "[    0.250000] devtmpfs: initialized\n"
    "[    0.270000] NET: Registered protocol family 16\n"
    "#delay 400\n"
    "[    0.410000] bio: create slab <bio-0> at 0\n"
    "[    0.430000] SCSI subsystem initialized\n"
    "[    0.450000] usbcore: registered new interface driver usbfs\n"
    "[    0.470000] Switching to clocksource timer\n"
    "[    0.520000] NET: Registered protocol family 2\n"
    "[    0.540000] TCP: Hash tables configured (established 2048 bind 2048)\n"
    "[    0.610000] Serial: 8250/16550 driver, 4 ports, IRQ sharing disabled\n"
    "[    0.630000] ttyS0 at MMIO 0x44e09000 (irq = 72) is a 8250\n"
    "[    0.650000] console [ttyS0] enabled\n"
    "#delay 500\n"
    "[    1.120000] mmc0: host does not support reading read-only switch. assuming write-enable.\n"
    "[    1.140000] mmc0: new high speed SDHC card at address aaaa\n"
    "[    1.160000] mmcblk0: mmc0:aaaa SU04G 3.69 GiB\n"
    "[    1.180000]  mmcblk0: p1 p2\n"
    "#delay 300\n"
    "[    1.420000] EXT4-fs (mmcblk0p2): mounted filesystem with ordered data mode\n"
    "[    1.440000] VFS: Mounted root (ext4 filesystem) on device 179:2.\n"
    "[    1.460000] devtmpfs: mounted\n"
    "[    1.480000] Freeing init memory: 312K\n"
    "#delay 700\n"
    "\n"
    "benchsim login: \n";

/* ---------- options & state ---------- */
static const char *opt_log = NULL;
static unsigned opt_baud = 115200;
static const char *opt_link = "/tmp/benchsim0";
static int opt_loop = 0;
static const char *opt_prompt = "=> ";

struct seg {
    int is_delay;
    unsigned delay_ms;
    char *text;
    size_t len;
};
static struct seg *segs;
static size_t nsegs;

static int mfd = -1, sfd = -1;
static volatile sig_atomic_t stop_sig;

/* paced output queue */
static char *outq;
static size_t outq_len, outq_cap;
static uint64_t next_due_ns;
static double byte_ns;
static size_t chunk_bytes;

enum { M_PLAY, M_PAUSE, M_PROMPT };
static int mode = M_PLAY;
static size_t seg_idx;
static uint64_t delay_until_ns;
static char line[256];
static size_t line_len;
static int last_was_cr;

static void on_signal(int sig) { stop_sig = sig; }

static void q_put(const char *p, size_t n)
{
    if (outq_len + n > outq_cap) {
        outq_cap = (outq_len + n) * 2 + 1024;
        outq = realloc(outq, outq_cap);
        if (!outq) {
            perror("realloc");
            exit(1);
        }
    }
    memcpy(outq + outq_len, p, n);
    outq_len += n;
}
static void q_str(const char *s) { q_put(s, strlen(s)); }

static void add_seg_text(const char *p, size_t n)
{
    segs = realloc(segs, (nsegs + 1) * sizeof *segs);
    segs[nsegs].is_delay = 0;
    segs[nsegs].text = malloc(n + 2);
    memcpy(segs[nsegs].text, p, n);
    segs[nsegs].text[n] = '\r';          /* UART lines end in CRLF like real U-Boot */
    segs[nsegs].text[n + 1] = '\n';
    segs[nsegs].len = n + 2;
    nsegs++;
}

static void add_seg_delay(unsigned ms)
{
    segs = realloc(segs, (nsegs + 1) * sizeof *segs);
    segs[nsegs].is_delay = 1;
    segs[nsegs].delay_ms = ms;
    segs[nsegs].text = NULL;
    segs[nsegs].len = 0;
    nsegs++;
}

/* returns 1 if the buffer is a placeholder file */
static int parse_script(const char *buf, size_t len)
{
    const char *p = buf, *end = buf + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t l = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (l && p[l - 1] == '\r')
            l--;
        if (l >= 12 && strncmp(p, "#placeholder", 12) == 0)
            return 1;
        if (l >= 7 && strncmp(p, "#delay ", 7) == 0)
            add_seg_delay((unsigned)atoi(p + 7));
        else
            add_seg_text(p, l);
        p = nl ? nl + 1 : end;
    }
    return 0;
}

static void load_log(void)
{
    if (opt_log) {
        FILE *f = fopen(opt_log, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *buf = malloc((size_t)sz + 1);
            size_t got = fread(buf, 1, (size_t)sz, f);
            fclose(f);
            int placeholder = parse_script(buf, got);
            free(buf);
            if (!placeholder) {
                LOGI("loaded %s: %zu segments", opt_log, nsegs);
                return;
            }
            LOGW("%s is still a placeholder (to be replaced by the author); using built-in text",
                 opt_log);
            for (size_t i = 0; i < nsegs; i++)
                free(segs[i].text);
            nsegs = 0;
        } else {
            LOGW("cannot open %s (%s); using built-in generic U-Boot text", opt_log, strerror(errno));
        }
    } else {
        LOGW("no -l log given; using built-in generic U-Boot text");
    }
    parse_script(builtin_log, sizeof builtin_log - 1);
}

static void prompt(void)
{
    q_str(opt_prompt);
}

static void run_command(const char *cmd)
{
    while (*cmd == ' ')
        cmd++;
    if (!*cmd) {
        prompt();
        return;
    }
    if (strcmp(cmd, "version") == 0) {
        q_str("\r\nU-Boot 2014.07 (benchsim simulated device)\r\n"
              "arm-linux-gnueabihf-gcc (benchsim) 4.8.3\r\n"
              "GNU ld (benchsim) 2.24\r\n");
    } else if (strcmp(cmd, "printenv") == 0) {
        char b[128];
        q_str("arch=arm\r\nbaudrate=");
        snprintf(b, sizeof b, "%u", opt_baud);
        q_str(b);
        q_str("\r\nboard=benchsim\r\nbootcmd=run mmcboot\r\nbootdelay=1\r\n"
              "console=ttyS0,115200n8\r\nloadaddr=0x82000000\r\nstderr=serial\r\n"
              "stdin=serial\r\nstdout=serial\r\n\r\nEnvironment size: 214/131068 bytes\r\n");
    } else if (strcmp(cmd, "reset") == 0) {
        q_str("resetting ...\r\n");
        mode = M_PLAY;
        seg_idx = 0;
        delay_until_ns = mono_ns() + 300000000ull;
        LOGI("reset: replaying log");
        return;
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        q_str("help     - print command description/usage\r\n"
              "printenv - print environment variables\r\n"
              "reset    - Perform RESET of the CPU\r\n"
              "version  - print monitor, compiler and linker version\r\n");
    } else {
        char b[300];
        snprintf(b, sizeof b, "Unknown command '%.200s' - try 'help'\r\n", cmd);
        q_str(b);
    }
    prompt();
}

static void handle_input(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (mode != M_PROMPT) {
            /* any key during playback stops "autoboot" and drops to the prompt */
            LOGI("key during playback: entering prompt mode");
            outq_len = 0;
            mode = M_PROMPT;
            line_len = 0;
            q_str("\r\n");
            prompt();
            /* unlike real U-Boot the key is not swallowed: `bench send "version\r"` just works */
        }
        if (c == '\n' && last_was_cr) {
            last_was_cr = 0;
            continue;
        }
        last_was_cr = (c == '\r');
        if (c == '\r' || c == '\n') {
            q_str("\r\n");
            line[line_len] = 0;
            line_len = 0;
            run_command(line);
        } else if (c == 0x7f || c == 0x08) {
            if (line_len) {
                line_len--;
                q_str("\b \b");
            }
        } else if (c == 0x03) {
            line_len = 0;
            q_str("<INTERRUPT>\r\n");
            prompt();
        } else if (c >= 0x20 && c < 0x7f && line_len < sizeof line - 1) {
            line[line_len++] = (char)c;
            q_put((char *)&c, 1);      /* echo */
        }
    }
}

/* Keep at least one write chunk queued from the script (unless we are inside a #delay). */
static void produce(uint64_t now)
{
    while (outq_len < chunk_bytes && now >= delay_until_ns) {
        if (mode == M_PAUSE) {
            mode = M_PLAY;
            seg_idx = 0;
        }
        if (mode != M_PLAY)
            return;
        if (seg_idx >= nsegs) {
            if (outq_len)
                return;                       /* let the tail of the log go out first */
            if (opt_loop) {
                mode = M_PAUSE;
                delay_until_ns = now + 3000000000ull;
                LOGD("end of log, looping in 3 s");
            } else {
                mode = M_PROMPT;
                prompt();
                LOGI("end of log, prompt mode");
            }
            return;
        }
        struct seg *s = &segs[seg_idx++];
        if (s->is_delay) {
            /* the pause starts once the text queued before it has gone out on the "wire" */
            uint64_t start = next_due_ns > now ? next_due_ns : now;
            start += (uint64_t)((double)outq_len * byte_ns);
            delay_until_ns = start + (uint64_t)s->delay_ms * 1000000ull;
        }
        else
            q_put(s->text, s->len);
    }
}

/* Write at most one chunk if it is due. */
static void drain(uint64_t now)
{
    if (!outq_len || now < next_due_ns)
        return;
    if (next_due_ns + 50000000ull < now)       /* we were idle: don't burst to catch up */
        next_due_ns = now;
    size_t n = outq_len < chunk_bytes ? outq_len : chunk_bytes;
    ssize_t w = write(mfd, outq, n);
    if (w < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO)
            w = (ssize_t)n;      /* nobody reading: a real UART would lose it too */
        else {
            LOGE("write pty: %s", strerror(errno));
            w = (ssize_t)n;
        }
    }
    memmove(outq, outq + w, outq_len - (size_t)w);
    outq_len -= (size_t)w;
    next_due_ns += (uint64_t)((double)w * byte_ns);
}

static void usage(FILE *f)
{
    fprintf(f,
        "usage: benchsim -l <logfile> [-b 115200] [-L /tmp/benchsim0] [--loop] [--prompt \"=> \"] [-v]\n"
        "  -l, --log FILE     boot log to replay (lines '#delay <ms>' pause the output)\n"
        "  -b, --baud N       pace output at N baud (10 bits per byte), default 115200\n"
        "  -L, --link PATH    symlink to the pty slave, default /tmp/benchsim0\n"
        "      --loop         replay forever (3 s pause between runs)\n"
        "      --prompt STR   fake U-Boot prompt, default \"=> \"\n"
        "  -v                 more logging (repeatable)\n"
        "  -h, --help\n");
}

int main(int argc, char **argv)
{
    static const struct option lo[] = {
        { "log", required_argument, 0, 'l' }, { "baud", required_argument, 0, 'b' },
        { "link", required_argument, 0, 'L' }, { "loop", no_argument, 0, 1 },
        { "prompt", required_argument, 0, 2 }, { "help", no_argument, 0, 'h' }, { 0, 0, 0, 0 } };
    int c, verbose = 0;
    while ((c = getopt_long(argc, argv, "l:b:L:vh", lo, NULL)) != -1) {
        switch (c) {
        case 'l': opt_log = optarg; break;
        case 'b': opt_baud = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'L': opt_link = optarg; break;
        case 1: opt_loop = 1; break;
        case 2: opt_prompt = optarg; break;
        case 'v': verbose++; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 1;
        }
    }
    if (opt_baud < 50) {
        fprintf(stderr, "benchsim: bad baud rate\n");
        return 1;
    }
    bench_log_set_role("benchsim");
    bench_log_init_level(verbose);

    byte_ns = 10.0 * 1e9 / (double)opt_baud;
    chunk_bytes = opt_baud / 10 / 100;          /* ~10 ms worth of bytes per write */
    if (chunk_bytes < 1)
        chunk_bytes = 1;
    load_log();

    mfd = posix_openpt(O_RDWR | O_NOCTTY);
    if (mfd < 0 || grantpt(mfd) < 0 || unlockpt(mfd) < 0) {
        LOGE("posix_openpt: %s", strerror(errno));
        return 1;
    }
    const char *slave = ptsname(mfd);
    /* Hold a slave fd ourselves: keeps the termios settings and avoids EIO on the master
     * while benchd is not connected. We never read from it. */
    sfd = open(slave, O_RDWR | O_NOCTTY);
    if (sfd >= 0) {
        struct termios t;
        if (tcgetattr(sfd, &t) == 0) {
            cfmakeraw(&t);
            tcsetattr(sfd, TCSANOW, &t);
        }
    }
    int fl = fcntl(mfd, F_GETFL);
    fcntl(mfd, F_SETFL, fl | O_NONBLOCK);

    unlink(opt_link);
    if (symlink(slave, opt_link) < 0) {
        LOGE("symlink %s -> %s: %s", opt_link, slave, strerror(errno));
        return 1;
    }
    LOGI("simulating device on %s -> %s at %u baud%s", opt_link, slave, opt_baud,
         opt_loop ? " (loop)" : "");

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    next_due_ns = mono_ns();
    delay_until_ns = mono_ns();
    while (!stop_sig) {
        uint64_t now = mono_ns();
        produce(now);
        drain(now);

        int timeout = 50;
        now = mono_ns();
        if (outq_len) {
            int64_t d = (int64_t)(next_due_ns - now) / 1000000;
            timeout = d < 0 ? 0 : (d > 50 ? 50 : (int)d);
            if (timeout == 0 && next_due_ns > now)
                timeout = 1;
        } else if (mode != M_PROMPT && delay_until_ns > now) {
            int64_t d = (int64_t)(delay_until_ns - now) / 1000000;
            timeout = d > 50 ? 50 : (int)d + 1;
        }
        struct pollfd p = { .fd = mfd, .events = POLLIN };
        int r = poll(&p, 1, timeout);
        if (r > 0 && (p.revents & POLLIN)) {
            uint8_t buf[256];
            ssize_t n = read(mfd, buf, sizeof buf);
            if (n > 0) {
                LOGD("rx %zd bytes from host", n);
                handle_input(buf, (size_t)n);
            }
        }
    }
    LOGI("signal %d: removing %s", (int)stop_sig, opt_link);
    unlink(opt_link);
    return 0;
}
