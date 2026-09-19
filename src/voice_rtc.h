/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VOICE_RTC_H
#define SNAJPAGENT_VOICE_RTC_H
#include "base.h"
struct snag_voice_rtc;
int snag_voice_rtc_open(struct snag_voice_rtc **, char *, size_t);
int snag_voice_rtc_offer(struct snag_voice_rtc *, struct snag_buf *);
int snag_voice_rtc_answer(struct snag_voice_rtc *, const char *);
bool snag_voice_rtc_ready(struct snag_voice_rtc *);
int snag_voice_rtc_input(struct snag_voice_rtc *, const int16_t *, uint32_t);
/* Zero input frames discard partial capture at a mute boundary. */
/* Native media uses 48 kHz RTP timestamps and 24 kHz mono device PCM. */
int snag_voice_rtc_output(struct snag_voice_rtc *, int16_t *, uint32_t);
void snag_voice_rtc_flush(struct snag_voice_rtc *);
void snag_voice_rtc_close(struct snag_voice_rtc *);
#endif
