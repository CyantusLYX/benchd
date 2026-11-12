/*
 * bench_net.h — socket helpers shared by client and server (part of libbench.so).
 */
#ifndef BENCH_NET_H
#define BENCH_NET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Read/write exactly n bytes; restart on EINTR; handle partial I/O.
 * readn returns n, a short count on EOF, or -1 on error. writen returns n or -1. */
ssize_t readn(int fd, void *buf, size_t n);
ssize_t writen(int fd, const void *buf, size_t n);

int  tcp_listen(uint16_t port, int backlog);                              /* SO_REUSEADDR set */
int  tcp_connect_timeout(const char *host, uint16_t port, int timeout_ms); /* -1 on failure */
int  set_nonblock(int fd, int on);
int  set_keepalive(int fd, int idle_s, int intvl_s, int cnt);
int  set_io_timeouts(int fd, int rcv_s, int snd_s);                        /* 0 = unchanged */
char *sock_peer_str(int fd, char *buf, size_t n);                          /* "1.2.3.4:5678" */

#endif
