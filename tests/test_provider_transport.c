/* SPDX-License-Identifier: GPL-2.0-only */
#include "checked_json.h"
#include "base.h"
#include "app_internal.h"
#include "config.h"
#include "credential.h"
#include "json.h"
#include "model_cache.h"
#include "provider.h"
#include "snajpagent.h"
#include "turn.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

#define REQUEST_MAX (64u * 1024u)
#define BODY_MAX (32u * 1024u)

struct local_server {
    int fd;
    pid_t pid;
    unsigned short port;
    char endpoint[128];
};

struct http_request {
    char method[8];
    char path[128];
    char headers[REQUEST_MAX];
    char body[BODY_MAX];
    size_t body_len;
};

struct emitted_text {
    struct snag_buf text;
    unsigned int calls;
};

enum model_fixture {
    MODEL_OPENAI,
    MODEL_CODEX_MALFORMED,
    MODEL_CODEX_LOOKALIKE,
    MODEL_CODEX_FAILURE,
    MODEL_LIMIT_CONFLICT,
    MODEL_CREATE_HTTP_FAILURE,
    MODEL_CREATE_SSE_FAILURE,
    MODEL_OPENROUTER_SEARCH,
    MODEL_CREATE_RETRY,
    MODEL_COUNT_404,
    MODEL_COUNT_405,
    MODEL_COUNT_501,
    MODEL_COUNT_401,
    MODEL_COUNT_403,
    MODEL_COUNT_OK,
    MODEL_COUNT_MODEL_404,
    MODEL_COUNT_OVERFLOW,
    MODEL_COMPACT_404,
    MODEL_COMPACT_403,
    MODEL_AUTH_DEVICE,
    MODEL_AUTH_CANCEL,
    MODEL_AUTH_EXPIRED,
    MODEL_AUTH_REFRESH,
    MODEL_AUTH_REFRESH_FAILURE,
    MODEL_AUTH_401,
    MODEL_AUTH_401_TWICE
};

static bool authentication_fixture;

static const struct retry_case {
    const char *prefix;
    const char *body;
    unsigned int status;
    unsigned int failures;
    unsigned int retries;
    bool truncated;
    const char *diagnostic; /* NULL: failure(s), then success. */
    const char *emitted;
} *retry_case;

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
    matched = sscanf(request->headers, "%7s %127s HTTP/1",
                     request->method, request->path);
    if (matched != 2)
        server_fail("unexpected request line");
    if (!authentication_fixture &&
        !header_contains(request->headers, "Authorization: Bearer transport-secret"))
        server_fail("authorization header missing or unredacted differently");
    if (!authentication_fixture && !header_contains(request->headers,
                         "HTTP-Referer: https://github.com/snajpa/snajpagent"))
        server_fail("OpenRouter referer header missing");
    if (!authentication_fixture && !header_contains(request->headers,
                         "X-OpenRouter-Title: snajpagent"))
        server_fail("OpenRouter title header missing");
    if (!header_contains(request->headers,
                         "User-Agent: " SNAJPAGENT_NAME "/"
                         SNAJPAGENT_VERSION))
        server_fail("product user agent missing or stale");
    cl = content_length(request->headers);
    if (strcmp(request->method, "GET") == 0 && cl < 0)
        cl = 0;
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
send_response(int fd, unsigned int status, const char *content_type, const char *body)
{
    char header[256];
    size_t len = strlen(body);
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %u Test\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %llu\r\n"
                     "Connection: close\r\n\r\n",
                     status, content_type, (unsigned long long)len);
    if (n <= 0 || (size_t)n >= sizeof(header))
        server_fail("response header build failed");
    write_all_or_die(fd, header, (size_t)n);
    write_all_or_die(fd, body, len);
}

static void
serve_one(int listen_fd, unsigned int status, const char *method, const char *path,
          const char *marker,
          const char *content_type, const char *body)
{
    struct http_request request;
    int fd;

    fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
        server_fail("accept failed");
    read_request(fd, &request);
    if (strcmp(request.method, method) != 0)
        server_fail("unexpected provider HTTP method");
    if (strcmp(request.path, path) != 0)
        server_fail("unexpected provider endpoint path");
    if (marker && !strstr(request.body, marker))
        server_fail("request body marker missing");
    send_response(fd, status, content_type, body);
    if (close(fd) < 0)
        server_fail("close accepted socket failed");
}

static void
auth_server_child(int listen_fd, enum model_fixture fixture)
{
    static const char tokens[] =
        "{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"expires_in\":3600,"
        "\"id_token\":\"e30.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjdC10ZXN0In19.sig\"}";
    unsigned int count = fixture == MODEL_AUTH_DEVICE ? 4u :
        fixture == MODEL_AUTH_CANCEL || fixture == MODEL_AUTH_EXPIRED ? 2u :
        fixture >= MODEL_AUTH_401 ? 3u : 1u;

    authentication_fixture = true;
    (void)alarm(15u);
    for (unsigned int i = 0; i < count; ++i) {
        struct http_request request;
        const char *body = tokens;
        unsigned int status = 200u;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0)
            server_fail("auth accept failed");
        read_request(fd, &request);
        if (fixture <= MODEL_AUTH_EXPIRED) {
            const char *path = i == 0u ? "/api/accounts/deviceauth/usercode" :
                i == 3u ? "/oauth/token" : "/api/accounts/deviceauth/token";
            if (strcmp(request.path, path))
                server_fail("incorrect device flow path");
            if (i == 0u) {
                if (!strstr(request.body, "app_EMoamEEZ73f0CkXaXp7hrann"))
                    server_fail("missing device client identifier");
                body = "{\"device_auth_id\":\"device-id\",\"user_code\":\"CODE-1234\",\"interval\":\"1\"}";
            } else if (i == 1u) {
                status = fixture == MODEL_AUTH_EXPIRED ? 410u : 403u;
                body = "{}";
            } else if (i == 2u) {
                body = "{\"authorization_code\":\"auth-code\",\"code_verifier\":\"verifier\",\"code_challenge\":\"challenge\"}";
            } else if (!strstr(request.body, "grant_type=authorization_code") ||
                       !strstr(request.body, "code_verifier=verifier")) {
                server_fail("missing PKCE code exchange");
            }
        } else if (fixture >= MODEL_AUTH_401 && i != 1u) {
            if (strcmp(request.path, "/models?client_version=0.146.0") ||
                !strstr(request.headers, "ChatGPT-Account-Id: acct-test") ||
                !strstr(request.headers, i == 0u ? "Bearer old-access" : "Bearer new-access"))
                server_fail("wrong Codex account or access header");
            if (i == 0u || fixture == MODEL_AUTH_401_TWICE) {
                status = 401u;
                body = "{\"error\":{\"message\":\"not authorized\"}}";
            } else {
                body = "{\"models\":[{\"slug\":\"gpt-5.6-luna\",\"visibility\":\"list\",\"priority\":0,\"default_reasoning_level\":\"high\",\"supported_reasoning_levels\":[{\"effort\":\"high\"}]}]}";
            }
        } else {
            if (strcmp(request.path, "/oauth/token") ||
                !strstr(request.body, "\"grant_type\":\"refresh_token\"") ||
                !strstr(request.body, "old-refresh"))
                server_fail("wrong refresh request");
            if (fixture == MODEL_AUTH_REFRESH_FAILURE) {
                status = 401u;
                body = "{\"error\":\"private-refresh-server-detail\"}";
            }
        }
        send_response(fd, status, "application/json", body);
        (void)close(fd);
    }
    _exit(0);
}

