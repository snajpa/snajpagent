/* SPDX-License-Identifier: GPL-2.0-only */
#include "pcm.h"
#include <limits.h>
#include <string.h>

int
snag_pcm_init(struct snag_pcm *ring, int16_t *samples, uint32_t capacity)
{
    ring->samples = samples; ring->capacity = capacity;
    atomic_init(&ring->read, 0u); atomic_init(&ring->written, 0u); atomic_init(&ring->discard, 0u);
    atomic_init(&ring->flush_seq, 0u); ring->seen_flush = 0u;
    return samples && capacity && !(capacity & (capacity - 1u)) && capacity < INT32_MAX && UINT_MAX == UINT32_MAX &&
        atomic_is_lock_free(&ring->read) ? 0 : -1;
}

uint32_t
snag_pcm_available(const struct snag_pcm *ring)
{
    uint32_t written = atomic_load_explicit(&ring->written, memory_order_acquire);
    uint32_t read = atomic_load_explicit(&ring->read, memory_order_acquire);
    uint32_t available = written - read;
    return available <= ring->capacity ? available : 0;
}

uint32_t
snag_pcm_write(struct snag_pcm *ring, const int16_t *samples, uint32_t count)
{
    uint32_t written = atomic_load_explicit(&ring->written, memory_order_relaxed);
    uint32_t read = atomic_load_explicit(&ring->read, memory_order_acquire);
    uint32_t used = written - read;
    if (used > ring->capacity) return 0;
    if (count > ring->capacity - used) count = ring->capacity - used;
    uint32_t at = written % ring->capacity, first = ring->capacity - at;
    if (first > count) first = count;
    if (first) memcpy(ring->samples + at, samples, (size_t)first * sizeof(*samples));
    if (count > first) memcpy(ring->samples, samples + first, (size_t)(count - first) * sizeof(*samples));
    atomic_store_explicit(&ring->written, written + count, memory_order_release);
    return count;
}

uint32_t
snag_pcm_read(struct snag_pcm *ring, int16_t *samples, uint32_t count)
{
    uint32_t read = atomic_load_explicit(&ring->read, memory_order_relaxed);
    uint32_t seq = atomic_load_explicit(&ring->flush_seq, memory_order_acquire);
    if (seq != ring->seen_flush) {
        uint32_t discard = atomic_load_explicit(&ring->discard, memory_order_acquire);
        if (discard - read <= ring->capacity) read = discard;
        ring->seen_flush = seq;
    }
    uint32_t written = atomic_load_explicit(&ring->written, memory_order_acquire);
    uint32_t used = written - read;
    if (used > ring->capacity) return 0;
    if (count > used) count = used;
    uint32_t at = read % ring->capacity, first = ring->capacity - at;
    if (first > count) first = count;
    if (first && samples) memcpy(samples, ring->samples + at, (size_t)first * sizeof(*samples));
    if (count > first && samples) memcpy(samples + first, ring->samples, (size_t)(count - first) * sizeof(*samples));
    atomic_store_explicit(&ring->read, read + count, memory_order_release);
    return count;
}

void snag_pcm_flush(struct snag_pcm *ring)
{
    atomic_store_explicit(&ring->discard, atomic_load_explicit(&ring->written, memory_order_acquire), memory_order_release);
    atomic_fetch_add_explicit(&ring->flush_seq, 1u, memory_order_release);
}
void snag_pcm_discard(struct snag_pcm *ring)
{
    (void)snag_pcm_read(ring, NULL, ring->capacity);
}

void snag_pcm_playout_init(struct snag_pcm_playout *p, uint32_t step)
{
    memset(p, 0, sizeof(*p));
    atomic_init(&p->ended, 0u); atomic_init(&p->gaps, 0u);
    p->step = step; p->target = 3u * step; p->maximum = 10u * step;
    p->waiting = true;
}

void snag_pcm_playout_end(struct snag_pcm_playout *p, const struct snag_pcm *ring)
{
    atomic_store_explicit(&p->ended, atomic_load_explicit(&ring->written, memory_order_acquire), memory_order_release);
}

uint32_t snag_pcm_playout_read(struct snag_pcm_playout *p, struct snag_pcm *ring,
                               int16_t *samples, uint32_t count)
{
    /* Apply pending cancellation even while waiting for prefill. */
    (void)snag_pcm_read(ring, NULL, 0u);
    if(p->seen_flush != ring->seen_flush) {
        p->seen_flush = ring->seen_flush;
        p->started = p->incident = p->starved = false;
        p->waiting = true; p->waited = 0;
    }
    uint32_t end = atomic_load_explicit(&p->ended, memory_order_acquire);
    uint32_t written = atomic_load_explicit(&ring->written, memory_order_acquire);
    uint32_t read = atomic_load_explicit(&ring->read, memory_order_relaxed);
    uint32_t available = written - read;
    if(available > ring->capacity || !count)return 0;
    bool finished = written == end;
    if(!available) {
        if(finished) {
            if(p->started && !p->starved && ++p->clean == 8u) {
                if(p->target > 3u * p->step)p->target -= p->step;
                p->clean = 0;
            }
            p->started = p->incident = p->starved = false;
        } else if(p->started && !p->waiting) {
            p->incident = true;
        }
        p->waiting = true; p->waited = 0;
        return 0;
    }
    if(p->incident) {
        /* New samples prove a gap was mid-stream, not merely delayed EOF.
         * Count once per gap; never enlarge capture or the allocation. */
        if(p->target < p->maximum)p->target += p->step;
        atomic_fetch_add_explicit(&p->gaps, 1u, memory_order_relaxed);
        p->incident = false; p->starved = true; p->clean = 0;
    }
    p->started = true;
    if(p->waiting && !finished && available < p->target && p->waited < p->maximum) {
        p->waited += count < p->maximum - p->waited ? count : p->maximum - p->waited;
        return 0;
    }
    p->waiting = false; p->waited = 0;
    uint32_t n = snag_pcm_read(ring, samples, count);
    if(n < count && !finished) {
        p->waiting = true; p->incident = true;
    }
    return n;
}
