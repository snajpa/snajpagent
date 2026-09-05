/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_IRC_H
#define SNAJPAGENT_IRC_H

#include "base.h"
#include "cli.h"
#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNJ_IRC_TEXT_MAX 4096u
#define SNJ_IRC_LINE_MAX 8192u /* Includes CRLF. */

enum snj_irc_event_kind {
    SNJ_IRC_CONNECTED,
    SNJ_IRC_DISCONNECTED,
    SNJ_IRC_JOIN,
    SNJ_IRC_PART,
    SNJ_IRC_QUIT,
    SNJ_IRC_NICK,
    SNJ_IRC_MESSAGE,
    SNJ_IRC_NOTICE,
    SNJ_IRC_TOPIC,
    SNJ_IRC_MODE,
    SNJ_IRC_HISTORY_READY
};

struct snj_irc_event {
    enum snj_irc_event_kind kind;
    uint64_t timestamp_ms;
    char endpoint[SNJ_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char room[SNJ_CONFIG_IRC_ROOM_MAX + 2u];
    char nick[SNJ_CONFIG_IRC_NICK_MAX + 1u];
    char text[SNJ_IRC_TEXT_MAX + 1u];
    bool op;
    bool historical;
    bool local;
};

typedef int (*snj_irc_event_fn)(void *opaque,
                                const struct snj_irc_event *event);
typedef int (*snj_irc_trace_fn)(void *opaque, unsigned int level,
                                char direction, const char *endpoint,
                                const char *text, size_t len);

struct snj_irc;

int snj_irc_apply_cli(struct snj_config *config, const struct snj_cli *cli,
                      char *error, size_t error_size);
bool snj_irc_enabled(const struct snj_config *config);
int snj_irc_open(struct snj_irc **out, const struct snj_config *config,
                 const char *workspace, snj_irc_event_fn event_fn,
                 snj_irc_trace_fn trace_fn, void *event_opaque,
                 char *error, size_t error_size);
void snj_irc_close(struct snj_irc *irc);
int snj_irc_tick(struct snj_irc *irc, int timeout_ms,
                 char *error, size_t error_size);
int snj_irc_send_operator(struct snj_irc *irc, const char *text,
                          char *error, size_t error_size);
int snj_irc_send_agent(struct snj_irc *irc, const char *text,
                       char *error, size_t error_size);
int snj_irc_send_agent_notice(struct snj_irc *irc, const char *text,
                              char *error, size_t error_size);
int snj_irc_set_operator_topic(struct snj_irc *irc, const char *topic,
                               char *error, size_t error_size);
int snj_irc_set_agent_topic(struct snj_irc *irc, const char *topic,
                            char *error, size_t error_size);
int snj_irc_snapshot(const struct snj_irc *irc, struct snj_buf *out,
                     char *error, size_t error_size);
int snj_irc_restore_event(struct snj_irc *irc,
                          const struct snj_irc_event *event);
/* Render-only replay; does not emit, re-record, or send retained events. */
int snj_irc_replay_hosted_history(const struct snj_irc *irc,
                                  snj_irc_event_fn render, void *opaque);
/* Hosted identity, or the first configured server's last accepted identity. */
const char *snj_irc_model_nick(const struct snj_irc *irc);
const char *snj_irc_operator_nick(const struct snj_irc *irc);
const char *snj_irc_room_name(const struct snj_irc *irc);
/* Consumes an admitted primary-identity change, including during command waits. */
bool snj_irc_identity_changed(struct snj_irc *irc);
/* Returns 1 with changed newline-separated membership, 0 unchanged, -1 on error. */
int snj_irc_take_nicks(struct snj_irc *irc, struct snj_buf *out);
/* Endpoint "local" broadcasts, so any joined endpoint's model alias matches. */
bool snj_irc_mentions_agent(const struct snj_irc *irc, const char *endpoint,
                            const char *text);

#endif
