/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"
#include "cli.h"
#include "config.h"
#include "irc.h"
#include "irc_internal.h"
#include "json.h"
#include "snajpagent.h"
#include "store.h"
#include "net.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif

static void
set_user(const char *value)
{
#ifdef _WIN32
    assert(_putenv_s("USER", value) == 0);
#else
    assert(setenv("USER", value, 1) == 0);
#endif
}

struct capture {
    unsigned int events[SNAG_IRC_HISTORY_READY + 1u];
    unsigned int protocol_traces;
    unsigned int transport_traces;
    struct snag_irc_event last_message;
    struct snag_irc_event last_notice;
    char message_text[2u * SNAG_IRC_TEXT_MAX + 1u];
    struct snag_irc_event last_nick;
    struct snag_irc_event last_part;
    struct snag_irc_event last_connected;
    struct snag_irc_event last_history_ready;
    bool slow_quit;
    bool fail_message;
    bool private_trace;
    bool retain_conversations;
    json_t *conversations;
    uint64_t sequence;
    struct snag_irc_event query[2u];
    unsigned int query_count[2u];
    unsigned int delivery[2u][SNAG_IRC_UNCERTAIN + 1u];
    struct snag_irc_event outgoing[2u];
};

static pthread_t engine_thread;

static int
capture_event(void *opaque, const struct snag_irc_event *event)
{
    struct capture *capture = opaque;

    assert(pthread_equal(pthread_self(), engine_thread));
    assert(event->kind <= SNAG_IRC_HISTORY_READY);
    ++capture->events[event->kind];
    if (event->routed) {
        json_t *data = snag_irc_event_data(event);
        struct snag_irc_event decoded;
        assert(snag_irc_event_record_read("irc_event_v2", data, &decoded) == 0);
        if (capture->retain_conversations) {
            json_t *next = snag_irc_conversations_update(capture->conversations,
                data, ++capture->sequence);
            assert(next);
            json_decref(capture->conversations);
            capture->conversations = next;
        }
        json_decref(data);
        if (event->route.kind == SNAG_IRC_QUERY) {
            capture->query[event->route.identity] = *event;
            ++capture->query_count[event->route.identity];
            if (event->route.direction == SNAG_IRC_OUTGOING) {
                ++capture->delivery[event->route.identity][event->route.delivery];
                capture->outgoing[event->route.identity] = *event;
            }
        }
    }
    if (event->kind == SNAG_IRC_MESSAGE) {
        size_t used = strlen(capture->message_text);
        size_t len = strlen(event->text);

        if (len < sizeof(capture->message_text) - used)
            memcpy(capture->message_text + used, event->text, len + 1u);
        capture->last_message = *event;
    }
    if (event->kind == SNAG_IRC_NOTICE) capture->last_notice = *event;
    if (event->kind == SNAG_IRC_NICK) capture->last_nick = *event;
    if (event->kind == SNAG_IRC_PART) capture->last_part = *event;
    if (event->kind == SNAG_IRC_CONNECTED) capture->last_connected = *event;
    if (event->kind == SNAG_IRC_HISTORY_READY) capture->last_history_ready = *event;
    if (event->kind == SNAG_IRC_QUIT && strcmp(event->nick, "slow") == 0) capture->slow_quit = true;
    return capture->fail_message && event->kind == SNAG_IRC_MESSAGE ? -1 : 0;
}

static int
capture_trace(void *opaque, unsigned int level, char direction,
              const char *endpoint, const char *text, size_t len)
{
    struct capture *capture = opaque;

    assert(pthread_equal(pthread_self(), engine_thread));
    assert((level == 5u || level == 6u) && (direction == '<' || direction == '>'));
    assert(endpoint && *endpoint && text && len != 0u);
    if (strstr(text, "private-body")) capture->private_trace = true;
    if (level == 5u) ++capture->protocol_traces;
    else ++capture->transport_traces;
    return 0;
}

static snag_socket
listen_local(unsigned short *port)
{
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    snag_socket fd = snag_socket_open(AF_INET, SOCK_STREAM, 0);

    assert(fd != SNAG_SOCKET_INVALID);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(snag_socket_bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
    assert(snag_socket_listen(fd, 4) == 0);
    *port = ntohs(address.sin_port);
    return fd;
}

static unsigned short
free_port(void)
{
    unsigned short port;
    snag_socket fd = listen_local(&port);

    assert(snag_socket_close(fd) == 0);
    return port;
}

static void
endpoint(char out[64u], unsigned short port)
{
    int n = snprintf(out, 64u, "127.0.0.1:%u", (unsigned int)port);

    assert(n > 0 && n < 64);
}

static snag_socket
connect_local(unsigned short port, bool slow)
{
    struct sockaddr_in address;
    snag_socket fd = snag_socket_open(AF_INET, SOCK_STREAM, 0);

    assert(fd != SNAG_SOCKET_INVALID);
    snag_socket_nodelay(fd);
    if (slow) {
        int size = 1024;
        assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *)&size, sizeof(size)) == 0);
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    int rc = snag_socket_connect(fd, (struct sockaddr *)&address, sizeof(address));
    assert(rc == 0 || (rc < 0 && errno == EINPROGRESS));
    snag_socket_event ready = {fd, SNAG_NET_WRITE, 0};
    assert(snag_socket_poll(&ready, 1u, 1000) > 0 && snag_socket_connected(fd) == 0);
    return fd;
}

static void
send_text(snag_socket fd, const char *text)
{
    size_t offset = 0u;
    size_t len = strlen(text);

    while (offset < len) {
        ssize_t written = snag_socket_send(fd, text + offset, len - offset);
        if (written < 0 && errno == EINTR) continue;
        assert(written > 0);
        offset += (size_t)written;
    }
}

static void
tick(struct snag_irc *irc, unsigned int rounds)
{
    char error[256] = {0};
    uint64_t until = snag_monotonic_ms() + rounds * 2u;

    /* Protocol I/O is independent; trace wakeups are not completed I/O ticks. */
    do {
        if (snag_irc_tick(irc, 2, error, sizeof(error)) < 0) fprintf(stderr, "IRC tick failed: %s\n", error);
        assert(error[0] == '\0');
    } while (snag_monotonic_ms() < until);
}

