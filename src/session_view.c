/* SPDX-License-Identifier: GPL-2.0-only */
#include "session_view.h"
#include "irc.h"
#include "turn.h"
#include "vm_text.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Same stalled-I/O bound as the terminal relay. Idle, complete connections
 * have no deadline; partial messages and unconsumed writes do. */
#define VIEW_STALL_MS 5000u
#define VIEW_HANDSHAKE_MS 15000u

static uint64_t
read_u64(const unsigned char *bytes)
{
    uint64_t n = 0u;
    for (unsigned int i = 0u; i < 8u; ++i) n |= (uint64_t)bytes[i] << (8u * i);
    return n;
}

static void
write_u64(unsigned char *bytes, uint64_t n)
{
    for (unsigned int i = 0u; i < 8u; ++i) bytes[i] = (unsigned char)(n >> (8u * i));
}

void
snag_view_channel_init(struct snag_view_channel *channel, int fd)
{
    *channel = (struct snag_view_channel){.fd = fd};
    snag_buf_init(&channel->input, SNAG_VIEW_MESSAGE_MAX);
}

void
snag_view_channel_close(struct snag_view_channel *channel)
{
    if (channel->fd >= 0) (void)close(channel->fd);
    if (channel->input.data) memset(channel->input.data, 0, channel->input.len);
    snag_buf_free(&channel->input);
    if (channel->output) memset(channel->output, 0, channel->output_len);
    free(channel->output);
    snag_view_channel_init(channel, -1);
}

int
snag_view_channel_send(struct snag_view_channel *channel, const json_t *value)
{
    if (channel->output) return snag_errno(EAGAIN);
    if (!json_is_object(value)) return snag_errno(EINVAL);
    char *text = json_dumps(value, JSON_COMPACT | JSON_ENSURE_ASCII);
    if (!text) return -1;
    size_t len = strlen(text);
    if (len > SNAG_VIEW_MESSAGE_MAX) {
        free(text);
        return snag_errno(EOVERFLOW);
    }
    channel->output = text;
    channel->output_len = len;
    channel->output_offset = 0u;
    channel->write_deadline = snag_monotonic_ms() + VIEW_STALL_MS;
    return 0;
}

int
snag_view_channel_write(struct snag_view_channel *channel)
{
    if (!channel->output) return 1;
    if (!channel->outgoing.used) {
        unsigned char slice[SNAG_SESSION_FRAME_MAX];
        size_t len = channel->output_len - channel->output_offset;
        if (len > sizeof(slice) - 16u) len = sizeof(slice) - 16u;
        write_u64(slice, channel->output_offset);
        write_u64(slice + 8u, channel->output_len);
        memcpy(slice + 16u, channel->output + channel->output_offset, len);
        if (snag_session_view_packet_set(&channel->outgoing, slice, len + 16u) < 0) return -1;
    }
    size_t before = channel->outgoing.offset;
    int rc = snag_session_packet_write(channel->fd, &channel->outgoing);
    if (channel->outgoing.offset != before)
        channel->write_deadline = snag_monotonic_ms() + VIEW_STALL_MS;
    if (rc <= 0) return rc;
    channel->output_offset += snag_session_packet_length(&channel->outgoing) - 16u;
    channel->outgoing = (struct snag_session_packet){0};
    if (channel->output_offset < channel->output_len) return 0;
    memset(channel->output, 0, channel->output_len);
    free(channel->output);
    channel->output = NULL;
    channel->output_len = channel->output_offset = 0u;
    channel->write_deadline = 0u;
    return 1;
}

int
snag_view_channel_read(struct snag_view_channel *channel, json_t **value)
{
    *value = NULL;
    if (!channel->verified) {
        int verified = snag_session_peer_verify(channel->fd);
        if (verified <= 0) return verified;
        channel->verified = true;
    }
    size_t before = channel->incoming.used;
    int rc = snag_session_view_packet_read(channel->fd, &channel->incoming);
    if (channel->incoming.used != before)
        channel->read_deadline = snag_monotonic_ms() + VIEW_STALL_MS;
    if (rc <= 0) return rc;
    size_t len = snag_session_packet_length(&channel->incoming);
    const unsigned char *bytes = channel->incoming.bytes + SNAG_SESSION_HEADER;
    if (len <= 16u || read_u64(bytes) != channel->input.len) return snag_errno(EPROTO);
    uint64_t total = read_u64(bytes + 8u);
    if (!total || total > SNAG_VIEW_MESSAGE_MAX ||
        (channel->input.len && total != channel->total) ||
        channel->input.len > total || len - 16u > total - channel->input.len)
        return snag_errno(EPROTO);
    channel->total = total;
    if (snag_buf_append(&channel->input, bytes + 16u, len - 16u) < 0) return -1;
    channel->incoming = (struct snag_session_packet){0};
    if (channel->input.len < total) return 0;
    json_t *message = snag_json_load_strict(channel->input.data, channel->input.len,
        SNAG_VIEW_MESSAGE_MAX, NULL, 0u);
    memset(channel->input.data, 0, channel->input.len);
    snag_buf_reset(&channel->input);
    channel->total = 0u;
    channel->read_deadline = 0u;
    if (!json_is_object(message)) {
        json_decref(message);
        return snag_errno(EPROTO);
    }
    *value = message;
    return 1;
}

