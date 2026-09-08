/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VOICE_H
#define SNAJPAGENT_VOICE_H
#include "base.h"
#include "snag_jansson.h"

/* Realtime protocol state has one owner and no device, journal, UI or executor.
 * Hooks borrow their inputs. The desktop owner queues notices to the existing
 * session owner; only that owner may accept a coding handoff durably. */
struct snag_voice;
struct snag_voice_io {
    int (*send)(void *,const json_t *);
    int (*notice)(void *,const json_t *);
    /* Zero frames marks audio end; release a short prefill without waiting. */
    int (*play)(void *,const char *item,const int16_t *,uint32_t);
    /* Stop output first; return conservative played ms of its current item. */
    uint32_t (*interrupt)(void *);
};
struct snag_voice *snag_voice_new(const struct snag_voice_io *,void *,const char *model,
                                 const char *transcribe_model,const char *voice);
void snag_voice_free(struct snag_voice *);
int snag_voice_begin(struct snag_voice *,char *,size_t);
/* Replace textual session context; never requests speech or coding work. */
int snag_voice_context(struct snag_voice *,const json_t *,char *,size_t);
int snag_voice_event(struct snag_voice *,const json_t *,char *,size_t);
/* At most one response at once. Committed audio may be answered before ASR
 * finishes; only a coding handoff waits for its correlated final transcript. */
int snag_voice_respond(struct snag_voice *,bool drained,char *,size_t);
int snag_voice_input(struct snag_voice *,const int16_t *,uint32_t,char *,size_t);
int snag_voice_mute(struct snag_voice *,bool,char *,size_t);
int snag_voice_result(struct snag_voice *,const char *call,const char *result,char *,size_t);
bool snag_voice_ready(const struct snag_voice *);
#endif
