/*
 * robust.c — the shared-memory lock (R7), slot helpers and the --unsafe banner.
 *
 * About the mutex: pthread_mutex_t here is configured PTHREAD_PROCESS_SHARED and lives inside
 * the MAP_SHARED mapping, so it synchronises *processes*. It is a synchronisation primitive,
 * not a thread — benchd never creates a thread: no pthread_create anywhere, see docs/DECISIONS.md.
 *
 * R7 robust-mutex: with PTHREAD_MUTEX_ROBUST, if a process dies while holding the lock the next
 * locker gets EOWNERDEAD instead of blocking forever. We then re-validate the ring (the dead
 * owner may have been half way through an update), mark the mutex consistent and continue.
 * With --unsafe=robust-mutex the mutex is a plain one and a death inside the critical section
 * freezes every process of the server.
 */
#include "server.h"
#include "bench_log.h"
#include "bench_util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int pending_recovery_log;   /* log after unlocking: no I/O inside the critical section */
static int pending_repaired;

void shm_mutex_init(pthread_mutex_t *m)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    if (!UNSAFE(R_ROBUST_MUTEX))
        pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}

void shm_lock(struct bench_shm *s)
{
    int rc = pthread_mutex_lock(&s->lock);
    if (rc == EOWNERDEAD) {
        /* The previous owner died inside the critical section. Shared state may be half
         * updated: re-check the ring and the control fields before trusting them again. */
        pending_repaired = ring_repair(s);
        if (s->ctrl_pid && kill(s->ctrl_pid, 0) < 0 && errno == ESRCH) {
            s->ctrl_pid = 0;
            s->ctrl_name[0] = 0;
        }
        slots_recount(s);
        s->lock_recoveries++;
        pthread_mutex_consistent(&s->lock);
        pending_recovery_log = 1;
    } else if (rc != 0) {
        LOGE("pthread_mutex_lock: %s", strerror(rc));
        abort();
    }
}

void shm_unlock(struct bench_shm *s)
{
    pthread_mutex_unlock(&s->lock);
    if (pending_recovery_log) {
        pending_recovery_log = 0;
        LOGW("EOWNERDEAD recovered: previous lock owner died inside the critical section%s",
             pending_repaired ? " (ring was inconsistent: tail reset to head)" : " (ring consistent)");
    }
}

uint32_t slots_recount(struct bench_shm *s)
{
    uint32_t v = 0;
    for (int i = 0; i < BENCH_MAX_SLOTS; i++)
        if (s->slots[i].pid && s->slots[i].joined)
            v++;
    s->viewers = v;
    return v;
}

int slots_in_use(struct bench_shm *s)
{
    int n = 0;
    for (int i = 0; i < BENCH_MAX_SLOTS; i++)
        if (s->slots[i].pid)
            n++;
    return n;
}

void print_unsafe_banner(void)
{
    if (!bench_unsafe_mask)
        return;
    char list[256];
    bench_unsafe_format(bench_unsafe_mask, list, sizeof list);
    int tty = isatty(STDERR_FILENO);
    fprintf(stderr, "%s"
            "############################################################\n"
            "#  UNSAFE MODE: robustness mechanisms DISABLED:            \n"
            "#    %s\n"
            "#  (for before/after demonstrations only)                  \n"
            "############################################################%s\n",
            tty ? "\033[1;97;41m" : "", list, tty ? "\033[0m" : "");
}