json_t *
snag_view_query_route(const struct snag_irc_query_target *target)
{
    return json_pack("{s:s,s:s,s:I,s:s,s:s}", "connection", target->connection,
        "conversation", target->conversation, "generation", (json_int_t)target->generation,
        "identity", target->identity == SNAG_IRC_OPERATOR ? "operator" : "agent",
        "peer", target->peer);
}

int
snag_view_query_read(const json_t *route, struct snag_irc_query_target *target)
{
    const char *connection = snag_json_bounded_string(
        json_object_get(route, "connection"), SNAG_ID_HEX_LEN);
    const char *conversation = snag_json_bounded_string(
        json_object_get(route, "conversation"), SNAG_ID_HEX_LEN);
    const char *identity = snag_json_bounded_string(json_object_get(route, "identity"), 8u);
    const char *peer = snag_json_bounded_string(json_object_get(route, "peer"),
        SNAG_CONFIG_IRC_NICK_MAX);
    uint64_t generation;
    if (!snag_json_exact_keys(route, "connection conversation generation identity peer") ||
        !connection || !snag_hex_is_lower(connection, SNAG_ID_HEX_LEN) ||
        !conversation || !snag_hex_is_lower(conversation, SNAG_ID_HEX_LEN) ||
        !identity || (strcmp(identity, "operator") && strcmp(identity, "agent")) ||
        !peer || !snag_text_valid(peer, 1u, SNAG_CONFIG_IRC_NICK_MAX) ||
        snag_json_integer_u64(route, "generation", &generation) < 0 || !generation)
        return snag_errno(EINVAL);
    *target = (struct snag_irc_query_target){.generation = generation,
        .identity = !strcmp(identity, "operator") ? SNAG_IRC_OPERATOR : SNAG_IRC_AGENT};
    (void)snag_strcpy(target->connection, sizeof(target->connection), connection);
    (void)snag_strcpy(target->conversation, sizeof(target->conversation), conversation);
    (void)snag_strcpy(target->peer, sizeof(target->peer), peer);
    return 0;
}

json_t *
snag_view_conversation_route(const struct snag_irc_conversation_target *target)
{
    if (target->kind == SNAG_IRC_QUERY || target->kind == SNAG_IRC_CONNECTION_EVENTS) {
        return json_pack("{s:s,s:s,s:I,s:s,s:s}", "connection", target->connection,
            "conversation", target->conversation, "generation", (json_int_t)target->generation,
            "identity", target->identity == SNAG_IRC_OPERATOR ? "operator" : "agent",
            target->kind == SNAG_IRC_QUERY ? "peer" : "endpoint",
            target->kind == SNAG_IRC_QUERY ? target->peer : target->endpoint);
    }
    if (target->kind != SNAG_IRC_CHANNEL) { errno = EINVAL; return NULL; }
    return json_pack("{s:s,s:s,s:I,s:s,s:s,s:s,s:s,s:i}", "connection", target->connection,
        "conversation", target->conversation, "generation", (json_int_t)target->generation,
        "identity", target->identity == SNAG_IRC_OPERATOR ? "operator" : "agent",
        "room", target->room, "membership", target->membership, "endpoint", target->endpoint,
        "casemapping", (int)target->casemapping);
}

const char *
snag_view_conversation_name(const json_t *route)
{
    const char *room = snag_json_string(route, "room");
    const char *peer = snag_json_string(route, "peer");
    return room ? room : peer ? peer : snag_json_string(route, "endpoint");
}

