/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_PCM_H
#define SNAJPAGENT_PCM_H
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
/* SPSC signed native-endian samples. Each cursor has exactly one writer.
 * Caller owns storage; power-of-two capacity < 2^31. No allocation, OS or device dependency. */
struct snag_pcm {
    int16_t *samples;
    uint32_t capacity;
    atomic_uint read, written, discard, flush_seq;
    uint32_t seen_flush; /* consumer-only */
};
int snag_pcm_init(struct snag_pcm *, int16_t *, uint32_t);
uint32_t snag_pcm_write(struct snag_pcm *, const int16_t *, uint32_t);
uint32_t snag_pcm_read(struct snag_pcm *, int16_t *, uint32_t);
uint32_t snag_pcm_available(const struct snag_pcm *);
/* Producer requests a flush; only the consumer advances its read cursor. */
void snag_pcm_flush(struct snag_pcm *);
/* Consumer discards all currently published samples. */
void snag_pcm_discard(struct snag_pcm *);
/* Optional streaming playback prefill. Producer publishes an end cursor after
 * the final write; all other state belongs to the consumer. No extra storage. */
struct snag_pcm_playout {
    atomic_uint ended, gaps;
    uint32_t step, target, maximum, waited, clean, seen_flush;
    bool started, waiting, incident, starved;
};
void snag_pcm_playout_init(struct snag_pcm_playout *, uint32_t samples_per_20ms);
void snag_pcm_playout_end(struct snag_pcm_playout *, const struct snag_pcm *);
uint32_t snag_pcm_playout_read(struct snag_pcm_playout *, struct snag_pcm *, int16_t *, uint32_t);
#endif
