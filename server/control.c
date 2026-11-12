/*
 * control.c — who may type into the UART (§5.2, §5.3, R6).
 *
 * Arbitration is a single field, shm->ctrl_pid, changed only under the lock:
 *   TAKE     free (or holder is dead: kill(pid,0) == ESRCH) -> me; held by me -> granted again;
 *            held by someone alive -> denied with holder name / idle time / auto-release time.
 *            No queueing, no forced takeover.
 *   release  explicit RELEASE, idle timeout (reader checks every second), or disconnect
 *            (child exit path, or the master's SIGCHLD reaper for a child that crashed).
 * Every take/release is written into the ring as an event so all viewers see it in order.
 *
 * With --unsafe=ctrl-release only explicit RELEASE frees control: a controller that crashes
 * leaves its dead pid behind and nobody can ever type again.
 */
#include "server.h"
#include "bench_log.h"
#include "bench_util.h"

#include <errno.h>
#include <signal.h>
#include <string.h>

uint32_t ctrl_idle_sec(const struct bench_shm *s, uint64_t now)
{
    if (!s->ctrl_pid || now < s->ctrl_last_input_ns)
        return 0;
    return (uint32_t)((now - s->ctrl_last_input_ns) / 1000000000ull);
}

uint32_t ctrl_auto_release_in(const struct bench_shm *s, uint64_t now)
{
    if (!s->ctrl_pid || UNSAFE(R_CTRL_RELEASE) || !s->idle_release_sec)
        return UINT32_MAX;
    uint32_t idle = ctrl_idle_sec(s, now);
    return idle >= s->idle_release_sec ? 0 : s->idle_release_sec - idle;
}

int ctrl_take_locked(struct bench_shm *s, pid_t me, const char *name, struct bn_ctrl_result *res)
{
    uint64_t now = now_ns();
    memset(res, 0, sizeof *res);

    if (s->ctrl_pid && s->ctrl_pid != me && !UNSAFE(R_CTRL_RELEASE)) {
        /* holder may have died without anybody noticing yet */
        if (kill(s->ctrl_pid, 0) < 0 && errno == ESRCH) {
            ring_event(s, EV_CTRL_RELEASED, "control released by %s (disconnect)", s->ctrl_name);
            s->ctrl_pid = 0;
            s->ctrl_name[0] = 0;
        }
    }

    if (s->ctrl_pid == 0 || s->ctrl_pid == me) {
        int fresh = s->ctrl_pid == 0;
        s->ctrl_pid = me;
        bench_strlcpy(s->ctrl_name, name, sizeof s->ctrl_name);
        s->ctrl_last_input_ns = now;
        if (fresh)
            ring_event(s, EV_CTRL_TAKEN, "control taken by %s", name);
        res->granted = 1;
        bench_strlcpy(res->holder, name, sizeof res->holder);
        res->auto_release_in_sec = htobe32(ctrl_auto_release_in(s, now));
        return 1;
    }
    res->granted = 0;
    bench_strlcpy(res->holder, s->ctrl_name, sizeof res->holder);
    res->holder_idle_sec = htobe32(ctrl_idle_sec(s, now));
    res->auto_release_in_sec = htobe32(ctrl_auto_release_in(s, now));
    return 0;
}

void ctrl_release_locked(struct bench_shm *s, const char *reason, struct ctrl_note *note)
{
    if (!s->ctrl_pid)
        return;
    ring_event(s, EV_CTRL_RELEASED, "control released by %s (%s)", s->ctrl_name, reason);
    if (note) {
        note->happened = 1;
        bench_strlcpy(note->who, s->ctrl_name, sizeof note->who);
        bench_strlcpy(note->reason, reason, sizeof note->reason);
    }
    s->ctrl_pid = 0;
    s->ctrl_name[0] = 0;
}

void ctrl_check_idle_locked(struct bench_shm *s, uint64_t now, struct ctrl_note *note)
{
    if (!s->ctrl_pid || UNSAFE(R_CTRL_RELEASE) || !s->idle_release_sec)
        return;
    if (now > s->ctrl_last_input_ns &&
        now - s->ctrl_last_input_ns > (uint64_t)s->idle_release_sec * 1000000000ull)
        ctrl_release_locked(s, "idle", note);
}

void ctrl_log_note(const struct ctrl_note *note)
{
    if (note->happened)
        LOGI("control released by %s (%s)", note->who, note->reason);
}