int
snag_view_conversation_read(const json_t *route, struct snag_irc_conversation_target *target)
{
    struct snag_irc_query_target query;
    if (snag_view_query_read(route, &query) == 0) {
        *target = (struct snag_irc_conversation_target){.kind = SNAG_IRC_QUERY,
            .generation = query.generation, .identity = query.identity};
        memcpy(target->connection, query.connection, sizeof(target->connection));
        memcpy(target->conversation, query.conversation, sizeof(target->conversation));
        memcpy(target->peer, query.peer, sizeof(target->peer));
        return 0;
    }
    const char *connection = snag_json_bounded_string(
        json_object_get(route, "connection"), SNAG_ID_HEX_LEN);
    const char *conversation = snag_json_bounded_string(
        json_object_get(route, "conversation"), SNAG_ID_HEX_LEN);
    const char *membership = snag_json_bounded_string(
        json_object_get(route, "membership"), SNAG_ID_HEX_LEN);
    const char *identity = snag_json_bounded_string(json_object_get(route, "identity"), 8u);
    const char *room = snag_json_bounded_string(json_object_get(route, "room"),
        SNAG_CONFIG_IRC_ROOM_MAX + 1u);
    const char *endpoint = snag_json_bounded_string(json_object_get(route, "endpoint"),
        SNAG_CONFIG_IRC_ENDPOINT_MAX);
    bool channel = json_object_get(route, "room") != NULL;
    uint64_t generation, casemapping = SNAG_IRC_CASE_UNKNOWN;
    if (!snag_json_exact_keys(route, channel ?
        "connection conversation generation identity room membership endpoint casemapping" :
        "connection conversation generation identity endpoint") ||
        !connection || !snag_hex_is_lower(connection, SNAG_ID_HEX_LEN) ||
        !conversation || !snag_hex_is_lower(conversation, SNAG_ID_HEX_LEN) ||
        (channel && (!membership || !snag_hex_is_lower(membership, SNAG_ID_HEX_LEN))) ||
        !identity || (strcmp(identity, "operator") && strcmp(identity, "agent")) ||
        (channel && (!room || !snag_text_valid(room, 1u, SNAG_CONFIG_IRC_ROOM_MAX + 1u) ||
        strcspn(room, " \t\r\n") != strlen(room))) ||
        !endpoint || !snag_text_valid(endpoint, 1u, SNAG_CONFIG_IRC_ENDPOINT_MAX) ||
        strcspn(endpoint, " \t\r\n") != strlen(endpoint) ||
        (channel && (snag_json_integer_u64(route, "casemapping", &casemapping) < 0 ||
        casemapping > SNAG_IRC_CASE_UNKNOWN)) ||
        snag_json_integer_u64(route, "generation", &generation) < 0 || !generation)
        return snag_errno(EINVAL);
    *target = (struct snag_irc_conversation_target){
        .kind = channel ? SNAG_IRC_CHANNEL : SNAG_IRC_CONNECTION_EVENTS,
        .generation = generation, .casemapping = (enum snag_irc_casemapping)casemapping,
        .identity = !strcmp(identity, "operator") ? SNAG_IRC_OPERATOR : SNAG_IRC_AGENT};
    (void)snag_strcpy(target->connection, sizeof(target->connection), connection);
    (void)snag_strcpy(target->conversation, sizeof(target->conversation), conversation);
    (void)snag_strcpy(target->membership, sizeof(target->membership), membership);
    (void)snag_strcpy(target->room, sizeof(target->room), room);
    (void)snag_strcpy(target->endpoint, sizeof(target->endpoint), endpoint);
    return 0;
}

static bool
writable_route(const json_t *route)
{
    if (json_is_string(route)) return !strcmp(json_string_value(route), "rollout") &&
        json_string_length(route) == 7u;
    struct snag_irc_conversation_target target;
    return snag_view_conversation_read(route, &target) == 0 &&
        target.identity == SNAG_IRC_OPERATOR;
}

struct view_draft {
    struct view_draft *next;
    json_t *route, *text;
    uint64_t revision;
    size_t cursor;
};

struct view_receipt {
    char id[SNAG_ID_HEX_LEN + 1u], sha256[SNAG_SHA256_HEX_LEN + 1u];
    json_t *result;
    struct view_draft *draft;
    char *command_text;
    bool pending, command, terminal_dispatched;
    uint64_t draft_revision, terminal_generation;
    struct view_receipt *next;
};

struct view_peer {
    struct snag_view_channel channel;
    uint64_t generation, deadline, revision, draft_revision;
    bool hello, bound, closing, reports, reports_ready;
    struct view_draft *draft;
    size_t report_cursor;
    struct view_receipt *waiting;
    struct view_peer *next;
};