static void
server_child(int listen_fd, enum model_fixture models, bool transport)
{
    if (models >= MODEL_AUTH_DEVICE)
        auth_server_child(listen_fd, models);
    if (models == MODEL_COMPACT_404 || models == MODEL_COMPACT_403) {
        struct http_request request;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0)
            server_fail("compact accept failed");
        read_request(fd, &request);
        if (strcmp(request.method, "POST") ||
            (strcmp(request.path, "/responses/compact") && strcmp(request.path, "/v1/responses/compact")))
            server_fail("invalid native compact path");
        send_response(fd, models == MODEL_COMPACT_404 ? 404u : 403u, "application/json", "{\"detail\":\"Not Found\"}");
        (void)close(fd);
        _exit(0);
    }
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

    if (models == MODEL_OPENROUTER_SEARCH) {
        serve_one(listen_fd, 200u, "POST", "/v1/responses", "openrouter:web_search",
                  "text/event-stream",
                  "event: response.created\n"
                  "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_search\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
                  "event: response.output_item.added\n"
                  "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"openrouter:web_search\",\"id\":\"ws_tmp_abc123\",\"status\":\"in_progress\"}}\n\n"
                  "event: response.output_item.done\n"
                  "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"openrouter:web_search\",\"id\":\"ws_tmp_abc123\",\"status\":\"completed\",\"action\":{\"type\":\"search\",\"query\":\"example domains\",\"sources\":[{\"type\":\"url\",\"url\":\"https://example.com\"}]}}}\n\n"
                  "event: response.output_item.done\n"
                  "data: {\"type\":\"response.output_item.done\",\"output_index\":1,\"item\":{\"type\":\"message\",\"id\":\"msg_search\",\"role\":\"assistant\",\"status\":\"completed\",\"content\":[{\"type\":\"output_text\",\"text\":\"Found https://example.com\",\"annotations\":[{\"type\":\"url_citation\",\"url\":\"https://example.com\",\"title\":\"Example\",\"start_index\":6,\"end_index\":25}]}]}}\n\n"
                  "event: response.output_item.done\n"
                  "data: {\"type\":\"response.output_item.done\",\"output_index\":2,\"item\":{\"type\":\"function_call\",\"id\":\"fc_after_search\",\"call_id\":\"call_after_search\",\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"README.md\\\",\\\"start_line\\\":1,\\\"end_line\\\":1}\",\"status\":\"completed\"}}\n\n"
                  "event: response.completed\n"
                  "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_search\",\"status\":\"completed\",\"output\":[]}}\n\n"
                  "data: [DONE]\n\n");
        serve_one(listen_fd, 200u, "POST", "/v1/responses", "function_call_output",
                  "text/event-stream", create_sse);
        _exit(0);
    }
    if (models == MODEL_CREATE_RETRY) {
        struct http_request request;
        char first[BODY_MAX], body[BODY_MAX];
        unsigned int attempts = retry_case->failures + !retry_case->diagnostic;
        alarm(15u);
        for (unsigned int i = 0; i < attempts; ++i) {
            int fd = accept(listen_fd, NULL, NULL);
            if (fd < 0)
                server_fail("retry accept failed");
            read_request(fd, &request);
            if (strcmp(request.method, "POST") || strcmp(request.path, "/v1/responses"))
                server_fail("unexpected retry request");
            if (i == 0)
                memcpy(first, request.body, request.body_len + 1u);
            else if (strcmp(first, request.body))
                server_fail("retry changed request bytes");
            if (i == retry_case->failures) {
                send_response(fd, 200u, "text/event-stream", create_sse);
            } else {
                int n = snprintf(body, sizeof(body), "%s%s", retry_case->prefix,
                                 retry_case->body);
                if (n < 0 || (size_t)n >= sizeof(body))
                    server_fail("retry fixture overflow");
                if (retry_case->truncated) {
                    char header[256];
                    int h = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                        "Content-Length: %u\r\nConnection: close\r\n\r\n",
                        (unsigned int)n + 100u);
                    assert(h > 0 && (size_t)h < sizeof(header));
                    write_all_or_die(fd, header, (size_t)h);
                    write_all_or_die(fd, body, (size_t)n);
                } else if (retry_case->status == 200u) {
                    send_response(fd, 200u, "text/event-stream", body);
                } else {
                    send_response(fd, retry_case->status, "application/json", body);
                }
            }
            if (close(fd) < 0)
                server_fail("close retry socket failed");
        }
        _exit(0);
    }
    if (models >= MODEL_COUNT_404) {
        unsigned int status = models == MODEL_COUNT_404 ? 404u :
                              models == MODEL_COUNT_405 ? 405u :
                              models == MODEL_COUNT_401 ? 401u :
                              models == MODEL_COUNT_403 ? 403u :
                              models == MODEL_COUNT_OK ? 200u :
                              models == MODEL_COUNT_MODEL_404 ? 404u :
                              models == MODEL_COUNT_OVERFLOW ? 400u : 501u;
        const char *body = models == MODEL_COUNT_OK ?
            "{\"object\":\"response.input_tokens\",\"input_tokens\":42}" :
            models == MODEL_COUNT_MODEL_404 ?
            "{\"error\":{\"code\":\"model_not_found\",\"message\":\"no such model\"}}" :
            models == MODEL_COUNT_OVERFLOW ?
            "{\"error\":{\"code\":400,\"type\":\"exceed_context_size_error\",\"n_ctx\":10,\"n_prompt_tokens\":12}}" :
            "{\"error\":{\"message\":\"not available\"}}";
        serve_one(listen_fd, status, "POST", "/v1/responses/input_tokens", NULL,
                  "application/json", body);
        _exit(0);
    }
    static const struct {
        enum model_fixture fixture;
        unsigned int status;
        const char *method, *path, *content_type, *body;
    } replies[] = {
        {MODEL_CREATE_HTTP_FAILURE, 400u, "POST", "/v1/responses", "application/json",
         "{\"error\":{\"code\":\"context_length_exceeded\","
         "\"message\":\"too large\",\"max_context_tokens\":272000,"
         "\"requested_input_tokens\":300000}}"},
        {MODEL_CREATE_SSE_FAILURE, 200u, "POST", "/v1/responses", "text/event-stream",
         "event: response.failed\n"
         "data: {\"type\":\"response.failed\",\"response\":{"
         "\"error\":{\"code\":\"context_length_exceeded\","
         "\"message\":\"stream too large transport-secret\","
         "\"context_length\":872000}}}\n\n"},
        {MODEL_CODEX_FAILURE, 400u, "GET", "/backend-api/codex/models?client_version=0.146.0", "application/json",
         "{\"error\":{\"message\":\"catalog rejected\"}}"},
        {MODEL_CODEX_MALFORMED, 200u, "GET", "/backend-api/codex/models?client_version=0.146.0", "application/json",
         "{\"models\":[{\"slug\":\"malformed\",\"visibility\":\"list\",\"priority\":1,\"supported_reasoning_levels\":[\"high\"]}]}"},
        {MODEL_CODEX_LOOKALIKE, 200u, "GET", "/backend-api/codexish/v1/models", "application/json",
         "{\"data\":[{\"id\":\"lookalike-openai\"}]}"},
        {MODEL_LIMIT_CONFLICT, 200u, "GET", "/v1/models", "application/json",
         "{\"data\":[{\"id\":\"conflict\",\"context_length\":100,"
         "\"metadata\":{\"contextWindow\":101}}]}"},
        {MODEL_OPENAI, 200u, "GET", "/v1/models", "application/json",
         "{\"object\":\"list\",\"data\":[{\"id\":\"gpt-standard\","
         "\"contextLength\":100000,\"metadata\":{\"context_window\":100000,"
         "\"inputContextWindow\":90000,\"supported_reasoning_levels\":[\"medium\",\"high\"],"
         "\"default_reasoning_level\":\"medium\"},\"capabilities\":{"
         "\"maxOutputTokens\":10000,\"effective_context_window_percent\":80}},"
         "{\"id\":\"future-standard\",\"supported_reasoning_levels\":[\"quantum\",\"cosmic\"]}]}"}
    };
    size_t reply = sizeof(replies) / sizeof(replies[0]) - 1u;
    for (size_t i = 0; i < sizeof(replies) / sizeof(replies[0]); ++i)
        if (replies[i].fixture == models)
            reply = i;
    serve_one(listen_fd, replies[reply].status, replies[reply].method,
              replies[reply].path, NULL, replies[reply].content_type, replies[reply].body);
    if (!transport || models == MODEL_CREATE_HTTP_FAILURE ||
        models == MODEL_CREATE_SSE_FAILURE || models == MODEL_CODEX_FAILURE)
        _exit(0);
    serve_one(listen_fd, 200u, "POST", "/v1/responses/input_tokens", "transport-count",
              "application/json",
              "{\"object\":\"response.input_tokens\",\"input_tokens\":7}");
    serve_one(listen_fd, 200u, "POST", "/v1/responses", "transport-create",
              "text/event-stream", create_sse);
    serve_one(listen_fd, 200u, "POST", "/v1/responses/compact", "transport-compact",
              "application/json",
              "{\"object\":\"response.compaction\",\"output\":[{\"type\":\"compaction\",\"encrypted_content\":\"transport-compact-output\"}]}");
    _exit(0);
}

