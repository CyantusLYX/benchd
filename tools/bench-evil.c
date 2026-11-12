/*
 * bench-evil.c — fault-injection client used to trigger the robustness scenarios (§9.3).
 *
 *   slow          HELLO and INFO_REQ dribbled 1 byte / 50 ms                 (R4 readn)
 *   bad-len       header claims len = 1 MiB, then 1 MiB of garbage             (R3 validate)
 *   bad-magic     HELLO with a wrong magic                                     (R3)
 *   bad-crc       HELLO with a wrong CRC                                       (R3)
 *   silent        connect and send nothing [--count N]                         (R5 timeout)
 *   rst           connect, then close with SO_LINGER{1,0} -> RST [--count N]   (R2 sigpipe)
 *   flood         open N connections as fast as possible and keep them         (R9 max-clients)
 *   take-and-die  HELLO, TAKE, on "granted" _exit(1) without BYE               (R6 ctrl-release)
 *   lag           HELLO, then do not read the socket for --sec seconds         (R10 overrun)
 *   take-hold     HELLO, TAKE, then hold control quietly for --sec seconds     (tests / demos)
 */
#include "bench_log.h"
#include "bench_net.h"
#include "bench_proto.h"
#include "bench_util.h"

#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

static char host[256];
static uint16_t port;
static int opt_count = 1;
static int opt_sec = -1;
static int opt_after = 5;
static int opt_hold = -1;
static char opt_name[BN_NAME_LEN] = "evil";
static uint8_t rbuf[BN_MAX_PAYLOAD];

static int dial(void)
{
    int fd = tcp_connect_timeout(host, port, 3000);
    if (fd < 0)
        fprintf(stderr, "evil: connect %s:%u: %s\n", host, port, strerror(errno));
    return fd;
}

static size_t build_packet(uint8_t *out, uint8_t type, const void *p, uint32_t len)
{
    bn_hdr_encode(out, type, len, len ? bench_crc32(p, len) : 0);
    if (len)
        memcpy(out + BN_HDR_LEN, p, len);
    return BN_HDR_LEN + len;
}

static void make_hello(struct bn_hello *h, uint32_t replay)
{
    memset(h, 0, sizeof *h);
    bench_strlcpy(h->name, opt_name, sizeof h->name);
    h->replay_bytes = htobe32(replay);
}

static int hello(int fd, uint32_t replay)
{
    struct bn_hello h;
    make_hello(&h, replay);
    return bn_send(fd, BN_HELLO, &h, sizeof h);
}

/* Read packets until `type` arrives (or timeout). Prints errors. Returns 1 found, 0 timeout, -1 closed. */
static int wait_for(int fd, int type, int ms, struct bn_hdr *out)
{
    uint64_t end = mono_ms() + (uint64_t)ms;
    for (;;) {
        int64_t left = (int64_t)(end - mono_ms());
        if (left <= 0)
            return 0;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, (int)left) <= 0)
            continue;
        struct bn_hdr h;
        int perr;
        int rc = bn_recv(fd, &h, rbuf, sizeof rbuf, &perr);
        if (rc != 1)
            return -1;
        if (h.type == BN_ERROR && h.len >= 4) {
            printf("evil: server replied ERROR %s: %.*s\n", bn_err_name(be16toh(*(uint16_t *)rbuf)),
                   (int)(h.len - 4), (char *)rbuf + 4);
        }
        if (h.type == type) {
            if (out)
                *out = h;
            return 1;
        }
    }
}

/* Wait for the server's reaction after we sent something nasty. */
static void report_reaction(int fd, int ms)
{
    uint64_t t0 = mono_ms();
    int rc = wait_for(fd, -1, ms, NULL);
    if (rc < 0)
        printf("evil: server closed the connection after %llu ms\n", (unsigned long long)(mono_ms() - t0));
    else
        printf("evil: connection still open after %d ms\n", ms);
}

static int mode_slow(void)
{
    int fd = dial();
    if (fd < 0)
        return 1;
    uint8_t pkt[256];
    struct bn_hello h;
    make_hello(&h, 0);
    size_t n = build_packet(pkt, BN_HELLO, &h, sizeof h);
    n += build_packet(pkt + n, BN_INFO_REQ, NULL, 0);
    printf("evil: sending HELLO + INFO_REQ (%zu bytes) one byte every 50 ms (%.1f s)\n", n, (double)n * 0.05);
    fflush(stdout);
    for (size_t i = 0; i < n; i++) {
        if (send(fd, pkt + i, 1, MSG_NOSIGNAL) != 1) {
            printf("evil: send failed at byte %zu: %s\n", i, strerror(errno));
            return 1;
        }
        sleep_ms(50);
    }
    struct bn_hdr hh;
    int rc = wait_for(fd, BN_INFO, 5000, &hh);
    if (rc == 1) {
        printf("evil: got INFO (%u bytes) — server reassembled the dribbled packets correctly\n", hh.len);
        fwrite(rbuf, 1, hh.len > 200 ? 200 : hh.len, stdout);
        printf("...\n");
        close(fd);
        return 0;
    }
    printf("evil: no INFO reply (%s)\n", rc < 0 ? "connection closed" : "timeout");
    close(fd);
    return 1;
}

