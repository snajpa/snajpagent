/* SPDX-License-Identifier: GPL-2.0-only */
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
};

struct http_request {
    char method[8];
    char path[128];
    char headers[REQUEST_MAX];
    char body[BODY_MAX];
    size_t body_len;
};

struct emitted_text {
    struct snj_buf text;
    unsigned int calls;
};

enum model_fixture {
    MODEL_OPENAI,
    MODEL_CODEX,
    MODEL_CODEX_MALFORMED,
    MODEL_CODEX_LOOKALIKE,
    MODEL_CODEX_FAILURE,
    MODEL_LIMIT_CONFLICT,
    MODEL_CREATE_HTTP_FAILURE,
    MODEL_CREATE_SSE_FAILURE,
    MODEL_OPENROUTER_SEARCH,
    MODEL_COUNT_404,
    MODEL_COUNT_405,
    MODEL_COUNT_501,
    MODEL_COUNT_401,
    MODEL_COUNT_403,
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
send_status(int fd, unsigned int status, const char *body)
{
    char header[256];
    size_t len = strlen(body);
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %u Test\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %llu\r\n"
                     "Connection: close\r\n\r\n",
                     status, (unsigned long long)len);
    if (n <= 0 || (size_t)n >= sizeof(header))
        server_fail("status response header build failed");
    write_all_or_die(fd, header, (size_t)n);
    write_all_or_die(fd, body, len);
}

static void
serve_one(int listen_fd, const char *method, const char *path,
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
    send_response(fd, content_type, body);
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
        send_status(fd, status, body);
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
        send_status(fd, models == MODEL_COMPACT_404 ? 404u : 403u, "{\"detail\":\"Not Found\"}");
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
        serve_one(listen_fd, "POST", "/v1/responses", "openrouter:web_search",
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
        serve_one(listen_fd, "POST", "/v1/responses", "function_call_output",
                  "text/event-stream", create_sse);
        _exit(0);
    }
    if (models >= MODEL_COUNT_404) {
        struct http_request request;
        unsigned int status = models == MODEL_COUNT_404 ? 404u :
                              models == MODEL_COUNT_405 ? 405u :
                              models == MODEL_COUNT_401 ? 401u :
                              models == MODEL_COUNT_403 ? 403u : 501u;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0)
            server_fail("accept failed");
        read_request(fd, &request);
        if (strcmp(request.method, "POST") != 0 ||
            strcmp(request.path, "/v1/responses/input_tokens") != 0)
            server_fail("unexpected failed count request");
        send_status(fd, status, "{\"error\":{\"message\":\"not available\"}}");
        if (close(fd) < 0)
            server_fail("close failed count socket");
        _exit(0);
    }
    if (models == MODEL_CREATE_HTTP_FAILURE) {
        struct http_request request;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0)
            server_fail("accept failed");
        read_request(fd, &request);
        if (strcmp(request.method, "POST") != 0 ||
            strcmp(request.path, "/v1/responses") != 0)
            server_fail("unexpected failed create request");
        send_status(fd, 400u,
            "{\"error\":{\"code\":\"context_length_exceeded\","
            "\"message\":\"too large\",\"max_context_tokens\":272000,"
            "\"requested_input_tokens\":300000}}");
        if (close(fd) < 0)
            server_fail("close failed create socket");
        _exit(0);
    }
    if (models == MODEL_CREATE_SSE_FAILURE) {
        serve_one(listen_fd, "POST", "/v1/responses", NULL,
                  "text/event-stream",
                  "event: response.failed\n"
                  "data: {\"type\":\"response.failed\",\"response\":{"
                  "\"error\":{\"code\":\"context_length_exceeded\","
                  "\"message\":\"stream too large\","
                  "\"context_length\":872000}}}\n\n");
        _exit(0);
    }

    if (models == MODEL_CODEX_FAILURE) {
        struct http_request request;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0)
            server_fail("accept failed");
        read_request(fd, &request);
        if (strcmp(request.method, "GET") != 0 ||
            strcmp(request.path,
                   "/backend-api/codex/models?client_version=0.146.0") != 0)
            server_fail("unexpected failed Codex catalog request");
        send_status(fd, 400u, "{\"error\":{\"message\":\"catalog rejected\"}}");
        if (close(fd) < 0)
            server_fail("close failed Codex request socket");
        _exit(0);
    }
    if (models == MODEL_CODEX_MALFORMED) {
        serve_one(listen_fd, "GET",
                  "/backend-api/codex/models?client_version=0.146.0", NULL,
                  "application/json",
                  "{\"models\":[{\"slug\":\"malformed\",\"visibility\":\"list\",\"priority\":1,\"supported_reasoning_levels\":[\"high\"]}]}");
    } else if (models == MODEL_CODEX) {
        serve_one(listen_fd, "GET",
                  "/backend-api/codex/models?client_version=0.146.0", NULL,
                  "application/json",
                  "{\"models\":["
                  "{\"slug\":\"hidden-first\",\"visibility\":\"hide\",\"priority\":0},"
                  "{\"slug\":\"vendor/native-model\",\"visibility\":\"list\",\"priority\":20,\"context_window\":272000,\"max_context_window\":872000,\"auto_compact_token_limit\":null,\"supported_reasoning_levels\":[{\"effort\":\"low\"},{\"effort\":\"ultra\"},{\"effort\":\"low\"}],\"default_reasoning_level\":\"low\"},"
                  "{\"slug\":\"codex-fast\",\"visibility\":\"list\",\"priority\":5,\"supported_reasoning_levels\":[{\"effort\":\"medium\"}],\"default_reasoning_level\":\"medium\"},"
                  "{\"slug\":\"codex-tied\",\"visibility\":\"list\",\"priority\":5,\"supported_reasoning_levels\":[{\"effort\":\"high\"}],\"default_reasoning_level\":\"high\"},"
                  "{\"slug\":\"missing-visibility\",\"priority\":1},"
                  "{\"slug\":\"none\",\"visibility\":\"none\",\"priority\":1},"
                  "{\"slug\":\"unknown\",\"visibility\":\"future\",\"priority\":1}]}");
    } else if (models == MODEL_CODEX_LOOKALIKE) {
        serve_one(listen_fd, "GET", "/backend-api/codexish/v1/models", NULL,
                  "application/json",
                  "{\"data\":[{\"id\":\"lookalike-openai\"}]}");
    } else if (models == MODEL_LIMIT_CONFLICT) {
        serve_one(listen_fd, "GET", "/v1/models", NULL,
                  "application/json",
                  "{\"data\":[{\"id\":\"conflict\",\"context_length\":100,"
                  "\"metadata\":{\"contextWindow\":101}}]}");
    } else {
        serve_one(listen_fd, "GET", "/v1/models", NULL,
                  "application/json",
                  "{\"object\":\"list\",\"data\":[{\"id\":\"gpt-standard\","
                  "\"contextLength\":100000,\"metadata\":{\"context_window\":100000,"
                  "\"inputContextWindow\":90000,\"supported_reasoning_levels\":[\"medium\",\"high\"],"
                  "\"default_reasoning_level\":\"medium\"},\"capabilities\":{"
                  "\"maxOutputTokens\":10000,\"effective_context_window_percent\":80}},"
                  "{\"id\":\"future-standard\",\"supported_reasoning_levels\":[\"quantum\",\"cosmic\"]}]}");
    }
    if (!transport)
        _exit(0);
    serve_one(listen_fd, "POST", "/v1/responses/input_tokens", "transport-count",
              "application/json",
              "{\"object\":\"response.input_tokens\",\"input_tokens\":7}");
    serve_one(listen_fd, "POST", "/v1/responses", "transport-create",
              "text/event-stream", create_sse);
    serve_one(listen_fd, "POST", "/v1/responses/compact", "transport-compact",
              "application/json",
              "{\"object\":\"response.compaction\",\"output\":[{\"type\":\"compaction\",\"encrypted_content\":\"transport-compact-output\"}]}");
    _exit(0);
}

