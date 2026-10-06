/* SPDX-License-Identifier: GPL-2.0-only */
#include "session_view.h"
#include "turn.h"

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

struct view_receipt {
    char id[SNAG_ID_HEX_LEN + 1u], sha256[SNAG_SHA256_HEX_LEN + 1u];
    json_t *result;
    bool pending;
    struct view_receipt *next;
};

struct view_peer {
    struct snag_view_channel channel;
    uint64_t generation, deadline, revision;
    bool hello, bound, closing;
    struct view_receipt *waiting;
    struct view_peer *next;
};

struct snag_view_server {
    struct snag_session_listener listener;
    struct snag_session_relay *relay;
    struct snag_view_callbacks callbacks;
    char session[SNAG_ID_HEX_LEN + 1u], instance[SNAG_ID_HEX_LEN + 1u];
    json_t *state;
    uint64_t revision;
    bool stopping, exiting;
    unsigned int exit_status;
    struct view_peer *peers;
    struct view_receipt *receipts, *pending;
};

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
    if (!callbacks.bound || !callbacks.submit || !callbacks.control ||
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
        free(receipt);
    }
    json_decref(server->state);
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

int
snag_view_server_result(struct snag_view_server *server, const char *id,
    const char *status, uint64_t seq, const char *event)
{
    if (!server) return 0;
    struct view_receipt *receipt = find_receipt(server, id);
    if (!receipt) return snag_errno(ENOENT);
    json_t *result = json_pack("{s:s,s:s,s:s,s:I,s:s}", "type", "result", "id", id,
        "status", status, "seq", (json_int_t)seq, "event", event ? event : "");
    if (!result) return -1;
    json_decref(receipt->result);
    receipt->result = result;
    receipt->pending = false;
    if (server->pending == receipt) server->pending = NULL;
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
refuse_submission(struct view_peer *peer, const json_t *message, const char *error)
{
    const char *id = snag_json_string(message, "id");
    if (!request_id(id)) return refuse(peer, error);
    return reply(peer, json_pack("{s:s,s:s,s:s}", "type", "error", "id", id, "message", error));
}

static int
submit(struct snag_view_server *server, struct view_peer *peer, const json_t *message)
{
    const char *id = snag_json_string(message, "id");
    const char *text = snag_json_bounded_string(json_object_get(message, "text"),
        SNAG_MAX_DIRECT_PROMPT);
    if (!request_id(id) || !text) return refuse_submission(peer, message, "invalid submission");
    /* Command/report and IRC routing are separate advertised capabilities.
     * Plain submissions always address the rollout, including multiline text. */
    if (snag_prompt_command(text))
        return refuse_submission(peer, message, "command capability unavailable");
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    snag_sha256_hex(text, strlen(text), digest);
    struct view_receipt *receipt = find_receipt(server, id);
    if (receipt) {
        if (strcmp(receipt->sha256, digest))
            return refuse_submission(peer, message, "request ID already used");
        return reply(peer, json_incref(receipt->result));
    }
    if (server->pending)
        return refuse_submission(peer, message, "submission awaiting owner admission");
    receipt = calloc(1u, sizeof(*receipt));
    if (!receipt) return -1;
    memcpy(receipt->id, id, sizeof(receipt->id));
    memcpy(receipt->sha256, digest, sizeof(receipt->sha256));
    receipt->result = json_pack("{s:s,s:s,s:s}", "type", "result", "id", id,
        "status", "pending");
    if (!receipt->result) {
        free(receipt);
        return -1;
    }
    receipt->pending = true;
    receipt->next = server->receipts;
    server->receipts = server->pending = receipt;
    peer->waiting = receipt;
    if (server->callbacks.submit(server->callbacks.opaque, id, text, peer->generation) < 0)
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
        !strcmp(type, "reserve") ? "type" : !strcmp(type, "receipt") ? "type id" :
        !strcmp(type, "submit") ? "type generation id text" : "type generation";
    if (!snag_json_exact_keys(message, fields)) {
        if (!peer->hello) return snag_errno(EPROTO);
        return !strcmp(type, "submit") ?
            refuse_submission(peer, message, "unsupported message fields") :
            refuse(peer, "unsupported message fields");
    }
    if (!peer->hello) {
        uint64_t version;
        if (strcmp(type, "hello") || snag_json_integer_u64(message, "version", &version) < 0 ||
            version != 1u) return snag_errno(EPROTO);
        peer->hello = true;
        peer->deadline = 0u;
        return reply(peer, json_pack("{s:s,s:i,s:s,s:s,s:[s,s,s,s,s,s,s]}",
            "type", "capabilities", "version", 1, "session", server->session,
            "instance", server->instance, "features",
            "observe", "control", "submit", "cancel", "quit", "detach", "receipts"));
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
        return !strcmp(type, "submit") ?
            refuse_submission(peer, message, "stale controller generation") :
            refuse(peer, "stale controller generation");
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
    if (!peer->bound) return !strcmp(type, "submit") ?
        refuse_submission(peer, message, "controller is not bound") :
        refuse(peer, "controller is not bound");
    if (!strcmp(type, "submit")) return submit(server, peer, message);
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
