/* SPDX-License-Identifier: GPL-2.0-only */
#include "config.h"
#include "irc_internal.h"
#include "net.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Only the separately compiled IRC core uses this write wrapper. Peers use
 * real sockets, making partial frames deterministic on both BSD and Linux. */
static size_t write_budget = SIZE_MAX;

ssize_t
snag_test_irc_socket_send(snag_socket fd, const void *data, size_t size)
{
    if (!write_budget) return snag_errno(EAGAIN);
    if (size > write_budget) size = write_budget;
    ssize_t written = snag_socket_send(fd, data, size);
    if (written > 0 && write_budget != SIZE_MAX) write_budget -= (size_t)written;
    return written;
}

struct fixture {
    struct snag_config config;
    struct snag_irc_core *core;
    snag_wake_fd wake[2u];
    unsigned short port;
    unsigned int delivery[SNAG_IRC_UNCERTAIN + 1u];
    char wire[32768u];
    size_t used;
};

static int
capture(void *opaque, const struct snag_irc_event *event)
{
    struct fixture *fixture = opaque;
    if (event->routed && event->route.direction == SNAG_IRC_OUTGOING)
        ++fixture->delivery[event->route.delivery];
    return 0;
}

static void
pump(struct fixture *fixture)
{
    char error[256u] = {0};
    assert(snag_irc_core_tick(fixture->core, 1, fixture->wake[0], error, sizeof(error)) == 0);
}

static void
write_peer(snag_socket peer, const char *text)
{
    size_t length = strlen(text);
    assert(snag_socket_send(peer, text, length) == (ssize_t)length);
}

static bool
read_peer(struct fixture *fixture, snag_socket peer)
{
    ssize_t n = snag_socket_recv(peer, fixture->wire + fixture->used,
        sizeof(fixture->wire) - fixture->used - 1u);
    if (n > 0) fixture->used += (size_t)n;
    else assert(n == 0 || errno == EAGAIN || errno == EWOULDBLOCK);
    fixture->wire[fixture->used] = '\0';
    return n == 0;
}

static void
wait_text(struct fixture *fixture, snag_socket peer, const char *text)
{
    uint64_t until = snag_monotonic_ms() + 1000u;
    while (!strstr(fixture->wire, text)) {
        assert(snag_monotonic_ms() < until);
        pump(fixture);
        assert(!read_peer(fixture, peer));
    }
}

static void
wait_partial(struct fixture *fixture, snag_socket peer)
{
    uint64_t until = snag_monotonic_ms() + 1000u;
    while (fixture->used < 7u) {
        assert(snag_monotonic_ms() < until);
        pump(fixture);
        assert(!read_peer(fixture, peer));
    }
    assert(fixture->used == 7u);
}

static snag_socket
connect_peer(struct fixture *fixture)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(fixture->port)};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    snag_socket peer = snag_socket_open(AF_INET, SOCK_STREAM, 0);
    assert(peer != SNAG_SOCKET_INVALID);
    snag_socket_nodelay(peer);
    int rc = snag_socket_connect(peer, (struct sockaddr *)&address, sizeof(address));
    assert(rc == 0 || errno == EINPROGRESS);
    snag_socket_event ready = {peer, SNAG_NET_WRITE, 0};
    assert(snag_socket_poll(&ready, 1u, 1000) > 0 && snag_socket_connected(peer) == 0);
    write_peer(peer, "NICK peer\r\nUSER peer 0 * :peer\r\nPING :ready\r\n");
    fixture->used = 0u;
    fixture->wire[0] = '\0';
    wait_text(fixture, peer, " :ready\r\n");
    fixture->used = 0u;
    fixture->wire[0] = '\0';
    return peer;
}

