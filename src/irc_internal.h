/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_IRC_INTERNAL_H
#define SNAJPAGENT_IRC_INTERNAL_H

#include "irc.h"

/* One server, or one endpoint's paired agent/operator connections. No history. */
struct snj_irc_view {
    char model[SNJ_CONFIG_IRC_NICK_MAX + 1u];
    char operator[SNJ_CONFIG_IRC_NICK_MAX + 1u];
    char text[32768u];
    char nicks[4096u]; /* Newline-separated current members, without op prefixes. */
    bool joined;
};

struct snj_irc_core;
bool snj_irc_endpoint_equal(const char *a, const char *b);
bool snj_irc_nick_mentioned(const char *text, const char *nick);
int snj_irc_core_open(struct snj_irc_core **out, const struct snj_config *config,
                      const char *workspace, bool network,
                      snj_irc_event_fn event_fn, snj_irc_trace_fn trace_fn,
                      void *opaque, char *error, size_t error_size);
void snj_irc_core_close(struct snj_irc_core *irc);
int snj_irc_core_tick(struct snj_irc_core *irc, int timeout_ms, int wake_fd,
                      char *error, size_t error_size);
int snj_irc_core_send_operator(struct snj_irc_core *irc, const char *text,
                               char *error, size_t error_size);
int snj_irc_core_send_agent(struct snj_irc_core *irc, const char *text,
                            char *error, size_t error_size);
int snj_irc_core_send_agent_notice(struct snj_irc_core *irc, const char *text,
                                   char *error, size_t error_size);
int snj_irc_core_set_operator_topic(struct snj_irc_core *irc, const char *text,
                                    char *error, size_t error_size);
int snj_irc_core_set_agent_topic(struct snj_irc_core *irc, const char *text,
                                 char *error, size_t error_size);
int snj_irc_core_view(const struct snj_irc_core *irc, struct snj_irc_view *view);
int snj_irc_core_history(const struct snj_irc_core *irc, struct snj_buf *out);
void snj_irc_core_remember(struct snj_irc_core *irc,
                           const struct snj_irc_event *event);
int snj_irc_core_restore_event(struct snj_irc_core *irc,
                               const struct snj_irc_event *event);
int snj_irc_core_replay_hosted_history(const struct snj_irc_core *irc,
                                       snj_irc_event_fn render, void *opaque);
const char *snj_irc_core_model_nick(const struct snj_irc_core *irc);
const char *snj_irc_core_operator_nick(const struct snj_irc_core *irc);
const char *snj_irc_core_room_name(const struct snj_irc_core *irc);

#endif
