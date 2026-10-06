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
    struct snag_irc_event last_send;
    struct snag_irc_event last_input;
    unsigned int inputs;
    unsigned int incoming_messages;
    char wire[32768u];
    size_t used;
};

static int
capture(void *opaque, const struct snag_irc_event *event)
{
    struct fixture *fixture = opaque;
    if (event->input) {
        ++fixture->inputs;
        fixture->last_input = *event;
    }
    if (event->routed && event->route.kind == SNAG_IRC_QUERY &&
        event->route.direction == SNAG_IRC_INCOMING && event->kind == SNAG_IRC_MESSAGE)
        ++fixture->incoming_messages;
    if (event->routed && event->route.direction == SNAG_IRC_OUTGOING) {
        json_t *data = snag_irc_event_data(event);
        struct snag_irc_event decoded;
        assert(data && snag_irc_event_record_read("irc_event_v2", data, &decoded) == 0);
        json_decref(data);
        ++fixture->delivery[event->route.delivery];
        fixture->last_send = *event;
    }
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

static void
private_frames(void)
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
}

static void
external_fixture(struct fixture *fixture, snag_socket peers[2u])
{
    memset(fixture, 0, sizeof(*fixture));
    assert(snag_network_init() == 0);
    snag_config_init(&fixture->config);
    snag_socket listener = snag_socket_open(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    assert(snag_socket_bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    assert(snag_socket_listen(listener, 2) == 0);
    fixture->config.irc.client_count = 1u;
    snprintf(fixture->config.irc.clients[0], sizeof(fixture->config.irc.clients[0]),
        "127.0.0.1:%u", ntohs(address.sin_port));
    strcpy(fixture->config.irc.model_nick, "agent");
    strcpy(fixture->config.irc.operator_nick, "operator");
    strcpy(fixture->config.irc.room_name, "#lab");
    char error[256u] = {0};
    struct snag_cli cli = {0};
    assert(snag_irc_apply_cli(&fixture->config, &cli, error, sizeof(error)) == 0);
    assert(snag_wakeup_create(fixture->wake) == 0);
    assert(snag_irc_core_open(&fixture->core, &fixture->config, "/test", true,
        capture, NULL, fixture, error, sizeof(error)) == 0);
    assert(snag_irc_core_bind(fixture->core, NULL, fixture->config.irc.clients[0], 1u) == 0);
    write_budget = SIZE_MAX;
    pump(fixture);
    for (size_t i = 0u; i < 2u; ++i) {
        snag_socket_event ready = {listener, SNAG_NET_READ, 0};
        assert(snag_socket_poll(&ready, 1u, 1000) > 0);
        snag_socket fd = snag_socket_accept(listener);
        assert(fd != SNAG_SOCKET_INVALID);
        snag_socket_nodelay(fd);
        fixture->used = 0u;
        fixture->wire[0] = '\0';
        wait_text(fixture, fd, "USER ");
        bool model = strstr(fixture->wire, "NICK agent\r\n") != NULL;
        peers[model ? SNAG_IRC_AGENT : SNAG_IRC_OPERATOR] = fd;
        write_peer(fd, model ? ":fake 001 agent :welcome\r\n:fake 376 agent :end\r\n" :
            ":fake 001 operator :welcome\r\n:fake 376 operator :end\r\n");
        fixture->used = 0u;
        fixture->wire[0] = '\0';
        wait_text(fixture, fd, "JOIN #lab\r\n");
        write_peer(fd, model ? ":agent!u@fake JOIN #lab\r\n" :
            ":operator!u@fake JOIN #lab\r\n:operator!u@fake JOIN #side\r\n");
    }
    for (size_t i = 0u; i < 10u; ++i) pump(fixture);
    snag_socket_close(listener);
}

static void
close_external(struct fixture *fixture, const snag_socket peers[2u])
{
    write_budget = SIZE_MAX;
    for (size_t i = 0u; i < 2u; ++i) snag_socket_close(peers[i]);
    snag_irc_core_close(fixture->core);
    snag_config_free(&fixture->config);
    snag_wakeup_close(fixture->wake);
    snag_network_free();
}

static void
peer_sync(struct fixture *fixture, snag_socket peer, const char *text)
{
    static unsigned int counter;
    char ping[80u];
    snprintf(ping, sizeof(ping), "PING :receipt-check-%u\r\n", ++counter);
    fixture->used = 0u;
    fixture->wire[0] = '\0';
    write_peer(peer, text);
    write_peer(peer, ping);
    wait_text(fixture, peer, ping + 5u);
}

static void
labeled_receipts(void)
{
    struct fixture fixture;
    snag_socket peers[2u];
    external_fixture(&fixture, peers);
    snag_socket peer = peers[SNAG_IRC_OPERATOR];
    peer_sync(&fixture, peer, ":fake CAP * LS * :echo-message labeled-response\r\n"
        ":fake CAP * LS :batch\r\n");
    assert(strstr(fixture.wire, "CAP REQ :batch echo-message labeled-response\r\n"));
    peer_sync(&fixture, peer, ":fake CAP * ACK :batch echo-message labeled-response\r\n");

    char error[256u] = {0};
    struct snag_irc_query_target query;
    assert(snag_irc_core_query_open(fixture.core, SNAG_IRC_OPERATOR, "peer",
        &query, error, sizeof(error)) == 0);
    char ids[2u][SNAG_ID_HEX_LEN + 1u];
    fixture.used = 0u;
    fixture.wire[0] = '\0';
    for (size_t i = 0u; i < 2u; ++i) {
        assert(snag_irc_core_query_send(fixture.core, &query, SNAG_IRC_MESSAGE,
            "same body", false, NULL, error, sizeof(error)) == 0);
        strcpy(ids[i], fixture.last_send.route.send);
    }
    uint64_t until = snag_monotonic_ms() + 1000u;
    while (fixture.delivery[SNAG_IRC_WRITTEN] < 2u) {
        assert(snag_monotonic_ms() < until);
        pump(&fixture);
    }
    char response[2048u];
    snprintf(response, sizeof(response), "@label=%s PRIVMSG peer :same body\r\n", ids[1]);
    wait_text(&fixture, peer, response);
    assert(strcmp(ids[0], ids[1]) && strstr(fixture.wire, ids[0]));
    peer_sync(&fixture, peer, ":operator!u@fake PRIVMSG peer :same body\r\n");
    assert(!fixture.delivery[SNAG_IRC_ACKNOWLEDGED]);
    snprintf(response, sizeof(response), ":fake BATCH +history chathistory peer\r\n"
        "@batch=history;label=%s :operator!u@fake PRIVMSG peer :same body\r\n"
        ":fake BATCH -history\r\n", ids[0]);
    peer_sync(&fixture, peer, response);
    assert(!fixture.delivery[SNAG_IRC_ACKNOWLEDGED]);

    /* Reverse-order identical messages cannot be matched by text or target. */
    snprintf(response, sizeof(response), "@label=%s;msgid=server/2 "
        ":operator!u@fake PRIVMSG peer :server revised body\r\n", ids[1]);
    peer_sync(&fixture, peer, response);
    assert(fixture.delivery[SNAG_IRC_ACKNOWLEDGED] == 1u);
    assert(!strcmp(fixture.last_send.route.send, ids[1]));
    assert(!strcmp(fixture.last_send.text, "server revised body"));
    assert(fixture.last_send.route.revised);
    assert(!strcmp(fixture.last_send.route.source, "server/2"));
    snprintf(response, sizeof(response), "@label=%s :fake 401 operator peer :No such nick\r\n",
        ids[0]);
    peer_sync(&fixture, peer, response);
    assert(fixture.delivery[SNAG_IRC_FAILED] == 1u);
    assert(!strcmp(fixture.last_send.route.send, ids[0]));

    /* A complete response without proof of handling stays uncertain. */
    for (size_t i = 0u; i < 2u; ++i) {
        fixture.used = 0u;
        fixture.wire[0] = '\0';
        assert(snag_irc_core_query_send(fixture.core, &query, SNAG_IRC_MESSAGE,
            "unconfirmed response", false, NULL, error, sizeof(error)) == 0);
        strcpy(ids[0], fixture.last_send.route.send);
        wait_text(&fixture, peer, "PRIVMSG peer :unconfirmed response\r\n");
        if (!i) snprintf(response, sizeof(response),
            "@label=%s :fake BATCH +empty labeled-response\r\n:fake BATCH -empty\r\n", ids[0]);
        else snprintf(response, sizeof(response),
            "@label=%s :operator!u@fake PRIVMSG wrong-peer :unconfirmed response\r\n", ids[0]);
        peer_sync(&fixture, peer, response);
        assert(fixture.delivery[SNAG_IRC_UNCERTAIN] == i + 1u);
        assert(fixture.delivery[SNAG_IRC_ACKNOWLEDGED] == 1u);
    }

    unsigned int incoming = fixture.incoming_messages;
    struct snag_irc_query_target self;
    assert(snag_irc_core_query_open(fixture.core, SNAG_IRC_OPERATOR, "operator",
        &self, error, sizeof(error)) == 0);
    assert(snag_irc_core_query_send(fixture.core, &self, SNAG_IRC_MESSAGE,
        "self message", false, NULL, error, sizeof(error)) == 0);
    strcpy(ids[0], fixture.last_send.route.send);
    wait_text(&fixture, peer, "PRIVMSG operator :self message\r\n");
    snprintf(response, sizeof(response), ":operator!u@fake PRIVMSG operator :self message\r\n"
        "@label=%s :operator!u@fake PRIVMSG operator :self message\r\n", ids[0]);
    peer_sync(&fixture, peer, response);
    assert(fixture.incoming_messages == incoming);
    assert(fixture.delivery[SNAG_IRC_ACKNOWLEDGED] == 2u);

    /* A label without echo-message still yields a server ACK for NOTICE. */
    peer_sync(&fixture, peer, ":fake CAP * DEL :echo-message\r\n");
    assert(snag_irc_core_query_send(fixture.core, &query, SNAG_IRC_NOTICE,
        "notice body", false, NULL, error, sizeof(error)) == 0);
    strcpy(ids[0], fixture.last_send.route.send);
    wait_text(&fixture, peer, "NOTICE peer :notice body\r\n");
    snprintf(response, sizeof(response), "@label=\\%s :fake ACK\r\n", ids[0]);
    peer_sync(&fixture, peer, response);
    assert(fixture.delivery[SNAG_IRC_ACKNOWLEDGED] == 3u);
    assert(!strcmp(fixture.last_send.route.send, ids[0]));
    close_external(&fixture, peers);
}

static void
labeled_line_budget(void)
{
    struct fixture fixture;
    snag_socket peers[2u];
    external_fixture(&fixture, peers);
    snag_socket peer = peers[SNAG_IRC_OPERATOR];
    peer_sync(&fixture, peer, ":fake CAP * ACK :batch echo-message labeled-response\r\n");
    char error[256u] = {0};
    struct snag_irc_query_target query;
    assert(snag_irc_core_query_open(fixture.core, SNAG_IRC_OPERATOR, "peer",
        &query, error, sizeof(error)) == 0);
    char text[4096u];
    size_t length = 0u;
    for (size_t i = 0u; i < 300u; ++i) {
        memcpy(text + length, "é界😀", strlen("é界😀"));
        length += strlen("é界😀");
    }
    text[length] = '\0';
    fixture.used = 0u;
    fixture.wire[0] = '\0';
    assert(snag_irc_core_query_send(fixture.core, &query, SNAG_IRC_MESSAGE,
        text, true, NULL, error, sizeof(error)) == 0);
    unsigned int chunks = fixture.delivery[SNAG_IRC_PENDING];
    assert(chunks > 1u);
    uint64_t until = snag_monotonic_ms() + 1000u;
    size_t lines = 0u;
    while (lines < chunks) {
        assert(snag_monotonic_ms() < until);
        pump(&fixture);
        assert(!read_peer(&fixture, peer));
        lines = 0u;
        for (const char *at = fixture.wire; (at = strstr(at, "\r\n")) != NULL; at += 2u) ++lines;
    }
    char rebuilt[sizeof(text)];
    size_t used = 0u;
    struct snag_buf replies = {.max = sizeof(fixture.wire)};
    for (char *at = fixture.wire; *at;) {
        char *end = strstr(at, "\r\n");
        assert(end && (size_t)(end + 2u - at) <= 512u);
        assert(!strncmp(at, "@label=", 7u) && at[39u] == ' ');
        char id[SNAG_ID_HEX_LEN + 1u];
        memcpy(id, at + 7u, SNAG_ID_HEX_LEN);
        id[SNAG_ID_HEX_LEN] = '\0';
        assert(snag_hex_is_lower(id, SNAG_ID_HEX_LEN));
        assert(snag_buf_printf(&replies, "@label=%s :fake ACK\r\n", id) == 0);
        const char *body = strstr(at, "PRIVMSG peer :\001ACTION ");
        assert(body && body < end && end[-1] == '\001');
        body += strlen("PRIVMSG peer :\001ACTION ");
        size_t bytes = (size_t)(end - 1u - body);
        assert(snag_utf8_valid((const unsigned char *)body, bytes, true));
        assert(bytes < sizeof(rebuilt) - used);
        memcpy(rebuilt + used, body, bytes);
        used += bytes;
        at = end + 2u;
    }
    assert(used == length && !memcmp(rebuilt, text, length));
    assert(snag_buf_terminate(&replies) == 0);
    peer_sync(&fixture, peer, (const char *)replies.data);
    assert(fixture.delivery[SNAG_IRC_ACKNOWLEDGED] == chunks);
    snag_buf_free(&replies);
    close_external(&fixture, peers);
}

static void
label_requires_batch(void)
{
    struct fixture fixture;
    snag_socket peers[2u];
    external_fixture(&fixture, peers);
    snag_socket peer = peers[SNAG_IRC_OPERATOR];
    peer_sync(&fixture, peer, ":fake CAP * LS :echo-message labeled-response\r\n");
    assert(strstr(fixture.wire, "CAP REQ :echo-message\r\n"));
    peer_sync(&fixture, peer, ":fake CAP * ACK :echo-message\r\n");
    char error[256u] = {0};
    struct snag_irc_query_target query;
    assert(snag_irc_core_query_open(fixture.core, SNAG_IRC_OPERATOR, "peer",
        &query, error, sizeof(error)) == 0);
    assert(snag_irc_core_query_send(fixture.core, &query, SNAG_IRC_MESSAGE,
        "unlabeled fallback", false, NULL, error, sizeof(error)) == 0);
    wait_text(&fixture, peer, "PRIVMSG peer :unlabeled fallback\r\n");
    assert(!strstr(fixture.wire, "@label="));
    peer_sync(&fixture, peer, ":operator!u@fake PRIVMSG peer :unlabeled fallback\r\n");
    assert(fixture.delivery[SNAG_IRC_WRITTEN] == 1u);
    assert(!fixture.delivery[SNAG_IRC_ACKNOWLEDGED]);
    close_external(&fixture, peers);
}

static void
labeled_batches(void)
{
    struct fixture fixture;
    snag_socket peers[2u];
    external_fixture(&fixture, peers);
    snag_socket peer = peers[SNAG_IRC_OPERATOR];
    peer_sync(&fixture, peer, ":fake CAP * ACK :batch echo-message labeled-response\r\n");
    char error[256u] = {0};
    struct snag_irc_query_target query;
    assert(snag_irc_core_query_open(fixture.core, SNAG_IRC_OPERATOR, "peer",
        &query, error, sizeof(error)) == 0);
    struct snag_irc_channel_target lab;
    assert(snag_irc_core_channel_open(fixture.core, &query, "#lab", false,
        &lab, error, sizeof(error)) == 0);
    char first[SNAG_ID_HEX_LEN + 1u], second[SNAG_ID_HEX_LEN + 1u];
    assert(snag_irc_core_channel_send(fixture.core, &lab, SNAG_IRC_MESSAGE,
        "channel body", false, NULL, error, sizeof(error)) == 0);
    strcpy(first, fixture.last_send.route.send);
    assert(snag_irc_core_query_send(fixture.core, &query, SNAG_IRC_MESSAGE,
        "private body", false, NULL, error, sizeof(error)) == 0);
    strcpy(second, fixture.last_send.route.send);
    wait_text(&fixture, peer, "PRIVMSG peer :private body\r\n");
    char response[2048u];
    snprintf(response, sizeof(response), "@label=%s :fake BATCH +outer labeled-response\r\n"
        "@batch=outer :fake BATCH +inner example/nested\r\n"
        "@batch=inner :operator!u@fake PRIVMSG #lab :amended channel body\r\n"
        "@label=%s :fake ACK\r\n"
        "@batch=outer :fake BATCH -inner\r\n", first, second);
    peer_sync(&fixture, peer, response);
    assert(fixture.delivery[SNAG_IRC_ACKNOWLEDGED] == 1u);
    assert(!strcmp(fixture.last_send.route.send, second));
    peer_sync(&fixture, peer, "@batch=outer :fake 404 operator #lab :Cannot send\r\n"
        ":fake BATCH -outer\r\n");
    assert(fixture.delivery[SNAG_IRC_FAILED] == 1u);
    assert(!strcmp(fixture.last_send.route.send, first));
    assert(fixture.last_send.route.revised);
    assert(!strcmp(fixture.last_send.text, "amended channel body"));

    /* A successful nested action waits for the enclosing batch to close. */
    assert(snag_irc_core_channel_send(fixture.core, &lab, SNAG_IRC_MESSAGE,
        "wave", true, NULL, error, sizeof(error)) == 0);
    strcpy(first, fixture.last_send.route.send);
    wait_text(&fixture, peer, "PRIVMSG #lab :\001ACTION wave\001\r\n");
    snprintf(response, sizeof(response), "@label=%s :fake BATCH +root labeled-response\r\n"
        "@batch=root :fake BATCH +nested example/action\r\n"
        "@batch=nested;msgid=server/action :operator!u@fake PRIVMSG #lab "
        ":\001ACTION waves\001\r\n"
        "@batch=root :fake BATCH -nested\r\n", first);
    peer_sync(&fixture, peer, response);
    assert(fixture.delivery[SNAG_IRC_ACKNOWLEDGED] == 1u);
    peer_sync(&fixture, peer, ":fake BATCH -root\r\n");
    assert(fixture.delivery[SNAG_IRC_ACKNOWLEDGED] == 2u);
    assert(fixture.last_send.route.action && fixture.last_send.route.revised);
    assert(!strcmp(fixture.last_send.text, "waves"));
    assert(!strcmp(fixture.last_send.route.source, "server/action"));

    /* A late batched error must not invalidate a new membership. */
    assert(snag_irc_core_channel_send(fixture.core, &lab, SNAG_IRC_MESSAGE,
        "old membership", false, NULL, error, sizeof(error)) == 0);
    strcpy(first, fixture.last_send.route.send);
    wait_text(&fixture, peer, "PRIVMSG #lab :old membership\r\n");
    snprintf(response, sizeof(response), "@label=%s :fake BATCH +late labeled-response\r\n"
        ":peer!u@fake KICK #lab operator :bye\r\n"
        ":operator!u@fake JOIN #lab\r\n"
        "@batch=late :fake 403 operator #lab :Old channel error\r\n"
        ":fake BATCH -late\r\n", first);
    peer_sync(&fixture, peer, response);
    assert(fixture.delivery[SNAG_IRC_UNCERTAIN] == 1u);
    struct snag_irc_channel_target fresh;
    assert(snag_irc_core_channel_open(fixture.core, &query, "#lab", false,
        &fresh, error, sizeof(error)) == 0);
    assert(strcmp(fresh.membership, lab.membership));
    assert(snag_irc_core_channel_send(fixture.core, &fresh, SNAG_IRC_NOTICE,
        "fresh membership", false, NULL, error, sizeof(error)) == 0);
    strcpy(first, fixture.last_send.route.send);
    wait_text(&fixture, peer, "NOTICE #lab :fresh membership\r\n");
    snprintf(response, sizeof(response), "@label=%s :fake ACK\r\n", first);
    peer_sync(&fixture, peer, response);
    assert(fixture.delivery[SNAG_IRC_ACKNOWLEDGED] == 3u);
    close_external(&fixture, peers);
}

static void
invalid_labeled_echo(void)
{
    struct fixture fixture;
    snag_socket peers[2u];
    external_fixture(&fixture, peers);
    snag_socket peer = peers[SNAG_IRC_OPERATOR];
    peer_sync(&fixture, peer, ":fake CAP * ACK :batch echo-message labeled-response\r\n");
    char error[256u] = {0};
    struct snag_irc_query_target query;
    assert(snag_irc_core_query_open(fixture.core, SNAG_IRC_OPERATOR, "peer",
        &query, error, sizeof(error)) == 0);
    assert(snag_irc_core_query_send(fixture.core, &query, SNAG_IRC_MESSAGE,
        "valid pending body", false, NULL, error, sizeof(error)) == 0);
    char response[256u];
    snprintf(response, sizeof(response),
        "@label=%s :operator!u@fake PRIVMSG peer :invalid \xff\r\n", fixture.last_send.route.send);
    wait_text(&fixture, peer, "PRIVMSG peer :valid pending body\r\n");
    write_peer(peer, response);
    uint64_t until = snag_monotonic_ms() + 1000u;
    while (!read_peer(&fixture, peer)) {
        assert(snag_monotonic_ms() < until);
        pump(&fixture);
    }
    assert(!fixture.delivery[SNAG_IRC_ACKNOWLEDGED]);
    assert(fixture.delivery[SNAG_IRC_UNCERTAIN] == 1u);
    assert(!strcmp(fixture.last_send.text, "valid pending body"));
    close_external(&fixture, peers);
}

static void
labeled_capability_loss(void)
{
    struct fixture fixture;
    snag_socket peers[2u];
    external_fixture(&fixture, peers);
    snag_socket peer = peers[SNAG_IRC_OPERATOR];
    peer_sync(&fixture, peer, ":fake CAP * ACK :batch echo-message labeled-response\r\n");
    char error[256u] = {0};
    struct snag_irc_query_target query;
    assert(snag_irc_core_query_open(fixture.core, SNAG_IRC_OPERATOR, "peer",
        &query, error, sizeof(error)) == 0);
    assert(snag_irc_core_query_send(fixture.core, &query, SNAG_IRC_MESSAGE,
        "written labeled", false, NULL, error, sizeof(error)) == 0);
    wait_text(&fixture, peer, "PRIVMSG peer :written labeled\r\n");
    peer_sync(&fixture, peer, ":fake CAP * DEL :batch\r\n");
    assert(fixture.delivery[SNAG_IRC_UNCERTAIN] == 1u);
    peer_sync(&fixture, peer, ":fake CAP * ACK :batch\r\n");
    assert(snag_irc_core_query_send(fixture.core, &query, SNAG_IRC_MESSAGE,
        "unsent labeled", false, NULL, error, sizeof(error)) == 0);
    write_budget = 0u;
    write_peer(peer, ":fake CAP * DEL :labeled-response\r\n");
    uint64_t until = snag_monotonic_ms() + 1000u;
    while (!fixture.delivery[SNAG_IRC_FAILED]) {
        assert(snag_monotonic_ms() < until);
        pump(&fixture);
    }
    write_budget = SIZE_MAX;
    peer_sync(&fixture, peer, ":fake CAP * ACK :labeled-response\r\n");
    assert(!strstr(fixture.wire, "unsent labeled"));
    assert(snag_irc_core_query_send(fixture.core, &query, SNAG_IRC_MESSAGE,
        "partial labeled", false, NULL, error, sizeof(error)) == 0);
    fixture.used = 0u;
    fixture.wire[0] = '\0';
    write_budget = 7u;
    wait_partial(&fixture, peer);
    write_peer(peer, ":fake CAP * DEL :labeled-response\r\nPING :must-not-interleave\r\n");
    until = snag_monotonic_ms() + 1000u;
    while (!read_peer(&fixture, peer)) {
        assert(snag_monotonic_ms() < until);
        pump(&fixture);
    }
    assert(fixture.used == 7u && !memcmp(fixture.wire, "@label=", 7u));
    assert(fixture.delivery[SNAG_IRC_UNCERTAIN] == 2u);
    close_external(&fixture, peers);
}

static void
channel_send_input(void)
{
    struct fixture fixture;
    snag_socket peers[2u];
    external_fixture(&fixture, peers);
    char error[256u] = {0};
    struct snag_irc_query_target scope;
    assert(snag_irc_core_query_open(fixture.core, SNAG_IRC_OPERATOR, "peer",
        &scope, error, sizeof(error)) == 0);
    struct snag_irc_channel_target lab;
    assert(snag_irc_core_channel_open(fixture.core, &scope, "#lab", false,
        &lab, error, sizeof(error)) == 0);
    snag_socket peer = peers[SNAG_IRC_OPERATOR];
    assert(snag_irc_core_channel_send(fixture.core, &lab, SNAG_IRC_MESSAGE,
        "unconfirmed operator input", false, NULL, error, sizeof(error)) == 0);
    assert(!fixture.inputs);
    wait_text(&fixture, peer, "PRIVMSG #lab :unconfirmed operator input\r\n");
    assert(fixture.inputs == 1u && fixture.last_input.route.delivery == SNAG_IRC_WRITTEN);
    peer_sync(&fixture, peer,
        ":operator!u@fake PRIVMSG #lab :unconfirmed operator input\r\n");
    peer_sync(&fixture, peers[SNAG_IRC_AGENT],
        ":operator!u@fake PRIVMSG #lab :unconfirmed operator input\r\n");
    assert(fixture.inputs == 1u);

    peer_sync(&fixture, peer, ":fake CAP * ACK :batch echo-message labeled-response\r\n");
    assert(snag_irc_core_channel_send(fixture.core, &lab, SNAG_IRC_MESSAGE,
        "pending body", false, NULL, error, sizeof(error)) == 0);
    char id[SNAG_ID_HEX_LEN + 1u];
    strcpy(id, fixture.last_send.route.send);
    wait_text(&fixture, peer, "PRIVMSG #lab :pending body\r\n");
    assert(fixture.inputs == 1u);
    char response[1024u];
    snprintf(response, sizeof(response), "@label=%s :fake BATCH +receipt labeled-response\r\n"
        "@batch=receipt :operator!u@fake PRIVMSG #lab :final server body\r\n", id);
    peer_sync(&fixture, peer, response);
    assert(fixture.inputs == 1u);
    peer_sync(&fixture, peer, ":fake BATCH -receipt\r\n");
    assert(fixture.inputs == 2u && fixture.last_input.route.delivery == SNAG_IRC_ACKNOWLEDGED);
    assert(!strcmp(fixture.last_input.text, "final server body"));
    assert(fixture.last_input.route.revised);
    snprintf(response, sizeof(response), "@label=%s :fake ACK\r\n", id);
    peer_sync(&fixture, peer, response);
    assert(fixture.inputs == 2u);

    assert(snag_irc_core_channel_send(fixture.core, &lab, SNAG_IRC_MESSAGE,
        "rejected body", false, NULL, error, sizeof(error)) == 0);
    strcpy(id, fixture.last_send.route.send);
    wait_text(&fixture, peer, "PRIVMSG #lab :rejected body\r\n");
    snprintf(response, sizeof(response), "@label=%s :fake 404 operator #lab :denied\r\n", id);
    peer_sync(&fixture, peer, response);
    assert(fixture.inputs == 2u);

    assert(snag_irc_core_query_open(fixture.core, SNAG_IRC_AGENT, "peer",
        &scope, error, sizeof(error)) == 0);
    assert(snag_irc_core_channel_open(fixture.core, &scope, "#lab", false,
        &lab, error, sizeof(error)) == 0);
    peer = peers[SNAG_IRC_AGENT];
    peer_sync(&fixture, peer, ":fake CAP * ACK :batch labeled-response\r\n");
    assert(snag_irc_core_channel_send(fixture.core, &lab, SNAG_IRC_MESSAGE,
        "agent reply", false, NULL, error, sizeof(error)) == 0);
    strcpy(id, fixture.last_send.route.send);
    wait_text(&fixture, peer, "PRIVMSG #lab :agent reply\r\n");
    snprintf(response, sizeof(response), "@label=%s :fake ACK\r\n", id);
    peer_sync(&fixture, peer, response);
    assert(fixture.inputs == 2u);
    close_external(&fixture, peers);
}

static void
channel_frames(void)
{
    struct fixture fixture;
    snag_socket peers[2u];
    external_fixture(&fixture, peers);
    char error[256u] = {0};
    struct snag_irc_view view;
    assert(snag_irc_core_view(fixture.core, &view) == 0);
    struct snag_irc_query_target scope = {.identity = SNAG_IRC_OPERATOR,
        .generation = view.generation};
    memcpy(scope.connection, view.connection, sizeof(scope.connection));
    struct snag_irc_channel_target lab, side;
    assert(snag_irc_core_channel_open(fixture.core, &scope, "#lab", false,
        &lab, error, sizeof(error)) == 0);
    assert(snag_irc_core_channel_open(fixture.core, &scope, "#side", false,
        &side, error, sizeof(error)) == 0);
    snag_socket peer = peers[SNAG_IRC_OPERATOR];
    fixture.used = 0u;
    fixture.wire[0] = '\0';
    write_peer(peer, ":fake CAP * ACK :" SNAJPAGENT_NAME "/catchup echo-message\r\n");
    wait_text(&fixture, peer, "CAP END\r\n");
    fixture.used = 0u;
    fixture.wire[0] = '\0';
    assert(snag_irc_core_channel_send(fixture.core, &lab, SNAG_IRC_MESSAGE,
        "channel frame", false, NULL, error, sizeof(error)) == 0);
    write_budget = 7u;
    wait_partial(&fixture, peer);
    write_peer(peer, "PING :between-channel-frames\r\n");
    pump(&fixture);
    write_budget = SIZE_MAX;
    wait_text(&fixture, peer, " :between-channel-frames\r\n");
    char *body = strstr(fixture.wire, "PRIVMSG #lab :channel frame\r\n");
    char *pong = strstr(fixture.wire, "PONG ");
    assert(body && pong && body < pong);
    assert(fixture.delivery[SNAG_IRC_WRITTEN] == 1u);
    assert(!fixture.delivery[SNAG_IRC_ACKNOWLEDGED]);
    assert(!fixture.inputs);
    write_peer(peer, "@saj-id=11111111111111111111111111111111:1 "
        ":operator!u@fake PRIVMSG #lab :channel frame\r\n");
    for (size_t i = 0u; i < 5u; ++i) pump(&fixture);
    assert(!fixture.delivery[SNAG_IRC_ACKNOWLEDGED]);
    assert(!fixture.inputs);

    /* Only the kicked channel loses an entirely unwritten message. */
    fixture.used = 0u;
    fixture.wire[0] = '\0';
    write_budget = 0u;
    assert(snag_irc_core_channel_send(fixture.core, &side, SNAG_IRC_MESSAGE,
        "cancelled side", false, NULL, error, sizeof(error)) == 0);
    assert(snag_irc_core_channel_send(fixture.core, &lab, SNAG_IRC_MESSAGE,
        "retained lab", false, NULL, error, sizeof(error)) == 0);
    write_peer(peer, ":peer!u@fake KICK #side operator :bye\r\n");
    uint64_t until = snag_monotonic_ms() + 1000u;
    while (!fixture.delivery[SNAG_IRC_FAILED]) {
        assert(snag_monotonic_ms() < until);
        pump(&fixture);
    }
    assert(fixture.delivery[SNAG_IRC_FAILED] == 1u);
    assert(!strcmp(fixture.last_send.route.conversation, side.conversation));
    write_budget = SIZE_MAX;
    wait_text(&fixture, peer, "PRIVMSG #lab :retained lab\r\n");
    assert(!strstr(fixture.wire, "cancelled side"));

    /* A partial frame cannot be completed after its membership ends. */
    fixture.used = 0u;
    fixture.wire[0] = '\0';
    assert(snag_irc_core_channel_send(fixture.core, &lab, SNAG_IRC_MESSAGE,
        "uncertain channel", false, NULL, error, sizeof(error)) == 0);
    write_budget = 7u;
    wait_partial(&fixture, peer);
    write_peer(peer, ":peer!u@fake KICK #lab operator :bye\r\n");
    until = snag_monotonic_ms() + 1000u;
    while (!read_peer(&fixture, peer)) {
        assert(snag_monotonic_ms() < until);
        pump(&fixture);
    }
    assert(fixture.delivery[SNAG_IRC_UNCERTAIN] == 1u);
    assert(fixture.used == 7u);
    assert(!strcmp(fixture.last_send.route.conversation, lab.conversation));
    assert(snag_irc_core_channel_send(fixture.core, &lab, SNAG_IRC_MESSAGE,
        "stale channel", false, NULL, error, sizeof(error)) < 0);
    close_external(&fixture, peers);
}

int
main(void)
{
    private_frames();
    channel_frames();
    channel_send_input();
    labeled_receipts();
    labeled_line_budget();
    label_requires_batch();
    labeled_batches();
    labeled_capability_loss();
    invalid_labeled_echo();
    puts("IRC partial-write tests passed");
    return 0;
}
