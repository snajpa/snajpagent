/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"
#include "config.h"
#include "credential.h"
#include "json.h"
#include "provider.h"
#include "turn.h"

#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define REQUEST_MAX (64u * 1024u)
#define BODY_MAX (32u * 1024u)

struct local_server {
    int fd;
    pid_t pid;
    unsigned short port;
};

struct http_request {
    char path[128];
    char headers[REQUEST_MAX];
    char body[BODY_MAX];
    size_t body_len;
};

struct emitted_text {
    struct snj_buf text;
    unsigned int calls;
};

static void
write_all_or_die(int fd, const char *data, size_t len)
{
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            _exit(90);
        }
        if (n == 0)
            _exit(91);
        data += (size_t)n;
        len -= (size_t)n;
    }
}

static void
server_fail(const char *message)
{
    (void)write(STDERR_FILENO, message, strlen(message));
    (void)write(STDERR_FILENO, "\n", 1u);
    _exit(92);
}

static bool
header_contains(const char *headers, const char *needle)
{
    return strstr(headers, needle) != NULL;
}

static long
content_length(const char *headers)
{
    const char *p = strstr(headers, "Content-Length:");
    long value = 0;

    if (!p)
        p = strstr(headers, "content-length:");
    if (!p)
        return -1;
    p = strchr(p, ':');
    if (!p)
        return -1;
    ++p;
    while (*p == ' ' || *p == '\t')
        ++p;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (long)(*p - '0');
        ++p;
    }
    return value;
}

static size_t
find_header_end(const char *buffer, size_t len)
{
    for (size_t i = 0; i + 3u < len; ++i)
        if (buffer[i] == '\r' && buffer[i + 1u] == '\n' &&
            buffer[i + 2u] == '\r' && buffer[i + 3u] == '\n')
            return i + 4u;
    for (size_t i = 0; i + 1u < len; ++i)
        if (buffer[i] == '\n' && buffer[i + 1u] == '\n')
            return i + 2u;
    return 0u;
}

static void
read_request(int fd, struct http_request *request)
{
    char buffer[REQUEST_MAX];
    size_t used = 0u;
    size_t header_end = 0u;
    long cl;
    int matched;

    memset(request, 0, sizeof(*request));
    while (!header_end) {
        ssize_t n;
        if (used == sizeof(buffer))
            server_fail("request headers exceeded test bound");
        n = read(fd, buffer + used, sizeof(buffer) - used);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            server_fail("request read failed");
        }
        if (n == 0)
            server_fail("request closed before headers");
        used += (size_t)n;
        header_end = find_header_end(buffer, used);
    }
    if (header_end >= sizeof(request->headers))
        server_fail("retained headers exceeded test bound");
    memcpy(request->headers, buffer, header_end);
    request->headers[header_end] = '\0';
    matched = sscanf(request->headers, "POST %127s HTTP/1", request->path);
    if (matched != 1)
        server_fail("unexpected request line");
    if (!header_contains(request->headers, "Authorization: Bearer transport-secret"))
        server_fail("authorization header missing or unredacted differently");
    cl = content_length(request->headers);
    if (cl < 0 || cl > (long)(sizeof(request->body) - 1u))
        server_fail("invalid content length");
    request->body_len = (size_t)cl;
    if (used - header_end > request->body_len)
        server_fail("request body overflowed expected length");
    memcpy(request->body, buffer + header_end, used - header_end);
    while (used - header_end < request->body_len) {
        ssize_t n = read(fd, request->body + (used - header_end),
                         request->body_len - (used - header_end));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            server_fail("body read failed");
        }
        if (n == 0)
            server_fail("request closed before body");
        used += (size_t)n;
    }
    request->body[request->body_len] = '\0';
}

static void
send_response(int fd, const char *content_type, const char *body)
{
    char header[256];
    size_t len = strlen(body);
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %llu\r\n"
                     "Connection: close\r\n\r\n",
                     content_type, (unsigned long long)len);
    if (n <= 0 || (size_t)n >= sizeof(header))
        server_fail("response header build failed");
    write_all_or_die(fd, header, (size_t)n);
    write_all_or_die(fd, body, len);
}

static void
serve_one(int listen_fd, const char *path, const char *marker,
          const char *content_type, const char *body)
{
    struct http_request request;
    int fd;

    fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
        server_fail("accept failed");
    read_request(fd, &request);
    if (strcmp(request.path, path) != 0)
        server_fail("unexpected provider endpoint path");
    if (!strstr(request.body, marker))
        server_fail("request body marker missing");
    send_response(fd, content_type, body);
    if (close(fd) < 0)
        server_fail("close accepted socket failed");
}

static void
server_child(int listen_fd)
{
    static const char create_sse[] =
        "event: response.created\n"
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_transport\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "event: response.output_item.added\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"rs_transport\",\"type\":\"reasoning\",\"content\":[],\"summary\":[]}}\n\n"
        "event: response.output_item.done\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"rs_transport\",\"type\":\"reasoning\",\"content\":[],\"summary\":[]}}\n\n"
        "event: response.output_item.added\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":1,\"item\":{\"id\":\"msg_transport\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[]}}\n\n"
        "event: response.content_part.added\n"
        "data: {\"type\":\"response.content_part.added\",\"item_id\":\"msg_transport\",\"output_index\":1,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]}}\n\n"
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_transport\",\"output_index\":1,\"content_index\":0,\"delta\":\"local transport\"}\n\n"
        "event: response.output_text.done\n"
        "data: {\"type\":\"response.output_text.done\",\"item_id\":\"msg_transport\",\"output_index\":1,\"content_index\":0,\"text\":\"local transport\"}\n\n"
        "event: response.output_item.done\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":1,\"item\":{\"id\":\"msg_transport\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"local transport\",\"annotations\":[]}]}}\n\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_transport\",\"status\":\"completed\",\"usage\":{\"input_tokens\":7,\"output_tokens\":2,\"total_tokens\":9},\"output\":[]}}\n\n";

    serve_one(listen_fd, "/v1/responses/input_tokens", "transport-count",
              "application/json",
              "{\"object\":\"response.input_tokens\",\"input_tokens\":7}");
    serve_one(listen_fd, "/v1/responses", "transport-create",
              "text/event-stream", create_sse);
    serve_one(listen_fd, "/v1/responses/compact", "transport-compact",
              "application/json",
              "{\"object\":\"response.compaction\",\"output\":[{\"type\":\"compaction\",\"encrypted_content\":\"transport-compact-output\"}]}");
    _exit(0);
}