struct snag_view_server {
    struct snag_session_listener listener;
    struct snag_session_relay *relay;
    struct snag_view_callbacks callbacks;
    char session[SNAG_ID_HEX_LEN + 1u], instance[SNAG_ID_HEX_LEN + 1u];
    json_t *state, *reports;
    json_t *empty_draft;
    struct view_draft *drafts;
    uint64_t revision;
    bool stopping, exiting;
    unsigned int exit_status;
    struct view_peer *peers;
    struct view_receipt *receipts, *pending;
};

/* A changed nick or connection epoch is a different frozen route. Keeping its
 * draft preserves unsent text without silently rebinding it to the new peer. */
static struct view_draft *
route_draft(struct snag_view_server *server, const json_t *route)
{
    for (struct view_draft *draft = server->drafts; draft; draft = draft->next)
        if (json_equal(draft->route, route)) return draft;
    struct view_draft *draft = calloc(1u, sizeof(*draft));
    if (!draft) return NULL;
    draft->route = json_deep_copy(route);
    if (!draft->route) { free(draft); return NULL; }
    draft->text = json_incref(server->empty_draft);
    draft->revision = 1u;
    draft->next = server->drafts;
    server->drafts = draft;
    return draft;
}

static void
release_peer(struct snag_view_server *server, struct view_peer *peer)
{
    if (!peer->generation) return;
    snag_session_relay_view_release(server->relay, peer->generation);
    if (peer->bound) server->callbacks.bound(server->callbacks.opaque, 0u);
    peer->generation = 0u;
    peer->bound = false;
}

struct snag_view_server *
snag_view_server_open(int dir_fd, const char *path, int lock_fd, const char *session,
    struct snag_session_relay *relay, struct snag_view_callbacks callbacks)
{
    struct snag_view_server *server = calloc(1u, sizeof(*server));
    if (!server) return NULL;
    server->listener = (struct snag_session_listener){.fd = -1, .dir_fd = -1};
    server->relay = relay;
    server->callbacks = callbacks;
    server->empty_draft = json_string("");
    server->reports = json_array();
    if (!server->empty_draft || !server->reports ||
        !callbacks.bound || !callbacks.submit || !callbacks.control ||
        !snag_strcpy(server->session, sizeof(server->session), session) ||
        snag_random_id(server->instance) < 0 ||
        snag_session_view_listen(&server->listener, dir_fd, path, lock_fd) < 0) {
        snag_view_server_close(server);
        return NULL;
    }
    return server;
}

void
snag_view_server_close(struct snag_view_server *server)
{
    if (!server) return;
    snag_session_listener_close(&server->listener);
    while (server->peers) {
        struct view_peer *peer = server->peers;
        server->peers = peer->next;
        release_peer(server, peer);
        snag_view_channel_close(&peer->channel);
        free(peer);
    }
    while (server->receipts) {
        struct view_receipt *receipt = server->receipts;
        server->receipts = receipt->next;
        json_decref(receipt->result);
        free(receipt->command_text);
        free(receipt);
    }
    json_decref(server->state);
    json_decref(server->reports);
    while (server->drafts) {
        struct view_draft *draft = server->drafts;
        server->drafts = draft->next;
        json_decref(draft->route);
        json_decref(draft->text);
        free(draft);
    }
    json_decref(server->empty_draft);
    free(server);
}

void
snag_view_server_stop(struct snag_view_server *server)
{
    if (!server) return;
    snag_session_listener_close(&server->listener);
    server->stopping = true;
}

void
snag_view_server_exit(struct snag_view_server *server, unsigned int status)
{
    if (!server) return;
    snag_view_server_stop(server);
    server->exiting = true;
    server->exit_status = status;
}

bool
snag_view_server_busy(const struct snag_view_server *server)
{
    return server && server->peers;
}

bool
snag_view_server_attached(const struct snag_view_server *server)
{
    return server && server->relay->view_attached;
}

void
snag_view_server_state(struct snag_view_server *server, const json_t *state)
{
    if (!server || !json_is_object(state) || json_equal(server->state, state)) return;
    json_t *copy = json_incref((json_t *)state);
    json_decref(server->state);
    server->state = copy;
    ++server->revision;
}

static struct view_receipt *
find_receipt(struct snag_view_server *server, const char *id)
{
    for (struct view_receipt *r = server->receipts; r; r = r->next)
        if (!strcmp(r->id, id)) return r;
    return NULL;
}

