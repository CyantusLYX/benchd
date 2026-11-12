/*
 * server.h — internal definitions shared by the benchd processes.
 *
 * Process model (§3.2):
 *   master  — owns the listening socket, forks one child per client, reaps children (SIGCHLD),
 *             releases control if a dead child held it, handles SIGUSR1 / shutdown.
 *   reader  — the ONLY process holding the serial fd. tty -> ring; input pipe -> tty.
 *   child   — one per TCP client: handshake, history replay, stream ring records, commands.
 *
 * IPC is exactly three things, all created before the first fork():
 *   1. struct bench_shm in an anonymous MAP_SHARED mapping (ring buffer + state),
 *   2. one process-shared robust pthread mutex inside it,
 *   3. one pipe (children -> reader) carrying controller input.
 */
#ifndef BENCH_SERVER_H
#define BENCH_SERVER_H

#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>

#include "bench_proto.h"

#define BENCH_MAX_SLOTS   1024
#define REC_DATA          1
#define REC_EVENT         2
#define REC_MAX_DATA      1024
#define HELLO_TIMEOUT_MS  5000
#define IDLE_TIMEOUT_MS   60000

/* One record in the ring (host byte order; never leaves the machine). */
struct ring_rec {
    uint32_t len;      /* payload bytes */
    uint16_t type;     /* REC_DATA / REC_EVENT */
    uint16_t code;     /* event code for REC_EVENT, 0 for DATA */
    uint64_t ts_ns;    /* CLOCK_REALTIME when written */
};
#define REC_SIZE(len) ((uint64_t)((sizeof(struct ring_rec) + (uint64_t)(len) + 7u) & ~(uint64_t)7u))

/* Per-client bookkeeping (lets the master count children and flush on shutdown). */
struct bench_slot {
    pid_t    pid;        /* 0 = free */
    int      joined;     /* handshake complete */
    char     name[BN_NAME_LEN];
    char     peer[64];
    uint64_t offset;     /* how far this child has streamed */
    uint64_t joined_ns;
};

struct bench_shm {
    /* NOTE: this is a *process-shared* mutex (PTHREAD_PROCESS_SHARED | PTHREAD_MUTEX_ROBUST)
     * used purely as an inter-process lock. benchd never creates threads. */
    pthread_mutex_t lock;
    /* ring buffer: logical offsets grow forever, physical position = offset % cap */
    uint64_t cap;
    uint64_t head;              /* where the next record is written */
    uint64_t tail;              /* oldest valid record */
    /* device */
    int      dev_state;         /* DEV_UP / DEV_LOST */
    char     dev_path[128];
    uint32_t baud;
    uint64_t rx_bytes, tx_bytes;
    uint32_t dev_reconnects;
    uint64_t last_rx_ns;
    /* control */
    pid_t    ctrl_pid;          /* 0 = nobody */
    char     ctrl_name[BN_NAME_LEN];
    uint64_t ctrl_last_input_ns;
    uint32_t idle_release_sec;
    /* stats & config */
    uint32_t viewers;           /* clients that finished the handshake */
    uint32_t max_clients;
    uint64_t server_start_ns;
    volatile int log_level;     /* shared runtime log level (SIGUSR1 changes it) */
    uint32_t unsafe_mask;
    pid_t    master_pid, reader_pid;
    uint32_t lock_recoveries;   /* EOWNERDEAD recoveries so far */
    struct bench_slot slots[BENCH_MAX_SLOTS];
    uint8_t  ring[];            /* cap bytes */
};

/* Message on the input pipe (children -> reader). Whole message <= PIPE_BUF: atomic write. */
enum { PIPE_INPUT = 1, PIPE_BAUD = 2, PIPE_RELEASE = 3 };
struct pipe_msg {
    int32_t  pid;
    uint16_t type;
    uint16_t len;
    uint8_t  data[BN_MAX_INPUT];
};
#define PIPE_MSG_HDR 8

struct server_cfg {
    const char *device;
    uint32_t baud;
    uint16_t port;
    uint64_t history;
    uint32_t max_clients;
    uint32_t idle_release;
    int      input_pipe[2];
    int      listen_fd;
    int      fault_die_in_lock_at;   /* debug builds: connection # that aborts inside the lock */
};

extern struct bench_shm *g_shm;
extern struct server_cfg g_cfg;

/* ring.c — all require the lock held */
void     ring_init(struct bench_shm *s, uint64_t cap);
void     ring_append(struct bench_shm *s, uint16_t type, uint16_t code, const void *data, uint32_t len);
void     ring_event(struct bench_shm *s, uint16_t code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
uint64_t ring_seek(struct bench_shm *s, uint64_t target);
size_t   ring_copy(struct bench_shm *s, uint64_t *off, uint64_t end, uint8_t *out, size_t outcap);
int      ring_repair(struct bench_shm *s);

/* robust.c */
void shm_mutex_init(pthread_mutex_t *m);
void shm_lock(struct bench_shm *s);
void shm_unlock(struct bench_shm *s);
void print_unsafe_banner(void);
uint32_t slots_recount(struct bench_shm *s);      /* lock held; updates s->viewers */
int  slots_in_use(struct bench_shm *s);           /* lock held */

/* control.c — lock held unless noted */
struct ctrl_note { int happened; char who[BN_NAME_LEN]; char reason[32]; };
int  ctrl_take_locked(struct bench_shm *s, pid_t me, const char *name, struct bn_ctrl_result *res);
void ctrl_release_locked(struct bench_shm *s, const char *reason, struct ctrl_note *note);
void ctrl_check_idle_locked(struct bench_shm *s, uint64_t now, struct ctrl_note *note);
uint32_t ctrl_idle_sec(const struct bench_shm *s, uint64_t now);
uint32_t ctrl_auto_release_in(const struct bench_shm *s, uint64_t now);
void ctrl_log_note(const struct ctrl_note *note);   /* call after unlocking */

/* sysinfo.c */
size_t sysinfo_build(char *buf, size_t cap);

/* reader.c / session.c */
void reader_main(void) __attribute__((noreturn));
void session_main(int fd, int slot, int conn_seq, const char *peer) __attribute__((noreturn));

/* baud helpers (reader.c) */
int baud_supported(uint32_t baud);

#endif
