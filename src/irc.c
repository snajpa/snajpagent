/* SPDX-License-Identifier: GPL-2.0-only */
#include "irc_internal.h"
#include "json.h"
#include "snajpagent.h"
#include "net.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define IRC_DEFAULT_ENDPOINT "localhost:6667"
#define IRC_DEFAULT_PORT "6667"
#define IRC_SERVER_PEERS 64u
#define IRC_MEMBERS_MAX 128u
#define IRC_REPLAY_MEMBERS_MAX \
    (IRC_MEMBERS_MAX * (SNAG_CONFIG_IRC_CLIENT_MAX + 1u))
#define IRC_OUTPUT_MAX (6u * 1024u * 1024u)
#define IRC_PENDING_MAX (2u * 1024u * 1024u + 64u * 1024u)
#define IRC_LINE_MAX (SNAG_IRC_LINE_MAX - 2u)
#define IRC_TOPIC_MAX 280u
#define IRC_RETRY_MS 1000u

enum link_role {
    LINK_AGENT, LINK_OPERATOR };

struct irc_query {
    struct irc_query *next;
    enum link_role role;
    char id[SNAG_ID_HEX_LEN + 1u];
    char peer[SNAG_CONFIG_IRC_NICK_MAX + 1u];
};

struct irc_private_send {
    struct irc_private_send *next;
    struct snag_irc_event event;
    size_t length, offset;
    bool await_receipt;
    bool labeled, receipt_echo;
    enum snag_irc_delivery receipt;
    char wire[];
};

struct irc_receipt_batch {
    struct irc_receipt_batch *next, *parent;
    char label[SNAG_ID_HEX_LEN + 1u];
    char id[];
};

struct snag_irc_core;

struct irc_member {
    char nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    bool op;
};

struct irc_channel {
    struct irc_channel *next;
    char id[SNAG_ID_HEX_LEN + 1u];
    char membership[SNAG_ID_HEX_LEN + 1u];
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char topic[513u];
    struct irc_member *members;
    size_t member_count, member_capacity;
    uint64_t restored_seq;
    bool joined, wanted, op, names_active, parting, published;
};

struct irc_replay_member {
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    bool op;
};

struct irc_cursor {
    struct irc_cursor *next;
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char stream[SNAG_ID_HEX_LEN + 1u];
    uint64_t sequence;
};

struct irc_conn {
    struct snag_irc_core *owner;
    snag_socket fd;
    struct snag_buf output;
    struct snag_buf pending;
    size_t output_offset;
    size_t pending_inflight;
    struct irc_private_send *private_sends;
    struct irc_private_send *private_active;
    struct irc_receipt_batch *receipt_batches;
    size_t private_bytes;
    size_t line_limit;
    bool private_broken;
    unsigned char input[SNAG_IRC_LINE_MAX];
    size_t input_len;
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char user[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char accepted_nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char preferred_nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    bool nick_implicit;
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char previous_room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    struct irc_channel *channels;
    char chantypes[128u];
    size_t nick_suffix;
    uint64_t retry_at_ms;
    enum link_role role;
    enum snag_irc_casemapping casemapping;
    bool used;
    bool outgoing;
    bool connecting;
    bool registered;
    /* One notice per outage: repeated retries must not fill the model context
     * or stdout while the server stays down. */
    bool outage_reported;
    bool cap_active;
    bool cap_end;
    bool cap_batch;
    bool cap_server_time;
    bool cap_echo;
    bool cap_labeled;
    unsigned int cap_offered;
    bool cap_catchup;
    bool syncing;
    uint64_t sync_sequence;
    char since_stream[SNAG_ID_HEX_LEN + 1u];
    uint64_t since_sequence;
    char batch[64u];
    char batch_room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char event_stream[SNAG_ID_HEX_LEN + 1u];
    uint64_t event_sequence;
    bool event_op;
    size_t replayed;
    bool history_gap;
    bool agent_role;
    bool joined;
    bool op;
    bool historical;
    bool batch_query;
};

struct irc_message {
    char *tags;
    char *prefix;
    char *command;
    char *params[15u];
    size_t param_count;
};

struct pending_publication {
    struct snag_irc_event event;
    char user[SNAG_CONFIG_IRC_NICK_MAX + 1u];
};

struct snag_irc_core {
    char connection[SNAG_ID_HEX_LEN + 1u];
    char connection_endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char connection_events[2u][SNAG_ID_HEX_LEN + 1u];
    uint64_t generation;
    struct irc_query *queries;
    bool connections_announced;
    uint64_t route_revision;
    char stream[SNAG_ID_HEX_LEN + 1u];
    uint64_t sequence;
    struct irc_cursor *cursors;
    bool deferred;
    struct snag_buf publications;
    snag_socket listener;
    bool hosting;
    char listen[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char server_name[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    bool room_explicit;
    char topic[513u];
    /* Agent/operator identities first, then the hosted room's remote peers. */
    struct irc_conn *conns;
    size_t conn_count;
    struct snag_irc_event *history;
    struct irc_replay_member *replay_members;
    size_t replay_member_count;
    size_t history_limit;
    size_t history_start;
    size_t history_count;
    snag_irc_event_fn event_fn;
    snag_irc_trace_fn trace_fn;
    void *event_opaque;
    bool callback_failed;
};

static size_t utf8_chunk(const char *text, size_t len, size_t max);
static void format_time(uint64_t timestamp_ms, char out[32u]);
static size_t chat_chunk(const char *text, size_t len);
static int trace_wire(struct irc_conn *, char, const char *, size_t);
static int private_flush(struct irc_conn *);
static int private_discard(struct irc_conn *, bool record);
static bool private_ready(const struct irc_conn *);
static void channels_free(struct irc_conn *);
static int
sanitize_text(char *dst, size_t size, const char *src)
{
    size_t used = 0u;

    if (!dst || !size || !src) return snag_errno(EINVAL);
    for (size_t i = 0; src[i];) {
        unsigned char c = (unsigned char)src[i];

        if (c == 0x03u) {
            ++i;
            for (unsigned int n = 0u; n < 2u && src[i] >= '0' && src[i] <= '9'; ++n, ++i) {
            }
            if (src[i] == ',') {
                ++i;
                for (unsigned int n = 0u; n < 2u && src[i] >= '0' && src[i] <= '9'; ++n, ++i) {
                }
            }
            continue;
        }
        if (c < 0x20u || c == 0x7fu) {
            ++i;
            continue;
        }
        if (c == 0xc2u && (unsigned char)src[i + 1u] >= 0x80u && (unsigned char)src[i + 1u] <= 0x9fu) {
            i += 2u;
            continue;
        }
        {
            size_t bytes = 1u;
            if (c >= 0xc2u && c <= 0xdfu) bytes = 2u;
            else if (c >= 0xe0u && c <= 0xefu) bytes = 3u;
            else if (c >= 0xf0u && c <= 0xf4u) bytes = 4u;
            if (used > size - 1u || bytes > size - 1u - used) return snag_errno(EOVERFLOW);
            memcpy(dst + used, src + i, bytes);
            used += bytes;
            i += bytes;
        }
    }
    dst[used] = '\0';
    return 0;
}

static int
irc_casecmp(const char *a, const char *b)
{
    for (;; ++a, ++b) {
        unsigned char ac = snag_irc_fold((unsigned char)*a);
        unsigned char bc = snag_irc_fold((unsigned char)*b);

        if (ac != bc || !ac || !bc) return (ac > bc) - (ac < bc);
    }
}

static bool
link_name_equal(const struct irc_conn *link, const char *a, const char *b)
{
    return snag_irc_name_equal(link->casemapping, a, b);
}

static bool
identifier_unicode_unsafe(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    size_t remaining = strlen(text);

    while (remaining) {
        uint32_t cp;
        size_t bytes = snag_utf8_decode(p, remaining, &cp);

        if (!bytes) return true;

        if (cp == 0x0085u || cp == 0x00a0u || cp == 0x00adu ||
            cp == 0x061cu || cp == 0x1680u || cp == 0x180eu || (cp >= 0x2000u && cp <= 0x200fu) ||
            (cp >= 0x2028u && cp <= 0x202fu) || (cp >= 0x2060u && cp <= 0x206fu) || cp == 0x3000u ||
            cp == 0xfeffu || (cp >= 0xfff9u && cp <= 0xfffbu)) return true;
        p += bytes;
        remaining -= bytes;
    }
    return false;
}

static bool
nick_valid(const char *nick)
{
    size_t len = strlen(nick);

    if (!len || len > SNAG_CONFIG_IRC_NICK_MAX || ((nick[0] >= '0' && nick[0] <= '9') || nick[0] == '-') ||
        !snag_utf8_valid((const unsigned char *)nick, len, true) || identifier_unicode_unsafe(nick))
        return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)nick[i];

        if (c < 0x80u && !snag_irc_nick_char(c)) return false;
    }
    return true;
}

static int
numbered_nick(char out[SNAG_CONFIG_IRC_NICK_MAX + 1u],
              const char *preferred, size_t number, bool replace_zero)
{
    char suffix[32u];
    size_t len = strlen(preferred);
    int n;

    if (replace_zero) {
        if (!len || preferred[len - 1u] != '0') return snag_errno(EINVAL);
        --len;
    }
    n = snprintf(suffix, sizeof(suffix), "%zu", number);
    if (n < 0 || (size_t)n >= sizeof(suffix) || (size_t)n > SNAG_CONFIG_IRC_NICK_MAX)
        return snag_errno(EOVERFLOW);
    if (len + (size_t)n > SNAG_CONFIG_IRC_NICK_MAX) {
        len = SNAG_CONFIG_IRC_NICK_MAX - (size_t)n;
        while (len && ((unsigned char)preferred[len] & 0xc0u) == 0x80u) --len;
    }
    memcpy(out, preferred, len);
    memcpy(out + len, suffix, (size_t)n + 1u);
    return 0;
}

static bool
room_valid(const char *room)
{
    const char *p = room;
    size_t len = strlen(room);

    if (*p == '#') {
        ++p;
        --len;
    }
    if (!len || len > SNAG_CONFIG_IRC_ROOM_MAX ||
        !snag_utf8_valid((const unsigned char *)room, strlen(room), true) || identifier_unicode_unsafe(p))
        return false;
    for (; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x21u || c == 0x7fu || c == ',' || c == ':' || c == '#') return false;
    }
    return true;
}

static int
normalize_room(char *dst, size_t size, const char *room)
{
    if (!room_valid(room)) return snag_errno(EINVAL);
    if (room[0] == '#') return snag_strcpy(dst, size, room) ? 0 : -1;
    if (strlen(room) + 2u > size) return snag_errno(EINVAL);
    dst[0] = '#';
    memcpy(dst + 1u, room, strlen(room) + 1u);
    return 0;
}

static int
split_endpoint(const char *endpoint, char *host, size_t host_size, char *port, size_t port_size)
{
    const char *host_begin = endpoint;
    const char *host_end;
    const char *port_begin = IRC_DEFAULT_PORT;
    size_t host_len;
    unsigned long parsed = 0u;

    if (!endpoint || !*endpoint || strlen(endpoint) > SNAG_CONFIG_IRC_ENDPOINT_MAX) goto invalid;
    if (endpoint[0] == '[') {
        host_begin = endpoint + 1u;
        host_end = strchr(host_begin, ']');
        if (!host_end || host_end == host_begin) goto invalid;
        if (host_end[1] == ':') port_begin = host_end + 2u;
        else if (host_end[1] != '\0') goto invalid;
    } else {
        const char *colon = strrchr(endpoint, ':');
        if (colon && strchr(endpoint, ':') == colon) {
            host_end = colon;
            port_begin = colon + 1u;
        } else if (colon) {
            goto invalid;
        } else {
            host_end = endpoint + strlen(endpoint);
        }
    }
    host_len = (size_t)(host_end - host_begin);
    if (!host_len || host_len >= host_size || !*port_begin || strlen(port_begin) >= port_size ||
        !snag_utf8_valid((const unsigned char *)host_begin, host_len, true)) goto invalid;
    for (size_t i = 0; port_begin[i]; ++i) {
        unsigned char c = (unsigned char)port_begin[i];
        if (c < '0' || c > '9') goto invalid;
        parsed = parsed * 10u + (unsigned long)(c - '0');
        if (parsed > 65535u) goto invalid;
    }
    if (!parsed) goto invalid;
    for (size_t i = 0; i < host_len; ++i) {
        unsigned char c = (unsigned char)host_begin[i];
        if (c <= 0x20u || c == 0x7fu || c == ',' || c == '/') goto invalid;
    }
    memcpy(host, host_begin, host_len);
    host[host_len] = '\0';
    if (identifier_unicode_unsafe(host)) goto invalid;
    memcpy(port, port_begin, strlen(port_begin) + 1u);
    return 0;
invalid: return snag_errno(EINVAL);
}