static int
finish_receipt(struct snag_view_server *server, struct view_receipt *receipt,
    json_t *result, bool accepted)
{
    struct view_draft *draft = receipt->draft;
    bool clear = accepted && receipt->draft_revision &&
        receipt->draft_revision == draft->revision && draft->revision < INT64_MAX;
    if (!result || json_object_set_new(result, "type", json_string("result")) < 0 ||
        json_object_set_new(result, "draft_cleared", json_integer(
            (json_int_t)(clear ? draft->revision + 1u : 0u))) < 0) {
        json_decref(result);
        return -1;
    }
    if (clear) {
        json_decref(draft->text);
        draft->text = json_incref(server->empty_draft);
        draft->cursor = 0u;
        ++draft->revision;
    }
    json_decref(receipt->result);
    receipt->result = result;
    if (strcmp(snag_json_string(result, "status"), "terminal")) {
        free(receipt->command_text);
        receipt->command_text = NULL;
    }
    receipt->pending = false;
    if (server->pending == receipt) server->pending = NULL;
    return 0;
}

int
snag_view_server_result(struct snag_view_server *server, const char *id,
    const char *status, uint64_t seq, const char *event)
{
    if (!server) return 0;
    struct view_receipt *receipt = find_receipt(server, id);
    if (!receipt) return snag_errno(ENOENT);
    json_t *result = json_pack("{s:s,s:s,s:I,s:s}", "id", id,
        "status", status, "seq", (json_int_t)seq, "event", event ? event : "");
    return finish_receipt(server, receipt, result, !strcmp(status, "committed"));
}

int
snag_view_server_report(struct snag_view_server *server, const json_t *report, const char *error)
{
    if (!server) return 0;
    const char *id = snag_json_string(report, "id");
    for (size_t i = 0u; id && i < json_array_size(server->reports); ++i) {
        const json_t *existing = json_object_get(json_array_get(server->reports, i), "report");
        const char *previous = snag_json_string(existing, "id");
        if (previous && !strcmp(previous, id))
            return json_equal(existing, report) ? 0 : snag_errno(ESTALE);
    }
    json_t *entry = json_pack("{s:s,s:O,s:s}", "type", "report",
        "report", report ? report : json_null(), "error", error ? error : "");
    if (!entry) return -1;
    return json_array_append_new(server->reports, entry);
}

int
snag_view_server_command_result(struct snag_view_server *server, const json_t *result)
{
    if (!server) return 0;
    const char *id = snag_json_string(result, "id");
    struct view_receipt *receipt = id ? find_receipt(server, id) : NULL;
    if (!receipt || !receipt->command) return snag_errno(ENOENT);
    int rc = finish_receipt(server, receipt, json_deep_copy(result), true);
    if (!rc && !strcmp(snag_json_string(result, "status"), "completed"))
        rc = snag_view_server_report(server, json_object_get(result, "report"),
            snag_json_string(result, "report_error"));
    return rc;
}

int
snag_view_server_terminal_generation(struct snag_view_server *server, const char *id,
    uint64_t *generation)
{
    *generation = 0u;
    struct view_receipt *receipt = server && id ? find_receipt(server, id) : NULL;
    if (!receipt) return snag_errno(ENOENT);
    *generation = receipt->terminal_generation;
    return 0;
}

int
snag_view_server_terminal(struct snag_view_server *server, const unsigned char *reference)
{
    if (!server || server->stopping || !reference ||
        server->relay->phase != SNAG_SESSION_ATTACHED || server->relay->peer < 0 ||
        memcmp(reference, server->instance, SNAG_ID_HEX_LEN)) return snag_errno(ESTALE);
    char id[SNAG_ID_HEX_LEN + 1u];
    memcpy(id, reference + SNAG_ID_HEX_LEN, SNAG_ID_HEX_LEN);
    id[SNAG_ID_HEX_LEN] = 0;
    struct view_receipt *receipt = find_receipt(server, id);
    if (!receipt || !receipt->command) return snag_errno(ENOENT);
    if (receipt->terminal_dispatched) return 0;
    if (server->pending || !receipt->command_text ||
        strcmp(snag_json_string(receipt->result, "status"), "terminal"))
        return snag_errno(EBUSY);
    json_t *pending = json_pack("{s:s,s:s,s:s}", "type", "result", "id", id,
        "status", "pending");
    if (!pending) return -1;
    int rc = server->callbacks.submit(server->callbacks.opaque, id, receipt->command_text,
        receipt->draft->route, server->relay->generation, true);
    if (rc < 0) { json_decref(pending); return -1; }
    receipt->terminal_dispatched = receipt->pending = true;
    receipt->terminal_generation = server->relay->generation;
    receipt->draft_revision = 0u;
    server->pending = receipt;
    json_decref(receipt->result);
    receipt->result = pending;
    return 0;
}