static void
start_server(struct local_server *server)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int one = 1;

    memset(server, 0, sizeof(*server));
    server->fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(server->fd >= 0);
    assert(setsockopt(server->fd, SOL_SOCKET, SO_REUSEADDR,
                      &one, sizeof(one)) == 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(server->fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(server->fd, 3) == 0);
    assert(getsockname(server->fd, (struct sockaddr *)&addr, &len) == 0);
    server->port = ntohs(addr.sin_port);
    server->pid = fork();
    assert(server->pid >= 0);
    if (server->pid == 0)
        server_child(server->fd);
}

static void
stop_server(struct local_server *server)
{
    int status;

    assert(close(server->fd) == 0);
    assert(waitpid(server->pid, &status, 0) == server->pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

static json_t *
request_with_marker(const char *marker)
{
    json_t *request = json_object();
    json_t *input = json_array();
    json_t *message = json_object();
    assert(request && input && message);
    assert(snj_json_set_new(message, "role", json_string("user")) == 0);
    assert(snj_json_set_new(message, "content", json_string(marker)) == 0);
    assert(json_array_append_new(input, message) == 0);
    assert(snj_json_set_new(request, "model", json_string("gpt-transport-test")) == 0);
    assert(snj_json_set_new(request, "input", input) == 0);
    return request;
}

static int
emit_capture(void *opaque, size_t item_index, enum snj_item_kind kind,
             enum snj_item_phase phase, const char *provider_item_id,
             const char *text, size_t len)
{
    struct emitted_text *emitted = opaque;

    (void)item_index;
    (void)kind;
    (void)phase;
    (void)provider_item_id;
    ++emitted->calls;
    return snj_buf_append(&emitted->text, text, len);
}

static void
credential_set(struct snj_credential *credential, const char *value)
{
    snj_credential_clear(credential);
    credential->len = strlen(value);
    assert(credential->len <= SNJ_CREDENTIAL_MAX);
    memcpy(credential->value, value, credential->len + 1u);
}

static void
test_local_provider_transport(void)
{
    struct local_server server;
    struct snj_config config;
    struct snj_credential credential;
    struct snj_response_graph graph;
    struct emitted_text emitted;
    json_t *request;
    json_t *compact_output = NULL;
    uint64_t tokens = 0u;
    uint64_t compact_bytes = 0u;
    unsigned int retries = 99u;
    int cancel = 99;
    char endpoint[128];
    char error[256] = {0};

    start_server(&server);
    assert(snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u",
                    (unsigned int)server.port) > 0);
    snj_config_init(&config);
    config.connect_timeout_ms = 1000u;
    config.idle_timeout_ms = 1000u;
    config.request_timeout_ms = 3000u;
    assert(snprintf(config.provider_base_url, sizeof(config.provider_base_url),
                    "%s", endpoint) > 0);
    credential_set(&credential, "transport-secret");

    request = request_with_marker("transport-count");
    assert(snj_provider_responses_count(request, &config, &credential, NULL,
                                        NULL, NULL, &tokens, error,
                                        sizeof(error), &cancel, &retries) == 0);
    assert(tokens == 7u);
    assert(cancel == 0);
    assert(retries == 0u);
    json_decref(request);

    request = request_with_marker("transport-create");
    snj_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snj_buf_init(&emitted.text, 128u);
    assert(snj_provider_responses_create(request, &config, &credential, NULL,
                                         emit_capture, &emitted, NULL, NULL,
                                         &graph, error, sizeof(error), &cancel,
                                         &retries) == 0);
    assert(strcmp(graph.provider_response_id, "resp_transport") == 0);
    assert(graph.count == 1u);
    assert(strcmp(graph.items[0].text, "local transport") == 0);
    assert(emitted.calls == 1u);
    assert(emitted.text.len == strlen("local transport"));
    assert(memcmp(emitted.text.data, "local transport",
                  strlen("local transport")) == 0);
    assert(retries == 0u);
    snj_buf_free(&emitted.text);
    snj_response_graph_free(&graph);
    json_decref(request);

    request = request_with_marker("transport-compact");
    assert(snj_provider_responses_compact(request, &config, &credential, NULL,
                                          NULL, NULL, &compact_output,
                                          &compact_bytes, error, sizeof(error),
                                          &cancel, &retries) == 0);
    assert(json_is_array(compact_output));
    assert(json_array_size(compact_output) == 1u);
    assert(compact_bytes > 0u);
    assert(retries == 0u);
    json_decref(compact_output);
    json_decref(request);

    snj_config_free(&config);
    stop_server(&server);
}

int
main(void)
{
    test_local_provider_transport();
    puts("test_provider_transport: ok");
    return 0;
}
