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
    char text[32768u];
    char nicks[4096u]; /* Newline-separated current members, without op prefixes. */
    bool joined;
};

struct snag_irc_core;
int snag_irc_core_open(struct snag_irc_core **out, const struct snag_config *config,
                      const char *workspace, bool network,
                      snag_irc_event_fn event_fn, snag_irc_trace_fn trace_fn,
                      void *opaque, char *error, size_t error_size);
void snag_irc_core_close(struct snag_irc_core *irc);
size_t snag_irc_core_pending(const struct snag_irc_core *irc);
int snag_irc_core_copy_history(struct snag_irc_core *dst,
                              const struct snag_irc_core *src, bool hosted_only);
int snag_irc_core_tick(struct snag_irc_core *irc, int timeout_ms, snag_wake_fd wake_fd,
                      char *error, size_t error_size);
int snag_irc_core_send(struct snag_irc_core *irc, bool model,
                       enum snag_irc_event_kind kind, const char *text,
                       char *error, size_t error_size);
int snag_irc_core_view(const struct snag_irc_core *irc, struct snag_irc_view *view);
int snag_irc_core_history(const struct snag_irc_core *irc, struct snag_buf *out);
void snag_irc_core_remember(struct snag_irc_core *irc,
                           const struct snag_irc_event *event);
int snag_irc_core_restore_event(struct snag_irc_core *irc,
                               const struct snag_irc_event *event);
int snag_irc_core_replay_hosted_history(const struct snag_irc_core *irc,
                                       snag_irc_event_fn render, void *opaque);
const char *snag_irc_core_model_nick(const struct snag_irc_core *irc);
const char *snag_irc_core_operator_nick(const struct snag_irc_core *irc);

#endif