static int
reply(struct view_peer *peer, json_t *value)
{
    int rc = snag_view_channel_send(&peer->channel, value);
    json_decref(value);
    return rc;
}

static int
refuse(struct view_peer *peer, const char *message)
{
    return reply(peer, json_pack("{s:s,s:s}", "type", "error", "message", message));
}

static bool
request_id(const char *id)
{
    return id && strlen(id) == SNAG_ID_HEX_LEN &&
        strspn(id, "0123456789abcdef") == SNAG_ID_HEX_LEN;
}

static int
refuse_request(struct view_peer *peer, const json_t *message, const char *error)
{
    const char *type = snag_json_string(message, "type");
    const char *id = snag_json_string(message, "id");
    if (type && (!strcmp(type, "submit") || !strcmp(type, "command")) && request_id(id))
        return reply(peer, json_pack("{s:s,s:s,s:s}", "type", "error", "id", id, "message", error));
    uint64_t edit;
    if (type && !strcmp(type, "draft") &&
        snag_json_integer_u64(message, "edit", &edit) == 0 && edit)
        return reply(peer, json_pack("{s:s,s:I,s:s}", "type", "error",
            "edit", (json_int_t)edit, "message", error));
    return refuse(peer, error);
}

static int
draft_snapshot(struct view_peer *peer,
    uint64_t edit, const char *status)
{
    struct view_draft *draft = peer->draft;
    peer->draft_revision = draft->revision;
    return reply(peer, json_pack("{s:s,s:I,s:s,s:{s:O,s:I,s:O,s:I}}",
        "type", "draft", "edit", (json_int_t)edit, "status", status,
        "draft", "route", draft->route, "revision", (json_int_t)draft->revision,
        "text", draft->text, "cursor", (json_int_t)draft->cursor));
}

static int
replace_draft(struct snag_view_server *server, struct view_peer *peer, const json_t *message)
{
    const json_t *route = json_object_get(message, "route");
    const json_t *text = json_object_get(message, "text");
    const char *bytes = json_string_value(text);
    size_t length = json_string_length(text);
    uint64_t revision;
    uint64_t edit;
    uint64_t cursor;
    if (!writable_route(route) || !bytes || strlen(bytes) != length ||
        length > SNAG_MAX_DIRECT_PROMPT ||
        snag_json_integer_u64(message, "revision", &revision) < 0 ||
        snag_json_integer_u64(message, "edit", &edit) < 0 || !edit ||
        snag_json_integer_u64(message, "cursor", &cursor) < 0 || cursor > length ||
        snag_vm_text_floor(bytes, length, (size_t)cursor) != cursor)
        return refuse_request(peer, message, "invalid draft or unsupported route");
    struct view_draft *draft = route_draft(server, route);
    if (!draft) return -1;
    peer->draft = draft;
    if (revision != draft->revision) return draft_snapshot(peer, edit, "conflict");
    if (draft->revision == INT64_MAX)
        return refuse_request(peer, message, "draft revision exhausted");
    json_decref(draft->text);
    draft->text = json_incref((json_t *)text);
    draft->cursor = (size_t)cursor;
    ++draft->revision;
    return draft_snapshot(peer, edit, "accepted");
}