static void
start_server(struct local_server *server, enum model_fixture models,
             bool transport, const char *suffix)
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
    assert(listen(server->fd, 4) == 0);
    assert(getsockname(server->fd, (struct sockaddr *)&addr, &len) == 0);
    server->port = ntohs(addr.sin_port);
    int written = snprintf(server->endpoint, sizeof(server->endpoint),
                           "http://127.0.0.1:%u%s", server->port, suffix);
    assert(written > 0 && (size_t)written < sizeof(server->endpoint));
    server->pid = fork();
    assert(server->pid >= 0);
    if (server->pid == 0)
        server_child(server->fd, models, transport);
    if (models == MODEL_CREATE_RETRY) {
        assert(close(server->fd) == 0);
        server->fd = -1;
    }
}

static void
stop_server(struct local_server *server)
{
    int status;

    if (server->fd >= 0)
        assert(close(server->fd) == 0);
    assert(waitpid(server->pid, &status, 0) == server->pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

static json_t *
request_with_marker(const char *marker)
{
    return checked_json(json_pack("{s:s,s:[{s:s,s:s}]}", "model", "gpt-transport-test",
        "input", "role", "user", "content", marker));
}

static int
emit_capture(void *opaque, size_t item_index, enum snag_item_kind kind,
             enum snag_item_phase phase, const char *provider_item_id,
             const char *text, size_t len)
{
    struct emitted_text *emitted = opaque;

    (void)item_index;
    (void)kind;
    (void)phase;
    (void)provider_item_id;
    ++emitted->calls;
    return snag_buf_append(&emitted->text, text, len);
}

static void
credential_set(struct snag_credential *credential, const char *value)
{
    snag_credential_clear(credential);
    credential->len = strlen(value);
    assert(credential->len <= SNAG_CREDENTIAL_MAX);
    memcpy(credential->value, value, credential->len + 1u);
}

static void
transport_settings(struct snag_provider_config *provider, struct snag_credential *credential)
{
    provider->connect_timeout_ms = 1000u;
    provider->idle_timeout_ms = 1000u;
    provider->request_timeout_ms = 3000u;
    assert(snag_strcpy(provider->openrouter_referer, sizeof(provider->openrouter_referer),
                       "https://github.com/snajpa/snajpagent"));
    assert(snag_strcpy(provider->openrouter_title, sizeof(provider->openrouter_title),
                       "snajpagent"));
    credential_set(credential, "transport-secret");
}

static struct snag_provider_connection
transport_connection(struct snag_config *config, struct snag_credential *credential,
                     const char *base_url)
{
    snag_config_init(config);
    assert(snag_strcpy(config->providers[0].base_url,
                       sizeof(config->providers[0].base_url), base_url));
    transport_settings(&config->providers[0], credential);
    return (struct snag_provider_connection){config, &config->providers[0], credential, NULL, NULL, NULL};
}

static void
test_local_provider_transport(void)
{
    struct local_server server;
    struct snag_config config;
    struct snag_credential credential;
    struct emitted_text emitted;
    json_t *request;
    struct snag_json_document compact_output = {0};
    json_t *models = NULL;
    uint64_t tokens = 0u;
    unsigned int retries = 99u;
    char error[256] = {0};

    start_server(&server, MODEL_OPENAI, true, "");
    struct snag_provider_connection connection = {
        &config, &config.providers[1], &credential, NULL,
        NULL, NULL};
    snag_config_init(&config);
    snag_config_provider_init(&config.providers[1], "second");
    config.provider_count = 2u;
    assert(snprintf(config.providers[1].name,
                    sizeof(config.providers[1].name), "transport") > 0);
    assert(snprintf(config.providers[1].base_url,
                    sizeof(config.providers[1].base_url),
                    "%s/v1/", server.endpoint) > 0);
    transport_settings(&config.providers[1], &credential);

    assert(snag_provider_models_list(connection,
        &models, error, sizeof(error)) == 0);
    assert(json_array_size(models) == 2u);
    assert(strcmp(snag_json_string(json_array_get(models, 0), "id"),
                  "gpt-standard") == 0);
    assert(strcmp(json_string_value(json_array_get(json_object_get(
                      json_array_get(models, 0), "efforts"), 1)),
                  "high") == 0);
    assert(strcmp(snag_json_string(json_array_get(models, 0),
                                  "default_effort"), "medium") == 0);
    {
        json_t *limits = json_object_get(json_array_get(models, 0), "limits");
        assert(limits);
        assert(json_integer_value(json_object_get(
                   limits, "context_window_tokens")) == 100000);
        assert(json_integer_value(json_object_get(
                   limits, "input_context_window_tokens")) == 90000);
        assert(json_integer_value(json_object_get(
                   limits, "max_output_tokens")) == 10000);
        assert(json_integer_value(json_object_get(
                   limits, "effective_context_window_percent")) == 80);
        assert(json_is_null(json_object_get(limits, "max_input_tokens")));
    }
    {
        json_t *limits = json_object_get(json_array_get(models, 1), "limits");
        assert(limits);
        assert(json_is_null(json_object_get(
                   limits, "context_window_tokens")));
        assert(json_is_null(json_object_get(limits, "max_output_tokens")));
    }
    assert(strcmp(snag_model_cache_best_effort(json_array_get(models, 1),
                                              "fallback"),
                  "quantum") == 0);
    json_decref(models);
    models = NULL;

    request = request_with_marker("transport-count");
    assert(snag_provider_responses_count(connection,
        request, &tokens, NULL, error, sizeof(error), &retries) == 0);
    assert(tokens == 7u);
    assert(retries == 0u);
    json_decref(request);

    request = request_with_marker("transport-create");
    struct snag_response_graph graph = {0};
    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 128u);
    assert(snag_provider_responses_create(connection,
        request, emit_capture, &emitted, &graph, NULL, error, sizeof(error), &retries) == 0);
    assert(strcmp(graph.provider_response_id, "resp_transport") == 0);
    assert(graph.count == 1u);
    assert(strcmp(snag_response_graph_item(&graph, 0).text, "local transport") == 0);
    assert(emitted.calls == 1u);
    assert(emitted.text.len == strlen("local transport"));
    assert(memcmp(emitted.text.data, "local transport",
                  strlen("local transport")) == 0);
    assert(retries == 0u);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
    json_decref(request);

    request = request_with_marker("transport-compact");
    assert(snag_provider_responses_compact(connection,
        request, &compact_output, error, sizeof(error), &retries) == 0);
    assert(json_is_array(compact_output.value));
    assert(json_array_size(compact_output.value) == 1u);
    assert(compact_output.bytes > 0u);
    assert(retries == 0u);
    snag_json_document_free(&compact_output);
    json_decref(request);

    snag_config_free(&config);
    stop_server(&server);
}

static void
test_codex_path_selection(void)
{
    static const struct {
        enum model_fixture fixture;
        const char *path, *provider, *base_override, *id, *diagnostic;
        size_t count;
    } cases[] = {
        {MODEL_CODEX_LOOKALIKE, "/backend-api/codexish", "codex", NULL,
         "lookalike-openai", NULL, 1u},
        {MODEL_OPENAI, "", "codex", "http://backend-api/codex", NULL, NULL, 2u},
        {MODEL_CODEX_MALFORMED, "/backend-api/codex", "codex", NULL,
         NULL, "invalid model entry", 0u},
        {MODEL_CODEX_FAILURE, "/backend-api/codex", "neutral", NULL,
         NULL, "catalog rejected", 0u},
        {MODEL_LIMIT_CONFLICT, "", "neutral", NULL, NULL, "invalid model entry", 0u}
    };
    struct local_server server;
    struct snag_config config;
    struct snag_credential credential;
    char error[256] = {0};
    struct snag_provider_connection connection = {
        &config, &config.providers[0], &credential, NULL, NULL, NULL};

    snag_config_init(&config);
    transport_settings(&config.providers[0], &credential);
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        json_t *models = NULL;
        start_server(&server, cases[i].fixture, false, cases[i].path);
        assert(snag_strcpy(config.providers[0].name,
                           sizeof(config.providers[0].name), cases[i].provider));
        assert(snag_strcpy(config.providers[0].base_url, sizeof(config.providers[0].base_url),
                           cases[i].base_override ? cases[i].base_override : server.endpoint));
        if (cases[i].base_override)
            assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", server.endpoint, 1) == 0);
        int rc = snag_provider_models_list(connection, &models, error, sizeof(error));
        if (cases[i].diagnostic) {
            assert(rc < 0 && models == NULL);
            assert(strstr(error, cases[i].diagnostic));
        } else {
            assert(rc == 0 && json_array_size(models) == cases[i].count);
            if (cases[i].id)
                assert(!strcmp(snag_json_string(json_array_get(models, 0), "id"), cases[i].id));
        }
        json_decref(models);
        if (cases[i].base_override)
            assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
        stop_server(&server);
    }
    snag_config_free(&config);
}

