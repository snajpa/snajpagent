/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_IRC_INTERNAL_H
#define SNAJPAGENT_IRC_INTERNAL_H
#include "wake.h"

#include "irc.h"

/* One server, or one endpoint's paired agent/operator connections. No history. */
struct snag_irc_view {
    char model[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char operator[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    uint64_t revision;
    char connection[SNAG_ID_HEX_LEN + 1u];
    uint64_t generation;
    char text[32768u];
    char nicks[4096u]; /* Newline-separated current members, without op prefixes. */
    bool joined;
};

struct snag_irc_core;
int snag_irc_core_open(struct snag_irc_core **out, const struct snag_config *config,
                      const char *cwd, bool network,
                      snag_irc_event_fn event_fn, snag_irc_trace_fn trace_fn,
                      void *opaque, char *error, size_t error_size);
void snag_irc_core_close(struct snag_irc_core *irc);
int snag_irc_core_retire(struct snag_irc_core *irc);
int snag_irc_core_bind(struct snag_irc_core *, const char *connection,
                       const char *endpoint, uint64_t generation);
size_t snag_irc_core_pending(const struct snag_irc_core *irc);
int snag_irc_core_copy_history(struct snag_irc_core *dst, const struct snag_irc_core *src, bool hosted_only);
int snag_irc_core_tick(struct snag_irc_core *irc, int timeout_ms, snag_wake_fd wake_fd,
                      char *error, size_t error_size);
int snag_irc_core_send(struct snag_irc_core *irc, bool model, enum snag_irc_event_kind kind, const char *text,
                       char *error, size_t error_size);
int snag_irc_core_query_open(struct snag_irc_core *, enum snag_irc_identity, const char *,
                             struct snag_irc_query_target *, char *, size_t);
int snag_irc_core_query_open_frozen(struct snag_irc_core *, const struct snag_irc_query_target *,
                                    const char *, struct snag_irc_query_target *, char *, size_t);
int snag_irc_core_query_send(struct snag_irc_core *, const struct snag_irc_query_target *,
                             enum snag_irc_event_kind, const char *, bool, struct snag_buf *,
                             char *, size_t);
int snag_irc_core_view(const struct snag_irc_core *irc, struct snag_irc_view *view);
int snag_irc_core_history(const struct snag_irc_core *irc, struct snag_buf *out);
void snag_irc_core_remember(struct snag_irc_core *irc, const struct snag_irc_event *event);
void snag_irc_core_defer(struct snag_irc_core *irc);
bool snag_irc_core_received(struct snag_irc_core *irc, const struct snag_irc_event *event);
int snag_irc_core_ack(struct snag_irc_core *irc, const struct snag_irc_event *event);
int snag_irc_core_accept(struct snag_irc_core *irc, const struct snag_irc_event *event);
int snag_irc_core_restore_event(struct snag_irc_core *irc, const struct snag_irc_event *event);
/* Replay metadata only: never sockets or uncommitted owner-thread traffic. */
json_t *snag_irc_core_checkpoint(const struct snag_irc_core *irc);
int snag_irc_core_restore_checkpoint(struct snag_irc_core *irc, const json_t *data);
int snag_irc_core_replay_hosted_history(const struct snag_irc_core *irc,
                                       snag_irc_event_fn render, void *opaque);
const char *snag_irc_core_model_nick(const struct snag_irc_core *irc);
const char *snag_irc_core_operator_nick(const struct snag_irc_core *irc);

#endif