static int
submit(struct snag_view_server *server, struct view_peer *peer, const json_t *message)
{
    const char *id = snag_json_string(message, "id");
    const char *text = snag_json_bounded_string(json_object_get(message, "text"),
        SNAG_MAX_DIRECT_PROMPT);
    if (!request_id(id) || !text) return refuse_request(peer, message, "invalid submission");
    bool command = !strcmp(snag_json_string(message, "type"), "command");
    if (command != snag_prompt_command(text))
        return refuse_request(peer, message, "use the command capability for slash commands");
    const json_t *route = json_object_get(message, "route");
    if ((route && !writable_route(route)) || (command && !route))
        return refuse_request(peer, message, "unsupported submission route");
    json_t *default_route = route ? NULL : json_string("rollout");
    struct view_draft *draft = route_draft(server, route ? route : default_route);
    json_decref(default_route);
    if (!draft) return -1;
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    snag_sha256_hex(text, strlen(text), digest);
    struct view_receipt *receipt = find_receipt(server, id);
    if (receipt) {
        if (receipt->command != command || receipt->draft != draft ||
            strcmp(receipt->sha256, digest))
            return refuse_request(peer, message, "request ID already used");
        return reply(peer, json_incref(receipt->result));
    }
    if (server->pending)
        return refuse_request(peer, message, "submission awaiting owner admission");
    uint64_t draft_revision = 0u;
    if (json_object_get(message, "draft_revision")) {
        if (!route ||
            snag_json_integer_u64(message, "draft_revision", &draft_revision) < 0 ||
            draft_revision != draft->revision ||
            strcmp(text, json_string_value(draft->text)))
            return refuse_request(peer, message, "draft changed before submission");
    }
    receipt = calloc(1u, sizeof(*receipt));
    if (!receipt) return -1;
    memcpy(receipt->id, id, sizeof(receipt->id));
    memcpy(receipt->sha256, digest, sizeof(receipt->sha256));
    receipt->result = json_pack("{s:s,s:s,s:s}", "type", "result", "id", id,
        "status", "pending");
    receipt->command_text = command ? strdup(text) : NULL;
    if (!receipt->result || (command && !receipt->command_text)) {
        json_decref(receipt->result);
        free(receipt->command_text);
        free(receipt);
        return -1;
    }
    receipt->pending = true;
    receipt->command = command;
    receipt->draft_revision = draft_revision;
    receipt->draft = draft;
    receipt->next = server->receipts;
    server->receipts = server->pending = receipt;
    peer->waiting = receipt;
    if (server->callbacks.submit(server->callbacks.opaque, id, text, draft->route,
        peer->generation, false) < 0)
        return snag_view_server_result(server, id, "rejected", 0u, "admission unavailable");
    /* Pending is deliberately not an acceptance acknowledgement. The final
     * result is published by the engine only after its durable admission. */
    return reply(peer, json_incref(receipt->result));
}

static int
dispatch(struct snag_view_server *server, struct view_peer *peer, const json_t *message)
{
    const char *type = snag_json_string(message, "type");
    if (!type) return snag_errno(EPROTO);
    const char *fields = !strcmp(type, "hello") ? "type version" :
        (!strcmp(type, "reserve") || !strcmp(type, "reports")) ? "type" :
        !strcmp(type, "receipt") ? "type id" :
        !strcmp(type, "draft_get") ? "type generation route" :
        !strcmp(type, "draft") ? "type generation route revision edit text cursor" :
        !strcmp(type, "command") ? json_object_get(message, "draft_revision") ?
            "type generation id text route draft_revision" : "type generation id text route" :
        !strcmp(type, "submit") ?
            json_object_get(message, "draft_revision") ?
            "type generation id text route draft_revision" : json_object_get(message, "route") ?
            "type generation id text route" : "type generation id text" :
            "type generation";
    if (!snag_json_exact_keys(message, fields)) {
        if (!peer->hello) return snag_errno(EPROTO);
        return refuse_request(peer, message, "unsupported message fields");
    }
    if (!peer->hello) {
        uint64_t version;
        if (strcmp(type, "hello") || snag_json_integer_u64(message, "version", &version) < 0 ||
            version != 1u) return snag_errno(EPROTO);
        peer->hello = true;
        peer->deadline = 0u;
        return reply(peer, json_pack("{s:s,s:i,s:s,s:s,s:[s,s,s,s,s,s,s,s,s,s,s,s,s,s]}",
            "type", "capabilities", "version", 1, "session", server->session,
            "instance", server->instance, "features",
            "observe", "control", "submit", "cancel", "quit", "detach", "receipts", "drafts",
            "commands", "terminal_commands", "reports", "irc_queries", "irc_channels",
            "irc_connections"));
    }
    if (!strcmp(type, "reports")) {
        peer->reports = true;
        peer->reports_ready = false;
        peer->report_cursor = 0u;
        return 0;
    }
    if (!strcmp(type, "receipt")) {
        const char *id = snag_json_string(message, "id");
        if (!request_id(id)) return refuse(peer, "invalid request ID");
        struct view_receipt *receipt = find_receipt(server, id);
        return reply(peer, receipt ? json_incref(receipt->result) :
            json_pack("{s:s,s:s,s:s}", "type", "result", "id", id, "status", "unknown"));
    }
    if (!strcmp(type, "reserve")) {
        if (peer->generation ||
            snag_session_relay_view_reserve(server->relay, &peer->generation) < 0)
            return refuse(peer, "session already has a controller or reservation");
        peer->deadline = snag_monotonic_ms() + VIEW_HANDSHAKE_MS;
        return reply(peer, json_pack("{s:s,s:I}", "type", "reserved", "generation",
            (json_int_t)peer->generation));
    }
    uint64_t generation;
    if (snag_json_integer_u64(message, "generation", &generation) < 0 ||
        !generation || generation != peer->generation)
        return refuse_request(peer, message, "stale controller generation");
    if (!strcmp(type, "commit")) {
        if (peer->bound || snag_session_relay_view_bind(server->relay, generation) < 0)
            return refuse(peer, "controller cannot bind");
        peer->bound = true;
        peer->deadline = 0u;
        server->callbacks.bound(server->callbacks.opaque, generation);
        return reply(peer, json_pack("{s:s,s:I}", "type", "bound", "generation",
            (json_int_t)generation));
    }
    if (!strcmp(type, "detach")) {
        release_peer(server, peer);
        peer->closing = true;
        return reply(peer, json_pack("{s:s}", "type", "detached"));
    }
    if (!peer->bound) return refuse_request(peer, message, "controller is not bound");
    if (!strcmp(type, "draft_get")) {
        const json_t *route = json_object_get(message, "route");
        if (!writable_route(route)) return refuse(peer, "unsupported draft route");
        peer->draft = route_draft(server, route);
        if (!peer->draft) return -1;
        return draft_snapshot(peer, 0u, "snapshot");
    }
    if (!strcmp(type, "draft")) return replace_draft(server, peer, message);
    if (!strcmp(type, "submit") || !strcmp(type, "command"))
        return submit(server, peer, message);
    if (!strcmp(type, "cancel") || !strcmp(type, "quit")) {
        if (server->callbacks.control(server->callbacks.opaque, !strcmp(type, "quit")) < 0)
            return refuse(peer, "control unavailable");
        return reply(peer, json_pack("{s:s,s:s}", "type", "control", "intent", type));
    }
    return refuse(peer, "unsupported message");
}