static void
test_structured_create_failures(void)
{
    const enum model_fixture fixtures[] = {
        MODEL_CREATE_HTTP_FAILURE, MODEL_CREATE_SSE_FAILURE
    };

    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        struct local_server server;
        struct snag_config config;
        struct snag_credential credential;
        struct snag_provider_failure failure;
        json_t *request = request_with_marker("capacity-failure");
        char error[256] = {0};

        start_server(&server, fixtures[i], false, "/v1");
        struct snag_provider_connection connection =
            transport_connection(&config, &credential, server.endpoint);
        struct snag_response_graph graph = {0};
        memset(&failure, 0, sizeof(failure));
        assert(snag_provider_responses_create(connection,
            request, NULL, NULL, &graph, &failure, error, sizeof(error), NULL) < 0);
        assert(snag_provider_failure_is_capacity(&failure));
        assert(!strstr(error, "transport-secret"));
        assert(!strstr(failure.message, "transport-secret"));
        assert(failure.context_limit_tokens ==
               (fixtures[i] == MODEL_CREATE_HTTP_FAILURE ?
                    272000u : 872000u));
        if (fixtures[i] == MODEL_CREATE_HTTP_FAILURE) {
            assert(failure.requested_input_tokens == 300000u);
        }
        snag_response_graph_free(&graph);
        json_decref(request);
        snag_config_free(&config);
        stop_server(&server);
    }
}

struct retry_cancel {
    int fd, code;
    struct snag_buf notice;
};

static int
cancel_retry(void *opaque, uint32_t wait_ms)
{
    struct retry_cancel *cancel = opaque;
    char buf[512];
    ssize_t n;
    (void)wait_ms;
    while ((n = read(cancel->fd, buf, sizeof(buf))) > 0)
        assert(snag_buf_append(&cancel->notice, buf, (size_t)n) == 0);
    assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    return cancel->notice.data && strstr((const char *)cancel->notice.data,
        "provider retry 1/2") ? cancel->code : 0;
}

