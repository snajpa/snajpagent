/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_IRC_H
#define SNAJPAGENT_IRC_H

#include "base.h"
#include "cli.h"
#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNAG_IRC_TEXT_MAX 4096u
#define SNAG_IRC_LINE_MAX 8192u /* Includes CRLF. */
#define SNAG_IRC_DESTINATIONS_MAX (SNAG_CONFIG_IRC_CLIENT_MAX + 1u)

/* Process-local handles; frozen routes never retain owner pointers. */
struct snag_irc_target {
    uint32_t id;
    uint64_t revision;
};

struct snag_irc_route {
    struct snag_irc_target targets[SNAG_IRC_DESTINATIONS_MAX];
    size_t count;
};

struct snag_irc_destination {
    struct snag_irc_target target;
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char model[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char operator[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char nicks[4096u];
    bool joined;
};

struct snag_irc_destinations {
    struct snag_irc_destination items[SNAG_IRC_DESTINATIONS_MAX];
    size_t count;
};

enum snag_irc_event_kind {
    SNAG_IRC_CONNECTED,
    SNAG_IRC_DISCONNECTED,
    SNAG_IRC_JOIN,
    SNAG_IRC_PART,
    SNAG_IRC_QUIT,
    SNAG_IRC_NICK,
    SNAG_IRC_MESSAGE,
    SNAG_IRC_NOTICE,
    SNAG_IRC_TOPIC,
    SNAG_IRC_MODE,
    SNAG_IRC_HISTORY_READY
};

struct snag_irc_event {
    enum snag_irc_event_kind kind;
    uint64_t timestamp_ms;
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char text[SNAG_IRC_TEXT_MAX + 1u];
    bool op;
    bool historical;
    bool local;
};

typedef int (*snag_irc_event_fn)(void *opaque,
                                const struct snag_irc_event *event);
typedef int (*snag_irc_trace_fn)(void *opaque, unsigned int level,
                                char direction, const char *endpoint,
                                const char *text, size_t len);

struct snag_irc;

void snag_irc_destinations(const struct snag_irc *irc,
                            struct snag_irc_destinations *out);
void snag_irc_capture_route(const struct snag_irc *irc, struct snag_irc_route *out);
bool snag_irc_event_target(const struct snag_irc *irc,
                            const struct snag_irc_event *event,
                            struct snag_irc_target *target);
bool snag_irc_local_identity(const struct snag_irc *irc,
                              const struct snag_irc_event *event, bool model);
/* 0: all queued, 1: none queued, 2: partial, -1: local/runtime failure. */
int snag_irc_send_route(struct snag_irc *irc, const struct snag_irc_route *route,
                         bool model, enum snag_irc_event_kind kind, const char *text,
                         struct snag_buf *report, char *error, size_t error_size);
int snag_irc_apply_cli(struct snag_config *config, const struct snag_cli *cli,
                      char *error, size_t error_size);
int snag_irc_normalize(struct snag_config *config, char *error, size_t error_size);
bool snag_irc_endpoint_equal(const char *a, const char *b);
bool snag_irc_enabled(const struct snag_config *config);
int snag_irc_open(struct snag_irc **out, const struct snag_config *config,
                 const char *workspace, snag_irc_event_fn event_fn,
                 snag_irc_trace_fn trace_fn, void *event_opaque,
                 char *error, size_t error_size);
void snag_irc_close(struct snag_irc *irc);
/* Engine-owned transitions; unrelated endpoint owners remain running. */
int snag_irc_add(struct snag_irc *irc, const struct snag_config *config,
                 const char *workspace, bool hosting, const char *endpoint,
                 char *error, size_t error_size);
int snag_irc_remove(struct snag_irc *irc, bool hosting, const char *endpoint,
                    char *error, size_t error_size);
int snag_irc_preferences(struct snag_irc *irc, const struct snag_config *config,
                         const char *workspace, char *error, size_t error_size);
int snag_irc_configure(struct snag_irc *irc, const struct snag_config *config,
                       const char *workspace, char *error, size_t error_size);
/* Observed owner roles, including owners still connecting or retrying. */
void snag_irc_roles(const struct snag_irc *irc, struct snag_config *config);
uint64_t snag_irc_routing_revision(const struct snag_irc *irc);
int snag_irc_state(const struct snag_irc *irc, struct snag_buf *out,
                   char *error, size_t error_size);
int snag_irc_tick(struct snag_irc *irc, int timeout_ms,
                 char *error, size_t error_size);
int snag_irc_snapshot(const struct snag_irc *irc, struct snag_buf *out,
                     char *error, size_t error_size);
int snag_irc_restore_event(struct snag_irc *irc,
                          const struct snag_irc_event *event);
/* Render-only replay; no re-recording or sends. Nonempty replay ends with HISTORY_READY. */
int snag_irc_replay_hosted_history(const struct snag_irc *irc,
                                  snag_irc_event_fn render, void *opaque);
/* Hosted identity, or the first configured server's last accepted identity. */
const char *snag_irc_model_nick(const struct snag_irc *irc);
const char *snag_irc_operator_nick(const struct snag_irc *irc);
/* Consumes an admitted primary-identity change, including during command waits. */
bool snag_irc_identity_changed(struct snag_irc *irc);
/* Endpoint "local" broadcasts, so any joined endpoint's model alias matches. */
bool snag_irc_mentions_agent(const struct snag_irc *irc, const char *endpoint,
                            const char *text);

#endif