static int mode_bad(const char *which)
{
    int fd = dial();
    if (fd < 0)
        return 1;
    struct bn_hello h;
    make_hello(&h, 0);
    uint8_t pkt[256];
    size_t n = build_packet(pkt, BN_HELLO, &h, sizeof h);
    if (!strcmp(which, "bad-magic")) {
        pkt[0] = 'X';
        pkt[1] = 'Y';
        printf("evil: HELLO with magic 0x5859 instead of 0x424E\n");
        send(fd, pkt, n, MSG_NOSIGNAL);
    } else if (!strcmp(which, "bad-crc")) {
        pkt[8] ^= 0xFF;
        printf("evil: HELLO with a corrupted CRC32\n");
        send(fd, pkt, n, MSG_NOSIGNAL);
    } else {                                        /* bad-len */
        uint32_t len = 0x00100000;
        bn_hdr_encode(pkt, BN_HELLO, len, 0xDEADBEEF);
        printf("evil: header claims len = %u (1 MiB); sending that much garbage\n", len);
        fflush(stdout);
        send(fd, pkt, BN_HDR_LEN, MSG_NOSIGNAL);
        uint8_t junk[8192];
        memset(junk, 'A', sizeof junk);
        size_t sent = 0;
        while (sent < len) {
            size_t c = len - sent < sizeof junk ? len - sent : sizeof junk;
            ssize_t w = send(fd, junk, c, MSG_NOSIGNAL);
            if (w <= 0) {
                printf("evil: server stopped accepting after %zu bytes (%s)\n", sent, strerror(errno));
                break;
            }
            sent += (size_t)w;
        }
    }
    fflush(stdout);
    report_reaction(fd, 3000);
    close(fd);
    return 0;
}