static void
test_create_retries(void)
{
    static const char created[] =
        ": heartbeat\n\n"
        "id: discarded-event\n"
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"discarded\",\"status\":\"in_progress\",\"output\":[]}}\n\n";
    static const char transient[] =
        "data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":\"server_error\","
        "\"message\":\"Response payload is not completed: <TransferEncodingError: 400, message='Not enough data to satisfy transfer length header.'> transport-secret\"}}}\n\n";
    static const char partial[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"partial\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"m\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"phase\":\"commentary\",\"content\":[]}}\n\n"
        "data: {\"type\":\"response.content_part.added\",\"output_index\":0,\"item_id\":\"m\",\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\"}}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"output_index\":0,\"item_id\":\"m\",\"content_index\":0,\"delta\":\"once\"}\n\n";
    const struct retry_case cases[] = {
        {created, transient, 200, 1, 1, false, NULL, "local transport"},
        {created, transient, 200, 3, 2, false, "retried 2 times", ""},
        {"", "data: {\"type\":\"error\",\"code\":\"rate_limit_exceeded\"}\n\n",
            200, 1, 1, false, NULL, "local transport"},
        {"", "data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"type\":\"service_unavailable_error\"}}}\n\n",
            200, 1, 1, false, NULL, "local transport"},
        {"", "data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":\"cyber_policy\",\"message\":\"flagged for possible cybersecurity risk\"}}}\n\n",
            200, 1, 0, false, "[cyber_policy]", ""},
        {"", "data: {\"type\":\"error\",\"code\":\"unknown\",\"message\":\"TransferEncodingError\"}\n\n",
            200, 1, 0, false, "[unknown]", ""},
        {"", "data: {\"type\":\"error\",\"message\":\"TransferEncodingError\"}\n\n",
            200, 1, 0, false, "TransferEncodingError", ""},
        {"", "data: {\"type\":\"response.incomplete\",\"response\":{\"incomplete_details\":{\"reason\":\"max_output_tokens\"}}}\n\n",
            200, 1, 0, false, "[max_output_tokens]", ""},
        {"", "data: {\"type\":\"error\",\"code\":\"server_error\",\"context_limit\":-1}\n\n",
            200, 1, 0, false, "invalid structured", ""},
        {"", "{\"error\":{\"code\":\"slow_down\",\"type\":\"rate_limit_error\"}}",
            429, 1, 1, false, NULL, "local transport"},
        {"", "{\"error\":{\"code\":\"credit_balance_exhausted\",\"type\":\"insufficient_quota\"}}",
            429, 1, 0, false, "credit_balance_exhausted", ""},
        {"", "{\"error\":{\"type\":\"insufficient_quota\"}}",
            429, 1, 0, false, "insufficient_quota", ""},
        {"", "{\"error\":{\"code\":\"cyber_policy\"}}",
            500, 1, 0, false, "cyber_policy", ""},
        {"", "{\"error\":{\"code\":\"unknown\",\"type\":\"server_error\"}}",
            503, 1, 0, false, "unknown", ""},
        {"", "{\"error\":{\"code\":23,\"type\":\"server_error\"}}",
            500, 1, 0, false, "HTTP 500", ""},
        {"", "{\"error\":{\"code\":\"server_error\"}}",
            503, 3, 2, false, "retried 2 times", ""},
        {"", "temporarily unavailable", 503, 1, 1, false, NULL, "local transport"},
        {"", "", 200, 1, 1, true, NULL, "local transport"},
        {created, "", 200, 1, 1, true, NULL, "local transport"},
        {created, "", 200, 1, 1, false, NULL, "local transport"},
        {created, "data: {", 200, 1, 0, false, "mid-event", ""},
        {created, "data: {", 200, 1, 0, true, "provider transport failed", ""},
        {created, "data: {bad}\n\n", 200, 1, 0, false, "JSON", ""},
        {partial, transient, 200, 1, 0, false, "[server_error]", "once"},
        {partial, "", 200, 1, 0, true, "provider transport failed", "once"},
        {created, "data: {\"type\":\"response.failed\",\"response\":{\"output\":[{\"type\":\"function_call\"}],\"error\":{\"code\":\"server_error\"}}}\n\n",
            200, 1, 0, false, "[server_error]", ""},
        {"data: {\"type\":\"response.web_search_call.in_progress\"}\n\n",
            transient, 200, 1, 0, false, "[server_error]", ""},
        {"data: {\"type\":\"response.future_activity\"}\n\n",
            transient, 200, 1, 0, false, "[server_error]", ""},
        {"data: {\"type\":\"response.created\",\"response\":{\"id\":\"tools\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
         "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"function_call\",\"status\":\"in_progress\",\"id\":\"f\",\"call_id\":\"c\",\"name\":\"exec_command\",\"arguments\":\"\"}}\n\n",
            transient, 200, 1, 0, false, "[server_error]", ""}
    };

    size_t count = sizeof(cases) / sizeof(cases[0]);
    const struct retry_case cancelled = {created, transient, 200, 1, 0, false, "", ""};
    for (size_t i = 0; i < count + 3u; ++i) {
        struct local_server server;
        struct snag_config config;
        struct snag_credential credential;
        struct snag_provider_failure failure;
        struct emitted_text emitted = {0};
        json_t *request = request_with_marker("retry-current-cycle");
        char error[512] = {0};
        unsigned int retries = 99u;
        int pipefd[2], saved_stderr = -1;
        struct snag_ui ui;
        struct retry_cancel cancellation = {.code = i < count ? 0 : (int)(i - count + 1u)};

        retry_case = i < count ? &cases[i] : &cancelled;
        start_server(&server, MODEL_CREATE_RETRY, false, "/v1");
        if (cancellation.code) {
            assert(pipe(pipefd) == 0);
            assert(fcntl(pipefd[0], F_SETFL, O_NONBLOCK) == 0);
            saved_stderr = dup(STDERR_FILENO);
            assert(saved_stderr >= 0 && dup2(pipefd[1], STDERR_FILENO) == STDERR_FILENO);
            assert(close(pipefd[1]) == 0);
            cancellation.fd = pipefd[0];
            snag_buf_init(&cancellation.notice, 4096u);
            assert(snag_ui_init(&ui) == 0);
        }
        snag_config_init(&config);
        strcpy(config.providers[0].base_url, server.endpoint);
        strcpy(config.providers[0].openrouter_referer, "https://github.com/snajpa/snajpagent");
        strcpy(config.providers[0].openrouter_title, "snajpagent");
        config.providers[0].request_timeout_ms = 3000u;
        credential_set(&credential, "transport-secret");
        struct snag_response_graph graph = {0};
        snag_buf_init(&emitted.text, 1024u);
        int rc = snag_provider_responses_create((struct snag_provider_connection){
            &config, &config.providers[0], &credential, cancellation.code ? &ui : NULL,
            cancellation.code ? cancel_retry : NULL, &cancellation},
            request, emit_capture, &emitted, &graph, &failure, error, sizeof(error), &retries);
        if (cancellation.code) {
            snag_ui_free(&ui);
            assert(dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);
            assert(close(saved_stderr) == 0 && close(pipefd[0]) == 0);
            snag_buf_free(&cancellation.notice);
        }
        if ((!cancellation.code && (retry_case->diagnostic ? rc >= 0 : rc != 0)) ||
            retries != retry_case->retries ||
            (retry_case->diagnostic && !strstr(error, retry_case->diagnostic)))
            fprintf(stderr, "retry case %zu: rc=%d retries=%u: %s\n", i, rc, retries, error);
        int expected_cancel = cancellation.code == SNAG_PROVIDER_NEW_INPUT ? 0 : cancellation.code;
        assert(rc == (expected_cancel ? expected_cancel : retry_case->diagnostic ? -1 : 0));
        assert(retries == retry_case->retries);
        assert(!strstr(error, "transport-secret") && !strstr(failure.message, "transport-secret"));
        if (retry_case->diagnostic) {
            assert(strstr(error, retry_case->diagnostic));
            assert(graph.count == 0u);
        } else {
            assert(!failure.code[0] && !failure.type[0] && !failure.message[0]);
            assert(graph.count == 1u && strcmp(graph.provider_response_id, "resp_transport") == 0);
        }
        assert(emitted.text.len == strlen(retry_case->emitted));
        if (emitted.text.len)
            assert(memcmp(emitted.text.data, retry_case->emitted, emitted.text.len) == 0);
        snag_response_graph_free(&graph);
        snag_buf_free(&emitted.text);
        snag_credential_clear(&credential);
        snag_config_free(&config);
        json_decref(request);
        stop_server(&server);
    }
    retry_case = NULL;
}

static void
test_policy_clarification_after_reasoning(void)
{
    const struct retry_case policy = {
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"policy\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"reasoning\",\"id\":\"r\",\"summary\":[]}}\n\n"
        "data: {\"type\":\"response.reasoning_summary_text.delta\",\"output_index\":0,\"item_id\":\"r\",\"summary_index\":0,\"delta\":\"Reviewing the task\"}\n\n",
        "data: {\"type\":\"response.failed\",\"response\":{\"output\":[{\"type\":\"reasoning\",\"id\":\"r\",\"summary\":[]}],\"error\":{\"code\":\"cyber_policy\"}}}\n\n",
        200, 1, 0, false, "[cyber_policy]", ""
    };
    struct local_server server;
    struct snag_config config;
    struct snag_credential credential;
    struct snag_provider_failure failure;
    struct snag_response_graph graph = {0};
    struct emitted_text emitted = {0};
    json_t *request = request_with_marker("policy-reasoning");
    char error[512] = {0};
    unsigned int retries = 99u;

    retry_case = &policy;
    start_server(&server, MODEL_CREATE_RETRY, false, "/v1");
    snag_config_init(&config);
    strcpy(config.providers[0].base_url, server.endpoint);
    strcpy(config.providers[0].openrouter_referer, "https://github.com/snajpa/snajpagent");
    strcpy(config.providers[0].openrouter_title, "snajpagent");
    credential_set(&credential, "transport-secret");
    snag_buf_init(&emitted.text, 1024u);
    int rc = snag_provider_responses_create((struct snag_provider_connection){
        &config, &config.providers[0], &credential, NULL, NULL, NULL},
        request, emit_capture, &emitted, &graph, &failure, error, sizeof(error), &retries);
    assert(rc < 0 && retries == 0u && emitted.text.len == 0u);
    assert(!strcmp(failure.code, "cyber_policy"));
    assert(failure.output_correction == SNAG_OUTPUT_CORRECTION_CYBER_POLICY);
    snag_response_graph_free(&graph);
    snag_buf_free(&emitted.text);
    snag_credential_clear(&credential);
    snag_config_free(&config);
    json_decref(request);
    stop_server(&server);
    retry_case = NULL;
}

static void
test_count_capability_statuses(void)
{
    const enum model_fixture fixtures[] = {
        MODEL_COUNT_404, MODEL_COUNT_405, MODEL_COUNT_501
    };
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        struct local_server server;
        struct snag_config config;
        struct snag_credential credential;
        json_t *request = request_with_marker("count-status");
        uint64_t tokens = 0u;
        bool endpoint_unsupported = true;
        char error[256] = {0};

        start_server(&server, fixtures[i], false, "/v1");
        struct snag_provider_connection connection =
            transport_connection(&config, &credential, server.endpoint);
        assert(snag_provider_responses_count(connection,
            request, &tokens, &endpoint_unsupported, error, sizeof(error), NULL) < 0);
        assert(endpoint_unsupported);
        json_decref(request);
        snag_config_free(&config);
        stop_server(&server);
    }
}