static bool
endpoint_valid(const char *endpoint)
{
    char host[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char port[6u];

    return split_endpoint(endpoint, host, sizeof(host), port, sizeof(port)) == 0;
}

bool
snag_irc_endpoint_equal(const char *a, const char *b)
{
    char ahost[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char bhost[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char aport[6u];
    char bport[6u];

    return split_endpoint(a, ahost, sizeof(ahost), aport, sizeof(aport)) == 0 &&
           split_endpoint(b, bhost, sizeof(bhost), bport, sizeof(bport)) == 0 &&
           strcasecmp(ahost, bhost) == 0 && strcmp(aport, bport) == 0;
}

static int
config_copy(char *dst, size_t size, const char *src, const char *what, char *error, size_t error_size)
{
    if (!src || !*src || !snag_strcpy(dst, size, src))
        return snag_errorf(error, error_size, "%s exceeds its supported bound", what);
    return 0;
}

bool
snag_irc_enabled(const struct snag_config *config)
{
    return config && (config->irc.listen_explicit || config->irc.client_count != 0u);
}

int
snag_irc_apply_cli(struct snag_config *config, const struct snag_cli *cli, char *error, size_t error_size)
{
    if (!config || !cli) return snag_errno(EINVAL);
    if ((cli->irc_no_listen && cli->irc_listen) || (cli->irc_no_client && cli->irc_client_count))
        return snag_fail(error, error_size, EINVAL, "conflicting positive and negative IRC role options");
    if (cli->irc_no_listen) config->irc.listen_explicit = false;
    if (cli->irc_no_client) {
        config->irc.client_count = 0u;
        memset(config->irc.clients, 0, sizeof(config->irc.clients));
    }
    if (cli->irc_listen && config_copy(config->irc.listen, sizeof(config->irc.listen),
                    cli->irc_listen, "IRC listen endpoint", error, error_size) < 0) return -1;
    if (cli->irc_listen) config->irc.listen_explicit = true;
    if (cli->irc_client_count) {
        memset(config->irc.clients, 0, sizeof(config->irc.clients));
        config->irc.client_count = 0u;
        for (size_t i = 0; i < cli->irc_client_count; ++i) {
            if (config_copy(config->irc.clients[i], sizeof(config->irc.clients[i]),
                            cli->irc_clients[i], "IRC client endpoint", error, error_size) < 0) return -1;
            ++config->irc.client_count;
        }
    }
    if (cli->irc_model_nick) {
        if (config_copy(config->irc.model_nick, sizeof(config->irc.model_nick), cli->irc_model_nick,
                        "IRC model nick", error, error_size) < 0) return -1;
        config->irc.model_nick_implicit = false;
    }
    if (cli->irc_operator_nick) {
        if (config_copy(config->irc.operator_nick, sizeof(config->irc.operator_nick),
                        cli->irc_operator_nick, "IRC operator nick", error, error_size) < 0) return -1;
        config->irc.operator_nick_implicit = false;
    }
    if (cli->irc_room_name && config_copy(config->irc.room_name, sizeof(config->irc.room_name),
                    cli->irc_room_name, "IRC room name", error, error_size) < 0) return -1;
    if (!cli->execute && snag_irc_enabled(config) && cli->prompt && !cli->prompt_after_dashdash)
        return snag_fail(error, error_size, EINVAL, "networked initial chat text must follow --");
    return snag_irc_normalize(config, error, error_size);
}

int
snag_irc_normalize(struct snag_config *config, char *error, size_t error_size)
{
    const char *login;

    if (!config || config->irc.client_count > SNAG_CONFIG_IRC_CLIENT_MAX)
        return snag_fail(error, error_size, EINVAL, "invalid IRC configuration");
    if (!config->irc.model_nick[0]) {
        if (numbered_nick(config->irc.model_nick, "agent", 0u, false) < 0) return -1;
        config->irc.model_nick_implicit = true;
    }
    if (!nick_valid(config->irc.model_nick))
        return snag_fail(error, error_size, EINVAL, "IRC model nick is invalid");
    if (!endpoint_valid(config->irc.listen))
        return snag_errorf(error, error_size, "invalid IRC listen endpoint");
    for (size_t i = 0; i < config->irc.client_count; ++i) {
        if (!endpoint_valid(config->irc.clients[i]))
            return snag_errorf(error, error_size, "invalid IRC client endpoint: %s", config->irc.clients[i]);
        for (size_t j = 0; j < i; ++j)
            if (snag_irc_endpoint_equal(config->irc.clients[i], config->irc.clients[j])) {
                return snag_fail(error, error_size, EINVAL, "duplicate IRC client endpoint: %s",
                          config->irc.clients[i]);
            }
    }
    if (!config->irc.operator_nick[0]) {
        login = getenv("USER");
        if (!login || !nick_valid(login)) login = "operator";
        if (numbered_nick(config->irc.operator_nick, login, 0u, false) < 0) return -1;
        if (irc_casecmp(config->irc.operator_nick, config->irc.model_nick) == 0 &&
            numbered_nick(config->irc.operator_nick, "localop", 0u, false) < 0) return -1;
        config->irc.operator_nick_implicit = true;
    }
    if (!nick_valid(config->irc.operator_nick) ||
        irc_casecmp(config->irc.operator_nick, config->irc.model_nick) == 0) {
        return snag_fail(error, error_size, EINVAL,
                  "IRC operator and model nicks must be valid and distinct");
    }
    if (config->irc.room_name[0]) {
        char normalized[sizeof(config->irc.room_name)];
        if (normalize_room(normalized, sizeof(normalized), config->irc.room_name) < 0)
            return snag_errorf(error, error_size, "invalid IRC room name");
        memcpy(config->irc.room_name, normalized, sizeof(normalized));
    }
    return 0;
}

static snag_socket
open_listener(const char *endpoint, char *error, size_t error_size)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *it;
    char host[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char port[6u];
    snag_socket fd = SNAG_SOCKET_INVALID;
    int saved = EADDRNOTAVAIL;
    int gai;

    if (split_endpoint(endpoint, host, sizeof(host), port, sizeof(port)) < 0)
        return snag_errorf(error, error_size, "invalid IRC listen endpoint");
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    gai = snag_socket_addresses(host, port, &hints, &addresses);
    if (gai != 0) {
        return snag_fail(error, error_size, EADDRNOTAVAIL, "cannot resolve IRC listen endpoint: %s",
                  gai_strerror(gai));
    }
    for (it = addresses; it; it = it->ai_next) {
        fd = snag_socket_open(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == SNAG_SOCKET_INVALID) {
            saved = errno;
            continue;
        }
        if (snag_socket_reuse(fd) == 0 && snag_socket_bind(fd, it->ai_addr, it->ai_addrlen) == 0 &&
            snag_socket_listen(fd, 32) == 0) break;
        saved = errno;
        (void)snag_socket_close(fd);
        fd = SNAG_SOCKET_INVALID;
        if (saved == EADDRINUSE) break;
    }
    snag_socket_addresses_free(addresses);
    if (fd == SNAG_SOCKET_INVALID) {
        snag_errorf(error, error_size, "cannot listen on IRC endpoint %s: %s", endpoint, strerror(saved));
        errno = saved;
    }
    return fd;
}

static void
conn_init(struct irc_conn *conn, struct snag_irc_core *owner)
{
    memset(conn, 0, sizeof(*conn));
    conn->owner = owner;
    conn->fd = SNAG_SOCKET_INVALID;
    conn->line_limit = 512u;
    memcpy(conn->chantypes, "#&", 3u);
    snag_buf_init(&conn->output, IRC_OUTPUT_MAX);
    snag_buf_init(&conn->pending, IRC_PENDING_MAX);
}

static void
receipt_batches_free(struct irc_conn *conn)
{
    while (conn->receipt_batches) {
        struct irc_receipt_batch *batch = conn->receipt_batches;
        conn->receipt_batches = batch->next;
        conn->private_bytes -= sizeof(*batch) + strlen(batch->id) + 1u;
        free(batch);
    }
}

static void
conn_release(struct irc_conn *conn)
{
    (void)private_discard(conn, false);
    receipt_batches_free(conn);
    if (conn->fd != SNAG_SOCKET_INVALID) (void)snag_socket_close(conn->fd);
    snag_buf_free(&conn->output);
    snag_buf_free(&conn->pending);
    channels_free(conn);
    memset(conn, 0, sizeof(*conn));
    conn->fd = SNAG_SOCKET_INVALID;
}

static int
queue_line(struct irc_conn *conn, const char *fmt, ...)
{
    char line[IRC_LINE_MAX + 1u];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n > IRC_LINE_MAX) return snag_errno(EMSGSIZE);
    if (trace_wire(conn, '>', line, (size_t)n) < 0) return -1;
    if (conn->output_offset) {
        memmove(conn->output.data, conn->output.data + conn->output_offset,
                conn->output.len - conn->output_offset);
        conn->output.len -= conn->output_offset;
        conn->output_offset = 0u;
    }
    if ((size_t)n + 2u > conn->output.max - conn->output.len) return snag_errno(EOVERFLOW);
    if (snag_buf_append(&conn->output, line, (size_t)n) < 0 || snag_buf_append(&conn->output, "\r\n", 2u) < 0)
        return -1;
    return 0;
}

static int
flush_conn(struct irc_conn *conn)
{
    if (conn->private_active) {
        if (private_flush(conn) < 0) return -1;
        if (conn->private_active) return 0;
    }
    while (conn->output_offset < conn->output.len) {
        ssize_t written = snag_socket_send(conn->fd, conn->output.data + conn->output_offset,
            conn->output.len - conn->output_offset);

        if (written > 0) {
            conn->output_offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
    if (conn->pending_inflight) {
        if (conn->pending_inflight > conn->pending.len) return snag_errno(EPROTO);
        memmove(conn->pending.data, conn->pending.data + conn->pending_inflight,
                conn->pending.len - conn->pending_inflight);
        conn->pending.len -= conn->pending_inflight;
        conn->pending_inflight = 0u;
    }
    snag_buf_reset(&conn->output);
    conn->output_offset = 0u;
    return private_flush(conn);
}

static bool
event_remembered(enum snag_irc_event_kind kind)
{
    return kind == SNAG_IRC_JOIN || kind == SNAG_IRC_PART || kind == SNAG_IRC_QUIT || kind == SNAG_IRC_NICK ||
           kind == SNAG_IRC_MESSAGE || kind == SNAG_IRC_NOTICE ||
           kind == SNAG_IRC_TOPIC || kind == SNAG_IRC_MODE;
}

static struct irc_cursor *
find_cursor(struct snag_irc_core *irc, const char *endpoint, const char *room, bool create)
{
    struct irc_cursor *cursor;
    for (cursor = irc->cursors; cursor; cursor = cursor->next)
        if (snag_irc_endpoint_equal(cursor->endpoint, endpoint) && !strcmp(cursor->room, room)) return cursor;
    if (!create) return NULL;
    cursor = calloc(1u, sizeof(*cursor));
    if (!cursor) return NULL;
    if (!snag_strcpy(cursor->endpoint, sizeof(cursor->endpoint), endpoint) ||
        !snag_strcpy(cursor->room, sizeof(cursor->room), room)) {
        free(cursor);
        return NULL;
    }
    cursor->next = irc->cursors;
    irc->cursors = cursor;
    return cursor;
}

bool
snag_irc_core_received(struct snag_irc_core *irc, const struct snag_irc_event *event)
{
    if (event->routed && event->route.kind != SNAG_IRC_CHANNEL) return false;
    struct irc_cursor *cursor = find_cursor(irc, event->endpoint, event->room, false);
    return event->stream[0] && cursor && !strcmp(cursor->stream, event->stream) &&
           event->sequence <= cursor->sequence;
}

int
snag_irc_core_accept(struct snag_irc_core *irc, const struct snag_irc_event *event)
{
    if (event->routed && event->route.kind != SNAG_IRC_CHANNEL) return 0;
    if (!event->stream[0]) return 0;
    struct irc_cursor *cursor = find_cursor(irc, event->endpoint, event->room, true);
    if (!cursor) return -1;
    if (strcmp(cursor->stream, event->stream) || event->sequence > cursor->sequence) {
        memcpy(cursor->stream, event->stream, sizeof(cursor->stream));
        cursor->sequence = event->sequence;
    }
    if (irc->hosting && snag_irc_endpoint_equal(irc->listen, event->endpoint) &&
        !strcmp(irc->room, event->room)) {
        memcpy(irc->stream, event->stream, sizeof(irc->stream));
        if (cursor->sequence > irc->sequence) irc->sequence = cursor->sequence;
    }
    return 0;
}

void
snag_irc_core_remember(struct snag_irc_core *irc, const struct snag_irc_event *event)
{
    size_t index;

    if (event->routed && (event->route.kind != SNAG_IRC_CHANNEL ||
        !snag_irc_event_model_visible(event) || event->route.direction == SNAG_IRC_OUTGOING)) {
        return;
    }
    for (size_t i = 0u; event->stream[0] && i < irc->history_count; ++i) {
        const struct snag_irc_event *old = &irc->history[(irc->history_start + i) % irc->history_limit];
        if (old->sequence == event->sequence && !strcmp(old->stream, event->stream)) return;
    }
    if (!irc->history_limit || !event_remembered(event->kind)) return;
    if (irc->history_count < irc->history_limit) {
        index = (irc->history_start + irc->history_count) % irc->history_limit;
        ++irc->history_count;
    } else {
        index = irc->history_start;
        irc->history_start = (irc->history_start + 1u) % irc->history_limit;
    }
    irc->history[index] = *event;
}

static int
emit_event(struct snag_irc_core *irc, const struct snag_irc_event *event, bool remember)
{
    int rc;

    if (snag_irc_core_received(irc, event)) return 0;
    rc = irc->event_fn ? irc->event_fn(irc->event_opaque, event) : 0;
    if (rc == 0 && !irc->deferred) rc = snag_irc_core_accept(irc, event);
    if (rc == 0 && remember) snag_irc_core_remember(irc, event);
    if (rc < 0) irc->callback_failed = true;
    return rc;
}

static void
event_init(struct snag_irc_core *irc, struct snag_irc_event *event,
           enum snag_irc_event_kind kind, const char *endpoint,
           const char *room, const char *nick, const char *text, bool op, bool historical, bool local)
{
    memset(event, 0, sizeof(*event));
    event->kind = kind;
    event->timestamp_ms = snag_time_ms();
    if (endpoint) (void)snprintf(event->endpoint, sizeof(event->endpoint), "%s", endpoint);
    if (room) (void)snprintf(event->room, sizeof(event->room), "%s", room);
    if (nick) (void)snprintf(event->nick, sizeof(event->nick), "%s", nick);
    if (text && sanitize_text(event->text, sizeof(event->text), text) < 0) event->text[0] = '\0';
    event->op = op;
    event->historical = historical;
    event->local = local;
    (void)irc;
}

static void
route_init(struct snag_irc_core *irc, struct snag_irc_event *event,
           enum link_role role, enum snag_irc_conversation_kind kind, const char *conversation)
{
    event->routed = true;
    event->route.kind = kind;
    event->route.identity = role == LINK_AGENT ? SNAG_IRC_AGENT : SNAG_IRC_OPERATOR;
    event->route.generation = irc->generation;
    memcpy(event->endpoint, irc->connection_endpoint, sizeof(event->endpoint));
    memcpy(event->route.connection, irc->connection, sizeof(event->route.connection));
    memcpy(event->route.conversation, conversation, sizeof(event->route.conversation));
}

static struct irc_query *
query_find(struct snag_irc_core *irc, enum link_role role, const char *peer, bool create)
{
    for (struct irc_query *query = irc->queries; query; query = query->next)
        if (query->role == role && link_name_equal(&irc->conns[role], query->peer, peer))
            return query;
    if (!create) return NULL;
    struct irc_query *query = calloc(1u, sizeof(*query));
    if (!query) return NULL;
    if (snag_random_id(query->id) < 0 || !snag_strcpy(query->peer, sizeof(query->peer), peer)) {
        free(query);
        return NULL;
    }
    query->role = role;
    query->next = irc->queries;
    irc->queries = query;
    return query;
}

static void
queries_free(struct snag_irc_core *irc)
{
    while (irc->queries) {
        struct irc_query *query = irc->queries;
        irc->queries = query->next;
        free(query);
    }
}

static struct irc_query *
query_by_id(struct snag_irc_core *irc, const char *id)
{
    for (struct irc_query *query = irc->queries; query; query = query->next)
        if (!strcmp(query->id, id)) return query;
    return NULL;
}

static int
private_state(struct irc_conn *conn, struct irc_private_send *send, enum snag_irc_delivery state)
{
    send->event.route.delivery = state;
    send->event.timestamp_ms = snag_time_ms();
    /* A correlated result supplies the final server body once. Native
     * catch-up supplies its own canonical public event; an ordinary external
     * write without receipts remains explicitly unconfirmed model input. */
    send->event.input = send->event.route.kind == SNAG_IRC_CHANNEL &&
        send->event.route.identity == SNAG_IRC_OPERATOR &&
        (state == SNAG_IRC_ACKNOWLEDGED || (state == SNAG_IRC_WRITTEN &&
            !send->await_receipt && !conn->cap_catchup && !conn->owner->hosting));
    struct irc_query *query = query_by_id(conn->owner, send->event.route.conversation);
    if (query)
        (void)snag_strcpy(send->event.route.peer, sizeof(send->event.route.peer), query->peer);
    return emit_event(conn->owner, &send->event, false);
}

static void
private_remove(struct irc_conn *conn, struct irc_private_send **previous)
{
    struct irc_private_send *send = *previous;
    *previous = send->next;
    if (conn->private_active == send) conn->private_active = NULL;
    conn->private_bytes -= sizeof(*send) + send->length + 1u;
    free(send);
}

static int
private_discard(struct irc_conn *conn, bool record)
{
    int rc = 0;
    while (conn->private_sends) {
        struct irc_private_send *send = conn->private_sends;
        if (send->offset && send->offset < send->length) conn->private_broken = true;
        if (record && private_state(conn, send,
            send->offset ? SNAG_IRC_UNCERTAIN : SNAG_IRC_FAILED) < 0) rc = -1;
        private_remove(conn, &conn->private_sends);
    }
    return rc;
}

static bool
private_ready(const struct irc_conn *conn)
{
    for (const struct irc_private_send *send = conn->private_sends; send; send = send->next)
        if (send->offset < send->length) return true;
    return false;
}

static struct irc_channel *channel_find(const struct irc_conn *, const char *);

static bool
private_current(struct irc_conn *conn, const struct snag_irc_event *event)
{
    struct snag_irc_core *irc = conn->owner;
    enum link_role role = event->route.identity == SNAG_IRC_AGENT ? LINK_AGENT : LINK_OPERATOR;
    struct irc_conn *identity = &irc->conns[role];
    if (!conn->registered || event->route.generation != irc->generation ||
        strcmp(event->route.connection, irc->connection) ||
        !link_name_equal(identity, identity->accepted_nick, event->nick) ||
        !link_name_equal(identity, identity->nick, identity->accepted_nick)) return false;
    if (event->route.kind == SNAG_IRC_CHANNEL) {
        struct irc_channel *channel = channel_find(identity, event->route.target);
        return conn == identity && channel && channel->joined && !channel->parting &&
            !strcmp(channel->id, event->route.conversation) &&
            !strcmp(channel->membership, event->route.membership);
    }
    struct irc_query *query = query_by_id(irc, event->route.conversation);
    return query && query->role == role &&
        link_name_equal(identity, query->peer, event->route.target) &&
        (!irc->hosting || link_name_equal(conn, conn->nick, event->route.target));
}

static int
private_flush(struct irc_conn *conn)
{
    struct irc_private_send **previous = &conn->private_sends;
    while (*previous) {
        struct irc_private_send *send = *previous;
        if (send->offset == send->length) {
            previous = &send->next;
            continue;
        }
        if (!private_current(conn, &send->event) ||
            (!conn->owner->hosting && send->length > conn->line_limit)) {
            bool partial = send->offset != 0u;
            int rc = private_state(conn, send, partial ? SNAG_IRC_UNCERTAIN : SNAG_IRC_FAILED);
            private_remove(conn, previous);
            if (rc < 0 || partial) return snag_errno(ESTALE);
            continue;
        }
        conn->private_active = send;
        ssize_t written = snag_socket_send(conn->fd, send->wire + send->offset,
            send->length - send->offset);
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (written <= 0) return -1;
        send->offset += (size_t)written;
        if (send->offset < send->length) continue;
        conn->private_active = NULL;
        if (private_state(conn, send, SNAG_IRC_WRITTEN) < 0) return -1;
        if (conn->owner->hosting && private_state(conn, send, SNAG_IRC_ACKNOWLEDGED) < 0) return -1;
        if (!send->await_receipt) private_remove(conn, previous);
        else previous = &send->next;
    }
    return 0;
}

static int
query_cancel_sends(struct snag_irc_core *irc, const char *id)
{
    for (size_t i = 0u; i < irc->conn_count; ++i) {
        struct irc_conn *conn = &irc->conns[i];
        struct irc_private_send **previous = &conn->private_sends;
        while (*previous) {
            struct irc_private_send *send = *previous;
            if (strcmp(send->event.route.conversation, id)) {
                previous = &send->next;
                continue;
            }
            if (send->offset && send->offset < send->length) conn->private_broken = true;
            if (private_state(conn, send, send->offset ? SNAG_IRC_UNCERTAIN : SNAG_IRC_FAILED) < 0)
                return -1;
            private_remove(conn, previous);
        }
    }
    return 0;
}

static int
connection_event(struct snag_irc_core *irc, struct irc_conn *link, const char *reason)
{
    struct snag_irc_event event;
    event_init(irc, &event, link->registered ? SNAG_IRC_CONNECTED : SNAG_IRC_DISCONNECTED,
        irc->hosting ? irc->listen : link->endpoint, NULL, link->accepted_nick,
        reason, false, false, true);
    route_init(irc, &event, link->role, SNAG_IRC_CONNECTION_EVENTS,
        irc->connection_events[link->role]);
    return emit_event(irc, &event, false);
}

static int channel_state(struct irc_conn *, struct irc_channel *, const char *);

static int
query_epoch_end(struct snag_irc_core *irc, const char *reason)
{
    for (size_t i = 0u; i < irc->conn_count; ++i)
        if (private_discard(&irc->conns[i], true) < 0) return -1;
    if (irc->generation == INT64_MAX) return snag_errno(EOVERFLOW);
    ++irc->generation;
    queries_free(irc);
    for (size_t role = 0u; role < 2u; ++role) {
        if (connection_event(irc, &irc->conns[role], reason) < 0) return -1;
        for (struct irc_channel *channel = irc->conns[role].channels; channel;
            channel = channel->next)
            if (channel_state(&irc->conns[role], channel, reason) < 0) return -1;
    }
    return 0;
}

static int
query_receive(struct snag_irc_core *irc, struct irc_conn *link, enum snag_irc_event_kind kind,
              const char *sender, const char *target, const char *text, bool historical,
              uint64_t timestamp_ms, const char *source, bool action)
{
    if (!irc->connection[0] || !nick_valid(sender)) return 0;
    struct irc_query *query = query_find(irc, link->role, sender, true);
    if (!query) return -1;
    char clean[SNAG_IRC_LINE_MAX];
    if (sanitize_text(clean, sizeof(clean), text) < 0) return -1;
    for (size_t offset = 0u, len = strlen(clean); offset < len;) {
        char chunk[SNAG_IRC_TEXT_MAX + 1u];
        size_t take = chat_chunk(clean + offset, len - offset);
        if (!take) return -1;
        memcpy(chunk, clean + offset, take);
        chunk[take] = '\0';
        struct snag_irc_event event;
        event_init(irc, &event, kind, irc->hosting ? irc->listen : link->endpoint,
            NULL, sender, chunk, false, historical,
            !historical && link_name_equal(link, sender, link->accepted_nick));
        if (timestamp_ms) event.timestamp_ms = timestamp_ms;
        route_init(irc, &event, link->role, SNAG_IRC_QUERY, query->id);
        (void)snag_strcpy(event.route.peer, sizeof(event.route.peer), query->peer);
        (void)snag_strcpy(event.route.target, sizeof(event.route.target), target);
        if (source) (void)snag_strcpy(event.route.source, sizeof(event.route.source), source);
        event.route.action = action;
        if (emit_event(irc, &event, false) < 0) return -1;
        offset += take;
    }
    return 0;
}

static int
query_lifecycle(struct snag_irc_core *irc, struct irc_conn *link, enum snag_irc_event_kind kind,
                const char *sender, const char *text, uint64_t timestamp_ms)
{
    struct irc_query *query = query_find(irc, link->role, sender, false);
    if (!query) return 0;
    int cancelled = query_cancel_sends(irc, query->id);
    if (cancelled < 0) return -1;
    struct snag_irc_event event;
    /* Persist the canonical previous spelling so the durable alias transition
     * can be checked independently of the endpoint's live CASEMAPPING. */
    event_init(irc, &event, kind, irc->hosting ? irc->listen : link->endpoint,
        NULL, query->peer, text, false, false, false);
    if (timestamp_ms) event.timestamp_ms = timestamp_ms;
    route_init(irc, &event, link->role, SNAG_IRC_QUERY, query->id);
    (void)snag_strcpy(event.route.peer, sizeof(event.route.peer),
        kind == SNAG_IRC_NICK ? text : query->peer);
    if (emit_event(irc, &event, false) < 0) return -1;
    if (kind == SNAG_IRC_NICK) {
        (void)snag_strcpy(query->peer, sizeof(query->peer), text);
    } else {
        struct irc_query **previous = &irc->queries;
        while (*previous != query) previous = &(*previous)->next;
        *previous = query->next;
        free(query);
    }
    return cancelled;
}

static int
parse_message(char *line, struct irc_message *message)
{
    char *p = line;

    memset(message, 0, sizeof(*message));
    if (*p == '@') {
        message->tags = p + 1u;
        p = strchr(p, ' ');
        if (!p) return -1;
        *p++ = '\0';
        while (*p == ' ') ++p;
    }
    if (*p == ':') {
        message->prefix = ++p;
        p = strchr(p, ' ');
        if (!p) return -1;
        *p++ = '\0';
        while (*p == ' ') ++p;
    }
    if (!*p) return -1;
    message->command = p;
    p = strchr(p, ' ');
    if (p) {
        *p++ = '\0';
        while (*p == ' ') ++p;
    }
    for (char *c = message->command; *c; ++c)
        if (*c >= 'a' && *c <= 'z') *c = (char)(*c - ('a' - 'A'));
    while (p && *p && message->param_count < 15u) {
        if (*p == ':') {
            message->params[message->param_count++] = p + 1u;
            break;
        }
        message->params[message->param_count++] = p;
        p = strchr(p, ' ');
        if (!p) break;
        *p++ = '\0';
        while (*p == ' ') ++p;
    }
    return 0;
}

static int
trace_wire(struct irc_conn *conn, char direction, const char *line, size_t len)
{
    struct snag_irc_core *irc = conn->owner;
    if (!irc || !irc->trace_fn) return 0;
    char copy[SNAG_IRC_LINE_MAX];
    struct irc_message message;
    if (!snag_strcpy(copy, sizeof(copy), line)) return -1;
    const char *room = conn->outgoing ? conn->room : irc->room;
    if (parse_message(copy, &message) == 0 &&
        snag_string_in(message.command, "PRIVMSG NOTICE") &&
        (!message.param_count || !room[0] || irc_casecmp(message.params[0], room))) {
        /* Raw transport traces are shared with the hosting session. Private
         * bodies belong only to the participant's typed conversation record. */
        line = "private IRC message (body omitted)";
        len = strlen(line);
    }
    if (irc->trace_fn(irc->event_opaque, 6u, direction,
        conn->outgoing ? conn->endpoint : irc->listen, line, len) < 0) {
        irc->callback_failed = true;
        return -1;
    }
    return 0;
}

static bool
decimal_field(const char *text, size_t len, unsigned int *value)
{
    unsigned int parsed = 0u;

    for (size_t i = 0u; i < len; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        parsed = parsed * 10u + (unsigned int)(text[i] - '0');
    }
    *value = parsed;
    return true;
}

static bool
server_time_value(const char *text, size_t len, uint64_t *timestamp_ms)
{
    static const unsigned int month_days[] = {
        31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u };
    unsigned int year, month, day, hour, minute, second, millis = 0u;
    unsigned int limit;
    int adjusted_year;
    int era;
    unsigned int year_of_era;
    unsigned int day_of_year;
    unsigned int day_of_era;
    int64_t days;

    if (len < 20u || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[len - 1u] != 'Z' || !decimal_field(text, 4u, &year) ||
        !decimal_field(text + 5u, 2u, &month) || !decimal_field(text + 8u, 2u, &day) ||
        !decimal_field(text + 11u, 2u, &hour) || !decimal_field(text + 14u, 2u, &minute) ||
        !decimal_field(text + 17u, 2u, &second) || year < 1970u ||
        month < 1u || month > 12u || hour > 23u || minute > 59u || second > 60u) return false;
    limit = month_days[month - 1u];
    if (month == 2u && (year % 4u == 0u) && (year % 100u != 0u || year % 400u == 0u)) ++limit;
    if (day < 1u || day > limit) return false;
    if (len != 20u) {
        size_t digits = len - 21u;

        if (text[19] != '.' || digits == 0u) return false;
        for (size_t i = 0u; i < digits; ++i) {
            if (text[20u + i] < '0' || text[20u + i] > '9') return false;
            if (i < 3u) millis = millis * 10u + (unsigned int)(text[20u + i] - '0');
        }
        for (size_t i = digits; i < 3u; ++i) millis *= 10u;
    }
    adjusted_year = (int)year - (month <= 2u ? 1 : 0);
    era = adjusted_year / 400;
    year_of_era = (unsigned int)(adjusted_year - era * 400);
    day_of_year = (153u * (month > 2u ? month - 3u : month + 9u) + 2u) / 5u + day - 1u;
    day_of_era = year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;
    days = (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
    if (days < 0) return false;
    *timestamp_ms = ((uint64_t)days * 86400u + (uint64_t)hour * 3600u +
                     (uint64_t)minute * 60u + second) * 1000u + millis;
    return *timestamp_ms != 0u;
}

static uint64_t
server_time_tag(const char *tags)
{
    while (tags && *tags) {
        const char *end = strchr(tags, ';');
        size_t len = end ? (size_t)(end - tags) : strlen(tags);
        uint64_t timestamp_ms;

        if (len > 5u && memcmp(tags, "time=", 5u) == 0 &&
            server_time_value(tags + 5u, len - 5u, &timestamp_ms)) return timestamp_ms;
        tags = end ? end + 1u : NULL;
    }
    return 0u;
}

static const char *
prefix_nick(const char *prefix, char out[SNAG_CONFIG_IRC_NICK_MAX + 1u])
{
    const char *end;
    size_t len;

    if (!prefix) return NULL;
    end = strpbrk(prefix, "!@");
    len = end ? (size_t)(end - prefix) : strlen(prefix);
    if (!len || len > SNAG_CONFIG_IRC_NICK_MAX) return NULL;
    memcpy(out, prefix, len);
    out[len] = '\0';
    return nick_valid(out) ? out : NULL;
}

static bool
channel_syntax_valid(const char *room)
{
    if (!room || !room[0] || (unsigned char)room[0] >= 0x7fu || room[0] == ':') return false;
    size_t len = strlen(room);
    if (len < 2u || len > SNAG_CONFIG_IRC_ROOM_MAX + 1u ||
        !snag_utf8_valid((const unsigned char *)room, len, true) || identifier_unicode_unsafe(room))
        return false;
    for (const unsigned char *at = (const unsigned char *)room; *at; ++at)
        if (*at < 0x21u || *at == 0x7fu || *at == ',') return false;
    return true;
}

static bool
channel_name_valid(const struct irc_conn *link, const char *room)
{
    return channel_syntax_valid(room) && strchr(link->chantypes, room[0]);
}

static struct irc_channel *
channel_find(const struct irc_conn *link, const char *room)
{
    for (struct irc_channel *channel = link->channels; channel; channel = channel->next)
        if (link_name_equal(link, channel->room, room)) return channel;
    return NULL;
}

static struct irc_channel *
channel_open(struct irc_conn *link, const char *room)
{
    struct irc_channel *channel = channel_find(link, room);
    if (channel) return channel;
    if (!channel_name_valid(link, room)) {
        errno = EINVAL;
        return NULL;
    }
    channel = calloc(1u, sizeof(*channel));
    if (!channel) return NULL;
    if (snag_random_id(channel->id) < 0 || snag_random_id(channel->membership) < 0) {
        free(channel);
        return NULL;
    }
    (void)snag_strcpy(channel->room, sizeof(channel->room), room);
    channel->next = link->channels;
    link->channels = channel;
    return channel;
}

static void
channels_free(struct irc_conn *link)
{
    while (link->channels) {
        struct irc_channel *channel = link->channels;
        link->channels = channel->next;
        free(channel->members);
        free(channel);
    }
}

static void
channel_clear(struct irc_channel *channel)
{
    free(channel->members);
    channel->members = NULL;
    channel->member_count = channel->member_capacity = 0u;
    channel->topic[0] = '\0';
    channel->joined = channel->op = channel->names_active = channel->parting = false;
}

static void
channels_reset(struct irc_conn *link)
{
    for (struct irc_channel *channel = link->channels; channel; channel = channel->next)
        channel_clear(channel);
    link->joined = link->op = false;
}

/* The numbered legacy destination still names the configured default room. */
static void
channel_default_status(struct irc_conn *link)
{
    const struct irc_channel *channel = channel_find(link, link->room);
    link->joined = channel && channel->joined && !channel->parting;
    link->op = link->joined && channel->op;
}

static int
channel_state(struct irc_conn *link, struct irc_channel *channel, const char *reason)
{
    struct snag_irc_core *irc = link->owner;
    if (!irc->connection[0]) return 0;
    struct snag_irc_event event;
    bool joined = channel->joined && !channel->parting;
    event_init(irc, &event, joined ? SNAG_IRC_CONNECTED : SNAG_IRC_DISCONNECTED,
        irc->connection_endpoint, channel->room, link->accepted_nick, reason,
        channel->op, false, true);
    route_init(irc, &event, link->role, SNAG_IRC_CHANNEL, channel->id);
    memcpy(event.route.target, channel->room, sizeof(event.route.target));
    memcpy(event.route.membership, channel->membership, sizeof(event.route.membership));
    event.route.joined = joined;
    event.route.rejoin = channel->wanted;
    int rc = emit_event(irc, &event, false);
    if (rc == 0) channel->published = true;
    return rc;
}

static int
channel_leave(struct irc_conn *link, struct irc_channel *channel, const char *reason)
{
    if (query_cancel_sends(link->owner, channel->id) < 0) return -1;
    channel->wanted = false;
    channel_clear(channel);
    channel_default_status(link);
    if (snag_random_id(channel->membership) < 0) return -1;
    return channel_state(link, channel, reason);
}

int
snag_irc_core_channel_open(struct snag_irc_core *irc, const struct snag_irc_query_target *scope,
                          const char *room, bool join, struct snag_irc_channel_target *target,
                          char *error, size_t error_size)
{
    if (!irc || !scope || !target || !room ||
        (unsigned int)scope->identity > SNAG_IRC_AGENT) return snag_errno(EINVAL);
    if (!irc->connection[0] || strcmp(scope->connection, irc->connection) ||
        scope->generation != irc->generation)
        return snag_fail(error, error_size, ESTALE, "IRC connection changed; refresh channels");
    if (irc->hosting)
        return snag_fail(error, error_size, ENOTSUP, "hosted channel actions are unavailable");
    struct irc_conn *link = &irc->conns[scope->identity == SNAG_IRC_AGENT ?
        LINK_AGENT : LINK_OPERATOR];
    if (!link->registered)
        return snag_fail(error, error_size, ENOTCONN, "IRC identity is disconnected");
    if (!channel_name_valid(link, room))
        return snag_fail(error, error_size, EINVAL, "invalid IRC channel name");
    struct irc_channel *channel = channel_find(link, room);
    if (!channel && !join)
        return snag_fail(error, error_size, ENOENT, "IRC channel is not open");
    if (!channel && !(channel = channel_open(link, room))) return -1;
    if (channel->parting)
        return snag_fail(error, error_size, EAGAIN, "IRC channel is still parting");
    bool changed = !channel->published;
    if (join && !channel->wanted) {
        if (!channel->joined && queue_line(link, "JOIN %s", channel->room) < 0) return -1;
        channel->wanted = true;
        changed = true;
    }
    if (changed && channel_state(link, channel, "channel opened") < 0) return -1;
    *target = (struct snag_irc_channel_target){.identity = scope->identity,
        .generation = irc->generation};
    memcpy(target->connection, irc->connection, sizeof(target->connection));
    memcpy(target->conversation, channel->id, sizeof(target->conversation));
    memcpy(target->membership, channel->membership, sizeof(target->membership));
    memcpy(target->room, channel->room, sizeof(target->room));
    return 0;
}

int
snag_irc_core_channel_action(struct snag_irc_core *irc,
                            const struct snag_irc_channel_target *target,
                            enum snag_irc_channel_action action, const char *text,
                            char *error, size_t error_size)
{
    if (!irc || !target || (unsigned int)target->identity > SNAG_IRC_AGENT ||
        (unsigned int)action > SNAG_IRC_CHANNEL_TOPIC) return snag_errno(EINVAL);
    struct irc_conn *link = &irc->conns[target->identity == SNAG_IRC_AGENT ?
        LINK_AGENT : LINK_OPERATOR];
    struct irc_channel *channel = channel_find(link, target->room);
    if (irc->hosting || !link->registered || !channel || !channel->joined || channel->parting ||
        strcmp(target->connection, irc->connection) || target->generation != irc->generation ||
        strcmp(target->conversation, channel->id) ||
        strcmp(target->membership, channel->membership))
        return snag_fail(error, error_size, ESTALE, "IRC membership changed; not performed");
    if (text && (strchr(text, '\r') || strchr(text, '\n') ||
        !snag_utf8_valid((const unsigned char *)text, strlen(text), true)))
        return snag_fail(error, error_size, EINVAL, "invalid IRC channel text");
    char clean[SNAG_IRC_LINE_MAX];
    if (text && sanitize_text(clean, sizeof(clean), text) < 0) return -1;
    const char *command = action == SNAG_IRC_CHANNEL_PART ? "PART" :
        action == SNAG_IRC_CHANNEL_NAMES ? "NAMES" : "TOPIC";
    if (text && action != SNAG_IRC_CHANNEL_NAMES) {
        if (strlen(command) + strlen(channel->room) + strlen(clean) + 5u > link->line_limit)
            return snag_fail(error, error_size, EMSGSIZE, "IRC command exceeds server LINELEN");
        if (queue_line(link, "%s %s :%s", command, channel->room, clean) < 0) return -1;
    } else if (queue_line(link, "%s %s", command, channel->room) < 0) return -1;
    /* Invalidate immediately: a queued PART must not leave a writable old draft
     * while its acknowledgement is still in flight. */
    if (action == SNAG_IRC_CHANNEL_PART) {
        if (query_cancel_sends(irc, channel->id) < 0 ||
            snag_random_id(channel->membership) < 0) return -1;
        channel->wanted = false;
        channel->parting = true;
        channel_default_status(link);
        return channel_state(link, channel, "part requested");
    }
    return 0;
}

static struct irc_member *
member_find(const struct irc_conn *link, struct irc_channel *channel, const char *nick)
{
    for (size_t i = 0u; channel && i < channel->member_count; ++i)
        if (link_name_equal(link, channel->members[i].nick, nick)) return &channel->members[i];
    return NULL;
}

static struct irc_member *
member_add(const struct irc_conn *link, struct irc_channel *channel, const char *nick, bool op)
{
    struct irc_member *member = member_find(link, channel, nick);

    if (member) {
        member->op = op || member->op;
        return member;
    }
    if (!channel || !nick_valid(nick)) {
        errno = EINVAL;
        return NULL;
    }
    if (channel->member_count == channel->member_capacity) {
        size_t capacity = channel->member_capacity ? channel->member_capacity : 16u;
        if (capacity > SIZE_MAX / 2u / sizeof(*channel->members)) {
            errno = EOVERFLOW;
            return NULL;
        }
        capacity *= 2u;
        struct irc_member *members = realloc(channel->members, capacity * sizeof(*members));
        if (!members) return NULL;
        channel->members = members;
        channel->member_capacity = capacity;
    }
    member = &channel->members[channel->member_count++];
    memset(member, 0, sizeof(*member));
    (void)snag_strcpy(member->nick, sizeof(member->nick), nick);
    member->op = op;
    return member;
}

static void
member_remove(const struct irc_conn *link, struct irc_channel *channel, const char *nick)
{
    for (size_t i = 0u; channel && i < channel->member_count; ++i) {
        if (!link_name_equal(link, channel->members[i].nick, nick)) continue;
        memmove(&channel->members[i], &channel->members[i + 1u],
                (channel->member_count - i - 1u) * sizeof(channel->members[0]));
        --channel->member_count;
        return;
    }
}

static struct irc_conn *
server_peer_by_nick(const struct snag_irc_core *irc, const char *nick)
{
    for (size_t i = 0; i < irc->conn_count; ++i)
        if (irc->conns[i].used && irc->conns[i].nick[0] && irc_casecmp(irc->conns[i].nick, nick) == 0)
            return &irc->conns[i];
    return NULL;
}

static void server_drop_peer(struct snag_irc_core *irc, struct irc_conn *peer, const char *reason);

static int
server_event_line(struct snag_irc_core *irc, struct irc_conn *peer,
                  const struct snag_irc_event *event, const char *user, const char *batch)
{
    static const char *const commands[] = {
        [SNAG_IRC_JOIN] = "JOIN", [SNAG_IRC_PART] = "PART",
        [SNAG_IRC_QUIT] = "QUIT", [SNAG_IRC_NICK] = "NICK",
        [SNAG_IRC_MESSAGE] = "PRIVMSG", [SNAG_IRC_NOTICE] = "NOTICE",
        [SNAG_IRC_TOPIC] = "TOPIC", [SNAG_IRC_MODE] = "MODE" };
    char prefix[3u * SNAG_CONFIG_IRC_NICK_MAX + 3u];
    char line[IRC_LINE_MAX + 1u];
    bool channel = event->kind != SNAG_IRC_NICK && event->kind != SNAG_IRC_QUIT;
    int n;

    if ((size_t)event->kind >= sizeof(commands) / sizeof(commands[0]) || !commands[event->kind])
        return snag_errno(EINVAL);
    n = snprintf(prefix, sizeof(prefix), user ? "%s!%s@%s" : "%s", event->nick, user, irc->server_name);
    if (n < 0 || (size_t)n >= sizeof(prefix)) return -1;
    n = snprintf(line, sizeof(line), ":%s %s%s%s%s%s", prefix, commands[event->kind], channel ? " " : "",
                 channel ? event->room : "", event->kind == SNAG_IRC_JOIN ? "" :
                 event->kind == SNAG_IRC_MODE ? " " : " :", event->text);
    if (n < 0 || (size_t)n > IRC_LINE_MAX) return snag_errno(EMSGSIZE);
    char tags[256u] = "", when[32u];
    if (peer->cap_catchup) {
        format_time(event->timestamp_ms, when);
        (void)snprintf(tags, sizeof(tags), "@saj-id=%s:%llu;saj-kind=%s;saj-op=%u;time=%s%s%s ",
            event->stream, (unsigned long long)event->sequence,
            snag_irc_kind_name(event->kind), event->op ? 1u : 0u,
            when, batch ? ";batch=" : "", batch ? batch : "");
    } else if (peer->cap_server_time) {
        format_time(event->timestamp_ms, when);
        (void)snprintf(tags, sizeof(tags), "@time=%s ", when);
    }
    return queue_line(peer, "%s%s", tags, line);
}

static int
server_broadcast(struct snag_irc_core *irc, const struct snag_irc_event *event, const char *user)
{
    for (size_t i = 2u; i < irc->conn_count; ++i) {
        struct irc_conn *peer = &irc->conns[i];
        if (peer->used && peer->joined && (!peer->syncing || !peer->cap_catchup) &&
            server_event_line(irc, peer, event, user, NULL) < 0)
            server_drop_peer(irc, peer, "output queue exceeded");
    }
    return irc->callback_failed ? -1 : 0;
}

static int
server_publish(struct snag_irc_core *irc, enum snag_irc_event_kind kind,
               const char *nick, const char *user, const char *text, bool op, bool local)
{
    struct snag_irc_event event;

    event_init(irc, &event, kind, irc->listen, irc->room, nick, text, op, false, local);
    if (irc->sequence == INT64_MAX) { errno = EOVERFLOW; return -1; }
    memcpy(event.stream, irc->stream, sizeof(event.stream));
    event.sequence = ++irc->sequence;
    if (emit_event(irc, &event, !irc->deferred) < 0) return -1;
    if (!irc->deferred) return server_broadcast(irc, &event, user);
    struct pending_publication publication = {.event = event};
    if (user && !snag_strcpy(publication.user, sizeof(publication.user), user)) return -1;
    return snag_buf_append(&irc->publications, &publication, sizeof(publication));
}

static int
server_send_names(struct snag_irc_core *irc, struct irc_conn *peer)
{
    for (size_t i = 0; i < irc->conn_count; ++i) {
        struct irc_conn *it = &irc->conns[i];
        if (!it->used || !it->joined) continue;
        if (queue_line(peer, ":%s 353 %s = %s :%s%s", irc->server_name,
                       peer->nick, irc->room, it->op ? "@" : "", it->nick) < 0) return -1;
    }
    return queue_line(peer, ":%s 366 %s %s :End of NAMES list", irc->server_name, peer->nick, irc->room);
}

static void
format_time(uint64_t timestamp_ms, char out[32u])
{
    time_t seconds = (time_t)(timestamp_ms / 1000u);
    struct tm tm;

    if (!snag_gmtime(&seconds, &tm) || strftime(out, 32u, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0)
        (void)snprintf(out, 32u, "1970-01-01T00:00:00Z");
}

static const char *
event_kind_text(enum snag_irc_event_kind kind)
{
    switch (kind) {
    case SNAG_IRC_JOIN: return "joined";
    case SNAG_IRC_PART: return "left";
    case SNAG_IRC_QUIT: return "quit";
    case SNAG_IRC_NICK: return "is now known as";
    case SNAG_IRC_TOPIC: return "changed the topic to";
    case SNAG_IRC_MODE: return "changed mode";
    case SNAG_IRC_CONNECTED: return "connected";
    case SNAG_IRC_DISCONNECTED: return "disconnected";
    case SNAG_IRC_MESSAGE: case SNAG_IRC_NOTICE:
    case SNAG_IRC_HISTORY_READY: break;
    }
    return "event";
}

static bool
hosted_history_event(const struct snag_irc_core *irc, const struct snag_irc_event *event)
{
    return strcmp(event->room, irc->room) == 0 && snag_irc_endpoint_equal(event->endpoint, irc->listen);
}

int
snag_irc_core_replay_hosted_history(const struct snag_irc_core *irc, snag_irc_event_fn render, void *opaque)
{
    bool replayed = false;
    if (!irc || !render) return snag_errno(EINVAL);
    if (!irc->hosting) return 0;
    for (size_t i = 0u; i < irc->history_count; ++i) {
        struct snag_irc_event event = irc->history[(irc->history_start + i) % irc->history_limit];

        if (!hosted_history_event(irc, &event)) continue;
        event.historical = true;
        if (render(opaque, &event) < 0) return -1;
        replayed = true;
    }
    /* This callback is display-only: do not append or broadcast the boundary.
     * Keep it scoped to the replayed room so per-room presentation queues
     * cannot strand the completion marker in the unselected empty room. */
    struct snag_irc_event ready = {.kind = SNAG_IRC_HISTORY_READY, .text = "replayed"};
    if (!snag_strcpy(ready.endpoint, sizeof(ready.endpoint), irc->listen) ||
        !snag_strcpy(ready.room, sizeof(ready.room), irc->room)) return snag_errno(EOVERFLOW);
    return replayed ? render(opaque, &ready) : 0;
}

static int
server_send_history(struct snag_irc_core *irc, struct irc_conn *peer)
{
    char batch_id[17u];
    uint64_t after = 0u;

    (void)snprintf(batch_id, sizeof(batch_id), "%08llx", (unsigned long long)(snag_time_ms() & 0xffffffffu));
    if (peer->cap_catchup) {
        after = !strcmp(peer->since_stream, irc->stream) ? peer->since_sequence : 0u;
        uint64_t oldest = irc->history_count ? irc->history[irc->history_start].sequence : irc->sequence + 1u;
        bool gap = after && (after < oldest - 1u || after > irc->sequence);
        if (after > irc->sequence) after = 0u;
        if (queue_line(peer, ":%s BATCH +%s chathistory %s %s %u", irc->server_name,
                       batch_id, irc->room, irc->stream, gap ? 1u : 0u) < 0) return -1;
    } else if (peer->cap_batch && queue_line(peer, ":%s BATCH +%s chathistory %s",
                   irc->server_name, batch_id, irc->room) < 0) return -1;
    for (size_t i = 0; i < irc->history_count; ++i) {
        const struct snag_irc_event *event = &irc->history[(irc->history_start + i) % irc->history_limit];
        char when[32u];
        const char *tag = "";
        char tag_buf[96u];

        if (!hosted_history_event(irc, event)) continue;
        if (peer->cap_catchup) {
            if (event->sequence > after && server_event_line(irc, peer, event, "history", batch_id) < 0)
                return -1;
            continue;
        }
        format_time(event->timestamp_ms, when);
        if (peer->cap_batch) {
            (void)snprintf(tag_buf, sizeof(tag_buf), peer->cap_server_time ? "@batch=%s;time=%s " :
                                                   "@batch=%s ", batch_id, when);
            tag = tag_buf;
        } else if (peer->cap_server_time) {
            (void)snprintf(tag_buf, sizeof(tag_buf), "@time=%s ", when);
            tag = tag_buf;
        }
        if (peer->cap_batch && (event->kind == SNAG_IRC_MESSAGE || event->kind == SNAG_IRC_NOTICE)) {
            if (queue_line(peer, "%s:%s!user@%s %s %s :%s", tag, event->nick, irc->server_name,
                           event->kind == SNAG_IRC_MESSAGE ? "PRIVMSG" : "NOTICE",
                           irc->room, event->text) < 0) return -1;
        } else if (queue_line(peer, "%s:%s NOTICE %s :[history %s] %s%s%s %s %s", tag,
                   irc->server_name, peer->nick, when, event->op ? "@" : "", event->nick,
                   event->nick[0] ? " " : "", event_kind_text(event->kind), event->text) < 0) {
            return -1;
        }
    }
    return peer->cap_catchup || peer->cap_batch ?
        queue_line(peer, ":%s BATCH -%s", irc->server_name, batch_id) : 0;
}

static int server_finish_join(struct snag_irc_core *, struct irc_conn *);

void
snag_irc_core_defer(struct snag_irc_core *irc)
{
    irc->deferred = true;
}

int
snag_irc_core_ack(struct snag_irc_core *irc, const struct snag_irc_event *event)
{
    if (snag_irc_core_accept(irc, event) < 0) return -1;
    if (!irc->hosting || strcmp(irc->stream, event->stream)) return 0;
    size_t used = 0u;
    while (used < irc->publications.len) {
        struct pending_publication item;
        memcpy(&item, irc->publications.data + used, sizeof(item));
        if (item.event.sequence > event->sequence) break;
        snag_irc_core_remember(irc, &item.event);
        if (server_broadcast(irc, &item.event, item.user[0] ? item.user : NULL) < 0) return -1;
        used += sizeof(item);
    }
    if (used) {
        memmove(irc->publications.data, irc->publications.data + used, irc->publications.len - used);
        irc->publications.len -= used;
    }
    for (size_t i = 2u; i < irc->conn_count; ++i) {
        struct irc_conn *peer = &irc->conns[i];
        if (peer->used && peer->syncing && peer->sync_sequence <= event->sequence) {
            if (server_finish_join(irc, peer) < 0) return -1;
        }
    }
    return 0;
}

static int
server_welcome(struct snag_irc_core *irc, struct irc_conn *peer)
{
    if (peer->registered || !peer->nick[0] || !peer->user[0] || (peer->cap_active && !peer->cap_end))
        return 0;
    peer->registered = true;
    if (queue_line(peer, ":%s 001 %s :Welcome to " SNAJPAGENT_NAME " IRC",
                   irc->server_name, peer->nick) < 0 ||
        queue_line(peer, ":%s 005 %s CHANTYPES=# PREFIX=(o)@ SAJROOM=%s "
                   "LINELEN=%u :are supported", irc->server_name, peer->nick,
                   irc->room, SNAG_IRC_LINE_MAX) < 0 ||
        queue_line(peer, ":%s 376 %s :End of MOTD", irc->server_name, peer->nick) < 0) return -1;
    return 0;
}

static int
server_finish_join(struct snag_irc_core *irc, struct irc_conn *peer)
{
    if (queue_line(peer, ":%s 332 %s %s :%s", irc->server_name, peer->nick, irc->room, irc->topic) < 0 ||
        server_send_names(irc, peer) < 0 || server_send_history(irc, peer) < 0) return -1;
    peer->syncing = false;
    return 0;
}

static int
server_join(struct snag_irc_core *irc, struct irc_conn *peer, const char *room)
{
    char mode_text[SNAG_CONFIG_IRC_NICK_MAX + 4u];

    if (!peer->registered) return queue_line(peer, ":%s 451 * :You have not registered", irc->server_name);
    if (irc_casecmp(room, irc->room) != 0) return queue_line(peer, ":%s 403 %s %s :No such channel",
                          irc->server_name, peer->nick, room);
    if (peer->joined) return 0;
    peer->joined = true;
    peer->syncing = irc->deferred || peer->cap_catchup;
    peer->op = false;
    (void)snag_strcpy(peer->room, sizeof(peer->room), irc->room);
    if (server_publish(irc, SNAG_IRC_JOIN, peer->nick, peer->user, "", false, false) < 0) return -1;
    peer->op = !peer->agent_role;
    (void)snprintf(mode_text, sizeof(mode_text), "+o %s", peer->nick);
    if (peer->op && server_publish(irc, SNAG_IRC_MODE, irc->server_name, NULL,
                       mode_text, false, false) < 0) return -1;
    peer->sync_sequence = irc->sequence;
    return irc->deferred ? 0 : server_finish_join(irc, peer);
}

static int
server_private_line(struct snag_irc_core *irc, struct irc_conn *recipient,
                    const struct irc_conn *sender, enum snag_irc_event_kind kind,
                    const char *target, const char *text, bool action)
{
    char when[32u];
    format_time(snag_time_ms(), when);
    return queue_line(recipient, "%s%s%s:%s!%s@%s %s %s :%s%s%s",
        recipient->cap_server_time ? "@time=" : "",
        recipient->cap_server_time ? when : "", recipient->cap_server_time ? " " : "",
        sender->nick, sender->user, irc->server_name,
        kind == SNAG_IRC_MESSAGE ? "PRIVMSG" : "NOTICE", target,
        action ? "\001ACTION " : "", text, action ? "\001" : "");
}

int
snag_irc_core_query_open(struct snag_irc_core *irc, enum snag_irc_identity identity,
                        const char *peer, struct snag_irc_query_target *target,
                        char *error, size_t error_size)
{
    if (!irc || !irc->connection[0] || !target || (unsigned int)identity > SNAG_IRC_AGENT ||
        !peer || !nick_valid(peer))
        return snag_fail(error, error_size, EINVAL, "invalid IRC query target");
    enum link_role role = identity == SNAG_IRC_AGENT ? LINK_AGENT : LINK_OPERATOR;
    struct irc_query *query = query_find(irc, role, peer, false);
    if (!query) {
        query = query_find(irc, role, peer, true);
        if (!query) return -1;
        struct snag_irc_event event;
        struct irc_conn *link = &irc->conns[role];
        event_init(irc, &event, link->registered ? SNAG_IRC_CONNECTED : SNAG_IRC_DISCONNECTED,
            irc->connection_endpoint, NULL, link->accepted_nick, "query opened",
            false, false, true);
        route_init(irc, &event, role, SNAG_IRC_QUERY, query->id);
        (void)snag_strcpy(event.route.peer, sizeof(event.route.peer), query->peer);
        if (emit_event(irc, &event, false) < 0) return -1;
    }
    memset(target, 0, sizeof(*target));
    memcpy(target->connection, irc->connection, sizeof(target->connection));
    memcpy(target->conversation, query->id, sizeof(target->conversation));
    memcpy(target->peer, query->peer, sizeof(target->peer));
    target->identity = identity;
    target->generation = irc->generation;
    return 0;
}

int
snag_irc_core_query_open_frozen(struct snag_irc_core *irc,
                              const struct snag_irc_query_target *scope,
                              const char *peer, struct snag_irc_query_target *target,
                              char *error, size_t error_size)
{
    if (!irc || !scope || strcmp(scope->connection, irc->connection) ||
        scope->generation != irc->generation) {
        return snag_fail(error, error_size, ESTALE,
            "IRC connection changed; refresh before opening");
    }
    return snag_irc_core_query_open(irc, scope->identity, peer, target, error, error_size);
}

static int
chat_send_chunk(struct snag_irc_core *irc, struct irc_conn *sender, struct irc_conn *recipient,
                const struct snag_irc_event_route *route, enum snag_irc_event_kind kind,
                const char *text, size_t length, bool action, struct snag_buf *report)
{
    char raw[SNAG_IRC_TEXT_MAX + 1u];
    char clean[sizeof(raw)];
    memcpy(raw, text, length);
    raw[length] = '\0';
    if (sanitize_text(clean, sizeof(clean), raw) < 0) return -1;
    if (!clean[0]) return 0;
    char id[SNAG_ID_HEX_LEN + 1u];
    if (snag_random_id(id) < 0) return -1;
    bool labeled = !irc->hosting && sender->cap_batch && sender->cap_labeled;
    char wire[SNAG_IRC_LINE_MAX + 1u];
    char prefix[3u * SNAG_CONFIG_IRC_NICK_MAX + 8u] = {0};
    if (irc->hosting) {
        int n = snprintf(prefix, sizeof(prefix), ":%s!%s@%s ", sender->accepted_nick,
            sender->user, irc->server_name);
        if (n < 0 || (size_t)n >= sizeof(prefix)) return snag_errno(EOVERFLOW);
    }
    int n = snprintf(wire, sizeof(wire), "%s%s%s%s%s %s :%s%s%s\r\n",
        labeled ? "@label=" : "", labeled ? id : "", labeled ? " " : "", prefix,
        kind == SNAG_IRC_MESSAGE ? "PRIVMSG" : "NOTICE", route->target,
        action ? "\001ACTION " : "", clean, action ? "\001" : "");
    if (n < 0 || (size_t)n >= sizeof(wire)) return snag_errno(EMSGSIZE);
    size_t bytes = sizeof(struct irc_private_send) + (size_t)n + 1u;
    if (bytes > IRC_PENDING_MAX - recipient->pending.len - recipient->private_bytes)
        return snag_errno(EOVERFLOW);
    struct irc_private_send *send = calloc(1u, bytes);
    if (!send) return -1;
    send->length = (size_t)n;
    memcpy(send->wire, wire, send->length + 1u);
    send->labeled = labeled;
    /* Only the native private-query path has unambiguous unlabeled ordering. */
    send->await_receipt = labeled || (route->kind == SNAG_IRC_QUERY && !irc->hosting &&
        sender->cap_catchup && sender->cap_echo && kind == SNAG_IRC_MESSAGE);
    const struct irc_channel *channel = route->kind == SNAG_IRC_CHANNEL ?
        channel_find(sender, route->target) : NULL;
    event_init(irc, &send->event, kind, irc->connection_endpoint,
        channel ? channel->room : NULL, sender->accepted_nick,
        clean, channel && channel->op, false, true);
    send->event.routed = true;
    send->event.route = *route;
    send->event.route.direction = SNAG_IRC_OUTGOING;
    send->event.route.action = action;
    memcpy(send->event.route.send, id, sizeof(id));
    /* Reserve the report before committing a send so an allocation failure
     * cannot conceal an accepted chunk from the caller. */
    size_t before = report ? report->len : 0u;
    if (report && snag_buf_printf(report, "%s: queued to %s\n",
        send->event.route.send, route->target) < 0) {
        free(send);
        return -1;
    }
    if (private_state(recipient, send, SNAG_IRC_PENDING) < 0) {
        if (report) report->len = before;
        free(send);
        return -1;
    }
    if (irc->hosting && recipient->fd == SNAG_SOCKET_INVALID) {
        int rc = 0;
        if (recipient != sender)
            rc = query_receive(irc, recipient, kind, sender->accepted_nick, recipient->nick,
                clean, false, 0u, NULL, action);
        if (rc == 0) rc = private_state(recipient, send, SNAG_IRC_ACKNOWLEDGED);
        free(send);
        return rc < 0 ? -1 : 1;
    }
    struct irc_private_send **tail = &recipient->private_sends;
    while (*tail) tail = &(*tail)->next;
    *tail = send;
    recipient->private_bytes += bytes;
    return 1;
}

static int
chat_send(struct snag_irc_core *irc, struct irc_conn *sender, struct irc_conn *recipient,
          const struct snag_irc_event_route *route, enum snag_irc_event_kind kind,
          const char *text, bool action, struct snag_buf *report, char *error, size_t error_size)
{
    size_t budget = irc->hosting ? SNAG_IRC_LINE_MAX : sender->line_limit;
    size_t overhead = strlen(kind == SNAG_IRC_MESSAGE ? "PRIVMSG" : "NOTICE") +
        strlen(route->target) + 5u + (action ? 9u : 0u);
    if (irc->hosting) overhead += strlen(sender->accepted_nick) + strlen(sender->user) +
        strlen(irc->server_name) + 4u;
    else if (sender->cap_batch && sender->cap_labeled) overhead += SNAG_ID_HEX_LEN + 8u;
    if (budget <= overhead) return snag_fail(error, error_size, EMSGSIZE, "IRC line is too short");
    budget -= overhead;
    if (budget > SNAG_IRC_TEXT_MAX) budget = SNAG_IRC_TEXT_MAX;
    bool queued = false;
    const char *cursor = text;
    while (*cursor) {
        const char *newline = strchr(cursor, '\n');
        size_t remaining = newline ? (size_t)(newline - cursor) : strlen(cursor);
        while (remaining) {
            size_t take = utf8_chunk(cursor, remaining, budget);
            int rc = take ? chat_send_chunk(irc, sender, recipient, route, kind,
                cursor, take, action, report) : snag_errno(EINVAL);
            if (rc < 0) {
                return snag_errorf(error, error_size,
                    "cannot queue IRC message: %s", strerror(errno));
            }
            queued |= rc > 0;
            cursor += take;
            remaining -= take;
        }
        if (newline) ++cursor;
    }
    return queued ? 0 : snag_fail(error, error_size, EINVAL,
        "IRC message requires a nonempty line");
}

int
snag_irc_core_query_send(struct snag_irc_core *irc, const struct snag_irc_query_target *target,
                        enum snag_irc_event_kind kind, const char *text, bool action,
                        struct snag_buf *report, char *error, size_t error_size)
{
    if (!irc || !target || (unsigned int)target->identity > SNAG_IRC_AGENT ||
        (kind != SNAG_IRC_MESSAGE && kind != SNAG_IRC_NOTICE) ||
        (action && kind != SNAG_IRC_MESSAGE) || !text || !*text ||
        !snag_utf8_valid((const unsigned char *)text, strlen(text), true))
        return snag_fail(error, error_size, EINVAL, "IRC query requires nonempty UTF-8 chat");
    enum link_role role = target->identity == SNAG_IRC_AGENT ? LINK_AGENT : LINK_OPERATOR;
    struct irc_conn *sender = &irc->conns[role];
    struct irc_query *query = query_by_id(irc, target->conversation);
    if (!query || query->role != role || target->generation != irc->generation ||
        strcmp(target->connection, irc->connection) ||
        strcmp(target->peer, query->peer))
        return snag_fail(error, error_size, ESTALE, "IRC query changed; reopen before sending");
    if (!sender->registered || !link_name_equal(sender, sender->nick, sender->accepted_nick))
        return snag_fail(error, error_size, ENOTCONN, "IRC identity is disconnected or renaming");
    struct irc_conn *recipient = irc->hosting ? server_peer_by_nick(irc, query->peer) : sender;
    if (!recipient || !recipient->registered || recipient->private_broken)
        return snag_fail(error, error_size, ENOENT, "IRC peer is unavailable; not sent");

    struct snag_irc_event routing = {0};
    route_init(irc, &routing, sender->role, SNAG_IRC_QUERY, query->id);
    memcpy(routing.route.peer, query->peer, sizeof(routing.route.peer));
    (void)snag_strcpy(routing.route.target, sizeof(routing.route.target), query->peer);
    return chat_send(irc, sender, recipient, &routing.route, kind, text, action,
        report, error, error_size);
}

int
snag_irc_core_channel_send(struct snag_irc_core *irc, const struct snag_irc_channel_target *target,
                          enum snag_irc_event_kind kind, const char *text, bool action,
                          struct snag_buf *report, char *error, size_t error_size)
{
    if (!irc || !target || (unsigned int)target->identity > SNAG_IRC_AGENT ||
        (kind != SNAG_IRC_MESSAGE && kind != SNAG_IRC_NOTICE) ||
        (action && kind != SNAG_IRC_MESSAGE) || !text || !*text ||
        !snag_utf8_valid((const unsigned char *)text, strlen(text), true))
        return snag_fail(error, error_size, EINVAL, "IRC channel requires nonempty UTF-8 chat");
    if (irc->hosting)
        return snag_fail(error, error_size, ENOTSUP, "hosted channel sends are unavailable");
    enum link_role role = target->identity == SNAG_IRC_AGENT ? LINK_AGENT : LINK_OPERATOR;
    struct irc_conn *sender = &irc->conns[role];
    struct irc_channel *channel = channel_find(sender, target->room);
    if (!channel || !channel->joined || channel->parting ||
        target->generation != irc->generation || strcmp(target->connection, irc->connection) ||
        strcmp(target->conversation, channel->id) ||
        strcmp(target->membership, channel->membership)) {
        return snag_fail(error, error_size, ESTALE,
                         "IRC membership changed; reopen before sending");
    }
    if (!sender->registered || sender->private_broken ||
        !link_name_equal(sender, sender->nick, sender->accepted_nick))
        return snag_fail(error, error_size, ENOTCONN, "IRC identity is disconnected or renaming");
    struct snag_irc_event routing = {0};
    route_init(irc, &routing, role, SNAG_IRC_CHANNEL, channel->id);
    memcpy(routing.route.target, channel->room, sizeof(routing.route.target));
    memcpy(routing.route.membership, channel->membership, sizeof(routing.route.membership));
    routing.route.joined = channel->joined;
    routing.route.rejoin = channel->wanted;
    return chat_send(irc, sender, sender, &routing.route, kind, text, action,
        report, error, error_size);
}

static int
server_private(struct snag_irc_core *irc, struct irc_conn *peer,
               enum snag_irc_event_kind kind, const char *target, const char *text)
{
    struct irc_conn *recipient = server_peer_by_nick(irc, target);
    if (!recipient || !recipient->registered) {
        return kind == SNAG_IRC_NOTICE ? 0 : queue_line(peer,
            ":%s 401 %s %s :No such nick", irc->server_name, peer->nick, target);
    }
    if (recipient->fd == SNAG_SOCKET_INVALID && !irc->connection[0]) {
        return kind == SNAG_IRC_NOTICE ? 0 : queue_line(peer,
            ":%s 531 %s %s :Recipient has no query receiver",
            irc->server_name, peer->nick, target);
    }
    char clean[IRC_LINE_MAX + 1u];
    char raw[IRC_LINE_MAX + 1u];
    size_t len = strlen(text);
    bool action = kind == SNAG_IRC_MESSAGE && len >= 9u &&
        !memcmp(text, "\001ACTION ", 8u) && text[len - 1u] == '\001';
    if (action) {
        memcpy(raw, text + 8u, len - 9u);
        raw[len - 9u] = '\0';
        text = raw;
    }
    if (sanitize_text(clean, sizeof(clean), text) < 0 || !clean[0]) return 0;
    for (size_t offset = 0u, length = strlen(clean); offset < length;) {
        char chunk[SNAG_IRC_TEXT_MAX + 1u];
        size_t take = chat_chunk(clean + offset, length - offset);
        if (!take) return -1;
        memcpy(chunk, clean + offset, take);
        chunk[take] = '\0';
        int delivered = recipient->fd == SNAG_SOCKET_INVALID ?
            query_receive(irc, recipient, kind, peer->nick, recipient->nick, chunk,
                false, 0u, NULL, action) :
            server_private_line(irc, recipient, peer, kind, recipient->nick, chunk, action);
        if (delivered < 0) {
            if (irc->callback_failed) return -1;
            server_drop_peer(irc, recipient, "private output queue full");
            return kind == SNAG_IRC_NOTICE ? 0 : queue_line(peer,
                ":%s 531 %s %s :Recipient output queue full",
                irc->server_name, peer->nick, target);
        }
        if (peer->cap_echo && peer != recipient &&
            server_private_line(irc, peer, peer, kind, recipient->nick, chunk, action) < 0)
            return -1;
        offset += take;
    }
    return 0;
}

static int
server_chat(struct snag_irc_core *irc, struct irc_conn *peer,
            enum snag_irc_event_kind kind, const char *target, const char *text)
{
    char clean[IRC_LINE_MAX + 1u];

    if (!peer->registered) return kind == SNAG_IRC_NOTICE ? 0 :
        queue_line(peer, ":%s 451 * :You have not registered", irc->server_name);
    if (!*text || strchr(text, '\r') || strchr(text, '\n') ||
        !snag_utf8_valid((const unsigned char *)text, strlen(text), true))
        return kind == SNAG_IRC_NOTICE ? 0 : queue_line(peer, ":%s 412 %s :No text to send",
            irc->server_name, peer->nick);
    if (*target != '#') return server_private(irc, peer, kind, target, text);
    if (!peer->joined || irc_casecmp(target, irc->room) != 0) {
        if (kind == SNAG_IRC_NOTICE) return 0;
        return queue_line(peer, ":%s 404 %s %s :Cannot send to channel",
                          irc->server_name, peer->nick, target);
    }
    if (!*text || strchr(text, '\r') || strchr(text, '\n') ||
        !snag_utf8_valid((const unsigned char *)text, strlen(text), true))
        return kind == SNAG_IRC_NOTICE ? 0 : queue_line(peer, ":%s 412 %s :No text to send",
                       irc->server_name, peer->nick);
    if (sanitize_text(clean, sizeof(clean), text) < 0 || !clean[0]) return kind == SNAG_IRC_NOTICE ? 0 :
            queue_line(peer, ":%s 412 %s :No safe text to send", irc->server_name, peer->nick);
    for (size_t offset = 0u, len = strlen(clean); offset < len;) {
        char chunk[SNAG_IRC_TEXT_MAX + 1u];
        size_t take = chat_chunk(clean + offset, len - offset);

        if (!take) return -1;
        memcpy(chunk, clean + offset, take);
        chunk[take] = '\0';
        if (server_publish(irc, kind, peer->nick, peer->user, chunk, peer->op, false) < 0) return -1;
        offset += take;
    }
    return 0;
}

static int
server_topic(struct snag_irc_core *irc, struct irc_conn *peer, const struct irc_message *message)
{
    const char *topic;
    char clean[sizeof(irc->topic)];

    if (!peer->joined || message->param_count < 1u || irc_casecmp(message->params[0], irc->room) != 0)
        return queue_line(peer, ":%s 442 %s %s :You're not on that channel",
                          irc->server_name, peer->nick, irc->room);
    if (message->param_count == 1u) return queue_line(peer, ":%s 332 %s %s :%s", irc->server_name,
                          peer->nick, irc->room, irc->topic);
    if (!peer->op) return queue_line(peer, ":%s 482 %s %s :You're not channel operator",
                          irc->server_name, peer->nick, irc->room);
    topic = message->params[1];
    if (strlen(topic) > IRC_TOPIC_MAX || strchr(topic, '\r') || strchr(topic, '\n') ||
        !snag_utf8_valid((const unsigned char *)topic, strlen(topic), true))
        return queue_line(peer, ":%s 417 %s :Topic is too long", irc->server_name, peer->nick);
    if (sanitize_text(clean, sizeof(clean), topic) < 0)
        return queue_line(peer, ":%s 417 %s :Topic is too long", irc->server_name, peer->nick);
    memcpy(irc->topic, clean, strlen(clean) + 1u);
    return server_publish(irc, SNAG_IRC_TOPIC, peer->nick, peer->user, irc->topic, peer->op, false);
}

static int
server_mode(struct snag_irc_core *irc, struct irc_conn *peer, const struct irc_message *message)
{
    struct irc_conn *target;
    bool add;
    char mode_text[SNAG_CONFIG_IRC_NICK_MAX + 4u];

    if (message->param_count < 1u || irc_casecmp(message->params[0], irc->room) != 0)
        return queue_line(peer, ":%s 403 %s * :No such channel", irc->server_name, peer->nick);
    if (message->param_count == 1u) return queue_line(peer, ":%s 324 %s %s +t", irc->server_name,
                          peer->nick, irc->room);
    if (!peer->op) return queue_line(peer, ":%s 482 %s %s :You're not channel operator",
                          irc->server_name, peer->nick, irc->room);
    if (message->param_count < 3u || (strcmp(message->params[1], "+o") != 0 &&
         strcmp(message->params[1], "-o") != 0))
        return queue_line(peer, ":%s 472 %s :Only +o and -o are supported", irc->server_name, peer->nick);
    target = server_peer_by_nick(irc, message->params[2]);
    if (!target || !target->joined) return queue_line(peer, ":%s 441 %s %s %s :They aren't on that channel",
                          irc->server_name, peer->nick, message->params[2], irc->room);
    add = message->params[1][0] == '+';
    target->op = add;
    (void)snprintf(mode_text, sizeof(mode_text), "%s %s", message->params[1], message->params[2]);
    return server_publish(irc, SNAG_IRC_MODE, peer->nick, peer->user, mode_text, peer->op, false);
}

static int
server_who(struct snag_irc_core *irc, struct irc_conn *peer)
{
    for (size_t i = 0u; i < irc->conn_count; ++i) {
        struct irc_conn *member = &irc->conns[i];

        if (member->used && member->joined && queue_line(peer, ":%s 352 %s %s %s %s %s %s H%s :0 %s",
                       irc->server_name, peer->nick, irc->room, member->user,
                       irc->server_name, irc->server_name, member->nick,
                       member->op ? "@" : "", i == LINK_AGENT ? SNAJPAGENT_NAME :
                       i == LINK_OPERATOR ? "operator" : "IRC user") < 0) return -1;
    }
    return queue_line(peer, ":%s 315 %s %s :End of WHO list", irc->server_name, peer->nick, irc->room);
}

static int
trace_message(struct snag_irc_core *irc, const char *endpoint, const struct irc_message *message)
{
    char trace[96u];

    if (irc->trace_fn) {
        int n = snprintf(trace, sizeof(trace), "%s params=%zu", message->command, message->param_count);
        if (n < 0 || (size_t)n >= sizeof(trace) || irc->trace_fn(irc->event_opaque, 5u, '<', endpoint,
                          trace, (size_t)n) < 0) {
            irc->callback_failed = true;
            return -1;
        }
    }
    return 0;
}

static int
server_cap_request(struct snag_irc_core *irc, struct irc_conn *peer, const char *caps)
{
    const char *supported = "batch server-time draft/chathistory echo-message "
        SNAJPAGENT_NAME "/agent " SNAJPAGENT_NAME "/catchup";
    char copy[SNAG_IRC_LINE_MAX];
    char *save = NULL;
    if (!snag_strcpy(copy, sizeof(copy), caps)) return -1;
    /* A failed REQ changes nothing, including capabilities from earlier REQs. */
    for (char *cap = strtok_r(copy, " ", &save); cap; cap = strtok_r(NULL, " ", &save)) {
        if (!snag_string_in(cap + (*cap == '-'), supported)) return queue_line(peer,
            ":%s CAP * NAK :%s", irc->server_name, caps);
    }
    (void)snag_strcpy(copy, sizeof(copy), caps);
    for (char *cap = strtok_r(copy, " ", &save); cap; cap = strtok_r(NULL, " ", &save)) {
        bool enabled = *cap != '-';
        if (!enabled) ++cap;
        if (!strcmp(cap, "batch")) peer->cap_batch = enabled;
        if (!strcmp(cap, "server-time")) peer->cap_server_time = enabled;
        if (!strcmp(cap, "echo-message")) peer->cap_echo = enabled;
        if (!strcmp(cap, SNAJPAGENT_NAME "/agent")) peer->agent_role = enabled;
        if (!strcmp(cap, SNAJPAGENT_NAME "/catchup")) peer->cap_catchup = enabled;
    }
    return queue_line(peer, ":%s CAP * ACK :%s", irc->server_name, caps);
}

static int
server_dispatch(struct snag_irc_core *irc, struct irc_conn *peer, char *line)
{
    struct irc_message message;

    if (parse_message(line, &message) < 0) return 0;
    if (trace_message(irc, irc->listen, &message) < 0) return -1;
    if (strcmp(message.command, "CAP") == 0) {
        const char *sub = message.param_count ? message.params[0] : "";
        const char *caps = message.param_count > 1u ? message.params[1] : "";
        if (strcmp(sub, "LS") == 0) {
            peer->cap_active = true;
            return queue_line(peer,
                ":%s CAP * LS :batch server-time draft/chathistory echo-message "
                SNAJPAGENT_NAME "/agent " SNAJPAGENT_NAME "/catchup", irc->server_name);
        }
        if (strcmp(sub, "REQ") == 0) {
            peer->cap_active = true;
            return server_cap_request(irc, peer, caps);
        }
        if (strcmp(sub, "END") == 0) {
            peer->cap_end = true;
            return server_welcome(irc, peer);
        }
        return 0;
    }
    if (strcmp(message.command, "SAJCATCHUP") == 0 && peer->cap_catchup &&
        !peer->joined && message.param_count == 3u && !strcmp(message.params[0], irc->room)) {
        char *end;
        errno = 0;
        unsigned long long seq = strtoull(message.params[2], &end, 10);
        if (errno || *end || seq > INT64_MAX || !*message.params[2] ||
            (!snag_hex_is_lower(message.params[1], SNAG_ID_HEX_LEN) && strcmp(message.params[1], "-")))
            return 1;
        if (strcmp(message.params[1], "-"))
            memcpy(peer->since_stream, message.params[1], sizeof(peer->since_stream));
        peer->since_sequence = seq;
        return 0;
    }
    if (strcmp(message.command, "NICK") == 0) {
        const char *next = message.param_count ? message.params[0] : "";
        char old[sizeof(peer->nick)];
        if (!nick_valid(next)) return queue_line(peer, ":%s 432 * %s :Erroneous nickname",
                              irc->server_name, next);
        struct irc_conn *collision = server_peer_by_nick(irc, next);
        if (collision && collision != peer)
            return queue_line(peer, ":%s 433 * %s :Nickname is already in use", irc->server_name, next);
        if (strcmp(peer->nick, next) == 0) return 0;
        memcpy(old, peer->nick, sizeof(old));
        (void)snag_strcpy(peer->nick, sizeof(peer->nick), next);
        if (peer->registered)
            for (size_t role = 0u; role < 2u; ++role)
                if (query_lifecycle(irc, &irc->conns[role], SNAG_IRC_NICK,
                    old, peer->nick, 0u) < 0) return -1;
        if (old[0] && peer->joined) {
            if (server_publish(irc, SNAG_IRC_NICK, old, peer->user, peer->nick, peer->op, false) < 0)
                return -1;
        } else if (peer->registered && queue_line(peer, ":%s!%s@%s NICK :%s", old,
                              peer->user, irc->server_name, peer->nick) < 0) {
            return -1;
        }
        return server_welcome(irc, peer);
    }
    if (strcmp(message.command, "USER") == 0) {
        if (message.param_count < 1u || !nick_valid(message.params[0]))
            return queue_line(peer, ":%s 461 * USER :Not enough parameters", irc->server_name);
        (void)snag_strcpy(peer->user, sizeof(peer->user), message.params[0]);
        if (message.param_count >= 4u && strcmp(message.params[3], SNAJPAGENT_NAME " agent") == 0)
            peer->agent_role = true;
        return server_welcome(irc, peer);
    }
    if (strcmp(message.command, "PING") == 0) return queue_line(peer, ":%s PONG %s :%s", irc->server_name,
                          irc->server_name, message.param_count ? message.params[0] : irc->server_name);
    if (strcmp(message.command, "PONG") == 0) return 0;
    if (strcmp(message.command, "QUIT") == 0) return 1;
    if (strcmp(message.command, "JOIN") == 0)
        return message.param_count ? server_join(irc, peer, message.params[0]) :
            queue_line(peer, ":%s 461 %s JOIN :Not enough parameters", irc->server_name, peer->nick);
    if (strcmp(message.command, "PART") == 0) {
        if (peer->joined && message.param_count && irc_casecmp(message.params[0], irc->room) == 0) {
            const char *reason = message.param_count > 1u ? message.params[1] : "";
            char clean[SNAG_IRC_TEXT_MAX + 1u];
            if (sanitize_text(clean, sizeof(clean), reason) < 0) clean[0] = '\0';
            if (server_publish(irc, SNAG_IRC_PART, peer->nick, peer->user, clean, peer->op, false) < 0)
                return -1;
            peer->joined = false;
        }
        return 0;
    }
    if (snag_string_in(message.command, "PRIVMSG NOTICE")) {
        if (message.param_count < 2u) return 0;
        return server_chat(irc, peer, strcmp(message.command, "NOTICE") == 0 ? SNAG_IRC_NOTICE :
                                                     SNAG_IRC_MESSAGE, message.params[0], message.params[1]);
    }
    if (strcmp(message.command, "TOPIC") == 0) return server_topic(irc, peer, &message);
    if (strcmp(message.command, "MODE") == 0) return server_mode(irc, peer, &message);
    if ((snag_string_in(message.command, "NAMES WHO")) && !peer->registered)
        return queue_line(peer, ":%s 451 * :You have not registered", irc->server_name);
    if (strcmp(message.command, "NAMES") == 0) return server_send_names(irc, peer);
    if (strcmp(message.command, "WHO") == 0) return server_who(irc, peer);
    if (peer->registered) return queue_line(peer, ":%s 421 %s %s :Unknown command",
                          irc->server_name, peer->nick, message.command);
    return 0;
}

static void
server_drop_peer(struct snag_irc_core *irc, struct irc_conn *peer, const char *reason)
{
    char nick[sizeof(peer->nick)];
    char user[sizeof(peer->user)];
    bool announced = peer->used && peer->joined && peer->nick[0];
    bool op = peer->op;

    (void)snprintf(nick, sizeof(nick), "%s", peer->nick);
    (void)snprintf(user, sizeof(user), "%s", peer->user);
    if (private_discard(peer, true) < 0) irc->callback_failed = true;
    if (peer->registered)
        for (size_t role = 0u; role < 2u; ++role)
            if (query_lifecycle(irc, &irc->conns[role], SNAG_IRC_QUIT,
                nick, reason, 0u) < 0) irc->callback_failed = true;
    conn_release(peer);
    if (announced) (void)server_publish(irc, SNAG_IRC_QUIT, nick, user, reason, op, false);
}

static int
client_handshake(struct snag_irc_core *irc, struct irc_conn *link)
{
    const char *role_text = link->role == LINK_AGENT ? "agent" : "operator";

    link->connecting = false;
    link->registered = false;
    link->joined = false;
    link->historical = false;
    link->cap_catchup = false;
    link->cap_end = false;
    link->cap_batch = false;
    link->cap_server_time = false;
    link->cap_echo = false;
    link->cap_labeled = false;
    receipt_batches_free(link);
    link->cap_offered = 0u;
    link->line_limit = 512u;
    link->casemapping = SNAG_IRC_RFC1459;
    memcpy(link->chantypes, "#&", 3u);
    link->batch[0] = '\0';
    link->batch_room[0] = '\0';
    channels_reset(link);
    link->room[0] = '\0';
    if (irc->room_explicit) (void)snag_strcpy(link->room, sizeof(link->room), irc->room);
    if (queue_line(link, "CAP LS 302") < 0 ||
        queue_line(link, "NICK %s", link->nick) < 0 ||
        queue_line(link, "USER %s 0 * :" SNAJPAGENT_NAME " %s", link->nick, role_text) < 0)
        return -1;
    return 0;
}

static int
start_link(struct snag_irc_core *irc, struct irc_conn *link)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *it;
    char host[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char port[6u];
    int gai;

    if (split_endpoint(link->endpoint, host, sizeof(host), port, sizeof(port)) < 0) return -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    gai = snag_socket_addresses(host, port, &hints, &addresses);
    if (gai != 0) {
        errno = EHOSTUNREACH;
        return 1;
    }
    for (it = addresses; it; it = it->ai_next) {
        int rc;
        link->fd = snag_socket_open(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (link->fd == SNAG_SOCKET_INVALID) continue;
        snag_socket_nodelay(link->fd);
        rc = snag_socket_connect(link->fd, it->ai_addr, it->ai_addrlen);
        if (rc == 0) {
            snag_socket_addresses_free(addresses);
            return client_handshake(irc, link);
        }
        if (errno == EINPROGRESS) {
            link->connecting = true;
            snag_socket_addresses_free(addresses);
            return 0;
        }
        (void)snag_socket_close(link->fd);
        link->fd = SNAG_SOCKET_INVALID;
    }
    snag_socket_addresses_free(addresses);
    return 1;
}

static bool
link_emit_enabled(const struct irc_conn *link)
{
    return link->role == LINK_OPERATOR;
}

static int
link_retry_nick(struct irc_conn *link)
{
    if (link->nick_suffix == SIZE_MAX) return snag_errno(EOVERFLOW);
    if (numbered_nick(link->nick, link->preferred_nick, ++link->nick_suffix, link->nick_implicit) < 0)
        return -1;
    return queue_line(link, "NICK %s", link->nick);
}

static int
link_queue_pending(struct irc_conn *link, enum snag_irc_event_kind kind, const char *text)
{
    unsigned char marker = kind == SNAG_IRC_NOTICE ? 'N' : 'M';
    size_t before = link->pending.len;

    if (strlen(text) + 2u > IRC_PENDING_MAX - before - link->private_bytes)
        return snag_errno(EOVERFLOW);

    if (snag_buf_putc(&link->pending, marker) < 0 ||
        snag_buf_append(&link->pending, text, strlen(text)) < 0 ||
        snag_buf_append(&link->pending, "\n", 1u) < 0) {
        link->pending.len = before;
        return -1;
    }
    return 0;
}

static int
link_flush_pending(struct irc_conn *link)
{
    size_t offset = 0u;

    if (link->pending_inflight || link->output.len) return 0;
    while (offset < link->pending.len) {
        unsigned char *lf = memchr(link->pending.data + offset, '\n', link->pending.len - offset);
        size_t len;
        const char *command;
        char text[SNAG_IRC_TEXT_MAX + 1u];

        if (!lf) return snag_errno(EPROTO);
        len = (size_t)(lf - (link->pending.data + offset));
        if (len < 2u || len > SNAG_IRC_TEXT_MAX + 1u || (link->pending.data[offset] != 'M' &&
             link->pending.data[offset] != 'N')) return snag_errno(EPROTO);
        command = link->pending.data[offset] == 'N' ? "NOTICE" : "PRIVMSG";
        memcpy(text, link->pending.data + offset + 1u, len - 1u);
        text[len - 1u] = '\0';
        if (queue_line(link, "%s %s :%s", command, link->room, text) < 0) {
            if (errno == EOVERFLOW && offset) break;
            return -1;
        }
        offset += len + 1u;
    }
    link->pending_inflight = offset;
    return 0;
}

static int
link_emit(struct snag_irc_core *irc, struct irc_conn *link,
          enum snag_irc_event_kind kind, const char *room, const char *nick,
          const char *text, bool op, uint64_t timestamp_ms)
{
    struct snag_irc_event event;

    if (kind == SNAG_IRC_TOPIC) {
        struct irc_channel *channel = channel_find(link, room);
        if (channel && sanitize_text(channel->topic, sizeof(channel->topic), text) < 0) return 1;
    }
    if (!link_emit_enabled(link)) return 0;
    event_init(irc, &event, kind, link->endpoint, room, nick, text, op, link->historical, false);
    if (timestamp_ms) event.timestamp_ms = timestamp_ms;
    if (link->event_stream[0]) {
        memcpy(event.stream, link->event_stream, sizeof(event.stream));
        event.sequence = link->event_sequence;
        event.op = link->event_op;
        for (size_t i = 0u; i < irc->conn_count; ++i)
            if ((kind == SNAG_IRC_MESSAGE || kind == SNAG_IRC_NOTICE || kind == SNAG_IRC_TOPIC) &&
                link_name_equal(link, event.nick, irc->conns[i].accepted_nick))
                event.local = !event.historical;
        if (snag_irc_core_received(irc, &event)) return 0;
        if (event.historical) ++link->replayed;
    } else if (event.historical) ++link->replayed;
    return emit_event(irc, &event, true);
}

static bool
message_tag(const char *tags, const char *key, char *out, size_t size)
{
    size_t key_len = strlen(key);
    for (const char *p = tags; p && *p;) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > key_len && !memcmp(p, key, key_len) && p[key_len] == '=') {
            size_t n = len - key_len - 1u;
            if (n >= size) return false;
            memcpy(out, p + key_len + 1u, n); out[n] = '\0'; return true;
        }
        p = end ? end + 1u : NULL;
    }
    return false;
}

static int
private_capabilities_changed(struct irc_conn *link)
{
    struct irc_private_send **previous = &link->private_sends;
    while (*previous) {
        struct irc_private_send *send = *previous;
        bool supported = send->labeled ? link->cap_batch && link->cap_labeled :
            !send->await_receipt || (link->cap_catchup && link->cap_echo);
        if (supported) {
            previous = &send->next;
            continue;
        }
        if (send->offset && send->offset < send->length) link->private_broken = true;
        if (private_state(link, send, send->offset ? SNAG_IRC_UNCERTAIN : SNAG_IRC_FAILED) < 0)
            return -1;
        private_remove(link, previous);
    }
    return 0;
}

static int
client_cap(struct irc_conn *link, struct irc_message *message)
{
    static const char *const wanted[] = {"batch", "server-time", "draft/chathistory",
        "message-tags", SNAJPAGENT_NAME "/catchup", SNAJPAGENT_NAME "/agent",
        "echo-message", "labeled-response"};
    const char *sub = message->params[1];
    char *caps = message->params[message->param_count - 1u];
    char *save = NULL;
    if (!strcmp(sub, "LS") && !link->cap_end) {
        for (char *cap = strtok_r(caps, " ", &save); cap; cap = strtok_r(NULL, " ", &save)) {
            char *value = strchr(cap, '=');
            if (value) *value = '\0';
            for (size_t i = 0u; i < sizeof(wanted) / sizeof(wanted[0]); ++i)
                if (!strcmp(cap, wanted[i]) &&
                    (link->role == LINK_AGENT || strcmp(cap, SNAJPAGENT_NAME "/agent")))
                    link->cap_offered |= 1u << i;
        }
        if (message->param_count > 3u && !strcmp(message->params[2], "*")) return 0;
        char requested[SNAG_IRC_LINE_MAX] = {0};
        size_t used = 0u;
        for (size_t i = 0u; i < sizeof(wanted) / sizeof(wanted[0]); ++i) {
            if (!(link->cap_offered & (1u << i))) continue;
            if (!strcmp(wanted[i], "labeled-response") && !(link->cap_offered & 1u)) continue;
            int n = snprintf(requested + used, sizeof(requested) - used, "%s%s",
                used ? " " : "", wanted[i]);
            if (n < 0 || (size_t)n >= sizeof(requested) - used) return snag_errno(EOVERFLOW);
            used += (size_t)n;
        }
        if (used) return queue_line(link, "CAP REQ :%s", requested);
    } else if (!strcmp(sub, "ACK") || !strcmp(sub, "DEL")) {
        for (char *cap = strtok_r(caps, " ", &save); cap; cap = strtok_r(NULL, " ", &save)) {
            bool enabled = *cap != '-' && strcmp(sub, "DEL");
            if (*cap == '-') ++cap;
            if (!strcmp(cap, "batch")) link->cap_batch = enabled;
            if (!strcmp(cap, "server-time")) link->cap_server_time = enabled;
            if (!strcmp(cap, "echo-message")) link->cap_echo = enabled;
            if (!strcmp(cap, "labeled-response")) link->cap_labeled = enabled;
            if (!strcmp(cap, SNAJPAGENT_NAME "/catchup")) link->cap_catchup = enabled;
        }
        if (private_capabilities_changed(link) < 0) return -1;
        if (!strcmp(sub, "DEL")) return 0;
    } else if (strcmp(sub, "NAK")) {
        return 0;
    }
    if (link->cap_end) return 0;
    link->cap_end = true;
    return queue_line(link, "CAP END");
}

static bool
decoded_tag(const char *tags, const char *key, char *out, size_t size)
{
    char encoded[SNAG_IRC_LINE_MAX + 1u];
    if (!size || !message_tag(tags, key, encoded, sizeof(encoded))) return false;
    size_t used = 0u;
    for (size_t i = 0u; encoded[i]; ++i) {
        unsigned char c = (unsigned char)encoded[i];
        if (c == '\\') {
            c = (unsigned char)encoded[++i];
            if (!c) break;
            if (c == ':') c = ';';
            else if (c == 's') c = ' ';
            else if (c == 'r' || c == 'n') return false;
        }
        if (c < 0x20u || c == 0x7fu || used >= size - 1u) return false;
        out[used++] = (char)c;
    }
    out[used] = '\0';
    return snag_utf8_valid((const unsigned char *)out, used, true);
}

static bool
private_source(const char *tags, char source[SNAG_IRC_LINE_MAX + 1u])
{
    return decoded_tag(tags, "msgid", source, SNAG_IRC_LINE_MAX + 1u);
}

static struct irc_private_send **
receipt_send(struct irc_conn *link, const char *label)
{
    for (struct irc_private_send **at = &link->private_sends; *at; at = &(*at)->next)
        if ((*at)->labeled && !strcmp((*at)->event.route.send, label)) return at;
    return NULL;
}

static struct irc_receipt_batch **
receipt_batch(struct irc_conn *link, const char *id)
{
    for (struct irc_receipt_batch **at = &link->receipt_batches; *at; at = &(*at)->next)
        if (!strcmp((*at)->id, id)) return at;
    return NULL;
}

static int
receipt_finish(struct irc_conn *link, struct irc_private_send **previous)
{
    if (!previous || (*previous)->offset != (*previous)->length) return 0;
    struct irc_private_send *send = *previous;
    int rc = private_state(link, send, send->receipt ? send->receipt : SNAG_IRC_UNCERTAIN);
    private_remove(link, previous);
    return rc;
}

static int
receipt_message(struct irc_conn *link, struct irc_private_send *send,
                const struct irc_message *message)
{
    const char *command = message->command;
    bool numeric = strlen(command) == 3u && command[0] >= '4' && command[0] <= '5' &&
        command[1] >= '0' && command[1] <= '9' && command[2] >= '0' && command[2] <= '9';
    if (numeric || !strcmp(command, "FAIL")) {
        send->receipt = SNAG_IRC_FAILED;
        return 0;
    }
    bool ack = !strcmp(command, "ACK") && !message->param_count;
    char nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    const char *sender = prefix_nick(message->prefix, nick);
    bool echo = message->param_count == 2u && sender &&
        !strcmp(command, send->event.kind == SNAG_IRC_MESSAGE ? "PRIVMSG" : "NOTICE") &&
        (link_name_equal(link, sender, send->event.nick) ||
         link_name_equal(link, sender, link->accepted_nick)) &&
        link_name_equal(link, message->params[0], send->event.route.target);
    if (echo) {
        /* Multiple echoes for one frame do not identify a unique final body. */
        if (send->receipt_echo) {
            if (send->receipt != SNAG_IRC_FAILED) send->receipt = SNAG_IRC_UNCERTAIN;
            return 0;
        }
        const char *text = message->params[1];
        size_t length = strlen(text);
        bool action = send->event.kind == SNAG_IRC_MESSAGE && length >= 9u &&
            !memcmp(text, "\001ACTION ", 8u) && text[length - 1u] == '\001';
        char raw[SNAG_IRC_TEXT_MAX + 1u];
        if (action) {
            text += 8u;
            length -= 9u;
        }
        if (length >= sizeof(raw) ||
            !snag_utf8_valid((const unsigned char *)text, length, true)) return 1;
        memcpy(raw, text, length);
        raw[length] = '\0';
        char clean[sizeof(raw)];
        if (sanitize_text(clean, sizeof(clean), raw) < 0) return -1;
        send->event.route.revised = strcmp(clean, send->event.text) != 0 ||
            action != send->event.route.action;
        memcpy(send->event.text, clean, strlen(clean) + 1u);
        send->event.route.action = action;
        char source[SNAG_IRC_LINE_MAX + 1u];
        if (private_source(message->tags, source))
            memcpy(send->event.route.source, source, strlen(source) + 1u);
        send->receipt_echo = true;
    }
    if ((echo || ack) && !send->receipt) send->receipt = SNAG_IRC_ACKNOWLEDGED;
    return 0;
}

static int
labeled_receipt(struct irc_conn *link, const struct irc_message *message, bool *handled)
{
    char label[SNAG_ID_HEX_LEN + 1u] = {0};
    char batch_id[SNAG_IRC_LINE_MAX + 1u];
    bool labeled = decoded_tag(message->tags, "label", label, sizeof(label)) &&
        snag_hex_is_lower(label, SNAG_ID_HEX_LEN);
    struct irc_receipt_batch **parent = decoded_tag(message->tags, "batch",
        batch_id, sizeof(batch_id)) ? receipt_batch(link, batch_id) : NULL;
    struct irc_receipt_batch *batch = parent ? *parent : NULL;
    if (!labeled && batch) memcpy(label, batch->label, sizeof(label));
    if (!strcmp(message->command, "BATCH") && message->param_count) {
        const char *id = message->params[0];
        struct irc_receipt_batch **previous = receipt_batch(link, id + (*id != '\0'));
        if (*id == '-' && previous) {
            *handled = true;
            struct irc_receipt_batch *closed = *previous;
            for (struct irc_receipt_batch *at = link->receipt_batches; at; at = at->next)
                if (at->parent == closed) return 1;
            if (closed->parent != batch) return 1;
            int rc = closed->parent ? 0 : receipt_finish(link, receipt_send(link, closed->label));
            *previous = closed->next;
            link->private_bytes -= sizeof(*closed) + strlen(closed->id) + 1u;
            free(closed);
            return rc;
        }
        if (*id != '+' || (!labeled && !batch)) return 0;
        *handled = true;
        if (previous || !id[1] || message->param_count < 2u ||
            (labeled && batch && strcmp(label, batch->label))) return 1;
        if (!batch) {
            for (struct irc_receipt_batch *at = link->receipt_batches; at; at = at->next)
                if (!strcmp(at->label, label)) return 1;
        }
        for (const char *p = id + 1u; *p; ++p)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                (*p >= '0' && *p <= '9') || *p == '-')) return 1;
        /* Retain cancelled labels until BATCH ends: late errors must not
         * change membership in a conversation that has since rejoined. */
        size_t bytes = sizeof(struct irc_receipt_batch) + strlen(id);
        if (bytes > IRC_PENDING_MAX - link->pending.len - link->private_bytes)
            return snag_errno(EOVERFLOW);
        struct irc_receipt_batch *opened = calloc(1u, bytes);
        if (!opened) return -1;
        memcpy(opened->label, label, sizeof(label));
        memcpy(opened->id, id + 1u, strlen(id));
        opened->parent = batch;
        opened->next = link->receipt_batches;
        link->receipt_batches = opened;
        link->private_bytes += bytes;
        return 0;
    }
    if (!labeled && !batch) return 0;
    *handled = true;
    if (link->historical || (labeled && batch && strcmp(label, batch->label))) return 0;
    struct irc_private_send **previous = receipt_send(link, label);
    if (!previous || (*previous)->offset != (*previous)->length) return 0;
    int rc = receipt_message(link, *previous, message);
    return rc || batch ? rc : receipt_finish(link, previous);
}

static int
private_receipt(struct irc_conn *link, const struct irc_message *message, bool *handled)
{
    *handled = false;
    int labeled = labeled_receipt(link, message, handled);
    if (labeled || *handled) return labeled;
    if (!link->cap_catchup || !link->cap_echo || link->historical) return 0;
    bool failed = snag_string_in(message->command, "401 404 407 411 412 531");
    char nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    const char *sender = prefix_nick(message->prefix, nick);
    bool echo = !strcmp(message->command, "PRIVMSG") && sender &&
        link_name_equal(link, sender, link->accepted_nick);
    if ((!failed && !echo) || message->param_count < (failed ? 3u : 2u)) return 0;
    const char *target = message->params[failed ? 1u : 0u];
    struct irc_private_send **previous = &link->private_sends;
    while (*previous) {
        struct irc_private_send *send = *previous;
        if (send->labeled || !send->await_receipt || send->offset != send->length ||
            !link_name_equal(link, send->event.route.target, target)) {
            previous = &send->next;
            continue;
        }
        /* The built-in server handles this link's PRIVMSGs in wire order.
         * Text equality cannot identify repeated identical messages. External
         * unlabeled echoes remain unconfirmed writes. */
        int rc = private_state(link, send, failed ? SNAG_IRC_FAILED : SNAG_IRC_ACKNOWLEDGED);
        private_remove(link, previous);
        *handled = true;
        return rc;
    }
    return 0;
}

static int
client_dispatch(struct snag_irc_core *irc, struct irc_conn *link, char *line)
{
    struct irc_message message;
    char nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    const char *sender;
    uint64_t timestamp_ms;

    if (parse_message(line, &message) < 0) return 0;
    bool direct = message.param_count >= 2u &&
        snag_string_in(message.command, "PRIVMSG NOTICE") &&
        link_name_equal(link, message.params[0], link->accepted_nick);
    link->event_stream[0] = '\0';
    link->event_sequence = 0u;
    link->historical = false;
    char identity[80u], batch[64u], op[4u];
    if (!direct && message_tag(message.tags, "saj-id", identity, sizeof(identity))) {
        char *sep = strchr(identity, ':');
        char *end;
        if (!sep) return 1;
        *sep++ = '\0'; errno = 0;
        unsigned long long seq = strtoull(sep, &end, 10);
        if (!snag_hex_is_lower(identity, SNAG_ID_HEX_LEN) || !seq || seq > INT64_MAX || errno || *end)
            return 1;
        memcpy(link->event_stream, identity, sizeof(link->event_stream));
        link->event_sequence = seq;
        link->event_op = message_tag(message.tags, "saj-op", op, sizeof(op)) && !strcmp(op, "1");
    }
    if (message_tag(message.tags, "batch", batch, sizeof(batch)) &&
        link->batch[0] && !strcmp(batch, link->batch)) link->historical = true;
    timestamp_ms = server_time_tag(message.tags);
    if (trace_message(irc, link->endpoint, &message) < 0) return -1;
    bool receipt = false;
    int receipt_rc = private_receipt(link, &message, &receipt);
    if (receipt_rc) return receipt_rc;
    if (receipt) return 0;
    if (!strcmp(message.command, "CAP") && message.param_count >= 3u)
        return client_cap(link, &message);
    if (strcmp(message.command, "PING") == 0) return queue_line(link, "PONG :%s",
                          message.param_count ? message.params[0] : SNAJPAGENT_NAME);
    if (strcmp(message.command, "001") == 0) {
        if (link->registered || !message.param_count || !nick_valid(message.params[0])) return 0;
        (void)snag_strcpy(link->nick, sizeof(link->nick), message.params[0]);
        (void)snag_strcpy(link->accepted_nick, sizeof(link->accepted_nick), link->nick);
        link->registered = true;
        link->outage_reported = false;
        if (irc->connection[0]) return connection_event(irc, link, "");
        return link_emit(irc, link, SNAG_IRC_CONNECTED, "", link->nick, "", false, timestamp_ms);
    }
    if (strcmp(message.command, "005") == 0) {
        for (size_t i = 1u; i < message.param_count; ++i) {
            if (!strncmp(message.params[i], "CASEMAPPING=", 12u) ||
                !strcmp(message.params[i], "-CASEMAPPING")) {
                const char *mapping = message.params[i][0] == '-' ?
                    "rfc1459" : message.params[i] + 12u;
                enum snag_irc_casemapping next = !strcmp(mapping, "ascii") ? SNAG_IRC_ASCII :
                    !strcmp(mapping, "rfc1459") ? SNAG_IRC_RFC1459 :
                    !strcmp(mapping, "rfc1459-strict") ? SNAG_IRC_RFC1459_STRICT :
                    SNAG_IRC_CASE_UNKNOWN;
                if (next != link->casemapping && irc->connection[0] &&
                    query_epoch_end(irc, "server casemapping changed") < 0) return -1;
                link->casemapping = next;
            }
            if (!strncmp(message.params[i], "LINELEN=", 8u)) {
                char *end;
                errno = 0;
                unsigned long limit = strtoul(message.params[i] + 8u, &end, 10);
                if (!errno && !*end && limit >= 512u)
                    link->line_limit = limit > SNAG_IRC_LINE_MAX ? SNAG_IRC_LINE_MAX : limit;
            }
            if (!strcmp(message.params[i], "-LINELEN")) link->line_limit = 512u;
            if (!strncmp(message.params[i], "CHANTYPES=", 10u)) {
                const char *types = message.params[i] + 10u;
                bool valid = strlen(types) < sizeof(link->chantypes);
                for (const unsigned char *at = (const unsigned char *)types; *at; ++at)
                    if (*at < 0x21u || *at >= 0x7fu || *at == ',' || *at == ':') valid = false;
                if (valid) (void)snag_strcpy(link->chantypes, sizeof(link->chantypes), types);
            }
            if (!strcmp(message.params[i], "-CHANTYPES")) memcpy(link->chantypes, "#&", 3u);
            if (strncmp(message.params[i], "SAJROOM=", 8u) == 0 && room_valid(message.params[i] + 8u)) {
                struct irc_channel *previous = channel_find(link,
                    link->room[0] ? link->room : link->previous_room);
                (void)normalize_room(link->room, sizeof(link->room), message.params[i] + 8u);
                if (previous && !link_name_equal(link, previous->room, link->room)) {
                    previous->wanted = false;
                    if (channel_state(link, previous, "default room changed") < 0) return -1;
                }
                channel_default_status(link);
                if (link->previous_room[0] && strcmp(link->previous_room, link->room)) {
                    ++irc->route_revision;
                    snag_buf_reset(&link->pending);
                    link->pending_inflight = 0u;
                    if (link_emit(irc, link, SNAG_IRC_NOTICE, link->room, SNAJPAGENT_NAME,
                            "room changed; old queued messages discarded", false, 0u) < 0) return -1;
                }
                memcpy(link->previous_room, link->room, sizeof(link->previous_room));
            }
        }
        return 0;
    }
    if (snag_string_in(message.command, "376 422")) {
        if (link->room[0] && !channel_find(link, link->room) &&
            channel_name_valid(link, link->room)) {
            struct irc_channel *channel = channel_open(link, link->room);
            if (!channel) return -1;
            channel->wanted = true;
            if (channel_state(link, channel, "join requested") < 0) return -1;
        }
        for (struct irc_channel *channel = link->channels; channel; channel = channel->next) {
            if (!channel->wanted || !channel_name_valid(link, channel->room)) continue;
            struct irc_cursor *cursor = find_cursor(irc, link->endpoint, channel->room, false);
            if (link->cap_catchup && queue_line(link, "SAJCATCHUP %s %s %llu", channel->room,
                cursor && cursor->stream[0] ? cursor->stream : "-",
                (unsigned long long)(cursor ? cursor->sequence : 0u)) < 0) return -1;
            if (queue_line(link, "JOIN %s", channel->room) < 0) return -1;
        }
        return 0;
    }
    if (strcmp(message.command, "BATCH") == 0 && message.param_count) {
        const char *id = message.params[0];
        bool query = irc->connection[0] && message.param_count >= 3u &&
            nick_valid(message.params[2]);
        if (*id == '+' && message.param_count >= 3u &&
            !strcmp(message.params[1], "chathistory") &&
            (channel_find(link, message.params[2]) || query)) {
            if (link->batch[0] || !snag_strcpy(link->batch, sizeof(link->batch), id + 1u)) return 1;
            link->batch_query = query;
            if (!query) (void)snag_strcpy(link->batch_room,
                sizeof(link->batch_room), message.params[2]);
            link->replayed = 0u;
            link->history_gap = message.param_count >= 5u && !strcmp(message.params[4], "1");
        } else if (*id == '-' && link->batch[0] && !strcmp(id + 1u, link->batch)) {
            link->batch[0] = '\0';
            link->historical = false;
            if (link->batch_query) {
                link->batch_query = false;
                return 0;
            }
            if (link_name_equal(link, link->batch_room, link->room) &&
                link_flush_pending(link) < 0) return -1;
            return link_emit(irc, link, SNAG_IRC_HISTORY_READY, link->batch_room, "",
                link->history_gap ? "history gap: earlier events are no longer retained" :
                link->replayed ? "replayed" : "", false, timestamp_ms);
        }
        return 0;
    }
    if (strcmp(message.command, "353") == 0 && message.param_count >= 4u) {
        struct irc_channel *channel = channel_find(link, message.params[2]);
        if (!channel || (!channel->joined && !(link->cap_catchup && channel->wanted))) return 0;
        if (!channel->names_active) {
            channel->member_count = 0u;
            channel->names_active = true;
        }
        char *save = NULL;
        for (char *name = strtok_r(message.params[3], " ", &save); name;
            name = strtok_r(NULL, " ", &save)) {
            bool op = false;
            while (*name && strchr("~&@%+", *name)) {
                if (strchr("~&@", *name)) op = true;
                ++name;
            }
            if (nick_valid(name) && !member_add(link, channel, name, op)) return -1;
        }
        return 0;
    }
    if (strcmp(message.command, "366") == 0 && message.param_count >= 2u) {
        struct irc_channel *channel = channel_find(link, message.params[1]);
        if (!channel || (!channel->joined && !(link->cap_catchup && channel->wanted))) return 0;
        struct irc_member *self = member_find(link, channel, link->accepted_nick);
        channel->op = self && self->op;
        /* Native catch-up defers the self JOIN into history; its initial
         * NAMES snapshot confirms the requested room's current membership. */
        bool confirmed = link->cap_catchup && channel->wanted && self && !channel->joined;
        if (confirmed) channel->joined = true;
        channel->names_active = false;
        channel_default_status(link);
        if (confirmed && channel_state(link, channel, "joined") < 0) return -1;
        return 0;
    }
    if (snag_string_in(message.command, "331 332") && message.param_count >= 2u &&
        channel_find(link, message.params[1])) {
        if (!strcmp(message.command, "332") && message.param_count < 3u) return 0;
        return link_emit(irc, link, SNAG_IRC_TOPIC, message.params[1], "",
            !strcmp(message.command, "332") ? message.params[2] : "", false, timestamp_ms);
    }
    sender = prefix_nick(message.prefix, nick);
    if (direct && sender && link->registered) {
        /* echo-message supplies the send receipt separately from the delivered
         * copy of a message addressed to our own nick. */
        if (!link->historical && link->cap_echo &&
            link_name_equal(link, sender, link->accepted_nick)) return 0;
        enum snag_irc_event_kind kind = !strcmp(message.command, "PRIVMSG") ?
            SNAG_IRC_MESSAGE : SNAG_IRC_NOTICE;
        char *text = message.params[1];
        size_t len = strlen(text);
        bool action = kind == SNAG_IRC_MESSAGE && len >= 9u &&
            !memcmp(text, "\001ACTION ", 8u) && text[len - 1u] == '\001';
        if (action) {
            text[len - 1u] = '\0';
            text += 8u;
        }
        char source[SNAG_IRC_LINE_MAX + 1u];
        return query_receive(irc, link, kind, sender, message.params[0], text,
            link->historical, timestamp_ms,
            private_source(message.tags, source) ? source : NULL, action);
    }
    if (link->historical && link->batch_query) return 0;
    if (link->historical && link->event_stream[0] && sender) {
        char kind[32u];
        if (!message_tag(message.tags, "saj-kind", kind, sizeof(kind))) return 1;
        for (enum snag_irc_event_kind k = SNAG_IRC_JOIN; k <= SNAG_IRC_MODE; ++k) {
            if (strcmp(kind, snag_irc_kind_name(k))) continue;
            const char *text = "";
            char mode[SNAG_CONFIG_IRC_NICK_MAX + 4u];
            if (k == SNAG_IRC_MODE && message.param_count >= 3u) {
                (void)snprintf(mode, sizeof(mode), "%s %s", message.params[1], message.params[2]); text = mode;
            } else if (k == SNAG_IRC_NICK || k == SNAG_IRC_QUIT)
                text = message.param_count ? message.params[0] : "";
            else if (k != SNAG_IRC_JOIN) text = message.param_count >= 2u ? message.params[1] : "";
            return link_emit(irc, link, k, link->batch_room, sender, text,
                link->event_op, timestamp_ms);
        }
        return 1;
    }
    if (strcmp(message.command, "JOIN") == 0 && sender && message.param_count) {
        const char *room = message.params[0];
        bool self = link_name_equal(link, sender, link->accepted_nick);
        struct irc_channel *channel = channel_find(link, room);
        if (!channel && self && channel_name_valid(link, room)) {
            channel = channel_open(link, room);
            if (!channel) return -1;
        }
        if (!channel || (!channel->joined && !self)) return 0;
        if (self) {
            bool changed = !channel->joined;
            channel->joined = true;
            if (!channel->parting) channel->wanted = true;
            channel_default_status(link);
            if (changed && channel_state(link, channel, "joined") < 0) return -1;
            if (link_name_equal(link, room, link->room) && link_flush_pending(link) < 0)
                return -1;
        }
        struct irc_member *member = member_add(link, channel, sender, false);
        if (!member) return -1;
        return link_emit(irc, link, SNAG_IRC_JOIN, channel->room, sender, "",
            member && member->op, timestamp_ms);
    }
    if (snag_string_in(message.command, "PART KICK") && sender && message.param_count) {
        bool kick = !strcmp(message.command, "KICK");
        if (kick && message.param_count < 2u) return 0;
        struct irc_channel *channel = channel_find(link, message.params[0]);
        if (!channel || !channel->joined) return 0;
        const char *leaving = kick ? message.params[1] : sender;
        struct irc_member *member = member_find(link, channel, leaving);
        bool op = member && member->op;
        const char *reason = message.param_count > (kick ? 2u : 1u) ?
            message.params[kick ? 2u : 1u] : "";
        char description[SNAG_IRC_TEXT_MAX + 1u];
        if (kick) {
            size_t prefix = strlen(sender) + sizeof("kicked by : ") - 1u;
            size_t take = utf8_chunk(reason, strlen(reason), SNAG_IRC_TEXT_MAX - prefix);
            (void)snprintf(description, sizeof(description), "kicked by %s: %.*s",
                sender, (int)take, reason);
            reason = description;
        }
        member_remove(link, channel, leaving);
        if (link_name_equal(link, leaving, link->accepted_nick)) {
            if (channel_leave(link, channel, reason) < 0) return -1;
        }
        return link_emit(irc, link, SNAG_IRC_PART, channel->room,
            leaving, reason, op, timestamp_ms);
    }
    if (strcmp(message.command, "QUIT") == 0 && sender) {
        const char *reason = message.param_count ? message.params[0] : "";
        if (!link->historical && query_lifecycle(irc, link, SNAG_IRC_QUIT,
            sender, reason, timestamp_ms) < 0) return -1;
        for (struct irc_channel *channel = link->channels; channel; channel = channel->next) {
            struct irc_member *member = member_find(link, channel, sender);
            if (!member) continue;
            bool op = member->op;
            member_remove(link, channel, sender);
            if (link_emit(irc, link, SNAG_IRC_QUIT, channel->room,
                sender, reason, op, timestamp_ms) < 0) return -1;
        }
        return 0;
    }
    if (strcmp(message.command, "NICK") == 0 && sender && message.param_count) {
        bool known = link_name_equal(link, sender, link->accepted_nick);
        if (!nick_valid(message.params[0]) || strcmp(sender, message.params[0]) == 0) return 0;
        if (!link->historical && query_lifecycle(irc, link, SNAG_IRC_NICK,
            sender, message.params[0], timestamp_ms) < 0) return -1;
        for (struct irc_channel *channel = link->channels; channel; channel = channel->next)
            if (member_find(link, channel, sender)) known = true;
        /* Either role may hear its partner's rename before the self ack. */
        if (!link->historical && known) {
            for (size_t i = 0u; i < irc->conn_count; ++i) {
                struct irc_conn *own = &irc->conns[i];
                if (own->registered && link_name_equal(link, own->accepted_nick, sender)) {
                    /* Keep a later requested name until its own acknowledgement. */
                    if (link_name_equal(link, own->nick, own->accepted_nick))
                        (void)snag_strcpy(own->nick, sizeof(own->nick), message.params[0]);
                    (void)snag_strcpy(own->accepted_nick,
                        sizeof(own->accepted_nick), message.params[0]);
                }
            }
        }
        for (struct irc_channel *channel = link->channels; channel; channel = channel->next) {
            struct irc_member *member = member_find(link, channel, sender);
            if (!member) continue;
            bool op = member->op;
            (void)snag_strcpy(member->nick, sizeof(member->nick), message.params[0]);
            if (link_emit(irc, link, SNAG_IRC_NICK, channel->room,
                sender, message.params[0], op, timestamp_ms) < 0) return -1;
        }
        return 0;
    }
    struct irc_channel *channel = message.param_count ?
        channel_find(link, message.params[0]) : NULL;
    if (strcmp(message.command, "MODE") == 0 && sender && message.param_count >= 3u &&
        channel && channel->joined && nick_valid(message.params[2]) &&
        (strcmp(message.params[1], "+o") == 0 || strcmp(message.params[1], "-o") == 0)) {
        struct irc_member *target = member_add(link, channel, message.params[2], false);
        if (!target) return -1;
        struct irc_member *actor = member_find(link, channel, sender);
        char mode_text[SNAG_CONFIG_IRC_NICK_MAX + 4u];
        if (target) target->op = message.params[1][0] == '+';
        if (link_name_equal(link, message.params[2], link->accepted_nick))
            channel->op = message.params[1][0] == '+';
        channel_default_status(link);
        (void)snprintf(mode_text, sizeof(mode_text), "%s %s", message.params[1], message.params[2]);
        return link_emit(irc, link, SNAG_IRC_MODE, channel->room, sender,
            mode_text, actor && actor->op, timestamp_ms);
    }
    if (strcmp(message.command, "TOPIC") == 0 && sender && message.param_count >= 2u &&
        channel && channel->joined) {
        struct irc_member *member = member_find(link, channel, sender);
        return link_emit(irc, link, SNAG_IRC_TOPIC, channel->room, sender,
            message.params[1], member && member->op, timestamp_ms);
    }
    if (snag_string_in(message.command, "PRIVMSG NOTICE") && sender &&
        message.param_count >= 2u && channel && channel->joined) {
        for (size_t i = 0u; i < irc->conn_count; ++i)
            if (!link->event_stream[0] && !link->historical && irc->conns[i].registered &&
                link_name_equal(link, sender, irc->conns[i].accepted_nick)) return 0;
        if (strlen(message.params[1]) > SNAG_IRC_TEXT_MAX) return 1;
        struct irc_member *member = member_find(link, channel, sender);
        return link_emit(irc, link, strcmp(message.command, "NOTICE") == 0 ?
            SNAG_IRC_NOTICE : SNAG_IRC_MESSAGE, channel->room, sender,
            message.params[1], member && member->op, timestamp_ms);
    }
    if (link->registered && message.param_count >= 3u &&
        snag_string_in(message.command, "403 404 405 407 442 471 473 474 475 476 477 482 489") &&
        channel_name_valid(link, message.params[1])) {
        channel = channel_find(link, message.params[1]);
        if (channel && (!channel->joined || !strcmp(message.command, "442"))) {
            if (channel_leave(link, channel, message.params[message.param_count - 1u]) < 0)
                return -1;
        }
        char description[SNAG_IRC_TEXT_MAX + 1u];
        const char *reason = message.params[message.param_count - 1u];
        size_t take = utf8_chunk(reason, strlen(reason), SNAG_IRC_TEXT_MAX - 4u);
        (void)snprintf(description, sizeof(description), "%s %.*s",
            message.command, (int)take, reason);
        return link_emit(irc, link, SNAG_IRC_NOTICE, message.params[1], "",
            description, false, timestamp_ms);
    }
    if (strcmp(message.command, "433") == 0 && !link->registered) return link_retry_nick(link);
    if (link->registered && link->joined &&
        snag_string_in(message.command, "432 433 436 437") && message.param_count &&
        *message.params[message.param_count - 1u]) {
        return link_emit(irc, link, SNAG_IRC_NOTICE, link->room, "",
            message.params[message.param_count - 1u], false, timestamp_ms);
    }
    if (!strcmp(message.command, "ERROR")) return 1;
    return 0;
}

static void
link_disconnect(struct snag_irc_core *irc, struct irc_conn *link, const char *reason)
{
    struct snag_irc_event event;

    bool was_registered = link->registered;
    if (link->fd != SNAG_SOCKET_INVALID) (void)snag_socket_close(link->fd);
    link->fd = SNAG_SOCKET_INVALID;
    link->connecting = false;
    link->registered = false;
    link->joined = false;
    link->input_len = 0u;
    channels_reset(link);
    snag_buf_reset(&link->output);
    link->output_offset = 0u;
    link->pending_inflight = 0u;
    link->private_broken = false;
    link->retry_at_ms = snag_monotonic_ms() + IRC_RETRY_MS;
    if (irc->connection[0]) {
        if (was_registered) {
            if (query_epoch_end(irc, reason) < 0) irc->callback_failed = true;
            link->private_broken = false;
        }
        return;
    }
    if (link_emit_enabled(link) && !link->outage_reported) {
        link->outage_reported = true;
        event_init(irc, &event, SNAG_IRC_DISCONNECTED, link->endpoint,
                   link->room, link->accepted_nick, reason, link->op, false, false);
        (void)emit_event(irc, &event, false);
    }
}

static int
read_conn(struct snag_irc_core *irc, struct irc_conn *conn)
{
    for (unsigned int batch = 0u; batch < 4u; ++batch) {
        ssize_t got;
        size_t available = sizeof(conn->input) - conn->input_len;

        if (!available) return 1;
        got = snag_socket_recv(conn->fd, conn->input + conn->input_len, available);
        if (got > 0) {
            conn->input_len += (size_t)got;
        } else if (got == 0) {
            return 1;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            return 1;
        }
        for (;;) {
            unsigned char *lf = memchr(conn->input, '\n', conn->input_len);
            char line[IRC_LINE_MAX + 1u];
            size_t wire_len;
            size_t len;
            int rc;

            if (!lf) break;
            wire_len = (size_t)(lf - conn->input) + 1u;
            len = wire_len - 1u;
            if (len && conn->input[len - 1u] == '\r') --len;
            if (len > IRC_LINE_MAX || !snag_utf8_valid(conn->input, len, true)) return 1;
            memcpy(line, conn->input, len);
            line[len] = '\0';
            memmove(conn->input, conn->input + wire_len, conn->input_len - wire_len);
            conn->input_len -= wire_len;
            if (trace_wire(conn, '<', line, len) < 0) return -1;
            rc = conn->outgoing ? client_dispatch(irc, conn, line) : server_dispatch(irc, conn, line);
            if (rc != 0) return rc;
        }
    }
    return 0;
}

static int
accept_peers(struct snag_irc_core *irc)
{
    for (unsigned int batch = 0u; batch < 8u; ++batch) {
        snag_socket fd = snag_socket_accept(irc->listener);
        struct irc_conn *peer = NULL;

        if (fd == SNAG_SOCKET_INVALID) {
            if (errno == EINTR) continue;
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        }
        for (size_t i = 2u; i < irc->conn_count; ++i)
            if (!irc->conns[i].used) {
                peer = &irc->conns[i];
                break;
            }
        if (!peer) {
            (void)snag_socket_close(fd);
            continue;
        }
        conn_init(peer, irc);
        peer->fd = fd;
        snag_socket_nodelay(fd);
        peer->used = true;
    }
    return 0;
}

static int
complete_link_connect(struct snag_irc_core *irc, struct irc_conn *link)
{
    if (snag_socket_connected(link->fd) < 0) return 1;
    return client_handshake(irc, link);
}

static int
start_due_links(struct snag_irc_core *irc)
{
    uint64_t now = snag_monotonic_ms();

    for (size_t i = 0; i < irc->conn_count; ++i) {
        struct irc_conn *link = &irc->conns[i];
        int rc;

        if (!link->outgoing || link->fd != SNAG_SOCKET_INVALID || now < link->retry_at_ms) continue;
        rc = start_link(irc, link);
        if (rc < 0) return -1;
        if (rc > 0) link_disconnect(irc, link, "connect failed; retrying");
    }
    return 0;
}

static void
derive_server_name(char out[SNAG_CONFIG_IRC_NICK_MAX + 1u])
{
    char host[256u];
    size_t used = 0u;

    if (gethostname(host, sizeof(host)) < 0) memcpy(host, "localhost", 10u);
    host[sizeof(host) - 1u] = '\0';
    for (size_t i = 0; host[i] && used < SNAG_CONFIG_IRC_NICK_MAX; ++i) {
        unsigned char c = (unsigned char)host[i];
        out[used++] = snag_irc_nick_char(c) ? (char)c : '_';
    }
    if (!used || (out[0] >= '0' && out[0] <= '9') || out[0] == '-') {
        memcpy(out, "localhost", 10u);
        return;
    }
    out[used] = '\0';
}

static void
derive_room(char out[SNAG_CONFIG_IRC_ROOM_MAX + 2u])
{
    char host[256u];
    size_t used = 0u;

    if (gethostname(host, sizeof(host)) < 0) memcpy(host, "localhost", 10u);
    host[sizeof(host) - 1u] = '\0';
    out[0] = '#';
    for (size_t i = 0u; host[i] && used < SNAG_CONFIG_IRC_ROOM_MAX; ++i) {
        unsigned char c = (unsigned char)host[i];
        out[++used] = c > 0x20u && c < 0x7fu && c != ',' && c != ':' && c != '#' ? (char)c : '_';
    }
    if (!used) memcpy(out, "#localhost", 11u);
    else out[used + 1u] = '\0';
}

int
snag_irc_core_open(struct snag_irc_core **out, const struct snag_config *config,
             const char *cwd, bool network, snag_irc_event_fn event_fn,
             snag_irc_trace_fn trace_fn, void *event_opaque, char *error, size_t error_size)
{
    struct snag_irc_core *irc;

    if (!out || !config || !cwd || (network &&
        (config->irc.listen_explicit ? config->irc.client_count != 0u : config->irc.client_count != 1u))) {
        errno = EINVAL;
        return snag_errorf(error, error_size, "invalid IRC startup state");
    }
    *out = NULL;
    irc = calloc(1u, sizeof(*irc));
    if (!irc) return -1;
    if (snag_network_init() < 0) {
        free(irc);
        return -1;
    }
    if (snag_random_id(irc->stream) < 0) {
        snag_network_free();
        free(irc);
        return -1;
    }
    snag_buf_init(&irc->publications, IRC_OUTPUT_MAX);
    irc->listener = SNAG_SOCKET_INVALID;
    irc->hosting = config->irc.listen_explicit;
    irc->event_fn = event_fn;
    irc->trace_fn = trace_fn;
    irc->event_opaque = event_opaque;
    irc->history_limit = config->irc.history_lines;
    irc->history = calloc(irc->history_limit, sizeof(*irc->history));
    irc->conn_count = network && irc->hosting ? IRC_SERVER_PEERS + 2u : 2u;
    irc->conns = calloc(irc->conn_count, sizeof(*irc->conns));
    irc->replay_members = calloc(IRC_REPLAY_MEMBERS_MAX, sizeof(*irc->replay_members));
    if (!irc->conns) goto fail;
    for (size_t i = 0; i < irc->conn_count; ++i) conn_init(&irc->conns[i], irc);
    if ((irc->history_limit && !irc->history) || !irc->replay_members ||
        !snag_strcpy(irc->listen, sizeof(irc->listen), config->irc.listen)) goto fail;
    for (size_t role = 0; role < 2u; ++role) {
        struct irc_conn *identity = &irc->conns[role];
        identity->used = true;
        identity->outgoing = network && !irc->hosting;
        identity->role = role == 0u ? LINK_AGENT : LINK_OPERATOR;
        identity->agent_role = role == LINK_AGENT;
        identity->joined = irc->hosting;
        identity->registered = irc->hosting;
        identity->op = irc->hosting && role == LINK_OPERATOR;
        identity->nick_implicit = role == LINK_AGENT ?
            config->irc.model_nick_implicit : config->irc.operator_nick_implicit;
        if (!snag_strcpy(identity->preferred_nick, sizeof(identity->preferred_nick),
                        role == LINK_AGENT ? config->irc.model_nick : config->irc.operator_nick)) goto fail;
        memcpy(identity->nick, identity->preferred_nick, sizeof(identity->nick));
        memcpy(identity->accepted_nick, identity->nick, sizeof(identity->nick));
        (void)snag_strcpy(identity->user, sizeof(identity->user), role == LINK_AGENT ? "agent" : "operator");
        if (identity->outgoing)
            (void)snag_strcpy(identity->endpoint, sizeof(identity->endpoint), config->irc.clients[0]);
    }
    derive_server_name(irc->server_name);
    if (config->irc.room_name[0]) {
        irc->room_explicit = true;
        if (normalize_room(irc->room, sizeof(irc->room), config->irc.room_name) < 0) goto fail;
    } else {
        derive_room(irc->room);
    }
    if (strlen(cwd) > IRC_TOPIC_MAX || sanitize_text(irc->topic, sizeof(irc->topic), cwd) < 0) {
        (void)snag_fail(error, error_size, ENAMETOOLONG, "IRC launch path is too long for a topic");
        goto fail;
    }
    if (network && irc->hosting) {
        irc->listener = open_listener(irc->listen, error, error_size);
        if (irc->listener == SNAG_SOCKET_INVALID) goto fail;
    }
    *out = irc;
    return 0;
fail:
    if (error_size && !error[0]) snag_errorf(error, error_size, "cannot initialize IRC state");
    snag_irc_core_close(irc);
    return -1;
}

void
snag_irc_core_close(struct snag_irc_core *irc)
{
    if (!irc) return;
    if (irc->listener != SNAG_SOCKET_INVALID) (void)snag_socket_close(irc->listener);
    for (size_t i = 0; irc->conns && i < irc->conn_count; ++i) conn_release(&irc->conns[i]);
    while (irc->cursors) {
        struct irc_cursor *next = irc->cursors->next;
        free(irc->cursors); irc->cursors = next;
    }
    snag_buf_free(&irc->publications);
    queries_free(irc);
    free(irc->history);
    free(irc->replay_members);
    free(irc->conns);
    snag_network_free();
    free(irc);
}

int
snag_irc_core_retire(struct snag_irc_core *irc)
{
    if (!irc->connection[0]) return 0;
    for (size_t role = 0u; role < 2u; ++role) {
        struct irc_conn *link = &irc->conns[role];
        link->registered = false;
        for (struct irc_channel *channel = link->channels; channel; channel = channel->next)
            channel->wanted = false;
        channels_reset(link);
    }
    return query_epoch_end(irc, "endpoint removed");
}

int
snag_irc_core_bind(struct snag_irc_core *irc, const char *connection,
                   const char *endpoint, uint64_t generation)
{
    if (!irc || irc->connection[0] || !generation || generation > INT64_MAX ||
        !endpoint || !snag_irc_endpoint_equal(endpoint,
            irc->hosting ? irc->listen : irc->conns[0].endpoint) ||
        (connection && !snag_hex_is_lower(connection, SNAG_ID_HEX_LEN))) return snag_errno(EINVAL);
    if (!snag_strcpy(irc->connection_endpoint, sizeof(irc->connection_endpoint), endpoint))
        return -1;
    if (connection) memcpy(irc->connection, connection, sizeof(irc->connection));
    else if (snag_random_id(irc->connection) < 0) return -1;
    irc->generation = generation;
    return snag_random_id(irc->connection_events[0]) < 0 ||
        snag_random_id(irc->connection_events[1]) < 0 ? -1 : 0;
}

int
snag_irc_core_restore_channels(struct snag_irc_core *irc, const json_t *directory)
{
    if (!irc || !irc->connection[0] || irc->connections_announced) return snag_errno(EINVAL);
    const json_t *connection = json_object_get(directory, irc->connection);
    const json_t *entries = json_object_get(connection, "conversations");
    const char *id;
    const json_t *entry;
    json_object_foreach((json_t *)entries, id, entry) {
        struct snag_irc_event event;
        uint64_t seq;
        const json_t *data = json_object_get(entry, "data");
        if (snag_irc_event_record_read("irc_event_v2", data, &event) < 0 ||
            snag_json_integer_u64(entry, "seq", &seq) < 0) return -1;
        if (event.route.kind != SNAG_IRC_CHANNEL || !event.route.membership[0]) continue;
        if (!channel_syntax_valid(event.room)) return snag_errno(EINVAL);
        struct irc_conn *link = &irc->conns[event.route.identity == SNAG_IRC_AGENT ?
            LINK_AGENT : LINK_OPERATOR];
        struct irc_channel *channel = link->channels;
        while (channel && strcmp(channel->room, event.room)) channel = channel->next;
        if (channel && channel->restored_seq >= seq) continue;
        if (!channel) {
            channel = calloc(1u, sizeof(*channel));
            if (!channel) return -1;
            if (snag_random_id(channel->membership) < 0) {
                free(channel);
                return -1;
            }
            memcpy(channel->room, event.room, sizeof(channel->room));
            channel->next = link->channels;
            link->channels = channel;
        }
        memcpy(channel->id, id, sizeof(channel->id));
        channel->wanted = event.route.rejoin;
        channel->restored_seq = seq;
    }
    return 0;
}

size_t
snag_irc_core_pending(const struct snag_irc_core *irc)
{
    size_t bytes = 0u;

    for (size_t i = 0u; i < irc->conn_count; ++i) {
        const struct irc_conn *conn = &irc->conns[i];
        bytes += conn->pending.len + conn->output.len - conn->output_offset;
        for (const struct irc_private_send *send = conn->private_sends; send; send = send->next)
            bytes += send->length - send->offset;
    }
    return bytes;
}

int
snag_irc_core_copy_history(struct snag_irc_core *dst, const struct snag_irc_core *src, bool hosted_only)
{
    if (dst->hosting && src->hosting && !strcmp(dst->room, src->room) &&
        snag_irc_endpoint_equal(dst->listen, src->listen)) {
        memcpy(dst->stream, src->stream, sizeof(dst->stream));
        dst->sequence = src->sequence;
    }
    if (dst->hosting) {
        struct irc_cursor *host = find_cursor((struct snag_irc_core *)src, dst->listen, dst->room, false);
        if (host) {
            memcpy(dst->stream, host->stream, sizeof(dst->stream));
            dst->sequence = host->sequence;
        }
    }
    for (const struct irc_cursor *c = src->cursors; c; c = c->next) {
        struct irc_cursor *copy = find_cursor(dst, c->endpoint, c->room, true);
        if (!copy) return -1;
        memcpy(copy->stream, c->stream, sizeof(copy->stream));
        copy->sequence = c->sequence;
    }
    for (size_t i = 0u; i < src->history_count; ++i) {
        const struct snag_irc_event *event = &src->history[(src->history_start + i) % src->history_limit];

        /* These are already admitted records; a bounded tail need not contain
         * all the membership transitions required for replay validation. */
        if (!hosted_only || hosted_history_event(dst, event)) snag_irc_core_remember(dst, event);
    }
    if (!hosted_only) {
        dst->replay_member_count = src->replay_member_count;
        memcpy(dst->replay_members, src->replay_members,
                src->replay_member_count * sizeof(*src->replay_members));
    }
    return 0;
}

int
snag_irc_core_tick(struct snag_irc_core *irc, int timeout_ms, snag_wake_fd wake_fd,
             char *error, size_t error_size)
{
    snag_socket_event fds[4u + IRC_SERVER_PEERS];
    struct irc_conn *owners[sizeof(fds) / sizeof(fds[0])];
    size_t count = 1u;
    int polled;

    if (!irc) return snag_errno(EINVAL);
    if (irc->connection[0] && !irc->connections_announced) {
        irc->connections_announced = true;
        for (size_t role = 0u; role < 2u; ++role) {
            if (connection_event(irc, &irc->conns[role], "") < 0) goto fail;
            for (struct irc_channel *channel = irc->conns[role].channels; channel;
                channel = channel->next)
                if (channel_state(&irc->conns[role], channel, "resumed") < 0) goto fail;
        }
    }
    if (start_due_links(irc) < 0) goto fail;
    for (size_t i = 0u; i < irc->conn_count; ++i) {
        uint64_t now = snag_monotonic_ms();
        const struct irc_conn *link = &irc->conns[i];
        int retry;

        if (!link->outgoing || link->fd != SNAG_SOCKET_INVALID) continue;
        retry = link->retry_at_ms > now ? (int)(link->retry_at_ms - now) : 0;
        if (timeout_ms < 0 || retry < timeout_ms) timeout_ms = retry;
    }
    fds[0] = (snag_socket_event){wake_fd, SNAG_NET_READ, 0};
    if (irc->listener != SNAG_SOCKET_INVALID) {
        fds[count].fd = irc->listener;
        fds[count].events = SNAG_NET_READ;
        fds[count].revents = 0;
        owners[count++] = NULL;
    }
    for (size_t i = 0; i < irc->conn_count; ++i) {
        struct irc_conn *link = &irc->conns[i];
        if (link->fd == SNAG_SOCKET_INVALID) continue;
        if (link->private_broken) {
            if (link->outgoing)
                link_disconnect(irc, link, "private recipient changed; reconnecting");
            else server_drop_peer(irc, link, "private recipient changed");
            if (irc->callback_failed) goto fail;
            continue;
        }
        if (link->outgoing && link->joined && !link->output.len && link->pending.len &&
            link_flush_pending(link) < 0) {
            link_disconnect(irc, link, "outbound queue failed; retrying");
            if (irc->callback_failed) goto fail;
            continue;
        }
        fds[count].fd = link->fd;
        fds[count].events = SNAG_NET_READ | (link->connecting ||
            link->output_offset < link->output.len || private_ready(link) ? SNAG_NET_WRITE : 0);
        fds[count].revents = 0;
        owners[count++] = link;
    }
    do {
        polled = snag_socket_poll(fds, count, timeout_ms);
    } while (polled < 0 && errno == EINTR);
    if (polled < 0) goto fail;
    for (size_t i = 1u; i < count; ++i) {
        struct irc_conn *conn = owners[i];
        int rc = 0;

        if (!fds[i].revents) continue;
        if (!conn) {
            if (accept_peers(irc) < 0) goto fail;
            continue;
        }
        if (conn->outgoing && conn->connecting &&
            (fds[i].revents & (SNAG_NET_WRITE | SNAG_NET_ERROR | SNAG_NET_HUP))) {
            rc = complete_link_connect(irc, conn);
            if (rc < 0) goto fail;
            if (rc > 0) {
                link_disconnect(irc, conn, "connect failed; retrying");
                continue;
            }
        }
        if (conn->fd != SNAG_SOCKET_INVALID && (fds[i].revents & SNAG_NET_READ)) rc = read_conn(irc, conn);
        if (rc < 0) {
            if (irc->callback_failed) goto fail;
            if (conn->outgoing) link_disconnect(irc, conn, "protocol error; retrying");
            else server_drop_peer(irc, conn, "malformed or oversized input");
            if (irc->callback_failed) goto fail;
            continue;
        }
        if (rc > 0 || conn->private_broken ||
            (fds[i].revents & (SNAG_NET_ERROR | SNAG_NET_HUP | SNAG_NET_INVALID))) {
            if (conn->outgoing) link_disconnect(irc, conn, "connection closed; retrying");
            else server_drop_peer(irc, conn, "connection closed");
            continue;
        }
        if (conn->fd != SNAG_SOCKET_INVALID &&
            (conn->output_offset < conn->output.len || private_ready(conn)) &&
            flush_conn(conn) < 0) {
            if (conn->outgoing) link_disconnect(irc, conn, "write failed; retrying");
            else server_drop_peer(irc, conn, "write failed");
            if (irc->callback_failed) goto fail;
        }
    }
    return 0;
fail: return snag_errorf(error, error_size, "IRC event loop failed: %s", strerror(errno));
}

static size_t
utf8_chunk(const char *text, size_t len, size_t max)
{
    size_t chunk = len < max ? len : max;

    while (chunk && chunk < len && (((unsigned char)text[chunk] & 0xc0u) == 0x80u)) --chunk;
    return chunk;
}

static size_t
chat_chunk(const char *text, size_t len)
{
    size_t take = utf8_chunk(text, len, SNAG_IRC_TEXT_MAX);

    if (take < len)
        for (size_t i = take; i > 0u; --i)
            if (text[i - 1u] == ' ') return i;
    return take;
}

static int
send_chat_line(struct snag_irc_core *irc, enum link_role role, enum snag_irc_event_kind kind,
               const char *text, size_t len)
{
    char clean[SNAG_IRC_TEXT_MAX + 1u];
    struct snag_irc_event event;
    struct irc_conn *identity = &irc->conns[role];

    if (!len || len > SNAG_IRC_TEXT_MAX) return 0;
    {
        char raw[SNAG_IRC_TEXT_MAX + 1u];
        memcpy(raw, text, len);
        raw[len] = '\0';
        if (sanitize_text(clean, sizeof(clean), raw) < 0) return -1;
    }
    if (!clean[0]) return 0;
    event_init(irc, &event, kind, irc->hosting ? irc->listen : identity->endpoint,
               irc->hosting ? irc->room : identity->room,
               identity->accepted_nick, clean, identity->joined && identity->op, false, true);
    if (irc->hosting) return server_publish(irc, kind, identity->accepted_nick, "local", clean,
                              identity->joined && identity->op, true);
    if (link_queue_pending(identity, kind, clean) < 0 ||
        (identity->joined && link_flush_pending(identity) < 0)) {
            return -1;
    }
    return irc->conn_count && irc->conns[0].cap_catchup ? 0 : emit_event(irc, &event, true);
}

static int
send_chat(struct snag_irc_core *irc, enum link_role role, enum snag_irc_event_kind kind, const char *text,
          char *error, size_t error_size)
{
    const char *cursor = text;
    size_t remaining;

    if (!irc || !text || !*text || !snag_utf8_valid((const unsigned char *)text, strlen(text), true)) {
        errno = EINVAL;
        return snag_errorf(error, error_size, "IRC chat must be nonempty valid UTF-8");
    }
    remaining = strlen(cursor);
    while (remaining) {
        const char *newline = memchr(cursor, '\n', remaining);
        size_t line_len = newline ? (size_t)(newline - cursor) : remaining;

        while (line_len) {
            size_t chunk = chat_chunk(cursor, line_len);
            if (!chunk || send_chat_line(irc, role, kind, cursor, chunk) < 0) goto fail;
            cursor += chunk;
            remaining -= chunk;
            line_len -= chunk;
        }
        if (newline) {
            ++cursor;
            --remaining;
        } else {
            break;
        }
    }
    return 0;
fail: return snag_errorf(error, error_size, "cannot queue IRC chat: %s", strerror(errno));
}

static int
set_topic_as(struct snag_irc_core *irc, const char *topic, enum link_role role,
             char *error, size_t error_size)
{
    char clean[sizeof(irc->topic)];
    struct irc_conn *identity;

    if (!irc || !topic || strlen(topic) > IRC_TOPIC_MAX || strchr(topic, '\r') || strchr(topic, '\n') ||
        !snag_utf8_valid((const unsigned char *)topic, strlen(topic), true)) {
        return snag_fail(error, error_size, EINVAL, "IRC topic is invalid or too long");
    }
    if (sanitize_text(clean, sizeof(clean), topic) < 0)
        return snag_errorf(error, error_size, "IRC topic is invalid or too long");
    identity = &irc->conns[role];
    bool hosted_agent = irc->hosting && role == LINK_AGENT;
    if (!identity->joined)
        return snag_fail(error, error_size, EACCES,
                      role == LINK_AGENT ? "agent identity is not in any joined room" :
                      "operator identity is not in any joined room");
    /* External servers own channel policy: a non-op member may set the topic
     * when -t is active, while a +t server will reject the queued command.
     * The embedded room is deliberately +t and can enforce that synchronously. */
    if (irc->hosting && !identity->op && !hosted_agent)
        return snag_fail(error, error_size, EACCES,
                      role == LINK_AGENT ? "agent identity is not an operator in any joined room" :
                      "operator identity is not an operator in any joined room");
    if (irc->hosting) {
        memcpy(irc->topic, clean, strlen(clean) + 1u);
        if (server_publish(irc, SNAG_IRC_TOPIC, identity->nick, "local", clean, true, true) < 0) goto fail;
    } else if (queue_line(identity, "TOPIC %s :%s", identity->room, clean) < 0) {
        goto fail;
    }
    return 0;
fail: return snag_errorf(error, error_size, "cannot queue IRC topic change");
}

static int
set_nick_as(struct snag_irc_core *irc, const char *nick, enum link_role role,
            char *error, size_t error_size)
{
    char clean[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char old[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    struct irc_conn *identity;

    if (!irc || !nick || !nick_valid(nick))
        return snag_fail(error, error_size, EINVAL, "IRC nick is invalid");
    if (!snag_strcpy(clean, sizeof(clean), nick))
        return snag_errorf(error, error_size, "IRC nick is invalid");
    identity = &irc->conns[role];
    if (!snag_strcpy(old, sizeof(old), identity->nick)) old[0] = '\0';
    if (irc->hosting) {
        struct irc_conn *collision = server_peer_by_nick(irc, clean);
        if (collision && collision != identity) {
            return snag_fail(error, error_size, EINVAL, "IRC nickname is already in use");
        }
        if (strcmp(old, clean) == 0) return 0;
        if (!snag_strcpy(identity->nick, sizeof(identity->nick), clean) ||
            !snag_strcpy(identity->accepted_nick, sizeof(identity->accepted_nick), clean))
            goto fail;
        if (server_publish(irc, SNAG_IRC_NICK, old, "local", clean,
                           identity->joined && identity->op, true) < 0)
            goto fail;
        return 0;
    }
    if (!snag_strcpy(identity->preferred_nick, sizeof(identity->preferred_nick), clean) ||
        !snag_strcpy(identity->nick, sizeof(identity->nick), clean))
        goto fail;
    identity->nick_implicit = false;
    identity->nick_suffix = 0u;
    if (!identity->registered) return 0;
    if (queue_line(identity, "NICK %s", clean) < 0) goto fail;
    return 0;
fail: return snag_errorf(error, error_size, "cannot change IRC nick");
}

int
snag_irc_core_send(struct snag_irc_core *irc, bool model, enum snag_irc_event_kind kind, const char *text,
                   char *error, size_t error_size)
{
    enum link_role role = model ? LINK_AGENT : LINK_OPERATOR;

    if (kind == SNAG_IRC_TOPIC) return set_topic_as(irc, text, role, error, error_size);
    if (kind == SNAG_IRC_NICK) return set_nick_as(irc, text, role, error, error_size);
    return send_chat(irc, role, kind, text, error, error_size);
}

static int
snapshot_member(struct snag_buf *out, struct snag_buf *nicks, const char *nick, bool op)
{
    if (snag_buf_printf(out, " %s%s", op ? "@" : "", nick) < 0) return -1;
    if (nicks && strlen(nick) + 1u <= nicks->max - nicks->len &&
        snag_buf_printf(nicks, "%s\n", nick) < 0) return -1;
    return 0;
}

static int
snapshot_channel(const struct irc_conn *link, const struct irc_channel *channel,
                 struct snag_buf *out, struct snag_buf *nicks)
{
    if (!channel || !channel->joined) return 0;
    if (snag_buf_printf(out, "channel[%s/%s]: joined%s\ntopic[%s/%s]: %s\nmembers[%s/%s]:",
        link->endpoint, channel->room, channel->op ? " as operator" : "",
        link->endpoint, channel->room, channel->topic,
        link->endpoint, channel->room) < 0) return -1;
    for (size_t j = 0u; j < channel->member_count; ++j)
        if (snapshot_member(out, nicks, channel->members[j].nick, channel->members[j].op) < 0)
            return -1;
    return snag_buf_putc(out, '\n');
}

static int
snapshot_network(const struct snag_irc_core *irc, struct snag_buf *out, struct snag_buf *nicks)
{
    if (irc->hosting) {
        if (snag_buf_printf(out, "room: %s\ntopic: %s\n", irc->room, irc->topic) < 0) goto fail;
        if (snag_buf_printf(out, "members[%s]:", irc->listen) < 0) goto fail;
        for (size_t i = 0; i < irc->conn_count; ++i) {
            const struct irc_conn *peer = &irc->conns[i];
            if (peer->used && peer->joined && snapshot_member(out, nicks, peer->nick, peer->op) < 0)
                goto fail;
        }
        if (snag_buf_append(out, "\n", 1u) < 0) goto fail;
    }
    if (irc->conns[LINK_OPERATOR].outgoing) {
        const struct irc_conn *link = &irc->conns[LINK_OPERATOR];
        const struct irc_conn *agent = &irc->conns[LINK_AGENT];

        if (snag_buf_printf(out, "endpoint[%s]: %s%s%s\n", link->endpoint,
                           link->joined ? "joined " : link->registered ? "registered" :
                           link->connecting ? "connecting" : "disconnected",
                           link->joined ? link->room : "",
                           link->joined && link->op ? " as operator" : "") < 0) goto fail;
        if (snag_buf_printf(out, "aliases[%s]: model %s operator %s\n", link->endpoint, agent->accepted_nick,
                           link->accepted_nick) < 0) goto fail;
        const struct irc_channel *primary = channel_find(link, link->room);
        if (snapshot_channel(link, primary, out, nicks) < 0) goto fail;
        for (const struct irc_channel *channel = link->channels; channel; channel = channel->next)
            if (channel != primary && snapshot_channel(link, channel, out, NULL) < 0) goto fail;
    }
    return 0;
fail: return -1;
}

int
snag_irc_core_view(const struct snag_irc_core *irc, struct snag_irc_view *view)
{
    int rc = -1;

    memset(view, 0, sizeof(*view));
    view->revision = irc->route_revision + 1u;
    memcpy(view->connection, irc->connection, sizeof(view->connection));
    view->generation = irc->generation;
    view->casemapping[SNAG_IRC_OPERATOR] = irc->conns[LINK_OPERATOR].casemapping;
    view->casemapping[SNAG_IRC_AGENT] = irc->conns[LINK_AGENT].casemapping;
    (void)snag_strcpy(view->model, sizeof(view->model), snag_irc_core_model_nick(irc));
    (void)snag_strcpy(view->operator, sizeof(view->operator), snag_irc_core_operator_nick(irc));
    view->joined = irc->hosting || irc->conns[LINK_AGENT].joined;
    if (irc->hosting || irc->conns[LINK_AGENT].outgoing) (void)snag_strcpy(view->room, sizeof(view->room),
                        irc->hosting ? irc->room : irc->conns[0].room[0] ?
                        irc->conns[0].room : irc->conns[0].previous_room);
    const char truncated[] = "\n[IRC state abbreviated; remaining members/channels omitted]\n";
    struct snag_buf text = {.max = sizeof(view->text) - sizeof(truncated)};
    struct snag_buf nicks = {.max = sizeof(view->nicks) - 1u};
    int snapshot = snapshot_network(irc, &text, &nicks);
    if (snapshot < 0 && errno == EOVERFLOW) {
        text.max = sizeof(view->text) - 1u;
        snapshot = snag_buf_append(&text, truncated, sizeof(truncated) - 1u);
    }
    if (snapshot == 0) {
        memcpy(view->text, text.data, text.len);
        if (nicks.len) memcpy(view->nicks, nicks.data, nicks.len);
        rc = 0;
    }
    snag_buf_free(&text);
    snag_buf_free(&nicks);
    return rc;
}

int
snag_irc_core_history(const struct snag_irc_core *irc, struct snag_buf *out)
{
    if (snag_buf_append(out, "history:\n", 9u) < 0) goto fail;
    for (size_t i = 0; i < irc->history_count; ++i) {
        const struct snag_irc_event *event = &irc->history[(irc->history_start + i) % irc->history_limit];
        char when[32u];
        format_time(event->timestamp_ms, when);
        if (event->kind == SNAG_IRC_MESSAGE || event->kind == SNAG_IRC_NOTICE) {
            if (snag_buf_printf(out, "%s %s %s%s: %s\n", when, event->endpoint, event->op ? "@" : "",
                               event->nick, event->text) < 0) goto fail;
        } else if (snag_buf_printf(out, "%s %s * %s%s %s %s\n", when, event->endpoint, event->op ? "@" : "",
                                  event->nick, event_kind_text(event->kind), event->text) < 0) {
            goto fail;
        }
    }
    if (snag_buf_append(out, "[end IRC room snapshot]", 23u) < 0) goto fail;
    return 0;
fail: return -1;
}

static bool
event_field_safe(const char *text)
{
    size_t len = strlen(text);

    if (!snag_utf8_valid((const unsigned char *)text, len, true)) return false;
    for (size_t i = 0u; i < len; ++i)
        if ((unsigned char)text[i] < 0x20u || (unsigned char)text[i] == 0x7fu) return false;
    return true;
}

static bool
restored_event_shape_valid(const struct snag_irc_event *event)
{
    bool room = event->room[0] == '#' && room_valid(event->room);
    bool nick = nick_valid(event->nick);
    bool remembered = event_remembered(event->kind);

    if ((unsigned int)event->kind > (unsigned int)SNAG_IRC_HISTORY_READY ||
        !endpoint_valid(event->endpoint) || !event_field_safe(event->endpoint) ||
        !event_field_safe(event->room) || !event_field_safe(event->nick) ||
        !event_field_safe(event->text) || (event->historical && (event->local || !remembered)) ||
        (event->local && event->kind != SNAG_IRC_MESSAGE &&
         event->kind != SNAG_IRC_NOTICE && event->kind != SNAG_IRC_TOPIC &&
         event->kind != SNAG_IRC_NICK)) return false;
    switch (event->kind) {
    case SNAG_IRC_CONNECTED: return nick && !event->room[0] && !event->text[0] && !event->op &&
               !event->historical && !event->local;
    case SNAG_IRC_DISCONNECTED: return nick && (!event->room[0] || room) && event->text[0] &&
               !event->historical && !event->local;
    case SNAG_IRC_HISTORY_READY: return room && !event->nick[0] && !event->op &&
               !event->historical && !event->local;
    case SNAG_IRC_JOIN: return room && nick && !event->text[0];
    case SNAG_IRC_PART: case SNAG_IRC_QUIT: return room && nick;
    case SNAG_IRC_NICK: return room && nick && nick_valid(event->text);
    case SNAG_IRC_NOTICE:
        if (!event->nick[0]) return room && !event->op && event->text[0];
        /* FALLTHROUGH */
    case SNAG_IRC_MESSAGE: return (room || (event->local && !event->room[0])) && nick &&
               event->text[0];
    case SNAG_IRC_TOPIC: return room && (!event->nick[0] || nick) && (event->nick[0] || !event->op);
    case SNAG_IRC_MODE: return room && nick &&
               ((strcmp(event->text, "+o") == 0 || strcmp(event->text, "-o") == 0) ||
                ((strncmp(event->text, "+o ", 3u) == 0 || strncmp(event->text, "-o ", 3u) == 0) &&
                 nick_valid(event->text + 3u)));
    }
    return false;
}

static struct irc_replay_member *
replay_member_find(struct snag_irc_core *irc, const char *endpoint, const char *room, const char *nick)
{
    for (size_t i = 0u; i < irc->replay_member_count; ++i) {
        struct irc_replay_member *member = &irc->replay_members[i];

        if (strcmp(member->endpoint, endpoint) == 0 && irc_casecmp(member->room, room) == 0 &&
            irc_casecmp(member->nick, nick) == 0) return member;
    }
    return NULL;
}

static struct irc_replay_member *
replay_member_set(struct snag_irc_core *irc, const struct snag_irc_event *event, const char *nick, bool op)
{
    struct irc_replay_member *member = replay_member_find( irc, event->endpoint, event->room, nick);

    if (!member) {
        if (irc->replay_member_count == IRC_REPLAY_MEMBERS_MAX) irc->replay_member_count = 0u;
        member = &irc->replay_members[irc->replay_member_count++];
        memset(member, 0, sizeof(*member));
        (void)snprintf(member->endpoint, sizeof(member->endpoint), "%s", event->endpoint);
        (void)snprintf(member->room, sizeof(member->room), "%s", event->room);
        (void)snprintf(member->nick, sizeof(member->nick), "%s", nick);
    }
    member->op = op;
    return member;
}

static void
replay_member_remove(struct snag_irc_core *irc, struct irc_replay_member *member)
{
    size_t index = (size_t)(member - irc->replay_members);

    memmove(member, member + 1u, (irc->replay_member_count - index - 1u) * sizeof(*member));
    --irc->replay_member_count;
}

static void
replay_endpoint_clear(struct snag_irc_core *irc, const char *endpoint)
{
    for (size_t i = irc->replay_member_count; i > 0u; --i)
        if (strcmp(irc->replay_members[i - 1u].endpoint, endpoint) == 0)
            replay_member_remove(irc, &irc->replay_members[i - 1u]);
}

static int
replay_transition(struct snag_irc_core *irc, const struct snag_irc_event *event)
{
    struct irc_replay_member *member;

    if (event->historical) return 0;
    if (event->kind == SNAG_IRC_CONNECTED || event->kind == SNAG_IRC_DISCONNECTED ||
        event->kind == SNAG_IRC_HISTORY_READY) {
        replay_endpoint_clear(irc, event->endpoint);
        return 0;
    }
    member = replay_member_find(irc, event->endpoint, event->room, event->nick);
    if (event->kind == SNAG_IRC_JOIN) {
        (void)replay_member_set(irc, event, event->nick, event->op);
        return 0;
    }
    if (event->kind == SNAG_IRC_MODE) {
        const char *target = event->text[2] == ' ' ? event->text + 3u : event->nick;
        bool add = event->text[0] == '+';
        bool self = irc_casecmp(event->nick, target) == 0;

        if (!member && event->op) member = replay_member_set(irc, event, event->nick, true);
        if (member && (!member->op || (self && !add ? event->op : !event->op))) return snag_errno(EINVAL);
        (void)replay_member_set(irc, event, target, add);
        return 0;
    }
    if (!member && !event->local && (event->kind == SNAG_IRC_MESSAGE || event->kind == SNAG_IRC_NOTICE ||
         event->kind == SNAG_IRC_TOPIC) && event->nick[0])
        member = replay_member_set(irc, event, event->nick, event->op);
    if (member && member->op != event->op) return snag_errno(EINVAL);
    if (event->kind == SNAG_IRC_PART || event->kind == SNAG_IRC_QUIT) {
        if (member) replay_member_remove(irc, member);
    } else if (event->kind == SNAG_IRC_NICK) {
        struct irc_replay_member *collision = replay_member_find(
            irc, event->endpoint, event->room, event->text);

        if (collision && collision != member) return snag_errno(EINVAL);
        if (member) memcpy(member->nick, event->text, strlen(event->text) + 1u);
        else (void)replay_member_set(irc, event, event->text, event->op);
    }
    return 0;
}

int
snag_irc_core_restore_event(struct snag_irc_core *irc, const struct snag_irc_event *event)
{
    if (irc && event && event->routed && (event->route.kind != SNAG_IRC_CHANNEL ||
        !snag_irc_event_model_visible(event) || event->route.direction == SNAG_IRC_OUTGOING)) {
        /* Conversation metadata is reconstructed from its session directory.
         * It cannot establish live membership or populate channel replay. */
        json_t *data = snag_irc_event_data(event);
        struct snag_irc_event decoded;
        int rc = snag_irc_event_record_read("irc_event_v2", data, &decoded);
        json_decref(data);
        return rc;
    }
    if (!irc || !event || event->timestamp_ms == 0u || !restored_event_shape_valid(event) ||
        replay_transition(irc, event) < 0) {
        return snag_errno(EINVAL);
    }
    if (snag_irc_core_accept(irc, event) < 0) return -1;
    snag_irc_core_remember(irc, event);
    return 0;
}

json_t *
snag_irc_core_checkpoint(const struct snag_irc_core *irc)
{
    if (!irc) return NULL;
    json_t *out = json_pack("{s:i,s:[],s:[],s:[]}", "v", 1,
        "cursors", "members", "history");
    if (!out) return NULL;
    json_t *cursors = json_object_get(out, "cursors");
    json_t *members = json_object_get(out, "members");
    json_t *history = json_object_get(out, "history");
    for (const struct irc_cursor *c = irc->cursors; c; c = c->next) {
        if (c->sequence > INT64_MAX || json_array_append_new(cursors,
            json_pack("{s:s,s:s,s:s,s:I}", "endpoint", c->endpoint, "room", c->room,
                "stream", c->stream, "sequence", (json_int_t)c->sequence)) < 0) goto fail;
    }
    for (size_t i = 0u; i < irc->replay_member_count; ++i) {
        const struct irc_replay_member *m = &irc->replay_members[i];
        if (json_array_append_new(members, json_pack("{s:s,s:s,s:s,s:b}",
            "endpoint", m->endpoint, "room", m->room, "nick", m->nick, "op", m->op)) < 0)
            goto fail;
    }
    for (size_t i = 0u; i < irc->history_count; ++i)
        if (json_array_append_new(history, snag_irc_event_data(
            &irc->history[(irc->history_start + i) % irc->history_limit])) < 0) goto fail;
    return out;
fail:
    json_decref(out);
    return NULL;
}

static void
checkpoint_replay_free(struct snag_irc_core *irc)
{
    while (irc->cursors) {
        struct irc_cursor *next = irc->cursors->next;
        free(irc->cursors);
        irc->cursors = next;
    }
    free(irc->replay_members);
    free(irc->history);
}

int
snag_irc_core_restore_checkpoint(struct snag_irc_core *irc, const json_t *data)
{
    struct snag_irc_core staged = {0};
    const json_t *version = json_object_get(data, "v");
    const json_t *cursors = json_object_get(data, "cursors");
    const json_t *members = json_object_get(data, "members");
    const json_t *history = json_object_get(data, "history");
    int rc = -1;

    if (!irc || !snag_json_exact_keys(data, "v cursors members history") ||
        !json_is_integer(version) || json_integer_value(version) != 1 ||
        !json_is_array(cursors) || !json_is_array(members) || !json_is_array(history) ||
        json_array_size(history) > SNAG_CONFIG_IRC_HISTORY_MAX ||
        json_array_size(members) > IRC_REPLAY_MEMBERS_MAX) return snag_errno(EINVAL);
    staged.history_limit = irc->history_limit;
    if (staged.history_limit)
        staged.history = calloc(staged.history_limit, sizeof(*staged.history));
    staged.replay_members = calloc(IRC_REPLAY_MEMBERS_MAX, sizeof(*staged.replay_members));
    if ((staged.history_limit && !staged.history) || !staged.replay_members) goto out;
    /* find_cursor prepends: decode backwards to preserve the encoded order. */
    for (size_t i = json_array_size(cursors); i > 0u; --i) {
        const json_t *entry = json_array_get(cursors, i - 1u);
        const char *endpoint = snag_json_string(entry, "endpoint");
        const char *room = snag_json_string(entry, "room");
        const char *stream = snag_json_string(entry, "stream");
        uint64_t sequence;
        if (!snag_json_exact_keys(entry, "endpoint room stream sequence") ||
            !endpoint || !endpoint_valid(endpoint) || !event_field_safe(endpoint) ||
            strlen(endpoint) > SNAG_CONFIG_IRC_ENDPOINT_MAX ||
            !room || !event_field_safe(room) || strlen(room) > SNAG_CONFIG_IRC_ROOM_MAX + 1u ||
            (room[0] && (room[0] != '#' || !room_valid(room))) ||
            !stream || !snag_hex_is_lower(stream, SNAG_ID_HEX_LEN) ||
            snag_json_integer_u64(entry, "sequence", &sequence) < 0 || !sequence ||
            find_cursor(&staged, endpoint, room, false)) goto invalid;
        struct irc_cursor *cursor = find_cursor(&staged, endpoint, room, true);
        if (!cursor) goto out;
        memcpy(cursor->stream, stream, sizeof(cursor->stream));
        cursor->sequence = sequence;
    }
    for (size_t i = 0u; i < json_array_size(members); ++i) {
        const json_t *entry = json_array_get(members, i);
        struct irc_replay_member *member = &staged.replay_members[i];
        const char *endpoint = snag_json_string(entry, "endpoint");
        const char *room = snag_json_string(entry, "room");
        const char *nick = snag_json_string(entry, "nick");
        if (!snag_json_exact_keys(entry, "endpoint room nick op") ||
            !endpoint || !endpoint_valid(endpoint) || !event_field_safe(endpoint) ||
            !room || room[0] != '#' || !room_valid(room) || !nick || !nick_valid(nick) ||
            !event_field_safe(room) || !event_field_safe(nick) ||
            !json_is_boolean(json_object_get(entry, "op")) ||
            replay_member_find(&staged, endpoint, room, nick) ||
            !snag_strcpy(member->endpoint, sizeof(member->endpoint), endpoint) ||
            !snag_strcpy(member->room, sizeof(member->room), room) ||
            !snag_strcpy(member->nick, sizeof(member->nick), nick)) goto invalid;
        member->op = json_is_true(json_object_get(entry, "op"));
        ++staged.replay_member_count;
    }
    for (size_t i = 0u; i < json_array_size(history); ++i) {
        struct snag_irc_event event;
        if (snag_irc_event_read(json_array_get(history, i), &event) < 0 ||
            !restored_event_shape_valid(&event) || !event_remembered(event.kind)) goto invalid;
        if (event.stream[0]) {
            const struct irc_cursor *cursor = find_cursor(
                &staged, event.endpoint, event.room, false);
            if (!cursor || (!strcmp(cursor->stream, event.stream) &&
                event.sequence > cursor->sequence)) goto invalid;
            for (size_t j = 0u; j < i; ++j) {
                const json_t *previous = json_array_get(history, j);
                const char *stream = snag_json_string(previous, "stream");
                if (stream && !strcmp(stream, event.stream) &&
                    json_integer_value(json_object_get(previous, "sequence")) ==
                        (json_int_t)event.sequence) goto invalid;
            }
        }
        /* Membership transitions preceding this bounded ring live in members. */
        snag_irc_core_remember(&staged, &event);
    }

    /* Every allocation and validation has succeeded. Network ownership stays. */
    checkpoint_replay_free(irc);
    irc->cursors = staged.cursors;
    irc->replay_members = staged.replay_members;
    irc->replay_member_count = staged.replay_member_count;
    irc->history = staged.history;
    irc->history_count = staged.history_count;
    irc->history_start = staged.history_start;
    staged.cursors = NULL;
    staged.replay_members = NULL;
    staged.history = NULL;
    if (irc->hosting) {
        const struct irc_cursor *own = find_cursor(irc, irc->listen, irc->room, false);
        if (own) {
            memcpy(irc->stream, own->stream, sizeof(irc->stream));
            irc->sequence = own->sequence;
        }
    }
    rc = 0;
    goto out;
invalid:
    errno = EINVAL;
out:
    checkpoint_replay_free(&staged);
    return rc;
}

const char *
snag_irc_core_model_nick(const struct snag_irc_core *irc)
{
    return irc ? irc->conns[LINK_AGENT].accepted_nick : NULL;
}

const char *
snag_irc_core_operator_nick(const struct snag_irc_core *irc)
{
    return irc ? irc->conns[LINK_OPERATOR].accepted_nick : NULL;
}