static void
start_server(struct local_server *server, enum model_fixture models,
             bool transport)
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
    server->pid = fork();
    assert(server->pid >= 0);
    if (server->pid == 0)
        server_child(server->fd, models, transport);
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

static int
capture_stderr_begin(int pipefd[2])
{
    int saved;

    assert(pipe(pipefd) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0);
    assert(dup2(pipefd[1], STDERR_FILENO) == STDERR_FILENO);
    assert(close(pipefd[1]) == 0);
    return saved;
}

static void
capture_stderr_end(int pipefd[2], int saved, char *out, size_t out_size)
{
    size_t used = 0u;

    assert(dup2(saved, STDERR_FILENO) == STDERR_FILENO);
    assert(close(saved) == 0);
    while (used + 1u < out_size) {
        ssize_t got = read(pipefd[0], out + used, out_size - used - 1u);
        if (got < 0) {
            assert(errno == EINTR);
            continue;
        }
        if (got == 0)
            break;
        used += (size_t)got;
    }
    out[used] = '\0';
    assert(close(pipefd[0]) == 0);
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
    json_t *models = NULL;
    uint64_t tokens = 0u;
    uint64_t compact_bytes = 0u;
    unsigned int retries = 99u;
    int cancel = 99;
    char endpoint[128];
    char error[256] = {0};

    start_server(&server, MODEL_OPENAI, true);
    assert(snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u",
                    (unsigned int)server.port) > 0);
    snj_config_init(&config);
    config.providers[1] = config.providers[0];
    config.provider_count = 2u;
    assert(snprintf(config.providers[1].name,
                    sizeof(config.providers[1].name), "transport") > 0);
    config.providers[1].connect_timeout_ms = 1000u;
    config.providers[1].idle_timeout_ms = 1000u;
    config.providers[1].request_timeout_ms = 3000u;
    assert(snprintf(config.providers[1].base_url,
                    sizeof(config.providers[1].base_url),
                    "%s/v1/", endpoint) > 0);
    assert(snprintf(config.providers[1].openrouter_referer,
                    sizeof(config.providers[1].openrouter_referer),
                    "%s", "https://github.com/snajpa/snajpagent") > 0);
    assert(snprintf(config.providers[1].openrouter_title,
                    sizeof(config.providers[1].openrouter_title),
                    "%s", "snajpagent") > 0);
    credential_set(&credential, "transport-secret");

    assert(snj_provider_models_list(&config, &config.providers[1],
                                    &credential, NULL, NULL, NULL, &models,
                                    error, sizeof(error)) == 0);
    assert(json_array_size(models) == 2u);
    assert(strcmp(snj_json_string(json_array_get(models, 0), "id"),
                  "gpt-standard") == 0);
    assert(strcmp(json_string_value(json_array_get(json_object_get(
                      json_array_get(models, 0), "efforts"), 1)),
                  "high") == 0);
    assert(strcmp(snj_json_string(json_array_get(models, 0),
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
    assert(strcmp(snj_model_cache_best_effort(json_array_get(models, 1),
                                              "fallback"),
                  "quantum") == 0);
    json_decref(models);
    models = NULL;

    request = request_with_marker("transport-count");
    assert(snj_provider_responses_count(request, &config, &config.providers[1],
                                        &credential, NULL,
                                        NULL, NULL, &tokens, NULL, error,
                                        sizeof(error), &cancel, &retries) == 0);
    assert(tokens == 7u);
    assert(cancel == 0);
    assert(retries == 0u);
    json_decref(request);

    request = request_with_marker("transport-create");
    snj_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snj_buf_init(&emitted.text, 128u);
    assert(snj_provider_responses_create(request, &config,
                                         &config.providers[1], &credential, NULL,
                                         emit_capture, &emitted, NULL, NULL,
                                         &graph, NULL, error, sizeof(error), &cancel,
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
    assert(snj_provider_responses_compact(request, &config,
                                          &config.providers[1], &credential, NULL,
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

static void
test_codex_model_list(void)
{
    struct local_server server;
    struct snj_config config;
    struct snj_credential credential;
    struct snj_ui render;
    json_t *models = NULL;
    json_t *model;
    json_t *efforts;
    char endpoint[128];
    char diagnostic[8192];
    char error[256] = {0};
    int pipefd[2];
    int saved_stderr;

    start_server(&server, MODEL_CODEX, false);
    assert(snprintf(endpoint, sizeof(endpoint),
                    "http://127.0.0.1:%u/backend-api/codex/",
                    (unsigned int)server.port) > 0);
    snj_config_init(&config);
    assert(snprintf(config.providers[0].base_url,
                    sizeof(config.providers[0].base_url), "%s", endpoint) > 0);
    assert(snprintf(config.providers[0].openrouter_referer,
                    sizeof(config.providers[0].openrouter_referer), "%s",
                    "https://github.com/snajpa/snajpagent") > 0);
    assert(snprintf(config.providers[0].openrouter_title,
                    sizeof(config.providers[0].openrouter_title), "%s",
                    "snajpagent") > 0);
    config.providers[0].connect_timeout_ms = 1000u;
    config.providers[0].idle_timeout_ms = 1000u;
    config.providers[0].request_timeout_ms = 3000u;
    credential_set(&credential, "transport-secret");
    assert(snj_ui_init(&render) == 0);
    render.verbosity = 6u;
    snj_ui_color(&render, SNJ_COLOR_NEVER);
    saved_stderr = capture_stderr_begin(pipefd);
    assert(snj_provider_models_list(&config, &config.providers[0],
                                    &credential, &render, NULL, NULL, &models,
                                    error, sizeof(error)) == 0);
    snj_ui_free(&render);
    capture_stderr_end(pipefd, saved_stderr, diagnostic, sizeof(diagnostic));
    assert(strstr(diagnostic,
                  "> GET /backend-api/codex/models?client_version=0.146.0 HTTP/1.1") != NULL);
    assert(strstr(diagnostic, "> authorization:") != NULL);
    assert(strstr(diagnostic, "<redacted:bearer>") != NULL);
    assert(strstr(diagnostic, "transport-secret") == NULL);
    assert(json_array_size(models) == 3u);
    model = json_array_get(models, 0);
    assert(strcmp(snj_json_string(model, "id"), "codex-fast") == 0);
    assert(strcmp(snj_json_string(model, "default_effort"), "medium") == 0);
    model = json_array_get(models, 1);
    assert(strcmp(snj_json_string(model, "id"), "codex-tied") == 0);
    model = json_array_get(models, 2);
    efforts = json_object_get(model, "efforts");
    assert(strcmp(snj_json_string(model, "id"), "vendor/native-model") == 0);
    assert(json_array_size(efforts) == 2u);
    assert(strcmp(json_string_value(json_array_get(efforts, 0)), "low") == 0);
    assert(strcmp(json_string_value(json_array_get(efforts, 1)), "ultra") == 0);
    assert(strcmp(snj_json_string(model, "default_effort"), "low") == 0);
    {
        json_t *limits = json_object_get(model, "limits");
        assert(json_integer_value(json_object_get(
                   limits, "context_window_tokens")) == 272000);
        assert(json_integer_value(json_object_get(
                   limits, "max_context_window_tokens")) == 872000);
        assert(json_is_null(json_object_get(limits, "max_output_tokens")));
        assert(json_is_null(json_object_get(
                   limits, "effective_context_window_percent")));
    }
    json_decref(models);
    snj_config_free(&config);
    stop_server(&server);
}

static void
test_codex_path_selection(void)
{
    struct local_server server;
    struct snj_config config;
    struct snj_credential credential;
    json_t *models = NULL;
    char endpoint[128];
    char error[256] = {0};

    snj_config_init(&config);
    credential_set(&credential, "transport-secret");
    assert(snprintf(config.providers[0].openrouter_referer,
                    sizeof(config.providers[0].openrouter_referer), "%s",
                    "https://github.com/snajpa/snajpagent") > 0);
    assert(snprintf(config.providers[0].openrouter_title,
                    sizeof(config.providers[0].openrouter_title), "%s",
                    "snajpagent") > 0);
    config.providers[0].connect_timeout_ms = 1000u;
    config.providers[0].idle_timeout_ms = 1000u;
    config.providers[0].request_timeout_ms = 3000u;
    start_server(&server, MODEL_CODEX_LOOKALIKE, false);
    assert(snprintf(endpoint, sizeof(endpoint),
                    "http://127.0.0.1:%u/backend-api/codexish",
                    (unsigned int)server.port) > 0);
    assert(snprintf(config.providers[0].name,
                    sizeof(config.providers[0].name), "codex") > 0);
    assert(snprintf(config.providers[0].base_url,
                    sizeof(config.providers[0].base_url), "%s", endpoint) > 0);
    assert(snj_provider_models_list(&config, &config.providers[0],
                                    &credential, NULL, NULL, NULL, &models,
                                    error, sizeof(error)) == 0);
    assert(json_array_size(models) == 1u);
    assert(strcmp(snj_json_string(json_array_get(models, 0), "id"),
                  "lookalike-openai") == 0);
    json_decref(models);
    stop_server(&server);

    models = NULL;
    start_server(&server, MODEL_OPENAI, false);
    assert(snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u",
                    (unsigned int)server.port) > 0);
    assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", endpoint, 1) == 0);
    assert(snprintf(config.providers[0].base_url,
                    sizeof(config.providers[0].base_url), "%s",
                    "http://backend-api/codex") > 0);
    assert(snj_provider_models_list(&config, &config.providers[0],
                                    &credential, NULL, NULL, NULL, &models,
                                    error, sizeof(error)) == 0);
    assert(json_array_size(models) == 2u);
    json_decref(models);
    assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
    stop_server(&server);

    models = NULL;
    start_server(&server, MODEL_CODEX_MALFORMED, false);
    assert(snprintf(endpoint, sizeof(endpoint),
                    "http://127.0.0.1:%u/backend-api/codex",
                    (unsigned int)server.port) > 0);
    assert(snprintf(config.providers[0].base_url,
                    sizeof(config.providers[0].base_url), "%s", endpoint) > 0);
    assert(snj_provider_models_list(&config, &config.providers[0],
                                    &credential, NULL, NULL, NULL, &models,
                                    error, sizeof(error)) < 0);
    assert(models == NULL);
    assert(strstr(error, "invalid model entry") != NULL);
    stop_server(&server);

    models = NULL;
    start_server(&server, MODEL_CODEX_FAILURE, false);
    assert(snprintf(endpoint, sizeof(endpoint),
                    "http://127.0.0.1:%u/backend-api/codex",
                    (unsigned int)server.port) > 0);
    assert(snprintf(config.providers[0].name,
                    sizeof(config.providers[0].name), "neutral") > 0);
    assert(snprintf(config.providers[0].base_url,
                    sizeof(config.providers[0].base_url), "%s", endpoint) > 0);
    assert(snj_provider_models_list(&config, &config.providers[0],
                                    &credential, NULL, NULL, NULL, &models,
                                    error, sizeof(error)) < 0);
    assert(models == NULL);
    assert(strstr(error, "catalog rejected") != NULL);
    stop_server(&server);

    models = NULL;
    start_server(&server, MODEL_LIMIT_CONFLICT, false);
    assert(snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u",
                    (unsigned int)server.port) > 0);
    assert(snprintf(config.providers[0].base_url,
                    sizeof(config.providers[0].base_url), "%s", endpoint) > 0);
    assert(snj_provider_models_list(&config, &config.providers[0],
                                    &credential, NULL, NULL, NULL, &models,
                                    error, sizeof(error)) < 0);
    assert(models == NULL);
    assert(strstr(error, "invalid model entry") != NULL);
    stop_server(&server);
    snj_config_free(&config);
}

static void
test_structured_create_failures(void)
{
    const enum model_fixture fixtures[] = {
        MODEL_CREATE_HTTP_FAILURE, MODEL_CREATE_SSE_FAILURE
    };

    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        struct local_server server;
        struct snj_config config;
        struct snj_credential credential;
        struct snj_response_graph graph;
        struct snj_provider_failure failure;
        json_t *request = request_with_marker("capacity-failure");
        char endpoint[128];
        char error[256] = {0};
        int cancel = 0;

        start_server(&server, fixtures[i], false);
        assert(snprintf(endpoint, sizeof(endpoint),
                        "http://127.0.0.1:%u/v1",
                        (unsigned int)server.port) > 0);
        snj_config_init(&config);
        assert(snprintf(config.providers[0].base_url,
                        sizeof(config.providers[0].base_url),
                        "%s", endpoint) > 0);
        config.providers[0].connect_timeout_ms = 1000u;
        config.providers[0].idle_timeout_ms = 1000u;
        config.providers[0].request_timeout_ms = 3000u;
        assert(snprintf(config.providers[0].openrouter_referer,
                        sizeof(config.providers[0].openrouter_referer),
                        "%s", "https://github.com/snajpa/snajpagent") > 0);
        assert(snprintf(config.providers[0].openrouter_title,
                        sizeof(config.providers[0].openrouter_title),
                        "%s", "snajpagent") > 0);
        credential_set(&credential, "transport-secret");
        snj_response_graph_init(&graph);
        memset(&failure, 0, sizeof(failure));
        assert(snj_provider_responses_create(request, &config,
                   &config.providers[0], &credential, NULL,
                   NULL, NULL, NULL, NULL, &graph, &failure,
                   error, sizeof(error), &cancel, NULL) < 0);
        assert(snj_provider_failure_is_capacity(&failure));
        assert(failure.context_limit_known);
        assert(failure.context_limit_tokens ==
               (fixtures[i] == MODEL_CREATE_HTTP_FAILURE ?
                    272000u : 872000u));
        if (fixtures[i] == MODEL_CREATE_HTTP_FAILURE) {
            assert(failure.requested_input_known);
            assert(failure.requested_input_tokens == 300000u);
        }
        snj_response_graph_free(&graph);
        json_decref(request);
        snj_config_free(&config);
        stop_server(&server);
    }
}

static void
test_count_capability_statuses(void)
{
    const enum model_fixture fixtures[] = {
        MODEL_COUNT_404, MODEL_COUNT_405, MODEL_COUNT_501
    };
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        struct local_server server;
        struct snj_config config;
        struct snj_credential credential;
        json_t *request = request_with_marker("count-status");
        uint64_t tokens = 0u;
        bool endpoint_unsupported = true;
        char endpoint[128];
        char error[256] = {0};

        start_server(&server, fixtures[i], false);
        assert(snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u/v1",
                        (unsigned int)server.port) > 0);
        snj_config_init(&config);
        assert(snprintf(config.providers[0].base_url,
                        sizeof(config.providers[0].base_url), "%s", endpoint) > 0);
        config.providers[0].connect_timeout_ms = 1000u;
        config.providers[0].idle_timeout_ms = 1000u;
        config.providers[0].request_timeout_ms = 3000u;
        assert(snprintf(config.providers[0].openrouter_referer,
                        sizeof(config.providers[0].openrouter_referer), "%s",
                        "https://github.com/snajpa/snajpagent") > 0);
        assert(snprintf(config.providers[0].openrouter_title,
                        sizeof(config.providers[0].openrouter_title), "%s",
                        "snajpagent") > 0);
        credential_set(&credential, "transport-secret");
        assert(snj_provider_responses_count(request, &config,
                   &config.providers[0], &credential, NULL, NULL, NULL,
                   &tokens, &endpoint_unsupported,
                   error, sizeof(error), NULL, NULL) < 0);
        assert(endpoint_unsupported == (fixtures[i] != MODEL_COUNT_404));
        json_decref(request);
        snj_config_free(&config);
        stop_server(&server);
    }
}

static void
test_count_modes(void)
{
    const struct {
        enum model_fixture fixture;
        enum snj_token_count_mode mode;
        int result;
        enum snj_count_capability capability;
        bool openrouter;
    } cases[] = {
        {MODEL_COUNT_405, SNJ_TOKEN_COUNT_AUTO,
         SNJ_APP_COUNT_SKIPPED, SNJ_COUNT_UNSUPPORTED, false},
        {MODEL_COUNT_405, SNJ_TOKEN_COUNT_STRICT, -1, SNJ_COUNT_UNSUPPORTED, false},
        {MODEL_COUNT_404, SNJ_TOKEN_COUNT_AUTO, -1, SNJ_COUNT_UNKNOWN, false},
        {MODEL_COUNT_404, SNJ_TOKEN_COUNT_AUTO,
         SNJ_APP_COUNT_SKIPPED, SNJ_COUNT_UNSUPPORTED, true},
        {MODEL_COUNT_404, SNJ_TOKEN_COUNT_STRICT, -1, SNJ_COUNT_UNSUPPORTED, true},
        {MODEL_COUNT_401, SNJ_TOKEN_COUNT_AUTO, -1, SNJ_COUNT_UNKNOWN, true},
        {MODEL_COUNT_403, SNJ_TOKEN_COUNT_AUTO, -1, SNJ_COUNT_UNKNOWN, true}
    };

    assert(!snj_app_exact_count_enabled(SNJ_TOKEN_COUNT_OFF,
                                        SNJ_COUNT_UNKNOWN));
    assert(!snj_app_exact_count_enabled(SNJ_TOKEN_COUNT_AUTO,
                                        SNJ_COUNT_UNSUPPORTED));
    assert(snj_app_exact_count_enabled(SNJ_TOKEN_COUNT_AUTO,
                                       SNJ_COUNT_SUPPORTED));
    assert(snj_app_exact_count_enabled(SNJ_TOKEN_COUNT_STRICT,
                                       SNJ_COUNT_UNSUPPORTED));

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct local_server server;
        struct snj_config config;
        struct snj_credential credential;
        struct app_state app;
        json_t *request = request_with_marker("count-mode");
        const char *method = "qualified_upper_bound";
        uint64_t tokens = 99u;
        char endpoint[128];
        char error[256] = {0};
        char temp[] = "/tmp/snajpagent-count-mode-XXXXXX";
        int rc;

        assert(mkdtemp(temp));
        start_server(&server, cases[i].fixture, false);
        assert(snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u/v1",
                        (unsigned int)server.port) > 0);
        snj_config_init(&config);
        assert(snprintf(config.providers[0].base_url,
                        sizeof(config.providers[0].base_url), "%s",
                        cases[i].openrouter ? "https://openrouter.ai/api/v1" : endpoint) > 0);
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", endpoint, 1) == 0);
        assert(snprintf(config.providers[0].openrouter_referer,
                        sizeof(config.providers[0].openrouter_referer), "%s",
                        "https://github.com/snajpa/snajpagent") > 0);
        assert(snprintf(config.providers[0].openrouter_title,
                        sizeof(config.providers[0].openrouter_title), "%s",
                        "snajpagent") > 0);
        config.providers[0].connect_timeout_ms = 1000u;
        config.providers[0].idle_timeout_ms = 1000u;
        config.providers[0].request_timeout_ms = 3000u;
        config.providers[0].exact_token_count = cases[i].mode;
        credential_set(&credential, "transport-secret");
        memset(&app, 0, sizeof(app));
        snj_store_init(&app.store);
        assert(snj_store_open(&app.store, temp, error, sizeof(error)) == 0);
        snj_model_cache_init(&app.model_cache);
        assert(snj_ui_init(&app.ui) == 0);
        app.config = &config;
        app.turn_provider = &config.providers[0];
        app.turn_model = "gpt-transport-test";
        rc = snj_app_provider_count(&app, request, &credential, 100u,
                                    &tokens, &method, error, sizeof(error));
        assert((cases[i].result < 0 && rc < 0) || rc == cases[i].result);
        assert(app.turn_capacity.count_capability == cases[i].capability);
        assert(tokens == 99u && strcmp(method, "qualified_upper_bound") == 0);
        assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
        snj_ui_free(&app.ui);
        snj_model_cache_free(&app.model_cache);
        if (unlinkat(app.store.root_fd, "models.lock", 0) < 0)
            assert(errno == ENOENT);
        snj_store_close(&app.store);
        {
            char path[512];
            assert(snprintf(path, sizeof(path), "%s/sessions", temp) > 0);
            assert(rmdir(path) == 0);
            assert(snprintf(path, sizeof(path), "%s/trash", temp) > 0);
            assert(rmdir(path) == 0);
        }
        assert(rmdir(temp) == 0);
        json_decref(request);
        snj_config_free(&config);
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
    struct snj_config config;
    struct snj_credential credential;
    struct snj_response_graph graph;
    struct emitted_text emitted = {0};
    json_t *request;
    char endpoint[128], error[256] = {0};
    int cancel = 0;
    unsigned int retries = 0u;

    start_server(&server, MODEL_OPENROUTER_SEARCH, false);
    assert(snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u",
                     (unsigned int)server.port) > 0);
    assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", endpoint, 1) == 0);
    snj_config_init(&config);
    (void)snprintf(config.providers[0].base_url, sizeof(config.providers[0].base_url),
                   "https://openrouter.ai/api/v1");
    (void)snprintf(config.providers[0].openrouter_referer,
                   sizeof(config.providers[0].openrouter_referer),
                   "https://github.com/snajpa/snajpagent");
    (void)snprintf(config.providers[0].openrouter_title,
                   sizeof(config.providers[0].openrouter_title), "snajpagent");
    config.providers[0].connect_timeout_ms = 1000u;
    config.providers[0].idle_timeout_ms = 1000u;
    config.providers[0].request_timeout_ms = 3000u;
    credential_set(&credential, "transport-secret");
    request = request_with_marker("search example domains");
    assert(json_object_set_new(request, "tools", json_loadb(
        search_tools, sizeof(search_tools) - 1u, 0, NULL)) == 0);
    assert(snj_config_provider_is_openrouter(&config.providers[0]));
    snj_buf_init(&emitted.text, 128u);
    snj_response_graph_init(&graph);
    assert(snj_provider_responses_create(request, &config, &config.providers[0],
        &credential, NULL, emit_capture, &emitted, NULL, NULL, &graph, NULL,
        error, sizeof(error), &cancel, &retries) == 0);
    assert(!cancel && !retries);
    assert(graph.count == 2u);
    assert(graph.items[0].kind == SNJ_ITEM_ASSISTANT);
    assert(strcmp(graph.items[0].text, "Found https://example.com") == 0);
    assert(graph.items[1].kind == SNJ_ITEM_TOOL_CALL);
    assert(strcmp(graph.items[1].name, "read_file") == 0);
    assert(strcmp(graph.items[1].provider_call_id, "call_after_search") == 0);
    assert(strcmp(snj_json_string(graph.items[1].arguments, "path"), "README.md") == 0);
    assert(emitted.calls == 1u);
    assert(emitted.text.len == strlen("Found https://example.com"));
    assert(memcmp(emitted.text.data, "Found https://example.com", emitted.text.len) == 0);
    /* A hosted item must not become a local call or contaminate the next
     * stateless response. The existing context tests cover replay projection. */
    assert(json_array_append_new(json_object_get(request, "input"), json_loadb(
        local_call, sizeof(local_call) - 1u, 0, NULL)) == 0);
    assert(json_array_append_new(json_object_get(request, "input"), json_loadb(
        local_output, sizeof(local_output) - 1u, 0, NULL)) == 0);
    snj_response_graph_free(&graph);
    snj_response_graph_init(&graph);
    snj_buf_reset(&emitted.text);
    emitted.calls = 0u;
    assert(snj_provider_responses_create(request, &config, &config.providers[0],
        &credential, NULL, emit_capture, &emitted, NULL, NULL, &graph, NULL,
        error, sizeof(error), &cancel, &retries) == 0);
    assert(graph.count == 1u && graph.items[0].kind == SNJ_ITEM_ASSISTANT);
    assert(strcmp(graph.items[0].text, "local transport") == 0);
    assert(emitted.calls == 1u && !cancel && !retries);
    snj_response_graph_free(&graph);
    snj_buf_free(&emitted.text);
    json_decref(request);
    snj_credential_clear(&credential);
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
    struct snj_response_item call = {0};
    json_t *result = NULL;
    char error[256] = {0};

    app.session.active_read_only = true;
    call.kind = SNJ_ITEM_TOOL_CALL;
    call.arguments = json_object();
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); ++i) {
        call.name = (char *)denied[i];
        /* No config, IRC, credential or process: no handler may be reached. */
        assert(snj_app_tool_run(&app, &call, NULL, &result,
                                error, sizeof(error)) == 0);
        assert(snj_tool_result_valid(result) == 0);
        assert(strstr(snj_json_string(result, "model_text"), "read-only"));
        json_decref(result);
    }
    app.session.active_read_only = false;
    call.name = "read_file";
    assert(snj_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snj_json_string(result, "model_text"), "only in /ro"));
    json_decref(result);
    json_decref(call.arguments);
}