static void raise_nofile(int need)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < (rlim_t)need + 32) {
        rl.rlim_cur = rl.rlim_max < (rlim_t)need + 32 ? rl.rlim_max : (rlim_t)need + 32;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

static int mode_silent(void)
{
    raise_nofile(opt_count);
    int *fds = calloc((size_t)opt_count, sizeof *fds);
    int open_n = 0;
    for (int i = 0; i < opt_count; i++) {
        fds[i] = dial();
        if (fds[i] >= 0)
            open_n++;
    }
    int hold = opt_sec > 0 ? opt_sec : 30;
    printf("evil: %d silent connections open; holding up to %d s\n", open_n, hold);
    fflush(stdout);
    uint64_t t0 = mono_ms(), end = t0 + (uint64_t)hold * 1000;
    while (open_n > 0 && mono_ms() < end) {
        struct pollfd *p = calloc((size_t)opt_count, sizeof *p);
        for (int i = 0; i < opt_count; i++) {
            p[i].fd = fds[i];
            p[i].events = POLLIN;
        }
        poll(p, (nfds_t)opt_count, 200);
        for (int i = 0; i < opt_count; i++) {
            if (fds[i] < 0 || !(p[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            char b[512];
            ssize_t r = recv(fds[i], b, sizeof b, 0);
            if (r <= 0) {
                close(fds[i]);
                fds[i] = -1;
                open_n--;
                if (open_n == 0 || i == 0)
                    printf("evil: server dropped silent connection #%d after %.1f s (%d left)\n", i,
                           (double)(mono_ms() - t0) / 1000.0, open_n);
            }
        }
        free(p);
    }
    printf("evil: %d of %d silent connections still open after %.1f s\n", open_n, opt_count,
           (double)(mono_ms() - t0) / 1000.0);
    return 0;
}

static int mode_rst(void)
{
    int ok = 0;
    for (int i = 0; i < opt_count; i++) {
        int fd = tcp_connect_timeout(host, port, 3000);
        if (fd < 0) {
            printf("evil: connect #%d failed: %s (server down?)\n", i, strerror(errno));
            break;
        }
        struct linger lg = { .l_onoff = 1, .l_linger = 0 };
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
        close(fd);                                  /* -> RST instead of FIN */
        ok++;
        sleep_ms(5);
    }
    printf("evil: sent %d connect+RST\n", ok);
    return 0;
}

static int mode_flood(void)
{
    raise_nofile(opt_count);
    int *fds = calloc((size_t)opt_count, sizeof *fds);
    int welcomed = 0, busy = 0, failed = 0;
    for (int i = 0; i < opt_count; i++) {
        fds[i] = tcp_connect_timeout(host, port, 3000);
        if (fds[i] < 0) {
            failed++;
            continue;
        }
        snprintf(opt_name, sizeof opt_name, "flood%03d", i);
        hello(fds[i], 0);
    }
    /* classify the answers */
    for (int i = 0; i < opt_count; i++) {
        if (fds[i] < 0)
            continue;
        struct pollfd p = { .fd = fds[i], .events = POLLIN };
        if (poll(&p, 1, 3000) <= 0) {
            failed++;
            continue;
        }
        struct bn_hdr h;
        int perr;
        if (bn_recv(fds[i], &h, rbuf, sizeof rbuf, &perr) == 1 && h.type == BN_WELCOME) {
            welcomed++;
        } else {
            if (h.type == BN_ERROR)
                busy++;
            else
                failed++;
            close(fds[i]);
            fds[i] = -1;
        }
    }
    printf("evil: flood of %d: %d welcomed, %d refused (SERVER_BUSY), %d failed\n", opt_count, welcomed,
           busy, failed);
    fflush(stdout);
    int hold = opt_sec > 0 ? opt_sec : 30;
    uint64_t end = mono_ms() + (uint64_t)hold * 1000, last_ping = mono_ms();
    /* keep the welcomed ones alive: drain their sockets, ping every 10 s */
    while (mono_ms() < end) {
        for (int i = 0; i < opt_count; i++) {
            if (fds[i] < 0)
                continue;
            char b[4096];
            while (recv(fds[i], b, sizeof b, MSG_DONTWAIT) > 0)
                ;
        }
        if (mono_ms() - last_ping > 10000) {
            for (int i = 0; i < opt_count; i++)
                if (fds[i] >= 0)
                    bn_send(fds[i], BN_PING, "\0\0\0\0\0\0\0\0", 8);
            last_ping = mono_ms();
        }
        sleep_ms(100);
    }
    return 0;
}

static int mode_take_and_die(void)
{
    int fd = dial();
    if (fd < 0)
        return 1;
    hello(fd, 0);
    bn_send(fd, BN_TAKE, NULL, 0);
    struct bn_hdr h;
    if (wait_for(fd, BN_CTRL_RESULT, 5000, &h) == 1) {
        if (rbuf[0]) {
            printf("evil: control granted to '%s' — now dying without BYE (_exit)\n", opt_name);
            fflush(stdout);
            _exit(1);
        }
        printf("evil: control denied (held by %.32s)\n", (char *)rbuf + 12);
        return 1;
    }
    printf("evil: no CTRL_RESULT\n");
    return 1;
}

static int mode_take_hold(void)
{
    int fd = dial();
    if (fd < 0)
        return 1;
    hello(fd, 0);
    bn_send(fd, BN_TAKE, NULL, 0);
    struct bn_hdr h;
    if (wait_for(fd, BN_CTRL_RESULT, 5000, &h) != 1 || !rbuf[0]) {
        printf("evil: control not granted\n");
        return 2;
    }
    int sec = opt_sec > 0 ? opt_sec : 30;
    printf("evil: '%s' holds control for %d s (no input)\n", opt_name, sec);
    uint64_t end = mono_ms() + (uint64_t)sec * 1000, last_ping = mono_ms();
    while (mono_ms() < end) {
        char b[4096];
        while (recv(fd, b, sizeof b, MSG_DONTWAIT) > 0)
            ;
        if (mono_ms() - last_ping > 10000) {
            bn_send(fd, BN_PING, "\0\0\0\0\0\0\0\0", 8);
            last_ping = mono_ms();
        }
        sleep_ms(100);
    }
    bn_send(fd, BN_BYE, NULL, 0);
    close(fd);
    return 0;
}

static void print_live(const struct bn_hdr *h)
{
    if (h->type == BN_DATA && h->len >= 16) {
        fwrite(rbuf + 16, 1, h->len - 16, stdout);
    } else if (h->type == BN_EVENT && h->len >= 20) {
        printf("\n[EVENT %s] %.*s\n", bn_evt_name(be16toh(*(uint16_t *)(rbuf + 16))), (int)(h->len - 20),
               (char *)rbuf + 20);
    }
}

/* Connect with a tiny receive buffer set BEFORE connect(), so the advertised TCP window stays
 * small and loopback autotuning cannot hide the lag in megabytes of socket buffers. */
static int dial_small_rcvbuf(void)
{
    char ports[8];
    snprintf(ports, sizeof ports, "%u", port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res;
    if (getaddrinfo(host, ports, &hints, &res) != 0)
        return -1;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    int small = 4096;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        fprintf(stderr, "evil: connect: %s\n", strerror(errno));
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int mode_lag(void)
{
    int fd = dial_small_rcvbuf();
    if (fd < 0)
        return 1;
    int sec = opt_sec > 0 ? opt_sec : 30;
    hello(fd, 0);
    printf("evil: joined; not reading the socket for %d s...\n", sec);
    fflush(stdout);
    uint64_t end = mono_ms() + (uint64_t)sec * 1000;
    while (mono_ms() < end) {
        sleep_ms(1000);
        if ((end - mono_ms()) / 1000 % 10 == 0)
            bn_send(fd, BN_PING, "\0\0\0\0\0\0\0\0", 8);   /* stay inside the 60 s idle limit */
    }
    printf("evil: reading again for %d s:\n---------------------------------------------\n", opt_after);
    fflush(stdout);
    end = mono_ms() + (uint64_t)opt_after * 1000;
    while (mono_ms() < end) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 200) <= 0)
            continue;
        struct bn_hdr h;
        int perr;
        int rc = bn_recv(fd, &h, rbuf, sizeof rbuf, &perr);
        if (rc != 1) {
            printf("\nevil: connection closed (%s)\n", rc == -2 ? bn_err_name(perr) : "EOF/error");
            break;
        }
        print_live(&h);
        fflush(stdout);
    }
    printf("\n---------------------------------------------\n");
    close(fd);
    return 0;
}

static void usage(FILE *f)
{
    fprintf(f,
        "usage: bench-evil <mode> <host[:port]> [options]\n"
        "modes: slow | bad-len | bad-magic | bad-crc | silent | rst | flood | take-and-die | lag | take-hold\n"
        "  --count N     connections for silent / rst / flood (default 1)\n"
        "  --sec N       hold time for silent / flood (default 30), lag duration for lag (default 30)\n"
        "  --after N     lag: seconds to read after lagging (default 5)\n"
        "  --name NAME   client name (default evil)\n"
        "  -h, --help\n");
}

int main(int argc, char **argv)
{
    static const struct option lo[] = {
        { "count", required_argument, 0, 1 }, { "sec", required_argument, 0, 2 },
        { "after", required_argument, 0, 3 }, { "name", required_argument, 0, 4 },
        { "hold", required_argument, 0, 5 }, { "help", no_argument, 0, 'h' }, { 0, 0, 0, 0 } };
    int c;
    while ((c = getopt_long(argc, argv, "h", lo, NULL)) != -1) {
        switch (c) {
        case 1: opt_count = atoi(optarg); break;
        case 2: opt_sec = atoi(optarg); break;
        case 3: opt_after = atoi(optarg); break;
        case 4: bench_strlcpy(opt_name, optarg, sizeof opt_name); break;
        case 5: opt_hold = atoi(optarg); break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 1;
        }
    }
    (void)opt_hold;
    if (argc - optind < 2) {
        usage(stderr);
        return 1;
    }
    const char *mode = argv[optind];
    if (parse_hostport(argv[optind + 1], host, sizeof host, &port, BN_DEFAULT_PORT) < 0) {
        fprintf(stderr, "evil: bad address\n");
        return 1;
    }
    if (opt_count < 1)
        opt_count = 1;
    signal(SIGPIPE, SIG_IGN);
    bench_log_set_role("evil");
    bench_log_init_level(0);
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (!strcmp(mode, "slow")) return mode_slow();
    if (!strcmp(mode, "bad-len") || !strcmp(mode, "bad-magic") || !strcmp(mode, "bad-crc"))
        return mode_bad(mode);
    if (!strcmp(mode, "silent")) return mode_silent();
    if (!strcmp(mode, "rst")) return mode_rst();
    if (!strcmp(mode, "flood")) return mode_flood();
    if (!strcmp(mode, "take-and-die")) return mode_take_and_die();
    if (!strcmp(mode, "lag")) return mode_lag();
    if (!strcmp(mode, "take-hold")) return mode_take_hold();
    fprintf(stderr, "evil: unknown mode '%s'\n", mode);
    usage(stderr);
    return 1;
}