static void
test_count_modes(void)
{
    const struct {
        enum model_fixture fixture;
        enum snag_token_count_mode mode;
        int result;
        enum snag_count_capability capability;
        bool openrouter;
    } cases[] = {
        {MODEL_COUNT_405, SNAG_TOKEN_COUNT_AUTO,
         SNAG_APP_COUNT_SKIPPED, SNAG_COUNT_UNSUPPORTED, false},
        {MODEL_COUNT_405, SNAG_TOKEN_COUNT_STRICT, -1, SNAG_COUNT_UNSUPPORTED, false},
        {MODEL_COUNT_404, SNAG_TOKEN_COUNT_AUTO, SNAG_APP_COUNT_SKIPPED, SNAG_COUNT_UNSUPPORTED, false},
        {MODEL_COUNT_404, SNAG_TOKEN_COUNT_AUTO,
         SNAG_APP_COUNT_SKIPPED, SNAG_COUNT_UNSUPPORTED, true},
        {MODEL_COUNT_404, SNAG_TOKEN_COUNT_STRICT, -1, SNAG_COUNT_UNSUPPORTED, true},
        {MODEL_COUNT_401, SNAG_TOKEN_COUNT_AUTO, -1, SNAG_COUNT_UNKNOWN, true},
        {MODEL_COUNT_403, SNAG_TOKEN_COUNT_AUTO, -1, SNAG_COUNT_UNKNOWN, true},
        {MODEL_COUNT_MODEL_404, SNAG_TOKEN_COUNT_AUTO, -1, SNAG_COUNT_UNKNOWN, false},
        {MODEL_COUNT_OVERFLOW, SNAG_TOKEN_COUNT_AUTO, SNAG_PROVIDER_CONTEXT_OVERFLOW, SNAG_COUNT_UNKNOWN, false},
        {MODEL_COUNT_OK, SNAG_TOKEN_COUNT_AUTO, 0, SNAG_COUNT_SUPPORTED, false},
        {MODEL_COUNT_OK, SNAG_TOKEN_COUNT_STRICT, 0, SNAG_COUNT_SUPPORTED, false}
    };

    assert(!snag_app_exact_count_enabled(SNAG_TOKEN_COUNT_OFF,
                                        SNAG_COUNT_UNKNOWN));
    assert(!snag_app_exact_count_enabled(SNAG_TOKEN_COUNT_AUTO,
                                        SNAG_COUNT_UNSUPPORTED));
    assert(snag_app_exact_count_enabled(SNAG_TOKEN_COUNT_AUTO,
                                       SNAG_COUNT_SUPPORTED));
    assert(snag_app_exact_count_enabled(SNAG_TOKEN_COUNT_STRICT,
                                       SNAG_COUNT_UNSUPPORTED));

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct local_server server;
        struct snag_config config;
        struct snag_credential credential;
        struct app_state app;
        json_t *request = request_with_marker("count-mode");
        const char *method = cases[i].mode == SNAG_TOKEN_COUNT_STRICT ?
            "anchored_upper_bound" : "unknown";
        uint64_t tokens = 99u;
        char error[256] = {0};
        char temp[] = "/tmp/snajpagent-count-mode-XXXXXX";
        int rc;

        assert(mkdtemp(temp));
        start_server(&server, cases[i].fixture, false, "/v1");
        (void)transport_connection(&config, &credential,
            cases[i].openrouter ? "https://openrouter.ai/api/v1" : server.endpoint);
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", server.endpoint, 1) == 0);
        config.providers[0].exact_token_count = cases[i].mode;
        memset(&app, 0, sizeof(app));
        snag_store_init(&app.store);
        assert(snag_store_open(&app.store, temp, error, sizeof(error)) == 0);
        app.model_cache = (struct snag_model_cache){0};
        assert(snag_ui_init(&app.ui) == 0);
        app.config = &config;
        app.turn_provider = &config.providers[0];
        app.turn_model = "gpt-transport-test";
        rc = snag_app_provider_count(&app, request, &credential,
                                    &tokens, &method, error, sizeof(error));
        assert((cases[i].result < 0 && rc < 0) || rc == cases[i].result);
        assert(app.turn_capacity.count_capability == cases[i].capability);
        if (cases[i].fixture == MODEL_COUNT_OK)
            assert(tokens == 42u && !strcmp(method, "exact"));
        else
            assert(tokens == 99u);
        assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
        snag_ui_free(&app.ui);
        snag_model_cache_free(&app.model_cache);
        if (unlinkat(app.store.root_fd, "models.lock", 0) < 0)
            assert(errno == ENOENT);
        snag_store_close(&app.store);
        {
            char path[512];
            assert(snprintf(path, sizeof(path), "%s/sessions", temp) > 0);
            assert(rmdir(path) == 0);
            assert(snprintf(path, sizeof(path), "%s/trash", temp) > 0);
            assert(rmdir(path) == 0);
        }
        assert(rmdir(temp) == 0);
        json_decref(request);
        snag_config_free(&config);
        stop_server(&server);
    }
}

static void
test_openrouter_search_transport(void)
{
    static const char search_tools[] =
        "[{\"type\":\"openrouter:web_search\"},{\"type\":\"function\","
        "\"name\":\"read_file\",\"parameters\":{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\"},\"start_line\":{\"type\":\"integer\"},"
        "\"end_line\":{\"type\":\"integer\"}}}}]";
    static const char local_call[] =
        "{\"type\":\"function_call\",\"call_id\":\"call_after_search\",\"name\":\"read_file\","
        "\"arguments\":\"{\\\"path\\\":\\\"README.md\\\",\\\"start_line\\\":1,\\\"end_line\\\":1}\"}";
    static const char local_output[] =
        "{\"type\":\"function_call_output\",\"call_id\":\"call_after_search\",\"output\":\"snajpagent\"}";
    struct local_server server;
    struct snag_config config;
    struct snag_credential credential;
    struct emitted_text emitted = {0};
    json_t *request;
    char error[256] = {0};
    unsigned int retries = 0u;

    start_server(&server, MODEL_OPENROUTER_SEARCH, false, "");
    assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", server.endpoint, 1) == 0);
    struct snag_provider_connection connection =
        transport_connection(&config, &credential, "https://openrouter.ai/api/v1");
    request = request_with_marker("search example domains");
    assert(json_object_set_new(request, "tools", json_loadb(
        search_tools, sizeof(search_tools) - 1u, 0, NULL)) == 0);
    assert(snag_config_provider_is_openrouter(&config.providers[0]));
    snag_buf_init(&emitted.text, 128u);
    struct snag_response_graph graph = {0};
    assert(snag_provider_responses_create(connection,
        request, emit_capture, &emitted, &graph, NULL, error, sizeof(error), &retries) == 0);
    assert(!retries);
    assert(graph.count == 2u);
    assert(snag_response_graph_item(&graph, 0).kind == SNAG_ITEM_ASSISTANT);
    assert(strcmp(snag_response_graph_item(&graph, 0).text, "Found https://example.com") == 0);
    assert(snag_response_graph_item(&graph, 1).kind == SNAG_ITEM_TOOL_CALL);
    assert(strcmp(snag_response_graph_item(&graph, 1).name, "read_file") == 0);
    assert(strcmp(snag_response_graph_item(&graph, 1).provider_call_id, "call_after_search") == 0);
    assert(strcmp(snag_json_string(snag_response_graph_item(&graph, 1).arguments, "path"), "README.md") == 0);
    assert(emitted.calls == 1u);
    assert(emitted.text.len == strlen("Found https://example.com"));
    assert(memcmp(emitted.text.data, "Found https://example.com", emitted.text.len) == 0);
    /* A hosted item must not become a local call or contaminate the next
     * stateless response. The existing context tests cover replay projection. */
    assert(json_array_append_new(json_object_get(request, "input"), json_loadb(
        local_call, sizeof(local_call) - 1u, 0, NULL)) == 0);
    assert(json_array_append_new(json_object_get(request, "input"), json_loadb(
        local_output, sizeof(local_output) - 1u, 0, NULL)) == 0);
    snag_response_graph_free(&graph);
    snag_buf_reset(&emitted.text);
    emitted.calls = 0u;
    assert(snag_provider_responses_create(connection,
        request, emit_capture, &emitted, &graph, NULL, error, sizeof(error), &retries) == 0);
    assert(graph.count == 1u && snag_response_graph_item(&graph, 0).kind == SNAG_ITEM_ASSISTANT);
    assert(strcmp(snag_response_graph_item(&graph, 0).text, "local transport") == 0);
    assert(emitted.calls == 1u && !retries);
    snag_response_graph_free(&graph);
    snag_buf_free(&emitted.text);
    json_decref(request);
    snag_credential_clear(&credential);
    assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
    stop_server(&server);
}

