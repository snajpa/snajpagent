/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VOICE_RTC_H
#define SNAJPAGENT_VOICE_RTC_H
#include "base.h"
struct snag_voice_rtc;
int snag_voice_rtc_open(struct snag_voice_rtc **, char *, size_t);
int snag_voice_rtc_offer(struct snag_voice_rtc *, struct snag_buf *);
int snag_voice_rtc_answer(struct snag_voice_rtc *, const char *);
bool snag_voice_rtc_ready(struct snag_voice_rtc *);
/* Position is the first sample on the continuous 24 kHz capture clock (modulo
 * 2^32). Partial input must be contiguous. Zero frames discard partial capture
 * and reset encoder lookahead at mute; position is then ignored. */
int snag_voice_rtc_input(struct snag_voice_rtc *, const int16_t *, uint32_t, uint32_t);
/* Native media uses 48 kHz RTP timestamps and 24 kHz mono device PCM. */
int snag_voice_rtc_output(struct snag_voice_rtc *, int16_t *, uint32_t);
void snag_voice_rtc_flush(struct snag_voice_rtc *);
void snag_voice_rtc_close(struct snag_voice_rtc *);
#if SNAJPAGENT_AUDIO_DEVICE && defined(SNAJPAGENT_TEST_TRANSPORT_ENDPOINTS)
int snag_voice_rtc_fixture_encode(struct snag_voice_rtc *, const int16_t *, unsigned char *, int);
void snag_voice_rtc_fixture_packet(struct snag_voice_rtc *, const void *, int, bool);
#endif
#endif
