/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_AUDIO_DEVICE_H
#define SNAJPAGENT_AUDIO_DEVICE_H
#include "base.h"
struct snag_audio_device;
/* Device owner alone calls lifecycle and ring APIs. Callbacks move samples only.
 * 24 kHz native s16; capture mono, playback mono or stereo. No capture on open
 * unless capture=true. Duplex voice opens with forwarding muted; its owner
 * explicitly unmutes after UI/control checks. Selection is an exact device
 * name, empty means default. */
int snag_audio_devices(struct snag_buf *, char *, size_t);
int snag_audio_open(bool capture, bool playback, unsigned int channels,
                     const char *, const char *, struct snag_audio_device **, char *, size_t);
void snag_audio_close(struct snag_audio_device *);
uint32_t snag_audio_capture(struct snag_audio_device *, int16_t *, uint32_t);
uint32_t snag_audio_play(struct snag_audio_device *, const int16_t *, uint32_t);
void snag_audio_interrupt(struct snag_audio_device *);
/* Publish the end of a voice audio item so short/tail audio drains promptly. */
void snag_audio_finish(struct snag_audio_device *);
uint32_t snag_audio_gaps(const struct snag_audio_device *);
/* Muting gates forwarding immediately. Unmute returns 1 until the callback
 * acknowledges mute, then discards old capture and admits only fresh samples. */
int snag_audio_mute(struct snag_audio_device *, bool);
int snag_audio_fault(const struct snag_audio_device *);
uint32_t snag_audio_pending(const struct snag_audio_device *);
uint32_t snag_audio_delivered(const struct snag_audio_device *);
uint32_t snag_audio_latency_ms(const struct snag_audio_device *);
#ifdef SNAJPAGENT_TEST_TRANSPORT_ENDPOINTS
int snag_audio_fixture_capture(void);
#endif
#endif