static void
test_read_only_dispatch(void)
{
    static const char *const denied[] = {
        "exec_command", "write_stdin", "apply_patch", "create_goal",
        "update_goal", "irc_send", "irc_topic", "irc_state", "unknown",
        "web_search", "openrouter:web_search"
    };
    struct app_state app = {0};
    struct snag_response_item call = {0};
    json_t *result = NULL;
    char error[256] = {0};

    app.session.active_read_only = true;
    call.kind = SNAG_ITEM_TOOL_CALL;
    call.arguments = json_object();
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); ++i) {
        call.name = (char *)denied[i];
        /* No config, IRC, credential or process: no handler may be reached. */
        assert(snag_app_tool_run(&app, &call, NULL, &result,
                                error, sizeof(error)) == 0);
        assert(snag_tool_result_valid(result) == 0);
        assert(strstr(snag_json_string(result, "model_text"), "read-only"));
        json_decref(result);
    }
    app.session.active_read_only = false;
    call.name = "read_file";
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "only in /ro"));
    json_decref(result);
    json_decref(call.arguments);
}

static void
test_ui_output_order_and_failure(void)
{
    struct snag_ui ui;
    unsigned char text[1024];
    enum snag_term_action action;
    char *line;
    int pipefd[2], status;
    pid_t reader;

    assert(pipe(pipefd) == 0);
    reader = fork();
    assert(reader >= 0);
    if (!reader) {
        size_t received = 0u;
        (void)close(pipefd[1]);
        while (received < 128u * sizeof(text)) {
            ssize_t got = read(pipefd[0], text, sizeof(text));
            assert(got > 0);
            for (ssize_t i = 0; i < got; ++i)
                assert(text[i] == (received + (size_t)i) / sizeof(text));
            received += (size_t)got;
        }
        (void)close(pipefd[0]);
        _exit(0);
    }
    assert(close(pipefd[0]) == 0);
    assert(snag_ui_init(&ui) == 0);
    for (unsigned int i = 0u; i < 128u; ++i) {
        memset(text, (int)i, sizeof(text));
        assert(snag_ui_send(&ui, (struct snag_ui_command){
            .kind = SNAG_UI_RAW, .data.value = (unsigned int)(pipefd[1]), .text = (char *)text, .len = sizeof(text)}) == 0);
    }
    assert(waitpid(reader, &status, 0) == reader);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* Beginning a public item must not erase an already delivered shutdown. */
    {
        struct snag_buf delivered = {.max = 16u};
        snag_ui_signal(&ui);
        assert(snag_ui_send(&ui, (struct snag_ui_command){
            .kind = SNAG_UI_PUBLIC_BEGIN, .label = NULL, .data.public = {STDOUT_FILENO, SNAG_PRESENT_CONVERSATION}}) == 0);
        assert(snag_ui_public(&ui, "stopped", 7u, &delivered) == 0);
        assert(delivered.len == 0u);
        assert(snag_ui_text(&ui, SNAG_UI_ROLLOUT_END, NULL) == 0);
        assert(snag_ui_poll(&ui, 0, &action, &line) == 1);
        assert(action == SNAG_TERM_EXIT);
        snag_buf_free(&delivered);
    }
    assert(close(pipefd[1]) == 0);
    assert(snag_ui_send(&ui, (struct snag_ui_command){
        .kind = SNAG_UI_RAW, .data.value = (unsigned int)(-1), .text = "x", .len = 1u}) < 0 && errno == EBADF);
    assert(snag_ui_send(&ui, (struct snag_ui_command){
        .kind = SNAG_UI_RAW, .data.value = (unsigned int)(pipefd[1]), .text = "x", .len = 1u}) < 0);
    assert(snag_ui_poll(&ui, 0, &action, &line) < 0);
    snag_ui_free(&ui);
}

static int
cancel_device_poll(void *opaque, uint32_t wait_ms)
{
    (void)opaque;
    return wait_ms ? 2 : 0;
}

