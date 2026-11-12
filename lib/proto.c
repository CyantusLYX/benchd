/*
 * proto.c — header encode/decode, bn_send/bn_recv with full validation (magic, version,
 * length, CRC), and type/error/event names. TRACE level prints every packet header so the
 * log can be lined up against a tcpdump capture.
 */
#include "bench_proto.h"
#include "bench_net.h"
#include "bench_log.h"
#include "bench_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

void bn_hdr_encode(uint8_t out[BN_HDR_LEN], uint8_t type, uint32_t len, uint32_t crc)
{
    uint16_t m = htons(BN_MAGIC);
    uint32_t l = htonl(len), c = htonl(crc);
    memcpy(out, &m, 2);
    out[2] = BN_VERSION;
    out[3] = type;
    memcpy(out + 4, &l, 4);
    memcpy(out + 8, &c, 4);
}

void bn_hdr_decode(const uint8_t in[BN_HDR_LEN], struct bn_hdr *h)
{
    uint16_t m;
    uint32_t l, c;
    memcpy(&m, in, 2);
    memcpy(&l, in + 4, 4);
    memcpy(&c, in + 8, 4);
    h->magic = ntohs(m);
    h->ver = in[2];
    h->type = in[3];
    h->len = ntohl(l);
    h->crc32 = ntohl(c);
}

/* One buffer for header+payload so each packet is a single write() (no threads -> static ok). */
static uint8_t txbuf[BN_HDR_LEN + BN_MAX_PAYLOAD];

int bn_send(int fd, uint8_t type, const void *payload, uint32_t len)
{
    if (len > BN_MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    uint32_t crc = len ? bench_crc32(payload, len) : 0;
    bn_hdr_encode(txbuf, type, len, crc);
    if (len)
        memcpy(txbuf + BN_HDR_LEN, payload, len);
    LOGT("tx pkt type=%s(0x%02x) len=%u crc=%08x", bn_type_name(type), type, len, crc);
    return writen(fd, txbuf, BN_HDR_LEN + len) < 0 ? -1 : 0;
}

int bn_send_error(int fd, uint16_t code, const char *fmt, ...)
{
    uint8_t buf[sizeof(struct bn_error_hdr) + 256];
    struct bn_error_hdr *e = (struct bn_error_hdr *)buf;
    e->code = htons(code);
    e->pad = 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf((char *)buf + sizeof *e, 256, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if (n > 255)
        n = 255;
    return bn_send(fd, BN_ERROR, buf, (uint32_t)(sizeof *e + (size_t)n));
}

int bn_recv(int fd, struct bn_hdr *h, void *buf, size_t cap, int *perr)
{
    uint8_t raw[BN_HDR_LEN];
    ssize_t n;
    int dummy;
    if (!perr)
        perr = &dummy;
    *perr = 0;

    if (UNSAFE(R_READN)) {
        /* naive: assume one recv() returns the whole header */
        memset(raw, 0, sizeof raw);
        n = recv(fd, raw, sizeof raw, 0);
        if (n <= 0)
            return n == 0 ? 0 : -1;
    } else {
        n = readn(fd, raw, sizeof raw);
        if (n < 0)
            return -1;
        if (n < BN_HDR_LEN)
            return 0;
    }
    bn_hdr_decode(raw, h);

    if (!UNSAFE(R_VALIDATE)) {
        if (h->magic != BN_MAGIC) {
            *perr = BN_ERR_BAD_MAGIC;
            return -2;
        }
        if (h->ver != BN_VERSION) {
            *perr = BN_ERR_BAD_VERSION;
            return -2;
        }
        if (h->len > BN_MAX_PAYLOAD || h->len > cap) {
            *perr = BN_ERR_BAD_LENGTH;
            return -2;
        }
    }
    /* With --unsafe=validate we trust h->len blindly: a 1 MiB len overflows the caller's buffer. */
    if (h->len) {
        if (UNSAFE(R_READN))
            n = recv(fd, buf, h->len, 0);
        else
            n = readn(fd, buf, h->len);
        if (n < 0)
            return -1;
        if (n == 0)
            return 0;
        if (!UNSAFE(R_READN) && (size_t)n < h->len)
            return 0;
    }
    if (!UNSAFE(R_VALIDATE)) {
        uint32_t crc = h->len ? bench_crc32(buf, h->len) : 0;
        if (crc != h->crc32) {
            *perr = BN_ERR_BAD_CRC;
            return -2;
        }
    }
    LOGT("rx pkt type=%s(0x%02x) len=%u crc=%08x", bn_type_name(h->type), h->type, h->len, h->crc32);
    return 1;
}

const char *bn_type_name(int t)
{
    switch (t) {
    case BN_HELLO: return "HELLO";
    case BN_INPUT: return "INPUT";
    case BN_TAKE: return "TAKE";
    case BN_RELEASE: return "RELEASE";
    case BN_INFO_REQ: return "INFO_REQ";
    case BN_BAUD: return "BAUD";
    case BN_PING: return "PING";
    case BN_BYE: return "BYE";
    case BN_WELCOME: return "WELCOME";
    case BN_DATA: return "DATA";
    case BN_EVENT: return "EVENT";
    case BN_REPLAY_BEGIN: return "REPLAY_BEGIN";
    case BN_REPLAY_END: return "REPLAY_END";
    case BN_CTRL_RESULT: return "CTRL_RESULT";
    case BN_INFO: return "INFO";
    case BN_STATUS: return "STATUS";
    case BN_ERROR: return "ERROR";
    case BN_PONG: return "PONG";
    default: return "?";
    }
}

const char *bn_err_name(int c)
{
    static const char *n[] = { "OK", "BAD_MAGIC", "BAD_VERSION", "BAD_LENGTH", "BAD_CRC",
                               "BAD_TYPE", "NOT_CONTROLLER", "DEVICE_LOST", "SERVER_BUSY",
                               "HANDSHAKE_TIMEOUT", "IDLE_TIMEOUT", "SERVER_SHUTDOWN",
                               "BAD_REQUEST" };
    return (c >= 0 && c < (int)(sizeof n / sizeof n[0])) ? n[c] : "?";
}

const char *bn_evt_name(int c)
{
    static const char *n[] = { "?", "DEVICE_LOST", "DEVICE_BACK", "CTRL_TAKEN", "CTRL_RELEASED",
                               "BAUD_CHANGED", "DROPPED", "SERVER_SHUTDOWN" };
    return (c >= 0 && c < (int)(sizeof n / sizeof n[0])) ? n[c] : "?";
}
