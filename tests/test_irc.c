/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"
#include "cli.h"
#include "config.h"
#include "irc.h"
#include "snajpagent.h"
#include "store.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct capture {
    unsigned int events[SNJ_IRC_HISTORY_READY + 1u];
    unsigned int protocol_traces;
    unsigned int transport_traces;
    struct snj_irc_event last_message;
    struct snj_irc_event last_notice;
    char message_text[2u * SNJ_IRC_TEXT_MAX + 1u];
    struct snj_irc_event last_nick;
    struct snj_irc_event last_connected;
    bool slow_quit;
    bool fail_message;
};

static pthread_t engine_thread;

static int
capture_event(void *opaque, const struct snj_irc_event *event)
{
    struct capture *capture = opaque;

    assert(pthread_equal(pthread_self(), engine_thread));
    assert(event->kind <= SNJ_IRC_HISTORY_READY);
    ++capture->events[event->kind];
    if (event->kind == SNJ_IRC_MESSAGE) {
        size_t used = strlen(capture->message_text);
        size_t len = strlen(event->text);

        if (len < sizeof(capture->message_text) - used)
            memcpy(capture->message_text + used, event->text, len + 1u);
        capture->last_message = *event;
    }
    if (event->kind == SNJ_IRC_NOTICE)
        capture->last_notice = *event;
    if (event->kind == SNJ_IRC_NICK)
        capture->last_nick = *event;
    if (event->kind == SNJ_IRC_CONNECTED)
        capture->last_connected = *event;
    if (event->kind == SNJ_IRC_QUIT && strcmp(event->nick, "slow") == 0)
        capture->slow_quit = true;
    return capture->fail_message && event->kind == SNJ_IRC_MESSAGE ? -1 : 0;
}

static int
capture_trace(void *opaque, unsigned int level, char direction,
              const char *endpoint, const char *text, size_t len)
{
    struct capture *capture = opaque;

    assert(pthread_equal(pthread_self(), engine_thread));
    assert((level == 5u || level == 6u) &&
           (direction == '<' || direction == '>'));
    assert(endpoint && *endpoint && text && len != 0u);
    if (level == 5u)
        ++capture->protocol_traces;
    else
        ++capture->transport_traces;
    return 0;
}