static void
test_provider_auth(void)
{
    struct snag_config config;
    struct snag_store store;
    struct snag_auth_tokens tokens, previous, loaded;
    struct snag_credential credential;
    struct local_server server;
    char path[4096], error[256] = {0};
    const char *tmp = getenv("TMPDIR");
    struct stat st;
    int status;

    assert(snprintf(path, sizeof(path), "%s/snajpagent-auth-XXXXXX", tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    snag_config_init(&config);
    snag_secret_source_free(&config.providers[0].api_key);
    strcpy(config.providers[0].name, "default");
    snag_store_init(&store);
    assert(snag_store_open(&store, path, error, sizeof(error)) == 0);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    assert(snag_auth_load(store.root_fd, &config.providers[0], &tokens, error, sizeof(error)) == 1);
    assert(snag_auth_key(&tokens, "stored-key", error, sizeof(error)) == 0);
    assert(snag_auth_save(store.root_fd, &config.providers[0], &tokens, &previous,
                          NULL, NULL, error, sizeof(error)) == 0);
    assert(!previous.credential.len);
    assert(fstatat(store.root_fd, "auth/default.json", &st, AT_SYMLINK_NOFOLLOW) == 0);
    assert((st.st_mode & 0777u) == 0600u);
    assert(snag_auth_read(store.root_fd, &config.providers[0], false, NULL, &credential,
                         NULL, NULL, error, sizeof(error)) == 0);
    assert(strcmp(credential.value, "stored-key") == 0);
    assert(credential.root_fd == store.root_fd);
    assert(snag_secret_source_parse(&config.providers[0].api_key, "${SNAG_MISSING_EXPLICIT_KEY}",
                                    NULL, error, sizeof(error)) == 0);
    assert(unsetenv("SNAG_MISSING_EXPLICIT_KEY") == 0);
    assert(snag_auth_read(store.root_fd, &config.providers[0], false, NULL, &credential,
                         NULL, NULL, error, sizeof(error)) < 0);
    assert(credential.len == 0u);
    assert(snag_secret_source_parse(&config.providers[0].api_key, "\"explicit-key\"",
                                    NULL, error, sizeof(error)) == 0);
    assert(snag_auth_read(store.root_fd, &config.providers[0], false, NULL, &credential,
                         NULL, NULL, error, sizeof(error)) == 0);
    assert(strcmp(credential.value, "explicit-key") == 0);
    snag_secret_source_free(&config.providers[0].api_key);
    error[0] = '\0';
    snag_config_provider_init(&config.providers[1], "other");
    strcpy(config.providers[1].name, "other");
    assert(snag_auth_load(store.root_fd, &config.providers[1], &loaded, error, sizeof(error)) == 1);
    strcpy(config.providers[0].base_url, "https://different.test");
    assert(snag_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) < 0);
    strcpy(config.providers[0].base_url, "https://api.openai.com");
    assert(fchmodat(store.root_fd, "auth/default.json", 0644, 0) == 0);
    assert(snag_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) < 0);
    assert(fchmodat(store.root_fd, "auth/default.json", 0600, 0) == 0);
    assert(snag_auth_key(&tokens, "replacement", error, sizeof(error)) == 0);
    assert(snag_auth_save(store.root_fd, &config.providers[0], &tokens, &previous,
                          NULL, NULL, error, sizeof(error)) == 0);
    assert(snag_auth_restore(store.root_fd, &config.providers[0], &tokens, &previous,
                             error, sizeof(error)) == 0);
    assert(snag_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) == 0);
    assert(strcmp(loaded.credential.value, "stored-key") == 0);
    assert(snag_auth_logout(store.root_fd, &config.providers[0], NULL, NULL, error, sizeof(error)) == 0);
    assert(symlinkat("outside", store.root_fd, "auth/default.json") == 0);
    assert(snag_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) < 0);
    assert(snag_auth_save(store.root_fd, &config.providers[0], &tokens, &previous,
                          NULL, NULL, error, sizeof(error)) < 0);
    assert(unlinkat(store.root_fd, "auth/default.json", 0) == 0);

    config.providers[0].auth = SNAG_AUTH_CHATGPT;
    strcpy(config.providers[0].base_url, SNAG_CHATGPT_BASE);
    {
        json_t *response = json_object();
        snag_auth_clear(&tokens);
        assert(snag_auth_token_response(response, &tokens, error, sizeof(error)) < 0);
        assert(tokens.credential.len == 0u);
        assert(snag_auth_key(&tokens, "old-access", error, sizeof(error)) == 0);
        strcpy(tokens.credential.account_id, "acct-test");
        strcpy(tokens.refresh_token, "old-refresh");
        assert(json_object_set_new(response, "access_token", json_string("rotated-access")) == 0);
        assert(json_object_set_new(response, "expires_in", json_integer(3600)) == 0);
        assert(snag_auth_token_response(response, &tokens, error, sizeof(error)) == 0);
        assert(strcmp(tokens.refresh_token, "old-refresh") == 0);
        assert(strcmp(tokens.credential.account_id, "acct-test") == 0);
        snag_auth_json_free(response);
    }
    for (int mode = MODEL_AUTH_DEVICE; mode <= MODEL_AUTH_EXPIRED; ++mode) {
        start_server(&server, (enum model_fixture)mode, false, "");
        assert(setenv("SNAJPAGENT_TEST_AUTH_BASE", server.endpoint, 1) == 0);
        error[0] = '\0';
        int rc = snag_auth_device(&tokens, mode == MODEL_AUTH_CANCEL ? cancel_device_poll : NULL,
                                  NULL, error, sizeof(error));
        if (mode == MODEL_AUTH_DEVICE) {
            assert(rc == 0);
            assert(strcmp(tokens.credential.value, "new-access") == 0);
            assert(strcmp(tokens.credential.account_id, "acct-test") == 0);
            assert(strcmp(tokens.refresh_token, "new-refresh") == 0);
            assert(tokens.expires_at_ms > snag_time_ms());
        } else {
            assert(rc < 0);
            assert(tokens.credential.len == 0u);
        }
        stop_server(&server);
    }
    for (int mode = MODEL_AUTH_REFRESH; mode <= MODEL_AUTH_401_TWICE; ++mode) {
        assert(snag_auth_key(&tokens, "old-access", error, sizeof(error)) == 0);
        strcpy(tokens.refresh_token, "old-refresh");
        strcpy(tokens.credential.account_id, "acct-test");
        tokens.expires_at_ms = mode >= MODEL_AUTH_401 ? snag_time_ms() + 3600000u : 1u;
        assert(snag_auth_save(store.root_fd, &config.providers[0], &tokens, NULL,
                              NULL, NULL, error, sizeof(error)) == 0);
        start_server(&server, (enum model_fixture)mode, false, "");
        assert(setenv("SNAJPAGENT_TEST_AUTH_BASE", server.endpoint, 1) == 0);
        error[0] = '\0';
        if (mode == MODEL_AUTH_REFRESH) {
            pid_t children[2];
            for (size_t i = 0; i < 2u; ++i) {
                children[i] = fork();
                assert(children[i] >= 0);
                if (children[i] == 0) {
                    int rc = snag_auth_read(store.root_fd, &config.providers[0], false,
                        NULL, &credential, NULL, NULL, error, sizeof(error));
                    _exit(rc == 0 && strcmp(credential.value, "new-access") == 0 ? 0 : 1);
                }
            }
            for (size_t i = 0; i < 2u; ++i) {
                assert(waitpid(children[i], &status, 0) == children[i]);
                assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            }
            assert(snag_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) == 0);
            assert(strcmp(loaded.refresh_token, "new-refresh") == 0);
        } else if (mode == MODEL_AUTH_REFRESH_FAILURE) {
            assert(snag_auth_read(store.root_fd, &config.providers[0], false, NULL,
                &credential, NULL, NULL, error, sizeof(error)) < 0);
            assert(!strstr(error, "private-refresh-server-detail"));
            assert(snag_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) == 0);
            assert(strcmp(loaded.refresh_token, "old-refresh") == 0);
        } else {
            json_t *models = NULL;
            assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", server.endpoint, 1) == 0);
            assert(snag_auth_read(store.root_fd, &config.providers[0], false, NULL,
                &credential, NULL, NULL, error, sizeof(error)) == 0);
            int rc = snag_provider_models_list((struct snag_provider_connection){
                &config, &config.providers[0], &credential, NULL,
                NULL, NULL},
                &models, error, sizeof(error));
            if (rc < 0 && mode == MODEL_AUTH_401)
                (void)fprintf(stderr, "auth fixture failed: %s\n", error);
            assert((rc == 0) == (mode == MODEL_AUTH_401));
            if (rc == 0)
                assert(json_array_size(models) == 1u);
            json_decref(models);
            assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
        }
        stop_server(&server);
    }
    assert(unsetenv("SNAJPAGENT_TEST_AUTH_BASE") == 0);
    for (unsigned int pass = 0u; pass < 3u; ++pass) {
        json_t *request = request_with_marker("transport-compact");
        struct snag_json_document output = {0};
        credential_set(&credential, "transport-secret");
        credential.root_fd = -1;
        config.providers[0].auth = pass == 0u ? SNAG_AUTH_API_KEY : SNAG_AUTH_CHATGPT;
        strcpy(config.providers[0].base_url, pass == 0u ? "https://api.openai.com" : SNAG_CHATGPT_BASE);
        strcpy(config.providers[0].openrouter_referer, "https://github.com/snajpa/snajpagent");
        strcpy(config.providers[0].openrouter_title, "snajpagent");
        start_server(&server, pass == 2u ? MODEL_COMPACT_403 : MODEL_COMPACT_404, false, "");
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", server.endpoint, 1) == 0);
        int rc = snag_provider_responses_compact((struct snag_provider_connection){
            &config, &config.providers[0], &credential, NULL,
            NULL, NULL},
            request, &output, error, sizeof(error), NULL);
        assert(rc == (pass < 2u ? SNAG_PROVIDER_UNSUPPORTED : -1));
        assert(output.value == NULL);
        stop_server(&server);
        json_decref(request);
    }
    assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
    assert(snag_auth_logout(store.root_fd, &config.providers[0], NULL, NULL, error, sizeof(error)) == 0);
    assert(unlinkat(store.root_fd, "auth/default.lock", 0) == 0);
    assert(unlinkat(store.root_fd, "auth", AT_REMOVEDIR) == 0);
    assert(unlinkat(store.root_fd, "sessions", AT_REMOVEDIR) == 0);
    assert(unlinkat(store.root_fd, "trash", AT_REMOVEDIR) == 0);
    snag_store_close(&store);
    assert(rmdir(path) == 0);
    snag_config_free(&config);
    snag_auth_clear(&tokens);
    snag_auth_clear(&previous);
    snag_auth_clear(&loaded);
    snag_credential_clear(&credential);
}

static void
test_irc_failed_intent_retains_pending(void)
{
    struct snag_config config = {0};
    struct app_state app = {0};
    char error[256] = {0};
    snag_session_init(&app.session);
    assert(snag_ui_init(&app.ui) == 0);
    app.config = &config;
    app.irc_urgent.max = 128u;
    assert(snag_buf_append(&app.irc_urgent, "retained", sizeof("retained")) == 0);
    /* No provider means input construction fails before event admission. */
    assert(!snag_app_irc_take_pending(&app, NULL, false));
    assert(app.irc_urgent.len == sizeof("retained"));
    assert(app.input_closed);
    app.input_closed = false;
    app.session.active_turn = true;
    /* Invalid UTF-8 exercises failed steering JSON construction without an
     * allocator hook. Neither input nor steering may consume its projection. */
    app.irc_urgent.data[0] = 0xff;
    assert(snag_app_irc_flush_urgent(&app, error, sizeof(error)) < 0);
    assert(app.irc_urgent.len == sizeof("retained"));
    assert(app.input_closed);
    snag_buf_free(&app.irc_urgent);
    snag_ui_free(&app.ui);
    snag_session_close(&app.session);
}

int
main(void)
{
    test_irc_failed_intent_retains_pending();
    test_provider_auth();
    test_ui_output_order_and_failure();
    test_read_only_dispatch();
    test_local_provider_transport();
    test_openrouter_search_transport();
    test_codex_path_selection();
    test_structured_create_failures();
    test_create_retries();
    test_policy_clarification_after_reasoning();
    test_count_capability_statuses();
    test_count_modes();
    puts("test_provider_transport: ok");
    return 0;
}