static size_t
drain(snag_socket fd, char *out, size_t size)
{
    size_t used = 0u;

    assert(size > 1u);
    for (;;) {
        ssize_t got = snag_socket_recv(fd, out + used, size - used - 1u);
        if (got > 0) {
            used += (size_t)got;
            if (used == size - 1u) break;
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        assert(got == 0 || errno == EAGAIN || errno == EWOULDBLOCK);
        break;
    }
    out[used] = '\0';
    return used;
}

static void
wait_wire(struct snag_irc *server, snag_socket fd, char *wire, size_t wire_size, const char *expected)
{
    size_t used = 0;
    uint64_t deadline = snag_monotonic_ms() + 1000u;
    for (;;) {
        tick(server, 1u);
        used += drain(fd, wire + used, wire_size - used);
        if (strstr(wire, expected)) break;
        if (used + 1u == wire_size || snag_monotonic_ms() >= deadline) {
            (void)fprintf(stderr, "missing IRC reply %s in: %s\n", expected, wire);
            abort();
        }
        snag_socket_event ready = {fd, SNAG_NET_READ, 0};
        assert(snag_socket_poll(&ready, 1u, 20) >= 0);
    }
}

static void
drain_ready(struct snag_irc *runtime, snag_socket fd, char *wire, size_t size)
{
    send_text(fd, "PING :snajpagent-test-barrier\r\n");
    wait_wire(runtime, fd, wire, size, " :snajpagent-test-barrier\r\n");
}

static void
register_peer(struct snag_irc *server, snag_socket fd, const char *nick,
              bool agent, char *wire, size_t wire_size)
{
    char input[1024u];
    int n = snprintf(input, sizeof(input), "CAP LS 302\r\nCAP REQ :batch server-time draft/chathistory%s\r\n"
        "CAP END\r\nNICK %s\r\nUSER %s 0 * :%s\r\nJOIN #lab\r\n",
        agent ? " " SNAJPAGENT_NAME "/agent" : "", nick, nick, agent ? SNAJPAGENT_NAME " agent" : "human");
    assert(n > 0 && (size_t)n < sizeof(input));
    send_text(fd, input);
    wait_wire(server, fd, wire, wire_size, " BATCH -");
}

static void
ping_without_engine(snag_socket fd)
{
    char wire[65536u];
    snag_socket_event ready = {fd, SNAG_NET_READ, 0};
    uint64_t deadline = snag_monotonic_ms() + 250u;

    (void)drain(fd, wire, sizeof(wire));
    send_text(fd, "PING :independent-owner\r\n");
    do {
        assert(snag_socket_poll(&ready, 1u, 20) >= 0);
        (void)drain(fd, wire, sizeof(wire));
        if (strstr(wire, "PONG") && strstr(wire, "independent-owner")) return;
    } while (snag_monotonic_ms() < deadline);
    assert(!"IRC I/O stopped while the engine was not pumping");
}

static void
init_server_config(struct snag_config *config, unsigned short port)
{
    char address[64u];

    snag_config_init(config);
    endpoint(address, port);
    config->irc.listen_explicit = true;
    assert(snprintf(config->irc.listen, sizeof(config->irc.listen), "%s", address) > 0);
    memcpy(config->irc.model_nick, "agent", 6u);
    memcpy(config->irc.operator_nick, "operator", 9u);
    memcpy(config->irc.room_name, "#lab", 5u);
    config->irc.history_lines = 4u;
}

static struct snag_irc *
open_server(struct snag_config *config, struct capture *capture)
{
    struct snag_cli cli;
    struct snag_irc *server = NULL;
    char error[256] = {0};

    memset(&cli, 0, sizeof(cli));
    assert(snag_irc_apply_cli(config, &cli, error, sizeof(error)) == 0);
    if (snag_irc_open(&server, config, "/workspace", capture_event,
                      capture_trace, capture, error, sizeof(error)) < 0) {
        (void)fprintf(stderr, "IRC server %s open: errno=%d %s\n", config->irc.listen, errno, error);
        abort();
    }
    return server;
}

static void
init_client_config(struct snag_config *config, const char *address, const char *model, const char *operator)
{
    struct snag_cli cli = {0};
    char error[256] = {0};

    snag_config_init(config);
    config->irc.client_count = 1u;
    assert(snag_strcpy(config->irc.clients[0], sizeof(config->irc.clients[0]), address));
    assert(snag_strcpy(config->irc.model_nick, sizeof(config->irc.model_nick), model));
    assert(snag_strcpy(config->irc.operator_nick, sizeof(config->irc.operator_nick), operator));
    assert(snag_irc_apply_cli(config, &cli, error, sizeof(error)) == 0);
}

static int
send_all(struct snag_irc *irc, bool model, enum snag_irc_event_kind kind,
         const char *text, char *error, size_t error_size)
{
    struct snag_irc_route route;

    snag_irc_capture_route(irc, &route);
    return snag_irc_send_route(irc, &route, model, kind, text, NULL, error, error_size);
}

/* Keep large scenario frames separate from main on small legacy stacks. */
static void __attribute__((noinline)) test_listener_collision(void)
{
    static const char *const hosts[] = {"127.0.0.1", "localhost"};

    for (size_t i = 0u; i < sizeof(hosts) / sizeof(hosts[0]); ++i) {
        struct snag_config config;
        struct capture capture = {0};
        struct snag_irc *server;
        struct snag_irc *duplicate = NULL;
        unsigned short port = free_port();
        char error[256u] = {0};

        init_server_config(&config, port);
        assert(snprintf(config.irc.listen, sizeof(config.irc.listen),
                        "%s:%u", hosts[i], (unsigned int)port) > 0);
        server = open_server(&config, &capture);
        assert(snag_irc_open(&duplicate, &config, "/duplicate", capture_event,
                            capture_trace, &capture, error, sizeof(error)) < 0);
        assert(!duplicate);
        assert(strstr(error, config.irc.listen));
        assert(strstr(error, strerror(EADDRINUSE)));
        assert(send_all(server, true, SNAG_IRC_MESSAGE, "still here", error, sizeof(error)) == 0);
        snag_irc_close(server);
        server = open_server(&config, &capture);
        snag_irc_close(server);
        snag_config_free(&config);
    }
}

static void __attribute__((noinline)) test_runtime_roles(void)
{
    struct snag_config config, upstream_config;
    struct capture capture = {0}, upstream_capture = {0};
    struct snag_irc *runtime, *upstream;
    char error[256] = {0}, wire[65536], host[64], other[64];
    uint64_t revision;
    unsigned int joins;
    unsigned short host_port = free_port();
    unsigned short upstream_port = free_port();
    snag_socket human;
    struct snag_irc_destinations destinations;
    struct snag_irc_route route = {0}, frozen;
    uint32_t removed_id;

    assert(snag_irc_destinations_generation(NULL) == 0u);
    init_server_config(&upstream_config, upstream_port);
    upstream = open_server(&upstream_config, &upstream_capture);
    tick(upstream, 1u);
    init_server_config(&config, host_port);
    config.irc.listen_explicit = false;
    assert(snag_irc_normalize(&config, error, sizeof(error)) == 0);
    assert(snag_irc_open(&runtime, &config, "/private-workspace", capture_event,
                         capture_trace, &capture, error, sizeof(error)) == 0);
    struct snag_buf state = {.max = 65536u};
    assert(snag_irc_state(runtime, &state, error, sizeof(error)) == 0);
    assert(snag_buf_terminate(&state) == 0);
    assert(strstr((char *)state.data, "no active endpoints"));
    assert(send_all(runtime, false, SNAG_IRC_MESSAGE, "not queued", error, sizeof(error)) == 1);
    assert(!capture.events[SNAG_IRC_MESSAGE]);

    endpoint(host, host_port);
    endpoint(other, upstream_port);
    config.irc.client_count = 1u;
    assert(snag_strcpy(config.irc.clients[0], sizeof(config.irc.clients[0]), other));
    joins = upstream_capture.events[SNAG_IRC_JOIN];
    assert(snag_irc_configure(runtime, &config, "/private-workspace", error, sizeof(error)) == 0);
    /* Registration includes both clients and catch-up, even on slow CPUs. */
    uint64_t registration_deadline = snag_monotonic_ms() + 10000u;
    while ((!snag_irc_mentions_agent(runtime, other, "agent1: work") ||
            upstream_capture.events[SNAG_IRC_JOIN] < joins + 2u) &&
           snag_monotonic_ms() < registration_deadline) {
        tick(runtime, 1u);
        tick(upstream, 1u);
    }
    assert(snag_irc_mentions_agent(runtime, other, "agent1: work"));
    assert(upstream_capture.events[SNAG_IRC_JOIN] == joins + 2u);
    assert(strcmp(snag_irc_model_nick(runtime), "agent1") == 0);
    joins = upstream_capture.events[SNAG_IRC_JOIN];
    revision = snag_irc_routing_revision(runtime);
    uint64_t destination_generation = snag_irc_destinations_generation(runtime);
    assert(snag_irc_configure(runtime, &config, "/private-workspace", error, sizeof(error)) == 0);
    assert(snag_irc_routing_revision(runtime) == revision);
    assert(snag_irc_destinations_generation(runtime) == destination_generation);

    /* A failed listener addition leaves the existing client connected. */
    assert(snag_irc_add(runtime, &config, "/private-workspace", true, other, error, sizeof(error)) < 0);
    tick(upstream, 2u);
    assert(upstream_capture.events[SNAG_IRC_JOIN] == joins);
    assert(snag_irc_routing_revision(runtime) == revision);
    assert(snag_irc_destinations_generation(runtime) == destination_generation);

    config.irc.listen_explicit = true;
    assert(snag_irc_configure(runtime, &config, "/private-workspace", error, sizeof(error)) == 0);
    assert(strcmp(snag_irc_model_nick(runtime), "agent") == 0);
    human = connect_local(host_port, false);
    register_peer(runtime, human, "human", false, wire, sizeof(wire));
    assert(send_all(runtime, false, SNAG_IRC_MESSAGE, "shared-before-stop", error, sizeof(error)) == 0);
    tick(runtime, 3u);
    drain_ready(runtime, human, wire, sizeof(wire));
    assert(strstr(wire, "shared-before-stop"));
    uint64_t shared_deadline = snag_monotonic_ms() + 1000u;
    while (capture.events[SNAG_IRC_MESSAGE] < 2u && snag_monotonic_ms() < shared_deadline) {
        tick(upstream, 1u);
        tick(runtime, 1u);
    }
    assert(capture.events[SNAG_IRC_MESSAGE] == 2u);
    snag_irc_destinations(runtime, &destinations);
    assert(destinations.count == 2u);
    assert(destinations.items[0].target.id < destinations.items[1].target.id);
    assert(strcmp(destinations.items[0].endpoint, other) == 0);
    assert(strcmp(destinations.items[1].endpoint, host) == 0);
    assert(strcmp(destinations.items[0].model, "agent1") == 0);
    route.count = 1u;
    route.targets[0] = destinations.items[1].target;
    removed_id = route.targets[0].id;
    snag_buf_reset(&state);
    assert(snag_irc_send_route(runtime, &route, false, SNAG_IRC_MESSAGE,
        "host-only", &state, error, sizeof(error)) == 0);
    tick(runtime, 3u);
    tick(upstream, 3u);
    drain_ready(runtime, human, wire, sizeof(wire));
    assert(strstr(wire, "host-only"));
    assert(!strstr(upstream_capture.message_text, "host-only"));
    route.targets[0] = destinations.items[0].target;
    assert(snag_irc_send_route(runtime, &route, true, SNAG_IRC_MESSAGE,
        "upstream-only", NULL, error, sizeof(error)) == 0);
    uint64_t route_deadline = snag_monotonic_ms() + 1000u;
    do {
        tick(runtime, 1u);
        tick(upstream, 1u);
    } while ((!strstr(upstream_capture.message_text, "upstream-only") ||
              strcmp(capture.last_message.text, "upstream-only")) && snag_monotonic_ms() < route_deadline);
    drain_ready(runtime, human, wire, sizeof(wire));
    assert(!strstr(wire, "upstream-only"));
    assert(strstr(upstream_capture.message_text, "upstream-only"));
    assert(strcmp(capture.last_message.text, "upstream-only") == 0);
    assert(strcmp(capture.last_message.endpoint, other) == 0);
    assert(strcmp(capture.last_message.nick, "agent1") == 0);
    /* A secondary endpoint's accepted rename must refresh the saved identities. */
    (void)snag_irc_identity_changed(runtime);
    assert(snag_irc_send_route(runtime, &route, true, SNAG_IRC_NICK,
        "secondary", NULL, error, sizeof(error)) == 0);
    uint64_t nick_deadline = snag_monotonic_ms() + 1000u;
    do {
        tick(upstream, 1u);
        tick(runtime, 1u);
        snag_irc_destinations(runtime, &destinations);
    } while (strcmp(destinations.items[0].model, "secondary") &&
        snag_monotonic_ms() < nick_deadline);
    assert(strcmp(destinations.items[0].model, "secondary") == 0);
    assert(strcmp(snag_irc_model_nick(runtime), "agent") == 0);
    assert(snag_irc_identity_changed(runtime));
    route.targets[0] = destinations.items[0].target;
    ++route.targets[0].revision;
    assert(snag_irc_send_route(runtime, &route, true, SNAG_IRC_MESSAGE,
        "wrong-revision", NULL, error, sizeof(error)) == 1);
    /* A stored route gone stale across a reconnect still names the same
     * destination by id: it rebinds instead of failing like a future revision. */
    snag_irc_destinations(runtime, &destinations);
    for (size_t i = 0u; i < destinations.count; ++i)
        if (destinations.items[i].target.id == route.targets[0].id &&
            destinations.items[i].target.revision > 0u) {
            route.targets[0] = destinations.items[i].target;
            --route.targets[0].revision;
        }
    assert(snag_irc_send_route(runtime, &route, true, SNAG_IRC_MESSAGE,
        "stale-revision", NULL, error, sizeof(error)) == 0);
    snag_irc_capture_route(runtime, &frozen);
    assert(frozen.count == 2u);

    /* The owner receives this before removal; admission happens during stop. */
    send_text(human, "PRIVMSG #lab :accepted-before-removal\r\nPING :barrier\r\n");
    {
        snag_socket_event fd = {human, SNAG_NET_READ, 0};
        bool pong = false;
        uint64_t deadline = snag_monotonic_ms() + 1000u;

        while (!pong && snag_monotonic_ms() < deadline) {
            assert(snag_socket_poll(&fd, 1u, 20) >= 0);
            (void)drain(human, wire, sizeof(wire));
            pong = strstr(wire, "PONG") != NULL;
        }
        assert(pong);
    }
    config.irc.listen_explicit = false;
    assert(snag_irc_configure(runtime, &config, "/private-workspace", error, sizeof(error)) == 0);
    assert(strstr(capture.message_text, "accepted-before-removal"));
    assert(strcmp(snag_irc_model_nick(runtime), "secondary") == 0);
    snag_socket_close(human);
    tick(upstream, 3u);
    assert(upstream_capture.events[SNAG_IRC_JOIN] == joins);
    assert(send_all(runtime, false, SNAG_IRC_MESSAGE, "after-host-stop", error, sizeof(error)) == 0);
    route_deadline = snag_monotonic_ms() + 1000u;
    while (!strstr(upstream_capture.message_text, "after-host-stop") && snag_monotonic_ms() < route_deadline)
        tick(upstream, 1u);
    assert(strstr(upstream_capture.message_text, "after-host-stop"));
    assert(snag_irc_send_route(runtime, &frozen, false, SNAG_IRC_MESSAGE,
        "partial-to-survivor", NULL, error, sizeof(error)) == 2);
    route_deadline = snag_monotonic_ms() + 1000u;
    while (!strstr(upstream_capture.message_text, "partial-to-survivor") &&
           snag_monotonic_ms() < route_deadline) tick(upstream, 1u);
    assert(strstr(upstream_capture.message_text, "partial-to-survivor"));

    config.irc.client_count = 0u;
    assert(snag_irc_configure(runtime, &config, "/private-workspace", error, sizeof(error)) == 0);
    assert(!snag_irc_mentions_agent(runtime, other, "agent1: no destination"));
    snag_buf_reset(&state);
    assert(snag_irc_snapshot(runtime, &state, error, sizeof(error)) == 0);
    assert(snag_buf_terminate(&state) == 0);
    assert(strstr((char *)state.data, "no active endpoints"));
    assert(strstr((char *)state.data, "accepted-before-removal"));
    assert(snag_irc_send_route(runtime, &frozen, true, SNAG_IRC_MESSAGE,
        "no-targets", NULL, error, sizeof(error)) == 1);

    /* Re-add the host after freeing both primary owners. Public history only. */
    config.irc.listen_explicit = true;
    assert(snag_irc_configure(runtime, &config, "/private-workspace", error, sizeof(error)) == 0);
    human = connect_local(host_port, false);
    register_peer(runtime, human, "newhuman", false, wire, sizeof(wire));
    snag_irc_destinations(runtime, &destinations);
    assert(destinations.count == 1u && destinations.items[0].target.id > removed_id);
    assert(snag_irc_send_route(runtime, &frozen, false, SNAG_IRC_MESSAGE,
        "not-to-replacement", NULL, error, sizeof(error)) == 1);
    assert(strstr(wire, "accepted-before-removal"));
    assert(!strstr(wire, "after-host-stop"));
    assert(!strstr(wire, "not queued"));
    snag_socket_close(human);
    snag_buf_free(&state);
    snag_irc_close(runtime);
    snag_irc_close(upstream);
    snag_config_free(&config);
    snag_config_free(&upstream_config);
}

static void __attribute__((noinline)) test_validation(void)
{
    struct snag_config config;
    struct snag_cli cli;
    char error[256] = {0};

    memset(&cli, 0, sizeof(cli));
    const struct {
        const char *user, *operator;
    } defaults[] = {
        {"root", "root0"}, {"agent", "localop0"}, {"not valid", "operator0"}
    };
    for (size_t i = 0u; i < sizeof(defaults) / sizeof(defaults[0]); ++i) {
        set_user(defaults[i].user);
        snag_config_init(&config);
        config.irc.listen_explicit = true;
        assert(snag_irc_apply_cli(&config, &cli, error, sizeof(error)) == 0);
        assert(strcmp(config.irc.model_nick, "agent0") == 0);
        assert(strcmp(config.irc.operator_nick, defaults[i].operator) == 0);
        assert(strcmp(config.irc.operator_nick, config.irc.model_nick) != 0);
        assert(config.irc.model_nick_implicit);
        assert(config.irc.operator_nick_implicit);
        snag_config_free(&config);
    }
    set_user("root");

    snag_config_init(&config);
    config.irc.client_count = 2u;
    memcpy(config.irc.clients[0], "localhost", 10u);
    memcpy(config.irc.clients[1], "localhost:6667", 15u);
    memcpy(config.irc.model_nick, "worker", 7u);
    error[0] = '\0';
    assert(snag_irc_apply_cli(&config, &cli, error, sizeof(error)) < 0);
    assert(strstr(error, "duplicate") != NULL);
    snag_config_free(&config);

    snag_config_init(&config);
    config.irc.client_count = 1u;
    memcpy(config.irc.clients[0], "bad\xc3\x28", 6u);
    memcpy(config.irc.model_nick, "worker", 7u);
    error[0] = '\0';
    assert(snag_irc_apply_cli(&config, &cli, error, sizeof(error)) < 0);
    assert(strstr(error, "invalid IRC client endpoint") != NULL);
    snag_config_free(&config);

    const struct {
        const char *model, *operator, *room;
        bool valid;
    } names[] = {
        {"worker", "WORKER", "", false}, {"b\xc3\xb6t", "alice", "", true},
        {"bad\xc2\x85", "alice", "", false}, {"bad\xc2\xa0nick", "alice", "", false},
        {"worker", "alice", "bad\xe2\x80\x8broom", false}
    };
    for (size_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
        snag_config_init(&config);
        config.irc.listen_explicit = true;
        assert(snag_strcpy(config.irc.model_nick, sizeof(config.irc.model_nick), names[i].model));
        assert(snag_strcpy(config.irc.operator_nick, sizeof(config.irc.operator_nick), names[i].operator));
        assert(snag_strcpy(config.irc.room_name, sizeof(config.irc.room_name), names[i].room));
        error[0] = '\0';
        assert(snag_irc_apply_cli(&config, &cli, error, sizeof(error)) == (names[i].valid ? 0 : -1));
        snag_config_free(&config);
    }
}

static void __attribute__((noinline)) test_cli_network_roles(void)
{
    struct snag_config config;
    struct snag_cli cli;
    char error[256] = {0};

    memset(&cli, 0, sizeof(cli));
    cli.irc_listen = "irc.example:7667";
    snag_config_init(&config);
    assert(snag_irc_apply_cli(&config, &cli, error, sizeof(error)) == 0);
    assert(config.irc.listen_explicit);
    assert(strcmp(config.irc.listen, "irc.example:7667") == 0);
    assert(config.irc.client_count == 0u);
    assert(strcmp(config.irc.model_nick, "agent0") == 0);
    assert(strcmp(config.irc.operator_nick, "root0") == 0);
    assert(config.irc.model_nick_implicit);
    assert(config.irc.operator_nick_implicit);
    cli.irc_model_nick = "worker";
    cli.irc_operator_nick = "operator";
    assert(snag_irc_apply_cli(&config, &cli, error, sizeof(error)) == 0);
    assert(strcmp(config.irc.model_nick, "worker") == 0);
    assert(strcmp(config.irc.operator_nick, "operator") == 0);
    assert(!config.irc.model_nick_implicit);
    assert(!config.irc.operator_nick_implicit);
    snag_config_free(&config);

    memset(&cli, 0, sizeof(cli));
    cli.irc_listen = "127.0.0.1:7667";
    cli.irc_clients[0] = "upstream.example:6667";
    cli.irc_client_count = 1u;
    cli.irc_model_nick = "worker";
    cli.irc_operator_nick = "operator";
    snag_config_init(&config);
    error[0] = '\0';
    assert(snag_irc_apply_cli(&config, &cli, error, sizeof(error)) == 0);
    assert(config.irc.listen_explicit);
    assert(strcmp(config.irc.listen, "127.0.0.1:7667") == 0);
    assert(config.irc.client_count == 1u);
    assert(strcmp(config.irc.clients[0], "upstream.example:6667") == 0);
    assert(!config.irc.model_nick_implicit);
    assert(!config.irc.operator_nick_implicit);
    snag_config_free(&config);
}

static void __attribute__((noinline)) test_server(void)
{
    struct snag_config config;
    struct capture capture = {0};
    struct snag_irc *server;
    unsigned short port = free_port();
    char wire[128u * 1024u];
    char traffic[SNAG_IRC_TEXT_MAX + 1u];
    char error[256] = {0};
    snag_socket human;
    snag_socket history;
    snag_socket bad;
    snag_socket slow;

    init_server_config(&config, port);
    config.irc.client_count = 1u;
    assert(snprintf(config.irc.clients[0], sizeof(config.irc.clients[0]), "%s", config.irc.listen) > 0);
    server = open_server(&config, &capture);
    assert(snag_irc_mentions_agent(server, config.irc.listen, "AGENT: please"));
    assert(!snag_irc_mentions_agent(server, config.irc.listen, "otheragent: no"));

    human = connect_local(port, false);
    register_peer(server, human, "human", false, wire, sizeof(wire));
    assert(strstr(wire, " 001 human ") != NULL);
    assert(strstr(wire, "SAJROOM=#lab") != NULL);
    assert(strstr(wire, "LINELEN=8192") != NULL);
    assert(strstr(wire, " 332 human #lab :/workspace") != NULL);
    assert(strstr(wire, "MODE #lab +o human") != NULL);
    assert(strstr(wire, " = #lab :agent") != NULL);
    assert(strstr(wire, " = #lab :@operator") != NULL);
    assert(capture.events[SNAG_IRC_JOIN] != 0u);

    send_text(human, "PING :token-123\r\nMODE #lab +o agent\r\n");
    wait_wire(server, human, wire, sizeof(wire), "MODE #lab +o agent");
    assert(strstr(wire, "PONG") && strstr(wire, "token-123"));
    assert(strstr(wire, "MODE #lab +o agent") != NULL);
    assert(send_all(server, true, SNAG_IRC_TOPIC, "agent topic", error, sizeof(error)) == 0);
    tick(server, 5u);
    drain_ready(server, human, wire, sizeof(wire));
    assert(strstr(wire, "TOPIC #lab :agent topic") != NULL);

    send_text(human, "PRIVMSG #lab :hello \00304red\r\nNAMES #lab\r\nWHO #lab\r\n");
    tick(server, 10u);
    drain_ready(server, human, wire, sizeof(wire));
    assert(strcmp(capture.last_message.nick, "human") == 0);
    assert(strcmp(capture.last_message.text, "hello red") == 0);
    assert(capture.last_message.op);
    assert(strstr(wire, " 366 human #lab") != NULL);
    assert(strstr(wire, " 315 human #lab") != NULL);
    assert(strstr(wire, " 352 human #lab agent ") != NULL);
    assert(strstr(wire, " agent H@ :0 " SNAJPAGENT_NAME "\r\n") != NULL);
    assert(strstr(wire, " 352 human #lab operator ") != NULL);
    assert(strstr(wire, " operator H@ :0 operator\r\n") != NULL);
    assert(strstr(wire, " human H@ :0 IRC user\r\n") != NULL);
    assert(capture.protocol_traces != 0u && capture.transport_traces != 0u);

    send_text(human, "NICK renamed\r\nNICK renamed\r\nNICK RENAMED\r\n"
                     "NICK agent\r\nPRIVMSG #lab :renamed speech\r\n" "TOPIC #lab :agent topic\r\n");
    wait_wire(server, human, wire, sizeof(wire), " :renamed speech\r\n");
    assert(capture.events[SNAG_IRC_NICK] == 2u);
    assert(strcmp(capture.last_nick.nick, "renamed") == 0);
    assert(strcmp(capture.last_nick.text, "RENAMED") == 0);
    assert(capture.last_nick.op);
    assert(strstr(wire, " NICK :renamed\r\n"));
    assert(strstr(wire, " NICK :RENAMED\r\n"));
    assert(strstr(wire, " 433 "));
    assert(strcmp(capture.last_message.nick, "RENAMED") == 0);
    assert(capture.last_message.op);
    send_text(human, "PART #lab\r\nNICK away\r\nJOIN #lab\r\n");
    wait_wire(server, human, wire, sizeof(wire), " 366 away #lab ");
    assert(strstr(wire, " NICK :away\r\n"));
    assert(strstr(wire, " 366 away #lab "));
    assert(capture.events[SNAG_IRC_NICK] == 2u);

    {
        struct snag_irc_event foreign = {0};

        foreign.timestamp_ms = snag_time_ms();
        memcpy(foreign.endpoint, "elsewhere:6667", 15u);
        memcpy(foreign.room, "#lab", 5u);
        memcpy(foreign.nick, "guest", 6u);
        foreign.kind = SNAG_IRC_JOIN;
        assert(snag_irc_restore_event(server, &foreign) == 0);
        foreign.kind = SNAG_IRC_MESSAGE;
        memcpy(foreign.text, "ordinary", 9u);
        assert(snag_irc_restore_event(server, &foreign) == 0);
        foreign.op = true;
        assert(snag_irc_restore_event(server, &foreign) < 0);
        foreign.op = false;
        foreign.kind = SNAG_IRC_MODE;
        memcpy(foreign.text, "+o other", 9u);
        assert(snag_irc_restore_event(server, &foreign) < 0);
        memcpy(foreign.nick, "otherop", 8u);
        foreign.kind = SNAG_IRC_JOIN;
        foreign.text[0] = '\0';
        foreign.op = true;
        assert(snag_irc_restore_event(server, &foreign) == 0);
        foreign.kind = SNAG_IRC_MODE;
        memcpy(foreign.text, "+o guest", 9u);
        assert(snag_irc_restore_event(server, &foreign) == 0);
        memcpy(foreign.nick, "guest", 6u);
        foreign.kind = SNAG_IRC_MESSAGE;
        memcpy(foreign.text, "promoted", 9u);
        assert(snag_irc_restore_event(server, &foreign) == 0);

        memcpy(foreign.nick, "unknown", 8u);
        memcpy(foreign.text, "forged op", 10u);
        assert(snag_irc_restore_event(server, &foreign) == 0);
        foreign.op = false;
        assert(snag_irc_restore_event(server, &foreign) < 0);
        foreign.op = true;

        foreign.kind = SNAG_IRC_TOPIC;
        memcpy(foreign.nick, "otherop", 8u);
        memcpy(foreign.text, "foreign topic must not leak", 28u);
        foreign.op = true;
        assert(snag_irc_restore_event(server, &foreign) == 0);
    }

    assert(send_all(server, true, SNAG_IRC_MESSAGE, "history marker", error, sizeof(error)) == 0);
    tick(server, 5u);
    drain_ready(server, human, wire, sizeof(wire));
    history = connect_local(port, false);
    register_peer(server, history, "reader", false, wire, sizeof(wire));
    assert(strstr(wire, "BATCH +") != NULL);
    assert(strstr(wire, "chathistory #lab") != NULL);
    assert(strstr(wire, "history marker") != NULL);
    assert(strstr(wire, "@batch=") != NULL);
    assert(strstr(wire, " 332 reader #lab :agent topic") != NULL);
    assert(strstr(wire, "foreign topic must not leak") == NULL);
    assert(snag_socket_close(history) == 0);
    tick(server, 5u);

    bad = connect_local(port, false);
    memset(wire, 'x', 8191u);
    memcpy(wire + 8191u, "\r\n", 3u);
    send_text(bad, wire);
    tick(server, 10u);
    assert(snag_socket_close(bad) == 0);
    send_text(human, "PING :still-alive\r\n");
    tick(server, 5u);
    drain_ready(server, human, wire, sizeof(wire));
    assert(strstr(wire, "still-alive") != NULL);

    slow = connect_local(port, true);
    register_peer(server, slow, "slow", false, wire, sizeof(wire));
    memset(traffic, 'x', sizeof(traffic) - 1u);
    traffic[sizeof(traffic) - 1u] = '\0';
    for (unsigned int i = 0u; i < 20000u && !capture.slow_quit; ++i) {
        assert(send_all(server, true, SNAG_IRC_MESSAGE, traffic, error, sizeof(error)) == 0);
        tick(server, 1u);
        (void)drain(human, wire, sizeof(wire));
    }
    assert(capture.slow_quit);
    assert(snag_socket_close(slow) == 0);

    send_text(human, "MODE #lab -o agent\r\n");
    wait_wire(server, human, wire, sizeof(wire), "MODE #lab -o agent");
    error[0] = '\0';
    /* Session-hosted room: the model link sets the topic even while un-opped. */
    assert(send_all(server, true, SNAG_IRC_TOPIC, "hosted agent topic", error, sizeof(error)) == 0);
    tick(server, 5u);
    drain_ready(server, human, wire, sizeof(wire));
    assert(strstr(wire, "TOPIC #lab :hosted agent topic") != NULL);
    ping_without_engine(human);
    assert(snag_socket_close(human) == 0);
    tick(server, 5u);
    /* Local identities must not consume any of the 64 remote peer slots. */
    snag_socket capacity[64u];
    for (size_t i = 0; i < 64u; ++i) {
        capacity[i] = connect_local(port, false);
        send_text(capacity[i], "PING :capacity\r\n");
        tick(server, 1u);
    }
    for (size_t i = 0; i < 64u; ++i) {
        drain_ready(server, capacity[i], wire, sizeof(wire));
        assert(strstr(wire, "PONG") && strstr(wire, "capacity"));
        assert(snag_socket_close(capacity[i]) == 0);
    }
    snag_irc_close(server);
    snag_config_free(&config);
}

static void __attribute__((noinline)) test_nick_rename(void)
{
    struct snag_config config;
    struct capture capture = {0};
    struct snag_irc *server;
    unsigned short port = free_port();
    char error[256] = {0};
    unsigned int nicks_before;

    init_server_config(&config, port);
    server = open_server(&config, &capture);
    assert(strcmp(snag_irc_model_nick(server), "agent") == 0);
    nicks_before = capture.events[SNAG_IRC_NICK];
    /* Session-hosted rename shares the new agent nick via a NICK event. */
    assert(send_all(server, true, SNAG_IRC_NICK, "lead", error, sizeof(error)) == 0);
    assert(strcmp(snag_irc_model_nick(server), "lead") == 0);
    assert(capture.events[SNAG_IRC_NICK] == nicks_before + 1u);
    assert(strcmp(capture.last_nick.nick, "agent") == 0);
    assert(strcmp(capture.last_nick.text, "lead") == 0);
    /* Invalid nicks are rejected and leave the identity alone. */
    assert(send_all(server, true, SNAG_IRC_NICK, "9bad nick", error, sizeof(error)) != 0);
    assert(strcmp(snag_irc_model_nick(server), "lead") == 0);
    assert(capture.events[SNAG_IRC_NICK] == nicks_before + 1u);
    /* The operator identity is untouched by the agent rename. */
    assert(strcmp(snag_irc_operator_nick(server), "operator") == 0);
    snag_irc_close(server);
    snag_config_free(&config);
}

static void
pump_pair(struct snag_irc *server, struct snag_irc *client, unsigned int rounds)
{
    char error[256];
    uint64_t until = snag_monotonic_ms() + rounds * 2u;

    do {
        error[0] = '\0';
        assert(snag_irc_tick(client, 2, error, sizeof(error)) == 0);
        error[0] = '\0';
        assert(snag_irc_tick(server, 0, error, sizeof(error)) == 0);
    } while (snag_monotonic_ms() < until);
}

static void
wait_pair_event(struct snag_irc *server, struct snag_irc *client,
                const struct capture *capture, enum snag_irc_event_kind kind, unsigned int count)
{
    /* Registration includes names/history exchange on both client identities.
     * Expected output gets the same 30 s floor as the pty and tmux harnesses: a
     * loaded host must not fail a correct exchange on a one-second budget. */
    uint64_t deadline = snag_monotonic_ms() + 30000u;
    while (capture->events[kind] < count && snag_monotonic_ms() < deadline) {
        if (server) pump_pair(server, client, 1u);
        else tick(client, 1u);
    }
    if (capture->events[kind] < count)
        (void)fprintf(stderr, "wait_pair_event: kind=%u expected=%u received=%u messages=%u notices=%u history=%u\n",
            (unsigned int)kind, count, capture->events[kind], capture->events[SNAG_IRC_MESSAGE],
            capture->events[SNAG_IRC_NOTICE], capture->events[SNAG_IRC_HISTORY_READY]);
    assert(capture->events[kind] >= count);
}

static void
wait_pair_state(struct snag_irc *server, struct snag_irc *client, const char *joined)
{
    uint64_t deadline = snag_monotonic_ms() + 10000u;
    struct snag_buf state;
    char error[256] = {0};

    snag_buf_init(&state, SNAG_MAX_IRC_SNAPSHOT);
    for (;;) {
        pump_pair(server, client, 1u);
        snag_buf_reset(&state);
        assert(snag_irc_state(client, &state, error, sizeof(error)) == 0);
        assert(snag_buf_terminate(&state) == 0);
        if (strstr((const char *)state.data, joined) && strstr((const char *)state.data, "]: /workspace"))
            break;
        if (snag_monotonic_ms() >= deadline) {
            (void)fprintf(stderr, "missing IRC state %s: %s\n", joined, state.data);
            abort();
        }
    }
    snag_buf_free(&state);
}

static void __attribute__((noinline)) test_client_reconnect(void)
{
    struct snag_config server_config;
    struct snag_config client_config;
    struct capture server_capture = {0};
    struct capture next_capture = {0};
    struct capture client_capture = {0};
    struct snag_irc *server;
    struct snag_irc *next_server;
    struct snag_irc *client = NULL;
    unsigned short port = free_port();
    char address[64u];
    char error[256] = {0};
    char payload[SNAG_IRC_TEXT_MAX + 1u];
    char long_text[SNAG_IRC_TEXT_MAX + 32u];
    uint64_t history_before;
    uint64_t history_after;
    unsigned int messages;

    init_server_config(&server_config, port);
    server_config.irc.history_lines = 1000u;
    server = open_server(&server_config, &server_capture);
    for (size_t i = 0u; i < SNAG_IRC_TEXT_MAX; i += 4u) memcpy(payload + i, "\xf0\x9f\x8c\x99", 4u);
    payload[SNAG_IRC_TEXT_MAX] = '\0';
    history_before = snag_time_ms();
    for (unsigned int i = 0u; i < 1000u; ++i)
        assert(send_all(server, true, SNAG_IRC_MESSAGE, payload, error, sizeof(error)) == 0);
    history_after = snag_time_ms();
    {
        struct capture replay = {0};

        assert(snag_irc_replay_hosted_history(server, capture_event, &replay) == 0);
        assert(replay.events[SNAG_IRC_MESSAGE] == 1000u);
        assert(replay.events[SNAG_IRC_HISTORY_READY] == 1u);
        assert(strcmp(replay.last_history_ready.endpoint, server_config.irc.listen) == 0);
        assert(strcmp(replay.last_history_ready.room, server_config.irc.room_name) == 0);
        assert(replay.last_message.historical);
        assert(strcmp(replay.last_message.text, payload) == 0);
        assert(!server_capture.last_message.historical);
    }
    endpoint(address, port);
    init_client_config(&client_config, address, "remoteagent", "remoteop");
    client_config.irc.history_lines = 1000u;
    assert(snag_irc_open(&client, &client_config, "/client", capture_event, capture_trace, &client_capture,
                        error, sizeof(error)) == 0);
    /* Observe the complete 4 MiB replay; poll counts do not measure progress. */
    uint64_t history_deadline = snag_monotonic_ms() + 120000u;
    while (!client_capture.events[SNAG_IRC_HISTORY_READY] && snag_monotonic_ms() < history_deadline)
        pump_pair(server, client, 1u);
    assert(client_capture.events[SNAG_IRC_HISTORY_READY] != 0u);
    assert(server_capture.events[SNAG_IRC_JOIN] >= 2u);
    assert(strcmp(client_capture.last_message.text, payload) == 0);
    assert(client_capture.events[SNAG_IRC_MESSAGE] >= 990u);
    assert(client_capture.last_message.historical);
    assert(client_capture.last_message.timestamp_ms % 1000u == 0u);
    assert(client_capture.last_message.timestamp_ms >= history_before / 1000u * 1000u);
    assert(client_capture.last_message.timestamp_ms <= history_after);
    struct snag_buf snapshot = {.max = SNAG_MAX_IRC_SNAPSHOT};
    assert(snag_irc_snapshot(client, &snapshot, error, sizeof(error)) == 0);
    assert(snapshot.len > 4u * 1000u * 1000u);
    snag_buf_free(&snapshot);

    messages = client_capture.events[SNAG_IRC_MESSAGE];
    assert(send_all(client, true, SNAG_IRC_MESSAGE, payload, error, sizeof(error)) == 0);
    wait_pair_event(server, client, &client_capture, SNAG_IRC_MESSAGE, messages + 1u);
    assert(client_capture.events[SNAG_IRC_MESSAGE] == messages + 1u);
    assert(strcmp(server_capture.last_message.text, payload) == 0);
    assert(strcmp(server_capture.last_message.nick, "remoteagent") == 0);
    snag_irc_close(client);
    tick(server, 10u);
    client = NULL;
    client_capture.events[SNAG_IRC_HISTORY_READY] = 0u;
    assert(snag_irc_open(&client, &client_config, "/client", capture_event, capture_trace, &client_capture,
                        error, sizeof(error)) == 0);
    history_deadline = snag_monotonic_ms() + 120000u;
    while (!client_capture.events[SNAG_IRC_HISTORY_READY] && snag_monotonic_ms() < history_deadline)
        pump_pair(server, client, 1u);
    assert(client_capture.events[SNAG_IRC_HISTORY_READY] != 0u);
    assert(client_capture.last_message.historical);
    assert(strcmp(client_capture.last_message.nick, "remoteagent") == 0);
    assert(strcmp(client_capture.last_message.text, payload) == 0);

    unsigned int received = client_capture.events[SNAG_IRC_NOTICE];
    assert(send_all(client, true, SNAG_IRC_NOTICE, payload, error, sizeof(error)) == 0);
    wait_pair_event(server, client, &client_capture, SNAG_IRC_NOTICE, received + 1u);
    assert(strcmp(server_capture.last_notice.text, payload) == 0);
    assert(strcmp(client_capture.last_notice.text, payload) == 0);

    memset(long_text, 'a', SNAG_IRC_TEXT_MAX - 4u);
    memcpy(long_text + SNAG_IRC_TEXT_MAX - 4u, " remaining artifact gaps", 25u);
    server_capture.message_text[0] = '\0';
    client_capture.message_text[0] = '\0';
    messages = server_capture.events[SNAG_IRC_MESSAGE];
    received = client_capture.events[SNAG_IRC_MESSAGE];
    assert(send_all(server, true, SNAG_IRC_MESSAGE, long_text, error, sizeof(error)) == 0);
    wait_pair_event(server, client, &client_capture, SNAG_IRC_MESSAGE, received + 2u);
    assert(server_capture.events[SNAG_IRC_MESSAGE] == messages + 2u);
    assert(strcmp(client_capture.last_message.text, "remaining artifact gaps") == 0);
    assert(strcmp(server_capture.message_text, long_text) == 0);
    assert(strcmp(client_capture.message_text, long_text) == 0);

    memset(long_text, 'b', SNAG_IRC_TEXT_MAX - 1u);
    memcpy(long_text + SNAG_IRC_TEXT_MAX - 1u, "\xf0\x9f\x8c\x99" "end", 8u);
    server_capture.message_text[0] = '\0';
    client_capture.message_text[0] = '\0';
    received = client_capture.events[SNAG_IRC_MESSAGE];
    assert(send_all(client, true, SNAG_IRC_MESSAGE, long_text, error, sizeof(error)) == 0);
    wait_pair_event(server, client, &client_capture, SNAG_IRC_MESSAGE, received + 2u);
    assert(strcmp(server_capture.last_message.text, "\xf0\x9f\x8c\x99" "end") == 0);
    assert(strcmp(server_capture.message_text, long_text) == 0);
    assert(strcmp(client_capture.message_text, long_text) == 0);

    received = client_capture.events[SNAG_IRC_MESSAGE];
    assert(send_all(client, false, SNAG_IRC_MESSAGE, "remote hello", error, sizeof(error)) == 0);
    wait_pair_event(server, client, &client_capture, SNAG_IRC_MESSAGE, received + 1u);
    assert(strcmp(server_capture.last_message.nick, "remoteop") == 0);
    assert(server_capture.last_message.op);

    uint64_t revision = snag_irc_routing_revision(client);
    snag_irc_close(server);
    for (unsigned int i = 0u;
         i < 50u && !client_capture.events[SNAG_IRC_DISCONNECTED]; ++i) tick(client, 1u);
    assert(client_capture.events[SNAG_IRC_DISCONNECTED] != 0u);
    /* A server that stays down reports one disconnect, not one per retry: the
     * outage must not crowd the model context or stdout. The reconnect below
     * runs through several retries, so the count must stay exactly one. */
    unsigned int disconnected_once = client_capture.events[SNAG_IRC_DISCONNECTED];
    assert(send_all(client, true, SNAG_IRC_MESSAGE, "retained while disconnected",
                              error, sizeof(error)) == 0);
    assert(strcmp(client_capture.last_message.room, "#lab") == 0);
    next_server = open_server(&server_config, &next_capture);
    unsigned int connected_before = client_capture.events[SNAG_IRC_CONNECTED];
    uint64_t reconnect_deadline = snag_monotonic_ms() + 10000u;
    /* The retained message can reach the new server before the client's own
     * connected counter is updated: pump until both facts hold (or the deadline
     * passes) instead of stopping at the message and racing the counter. */
    while ((strcmp(next_capture.last_message.text, "retained while disconnected") != 0 ||
            client_capture.events[SNAG_IRC_CONNECTED] <= connected_before) &&
           snag_monotonic_ms() < reconnect_deadline) pump_pair(next_server, client, 1u);
    assert(strcmp(next_capture.last_message.nick, "remoteagent") == 0);
    assert(strcmp(next_capture.last_message.text, "retained while disconnected") == 0);
    /* Recovery reports itself once, after the single outage notice, and the
     * retries that reconnected never emitted another disconnect. */
    assert(client_capture.events[SNAG_IRC_CONNECTED] > connected_before);
    assert(client_capture.events[SNAG_IRC_DISCONNECTED] == disconnected_once);
    wait_pair_state(next_server, client, "joined #lab");
    snag_buf_init(&snapshot, SNAG_MAX_IRC_SNAPSHOT);
    assert(snag_irc_snapshot(client, &snapshot, error, sizeof(error)) == 0);
    assert(snag_buf_terminate(&snapshot) == 0);
    assert(strstr((const char *)snapshot.data, address) != NULL);
    assert(strstr((const char *)snapshot.data, "joined #lab") != NULL);
    assert(strstr((const char *)snapshot.data, "topic[") != NULL);
    assert(strstr((const char *)snapshot.data, "]: /workspace") != NULL);
    snag_buf_free(&snapshot);
    assert(snag_irc_routing_revision(client) == revision);
    uint64_t destination_generation = snag_irc_destinations_generation(client);

    /* Same endpoint can advertise a different room on reconnect. */
    snag_irc_close(next_server);
    tick(client, 50u);
    memcpy(server_config.irc.room_name, "#other", sizeof("#other"));
    next_server = open_server(&server_config, &next_capture);
    wait_pair_state(next_server, client, "joined #other");
    assert(snag_irc_routing_revision(client) > revision);
    assert(snag_irc_destinations_generation(client) > destination_generation);
    snag_buf_init(&snapshot, SNAG_MAX_IRC_SNAPSHOT);
    assert(snag_irc_state(client, &snapshot, error, sizeof(error)) == 0);
    assert(snag_buf_terminate(&snapshot) == 0);
    assert(strstr((const char *)snapshot.data, "joined #other"));
    snag_buf_free(&snapshot);

    pump_pair(next_server, client, 1u);
    uint64_t steady_generation = snag_irc_destinations_generation(client);
    tick(client, 2u);
    tick(client, 2u);
    assert(snag_irc_destinations_generation(client) == steady_generation);

    snag_irc_close(client);
    snag_irc_close(next_server);
    snag_config_free(&client_config);
    snag_config_free(&server_config);
}

static void __attribute__((noinline)) test_default_nick_sequence(void)
{
    struct snag_config server_config;
    struct snag_config client_config[2u];
    struct capture server_capture = {0};
    struct capture client_capture[2u];
    struct snag_irc *server;
    struct snag_irc *client[2u] = {NULL, NULL};
    unsigned short port = free_port();
    char address[64u];
    char expected[32u];
    char error[256] = {0};

    snag_config_init(&server_config);
    endpoint(address, port);
    server_config.irc.listen_explicit = true;
    assert(snprintf(server_config.irc.listen, sizeof(server_config.irc.listen), "%s", address) > 0);
    memcpy(server_config.irc.room_name, "#lab", 5u);
    server = open_server(&server_config, &server_capture);
    assert(strcmp(server_config.irc.model_nick, "agent0") == 0);
    assert(strcmp(server_config.irc.operator_nick, "root0") == 0);
    assert(server_config.irc.model_nick_implicit);
    assert(server_config.irc.operator_nick_implicit);
    assert(strcmp(snag_irc_model_nick(server), "agent0") == 0);
    assert(strcmp(snag_irc_operator_nick(server), "root0") == 0);

    memset(client_capture, 0, sizeof(client_capture));
    for (size_t i = 0u; i < 2u; ++i) {
        init_client_config(&client_config[i], address, "", "");
        assert(strcmp(client_config[i].irc.model_nick, "agent0") == 0);
        assert(strcmp(client_config[i].irc.operator_nick, "root0") == 0);
        assert(client_config[i].irc.model_nick_implicit);
        assert(client_config[i].irc.operator_nick_implicit);
        assert(snag_irc_open(&client[i], &client_config[i], "/client",
                            capture_event, capture_trace, &client_capture[i], error, sizeof(error)) == 0);
        wait_pair_event(server, client[i], &client_capture[i], SNAG_IRC_HISTORY_READY, 1u);
        assert(client_capture[i].events[SNAG_IRC_CONNECTED] == 1u);
        assert(client_capture[i].events[SNAG_IRC_DISCONNECTED] == 0u);
        assert(snprintf(expected, sizeof(expected), "agent%zu", i + 1u) > 0);
        assert(strcmp(snag_irc_model_nick(client[i]), expected) == 0);
        assert(snprintf(expected, sizeof(expected), "root%zu", i + 1u) > 0);
        assert(strcmp(snag_irc_operator_nick(client[i]), expected) == 0);
        assert(strstr(snag_irc_model_nick(client[i]), "01") == NULL);
        assert(strstr(snag_irc_operator_nick(client[i]), "01") == NULL);
    }

    for (size_t i = 0u; i < 2u; ++i) {
        snag_irc_close(client[i]);
        snag_config_free(&client_config[i]);
    }
    snag_irc_close(server);
    snag_config_free(&server_config);
}

static void __attribute__((noinline)) test_client_nick_collision(bool explicit_zero)
{
    struct snag_config server_config;
    struct snag_config client_config;
    struct capture server_capture = {0};
    struct capture next_capture = {0};
    struct capture client_capture = {0};
    struct snag_irc *server;
    struct snag_irc *next_server;
    struct snag_irc *client = NULL;
    unsigned short port = free_port();
    snag_socket occupied[2u];
    char address[64u];
    char wire[8192u];
    char error[256] = {0};
    unsigned int messages;

    init_server_config(&server_config, port);
    server = open_server(&server_config, &server_capture);
    occupied[0] = connect_local(port, false);
    register_peer(server, occupied[0], explicit_zero ? "worker0" : "agent1",
                   explicit_zero, wire, sizeof(wire));
    occupied[1] = connect_local(port, false);
    register_peer(server, occupied[1], explicit_zero ? "local0" : "operator1", false, wire, sizeof(wire));

    endpoint(address, port);
    init_client_config(&client_config, address, explicit_zero ? "worker0" : "agent",
                        explicit_zero ? "local0" : "operator");
    assert(!client_config.irc.model_nick_implicit);
    assert(!client_config.irc.operator_nick_implicit);
    assert(snag_irc_open(&client, &client_config, "/client", capture_event, capture_trace, &client_capture,
                        error, sizeof(error)) == 0);
    wait_pair_event(server, client, &client_capture, SNAG_IRC_HISTORY_READY, 1u);
    assert(client_capture.events[SNAG_IRC_HISTORY_READY] != 0u);
    assert(client_capture.events[SNAG_IRC_CONNECTED] == 1u);
    assert(client_capture.events[SNAG_IRC_DISCONNECTED] == 0u);
    if (explicit_zero) {
        assert(strcmp(snag_irc_model_nick(client), "worker01") == 0);
        assert(strcmp(snag_irc_operator_nick(client), "local01") == 0);
        goto out;
    }
    assert(strcmp(client_capture.last_connected.nick, "operator2") == 0);
    assert(strcmp(snag_irc_model_nick(client), "agent2") == 0);
    assert(strcmp(snag_irc_operator_nick(client), "operator2") == 0);
    assert(client_capture.events[SNAG_IRC_NICK] == 0u);

    messages = client_capture.events[SNAG_IRC_MESSAGE];
    assert(send_all(client, false, SNAG_IRC_MESSAGE, "operator alias", error, sizeof(error)) == 0);
    wait_pair_event(server, client, &client_capture, SNAG_IRC_MESSAGE, messages + 1u);
    assert(strcmp(client_capture.last_message.nick, "operator2") == 0);
    assert(strcmp(server_capture.last_message.nick, "operator2") == 0);
    assert(strcmp(server_capture.last_message.text, "operator alias") == 0);
    messages = client_capture.events[SNAG_IRC_MESSAGE];
    assert(send_all(client, true, SNAG_IRC_MESSAGE, "agent alias", error, sizeof(error)) == 0);
    wait_pair_event(server, client, &client_capture, SNAG_IRC_MESSAGE, messages + 1u);
    assert(strcmp(client_capture.last_message.nick, "agent2") == 0);
    assert(strcmp(server_capture.last_message.nick, "agent2") == 0);
    assert(strcmp(server_capture.last_message.text, "agent alias") == 0);

    messages = client_capture.events[SNAG_IRC_MESSAGE];
    assert(send_all(server, true, SNAG_IRC_MESSAGE, "preferred nick is remote", error, sizeof(error)) == 0);
    wait_pair_event(server, client, &client_capture, SNAG_IRC_MESSAGE, messages + 1u);
    assert(client_capture.events[SNAG_IRC_MESSAGE] == messages + 1u);
    assert(strcmp(client_capture.last_message.nick, "agent") == 0);
    assert(strcmp(client_capture.last_message.text, "preferred nick is remote") == 0);
    assert(snag_irc_mentions_agent(client, address, "agent2: respond"));
    assert(!snag_irc_mentions_agent(client, address, "agent: not this client"));
    struct snag_buf snapshot = {.max = SNAG_MAX_IRC_SNAPSHOT};
    assert(snag_irc_snapshot(client, &snapshot, error, sizeof(error)) == 0);
    assert(snag_buf_terminate(&snapshot) == 0);
    assert(strstr((const char *)snapshot.data, "model agent2 operator operator2") != NULL);
    snag_buf_free(&snapshot);

    for (size_t i = 0u; i < 2u; ++i) assert(snag_socket_close(occupied[i]) == 0);
    snag_irc_close(server);
    for (unsigned int i = 0u;
         i < 50u && !client_capture.events[SNAG_IRC_DISCONNECTED]; ++i) tick(client, 1u);
    assert(client_capture.events[SNAG_IRC_DISCONNECTED] != 0u);
    next_server = open_server(&server_config, &next_capture);
    wait_pair_state(next_server, client, "joined #lab");
    assert(client_capture.events[SNAG_IRC_CONNECTED] == 2u);
    messages = client_capture.events[SNAG_IRC_MESSAGE];
    assert(send_all(client, false, SNAG_IRC_MESSAGE, "stable operator alias", error, sizeof(error)) == 0);
    wait_pair_event(next_server, client, &client_capture, SNAG_IRC_MESSAGE, messages + 1u);
    assert(strcmp(next_capture.last_message.nick, "operator2") == 0);
    messages = client_capture.events[SNAG_IRC_MESSAGE];
    assert(send_all(client, true, SNAG_IRC_MESSAGE, "stable agent alias", error, sizeof(error)) == 0);
    wait_pair_event(next_server, client, &client_capture, SNAG_IRC_MESSAGE, messages + 1u);
    assert(strcmp(next_capture.last_message.nick, "agent2") == 0);

    server = next_server;
out: snag_irc_close(client);
    if (explicit_zero)
        for (size_t i = 0u; i < 2u; ++i) assert(snag_socket_close(occupied[i]) == 0);
    snag_irc_close(server);
    snag_config_free(&client_config);
    snag_config_free(&server_config);
}

static void
test_private_relay(void)
{
    struct snag_config config;
    struct capture capture = {0};
    unsigned short port = free_port();
    char wire[32768u];
    char error[256u] = {0};
    init_server_config(&config, port);
    struct snag_irc *server = open_server(&config, &capture);
    snag_socket sender = connect_local(port, false);
    snag_socket recipient = connect_local(port, false);
    snag_socket observer = connect_local(port, false);
    register_peer(server, sender, "sender", false, wire, sizeof(wire));
    register_peer(server, recipient, "recipient", false, wire, sizeof(wire));
    register_peer(server, observer, "observer", false, wire, sizeof(wire));
    drain_ready(server, sender, wire, sizeof(wire));
    drain_ready(server, recipient, wire, sizeof(wire));
    unsigned int messages = capture.events[SNAG_IRC_MESSAGE];

    /* Private delivery works after leaving the shared channel. */
    send_text(recipient, "PART #lab\r\n");
    drain_ready(server, recipient, wire, sizeof(wire));
    send_text(sender, "PRIVMSG ReCiPiEnT :private-body unicast\r\n");
    wait_wire(server, recipient, wire, sizeof(wire),
        "PRIVMSG recipient :private-body unicast\r\n");
    assert(strstr(wire, ":sender!sender@"));
    drain_ready(server, sender, wire, sizeof(wire));
    assert(!strstr(wire, "private-body"));
    drain_ready(server, observer, wire, sizeof(wire));
    assert(!strstr(wire, "private-body"));
    assert(capture.events[SNAG_IRC_MESSAGE] == messages);
    assert(!capture.private_trace);

    /* Echo is explicit, and another REQ preserves previously negotiated caps. */
    send_text(sender, "CAP REQ :echo-message\r\nCAP REQ :server-time\r\n");
    drain_ready(server, sender, wire, sizeof(wire));
    assert(strstr(wire, " ACK :echo-message\r\n"));
    send_text(sender, "PRIVMSG recipient :private-body echoed\r\n");
    wait_wire(server, recipient, wire, sizeof(wire), " :private-body echoed\r\n");
    wait_wire(server, sender, wire, sizeof(wire), " :private-body echoed\r\n");
    assert(strstr(wire, "@time="));
    send_text(sender, "PRIVMSG sender :private-body self\r\n");
    drain_ready(server, sender, wire, sizeof(wire));
    char *self = strstr(wire, " :private-body self\r\n");
    assert(self && !strstr(self + 1u, " :private-body self\r\n"));

    /* Failed negotiation is atomic; NOTICE failures never generate replies. */
    send_text(sender, "CAP REQ :-echo-message nonexistent\r\n"
        "PRIVMSG recipient :private-body still-echoed\r\n");
    drain_ready(server, sender, wire, sizeof(wire));
    assert(strstr(wire, " NAK :-echo-message nonexistent\r\n"));
    assert(strstr(wire, " :private-body still-echoed\r\n"));
    drain_ready(server, recipient, wire, sizeof(wire));
    send_text(sender, "PRIVMSG missing :private-body missing\r\n"
        "NOTICE missing :private-body missing-notice\r\n");
    drain_ready(server, sender, wire, sizeof(wire));
    assert(strstr(wire, " 401 sender missing :"));
    assert(!strstr(wire, "missing-notice"));
    assert(!strstr(strstr(wire, " 401 ") + 1u, " 401 "));
    send_text(sender, "CAP REQ :-echo-message\r\n"
        "NOTICE recipient :private-body notice\r\n");
    drain_ready(server, sender, wire, sizeof(wire));
    assert(!strstr(wire, "private-body notice"));
    wait_wire(server, recipient, wire, sizeof(wire),
        "NOTICE recipient :private-body notice\r\n");

    /* Registration is required on both ends. A reserved nick is not a user. */
    snag_socket unregistered = connect_local(port, false);
    send_text(unregistered, "NICK reserved\r\nPRIVMSG recipient :private-body unregistered\r\n"
        "NOTICE recipient :private-body unregistered-notice\r\n");
    drain_ready(server, unregistered, wire, sizeof(wire));
    assert(strstr(wire, " 451 "));
    send_text(sender, "PRIVMSG reserved :private-body reserved\r\n");
    drain_ready(server, sender, wire, sizeof(wire));
    assert(strstr(wire, " 401 sender reserved :"));
    drain_ready(server, recipient, wire, sizeof(wire));
    assert(!strstr(wire, "private-body"));

    /* A fresh join and the model-facing room snapshot contain no DM bodies. */
    send_text(recipient, "JOIN #lab\r\n");
    wait_wire(server, recipient, wire, sizeof(wire), " BATCH -");
    assert(!strstr(wire, "private-body"));
    struct snag_buf snapshot = {.max = SNAG_MAX_IRC_SNAPSHOT};
    assert(snag_irc_snapshot(server, &snapshot, error, sizeof(error)) == 0);
    assert(snag_buf_terminate(&snapshot) == 0);
    assert(!strstr((char *)snapshot.data, "private-body"));
    assert(capture.events[SNAG_IRC_MESSAGE] == messages);
    assert(!capture.private_trace);
    snag_buf_free(&snapshot);
    snag_socket_close(unregistered);
    snag_socket_close(observer);
    snag_socket_close(recipient);
    snag_socket_close(sender);
    snag_irc_close(server);
    snag_config_free(&config);
}

static void
test_private_hosted_identities(void)
{
    struct snag_config config;
    struct capture capture = {.retain_conversations = true};
    unsigned short port = free_port();
    char wire[32768u];
    char error[256u] = {0};
    init_server_config(&config, port);
    struct snag_irc *server = open_server(&config, &capture);
    assert(snag_irc_bind_conversations(server, NULL) == 0);
    snag_socket sender = connect_local(port, false);
    register_peer(server, sender, "peer", false, wire, sizeof(wire));
    send_text(sender, "CAP REQ :echo-message\r\nPART #lab\r\n"
        "PRIVMSG operator :private-body operator\r\n");
    wait_wire(server, sender, wire, sizeof(wire), " :private-body operator\r\n");
    struct snag_irc_event operator = capture.query[SNAG_IRC_OPERATOR];
    assert(operator.routed && !operator.room[0] && !operator.stream[0]);
    assert(!strcmp(operator.route.peer, "peer") && !strcmp(operator.route.target, "operator"));
    assert(!snag_irc_event_model_visible(&operator));
    assert(capture.query_count[SNAG_IRC_AGENT] == 0u);
    send_text(sender, "PRIVMSG agent :\001ACTION private-body action\001\r\n");
    wait_wire(server, sender, wire, sizeof(wire), " :\001ACTION private-body action\001\r\n");
    struct snag_irc_event agent = capture.query[SNAG_IRC_AGENT];
    assert(agent.route.action && !strcmp(agent.text, "private-body action"));
    assert(snag_irc_event_model_visible(&agent));
    assert(!strcmp(agent.route.connection, operator.route.connection));
    assert(strcmp(agent.route.conversation, operator.route.conversation));
    send_text(sender, "NOTICE operator :private-body notice\r\n");
    wait_wire(server, sender, wire, sizeof(wire), " :private-body notice\r\n");
    assert(capture.query[SNAG_IRC_OPERATOR].kind == SNAG_IRC_NOTICE);
    assert(!strcmp(capture.query[SNAG_IRC_OPERATOR].route.conversation,
        operator.route.conversation));

    /* A verified nick change outside the channel preserves both queries. */
    send_text(sender, "NICK renamed\r\n");
    drain_ready(server, sender, wire, sizeof(wire));
    for (size_t role = 0u; role < 2u; ++role) {
        assert(capture.query[role].kind == SNAG_IRC_NICK);
        assert(!strcmp(capture.query[role].nick, "peer"));
        assert(!strcmp(capture.query[role].route.peer, "renamed"));
    }
    assert(!strcmp(capture.query[SNAG_IRC_AGENT].route.conversation, agent.route.conversation));
    send_text(sender, "PRIVMSG agent :private-body renamed\r\n");
    wait_wire(server, sender, wire, sizeof(wire), " :private-body renamed\r\n");
    assert(!strcmp(capture.query[SNAG_IRC_AGENT].route.conversation, agent.route.conversation));
    assert(!capture.private_trace);
    assert(snag_irc_conversations_valid(capture.conversations, capture.sequence + 1u));
    struct snag_buf snapshot = {.max = SNAG_MAX_IRC_SNAPSHOT};
    assert(snag_irc_snapshot(server, &snapshot, error, sizeof(error)) == 0);
    assert(snag_buf_terminate(&snapshot) == 0);
    assert(!strstr((char *)snapshot.data, "private-body"));
    snag_buf_free(&snapshot);
    assert(snag_socket_close(sender) == 0);
    unsigned int quits = capture.events[SNAG_IRC_QUIT];
    wait_pair_event(NULL, server, &capture, SNAG_IRC_QUIT, quits + 2u);
    assert(capture.query[SNAG_IRC_AGENT].kind == SNAG_IRC_QUIT);
    sender = connect_local(port, false);
    register_peer(server, sender, "renamed", false, wire, sizeof(wire));
    send_text(sender, "PRIVMSG agent :private-body reclaimed\r\n");
    drain_ready(server, sender, wire, sizeof(wire));
    assert(strcmp(capture.query[SNAG_IRC_AGENT].route.conversation, agent.route.conversation));
    agent = capture.query[SNAG_IRC_AGENT];
    json_t *saved = json_incref(capture.conversations);
    snag_irc_close(server);
    snag_socket_close(sender);
    json_decref(capture.conversations);
    memset(&capture, 0, sizeof(capture));
    capture.retain_conversations = true;
    capture.conversations = json_incref(saved);
    capture.sequence = 1000u;

    /* Restore binds the same endpoint ID, but a nick alone cannot resume a peer. */
    server = open_server(&config, &capture);
    assert(snag_irc_bind_conversations(server, saved) == 0);
    assert(snag_irc_restore_event(server, &agent) == 0);
    sender = connect_local(port, false);
    register_peer(server, sender, "renamed", false, wire, sizeof(wire));
    assert(!strstr(wire, "private-body"));
    send_text(sender, "PRIVMSG agent :private-body after-resume\r\n");
    drain_ready(server, sender, wire, sizeof(wire));
    assert(!strcmp(capture.query[SNAG_IRC_AGENT].route.connection, agent.route.connection));
    assert(capture.query[SNAG_IRC_AGENT].route.generation == agent.route.generation + 1u);
    assert(strcmp(capture.query[SNAG_IRC_AGENT].route.conversation, agent.route.conversation));
    snag_socket_close(sender);
    snag_irc_close(server);
    snag_config_free(&config);
    json_decref(saved);
    json_decref(capture.conversations);
}

static void
test_private_client_identities(void)
{
    struct snag_config config;
    struct capture capture = {.retain_conversations = true};
    unsigned short port;
    snag_socket listener = listen_local(&port);
    snag_socket peers[2u];
    char address[64u];
    char wire[32768u];
    char error[256u] = {0};
    struct snag_irc *client;
    endpoint(address, port);
    init_client_config(&config, address, "agent", "operator");
    assert(snag_irc_open(&client, &config, "/client", capture_event,
        capture_trace, &capture, error, sizeof(error)) == 0);
    assert(snag_irc_bind_conversations(client, NULL) == 0);
    tick(client, 5u);
    for (size_t i = 0u; i < 2u; ++i) {
        snag_socket_event ready = {listener, SNAG_NET_READ, 0};
        assert(snag_socket_poll(&ready, 1u, 1000) > 0);
        snag_socket fd = snag_socket_accept(listener);
        assert(fd != SNAG_SOCKET_INVALID);
        snag_socket_nodelay(fd);
        drain_ready(client, fd, wire, sizeof(wire));
        bool model = strstr(wire, "NICK agent\r\n") != NULL;
        peers[model ? SNAG_IRC_AGENT : SNAG_IRC_OPERATOR] = fd;
        assert(strstr(wire, "CAP LS 302\r\n"));
        assert(!strstr(wire, "CAP REQ") && !strstr(wire, "CAP END"));
        send_text(fd, ":fake CAP * LS * :batch server-time\r\n");
        drain_ready(client, fd, wire, sizeof(wire));
        assert(!strstr(wire, "CAP REQ") && !strstr(wire, "CAP END"));
        send_text(fd, ":fake CAP * LS :unknown message-tags=value\r\n");
        wait_wire(client, fd, wire, sizeof(wire), "CAP REQ :batch server-time message-tags\r\n");
        assert(!strstr(wire, SNAJPAGENT_NAME "/catchup"));
        assert(!strstr(wire, "CAP END"));
        send_text(fd, model ? ":fake CAP * ACK :batch server-time message-tags\r\n" :
            ":fake CAP * NAK :batch server-time message-tags\r\n");
        wait_wire(client, fd, wire, sizeof(wire), "CAP END\r\n");
        send_text(fd, model ? ":fake 001 agent :welcome\r\n"
            ":fake 005 agent CASEMAPPING=ascii :supported\r\n:fake 376 agent :end\r\n" :
            ":fake 001 operator :welcome\r\n:fake 005 operator CASEMAPPING=ascii :supported\r\n"
            ":fake 376 operator :end\r\n");
    }
    tick(client, 5u);
    send_text(peers[SNAG_IRC_OPERATOR],
        ":peer[!u@host PRIVMSG operator :private-body operator\r\n");
    drain_ready(client, peers[SNAG_IRC_OPERATOR], wire, sizeof(wire));
    assert(capture.query_count[SNAG_IRC_OPERATOR] == 1u);
    assert(capture.query_count[SNAG_IRC_AGENT] == 0u);
    struct snag_irc_event first = capture.query[SNAG_IRC_OPERATOR];
    send_text(peers[SNAG_IRC_OPERATOR],
        ":peer{!u@host PRIVMSG operator :private-body distinct\r\n");
    drain_ready(client, peers[SNAG_IRC_OPERATOR], wire, sizeof(wire));
    assert(strcmp(first.route.conversation, capture.query[SNAG_IRC_OPERATOR].route.conversation));
    send_text(peers[SNAG_IRC_OPERATOR], ":PEER[!u@host NICK :renamed\r\n"
        ":renamed!u@host PRIVMSG OPERATOR :private-body renamed\r\n"
        ":peer!u@host PRIVMSG agent :private-body wrong-link\r\n");
    drain_ready(client, peers[SNAG_IRC_OPERATOR], wire, sizeof(wire));
    assert(!strcmp(first.route.conversation, capture.query[SNAG_IRC_OPERATOR].route.conversation));
    assert(!strcmp(capture.query[SNAG_IRC_OPERATOR].route.peer, "renamed"));
    assert(!strcmp(capture.query[SNAG_IRC_OPERATOR].text, "private-body renamed"));
    assert(capture.query_count[SNAG_IRC_AGENT] == 0u);
    send_text(peers[SNAG_IRC_AGENT], "@msgid=one\\:two\\sthree "
        ":peer!u@host PRIVMSG agent :private-body agent\r\n");
    drain_ready(client, peers[SNAG_IRC_AGENT], wire, sizeof(wire));
    assert(capture.query_count[SNAG_IRC_AGENT] == 1u);
    assert(!strcmp(capture.query[SNAG_IRC_AGENT].route.target, "agent"));
    assert(!strcmp(capture.query[SNAG_IRC_AGENT].route.source, "one;two three"));
    send_text(peers[SNAG_IRC_AGENT], ":fake BATCH +query chathistory peer\r\n"
        "@batch=query :peer!u@host PRIVMSG agent :private-body historical\r\n"
        ":fake BATCH -query\r\n");
    drain_ready(client, peers[SNAG_IRC_AGENT], wire, sizeof(wire));
    assert(capture.query[SNAG_IRC_AGENT].historical);
    send_text(peers[SNAG_IRC_OPERATOR], ":fake BATCH +private chathistory peer\r\n"
        "@batch=private;saj-id=11111111111111111111111111111111:1;saj-kind=message "
        ":peer!u@host PRIVMSG operator :private-body tagged-history\r\n"
        ":fake BATCH -private\r\n");
    drain_ready(client, peers[SNAG_IRC_OPERATOR], wire, sizeof(wire));
    assert(capture.last_message.routed && capture.last_message.route.kind == SNAG_IRC_QUERY);
    assert(capture.last_message.historical && !capture.last_message.room[0]);
    assert(!snag_irc_event_model_visible(&capture.last_message));
    unsigned int messages = capture.events[SNAG_IRC_MESSAGE];
    send_text(peers[SNAG_IRC_OPERATOR], ":fake 005 operator SAJROOM=#lab :supported\r\n"
        ":fake BATCH +public chathistory #lab\r\n"
        "@batch=public;saj-id=11111111111111111111111111111111:2;saj-kind=message "
        ":peer!u@host PRIVMSG agent :private-body wrong-history-link\r\n"
        ":fake BATCH -public\r\n");
    drain_ready(client, peers[SNAG_IRC_OPERATOR], wire, sizeof(wire));
    assert(capture.events[SNAG_IRC_MESSAGE] == messages);
    unsigned int before = capture.query_count[SNAG_IRC_AGENT];
    char long_message[7000u] = ":peer!u@host PRIVMSG agent :";
    size_t used = strlen(long_message);
    for (size_t i = 0u; i < 2048u; ++i) {
        memcpy(long_message + used, "界", 3u);
        used += 3u;
    }
    memcpy(long_message + used, "\r\n", 3u);
    send_text(peers[SNAG_IRC_AGENT], long_message);
    drain_ready(client, peers[SNAG_IRC_AGENT], wire, sizeof(wire));
    assert(capture.query_count[SNAG_IRC_AGENT] == before + 2u);
    assert(strlen(capture.query[SNAG_IRC_AGENT].text) == 2049u);
    send_text(peers[SNAG_IRC_OPERATOR], ":operator!u@host NICK :operator[\r\n"
        ":operator{!u@host NICK :unrelated\r\n"
        ":peer!u@host PRIVMSG operator[ :private-body still-local\r\n");
    drain_ready(client, peers[SNAG_IRC_OPERATOR], wire, sizeof(wire));
    assert(!strcmp(snag_irc_operator_nick(client), "operator["));
    assert(!strcmp(capture.query[SNAG_IRC_OPERATOR].text, "private-body still-local"));
    /* External endpoints use their line budget even without a joined room. */
    struct snag_irc_destinations destinations;
    snag_irc_destinations(client, &destinations);
    struct snag_irc_query_target target;
    assert(snag_irc_query_open(client, destinations.items[0].target.id, SNAG_IRC_AGENT, "peer",
        &target, error, sizeof(error)) == 0);
    char outgoing[1801u];
    for (size_t i = 0u; i < sizeof(outgoing) - 1u; i += 3u) memcpy(outgoing + i, "界", 3u);
    outgoing[sizeof(outgoing) - 1u] = '\0';
    assert(snag_irc_query_send(client, &target, SNAG_IRC_MESSAGE, outgoing,
        true, NULL, error, sizeof(error)) == 0);
    drain_ready(client, peers[SNAG_IRC_AGENT], wire, sizeof(wire));
    size_t chunks = 0u;
    for (char *p = wire; *p;) {
        char *end = strstr(p, "\r\n");
        assert(end && (size_t)(end + 2u - p) <= 512u);
        assert(snag_utf8_valid((unsigned char *)p, (size_t)(end - p), true));
        if (!strncmp(p, "PRIVMSG peer :\001ACTION ", 22u)) ++chunks;
        p = end + 2u;
    }
    assert(chunks == 4u);
    assert(capture.delivery[SNAG_IRC_AGENT][SNAG_IRC_WRITTEN] == chunks);
    assert(capture.delivery[SNAG_IRC_AGENT][SNAG_IRC_ACKNOWLEDGED] == 0u);
    send_text(peers[SNAG_IRC_AGENT], ":fake 005 agent LINELEN=1024 :supported\r\n");
    drain_ready(client, peers[SNAG_IRC_AGENT], wire, sizeof(wire));
    assert(snag_irc_query_send(client, &target, SNAG_IRC_MESSAGE, outgoing,
        false, NULL, error, sizeof(error)) == 0);
    drain_ready(client, peers[SNAG_IRC_AGENT], wire, sizeof(wire));
    assert(strstr(wire, "\r\n") - wire > 512);
    assert(strstr(wire, "\r\n") - wire <= 1022);
    assert(!capture.private_trace);
    assert(snag_irc_conversations_valid(capture.conversations, capture.sequence + 1u));
    uint64_t generation = first.route.generation;
    snag_socket_close(peers[SNAG_IRC_OPERATOR]);
    unsigned int disconnected = capture.events[SNAG_IRC_DISCONNECTED];
    wait_pair_event(NULL, client, &capture, SNAG_IRC_DISCONNECTED, disconnected + 1u);
    send_text(peers[SNAG_IRC_AGENT], ":peer!u@host PRIVMSG agent :private-body new-epoch\r\n");
    drain_ready(client, peers[SNAG_IRC_AGENT], wire, sizeof(wire));
    assert(capture.query[SNAG_IRC_AGENT].route.generation > generation);
    assert(!capture.query[SNAG_IRC_AGENT].historical);
    snag_socket_close(peers[SNAG_IRC_AGENT]);
    snag_socket_close(listener);
    snag_irc_close(client);
    snag_config_free(&config);
    json_decref(capture.conversations);
}

static uint32_t
private_destination(struct snag_irc *irc)
{
    struct snag_irc_destinations destinations;
    snag_irc_destinations(irc, &destinations);
    assert(destinations.count == 1u);
    return destinations.items[0].target.id;
}

static void
channel_fixture_connect(struct snag_irc *client, snag_socket listener, snag_socket peers[2u],
                        bool resumed)
{
    char wire[8192u];
    tick(client, 5u);
    for (size_t i = 0u; i < 2u; ++i) {
        snag_socket_event ready = {listener, SNAG_NET_READ, 0};
        assert(snag_socket_poll(&ready, 1u, 1000) > 0);
        snag_socket fd = snag_socket_accept(listener);
        assert(fd != SNAG_SOCKET_INVALID);
        snag_socket_nodelay(fd);
        drain_ready(client, fd, wire, sizeof(wire));
        bool model = strstr(wire, "NICK agent\r\n") != NULL;
        peers[model ? SNAG_IRC_AGENT : SNAG_IRC_OPERATOR] = fd;
        send_text(fd, model ? ":fake 001 agent :welcome\r\n"
            ":fake 005 agent SAJROOM=#lab :supported\r\n:fake 376 agent :end\r\n" :
            ":fake 001 operator :welcome\r\n"
            ":fake 005 operator SAJROOM=#lab :supported\r\n:fake 376 operator :end\r\n");
        wait_wire(client, fd, wire, sizeof(wire), "JOIN #lab\r\n");
        assert(!strstr(wire, "JOIN #closed\r\n"));
        if (resumed && !model) assert(strstr(wire, "JOIN #side\r\n"));
        if (model) assert(!strstr(wire, "JOIN #side\r\n"));
        send_text(fd, model ? ":agent!u@fake JOIN #lab\r\n" :
            ":operator!u@fake JOIN #lab\r\n");
        if (resumed && !model) send_text(fd, ":operator!u@fake JOIN #side\r\n");
    }
    tick(client, 10u);
}

static struct snag_irc_query_target
channel_fixture_scope(struct snag_irc *client)
{
    struct snag_irc_destinations destinations;
    struct snag_irc_scopes scopes;
    snag_irc_destinations(client, &destinations);
    snag_irc_capture_scopes(&destinations, &scopes);
    assert(scopes.count == 1u);
    return scopes.items[0].target;
}

static void
test_channel_routes(void)
{
    struct snag_config config;
    struct capture capture = {.retain_conversations = true};
    unsigned short port;
    snag_socket listener = listen_local(&port);
    snag_socket peers[2u];
    char address[64u];
    char wire[8192u];
    char error[256u] = {0};
    struct snag_irc *client;
    endpoint(address, port);
    init_client_config(&config, address, "agent", "operator");
    assert(snag_irc_open(&client, &config, "/client", capture_event,
        capture_trace, &capture, error, sizeof(error)) == 0);
    assert(snag_irc_bind_conversations(client, NULL) == 0);
    channel_fixture_connect(client, listener, peers, false);
    struct snag_irc_query_target scope = channel_fixture_scope(client);
    struct snag_irc_channel_target lab, side, current, closed;
    assert(snag_irc_channel_open(client, &scope, "#LAB", false, &lab, error, sizeof(error)) == 0);
    assert(!strcmp(lab.room, "#lab"));
    struct snag_irc_channel_target wrong_role = lab;
    wrong_role.identity = SNAG_IRC_AGENT;
    assert(snag_irc_channel_action(client, &wrong_role, SNAG_IRC_CHANNEL_NAMES,
        NULL, error, sizeof(error)) < 0);
    assert(snag_irc_channel_open(client, &scope, "#missing", false,
        &side, error, sizeof(error)) < 0);
    assert(snag_irc_channel_open(client, &scope, "#side\nJOIN #wrong", true,
        &side, error, sizeof(error)) < 0);
    assert(snag_irc_channel_open(client, &scope, "#side", true,
        &side, error, sizeof(error)) == 0);
    wait_wire(client, peers[SNAG_IRC_OPERATOR], wire, sizeof(wire), "JOIN #side\r\n");
    assert(snag_irc_channel_action(client, &side, SNAG_IRC_CHANNEL_NAMES,
        NULL, error, sizeof(error)) < 0);
    send_text(peers[SNAG_IRC_OPERATOR], ":operator!u@fake JOIN #side\r\n");
    tick(client, 5u);
    assert(snag_irc_channel_action(client, &side, SNAG_IRC_CHANNEL_TOPIC,
        "é界 channel topic", error, sizeof(error)) == 0);
    wait_wire(client, peers[SNAG_IRC_OPERATOR], wire, sizeof(wire),
        "TOPIC #side :é界 channel topic\r\n");
    assert(snag_irc_channel_action(client, &side, SNAG_IRC_CHANNEL_TOPIC,
        "bad\r\nPRIVMSG #lab :wrong", error, sizeof(error)) < 0);
    char large[513u];
    memset(large, 'x', sizeof(large) - 1u);
    large[sizeof(large) - 1u] = '\0';
    assert(snag_irc_channel_action(client, &side, SNAG_IRC_CHANNEL_TOPIC,
        large, error, sizeof(error)) < 0);
    assert(snag_irc_channel_action(client, &side, SNAG_IRC_CHANNEL_PART,
        "leaving", error, sizeof(error)) == 0);
    assert(snag_irc_channel_action(client, &side, SNAG_IRC_CHANNEL_NAMES,
        NULL, error, sizeof(error)) < 0);
    assert(snag_irc_channel_open(client, &scope, "#side", true,
        &current, error, sizeof(error)) < 0);
    wait_wire(client, peers[SNAG_IRC_OPERATOR], wire, sizeof(wire), "PART #side :leaving\r\n");
    send_text(peers[SNAG_IRC_OPERATOR], ":operator!u@fake PART #side :leaving\r\n");
    tick(client, 5u);
    assert(snag_irc_channel_open(client, &scope, "#side", true,
        &current, error, sizeof(error)) == 0);
    assert(!strcmp(side.conversation, current.conversation));
    assert(strcmp(side.membership, current.membership));
    wait_wire(client, peers[SNAG_IRC_OPERATOR], wire, sizeof(wire), "JOIN #side\r\n");
    send_text(peers[SNAG_IRC_OPERATOR], ":operator!u@fake JOIN #side\r\n");
    tick(client, 5u);
    assert(snag_irc_channel_action(client, &side, SNAG_IRC_CHANNEL_NAMES,
        NULL, error, sizeof(error)) < 0);
    assert(snag_irc_channel_action(client, &lab, SNAG_IRC_CHANNEL_NAMES,
        NULL, error, sizeof(error)) == 0);
    wait_wire(client, peers[SNAG_IRC_OPERATOR], wire, sizeof(wire), "NAMES #lab\r\n");
    assert(snag_irc_channel_action(client, &current, SNAG_IRC_CHANNEL_NAMES,
        NULL, error, sizeof(error)) == 0);
    wait_wire(client, peers[SNAG_IRC_OPERATOR], wire, sizeof(wire), "NAMES #side\r\n");
    send_text(peers[SNAG_IRC_OPERATOR], ":peer!u@fake KICK #side operator :kicked\r\n");
    tick(client, 5u);
    assert(snag_irc_channel_action(client, &current, SNAG_IRC_CHANNEL_NAMES,
        NULL, error, sizeof(error)) < 0);
    assert(snag_irc_channel_open(client, &scope, "#side", true,
        &side, error, sizeof(error)) == 0);
    assert(!strcmp(side.conversation, current.conversation));
    assert(strcmp(side.membership, current.membership));
    wait_wire(client, peers[SNAG_IRC_OPERATOR], wire, sizeof(wire), "JOIN #side\r\n");
    send_text(peers[SNAG_IRC_OPERATOR], ":operator!u@fake JOIN #side\r\n"
        ":operator!u@fake JOIN #closed\r\n:operator!u@fake PART #closed :done\r\n");
    tick(client, 5u);
    assert(snag_irc_channel_open(client, &scope, "#closed", false,
        &closed, error, sizeof(error)) == 0);
    assert(snag_irc_conversations_valid(capture.conversations, capture.sequence + 1u));
    json_t *saved = json_incref(capture.conversations);
    struct snag_buf snapshot = {.max = SNAG_MAX_IRC_SNAPSHOT};
    assert(snag_irc_snapshot(client, &snapshot, error, sizeof(error)) == 0);
    assert(snag_buf_terminate(&snapshot) == 0);
    assert(!strstr((char *)snapshot.data, "channel opened"));
    assert(!strstr((char *)snapshot.data, "part requested"));
    snag_buf_free(&snapshot);
    snag_irc_close(client);
    for (size_t i = 0u; i < 2u; ++i) snag_socket_close(peers[i]);
    assert(snag_irc_open(&client, &config, "/client", capture_event,
        capture_trace, &capture, error, sizeof(error)) == 0);
    assert(snag_irc_bind_conversations(client, saved) == 0);
    channel_fixture_connect(client, listener, peers, true);
    const char *key;
    json_t *entry;
    json_t *connection = json_object_get(saved, side.connection);
    json_object_foreach(json_object_get(connection, "conversations"), key, entry) {
        (void)key;
        struct snag_irc_event event;
        assert(snag_irc_event_record_read("irc_event_v2", json_object_get(entry, "data"),
            &event) == 0);
        if (event.route.kind == SNAG_IRC_CHANNEL)
            assert(snag_irc_restore_event(client, &event) == 0);
    }
    assert(snag_irc_channel_open(client, &scope, "#side", false,
        &current, error, sizeof(error)) < 0);
    scope = channel_fixture_scope(client);
    assert(snag_irc_channel_open(client, &scope, "#side", false,
        &current, error, sizeof(error)) == 0);
    assert(!strcmp(side.connection, current.connection));
    assert(!strcmp(side.conversation, current.conversation));
    assert(strcmp(side.membership, current.membership));
    assert(current.generation > side.generation);
    assert(snag_irc_channel_action(client, &side, SNAG_IRC_CHANNEL_NAMES,
        NULL, error, sizeof(error)) < 0);
    assert(snag_irc_channel_action(client, &current, SNAG_IRC_CHANNEL_NAMES,
        NULL, error, sizeof(error)) == 0);
    wait_wire(client, peers[SNAG_IRC_OPERATOR], wire, sizeof(wire), "NAMES #side\r\n");
    assert(snag_irc_conversations_valid(capture.conversations, capture.sequence + 1u));
    snag_irc_close(client);
    for (size_t i = 0u; i < 2u; ++i) snag_socket_close(peers[i]);
    snag_socket_close(listener);
    snag_config_free(&config);
    json_decref(saved);
    json_decref(capture.conversations);
}

static void
test_private_sends(void)
{
    struct snag_config config;
    struct capture capture = {.retain_conversations = true};
    unsigned short port = free_port();
    char wire[32768u];
    char error[256u] = {0};
    init_server_config(&config, port);
    struct snag_irc *server = open_server(&config, &capture);
    assert(snag_irc_bind_conversations(server, NULL) == 0);
    snag_socket peer = connect_local(port, false);
    register_peer(server, peer, "peer", false, wire, sizeof(wire));
    snag_socket observer = connect_local(port, false);
    register_peer(server, observer, "observer", false, wire, sizeof(wire));
    send_text(peer, "PART #lab\r\n");
    drain_ready(server, peer, wire, sizeof(wire));
    struct snag_irc_query_target target;
    uint32_t destination = private_destination(server);
    assert(snag_irc_query_open(server, destination, SNAG_IRC_OPERATOR, "peer",
        &target, error, sizeof(error)) == 0);
    assert(capture.query[SNAG_IRC_OPERATOR].kind == SNAG_IRC_CONNECTED);
    struct snag_irc_query_target scope = target;
    struct snag_irc_query_target rejected;
    ++scope.generation;
    unsigned int opened = capture.query_count[SNAG_IRC_OPERATOR];
    assert(snag_irc_query_open_frozen(server, &scope, "other-peer", &rejected,
        error, sizeof(error)) < 0 && errno == ESTALE);
    assert(capture.query_count[SNAG_IRC_OPERATOR] == opened);
    drain_ready(server, peer, wire, sizeof(wire));
    assert(!strstr(wire, "PRIVMSG") && !strstr(wire, "NOTICE"));
    struct snag_buf report = {.max = 32768u};
    assert(snag_irc_query_send(server, &target, SNAG_IRC_MESSAGE, "private-body one\nsecond",
        true, &report, error, sizeof(error)) == 0);
    wait_wire(server, peer, wire, sizeof(wire), " :\001ACTION second\001\r\n");
    assert(strstr(wire, ":operator!operator@"));
    assert(strstr(wire, " PRIVMSG peer :\001ACTION private-body one"));
    assert(capture.delivery[SNAG_IRC_OPERATOR][SNAG_IRC_PENDING] == 2u);
    assert(capture.delivery[SNAG_IRC_OPERATOR][SNAG_IRC_WRITTEN] == 2u);
    assert(capture.delivery[SNAG_IRC_OPERATOR][SNAG_IRC_ACKNOWLEDGED] == 2u);
    assert(snag_buf_terminate(&report) == 0);
    assert(strstr((char *)report.data, capture.outgoing[SNAG_IRC_OPERATOR].route.send));
    drain_ready(server, observer, wire, sizeof(wire));
    assert(!strstr(wire, "private-body") && !strstr(wire, "second"));

    struct snag_irc_query_target same;
    assert(snag_irc_query_open(server, destination, SNAG_IRC_OPERATOR, "PEER",
        &same, error, sizeof(error)) == 0);
    assert(!strcmp(same.conversation, target.conversation));
    send_text(peer, "NICK renamed\r\n");
    drain_ready(server, peer, wire, sizeof(wire));
    assert(snag_irc_query_send(server, &target, SNAG_IRC_MESSAGE, "private-body stale",
        false, NULL, error, sizeof(error)) < 0 && errno == ESTALE);
    assert(snag_irc_query_open(server, destination, SNAG_IRC_OPERATOR, "renamed",
        &same, error, sizeof(error)) == 0);
    assert(!strcmp(same.conversation, target.conversation));
    assert(snag_irc_query_send(server, &same, SNAG_IRC_NOTICE, "private-body notice",
        false, NULL, error, sizeof(error)) == 0);
    wait_wire(server, peer, wire, sizeof(wire), " NOTICE renamed :private-body notice\r\n");
    assert(!strstr(wire, "stale"));

    /* The hosted operator and model keep distinct incoming/outgoing records. */
    assert(snag_irc_query_open(server, destination, SNAG_IRC_OPERATOR, "agent",
        &target, error, sizeof(error)) == 0);
    assert(snag_irc_query_send(server, &target, SNAG_IRC_MESSAGE, "private-body local",
        false, NULL, error, sizeof(error)) == 0);
    assert(capture.query[SNAG_IRC_AGENT].route.direction == SNAG_IRC_INCOMING);
    assert(!strcmp(capture.query[SNAG_IRC_AGENT].nick, "operator"));
    assert(capture.outgoing[SNAG_IRC_OPERATOR].route.delivery == SNAG_IRC_ACKNOWLEDGED);
    assert(strcmp(capture.query[SNAG_IRC_AGENT].route.conversation, target.conversation));
    assert(snag_irc_query_open(server, destination, SNAG_IRC_AGENT, "agent",
        &target, error, sizeof(error)) == 0);
    unsigned int before = capture.query_count[SNAG_IRC_AGENT];
    assert(snag_irc_query_send(server, &target, SNAG_IRC_MESSAGE, "private-body self",
        false, NULL, error, sizeof(error)) == 0);
    assert(capture.query_count[SNAG_IRC_AGENT] == before + 2u);
    assert(capture.query[SNAG_IRC_AGENT].local);
    assert(!capture.private_trace);
    assert(snag_irc_conversations_valid(capture.conversations, capture.sequence + 1u));
    snag_buf_free(&report);
    snag_socket_close(observer);
    snag_socket_close(peer);
    snag_irc_close(server);
    snag_config_free(&config);
    json_decref(capture.conversations);
}

static void
test_private_native_receipts(void)
{
    struct snag_config server_config, client_config;
    struct capture server_capture = {.retain_conversations = true};
    struct capture client_capture = {.retain_conversations = true};
    unsigned short port = free_port();
    char address[64u];
    char error[256u] = {0};
    endpoint(address, port);
    init_server_config(&server_config, port);
    struct snag_irc *server = open_server(&server_config, &server_capture);
    assert(snag_irc_bind_conversations(server, NULL) == 0);
    init_client_config(&client_config, address, "remoteagent", "remoteop");
    struct snag_irc *client;
    assert(snag_irc_open(&client, &client_config, "/client", capture_event,
        capture_trace, &client_capture, error, sizeof(error)) == 0);
    assert(snag_irc_bind_conversations(client, NULL) == 0);
    wait_pair_event(server, client, &client_capture, SNAG_IRC_HISTORY_READY, 1u);
    uint32_t destination = private_destination(client);
    struct snag_irc_query_target target;
    assert(snag_irc_query_open(client, destination, SNAG_IRC_AGENT, "operator",
        &target, error, sizeof(error)) == 0);
    assert(snag_irc_query_send(client, &target, SNAG_IRC_MESSAGE,
        "private-body same\nprivate-body same", false, NULL, error, sizeof(error)) == 0);
    uint64_t until = snag_monotonic_ms() + 1000u;
    while (client_capture.delivery[SNAG_IRC_AGENT][SNAG_IRC_ACKNOWLEDGED] < 2u) {
        assert(snag_monotonic_ms() < until);
        tick(server, 1u);
        tick(client, 1u);
    }
    assert(server_capture.query_count[SNAG_IRC_OPERATOR] == 2u);
    assert(client_capture.delivery[SNAG_IRC_AGENT][SNAG_IRC_PENDING] == 2u);
    assert(client_capture.delivery[SNAG_IRC_AGENT][SNAG_IRC_WRITTEN] == 2u);
    assert(snag_irc_query_open(client, destination, SNAG_IRC_AGENT, "missing",
        &target, error, sizeof(error)) == 0);
    assert(snag_irc_query_send(client, &target, SNAG_IRC_MESSAGE, "private-body nonexistent",
        false, NULL, error, sizeof(error)) == 0);
    until = snag_monotonic_ms() + 1000u;
    while (!client_capture.delivery[SNAG_IRC_AGENT][SNAG_IRC_FAILED]) {
        assert(snag_monotonic_ms() < until);
        tick(server, 1u);
        tick(client, 1u);
    }
    assert(!strcmp(client_capture.outgoing[SNAG_IRC_AGENT].route.peer, "missing"));
    assert(!server_capture.private_trace && !client_capture.private_trace);
    assert(snag_irc_query_open(client, destination, SNAG_IRC_AGENT, "operator",
        &target, error, sizeof(error)) == 0);
    /* Hold engine admission at the receiver: a complete write without its
     * receipt becomes uncertain when the endpoint is explicitly removed. */
    assert(snag_irc_query_send(client, &target, SNAG_IRC_MESSAGE, "private-body unconfirmed",
        false, NULL, error, sizeof(error)) == 0);
    until = snag_monotonic_ms() + 1000u;
    while (client_capture.delivery[SNAG_IRC_AGENT][SNAG_IRC_WRITTEN] < 4u) {
        assert(snag_monotonic_ms() < until);
        tick(client, 1u);
    }
    assert(snag_irc_remove(client, false, address, error, sizeof(error)) == 0);
    assert(client_capture.delivery[SNAG_IRC_AGENT][SNAG_IRC_UNCERTAIN] == 1u);
    assert(snag_irc_query_send(client, &target, SNAG_IRC_MESSAGE, "private-body removed",
        false, NULL, error, sizeof(error)) < 0 && errno == ESTALE);
    snag_irc_close(client);
    snag_irc_close(server);
    snag_config_free(&client_config);
    snag_config_free(&server_config);
    json_decref(client_capture.conversations);
    json_decref(server_capture.conversations);
}

static void
test_private_commit_failure(void)
{
    struct snag_config config;
    struct capture capture = {0};
    unsigned short port = free_port();
    char wire[32768u];
    char error[256u] = {0};
    init_server_config(&config, port);
    struct snag_irc *server = open_server(&config, &capture);
    assert(snag_irc_bind_conversations(server, NULL) == 0);
    snag_socket sender = connect_local(port, false);
    register_peer(server, sender, "peer", false, wire, sizeof(wire));
    send_text(sender, "CAP REQ :echo-message\r\n");
    drain_ready(server, sender, wire, sizeof(wire));
    capture.fail_message = true;
    send_text(sender, "PRIVMSG agent :private-body uncommitted\r\n");
    uint64_t until = snag_monotonic_ms() + 1000u;
    while (snag_irc_tick(server, 2, error, sizeof(error)) == 0)
        assert(snag_monotonic_ms() < until);
    assert(capture.query_count[SNAG_IRC_AGENT] == 1u);
    (void)drain(sender, wire, sizeof(wire));
    assert(!strstr(wire, "private-body"));
    /* Closing also joins an owner waiting for the failed durable callback. */
    snag_irc_close(server);
    snag_socket_close(sender);
    snag_config_free(&config);
}

static snag_socket
client_channel_state(struct snag_irc *client, struct capture *capture,
                     snag_socket operator_fd, snag_socket agent_fd, snag_socket listener)
{
    unsigned int joins = capture->events[SNAG_IRC_JOIN];
    send_text(operator_fd, ":OpFinal!u@fake JOIN #side\r\n"
        ":fake 353 OpFinal = #side :@OpFinal final friend sidepeer\r\n"
        ":fake 366 OpFinal #side :end\r\n"
        ":fake 332 OpFinal #side :side topic\r\n");
    send_text(agent_fd, ":final!u@fake JOIN #side\r\n"
        ":fake 353 final = #side :@OpFinal final friend sidepeer\r\n"
        ":fake 366 final #side :end\r\n");
    wait_pair_event(NULL, client, capture, SNAG_IRC_JOIN, joins + 1u);
    tick(client, 10u);
    assert(capture->events[SNAG_IRC_DISCONNECTED] == 0u);

    char error[256] = {0};
    struct snag_buf state = {.max = SNAG_MAX_IRC_SNAPSHOT};
    assert(snag_irc_state(client, &state, error, sizeof(error)) == 0);
    assert(snag_buf_terminate(&state) == 0);
    assert(strstr((const char *)state.data, "#side"));
    assert(strstr((const char *)state.data, "side topic"));
    assert(strstr((const char *)state.data, "sidepeer"));
    snag_buf_reset(&state);
    struct snag_irc_destinations destinations;
    snag_irc_destinations(client, &destinations);
    assert(destinations.count == 1u && !strcmp(destinations.items[0].room, "#lab"));
    assert(destinations.items[0].joined && !strstr(destinations.items[0].nicks, "sidepeer"));

    unsigned int messages = capture->events[SNAG_IRC_MESSAGE];
    send_text(operator_fd, ":sidepeer!u@fake PRIVMSG #side :side message\r\n");
    send_text(agent_fd, ":sidepeer!u@fake PRIVMSG #side :side message\r\n");
    wait_pair_event(NULL, client, capture, SNAG_IRC_MESSAGE, messages + 1u);
    tick(client, 5u);
    assert(capture->events[SNAG_IRC_MESSAGE] == messages + 1u);
    assert(!strcmp(capture->last_message.room, "#side"));
    send_text(operator_fd, ":outsider!u@fake PRIVMSG #lab :main message\r\n");
    wait_pair_event(NULL, client, capture, SNAG_IRC_MESSAGE, messages + 2u);
    assert(!strcmp(capture->last_message.room, "#lab"));

    unsigned int nicks = capture->events[SNAG_IRC_NICK];
    send_text(operator_fd, ":outsider!u@fake JOIN #side\r\n"
        ":outsider!u@fake NICK :bothrooms\r\n");
    wait_pair_event(NULL, client, capture, SNAG_IRC_NICK, nicks + 2u);
    unsigned int quits = capture->events[SNAG_IRC_QUIT];
    send_text(operator_fd, ":bothrooms!u@fake QUIT :gone\r\n");
    wait_pair_event(NULL, client, capture, SNAG_IRC_QUIT, quits + 2u);
    send_text(operator_fd, ":sidepeer!u@fake KICK #side OpFinal :removed\r\n"
        ":sidepeer!u@fake PRIVMSG #side :after removal\r\n");
    tick(client, 10u);
    assert(capture->events[SNAG_IRC_MESSAGE] == messages + 2u);
    snag_irc_destinations(client, &destinations);
    assert(destinations.items[0].joined);

    /* Channel errors are scoped failures; the same connection stays usable. */
    unsigned int notices = capture->events[SNAG_IRC_NOTICE];
    send_text(operator_fd, ":fake 403 OpFinal #missing :No such channel\r\n"
        ":fake 404 OpFinal #lab :Cannot send to channel\r\n"
        ":fake 471 OpFinal #full :Channel is full\r\n"
        ":sidepeer!u@fake PRIVMSG #lab :after channel errors\r\n");
    wait_pair_event(NULL, client, capture, SNAG_IRC_MESSAGE, messages + 3u);
    assert(capture->events[SNAG_IRC_NOTICE] == notices + 3u);
    assert(capture->events[SNAG_IRC_DISCONNECTED] == 0u);
    assert(!strcmp(capture->last_message.text, "after channel errors"));

    /* The advertised channel prefixes and mapping also apply to non-default rooms. */
    joins = capture->events[SNAG_IRC_JOIN];
    send_text(operator_fd, ":fake 005 OpFinal CHANTYPES=#& CASEMAPPING=ascii :supported\r\n"
        ":OpFinal!u@fake JOIN &keep\r\n"
        ":fake 353 OpFinal = &keep :@OpFinal peer[ peer{\r\n"
        ":fake 366 OpFinal &keep :end\r\n"
        ":peer[!u@fake PART &KEEP :bye\r\n"
        ":fake 332 OpFinal &keep :old topic\r\n"
        ":OpFinal!u@fake JOIN #side\r\n"
        ":OpFinal!u@fake PART #side :leave\r\n"
        ":fake 353 OpFinal = #side :OpFinal phantom\r\n"
        ":fake 366 OpFinal #side :end\r\n"
        ":phantom!u@fake PRIVMSG #side :names alone cannot join\r\n");
    wait_pair_event(NULL, client, capture, SNAG_IRC_JOIN, joins + 2u);
    tick(client, 10u);
    assert(capture->events[SNAG_IRC_MESSAGE] == messages + 3u);
    assert(snag_irc_state(client, &state, error, sizeof(error)) == 0);
    assert(snag_buf_terminate(&state) == 0);
    assert(strstr((const char *)state.data, "peer{"));
    assert(!strstr((const char *)state.data, "peer["));
    assert(!strstr((const char *)state.data, "phantom"));
    snag_buf_reset(&state);

    assert(snag_socket_close(operator_fd) == 0);
    wait_pair_event(NULL, client, capture, SNAG_IRC_DISCONNECTED, 1u);
    snag_socket_event ready = {listener, SNAG_NET_READ, 0};
    assert(snag_socket_poll(&ready, 1u, 3000) > 0);
    operator_fd = snag_socket_accept(listener);
    assert(operator_fd != SNAG_SOCKET_INVALID);
    snag_socket_nodelay(operator_fd);
    char wire[8192u];
    wait_wire(client, operator_fd, wire, sizeof(wire), "USER OpFinal");
    send_text(operator_fd, ":fake 001 OpFinal :welcome\r\n"
        ":fake 005 OpFinal CHANTYPES=#& SAJROOM=#lab :supported\r\n"
        ":fake 376 OpFinal :end\r\n");
    wait_wire(client, operator_fd, wire, sizeof(wire), "JOIN #lab\r\n");
    assert(strstr(wire, "JOIN &keep\r\n"));
    assert(!strstr(wire, "JOIN #side\r\n"));
    send_text(operator_fd, ":OpFinal!u@fake JOIN #lab\r\n"
        ":OpFinal!u@fake JOIN &keep\r\n"
        ":fake 353 OpFinal = &keep :OpFinal fresh\r\n"
        ":fake 366 OpFinal &keep :end\r\n"
        ":fresh!u@fake PRIVMSG &keep :after reconnect\r\n");
    wait_pair_event(NULL, client, capture, SNAG_IRC_MESSAGE, messages + 4u);
    assert(!strcmp(capture->last_message.room, "&keep"));
    assert(snag_irc_state(client, &state, error, sizeof(error)) == 0);
    assert(snag_buf_terminate(&state) == 0);
    assert(strstr((const char *)state.data, "fresh"));
    assert(!strstr((const char *)state.data, "peer{"));
    assert(!strstr((const char *)state.data, "old topic"));
    snag_buf_reset(&state);

    /* Membership remains complete when the bounded display summary is full. */
    send_text(operator_fd, ":OpFinal!u@fake JOIN #large\r\n");
    for (unsigned int batch = 0u; batch < 128u; ++batch) {
        struct snag_buf names = {.max = 512u};
        assert(snag_buf_printf(&names, ":fake 353 OpFinal = #large :") == 0);
        for (unsigned int item = 0u; item < 16u; ++item)
            assert(snag_buf_printf(&names, "member-%04u-abcdefghijkl ", batch * 16u + item) == 0);
        assert(snag_buf_append(&names, "\r\n", 2u) == 0 && snag_buf_terminate(&names) == 0);
        send_text(operator_fd, (const char *)names.data);
        snag_buf_free(&names);
        if (batch % 8u == 7u) tick(client, 1u);
    }
    nicks = capture->events[SNAG_IRC_NICK];
    send_text(operator_fd, ":fake 366 OpFinal #large :end\r\n"
        ":member-2047-abcdefghijkl!u@fake NICK :last-member\r\n");
    wait_pair_event(NULL, client, capture, SNAG_IRC_NICK, nicks + 1u);
    assert(!strcmp(capture->last_nick.room, "#large"));
    assert(!strcmp(capture->last_nick.text, "last-member"));
    assert(snag_irc_state(client, &state, error, sizeof(error)) == 0);
    assert(snag_buf_terminate(&state) == 0);
    assert(strstr((const char *)state.data, "IRC state abbreviated"));
    snag_buf_free(&state);

    char reason[6002u] = "x";
    for (size_t i = 1u; i < sizeof(reason) - 1u; i += 3u) memcpy(reason + i, "界", 3u);
    reason[sizeof(reason) - 1u] = '\0';
    struct snag_buf long_wire = {.max = SNAG_IRC_LINE_MAX};
    assert(snag_buf_printf(&long_wire, ":fresh!u@fake KICK &keep OpFinal :%s\r\n", reason) == 0);
    assert(snag_buf_terminate(&long_wire) == 0);
    unsigned int parts = capture->events[SNAG_IRC_PART];
    send_text(operator_fd, (const char *)long_wire.data);
    wait_pair_event(NULL, client, capture, SNAG_IRC_PART, parts + 1u);
    assert(strlen(capture->last_part.text) > 4000u);
    assert(snag_utf8_valid((const unsigned char *)capture->last_part.text,
        strlen(capture->last_part.text), true));
    snag_buf_reset(&long_wire);
    assert(snag_buf_printf(&long_wire, ":fake 404 OpFinal #lab :%s\r\n", reason) == 0);
    assert(snag_buf_terminate(&long_wire) == 0);
    notices = capture->events[SNAG_IRC_NOTICE];
    send_text(operator_fd, (const char *)long_wire.data);
    wait_pair_event(NULL, client, capture, SNAG_IRC_NOTICE, notices + 1u);
    assert(strlen(capture->last_notice.text) > 4000u);
    assert(snag_utf8_valid((const unsigned char *)capture->last_notice.text,
        strlen(capture->last_notice.text), true));
    snag_buf_free(&long_wire);

    send_text(agent_fd, ":fake 005 final CASEMAPPING=ascii :supported\r\n"
        ":final!u@fake NICK :Attention[\r\n");
    tick(client, 10u);
    assert(snag_irc_mentions_agent(client, "local", "@attention[ hello"));
    assert(!snag_irc_mentions_agent(client, "local", "@attention{ hello"));
    send_text(agent_fd, ":fake 005 Attention[ CASEMAPPING=rfc1459-strict :supported\r\n"
        ":Attention[!u@fake NICK :Attention^\r\n");
    tick(client, 10u);
    assert(snag_irc_mentions_agent(client, "local", "attention^: hello"));
    assert(!snag_irc_mentions_agent(client, "local", "attention~: hello"));
    send_text(agent_fd, ":fake 005 Attention^ CASEMAPPING=unrecognized :supported\r\n");
    tick(client, 10u);
    assert(snag_irc_mentions_agent(client, "local", "Attention^: hello"));
    assert(!snag_irc_mentions_agent(client, "local", "attention^: hello"));
    send_text(agent_fd, ":fake 005 Attention^ -CASEMAPPING :supported\r\n");
    tick(client, 10u);
    assert(snag_irc_mentions_agent(client, "local", "attention~: hello"));
    assert(!snag_irc_mentions_agent(client, "local", "attention^2: hello"));
    assert(!snag_irc_mentions_agent(client, "local", "éAttention^: hello"));
    assert(!snag_irc_mentions_agent(client, "local", "Attention^界: hello"));
    return operator_fd;
}

static void __attribute__((noinline)) test_client_events(void)
{
    struct snag_config config;
    struct capture capture = {0};
    struct snag_irc *client = NULL;
    unsigned short port;
    snag_socket listener = listen_local(&port);
    snag_socket peers[2u];
    snag_socket operator_fd = SNAG_SOCKET_INVALID;
    snag_socket agent_fd = SNAG_SOCKET_INVALID;
    char address[64u];
    char wire[8192u];
    char error[256] = {0};

    endpoint(address, port);
    init_client_config(&config, address, "remoteagent", "remoteop");
    assert(snag_irc_open(&client, &config, "/client", capture_event,
                        capture_trace, &capture, error, sizeof(error)) == 0);
    tick(client, 5u);
    for (size_t i = 0u; i < 2u; ++i) {
        snag_socket_event ready = {listener, SNAG_NET_READ, 0};
        assert(snag_socket_poll(&ready, 1u, 1000) > 0);
        peers[i] = snag_socket_accept(listener);
        assert(peers[i] != SNAG_SOCKET_INVALID);
        /* Match the real server: fixture timing must not depend on delayed ACKs. */
        snag_socket_nodelay(peers[i]);
    }
    tick(client, 10u);
    for (size_t i = 0u; i < 2u; ++i) {
        const char *nick;

        drain_ready(client, peers[i], wire, sizeof(wire));
        nick = strstr(wire, "NICK remoteop\r\n") ? "remoteop" : "remoteagent";
        if (strcmp(nick, "remoteop") == 0) operator_fd = peers[i];
        else agent_fd = peers[i];
        assert(strstr(wire, strcmp(nick, "remoteop") == 0 ? "NICK remoteop\r\n" : "NICK remoteagent\r\n"));
        {
            char welcome[1024u];
            int n = snprintf(welcome, sizeof(welcome), ":fake 001 %s :welcome\r\n"
                ":fake 005 %s SAJROOM=#lab :supported\r\n" ":fake 376 %s :end\r\n",
                peers[i] == agent_fd ? "acceptedagent" : nick, nick, nick);
            assert(n > 0 && (size_t)n < sizeof(welcome));
            send_text(peers[i], welcome);
        }
    }
    /* Welcome may confirm a different nick than the registration request. */
    tick(client, 10u);
    assert(strcmp(snag_irc_model_nick(client), "acceptedagent") == 0);
    send_text(agent_fd, ":acceptedagent!u@fake NICK :remoteagent\r\n");
    tick(client, 5u);
    for (size_t i = 0u; i < 2u; ++i) {
        const char *nick;
        char joined[1024u];
        int n;

        drain_ready(client, peers[i], wire, sizeof(wire));
        assert(strstr(wire, "JOIN #lab\r\n"));
        nick = peers[i] == operator_fd ? "remoteop" : "remoteagent";
        n = snprintf(joined, sizeof(joined), ":%s!u@fake JOIN #lab\r\n"
            ":fake 353 %s = #lab :  @remoteop   remoteagent  peer  \r\n" ":fake 366 %s #lab :end\r\n"
            ":fake BATCH +h chathistory #lab\r\n" ":fake BATCH -h\r\n", nick, nick, nick);
        assert(n > 0 && (size_t)n < sizeof(joined));
        send_text(peers[i], joined);
    }
    wait_pair_event(NULL, client, &capture, SNAG_IRC_HISTORY_READY, 1u);
    assert(operator_fd != SNAG_SOCKET_INVALID);
    {
        struct snag_irc_destinations first, next;

        snag_irc_destinations(client, &first);
        assert(first.count == 1u);
        assert(strstr(first.items[0].nicks, "remoteop\n"));
        assert(strstr(first.items[0].nicks, "remoteagent\n"));
        assert(strstr(first.items[0].nicks, "peer\n"));
        snag_irc_destinations(client, &next);
        assert(!memcmp(&first, &next, sizeof(first)));
        unsigned int nicks = capture.events[SNAG_IRC_NICK];
        send_text(operator_fd, ":peer!u@fake NICK :renamed\r\n");
        wait_pair_event(NULL, client, &capture, SNAG_IRC_NICK, nicks + 1u);
        snag_irc_destinations(client, &next);
        assert(next.count == 1u);
        assert(!strstr(next.items[0].nicks, "peer\n"));
        assert(strstr(next.items[0].nicks, "renamed\n"));
        unsigned int parts = capture.events[SNAG_IRC_PART];
        send_text(operator_fd, ":renamed!u@fake PART #lab :bye\r\n");
        wait_pair_event(NULL, client, &capture, SNAG_IRC_PART, parts + 1u);
        snag_irc_destinations(client, &next);
        assert(next.count == 1u);
        assert(!strstr(next.items[0].nicks, "renamed\n"));
        unsigned int joins = capture.events[SNAG_IRC_JOIN];
        send_text(operator_fd, ":peer!u@fake JOIN #lab\r\n");
        wait_pair_event(NULL, client, &capture, SNAG_IRC_JOIN, joins + 1u);
    }
    {
        unsigned int before = capture.events[SNAG_IRC_MESSAGE];

        send_text(operator_fd, ":peer!u@fake PRIVMSG remoteop :remoteagent: direct ignored\r\n");
        tick(client, 10u);
        assert(capture.events[SNAG_IRC_MESSAGE] == before);
        send_text(operator_fd, ":peer!u@fake PRIVMSG #lab :remoteagent: room accepted\r\n");
        wait_pair_event(NULL, client, &capture, SNAG_IRC_MESSAGE, before + 1u);
        assert(capture.events[SNAG_IRC_MESSAGE] == before + 1u);
        assert(strcmp(capture.last_message.text, "remoteagent: room accepted") == 0);
    }
    {
        unsigned int messages = capture.events[SNAG_IRC_MESSAGE];

        /* Observer receives the model rename first, followed by its echo. */
        send_text(operator_fd, ":remoteagent!u@fake NICK :agent7\r\n"
            ":agent7!u@fake PRIVMSG #lab :own echo\r\n" ":remoteop!u@fake NICK :operator7\r\n"
            ":operator7!u@fake NICK :Operator7\r\n" ":outsider!u@fake NICK :ignored\r\n"
            ":peer!u@fake NICK :friend\r\n" ":fake 433 Operator7 taken :Nickname is already in use\r\n");
        tick(client, 10u);
        send_text(agent_fd, ":remoteagent!u@fake NICK :agent7\r\n"
            ":remoteop!u@fake NICK :operator7\r\n" ":operator7!u@fake NICK :Operator7\r\n"
            ":peer!u@fake NICK :friend\r\n");
        wait_pair_event(NULL, client, &capture, SNAG_IRC_NICK, 5u);
        assert(capture.events[SNAG_IRC_NICK] == 5u);
        assert(capture.events[SNAG_IRC_MESSAGE] == messages);
        assert(capture.events[SNAG_IRC_DISCONNECTED] == 0u);
        assert(strcmp(snag_irc_model_nick(client), "agent7") == 0);
        assert(strcmp(snag_irc_operator_nick(client), "Operator7") == 0);
        assert(snag_irc_mentions_agent(client, address, "AGENT7: hello"));
        assert(snag_irc_mentions_agent(client, "local", "@agent7 hello"));
        assert(!snag_irc_mentions_agent(client, "local", "@remoteagent old"));
        assert(!snag_irc_mentions_agent(client, address, "remoteagent: old"));
        struct snag_buf snapshot = {.max = SNAG_MAX_IRC_SNAPSHOT};
        assert(snag_irc_snapshot(client, &snapshot, error, sizeof(error)) == 0);
        assert(snag_buf_terminate(&snapshot) == 0);
        assert(strstr((const char *)snapshot.data, "model nick: agent7\noperator nick: Operator7\n"));
        assert(strstr((const char *)snapshot.data, "@Operator7 agent7 friend"));
        snag_buf_free(&snapshot);
        assert(send_all(client, false, SNAG_IRC_TOPIC, "renamed topic", error, sizeof(error)) == 0);
        unsigned int modes = capture.events[SNAG_IRC_MODE];
        send_text(operator_fd, ":friend!u@fake MODE #lab -o Operator7\r\n"
            ":friend!u@fake MODE #lab +o agent7\r\n");
        send_text(agent_fd, ":friend!u@fake MODE #lab +o agent7\r\n");
        wait_pair_event(NULL, client, &capture, SNAG_IRC_MODE, modes + 2u);
        /* External room policy belongs to the server: -t permits this joined
         * non-op identity, while +t would reject the same queued TOPIC. */
        assert(send_all(client, false, SNAG_IRC_TOPIC, "not op", error, sizeof(error)) == 0);
        wait_wire(client, operator_fd, wire, sizeof(wire), "TOPIC #lab :not op\r\n");
        assert(send_all(client, true, SNAG_IRC_TOPIC, "agent op", error, sizeof(error)) == 0);
        send_text(agent_fd, ":friend!u@fake MODE #lab -o agent7\r\n");
        tick(client, 5u);
        error[0] = '\0';
        assert(send_all(client, true, SNAG_IRC_TOPIC, "external", error, sizeof(error)) == 0);
        wait_wire(client, agent_fd, wire, sizeof(wire), "TOPIC #lab :external\r\n");
        send_text(agent_fd, ":friend!u@fake MODE #lab +o agent7\r\n");
        tick(client, 5u);
        send_text(operator_fd, ":remoteagent!u@fake JOIN #lab\r\n"
            ":remoteagent!u@fake PRIVMSG #lab :old nick is someone else\r\n"
            ":friend!u@fake PART #lab :bye\r\n");
        wait_pair_event(NULL, client, &capture, SNAG_IRC_MESSAGE, messages + 1u);
        assert(capture.events[SNAG_IRC_MESSAGE] == messages + 1u);
        assert(strcmp(capture.last_message.nick, "remoteagent") == 0);
        assert(send_all(client, false, SNAG_IRC_MESSAGE, "local renamed op", error, sizeof(error)) == 0);
        assert(strcmp(capture.last_message.nick, "Operator7") == 0);
        assert(send_all(client, true, SNAG_IRC_MESSAGE, "local renamed model", error, sizeof(error)) == 0);
        assert(strcmp(capture.last_message.nick, "agent7") == 0);
    }
    /* Requested names differ from the server-confirmed sender until its ack. */
    assert(send_all(client, true, SNAG_IRC_NICK, "docsowner", error, sizeof(error)) == 0);
    assert(strcmp(snag_irc_model_nick(client), "agent7") == 0);
    send_text(operator_fd, ":agent7!u@fake NICK :docsowner\r\n");
    tick(client, 10u);
    assert(strcmp(snag_irc_model_nick(client), "docsowner") == 0);
    assert(send_all(client, true, SNAG_IRC_NICK, "first", error, sizeof(error)) == 0);
    assert(send_all(client, true, SNAG_IRC_NICK, "final", error, sizeof(error)) == 0);
    send_text(agent_fd, ":agent7!u@fake NICK :docsowner\r\n"
        ":docsowner!u@fake NICK :first\r\n:first!u@fake NICK :final\r\n");
    tick(client, 10u);
    assert(strcmp(snag_irc_model_nick(client), "final") == 0);
    send_text(operator_fd, ":docsowner!u@fake NICK :first\r\n:first!u@fake NICK :final\r\n");
    tick(client, 10u);
    assert(strcmp(snag_irc_model_nick(client), "final") == 0);
    send_text(agent_fd, ":Operator7!u@fake NICK :OpFinal\r\n");
    tick(client, 10u);
    assert(strcmp(snag_irc_operator_nick(client), "OpFinal") == 0);
    /* Another member may own a requested name that the server has not accepted. */
    assert(send_all(client, true, SNAG_IRC_NICK, "remoteagent", error, sizeof(error)) == 0);
    send_text(operator_fd, ":remoteagent!u@fake NICK :outsider\r\n");
    tick(client, 10u);
    assert(strcmp(snag_irc_model_nick(client), "final") == 0);
    send_text(agent_fd, ":fake 433 final remoteagent :Nickname is already in use\r\n");
    tick(client, 5u);
    assert(strcmp(snag_irc_model_nick(client), "final") == 0);
    snag_socket previous = operator_fd;
    operator_fd = client_channel_state(client, &capture, operator_fd, agent_fd, listener);
    for (size_t i = 0u; i < 2u; ++i)
        if (peers[i] == previous) peers[i] = operator_fd;
    ping_without_engine(operator_fd);
    ping_without_engine(agent_fd);
    snag_irc_close(client);
    for (size_t i = 0u; i < 2u; ++i) assert(snag_socket_close(peers[i]) == 0);
    assert(snag_socket_close(listener) == 0);
    snag_config_free(&config);
}

static void __attribute__((noinline)) test_independent_owners(void)
{
    struct snag_config config;
    struct capture capture = {0};
    struct snag_irc *irc = NULL;
    unsigned short port = free_port();
    snag_socket listeners[2u], peers[2u][2u], human;
    char address[64u], wire[8192u], error[256u] = {0};
    uint64_t started;

    init_server_config(&config, port);
    config.irc.client_count = 2u;
    for (size_t i = 0u; i < 2u; ++i) {
        unsigned short remote;

        listeners[i] = listen_local(&remote);
        endpoint(address, remote);
        assert(snag_strcpy(config.irc.clients[i], sizeof(config.irc.clients[i]), address));
    }
    assert(snag_irc_open(&irc, &config, "/workspace", capture_event,
                        capture_trace, &capture, error, sizeof(error)) == 0);
    tick(irc, 10u);
    for (size_t i = 0u; i < 2u; ++i)
        for (size_t j = 0u; j < 2u; ++j) {
            snag_socket_event ready = {listeners[i], SNAG_NET_READ, 0};

            assert(snag_socket_poll(&ready, 1u, 250) == 1);
            peers[i][j] = snag_socket_accept(listeners[i]);
            assert(peers[i][j] != SNAG_SOCKET_INVALID);
        }
    human = connect_local(port, false);
    register_peer(irc, human, "human", false, wire, sizeof(wire));
    tick(irc, 10u);
    /* Stop engine admission and saturate only the first endpoint's mailbox. */
    for (size_t i = 0u; i < 100u; ++i) send_text(peers[0][0], "PING :fill-mailbox\r\n");
    ping_without_engine(human);
    ping_without_engine(peers[1][0]);
    ping_without_engine(peers[1][1]);
    started = snag_monotonic_ms();
    snag_irc_close(irc);
    assert(snag_monotonic_ms() - started < 250u);
    assert(snag_socket_close(human) == 0);
    for (size_t i = 0u; i < 2u; ++i) {
        for (size_t j = 0u; j < 2u; ++j) assert(snag_socket_close(peers[i][j]) == 0);
        assert(snag_socket_close(listeners[i]) == 0);
    }
    snag_config_free(&config);
}

static void __attribute__((noinline)) test_callback_failure(void)
{
    struct snag_config config;
    struct capture capture = {.fail_message = true};
    struct snag_irc *server;
    char error[256u] = {0};

    init_server_config(&config, free_port());
    server = open_server(&config, &capture);
    assert(send_all(server, true, SNAG_IRC_MESSAGE, "failed admission", error, sizeof(error)) < 0);
    snag_irc_close(server);
    snag_config_free(&config);
}

static void
test_replay_checkpoint(void)
{
    struct snag_config config;
    struct snag_irc_core *source = NULL, *restored = NULL, *small = NULL;
    struct capture capture = {0};
    char error[256] = {0};
    struct snag_irc_event quiet = {.kind = SNAG_IRC_JOIN, .timestamp_ms = 1u, .sequence = 9u};
    struct snag_irc_event event = {.kind = SNAG_IRC_JOIN, .timestamp_ms = 2u,
        .sequence = 41u, .op = true};

    init_server_config(&config, 16667u);
    config.irc.history_lines = 2u;
    assert(snag_irc_core_open(&source, &config, "/fixture", false,
        NULL, NULL, NULL, error, sizeof(error)) == 0);
    assert(snag_irc_core_open(&restored, &config, "/fixture", false,
        capture_event, NULL, &capture, error, sizeof(error)) == 0);
    strcpy(quiet.endpoint, "127.0.0.1:16668");
    strcpy(quiet.room, "#lab");
    strcpy(quiet.nick, "quiet");
    strcpy(quiet.stream, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    assert(snag_irc_core_restore_event(source, &quiet) == 0);
    strcpy(event.endpoint, config.irc.listen);
    strcpy(event.room, "#lab");
    strcpy(event.nick, "operator");
    strcpy(event.stream, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    assert(snag_irc_core_restore_event(source, &event) == 0);
    event.kind = SNAG_IRC_MESSAGE;
    for (unsigned int i = 0u; i < 2u; ++i) {
        ++event.sequence;
        strcpy(event.text, i ? "second" : "first");
        assert(snag_irc_core_restore_event(source, &event) == 0);
    }

    /* Both JOINs have left the visible ring; authority and dedup still survive. */
    json_t *saved = snag_irc_core_checkpoint(source);
    assert(saved && json_array_size(json_object_get(saved, "history")) == 2u);
    assert(json_array_size(json_object_get(saved, "members")) == 2u);
    assert(json_array_size(json_object_get(saved, "cursors")) == 2u);
    assert(snag_irc_core_restore_checkpoint(restored, saved) == 0);
    json_t *roundtrip = snag_irc_core_checkpoint(restored);
    assert(roundtrip && json_equal(saved, roundtrip));
    json_decref(roundtrip);
    assert(snag_irc_core_received(restored, &quiet));
    assert(snag_irc_core_received(restored, &event));

    /* Reject late corruption and conflicting metadata without partial adoption. */
    for (unsigned int i = 0u; i < 8u; ++i) {
        json_t *bad = json_deep_copy(saved);
        json_t *cursors = json_object_get(bad, "cursors");
        json_t *members = json_object_get(bad, "members");
        json_t *history = json_object_get(bad, "history");
        assert(bad);
        if (i == 0u)
            assert(json_object_set_new(json_array_get(cursors, 1u),
                "sequence", json_integer(0)) == 0);
        else if (i == 1u)
            assert(json_array_append(cursors, json_array_get(cursors, 0u)) == 0);
        else if (i == 2u)
            assert(json_array_append(members, json_array_get(members, 0u)) == 0);
        else if (i == 3u)
            assert(json_object_set_new(json_array_get(history, 1u),
                "room", json_string("invalid")) == 0);
        else if (i == 4u)
            assert(json_object_set_new(bad, "v", json_integer(2)) == 0);
        else if (i == 5u)
            assert(json_array_set(history, 1u, json_array_get(history, 0u)) == 0);
        else if (i == 6u)
            assert(json_object_set_new(json_array_get(cursors, 0u),
                "sequence", json_integer(1)) == 0);
        else
            while (json_array_size(history) <= SNAG_CONFIG_IRC_HISTORY_MAX)
                assert(json_array_append(history, json_array_get(history, 0u)) == 0);
        assert(snag_irc_core_restore_checkpoint(restored, bad) < 0 && errno == EINVAL);
        roundtrip = snag_irc_core_checkpoint(restored);
        assert(roundtrip && json_equal(saved, roundtrip));
        json_decref(roundtrip);
        json_decref(bad);
    }

    ++event.sequence;
    event.op = false;
    assert(snag_irc_core_restore_event(restored, &event) < 0);
    event.op = true;
    event.kind = SNAG_IRC_MODE;
    strcpy(event.text, "+o visitor");
    assert(snag_irc_core_restore_event(restored, &event) == 0);
    /* A restored host continues the original stream, not a newly minted one. */
    assert(snag_irc_core_send(restored, true, SNAG_IRC_MESSAGE,
        "after checkpoint", error, sizeof(error)) == 0);
    assert(capture.events[SNAG_IRC_MESSAGE] == 1u);
    assert(capture.last_message.sequence == event.sequence + 1u);
    assert(!strcmp(capture.last_message.stream, event.stream));

    for (unsigned int limit = 0u; limit < 2u; ++limit) {
        config.irc.history_lines = limit;
        assert(snag_irc_core_open(&small, &config, "/fixture", false,
            NULL, NULL, NULL, error, sizeof(error)) == 0);
        assert(snag_irc_core_restore_checkpoint(small, saved) == 0);
        roundtrip = snag_irc_core_checkpoint(small);
        assert(roundtrip && json_array_size(json_object_get(roundtrip, "history")) == limit);
        assert(json_equal(json_object_get(saved, "members"),
            json_object_get(roundtrip, "members")));
        assert(json_equal(json_object_get(saved, "cursors"),
            json_object_get(roundtrip, "cursors")));
        if (limit) assert(!strcmp(snag_json_string(json_array_get(
            json_object_get(roundtrip, "history"), 0u), "text"), "second"));
        json_decref(roundtrip);
        snag_irc_core_close(small);
    }
    json_decref(saved);
    snag_irc_core_close(source);
    snag_irc_core_close(restored);
    snag_config_free(&config);
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#ifndef _WIN32
    /* The 64-peer capacity case holds both endpoints in this process. */
    struct rlimit files;
    assert(getrlimit(RLIMIT_NOFILE, &files) == 0);
    if (files.rlim_cur < 256u) {
        files.rlim_cur = 256u;
        assert(setrlimit(RLIMIT_NOFILE, &files) == 0);
        /* Early BSD libpthread caches its fd table size before main(). */
        execv(argv[0], argv);
        perror("exec after raising the descriptor limit");
        abort();
    }
#endif
    engine_thread = pthread_self();
    assert(snag_network_init() == 0);
    set_user("root");
    test_validation();
    test_replay_checkpoint();
    test_cli_network_roles();
    test_listener_collision();
    test_runtime_roles();
    test_private_relay();
    test_private_hosted_identities();
    test_private_client_identities();
    test_channel_routes();
    test_private_sends();
    test_private_native_receipts();
    test_private_commit_failure();
    test_server();
    test_nick_rename();
    test_client_reconnect();
    test_default_nick_sequence();
    test_client_nick_collision(true);
    test_client_nick_collision(false);
    test_client_events();
    test_independent_owners();
    test_callback_failure();
    snag_network_free();
    puts("test_irc: ok");
    return 0;
}
