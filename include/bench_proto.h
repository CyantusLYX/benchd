/*
 * bench_proto.h — the benchd wire protocol (§6). Shared by every executable via libbench.so.
 *
 * Every packet = 12-byte header + payload. All multi-byte fields are network byte order.
 *
 *   0       2     3      4               8              12
 *   +-------+-----+------+---------------+---------------+------------
 *   | magic | ver | type |      len      |     crc32     | payload ...
 *   | 'BN'  |  1  |      | payload bytes | of payload    |
 *   +-------+-----+------+---------------+---------------+------------
 *
 * Stream offsets: DATA/EVENT carry the logical ring offset at the END of that record (i.e. the
 * offset where the next record starts). A client therefore resumes from "the last offset it saw".
 */
#ifndef BENCH_PROTO_H
#define BENCH_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <endian.h>

#define BN_MAGIC        0x424E      /* 'B''N' */
#define BN_VERSION      1
#define BN_HDR_LEN      12
#define BN_MAX_PAYLOAD  65536
#define BN_DEFAULT_PORT 9000
#define BN_MAX_INPUT    256
#define BN_NAME_LEN     32
#define BENCH_VERSION   "1.0.0"

enum bn_type {
    /* client -> server */
    BN_HELLO = 0x01, BN_INPUT = 0x02, BN_TAKE = 0x03, BN_RELEASE = 0x04,
    BN_INFO_REQ = 0x05, BN_BAUD = 0x06, BN_PING = 0x07, BN_BYE = 0x08,
    /* server -> client */
    BN_WELCOME = 0x81, BN_DATA = 0x82, BN_EVENT = 0x83, BN_REPLAY_BEGIN = 0x84,
    BN_REPLAY_END = 0x85, BN_CTRL_RESULT = 0x86, BN_INFO = 0x87, BN_STATUS = 0x88,
    BN_ERROR = 0x89, BN_PONG = 0x8A,
};

enum bn_err {
    BN_ERR_BAD_MAGIC = 1, BN_ERR_BAD_VERSION = 2, BN_ERR_BAD_LENGTH = 3, BN_ERR_BAD_CRC = 4,
    BN_ERR_BAD_TYPE = 5, BN_ERR_NOT_CONTROLLER = 6, BN_ERR_DEVICE_LOST = 7, BN_ERR_SERVER_BUSY = 8,
    BN_ERR_HANDSHAKE_TIMEOUT = 9, BN_ERR_IDLE_TIMEOUT = 10, BN_ERR_SERVER_SHUTDOWN = 11,
    BN_ERR_BAD_REQUEST = 12,   /* extension: request refused, connection stays open */
};

enum bn_evt {
    EV_DEVICE_LOST = 1, EV_DEVICE_BACK = 2, EV_CTRL_TAKEN = 3, EV_CTRL_RELEASED = 4,
    EV_BAUD_CHANGED = 5, EV_DROPPED = 6, EV_SERVER_SHUTDOWN = 7,
};

enum { DEV_LOST = 0, DEV_UP = 1 };

/* Decoded header (host order). */
struct bn_hdr {
    uint16_t magic;
    uint8_t  ver;
    uint8_t  type;
    uint32_t len;
    uint32_t crc32;
};

/* ---- payload layouts (wire format, packed, big-endian fields) ---- */
#define BN_PACKED __attribute__((packed))

struct BN_PACKED bn_hello {
    char     name[BN_NAME_LEN];
    uint32_t replay_bytes;
    uint8_t  has_resume;
    uint8_t  pad[3];
    uint64_t resume_offset;
    uint64_t last_server_start_ns;
};

struct BN_PACKED bn_welcome {
    uint8_t  proto_ver;
    uint8_t  dev_state;
    uint16_t pad;
    uint32_t baud;
    uint32_t viewers;
    uint32_t history_cap;
    uint64_t head;
    uint64_t server_start_ns;
    char     dev_path[128];
    char     ctrl_name[32];
    char     server_version[16];
};

struct BN_PACKED bn_data_hdr {      /* DATA: followed by raw bytes */
    uint64_t offset;
    uint64_t ts_ns;
};

struct BN_PACKED bn_event_hdr {     /* EVENT: followed by UTF-8 text */
    uint64_t offset;
    uint64_t ts_ns;
    uint16_t code;
    uint16_t pad;
};

struct BN_PACKED bn_replay_begin { uint64_t from_offset; uint64_t dropped_bytes; };
struct BN_PACKED bn_replay_end   { uint64_t head; };

struct BN_PACKED bn_ctrl_result {
    uint8_t  granted;
    uint8_t  pad[3];
    uint32_t holder_idle_sec;
    uint32_t auto_release_in_sec;   /* UINT32_MAX = never */
    char     holder[32];
};

struct BN_PACKED bn_status {
    uint8_t  dev_state;
    uint8_t  you_are_ctrl;
    uint16_t pad;
    uint32_t baud;
    uint32_t viewers;
    uint32_t ctrl_idle_sec;
    uint64_t rx_bytes;
    char     ctrl_name[32];
};

struct BN_PACKED bn_error_hdr { uint16_t code; uint16_t pad; };   /* followed by text */
struct BN_PACKED bn_ping      { uint64_t client_ts_ns; };
struct BN_PACKED bn_pong      { uint64_t client_ts_ns; uint64_t server_ts_ns; };

/* ---- CRC32 (IEEE 802.3, reflected 0xEDB88320) ---- */
uint32_t bench_crc32(const void *data, size_t len);

/* ---- framing ---- */
void bn_hdr_encode(uint8_t out[BN_HDR_LEN], uint8_t type, uint32_t len, uint32_t crc);
void bn_hdr_decode(const uint8_t in[BN_HDR_LEN], struct bn_hdr *h);

/* Send one packet (header + payload in a single write). 0 ok, -1 error (errno). */
int bn_send(int fd, uint8_t type, const void *payload, uint32_t len);
int bn_send_error(int fd, uint16_t code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Receive one packet into buf (capacity cap).
 *  1  packet ok (h filled, payload in buf)
 *  0  orderly EOF
 * -1  I/O error (errno)
 * -2  protocol error, *perr = BN_ERR_BAD_MAGIC/VERSION/LENGTH/CRC
 * Honors --unsafe=readn (single recv()) and --unsafe=validate (no checks). */
int bn_recv(int fd, struct bn_hdr *h, void *buf, size_t cap, int *perr);

const char *bn_type_name(int type);
const char *bn_err_name(int code);
const char *bn_evt_name(int code);

#endif
