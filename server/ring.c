/*
 * ring.c — the shared history ring buffer (§3.4).
 *
 * Every byte from the UART and every event (device lost, control taken, ...) becomes a record
 * appended at `head`. Offsets are logical and only grow; the physical position is offset % cap,
 * so a record may wrap around the end of the array — ring_put/ring_get copy in two pieces.
 * When there is no room, whole records are evicted from `tail`.
 *
 * Broadcast needs no child-to-child IPC: each child keeps its own read offset and copies
 * [my_offset, head) out under the lock. Data and events are therefore seen in the same order
 * by every viewer, including during history replay.
 *
 * Crash safety: a writer publishes a record by advancing `head` LAST, so a process dying in the
 * middle of ring_append leaves at most an invisible half-written record past head. ring_repair()
 * (called after EOWNERDEAD) re-validates the chain from tail to head anyway.
 */
#include "server.h"
#include "bench_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void ring_put(struct bench_shm *s, uint64_t off, const void *src, size_t n)
{
    uint64_t pos = off % s->cap;
    size_t first = (size_t)(s->cap - pos) < n ? (size_t)(s->cap - pos) : n;
    memcpy(s->ring + pos, src, first);
    if (n > first)
        memcpy(s->ring, (const uint8_t *)src + first, n - first);
}

static void ring_get(const struct bench_shm *s, uint64_t off, void *dst, size_t n)
{
    uint64_t pos = off % s->cap;
    size_t first = (size_t)(s->cap - pos) < n ? (size_t)(s->cap - pos) : n;
    memcpy(dst, s->ring + pos, first);
    if (n > first)
        memcpy((uint8_t *)dst + first, s->ring, n - first);
}

void ring_init(struct bench_shm *s, uint64_t cap)
{
    s->cap = cap;
    s->head = s->tail = 0;
}

/* Append one record, evicting the oldest records if needed. Lock must be held. */
void ring_append(struct bench_shm *s, uint16_t type, uint16_t code, const void *data, uint32_t len)
{
    if (len > REC_MAX_DATA)
        len = REC_MAX_DATA;
    uint64_t size = REC_SIZE(len);

    while (s->head + size - s->tail > s->cap) {
        struct ring_rec old;
        ring_get(s, s->tail, &old, sizeof old);
        uint64_t osz = REC_SIZE(old.len);
        if (old.len > REC_MAX_DATA || s->tail + osz > s->head) {
            s->tail = s->head;           /* chain is broken: drop everything */
            break;
        }
        s->tail += osz;
    }

    struct ring_rec r = { .len = len, .type = type, .code = code, .ts_ns = now_ns() };
    ring_put(s, s->head, &r, sizeof r);
    if (len)
        ring_put(s, s->head + sizeof r, data, len);
    s->head += size;                     /* publish */
}

void ring_event(struct bench_shm *s, uint16_t code, const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if (n >= (int)sizeof text)
        n = sizeof text - 1;
    ring_append(s, REC_EVENT, code, text, (uint32_t)n);
}

/* First record boundary >= target (walking from tail). Lock must be held. */
uint64_t ring_seek(struct bench_shm *s, uint64_t target)
{
    uint64_t off = s->tail;
    while (off < target && off < s->head) {
        struct ring_rec r;
        ring_get(s, off, &r, sizeof r);
        if (r.len > REC_MAX_DATA)
            return s->head;
        off += REC_SIZE(r.len);
    }
    return off > s->head ? s->head : off;
}

/*
 * Copy whole records starting at *off (up to `end`) into out[], at most outcap bytes, and
 * advance *off. Records keep their in-ring layout (header, data, padding). Lock must be held.
 *
 * With --unsafe=overrun the caller does not check *off < tail first, so *off may point into
 * bytes that were already overwritten: the "header" we read is then garbage. The naive code
 * just clamps the length and carries on, which shows up as scrambled output on screen.
 */
size_t ring_copy(struct bench_shm *s, uint64_t *off, uint64_t end, uint8_t *out, size_t outcap)
{
    size_t n = 0;
    uint64_t o = *off;
    while (o < end) {
        struct ring_rec r;
        ring_get(s, o, &r, sizeof r);
        uint32_t len = r.len;
        if (len > REC_MAX_DATA) {
            if (!UNSAFE(R_OVERRUN)) {     /* corrupt chain: skip to the end */
                o = end;
                break;
            }
            len = REC_MAX_DATA;
        }
        uint64_t rs = REC_SIZE(len);
        if (o + rs > end) {
            if (n == 0)
                o = end;                 /* cannot make progress (only reachable when unsafe) */
            break;
        }
        if (n + rs > outcap)
            break;
        ring_get(s, o, out + n, (size_t)rs);
        ((struct ring_rec *)(out + n))->len = len;
        n += (size_t)rs;
        o += rs;
    }
    *off = o;
    return n;
}

/* Validate tail..head after a lock owner died. Returns 1 if something had to be fixed. */
int ring_repair(struct bench_shm *s)
{
    if (s->tail > s->head || s->head - s->tail > s->cap) {
        s->tail = s->head;
        return 1;
    }
    uint64_t off = s->tail;
    while (off < s->head) {
        struct ring_rec r;
        ring_get(s, off, &r, sizeof r);
        if (r.len > REC_MAX_DATA || (r.type != REC_DATA && r.type != REC_EVENT) ||
            off + REC_SIZE(r.len) > s->head) {
            s->tail = s->head;
            return 1;
        }
        off += REC_SIZE(r.len);
    }
    return 0;
}