static void
open_fixture(struct fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    snag_config_init(&fixture->config);
    assert(snag_network_init() == 0);
    snag_socket probe = snag_socket_open(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    assert(snag_socket_bind(probe, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(probe, (struct sockaddr *)&address, &length) == 0);
    fixture->port = ntohs(address.sin_port);
    snag_socket_close(probe);
    fixture->config.irc.listen_explicit = true;
    snprintf(fixture->config.irc.listen, sizeof(fixture->config.irc.listen),
        "127.0.0.1:%u", fixture->port);
    strcpy(fixture->config.irc.model_nick, "agent");
    strcpy(fixture->config.irc.operator_nick, "operator");
    strcpy(fixture->config.irc.room_name, "#lab");
    char error[256u] = {0};
    struct snag_cli cli = {0};
    assert(snag_irc_apply_cli(&fixture->config, &cli, error, sizeof(error)) == 0);
    assert(snag_wakeup_create(fixture->wake) == 0);
    assert(snag_irc_core_open(&fixture->core, &fixture->config, "/test", true,
        capture, NULL, fixture, error, sizeof(error)) == 0);
    assert(snag_irc_core_bind(fixture->core, NULL, fixture->config.irc.listen, 1u) == 0);
}

int
main(void)
{
    struct fixture fixture;
    open_fixture(&fixture);
    snag_socket peer = connect_peer(&fixture);
    char error[256u] = {0};
    struct snag_irc_query_target target;
    assert(snag_irc_core_query_open(fixture.core, SNAG_IRC_OPERATOR, "peer",
        &target, error, sizeof(error)) == 0);
    assert(snag_irc_core_query_send(fixture.core, &target, SNAG_IRC_MESSAGE,
        "private-body frame", false, NULL, error, sizeof(error)) == 0);
    write_budget = 7u;
    pump(&fixture);
    assert(fixture.delivery[SNAG_IRC_PENDING] == 1u);
    assert(!fixture.delivery[SNAG_IRC_WRITTEN]);
    assert(snag_irc_core_pending(fixture.core) > 0u);
    wait_partial(&fixture, peer);
    write_peer(peer, "PING :between-frames\r\n");
    pump(&fixture);
    write_budget = SIZE_MAX;
    wait_text(&fixture, peer, " :between-frames\r\n");
    char *message = strstr(fixture.wire, " PRIVMSG peer :private-body frame\r\n");
    char *pong = strstr(fixture.wire, " PONG ");
    assert(message && pong && message < pong);
    assert(fixture.delivery[SNAG_IRC_WRITTEN] == 1u);
    assert(fixture.delivery[SNAG_IRC_ACKNOWLEDGED] == 1u);

    /* A rename cancels an entirely unwritten frame and keeps the socket. */
    fixture.used = 0u;
    fixture.wire[0] = '\0';
    assert(snag_irc_core_query_send(fixture.core, &target, SNAG_IRC_MESSAGE,
        "private-body unsent", false, NULL, error, sizeof(error)) == 0);
    write_budget = 0u;
    pump(&fixture);
    write_peer(peer, "NICK renamed\r\n");
    uint64_t until = snag_monotonic_ms() + 1000u;
    while (!fixture.delivery[SNAG_IRC_FAILED]) {
        assert(snag_monotonic_ms() < until);
        pump(&fixture);
    }
    assert(fixture.delivery[SNAG_IRC_FAILED] == 1u);
    write_budget = SIZE_MAX;
    wait_text(&fixture, peer, " NICK :renamed\r\n");
    assert(!strstr(fixture.wire, "private-body"));
    assert(snag_irc_core_query_send(fixture.core, &target, SNAG_IRC_MESSAGE,
        "private-body stale", false, NULL, error, sizeof(error)) < 0 && errno == ESTALE);

    /* A rename during a partial frame closes that socket. Reusing its nick
     * opens a fresh conversation and never replays the uncertain bytes. */
    assert(snag_irc_core_query_open(fixture.core, SNAG_IRC_OPERATOR, "renamed",
        &target, error, sizeof(error)) == 0);
    assert(snag_irc_core_query_send(fixture.core, &target, SNAG_IRC_MESSAGE,
        "private-body partial", false, NULL, error, sizeof(error)) == 0);
    fixture.used = 0u;
    fixture.wire[0] = '\0';
    write_budget = 7u;
    pump(&fixture);
    wait_partial(&fixture, peer);
    write_peer(peer, "NICK peer\r\n");
    until = snag_monotonic_ms() + 1000u;
    while (!read_peer(&fixture, peer)) {
        assert(snag_monotonic_ms() < until);
        pump(&fixture);
    }
    assert(fixture.delivery[SNAG_IRC_UNCERTAIN] == 1u);
    snag_socket_close(peer);
    write_budget = SIZE_MAX;
    peer = connect_peer(&fixture);
    struct snag_irc_query_target next;
    assert(snag_irc_core_query_open(fixture.core, SNAG_IRC_OPERATOR, "peer",
        &next, error, sizeof(error)) == 0);
    assert(strcmp(target.conversation, next.conversation));
    assert(snag_irc_core_query_send(fixture.core, &target, SNAG_IRC_MESSAGE,
        "private-body old-peer", false, NULL, error, sizeof(error)) < 0 && errno == ESTALE);
    assert(snag_irc_core_query_send(fixture.core, &next, SNAG_IRC_MESSAGE,
        "private-body fresh", false, NULL, error, sizeof(error)) == 0);
    wait_text(&fixture, peer, " :private-body fresh\r\n");
    assert(!strstr(fixture.wire, "partial") && !strstr(fixture.wire, "old-peer"));
    snag_socket_close(peer);
    snag_irc_core_close(fixture.core);
    snag_config_free(&fixture.config);
    snag_wakeup_close(fixture.wake);
    snag_network_free();
    puts("IRC partial-write tests passed");
    return 0;
}