static int
step_peer(struct snag_view_server *server, struct view_peer *peer)
{
    uint64_t now = snag_monotonic_ms();
    if ((peer->deadline && now >= peer->deadline) ||
        (peer->channel.read_deadline && now >= peer->channel.read_deadline) ||
        (peer->channel.write_deadline && now >= peer->channel.write_deadline)) return -1;
    if (peer->channel.output) return snag_view_channel_write(&peer->channel) < 0 ? -1 : 0;
    if (peer->closing) return -1;
    if (peer->waiting && !peer->waiting->pending) {
        struct view_receipt *receipt = peer->waiting;
        peer->waiting = NULL;
        return reply(peer, json_incref(receipt->result));
    }
    if (server->exiting) {
        if (!peer->hello) return -1;
        peer->closing = true;
        return reply(peer, json_pack("{s:s,s:i}", "type", "exit",
            "status", (int)server->exit_status));
    }
    if (server->stopping) return 0;
    json_t *message = NULL;
    int rc = snag_view_channel_read(&peer->channel, &message);
    if (rc < 0) return -1;
    if (rc) {
        rc = dispatch(server, peer, message);
        json_decref(message);
        return rc;
    }
    if (peer->hello && peer->revision != server->revision && server->state) {
        peer->revision = server->revision;
        return reply(peer, json_pack("{s:s,s:O}", "type", "state", "state", server->state));
    }
    if (peer->bound && peer->draft && peer->draft_revision != peer->draft->revision)
        return draft_snapshot(peer, 0u, "snapshot");
    if (peer->reports && peer->report_cursor < json_array_size(server->reports))
        return reply(peer, json_incref(json_array_get(server->reports, peer->report_cursor++)));
    if (peer->reports && !peer->reports_ready) {
        peer->reports_ready = true;
        return reply(peer, json_pack("{s:s}", "type", "reports_ready"));
    }
    return 0;
}

void
snag_view_server_step(struct snag_view_server *server)
{
    if (!server) return;
    int fd = server->stopping ? -1 : snag_session_listener_accept(&server->listener);
    if (fd >= 0) {
        struct view_peer *peer = calloc(1u, sizeof(*peer));
        if (!peer) (void)close(fd);
        else {
            snag_view_channel_init(&peer->channel, fd);
            peer->deadline = snag_monotonic_ms() + VIEW_HANDSHAKE_MS;
            peer->next = server->peers;
            server->peers = peer;
        }
    }
    struct view_peer **link = &server->peers;
    while (*link) {
        struct view_peer *peer = *link;
        if (step_peer(server, peer) >= 0) {
            link = &peer->next;
            continue;
        }
        *link = peer->next;
        release_peer(server, peer);
        snag_view_channel_close(&peer->channel);
        free(peer);
    }
}
