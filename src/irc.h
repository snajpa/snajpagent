/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_IRC_H
#define SNAJPAGENT_IRC_H

#include "base.h"
#include "cli.h"
#include "config.h"
#include "snag_jansson.h"

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

enum snag_irc_casemapping {
    SNAG_IRC_RFC1459,
    SNAG_IRC_ASCII,
    SNAG_IRC_RFC1459_STRICT,
    SNAG_IRC_CASE_UNKNOWN
};

struct snag_irc_destination {
    struct snag_irc_target target;
    char connection[SNAG_ID_HEX_LEN + 1u];
    uint64_t generation;
    uint64_t names_revision;
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char model[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char operator[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    enum snag_irc_casemapping casemapping[2u];
    char chantypes[2u][128u];
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

enum snag_irc_identity { SNAG_IRC_OPERATOR, SNAG_IRC_AGENT };

struct snag_irc_query_target {
    uint32_t destination;
    char connection[SNAG_ID_HEX_LEN + 1u];
    char conversation[SNAG_ID_HEX_LEN + 1u];
    char peer[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    uint64_t generation;
    enum snag_irc_identity identity;
};

struct snag_irc_channel_target {
    uint32_t destination;
    char connection[SNAG_ID_HEX_LEN + 1u];
    char conversation[SNAG_ID_HEX_LEN + 1u];
    char membership[SNAG_ID_HEX_LEN + 1u];
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    uint64_t generation;
    enum snag_irc_identity identity;
};

enum snag_irc_channel_action {
    SNAG_IRC_CHANNEL_PART,
    SNAG_IRC_CHANNEL_NAMES,
    SNAG_IRC_CHANNEL_TOPIC
};
enum snag_irc_connection_action { SNAG_IRC_CONNECTION_NICK, SNAG_IRC_CONNECTION_WHOIS };
/* Address resolution captures connection identity without copying member lists. */
struct snag_irc_scope {
    struct snag_irc_query_target target;
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    enum snag_irc_casemapping casemapping[2u];
    char chantypes[2u][128u];
};

struct snag_irc_scopes {
    struct snag_irc_scope items[SNAG_IRC_DESTINATIONS_MAX];
    size_t count;
};

struct snag_irc;
int snag_irc_connection_action(struct snag_irc *, const struct snag_irc_query_target *,
    enum snag_irc_connection_action, const char *, struct snag_buf *, char *, size_t);
void snag_irc_capture_scopes(const struct snag_irc_destinations *, struct snag_irc_scopes *);
json_t *snag_irc_names(const struct snag_irc *);
unsigned char snag_irc_name_fold(enum snag_irc_casemapping, unsigned char);
bool snag_irc_name_equal(enum snag_irc_casemapping, const char *, const char *);
bool snag_irc_name_mentioned(enum snag_irc_casemapping, const char *, const char *);
const struct snag_irc_scope *snag_irc_scope_resolve(
    const struct snag_irc_scopes *, uint32_t, const char *, char *, size_t);

enum snag_irc_conversation_kind { SNAG_IRC_CONNECTION_EVENTS, SNAG_IRC_CHANNEL, SNAG_IRC_QUERY };
/* Presentation selectors share draft/focus handling. Runtime sends still use
 * their kind-specific targets and validate the captured lifetime. */
struct snag_irc_conversation_target {
    uint32_t destination;
    char connection[SNAG_ID_HEX_LEN + 1u];
    char conversation[SNAG_ID_HEX_LEN + 1u];
    uint64_t generation;
    enum snag_irc_identity identity;
    enum snag_irc_conversation_kind kind;
    enum snag_irc_casemapping casemapping;
    char peer[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char membership[SNAG_ID_HEX_LEN + 1u];
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
};
/* Returns an owned array, empty when the captured destination is stale. */
json_t *snag_irc_completion_names(
    const json_t *, const struct snag_irc_conversation_target *, enum snag_irc_casemapping *);

enum snag_irc_direction { SNAG_IRC_INCOMING, SNAG_IRC_OUTGOING };
enum snag_irc_delivery {
    SNAG_IRC_DELIVERY_NONE,
    SNAG_IRC_PENDING,
    SNAG_IRC_WRITTEN,
    SNAG_IRC_ACKNOWLEDGED,
    SNAG_IRC_FAILED,
    SNAG_IRC_UNCERTAIN
};

/* Durable conversation identity is independent of a socket, list position or
 * current nickname. Target is the actual wire recipient; peer names the query
 * counterpart even when an incoming message targets our own accepted nick. */
struct snag_irc_event_route {
    char connection[SNAG_ID_HEX_LEN + 1u], conversation[SNAG_ID_HEX_LEN + 1u];
    char peer[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char target[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char send[SNAG_ID_HEX_LEN + 1u];
    char source[SNAG_IRC_LINE_MAX + 1u];
    uint64_t generation;
    enum snag_irc_identity identity;
    enum snag_irc_conversation_kind kind;
    enum snag_irc_direction direction;
    enum snag_irc_delivery delivery;
    bool action;
    /* A receipt carries different server text/action from the pending chunk. */
    bool revised;
    /* Present on channel records that carry the current membership lifetime. */
    char membership[SNAG_ID_HEX_LEN + 1u];
    bool joined, rejoin;
};

struct snag_irc_event {
    enum snag_irc_event_kind kind;
    uint64_t timestamp_ms;
    char stream[SNAG_ID_HEX_LEN + 1u];
    uint64_t sequence;
    /* Engine-classified durable input; transport marks eligible sent receipts. */
    bool input;
    bool classified, urgent, reply;
    /* The agent counterpart at receipt time; captured with empty IDs means unavailable. */
    bool reply_captured;
    char reply_conversation[SNAG_ID_HEX_LEN + 1u], reply_membership[SNAG_ID_HEX_LEN + 1u];
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char text[SNAG_IRC_TEXT_MAX + 1u];
    bool op;
    bool historical;
    bool local;
    bool routed;
    struct snag_irc_event_route route;
};

const char *snag_irc_kind_name(enum snag_irc_event_kind kind);
json_t *snag_irc_event_data(const struct snag_irc_event *event);
/* Durable field validation only; live/replay membership rules remain separate. */
int snag_irc_event_projection(struct snag_buf *out, const struct snag_irc_event *event);
int snag_irc_event_reference(struct snag_buf *, const struct snag_irc_event *);
int snag_irc_event_read(const json_t *data, struct snag_irc_event *event);
/* Record type and payload revision must agree. The legacy decoder above stays
 * strict so new routing fields cannot be hidden inside an old event type. */
int snag_irc_event_record_read(const char *, const json_t *, struct snag_irc_event *);
const char *snag_irc_event_record_type(const struct snag_irc_event *);
bool snag_irc_event_model_visible(const struct snag_irc_event *);
int snag_irc_event_capture_reply(
    struct snag_irc_event *, const json_t *, enum snag_irc_casemapping);
/* Only for payloads retained after typed record validation. */
int snag_irc_event_payload_read(const json_t *, struct snag_irc_event *);
/* Immutable conversation directory; update returns a new owned reference. */
json_t *snag_irc_conversations_update(const json_t *, const json_t *, uint64_t);
bool snag_irc_conversations_valid(const json_t *, uint64_t);
json_t *snag_irc_activity_update(const json_t *, const struct snag_irc_event *, uint64_t, uint64_t);
bool snag_irc_activity_valid(const json_t *, const json_t *, uint64_t);
bool snag_irc_activity_item_valid(const json_t *, uint64_t, uint64_t);

typedef int (*snag_irc_event_fn)(void *opaque, const struct snag_irc_event *event);
typedef int (*snag_irc_trace_fn)(void *opaque, unsigned int level, char direction,
    const char *endpoint, const char *text, size_t len);

struct snag_irc;

void snag_irc_destinations(const struct snag_irc *irc, struct snag_irc_destinations *out);
void snag_irc_capture_route(const struct snag_irc *irc, struct snag_irc_route *out);
bool snag_irc_event_target(
    const struct snag_irc *irc, const struct snag_irc_event *event, struct snag_irc_target *target);
bool snag_irc_local_identity(
    const struct snag_irc *irc, const struct snag_irc_event *event, bool model);
/* 0: all queued, 1: none queued, 2: partial, -1: local/runtime failure. */
int snag_irc_send_route(struct snag_irc *irc, const struct snag_irc_route *route, bool model,
    enum snag_irc_event_kind kind, const char *text, struct snag_buf *report, char *error,
    size_t error_size);
int snag_irc_query_open(struct snag_irc *, uint32_t destination, enum snag_irc_identity,
    const char *peer, struct snag_irc_query_target *, char *, size_t);
/* Open within a request's captured connection generation. */
int snag_irc_query_open_frozen(struct snag_irc *, const struct snag_irc_query_target *,
    const char *, struct snag_irc_query_target *, char *, size_t);
bool snag_irc_event_query_target(
    const struct snag_irc *, const struct snag_irc_event *, struct snag_irc_query_target *);
int snag_irc_query_send(struct snag_irc *, const struct snag_irc_query_target *,
    enum snag_irc_event_kind, const char *text, bool action, struct snag_buf *report, char *,
    size_t);
int snag_irc_channel_open(struct snag_irc *, const struct snag_irc_query_target *scope,
    const char *room, bool join, struct snag_irc_channel_target *, char *, size_t);
int snag_irc_channel_action(struct snag_irc *, const struct snag_irc_channel_target *,
    enum snag_irc_channel_action, const char *, struct snag_buf *, char *, size_t);
int snag_irc_channel_send(struct snag_irc *, const struct snag_irc_channel_target *,
    enum snag_irc_event_kind, const char *, bool, struct snag_buf *, char *, size_t);
int snag_irc_apply_cli(
    struct snag_config *config, const struct snag_cli *cli, char *error, size_t error_size);
int snag_irc_normalize(struct snag_config *config, char *error, size_t error_size);
bool snag_irc_endpoint_equal(const char *a, const char *b);
bool snag_irc_enabled(const struct snag_config *config);
int snag_irc_open(struct snag_irc **out, const struct snag_config *config, const char *cwd,
    snag_irc_event_fn event_fn, snag_irc_trace_fn trace_fn, void *event_opaque, char *error,
    size_t error_size);
void snag_irc_close(struct snag_irc *irc);
/* Engine-owned transitions; unrelated endpoint owners remain running. */
int snag_irc_add(struct snag_irc *irc, const struct snag_config *config, const char *cwd,
    bool hosting, const char *endpoint, char *error, size_t error_size);
int snag_irc_remove(
    struct snag_irc *irc, bool hosting, const char *endpoint, char *error, size_t error_size);
int snag_irc_preferences(struct snag_irc *irc, const struct snag_config *config, const char *cwd,
    char *error, size_t error_size);
int snag_irc_configure(struct snag_irc *irc, const struct snag_config *config, const char *cwd,
    char *error, size_t error_size);
/* Observed owner roles, including owners still connecting or retrying. */
void snag_irc_roles(const struct snag_irc *irc, struct snag_config *config);
uint64_t snag_irc_routing_revision(const struct snag_irc *irc);
/* Monotonic count of destination-set mutations; advances when an owner view,
 * room, target or endpoint changes, and when owners are added or removed. */
uint64_t snag_irc_destinations_generation(const struct snag_irc *irc);
int snag_irc_state(
    const struct snag_irc *irc, struct snag_buf *out, char *error, size_t error_size);
int snag_irc_tick(struct snag_irc *irc, int timeout_ms, char *error, size_t error_size);
int snag_irc_snapshot(
    const struct snag_irc *irc, struct snag_buf *out, char *error, size_t error_size);
int snag_irc_restore_event(struct snag_irc *irc, const struct snag_irc_event *event);
/* Initialize session-owned conversations before starting endpoint threads.
 * Resume preserves connection IDs and advances their network generations. */
int snag_irc_bind_conversations(struct snag_irc *, const json_t *conversations);
/* Render-only replay; no re-recording or sends. Nonempty replay ends with HISTORY_READY. */
int snag_irc_replay_hosted_history(
    const struct snag_irc *irc, snag_irc_event_fn render, void *opaque);
/* Hosted identity, or the first configured server's last accepted identity. */
const char *snag_irc_model_nick(const struct snag_irc *irc);
const char *snag_irc_operator_nick(const struct snag_irc *irc);
/* Consumes an admitted primary-identity change, including during command waits. */
bool snag_irc_identity_changed(struct snag_irc *irc);
/* Endpoint "local" broadcasts, so any joined endpoint's model alias matches. */
bool snag_irc_prompt(const char *text);
bool snag_irc_mentions_agent(const struct snag_irc *irc, const char *endpoint, const char *text);

#endif
