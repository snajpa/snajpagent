/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_AV_H
#define SNAJPAGENT_AV_H
#include "base.h"

/* File decoding only. Capture/realtime do not depend on this host adapter.
 * Append 24 kHz stereo signed little-endian PCM; reject >2 source channels.
 * The buffer is unchanged on failure. 2 means explicit cancellation. */
int snag_av_pcm(const char *, uint64_t, uint64_t, struct snag_buf *,
                int (*)(void *, unsigned int), void *, char *, size_t);
/* Pull playback PCM in native-endian s16 stereo. Caller supplies 16384 samples.
 * 0 frames means EOF; 2 means cancel. Earlier played chunks cannot be rolled back. */
struct snag_av_audio;
int snag_av_audio_open(const char *, uint64_t, uint64_t, int (*)(void *, unsigned int), void *,
                       struct snag_av_audio **, char *, size_t);
int snag_av_audio_read(struct snag_av_audio *, int16_t *, uint32_t *, char *, size_t);
void snag_av_audio_close(struct snag_av_audio *);
struct snag_image_crop { uint32_t x, y, width, height; };
/* Zero-based animation frame (0..999); crop in source pixels before orientation.
 * Bounded RGBA PNG <=1600px, appended atomically on success. */
int snag_av_image(const char *, uint32_t, const struct snag_image_crop *, struct snag_buf *,
                   char *, size_t, int (*)(void *, unsigned int), void *, char *, size_t);
struct snag_av_video;
struct snag_av_video_info { double duration; bool has_audio; };
int snag_av_video_open(const char *, int (*)(void *, unsigned int), void *,
                       struct snag_av_video **, struct snag_av_video_info *, char *, size_t);
/* Select first frame at/after target, or last frame before end at EOF.
 * Source PTS and applied orthogonal display transform are returned separately. */
int snag_av_video_frame(struct snag_av_video *, double, double, double,
                        struct snag_buf *, double *, char *, size_t, char *, size_t);
void snag_av_video_close(struct snag_av_video *);
#endif
