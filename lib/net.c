/*
 * net.c — readn/writen and TCP helpers shared by client and server.
 */
#include "bench_net.h"
#include "bench_log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Keep reading until n bytes arrived. TCP may hand us a packet in arbitrary pieces (R4). */
ssize_t readn(int fd, void *buf, size_t n)
{
    size_t got = 0;
    char *p = buf;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;          /* EOF: return short count */
        got += (size_t)r;
    }
    return (ssize_t)got;
}

ssize_t writen(int fd, const void *buf, size_t n)
{
    size_t put = 0;
    const char *p = buf;
    while (put < n) {
        ssize_t w = write(fd, p + put, n - put);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        put += (size_t)w;
    }
    return (ssize_t)put;
}

int tcp_listen(uint16_t port, int backlog)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    int v6 = 1;
    if (fd < 0) {               /* no IPv6 in this kernel: plain IPv4 */
        v6 = 0;
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
    }
    int one = 1, zero = 0;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);   /* always on (§9) */
    int rc;
    if (v6) {
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);  /* dual stack */
        struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons(port),
                                  .sin6_addr = in6addr_any };
        rc = bind(fd, (struct sockaddr *)&a, sizeof a);
    } else {
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                                 .sin_addr.s_addr = htonl(INADDR_ANY) };
        rc = bind(fd, (struct sockaddr *)&a, sizeof a);
    }
    if (rc < 0 || listen(fd, backlog) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int set_nonblock(int fd, int on)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0)
        return -1;
    fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, fl);
}

int tcp_connect_timeout(const char *host, uint16_t port, int timeout_ms)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res, *ai;
    char ports[8];
    snprintf(ports, sizeof ports, "%u", port);
    int gai = getaddrinfo(host, ports, &hints, &res);
    if (gai != 0) {
        LOGD("getaddrinfo(%s): %s", host, gai_strerror(gai));
        errno = EHOSTUNREACH;
        return -1;
    }
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        set_nonblock(fd, 1);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            rc = poll(&p, 1, timeout_ms);
            if (rc == 1) {
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                rc = err ? -1 : 0;
                if (err)
                    errno = err;
            } else {
                if (rc == 0)
                    errno = ETIMEDOUT;
                rc = -1;
            }
        }
        if (rc == 0) {
            set_nonblock(fd, 0);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   /* interactive keys */
            break;
        }
        int e = errno;
        close(fd);
        errno = e;
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int set_keepalive(int fd, int idle_s, int intvl_s, int cnt)
{
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one) < 0)
        return -1;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_s, sizeof idle_s);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl_s, sizeof intvl_s);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    return 0;
}

int set_io_timeouts(int fd, int rcv_s, int snd_s)
{
    int rc = 0;
    if (rcv_s > 0) {
        struct timeval tv = { .tv_sec = rcv_s };
        rc |= setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    if (snd_s > 0) {
        struct timeval tv = { .tv_sec = snd_s };
        rc |= setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    return rc;
}

char *sock_peer_str(int fd, char *buf, size_t n)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    char host[INET6_ADDRSTRLEN] = "?";
    unsigned port = 0;
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) == 0) {
        if (ss.ss_family == AF_INET) {
            struct sockaddr_in *a = (struct sockaddr_in *)&ss;
            inet_ntop(AF_INET, &a->sin_addr, host, sizeof host);
            port = ntohs(a->sin_port);
        } else if (ss.ss_family == AF_INET6) {
            struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
            if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr))
                inet_ntop(AF_INET, &a->sin6_addr.s6_addr[12], host, sizeof host);
            else
                inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof host);
            port = ntohs(a->sin6_port);
        }
    }
    snprintf(buf, n, "%s:%u", host, port);
    return buf;
}