static unsigned short
free_port(void)
{
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    assert(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
    assert(close(fd) == 0);
    return ntohs(address.sin_port);
}

static int
listen_local(unsigned short *port)
{
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    assert(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
    assert(listen(fd, 4) == 0);
    *port = ntohs(address.sin_port);
    return fd;
}

static void
endpoint(char out[64u], unsigned short port)
{
    int n = snprintf(out, 64u, "127.0.0.1:%u", (unsigned int)port);

    assert(n > 0 && n < 64);
}

static int
connect_local(unsigned short port, bool slow)
{
    struct sockaddr_in address;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int flags;

    assert(fd >= 0);
    if (slow) {
        int size = 1024;
        assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == 0);
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    flags = fcntl(fd, F_GETFL, 0);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    return fd;
}

static void
send_text(int fd, const char *text)
{
    size_t offset = 0u;
    size_t len = strlen(text);

    while (offset < len) {
        ssize_t written = send(fd, text + offset, len - offset, 0);
        if (written < 0 && errno == EINTR)
            continue;
        assert(written > 0);
        offset += (size_t)written;
    }
}

static void
tick(struct snj_irc *irc, unsigned int rounds)
{
    char error[256] = {0};
    uint64_t until = snj_monotonic_ms() + rounds * 2u;

    /* Protocol I/O is independent; trace wakeups are not completed I/O ticks. */
    do {
        if (snj_irc_tick(irc, 2, error, sizeof(error)) < 0)
            fprintf(stderr, "IRC tick failed: %s\n", error);
        assert(error[0] == '\0');
    } while (snj_monotonic_ms() < until);
}

static size_t
drain(int fd, char *out, size_t size)
{
    size_t used = 0u;

    assert(size > 1u);
    for (;;) {
        ssize_t got = recv(fd, out + used, size - used - 1u, 0);
        if (got > 0) {
            used += (size_t)got;
            if (used == size - 1u)
                break;
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        assert(got == 0 || errno == EAGAIN || errno == EWOULDBLOCK);
        break;
    }
    out[used] = '\0';
    return used;
}

static void
register_peer(struct snj_irc *server, int fd, const char *nick,
              bool agent, char *wire, size_t wire_size)
{
    char input[1024u];
    int n = snprintf(input, sizeof(input),
        "CAP LS 302\r\nCAP REQ :batch server-time draft/chathistory%s\r\n"
        "CAP END\r\nNICK %s\r\nUSER %s 0 * :%s\r\nJOIN #lab\r\n",
        agent ? " " SNAJPAGENT_NAME "/agent" : "", nick, nick,
        agent ? SNAJPAGENT_NAME " agent" : "human");

    assert(n > 0 && (size_t)n < sizeof(input));
    send_text(fd, input);
    tick(server, 20u);
    assert(drain(fd, wire, wire_size) != 0u);
}

static void
ping_without_engine(int fd)
{
    char wire[65536u];
    struct pollfd ready = {fd, POLLIN, 0};
    uint64_t deadline = snj_monotonic_ms() + 250u;

    (void)drain(fd, wire, sizeof(wire));
    send_text(fd, "PING :independent-owner\r\n");
    do {
        assert(poll(&ready, 1u, 20) >= 0);
        (void)drain(fd, wire, sizeof(wire));
        if (strstr(wire, "PONG") && strstr(wire, "independent-owner"))
            return;
    } while (snj_monotonic_ms() < deadline);
    assert(!"IRC I/O stopped while the engine was not pumping");
}

static void
init_server_config(struct snj_config *config, unsigned short port)
{
    char address[64u];

    snj_config_init(config);
    endpoint(address, port);
    config->irc_listen_explicit = true;
    assert(snprintf(config->irc_listen, sizeof(config->irc_listen),
                    "%s", address) > 0);
    memcpy(config->irc_model_nick, "agent", 6u);
    memcpy(config->irc_operator_nick, "operator", 9u);
    memcpy(config->irc_room_name, "#lab", 5u);
    config->irc_history_lines = 4u;
}

static struct snj_irc *
open_server(struct snj_config *config, struct capture *capture)
{
    struct snj_cli cli;
    struct snj_irc *server = NULL;
    char error[256] = {0};

    memset(&cli, 0, sizeof(cli));
    assert(snj_irc_apply_cli(config, &cli, error, sizeof(error)) == 0);
    assert(snj_irc_open(&server, config, "/workspace", capture_event,
                        capture_trace, capture, error, sizeof(error)) == 0);
    return server;
}

static void
test_validation(void)
{
    struct snj_config config;
    struct snj_cli cli;
    char error[256] = {0};

    memset(&cli, 0, sizeof(cli));
    snj_config_init(&config);
    config.irc_listen_explicit = true;
    assert(snj_irc_apply_cli(&config, &cli, error, sizeof(error)) == 0);
    assert(strcmp(config.irc_model_nick, "agent0") == 0);
    assert(strcmp(config.irc_operator_nick, "root0") == 0);
    assert(strcmp(config.irc_operator_nick, config.irc_model_nick) != 0);
    assert(config.irc_model_nick_implicit);
    assert(config.irc_operator_nick_implicit);
    snj_config_free(&config);

    assert(setenv("USER", "agent", 1) == 0);
    snj_config_init(&config);
    config.irc_listen_explicit = true;
    assert(snj_irc_apply_cli(&config, &cli, error, sizeof(error)) == 0);
    assert(strcmp(config.irc_operator_nick, "localop0") == 0);
    assert(config.irc_operator_nick_implicit);
    snj_config_free(&config);

    assert(setenv("USER", "not valid", 1) == 0);
    snj_config_init(&config);
    config.irc_listen_explicit = true;
    assert(snj_irc_apply_cli(&config, &cli, error, sizeof(error)) == 0);
    assert(strcmp(config.irc_operator_nick, "operator0") == 0);
    assert(config.irc_operator_nick_implicit);
    snj_config_free(&config);
    assert(setenv("USER", "root", 1) == 0);

    snj_config_init(&config);
    config.irc_client_count = 2u;
    memcpy(config.irc_clients[0], "localhost", 10u);
    memcpy(config.irc_clients[1], "localhost:6667", 15u);
    memcpy(config.irc_model_nick, "worker", 7u);
    error[0] = '\0';
    assert(snj_irc_apply_cli(&config, &cli, error, sizeof(error)) < 0);
    assert(strstr(error, "duplicate") != NULL);
    snj_config_free(&config);

    snj_config_init(&config);
    config.irc_client_count = 1u;
    memcpy(config.irc_clients[0], "bad\xc3\x28", 6u);
    memcpy(config.irc_model_nick, "worker", 7u);
    error[0] = '\0';
    assert(snj_irc_apply_cli(&config, &cli, error, sizeof(error)) < 0);
    assert(strstr(error, "invalid IRC client endpoint") != NULL);
    snj_config_free(&config);

    snj_config_init(&config);
    config.irc_listen_explicit = true;
    memcpy(config.irc_model_nick, "worker", 7u);
    memcpy(config.irc_operator_nick, "WORKER", 7u);
    error[0] = '\0';
    assert(snj_irc_apply_cli(&config, &cli, error, sizeof(error)) < 0);
    snj_config_free(&config);

    snj_config_init(&config);
    config.irc_listen_explicit = true;
    memcpy(config.irc_model_nick, "b\xc3\xb6t", 5u);
    memcpy(config.irc_operator_nick, "alice", 6u);
    error[0] = '\0';
    assert(snj_irc_apply_cli(&config, &cli, error, sizeof(error)) == 0);
    snj_config_free(&config);

    snj_config_init(&config);
    config.irc_listen_explicit = true;
    memcpy(config.irc_model_nick, "bad\xc2\x85", 6u);
    memcpy(config.irc_operator_nick, "alice", 6u);
    error[0] = '\0';
    assert(snj_irc_apply_cli(&config, &cli, error, sizeof(error)) < 0);
    snj_config_free(&config);

    snj_config_init(&config);
    config.irc_listen_explicit = true;
    memcpy(config.irc_model_nick, "bad\xc2\xa0nick", 10u);
    memcpy(config.irc_operator_nick, "alice", 6u);
    error[0] = '\0';
    assert(snj_irc_apply_cli(&config, &cli, error, sizeof(error)) < 0);
    snj_config_free(&config);

    snj_config_init(&config);
    config.irc_listen_explicit = true;
    memcpy(config.irc_model_nick, "worker", 7u);
    memcpy(config.irc_operator_nick, "alice", 6u);
    memcpy(config.irc_room_name, "bad\xe2\x80\x8broom", 11u);
    error[0] = '\0';
    assert(snj_irc_apply_cli(&config, &cli, error, sizeof(error)) < 0);
    snj_config_free(&config);
}

static void
test_cli_network_roles(void)
{
    struct snj_config config;
    struct snj_cli cli;
    char error[256] = {0};

    memset(&cli, 0, sizeof(cli));
    cli.irc_listen = "irc.example:7667";
    snj_config_init(&config);
    assert(snj_irc_apply_cli(&config, &cli, error, sizeof(error)) == 0);
    assert(config.irc_listen_explicit);
    assert(strcmp(config.irc_listen, "irc.example:7667") == 0);
    assert(config.irc_client_count == 0u);
    assert(strcmp(config.irc_model_nick, "agent0") == 0);
    assert(strcmp(config.irc_operator_nick, "root0") == 0);
    assert(config.irc_model_nick_implicit);
    assert(config.irc_operator_nick_implicit);
    cli.irc_model_nick = "worker";
    cli.irc_operator_nick = "operator";
    assert(snj_irc_apply_cli(&config, &cli, error, sizeof(error)) == 0);
    assert(strcmp(config.irc_model_nick, "worker") == 0);
    assert(strcmp(config.irc_operator_nick, "operator") == 0);
    assert(!config.irc_model_nick_implicit);
    assert(!config.irc_operator_nick_implicit);
    snj_config_free(&config);

    memset(&cli, 0, sizeof(cli));
    cli.irc_listen = "127.0.0.1:7667";
    cli.irc_clients[0] = "upstream.example:6667";
    cli.irc_client_count = 1u;
    cli.irc_model_nick = "worker";
    cli.irc_operator_nick = "operator";
    snj_config_init(&config);
    error[0] = '\0';
    assert(snj_irc_apply_cli(&config, &cli, error, sizeof(error)) == 0);
    assert(config.irc_listen_explicit);
    assert(strcmp(config.irc_listen, "127.0.0.1:7667") == 0);
    assert(config.irc_client_count == 1u);
    assert(strcmp(config.irc_clients[0], "upstream.example:6667") == 0);
    assert(!config.irc_model_nick_implicit);
    assert(!config.irc_operator_nick_implicit);
    snj_config_free(&config);
}

static void
test_server(void)
{
    struct snj_config config;
    struct capture capture = {0};
    struct snj_irc *server;
    unsigned short port = free_port();
    char wire[128u * 1024u];
    char traffic[SNJ_IRC_TEXT_MAX + 1u];
    char error[256] = {0};
    int human;
    int history;
    int bad;
    int slow;

    init_server_config(&config, port);
    config.irc_client_count = 1u;
    assert(snprintf(config.irc_clients[0], sizeof(config.irc_clients[0]),
                    "%s", config.irc_listen) > 0);
    server = open_server(&config, &capture);
    assert(strcmp(snj_irc_room_name(server), "#lab") == 0);
    assert(snj_irc_mentions_agent(server, config.irc_listen, "AGENT: please"));
    assert(!snj_irc_mentions_agent(server, config.irc_listen,
                                   "otheragent: no"));

    human = connect_local(port, false);
    register_peer(server, human, "human", false, wire, sizeof(wire));
    assert(strstr(wire, " 001 human ") != NULL);
    assert(strstr(wire, "SAJROOM=#lab") != NULL);
    assert(strstr(wire, "LINELEN=8192") != NULL);
    assert(strstr(wire, " 332 human #lab :/workspace") != NULL);
    assert(strstr(wire, "MODE #lab +o human") != NULL);
    assert(strstr(wire, " = #lab :agent") != NULL);
    assert(strstr(wire, " = #lab :@operator") != NULL);
    assert(capture.events[SNJ_IRC_JOIN] != 0u);

    send_text(human, "PING :token-123\r\nMODE #lab +o agent\r\n");
    tick(server, 10u);
    (void)drain(human, wire, sizeof(wire));
    assert(strstr(wire, "PONG") && strstr(wire, "token-123"));
    assert(strstr(wire, "MODE #lab +o agent") != NULL);
    assert(snj_irc_set_agent_topic(server, "agent topic",
                                   error, sizeof(error)) == 0);
    tick(server, 5u);
    (void)drain(human, wire, sizeof(wire));
    assert(strstr(wire, "TOPIC #lab :agent topic") != NULL);

    send_text(human, "PRIVMSG #lab :hello \00304red\r\nNAMES #lab\r\nWHO #lab\r\n");
    tick(server, 10u);
    (void)drain(human, wire, sizeof(wire));
    assert(strcmp(capture.last_message.nick, "human") == 0);
    assert(strcmp(capture.last_message.text, "hello red") == 0);
    assert(capture.last_message.op);
    assert(strstr(wire, " 366 human #lab") != NULL);
    assert(strstr(wire, " 315 human #lab") != NULL);
    assert(capture.protocol_traces != 0u && capture.transport_traces != 0u);

    send_text(human, "NICK renamed\r\nNICK renamed\r\nNICK RENAMED\r\n"
                     "NICK agent\r\nPRIVMSG #lab :renamed speech\r\n"
                     "TOPIC #lab :agent topic\r\n");
    tick(server, 10u);
    (void)drain(human, wire, sizeof(wire));
    assert(capture.events[SNJ_IRC_NICK] == 2u);
    assert(strcmp(capture.last_nick.nick, "renamed") == 0);
    assert(strcmp(capture.last_nick.text, "RENAMED") == 0);
    assert(capture.last_nick.op);
    assert(strstr(wire, " NICK :renamed\r\n"));
    assert(strstr(wire, " NICK :RENAMED\r\n"));
    assert(strstr(wire, " 433 "));
    assert(strcmp(capture.last_message.nick, "RENAMED") == 0);
    assert(capture.last_message.op);
    send_text(human, "PART #lab\r\nNICK away\r\nJOIN #lab\r\n");
    tick(server, 10u);
    (void)drain(human, wire, sizeof(wire));
    assert(strstr(wire, " NICK :away\r\n"));
    assert(strstr(wire, " 366 away #lab "));
    assert(capture.events[SNJ_IRC_NICK] == 2u);

    {
        struct snj_irc_event foreign = {0};

        foreign.timestamp_ms = snj_time_ms();
        memcpy(foreign.endpoint, "elsewhere:6667", 15u);
        memcpy(foreign.room, "#lab", 5u);
        memcpy(foreign.nick, "guest", 6u);
        foreign.kind = SNJ_IRC_JOIN;
        assert(snj_irc_restore_event(server, &foreign) == 0);
        foreign.kind = SNJ_IRC_MESSAGE;
        memcpy(foreign.text, "ordinary", 9u);
        assert(snj_irc_restore_event(server, &foreign) == 0);
        foreign.op = true;
        assert(snj_irc_restore_event(server, &foreign) < 0);
        foreign.op = false;
        foreign.kind = SNJ_IRC_MODE;
        memcpy(foreign.text, "+o other", 9u);
        assert(snj_irc_restore_event(server, &foreign) < 0);
        memcpy(foreign.nick, "otherop", 8u);
        foreign.kind = SNJ_IRC_JOIN;
        foreign.text[0] = '\0';
        foreign.op = true;
        assert(snj_irc_restore_event(server, &foreign) == 0);
        foreign.kind = SNJ_IRC_MODE;
        memcpy(foreign.text, "+o guest", 9u);
        assert(snj_irc_restore_event(server, &foreign) == 0);
        memcpy(foreign.nick, "guest", 6u);
        foreign.kind = SNJ_IRC_MESSAGE;
        memcpy(foreign.text, "promoted", 9u);
        assert(snj_irc_restore_event(server, &foreign) == 0);

        memcpy(foreign.nick, "unknown", 8u);
        memcpy(foreign.text, "forged op", 10u);
        assert(snj_irc_restore_event(server, &foreign) == 0);
        foreign.op = false;
        assert(snj_irc_restore_event(server, &foreign) < 0);
        foreign.op = true;

        foreign.kind = SNJ_IRC_TOPIC;
        memcpy(foreign.nick, "otherop", 8u);
        memcpy(foreign.text, "foreign topic must not leak", 28u);
        foreign.op = true;
        assert(snj_irc_restore_event(server, &foreign) == 0);
    }

    assert(snj_irc_send_agent(server, "history marker",
                              error, sizeof(error)) == 0);
    tick(server, 5u);
    (void)drain(human, wire, sizeof(wire));
    history = connect_local(port, false);
    register_peer(server, history, "reader", false, wire, sizeof(wire));
    assert(strstr(wire, "BATCH +") != NULL);
    assert(strstr(wire, "chathistory #lab") != NULL);
    assert(strstr(wire, "history marker") != NULL);
    assert(strstr(wire, "@batch=") != NULL);
    assert(strstr(wire, " 332 reader #lab :agent topic") != NULL);
    assert(strstr(wire, "foreign topic must not leak") == NULL);
    assert(close(history) == 0);
    tick(server, 5u);

    bad = connect_local(port, false);
    memset(wire, 'x', 8191u);
    memcpy(wire + 8191u, "\r\n", 3u);
    send_text(bad, wire);
    tick(server, 10u);
    assert(close(bad) == 0);
    send_text(human, "PING :still-alive\r\n");
    tick(server, 5u);
    (void)drain(human, wire, sizeof(wire));
    assert(strstr(wire, "still-alive") != NULL);

    slow = connect_local(port, true);
    register_peer(server, slow, "slow", false, wire, sizeof(wire));
    memset(traffic, 'x', sizeof(traffic) - 1u);
    traffic[sizeof(traffic) - 1u] = '\0';
    for (unsigned int i = 0u; i < 20000u && !capture.slow_quit; ++i) {
        assert(snj_irc_send_agent(server, traffic, error, sizeof(error)) == 0);
        tick(server, 1u);
        (void)drain(human, wire, sizeof(wire));
    }
    assert(capture.slow_quit);
    assert(close(slow) == 0);

    send_text(human, "MODE #lab -o agent\r\n");
    tick(server, 5u);
    error[0] = '\0';
    assert(snj_irc_set_agent_topic(server, "denied",
                                   error, sizeof(error)) < 0);
    assert(errno == EACCES);
    ping_without_engine(human);
    assert(close(human) == 0);
    snj_irc_close(server);
    snj_config_free(&config);
}

static void
pump_pair(struct snj_irc *server, struct snj_irc *client,
          unsigned int rounds)
{
    char error[256];
    uint64_t until = snj_monotonic_ms() + rounds * 2u;

    do {
        error[0] = '\0';
        assert(snj_irc_tick(client, 2, error, sizeof(error)) == 0);
        error[0] = '\0';
        assert(snj_irc_tick(server, 0, error, sizeof(error)) == 0);
    } while (snj_monotonic_ms() < until);
}

static void
test_client_reconnect(void)
{
    struct snj_config server_config;
    struct snj_config client_config;
    struct snj_cli cli;
    struct capture server_capture = {0};
    struct capture next_capture = {0};
    struct capture client_capture = {0};
    struct snj_irc *server;
    struct snj_irc *next_server;
    struct snj_irc *client = NULL;
    struct snj_buf snapshot;
    unsigned short port = free_port();
    char address[64u];
    char error[256] = {0};
    char payload[SNJ_IRC_TEXT_MAX + 1u];
    char long_text[SNJ_IRC_TEXT_MAX + 32u];
    uint64_t history_before;
    uint64_t history_after;
    unsigned int messages;

    init_server_config(&server_config, port);
    server_config.irc_history_lines = 1000u;
    server = open_server(&server_config, &server_capture);
    for (size_t i = 0u; i < SNJ_IRC_TEXT_MAX; i += 4u)
        memcpy(payload + i, "\xf0\x9f\x8c\x99", 4u);
    payload[SNJ_IRC_TEXT_MAX] = '\0';
    history_before = snj_time_ms();
    for (unsigned int i = 0u; i < 1000u; ++i)
        assert(snj_irc_send_agent(server, payload, error, sizeof(error)) == 0);
    history_after = snj_time_ms();
    {
        struct capture replay = {0};

        assert(snj_irc_replay_hosted_history(server, capture_event, &replay) == 0);
        assert(replay.events[SNJ_IRC_MESSAGE] == 1000u);
        assert(replay.last_message.historical);
        assert(strcmp(replay.last_message.text, payload) == 0);
        assert(!server_capture.last_message.historical);
    }
    snj_config_init(&client_config);
    client_config.irc_history_lines = 1000u;
    memset(&cli, 0, sizeof(cli));
    endpoint(address, port);
    client_config.irc_client_count = 1u;
    assert(snprintf(client_config.irc_clients[0],
                    sizeof(client_config.irc_clients[0]), "%s", address) > 0);
    memcpy(client_config.irc_model_nick, "remoteagent", 12u);
    memcpy(client_config.irc_operator_nick, "remoteop", 9u);
    assert(snj_irc_apply_cli(&client_config, &cli,
                             error, sizeof(error)) == 0);
    assert(snj_irc_open(&client, &client_config, "/client", capture_event,
                        capture_trace, &client_capture,
                        error, sizeof(error)) == 0);
    /* Bounded network ticks drain the extended 4 MiB history incrementally. */
    for (unsigned int i = 0u; i < 2000u &&
         !client_capture.events[SNJ_IRC_HISTORY_READY]; ++i)
        pump_pair(server, client, 1u);
    assert(client_capture.events[SNJ_IRC_HISTORY_READY] != 0u);
    assert(server_capture.events[SNJ_IRC_JOIN] >= 2u);
    assert(strcmp(client_capture.last_message.text, payload) == 0);
    assert(client_capture.events[SNJ_IRC_MESSAGE] >= 990u);
    assert(client_capture.last_message.historical);
    assert(client_capture.last_message.timestamp_ms % 1000u == 0u);
    assert(client_capture.last_message.timestamp_ms >=
           history_before / 1000u * 1000u);
    assert(client_capture.last_message.timestamp_ms <= history_after);
    snj_buf_init(&snapshot, SNJ_MAX_IRC_SNAPSHOT);
    assert(snj_irc_snapshot(client, &snapshot, error, sizeof(error)) == 0);
    assert(snapshot.len > 4u * 1000u * 1000u);
    snj_buf_free(&snapshot);

    messages = client_capture.events[SNJ_IRC_MESSAGE];
    assert(snj_irc_send_agent(client, payload, error, sizeof(error)) == 0);
    pump_pair(server, client, 20u);
    assert(client_capture.events[SNJ_IRC_MESSAGE] == messages + 1u);
    assert(strcmp(server_capture.last_message.text, payload) == 0);
    assert(strcmp(server_capture.last_message.nick, "remoteagent") == 0);
    snj_irc_close(client);
    tick(server, 10u);
    client = NULL;
    client_capture.events[SNJ_IRC_HISTORY_READY] = 0u;
    assert(snj_irc_open(&client, &client_config, "/client", capture_event,
                        capture_trace, &client_capture,
                        error, sizeof(error)) == 0);
    for (unsigned int i = 0u; i < 2000u &&
         !client_capture.events[SNJ_IRC_HISTORY_READY]; ++i)
        pump_pair(server, client, 1u);
    assert(client_capture.events[SNJ_IRC_HISTORY_READY] != 0u);
    assert(client_capture.last_message.historical);
    assert(strcmp(client_capture.last_message.nick, "remoteagent") == 0);
    assert(strcmp(client_capture.last_message.text, payload) == 0);

    assert(snj_irc_send_agent_notice(client, payload, error, sizeof(error)) == 0);
    pump_pair(server, client, 20u);
    assert(strcmp(server_capture.last_notice.text, payload) == 0);
    assert(strcmp(client_capture.last_notice.text, payload) == 0);

    memset(long_text, 'a', SNJ_IRC_TEXT_MAX - 4u);
    memcpy(long_text + SNJ_IRC_TEXT_MAX - 4u, " remaining artifact gaps", 25u);
    server_capture.message_text[0] = '\0';
    client_capture.message_text[0] = '\0';
    messages = server_capture.events[SNJ_IRC_MESSAGE];
    assert(snj_irc_send_agent(server, long_text, error, sizeof(error)) == 0);
    pump_pair(server, client, 20u);
    assert(server_capture.events[SNJ_IRC_MESSAGE] == messages + 2u);
    assert(strcmp(client_capture.last_message.text, "remaining artifact gaps") == 0);
    assert(strcmp(server_capture.message_text, long_text) == 0);
    assert(strcmp(client_capture.message_text, long_text) == 0);

    memset(long_text, 'b', SNJ_IRC_TEXT_MAX - 1u);
    memcpy(long_text + SNJ_IRC_TEXT_MAX - 1u, "\xf0\x9f\x8c\x99" "end", 8u);
    server_capture.message_text[0] = '\0';
    client_capture.message_text[0] = '\0';
    assert(snj_irc_send_agent(client, long_text, error, sizeof(error)) == 0);
    pump_pair(server, client, 20u);
    assert(strcmp(server_capture.last_message.text, "\xf0\x9f\x8c\x99" "end") == 0);
    assert(strcmp(server_capture.message_text, long_text) == 0);
    assert(strcmp(client_capture.message_text, long_text) == 0);

    assert(snj_irc_send_operator(client, "remote hello",
                                 error, sizeof(error)) == 0);
    pump_pair(server, client, 20u);
    assert(strcmp(server_capture.last_message.nick, "remoteop") == 0);
    assert(server_capture.last_message.op);

    snj_irc_close(server);
    for (unsigned int i = 0u;
         i < 50u && !client_capture.events[SNJ_IRC_DISCONNECTED]; ++i)
        tick(client, 1u);
    assert(client_capture.events[SNJ_IRC_DISCONNECTED] != 0u);
    assert(snj_irc_send_agent(client, "retained while disconnected",
                              error, sizeof(error)) == 0);
    assert(strcmp(client_capture.last_message.room, "#lab") == 0);
    next_server = open_server(&server_config, &next_capture);
    pump_pair(next_server, client, 800u);
    assert(strcmp(next_capture.last_message.nick, "remoteagent") == 0);
    assert(strcmp(next_capture.last_message.text,
                  "retained while disconnected") == 0);
    snj_buf_init(&snapshot, SNJ_MAX_IRC_SNAPSHOT);
    assert(snj_irc_snapshot(client, &snapshot, error, sizeof(error)) == 0);
    assert(snj_buf_terminate(&snapshot) == 0);
    assert(strstr((const char *)snapshot.data, address) != NULL);
    assert(strstr((const char *)snapshot.data, "joined #lab") != NULL);
    assert(strstr((const char *)snapshot.data, "topic[") != NULL);
    assert(strstr((const char *)snapshot.data, "]: /workspace") != NULL);
    snj_buf_free(&snapshot);

    snj_irc_close(client);
    snj_irc_close(next_server);
    snj_config_free(&client_config);
    snj_config_free(&server_config);
}

static void
test_default_nick_sequence(void)
{
    struct snj_config server_config;
    struct snj_config client_config[2u];
    struct snj_cli cli;
    struct capture server_capture = {0};
    struct capture client_capture[2u];
    struct snj_irc *server;
    struct snj_irc *client[2u] = {NULL, NULL};
    unsigned short port = free_port();
    char address[64u];
    char expected[32u];
    char error[256] = {0};

    snj_config_init(&server_config);
    endpoint(address, port);
    server_config.irc_listen_explicit = true;
    assert(snprintf(server_config.irc_listen,
                    sizeof(server_config.irc_listen), "%s", address) > 0);
    memcpy(server_config.irc_room_name, "#lab", 5u);
    server = open_server(&server_config, &server_capture);
    assert(strcmp(server_config.irc_model_nick, "agent0") == 0);
    assert(strcmp(server_config.irc_operator_nick, "root0") == 0);
    assert(server_config.irc_model_nick_implicit);
    assert(server_config.irc_operator_nick_implicit);
    assert(strcmp(snj_irc_model_nick(server), "agent0") == 0);
    assert(strcmp(snj_irc_operator_nick(server), "root0") == 0);

    memset(&cli, 0, sizeof(cli));
    memset(client_capture, 0, sizeof(client_capture));
    for (size_t i = 0u; i < 2u; ++i) {
        snj_config_init(&client_config[i]);
        client_config[i].irc_client_count = 1u;
        assert(snprintf(client_config[i].irc_clients[0],
                        sizeof(client_config[i].irc_clients[0]),
                        "%s", address) > 0);
        assert(snj_irc_apply_cli(&client_config[i], &cli,
                                 error, sizeof(error)) == 0);
        assert(strcmp(client_config[i].irc_model_nick, "agent0") == 0);
        assert(strcmp(client_config[i].irc_operator_nick, "root0") == 0);
        assert(client_config[i].irc_model_nick_implicit);
        assert(client_config[i].irc_operator_nick_implicit);
        assert(snj_irc_open(&client[i], &client_config[i], "/client",
                            capture_event, capture_trace, &client_capture[i],
                            error, sizeof(error)) == 0);
        pump_pair(server, client[i], 300u);
        assert(client_capture[i].events[SNJ_IRC_CONNECTED] == 1u);
        assert(client_capture[i].events[SNJ_IRC_DISCONNECTED] == 0u);
        assert(snprintf(expected, sizeof(expected), "agent%zu", i + 1u) > 0);
        assert(strcmp(snj_irc_model_nick(client[i]), expected) == 0);
        assert(snprintf(expected, sizeof(expected), "root%zu", i + 1u) > 0);
        assert(strcmp(snj_irc_operator_nick(client[i]), expected) == 0);
        assert(strstr(snj_irc_model_nick(client[i]), "01") == NULL);
        assert(strstr(snj_irc_operator_nick(client[i]), "01") == NULL);
    }

    for (size_t i = 0u; i < 2u; ++i) {
        snj_irc_close(client[i]);
        snj_config_free(&client_config[i]);
    }
    snj_irc_close(server);
    snj_config_free(&server_config);
}

static void
test_explicit_zero_nick_collision(void)
{
    struct snj_config server_config;
    struct snj_config client_config;
    struct snj_cli cli;
    struct capture server_capture = {0};
    struct capture client_capture = {0};
    struct snj_irc *server;
    struct snj_irc *client = NULL;
    unsigned short port = free_port();
    int occupied[2u];
    char address[64u];
    char wire[8192u];
    char error[256] = {0};

    init_server_config(&server_config, port);
    server = open_server(&server_config, &server_capture);
    occupied[0] = connect_local(port, false);
    register_peer(server, occupied[0], "worker0", true, wire, sizeof(wire));
    occupied[1] = connect_local(port, false);
    register_peer(server, occupied[1], "local0", false, wire, sizeof(wire));

    snj_config_init(&client_config);
    memset(&cli, 0, sizeof(cli));
    endpoint(address, port);
    client_config.irc_client_count = 1u;
    assert(snprintf(client_config.irc_clients[0],
                    sizeof(client_config.irc_clients[0]), "%s", address) > 0);
    memcpy(client_config.irc_model_nick, "worker0", 8u);
    memcpy(client_config.irc_operator_nick, "local0", 7u);
    assert(snj_irc_apply_cli(&client_config, &cli,
                             error, sizeof(error)) == 0);
    assert(!client_config.irc_model_nick_implicit);
    assert(!client_config.irc_operator_nick_implicit);
    assert(snj_irc_open(&client, &client_config, "/client", capture_event,
                        capture_trace, &client_capture,
                        error, sizeof(error)) == 0);
    pump_pair(server, client, 300u);
    assert(client_capture.events[SNJ_IRC_CONNECTED] == 1u);
    assert(client_capture.events[SNJ_IRC_DISCONNECTED] == 0u);
    assert(strcmp(snj_irc_model_nick(client), "worker01") == 0);
    assert(strcmp(snj_irc_operator_nick(client), "local01") == 0);

    snj_irc_close(client);
    for (size_t i = 0u; i < 2u; ++i)
        assert(close(occupied[i]) == 0);
    snj_irc_close(server);
    snj_config_free(&client_config);
    snj_config_free(&server_config);
}

static void
test_client_nick_collision(void)
{
    struct snj_config server_config;
    struct snj_config client_config;
    struct snj_cli cli;
    struct capture server_capture = {0};
    struct capture next_capture = {0};
    struct capture client_capture = {0};
    struct snj_irc *server;
    struct snj_irc *next_server;
    struct snj_irc *client = NULL;
    struct snj_buf snapshot;
    unsigned short port = free_port();
    int occupied[2u];
    char address[64u];
    char wire[8192u];
    char error[256] = {0};
    unsigned int messages;

    init_server_config(&server_config, port);
    server = open_server(&server_config, &server_capture);
    occupied[0] = connect_local(port, false);
    register_peer(server, occupied[0], "agent1", false, wire, sizeof(wire));
    occupied[1] = connect_local(port, false);
    register_peer(server, occupied[1], "operator1", false, wire, sizeof(wire));

    snj_config_init(&client_config);
    memset(&cli, 0, sizeof(cli));
    endpoint(address, port);
    client_config.irc_client_count = 1u;
    assert(snprintf(client_config.irc_clients[0],
                    sizeof(client_config.irc_clients[0]), "%s", address) > 0);
    memcpy(client_config.irc_model_nick, "agent", 6u);
    memcpy(client_config.irc_operator_nick, "operator", 9u);
    assert(snj_irc_apply_cli(&client_config, &cli,
                             error, sizeof(error)) == 0);
    assert(!client_config.irc_model_nick_implicit);
    assert(!client_config.irc_operator_nick_implicit);
    assert(snj_irc_open(&client, &client_config, "/client", capture_event,
                        capture_trace, &client_capture,
                        error, sizeof(error)) == 0);
    pump_pair(server, client, 300u);
    assert(client_capture.events[SNJ_IRC_HISTORY_READY] != 0u);
    assert(client_capture.events[SNJ_IRC_CONNECTED] == 1u);
    assert(client_capture.events[SNJ_IRC_DISCONNECTED] == 0u);
    assert(strcmp(client_capture.last_connected.nick, "operator2") == 0);
    assert(strcmp(snj_irc_model_nick(client), "agent2") == 0);
    assert(strcmp(snj_irc_operator_nick(client), "operator2") == 0);
    assert(client_capture.events[SNJ_IRC_NICK] == 0u);

    assert(snj_irc_send_operator(client, "operator alias",
                                 error, sizeof(error)) == 0);
    assert(strcmp(client_capture.last_message.nick, "operator2") == 0);
    pump_pair(server, client, 20u);
    assert(strcmp(server_capture.last_message.nick, "operator2") == 0);
    assert(strcmp(server_capture.last_message.text, "operator alias") == 0);
    assert(snj_irc_send_agent(client, "agent alias",
                              error, sizeof(error)) == 0);
    assert(strcmp(client_capture.last_message.nick, "agent2") == 0);
    pump_pair(server, client, 20u);
    assert(strcmp(server_capture.last_message.nick, "agent2") == 0);
    assert(strcmp(server_capture.last_message.text, "agent alias") == 0);

    messages = client_capture.events[SNJ_IRC_MESSAGE];
    assert(snj_irc_send_agent(server, "preferred nick is remote",
                              error, sizeof(error)) == 0);
    pump_pair(server, client, 20u);
    assert(client_capture.events[SNJ_IRC_MESSAGE] == messages + 1u);
    assert(strcmp(client_capture.last_message.nick, "agent") == 0);
    assert(strcmp(client_capture.last_message.text,
                  "preferred nick is remote") == 0);
    assert(snj_irc_mentions_agent(client, address, "agent2: respond"));
    assert(!snj_irc_mentions_agent(client, address, "agent: not this client"));
    snj_buf_init(&snapshot, SNJ_MAX_IRC_SNAPSHOT);
    assert(snj_irc_snapshot(client, &snapshot, error, sizeof(error)) == 0);
    assert(snj_buf_terminate(&snapshot) == 0);
    assert(strstr((const char *)snapshot.data,
                  "model agent2 operator operator2") != NULL);
    snj_buf_free(&snapshot);

    for (size_t i = 0u; i < 2u; ++i)
        assert(close(occupied[i]) == 0);
    snj_irc_close(server);
    for (unsigned int i = 0u;
         i < 50u && !client_capture.events[SNJ_IRC_DISCONNECTED]; ++i)
        tick(client, 1u);
    assert(client_capture.events[SNJ_IRC_DISCONNECTED] != 0u);
    next_server = open_server(&server_config, &next_capture);
    pump_pair(next_server, client, 800u);
    assert(client_capture.events[SNJ_IRC_CONNECTED] == 2u);
    assert(snj_irc_send_operator(client, "stable operator alias",
                                 error, sizeof(error)) == 0);
    pump_pair(next_server, client, 20u);
    assert(strcmp(next_capture.last_message.nick, "operator2") == 0);
    assert(snj_irc_send_agent(client, "stable agent alias",
                              error, sizeof(error)) == 0);
    pump_pair(next_server, client, 20u);
    assert(strcmp(next_capture.last_message.nick, "agent2") == 0);

    snj_irc_close(client);
    snj_irc_close(next_server);
    snj_config_free(&client_config);
    snj_config_free(&server_config);
}

static void
test_client_events(void)
{
    struct snj_config config;
    struct snj_cli cli;
    struct capture capture = {0};
    struct snj_irc *client = NULL;
    unsigned short port;
    int listener = listen_local(&port);
    int peers[2u];
    int operator_fd = -1;
    int agent_fd = -1;
    char address[64u];
    char wire[8192u];
    char error[256] = {0};

    snj_config_init(&config);
    memset(&cli, 0, sizeof(cli));
    endpoint(address, port);
    config.irc_client_count = 1u;
    assert(snprintf(config.irc_clients[0], sizeof(config.irc_clients[0]),
                    "%s", address) > 0);
    memcpy(config.irc_model_nick, "remoteagent", 12u);
    memcpy(config.irc_operator_nick, "remoteop", 9u);
    assert(snj_irc_apply_cli(&config, &cli, error, sizeof(error)) == 0);
    assert(snj_irc_open(&client, &config, "/client", capture_event,
                        capture_trace, &capture, error, sizeof(error)) == 0);
    tick(client, 5u);
    for (size_t i = 0u; i < 2u; ++i) {
        int flags;
        int one = 1;

        peers[i] = accept(listener, NULL, NULL);
        assert(peers[i] >= 0);
        /* Match the real server: fixture timing must not depend on delayed ACKs. */
        assert(setsockopt(peers[i], IPPROTO_TCP, TCP_NODELAY,
                          &one, sizeof(one)) == 0);
        flags = fcntl(peers[i], F_GETFL, 0);
        assert(flags >= 0 &&
               fcntl(peers[i], F_SETFL, flags | O_NONBLOCK) == 0);
    }
    tick(client, 10u);
    for (size_t i = 0u; i < 2u; ++i) {
        const char *nick;

        assert(drain(peers[i], wire, sizeof(wire)) != 0u);
        nick = strstr(wire, "NICK remoteop\r\n") ? "remoteop" : "remoteagent";
        if (strcmp(nick, "remoteop") == 0)
            operator_fd = peers[i];
        else
            agent_fd = peers[i];
        assert(strstr(wire, strcmp(nick, "remoteop") == 0 ?
                           "NICK remoteop\r\n" : "NICK remoteagent\r\n"));
        {
            char welcome[1024u];
            int n = snprintf(welcome, sizeof(welcome),
                ":fake 001 %s :welcome\r\n"
                ":fake 005 %s SAJROOM=#lab :supported\r\n"
                ":fake 376 %s :end\r\n",
                peers[i] == agent_fd ? "acceptedagent" : nick, nick, nick);
            assert(n > 0 && (size_t)n < sizeof(welcome));
            send_text(peers[i], welcome);
        }
    }
    /* Welcome may confirm a different nick than the registration request. */
    tick(client, 10u);
    assert(strcmp(snj_irc_model_nick(client), "acceptedagent") == 0);
    send_text(agent_fd, ":acceptedagent!u@fake NICK :remoteagent\r\n");
    tick(client, 5u);
    for (size_t i = 0u; i < 2u; ++i) {
        const char *nick;
        char joined[1024u];
        int n;

        assert(drain(peers[i], wire, sizeof(wire)) != 0u);
        assert(strstr(wire, "JOIN #lab\r\n"));
        nick = peers[i] == operator_fd ? "remoteop" : "remoteagent";
        n = snprintf(joined, sizeof(joined),
            ":%s!u@fake JOIN #lab\r\n"
            ":fake 353 %s = #lab :@remoteop remoteagent peer\r\n"
            ":fake 366 %s #lab :end\r\n"
            ":fake BATCH +h chathistory #lab\r\n"
            ":fake BATCH -h\r\n",
            nick, nick, nick);
        assert(n > 0 && (size_t)n < sizeof(joined));
        send_text(peers[i], joined);
    }
    tick(client, 20u);
    assert(operator_fd >= 0);
    {
        struct snj_buf nicks;

        snj_buf_init(&nicks, 65536u);
        assert(snj_irc_take_nicks(client, &nicks) == 1);
        assert(strstr((const char *)nicks.data, "remoteop\n"));
        assert(strstr((const char *)nicks.data, "remoteagent\n"));
        assert(strstr((const char *)nicks.data, "peer\n"));
        snj_buf_reset(&nicks);
        assert(snj_irc_take_nicks(client, &nicks) == 0);
        send_text(operator_fd, ":peer!u@fake NICK :renamed\r\n");
        tick(client, 10u);
        assert(snj_irc_take_nicks(client, &nicks) == 1);
        assert(!strstr((const char *)nicks.data, "peer\n"));
        assert(strstr((const char *)nicks.data, "renamed\n"));
        snj_buf_reset(&nicks);
        send_text(operator_fd, ":renamed!u@fake PART #lab :bye\r\n");
        tick(client, 10u);
        assert(snj_irc_take_nicks(client, &nicks) == 1);
        assert(!strstr((const char *)nicks.data, "renamed\n"));
        snj_buf_free(&nicks);
        send_text(operator_fd, ":peer!u@fake JOIN #lab\r\n");
        tick(client, 10u);
    }
    {
        unsigned int before = capture.events[SNJ_IRC_MESSAGE];

        send_text(operator_fd,
            ":peer!u@fake PRIVMSG remoteop :remoteagent: direct ignored\r\n");
        tick(client, 10u);
        assert(capture.events[SNJ_IRC_MESSAGE] == before);
        send_text(operator_fd,
            ":peer!u@fake PRIVMSG #lab :remoteagent: room accepted\r\n");
        tick(client, 10u);
        assert(capture.events[SNJ_IRC_MESSAGE] == before + 1u);
        assert(strcmp(capture.last_message.text,
                      "remoteagent: room accepted") == 0);
    }
    {
        unsigned int messages = capture.events[SNJ_IRC_MESSAGE];
        struct snj_buf snapshot;

        /* Observer receives the model rename first, followed by its echo. */
        send_text(operator_fd,
            ":remoteagent!u@fake NICK :agent7\r\n"
            ":agent7!u@fake PRIVMSG #lab :own echo\r\n"
            ":remoteop!u@fake NICK :operator7\r\n"
            ":operator7!u@fake NICK :Operator7\r\n"
            ":outsider!u@fake NICK :ignored\r\n"
            ":peer!u@fake NICK :friend\r\n"
            ":fake 433 Operator7 taken :Nickname is already in use\r\n");
        tick(client, 10u);
        send_text(agent_fd,
            ":remoteagent!u@fake NICK :agent7\r\n"
            ":remoteop!u@fake NICK :operator7\r\n"
            ":operator7!u@fake NICK :Operator7\r\n"
            ":peer!u@fake NICK :friend\r\n");
        tick(client, 10u);
        assert(capture.events[SNJ_IRC_NICK] == 5u);
        assert(capture.events[SNJ_IRC_MESSAGE] == messages);
        assert(capture.events[SNJ_IRC_DISCONNECTED] == 0u);
        assert(strcmp(snj_irc_model_nick(client), "agent7") == 0);
        assert(strcmp(snj_irc_operator_nick(client), "Operator7") == 0);
        assert(snj_irc_mentions_agent(client, address, "AGENT7: hello"));
        assert(snj_irc_mentions_agent(client, "local", "@agent7 hello"));
        assert(!snj_irc_mentions_agent(client, "local", "@remoteagent old"));
        assert(!snj_irc_mentions_agent(client, address, "remoteagent: old"));
        snj_buf_init(&snapshot, SNJ_MAX_IRC_SNAPSHOT);
        assert(snj_irc_snapshot(client, &snapshot, error, sizeof(error)) == 0);
        assert(snj_buf_terminate(&snapshot) == 0);
        assert(strstr((const char *)snapshot.data,
                      "model nick: agent7\noperator nick: Operator7\n"));
        assert(strstr((const char *)snapshot.data,
                      "@Operator7 agent7 friend"));
        snj_buf_free(&snapshot);
        assert(snj_irc_set_operator_topic(client, "renamed topic",
                                          error, sizeof(error)) == 0);
        send_text(operator_fd,
            ":friend!u@fake MODE #lab -o Operator7\r\n"
            ":friend!u@fake MODE #lab +o agent7\r\n");
        send_text(agent_fd, ":friend!u@fake MODE #lab +o agent7\r\n");
        tick(client, 10u);
        assert(snj_irc_set_operator_topic(client, "not op",
                                          error, sizeof(error)) < 0);
        assert(snj_irc_set_agent_topic(client, "agent op",
                                       error, sizeof(error)) == 0);
        send_text(operator_fd,
            ":remoteagent!u@fake JOIN #lab\r\n"
            ":remoteagent!u@fake PRIVMSG #lab :old nick is someone else\r\n"
            ":friend!u@fake PART #lab :bye\r\n");
        tick(client, 10u);
        assert(capture.events[SNJ_IRC_MESSAGE] == messages + 1u);
        assert(strcmp(capture.last_message.nick, "remoteagent") == 0);
        assert(snj_irc_send_operator(client, "local renamed op",
                                     error, sizeof(error)) == 0);
        assert(strcmp(capture.last_message.nick, "Operator7") == 0);
        assert(snj_irc_send_agent(client, "local renamed model",
                                  error, sizeof(error)) == 0);
        assert(strcmp(capture.last_message.nick, "agent7") == 0);
    }
    ping_without_engine(operator_fd);
    ping_without_engine(agent_fd);
    snj_irc_close(client);
    for (size_t i = 0u; i < 2u; ++i)
        assert(close(peers[i]) == 0);
    assert(close(listener) == 0);
    snj_config_free(&config);
}

static void
test_independent_owners(void)
{
    struct snj_config config;
    struct capture capture = {0};
    struct snj_irc *irc = NULL;
    unsigned short port = free_port();
    int listeners[2u], peers[2u][2u], human;
    char address[64u], wire[8192u], error[256u] = {0};
    uint64_t started;

    init_server_config(&config, port);
    config.irc_client_count = 2u;
    for (size_t i = 0u; i < 2u; ++i) {
        unsigned short remote;

        listeners[i] = listen_local(&remote);
        endpoint(address, remote);
        assert(snj_strcpy(config.irc_clients[i], sizeof(config.irc_clients[i]),
                           address));
    }
    assert(snj_irc_open(&irc, &config, "/workspace", capture_event,
                        capture_trace, &capture, error, sizeof(error)) == 0);
    tick(irc, 10u);
    for (size_t i = 0u; i < 2u; ++i)
        for (size_t j = 0u; j < 2u; ++j) {
            struct pollfd ready = {listeners[i], POLLIN, 0};

            assert(poll(&ready, 1u, 250) == 1);
            peers[i][j] = accept(listeners[i], NULL, NULL);
            assert(peers[i][j] >= 0);
            assert(fcntl(peers[i][j], F_SETFL, O_NONBLOCK) == 0);
        }
    human = connect_local(port, false);
    register_peer(irc, human, "human", false, wire, sizeof(wire));
    tick(irc, 10u);
    /* Stop engine admission and saturate only the first endpoint's mailbox. */
    for (size_t i = 0u; i < 100u; ++i)
        send_text(peers[0][0], "PING :fill-mailbox\r\n");
    ping_without_engine(human);
    ping_without_engine(peers[1][0]);
    ping_without_engine(peers[1][1]);
    started = snj_monotonic_ms();
    snj_irc_close(irc);
    assert(snj_monotonic_ms() - started < 250u);
    assert(close(human) == 0);
    for (size_t i = 0u; i < 2u; ++i) {
        for (size_t j = 0u; j < 2u; ++j)
            assert(close(peers[i][j]) == 0);
        assert(close(listeners[i]) == 0);
    }
    snj_config_free(&config);
}

static void
test_callback_failure(void)
{
    struct snj_config config;
    struct capture capture = {.fail_message = true};
    struct snj_irc *server;
    char error[256u] = {0};

    init_server_config(&config, free_port());
    server = open_server(&config, &capture);
    assert(snj_irc_send_agent(server, "failed admission", error, sizeof(error)) < 0);
    snj_irc_close(server);
    snj_config_free(&config);
}

int
main(void)
{
    engine_thread = pthread_self();
    assert(setenv("USER", "root", 1) == 0);
    test_validation();
    test_cli_network_roles();
    test_server();
    test_client_reconnect();
    test_default_nick_sequence();
    test_explicit_zero_nick_collision();
    test_client_nick_collision();
    test_client_events();
    test_independent_owners();
    test_callback_failure();
    puts("test_irc: ok");
    return 0;
}
