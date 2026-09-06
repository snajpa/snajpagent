/* SPDX-License-Identifier: GPL-2.0-only */
#include "irc_internal.h"
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
    LINK_AGENT,
    LINK_OPERATOR
};

struct snag_irc_core;

struct irc_member {
    char nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    bool op;
};

struct irc_replay_member {
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    bool op;
};

struct irc_conn {
    struct snag_irc_core *owner;
    snag_socket fd;
    struct snag_buf output;
    struct snag_buf pending;
    size_t output_offset;
    size_t pending_inflight;
    unsigned char input[SNAG_IRC_LINE_MAX];
    size_t input_len;
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char user[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char accepted_nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char previous_room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char topic[513u];
    struct irc_member members[IRC_MEMBERS_MAX];
    size_t member_count;
    size_t nick_suffix;
    uint64_t retry_at_ms;
    enum link_role role;
    bool used;
    bool outgoing;
    bool connecting;
    bool registered;
    bool cap_active;
    bool cap_end;
    bool cap_batch;
    bool cap_server_time;
    bool agent_role;
    bool joined;
    bool op;
    bool historical;
    bool names_active;
};

struct irc_message {
    char *tags;
    char *prefix;
    char *command;
    char *params[15u];
    size_t param_count;
};

struct snag_irc_core {
    uint64_t route_revision;
    snag_socket listener;
    bool hosting;
    char listen[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char server_name[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char model_nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char operator_nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    bool model_nick_implicit;
    bool operator_nick_implicit;
    char room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char topic[513u];
    struct irc_conn *peers;
    struct irc_conn *links;
    size_t link_count;
    struct snag_irc_event *history;
    struct irc_replay_member *replay_members;
    size_t replay_member_count;
    size_t history_limit;
    size_t history_start;
    size_t history_count;
    snag_irc_event_fn event_fn;
    snag_irc_trace_fn trace_fn;
    void *event_opaque;
    bool agent_op;
    bool operator_op;
    bool callback_failed;
};

static size_t utf8_chunk(const char *text, size_t len, size_t max);
static size_t chat_chunk(const char *text, size_t len);
static int
sanitize_text(char *dst, size_t size, const char *src)
{
    size_t used = 0u;

    if (!dst || !size || !src) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; src[i];) {
        unsigned char c = (unsigned char)src[i];

        if (c == 0x03u) {
            ++i;
            for (unsigned int n = 0u; n < 2u && src[i] >= '0' &&
                 src[i] <= '9'; ++n, ++i) {
            }
            if (src[i] == ',') {
                ++i;
                for (unsigned int n = 0u; n < 2u && src[i] >= '0' &&
                     src[i] <= '9'; ++n, ++i) {
                }
            }
            continue;
        }
        if (c < 0x20u || c == 0x7fu) {
            ++i;
            continue;
        }
        if (c == 0xc2u && (unsigned char)src[i + 1u] >= 0x80u &&
            (unsigned char)src[i + 1u] <= 0x9fu) {
            i += 2u;
            continue;
        }
        {
            size_t bytes = 1u;
            if (c >= 0xc2u && c <= 0xdfu)
                bytes = 2u;
            else if (c >= 0xe0u && c <= 0xefu)
                bytes = 3u;
            else if (c >= 0xf0u && c <= 0xf4u)
                bytes = 4u;
            if (used > size - 1u || bytes > size - 1u - used) {
                errno = EOVERFLOW;
                return -1;
            }
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

        if (ac != bc || !ac || !bc)
            return (ac > bc) - (ac < bc);
    }
}

static bool
identifier_unicode_unsafe(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    size_t remaining = strlen(text);

    while (remaining) {
        uint32_t cp;
        size_t bytes = snag_utf8_decode(p, remaining, &cp);

        if (!bytes)
            return true;

        if (cp == 0x0085u || cp == 0x00a0u || cp == 0x00adu ||
            cp == 0x061cu || cp == 0x1680u || cp == 0x180eu ||
            (cp >= 0x2000u && cp <= 0x200fu) ||
            (cp >= 0x2028u && cp <= 0x202fu) ||
            (cp >= 0x2060u && cp <= 0x206fu) || cp == 0x3000u ||
            cp == 0xfeffu || (cp >= 0xfff9u && cp <= 0xfffbu))
            return true;
        p += bytes;
        remaining -= bytes;
    }
    return false;
}

static bool
nick_valid(const char *nick)
{
    size_t len = strlen(nick);

    if (!len || len > SNAG_CONFIG_IRC_NICK_MAX ||
        ((nick[0] >= '0' && nick[0] <= '9') || nick[0] == '-') ||
        !snag_utf8_valid((const unsigned char *)nick, len, true) ||
        identifier_unicode_unsafe(nick))
        return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)nick[i];

        if (c < 0x80u && !snag_irc_nick_char(c))
            return false;
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
        if (!len || preferred[len - 1u] != '0') {
            errno = EINVAL;
            return -1;
        }
        --len;
    }
    n = snprintf(suffix, sizeof(suffix), "%zu", number);
    if (n < 0 || (size_t)n >= sizeof(suffix) ||
        (size_t)n > SNAG_CONFIG_IRC_NICK_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    if (len + (size_t)n > SNAG_CONFIG_IRC_NICK_MAX) {
        len = SNAG_CONFIG_IRC_NICK_MAX - (size_t)n;
        while (len && ((unsigned char)preferred[len] & 0xc0u) == 0x80u)
            --len;
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
        !snag_utf8_valid((const unsigned char *)room, strlen(room), true) ||
        identifier_unicode_unsafe(p))
        return false;
    for (; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x21u || c == 0x7fu || c == ',' || c == ':' || c == '#')
            return false;
    }
    return true;
}

static int
normalize_room(char *dst, size_t size, const char *room)
{
    if (!room_valid(room)) {
        errno = EINVAL;
        return -1;
    }
    if (room[0] == '#')
        return snag_strcpy(dst, size, room) ? 0 : -1;
    if (strlen(room) + 2u > size) {
        errno = EINVAL;
        return -1;
    }
    dst[0] = '#';
    memcpy(dst + 1u, room, strlen(room) + 1u);
    return 0;
}

static int
split_endpoint(const char *endpoint, char *host, size_t host_size,
               char *port, size_t port_size)
{
    const char *host_begin = endpoint;
    const char *host_end;
    const char *port_begin = IRC_DEFAULT_PORT;
    size_t host_len;
    unsigned long parsed = 0u;

    if (!endpoint || !*endpoint || strlen(endpoint) > SNAG_CONFIG_IRC_ENDPOINT_MAX)
        goto invalid;
    if (endpoint[0] == '[') {
        host_begin = endpoint + 1u;
        host_end = strchr(host_begin, ']');
        if (!host_end || host_end == host_begin)
            goto invalid;
        if (host_end[1] == ':')
            port_begin = host_end + 2u;
        else if (host_end[1] != '\0')
            goto invalid;
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
    if (!host_len || host_len >= host_size || !*port_begin ||
        strlen(port_begin) >= port_size ||
        !snag_utf8_valid((const unsigned char *)host_begin, host_len, true))
        goto invalid;
    for (size_t i = 0; port_begin[i]; ++i) {
        unsigned char c = (unsigned char)port_begin[i];
        if (c < '0' || c > '9')
            goto invalid;
        parsed = parsed * 10u + (unsigned long)(c - '0');
        if (parsed > 65535u)
            goto invalid;
    }
    if (!parsed)
        goto invalid;
    for (size_t i = 0; i < host_len; ++i) {
        unsigned char c = (unsigned char)host_begin[i];
        if (c <= 0x20u || c == 0x7fu || c == ',' || c == '/')
            goto invalid;
    }
    memcpy(host, host_begin, host_len);
    host[host_len] = '\0';
    if (identifier_unicode_unsafe(host))
        goto invalid;
    memcpy(port, port_begin, strlen(port_begin) + 1u);
    return 0;
invalid:
    errno = EINVAL;
    return -1;
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
config_copy(char *dst, size_t size, const char *src, const char *what,
            char *error, size_t error_size)
{
    if (!src || !*src || !snag_strcpy(dst, size, src)) {
        snag_errorf(error, error_size, "%s exceeds its supported bound", what);
        return -1;
    }
    return 0;
}

bool
snag_irc_enabled(const struct snag_config *config)
{
    return config &&
        (config->irc.listen_explicit || config->irc.client_count != 0u);
}

int
snag_irc_apply_cli(struct snag_config *config, const struct snag_cli *cli,
                  char *error, size_t error_size)
{
    if (!config || !cli) {
        errno = EINVAL;
        return -1;
    }
    if ((cli->irc_no_listen && cli->irc_listen) ||
        (cli->irc_no_client && cli->irc_client_count)) {
        snag_errorf(error, error_size, "conflicting positive and negative IRC role options");
        errno = EINVAL;
        return -1;
    }
    if (cli->irc_no_listen)
        config->irc.listen_explicit = false;
    if (cli->irc_no_client) {
        config->irc.client_count = 0u;
        memset(config->irc.clients, 0, sizeof(config->irc.clients));
    }
    if (cli->irc_listen &&
        config_copy(config->irc.listen, sizeof(config->irc.listen),
                    cli->irc_listen, "IRC listen endpoint", error,
                    error_size) < 0)
        return -1;
    if (cli->irc_listen)
        config->irc.listen_explicit = true;
    if (cli->irc_client_count) {
        memset(config->irc.clients, 0, sizeof(config->irc.clients));
        config->irc.client_count = 0u;
        for (size_t i = 0; i < cli->irc_client_count; ++i) {
            if (config_copy(config->irc.clients[i],
                            sizeof(config->irc.clients[i]),
                            cli->irc_clients[i], "IRC client endpoint",
                            error, error_size) < 0)
                return -1;
            ++config->irc.client_count;
        }
    }
    if (cli->irc_model_nick) {
        if (config_copy(config->irc.model_nick,
                        sizeof(config->irc.model_nick), cli->irc_model_nick,
                        "IRC model nick", error, error_size) < 0)
            return -1;
        config->irc.model_nick_implicit = false;
    }
    if (cli->irc_operator_nick) {
        if (config_copy(config->irc.operator_nick,
                        sizeof(config->irc.operator_nick),
                        cli->irc_operator_nick, "IRC operator nick", error,
                        error_size) < 0)
            return -1;
        config->irc.operator_nick_implicit = false;
    }
    if (cli->irc_room_name &&
        config_copy(config->irc.room_name, sizeof(config->irc.room_name),
                    cli->irc_room_name, "IRC room name", error,
                    error_size) < 0)
        return -1;
    if (snag_irc_enabled(config) && cli->prompt && !cli->prompt_after_dashdash) {
        snag_errorf(error, error_size,
                  "networked initial chat text must follow --");
        errno = EINVAL;
        return -1;
    }
    return snag_irc_normalize(config, error, error_size);
}

int
snag_irc_normalize(struct snag_config *config, char *error, size_t error_size)
{
    const char *login;

    if (!config || config->irc.client_count > SNAG_CONFIG_IRC_CLIENT_MAX) {
        snag_errorf(error, error_size, "invalid IRC configuration");
        errno = EINVAL;
        return -1;
    }
    if (!config->irc.model_nick[0]) {
        if (numbered_nick(config->irc.model_nick, "agent", 0u, false) < 0)
            return -1;
        config->irc.model_nick_implicit = true;
    }
    if (!nick_valid(config->irc.model_nick)) {
        snag_errorf(error, error_size, "IRC model nick is invalid");
        errno = EINVAL;
        return -1;
    }
    if (!endpoint_valid(config->irc.listen)) {
        snag_errorf(error, error_size, "invalid IRC listen endpoint");
        return -1;
    }
    for (size_t i = 0; i < config->irc.client_count; ++i) {
        if (!endpoint_valid(config->irc.clients[i])) {
            snag_errorf(error, error_size, "invalid IRC client endpoint: %s",
                      config->irc.clients[i]);
            return -1;
        }
        for (size_t j = 0; j < i; ++j)
            if (snag_irc_endpoint_equal(config->irc.clients[i],
                               config->irc.clients[j])) {
                snag_errorf(error, error_size,
                          "duplicate IRC client endpoint: %s",
                          config->irc.clients[i]);
                errno = EINVAL;
                return -1;
            }
    }
    if (!config->irc.operator_nick[0]) {
        login = getenv("USER");
        if (!login || !nick_valid(login))
            login = "operator";
        if (numbered_nick(config->irc.operator_nick, login, 0u, false) < 0)
            return -1;
        if (irc_casecmp(config->irc.operator_nick,
                        config->irc.model_nick) == 0 &&
            numbered_nick(config->irc.operator_nick,
                          "localop", 0u, false) < 0)
            return -1;
        config->irc.operator_nick_implicit = true;
    }
    if (!nick_valid(config->irc.operator_nick) ||
        irc_casecmp(config->irc.operator_nick, config->irc.model_nick) == 0) {
        snag_errorf(error, error_size,
                  "IRC operator and model nicks must be valid and distinct");
        errno = EINVAL;
        return -1;
    }
    if (config->irc.room_name[0]) {
        char normalized[sizeof(config->irc.room_name)];
        if (normalize_room(normalized, sizeof(normalized),
                           config->irc.room_name) < 0) {
            snag_errorf(error, error_size, "invalid IRC room name");
            return -1;
        }
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

    if (split_endpoint(endpoint, host, sizeof(host), port, sizeof(port)) < 0) {
        snag_errorf(error, error_size, "invalid IRC listen endpoint");
        return -1;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    gai = snag_socket_addresses(host, port, &hints, &addresses);
    if (gai != 0) {
        snag_errorf(error, error_size, "cannot resolve IRC listen endpoint: %s",
                  gai_strerror(gai));
        errno = EADDRNOTAVAIL;
        return -1;
    }
    for (it = addresses; it; it = it->ai_next) {
        fd = snag_socket_open(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == SNAG_SOCKET_INVALID) {
            saved = errno;
            continue;
        }
        if (snag_socket_reuse(fd) == 0 &&
            snag_socket_bind(fd, it->ai_addr, it->ai_addrlen) == 0 &&
            snag_socket_listen(fd, 32) == 0)
            break;
        saved = errno;
        (void)snag_socket_close(fd);
        fd = SNAG_SOCKET_INVALID;
        if (saved == EADDRINUSE)
            break;
    }
    freeaddrinfo(addresses);
    if (fd == SNAG_SOCKET_INVALID) {
        snag_errorf(error, error_size, "cannot listen on IRC endpoint %s: %s",
                  endpoint, strerror(saved));
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
    snag_buf_init(&conn->output, IRC_OUTPUT_MAX);
    snag_buf_init(&conn->pending, IRC_PENDING_MAX);
}

static void
conn_release(struct irc_conn *conn)
{
    if (conn->fd != SNAG_SOCKET_INVALID)
        (void)snag_socket_close(conn->fd);
    snag_buf_free(&conn->output);
    snag_buf_free(&conn->pending);
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
    if (n < 0 || (size_t)n > IRC_LINE_MAX) {
        errno = EMSGSIZE;
        return -1;
    }
    if (conn->owner && conn->owner->trace_fn &&
        conn->owner->trace_fn(conn->owner->event_opaque, 6u, '>',
            conn->outgoing ? conn->endpoint : conn->owner->listen,
            line, (size_t)n) < 0) {
        conn->owner->callback_failed = true;
        return -1;
    }
    if (conn->output_offset) {
        memmove(conn->output.data,
                conn->output.data + conn->output_offset,
                conn->output.len - conn->output_offset);
        conn->output.len -= conn->output_offset;
        conn->output_offset = 0u;
    }
    if ((size_t)n + 2u > conn->output.max - conn->output.len) {
        errno = EOVERFLOW;
        return -1;
    }
    if (snag_buf_append(&conn->output, line, (size_t)n) < 0 ||
        snag_buf_append(&conn->output, "\r\n", 2u) < 0)
        return -1;
    return 0;
}

static int
flush_conn(struct irc_conn *conn)
{
    while (conn->output_offset < conn->output.len) {
        ssize_t written = snag_socket_send(conn->fd,
            conn->output.data + conn->output_offset,
            conn->output.len - conn->output_offset);

        if (written > 0) {
            conn->output_offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        return -1;
    }
    if (conn->pending_inflight) {
        if (conn->pending_inflight > conn->pending.len) {
            errno = EPROTO;
            return -1;
        }
        memmove(conn->pending.data,
                conn->pending.data + conn->pending_inflight,
                conn->pending.len - conn->pending_inflight);
        conn->pending.len -= conn->pending_inflight;
        conn->pending_inflight = 0u;
    }
    snag_buf_reset(&conn->output);
    conn->output_offset = 0u;
    return 0;
}

static bool
event_remembered(enum snag_irc_event_kind kind)
{
    return kind == SNAG_IRC_JOIN || kind == SNAG_IRC_PART ||
           kind == SNAG_IRC_QUIT || kind == SNAG_IRC_NICK ||
           kind == SNAG_IRC_MESSAGE || kind == SNAG_IRC_NOTICE ||
           kind == SNAG_IRC_TOPIC || kind == SNAG_IRC_MODE;
}

void
snag_irc_core_remember(struct snag_irc_core *irc, const struct snag_irc_event *event)
{
    size_t index;

    if (!irc->history_limit || !event_remembered(event->kind))
        return;
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
emit_event(struct snag_irc_core *irc, const struct snag_irc_event *event,
           bool remember)
{
    int rc;

    if (remember)
        snag_irc_core_remember(irc, event);
    rc = irc->event_fn ? irc->event_fn(irc->event_opaque, event) : 0;
    if (rc < 0)
        irc->callback_failed = true;
    return rc;
}

static void
event_init(struct snag_irc_core *irc, struct snag_irc_event *event,
           enum snag_irc_event_kind kind, const char *endpoint,
           const char *room, const char *nick, const char *text,
           bool op, bool historical, bool local)
{
    memset(event, 0, sizeof(*event));
    event->kind = kind;
    event->timestamp_ms = snag_time_ms();
    if (endpoint)
        (void)snprintf(event->endpoint, sizeof(event->endpoint), "%s", endpoint);
    if (room)
        (void)snprintf(event->room, sizeof(event->room), "%s", room);
    if (nick)
        (void)snprintf(event->nick, sizeof(event->nick), "%s", nick);
    if (text && sanitize_text(event->text, sizeof(event->text), text) < 0)
        event->text[0] = '\0';
    event->op = op;
    event->historical = historical;
    event->local = local;
    (void)irc;
}

static bool
cap_has(const char *text, const char *cap)
{
    size_t cap_len = strlen(cap);

    while (*text) {
        while (*text == ' ')
            ++text;
        if (strncmp(text, cap, cap_len) == 0 &&
            (text[cap_len] == '\0' || text[cap_len] == ' '))
            return true;
        while (*text && *text != ' ')
            ++text;
    }
    return false;
}

static int
parse_message(char *line, struct irc_message *message)
{
    char *p = line;

    memset(message, 0, sizeof(*message));
    if (*p == '@') {
        message->tags = p + 1u;
        p = strchr(p, ' ');
        if (!p)
            return -1;
        *p++ = '\0';
        while (*p == ' ')
            ++p;
    }
    if (*p == ':') {
        message->prefix = ++p;
        p = strchr(p, ' ');
        if (!p)
            return -1;
        *p++ = '\0';
        while (*p == ' ')
            ++p;
    }
    if (!*p)
        return -1;
    message->command = p;
    p = strchr(p, ' ');
    if (p) {
        *p++ = '\0';
        while (*p == ' ')
            ++p;
    }
    for (char *c = message->command; *c; ++c)
        if (*c >= 'a' && *c <= 'z')
            *c = (char)(*c - ('a' - 'A'));
    while (p && *p && message->param_count < 15u) {
        if (*p == ':') {
            message->params[message->param_count++] = p + 1u;
            break;
        }
        message->params[message->param_count++] = p;
        p = strchr(p, ' ');
        if (!p)
            break;
        *p++ = '\0';
        while (*p == ' ')
            ++p;
    }
    return 0;
}

static bool
decimal_field(const char *text, size_t len, unsigned int *value)
{
    unsigned int parsed = 0u;

    for (size_t i = 0u; i < len; ++i) {
        if (text[i] < '0' || text[i] > '9')
            return false;
        parsed = parsed * 10u + (unsigned int)(text[i] - '0');
    }
    *value = parsed;
    return true;
}

static bool
server_time_value(const char *text, size_t len, uint64_t *timestamp_ms)
{
    static const unsigned int month_days[] = {
        31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u
    };
    unsigned int year, month, day, hour, minute, second, millis = 0u;
    unsigned int limit;
    int adjusted_year;
    int era;
    unsigned int year_of_era;
    unsigned int day_of_year;
    unsigned int day_of_era;
    int64_t days;

    if (len < 20u || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[len - 1u] != 'Z' ||
        !decimal_field(text, 4u, &year) ||
        !decimal_field(text + 5u, 2u, &month) ||
        !decimal_field(text + 8u, 2u, &day) ||
        !decimal_field(text + 11u, 2u, &hour) ||
        !decimal_field(text + 14u, 2u, &minute) ||
        !decimal_field(text + 17u, 2u, &second) || year < 1970u ||
        month < 1u || month > 12u || hour > 23u || minute > 59u ||
        second > 60u)
        return false;
    limit = month_days[month - 1u];
    if (month == 2u && (year % 4u == 0u) &&
        (year % 100u != 0u || year % 400u == 0u))
        ++limit;
    if (day < 1u || day > limit)
        return false;
    if (len != 20u) {
        size_t digits = len - 21u;

        if (text[19] != '.' || digits == 0u)
            return false;
        for (size_t i = 0u; i < digits; ++i) {
            if (text[20u + i] < '0' || text[20u + i] > '9')
                return false;
            if (i < 3u)
                millis = millis * 10u +
                         (unsigned int)(text[20u + i] - '0');
        }
        for (size_t i = digits; i < 3u; ++i)
            millis *= 10u;
    }
    adjusted_year = (int)year - (month <= 2u ? 1 : 0);
    era = adjusted_year / 400;
    year_of_era = (unsigned int)(adjusted_year - era * 400);
    day_of_year = (153u * (month > 2u ? month - 3u : month + 9u) + 2u) /
                  5u + day - 1u;
    day_of_era = year_of_era * 365u + year_of_era / 4u -
                 year_of_era / 100u + day_of_year;
    days = (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
    if (days < 0)
        return false;
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
            server_time_value(tags + 5u, len - 5u, &timestamp_ms))
            return timestamp_ms;
        tags = end ? end + 1u : NULL;
    }
    return 0u;
}

static const char *
prefix_nick(const char *prefix, char out[SNAG_CONFIG_IRC_NICK_MAX + 1u])
{
    const char *end;
    size_t len;

    if (!prefix)
        return NULL;
    end = strpbrk(prefix, "!@");
    len = end ? (size_t)(end - prefix) : strlen(prefix);
    if (!len || len > SNAG_CONFIG_IRC_NICK_MAX)
        return NULL;
    memcpy(out, prefix, len);
    out[len] = '\0';
    return nick_valid(out) ? out : NULL;
}

static struct irc_member *
member_find(struct irc_conn *conn, const char *nick)
{
    for (size_t i = 0; i < conn->member_count; ++i)
        if (irc_casecmp(conn->members[i].nick, nick) == 0)
            return &conn->members[i];
    return NULL;
}

static struct irc_member *
member_add(struct irc_conn *conn, const char *nick, bool op)
{
    struct irc_member *member = member_find(conn, nick);

    if (member) {
        member->op = op || member->op;
        return member;
    }
    if (conn->member_count >= IRC_MEMBERS_MAX || !nick_valid(nick))
        return NULL;
    member = &conn->members[conn->member_count++];
    memset(member, 0, sizeof(*member));
    (void)snag_strcpy(member->nick, sizeof(member->nick), nick);
    member->op = op;
    return member;
}

static void
member_remove(struct irc_conn *conn, const char *nick)
{
    for (size_t i = 0; i < conn->member_count; ++i) {
        if (irc_casecmp(conn->members[i].nick, nick) != 0)
            continue;
        memmove(&conn->members[i], &conn->members[i + 1u],
                (conn->member_count - i - 1u) * sizeof(conn->members[0]));
        --conn->member_count;
        return;
    }
}

static bool
server_nick_used(const struct snag_irc_core *irc, const char *nick,
                 const struct irc_conn *except)
{
    if (irc_casecmp(irc->model_nick, nick) == 0 ||
        irc_casecmp(irc->operator_nick, nick) == 0)
        return true;
    for (size_t i = 0; i < IRC_SERVER_PEERS; ++i)
        if (irc->peers[i].used && &irc->peers[i] != except &&
            irc->peers[i].nick[0] &&
            irc_casecmp(irc->peers[i].nick, nick) == 0)
            return true;
    return false;
}

static void server_drop_peer(struct snag_irc_core *irc, struct irc_conn *peer,
                             const char *reason);

static int
server_broadcast(struct snag_irc_core *irc, const struct snag_irc_event *event,
                 const char *user)
{
    static const char *const commands[] = {
        [SNAG_IRC_JOIN] = "JOIN", [SNAG_IRC_PART] = "PART",
        [SNAG_IRC_QUIT] = "QUIT", [SNAG_IRC_NICK] = "NICK",
        [SNAG_IRC_MESSAGE] = "PRIVMSG", [SNAG_IRC_NOTICE] = "NOTICE",
        [SNAG_IRC_TOPIC] = "TOPIC", [SNAG_IRC_MODE] = "MODE"
    };
    char prefix[3u * SNAG_CONFIG_IRC_NICK_MAX + 3u];
    char line[IRC_LINE_MAX + 1u];
    bool channel = event->kind != SNAG_IRC_NICK && event->kind != SNAG_IRC_QUIT;
    int n;

    if ((size_t)event->kind >= sizeof(commands) / sizeof(commands[0]) ||
        !commands[event->kind]) {
        errno = EINVAL;
        return -1;
    }
    n = snprintf(prefix, sizeof(prefix), user ? "%s!%s@%s" : "%s",
                 event->nick, user, irc->server_name);
    if (n < 0 || (size_t)n >= sizeof(prefix))
        return -1;
    n = snprintf(line, sizeof(line), ":%s %s%s%s%s%s", prefix,
                 commands[event->kind], channel ? " " : "",
                 channel ? event->room : "",
                 event->kind == SNAG_IRC_JOIN ? "" :
                 event->kind == SNAG_IRC_MODE ? " " : " :", event->text);
    if (n < 0 || (size_t)n > IRC_LINE_MAX) {
        errno = EMSGSIZE;
        return -1;
    }
    for (size_t i = 0; i < IRC_SERVER_PEERS; ++i) {
        struct irc_conn *peer = &irc->peers[i];

        if (peer->used && peer->joined &&
            queue_line(peer, "%s", line) < 0)
            server_drop_peer(irc, peer, "output queue exceeded");
    }
    return irc->callback_failed ? -1 : 0;
}

static int
server_publish(struct snag_irc_core *irc, enum snag_irc_event_kind kind,
               const char *nick, const char *user, const char *text,
               bool op, bool local)
{
    struct snag_irc_event event;

    event_init(irc, &event, kind, irc->listen, irc->room, nick, text,
               op, false, local);
    if (server_broadcast(irc, &event, user) < 0)
        return -1;
    return emit_event(irc, &event, true);
}

static int
server_send_names(struct snag_irc_core *irc, struct irc_conn *peer)
{
    if (queue_line(peer, ":%s 353 %s = %s :%s%s", irc->server_name,
                   peer->nick, irc->room, irc->agent_op ? "@" : "",
                   irc->model_nick) < 0 ||
        queue_line(peer, ":%s 353 %s = %s :%s%s", irc->server_name,
                   peer->nick, irc->room, irc->operator_op ? "@" : "",
                   irc->operator_nick) < 0)
        return -1;
    for (size_t i = 0; i < IRC_SERVER_PEERS; ++i) {
        struct irc_conn *it = &irc->peers[i];
        if (!it->used || !it->joined)
            continue;
        if (queue_line(peer, ":%s 353 %s = %s :%s%s", irc->server_name,
                       peer->nick, irc->room, it->op ? "@" : "",
                       it->nick) < 0)
            return -1;
    }
    return queue_line(peer, ":%s 366 %s %s :End of NAMES list",
                      irc->server_name, peer->nick, irc->room);
}

static void
format_time(uint64_t timestamp_ms, char out[32u])
{
    time_t seconds = (time_t)(timestamp_ms / 1000u);
    struct tm tm;

    if (!snag_gmtime(&seconds, &tm) ||
        strftime(out, 32u, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0)
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
hosted_history_event(const struct snag_irc_core *irc,
                      const struct snag_irc_event *event)
{
    return strcmp(event->room, irc->room) == 0 &&
        snag_irc_endpoint_equal(event->endpoint, irc->listen);
}

int
snag_irc_core_replay_hosted_history(const struct snag_irc_core *irc,
                              snag_irc_event_fn render, void *opaque)
{
    bool replayed = false;
    if (!irc || !render) {
        errno = EINVAL;
        return -1;
    }
    if (!irc->hosting)
        return 0;
    for (size_t i = 0u; i < irc->history_count; ++i) {
        struct snag_irc_event event =
            irc->history[(irc->history_start + i) % irc->history_limit];

        if (!hosted_history_event(irc, &event))
            continue;
        event.historical = true;
        if (render(opaque, &event) < 0)
            return -1;
        replayed = true;
    }
    /* This callback is display-only: do not append or broadcast the boundary. */
    struct snag_irc_event ready = {.kind = SNAG_IRC_HISTORY_READY};
    return replayed ? render(opaque, &ready) : 0;
}

static int
server_send_history(struct snag_irc_core *irc, struct irc_conn *peer)
{
    char batch_id[17u];

    (void)snprintf(batch_id, sizeof(batch_id), "%08llx",
                   (unsigned long long)(snag_time_ms() & 0xffffffffu));
    if (peer->cap_batch &&
        queue_line(peer, ":%s BATCH +%s chathistory %s",
                   irc->server_name, batch_id, irc->room) < 0)
        return -1;
    for (size_t i = 0; i < irc->history_count; ++i) {
        const struct snag_irc_event *event =
            &irc->history[(irc->history_start + i) % irc->history_limit];
        char when[32u];
        const char *tag = "";
        char tag_buf[96u];

        if (!hosted_history_event(irc, event))
            continue;
        format_time(event->timestamp_ms, when);
        if (peer->cap_batch) {
            (void)snprintf(tag_buf, sizeof(tag_buf),
                           peer->cap_server_time ? "@batch=%s;time=%s " :
                                                   "@batch=%s ",
                           batch_id, when);
            tag = tag_buf;
        } else if (peer->cap_server_time) {
            (void)snprintf(tag_buf, sizeof(tag_buf), "@time=%s ", when);
            tag = tag_buf;
        }
        if (peer->cap_batch && event->kind == SNAG_IRC_MESSAGE) {
            if (queue_line(peer, "%s:%s!user@%s PRIVMSG %s :%s", tag,
                           event->nick, irc->server_name, irc->room,
                           event->text) < 0)
                return -1;
        } else if (peer->cap_batch && event->kind == SNAG_IRC_NOTICE) {
            if (queue_line(peer, "%s:%s!user@%s NOTICE %s :%s", tag,
                           event->nick, irc->server_name, irc->room,
                           event->text) < 0)
                return -1;
        } else if (queue_line(peer,
                   "%s:%s NOTICE %s :[history %s] %s%s%s %s %s", tag,
                   irc->server_name,
                   peer->nick, when, event->op ? "@" : "", event->nick,
                   event->nick[0] ? " " : "", event_kind_text(event->kind),
                   event->text) < 0) {
            return -1;
        }
    }
    if (peer->cap_batch &&
        queue_line(peer, ":%s BATCH -%s", irc->server_name, batch_id) < 0)
        return -1;
    return 0;
}

static int
server_welcome(struct snag_irc_core *irc, struct irc_conn *peer)
{
    if (peer->registered || !peer->nick[0] || !peer->user[0] ||
        (peer->cap_active && !peer->cap_end))
        return 0;
    peer->registered = true;
    if (queue_line(peer, ":%s 001 %s :Welcome to " SNAJPAGENT_NAME " IRC",
                   irc->server_name, peer->nick) < 0 ||
        queue_line(peer, ":%s 005 %s CHANTYPES=# PREFIX=(o)@ SAJROOM=%s "
                   "LINELEN=%u :are supported", irc->server_name, peer->nick,
                   irc->room, SNAG_IRC_LINE_MAX) < 0 ||
        queue_line(peer, ":%s 376 %s :End of MOTD", irc->server_name,
                   peer->nick) < 0)
        return -1;
    return 0;
}

static struct irc_conn *
server_peer_by_nick(struct snag_irc_core *irc, const char *nick)
{
    for (size_t i = 0; i < IRC_SERVER_PEERS; ++i)
        if (irc->peers[i].used && irc->peers[i].nick[0] &&
            irc_casecmp(irc->peers[i].nick, nick) == 0)
            return &irc->peers[i];
    return NULL;
}

static int
server_join(struct snag_irc_core *irc, struct irc_conn *peer, const char *room)
{
    char mode_text[SNAG_CONFIG_IRC_NICK_MAX + 4u];

    if (!peer->registered)
        return queue_line(peer, ":%s 451 * :You have not registered",
                          irc->server_name);
    if (irc_casecmp(room, irc->room) != 0)
        return queue_line(peer, ":%s 403 %s %s :No such channel",
                          irc->server_name, peer->nick, room);
    if (peer->joined)
        return 0;
    peer->joined = true;
    peer->op = false;
    (void)snag_strcpy(peer->room, sizeof(peer->room), irc->room);
    if (server_publish(irc, SNAG_IRC_JOIN, peer->nick, peer->user,
                       "", false, false) < 0)
        return -1;
    peer->op = !peer->agent_role;
    (void)snprintf(mode_text, sizeof(mode_text), "+o %s", peer->nick);
    if (peer->op &&
        server_publish(irc, SNAG_IRC_MODE, irc->server_name, NULL,
                       mode_text, false, false) < 0)
        return -1;
    if (queue_line(peer, ":%s 332 %s %s :%s", irc->server_name,
                   peer->nick, irc->room, irc->topic) < 0 ||
        server_send_names(irc, peer) < 0 || server_send_history(irc, peer) < 0)
        return -1;
    return 0;
}

static int
server_chat(struct snag_irc_core *irc, struct irc_conn *peer,
            enum snag_irc_event_kind kind, const char *target,
            const char *text)
{
    char clean[IRC_LINE_MAX + 1u];

    if (!peer->joined || irc_casecmp(target, irc->room) != 0) {
        if (kind == SNAG_IRC_NOTICE)
            return 0;
        return queue_line(peer, ":%s 404 %s %s :Cannot send to channel",
                          irc->server_name, peer->nick, target);
    }
    if (!*text || strchr(text, '\r') || strchr(text, '\n') ||
        !snag_utf8_valid((const unsigned char *)text, strlen(text), true))
        return kind == SNAG_IRC_NOTICE ? 0 :
            queue_line(peer, ":%s 412 %s :No text to send",
                       irc->server_name, peer->nick);
    if (sanitize_text(clean, sizeof(clean), text) < 0 || !clean[0])
        return kind == SNAG_IRC_NOTICE ? 0 :
            queue_line(peer, ":%s 412 %s :No safe text to send",
                       irc->server_name, peer->nick);
    for (size_t offset = 0u, len = strlen(clean); offset < len;) {
        char chunk[SNAG_IRC_TEXT_MAX + 1u];
        size_t take = chat_chunk(clean + offset, len - offset);

        if (!take)
            return -1;
        memcpy(chunk, clean + offset, take);
        chunk[take] = '\0';
        if (server_publish(irc, kind, peer->nick, peer->user, chunk,
                           peer->op, false) < 0)
            return -1;
        offset += take;
    }
    return 0;
}

static int
server_topic(struct snag_irc_core *irc, struct irc_conn *peer,
             const struct irc_message *message)
{
    const char *topic;
    char clean[sizeof(irc->topic)];

    if (!peer->joined || message->param_count < 1u ||
        irc_casecmp(message->params[0], irc->room) != 0)
        return queue_line(peer, ":%s 442 %s %s :You're not on that channel",
                          irc->server_name, peer->nick, irc->room);
    if (message->param_count == 1u)
        return queue_line(peer, ":%s 332 %s %s :%s", irc->server_name,
                          peer->nick, irc->room, irc->topic);
    if (!peer->op)
        return queue_line(peer, ":%s 482 %s %s :You're not channel operator",
                          irc->server_name, peer->nick, irc->room);
    topic = message->params[1];
    if (strlen(topic) > IRC_TOPIC_MAX || strchr(topic, '\r') ||
        strchr(topic, '\n') ||
        !snag_utf8_valid((const unsigned char *)topic, strlen(topic), true))
        return queue_line(peer, ":%s 417 %s :Topic is too long",
                          irc->server_name, peer->nick);
    if (sanitize_text(clean, sizeof(clean), topic) < 0)
        return queue_line(peer, ":%s 417 %s :Topic is too long",
                          irc->server_name, peer->nick);
    memcpy(irc->topic, clean, strlen(clean) + 1u);
    return server_publish(irc, SNAG_IRC_TOPIC, peer->nick, peer->user,
                          irc->topic, peer->op, false);
}

static int
server_mode(struct snag_irc_core *irc, struct irc_conn *peer,
            const struct irc_message *message)
{
    struct irc_conn *target;
    bool *local_target = NULL;
    bool add;
    char mode_text[SNAG_CONFIG_IRC_NICK_MAX + 4u];

    if (message->param_count < 1u ||
        irc_casecmp(message->params[0], irc->room) != 0)
        return queue_line(peer, ":%s 403 %s * :No such channel",
                          irc->server_name, peer->nick);
    if (message->param_count == 1u)
        return queue_line(peer, ":%s 324 %s %s +t", irc->server_name,
                          peer->nick, irc->room);
    if (!peer->op)
        return queue_line(peer, ":%s 482 %s %s :You're not channel operator",
                          irc->server_name, peer->nick, irc->room);
    if (message->param_count < 3u ||
        (strcmp(message->params[1], "+o") != 0 &&
         strcmp(message->params[1], "-o") != 0))
        return queue_line(peer, ":%s 472 %s :Only +o and -o are supported",
                          irc->server_name, peer->nick);
    target = server_peer_by_nick(irc, message->params[2]);
    if (irc_casecmp(message->params[2], irc->model_nick) == 0)
        local_target = &irc->agent_op;
    else if (irc_casecmp(message->params[2], irc->operator_nick) == 0)
        local_target = &irc->operator_op;
    if ((!target || !target->joined) && !local_target)
        return queue_line(peer, ":%s 441 %s %s %s :They aren't on that channel",
                          irc->server_name, peer->nick, message->params[2],
                          irc->room);
    add = message->params[1][0] == '+';
    if (local_target)
        *local_target = add;
    else
        target->op = add;
    (void)snprintf(mode_text, sizeof(mode_text), "%s %s",
                   message->params[1], message->params[2]);
    return server_publish(irc, SNAG_IRC_MODE, peer->nick, peer->user,
                          mode_text, peer->op, false);
}

static int
server_who(struct snag_irc_core *irc, struct irc_conn *peer)
{
    if (queue_line(peer, ":%s 352 %s %s agent %s %s %s H%s :0 "
                   SNAJPAGENT_NAME,
                   irc->server_name, peer->nick, irc->room,
                   irc->server_name, irc->server_name, irc->model_nick,
                   irc->agent_op ? "@" : "") < 0 ||
        queue_line(peer, ":%s 352 %s %s operator %s %s %s H%s :0 operator",
                   irc->server_name, peer->nick, irc->room,
                   irc->server_name, irc->server_name, irc->operator_nick,
                   irc->operator_op ? "@" : "") < 0)
        return -1;
    for (size_t i = 0u; i < IRC_SERVER_PEERS; ++i) {
        struct irc_conn *member = &irc->peers[i];

        if (member->used && member->joined &&
            queue_line(peer, ":%s 352 %s %s %s %s %s %s H%s :0 IRC user",
                       irc->server_name, peer->nick, irc->room, member->user,
                       irc->server_name, irc->server_name, member->nick,
                       member->op ? "@" : "") < 0)
            return -1;
    }
    return queue_line(peer, ":%s 315 %s %s :End of WHO list",
                      irc->server_name, peer->nick, irc->room);
}

static int
server_dispatch(struct snag_irc_core *irc, struct irc_conn *peer, char *line)
{
    struct irc_message message;
    char trace[96u];

    if (parse_message(line, &message) < 0)
        return 0;
    if (irc->trace_fn) {
        int n = snprintf(trace, sizeof(trace), "%s params=%zu",
                         message.command, message.param_count);
        if (n < 0 || (size_t)n >= sizeof(trace) ||
            irc->trace_fn(irc->event_opaque, 5u, '<', irc->listen,
                          trace, (size_t)n) < 0) {
            irc->callback_failed = true;
            return -1;
        }
    }
    if (strcmp(message.command, "CAP") == 0) {
        const char *sub = message.param_count ? message.params[0] : "";
        const char *caps = message.param_count > 1u ? message.params[1] : "";
        if (strcmp(sub, "LS") == 0) {
            peer->cap_active = true;
            return queue_line(peer,
                ":%s CAP * LS :batch server-time draft/chathistory "
                SNAJPAGENT_NAME "/agent",
                irc->server_name);
        }
        if (strcmp(sub, "REQ") == 0) {
            peer->cap_batch = cap_has(caps, "batch");
            peer->cap_server_time = cap_has(caps, "server-time");
            peer->agent_role = cap_has(caps, SNAJPAGENT_NAME "/agent");
            return queue_line(peer, ":%s CAP * ACK :%s", irc->server_name,
                              caps);
        }
        if (strcmp(sub, "END") == 0) {
            peer->cap_end = true;
            return server_welcome(irc, peer);
        }
        return 0;
    }
    if (strcmp(message.command, "NICK") == 0) {
        const char *next = message.param_count ? message.params[0] : "";
        char old[sizeof(peer->nick)];
        if (!nick_valid(next))
            return queue_line(peer, ":%s 432 * %s :Erroneous nickname",
                              irc->server_name, next);
        if (server_nick_used(irc, next, peer))
            return queue_line(peer, ":%s 433 * %s :Nickname is already in use",
                              irc->server_name, next);
        if (strcmp(peer->nick, next) == 0)
            return 0;
        memcpy(old, peer->nick, sizeof(old));
        (void)snag_strcpy(peer->nick, sizeof(peer->nick), next);
        if (old[0] && peer->joined) {
            if (server_publish(irc, SNAG_IRC_NICK, old, peer->user,
                               peer->nick, peer->op, false) < 0)
                return -1;
        } else if (peer->registered &&
                   queue_line(peer, ":%s!%s@%s NICK :%s", old,
                              peer->user, irc->server_name, peer->nick) < 0) {
            return -1;
        }
        return server_welcome(irc, peer);
    }
    if (strcmp(message.command, "USER") == 0) {
        if (message.param_count < 1u || !nick_valid(message.params[0]))
            return queue_line(peer, ":%s 461 * USER :Not enough parameters",
                              irc->server_name);
        (void)snag_strcpy(peer->user, sizeof(peer->user), message.params[0]);
        if (message.param_count >= 4u &&
            strcmp(message.params[3], SNAJPAGENT_NAME " agent") == 0)
            peer->agent_role = true;
        return server_welcome(irc, peer);
    }
    if (strcmp(message.command, "PING") == 0)
        return queue_line(peer, ":%s PONG %s :%s", irc->server_name,
                          irc->server_name,
                          message.param_count ? message.params[0] :
                                                irc->server_name);
    if (strcmp(message.command, "PONG") == 0)
        return 0;
    if (strcmp(message.command, "QUIT") == 0)
        return 1;
    if (strcmp(message.command, "JOIN") == 0)
        return message.param_count ? server_join(irc, peer, message.params[0]) :
            queue_line(peer, ":%s 461 %s JOIN :Not enough parameters",
                       irc->server_name, peer->nick);
    if (strcmp(message.command, "PART") == 0) {
        if (peer->joined && message.param_count &&
            irc_casecmp(message.params[0], irc->room) == 0) {
            const char *reason = message.param_count > 1u ? message.params[1] : "";
            char clean[SNAG_IRC_TEXT_MAX + 1u];
            if (sanitize_text(clean, sizeof(clean), reason) < 0)
                clean[0] = '\0';
            if (server_publish(irc, SNAG_IRC_PART, peer->nick, peer->user,
                               clean, peer->op, false) < 0)
                return -1;
            peer->joined = false;
        }
        return 0;
    }
    if (strcmp(message.command, "PRIVMSG") == 0 ||
        strcmp(message.command, "NOTICE") == 0) {
        if (message.param_count < 2u)
            return 0;
        return server_chat(irc, peer,
            strcmp(message.command, "NOTICE") == 0 ? SNAG_IRC_NOTICE :
                                                     SNAG_IRC_MESSAGE,
            message.params[0], message.params[1]);
    }
    if (strcmp(message.command, "TOPIC") == 0)
        return server_topic(irc, peer, &message);
    if (strcmp(message.command, "MODE") == 0)
        return server_mode(irc, peer, &message);
    if ((strcmp(message.command, "NAMES") == 0 ||
         strcmp(message.command, "WHO") == 0) && !peer->registered)
        return queue_line(peer, ":%s 451 * :You have not registered",
                          irc->server_name);
    if (strcmp(message.command, "NAMES") == 0)
        return server_send_names(irc, peer);
    if (strcmp(message.command, "WHO") == 0)
        return server_who(irc, peer);
    if (peer->registered)
        return queue_line(peer, ":%s 421 %s %s :Unknown command",
                          irc->server_name, peer->nick, message.command);
    return 0;
}

static void
server_drop_peer(struct snag_irc_core *irc, struct irc_conn *peer,
                 const char *reason)
{
    char nick[sizeof(peer->nick)];
    char user[sizeof(peer->user)];
    bool announced = peer->used && peer->joined && peer->nick[0];
    bool op = peer->op;

    (void)snprintf(nick, sizeof(nick), "%s", peer->nick);
    (void)snprintf(user, sizeof(user), "%s", peer->user);
    conn_release(peer);
    if (announced)
        (void)server_publish(irc, SNAG_IRC_QUIT, nick, user, reason, op, false);
}

static int
client_handshake(struct snag_irc_core *irc, struct irc_conn *link)
{
    const char *role_cap = link->role == LINK_AGENT ?
                           " " SNAJPAGENT_NAME "/agent" : "";
    const char *role_text = link->role == LINK_AGENT ? "agent" : "operator";

    (void)irc;
    link->connecting = false;
    link->registered = false;
    link->joined = false;
    link->historical = false;
    link->member_count = 0u;
    link->op = false;
    link->room[0] = '\0';
    link->topic[0] = '\0';
    if (queue_line(link, "CAP LS 302") < 0 ||
        queue_line(link, "CAP REQ :batch server-time draft/chathistory%s",
                   role_cap) < 0 ||
        queue_line(link, "NICK %s", link->nick) < 0 ||
        queue_line(link, "USER %s 0 * :" SNAJPAGENT_NAME " %s", link->nick,
                   role_text) < 0 ||
        queue_line(link, "CAP END") < 0)
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

    if (split_endpoint(link->endpoint, host, sizeof(host), port,
                       sizeof(port)) < 0)
        return -1;
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
        if (link->fd == SNAG_SOCKET_INVALID)
            continue;
        snag_socket_nodelay(link->fd);
        rc = snag_socket_connect(link->fd, it->ai_addr, it->ai_addrlen);
        if (rc == 0) {
            freeaddrinfo(addresses);
            return client_handshake(irc, link);
        }
        if (errno == EINPROGRESS) {
            link->connecting = true;
            freeaddrinfo(addresses);
            return 0;
        }
        (void)snag_socket_close(link->fd);
        link->fd = SNAG_SOCKET_INVALID;
    }
    freeaddrinfo(addresses);
    return 1;
}

static bool
link_emit_enabled(const struct irc_conn *link)
{
    return link->role == LINK_OPERATOR;
}

static int
link_retry_nick(struct snag_irc_core *irc, struct irc_conn *link)
{
    const char *preferred = link->role == LINK_AGENT ? irc->model_nick :
                                                       irc->operator_nick;
    bool implicit = link->role == LINK_AGENT ? irc->model_nick_implicit :
                                               irc->operator_nick_implicit;

    if (link->nick_suffix == SIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    if (numbered_nick(link->nick, preferred, ++link->nick_suffix,
                      implicit) < 0)
        return -1;
    return queue_line(link, "NICK %s", link->nick);
}

static int
link_queue_pending(struct irc_conn *link, enum snag_irc_event_kind kind,
                   const char *text)
{
    unsigned char marker = kind == SNAG_IRC_NOTICE ? 'N' : 'M';
    size_t before = link->pending.len;

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

    if (link->pending_inflight || link->output.len)
        return 0;
    while (offset < link->pending.len) {
        unsigned char *lf = memchr(link->pending.data + offset, '\n',
                                   link->pending.len - offset);
        size_t len;
        const char *command;
        char text[SNAG_IRC_TEXT_MAX + 1u];

        if (!lf) {
            errno = EPROTO;
            return -1;
        }
        len = (size_t)(lf - (link->pending.data + offset));
        if (len < 2u || len > SNAG_IRC_TEXT_MAX + 1u ||
            (link->pending.data[offset] != 'M' &&
             link->pending.data[offset] != 'N')) {
            errno = EPROTO;
            return -1;
        }
        command = link->pending.data[offset] == 'N' ? "NOTICE" : "PRIVMSG";
        memcpy(text, link->pending.data + offset + 1u, len - 1u);
        text[len - 1u] = '\0';
        if (queue_line(link, "%s %s :%s", command, link->room, text) < 0) {
            if (errno == EOVERFLOW && offset)
                break;
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

    if (!link_emit_enabled(link))
        return 0;
    event_init(irc, &event, kind, link->endpoint, room, nick, text, op,
               link->historical, false);
    if (timestamp_ms)
        event.timestamp_ms = timestamp_ms;
    if (kind == SNAG_IRC_TOPIC &&
        !snag_strcpy(link->topic, sizeof(link->topic), event.text))
        return 1;
    return emit_event(irc, &event, true);
}

static int
client_dispatch(struct snag_irc_core *irc, struct irc_conn *link, char *line)
{
    struct irc_message message;
    char nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char trace[96u];
    const char *sender;
    uint64_t timestamp_ms;

    if (parse_message(line, &message) < 0)
        return 0;
    timestamp_ms = server_time_tag(message.tags);
    if (irc->trace_fn) {
        int n = snprintf(trace, sizeof(trace), "%s params=%zu",
                         message.command, message.param_count);
        if (n < 0 || (size_t)n >= sizeof(trace) ||
            irc->trace_fn(irc->event_opaque, 5u, '<', link->endpoint,
                          trace, (size_t)n) < 0) {
            irc->callback_failed = true;
            return -1;
        }
    }
    if (strcmp(message.command, "PING") == 0)
        return queue_line(link, "PONG :%s",
                          message.param_count ? message.params[0] :
                                                SNAJPAGENT_NAME);
    if (strcmp(message.command, "001") == 0) {
        if (link->registered || !message.param_count ||
            !nick_valid(message.params[0]))
            return 0;
        (void)snag_strcpy(link->nick, sizeof(link->nick), message.params[0]);
        (void)snag_strcpy(link->accepted_nick, sizeof(link->accepted_nick),
                         link->nick);
        link->registered = true;
        return link_emit(irc, link, SNAG_IRC_CONNECTED, "", link->nick,
                         "", false, timestamp_ms);
    }
    if (strcmp(message.command, "005") == 0) {
        for (size_t i = 1u; i < message.param_count; ++i)
            if (strncmp(message.params[i], "SAJROOM=", 8u) == 0 &&
                room_valid(message.params[i] + 8u)) {
                (void)normalize_room(link->room, sizeof(link->room),
                                     message.params[i] + 8u);
                if (link->previous_room[0] && strcmp(link->previous_room, link->room)) {
                    ++irc->route_revision;
                    snag_buf_reset(&link->pending);
                    link->pending_inflight = 0u;
                    if (link_emit(irc, link, SNAG_IRC_NOTICE, link->room, SNAJPAGENT_NAME,
                            "room changed; old queued messages discarded", false, 0u) < 0)
                        return -1;
                }
                memcpy(link->previous_room, link->room, sizeof(link->previous_room));
            }
        return 0;
    }
    if (strcmp(message.command, "376") == 0 ||
        strcmp(message.command, "422") == 0) {
        if (!link->room[0])
            return 1;
        return queue_line(link, "JOIN %s", link->room);
    }
    if (strcmp(message.command, "BATCH") == 0 && message.param_count) {
        if (message.params[0][0] == '+')
            link->historical = true;
        else if (message.params[0][0] == '-') {
            link->historical = false;
            return link_emit(irc, link, SNAG_IRC_HISTORY_READY, link->room,
                             "", "", false, timestamp_ms);
        }
        return 0;
    }
    if (strcmp(message.command, "353") == 0 && message.param_count >= 4u) {
        char *cursor = message.params[3];

        if (!link->room[0] ||
            irc_casecmp(message.params[2], link->room) != 0)
            return 0;

        if (!link->names_active) {
            link->member_count = 0u;
            link->names_active = true;
        }
        while (*cursor) {
            char *end;
            bool op;
            while (*cursor == ' ')
                ++cursor;
            if (!*cursor)
                break;
            end = strchr(cursor, ' ');
            if (end)
                *end = '\0';
            op = *cursor == '@';
            if (op)
                ++cursor;
            (void)member_add(link, cursor, op);
            if (!end)
                break;
            cursor = end + 1u;
        }
        return 0;
    }
    if (strcmp(message.command, "366") == 0 && message.param_count >= 2u &&
        link->room[0] && irc_casecmp(message.params[1], link->room) == 0) {
        struct irc_member *self = member_find(link, link->nick);
        link->op = self && self->op;
        link->names_active = false;
        return 0;
    }
    if (strcmp(message.command, "332") == 0 && message.param_count >= 3u &&
        link->room[0] && irc_casecmp(message.params[1], link->room) == 0)
        return link_emit(irc, link, SNAG_IRC_TOPIC, message.params[1],
                         "", message.params[2], false, timestamp_ms);
    sender = prefix_nick(message.prefix, nick);
    if (strcmp(message.command, "JOIN") == 0 && sender) {
        const char *room = message.param_count ? message.params[0] : link->room;
        bool self = irc_casecmp(sender, link->nick) == 0;
        struct irc_member *member;

        if (!link->room[0] || irc_casecmp(room, link->room) != 0)
            return self ? 1 : 0;
        member = member_add(link, sender, false);
        if (self) {
            link->joined = true;
            (void)snag_strcpy(link->room, sizeof(link->room), room);
            if (link_flush_pending(link) < 0)
                return -1;
        }
        return link_emit(irc, link, SNAG_IRC_JOIN, room, sender, "",
                         member ? member->op : false, timestamp_ms);
    }
    if (strcmp(message.command, "PART") == 0 && sender) {
        const char *room = message.param_count ? message.params[0] : link->room;
        struct irc_member *member = member_find(link, sender);
        bool op = member && member->op;
        const char *reason = message.param_count > 1u ? message.params[1] : "";

        if (!link->room[0] || irc_casecmp(room, link->room) != 0)
            return 0;
        member_remove(link, sender);
        if (irc_casecmp(sender, link->nick) == 0)
            link->joined = false;
        return link_emit(irc, link, SNAG_IRC_PART, room, sender, reason, op,
                         timestamp_ms);
    }
    if (strcmp(message.command, "QUIT") == 0 && sender) {
        struct irc_member *member = member_find(link, sender);
        bool op = member && member->op;
        const char *reason = message.param_count ? message.params[0] : "";

        if (!member)
            return 0;
        member_remove(link, sender);
        return link_emit(irc, link, SNAG_IRC_QUIT, link->room, sender, reason,
                         op, timestamp_ms);
    }
    if (strcmp(message.command, "NICK") == 0 && sender && message.param_count) {
        struct irc_member *member = member_find(link, sender);
        bool op = member && member->op;
        bool self = irc_casecmp(sender, link->nick) == 0;

        if (!nick_valid(message.params[0]) ||
            strcmp(sender, message.params[0]) == 0)
            return 0;
        /* Either role may hear its partner's rename before the self ack. */
        if (!link->historical && (member || self))
            for (size_t i = 0u; i < irc->link_count; ++i) {
                struct irc_conn *own = &irc->links[i];
                if (own->registered &&
                    snag_irc_endpoint_equal(own->endpoint, link->endpoint) &&
                    irc_casecmp(own->nick, sender) == 0) {
                    (void)snag_strcpy(own->nick, sizeof(own->nick),
                                     message.params[0]);
                    (void)snag_strcpy(own->accepted_nick,
                                     sizeof(own->accepted_nick), own->nick);
                }
            }
        if (!member)
            return 0;
        (void)snag_strcpy(member->nick, sizeof(member->nick), message.params[0]);
        return link_emit(irc, link, SNAG_IRC_NICK, link->room, sender,
                         message.params[0], op, timestamp_ms);
    }
    if (strcmp(message.command, "MODE") == 0 && sender &&
        message.param_count >= 3u &&
        link->room[0] && irc_casecmp(message.params[0], link->room) == 0 &&
        nick_valid(message.params[2]) &&
        (strcmp(message.params[1], "+o") == 0 ||
         strcmp(message.params[1], "-o") == 0)) {
        struct irc_member *target = member_add(link, message.params[2], false);
        struct irc_member *actor = member_find(link, sender);
        char mode_text[SNAG_CONFIG_IRC_NICK_MAX + 4u];
        if (target)
            target->op = message.params[1][0] == '+';
        if (irc_casecmp(message.params[2], link->nick) == 0)
            link->op = message.params[1][0] == '+';
        (void)snprintf(mode_text, sizeof(mode_text), "%s %s",
                       message.params[1], message.params[2]);
        return link_emit(irc, link, SNAG_IRC_MODE, message.params[0], sender,
                         mode_text, actor && actor->op, timestamp_ms);
    }
    if (strcmp(message.command, "TOPIC") == 0 && sender &&
        message.param_count >= 2u && link->room[0] &&
        irc_casecmp(message.params[0], link->room) == 0) {
        struct irc_member *member = member_find(link, sender);
        return link_emit(irc, link, SNAG_IRC_TOPIC, message.params[0], sender,
                         message.params[1], member && member->op,
                         timestamp_ms);
    }
    if ((strcmp(message.command, "PRIVMSG") == 0 ||
         strcmp(message.command, "NOTICE") == 0) && sender &&
        message.param_count >= 2u && link->joined && link->room[0] &&
        irc_casecmp(message.params[0], link->room) == 0) {
        struct irc_member *member;
        for (size_t i = 0u; i < irc->link_count; ++i)
            if (!link->historical && irc->links[i].joined &&
                snag_irc_endpoint_equal(irc->links[i].endpoint, link->endpoint) &&
                irc_casecmp(sender, irc->links[i].nick) == 0)
                return 0;
        if (strlen(message.params[1]) > SNAG_IRC_TEXT_MAX)
            return 1;
        member = member_find(link, sender);
        return link_emit(irc, link,
            strcmp(message.command, "NOTICE") == 0 ? SNAG_IRC_NOTICE :
                                                     SNAG_IRC_MESSAGE,
            message.params[0], sender, message.params[1], member && member->op,
            timestamp_ms);
    }
    if (strcmp(message.command, "433") == 0 && !link->registered)
        return link_retry_nick(irc, link);
    if (strcmp(message.command, "ERROR") == 0 ||
        strcmp(message.command, "403") == 0 ||
        strcmp(message.command, "404") == 0)
        return 1;
    return 0;
}

static void
link_disconnect(struct snag_irc_core *irc, struct irc_conn *link,
                const char *reason)
{
    struct snag_irc_event event;

    if (link->fd != SNAG_SOCKET_INVALID)
        (void)snag_socket_close(link->fd);
    link->fd = SNAG_SOCKET_INVALID;
    link->connecting = false;
    link->registered = false;
    link->joined = false;
    link->input_len = 0u;
    link->member_count = 0u;
    link->op = false;
    link->names_active = false;
    snag_buf_reset(&link->output);
    link->output_offset = 0u;
    link->pending_inflight = 0u;
    link->retry_at_ms = snag_monotonic_ms() + IRC_RETRY_MS;
    if (link_emit_enabled(link)) {
        event_init(irc, &event, SNAG_IRC_DISCONNECTED, link->endpoint,
                   link->room, link->accepted_nick, reason,
                   link->op, false, false);
        (void)emit_event(irc, &event, false);
    }
}

static int
read_conn(struct snag_irc_core *irc, struct irc_conn *conn)
{
    for (unsigned int batch = 0u; batch < 4u; ++batch) {
        ssize_t got;
        size_t available = sizeof(conn->input) - conn->input_len;

        if (!available)
            return 1;
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

            if (!lf)
                break;
            wire_len = (size_t)(lf - conn->input) + 1u;
            len = wire_len - 1u;
            if (len && conn->input[len - 1u] == '\r')
                --len;
            if (len > IRC_LINE_MAX ||
                !snag_utf8_valid(conn->input, len, true))
                return 1;
            memcpy(line, conn->input, len);
            line[len] = '\0';
            memmove(conn->input, conn->input + wire_len,
                    conn->input_len - wire_len);
            conn->input_len -= wire_len;
            if (irc->trace_fn &&
                irc->trace_fn(irc->event_opaque, 6u, '<',
                    conn->outgoing ? conn->endpoint : irc->listen,
                    line, len) < 0) {
                irc->callback_failed = true;
                return -1;
            }
            rc = conn->outgoing ? client_dispatch(irc, conn, line) :
                                  server_dispatch(irc, conn, line);
            if (rc != 0)
                return rc;
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
            if (errno == EINTR)
                continue;
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        }
        for (size_t i = 0; i < IRC_SERVER_PEERS; ++i)
            if (!irc->peers[i].used) {
                peer = &irc->peers[i];
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
    if (snag_socket_connected(link->fd) < 0) {
        return 1;
    }
    return client_handshake(irc, link);
}

static int
start_due_links(struct snag_irc_core *irc)
{
    uint64_t now = snag_monotonic_ms();

    for (size_t i = 0; i < irc->link_count; ++i) {
        struct irc_conn *link = &irc->links[i];
        int rc;

        if (link->fd != SNAG_SOCKET_INVALID || now < link->retry_at_ms)
            continue;
        rc = start_link(irc, link);
        if (rc < 0)
            return -1;
        if (rc > 0)
            link_disconnect(irc, link, "connect failed; retrying");
    }
    return 0;
}

static void
derive_server_name(char out[SNAG_CONFIG_IRC_NICK_MAX + 1u])
{
    char host[256u];
    size_t used = 0u;

    if (gethostname(host, sizeof(host)) < 0)
        memcpy(host, "localhost", 10u);
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

    if (gethostname(host, sizeof(host)) < 0)
        memcpy(host, "localhost", 10u);
    host[sizeof(host) - 1u] = '\0';
    out[0] = '#';
    for (size_t i = 0u; host[i] && used < SNAG_CONFIG_IRC_ROOM_MAX; ++i) {
        unsigned char c = (unsigned char)host[i];
        out[++used] = c > 0x20u && c < 0x7fu && c != ',' && c != ':' &&
                      c != '#' ? (char)c : '_';
    }
    if (!used)
        memcpy(out, "#localhost", 11u);
    else
        out[used + 1u] = '\0';
}

int
snag_irc_core_open(struct snag_irc_core **out, const struct snag_config *config,
             const char *workspace, bool network, snag_irc_event_fn event_fn,
             snag_irc_trace_fn trace_fn, void *event_opaque,
             char *error, size_t error_size)
{
    struct snag_irc_core *irc;

    if (!out || !config || !workspace || (network && !snag_irc_enabled(config))) {
        errno = EINVAL;
        snag_errorf(error, error_size, "invalid IRC startup state");
        return -1;
    }
    *out = NULL;
    irc = calloc(1u, sizeof(*irc));
    if (!irc)
        return -1;
    if (snag_network_init() < 0) {
        free(irc);
        return -1;
    }
    irc->listener = SNAG_SOCKET_INVALID;
    irc->hosting = config->irc.listen_explicit;
    irc->event_fn = event_fn;
    irc->trace_fn = trace_fn;
    irc->event_opaque = event_opaque;
    irc->operator_op = true;
    irc->history_limit = config->irc.history_lines;
    irc->history = calloc(irc->history_limit, sizeof(*irc->history));
    if (irc->hosting)
        irc->peers = calloc(IRC_SERVER_PEERS, sizeof(*irc->peers));
    if (config->irc.client_count)
        irc->links = calloc(config->irc.client_count * 2u, sizeof(*irc->links));
    irc->replay_members = calloc(IRC_REPLAY_MEMBERS_MAX,
                                 sizeof(*irc->replay_members));
    if ((irc->history_limit && !irc->history) || !irc->replay_members ||
        (irc->hosting && !irc->peers) ||
        (config->irc.client_count && !irc->links) ||
        !snag_strcpy(irc->listen, sizeof(irc->listen), config->irc.listen) ||
        !snag_strcpy(irc->model_nick, sizeof(irc->model_nick),
                    config->irc.model_nick) ||
        !snag_strcpy(irc->operator_nick, sizeof(irc->operator_nick),
                    config->irc.operator_nick))
        goto fail;
    irc->model_nick_implicit = config->irc.model_nick_implicit;
    irc->operator_nick_implicit = config->irc.operator_nick_implicit;
    derive_server_name(irc->server_name);
    if (config->irc.room_name[0]) {
        if (normalize_room(irc->room, sizeof(irc->room),
                           config->irc.room_name) < 0)
            goto fail;
    } else {
        derive_room(irc->room);
    }
    if (strlen(workspace) > IRC_TOPIC_MAX ||
        sanitize_text(irc->topic, sizeof(irc->topic), workspace) < 0) {
        snag_errorf(error, error_size, "IRC launch path is too long for a topic");
        errno = ENAMETOOLONG;
        goto fail;
    }
    for (size_t i = 0; irc->peers && i < IRC_SERVER_PEERS; ++i)
        irc->peers[i].fd = SNAG_SOCKET_INVALID;
    for (size_t i = 0; i < config->irc.client_count; ++i) {
        if (config->irc.listen_explicit &&
            snag_irc_endpoint_equal(config->irc.clients[i], config->irc.listen))
            continue;
        for (size_t role = 0; role < 2u; ++role) {
            struct irc_conn *link = &irc->links[irc->link_count++];
            conn_init(link, irc);
            link->used = true;
            link->outgoing = true;
            link->role = role == 0u ? LINK_AGENT : LINK_OPERATOR;
            link->agent_role = role == 0u;
            (void)snag_strcpy(link->endpoint, sizeof(link->endpoint),
                              config->irc.clients[i]);
            (void)snag_strcpy(link->nick, sizeof(link->nick),
                              role == 0u ? irc->model_nick : irc->operator_nick);
            (void)snag_strcpy(link->accepted_nick,
                             sizeof(link->accepted_nick), link->nick);
        }
    }
    if (network && irc->hosting) {
        irc->listener = open_listener(irc->listen, error, error_size);
        if (irc->listener == SNAG_SOCKET_INVALID)
            goto fail;
    }
    *out = irc;
    return 0;
fail:
    if (error_size && !error[0])
        snag_errorf(error, error_size, "cannot initialize IRC state");
    snag_irc_core_close(irc);
    return -1;
}

void
snag_irc_core_close(struct snag_irc_core *irc)
{
    if (!irc)
        return;
    if (irc->listener != SNAG_SOCKET_INVALID)
        (void)snag_socket_close(irc->listener);
    for (size_t i = 0; irc->peers && i < IRC_SERVER_PEERS; ++i)
        if (irc->peers[i].used)
            conn_release(&irc->peers[i]);
    for (size_t i = 0; i < irc->link_count; ++i)
        conn_release(&irc->links[i]);
    free(irc->history);
    free(irc->replay_members);
    free(irc->peers);
    free(irc->links);
    snag_network_free();
    free(irc);
}

size_t
snag_irc_core_pending(const struct snag_irc_core *irc)
{
    size_t bytes = 0u;

    for (size_t i = 0u; i < irc->link_count; ++i)
        bytes += irc->links[i].pending.len + irc->links[i].output.len -
                 irc->links[i].output_offset;
    for (size_t i = 0u; irc->peers && i < IRC_SERVER_PEERS; ++i)
        if (irc->peers[i].used)
            bytes += irc->peers[i].output.len - irc->peers[i].output_offset;
    return bytes;
}

int
snag_irc_core_copy_history(struct snag_irc_core *dst, const struct snag_irc_core *src,
                           bool hosted_only)
{
    for (size_t i = 0u; i < src->history_count; ++i) {
        const struct snag_irc_event *event =
            &src->history[(src->history_start + i) % src->history_limit];

        /* These are already admitted records; a bounded tail need not contain
         * all the membership transitions required for replay validation. */
        if (!hosted_only || hosted_history_event(dst, event))
            snag_irc_core_remember(dst, event);
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
    snag_socket_event fds[2u + IRC_SERVER_PEERS + SNAG_CONFIG_IRC_CLIENT_MAX * 2u];
    struct irc_conn *owners[sizeof(fds) / sizeof(fds[0])];
    size_t count = 1u;
    int polled;

    if (!irc) {
        errno = EINVAL;
        return -1;
    }
    if (start_due_links(irc) < 0)
        goto fail;
    for (size_t i = 0u; i < irc->link_count; ++i) {
        uint64_t now = snag_monotonic_ms();
        const struct irc_conn *link = &irc->links[i];
        int retry;

        if (link->fd != SNAG_SOCKET_INVALID)
            continue;
        retry = link->retry_at_ms > now ? (int)(link->retry_at_ms - now) : 0;
        if (timeout_ms < 0 || retry < timeout_ms)
            timeout_ms = retry;
    }
    fds[0] = (snag_socket_event){wake_fd, SNAG_NET_READ, 0};
    if (irc->listener != SNAG_SOCKET_INVALID) {
        fds[count].fd = irc->listener;
        fds[count].events = SNAG_NET_READ;
        fds[count].revents = 0;
        owners[count++] = NULL;
    }
    for (size_t i = 0; irc->peers && i < IRC_SERVER_PEERS; ++i) {
        struct irc_conn *peer = &irc->peers[i];
        if (!peer->used || peer->fd == SNAG_SOCKET_INVALID)
            continue;
        fds[count].fd = peer->fd;
        fds[count].events = SNAG_NET_READ |
            (peer->output_offset < peer->output.len ? SNAG_NET_WRITE : 0);
        fds[count].revents = 0;
        owners[count++] = peer;
    }
    for (size_t i = 0; i < irc->link_count; ++i) {
        struct irc_conn *link = &irc->links[i];
        if (link->fd == SNAG_SOCKET_INVALID)
            continue;
        if (link->joined && !link->output.len && link->pending.len &&
            link_flush_pending(link) < 0) {
            link_disconnect(irc, link, "outbound queue failed; retrying");
            if (irc->callback_failed)
                goto fail;
            continue;
        }
        fds[count].fd = link->fd;
        fds[count].events = SNAG_NET_READ |
            (link->connecting || link->output_offset < link->output.len ?
             SNAG_NET_WRITE : 0);
        fds[count].revents = 0;
        owners[count++] = link;
    }
    do {
        polled = snag_socket_poll(fds, count, timeout_ms);
    } while (polled < 0 && errno == EINTR);
    if (polled < 0)
        goto fail;
    for (size_t i = 1u; i < count; ++i) {
        struct irc_conn *conn = owners[i];
        int rc = 0;

        if (!fds[i].revents)
            continue;
        if (!conn) {
            if (accept_peers(irc) < 0)
                goto fail;
            continue;
        }
        if (conn->outgoing && conn->connecting &&
            (fds[i].revents & (SNAG_NET_WRITE | SNAG_NET_ERROR | SNAG_NET_HUP))) {
            rc = complete_link_connect(irc, conn);
            if (rc < 0)
                goto fail;
            if (rc > 0) {
                link_disconnect(irc, conn, "connect failed; retrying");
                continue;
            }
        }
        if (conn->fd != SNAG_SOCKET_INVALID && (fds[i].revents & SNAG_NET_READ))
            rc = read_conn(irc, conn);
        if (rc < 0) {
            if (irc->callback_failed)
                goto fail;
            if (conn->outgoing)
                link_disconnect(irc, conn, "protocol error; retrying");
            else
                server_drop_peer(irc, conn, "malformed or oversized input");
            if (irc->callback_failed)
                goto fail;
            continue;
        }
        if (rc > 0 || (fds[i].revents & (SNAG_NET_ERROR | SNAG_NET_HUP | SNAG_NET_INVALID))) {
            if (conn->outgoing)
                link_disconnect(irc, conn, "connection closed; retrying");
            else
                server_drop_peer(irc, conn, "connection closed");
            continue;
        }
        if (conn->fd != SNAG_SOCKET_INVALID && conn->output_offset < conn->output.len &&
            flush_conn(conn) < 0) {
            if (conn->outgoing)
                link_disconnect(irc, conn, "write failed; retrying");
            else
                server_drop_peer(irc, conn, "write failed");
            if (irc->callback_failed)
                goto fail;
        }
    }
    return 0;
fail:
    snag_errorf(error, error_size, "IRC event loop failed: %s", strerror(errno));
    return -1;
}

static size_t
utf8_chunk(const char *text, size_t len, size_t max)
{
    size_t chunk = len < max ? len : max;

    while (chunk && chunk < len &&
           (((unsigned char)text[chunk] & 0xc0u) == 0x80u))
        --chunk;
    return chunk;
}

static bool
role_is_op(const struct snag_irc_core *irc, enum link_role role)
{
    if (irc->listener != SNAG_SOCKET_INVALID &&
        (role == LINK_AGENT ? irc->agent_op : irc->operator_op))
        return true;
    for (size_t i = 0u; i < irc->link_count; ++i)
        if (irc->links[i].role == role && irc->links[i].joined &&
            irc->links[i].op)
            return true;
    return false;
}

static size_t
chat_chunk(const char *text, size_t len)
{
    size_t take = utf8_chunk(text, len, SNAG_IRC_TEXT_MAX);

    if (take < len)
        for (size_t i = take; i > 0u; --i)
            if (text[i - 1u] == ' ')
                return i;
    return take;
}

static int
send_chat_line(struct snag_irc_core *irc, const char *nick, enum link_role role,
               enum snag_irc_event_kind kind,
               const char *text, size_t len)
{
    char clean[SNAG_IRC_TEXT_MAX + 1u];
    struct snag_irc_event event;
    const char *room = irc->listener != SNAG_SOCKET_INVALID ? irc->room : "";

    if (!len || len > SNAG_IRC_TEXT_MAX)
        return 0;
    {
        char raw[SNAG_IRC_TEXT_MAX + 1u];
        memcpy(raw, text, len);
        raw[len] = '\0';
        if (sanitize_text(clean, sizeof(clean), raw) < 0)
            return -1;
    }
    if (!clean[0])
        return 0;
    event_init(irc, &event, kind, irc->hosting ? irc->listen :
               irc->links[0].endpoint, room,
               nick, clean, role_is_op(irc, role), false, true);
    if (irc->listener != SNAG_SOCKET_INVALID &&
        server_broadcast(irc, &event, "local") < 0)
        return -1;
    for (size_t i = 0; i < irc->link_count; ++i) {
        struct irc_conn *link = &irc->links[i];
        if (link->role != role)
            continue;
        if (!event.room[0] && link->room[0])
            (void)snag_strcpy(event.room, sizeof(event.room), link->room);
        if (link_queue_pending(link, kind, clean) < 0 ||
            (link->joined && link_flush_pending(link) < 0)) {
            return -1;
        }
    }
    return emit_event(irc, &event, true);
}

static int
send_chat(struct snag_irc_core *irc, const char *nick, enum link_role role,
          enum snag_irc_event_kind kind, const char *text,
          char *error, size_t error_size)
{
    const char *cursor = text;
    size_t remaining;

    if (!irc || !text || !*text ||
        !snag_utf8_valid((const unsigned char *)text, strlen(text), true)) {
        errno = EINVAL;
        snag_errorf(error, error_size, "IRC chat must be nonempty valid UTF-8");
        return -1;
    }
    remaining = strlen(cursor);
    while (remaining) {
        const char *newline = memchr(cursor, '\n', remaining);
        size_t line_len = newline ? (size_t)(newline - cursor) : remaining;

        while (line_len) {
            size_t chunk = chat_chunk(cursor, line_len);
            if (!chunk || send_chat_line(irc, nick, role, kind,
                                         cursor, chunk) < 0)
                goto fail;
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
fail:
    snag_errorf(error, error_size, "cannot queue IRC chat: %s", strerror(errno));
    return -1;
}

static int
set_topic_as(struct snag_irc_core *irc, const char *topic, enum link_role role,
             char *error, size_t error_size)
{
    char clean[sizeof(irc->topic)];
    size_t destinations = 0u;

    if (!irc || !topic || strlen(topic) > IRC_TOPIC_MAX ||
        strchr(topic, '\r') || strchr(topic, '\n') ||
        !snag_utf8_valid((const unsigned char *)topic, strlen(topic), true)) {
        snag_errorf(error, error_size, "IRC topic is invalid or too long");
        errno = EINVAL;
        return -1;
    }
    if (sanitize_text(clean, sizeof(clean), topic) < 0) {
        snag_errorf(error, error_size, "IRC topic is invalid or too long");
        return -1;
    }
    if (irc->listener != SNAG_SOCKET_INVALID &&
        (role == LINK_AGENT ? irc->agent_op : irc->operator_op)) {
        memcpy(irc->topic, clean, strlen(clean) + 1u);
        if (server_publish(irc, SNAG_IRC_TOPIC,
                           role == LINK_AGENT ? irc->model_nick :
                                                irc->operator_nick,
                           "local", clean, true, true) < 0)
            goto fail;
        ++destinations;
    }
    for (size_t i = 0; i < irc->link_count; ++i) {
        struct irc_conn *link = &irc->links[i];
        if (link->role != role || !link->joined || !link->op)
            continue;
        if (queue_line(link, "TOPIC %s :%s", link->room, clean) < 0)
            goto fail;
        ++destinations;
    }
    if (!destinations) {
        snag_errorf(error, error_size,
                  role == LINK_AGENT ?
                  "agent identity is not an operator in any joined room" :
                  "operator identity is not an operator in any joined room");
        errno = EACCES;
        return -1;
    }
    return 0;
fail:
    snag_errorf(error, error_size, "cannot queue IRC topic change");
    return -1;
}

int
snag_irc_core_send(struct snag_irc_core *irc, bool model,
                   enum snag_irc_event_kind kind, const char *text,
                   char *error, size_t error_size)
{
    enum link_role role = model ? LINK_AGENT : LINK_OPERATOR;

    if (kind == SNAG_IRC_TOPIC)
        return set_topic_as(irc, text, role, error, error_size);
    return send_chat(irc, model ? snag_irc_core_model_nick(irc) :
                                 snag_irc_core_operator_nick(irc),
                     role, kind, text, error, error_size);
}

static int
snapshot_member(struct snag_buf *out, struct snag_buf *nicks, const char *nick, bool op)
{
    return snag_buf_printf(out, " %s%s", op ? "@" : "", nick) < 0 ||
           snag_buf_printf(nicks, "%s\n", nick) < 0 ? -1 : 0;
}

static int
snapshot_network(const struct snag_irc_core *irc, struct snag_buf *out,
                 struct snag_buf *nicks)
{
    if (irc->hosting) {
        if (snag_buf_printf(out, "room: %s\ntopic: %s\n",
                           irc->room, irc->topic) < 0)
            goto fail;
        if (snag_buf_printf(out, "members[%s]:", irc->listen) < 0 ||
            snapshot_member(out, nicks, irc->model_nick, irc->agent_op) < 0 ||
            snapshot_member(out, nicks, irc->operator_nick, irc->operator_op) < 0)
            goto fail;
        for (size_t i = 0; i < IRC_SERVER_PEERS; ++i) {
            const struct irc_conn *peer = &irc->peers[i];
            if (peer->used && peer->joined &&
                snapshot_member(out, nicks, peer->nick, peer->op) < 0)
                goto fail;
        }
        if (snag_buf_append(out, "\n", 1u) < 0)
            goto fail;
    }
    for (size_t i = 0; i < irc->link_count; ++i) {
        const struct irc_conn *link = &irc->links[i];
        const struct irc_conn *agent = NULL;

        if (link->role != LINK_OPERATOR)
            continue;
        for (size_t j = 0u; j < irc->link_count; ++j)
            if (irc->links[j].role == LINK_AGENT &&
                snag_irc_endpoint_equal(irc->links[j].endpoint, link->endpoint)) {
                agent = &irc->links[j];
                break;
            }
        if (snag_buf_printf(out, "endpoint[%s]: %s%s%s\n",
                           link->endpoint,
                           link->joined ? "joined " :
                           link->connecting ? "connecting" : "disconnected",
                           link->joined ? link->room : "",
                           link->joined && link->op ? " as operator" : "") < 0)
            goto fail;
        if (snag_buf_printf(out, "aliases[%s]: model %s operator %s\n",
                           link->endpoint, agent ? agent->accepted_nick : "",
                           link->accepted_nick) < 0)
            goto fail;
        if (link->joined) {
            if (snag_buf_printf(out, "topic[%s]: %s\nmembers[%s]:",
                               link->endpoint, link->topic,
                               link->endpoint) < 0)
                goto fail;
            for (size_t j = 0; j < link->member_count; ++j)
                if (snapshot_member(out, nicks, link->members[j].nick,
                                    link->members[j].op) < 0)
                    goto fail;
            if (snag_buf_append(out, "\n", 1u) < 0)
                goto fail;
        }
    }
    return 0;
fail:
    return -1;
}

int
snag_irc_core_view(const struct snag_irc_core *irc, struct snag_irc_view *view)
{
    struct snag_buf text, nicks;
    int rc = -1;

    memset(view, 0, sizeof(*view));
    view->revision = irc->route_revision + 1u;
    (void)snag_strcpy(view->model, sizeof(view->model),
                      snag_irc_core_model_nick(irc));
    (void)snag_strcpy(view->operator, sizeof(view->operator),
                      snag_irc_core_operator_nick(irc));
    view->joined = irc->hosting || (irc->link_count && irc->links[0].joined);
    if (irc->hosting || irc->link_count)
        (void)snag_strcpy(view->room, sizeof(view->room),
                        irc->hosting ? irc->room : irc->links[0].room[0] ?
                        irc->links[0].room : irc->links[0].previous_room);
    snag_buf_init(&text, sizeof(view->text) - 1u);
    snag_buf_init(&nicks, sizeof(view->nicks) - 1u);
    if (snapshot_network(irc, &text, &nicks) == 0) {
        memcpy(view->text, text.data, text.len);
        if (nicks.len)
            memcpy(view->nicks, nicks.data, nicks.len);
        rc = 0;
    }
    snag_buf_free(&text);
    snag_buf_free(&nicks);
    return rc;
}

int
snag_irc_core_history(const struct snag_irc_core *irc, struct snag_buf *out)
{
    if (snag_buf_append(out, "history:\n", 9u) < 0)
        goto fail;
    for (size_t i = 0; i < irc->history_count; ++i) {
        const struct snag_irc_event *event =
            &irc->history[(irc->history_start + i) % irc->history_limit];
        char when[32u];
        format_time(event->timestamp_ms, when);
        if (event->kind == SNAG_IRC_MESSAGE || event->kind == SNAG_IRC_NOTICE) {
            if (snag_buf_printf(out, "%s %s %s%s: %s\n", when,
                               event->endpoint, event->op ? "@" : "",
                               event->nick, event->text) < 0)
                goto fail;
        } else if (snag_buf_printf(out, "%s %s * %s%s %s %s\n", when,
                                  event->endpoint, event->op ? "@" : "",
                                  event->nick, event_kind_text(event->kind),
                                  event->text) < 0) {
            goto fail;
        }
    }
    if (snag_buf_append(out, "[end IRC room snapshot]", 23u) < 0)
        goto fail;
    return 0;
fail:
    return -1;
}

static bool
event_field_safe(const char *text)
{
    size_t len = strlen(text);

    if (!snag_utf8_valid((const unsigned char *)text, len, true))
        return false;
    for (size_t i = 0u; i < len; ++i)
        if ((unsigned char)text[i] < 0x20u ||
            (unsigned char)text[i] == 0x7fu)
            return false;
    return true;
}

static bool
restored_event_shape_valid(const struct snag_irc_event *event)
{
    bool room = event->room[0] == '#' && room_valid(event->room);
    bool nick = nick_valid(event->nick);
    bool remembered = event_remembered(event->kind);

    if ((unsigned int)event->kind > (unsigned int)SNAG_IRC_HISTORY_READY ||
        !endpoint_valid(event->endpoint) ||
        !event_field_safe(event->endpoint) ||
        !event_field_safe(event->room) ||
        !event_field_safe(event->nick) ||
        !event_field_safe(event->text) ||
        (event->historical && (event->local || !remembered)) ||
        (event->local && event->kind != SNAG_IRC_MESSAGE &&
         event->kind != SNAG_IRC_NOTICE && event->kind != SNAG_IRC_TOPIC))
        return false;
    switch (event->kind) {
    case SNAG_IRC_CONNECTED:
        return nick && !event->room[0] && !event->text[0] && !event->op &&
               !event->historical && !event->local;
    case SNAG_IRC_DISCONNECTED:
        return nick && (!event->room[0] || room) && event->text[0] &&
               !event->historical && !event->local;
    case SNAG_IRC_HISTORY_READY:
        return room && !event->nick[0] && !event->text[0] && !event->op &&
               !event->historical && !event->local;
    case SNAG_IRC_JOIN:
        return room && nick && !event->text[0];
    case SNAG_IRC_PART: case SNAG_IRC_QUIT:
        return room && nick;
    case SNAG_IRC_NICK:
        return room && nick && nick_valid(event->text);
    case SNAG_IRC_MESSAGE: case SNAG_IRC_NOTICE:
        return (room || (event->local && !event->room[0])) && nick &&
               event->text[0];
    case SNAG_IRC_TOPIC:
        return room && (!event->nick[0] || nick) &&
               (event->nick[0] || !event->op);
    case SNAG_IRC_MODE:
        return room && nick &&
               ((strcmp(event->text, "+o") == 0 ||
                 strcmp(event->text, "-o") == 0) ||
                ((strncmp(event->text, "+o ", 3u) == 0 ||
                  strncmp(event->text, "-o ", 3u) == 0) &&
                 nick_valid(event->text + 3u)));
    }
    return false;
}

static struct irc_replay_member *
replay_member_find(struct snag_irc_core *irc, const char *endpoint,
                   const char *room, const char *nick)
{
    for (size_t i = 0u; i < irc->replay_member_count; ++i) {
        struct irc_replay_member *member = &irc->replay_members[i];

        if (strcmp(member->endpoint, endpoint) == 0 &&
            irc_casecmp(member->room, room) == 0 &&
            irc_casecmp(member->nick, nick) == 0)
            return member;
    }
    return NULL;
}

static struct irc_replay_member *
replay_member_set(struct snag_irc_core *irc, const struct snag_irc_event *event,
                  const char *nick, bool op)
{
    struct irc_replay_member *member = replay_member_find(
        irc, event->endpoint, event->room, nick);

    if (!member) {
        if (irc->replay_member_count == IRC_REPLAY_MEMBERS_MAX)
            irc->replay_member_count = 0u;
        member = &irc->replay_members[irc->replay_member_count++];
        memset(member, 0, sizeof(*member));
        (void)snprintf(member->endpoint, sizeof(member->endpoint), "%s",
                       event->endpoint);
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

    memmove(member, member + 1u,
            (irc->replay_member_count - index - 1u) * sizeof(*member));
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

    if (event->historical)
        return 0;
    if (event->kind == SNAG_IRC_CONNECTED ||
        event->kind == SNAG_IRC_DISCONNECTED ||
        event->kind == SNAG_IRC_HISTORY_READY) {
        replay_endpoint_clear(irc, event->endpoint);
        return 0;
    }
    member = replay_member_find(irc, event->endpoint,
                                event->room, event->nick);
    if (event->kind == SNAG_IRC_JOIN) {
        (void)replay_member_set(irc, event, event->nick, event->op);
        return 0;
    }
    if (event->kind == SNAG_IRC_MODE) {
        const char *target = event->text[2] == ' ' ? event->text + 3u :
                                                    event->nick;
        bool add = event->text[0] == '+';
        bool self = irc_casecmp(event->nick, target) == 0;

        if (!member && event->op)
            member = replay_member_set(irc, event, event->nick, true);
        if (member && (!member->op ||
            (self && !add ? event->op : !event->op))) {
            errno = EINVAL;
            return -1;
        }
        (void)replay_member_set(irc, event, target, add);
        return 0;
    }
    if (!member && !event->local &&
        (event->kind == SNAG_IRC_MESSAGE || event->kind == SNAG_IRC_NOTICE ||
         event->kind == SNAG_IRC_TOPIC) && event->nick[0])
        member = replay_member_set(irc, event, event->nick, event->op);
    if (member && member->op != event->op) {
        errno = EINVAL;
        return -1;
    }
    if (event->kind == SNAG_IRC_PART || event->kind == SNAG_IRC_QUIT) {
        if (member)
            replay_member_remove(irc, member);
    } else if (event->kind == SNAG_IRC_NICK) {
        struct irc_replay_member *collision = replay_member_find(
            irc, event->endpoint, event->room, event->text);

        if (collision && collision != member) {
            errno = EINVAL;
            return -1;
        }
        if (member)
            memcpy(member->nick, event->text, strlen(event->text) + 1u);
        else
            (void)replay_member_set(irc, event, event->text, event->op);
    }
    return 0;
}

int
snag_irc_core_restore_event(struct snag_irc_core *irc,
                      const struct snag_irc_event *event)
{
    if (!irc || !event || event->timestamp_ms == 0u ||
        !restored_event_shape_valid(event) ||
        replay_transition(irc, event) < 0) {
        errno = EINVAL;
        return -1;
    }
    snag_irc_core_remember(irc, event);
    return 0;
}

const char *
snag_irc_core_model_nick(const struct snag_irc_core *irc)
{
    return !irc ? NULL : !irc->hosting && irc->link_count ?
        irc->links[0].accepted_nick : irc->model_nick;
}

const char *
snag_irc_core_operator_nick(const struct snag_irc_core *irc)
{
    return !irc ? NULL : !irc->hosting && irc->link_count ?
        irc->links[1].accepted_nick : irc->operator_nick;
}