static void
test_ui_output_order_and_failure(void)
{
    struct snj_ui ui;
    unsigned char text[1024];
    enum snj_term_action action;
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
    assert(snj_ui_init(&ui) == 0);
    for (unsigned int i = 0u; i < 128u; ++i) {
        memset(text, (int)i, sizeof(text));
        assert(snj_ui_raw(&ui, pipefd[1], (char *)text, sizeof(text)) == 0);
    }
    assert(waitpid(reader, &status, 0) == reader);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* Beginning a public item must not erase an already delivered shutdown. */
    {
        struct snj_buf delivered;
        snj_buf_init(&delivered, 16u);
        snj_ui_signal(&ui);
        assert(snj_ui_public_begin(&ui, STDOUT_FILENO, NULL, false) == 0);
        assert(snj_ui_public(&ui, "stopped", 7u, &delivered, false) == 0);
        assert(delivered.len == 0u);
        assert(snj_ui_text(&ui, SNJ_UI_PUBLIC_END, NULL) == 0);
        assert(snj_ui_poll(&ui, 0, false, &action, &line) == 1);
        assert(action == SNJ_TERM_EXIT);
        snj_buf_free(&delivered);
    }
    assert(close(pipefd[1]) == 0);
    assert(snj_ui_raw(&ui, -1, "x", 1u) < 0 && errno == EBADF);
    assert(snj_ui_raw(&ui, pipefd[1], "x", 1u) < 0);
    assert(snj_ui_poll(&ui, 0, false, &action, &line) < 0);
    snj_ui_free(&ui);
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
    struct snj_config config;
    struct snj_store store;
    struct snj_auth_tokens tokens, previous, loaded;
    struct snj_credential credential;
    struct local_server server;
    char path[4096], endpoint[128], error[256] = {0};
    const char *tmp = getenv("TMPDIR");
    struct stat st;
    int status;

    assert(snprintf(path, sizeof(path), "%s/snajpagent-auth-XXXXXX", tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    snj_config_init(&config);
    snj_store_init(&store);
    assert(snj_store_open(&store, path, error, sizeof(error)) == 0);
    config.providers[0].auth = SNJ_AUTH_API_KEY;
    assert(snj_auth_load(store.root_fd, &config.providers[0], &tokens, error, sizeof(error)) == 1);
    assert(snj_auth_key(&tokens, "stored-key", error, sizeof(error)) == 0);
    assert(snj_auth_save(store.root_fd, &config.providers[0], &tokens, &previous,
                          NULL, NULL, error, sizeof(error)) == 0);
    assert(!previous.credential.len);
    assert(fstatat(store.root_fd, "auth/default.json", &st, AT_SYMLINK_NOFOLLOW) == 0);
    assert((st.st_mode & 0777u) == 0600u);
    assert(snj_auth_read(store.root_fd, &config.providers[0], false, NULL, &credential,
                         NULL, NULL, error, sizeof(error)) == 0);
    assert(strcmp(credential.value, "stored-key") == 0);
    assert(credential.root_fd == store.root_fd);
    config.providers[1] = config.providers[0];
    strcpy(config.providers[1].name, "other");
    assert(snj_auth_load(store.root_fd, &config.providers[1], &loaded, error, sizeof(error)) == 1);
    strcpy(config.providers[0].base_url, "https://different.test");
    assert(snj_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) < 0);
    strcpy(config.providers[0].base_url, "https://api.openai.com");
    assert(fchmodat(store.root_fd, "auth/default.json", 0644, 0) == 0);
    assert(snj_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) < 0);
    assert(fchmodat(store.root_fd, "auth/default.json", 0600, 0) == 0);
    assert(snj_auth_key(&tokens, "replacement", error, sizeof(error)) == 0);
    assert(snj_auth_save(store.root_fd, &config.providers[0], &tokens, &previous,
                          NULL, NULL, error, sizeof(error)) == 0);
    assert(snj_auth_restore(store.root_fd, &config.providers[0], &tokens, &previous,
                             error, sizeof(error)) == 0);
    assert(snj_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) == 0);
    assert(strcmp(loaded.credential.value, "stored-key") == 0);
    assert(snj_auth_logout(store.root_fd, &config.providers[0], NULL, NULL, error, sizeof(error)) == 0);
    assert(symlinkat("outside", store.root_fd, "auth/default.json") == 0);
    assert(snj_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) < 0);
    assert(snj_auth_save(store.root_fd, &config.providers[0], &tokens, &previous,
                          NULL, NULL, error, sizeof(error)) < 0);
    assert(unlinkat(store.root_fd, "auth/default.json", 0) == 0);

    config.providers[0].auth = SNJ_AUTH_CHATGPT;
    strcpy(config.providers[0].base_url, SNJ_CHATGPT_BASE);
    {
        json_t *response = json_object();
        snj_auth_clear(&tokens);
        assert(snj_auth_token_response(response, &tokens, error, sizeof(error)) < 0);
        assert(tokens.credential.len == 0u);
        assert(snj_auth_key(&tokens, "old-access", error, sizeof(error)) == 0);
        strcpy(tokens.credential.account_id, "acct-test");
        strcpy(tokens.refresh_token, "old-refresh");
        assert(json_object_set_new(response, "access_token", json_string("rotated-access")) == 0);
        assert(json_object_set_new(response, "expires_in", json_integer(3600)) == 0);
        assert(snj_auth_token_response(response, &tokens, error, sizeof(error)) == 0);
        assert(strcmp(tokens.refresh_token, "old-refresh") == 0);
        assert(strcmp(tokens.credential.account_id, "acct-test") == 0);
        snj_auth_json_free(response);
    }
    for (int mode = MODEL_AUTH_DEVICE; mode <= MODEL_AUTH_EXPIRED; ++mode) {
        start_server(&server, (enum model_fixture)mode, false);
        snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u", server.port);
        assert(setenv("SNAJPAGENT_TEST_AUTH_BASE", endpoint, 1) == 0);
        error[0] = '\0';
        int rc = snj_auth_device(&tokens, mode == MODEL_AUTH_CANCEL ? cancel_device_poll : NULL,
                                  NULL, error, sizeof(error));
        if (mode == MODEL_AUTH_DEVICE) {
            assert(rc == 0);
            assert(strcmp(tokens.credential.value, "new-access") == 0);
            assert(strcmp(tokens.credential.account_id, "acct-test") == 0);
            assert(strcmp(tokens.refresh_token, "new-refresh") == 0);
            assert(tokens.expires_at_ms > snj_time_ms());
        } else {
            assert(rc < 0);
            assert(tokens.credential.len == 0u);
        }
        stop_server(&server);
    }
    for (int mode = MODEL_AUTH_REFRESH; mode <= MODEL_AUTH_401_TWICE; ++mode) {
        assert(snj_auth_key(&tokens, "old-access", error, sizeof(error)) == 0);
        strcpy(tokens.refresh_token, "old-refresh");
        strcpy(tokens.credential.account_id, "acct-test");
        tokens.expires_at_ms = mode >= MODEL_AUTH_401 ? snj_time_ms() + 3600000u : 1u;
        assert(snj_auth_save(store.root_fd, &config.providers[0], &tokens, NULL,
                              NULL, NULL, error, sizeof(error)) == 0);
        start_server(&server, (enum model_fixture)mode, false);
        snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u", server.port);
        assert(setenv("SNAJPAGENT_TEST_AUTH_BASE", endpoint, 1) == 0);
        error[0] = '\0';
        if (mode == MODEL_AUTH_REFRESH) {
            pid_t children[2];
            for (size_t i = 0; i < 2u; ++i) {
                children[i] = fork();
                assert(children[i] >= 0);
                if (children[i] == 0) {
                    int rc = snj_auth_read(store.root_fd, &config.providers[0], false,
                        NULL, &credential, NULL, NULL, error, sizeof(error));
                    _exit(rc == 0 && strcmp(credential.value, "new-access") == 0 ? 0 : 1);
                }
            }
            for (size_t i = 0; i < 2u; ++i) {
                assert(waitpid(children[i], &status, 0) == children[i]);
                assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            }
            assert(snj_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) == 0);
            assert(strcmp(loaded.refresh_token, "new-refresh") == 0);
        } else if (mode == MODEL_AUTH_REFRESH_FAILURE) {
            assert(snj_auth_read(store.root_fd, &config.providers[0], false, NULL,
                &credential, NULL, NULL, error, sizeof(error)) < 0);
            assert(!strstr(error, "private-refresh-server-detail"));
            assert(snj_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) == 0);
            assert(strcmp(loaded.refresh_token, "old-refresh") == 0);
        } else {
            json_t *models = NULL;
            assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", endpoint, 1) == 0);
            assert(snj_auth_read(store.root_fd, &config.providers[0], false, NULL,
                &credential, NULL, NULL, error, sizeof(error)) == 0);
            int rc = snj_provider_models_list(&config, &config.providers[0], &credential,
                NULL, NULL, NULL, &models, error, sizeof(error));
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
        json_t *request = request_with_marker("transport-compact"), *output = NULL;
        uint64_t bytes;
        credential_set(&credential, "transport-secret");
        credential.root_fd = -1;
        config.providers[0].auth = pass == 0u ? SNJ_AUTH_ENV : SNJ_AUTH_CHATGPT;
        strcpy(config.providers[0].base_url, pass == 0u ? "https://api.openai.com" : SNJ_CHATGPT_BASE);
        strcpy(config.providers[0].openrouter_referer, "https://github.com/snajpa/snajpagent");
        strcpy(config.providers[0].openrouter_title, "snajpagent");
        start_server(&server, pass == 2u ? MODEL_COMPACT_403 : MODEL_COMPACT_404, false);
        snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u", server.port);
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", endpoint, 1) == 0);
        int rc = snj_provider_responses_compact(request, &config, &config.providers[0],
            &credential, NULL, NULL, NULL, &output, &bytes, error, sizeof(error), NULL, NULL);
        assert(rc == (pass == 1u ? SNJ_PROVIDER_UNSUPPORTED : -1));
        assert(output == NULL);
        stop_server(&server);
        json_decref(request);
    }
    assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
    assert(snj_auth_logout(store.root_fd, &config.providers[0], NULL, NULL, error, sizeof(error)) == 0);
    assert(unlinkat(store.root_fd, "auth/default.lock", 0) == 0);
    assert(unlinkat(store.root_fd, "auth", AT_REMOVEDIR) == 0);
    assert(unlinkat(store.root_fd, "sessions", AT_REMOVEDIR) == 0);
    assert(unlinkat(store.root_fd, "trash", AT_REMOVEDIR) == 0);
    snj_store_close(&store);
    assert(rmdir(path) == 0);
    snj_config_free(&config);
    snj_auth_clear(&tokens);
    snj_auth_clear(&previous);
    snj_auth_clear(&loaded);
    snj_credential_clear(&credential);
}

int
main(void)
{
    test_provider_auth();
    test_ui_output_order_and_failure();
    test_read_only_dispatch();
    test_local_provider_transport();
    test_openrouter_search_transport();
    test_codex_model_list();
    test_codex_path_selection();
    test_structured_create_failures();
    test_count_capability_statuses();
    test_count_modes();
    puts("test_provider_transport: ok");
    return 0;
}
