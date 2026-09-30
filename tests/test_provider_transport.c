/* SPDX-License-Identifier: GPL-2.0-only */
#include "checked_json.h"
#include "base.h"
#include "app_internal.h"
#include "config.h"
#include "credential.h"
#include "http.h"
#include "json.h"
#include "model_cache.h"
#include "provider.h"
#include "provider_retry.h"
#include "sse.h"
#include "tools.h"
#include "convert.h"
#include "media.h"
#include "av.h"
#include "snajpagent.h"
#include "turn.h"
#include "voice.h"
#include "audio_device.h"
#include "voice_rtc.h"
#if SNAJPAGENT_AUDIO_DEVICE
#include <opus/opus.h>
#include <rtc/rtc.h>
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

#if SNAJPAGENT_AUDIO_DEVICE && defined(MA_NO_RUNTIME_LINKING) && defined(MA_ENABLE_ALSA)
#include <alsa/asoundlib.h>

/* Read configuration defaults only; never enumerate or open an audio device. */
static void test_static_alsa_config(void)
{
    const char *dirs[] = {NULL, "/fixture/alsa"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            assert((dirs[i] ? setenv("ALSA_CONFIG_DIR", dirs[i], 1) : unsetenv("ALSA_CONFIG_DIR")) == 0);
            assert(!strcmp(snd_config_topdir(), dirs[i] ? dirs[i] : "/usr/share/alsa"));
            _exit(0);
        }
        int status; pid_t done;
        do { done = waitpid(child, &status, 0); } while (done < 0 && errno == EINTR);
        assert(done == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}
#endif

#define REQUEST_MAX (64u * 1024u)
#define BODY_MAX (32u * 1024u)

/* Inherited only by the voice-request fixture child, never a product control. */
static int voice_request_ready_fd = -1;
static int voice_request_release_fd = -1;

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
    MODEL_CREATE_TYPELESS,
    MODEL_VOICE_REQUEST,
    MODEL_VOICE_REQUEST_WAIT,
    MODEL_VOICE_CLOSE,
    MODEL_VOICE_CLOSE_SUMMARY,
    MODEL_VOICE_CLOSE_CREDENTIAL,
    MODEL_VOICE_CLOSE_REFRESH,
    MODEL_VOICE_QUEUE,
    MODEL_VOICE_COMPACT,
    MODEL_VOICE_COMPACT_RETRY,
    MODEL_VOICE_COMPACT_TOOL,
    MODEL_VOICE_COMPACT_ACTIVE,
    MODEL_VOICE_BACKLOG,
    MODEL_VOICE_NATIVE_SUMMARY,
    MODEL_VOICE_NATIVE_TOOL,
    MODEL_VOICE_NATIVE_REFUSAL,
    MODEL_VOICE_NATIVE_EMPTY,
    MODEL_VOICE_NATIVE_NONREDUCING,
    MODEL_VOICE_NATIVE_OVERSIZED,
    MODEL_VOICE_NATIVE_CANCEL,
    MODEL_VOICE_NATIVE_CANCEL_DONE,
    MODEL_VOICE_NATIVE_HANDOFF,
    MODEL_VOICE_READ,
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
    MODEL_COMPACT_502,
    MODEL_COMPACT_OK,
    MODEL_COMPACT_403,
    MODEL_AUDIO_LISTEN,
    MODEL_AUDIO_TRANSCRIBE,
    MODEL_AUDIO_SPEAK,
    MODEL_AUDIO_FAILURE,
    MODEL_NATIVE_TRANSCRIBE,
    MODEL_NATIVE_CALL,
    MODEL_NATIVE_CALL_NO_ID,
    MODEL_NATIVE_CALL_DENIED,
    MODEL_NATIVE_CALL_TEMPORARY,
    MODEL_NATIVE_CALL_POLICY,
    MODEL_NATIVE_CALL_CAPACITY,
    MODEL_NATIVE_CALL_MALFORMED,
    MODEL_NATIVE_CALL_INVALID,
    MODEL_NATIVE_CALL_AUTH_ERROR,
    MODEL_NATIVE_CALL_EMPTY,
    MODEL_NATIVE_CALL_UNKNOWN,
    MODEL_NATIVE_CALL_PARTIAL,
    MODEL_AUTH_DEVICE,
    MODEL_AUTH_CANCEL,
    MODEL_AUTH_EXPIRED,
    MODEL_AUTH_REFRESH,
    MODEL_AUTH_REFRESH_FAILURE,
    MODEL_AUTH_401,
    MODEL_AUTH_401_TWICE,
    MODEL_META_DEVICE,
    MODEL_META_DENIED,
    MODEL_META_EXPIRED,
    MODEL_META_REFRESH,
    MODEL_META_REFRESH_FAILURE
};

static bool authentication_fixture;
/* When set, the transport fixture requires this exact header line. */
static const char *expected_session_header;
/* When set, the transport fixture requires this exact Authorization line. */
static const char *expected_authorization_header = "Authorization: Bearer transport-secret";
/* Scoped off for fixtures whose requests deliberately carry no OpenRouter headers. */
static bool fixture_requires_openrouter = true;

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
            if (errno == EINTR) continue;
            _exit(90);
        }
        if (n == 0) _exit(91);
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

    if (!p) p = strstr(headers, "content-length:");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
        ++p;
    while (*p == ' ' || *p == '\t') ++p;
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
        if (buffer[i] == '\r' && buffer[i + 1u] == '\n' && buffer[i + 2u] == '\r' && buffer[i + 3u] == '\n')
            return i + 4u;
    for (size_t i = 0; i + 1u < len; ++i)
        if (buffer[i] == '\n' && buffer[i + 1u] == '\n') return i + 2u;
    return 0u;
}

static void
read_request_body(int fd, struct http_request *request, char *body, size_t capacity)
{
    char buffer[REQUEST_MAX];
    size_t used = 0u;
    size_t header_end = 0u;
    long cl;
    int matched;

    memset(request, 0, sizeof(*request));
    while (!header_end) {
        ssize_t n;
        if (used == sizeof(buffer)) server_fail("request headers exceeded test bound");
        n = read(fd, buffer + used, sizeof(buffer) - used);
        if (n < 0) {
            if (errno == EINTR) continue;
            server_fail("request read failed");
        }
        if (n == 0) server_fail("request closed before headers");
        used += (size_t)n;
        header_end = find_header_end(buffer, used);
    }
    if (header_end >= sizeof(request->headers)) server_fail("retained headers exceeded test bound");
    memcpy(request->headers, buffer, header_end);
    request->headers[header_end] = '\0';
    matched = sscanf(request->headers, "%7s %127s HTTP/1", request->method, request->path);
    if (matched != 2) server_fail("unexpected request line");
    if (!authentication_fixture && expected_authorization_header &&
        !header_contains(request->headers, expected_authorization_header))
        server_fail("authorization header missing or unredacted differently");
    if (!authentication_fixture && fixture_requires_openrouter && !header_contains(request->headers,
                         "HTTP-Referer: https://github.com/snajpa/snajpagent"))
        server_fail("OpenRouter referer header missing");
    if (!authentication_fixture && fixture_requires_openrouter &&
        !header_contains(request->headers, "X-OpenRouter-Title: snajpagent"))
        server_fail("OpenRouter title header missing");
    if (!header_contains(request->headers, "User-Agent: " SNAJPAGENT_NAME "/"
                         SNAJPAGENT_VERSION)) server_fail("product user agent missing or stale");
    if (expected_session_header &&
        !header_contains(request->headers, expected_session_header))
        server_fail("session identity header missing or altered");
    cl = content_length(request->headers);
    if (strcmp(request->method, "GET") == 0 && cl < 0) cl = 0;
    if (cl < 0 || (size_t)cl >= capacity) server_fail("invalid content length");
    request->body_len = (size_t)cl;
    if (used - header_end > request->body_len) server_fail("request body overflowed expected length");
    if (header_contains(request->headers, "\r\nExpect: 100-continue\r\n")) {
        const char ready[] = "HTTP/1.1 100 Continue\r\n\r\n";
        write_all_or_die(fd, ready, sizeof(ready) - 1u);
    }
    memcpy(body, buffer + header_end, used - header_end);
    while (used - header_end < request->body_len) {
        ssize_t n = read(fd, body + (used - header_end), request->body_len - (used - header_end));
        if (n < 0) {
            if (errno == EINTR) continue;
            server_fail("body read failed");
        }
        if (n == 0) server_fail("request closed before body");
        used += (size_t)n;
    }
    body[request->body_len] = '\0';
}

static void
read_request(int fd, struct http_request *request)
{
    read_request_body(fd, request, request->body, sizeof(request->body));
}

static void
send_response(int fd, unsigned int status, const char *content_type, const char *body)
{
    char header[256];
    size_t len = strlen(body);
    int n = snprintf(header, sizeof(header), "HTTP/1.1 %u Test\r\n"
                     "Content-Type: %s\r\n" "Content-Length: %llu\r\n"
                     "Connection: close\r\n\r\n", status, content_type, (unsigned long long)len);
    if (n <= 0 || (size_t)n >= sizeof(header)) server_fail("response header build failed");
    write_all_or_die(fd, header, (size_t)n);
    write_all_or_die(fd, body, len);
}

static char
voice_close_release(void)
{
    struct pollfd release = {.fd = voice_request_release_fd, .events = POLLIN};
    char action = 0;
    if (write(voice_request_ready_fd, "R", 1u) != 1 ||
        poll(&release, 1u, 5000) != 1 || read(release.fd, &action, 1u) != 1) {
        server_fail("voice close request barrier failed");
    }
    return action;
}

static void
send_voice_item(int fd, json_t *event)
{
    char *wire = json_dumps(event, JSON_COMPACT);
    char response[BODY_MAX];
    int n = wire ? snprintf(response, sizeof(response),
        "data: {\"type\":\"response.created\","
        "\"response\":{\"id\":\"voice_request\","
        "\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: %s\n\n"
        "data: {\"type\":\"response.completed\","
        "\"response\":{\"id\":\"voice_request\","
        "\"status\":\"completed\",\"output\":[]}}\n\n", wire) : -1;
    if (n <= 0 || (size_t)n >= sizeof(response)) server_fail("voice fixture encoding failed");
    free(wire);
    json_decref(event);
    send_response(fd, 200u, "text/event-stream", response);
}

struct voice_summary_fixture {
    json_t *expected;
    struct snag_buf record;
    size_t index;
    uint64_t rejected_at;
    char *result;
    bool user_seen;
    bool assistant_seen;
    bool current_seen;
};

static const char *const voice_transition_types[] = {
    "control_requested", "control_started", "control_finished", "goal_reworded",
    "goal_lock_changed", "goal_cancelled", "model_selection_changed", "turn_model_changed"
};

static bool
serve_voice_summary(int fd, const json_t *body, bool retry, bool continued,
    struct voice_summary_fixture *state)
{
    json_t *input = json_object_get(body, "input");
    json_t *parts = NULL;
    for (size_t i = 0; i < json_array_size(input); ++i) {
        json_t *message = json_array_get(input, i);
        const char *role = snag_json_string(message, "role");
        const char *text = snag_json_string(message, "content");
        if (role && !strcmp(role, "user") && text) {
            parts = json_loads(text, JSON_REJECT_DUPLICATES, NULL);
            break;
        }
    }
    if (!json_is_array(json_object_get(body, "tools")) ||
        json_array_size(json_object_get(body, "tools")) || !json_array_size(parts)) {
        server_fail("compaction source missing or executable tools declared");
    }
    if (!state->expected) {
        state->expected = json_incref(parts);
        state->index = 1u;
        state->record.max = BODY_MAX;
        if (retry) {
            state->rejected_at = snag_monotonic_ms();
            send_response(fd, 400u, "application/json",
                "{\"error\":{\"code\":\"context_length_exceeded\","
                "\"message\":\"summary input too large\"}}");
            json_decref(parts);
            return false;
        }
    } else if (state->rejected_at) {
        if (snag_monotonic_ms() - state->rejected_at <
            snag_provider_retry_delay_ms(0u, false, 0u)) {
            server_fail("capacity recovery retried without pacing");
        }
        state->rejected_at = 0u;
    }
    for (size_t i = 0; i < json_array_size(parts); ++i) {
        json_t *part = json_array_get(parts, i);
        const char *source = snag_json_string(part, "source_json");
        size_t offset = (size_t)json_integer_value(json_object_get(part, "offset"));
        size_t end = (size_t)json_integer_value(json_object_get(part, "end"));
        if (!source || (size_t)json_integer_value(json_object_get(part, "history_item")) !=
                state->index || offset != state->record.len || end < offset ||
            end - offset != strlen(source) ||
            snag_buf_append(&state->record, source, strlen(source)) < 0) {
            server_fail("compaction reordered, duplicated or dropped a source fragment");
        }
        if (!json_is_true(json_object_get(part, "complete"))) continue;
        const char *expected = snag_json_string(
            json_array_get(state->expected, state->index - 1u), "source_json");
        if (!expected || strlen(expected) != state->record.len ||
            memcmp(expected, state->record.data, state->record.len)) {
            server_fail("compaction changed the original source while fragmenting it");
        }
        json_t *item = json_loadb((char *)state->record.data, state->record.len,
            JSON_REJECT_DUPLICATES, NULL);
        const char *role = snag_json_string(item, "role");
        const char *text = snag_json_string(item, "content");
        const char *output = snag_json_string(item, "output");
        if (role && !strcmp(role, "user") && text &&
            strstr(text, "retained-first-utterance")) {
            state->user_seen = true;
        }
        if (role && !strcmp(role, "user") && text && strstr(text, "second")) {
            state->current_seen = true;
        }
        if (continued && role && !strcmp(role, "developer") && text) {
            json_t *prior = json_loads(text, JSON_REJECT_DUPLICATES, NULL);
            const char *operation = snag_json_string(prior, "operation");
            const char *summary = snag_json_string(json_object_get(prior, "summary"), "text");
            if (operation && !strcmp(operation, "interface_compacted") && summary) {
                state->user_seen = strstr(summary, "retained-first-utterance") != NULL;
                free(state->result);
                state->result = strdup(summary);
                if (!state->result) server_fail("cannot retain the preceding summary");
            }
            json_decref(prior);
        }
        if (role && !strcmp(role, "assistant")) state->assistant_seen = true;
        if (output) {
            free(state->result);
            state->result = strdup(output);
            if (!state->result) server_fail("cannot retain the fixture's tool result");
        }
        json_decref(item);
        snag_buf_reset(&state->record);
        ++state->index;
    }
    bool done = state->index == json_array_size(state->expected) + 1u;
    if (done && continued && !state->current_seen) {
        server_fail("repeated capacity recovery skipped the active delegation");
    }
    if (done && (!state->user_seen || !state->result)) {
        server_fail("compaction lost original dialogue or the tool outcome");
    }
    char summary[BODY_MAX / 2u];
    int length = snprintf(summary, sizeof(summary),
        "%s%s Already admitted voice_call_first, result so far: %s",
        state->user_seen ? "User: retained-first-utterance. " : "Partial source. ",
        state->assistant_seen ? "Assistant replied. " : "",
        state->result ? state->result : "not yet in this fragment");
    if (length <= 0 || (size_t)length >= sizeof(summary)) {
        server_fail("compaction fixture summary exceeded its buffer");
    }
    send_voice_item(fd, json_pack("{s:s,s:i,s:{s:s,s:s,s:s,s:s,s:[{s:s,s:s}]}}",
        "type", "response.output_item.done", "output_index", 0,
        "item", "id", "voice_summary", "type", "message", "role", "assistant",
        "status", "completed", "content", "type", "output_text", "text", summary));
    json_decref(parts);
    return done;
}

static void
voice_backlog_record(const json_t *item, unsigned int *started, unsigned int *completed,
    uint64_t *last_seq)
{
    const char *text = snag_json_string(item, "content");
    json_t *view = text ? json_loads(text, JSON_REJECT_DUPLICATES, NULL) : NULL;
    const char *kind = snag_json_string(view, "kind");
    if (!kind || strcmp(kind, "session_observation")) {
        json_decref(view);
        return;
    }
    uint64_t seq = (uint64_t)json_integer_value(json_object_get(view, "seq"));
    const char *type = snag_json_string(view, "event_type");
    json_t *data = json_object_get(view, "data");
    if (seq <= *last_seq || !type) server_fail("initial history order changed");
    *last_seq = seq;
    if (!strcmp(type, "voice_event")) {
        json_decref(view);
        return;
    }
    char identity[33];
    if (!strcmp(type, "goal_started")) {
        const char *prompt = snag_json_string(data, "prompt");
        if (*started != *completed || !prompt || strlen(prompt) != SNAG_MAX_GOAL_PROMPT) {
            server_fail("initial goal history was dropped or truncated");
        }
        for (size_t i = 0u; i < SNAG_MAX_GOAL_PROMPT; i += 2u) {
            if (memcmp(prompt + i, "λ", 2u)) server_fail("initial goal source bytes changed");
        }
        ++*started;
    } else if (!strcmp(type, "goal_completed")) {
        if (*started != *completed + 1u) server_fail("initial goal outcome reordered");
        ++*completed;
    } else {
        server_fail("unexpected initial history event");
    }
    assert(snprintf(identity, sizeof(identity), "%032x", *started) == 32);
    const char *id = snag_json_string(data, "goal_id");
    if (!id || strcmp(id, identity)) server_fail("initial goal identity changed");
    json_decref(view);
}

static void
serve_voice_backlog(int listen_fd, const char *ordinary_reply)
{
    /* The small fixtures keep their stack buffer; this case reaches the real byte bound. */
    char *wire = malloc(SNAG_CONTEXT_MAX_REQUEST + 1u);
    struct snag_buf record = {.max = SNAG_CONTEXT_MAX_REQUEST};
    unsigned int started = 0u;
    unsigned int completed = 0u;
    unsigned int summaries = 0u;
    size_t index = 1u;
    uint64_t last_seq = 0u;
    char summary[128] = {0};
    if (!wire) server_fail("cannot allocate large request fixture");
    (void)alarm(90u);
    for (;;) {
        struct http_request request;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) server_fail("initial history accept failed");
        read_request_body(fd, &request, wire, SNAG_CONTEXT_MAX_REQUEST + 1u);
        if (strcmp(request.method, "POST") || strcmp(request.path, "/v1/responses")) {
            server_fail("unexpected initial history request");
        }
        json_t *body = json_loadb(wire, request.body_len, JSON_REJECT_DUPLICATES, NULL);
        json_t *input = json_object_get(body, "input");
        json_t *tools = json_object_get(body, "tools");
        if (!json_array_size(input) || !json_is_array(tools)) {
            server_fail("initial history request malformed");
        }
        if (summaries && !strstr(wire, summary)) {
            server_fail("initial history lost its preceding partial summary");
        }
        unsigned int users = 0u;
        json_t *parts = NULL;
        bool ordinary = json_array_size(tools) != 0u;
        for (size_t i = 0u; i < json_array_size(input); ++i) {
            json_t *item = json_array_get(input, i);
            const char *role = snag_json_string(item, "role");
            const char *text = snag_json_string(item, "content");
            if (ordinary) voice_backlog_record(item, &started, &completed, &last_seq);
            if (!role || strcmp(role, "user")) continue;
            ++users;
            if (ordinary) {
                if (!text || !strstr(text, "backlog-pending-utterance")) {
                    server_fail("preseed compaction lost the pending utterance");
                }
            } else {
                parts = text ? json_loads(text, JSON_REJECT_DUPLICATES, NULL) : NULL;
            }
        }
        if (users != 1u) server_fail("initial history replayed or omitted user input");
        if (ordinary) {
            if (summaries < 2u || record.len || started != 34u || completed != 34u) {
                server_fail("initial history compaction lost source or outcomes");
            }
            send_response(fd, 200u, "text/event-stream", ordinary_reply);
            json_decref(body);
            (void)close(fd);
            break;
        }
        if (!json_array_size(parts)) server_fail("initial summary source missing");
        for (size_t i = 0u; i < json_array_size(parts); ++i) {
            json_t *part = json_array_get(parts, i);
            const char *source = snag_json_string(part, "source_json");
            size_t offset = (size_t)json_integer_value(json_object_get(part, "offset"));
            size_t end = (size_t)json_integer_value(json_object_get(part, "end"));
            if (!source || (size_t)json_integer_value(json_object_get(part, "history_item")) !=
                    index || offset != record.len || end < offset ||
                end - offset != strlen(source) ||
                snag_buf_append(&record, source, strlen(source)) < 0) {
                server_fail("initial source fragment reordered or dropped");
            }
            if (!json_is_true(json_object_get(part, "complete"))) continue;
            json_t *item = json_loadb((char *)record.data, record.len,
                JSON_REJECT_DUPLICATES, NULL);
            if (!item) server_fail("initial history source is not complete JSON");
            voice_backlog_record(item, &started, &completed, &last_seq);
            json_decref(item);
            snag_buf_reset(&record);
            ++index;
        }
        ++summaries;
        int length = snprintf(summary, sizeof(summary),
            "Archived %u completed goals; no pending work. Earlier read was cancelled.", completed);
        if (length <= 0 || (size_t)length >= sizeof(summary)) server_fail("summary overflow");
        send_voice_item(fd, json_pack("{s:s,s:i,s:{s:s,s:s,s:s,s:s,s:[{s:s,s:s}]}}",
            "type", "response.output_item.done", "output_index", 0,
            "item", "id", "backlog_summary", "type", "message", "role", "assistant",
            "status", "completed", "content", "type", "output_text", "text", summary));
        json_decref(parts);
        json_decref(body);
        (void)close(fd);
    }
    snag_buf_free(&record);
    free(wire);
    _exit(0);
}

static void
send_native_large_summary(int fd)
{
    size_t length = SNAG_MAX_PUBLIC_ITEM;
    char *text = malloc(length + 1u);
    if (!text) server_fail("native summary allocation failed");
    memset(text, 'g', length);
    text[length] = '\0';
    memcpy(text + SNAG_MAX_SSE_EVENT / 2u - 11u,
        "native-capacity-secret", sizeof("native-capacity-secret") - 1u);
    memcpy(text + length - 19u, "native complete end", 19u);
    struct snag_buf response = {.max = SNAG_MAX_PROVIDER_WIRE};
    if (snag_buf_printf(&response,
            "data: {\"type\":\"response.created\",\"response\":{\"id\":\"voice_request\","
            "\"status\":\"in_progress\",\"output\":[]}}\n\n"
            "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{"
            "\"type\":\"message\",\"id\":\"native_summary\",\"role\":\"assistant\","
            "\"status\":\"in_progress\",\"phase\":\"final_answer\",\"content\":[]}}\n\n") < 0) {
        server_fail("native summary header failed");
    }
    /* Complete small parts form one large public message without exceeding
     * the existing per-event SSE bound. */
    size_t index = 0u;
    for (size_t offset = 0u; offset < length; offset += SNAG_MAX_SSE_EVENT / 2u) {
        size_t bytes = length - offset;
        if (bytes > SNAG_MAX_SSE_EVENT / 2u) bytes = SNAG_MAX_SSE_EVENT / 2u;
        json_t *part = json_pack("{s:s,s:s,s:i,s:I,s:{s:s,s:s%}}",
            "type", "response.content_part.done", "item_id", "native_summary",
            "output_index", 0, "content_index", (json_int_t)index++,
            "part", "type", "output_text", "text", text + offset, bytes);
        char *wire = part ? json_dumps(part, JSON_COMPACT) : NULL;
        if (!wire || snag_buf_printf(&response, "data: %s\n\n", wire) < 0) {
            server_fail("native summary part failed");
        }
        free(wire);
        json_decref(part);
    }
    free(text);
    if (snag_buf_printf(&response,
            "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"voice_request\","
            "\"status\":\"completed\",\"output\":[]}}\n\n") < 0 ||
        snag_buf_terminate(&response) < 0) server_fail("native summary completion failed");
    send_response(fd, 200u, "text/event-stream", (char *)response.data);
    snag_buf_free(&response);
}

static void
serve_native_summary(int listen_fd, enum model_fixture mode)
{
    char *wire = malloc(SNAG_CONTEXT_MAX_REQUEST + 1u);
    struct snag_buf record = {.max = SNAG_CONTEXT_MAX_REQUEST};
    uint64_t previous = 0u, rejected_at = 0u;
    size_t rejected_bytes = 0u;
    unsigned int requests = 0u, transcripts = 0u;
    unsigned int queues = 0u;
    bool finished = false, handoff_done = false, second = false;
    bool closing = mode == MODEL_VOICE_NATIVE_CANCEL || mode == MODEL_VOICE_NATIVE_CANCEL_DONE;
    if (!wire) server_fail("cannot allocate native summary request");
    while (!finished) {
        struct http_request request;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) server_fail("native summary accept failed");
        read_request_body(fd, &request, wire, SNAG_CONTEXT_MAX_REQUEST + 1u);
        json_t *body = json_loadb(wire, request.body_len, JSON_REJECT_DUPLICATES, NULL);
        json_t *tools = json_object_get(body, "tools");
        json_t *input = json_object_get(body, "input");
        json_t *parts = NULL;
        if (mode == MODEL_VOICE_NATIVE_HANDOFF && json_array_size(tools)) {
            if (handoff_done || !strstr(wire, "pre-existing discussion")) {
                server_fail("native recovery replayed or lost its pending handoff");
            }
            send_voice_item(fd, json_pack("{s:s,s:i,s:{s:s,s:s,s:s,s:s,s:[{s:s,s:s}]}}",
                "type", "response.output_item.done", "output_index", 0,
                "item", "type", "message", "id", "native_existing", "role", "assistant",
                "status", "completed", "content", "type", "output_text", "text",
                "Settled pre-existing discussion"));
            handoff_done = true;
            json_decref(body);
            close(fd);
            continue;
        }
        if (mode == MODEL_VOICE_NATIVE_HANDOFF && !handoff_done) {
            server_fail("native maintenance displaced its pending helper request");
        }
        if (strcmp(request.path, "/v1/responses") || !json_is_array(tools) ||
            json_array_size(tools) || strstr(wire, "native-capacity-secret")) {
            server_fail("native maintenance exposed tools or a credential");
        }
        if (second && strstr(wire, "next-native-secret")) {
            server_fail("native recovery carried a newly registered credential into its summary");
        }
        for (size_t i = 0u; i < json_array_size(input); ++i) {
            json_t *item = json_array_get(input, i);
            const char *role = snag_json_string(item, "role");
            const char *text = snag_json_string(item, "content");
            if (role && !strcmp(role, "user") && text) {
                parts = json_loads(text, JSON_REJECT_DUPLICATES, NULL);
                break;
            }
        }
        if (!json_array_size(parts)) server_fail("native maintenance omitted history");
        size_t bytes = 0u;
        for (size_t i = 0u; i < json_array_size(parts); ++i) {
            bytes += json_string_length(json_object_get(json_array_get(parts, i), "source_json"));
        }
        if (!requests++) {
            rejected_bytes = bytes;
            rejected_at = snag_monotonic_ms();
            send_response(fd, 400u, "application/json",
                "{\"error\":{\"code\":\"context_length_exceeded\","
                "\"message\":\"summary input too large\"}}");
        } else {
            if (rejected_at && (bytes >= rejected_bytes ||
                    snag_monotonic_ms() - rejected_at <
                        snag_provider_retry_delay_ms(0u, false, 0u))) {
                server_fail("native maintenance retried unchanged history or without pacing");
            }
            rejected_at = 0u;
            if (closing && voice_close_release() == 'X') {
                json_decref(parts);
                json_decref(body);
                close(fd);
                _exit(0);
            }
            const char *prior = mode == MODEL_VOICE_NATIVE_NONREDUCING ?
                "gggggggg" : "native carry";
            if (requests > 2u && !strstr(wire, prior)) {
                server_fail("native maintenance lost its prior summary");
            }
            for (size_t i = 0u; i < json_array_size(parts); ++i) {
                json_t *part = json_array_get(parts, i);
                const char *text = snag_json_string(part, "source_json");
                size_t offset = (size_t)json_integer_value(json_object_get(part, "offset"));
                size_t end = (size_t)json_integer_value(json_object_get(part, "end"));
                if (!text || offset != record.len || end <= offset ||
                    end - offset != strlen(text) ||
                    snag_buf_append(&record, text, end - offset) < 0) {
                    server_fail("native maintenance lost ordered UTF-8 fragments");
                }
                if (!json_is_true(json_object_get(part, "complete"))) continue;
                json_t *item = json_loadb((char *)record.data, record.len,
                    JSON_REJECT_DUPLICATES, NULL);
                uint64_t seq = (uint64_t)json_integer_value(json_object_get(part, "source_seq"));
                json_t *event = json_object_get(item, "event");
                const char *source = snag_json_string(event, "text");
                const char *operation = snag_json_string(event, "operation");
                const char *type = snag_json_string(part, "event_type");
                if (!seq || seq <= previous || !type) {
                    server_fail("native maintenance source cursor is not ordered");
                }
                previous = seq;
                if (!strcmp(type, "future_turn_queued")) {
                    const char *queue = snag_json_string(item, "queue_id");
                    if (!queue || strcmp(queue, "33333333333333333333333333333333")) {
                        server_fail("native maintenance changed an accepted queue identity");
                    }
                    ++queues;
                }
                if (source && strstr(source, "native transcript")) ++transcripts;
                if (operation && !strcmp(operation, mode == MODEL_VOICE_NATIVE_HANDOFF ?
                        "interface_reply" : "provider_error")) finished = true;
                json_decref(item);
                snag_buf_reset(&record);
            }
            if (mode == MODEL_VOICE_NATIVE_TOOL) {
                send_voice_item(fd, json_pack("{s:s,s:i,s:{s:s,s:s,s:s,s:s,s:s,s:s}}",
                    "type", "response.output_item.done", "output_index", 0,
                    "item", "type", "function_call", "id", "native_forbidden_tool",
                    "call_id", "do_not_execute", "name", "submit_input",
                    "arguments", "{\"target\":\"queue\",\"text\":\"must not run\"}",
                    "status", "completed"));
            } else if (mode == MODEL_VOICE_NATIVE_OVERSIZED) {
                send_native_large_summary(fd);
            } else {
                char growing[8193];
                memset(growing, 'g', sizeof(growing) - 1u);
                growing[sizeof(growing) - 1u] = '\0';
                bool refusal = mode == MODEL_VOICE_NATIVE_REFUSAL;
                const char *text = refusal ? "Cannot summarize" :
                    mode == MODEL_VOICE_NATIVE_EMPTY ? "" :
                    mode == MODEL_VOICE_NATIVE_NONREDUCING ? growing :
                    "native carry native-capacity-secret next-native-secret; "
                    "queue 33333333333333333333333333333333";
                send_voice_item(fd, json_pack("{s:s,s:i,s:{s:s,s:s,s:s,s:s,s:[{s:s,s:s}]}}",
                    "type", "response.output_item.done", "output_index", 0,
                    "item", "type", "message", "id", "native_summary", "role", "assistant",
                    "status", "completed", "content", "type", refusal ? "refusal" : "output_text",
                    refusal ? "refusal" : "text", text));
            }
        }
        json_decref(parts);
        json_decref(body);
        if (close(fd) < 0) server_fail("native summary close failed");
        if (requests > 1u && (closing || mode == MODEL_VOICE_NATIVE_OVERSIZED ||
                (mode >= MODEL_VOICE_NATIVE_TOOL &&
                mode <= MODEL_VOICE_NATIVE_EMPTY))) _exit(0);
        if (finished && mode == MODEL_VOICE_NATIVE_SUMMARY && !second) {
            if (transcripts != 5u || queues != 1u || requests < 4u) {
                server_fail("first native recovery skipped its source history");
            }
            second = true;
            finished = false;
            transcripts = queues = 0u;
        }
    }
    if (transcripts != (second ? 0u : 5u) || queues != (second ? 0u : 1u) ||
        ((mode == MODEL_VOICE_NATIVE_SUMMARY || mode == MODEL_VOICE_NATIVE_HANDOFF) &&
            requests < 4u)) {
        server_fail("native maintenance skipped part of its paged history");
    }
    snag_buf_free(&record);
    free(wire);
    _exit(0);
}

static void
serve_one(int listen_fd, unsigned int status, const char *method, const char *path, const char *marker,
          const char *content_type, const char *body)
{
    struct http_request request;
    int fd;

    fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) server_fail("accept failed");
    read_request(fd, &request);
    if (strcmp(request.method, method) != 0) server_fail("unexpected provider HTTP method");
    if (strcmp(request.path, path) != 0) server_fail("unexpected provider endpoint path");
    if (marker && !strstr(request.body, marker)) server_fail("request body marker missing");
    send_response(fd, status, content_type, body);
    if (close(fd) < 0) server_fail("close accepted socket failed");
}

static void
fixture_stop(int signal_number)
{
    (void)signal_number;
    _exit(0);
}

static void
meta_auth_server_child(int listen_fd, enum model_fixture fixture)
{
    struct http_request request;
    int fd;

    authentication_fixture = true;
    (void)alarm(120u);
    if (fixture == MODEL_META_REFRESH || fixture == MODEL_META_REFRESH_FAILURE) {
        fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) server_fail("meta accept failed");
        read_request(fd, &request);
        if (strcmp(request.method, "POST") || strcmp(request.path, "/oidc/device/token/") ||
            !strstr(request.body, "grant_type=refresh_token") ||
            !strstr(request.body, "meta-old-refresh")) server_fail("wrong Meta refresh request");
        if (fixture == MODEL_META_REFRESH_FAILURE)
            send_response(fd, 401u, "application/json", "{\"error\":\"revoked\"}");
        else
            send_response(fd, 200u, "application/json",
                          "{\"access_token\":\"meta-new-access\",\"refresh_token\":\"meta-new-refresh\",\"expires_in\":3600}");
        if (close(fd) < 0) server_fail("close Meta refresh socket failed");
        /* The client falls back to a device login when the refresh does not
         * succeed; serve those legs too so the test never waits out the link. */
        for (;;) {
            struct pollfd waiting = {.fd = listen_fd, .events = POLLIN};
            if (poll(&waiting, 1, 2000) <= 0) _exit(0);
            fd = accept(listen_fd, NULL, NULL);
            if (fd < 0) server_fail("meta accept failed");
            read_request(fd, &request);
            if (strstr(request.path, "/oidc/device/authorization/")) {
                send_response(fd, 200u, "application/json",
                              "{\"device_code\":\"meta-device\",\"user_code\":\"ABCD-1234\","
                              "\"verification_uri\":\"https://auth.meta.com/oidc/device/\","
                              "\"verification_uri_complete\":\"https://auth.meta.com/oidc/device/?code=ABCD-1234\","
                              "\"expires_in\":60,\"interval\":1}");
            } else if (strstr(request.path, "/oidc/device/token/")) {
                send_response(fd, 200u, "application/json",
                              "{\"access_token\":\"meta-access\",\"refresh_token\":\"meta-refresh\",\"expires_in\":3600}");
            } else {
                send_response(fd, 404u, "application/json", "{}");
            }
            if (close(fd) < 0) server_fail("close Meta socket failed");
        }
    }
fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) server_fail("meta accept failed");
    read_request(fd, &request);
    if (strcmp(request.method, "POST") || strcmp(request.path, "/oidc/device/authorization/") ||
        !strstr(request.body, "client_id=")) server_fail("wrong Meta authorization request");
    send_response(fd, 200u, "application/json",
                  "{\"device_code\":\"meta-device\",\"user_code\":\"ABCD-1234\","
                  "\"verification_uri\":\"https://auth.meta.com/oidc/device/\","
                  "\"verification_uri_complete\":\"https://auth.meta.com/oidc/device/?code=ABCD-1234\","
                  "\"expires_in\":60,\"interval\":1}");
    if (close(fd) < 0) server_fail("close Meta authorization socket failed");
    if (fixture == MODEL_META_DENIED || fixture == MODEL_META_EXPIRED) {
        fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) server_fail("meta accept failed");
        read_request(fd, &request);
        if (strcmp(request.method, "POST") || strcmp(request.path, "/oidc/device/token/") ||
            !strstr(request.body, "device_code=meta-device"))
            server_fail("wrong Meta token request");
        if (fixture == MODEL_META_DENIED)
            send_response(fd, 400u, "application/json", "{\"error\":\"access_denied\"}");
        else
            send_response(fd, 400u, "application/json", "{\"error\":\"expired_token\"}");
        if (close(fd) < 0) server_fail("close Meta token socket failed");
        _exit(0);
    }
    /* Token polling repeats until the client stops: a loaded host may poll more
     * times than the two-step minimum, and a fixture that exited after a fixed
     * count would leave the client waiting out its whole link lifetime. The
     * grant repeats; the fixture exits after it or after a 30 s quiet grace. */
    for (;;) {
        struct pollfd waiting = {.fd = listen_fd, .events = POLLIN};
        if (poll(&waiting, 1, 30000) <= 0) _exit(0);
        fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) server_fail("meta accept failed");
        read_request(fd, &request);
        if (strcmp(request.method, "POST") || strcmp(request.path, "/oidc/device/token/") ||
            !strstr(request.body, "device_code=meta-device")) {
            /* Never die mid-flow: an unexpected request gets an error and the
             * loop keeps serving, so the client is never left waiting out its
             * whole link lifetime after the fixture vanished. */
            send_response(fd, 400u, "application/json", "{\"error\":\"invalid_request\"}");
            if (close(fd) < 0) server_fail("close Meta token socket failed");
            continue;
        }
        /* Grant on every poll and keep serving: exiting after the first grant
         * left a client that polled again with a dead listener, which then
         * waited out its whole link lifetime. The test stops this fixture
         * when its own client has finished. */
        send_response(fd, 200u, "application/json",
                      "{\"access_token\":\"meta-access\",\"refresh_token\":\"meta-refresh\",\"expires_in\":3600}");
        if (close(fd) < 0) server_fail("close Meta token socket failed");
        /* Keep serving until the test stops us. Exiting on a short grace after
         * the grant used to leave a client that polled again holding a
         * connection the dead fixture never accepted; the suite then parked
         * forever, because the test keeps its copy of the listener open. */
    }
}

static void
auth_server_child(int listen_fd, enum model_fixture fixture)
{
    if (fixture >= MODEL_META_DEVICE) {
        meta_auth_server_child(listen_fd, fixture);
        return;
    }
    static const char tokens[] =
        "{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"expires_in\":3600,"
        "\"id_token\":\"e30.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjdC10ZXN0In19.sig\"}";
    unsigned int count = fixture == MODEL_AUTH_DEVICE ? 4u :
        fixture == MODEL_AUTH_CANCEL || fixture == MODEL_AUTH_EXPIRED ? 2u :
        fixture >= MODEL_AUTH_401 ? 3u : 1u;

    authentication_fixture = true;
    (void)alarm(120u);
    if (fixture <= MODEL_AUTH_EXPIRED) {
        struct http_request request;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) server_fail("auth accept failed");
        read_request(fd, &request);
        if (strcmp(request.path, "/api/accounts/deviceauth/usercode"))
            server_fail("incorrect device flow path");
        if (!strstr(request.body, "app_EMoamEEZ73f0CkXaXp7hrann"))
            server_fail("missing device client identifier");
        send_response(fd, 200u, "application/json",
                      "{\"device_auth_id\":\"device-id\",\"user_code\":\"CODE-1234\",\"interval\":\"1\"}");
        (void)close(fd);
        /* Token polling repeats until the client stops: a loaded host may poll
         * more times than the minimum, and a fixture that exited after a fixed
         * count would leave the client waiting out its whole link lifetime. */
        for (unsigned int polls = 0u;;) {
            struct pollfd waiting = {.fd = listen_fd, .events = POLLIN};
            const char *body = tokens;
            unsigned int status = 200u;
            if (poll(&waiting, 1, 30000) <= 0) _exit(0);
            fd = accept(listen_fd, NULL, NULL);
            if (fd < 0) server_fail("auth accept failed");
            read_request(fd, &request);
            if (strcmp(request.path, "/api/accounts/deviceauth/token") == 0) {
                ++polls;
                (void)polls;
                /* The success case grants on the first poll so it cannot depend
                 * on the client's poll interval; the expired mode always
                 * answers 410 and the cancel mode keeps answering pending so
                 * the client keeps polling until its own cancel fires. */
                if (fixture == MODEL_AUTH_EXPIRED) {
                    status = 410u;
                    body = "{}";
                } else if (fixture == MODEL_AUTH_CANCEL) {
                    status = 403u;
                    body = "{}";
                } else {
                    body = "{\"authorization_code\":\"auth-code\",\"code_verifier\":\"verifier\",\"code_challenge\":\"challenge\"}";
                }
            } else if (strcmp(request.path, "/oauth/token") == 0) {
                if (!strstr(request.body, "grant_type=authorization_code") ||
                    !strstr(request.body, "code_verifier=verifier"))
                    server_fail("missing PKCE code exchange");
                send_response(fd, status, "application/json", body);
                (void)close(fd);
                _exit(0);
            } else {
                /* Tolerant like the Meta fixture: answer and keep serving. */
                send_response(fd, 404u, "application/json", "{}");
                (void)close(fd);
                continue;
            }
            send_response(fd, status, "application/json", body);
            (void)close(fd);
        }
    }
    for (unsigned int i = 0; fixture > MODEL_AUTH_EXPIRED && i < count; ++i) {
        struct http_request request;
        const char *body = tokens;
        unsigned int status = 200u;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) server_fail("auth accept failed");
        read_request(fd, &request);
        if (fixture >= MODEL_AUTH_401 && i != 1u) {
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
                !strstr(request.body, "old-refresh")) server_fail("wrong refresh request");
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

static unsigned char fixture_wav[1004];
static const unsigned char *audio_expected = fixture_wav;
static size_t audio_expected_len = sizeof(fixture_wav);

static void
make_fixture_wav(void)
{
    memcpy(fixture_wav, "RIFF", 4u);
    fixture_wav[4] = 0xe4; fixture_wav[5] = 3; /* 996 bytes after RIFF header. */
    memcpy(fixture_wav + 8u, "WAVEfmt ", 8u);
    fixture_wav[16] = 16; fixture_wav[20] = 1; fixture_wav[22] = 2;
    fixture_wav[24] = 0xc0; fixture_wav[25] = 0x5d; /* 24000 Hz. */
    fixture_wav[28] = 0; fixture_wav[29] = 0x77; fixture_wav[30] = 1; /* 96000 B/s. */
    fixture_wav[32] = 4; fixture_wav[34] = 16;
    memcpy(fixture_wav + 36u, "data", 4u);
    fixture_wav[40] = 0xc0; fixture_wav[41] = 3;
}

static void
audio_server_child(int listen_fd, enum model_fixture mode)
{
    struct http_request req;
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) server_fail("audio accept failed");
    read_request(fd, &req);
    if (strcmp(req.method, "POST")) server_fail("audio method mismatch");
    if (mode == MODEL_AUDIO_TRANSCRIBE) {
        if (strcmp(req.path, "/v1/audio/transcriptions") ||
            !strstr(req.headers, "Content-Type: multipart/form-data; boundary=") ||
            !strstr(req.body, "name=\"file\"") || !strstr(req.body, "filename=\"segment.wav\""))
            server_fail("audio multipart path/header mismatch");
        /* The WAV contains NUL bytes, so inspect the complete binary body. */
        bool found = false, model = false;
        for (size_t i = 0; i < req.body_len; ++i) {
            if (i + sizeof(fixture_wav) <= req.body_len && !memcmp(req.body + i, fixture_wav, sizeof(fixture_wav))) found = true;
            if (i + 13u <= req.body_len && !memcmp(req.body + i, "audio-fixture", 13u)) model = true;
        }
        if (!found || !model) server_fail("multipart WAV/model missing");
        send_response(fd,200u,"application/json", "{\"text\":\"fixture transcript\",\"usage\":{\"type\":\"duration\",\"seconds\":0.01}}");
    } else {
        json_t *body = json_loadb(req.body, req.body_len, JSON_REJECT_DUPLICATES, NULL);
        const char *model = snag_json_string(body, "model");
        if (!body || !model || strcmp(model, "audio-fixture") || json_object_get(body, "tools"))
            server_fail("audio request must be one model without tools");
        if (mode == MODEL_AUDIO_SPEAK) {
            if (strcmp(req.path, "/v1/audio/speech") || strcmp(snag_json_string(body, "response_format"), "wav"))
                server_fail("speech path/format mismatch");
            char header[160];
            int n = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", sizeof(fixture_wav));
            write_all_or_die(fd, header, (size_t)n);
            write_all_or_die(fd, (char *)fixture_wav, sizeof(fixture_wav));
        } else {
            if (strcmp(req.path, "/v1/chat/completions")) server_fail("audio chat path mismatch");
            json_t *messages = json_object_get(body, "messages");
            if (!messages) server_fail("audio chat messages missing");
            if (messages) {
                json_t *user = json_array_get(messages, 1u);
                json_t *audio = json_object_get(json_array_get(json_object_get(user, "content"), 1u), "input_audio");
                struct snag_buf decoded;
                snag_buf_init(&decoded, 32768u);
                const char *base64 = snag_json_string(audio, "data");
                if (json_array_size(messages) != 2u || !base64 ||
                    snag_base64_decode(&decoded, base64) < 0 || decoded.len != audio_expected_len ||
                    memcmp(decoded.data, audio_expected, audio_expected_len)) server_fail("audio chat lost WAV data");
                snag_buf_free(&decoded);
            }
            send_response(fd,mode == MODEL_AUDIO_FAILURE ? 503u : 200u,"application/json",
                mode == MODEL_AUDIO_FAILURE ? "{\"error\":\"private audio diagnostic\"}" :
                "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"fixture sound answer\"}}],"
                "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":4,\"total_tokens\":14}}");
        }
        json_decref(body);
    }
    (void)close(fd);
    _exit(0);
}

static void
server_child(int listen_fd, enum model_fixture models, bool transport)
{
    (void)signal(SIGTERM, fixture_stop);
    if (models >= MODEL_NATIVE_TRANSCRIBE && models <= MODEL_NATIVE_CALL_PARTIAL) {
        struct http_request request;int fd=accept(listen_fd,NULL,NULL);
        if (fd<0)server_fail("native voice accept failed");
        read_request(fd,&request);
        if (strcmp(request.method,"POST"))server_fail("native voice method");
        if (!strstr(request.headers, expected_authorization_header)) {
            server_fail("native voice authorization");
        }
        if (models==MODEL_NATIVE_TRANSCRIBE) {
            if (strcmp(request.path,"/backend-api/transcribe") ||
                !strstr(request.body,"name=\"file\""))
                server_fail("native transcription endpoint");
            send_response(fd,200u,"application/json","{\"text\":\"native transcript\"}");
        } else {
            if (strcmp(request.path,
                "/backend-api/codex/realtime/calls?intent=quicksilver&architecture=avas") ||
                !strstr(request.headers,"openai-alpha: quicksilver=v2") ||
                !strstr(request.body,"\"sdp\"") ||
                !strstr(request.body,"\"session\""))server_fail("native call request");
            if (models==MODEL_NATIVE_CALL_DENIED)
                send_response(fd,403u,"application/json",
                    "{\"error\":{\"message\":\"access denied\"}}");
            else if (models >= MODEL_NATIVE_CALL_TEMPORARY) {
                static const char *const failures[] = {
                    "{\"error\":{\"code\":\"server_error\",\"type\":\"server_error\","
                        "\"message\":\"reflected transport-secret\"}}",
                    "{\"error\":{\"code\":\"context_length_exceeded\",\"type\":\"content_filter\","
                        "\"max_context_tokens\":12345,\"input_tokens\":12346}}",
                    "{\"error\":{\"code\":\"context_length_exceeded\","
                        "\"type\":\"invalid_request_error\","
                        "\"max_context_tokens\":12345,\"input_tokens\":12346}}",
                    "{",
                    "{\"error\":{\"code\":\"server_error\",\"max_context_tokens\":-1}}",
                    "{\"error\":{\"code\":\"server_error\",\"type\":\"server_error\"}}",
                    "",
                    "{\"error\":{\"code\":\"unknown_service_category\"}}",
                    "{\"error\":{\"code\":\"server_error\",\"type\":\"server_error\"}}"
                };
                const char *body = failures[models - MODEL_NATIVE_CALL_TEMPORARY];
                unsigned int status = models == MODEL_NATIVE_CALL_AUTH_ERROR ? 401u :
                    models == MODEL_NATIVE_CALL_EMPTY ? 429u : 503u;
                char header[256];
                int n = snprintf(header, sizeof(header), "HTTP/1.1 %u Failure\r\n"
                    "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                    "Retry-After: 2\r\nConnection: close\r\n\r\n", status,
                    strlen(body) + (models == MODEL_NATIVE_CALL_PARTIAL));
                assert(n > 0 && (size_t)n < sizeof(header));
                write_all_or_die(fd, header, (size_t)n);
                write_all_or_die(fd, body, strlen(body));
            } else {
                static const char answer[]="v=0\r\ns=native fixture\r\n";
                char header[256];int n=snprintf(header,sizeof(header),
                    "HTTP/1.1 201 Created\r\nContent-Type: application/sdp\r\n"
                    "Content-Length: %zu\r\n%sConnection: close\r\n\r\n",
                    sizeof(answer)-1u,
                    models==MODEL_NATIVE_CALL?"Location: /v1/realtime/calls/rtc_native\r\n":"");
                write_all_or_die(fd,header,(size_t)n);write_all_or_die(fd,answer,sizeof(answer)-1u);
            }
        }
        close(fd);close(listen_fd);_exit(0);
    }
    if (models >= MODEL_AUDIO_LISTEN && models <= MODEL_AUDIO_FAILURE)
        audio_server_child(listen_fd, models);
    if (models >= MODEL_AUTH_DEVICE)
        auth_server_child(listen_fd, models);
    if (models == MODEL_COMPACT_404 || models == MODEL_COMPACT_403 || models == MODEL_COMPACT_502 ||
        models == MODEL_COMPACT_OK) {
        /* The native compaction probe authenticates with its own credential and
         * sends no OpenRouter headers, so those transport checks do not apply. */
        expected_authorization_header = NULL;
        fixture_requires_openrouter = false;
        /* Keep answering until the test stops us: the client retries a failed
         * compact request, and a fixture that exited after the first answer
         * left the retry in the queue of a listener nobody accepts on, which
         * parked the suite in poll until it was killed. */
        for (;;) {
            struct http_request request;
            int fd = accept(listen_fd, NULL, NULL);
            if (fd < 0) server_fail("compact accept failed");
            read_request(fd, &request);
            if (strcmp(request.method, "POST") ||
                (strcmp(request.path, "/responses/compact") && strcmp(request.path, "/v1/responses/compact")))
                server_fail("invalid native compact path");
            send_response(fd, models == MODEL_COMPACT_OK ? 200u :
                              models == MODEL_COMPACT_404 ? 404u :
                              models == MODEL_COMPACT_502 ? 502u : 403u,
                          "application/json", models == MODEL_COMPACT_OK ?
                              "{\"object\":\"response.compaction\",\"output\":[]}" : "{\"detail\":\"Not Found\"}");
            (void)close(fd);
        }
    }
    if (models == MODEL_CREATE_TYPELESS) {
        serve_one(listen_fd, 200u, "POST", "/v1/responses", NULL, "text/event-stream",
                  "data: {\"kind\":\"private-value\"}\n\n");
        _exit(0);
    }
    static const char create_sse[] = "event: response.created\n"
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
        "data: {\"type\":\"response.completed\","
        "\"response\":{\"id\":\"resp_transport\",\"status\":\"completed\","
        "\"usage\":{\"input_tokens\":7,\"input_tokens_details\":{\"cached_tokens\":4},"
        "\"output_tokens\":2,\"total_tokens\":9},\"output\":[]}}\n\n";

    if (models == MODEL_VOICE_BACKLOG) serve_voice_backlog(listen_fd, create_sse);
    if (models >= MODEL_VOICE_NATIVE_SUMMARY && models <= MODEL_VOICE_NATIVE_HANDOFF) {
        serve_native_summary(listen_fd, models);
    }
    if (models == MODEL_VOICE_QUEUE || models == MODEL_VOICE_READ ||
        models == MODEL_VOICE_CLOSE || models == MODEL_VOICE_CLOSE_SUMMARY ||
        models == MODEL_VOICE_CLOSE_CREDENTIAL || models == MODEL_VOICE_CLOSE_REFRESH ||
        models == MODEL_VOICE_COMPACT || models == MODEL_VOICE_COMPACT_RETRY ||
        models == MODEL_VOICE_COMPACT_TOOL || models == MODEL_VOICE_COMPACT_ACTIVE) {
        bool retry = models == MODEL_VOICE_COMPACT_RETRY;
        bool bad_summary = models == MODEL_VOICE_COMPACT_TOOL;
        bool active = models == MODEL_VOICE_COMPACT_ACTIVE || models == MODEL_VOICE_CLOSE_SUMMARY;
        bool rotated = models == MODEL_VOICE_CLOSE_CREDENTIAL;
        bool refreshed = models == MODEL_VOICE_CLOSE_REFRESH;
        bool closing = models == MODEL_VOICE_CLOSE ||
            models == MODEL_VOICE_CLOSE_SUMMARY || rotated || refreshed;
        bool compact = models == MODEL_VOICE_COMPACT || retry || bad_summary || active;
        unsigned int rejected_request = active ? 1u : 2u;
        unsigned int summary_request = rejected_request + 1u;
        bool queue = models != MODEL_VOICE_READ;
        unsigned int count = retry ? 8u : compact && !bad_summary ? 6u : queue ? 4u : 3u;
        if (models == MODEL_VOICE_QUEUE) ++count;
        if (closing) count = active ? 3u : 2u;
        char affinity[SNAG_CACHE_KEY_LEN + 1u] = {0};
        struct voice_summary_fixture summary = {0};
        /* Retained control timelines exceed the small fixture request buffer. */
        char *large = models == MODEL_VOICE_QUEUE ? malloc(SNAG_CONTEXT_MAX_REQUEST + 1u) : NULL;
        if (models == MODEL_VOICE_QUEUE && !large) server_fail("cannot retain voice history");
        for (unsigned int i = 0; i < count; ++i) {
            int fd = accept(listen_fd, NULL, NULL);
            struct http_request request;
            char *wire = large ? large : request.body;
            if (fd < 0) server_fail("voice interface accept failed");
            if (rotated) expected_authorization_header = i ?
                "Authorization: Bearer transport-secret" : "Authorization: Bearer old-voice-secret";
            if (refreshed) expected_authorization_header = "Authorization: Bearer old-voice-secret";
            read_request_body(fd, &request, wire,
                large ? SNAG_CONTEXT_MAX_REQUEST + 1u : sizeof(request.body));
            if (refreshed && i == 1u) {
                if (voice_close_release() != 'N') server_fail("voice refresh barrier failed");
                send_response(fd, 401u, "application/json", "{\"error\":\"expired token\"}");
                close(fd);
                fd = accept(listen_fd, NULL, NULL);
                if (fd < 0) server_fail("voice refresh retry accept failed");
                expected_authorization_header = "Authorization: Bearer refreshed-voice-secret";
                struct http_request retried;
                read_request(fd, &retried);
                if (strcmp(request.method, retried.method) || strcmp(request.path, retried.path) ||
                    strcmp(wire, retried.body)) server_fail("voice refresh changed the request");
            }
            if (strcmp(request.method, "POST") || strcmp(request.path, "/v1/responses")) {
                server_fail("unexpected voice request route");
            }
            if (strstr(wire, "transport-secret")) {
                server_fail("retained interface history exposed a configured credential");
            }
            bool summarizing = compact && (i == summary_request || (retry && i == 5u));
            if (!summarizing && (!strstr(wire, "inspect_session") ||
                !strstr(wire, "get_cwd") || !strstr(wire, "Keyboard") ||
                !strstr(wire, "/session") || !strstr(wire, "/voice"))) {
                server_fail("missing voice capabilities or CLI help");
            }
            json_t *body = json_loads(wire, JSON_REJECT_DUPLICATES, NULL);
            const char *key = snag_json_string(body, "prompt_cache_key");
            char header[128];
            if (!key || strlen(key) != SNAG_CACHE_KEY_LEN ||
                snprintf(header, sizeof(header), "session_id: %s", key) <= 0 ||
                !header_contains(request.headers, header) ||
                (i && strcmp(affinity, key))) {
                server_fail("voice transport did not retain its independent cache affinity");
            }
            strcpy(affinity, key);
            if (models == MODEL_VOICE_QUEUE) {
                unsigned int failures = 0u;
                unsigned int outputs = 0u;
                unsigned int spoken = 0u;
                unsigned int transitions[8] = {0};
                uint64_t prior_seq = 0u;
                json_t *input = json_object_get(body, "input");
                for (size_t j = 0u; j < json_array_size(input); ++j) {
                    const char *content = snag_json_string(json_array_get(input, j), "content");
                    json_t *event = content ? json_loads(content, 0, NULL) : NULL;
                    const char *kind = snag_json_string(event, "kind");
                    const char *type = snag_json_string(event, "event_type");
                    if (kind && !strcmp(kind, "session_observation")) {
                        uint64_t seq = (uint64_t)json_integer_value(json_object_get(event, "seq"));
                        if (seq <= prior_seq) {
                            server_fail("interface replayed a working observation");
                        }
                        prior_seq = seq;
                        for (size_t k = 0u; k < 8u; ++k) {
                            if (type && !strcmp(type, voice_transition_types[k])) ++transitions[k];
                        }
                        if (type && !strcmp(type, "response_output")) {
                            const json_t *data = json_object_get(event, "data");
                            const char *text = snag_json_string(json_object_get(data, "item"),
                                "text");
                            if (!text || !strstr(text, outputs ? "new working outcome" :
                                    "initial working outcome")) {
                                server_fail("interface lost actual working narration");
                            }
                            ++outputs;
                        }
                    }
                    if (kind && type && !strcmp(kind, "session_observation") &&
                        !strcmp(type, "turn_failed")) {
                        const char *message = snag_json_string(json_object_get(event, "data"),
                            "message");
                        if (!message || strcmp(message, failures ? "new working outcome" :
                                "initial working outcome")) {
                            server_fail("interface working outcomes lost their source order");
                        }
                        ++failures;
                    }
                    if (kind && type && !strcmp(kind, "session_observation") &&
                        !strcmp(type, "voice_event")) {
                        const json_t *record = json_object_get(
                            json_object_get(event, "data"), "event");
                        const char *record_type = snag_json_string(record, "type");
                        if (record_type && !strcmp(record_type, "voice_transcript")) ++spoken;
                    }
                    json_decref(event);
                }
                if (failures != (i < 2u ? 1u : 2u) || outputs != failures) {
                    server_fail("interface omitted initial or newly completed working history");
                }
                if (spoken != (i < 2u ? 2u : 3u)) {
                    server_fail("interface omitted live native dialogue before a request");
                }
                for (size_t k = 0u; k < 8u; ++k) {
                    if (transitions[k] != (k >= 6u ? 2u : 1u) * failures) {
                        server_fail("interface omitted a control, goal or model transition");
                    }
                }
            }
            if (compact && (i == rejected_request || (retry && i == 4u))) {
                if (retry && i == 4u) {
                    json_decref(summary.expected);
                    snag_buf_free(&summary.record);
                    free(summary.result);
                    memset(&summary, 0, sizeof(summary));
                }
                send_response(fd, 400u, "application/json",
                    "{\"error\":{\"type\":\"invalid_request_error\","
                    "\"code\":\"context_length_exceeded\",\"message\":\"context exhausted\"}}");
                json_decref(body);
                close(fd);
                continue;
            }
            if (rotated && !i && voice_close_release() != 'N') {
                server_fail("voice credential rotation barrier failed");
            }
            if (closing && i + 1u == count) {
                char action = voice_close_release();
                close(voice_request_ready_fd);
                close(voice_request_release_fd);
                if (action == 'X') {
                    json_decref(body);
                    close(fd);
                    break;
                }
                if (action == 'C') {
                    send_voice_item(fd, json_pack("{s:s,s:i,s:{s:s,s:s,s:s,s:s,s:s,s:s}}",
                        "type", "response.output_item.done", "output_index", 0,
                        "item", "type", "function_call", "id", "discarded_function",
                        "call_id", "discarded_call", "name", "submit_input",
                        "arguments", "{\"target\":\"queue\","
                            "\"text\":\"discarded transport-secret\"}",
                        "status", "completed"));
                    json_decref(body);
                    close(fd);
                    continue;
                }
                if (action != 'T') server_fail("invalid voice close barrier action");
                if (summarizing) {
                    send_response(fd, 200u, "text/event-stream", create_sse);
                    json_decref(body);
                    close(fd);
                    continue;
                }
            }
            if (summarizing) {
                if (bad_summary) {
                    send_voice_item(fd, json_pack("{s:s,s:i,s:{s:s,s:s,s:s,s:s,s:s,s:s}}",
                        "type", "response.output_item.done", "output_index", 0,
                        "item", "type", "function_call", "id", "forbidden_summary_action",
                        "call_id", "summary_must_not_run", "name", "submit_input",
                        "arguments", "{\"target\":\"queue\",\"text\":\"must not be submitted\"}",
                        "status", "completed"));
                } else if (!serve_voice_summary(fd, body, retry && i == summary_request,
                        retry && i == 5u, &summary)) --i;
                json_decref(body);
                close(fd);
                continue;
            }
            if (compact && i > summary_request && (!strstr(wire, "interface_compacted") ||
                    !strstr(wire, "retained-first-utterance") ||
                    !strstr(wire, "voice_call_first"))) {
                server_fail("compaction lost dialogue, action identity or current delegation");
            }
            if (((active && i == 3u) || (retry && i == 6u)) &&
                !strstr(wire, "interface_continuation")) {
                server_fail("whole-history compaction replayed speech as a new user request");
            }
            if (models == MODEL_VOICE_QUEUE && i == 4u) {
                unsigned int requests = 0u, outputs = 0u, results = 0u, cancelled = 0u;
                unsigned int transcripts = 0u, queued = 0u;
                char identities[2][SNAG_ID_HEX_LEN + 1u];
                json_t *input = json_object_get(body, "input");
                for (size_t j = 0u; j < json_array_size(input); ++j) {
                    const char *text = snag_json_string(json_array_get(input, j), "content");
                    json_t *quote = text ? json_loads(text, JSON_REJECT_DUPLICATES, NULL) : NULL;
                    const char *event_type = snag_json_string(quote, "event_type");
                    json_t *data = json_object_get(quote, "data");
                    json_t *event = json_object_get(data, "event");
                    const char *type = snag_json_string(event, "type");
                    const char *operation = snag_json_string(event, "operation");
                    if (event_type && !strcmp(event_type, "future_turn_queued")) {
                        const char *id = snag_json_string(data, "queue_id");
                        if (queued >= 2u || !id || strlen(id) != SNAG_ID_HEX_LEN) {
                            server_fail("restored queue identity missing or duplicated");
                        }
                        strcpy(identities[queued++], id);
                    }
                    if (type && !strcmp(type, "voice_transcript")) {
                        const char *speaker = snag_json_string(event, "speaker");
                        const char *words = snag_json_string(event, "text");
                        const char *expected[] = {"native correction", "native acknowledgement",
                            "native follow-up discussion"};
                        if (transcripts >= 3u || !speaker || !words ||
                            strcmp(speaker, transcripts == 1u ? "assistant" : "user") ||
                            !strstr(words, expected[transcripts])) {
                            server_fail("restored transcript lost source speaker or order");
                        }
                        ++transcripts;
                    }
                    if (operation && !strcmp(operation, "interface_request")) ++requests;
                    if (operation && !strcmp(operation, "interface_output")) ++outputs;
                    if (operation && !strcmp(operation, "interface_tool")) ++results;
                    if (type && !strcmp(type, "voice_result") &&
                        !strcmp(snag_json_string(event, "status"), "cancelled")) {
                        const char *id = snag_json_string(event, "queue_id");
                        if (cancelled >= queued || !id || strcmp(id, identities[cancelled])) {
                            server_fail("restored outcome changed its queue identity");
                        }
                        ++cancelled;
                    }
                    json_decref(quote);
                }
                if (requests != 2u || outputs != 2u || results != 2u || cancelled != 2u ||
                    transcripts != 3u || queued != 2u) {
                    server_fail("reopened interface lost prior dialogue or actual outcomes");
                }
                send_response(fd, 200u, "text/event-stream", create_sse);
                json_decref(body);
                close(fd);
                continue;
            }
            if (models == MODEL_VOICE_QUEUE && i >= 2u) {
                json_t *input = json_object_get(body, "input");
                bool prior_user = false;
                bool prior_answer = false;
                bool prior_result = false;
                bool current_user = false;
                for (size_t j = 0; j < json_array_size(input); ++j) {
                    json_t *item = json_array_get(input, j);
                    const char *role = snag_json_string(item, "role");
                    const char *text = snag_json_string(item, "content");
                    const char *type = snag_json_string(item, "type");
                    const char *call = snag_json_string(item, "call_id");
                    if (role && !strcmp(role, "user") && text) {
                        if (strstr(text, "retained-first-utterance")) prior_user = true;
                        if (strstr(text, "second") && prior_user && prior_answer) {
                            current_user = true;
                        }
                    }
                    if (role && !strcmp(role, "assistant") && prior_user) prior_answer = true;
                    if (type && !strcmp(type, "function_call_output") && call &&
                        !strcmp(call, "voice_call_first")) prior_result = true;
                }
                if (!prior_user || !prior_answer || !prior_result || !current_user) {
                    server_fail("voice interface discarded prior dialogue or tool outcome");
                }
            }
            if (queue && i % 2u) {
                json_t *input = json_object_get(body, "input");
                size_t pending = SIZE_MAX;
                for (size_t j = 0; j < json_array_size(input); ++j) {
                    const char *text = snag_json_string(json_array_get(input, j), "content");
                    json_t *snapshot = text ? json_loads(text, JSON_REJECT_DUPLICATES, NULL) : NULL;
                    json_t *entries = json_object_get(snapshot, "queue");
                    if (json_is_array(entries)) pending = json_array_size(entries);
                    json_decref(snapshot);
                }
                if (pending != (i == 1u || (active && i == 3u) ? 1u : 2u)) {
                    server_fail("voice continuation used stale state after its tool action");
                }
            }
            json_decref(body);
            bool final = queue ? i % 2u : i == 2u;
            if (final) {
                if (!(active && i == 3u) && !strstr(wire, "function_call_output")) {
                    server_fail("missing voice tool result");
                }
                if (models == MODEL_VOICE_READ && !strstr(wire, "unavailable")) {
                    server_fail("write tool was not refused");
                }
                if ((models == MODEL_VOICE_QUEUE || closing) && i == 1u) {
                    struct snag_buf reflected = {.max = BODY_MAX};
                    const char *cursor = create_sse;
                    const char *match;
                    while ((match = strstr(cursor, " transport\""))) {
                        size_t bytes = (size_t)(match - cursor) + strlen(" transport");
                        if (snag_buf_append(&reflected, cursor, bytes) < 0 ||
                            snag_buf_printf(&reflected, "%s", rotated ?
                                " old-voice-secret transport-secret" : refreshed ?
                                " old-voice-secret refreshed-voice-secret" :
                                " transport-secret") < 0) {
                            server_fail("cannot build reflected-credential response");
                        }
                        cursor += bytes;
                    }
                    if (snag_buf_printf(&reflected, "%s", cursor) < 0 ||
                        snag_buf_terminate(&reflected) < 0) {
                        server_fail("cannot finish reflected-credential response");
                    }
                    send_response(fd, 200u, "text/event-stream", (char *)reflected.data);
                    snag_buf_free(&reflected);
                } else {
                    send_response(fd, 200u, "text/event-stream", create_sse);
                }
            } else {
                const char *name = queue ? "submit_input" :
                    i ? "write_file" : "get_cwd";
                const char *arguments = queue ?
                    "{\"target\":\"queue\",\"text\":\"model interpretation\"}" :
                    i ? "{\"path\":\"AGENTS.md\",\"content\":\"must not write\"}" : "{}";
                json_t *event = json_pack("{s:s,s:i,s:{s:s,s:s,s:s,s:s,s:s,s:s}}",
                    "type", "response.output_item.done", "output_index", 0,
                    "item", "type", "function_call", "id", "voice_function",
                    "call_id", i ? "voice_call_next" : "voice_call_first",
                    "name", name, "arguments", arguments, "status", "completed");
                send_voice_item(fd, event);
            }
            close(fd);
        }
        json_decref(summary.expected);
        free(summary.result);
        snag_buf_free(&summary.record);
        free(large);
        _exit(0);
    }
    if (models == MODEL_VOICE_REQUEST || models == MODEL_VOICE_REQUEST_WAIT) {
        struct http_request request;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) server_fail("voice interface accept failed");
        read_request(fd, &request);
        if (strcmp(request.method, "POST") || strcmp(request.path, "/v1/responses") ||
            !strstr(request.body, "get_cwd")) server_fail("invalid voice interface request");
        if (voice_request_ready_fd >= 0) {
            if (write(voice_request_ready_fd, "R", 1u) != 1) {
                server_fail("cannot acknowledge voice request");
            }
            close(voice_request_ready_fd);
        }
        snag_sleep_ms(models == MODEL_VOICE_REQUEST_WAIT ? 2000u : 250u);
        send_response(fd, 200u, "text/event-stream", create_sse);
        close(fd);
        _exit(0);
    }
    if (models == MODEL_OPENROUTER_SEARCH) {
        serve_one(listen_fd, 200u, "POST", "/v1/responses", "openrouter:web_search", "text/event-stream",
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
            if (fd < 0) server_fail("retry accept failed");
            read_request(fd, &request);
            if (strcmp(request.method, "POST") || strcmp(request.path, "/v1/responses"))
                server_fail("unexpected retry request");
            if (i == 0) memcpy(first, request.body, request.body_len + 1u);
            else if (strcmp(first, request.body)) server_fail("retry changed request bytes");
            if (i == retry_case->failures) {
                send_response(fd, 200u, "text/event-stream", create_sse);
            } else {
                int n = snprintf(body, sizeof(body), "%s%s", retry_case->prefix, retry_case->body);
                if (n < 0 || (size_t)n >= sizeof(body)) server_fail("retry fixture overflow");
                if (retry_case->truncated) {
                    char header[256];
                    int h = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                        "Content-Length: %u\r\nConnection: close\r\n\r\n", (unsigned int)n + 100u);
                    assert(h > 0 && (size_t)h < sizeof(header));
                    write_all_or_die(fd, header, (size_t)h);
                    write_all_or_die(fd, body, (size_t)n);
                } else if (retry_case->status == 200u) {
                    send_response(fd, 200u, "text/event-stream", body);
                } else {
                    send_response(fd, retry_case->status, "application/json", body);
                }
            }
            if (close(fd) < 0) server_fail("close retry socket failed");
        }
        _exit(0);
    }
    if (models >= MODEL_COUNT_404) {
        unsigned int status = models == MODEL_COUNT_404 ? 404u : models == MODEL_COUNT_405 ? 405u :
                              models == MODEL_COUNT_401 ? 401u : models == MODEL_COUNT_403 ? 403u :
                              models == MODEL_COUNT_OK ? 200u : models == MODEL_COUNT_MODEL_404 ? 404u :
                              models == MODEL_COUNT_OVERFLOW ? 400u : 501u;
        const char *body = models == MODEL_COUNT_OK ?
            "{\"object\":\"response.input_tokens\",\"input_tokens\":42}" : models == MODEL_COUNT_MODEL_404 ?
            "{\"error\":{\"code\":\"model_not_found\",\"message\":\"no such model\"}}" :
            models == MODEL_COUNT_OVERFLOW ?
            "{\"error\":{\"code\":400,\"type\":\"exceed_context_size_error\",\"n_ctx\":10,\"n_prompt_tokens\":12}}" :
            "{\"error\":{\"message\":\"not available\"}}";
        serve_one(listen_fd, status, "POST", "/v1/responses/input_tokens", NULL, "application/json", body);
        _exit(0);
    }
    static const struct {
        enum model_fixture fixture;
        unsigned int status;
        const char *method, *path, *content_type, *body;
    } replies[] = {
        {MODEL_CREATE_HTTP_FAILURE, 400u, "POST", "/v1/responses", "application/json",
         "{\"error\":{\"code\":\"context_length_exceeded\","
         "\"message\":\"too large\",\"max_context_tokens\":272000," "\"requested_input_tokens\":300000}}"},
        {MODEL_CREATE_SSE_FAILURE, 200u, "POST", "/v1/responses", "text/event-stream",
         "event: response.failed\n" "data: {\"type\":\"response.failed\",\"response\":{"
         "\"error\":{\"code\":\"context_length_exceeded\","
         "\"message\":\"stream too large transport-secret\"," "\"context_length\":872000}}}\n\n"},
        {MODEL_CODEX_FAILURE, 400u, "GET", "/backend-api/codex/models?client_version=0.146.0", "application/json",
         "{\"error\":{\"message\":\"catalog rejected\"}}"},
        {MODEL_CODEX_MALFORMED, 200u, "GET", "/backend-api/codex/models?client_version=0.146.0", "application/json",
         "{\"models\":[{\"slug\":\"malformed\",\"visibility\":\"list\",\"priority\":1,\"supported_reasoning_levels\":[\"high\"]}]}"},
        {MODEL_CODEX_LOOKALIKE, 200u, "GET", "/backend-api/codexish/v1/models", "application/json",
         "{\"data\":[{\"id\":\"lookalike-openai\"}]}"},
        {MODEL_LIMIT_CONFLICT, 200u, "GET", "/v1/models", "application/json",
         "{\"data\":[{\"id\":\"conflict\",\"context_length\":100," "\"metadata\":{\"contextWindow\":101}}]}"},
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
        if (replies[i].fixture == models) reply = i;
    serve_one(listen_fd, replies[reply].status, replies[reply].method,
              replies[reply].path, NULL, replies[reply].content_type, replies[reply].body);
    if (!transport || models == MODEL_CREATE_HTTP_FAILURE ||
        models == MODEL_CREATE_SSE_FAILURE || models == MODEL_CODEX_FAILURE) _exit(0);
    serve_one(listen_fd, 200u, "POST", "/v1/responses/input_tokens", "transport-count", "application/json",
              "{\"object\":\"response.input_tokens\",\"input_tokens\":7}");
    serve_one(listen_fd, 200u, "POST", "/v1/responses", "transport-create", "text/event-stream", create_sse);
    serve_one(listen_fd, 200u, "POST", "/v1/responses/compact", "transport-compact", "application/json",
              "{\"object\":\"response.compaction\",\"output\":[{\"type\":\"compaction\",\"encrypted_content\":\"transport-compact-output\"}]}");
    _exit(0);
}

static void
start_server(struct local_server *server, enum model_fixture models, bool transport, const char *suffix)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int one = 1;

    memset(server, 0, sizeof(*server));
    server->fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(server->fd >= 0);
    assert(setsockopt(server->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
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
    if (server->pid == 0) server_child(server->fd, models, transport);
    /* The fixture owns the listener. Closing the parent's copy means a fixture
     * that dies cannot leave a client waiting on a queued connection nobody
     * accepts on: the peer sees a reset and the test fails instead of parking. */
    assert(close(server->fd) == 0);
    server->fd = -1;
}

static void
stop_server(struct local_server *server)
{
    int status;

    if (server->fd >= 0) assert(close(server->fd) == 0);
    /* Fixtures keep serving until told, so ask this one to stop; the handler
     * exits with status 0, which keeps the assertions below meaningful. */
    (void)kill(server->pid, SIGTERM);
    assert(waitpid(server->pid, &status, 0) == server->pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

static void
test_http_continue(void)
{
    struct local_server server;
    start_server(&server, MODEL_CREATE_TYPELESS, false, "/v1");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
        .sin_port = htons(server.port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(fd >= 0 && connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    const char headers[] = "POST /v1/responses HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Authorization: Bearer transport-secret\r\n"
        "HTTP-Referer: https://github.com/snajpa/snajpagent\r\n"
        "X-OpenRouter-Title: snajpagent\r\n"
        "User-Agent: " SNAJPAGENT_NAME "/" SNAJPAGENT_VERSION "\r\n"
        "Content-Length: 2\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
    write_all_or_die(fd, headers, sizeof(headers) - 1u);
    const char expected[] = "HTTP/1.1 100 Continue\r\n\r\n";
    char response[512];
    size_t used = 0u;
    struct pollfd readable = {.fd = fd, .events = POLLIN};
    while (used < sizeof(expected) - 1u) {
        assert(poll(&readable, 1u, 1000) == 1);
        ssize_t n = read(fd, response + used, sizeof(expected) - 1u - used);
        assert(n > 0);
        used += (size_t)n;
    }
    assert(!memcmp(response, expected, used));
    /* Body transmission depends on the interim response, never a fallback timer. */
    write_all_or_die(fd, "{}", 2u);
    used = 0u;
    for (;;) {
        assert(used < sizeof(response) - 1u && poll(&readable, 1u, 1000) == 1);
        ssize_t n = read(fd, response + used, sizeof(response) - 1u - used);
        assert(n >= 0);
        if (!n) break;
        used += (size_t)n;
    }
    response[used] = '\0';
    assert(strstr(response, "HTTP/1.1 200 "));
    assert(close(fd) == 0);
    stop_server(&server);
}

static json_t *
request_with_marker(const char *marker)
{
    return checked_json(json_pack("{s:s,s:[{s:s,s:s}]}", "model", "gpt-transport-test",
        "input", "role", "user", "content", marker));
}

static int
emit_capture(void *opaque, size_t item_index, enum snag_item_kind kind,
             enum snag_item_phase phase, const char *provider_item_id, const char *text, size_t len)
{
    struct emitted_text *emitted = opaque;

    (void)item_index;
    (void)kind;
    (void)phase;
    (void)provider_item_id;
    ++emitted->calls;
    return snag_buf_append(&emitted->text, text, len);
}

static int
response_ready(void *opaque)
{
    unsigned int *count = opaque;
    ++*count;
    return 0;
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
    assert(snag_strcpy(provider->openrouter_title, sizeof(provider->openrouter_title), "snajpagent"));
    credential_set(credential, "transport-secret");
}

static struct snag_provider_connection
transport_connection(struct snag_config *config, struct snag_credential *credential, const char *base_url)
{
    snag_config_init(config);
    assert(snag_strcpy(config->providers[0].base_url, sizeof(config->providers[0].base_url), base_url));
    transport_settings(&config->providers[0], credential);
    return (struct snag_provider_connection){config, &config->providers[0], credential, NULL, NULL, NULL, NULL, 0};
}

/* The proxy keys prompt-cache affinity on a session identity from its session lane; without it every
 * request is pinned per-request and reuse collapses. This asserts the header reaches the wire: the fixture
 * aborts the request if the exact line is missing, so the case cannot pass vacuously. */
static void
test_session_identity_header(void)
{
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    struct local_server server;
    struct snag_config config;
    struct snag_credential credential;
    struct emitted_text emitted;
    json_t *request;
    json_t *models = NULL;
    struct snag_response_graph graph = {0};
    uint64_t tokens = 0u;
    unsigned int retries = 99u;
    char error[256] = {0};

    /* Set before the server forks: the fixture child inherits this expectation. */
    expected_session_header = "session_id: 0123456789abcdef0123456789abcdef";
    start_server(&server, MODEL_OPENAI, true, "");
    struct snag_provider_connection connection;
    snag_config_init(&config);
    connection = (struct snag_provider_connection){
        &config, &config.providers[1], &credential, NULL, NULL, NULL, session_id, 0};
    snag_config_provider_init(&config.providers[1], "second");
    config.provider_count = 2u;
    assert(snprintf(config.providers[1].name, sizeof(config.providers[1].name), "transport") > 0);
    assert(snprintf(config.providers[1].base_url, sizeof(config.providers[1].base_url),
                    "%s/v1/", server.endpoint) > 0);
    transport_settings(&config.providers[1], &credential);

    /* Every provider request must carry the session identity: the fixture aborts the request when the exact
     * header line is absent or altered, so the case cannot pass vacuously. The call order mirrors the
     * fixture's canned replies (models, count, create, compact). */
    assert(snag_provider_models_list(connection, &models, error, sizeof(error)) == 0);
    json_decref(models);

    request = request_with_marker("transport-count");
    assert(snag_provider_responses_count(connection, request, &tokens, NULL, error, sizeof(error),
                                         &retries) == 0);
    json_decref(request);

    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 128u);
    request = request_with_marker("transport-create");
    unsigned int ready_count = 0u;
    assert(snag_provider_responses_create(connection, request, emit_capture, &emitted,
        NULL, NULL, response_ready, &ready_count, &graph, NULL, NULL,
                                          error, sizeof(error), &retries) == 0);
    assert(ready_count == 1u);
    json_decref(request);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
    struct snag_json_document compact_output = {0};
    request = request_with_marker("transport-compact");
    assert(snag_provider_responses_compact(connection, request, &compact_output,
                                          error, sizeof(error), &retries) == 0);
    json_decref(request);
    snag_json_document_free(&compact_output);
    snag_credential_clear(&credential);
    snag_config_free(&config);
    stop_server(&server);
    expected_session_header = NULL;
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
    struct snag_provider_connection connection;
    snag_config_init(&config);
    connection = (struct snag_provider_connection){
        &config, &config.providers[1], &credential, NULL, NULL, NULL, NULL, 0};
    snag_config_provider_init(&config.providers[1], "second");
    config.provider_count = 2u;
    assert(snprintf(config.providers[1].name, sizeof(config.providers[1].name), "transport") > 0);
    assert(snprintf(config.providers[1].base_url, sizeof(config.providers[1].base_url),
                    "%s/v1/", server.endpoint) > 0);
    transport_settings(&config.providers[1], &credential);

    assert(snag_provider_models_list(connection, &models, error, sizeof(error)) == 0);
    assert(json_array_size(models) == 2u);
    assert(strcmp(snag_json_string(json_array_get(models, 0), "id"), "gpt-standard") == 0);
    assert(strcmp(json_string_value(json_array_get(json_object_get(
                      json_array_get(models, 0), "efforts"), 1)), "high") == 0);
    assert(strcmp(snag_json_string(json_array_get(models, 0), "default_effort"), "medium") == 0);
    {
        json_t *limits = json_object_get(json_array_get(models, 0), "limits");
        assert(limits);
        assert(json_integer_value(json_object_get( limits, "context_window_tokens")) == 100000);
        assert(json_integer_value(json_object_get( limits, "input_context_window_tokens")) == 90000);
        assert(json_integer_value(json_object_get( limits, "max_output_tokens")) == 10000);
        assert(json_integer_value(json_object_get( limits, "effective_context_window_percent")) == 80);
        assert(json_is_null(json_object_get(limits, "max_input_tokens")));
    }
    {
        json_t *limits = json_object_get(json_array_get(models, 1), "limits");
        assert(limits);
        assert(json_is_null(json_object_get( limits, "context_window_tokens")));
        assert(json_is_null(json_object_get(limits, "max_output_tokens")));
    }
    assert(strcmp(snag_model_best_effort(NULL, NULL, NULL, json_array_get(models, 1), "fallback"), "quantum") == 0);
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
        request, emit_capture, &emitted, NULL, NULL, NULL, NULL, &graph,
        NULL, NULL, error, sizeof(error), &retries) == 0);
    assert(strcmp(graph.provider_response_id, "resp_transport") == 0);
    assert(graph.count == 1u);
    assert(strcmp(snag_response_graph_item(&graph, 0).text, "local transport") == 0);
    assert(emitted.calls == 1u);
    assert(emitted.text.len == strlen("local transport"));
    assert(memcmp(emitted.text.data, "local transport", strlen("local transport")) == 0);
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
        {MODEL_CODEX_LOOKALIKE, "/backend-api/codexish", "codex", NULL, "lookalike-openai", NULL, 1u},
        {MODEL_OPENAI, "", "codex", "http://backend-api/codex", NULL, NULL, 2u},
        {MODEL_CODEX_MALFORMED, "/backend-api/codex", "codex", NULL, NULL, "invalid model entry", 0u},
        {MODEL_CODEX_FAILURE, "/backend-api/codex", "neutral", NULL, NULL, "catalog rejected", 0u},
        {MODEL_LIMIT_CONFLICT, "", "neutral", NULL, NULL, "invalid model entry", 0u}
    };
    struct local_server server;
    struct snag_config config;
    struct snag_credential credential;
    char error[256] = {0};
    struct snag_provider_connection connection;

    snag_config_init(&config);
    connection = (struct snag_provider_connection){
        &config, &config.providers[0], &credential, NULL, NULL, NULL, NULL, 0};
    transport_settings(&config.providers[0], &credential);
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        json_t *models = NULL;
        start_server(&server, cases[i].fixture, false, cases[i].path);
        assert(snag_strcpy(config.providers[0].name, sizeof(config.providers[0].name), cases[i].provider));
        assert(snag_strcpy(config.providers[0].base_url, sizeof(config.providers[0].base_url),
                           cases[i].base_override ? cases[i].base_override : server.endpoint));
        if (cases[i].base_override) assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", server.endpoint, 1) == 0);
        int rc = snag_provider_models_list(connection, &models, error, sizeof(error));
        if (cases[i].diagnostic) {
            assert(rc < 0 && models == NULL);
            assert(strstr(error, cases[i].diagnostic));
        } else {
            assert(rc == 0 && json_array_size(models) == cases[i].count);
            if (cases[i].id) assert(!strcmp(snag_json_string(json_array_get(models, 0), "id"), cases[i].id));
        }
        json_decref(models);
        if (cases[i].base_override) assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
        stop_server(&server);
    }
    snag_config_free(&config);
}

static void
test_structured_create_failures(void)
{
    const enum model_fixture fixtures[] = {
        MODEL_CREATE_HTTP_FAILURE, MODEL_CREATE_SSE_FAILURE };

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
            request, NULL, NULL, NULL, NULL, NULL, NULL, &graph,
            &failure, NULL, error, sizeof(error), NULL) < 0);
        assert(snag_provider_failure_is_capacity(&failure));
        assert(!strstr(error, "transport-secret"));
        assert(!strstr(failure.message, "transport-secret"));
        assert(failure.context_limit_tokens == (fixtures[i] == MODEL_CREATE_HTTP_FAILURE ?
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

static void
test_typeless_create_diagnostic(void)
{
    struct local_server server;
    struct snag_config config;
    struct snag_credential credential;
    json_t *request = request_with_marker("typeless-diagnostic");
    char error[1024] = {0};
    struct snag_response_graph graph = {0};

    start_server(&server, MODEL_CREATE_TYPELESS, false, "/v1");
    assert(snag_provider_responses_create(transport_connection(&config, &credential, server.endpoint),
        request, NULL, NULL, NULL, NULL, NULL, NULL, &graph, NULL, NULL,
        error, sizeof(error), NULL) < 0);
    assert(strstr(error, "Responses event has no type") != NULL);
    assert(strstr(error, "Responses record diagnostic: event=-") != NULL);
    assert(strstr(error, "json=object") != NULL);
    assert(strstr(error, "keys=kind") != NULL);
    assert(strstr(error, "private-value") == NULL);
    snag_response_graph_free(&graph);
    json_decref(request);
    snag_config_free(&config);
    stop_server(&server);
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
    static const char created[] = ": heartbeat\n\n" "id: discarded-event\n"
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
        {"", "{\"error\":{\"type\":\"insufficient_quota\"}}", 429, 1, 0, false, "insufficient_quota", ""},
        {"", "{\"error\":{\"code\":\"cyber_policy\"}}", 500, 1, 0, false, "cyber_policy", ""},
        {"", "{\"error\":{\"code\":\"unknown\",\"type\":\"server_error\"}}", 503, 1, 0, false, "unknown", ""},
        {"", "{\"error\":{\"code\":\"upstream_error\",\"type\":\"server_error\",\"message\":\"{\\\"detail\\\":\\\"Unable to verify provider access. Please try again.\\\"}\"}}",
            503, 3, 2, false, "retried 2 times", ""},
        {"", "{\"error\":{\"code\":23,\"type\":\"server_error\"}}", 500, 1, 0, false, "HTTP 500", ""},
        {"", "{\"error\":{\"code\":\"server_error\"}}", 503, 3, 2, false, "retried 2 times", ""},
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
            200, 1, 0, false, "invalid function call snapshot", ""},
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
            cancellation.code ? cancel_retry : NULL, &cancellation, NULL, 0},
            request, emit_capture, &emitted, NULL, NULL, NULL, NULL, &graph,
            &failure, NULL, error, sizeof(error), &retries);
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
        if (emitted.text.len) assert(memcmp(emitted.text.data, retry_case->emitted, emitted.text.len) == 0);
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
        200, 1, 0, false, "[cyber_policy]", "" };
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
        &config, &config.providers[0], &credential, NULL, NULL, NULL, NULL, 0},
        request, emit_capture, &emitted, NULL, NULL, NULL, NULL, &graph,
            &failure, NULL, error, sizeof(error), &retries);
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
        MODEL_COUNT_404, MODEL_COUNT_405, MODEL_COUNT_501 };
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
        bool openrouter, images;
    } cases[] = {
        {MODEL_COUNT_405, SNAG_TOKEN_COUNT_AUTO,
         SNAG_APP_COUNT_SKIPPED, SNAG_COUNT_UNSUPPORTED, false, false},
        {MODEL_COUNT_405, SNAG_TOKEN_COUNT_STRICT, -1, SNAG_COUNT_UNSUPPORTED, false, false},
        {MODEL_COUNT_404, SNAG_TOKEN_COUNT_AUTO, SNAG_APP_COUNT_SKIPPED, SNAG_COUNT_UNSUPPORTED, false, false},
        {MODEL_COUNT_404, SNAG_TOKEN_COUNT_AUTO,
         SNAG_APP_COUNT_SKIPPED, SNAG_COUNT_UNSUPPORTED, true, false},
        {MODEL_COUNT_404, SNAG_TOKEN_COUNT_STRICT, -1, SNAG_COUNT_UNSUPPORTED, true, false},
        {MODEL_COUNT_401, SNAG_TOKEN_COUNT_AUTO, -1, SNAG_COUNT_UNKNOWN, true, false},
        {MODEL_COUNT_403, SNAG_TOKEN_COUNT_AUTO, -1, SNAG_COUNT_UNKNOWN, true, false},
        {MODEL_COUNT_MODEL_404, SNAG_TOKEN_COUNT_AUTO, -1, SNAG_COUNT_UNKNOWN, false, false},
        {MODEL_COUNT_OVERFLOW, SNAG_TOKEN_COUNT_AUTO, SNAG_PROVIDER_CONTEXT_OVERFLOW, SNAG_COUNT_UNKNOWN, false, false},
        {MODEL_COUNT_OK, SNAG_TOKEN_COUNT_AUTO, 0, SNAG_COUNT_SUPPORTED, false, false},
        {MODEL_COUNT_OK, SNAG_TOKEN_COUNT_STRICT, 0, SNAG_COUNT_SUPPORTED, false, false},
        {MODEL_COUNT_405, SNAG_TOKEN_COUNT_AUTO, SNAG_APP_COUNT_SKIPPED, SNAG_COUNT_UNSUPPORTED, false, true},
        {MODEL_COUNT_405, SNAG_TOKEN_COUNT_STRICT, -1, SNAG_COUNT_UNSUPPORTED, false, true},
        {MODEL_COUNT_401, SNAG_TOKEN_COUNT_AUTO, -1, SNAG_COUNT_UNKNOWN, false, true}
    };

    assert(!snag_app_exact_count_enabled(SNAG_TOKEN_COUNT_OFF, SNAG_COUNT_UNKNOWN));
    assert(!snag_app_exact_count_enabled(SNAG_TOKEN_COUNT_AUTO, SNAG_COUNT_UNSUPPORTED));
    assert(snag_app_exact_count_enabled(SNAG_TOKEN_COUNT_AUTO, SNAG_COUNT_SUPPORTED));
    assert(snag_app_exact_count_enabled(SNAG_TOKEN_COUNT_STRICT, SNAG_COUNT_UNSUPPORTED));

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct local_server server;
        struct snag_config config;
        struct snag_credential credential;
        struct app_state app;
        json_t *request = request_with_marker("count-mode");
        const char *method = cases[i].mode == SNAG_TOKEN_COUNT_STRICT ? "anchored_upper_bound" : "unknown";
        uint64_t tokens = 99u;
        char error[256] = {0};
        char temp[4096];
        int rc;

        assert(snprintf(temp, sizeof(temp), "%s/snajpagent-count-mode-XXXXXX",
            getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") > 0);
        assert(mkdtemp(temp));
        start_server(&server,cases[i].fixture,false,"/v1");
        (void)transport_connection(&config,&credential,cases[i].images?"https://api.openai.com":
            cases[i].openrouter?"https://openrouter.ai/api/v1":server.endpoint);
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE",server.endpoint,1)==0);
        config.providers[0].exact_token_count = cases[i].mode;
        memset(&app, 0, sizeof(app));
        snag_store_init(&app.store);
        assert(snag_store_open(&app.store, temp, error, sizeof(error)) == 0);
        app.model_cache = (struct snag_model_cache){0};
        assert(snag_ui_init(&app.ui) == 0);
        app.config = &config;
        app.turn_provider = &config.providers[0];
        app.turn_model = "gpt-transport-test";
        uint64_t expected = tokens;
        if (cases[i].images) {
            app.turn_model = "gpt-5.5";
            assert(json_object_set_new(request, "model", json_string("gpt-5.5")) == 0);
            json_t *item = json_pack("{s:s,s:[{s:s,s:s,s:s}]}", "role", "user", "content",
                "type", "input_image", "detail", "high", "image_url", "data:image/png;base64,YWJj");
            assert(json_array_append_new(json_object_get(request, "input"), item) == 0);
            assert(snag_media_token_bound(request, 0u, &expected, error, sizeof(error)) == 0);
        }
        rc = snag_app_provider_count(&app, request, &credential,
                                    &tokens, &method, error, sizeof(error));
        assert((cases[i].result < 0 && rc < 0) || rc == cases[i].result);
        assert(app.turn_capacity.count_capability == cases[i].capability);
        if (cases[i].images && rc == SNAG_APP_COUNT_SKIPPED)
            assert(tokens == expected && !strcmp(method, "media_upper_bound"));
        else if(cases[i].fixture==MODEL_COUNT_OK)assert(tokens==42u && !strcmp(method,"exact"));
        else assert(tokens==99u);
        assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
        snag_ui_free(&app.ui);
        snag_model_cache_free(&app.model_cache);
        if (unlinkat(app.store.root_fd, "models.lock", 0) < 0) assert(errno == ENOENT);
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
    static const char search_tools[] = "[{\"type\":\"openrouter:web_search\"},{\"type\":\"function\","
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
        request, emit_capture, &emitted, NULL, NULL, NULL, NULL, &graph,
        NULL, NULL, error, sizeof(error), &retries) == 0);
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
        request, emit_capture, &emitted, NULL, NULL, NULL, NULL, &graph,
        NULL, NULL, error, sizeof(error), &retries) == 0);
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
        "update_goal", "irc_send", "irc_topic", "irc_nick", "irc_state", "unknown",
        "web_search", "openrouter:web_search", "speak_text", "write_file", "edit_file"
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
        assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
        assert(snag_tool_result_valid(result) == 0);
        assert(strstr(snag_json_string(result, "model_text"), "read-only"));
        json_decref(result);
    }
    app.session.active_read_only = false;
    char root[4096];
    {
        const char *tmp = getenv("TMPDIR");
        int length = snprintf(root, sizeof(root), "%s/snajpagent-dispatch-XXXXXX",
            tmp ? tmp : "/tmp");
        char probe[8192];
        FILE *out;
        assert(length > 0 && (size_t)length < sizeof(root));
        assert(mkdtemp(root) != NULL);
        assert(snprintf(probe, sizeof(probe), "%s/probe.txt", root) > 0);
        out = fopen(probe, "w");
        assert(out && fputs("hello\n", out) >= 0 && fclose(out) == 0);
        app.session.cwd = root;
    }
    call.name = "read_file";
    json_decref(call.arguments);
    call.arguments = json_pack("{s:s}", "path", "missing.txt");
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(snag_tool_result_valid(result) == 0);
    assert(!strstr(snag_json_string(result, "model_text"), "only in /ro"));
    json_decref(result);
    call.name = "write_file";
    json_decref(call.arguments);
    call.arguments = json_pack("{s:s,s:s}", "path", "written.txt", "content", "x\n");
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strcmp(snag_json_string(result, "status"), "succeeded") == 0);
    json_decref(result);
    json_decref(call.arguments);
}

static void
test_media_count_fallback(void)
{
    struct snag_config config;
    struct app_state app = {0};
    struct snag_credential credential;
    snag_config_init(&config); app.config = &config;
    app.turn_provider = &config.providers[0]; app.turn_model = "gpt-5.5";
    snag_credential_clear(&credential);
    assert(snag_ui_init(&app.ui) == 0);
    json_t *part = json_pack("{s:s,s:s,s:s}", "type", "input_image", "detail", "high",
        "image_url", "data:image/png;base64,YWJj");
    json_t *request = json_pack("{s:s,s:[{s:s,s:[O]}]}", "model", "gpt-5.5", "input", "role", "user", "content", part);
    uint64_t tokens = 0, expected = 0;
    const char *method = "qualified_upper_bound";
    char error[256] = {0};
    assert(snag_media_token_bound(request, 0u, &expected, error, sizeof(error)) == 0);
    config.providers[0].exact_token_count = SNAG_TOKEN_COUNT_OFF;
    assert(snag_app_provider_count(&app, request, &credential, &tokens, &method, error, sizeof(error)) == SNAG_APP_COUNT_SKIPPED);
    assert(tokens == expected && !strcmp(method, "media_upper_bound"));
    /* A configured per-image ceiling supersedes the generic budget on any route. */
    strcpy(config.providers[0].base_url, "https://api.deepseek.com");
    config.model_limit_count = 1u;
    strcpy(config.model_limits[0].provider, config.providers[0].name);
    config.model_limits[0].image_tokens = 1025u;
    assert(snag_media_token_bound(request, 1025u, &expected, error, sizeof(error)) == 0);
    assert(snag_app_provider_count(&app, request, &credential, &tokens, &method, error, sizeof(error)) == SNAG_APP_COUNT_SKIPPED);
    assert(tokens == expected && !strcmp(method, "media_upper_bound"));
    /* A media bound is a valid count method for compaction: the pre-response
     * and post-turn guards must accept it instead of failing the turn. */
    {
        bool compacted = true;
        assert(snag_app_compact_before_response(&app, &credential, tokens, method,
                                                &compacted, error, sizeof(error)) == 0);
        assert(!compacted);
        assert(snag_app_compact_after_turn(&app, tokens, method, error, sizeof(error)) == 0);
    }
    /* An exceeding upper bound does not prove actual overflow. Reproduce the
     * large-text plus one-image route without an exact token counter. */
    app.turn_capacity.hard_input_known = true;
    app.turn_capacity.hard_input_tokens = 258400u;
    {
        char *large = malloc(300001u);
        assert(large);
        memset(large, 'a', 300000u);
        large[300000] = '\0';
        json_t *parts = json_object_get(json_array_get(
            json_object_get(request, "input"), 0u), "content");
        assert(json_array_append_new(parts,
            json_pack("{s:s,s:s}", "type", "input_text", "text", large)) == 0);
        free(large);
        assert(snag_app_provider_count(&app, request, &credential, &tokens, &method,
            error, sizeof(error)) == SNAG_APP_COUNT_SKIPPED);
        assert(tokens > app.turn_capacity.hard_input_tokens &&
            !strcmp(method, "media_upper_bound"));
        bool compacted = false;
        char guard_error[256] = {0};
        int guard_rc = snag_app_compact_before_response(&app, &credential, tokens, method,
                                                        &compacted, guard_error, sizeof(guard_error));
        assert(guard_rc == 0);
        assert(!compacted);
        /* A real exact count above the same hard capacity remains guarded. */
        assert(snag_app_compact_before_response(&app, &credential, tokens, "exact",
            &compacted, guard_error, sizeof(guard_error)) != 0);
        assert(!compacted);
        assert(json_array_remove(parts, json_array_size(parts) - 1u) == 0);
    }
    app.turn_capacity.hard_input_known = false;
    strcpy(config.providers[0].base_url, "https://api.openai.com");
    config.model_limit_count = 0u;
    config.providers[0].exact_token_count = SNAG_TOKEN_COUNT_AUTO;
    app.turn_capacity.count_capability = SNAG_COUNT_UNSUPPORTED;
    /* Back on the generic image budget: recompute the expected bound. */
    assert(snag_media_token_bound(request, 0u, &expected, error, sizeof(error)) == 0);
    assert(snag_app_provider_count(&app, request, &credential, &tokens, &method, error, sizeof(error)) == SNAG_APP_COUNT_SKIPPED);
    /* Compressed payload length changes neither image budget nor textual count. */
    assert(json_object_set_new(part, "image_url", json_string("data:image/png;base64,YWJjYWJjYWJjYWJj")) == 0);
    assert(snag_app_provider_count(&app, request, &credential, &tokens, &method, error, sizeof(error)) == SNAG_APP_COUNT_SKIPPED && tokens == expected);
    assert(json_object_set_new(part, "detail", json_string("original")) == 0);
    assert(snag_app_provider_count(&app, request, &credential, &tokens, &method, error, sizeof(error)) < 0);
    assert(json_object_set_new(part, "detail", json_string("high")) == 0);
    /* Any model, including one absent from every table, budgets images. */
    assert(json_object_set_new(request, "model", json_string("unknown")) == 0);
    assert(snag_app_provider_count(&app, request, &credential, &tokens, &method, error, sizeof(error)) == SNAG_APP_COUNT_SKIPPED &&
           !strcmp(method, "media_upper_bound"));
    json_decref(part); json_decref(request);
    snag_ui_free(&app.ui); snag_config_free(&config);
}

static void
test_native_compaction_probe(void)
{
    struct snag_config config;
    struct snag_credential credential;
    struct local_server server;
    char error[256] = {0};
    const struct {
        enum model_fixture fixture;
        bool live;
        int expected;
    } cases[] = {
        {MODEL_COMPACT_OK, true, 1},
        {MODEL_COMPACT_404, true, 0},
        {MODEL_COMPACT_502, true, 0},
        {MODEL_COMPACT_OK, false, -1}, /* server stopped: no response at all */
    };

    snag_config_init(&config);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    strcpy(config.providers[0].base_url, "https://api.openai.com");
    credential_set(&credential, "probe-secret");
    credential.root_fd = -1;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        error[0] = '\0';
        start_server(&server, cases[i].fixture, false, "");
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", server.endpoint, 1) == 0);
        if (!cases[i].live) stop_server(&server);
        int rc = snag_provider_native_compaction_probe((struct snag_provider_connection){
            &config, &config.providers[0], &credential, NULL, NULL, NULL, NULL, 0},
            "gpt-5.5", error, sizeof(error));
        if (cases[i].live) stop_server(&server);
        assert(rc == cases[i].expected);
    }
    assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
    snag_config_free(&config);
}

static void
test_local_audio_admission(void)
{
    struct app_state app = {0};
    struct snag_config config;
    bool handled = false;
    snag_config_init(&config); app.config = &config;
    assert(snag_ui_init(&app.ui) == 0);
    assert(snag_app_voice_command(&app, "/voice on", &handled) == 0 && handled && !app.voice);
    assert(snag_app_audio_command(&app, "/dictate", &handled) == 0 && handled && !app.audio);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    strcpy(config.audio.provider, config.providers[0].name);
    strcpy(config.audio.transcribe_model, "fixture");
    /* UI not opened: must reject before calling a device backend. */
    assert(snag_app_audio_command(&app, "/dictate", &handled) == 0 && handled && !app.audio);
    assert(snag_app_audio_command(&app, "/play /unaccepted.wav", &handled) == 0 && handled && !app.audio);
    assert(snag_app_audio_command(&app, "/play stop", &handled) == 0 && handled && !app.audio);
    assert(snag_app_audio_command(&app, "/other", &handled) == 0 && !handled);
    strcpy(config.audio.realtime_model, "fixture-realtime");
    strcpy(config.audio.voice, "fixture-voice");
    assert(snag_app_voice_command(&app, "/voice on", &handled) == 0 && handled && !app.voice);
    assert(snag_app_voice_command(&app, "/voice off", &handled) == 0 && handled && !app.voice);
    assert(snag_app_voice_command(&app, "/voice mute", &handled) == 0 && handled && !app.voice);
    assert(snag_app_voice_command(&app, "/other", &handled) == 0 && !handled);
    assert(snag_ui_insert_draft(&app.ui, "UI still works") == 0);
    snag_app_audio_close(&app); snag_ui_free(&app.ui); snag_config_free(&config);
}

static int voice_close_record(void *opaque,const struct snag_session *state,uint64_t seq,const char *type,const json_t *data,char *error,size_t size)
{
    (void)state;(void)seq;(void)error;(void)size;
    if(strcmp(type,"voice_event"))return 0;
    unsigned int *counts=opaque;
    const char *kind=snag_json_string(json_object_get(data,"event"),"type");
    if(!strcmp(kind,"voice_transcript"))++counts[0];
    else if(!strcmp(kind,"voice_stopped"))++counts[1];
    else assert(snag_string_in(kind, "voice_started voice_response voice_muted"));
    return 0;
}

static void test_voice_close(void)
{
    char path[4096],error[256],id[33];const char *tmp=getenv("TMPDIR");
    assert(snprintf(path,sizeof(path),"%s/snajpagent-voice-close-XXXXXX",tmp?tmp:"/tmp")>0);
    assert(mkdtemp(path));
    struct app_state app={0};
    struct snag_config config;snag_config_init(&config);app.config=&config;
    snag_store_init(&app.store);snag_session_init(&app.session);
    assert(snag_store_open(&app.store,path,error,sizeof(error))==0);
    assert(snag_ui_init(&app.ui)==0);
    for(unsigned int mode=0;mode<3u;++mode) {
        assert(snag_session_create(&app.store,&app.session,path,"default","fixture","medium",error,sizeof(error))==0);
        strcpy(id,app.session.id);
        json_t *events=json_pack("[{s:s},{s:s,s:s,s:s,s:s},{s:s},{s:s},{s:s}]",
            "type","voice_ready","type","voice_transcript","speaker","user","item_id","input-1","text","final words",
            "type","voice_handoff","type","voice_buffering","type","voice_expiring");
        assert(events && snag_app_voice_fixture(&app,events,mode!=0u)==0);json_decref(events);
        if(!mode)snag_app_voice_close(&app);
        else if(mode==1u) {
            bool handled=false;assert(snag_app_voice_command(&app,"/voice off",&handled)==0 && handled);
        } else assert(snag_app_voice_service(&app)==0);
        assert(!app.voice && !app.session.pending_queue_count);
        snag_session_close(&app.session);snag_session_init(&app.session);
        assert(snag_session_open(&app.store,&app.session,id,error,sizeof(error))==0);
        unsigned int counts[2]={0};
        assert(snag_session_each_event(&app.session,voice_close_record,counts,error,sizeof(error))==0);
        assert(counts[0]==1u && counts[1]==1u && !app.session.pending_queue_count);
        snag_app_voice_close(&app); /* Repeated close cannot append another stop. */
        snag_session_close(&app.session);snag_session_init(&app.session);
    }
    snag_ui_free(&app.ui);snag_store_close(&app.store);snag_config_free(&config);
}

static void
test_voice_transcript_labels(void)
{
    char path[4096];
    char error[256];
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-labels-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    struct snag_config config;
    snag_config_init(&config);
    struct app_state app = {.config = &config};
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_prepare(&app.session, path, "default", "fixture", "medium",
        error, sizeof(error)) == 0 && app.session.pending_log);

    for (unsigned int verbosity = 0u; verbosity <= 6u; ++verbosity) {
        int captured[2];
        assert(pipe(captured) == 0);
        int saved = dup(STDERR_FILENO);
        assert(saved >= 0 && dup2(captured[1], STDERR_FILENO) >= 0);
        assert(close(captured[1]) == 0);
        assert(snag_ui_init(&app.ui) == 0);
        assert(snag_ui_set_verbosity(&app.ui, verbosity) == 0);
        json_t *events = json_pack("[{s:s,s:s,s:s,s:s},{s:s,s:s,s:s,s:s}]",
            "type", "voice_transcript", "speaker", "user", "item_id", "input-label",
            "text", "show the -l option", "type", "voice_transcript", "speaker", "assistant",
            "item_id", "reply-label", "text", "192.0.2.1\n/fixture/file");
        assert(events && snag_app_voice_fixture(&app, events, false) == 0);
        json_decref(events);
        assert(snag_app_voice_service(&app) == 0);
        assert(!app.session.pending_queue_count && !app.session.active_turn);
        bool persisted = app.session.pending_log == NULL;
        snag_app_voice_close(&app);
        snag_ui_free(&app.ui);
        assert(dup2(saved, STDERR_FILENO) >= 0 && close(saved) == 0);
        assert(persisted);

        char output[8192];
        size_t used = 0u;
        ssize_t n;
        while ((n = read(captured[0], output + used, sizeof(output) - used - 1u)) > 0) {
            used += (size_t)n;
            assert(used < sizeof(output) - 1u);
        }
        assert(n == 0 && close(captured[0]) == 0);
        output[used] = '\0';
        assert(strstr(output, "You [voice, ASR]: show the -l option"));
        assert(strstr(output, "Voice model [generated]: 192.0.2.1\n/fixture/file"));
    }
    char id[SNAG_ID_HEX_LEN + 1u];
    memcpy(id, app.session.id, sizeof(id));
    snag_session_close(&app.session);
    assert(snag_session_open(&app.store, &app.session, id, error, sizeof(error)) == 0);
    json_t *context = NULL;
    assert(snag_session_voice_context(&app.session, &context, error, sizeof(error)) == 0);
    const char *asr = snag_json_string(context, "recent_asr");
    const char *reply = snag_json_string(context, "recent_generated_reply");
    assert(asr && !strcmp(asr, "show the -l option"));
    assert(reply && !strcmp(reply, "192.0.2.1\n/fixture/file"));
    json_decref(context);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_config_free(&config);
}

static int
voice_tool_record(void *opaque, const struct snag_session *state, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t size)
{
    (void)state;
    (void)seq;
    (void)error;
    (void)size;
    unsigned int *counts = opaque;
    if (!strcmp(type, "future_turn_queued")) {
        assert(counts[0] == counts[1] + 1u && counts[1] == counts[2]);
        ++counts[1];
    } else if (!strcmp(type, "voice_event")) {
        const json_t *event = json_object_get(data, "event");
        const char *operation = snag_json_string(event, "operation");
        if (operation && !strcmp(operation, "interface_request_settled")) {
            const json_t *metrics = json_object_get(event, "metrics");
            const json_t *usage = json_object_get(metrics, "usage");
            const char *status = snag_json_string(event, "status");
            const char *model = snag_json_string(event, "model");
            if (status && !strcmp(status, "failed")) {
                assert(json_is_integer(json_object_get(metrics, "elapsed_ms")));
                ++counts[4];
                return 0;
            }
            assert(status && !strcmp(status, "completed"));
            assert(model && !strcmp(model, "fixture"));
            assert(snag_json_string(event, "provider") && snag_json_string(event, "call_id"));
            assert(json_is_integer(json_object_get(metrics, "elapsed_ms")));
            assert(json_is_integer(json_object_get(metrics, "response_ready_ms")));
            assert(json_integer_value(json_object_get(metrics, "retries")) == 0);
            if (counts[3] % 2u || !strcmp(snag_json_string(event, "call_id"), "third")) {
                assert(json_is_integer(json_object_get(metrics, "first_text_ms")));
                assert(json_integer_value(json_object_get(usage, "cached_tokens")) == 4);
            } else {
                assert(json_is_null(json_object_get(metrics, "first_text_ms")));
                assert(json_is_null(json_object_get(usage, "input_tokens")));
                assert(!json_object_get(usage, "cached_tokens"));
            }
            ++counts[3];
        } else if (operation && !strcmp(operation, "interface_compaction_settled")) {
            const char *status = snag_json_string(event, "status");
            assert(status);
            if (!strcmp(status, "failed")) {
                ++counts[7];
            } else {
                assert(!strcmp(status, "completed"));
                ++counts[5];
            }
        } else if (operation && !strcmp(operation, "interface_compacted")) {
            assert(counts[5] > 0u);
            assert(json_is_object(json_object_get(event, "summary")));
            ++counts[6];
        } else if (operation && !strcmp(operation, "interface_failed")) {
            assert(!strcmp(snag_json_string(event, "call_id"), "second"));
            assert(!strcmp(snag_json_string(event, "status"), "failed"));
            assert(!strcmp(snag_json_string(event, "submitted_id"), ""));
            ++counts[8];
        } else if (operation && !strcmp(operation, "interface_tool_started")) {
            assert(counts[0] == counts[1] && counts[1] == counts[2]);
            assert(!strcmp(snag_json_string(event, "tool"), "submit_input"));
            assert(json_is_object(json_object_get(event, "arguments")));
            assert(!json_object_get(event, "result"));
            ++counts[0];
        } else if (operation && !strcmp(operation, "interface_tool")) {
            assert(counts[0] == counts[1] && counts[1] == counts[2] + 1u);
            assert(!json_object_get(event, "arguments"));
            assert(json_is_object(json_object_get(event, "result")));
            ++counts[2];
        }
    }
    return 0;
}

static unsigned int
voice_fixture_transition(struct app_state *app, const char *type, json_t *data, bool observe)
{
    char error[256];
    uint64_t seq = app->session.next_seq;
    char *expected = observe ? snag_app_history_data(seq, type, data) : NULL;
    assert(data && (!observe || expected));
    assert(snag_session_commit(&app->session, type, data, NULL, error, sizeof(error)) == 0);
    if (!observe) return 0u;
    assert(snag_app_voice_service(app) == 0 && app->voice);
    json_t *packet = snag_app_voice_fixture_observation(app);
    if (!packet) {
        fprintf(stderr, "native observation omitted %s\n", type);
        free(expected);
        return 1u;
    }
    assert(!strcmp(snag_json_string(packet, "event_type"), type));
    assert((uint64_t)json_integer_value(json_object_get(packet, "seq")) == seq);
    assert(json_is_true(json_object_get(packet, "complete")));
    assert(!strcmp(snag_json_string(packet, "text"), expected));
    json_decref(packet);
    free(expected);
    return 0u;
}

static unsigned int
voice_fixture_model_changes(struct app_state *app, bool observe)
{
    unsigned int missing = 0u;
    struct snag_session *session = &app->session;
    for (unsigned int i = 0u; i < 2u; ++i) {
        const char *effort = !strcmp(session->default_effort, "medium") ? "high" : "medium";
        missing += voice_fixture_transition(app, "model_selection_changed",
            json_pack("{s:s,s:s,s:s,s:s,s:s,s:s}", "old_provider", session->default_provider,
                "old_model", session->default_model, "old_effort", session->default_effort,
                "new_provider", session->default_provider, "new_model", session->default_model,
                "new_effort", effort), observe);
        if (session->active_turn) {
            missing += voice_fixture_transition(app, "turn_model_changed",
                json_pack("{s:s,s:s,s:s,s:s,s:s}", "turn_id", session->active_turn_id,
                    "old_provider", session->active_turn_provider,
                    "old_model", session->active_turn_model,
                    "old_effort", session->active_turn_effort,
                    "new_effort", session->default_effort), observe);
        }
    }
    return missing;
}

static unsigned int
voice_fixture_control_changes(struct app_state *app, bool observe)
{
    unsigned int missing = 0u;
    for (size_t i = 0u; i < 6u; ++i) {
        json_t *data;
        if (i < 3u) {
            data = json_pack("{s:i}", "control", SNAG_CONTROL_RETRY);
        } else if (i == 3u) {
            data = json_pack("{s:s,s:s,s:s}", "goal_id", app->session.goal_id,
                "actor", "user", "prompt", "Corrected historical objective");
        } else if (i == 4u) {
            data = json_pack("{s:s,s:b}", "goal_id", app->session.goal_id, "locked", 1);
        } else {
            data = json_pack("{s:s}", "goal_id", app->session.goal_id);
        }
        missing += voice_fixture_transition(app, voice_transition_types[i], data, observe);
    }
    return missing + voice_fixture_model_changes(app, observe);
}

static void
voice_fixture_working_outcome(struct app_state *app, unsigned int number, const char *message)
{
    char id[33];
    char error[256];
    assert(snprintf(id, sizeof(id), "%032x", number) > 0);
    json_t *started = json_pack("{s:{s:s,s:s,s:n,s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:b},"
        "s:s,s:b,s:o,s:n,s:n,s:s,s:s,s:I,s:s}",
        "config", "capability_version", SNAJPAGENT_CAPABILITY_VERSION, "effort", "medium",
        "max_output_tokens", "model", "fixture", "provider", "openai",
        "profile_id", SNAJPAGENT_PROFILE_ID, "prompt_schema", 1, "replay_schema", 1,
        "tool_schema", 1, "max_parallel_commands", 4, "parallel_tool_calls", 1,
        "input_kind", "direct", "read_only", 0, "instructions", json_array(),
        "queue_id", "queue_seq", "text", "working input transport-secret", "turn_id", id,
        "turn_number", (json_int_t)number, "cwd", app->session.cwd);
    assert(started && snag_session_commit(&app->session, "turn_started", started,
        NULL, error, sizeof(error)) == 0);
    app->turn_provider = snag_config_provider(app->config, "openai");
    app->turn_model = "fixture";
    app->turn_effort = "medium";
    struct snag_instruction_set instructions = {0};
    struct snag_context_projection projection = {0};
    json_t *steering = json_array();
    assert(snag_context_build(&app->session, "fixture", "medium", 1u, steering, 0u, false,
        app->config, NULL, &instructions, NULL, &projection, error, sizeof(error), NULL) == 0);
    char source[SNAG_SHA256_HEX_LEN + 1u];
    snag_sha256_hex("working fixture", strlen("working fixture"), source);
    json_t *response = snag_app_response_started_data(app, id, id, 1u, &projection,
        "unknown", source, steering);
    assert(response && snag_session_commit(&app->session, "response_started", response,
        NULL, error, sizeof(error)) == 0);
    snag_context_projection_free(&projection);
    json_decref(steering);
    json_t *output = json_pack("{s:s,s:s,s:i,s:i,s:i,s:{s:s,s:s,s:s,s:s,s:s}}",
        "turn_id", id, "response_id", id, "cycle", 1, "index", 0, "offset", 0,
        "item", "kind", "assistant", "phase", "commentary", "local_item_id", id,
        "provider_item_id", "working_narration", "text", message);
    assert(output && snag_session_commit(&app->session, "response_output", output,
        NULL, error, sizeof(error)) == 0);
    assert(snag_session_commit(&app->session, "response_failed",
        json_pack("{s:s,s:i,s:s,s:O,s:s,s:i,s:s}", "class", "provider", "cycle", 1,
            "message", message, "partial_public", app->session.response_public,
            "response_id", id, "retry_count", 0, "turn_id", id),
        NULL, error, sizeof(error)) == 0);
    char goal[33];
    assert(snprintf(goal, sizeof(goal), "%032x", 0x100u + number) == 32);
    assert(snag_session_commit(&app->session, "goal_started", json_pack("{s:s,s:s}",
        "goal_id", goal, "prompt", "Initial historical objective"), NULL,
        error, sizeof(error)) == 0);
    assert(voice_fixture_control_changes(app, false) == 0u);
    assert(snag_session_commit(&app->session, "turn_failed", json_pack("{s:s,s:s,s:s}",
        "class", "provider", "message", message, "turn_id", id),
        NULL, error, sizeof(error)) == 0);
}

static void
test_voice_concurrent_owner(enum model_fixture model)
{
    bool compact = model != MODEL_VOICE_QUEUE;
    bool retry = model == MODEL_VOICE_COMPACT_RETRY;
    bool bad_summary = model == MODEL_VOICE_COMPACT_TOOL;
    unsigned int queued = bad_summary ? 1u : 2u;
    char path[4096], error[256], queues[2][33];
    const char *tmp = getenv("TMPDIR");
    struct app_state app = {0};
    struct snag_config config;
    struct snag_credential credential;
    struct local_server server;

    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-calls-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    start_server(&server, model, false, "/v1");
    (void)transport_connection(&config, &credential, server.endpoint);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    assert(snag_secret_source_parse(&config.providers[0].api_key,
        "\"transport-secret\"", NULL, error, sizeof(error)) == 0);
    app.config = &config;
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_ui_init(&app.ui) == 0);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "openai",
        "fixture", "medium", error, sizeof(error)) == 0);
    json_t *events = json_array();
    char speech[4096] = "retained-first-utterance";
    if (model == MODEL_VOICE_QUEUE) {
        for (unsigned int i = 0u; i < 2u; ++i) {
            assert(json_array_append_new(events, json_pack("{s:s,s:s,s:s,s:s}",
                "type", "voice_transcript", "speaker", i ? "assistant" : "user",
                "item_id", i ? "native-reply" : "native-input", "text", i ?
                    "native acknowledgement" : "native correction transport-secret")) == 0);
        }
        char *padding = malloc(1024u * 1024u + 1u);
        assert(padding);
        memset(padding, 'x', 1024u * 1024u);
        padding[1024u * 1024u] = '\0';
        for (unsigned int i = 0u; i < 4u; ++i) {
            assert(snag_session_commit(&app.session, "voice_event", json_pack(
                "{s:s,s:s,s:s,s:{s:s,s:s}}", "connection_id",
                "0123456789abcdef0123456789abcdef", "provider", "openai", "model", "fixture",
                "event", "type", "voice_usage", "padding", padding),
                NULL, error, sizeof(error)) == 0);
        }
        free(padding);
        voice_fixture_working_outcome(&app, 1u, "initial working outcome");
    }
    if (retry) {
        size_t length = strlen(speech);
        for (size_t i = 0; i < 1500u; ++i) {
            memcpy(speech + length, "λ", 2u);
            length += 2u;
        }
        speech[length] = '\0';
    }
    for (unsigned int i = 0; i < 2u; ++i) {
        const char *id = i ? "second" : "first";
        assert(json_array_append_new(events,
            json_pack("{s:s,s:s,s:s,s:s,s:s,s:s}", "type", "voice_handoff",
                "input_id", id, "response_id", id, "call_id", id,
                "transcript", i ? id : speech, "request", id)) == 0);
    }
    assert(snag_app_voice_fixture(&app, events, false) == 0);
    json_decref(events);
    unsigned int acknowledgements = 0u;
    uint64_t started = snag_monotonic_ms();
    while (acknowledgements < 2u && snag_monotonic_ms() - started < 5000u) {
        assert(snag_app_voice_service(&app) == 0 && app.voice);
        json_t *reply = snag_app_voice_fixture_result(&app);
        if (reply) {
            const char *reply_text = snag_json_string(reply, "text");
            if (reply_text && strstr(reply_text, "transport-secret")) {
                fprintf(stderr, "interface reply exposed a configured credential\n");
                abort();
            }
            bool failure = bad_summary && acknowledgements == 1u;
            if (!failure && !json_is_false(json_object_get(reply, "final"))) {
                const char *text = snag_json_string(reply, "text");
                fprintf(stderr, "unexpected voice reply in fixture %d: %.256s\n",
                    (int)model, text ? text : "(no text)");
            }
            assert(!strcmp(snag_json_string(reply, "call_id"),
                acknowledgements ? "second" : "first"));
            assert(failure ? json_is_true(json_object_get(reply, "final")) :
                json_is_false(json_object_get(reply, "final")));
            if (failure) assert(strstr(snag_json_string(reply, "text"), "no summary"));
            ++acknowledgements;
            if (model == MODEL_VOICE_QUEUE && acknowledgements == 1u) {
                voice_fixture_working_outcome(&app, 2u, "new working outcome");
                assert(snag_session_commit(&app.session, "voice_event", json_pack(
                    "{s:s,s:s,s:s,s:{s:s,s:s,s:s,s:s}}", "connection_id",
                    "0123456789abcdef0123456789abcdef", "provider", "openai", "model", "fixture",
                    "event", "type", "voice_transcript", "speaker", "user", "item_id", "follow-up",
                    "text", "native follow-up discussion"), NULL, error, sizeof(error)) == 0);
            }
            json_decref(reply);
        }
        snag_sleep_ms(10u);
    }
    assert(acknowledgements == 2u && app.session.pending_queue_count == queued);
    if (model == MODEL_VOICE_QUEUE) {
        /* Native delivery remains backpressured while interface history advances. */
        json_t *packet = snag_app_voice_fixture_observation(&app);
        assert(packet);
        json_decref(packet);
    }
    unsigned int tool_records[9] = {0};
    assert(snag_session_each_event(&app.session, voice_tool_record, tool_records,
        error, sizeof(error)) == 0);
    assert(tool_records[0] == queued && tool_records[1] == queued && tool_records[2] == queued);
    assert(tool_records[3] == queued * 2u);
    assert(tool_records[4] == (retry ? 2u : (unsigned int)compact));
    assert(tool_records[6] == (retry ? 2u : (unsigned int)(compact && !bad_summary)));
    assert(retry ? tool_records[5] >= 2u : tool_records[5] == (unsigned int)compact);
    assert(tool_records[7] == (unsigned int)retry);
    assert(tool_records[8] == (unsigned int)bad_summary);
    assert(app.session.usage_totals.responses == 0u &&
        app.session.usage_totals.input_tokens == 0u);
    assert(app.session.queue_armed);
    for (size_t i = 0; i < queued; ++i) {
        strcpy(queues[i], app.session.pending_queue[i].queue_id);
    }

    /* Both results can become ready before the audio owner consumes either. */
    for (size_t i = queued; i-- > 0u;) {
        json_t *cancel = json_pack("{s:[s],s:s}", "queue_ids", queues[i],
            "reason", "user");
        assert(snag_session_commit(&app.session, "future_turn_cancelled",
            json_incref(cancel), NULL, error, sizeof(error)) == 0);
        snag_app_voice_event(&app, "future_turn_cancelled", cancel);
        json_decref(cancel);
    }
    assert(snag_app_voice_service(&app) == 0 && app.voice);
    uint64_t seq = app.session.next_seq;
    assert(snag_app_voice_service(&app) == 0 && app.voice);
    assert(app.session.next_seq == seq);
    json_t *result = snag_app_voice_fixture_result(&app);
    assert(result && !strcmp(snag_json_string(result, "call_id"), "first"));
    assert(strstr(snag_json_string(result, "text"), "cancelled"));
    json_decref(result);
    if (queued == 2u) {
        assert(snag_app_voice_service(&app) == 0 && app.voice);
        result = snag_app_voice_fixture_result(&app);
        assert(result && !strcmp(snag_json_string(result, "call_id"), "second"));
        assert(strstr(snag_json_string(result, "text"), "cancelled"));
        json_decref(result);
    }
    seq = app.session.next_seq;
    assert(snag_app_voice_service(&app) == 0 && app.voice);
    assert(!snag_app_voice_fixture_result(&app) && app.session.next_seq == seq);
    snag_app_voice_close(&app);
    if (model == MODEL_VOICE_QUEUE) {
        char id[SNAG_ID_HEX_LEN + 1u];
        strcpy(id, app.session.id);
        snag_session_close(&app.session);
        assert(snag_session_open(&app.store, &app.session, id, error, sizeof(error)) == 0);
        events = json_pack("[{s:s,s:s,s:s,s:s,s:s,s:s}]", "type", "voice_handoff",
            "input_id", "third", "response_id", "third", "call_id", "third",
            "transcript", "What happened earlier?", "request", "Report earlier outcomes");
        assert(events && snag_app_voice_fixture(&app, events, false) == 0);
        json_decref(events);
        json_t *reply = NULL;
        started = snag_monotonic_ms();
        while (!reply && snag_monotonic_ms() - started < 5000u) {
            assert(snag_app_voice_service(&app) == 0 && app.voice);
            reply = snag_app_voice_fixture_result(&app);
            if (!reply) snag_sleep_ms(10u);
        }
        assert(reply && !strcmp(snag_json_string(reply, "call_id"), "third"));
        assert(strstr(snag_json_string(reply, "text"), "transport"));
        json_decref(reply);
        memset(tool_records, 0, sizeof(tool_records));
        assert(snag_session_each_event(&app.session, voice_tool_record, tool_records,
            error, sizeof(error)) == 0);
        assert(tool_records[0] == 2u && tool_records[1] == 2u && tool_records[2] == 2u);
        assert(tool_records[3] == 5u && tool_records[8] == 0u);
        assert(app.session.pending_queue_count == 0u && app.session.usage_totals.responses == 0u);
        snag_app_voice_close(&app);
    }
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_ui_free(&app.ui);
    snag_config_free(&config);
    snag_credential_clear(&credential);
    stop_server(&server);
}

static int
voice_backlog_settled(void *opaque, const struct snag_session *session, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t size)
{
    (void)session;
    (void)seq;
    (void)error;
    (void)size;
    if (strcmp(type, "voice_event")) return 0;
    const json_t *event = json_object_get(data, "event");
    const char *op = snag_json_string(event, "operation");
    if (!op) return 0;
    assert(strcmp(op, "interface_failed"));
    if (strcmp(op, "interface_request_settled")) return 0;
    assert(!strcmp(snag_json_string(event, "call_id"), "backlog"));
    assert(!strcmp(snag_json_string(event, "status"), "completed"));
    ++*(unsigned int *)opaque;
    return 0;
}

static void
test_voice_initial_backlog(void)
{
    char path[4096];
    char error[256];
    const char *tmp = getenv("TMPDIR");
    struct app_state app = {0};
    struct snag_config config;
    struct snag_credential credential;
    struct local_server server;
    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-backlog-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_ui_init(&app.ui) == 0);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "openai",
        "fixture", "medium", error, sizeof(error)) == 0);
    /* 34 legal 1 MiB objectives cross the existing 32 MiB serialization boundary. */
    char *prompt = malloc(SNAG_MAX_GOAL_PROMPT + 1u);
    assert(prompt);
    for (size_t i = 0u; i < SNAG_MAX_GOAL_PROMPT; i += 2u) memcpy(prompt + i, "λ", 2u);
    prompt[SNAG_MAX_GOAL_PROMPT] = '\0';
    for (unsigned int i = 1u; i <= 34u; ++i) {
        char identity[33];
        assert(snprintf(identity, sizeof(identity), "%032x", i) == 32);
        assert(snag_session_commit(&app.session, "goal_started", json_pack("{s:s,s:s}",
            "goal_id", identity, "prompt", prompt), NULL, error, sizeof(error)) == 0);
        assert(snag_session_commit(&app.session, "goal_completed", json_pack("{s:s,s:s}",
            "goal_id", identity, "actor", "model"), NULL, error, sizeof(error)) == 0);
    }
    free(prompt);
    start_server(&server, MODEL_VOICE_BACKLOG, false, "/v1");
    (void)transport_connection(&config, &credential, server.endpoint);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    assert(snag_secret_source_parse(&config.providers[0].api_key,
        "\"transport-secret\"", NULL, error, sizeof(error)) == 0);
    app.config = &config;
    for (unsigned int pass = 0u; pass < 2u; ++pass) {
        const char *id = pass ? "backlog" : "backlog-cancelled";
        json_t *events = json_pack("[{s:s,s:s,s:s,s:s,s:s,s:s}]", "type", "voice_handoff",
            "input_id", id, "response_id", id, "call_id", id,
            "transcript", pass ? "backlog-pending-utterance" : "cancel this read",
            "request", "Report the old goals");
        assert(events && snag_app_voice_fixture(&app, events, false) == 0);
        json_decref(events);
        assert(snag_app_voice_service(&app) == 0 && app.voice);
        assert(!snag_app_voice_fixture_result(&app));
        if (pass) {
            json_t *reply = NULL;
            uint64_t started = snag_monotonic_ms();
            while (!reply && snag_monotonic_ms() - started < 60000u) {
                assert(snag_app_voice_service(&app) == 0 && app.voice);
                reply = snag_app_voice_fixture_result(&app);
                if (!reply) snag_sleep_ms(10u);
            }
            assert(reply && !strcmp(snag_json_string(reply, "call_id"), "backlog"));
            assert(strstr(snag_json_string(reply, "text"), "transport"));
            json_decref(reply);
        }
        /* The first pass stops between reader quanta, before any request starts. */
        snag_app_voice_close(&app);
        assert(!app.voice && app.session.pending_queue_count == 0u);
    }
    assert(app.session.goal_status == SNAG_GOAL_COMPLETED);
    assert(app.session.usage_totals.responses == 0u);
    unsigned int settled = 0u;
    assert(snag_session_each_event(&app.session, voice_backlog_settled, &settled,
        error, sizeof(error)) == 0 && settled == 1u);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_ui_free(&app.ui);
    snag_config_free(&config);
    snag_credential_clear(&credential);
    stop_server(&server);
}

struct native_capacity_records {
    json_t *adopted;
    size_t largest_output;
    unsigned int adopted_count, waiting, settled, output, discarded, tools;
};

static int
native_capacity_record(void *opaque, const struct snag_session *session, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t size)
{
    (void)session;
    (void)seq;
    (void)error;
    (void)size;
    if (strcmp(type, "voice_event")) return 0;
    struct native_capacity_records *seen = opaque;
    json_t *event = json_object_get(data, "event");
    const char *operation = snag_json_string(event, "operation");
    if (!operation || strncmp(operation, "native_compact", 14u)) return 0;
    assert(!json_object_get(event, "call_id"));
    const char *text = snag_json_string(event, "text");
    assert(!text || !strstr(text, "native-capacity-secret"));
    if (!strcmp(operation, "native_compaction_waiting")) ++seen->waiting;
    else if (!strcmp(operation, "native_compacted")) {
        json_decref(seen->adopted);
        seen->adopted = json_incref(event);
        ++seen->adopted_count;
    } else if (!strcmp(operation, "native_compaction_settled")) {
        assert(json_is_object(json_object_get(event, "metrics")));
        ++seen->settled;
    } else if (!strcmp(operation, "native_compaction_output")) {
        size_t bytes = json_string_length(json_object_get(event, "text"));
        if (bytes > seen->largest_output) seen->largest_output = bytes;
        if (bytes > SNAG_MAX_PUBLIC_ITEM - 128u) {
            assert(!strcmp(text + bytes - 19u, "native complete end"));
        }
        ++seen->output;
    } else if (!strcmp(operation, "native_compaction_output_discarded")) ++seen->discarded;
    else if (!strcmp(operation, "native_compaction_tool_discarded")) ++seen->tools;
    return 0;
}

static int
native_capacity_prefix(void *opaque, const struct snag_session *session, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t size)
{
    (void)session;
    (void)type;
    (void)data;
    (void)error;
    (void)size;
    return seq >= *(uint64_t *)opaque ? 1 : 0;
}

static void
test_voice_native_capacity(enum model_fixture mode)
{
    char path[4096], error[256];
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(path, sizeof(path), "%s/snajpagent-native-capacity-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    struct local_server server;
    bool closing = mode == MODEL_VOICE_NATIVE_CANCEL || mode == MODEL_VOICE_NATIVE_CANCEL_DONE;
    bool success = mode == MODEL_VOICE_NATIVE_SUMMARY || mode == MODEL_VOICE_NATIVE_HANDOFF;
    int received[2] = {-1, -1}, release[2] = {-1, -1};
    if (closing) {
        assert(pipe(received) == 0 && pipe(release) == 0);
        voice_request_ready_fd = received[1];
        voice_request_release_fd = release[0];
    }
    const char *authorization = expected_authorization_header;
    expected_authorization_header = "Authorization: Bearer native-capacity-secret";
    start_server(&server, mode, false, "/v1");
    expected_authorization_header = authorization;
    if (closing) {
        assert(close(received[1]) == 0 && close(release[0]) == 0);
        voice_request_ready_fd = voice_request_release_fd = -1;
    }
    struct snag_config config;
    snag_config_init(&config);
    struct snag_provider_config *provider = &config.providers[0];
    strcpy(provider->base_url, server.endpoint);
    strcpy(provider->openrouter_referer, "https://github.com/snajpa/snajpagent");
    strcpy(provider->openrouter_title, "snajpagent");
    assert(snag_secret_source_parse(&provider->api_key,
        "\"native-capacity-secret\"", NULL, error, sizeof(error)) == 0);
    struct app_state app = {.config = &config};
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, provider->name, "fixture", "medium",
        error, sizeof(error)) == 0);
    assert(snag_ui_init(&app.ui) == 0);
    json_t *empty = json_array();
    if (mode == MODEL_VOICE_NATIVE_HANDOFF) {
        assert(json_array_append_new(empty, json_pack("{s:s,s:s,s:s,s:s,s:s,s:s}",
            "type", "voice_handoff", "call_id", "native_pending", "input_id", "original_input",
            "response_id", "original_response", "transcript", "pre-existing discussion",
            "request", "Continue the pre-existing discussion")) == 0);
    }
    assert(empty && snag_app_voice_fixture(&app, empty, false) == 0);
    json_decref(empty);
    /* These are live records, after the initial restoration boundary. An
     * interface summary's observation time cannot establish native coverage. */
    const char *queue = "33333333333333333333333333333333";
    assert(snag_session_commit(&app.session, "future_turn_queued",
        json_pack("{s:s,s:s,s:b,s:s,s:b}", "queue_id", queue,
            "text", "previously accepted work", "read_only", 0,
            "while_turn_id", "", "armed", 0), NULL, error, sizeof(error)) == 0);
    size_t length = success || closing ? 1024u * 1024u : 64u;
    char *text = malloc(length + 1u);
    assert(text);
    memset(text, 'x', length);
    memcpy(text, "native transcript native-capacity-secret ", 40u);
    for (size_t i = 40u; i + 2u <= length; i += 2u) memcpy(text + i, "ž", 2u);
    text[length] = '\0';
    for (unsigned int i = 0u; i < 5u; ++i) {
        json_t *event = json_pack("{s:s,s:s,s:s,s:{s:s,s:s,s:s}}",
            "connection_id", "0123456789abcdef0123456789abcdef", "provider", "default",
            "model", "fixture", "event", "type", "voice_transcript", "speaker", "user",
            "text", text);
        assert(event && snag_session_commit(&app.session, "voice_event", event,
            NULL, error, sizeof(error)) == 0);
        if (!i) {
            json_t *old = json_pack("{s:s,s:s,s:s,s:{s:s,s:s,s:I,s:{s:s}}}",
                "connection_id", "0123456789abcdef0123456789abcdef", "provider", "default",
                "model", "fixture", "event", "type", "voice_response",
                "operation", "interface_compacted", "source_as_of_seq", (json_int_t)999999,
                "summary", "text", "Earlier partial interface summary");
            assert(old && snag_session_commit(&app.session, "voice_event", old,
                NULL, error, sizeof(error)) == 0);
        }
    }
    free(text);
    bool handled = false;
    unsigned int rounds = mode == MODEL_VOICE_NATIVE_SUMMARY ? 2u : 1u;
    uint64_t covered = 0u, tail_seq = 0u;
    for (unsigned int round = 0u; round < rounds; ++round) {
        struct snag_provider_failure failure = {.http_status = 400,
            .code = "context_length_exceeded", .type = "invalid_request_error"};
        assert(snag_app_voice_fixture_failure(&app, NULL, &failure) == SNAG_VOICE_CAPACITY);
        uint64_t previous_boundary = covered;
        covered = tail_seq = 0u;
        assert(snag_app_voice_service(&app) == 0 && app.voice);
        uint64_t deadline = snag_monotonic_ms() + 30000u;
        for (;;) {
            assert(snag_monotonic_ms() < deadline);
            assert(snag_app_voice_service(&app) == 0);
            if (!app.voice) {
                assert(!success && !closing);
                break;
            }
            assert(!snag_app_voice_fixture_capture_ready(&app));
            json_t *state = snag_app_voice_fixture_state(&app);
            assert(state);
            uint64_t through =
                (uint64_t)json_integer_value(json_object_get(state, "compacting_through_seq"));
            if (!covered && through) {
                covered = through + 1u;
                tail_seq = app.session.next_seq;
                assert(snag_session_commit(&app.session, "voice_event", json_pack(
                    "{s:s,s:s,s:s,s:{s:s,s:s,s:s}}", "connection_id",
                    "0123456789abcdef0123456789abcdef",
                    "provider", "default", "model", "fixture", "event", "type", "voice_transcript",
                    "speaker", "user", "text", "native tail after boundary"),
                    NULL, error, sizeof(error)) == 0);
            }
            uint64_t boundary =
                (uint64_t)json_integer_value(json_object_get(state, "covered_next_seq"));
            bool advanced = boundary != previous_boundary;
            assert(!advanced || (success && boundary == covered));
            json_decref(state);
            if (advanced) break;
            struct pollfd waiting = {.fd = received[0], .events = POLLIN};
            if (closing && poll(&waiting, 1u, 0) == 1) {
                char ready;
                assert(read(received[0], &ready, 1u) == 1 && ready == 'R');
                if (mode == MODEL_VOICE_NATIVE_CANCEL_DONE) {
                    assert(write(release[1], "T", 1u) == 1);
                    while (!snag_app_voice_fixture_request_done(&app)) {
                        assert(snag_monotonic_ms() < deadline);
                        (void)snag_sleep_ms(1u);
                    }
                }
                break;
            }
            (void)snag_sleep_ms(1u);
        }
        if (success) {
            struct snag_credential replacement = {.value = "next-native-secret",
                .len = sizeof("next-native-secret") - 1u, .root_fd = -1};
            assert(snag_app_voice_fixture_restart(&app, &replacement) == 0);
            snag_credential_clear(&replacement);
            json_t *state = snag_app_voice_fixture_state(&app);
            assert(state && json_is_false(json_object_get(state, "muted")));
            assert(strcmp(snag_json_string(state, "connection_id"),
                "0123456789abcdef0123456789abcdef"));
            json_decref(state);
            assert(snag_app_voice_service(&app) == 0);
            json_t *summary = snag_app_voice_fixture_observation(&app);
            assert(summary &&
                !strcmp(snag_json_string(summary, "kind"), "session_history_summary"));
            assert((uint64_t)json_integer_value(json_object_get(summary, "seq")) == covered - 1u);
            assert(strstr(snag_json_string(summary, "text"), queue));
            assert(!strstr(snag_json_string(summary, "text"), "native-capacity-secret"));
            assert(!strstr(snag_json_string(summary, "text"), "next-native-secret"));
            assert(json_integer_value(json_object_get(summary, "length")) ==
                (json_int_t)json_string_length(json_object_get(summary, "text")));
            json_decref(summary);
            assert(snag_app_voice_service(&app) == 0);
            json_t *tail = snag_app_voice_fixture_observation(&app);
            assert(tail && (uint64_t)json_integer_value(json_object_get(tail, "seq")) == tail_seq);
            assert(strstr(snag_json_string(tail, "text"), "native tail after boundary"));
            json_decref(tail);
            bool fresh = false;
            for (unsigned int i = 0u; i < 100u && !fresh; ++i) {
                assert(snag_app_voice_service(&app) == 0);
                json_t *packet = snag_app_voice_fixture_observation(&app);
                if (packet) {
                    assert((uint64_t)json_integer_value(json_object_get(packet, "seq")) > tail_seq);
                    json_decref(packet);
                }
                json_t *context = snag_app_voice_fixture_context(&app);
                fresh = json_is_true(json_object_get(context, "history_complete"));
                json_decref(context);
            }
            assert(fresh);
        }
    }
    assert(app.session.pending_queue_count == 1u && app.session.usage_totals.responses == 0u);
    assert(!strcmp(app.session.pending_queue[0].queue_id, queue));
    uint64_t stopping = snag_monotonic_ms();
    /* The restoration fixture has no physical worker to finish on off. */
    if (success) snag_app_voice_close(&app);
    assert(snag_app_voice_command(&app, "/voice off", &handled) == 0 && handled && !app.voice);
    assert(snag_monotonic_ms() - stopping < 1000u);
    if (closing) {
        if (mode == MODEL_VOICE_NATIVE_CANCEL) assert(write(release[1], "X", 1u) == 1);
        assert(close(received[0]) == 0 && close(release[1]) == 0);
    }
    struct native_capacity_records seen = {0};
    assert(snag_session_each_event(&app.session, native_capacity_record, &seen,
        error, sizeof(error)) == 0);
    assert(seen.adopted_count == (success ? rounds : 0u) && seen.settled >= 2u);
    assert(seen.waiting == rounds);
    assert(seen.tools == (mode == MODEL_VOICE_NATIVE_TOOL));
    assert(seen.discarded == (mode == MODEL_VOICE_NATIVE_CANCEL_DONE));
    if (mode == MODEL_VOICE_NATIVE_OVERSIZED) {
        assert(seen.largest_output == SNAG_MAX_PUBLIC_ITEM -
            strlen("native-capacity-secret") + strlen("<redacted:secret>"));
    }
    if (success) {
        struct snag_journal_cursor cursor = {0};
        while (cursor.next_seq < covered) {
            assert(snag_session_each_event_forward(&app.session, &cursor,
                SNAG_JOURNAL_PAGE_BYTES, native_capacity_prefix, &covered,
                error, sizeof(error)) == 0);
        }
        assert(cursor.next_seq == covered);
        assert(json_integer_value(json_object_get(seen.adopted, "covered_offset")) ==
            cursor.offset);
        assert(!strcmp(snag_json_string(seen.adopted, "covered_sha256"), cursor.prev_sha256));
    }
    json_decref(seen.adopted);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_ui_free(&app.ui);
    snag_config_free(&config);
    stop_server(&server);
}

static void
test_voice_observation_cursor(void)
{
    char path[4096], error[256];
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-observe-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    struct snag_config config;
    snag_config_init(&config);
    assert(snag_secret_source_parse(&config.providers[0].api_key,
        "\"voice-observation-secret\"", NULL, error, sizeof(error)) == 0);
    struct app_state app = {.config = &config};
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "default", "fixture", "medium",
        error, sizeof(error)) == 0);
    assert(snag_ui_init(&app.ui) == 0);
    const char *archive_goal = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    uint64_t archive_seq = app.session.next_seq;
    json_t *archive[] = {
        json_pack("{s:s,s:s}", "goal_id", archive_goal, "prompt", "Earlier objective"),
        json_pack("{s:s}", "goal_id", archive_goal),
        json_pack("{s:s,s:s,s:s,s:{s:s,s:s,s:s,s:s}}", "connection_id", archive_goal,
            "provider", "default", "model", "fixture", "event", "type", "voice_transcript",
            "speaker", "user", "item_id", "old-input",
            "text", "Correction voice-observation-secret"),
        json_pack("{s:s,s:s,s:s,s:{s:s,s:s,s:s,s:s}}", "connection_id", archive_goal,
            "provider", "default", "model", "fixture", "event", "type", "voice_transcript",
            "speaker", "assistant", "item_id", "old-reply", "text", "Correction understood"),
        json_pack("{s:s,s:s,s:s,s:{s:s,s:s,s:s,s:s,s:{s:b,s:s}}}", "connection_id", archive_goal,
            "provider", "default", "model", "fixture", "event", "type", "voice_response",
            "operation", "interface_tool", "tool", "read_file", "call_id", "old-read",
            "result", "ok", 1, "model_text", "Earlier tool result")
    };
    const char *archive_types[] = {
        "goal_started", "goal_cancelled", "voice_event", "voice_event", "voice_event"
    };
    char *archive_expected[5];
    for (size_t i = 0u; i < 5u; ++i) {
        json_t *safe = json_deep_copy(archive[i]);
        assert(safe);
        if (i == 2u) {
            assert(json_object_set_new(json_object_get(safe, "event"), "text",
                json_string("Correction <redacted:secret>")) == 0);
        }
        archive_expected[i] = snag_app_history_data(archive_seq + i, archive_types[i], safe);
        json_decref(safe);
        assert(archive_expected[i] && snag_session_commit(&app.session, archive_types[i],
            archive[i], NULL, error, sizeof(error)) == 0);
    }
    char session_id[33];
    strcpy(session_id, app.session.id);
    snag_session_close(&app.session);
    snag_session_init(&app.session);
    assert(snag_session_open(&app.store, &app.session, session_id, error, sizeof(error)) == 0);
    json_t *notices = json_array();
    assert(notices && snag_app_voice_fixture(&app, notices, false) == 0);
    json_decref(notices);
    for (size_t i = 0u; i < 5u; ++i) {
        assert(snag_app_voice_service(&app) == 0 && app.voice);
        json_t *restored = snag_app_voice_fixture_observation(&app);
        if (!restored) fprintf(stderr, "native archive omitted %s at %llu\n", archive_types[i],
            (unsigned long long)(archive_seq + i));
        assert(restored && !strcmp(snag_json_string(restored, "session_id"), session_id));
        assert((uint64_t)json_integer_value(json_object_get(restored, "seq")) == archive_seq + i);
        assert(!strcmp(snag_json_string(restored, "event_type"), archive_types[i]));
        assert(json_is_true(json_object_get(restored, "complete")));
        assert(!strcmp(snag_json_string(restored, "text"), archive_expected[i]));
        json_decref(restored);
        free(archive_expected[i]);
    }
    assert(!app.session.active_turn && !app.session.pending_queue_count);
    assert(snag_app_voice_service(&app) == 0);
    assert(!snag_app_voice_fixture_observation(&app));
    json_t *initial_context = snag_app_voice_fixture_context(&app);
    assert(initial_context && json_is_true(json_object_get(initial_context, "history_complete")));
    json_decref(initial_context);
    assert(snag_session_commit(&app.session, "voice_event", json_pack(
        "{s:s,s:s,s:s,s:{s:s,s:s,s:s,s:s}}", "connection_id", "0123456789abcdef0123456789abcdef",
        "provider", "default", "model", "fixture", "event", "type", "voice_transcript",
        "speaker", "user", "item_id", "live-input", "text", "Current native speech"),
        NULL, error, sizeof(error)) == 0);
    assert(snag_app_voice_service(&app) == 0 && !snag_app_voice_fixture_observation(&app));
    uint64_t late_seq = app.session.next_seq;
    assert(snag_session_commit(&app.session, "voice_event", json_pack(
        "{s:s,s:s,s:s,s:{s:s,s:s,s:s,s:s}}", "connection_id", archive_goal,
        "provider", "default", "model", "fixture", "event", "type", "voice_response",
        "operation", "interface_reply", "call_id", "old-call",
        "text", "Late voice-observation-secret"), NULL, error, sizeof(error)) == 0);
    assert(snag_app_voice_service(&app) == 0);
    json_t *late = snag_app_voice_fixture_observation(&app);
    assert(late && (uint64_t)json_integer_value(json_object_get(late, "seq")) == late_seq);
    const char *late_text = snag_json_string(late, "text");
    assert(late_text && strstr(late_text, archive_goal) && strstr(late_text, "<redacted:secret>"));
    assert(!strstr(late_text, "voice-observation-secret"));
    json_decref(late);
    assert(!app.session.active_turn && !app.session.pending_queue_count);
    char prompt[6100];
    for (size_t i = 0; i < 6000u; i += 2u) memcpy(prompt + i, "λ", 2u);
    strcpy(prompt + 6000u, "voice-observation-secret");
    const char *goal = "0123456789abcdef0123456789abcdef";
    json_t *data = json_pack("{s:s,s:s}", "goal_id", goal, "prompt", prompt);
    uint64_t seq = app.session.next_seq;
    strcpy(prompt + 6000u, "<redacted:secret>");
    json_t *redacted = json_deep_copy(data);
    assert(redacted && json_object_set_new(redacted, "prompt", json_string(prompt)) == 0);
    char *expected = snag_app_history_data(seq, "goal_started", redacted);
    json_decref(redacted);
    assert(expected && snag_session_commit(&app.session, "goal_started", data,
        NULL, error, sizeof(error)) == 0);
    struct snag_buf joined = {.max = SNAG_MAX_EVENT_LINE};
    bool complete = false;
    size_t packets = 0u;
    while (!complete && packets < 16u) {
        assert(snag_app_voice_service(&app) == 0 && app.voice);
        /* A pending packet survives repeated owner service without overwrite. */
        assert(snag_app_voice_service(&app) == 0 && app.voice);
        json_t *packet = snag_app_voice_fixture_observation(&app);
        assert(packet && !strcmp(snag_json_string(packet, "session_id"), app.session.id));
        assert((uint64_t)json_integer_value(json_object_get(packet, "seq")) == seq);
        assert((size_t)json_integer_value(json_object_get(packet, "offset")) == joined.len);
        const char *text = snag_json_string(packet, "text");
        assert(text && strlen(text) <= 1024u && snag_text_valid(text, 1u, 1024u));
        assert(snag_buf_append(&joined, text, strlen(text)) == 0);
        complete = json_is_true(json_object_get(packet, "complete"));
        json_decref(packet);
        ++packets;
    }
    assert(complete && packets > 1u && snag_buf_terminate(&joined) == 0);
    assert(!strcmp((char *)joined.data, expected));
    free(expected);
    snag_buf_free(&joined);
    assert(snag_app_voice_service(&app) == 0 && !snag_app_voice_fixture_observation(&app));
    assert(snag_session_commit(&app.session, "effort_changed",
        json_pack("{s:s,s:s}", "old_effort", "medium", "new_effort", "high"),
        NULL, error, sizeof(error)) == 0);
    assert(snag_app_voice_service(&app) == 0);
    json_t *packet = snag_app_voice_fixture_observation(&app);
    assert(packet && !strcmp(snag_json_string(packet, "event_type"), "effort_changed"));
    json_decref(packet);
    unsigned int missing = voice_fixture_control_changes(&app, true);
    assert(!app.session.active_turn && !app.session.pending_queue_count);
    char *padding = malloc(1024u * 1024u + 1u);
    assert(padding);
    memset(padding, 'x', 1024u * 1024u);
    padding[1024u * 1024u] = '\0';
    for (unsigned int i = 0u; i < 4u; ++i) {
        assert(snag_session_commit(&app.session, "voice_event", json_pack(
            "{s:s,s:s,s:s,s:{s:s,s:s}}", "connection_id",
            "0123456789abcdef0123456789abcdef", "provider", "default", "model", "fixture",
            "event", "type", "voice_usage", "padding", padding), NULL, error, sizeof(error)) == 0);
    }
    free(padding);
    json_t *started = json_pack("{s:{s:s,s:s,s:n,s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:b},"
        "s:s,s:b,s:o,s:n,s:n,s:s,s:s,s:I,s:s}",
        "config", "capability_version", SNAJPAGENT_CAPABILITY_VERSION, "effort", "high",
        "max_output_tokens", "model", "fixture", "provider", "default",
        "profile_id", SNAJPAGENT_PROFILE_ID, "prompt_schema", 1, "replay_schema", 1,
        "tool_schema", 1, "max_parallel_commands", 4, "parallel_tool_calls", 1,
        "input_kind", "direct", "read_only", 0, "instructions", json_array(),
        "queue_id", "queue_seq", "text", "voice-observation-secret",
        "turn_id", "fedcba9876543210fedcba9876543210", "turn_number", (json_int_t)1,
        "cwd", app.session.cwd);
    assert(started && snag_session_commit(&app.session, "turn_started", started,
        NULL, error, sizeof(error)) == 0);
    snag_app_voice_event(&app, "turn_started", NULL);
    assert(snag_app_voice_service(&app) == 0 && app.voice);
    json_t *context = snag_app_voice_fixture_context(&app);
    assert(context && !strcmp(snag_json_string(context, "active_task"), "<redacted:secret>"));
    assert(json_is_false(json_object_get(context, "history_complete")));
    json_decref(context);
    /* No new event/utterance is needed to finish the remaining historical work. */
    uint64_t current_seq = app.session.next_seq;
    assert(snag_app_voice_service(&app) == 0 && app.voice);
    context = snag_app_voice_fixture_context(&app);
    assert(context && !strcmp(snag_json_string(context, "active_task"), "<redacted:secret>"));
    assert(json_is_true(json_object_get(context, "history_complete")));
    assert((uint64_t)json_integer_value(json_object_get(context,
        "history_as_of_seq")) == current_seq - 1u);
    assert(app.session.next_seq == current_seq);
    json_decref(context);
    assert(app.session.active_turn && !strcmp(app.session.active_prompt,
        "voice-observation-secret"));
    bool complete_turn = false;
    for (size_t i = 0u; !complete_turn && i < 16u; ++i) {
        assert(snag_app_voice_service(&app) == 0);
        packet = snag_app_voice_fixture_observation(&app);
        assert(packet && !strcmp(snag_json_string(packet, "event_type"), "turn_started"));
        complete_turn = json_is_true(json_object_get(packet, "complete"));
        json_decref(packet);
    }
    assert(complete_turn);
    missing += voice_fixture_model_changes(&app, true);
    assert(missing == 0u);
    snag_app_voice_close(&app);
    snag_ui_free(&app.ui);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_config_free(&config);
}

static void
test_voice_read_tools(void)
{
    char path[4096], file[4096], error[256];
    const char *tmp = getenv("TMPDIR");
    struct app_state app = {0};
    struct snag_config config;
    struct snag_response_item call = {.kind = SNAG_ITEM_TOOL_CALL};
    json_t *result = NULL;

    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-read-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    assert(snprintf(file, sizeof(file), "%s/AGENTS.md", path) > 0);
    FILE *out = fopen(file, "w");
    assert(out && fputs("Existing project instructions.\n", out) >= 0 && fclose(out) == 0);
    snag_config_init(&config);
    app.config = &config;
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "default",
        "fixture", "medium", error, sizeof(error)) == 0);
    uint64_t seq = app.session.next_seq;
    json_t *tools = snag_app_voice_tools();
    assert(tools && json_array_size(tools) == 11u);
    bool ui_input_tool = false;
    for (size_t i = 0u; i < json_array_size(tools); ++i) {
        const json_t *tool = json_array_get(tools, i);
        struct snag_response_graph graph = {0};
        struct snag_graph_decision decision;
        assert(snag_response_graph_set_provider_id(&graph, "voice_tools") == 0);
        assert(snag_response_graph_add_call(&graph, "tool_item", "tool_call",
            snag_json_string(tool, "name"), json_object()) == 0);
        assert(snag_response_graph_classify(&graph, &decision, error, sizeof(error)) == 0);
        assert(decision.outcome == SNAG_GRAPH_CALLS && decision.call_count == 1u);
        snag_response_graph_free(&graph);
        if (strcmp(snag_json_string(tool, "name"), "ui_input")) continue;
        assert(!ui_input_tool);
        ui_input_tool = true;
        const json_t *parameters = json_object_get(tool, "parameters");
        const json_t *properties = json_object_get(parameters, "properties");
        assert(json_object_size(properties) == 1u && json_object_get(properties, "text"));
        assert(json_is_false(json_object_get(parameters, "additionalProperties")));
    }
    assert(ui_input_tool);
    json_t *input = json_pack("[{s:s,s:s},{s:s,s:s},{s:s,s:s}]",
        "role", "developer", "content", "instructions",
        "role", "developer", "content", "host context",
        "role", "user", "content", "spoken request");
    for (unsigned int kind = 0; kind < 3u; ++kind) {
        config.providers[0].auth = kind == 1u ? SNAG_AUTH_CHATGPT : SNAG_AUTH_API_KEY;
        config.providers[0].leading_instructions = kind == 2u;
        json_t *request = snag_context_interface_request(&app.session, &config.providers[0],
            "fixture", "medium", input, tools);
        assert(request && json_array_size(input) == 3u);
        assert(strlen(snag_json_string(request, "prompt_cache_key")) == SNAG_CACHE_KEY_LEN);
        char working_key[SNAG_CACHE_KEY_LEN + 1u], retained_key[SNAG_CACHE_KEY_LEN + 1u];
        snag_context_cache_key(&app.session, config.providers[0].name, "fixture", working_key);
        assert(strcmp(working_key, snag_json_string(request, "prompt_cache_key")));
        json_t *again = snag_context_interface_request(&app.session, &config.providers[0],
            "fixture", "medium", input, tools);
        assert(again && !strcmp(snag_json_string(request, "prompt_cache_key"),
            snag_json_string(again, "prompt_cache_key")));
        json_decref(again);
        snag_context_cache_key(&app.session, config.providers[0].name, "fixture", retained_key);
        assert(!strcmp(working_key, retained_key));
        assert(!strcmp(snag_json_string(request, "model"), "fixture"));
        assert(json_is_true(json_object_get(request, "stream")));
        if (kind == 1u) {
            assert(json_is_string(json_object_get(request, "instructions")));
            assert(!json_object_get(request, "truncation"));
        }
        if (kind == 2u) {
            json_t *normalized = json_object_get(request, "input");
            assert(json_array_size(normalized) == 2u);
            assert(!strcmp(snag_json_string(json_array_get(normalized, 0u), "role"), "system"));
        }
        json_decref(request);
    }
    /* Byte staging exhaustion is distinct from an unknown model token window. */
    char *large = malloc(SNAG_CONTEXT_MAX_REQUEST);
    assert(large);
    memset(large, 'x', SNAG_CONTEXT_MAX_REQUEST);
    json_t *large_input = json_pack("[{s:s,s:o}]", "role", "user",
        "content", json_stringn(large, SNAG_CONTEXT_MAX_REQUEST));
    free(large);
    assert(large_input);
    errno = 0;
    assert(!snag_context_interface_request(&app.session, &config.providers[0],
        "fixture", "medium", large_input, tools));
    assert(errno == EOVERFLOW);
    json_decref(large_input);
    json_decref(input);
    assert(!snag_context_read_tool_schema("exec_command"));
    assert(!snag_context_read_tool_schema("write_file"));
    assert(!snag_context_read_tool_schema(NULL));
    for (size_t i = 0; i < 4u; ++i) {
        const char *name = snag_json_string(json_array_get(tools, i), "name");
        json_t *shared = snag_context_read_tool_schema(name);
        assert(shared && json_equal(shared, json_array_get(tools, i)));
        json_decref(shared);
    }
    json_decref(tools);
    call.name = "get_cwd";
    call.arguments = json_object();
    assert(snag_app_voice_read(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "model_text"), path));
    json_decref(result);
    call.name = "inspect_session";
    assert(snag_app_voice_read(&app, &call, &result, error, sizeof(error)) == 0);
    const char *text = snag_json_string(result, "model_text");
    json_t *context = json_loads(text, 0, NULL);
    assert(context && !strcmp(snag_json_string(context, "cwd"), path));
    assert(!strcmp(snag_json_string(context, "session_id"), app.session.id));
    const json_t *paths = json_object_get(context, "instructions");
    bool found = false;
    for (size_t i = 0; i < json_array_size(paths); ++i) {
        if (!strcmp(json_string_value(json_array_get(paths, i)), file)) found = true;
    }
    assert(found);
    json_decref(context);
    json_decref(result);
    json_decref(call.arguments);
    call.name = "read_file";
    call.arguments = json_pack("{s:s}", "path", "AGENTS.md");
    assert(snag_app_voice_read(&app, &call, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "Existing project instructions."));
    json_decref(result);
    json_decref(call.arguments);
    call.name = "write_file";
    call.arguments = json_pack("{s:s,s:s}", "path", "AGENTS.md", "content", "changed");
    assert(snag_app_voice_read(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    json_decref(result);
    call.name = "exec_command";
    assert(snag_app_voice_read(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    json_decref(result);
    json_decref(call.arguments);
    call.name = "read_file";
    call.arguments = json_pack("{s:s}", "path", "AGENTS.md");
    assert(snag_app_voice_read(&app, &call, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "Existing project instructions."));
    assert(app.session.next_seq == seq && !app.session.pending_queue_count);
    json_decref(result);
    json_decref(call.arguments);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_config_free(&config);
}

static void
test_voice_queue_inspection(void)
{
    char path[4096], error[256], prompt[1028];
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-queue-view-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    struct snag_config config;
    snag_config_init(&config);
    config.read_agents_md = false;
    struct app_state app = {.config = &config};
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "default", "fixture", "medium",
        error, sizeof(error)) == 0);
    const char *ids[] = {"11111111111111111111111111111111", "22222222222222222222222222222222"};
    for (size_t i = 0u; i < 1023u; i += 3u) memcpy(prompt + i, "€", 3u);
    strcpy(prompt + 1023u, "tail");
    uint64_t queued[2];
    for (unsigned int i = 0u; i < 2u; ++i) {
        queued[i] = app.session.next_seq;
        assert(snag_session_commit(&app.session, "future_turn_queued",
            json_pack("{s:s,s:s,s:b,s:s,s:b}", "queue_id", ids[i],
                "text", i ? prompt : "first queued task", "read_only", i != 0u,
                "while_turn_id", "", "armed", 0), NULL, error, sizeof(error)) == 0);
    }
    struct snag_response_item call = {.name = "inspect_session"};
    for (unsigned int page = 0u; page < 3u; ++page) {
        uint64_t seq = app.session.next_seq;
        call.arguments = json_pack("{s:i,s:I}", "queue_limit", 1,
            "queue_after_seq", (json_int_t)(page ? queued[0] : 0u));
        json_t *result = NULL;
        assert(snag_app_voice_read(&app, &call, &result, error, sizeof(error)) == 0);
        assert(!strcmp(snag_json_string(result, "status"), "succeeded"));
        json_t *view = json_loads(snag_json_string(result, "model_text"), 0, NULL);
        json_t *entries = json_object_get(view, "queue");
        assert(view && json_array_size(entries) == 1u);
        const json_t *entry = json_array_get(entries, 0u);
        assert(!strcmp(snag_json_string(entry, "queue_id"), ids[page ? 1 : 0]));
        assert(json_is_true(json_object_get(view, "queue_more")) == !page);
        assert(json_integer_value(json_object_get(view, "queue_next_after_seq")) ==
            (json_int_t)queued[page ? 1 : 0]);
        assert(json_is_true(json_object_get(entry, "read_only")) == (page != 0u));
        if (page == 1u) {
            const char *preview = snag_json_string(entry, "text");
            assert(preview && strlen(preview) == 510u && !strncmp(preview, prompt, 510u));
            assert(json_is_true(json_object_get(entry, "text_truncated")));
            assert(json_integer_value(json_object_get(entry, "text_bytes")) == 1027);
        } else if (page == 2u) {
            assert(!strcmp(snag_json_string(entry, "text"), "changed queued task"));
            assert(!json_is_true(json_object_get(entry, "text_truncated")));
        }
        assert(app.session.next_seq == seq && !app.session.active_turn && !app.session.queue_armed);
        json_decref(view);
        json_decref(result);
        json_decref(call.arguments);
        if (page == 0u) {
            assert(snag_session_commit(&app.session, "future_turn_cancelled",
                json_pack("{s:[s],s:s}", "queue_ids", ids[0], "reason", "user"),
                NULL, error, sizeof(error)) == 0);
        } else if (page == 1u) {
            assert(snag_session_commit(&app.session, "future_turn_edited",
                json_pack("{s:s,s:s,s:b}", "queue_id", ids[1],
                    "text", "changed queued task", "read_only", 1),
                NULL, error, sizeof(error)) == 0);
        }
    }
    call.arguments = json_pack("{s:i}", "queue_limit", 0);
    json_t *result = NULL;
    assert(snag_app_voice_read(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    json_decref(result);
    json_decref(call.arguments);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_config_free(&config);
}

static void
test_voice_interface_read(void)
{
    struct app_state app = {0};
    struct snag_config config;
    struct snag_credential credential;
    struct local_server server;
    char path[4096], file[4096], error[512];
    const char *tmp = getenv("TMPDIR");

    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-interface-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    assert(snprintf(file, sizeof(file), "%s/AGENTS.md", path) > 0);
    int fd = open(file, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0 && write(fd, "unchanged\n", 10u) == 10);
    start_server(&server, MODEL_VOICE_READ, false, "/v1");
    (void)transport_connection(&config, &credential, server.endpoint);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    assert(snag_secret_source_parse(&config.providers[0].api_key,
        "\"transport-secret\"", NULL, error, sizeof(error)) == 0);
    app.config = &config;
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_ui_init(&app.ui) == 0);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "openai", "fixture",
        "medium", error, sizeof(error)) == 0);
    json_t *events = json_pack("[{s:s,s:s,s:s,s:s,s:s,s:s}]", "type", "voice_handoff",
        "input_id", "read_input", "response_id", "read_response", "call_id", "read_call",
        "transcript", "Which directory is this?", "request", "Read the session cwd.");
    assert(snag_app_voice_fixture(&app, events, false) == 0);
    json_decref(events);
    json_t *reply = NULL;
    uint64_t start = snag_monotonic_ms();
    while (!reply && snag_monotonic_ms() - start < 5000u) {
        assert(snag_app_voice_service(&app) == 0 && app.voice);
        reply = snag_app_voice_fixture_result(&app);
        if (!reply) snag_sleep_ms(10u);
    }
    assert(reply && !strcmp(snag_json_string(reply, "call_id"), "read_call"));
    assert(json_is_true(json_object_get(reply, "final")));
    assert(strstr(snag_json_string(reply, "text"), "transport"));
    assert(!app.session.pending_queue_count && !app.session.active_turn);
    char contents[10];
    assert(lseek(fd, 0, SEEK_SET) == 0 && read(fd, contents, sizeof(contents)) == 10);
    assert(!memcmp(contents, "unchanged\n", 10u));
    close(fd);
    json_decref(reply);
    snag_app_voice_close(&app);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_ui_free(&app.ui);
    snag_config_free(&config);
    snag_credential_clear(&credential);
    stop_server(&server);
}

static void
test_public_item_growth(void)
{
    struct app_state app = {.execute = true};
    snag_session_init(&app.session);
    assert(snag_ui_init(&app.ui) == 0);
    for (size_t i = 0; i < 40u; ++i) {
        char provider_id[32];
        char local_id[SNAG_ID_HEX_LEN + 1u];
        snprintf(provider_id, sizeof(provider_id), "public-%zu", i);
        assert(snag_app_stream_public(&app, i, SNAG_ITEM_ASSISTANT,
            SNAG_PHASE_FINAL_ANSWER, provider_id, "first", 5u) == 0);
        assert(app.partial_count == i + 1u && app.partial_capacity >= app.partial_count);
        struct partial_public_item *item = &app.partial[i];
        assert(item->graph_index == i && !strcmp(item->provider_item_id, provider_id));
        assert(snag_hex_is_lower(item->local_item_id, SNAG_ID_HEX_LEN));
        strcpy(local_id, item->local_item_id);
        if (i) assert(strcmp(local_id, app.partial[i - 1u].local_item_id));
        assert(snag_app_stream_public(&app, i, SNAG_ITEM_ASSISTANT,
            SNAG_PHASE_FINAL_ANSWER, provider_id, "second", 6u) == 0);
        assert(app.partial_count == i + 1u && !strcmp(local_id, item->local_item_id));
        assert(item->text.len == 11u && !memcmp(item->text.data, "firstsecond", 11u));
    }
    assert(snag_app_stream_public(&app, 40u, SNAG_ITEM_ASSISTANT,
        SNAG_PHASE_FINAL_ANSWER, "", "bad", 3u) < 0);
    assert(app.partial_count == 40u && app.partial_bytes == 440u);
    snag_app_reset_stream(&app);
    assert(app.partial_count == 0u && app.partial_bytes == 0u);
    assert(snag_app_stream_public(&app, 0u, SNAG_ITEM_ASSISTANT,
        SNAG_PHASE_FINAL_ANSWER, "after-reset", "new", 3u) == 0);
    assert(app.partial_count == 1u && app.partial[0].text.len == 3u);
    snag_app_clear_partial_public(&app);
    free(app.partial);
    snag_ui_free(&app.ui);
    snag_session_close(&app.session);
}

struct voice_close_records {
    unsigned int mode, settled, discarded, output, calls, closed, started, queued;
    unsigned int leaked;
    char queue[33];
};

static int
voice_interface_close_record(void *opaque, const struct snag_session *state, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t size)
{
    (void)state;
    (void)seq;
    (void)error;
    (void)size;
    struct voice_close_records *seen = opaque;
    if (!strcmp(type, "future_turn_queued")) {
        assert(!seen->queued++);
        assert(!strcmp(snag_json_string(data, "queue_id"), seen->queue));
    }
    if (strcmp(type, "voice_event")) return 0;
    const json_t *event = json_object_get(data, "event");
    const char *op = snag_json_string(event, "operation");
    if (!op) return 0;
    if (!strcmp(op, "interface_request_settled") || !strcmp(op, "interface_compaction_settled")) {
        ++seen->settled;
        const char *disposition = snag_json_string(event, "disposition");
        bool live = seen->mode == 5u || seen->mode == 7u;
        if (!disposition && (!live || seen->settled < 2u)) return 0;
        if (live) assert(!disposition);
        else assert(!strcmp(disposition, "discarded_on_voice_stop"));
        assert(!strcmp(snag_json_string(event, "call_id"), "first"));
        assert(!strcmp(snag_json_string(event, "status"), seen->mode ? "completed" : "cancelled"));
        assert(!strcmp(op, seen->mode == 3u ? "interface_compaction_settled" :
            "interface_request_settled"));
        const json_t *metrics = json_object_get(event, "metrics");
        const json_t *usage = json_object_get(metrics, "usage");
        assert(json_is_integer(json_object_get(metrics, "elapsed_ms")));
        assert(json_integer_value(json_object_get(metrics, "retries")) == 0);
        if (!seen->mode) {
            assert(json_is_null(json_object_get(metrics, "response_ready_ms")));
            assert(json_is_null(json_object_get(metrics, "first_text_ms")));
            assert(json_is_null(json_object_get(usage, "input_tokens")));
        } else {
            assert(json_is_integer(json_object_get(metrics, "response_ready_ms")));
            if (seen->mode == 2u) {
                assert(json_is_null(json_object_get(metrics, "first_text_ms")));
            } else {
                assert(json_integer_value(json_object_get(usage, "input_tokens")) == 7);
                assert(json_integer_value(json_object_get(usage, "cached_tokens")) == 4);
            }
        }
        if (disposition) ++seen->discarded;
    } else if (!strcmp(op, "interface_output_discarded") || !strcmp(op, "interface_output")) {
        assert(!strcmp(op, seen->mode == 5u || seen->mode == 7u ?
            "interface_output" : "interface_output_discarded"));
        const char *text = snag_json_string(event, "text");
        assert(text && strstr(text, "transport"));
        if (strstr(text, "transport-secret") || strstr(text, "old-voice-secret") ||
            strstr(text, "refreshed-voice-secret")) {
            fprintf(stderr, "rotated credential reached history in mode %u\n", seen->mode);
            ++seen->leaked;
        } else if (seen->mode == 1u || seen->mode >= 4u) {
            const char *first = strstr(text, "<redacted:secret>");
            assert(first);
            if (seen->mode >= 4u) assert(strstr(first + 1u, "<redacted:secret>"));
        }
        ++seen->output;
    } else if (!strcmp(op, "interface_tool_discarded")) {
        assert(!strcmp(snag_json_string(event, "tool_call_id"), "discarded_call"));
        assert(!strcmp(snag_json_string(json_object_get(event, "arguments"), "text"),
            "discarded <redacted:secret>"));
        ++seen->calls;
    } else if (!strcmp(op, "interface_closed")) {
        assert(!strcmp(snag_json_string(event, "call_id"), "first"));
        assert(!strcmp(snag_json_string(event, "queue_id"), seen->queue));
        ++seen->closed;
    } else if (!strcmp(op, "interface_tool_started")) {
        assert(!strcmp(snag_json_string(event, "tool_call_id"), "voice_call_first"));
        ++seen->started;
    } else {
        assert(strcmp(op, "interface_compacted"));
    }
    return 0;
}

static void
voice_credential_file(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0 && write(fd, value, strlen(value)) == (ssize_t)strlen(value));
    assert(close(fd) == 0);
}

static void
voice_credential_cache(struct app_state *app, const char *value)
{
    struct snag_auth_tokens tokens = {0};
    char error[256] = {0};
    assert(snag_auth_key(&tokens, value, error, sizeof(error)) == 0);
    strcpy(tokens.refresh_token, "voice-fixture-refresh");
    strcpy(tokens.credential.account_id, "voice-fixture-account");
    tokens.expires_at_ms = snag_time_ms() + 3600000u;
    assert(snag_auth_save(app->store.root_fd, &app->config->providers[0], &tokens,
        NULL, NULL, NULL, error, sizeof(error)) == 0);
    snag_auth_clear(&tokens);
}

static void
test_voice_close_settlement(void)
{
    unsigned int privacy_failures = 0u;
    for (unsigned int mode = 0u; mode < 8u; ++mode) {
        bool refreshed = mode >= 6u;
        bool rotated = mode == 4u || mode == 5u;
        bool live = mode == 5u || mode == 7u;
        struct app_state app = {0};
        struct snag_config config;
        struct snag_credential credential;
        struct local_server server;
        char path[4096], key[4096], error[256];
        const char *tmp = getenv("TMPDIR");
        assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-close-XXXXXX",
            tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
        int received[2], release[2];
        assert(pipe(received) == 0 && pipe(release) == 0);
        voice_request_ready_fd = received[1];
        voice_request_release_fd = release[0];
        start_server(&server, refreshed ? MODEL_VOICE_CLOSE_REFRESH :
            rotated ? MODEL_VOICE_CLOSE_CREDENTIAL :
            mode == 3u ? MODEL_VOICE_CLOSE_SUMMARY : MODEL_VOICE_CLOSE, false, "/v1");
        assert(close(received[1]) == 0 && close(release[0]) == 0);
        voice_request_ready_fd = voice_request_release_fd = -1;
        (void)transport_connection(&config, &credential, server.endpoint);
        config.providers[0].auth = SNAG_AUTH_API_KEY;
        if (rotated) {
            int n = snprintf(key, sizeof(key), "%s/credential", path);
            assert(n > 0 && (size_t)n < sizeof(key));
            voice_credential_file(key, "old-voice-secret");
        }
        if (refreshed) {
            config.providers[0].auth = SNAG_AUTH_CHATGPT;
            strcpy(config.providers[0].base_url, SNAG_CHATGPT_BASE);
            snag_secret_source_free(&config.providers[0].api_key);
            assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", server.endpoint, 1) == 0);
            assert(setenv("SNAJPAGENT_TEST_AUTH_BASE", "http://127.0.0.1:1", 1) == 0);
        } else {
            assert(snag_secret_source_parse(&config.providers[0].api_key,
                rotated ? key : "\"transport-secret\"", NULL, error, sizeof(error)) == 0);
        }
        app.config = &config;
        snag_store_init(&app.store);
        snag_session_init(&app.session);
        assert(snag_ui_init(&app.ui) == 0);
        assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
        assert(snag_session_create(&app.store, &app.session, path, "openai", "fixture",
            "medium", error, sizeof(error)) == 0);
        if (refreshed) voice_credential_cache(&app, "old-voice-secret");
        json_t *events = json_pack("[{s:s,s:s,s:s,s:s,s:s,s:s}]", "type", "voice_handoff",
            "input_id", "first", "response_id", "first", "call_id", "first",
            "transcript", "retained-first-utterance", "request", "first");
        assert(snag_app_voice_fixture(&app, events, false) == 0);
        json_decref(events);
        uint64_t started = snag_monotonic_ms();
        struct pollfd ready = {.fd = received[0], .events = POLLIN};
        char reply;
        if (mode >= 4u) {
            while (poll(&ready, 1u, 0) == 0 && snag_monotonic_ms() - started < 5000u) {
                assert(snag_app_voice_service(&app) == 0 && app.voice);
                snag_sleep_ms(1u);
            }
            assert(poll(&ready, 1u, 0) == 1 && read(received[0], &reply, 1u) == 1 && reply == 'R');
            if (refreshed) voice_credential_cache(&app, "refreshed-voice-secret");
            else voice_credential_file(key, "transport-secret");
            assert(write(release[1], "N", 1u) == 1);
        }
        while (poll(&ready, 1u, 0) == 0 && snag_monotonic_ms() - started < 5000u) {
            assert(snag_app_voice_service(&app) == 0 && app.voice);
            snag_sleep_ms(1u);
        }
        assert(poll(&ready, 1u, 0) == 1 && read(received[0], &reply, 1u) == 1 && reply == 'R');
        assert(close(received[0]) == 0 && app.session.pending_queue_count == 1u);
        struct voice_close_records seen = {.mode = mode};
        strcpy(seen.queue, app.session.pending_queue[0].queue_id);
        if (refreshed) voice_credential_cache(&app, "rotated-again-secret");
        else if (rotated) voice_credential_file(key, "rotated-again-secret");
        if (mode) {
            const char *action = mode == 2u ? "C" : "T";
            assert(write(release[1], action, 1u) == 1);
            while (!snag_app_voice_fixture_request_done(&app) &&
                snag_monotonic_ms() - started < 5000u) snag_sleep_ms(1u);
            assert(snag_app_voice_fixture_request_done(&app));
        }
        if (live) {
            assert(snag_app_voice_service(&app) == 0 && app.voice);
            json_t *result = snag_app_voice_fixture_result(&app);
            const char *text = snag_json_string(result, "text");
            assert(text);
            if (strstr(text, "transport-secret") || strstr(text, "old-voice-secret") ||
                strstr(text, "refreshed-voice-secret")) {
                fprintf(stderr, "rotated credential reached live reply\n");
                ++seen.leaked;
            }
            assert(strstr(text, "<redacted:secret>"));
            json_decref(result);
        }
        uint64_t closing = snag_monotonic_ms();
        snag_app_voice_close(&app);
        uint64_t elapsed = snag_monotonic_ms() - closing;
        if (!mode) assert(write(release[1], "X", 1u) == 1);
        assert(close(release[1]) == 0);
        stop_server(&server);
        assert(elapsed < 1000u && !app.voice && app.session.pending_queue_count == 1u);
        assert(!strcmp(app.session.pending_queue[0].queue_id, seen.queue));
        assert(!app.session.active_turn && app.session.usage_totals.responses == 0u);
        assert(snag_session_each_event(&app.session, voice_interface_close_record, &seen,
            error, sizeof(error)) == 0);
        assert(seen.settled == (mode == 3u ? 3u : 2u) && seen.discarded == !live);
        assert(seen.output == (mode == 1u || mode >= 3u) && seen.calls == (mode == 2u));
        assert(seen.closed == 1u && seen.started == 1u && seen.queued == 1u);
        if (seen.leaked) ++privacy_failures;
        snag_ui_free(&app.ui);
        snag_session_close(&app.session);
        snag_store_close(&app.store);
        snag_config_free(&config);
        if (refreshed) {
            assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
            assert(unsetenv("SNAJPAGENT_TEST_AUTH_BASE") == 0);
        }
    }
    assert(!privacy_failures);
}

static void
test_voice_independent_request(void)
{
    for (unsigned int mode = 0; mode < 3u; ++mode) {
        struct local_server server;
        struct app_state app = {0};
        struct snag_config config;
        struct snag_credential credential;
        struct snag_response_graph graph = {0};
        char path[4096], error[512];
        const char *tmp = getenv("TMPDIR");

        assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-request-XXXXXX",
            tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
        int received[2] = {-1, -1};
        if (mode == 2u) {
            assert(pipe(received) == 0);
            voice_request_ready_fd = received[1];
        }
        start_server(&server, mode == 2u ? MODEL_VOICE_REQUEST_WAIT :
            mode == 1u ? MODEL_CREATE_TYPELESS : MODEL_VOICE_REQUEST, false, "/v1");
        if (mode == 2u) {
            assert(close(received[1]) == 0);
            voice_request_ready_fd = -1;
        }
        (void)transport_connection(&config, &credential, server.endpoint);
        config.providers[0].auth = SNAG_AUTH_API_KEY;
        assert(snag_secret_source_parse(&config.providers[0].api_key,
            "\"transport-secret\"", NULL, error, sizeof(error)) == 0);
        app.config = &config;
        snag_store_init(&app.store);
        snag_session_init(&app.session);
        assert(snag_ui_init(&app.ui) == 0);
        assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
        assert(snag_session_create(&app.store, &app.session, path, config.providers[0].name,
            "fixture", "medium", error, sizeof(error)) == 0);
        json_t *notices = json_pack("[{s:s,s:s,s:s,s:s}]", "type", "voice_transcript",
            "speaker", "user", "item_id", "input", "text", "still listening");
        assert(snag_app_voice_fixture(&app, notices, false) == 0);
        json_decref(notices);
        json_t *request = request_with_marker("get_cwd");
        assert(snag_app_voice_request_start(&app, request, error, sizeof(error)) == 0);
        assert(snag_app_voice_request_start(&app, request, error, sizeof(error)) < 0);
        json_decref(request);
        /* Servicing audio notices does not wait for the independent response. */
        uint64_t begin = snag_monotonic_ms();
        assert(snag_app_voice_service(&app) == 0 && app.voice);
        assert(snag_monotonic_ms() - begin < 1000u);
        if (mode == 2u) {
            /* Closing immediately after thread creation could cancel before
             * any network I/O. Exercise a request already waiting for a reply. */
            struct pollfd ready = {.fd = received[0], .events = POLLIN};
            assert(poll(&ready, 1u, 5000) == 1 && (ready.revents & POLLIN));
            char reply;
            assert(read(received[0], &reply, 1u) == 1 && reply == 'R');
            assert(close(received[0]) == 0);
            begin = snag_monotonic_ms();
            snag_app_voice_close(&app);
            assert(snag_monotonic_ms() - begin < 1000u && !app.voice);
        } else {
            int ready = 0, outcome = 0;
            json_t *metrics = NULL;
            uint64_t seq = app.session.next_seq;
            while (!ready && snag_monotonic_ms() - begin < 5000u) {
                ready = snag_app_voice_request_take(&app, &graph, &outcome, &metrics,
                    error, sizeof(error));
                assert(ready >= 0);
                if (!ready) snag_sleep_ms(10u);
            }
            assert(ready == 1 && app.voice && app.session.next_seq == seq);
            assert(json_is_object(metrics));
            assert(json_is_integer(json_object_get(metrics, "elapsed_ms")));
            assert(json_integer_value(json_object_get(metrics, "retries")) == 0);
            const json_t *usage = json_object_get(metrics, "usage");
            if (mode == 1u) {
                assert(outcome < 0 && strstr(error, "Responses event has no type"));
                assert(!strstr(error, "private-value"));
                assert(json_is_null(json_object_get(metrics, "response_ready_ms")));
                assert(json_is_null(json_object_get(metrics, "first_text_ms")));
                assert(json_is_null(json_object_get(usage, "input_tokens")));
                assert(!json_object_get(usage, "cached_tokens"));
            } else {
                assert(outcome == 0 && graph.count == 1u);
                assert(!strcmp(graph.provider_response_id, "resp_transport"));
                json_int_t elapsed = json_integer_value(json_object_get(metrics, "elapsed_ms"));
                json_int_t accepted =
                    json_integer_value(json_object_get(metrics, "response_ready_ms"));
                json_int_t text = json_integer_value(json_object_get(metrics, "first_text_ms"));
                assert(accepted >= 200 && accepted <= text && text <= elapsed);
                assert(json_integer_value(json_object_get(usage, "input_tokens")) == 7);
                assert(json_integer_value(json_object_get(usage, "cached_tokens")) == 4);
                assert(json_integer_value(json_object_get(usage, "output_tokens")) == 2);
            }
            snag_response_graph_free(&graph);
            json_decref(metrics);
            assert(snag_app_voice_request_take(&app, &graph, &outcome, NULL,
                error, sizeof(error)) < 0);
            snag_app_voice_close(&app);
        }
        snag_session_close(&app.session);
        snag_store_close(&app.store);
        snag_ui_free(&app.ui);
        snag_config_free(&config);
        snag_credential_clear(&credential);
        stop_server(&server);
    }
}

static void
test_voice_session_controls(void)
{
    struct app_state app = {0};
    struct snag_config config;
    char path[4096], error[512] = {0}, id[33], first[33], session_id[33];
    const char *tmp = getenv("TMPDIR");
    const char *turn = "0123456789abcdef0123456789abcdef";
    json_t *result = NULL;

    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-controls-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    snag_config_init(&config);
    app.config = &config;
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_ui_init(&app.ui) == 0);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "openai", "fixture",
        "medium", error, sizeof(error)) == 0);
    strcpy(session_id, app.session.id);
    json_t *source = json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s}",
        "connection_id", turn, "input_id", "spoken_input", "response_id", "spoken_response",
        "call_id", "spoken_call", "provider", "openai", "model", "fixture",
        "transcript", "Změň prosím tento úkol.", "request", "Change the current task.");
    assert(source);
    uint64_t seq = app.session.next_seq;
    assert(snag_app_voice_submit(&app, source, turn, id, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    assert(app.session.next_seq == seq && !id[0]);
    json_decref(result);

    json_t *started = json_pack("{s:{s:s,s:s,s:n,s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:b},"
        "s:s,s:b,s:o,s:n,s:n,s:s,s:s,s:I,s:s}",
        "config", "capability_version", SNAJPAGENT_CAPABILITY_VERSION, "effort", "medium",
        "max_output_tokens", "model", "fixture", "provider", "openai",
        "profile_id", SNAJPAGENT_PROFILE_ID, "prompt_schema", 1, "replay_schema", 1,
        "tool_schema", 1, "max_parallel_commands", 4, "parallel_tool_calls", 1,
        "input_kind", "direct", "read_only", 0, "instructions", json_array(),
        "queue_id", "queue_seq", "text", "Existing task", "turn_id", turn,
        "turn_number", (json_int_t)1, "cwd", app.session.cwd);
    assert(started && snag_session_commit(&app.session, "turn_started", started,
        NULL, error, sizeof(error)) == 0);
    app.ui.input_received_ms = 123u;
    assert(snag_app_voice_submit(&app, source, turn, id, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "succeeded"));
    assert(app.steering_requested && app.session.pending_steering_count == 1u);
    assert(!app.session.pending_queue_count && app.ui.input_received_ms == 123u);
    assert(strstr(app.session.pending_steering[0].text, "Změň prosím tento úkol."));
    assert(strstr(app.session.pending_steering[0].text, "derived context"));
    assert(app.session.pending_steering[0].received_ms != 123u);
    strcpy(first, id);
    json_decref(result);
    seq = app.session.next_seq;
    app.steering_requested = false;
    assert(snag_app_voice_submit(&app, source, turn, id, &result, error, sizeof(error)) == 0);
    assert(!strcmp(first, id) && !app.steering_requested && app.session.next_seq == seq);
    json_decref(result);
    assert(json_object_set_new(source, "transcript", json_string("Different input")) == 0);
    assert(snag_app_voice_submit(&app, source, turn, id, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    assert(app.session.next_seq == seq && app.session.pending_steering_count == 1u);
    json_decref(result);
    assert(json_object_set_new(source, "transcript", json_string("Změň prosím tento úkol.")) == 0);
    assert(json_object_set_new(source, "input_id", json_string("later_input")) == 0);
    app.session.steering_deferred = true;
    assert(snag_app_voice_submit(&app, source, turn, id, &result, error, sizeof(error)) == 0);
    assert(!app.steering_requested && app.session.pending_steering_count == 2u);
    assert(strstr(snag_json_string(result, "model_text"), "next permitted boundary"));
    json_decref(result);
    app.session.steering_deferred = false;

    assert(snag_app_voice_submit(&app, source, "queue", id, &result, error, sizeof(error)) == 0);
    assert(app.session.pending_queue_count == 1u && app.session.queue_armed);
    assert(strstr(snag_json_string(result, "model_text"), "queued"));
    json_decref(result);
    strcpy(first, id);
    seq = app.session.next_seq;
    assert(snag_app_voice_submit(&app, source, "queue", id, &result, error, sizeof(error)) == 0);
    assert(!strcmp(first, id) && app.session.next_seq == seq);
    json_decref(result);
    assert(snag_session_commit(&app.session, "future_turn_cancelled",
        json_pack("{s:[s],s:s}", "queue_ids", id, "reason", "user"),
        NULL, error, sizeof(error)) == 0);
    assert(snag_app_voice_submit(&app, source, "queue", id, &result, error, sizeof(error)) == 0);
    assert(!app.session.pending_queue_count && !app.session.queue_armed);
    json_decref(result);

    seq = app.session.next_seq;
    assert(snag_app_voice_interrupt(&app, "stale_turn", &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    assert(!app.interrupt_requested && app.session.next_seq == seq);
    json_decref(result);
    assert(snag_app_voice_interrupt(&app, turn, &result, error, sizeof(error)) == 0);
    assert(app.interrupt_requested && app.session.cancel_requested);
    json_decref(result);
    seq = app.session.next_seq;
    assert(snag_app_voice_interrupt(&app, turn, &result, error, sizeof(error)) == 0);
    assert(app.session.next_seq == seq);
    json_decref(result);

    snag_session_close(&app.session);
    assert(snag_session_open(&app.store, &app.session, session_id, error, sizeof(error)) == 0);
    assert(app.session.pending_steering_count == 2u && app.session.cancel_requested);
    seq = app.session.next_seq;
    assert(snag_app_voice_submit(&app, source, turn, id, &result, error, sizeof(error)) == 0);
    assert(app.session.next_seq == seq && app.session.pending_steering_count == 2u);
    json_decref(result);
    json_decref(source);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_ui_free(&app.ui);
    snag_config_free(&config);
}

static void
test_voice_output_tool(void)
{
    char path[4096], error[256];
    const char *tmp = getenv("TMPDIR");
    struct app_state app = {0};
    struct snag_config config;
    struct snag_response_item call = {.kind = SNAG_ITEM_TOOL_CALL, .name = "voice_output"};
    json_t *result = NULL;
    snag_config_init(&config);
    app.config = &config;
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-output-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    assert(snag_ui_init(&app.ui) == 0);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "default",
        "fixture", "medium", error, sizeof(error)) == 0);
    call.arguments = json_object();
    uint64_t seq = app.session.next_seq;
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "\"ready\":false"));
    json_decref(result);
    assert(json_object_set_new(call.arguments, "text", json_string("The build finished.")) == 0);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "unavailable"));
    assert(!app.voice && app.session.next_seq == seq);
    json_decref(result);
    assert(snag_app_voice_fixture(&app, NULL, false) == 0);
    assert(snag_app_voice_fixture_mute(&app) == 0);
    app.session.active_read_only = true;
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "Read-only"));
    assert(app.session.next_seq == seq && !snag_app_voice_fixture_result(&app));
    json_decref(result);
    app.session.active_read_only = false;
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "queued; playback is not confirmed"));
    json_decref(result);
    seq = app.session.next_seq;
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "not accepted"));
    assert(app.session.next_seq == seq && !app.session.pending_queue_count);
    json_decref(result);
    result = snag_app_voice_fixture_result(&app);
    assert(json_is_true(json_object_get(result, "standalone")));
    assert(!strcmp(snag_json_string(result, "text"), "Model output:\nThe build finished."));
    json_decref(result);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    json_decref(result);
    snag_app_voice_close(&app);
    assert(!app.voice);
    json_decref(call.arguments);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_ui_free(&app.ui);
    snag_config_free(&config);
}

static void test_voice_owner_mute(void)
{
    struct app_state app={0};
    assert(snag_app_voice_fixture(&app,NULL,false)==0);
    assert(snag_app_voice_fixture_mute(&app)==0);
    /* The close fixture separately tests durable notice draining. This owner
     * fixture needs a real private session for that same close path. */
    char path[4096],error[256];const char *tmp=getenv("TMPDIR");
    assert(snprintf(path,sizeof(path),"%s/snajpagent-voice-mute-XXXXXX",tmp?tmp:"/tmp")>0 && mkdtemp(path));
    struct snag_config config;snag_config_init(&config);app.config=&config;
    snag_store_init(&app.store);snag_session_init(&app.session);assert(snag_ui_init(&app.ui)==0);
    assert(snag_store_open(&app.store,path,error,sizeof(error))==0);
    assert(snag_session_create(&app.store,&app.session,path,"default","fixture","medium",error,sizeof(error))==0);
    snag_app_voice_close(&app);snag_session_close(&app.session);snag_store_close(&app.store);
    snag_ui_free(&app.ui);snag_config_free(&config);
}

static void
test_goal_tool_manipulates_unfinished_goals(void)
{
    struct snag_config config;
    struct app_state app = {0};
    struct snag_response_item call = {0};
    json_t *result = NULL;
    char error[256] = {0};

    snag_config_init(&config);
    app.config = &config;
    call.kind = SNAG_ITEM_TOOL_CALL;
    call.name = "update_goal";

    /* A locked goal is frozen for the model: the operator lock, not the status,
     * is what keeps the model from changing the objective, and the refusal names
     * the lock so the model can report why it stopped. */
    app.session.goal_status = SNAG_GOAL_BLOCKED;
    app.session.goal_locked = true;
    call.arguments = json_pack("{s:s,s:s}", "action", "rewrite", "text", "reworded objective");
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "locked by the operator"));
    json_decref(result);
    json_decref(call.arguments);

    /* resume mirrors the /goal command: only a paused or blocked goal, text null. */
    app.session.goal_locked = false;
    call.arguments = json_pack("{s:s,s:s}", "action", "resume", "text", "extra");
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "resume requires text to be null"));
    json_decref(result);
    json_decref(call.arguments);

    app.session.goal_status = SNAG_GOAL_ACTIVE;
    call.arguments = json_pack("{s:s}", "action", "resume");
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "only a paused or blocked goal can be resumed"));
    json_decref(result);
    json_decref(call.arguments);

    /* A finished goal reports the unfinished-goal gate, not an active one. */
    app.session.goal_status = SNAG_GOAL_COMPLETED;
    call.arguments = json_pack("{s:s,s:s}", "action", "block", "text", "done");
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "there is no unfinished goal to update"));
    json_decref(result);
    json_decref(call.arguments);
    snag_config_free(&config);
}

static json_t *
history_checkpoint_document(void *opaque, const struct snag_session *session)
{
    (void)session;
    return json_incref(opaque);
}

static void
test_history_and_goal_list_tools(void)
{
    char path[4096], error[256] = {0};
    const char *tmp = getenv("TMPDIR");
    const char *old_goal = "61000000000000000000000000000000";
    const char *new_goal = "62000000000000000000000000000000";
    struct app_state app = {0};
    struct snag_response_item call = {.kind = SNAG_ITEM_TOOL_CALL};
    json_t *result = NULL;

    assert(snprintf(path, sizeof(path), "%s/snajpagent-history-tools-XXXXXX",
                    tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "default", "fixture", "medium",
                               error, sizeof(error)) == 0);
    app.session.tool_output_bytes = 2048u;

    uint64_t started_seq = app.session.next_seq;
    assert(snag_session_commit(&app.session, "goal_started",
        json_pack("{s:s,s:s}", "goal_id", old_goal, "prompt", "original objective"),
        NULL, error, sizeof(error)) == 0);
    uint64_t replaced_seq = app.session.next_seq;
    assert(snag_session_commit(&app.session, "goal_replaced",
        json_pack("{s:s,s:s,s:s,s:s}", "actor", "user", "goal_id", old_goal,
                  "new_goal_id", new_goal, "prompt", "replacement objective"),
        NULL, error, sizeof(error)) == 0);
    uint64_t completed_seq = app.session.next_seq;
    assert(snag_session_commit(&app.session, "goal_completed",
        json_pack("{s:s,s:s}", "actor", "user", "goal_id", new_goal),
        NULL, error, sizeof(error)) == 0);

    call.name = "read_session_history";
    call.arguments = json_pack("{s:i,s:i}", "limit", 2, "detail_bytes", 256);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    const char *text = snag_json_string(result, "model_text");
    assert(text && strstr(text, "returned=2") && strstr(text, "order=newest-first"));
    char completed[64], replaced[64], cursor[64];
    assert(snprintf(completed, sizeof(completed), "%llu goal_completed",
                    (unsigned long long)completed_seq) > 0);
    assert(snprintf(replaced, sizeof(replaced), "%llu goal_replaced",
                    (unsigned long long)replaced_seq) > 0);
    assert(snprintf(cursor, sizeof(cursor), "next_before_seq=%llu",
                    (unsigned long long)replaced_seq) > 0);
    assert(strstr(text, completed) < strstr(text, replaced));
    assert(strstr(text, cursor));
    json_t *voice_result = NULL;
    assert(snag_app_voice_read(&app, &call, &voice_result, error, sizeof(error)) == 0);
    assert(json_equal(result, voice_result));
    json_decref(voice_result);
    json_decref(result);
    json_decref(call.arguments);

    call.arguments = json_pack("{s:I,s:i}", "before_seq", (json_int_t)replaced_seq, "limit", 50);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    char started[64];
    assert(snprintf(started, sizeof(started), "%llu goal_started",
                    (unsigned long long)started_seq) > 0);
    assert(text && strstr(text, started) && !strstr(text, "goal_replaced"));
    json_decref(result);
    json_decref(call.arguments);

    call.name = "list_goals";
    call.arguments = json_pack("{s:i}", "limit", 1);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, new_goal) && strstr(text, "status=completed") &&
           strstr(text, "parent=61000000000000000000000000000000") &&
           !strstr(text, "id=61000000000000000000000000000000 status=replaced"));
    assert(snprintf(cursor, sizeof(cursor), "next_before_seq=%llu",
                    (unsigned long long)replaced_seq) > 0);
    assert(strstr(text, cursor));
    json_decref(result);
    json_decref(call.arguments);

    call.arguments = json_pack("{s:I,s:i}", "before_seq", (json_int_t)replaced_seq, "limit", 1);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, old_goal) && strstr(text, "status=replaced") &&
           strstr(text, "replaced_by=62000000000000000000000000000000") &&
           !strstr(text, "id=62000000000000000000000000000000"));
    json_decref(result);
    json_decref(call.arguments);

    call.arguments = json_pack("{s:i}", "limit", 51);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    json_decref(result);
    json_decref(call.arguments);

    /* A recent page must not replay an unrelated old prefix. */
    assert(snag_session_checkpoint(&app.session, error, sizeof(error)) == 0);
    assert(snag_session_commit(&app.session, "effort_changed",
        json_pack("{s:s,s:s}", "old_effort", "medium", "new_effort", "high"),
        NULL, error, sizeof(error)) == 0);
    uint64_t tail_seq = app.session.next_seq - 1u;
    unsigned char original;
    assert(pread(app.session.log_fd, &original, 1u, 0) == 1);
    int writer = openat(app.session.dir_fd, "events.jsonl", O_WRONLY | O_CLOEXEC);
    assert(writer >= 0 && pwrite(writer, "X", 1u, 0) == 1);
    call.name = "read_session_history";
    call.arguments = json_pack("{s:i,s:i}", "limit", 1, "detail_bytes", 512);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, "returned=1") && strstr(text, "effort_changed"));
    json_decref(result);
    json_decref(call.arguments);
    call.arguments = json_pack("{s:I,s:i}", "before_seq", (json_int_t)tail_seq, "limit", 1);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, "session_checkpoint") && strstr(text, "provider_view"));
    assert(!strstr(text, "\"state\"") && !strstr(text, "\"context\""));
    json_decref(result);
    json_decref(call.arguments);
    assert(pwrite(writer, &original, 1u, 0) == 1 && close(writer) == 0);

    call.arguments = json_pack("{s:[s],s:i}", "event_types", "goal_started", "limit", 2);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, "returned=1") && strstr(text, started));
    assert(!strstr(text, "goal_replaced") && !strstr(text, "effort_changed"));
    assert(strstr(text, "scan_complete=true") && strstr(text, "next_before_seq=0"));
    json_decref(result);
    json_decref(call.arguments);

    /* A valid request that cannot fit a record is not an empty/end page. */
    app.session.tool_output_bytes = 256u;
    for (size_t i = 0u; i < 2u; ++i) {
        call.name = i ? "list_goals" : "read_session_history";
        call.arguments = json_pack("{s:i}", "limit", 1);
        assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
        assert(!strcmp(snag_json_string(result, "status"), "failed"));
        text = snag_json_string(result, "model_text");
        assert(text && strstr(text, "budget") && !strstr(text, "next_before_seq=0"));
        json_decref(result);
        json_decref(call.arguments);
    }

    /* A filtered scan quantum can end before any match; keep a usable cursor. */
    size_t padding_size = 4u * 1024u * 1024u + 4096u;
    char *padding = malloc(padding_size + 1u);
    assert(padding);
    memset(padding, 'p', padding_size);
    padding[padding_size] = '\0';
    json_t *document = json_pack("{s:s,s:s}", "marker", "history-private-payload",
        "padding", padding);
    free(padding);
    assert(document);
    app.session.tool_output_bytes = 2048u;
    app.session.on_checkpoint = history_checkpoint_document;
    app.session.on_commit_opaque = document;
    assert(snag_session_checkpoint(&app.session, error, sizeof(error)) == 0);
    app.session.on_checkpoint = NULL;
    app.session.on_commit_opaque = NULL;
    json_decref(document);
    uint64_t checkpoint_seq = app.session.checkpoint_seq;
    call.name = "read_session_history";
    call.arguments = json_pack("{s:[s]}", "event_types", "goal_started");
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, "returned=0") && strstr(text, "scan_complete=false"));
    (void)snprintf(cursor, sizeof(cursor), "next_before_seq=%llu",
        (unsigned long long)checkpoint_seq);
    assert(strstr(text, cursor));
    json_decref(result);
    assert(json_object_set_new(call.arguments, "before_seq",
        json_integer((json_int_t)checkpoint_seq)) == 0);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, "returned=1") && strstr(text, started));
    assert(strstr(text, "scan_complete=true") && strstr(text, "next_before_seq=0"));
    json_decref(result);
    json_decref(call.arguments);

    call.arguments = json_pack("{s:i}", "limit", 1);
    app.interrupt_requested = true;
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) < 0);
    assert(errno == ECANCELED && app.session.next_seq == checkpoint_seq + 1u);
    app.interrupt_requested = false;
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, "provider_view") && !strstr(text, "history-private-payload"));
    json_decref(result);
    json_decref(call.arguments);

    snag_session_close(&app.session);
    snag_store_close(&app.store);
}

static void
test_output_cache_failure(void)
{
    const char *a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const char *text = "output from the second command";
    char error[256] = {0}, hash[65];
    struct app_state app = {0};
    struct snag_buf journal = {.max = 65536u}, line = {.max = 65536u};
    struct snag_response_item call = {.kind = SNAG_ITEM_TOOL_CALL, .name = "read_tool_output"};
    json_t *result = NULL;
    snag_session_init(&app.session);
    strcpy(app.session.id, "cccccccccccccccccccccccccccccccc");
    app.session.tool_output_bytes = 1024u;
    app.session.output_cache_bytes = 4096u;
    app.session.pending_log = &journal;
    app.session.processes = calloc(1u, sizeof(*app.session.processes));
    assert(app.session.processes);
    app.session.process_count = app.session.process_capacity = 1u;
    struct snag_process_state *process = app.session.processes;
    strcpy(process->handle, b);
    memset(process->log_hash, 'e', 64u);
    process->log_hash[64] = '\0';
    /* The established process cursor makes this unrelated prefix irrelevant. */
    assert(snag_buf_printf(&journal, "unread earlier history\n") == 0);
    process->log_offset = journal.len;
    process->log_seq = 100u;
    process->output_bytes[0] = strlen(text);
    json_t *record = json_pack("{s:o,s:s,s:i,s:s,s:i,s:s,s:i}", "data",
        json_pack("{s:s,s:s,s:i,s:i,s:s,s:s}", "turn_id", app.session.id,
            "handle", b, "stream", 0, "offset", 0, "encoding", "utf8", "data", text),
        "prev_sha256", process->log_hash, "seq", 100, "session_id", app.session.id,
        "time_ms", 1, "type", "process_output", "v", 1);
    assert(record && snag_json_digest(record, hash) == 0);
    assert(json_object_set_new(record, "event_sha256", json_string(hash)) == 0);
    assert(snag_json_canonical(record, &line) == 0);
    assert(snag_buf_append(&journal, line.data, line.len) == 0 &&
        snag_buf_putc(&journal, '\n') == 0);
    json_decref(record);
    size_t complete = journal.len;
    assert(snag_buf_printf(&journal, "broken later record\n") == 0);
    app.session.log_end = (int64_t)journal.len;
    app.session.next_seq = 102u;

    /* Seed a previously verified cache, then fail while loading another handle. */
    strcpy(app.output_cache.handle, a);
    app.output_cache.valid = true;
    app.output_cache.total = 8u;
    snag_buf_init(&app.output_cache.data, 4096u);
    assert(snag_buf_printf(&app.output_cache.data, "original") == 0);
    call.arguments = json_pack("{s:s,s:s}", "handle", b, "stream", "stdout");
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    assert(app.output_cache.valid && !strcmp(app.output_cache.handle, a));
    assert(app.output_cache.data.len == 8u &&
        !memcmp(app.output_cache.data.data, "original", 8u));
    assert(strstr(snag_json_string(result, "model_text"), "corrupt event 101"));
    assert(!strstr(snag_json_string(result, "model_text"), "No durable"));
    json_decref(result);

    assert(json_object_set_new(call.arguments, "handle", json_string(a)) == 0);
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "cache=hit]\noriginal"));
    json_decref(result);
    app.interrupt_requested = true;
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) < 0);
    assert(errno == ECANCELED && !result && app.output_cache.valid);
    assert(!strcmp(app.output_cache.handle, a) && app.output_cache.data.len == 8u &&
        !memcmp(app.output_cache.data.data, "original", 8u));
    app.interrupt_requested = false;

    /* Repair only the selected suffix; successful fill never reads the prefix. */
    app.session.log_end = (int64_t)complete;
    app.session.next_seq = 101u;
    assert(json_object_set_new(call.arguments, "handle", json_string(b)) == 0);
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "succeeded"));
    assert(strstr(snag_json_string(result, "model_text"), text));
    json_decref(result);
    app.interrupt_requested = true;
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) < 0);
    assert(errno == ECANCELED && !result && app.output_cache.valid);
    assert(app.output_cache.data.len == strlen(text) &&
        !memcmp(app.output_cache.data.data, text, strlen(text)));
    app.interrupt_requested = false;
    /* A bad cursor and a hole in retained bytes remain explicit read failures. */
    uint64_t anchor = process->log_offset;
    process->log_offset = (uint64_t)app.session.log_end + 1u;
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    assert(strstr(snag_json_string(result, "model_text"), "invalid process output cursor"));
    json_decref(result);
    process->log_offset = anchor;
    ++process->output_bytes[0];
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    assert(strstr(snag_json_string(result, "model_text"), "missing bytes"));
    json_decref(result);
    --process->output_bytes[0];
    assert(app.output_cache.valid && !strcmp(app.output_cache.handle, b));
    assert(app.output_cache.data.len == strlen(text) &&
        !memcmp(app.output_cache.data.data, text, strlen(text)));
    /* Collection advances this cursor: older bytes must not be declared absent. */
    process->collected_bytes[0] = 1u;
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    assert(strstr(snag_json_string(result, "model_text"), "corrupt event 1"));
    json_decref(result);
    json_decref(call.arguments);
    app.session.pending_log = NULL;
    snag_session_close(&app.session);
    snag_buf_free(&app.output_cache.data);
    snag_buf_free(&journal);
    snag_buf_free(&line);
}

static void
history_voice_event(struct snag_session *session, json_t *event)
{
    char error[256] = {0};
    assert(snag_session_commit(session, "voice_event", json_pack("{s:s,s:s,s:s,s:o}",
        "connection_id", "12345678901234567890123456789012", "provider", "default",
        "model", "fixture", "event", event), NULL, error, sizeof(error)) == 0);
}

static void
test_ui_bounded_history(void)
{
    char path[4096], output_path[4096], error[256] = {0}, output[8192];
    struct snag_store store;
    struct snag_session session;
    struct snag_ui ui;
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(path, sizeof(path), "%s/snajpagent-ui-history-XXXXXX", tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    snag_store_init(&store);
    snag_session_init(&session);
    assert(snag_store_open(&store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&store, &session, path, "default", "fixture", "medium",
        error, sizeof(error)) == 0);
    assert(snprintf(output_path, sizeof(output_path), "%s/capture", path) > 0);
    int fd = open(output_path, O_CREAT | O_EXCL | O_RDWR, 0600), saved = dup(STDERR_FILENO);
    assert(fd >= 0 && saved >= 0 && dup2(fd, STDERR_FILENO) == STDERR_FILENO);
    assert(snag_ui_init(&ui) == 0);
    history_voice_event(&session, json_pack("{s:s,s:s,s:s}", "type", "voice_transcript",
        "speaker", "user", "text", "voice-only original"));
    history_voice_event(&session, json_pack("{s:s,s:s,s:s}", "type", "voice_transcript",
        "speaker", "assistant", "text", "voice-only reply"));
    char session_id[33];
    memcpy(session_id, session.id, sizeof(session_id));
    snag_session_close(&session);
    assert(snag_session_open(&store, &session, session_id, error, sizeof(error)) == 0);
    assert(session.turn_count == 0u);
    assert(snag_ui_history(&ui, &session, 0u) == 0);
    ssize_t voice_bytes = pread(fd, output, sizeof(output) - 1u, 0);
    assert(voice_bytes > 0);
    output[voice_bytes] = '\0';
    assert(!strstr(output, "voice-only"));
    assert(snag_ui_history(&ui, &session, 1u) == 0);
    voice_bytes = pread(fd, output, sizeof(output) - 1u, 0);
    assert(voice_bytes > 0);
    output[voice_bytes] = '\0';
    const char *asr = strstr(output, "You [voice, ASR]: voice-only original");
    const char *reply = strstr(output, "Voice model [generated]: voice-only reply");
    assert(asr && reply && asr < reply);
    assert(!strstr(reply + 1, "Voice model [generated]: voice-only reply"));
    assert(session.turn_count == 0u && session.pending_queue_count == 0u);
    assert(ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0);
    assert(snag_ui_set_verbosity(&ui, 2u) == 0);
    int64_t damaged = 0;
    json_t *checkpoint = NULL;
    for (unsigned int i = 1u; i <= 3u; ++i) {
        char id[33], text[32];
        assert(snprintf(id, sizeof(id), "%032x", i) > 0);
        assert(snprintf(text, sizeof(text), "history input %u", i) > 0);
        json_t *started = json_pack("{s:{s:s,s:s,s:n,s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:b},"
            "s:s,s:b,s:o,s:n,s:n,s:s,s:s,s:I,s:s}",
            "config", "capability_version", SNAJPAGENT_CAPABILITY_VERSION, "effort", "medium",
            "max_output_tokens", "model", "fixture", "provider", "default",
            "profile_id", SNAJPAGENT_PROFILE_ID,
            "prompt_schema", 1, "replay_schema", 1, "tool_schema", 1, "max_parallel_commands", 4,
            "parallel_tool_calls", 1, "input_kind", "direct", "read_only", 0,
            "instructions", json_array(), "queue_id", "queue_seq", "text", text,
            "turn_id", id, "turn_number", (json_int_t)i, "cwd", session.cwd);
        assert(snag_session_commit(&session, "turn_started", started,
            NULL, error, sizeof(error)) == 0);
        if (i == 2u) {
            history_voice_event(&session, json_pack("{s:s,s:s,s:s}",
                "type", "voice_transcript", "speaker", "user", "text", "during work"));
            history_voice_event(&session, json_pack("{s:s,s:s,s:s,s:s,s:{s:s}}",
                "type", "voice_response", "operation", "interface_tool_started",
                "tool", "inspect_session", "tool_call_id", "history-call",
                "arguments", "scope", "queue"));
            history_voice_event(&session, json_pack("{s:s,s:s,s:s,s:s,s:{s:s,s:s}}",
                "type", "voice_response", "operation", "interface_tool",
                "tool", "inspect_session", "tool_call_id", "history-call",
                "result", "status", "succeeded", "model_text", "queue remains empty"));
        }
        if (i == 1u) damaged = session.log_end;
        if (i == 3u) {
            size_t size = SNAG_JOURNAL_PAGE_BYTES + 4096u;
            char *padding = malloc(size);
            assert(padding); memset(padding, 'x', size);
            checkpoint = json_pack("{s:o}", "padding", json_stringn(padding, size));
            free(padding); assert(checkpoint);
            session.on_checkpoint = history_checkpoint_document;
            session.on_commit_opaque = checkpoint;
            assert(snag_session_checkpoint(&session, error, sizeof(error)) == 0);
            session.on_checkpoint = NULL; session.on_commit_opaque = NULL;
        }
        assert(snag_session_commit(&session, "turn_failed",
            json_pack("{s:s,s:s,s:s}", "class", "provider", "message", "fixture", "turn_id", id),
            NULL, error, sizeof(error)) == 0);
        if (i == 2u) {
            char original;
            int writer = openat(session.dir_fd, "events.jsonl", O_WRONLY | O_CLOEXEC);
            assert(writer >= 0);
            assert(pread(session.log_fd, &original, 1u, damaged) == 1);
            assert(pwrite(writer, "!", 1u, damaged) == 1);
            assert(snag_ui_history(&ui, &session, 0u) == 0);
            assert(snag_ui_history(&ui, &session, 1u) == 0);
            assert(snag_ui_history(&ui, &session, 2u) < 0);
            assert(pwrite(writer, &original, 1u, damaged) == 1);
            assert(close(writer) == 0);
        }
    }
    assert(snag_ui_history(&ui, &session, 1u) == 0);
    /* Admission metadata beyond a large checkpoint is not an invented turn. */
    struct snag_irc_event event = {.kind = SNAG_IRC_MESSAGE, .timestamp_ms = 1u,
        .stream = "11111111111111111111111111111111", .sequence = 1u,
        .endpoint = "fixture:1234", .room = "#fixture", .nick = "peer", .text = "background",
        .classified = true, .input = true};
    uint64_t irc_seq;
    assert(snag_session_commit(&session, "irc_event", snag_irc_event_data(&event),
        &irc_seq, error, sizeof(error)) == 0);
    session.on_checkpoint = history_checkpoint_document;
    session.on_commit_opaque = checkpoint;
    assert(snag_session_checkpoint(&session, error, sizeof(error)) == 0);
    session.on_checkpoint = NULL; session.on_commit_opaque = NULL;
    json_decref(checkpoint);
    assert(snag_session_commit(&session, "irc_admitted",
        json_pack("{s:[I]}", "sequences", (json_int_t)irc_seq), NULL, error, sizeof(error)) == 0);
    assert(snag_ui_history(&ui, &session, 1u) == 0);
    /* Integrity-valid envelopes still need safe presentation-field checks. */
    for (unsigned int malformed = 0u; malformed < 5u; ++malformed) {
        struct snag_session broken;
        struct snag_buf bytes = {.max = 65536u};
        snag_session_init(&broken);
        memcpy(broken.id, session.id, sizeof(broken.id));
        broken.pending_log = &bytes; broken.turn_count = 1u;
        for (unsigned int row = 0u; row <= (malformed ? 1u : 0u); ++row) {
            const char *type = !row ? "turn_started" : malformed == 1u ?
                "response_output" : malformed == 2u ? "response_completed" : "voice_event";
            json_t *data = !row ? (malformed ? json_pack("{s:s}", "text", "fixture") :
                json_object()) : malformed == 1u ? json_pack("{s:i,s:{}}", "offset", 0, "item") :
                malformed == 2u ? json_pack("{s:[{s:s}]}", "items", "kind", "assistant") :
                malformed == 3u ? json_pack("{s:{s:s,s:s,s:s}}", "event",
                    "type", "voice_transcript", "speaker", "invalid", "text", "fixture") :
                json_pack("{s:{s:s,s:s}}", "event", "type", "voice_transcript", "speaker", "user");
            json_t *record = json_pack("{s:o,s:s,s:I,s:s,s:i,s:s,s:i}", "data", data,
                "prev_sha256", broken.prev_sha256, "seq", (json_int_t)broken.next_seq,
                "session_id", broken.id, "time_ms", 1, "type", type, "v", 1);
            assert(record && snag_json_digest(record, broken.prev_sha256) == 0);
            assert(json_object_set_new(record, "event_sha256",
                json_string(broken.prev_sha256)) == 0);
            struct snag_buf line = {.max = 65536u};
            assert(snag_json_canonical(record, &line) == 0);
            assert(snag_buf_append(&bytes, line.data, line.len) == 0 &&
                snag_buf_putc(&bytes, '\n') == 0);
            snag_buf_free(&line);
            json_decref(record);
            ++broken.next_seq;
        }
        broken.log_end = (int64_t)bytes.len;
        assert(snag_ui_history(&ui, &broken, 1u) < 0 && errno == EINVAL);
        assert(broken.history_cursor.next_seq == 1u && broken.history_cursor.offset == 0);
        snag_buf_free(&bytes);
    }
    snag_ui_signal(&ui);
    assert(snag_ui_history(&ui, &session, 1u) < 0 && errno == ECANCELED);
    snag_ui_free(&ui);
    assert(dup2(saved, STDERR_FILENO) == STDERR_FILENO && close(saved) == 0);
    ssize_t got = pread(fd, output, sizeof(output) - 1u, 0);
    assert(got > 0); output[got] = '\0';
    assert(strstr(output, "history input 2") && !strstr(output, "history input 1"));
    const char *during = strstr(output, "You [voice, ASR]: during work");
    const char *started = strstr(output, "voice: inspect_session");
    const char *finished = started ? strstr(started + 1, "voice: inspect_session") : NULL;
    assert(during && started && finished);
    assert(strstr(output, "history input 2") < during && during < started && started < finished);
    assert(strstr(finished, "queue remains empty"));
    assert(!strstr(output, "voice-only"));
    assert(!strstr(output, "history input 3"));
    assert(strstr(output, "earlier input outside restored history window"));
    assert(!strstr(strstr(output, "earlier input outside restored history window") + 1,
        "earlier input outside restored history window"));
    assert(strstr(output, "History scan window ended"));
    assert(strstr(output, "1 shown · 0 completed among shown · 3 total"));
    assert(strstr(output, "0 shown · 0 completed among shown · 3 total"));
    assert(close(fd) == 0);
    snag_session_close(&session);
    snag_store_close(&store);
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
            for (ssize_t i = 0; i < got; ++i) assert(text[i] == (received + (size_t)i) / sizeof(text));
            received += (size_t)got;
        }
        (void)close(pipefd[0]);
        _exit(0);
    }
    assert(close(pipefd[0]) == 0);
    assert(snag_ui_init(&ui) == 0);
    /* Capture may not start without an interactive raw prompt; rejection and
     * invalid transcript insertion must not poison the presentation owner. */
    assert(snag_ui_audio(&ui, "[mic] ", true) == 1);
    assert(snag_ui_insert_draft(&ui, "\xff") == 1);
    assert(snag_ui_insert_draft(&ui, "kept draft") == 0);
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
                    error[0] = '\0';
                    int rc = snag_auth_read(store.root_fd, &config.providers[0], false,
                        NULL, &credential, NULL, NULL, error, sizeof(error));
                    bool matched = rc == 0 && strcmp(credential.value, "new-access") == 0;
                    if (!matched)
                        (void)fprintf(stderr,
                            "concurrent auth refresh child failed: rc=%d matched=%d error=%s\n",
                            rc, matched ? 1 : 0, error[0] ? error : "(none)");
                    _exit(matched ? 0 : 1);
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
                &config, &config.providers[0], &credential, NULL, NULL, NULL, NULL, 0}, &models, error, sizeof(error));
            if (rc < 0 && mode == MODEL_AUTH_401) (void)fprintf(stderr, "auth fixture failed: %s\n", error);
            assert((rc == 0) == (mode == MODEL_AUTH_401));
            if (rc == 0) assert(json_array_size(models) == 1u);
            json_decref(models);
            assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
        }
        stop_server(&server);
    }
    assert(unsetenv("SNAJPAGENT_TEST_AUTH_BASE") == 0);
    /* Meta subscription: token responses keep prior refresh and account. */
    config.providers[1].auth = SNAG_AUTH_META;
    strcpy(config.providers[1].base_url, SNAG_META_BASE);
    {
        json_t *response = json_object();
        snag_auth_clear(&tokens);
        assert(snag_auth_token_response_meta(response, &tokens, error, sizeof(error)) < 0);
        assert(tokens.credential.len == 0u);
        assert(snag_auth_key(&tokens, "old-access", error, sizeof(error)) == 0);
        strcpy(tokens.refresh_token, "meta-old-refresh");
        strcpy(tokens.credential.account_id, "meta-user");
        assert(json_object_set_new(response, "access_token", json_string("rotated-access")) == 0);
        assert(json_object_set_new(response, "expires_in", json_integer(3600)) == 0);
        assert(snag_auth_token_response_meta(response, &tokens, error, sizeof(error)) == 0);
        assert(!strcmp(tokens.credential.value, "rotated-access"));
        assert(!strcmp(tokens.refresh_token, "meta-old-refresh"));
        assert(!strcmp(tokens.credential.account_id, "meta-user"));
        assert(tokens.expires_at_ms > snag_time_ms());
        snag_auth_json_free(response);
    }
    {
        json_t *jwt = json_object();
        snag_auth_clear(&tokens);
        assert(json_object_set_new(jwt, "access_token", json_string("h.eyJzdWIiOiJtZXRhLXVzZXIiLCJleHAiOjk5OTk5OTk5OTl9.sig")) == 0);
        assert(snag_auth_token_response_meta(jwt, &tokens, error, sizeof(error)) == 0);
        assert(!strcmp(tokens.credential.account_id, "meta-user"));
        assert(tokens.expires_at_ms == 9999999999u * 1000u);
        assert(!tokens.refresh_token[0]);
        assert(json_object_set_new(jwt, "access_token", json_string("h.eyJzdWIiOiJzb21lb25lLWVsc2UiLCJleHAiOjk5OTk5OTk5OTl9.sig")) == 0);
        assert(json_object_set_new(jwt, "expires_in", json_integer(3600)) == 0);
        assert(snag_auth_token_response_meta(jwt, &tokens, error, sizeof(error)) < 0);
        snag_auth_json_free(jwt);
    }
    for (int mode = MODEL_META_DEVICE; mode <= MODEL_META_EXPIRED; ++mode) {
        start_server(&server, (enum model_fixture)mode, false, "");
        assert(setenv("SNAJPAGENT_TEST_META_AUTH_BASE", server.endpoint, 1) == 0);
        memset(error, 0, sizeof(error));
        int rc = snag_auth_device_meta(&tokens, NULL, NULL, error, sizeof(error));
        if (mode == MODEL_META_DEVICE) {
            assert(rc == 0);
            assert(!strcmp(tokens.credential.value, "meta-access"));
            assert(!strcmp(tokens.refresh_token, "meta-refresh"));
            assert(tokens.expires_at_ms > snag_time_ms());
        } else {
            assert(rc < 0);
            assert(tokens.credential.len == 0u);
        }
        stop_server(&server);
    }

    for (int mode = MODEL_META_REFRESH; mode <= MODEL_META_REFRESH_FAILURE; ++mode) {
        assert(snag_auth_key(&tokens, "old-access", error, sizeof(error)) == 0);
        strcpy(tokens.refresh_token, "meta-old-refresh");
        tokens.expires_at_ms = 1u;
        assert(snag_auth_save(store.root_fd, &config.providers[1], &tokens, NULL,
                              NULL, NULL, error, sizeof(error)) == 0);
        start_server(&server, (enum model_fixture)mode, false, "");
        assert(setenv("SNAJPAGENT_TEST_META_AUTH_BASE", server.endpoint, 1) == 0);
        memset(error, 0, sizeof(error));
        if (mode == MODEL_META_REFRESH) {
            assert(snag_auth_read(store.root_fd, &config.providers[1], false, NULL,
                &credential, NULL, NULL, error, sizeof(error)) == 0);
            assert(!strcmp(credential.value, "meta-new-access"));
            assert(snag_auth_load(store.root_fd, &config.providers[1], &loaded, error, sizeof(error)) == 0);
            assert(!strcmp(loaded.refresh_token, "meta-new-refresh"));
        } else {
            assert(snag_auth_read(store.root_fd, &config.providers[1], false, NULL,
                &credential, NULL, NULL, error, sizeof(error)) < 0);
            assert(snag_auth_load(store.root_fd, &config.providers[1], &loaded, error, sizeof(error)) == 0);
            assert(!strcmp(loaded.refresh_token, "meta-old-refresh"));
        }
        stop_server(&server);
    }
    assert(unsetenv("SNAJPAGENT_TEST_META_AUTH_BASE") == 0);
    strcpy(config.providers[1].base_url, "https://different.test");
    assert(snag_auth_load(store.root_fd, &config.providers[1], &loaded, error, sizeof(error)) < 0);
    strcpy(config.providers[1].base_url, SNAG_META_BASE);
    assert(snag_auth_load(store.root_fd, &config.providers[1], &loaded, error, sizeof(error)) == 0);
    assert(!strcmp(loaded.credential.value, "old-access"));
    assert(snag_auth_logout(store.root_fd, &config.providers[1], NULL, NULL, error, sizeof(error)) == 0);
    assert(unlinkat(store.root_fd, "auth/other.lock", 0) == 0);
    for (unsigned int pass = 0u; pass < 4u; ++pass) {
        json_t *request = request_with_marker("transport-compact");
        struct snag_json_document output = {0};
        credential_set(&credential, "transport-secret");
        credential.root_fd = -1;
        config.providers[0].auth = pass == 0u ? SNAG_AUTH_API_KEY : SNAG_AUTH_CHATGPT;
        strcpy(config.providers[0].base_url, pass == 0u ? "https://api.openai.com" : SNAG_CHATGPT_BASE);
        strcpy(config.providers[0].openrouter_referer, "https://github.com/snajpa/snajpagent");
        strcpy(config.providers[0].openrouter_title, "snajpagent");
        start_server(&server, pass == 3u ? MODEL_COMPACT_403 :
                          pass == 2u ? MODEL_COMPACT_502 : MODEL_COMPACT_404, false, "");
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", server.endpoint, 1) == 0);
        int rc = snag_provider_responses_compact((struct snag_provider_connection){
            &config, &config.providers[0], &credential, NULL, NULL, NULL, NULL, 0},
            request, &output, error, sizeof(error), NULL);
        assert(rc == (pass < 3u ? SNAG_PROVIDER_UNSUPPORTED : -1));
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
test_irc_steering_mode(void)
{
    struct snag_config config = {0};
    struct app_state app = {0};
    char error[256] = {0};
    snag_config_init(&config);
    snag_session_init(&app.session);
    assert(snag_ui_init(&app.ui) == 0);
    app.config = &config;
    app.irc_urgent.max = 128u;
    app.irc_background.max = 128u;
    assert(snag_buf_append(&app.irc_background, "room", sizeof("room")) == 0);
    app.session.active_turn = true;
    strcpy(config.model, "m");
    strcpy(app.session.active_turn_provider, "p");
    /* Default `mentions`: mid-turn, background room traffic is not admitted. */
    assert(snag_app_irc_flush_urgent(&app, error, sizeof(error)) == 0);
    assert(app.irc_background.len == sizeof("room"));
    /* `all`: the background projection is attempted; invalid UTF-8 fails it
     * without consuming the projection. */
    config.model_limit_count = 1u;
    strcpy(config.model_limits[0].provider, "p");
    strcpy(config.model_limits[0].model, "m");
    strcpy(config.model_limits[0].steering, "all");
    app.irc_background.data[0] = 0xff;
    assert(snag_app_irc_flush_urgent(&app, error, sizeof(error)) < 0);
    assert(app.irc_background.len == sizeof("room"));
    snag_buf_free(&app.irc_background);
    snag_ui_free(&app.ui);
    snag_session_close(&app.session);
    snag_config_free(&config);
}

static void
test_plain_irc_pending_resume(void)
{
    struct snag_config config;
    struct app_state app = {0};
    char path[4096], id[33], error[256] = {0};
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(path, sizeof(path), "%s/snajpagent-plain-irc-XXXXXX",
        tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    snag_config_init(&config);
    app.config = &config;
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_ui_init(&app.ui) == 0);
    for (unsigned int kind = 0u; kind < 2u; ++kind) {
        uint64_t received = 0u;
        struct snag_irc_event event = {
            .kind = kind ? SNAG_IRC_NOTICE : SNAG_IRC_MESSAGE, .timestamp_ms = 1u};
        strcpy(event.endpoint, "127.0.0.1:16669");
        strcpy(event.room, "#plain");
        strcpy(event.nick, "peer");
        strcpy(event.text, "unconsumed plain IRC input");
        for (unsigned int phase = 0u; phase < 3u; ++phase) {
            snag_buf_init(&app.irc_background, 4096u);
            snag_buf_init(&app.irc_background_refs, 1024u);
            snag_buf_init(&app.irc_urgent, 4096u);
            snag_buf_init(&app.irc_urgent_refs, 1024u);
            if (!phase) {
                assert(snag_session_create(&app.store, &app.session, path, "default",
                    "fixture", "medium", error, sizeof(error)) == 0);
                strcpy(id, app.session.id);
            } else {
                assert(snag_session_open(&app.store, &app.session, id,
                    error, sizeof(error)) == 0);
            }
            /* No endpoints: inject the already parsed event without networking. */
            assert(snag_irc_open(&app.irc, &config, path, NULL, NULL, NULL,
                error, sizeof(error)) == 0);
            if (!phase) {
                received = app.session.next_seq;
                assert(snag_app_irc_event(&app, &event) == 0);
            } else {
                assert(snag_app_irc_restore(&app, error, sizeof(error)) == 0);
            }
            if (phase < 2u) {
                assert(app.irc_background.len > 0u);
                assert(snag_buf_terminate(&app.irc_background) == 0);
                assert(strstr((const char *)app.irc_background.data, event.text));
            } else {
                assert(!app.irc_background.len && !app.irc_background_refs.len);
            }
            if (phase == 1u) {
                assert(app.irc_background_refs.len > 0u);
                assert(snag_session_commit(&app.session, "irc_admitted",
                    json_pack("{s:[I]}", "sequences", (json_int_t)received),
                    NULL, error, sizeof(error)) == 0);
            }
            snag_irc_close(app.irc);
            app.irc = NULL;
            snag_session_close(&app.session);
            snag_buf_free(&app.irc_background);
            snag_buf_free(&app.irc_background_refs);
            snag_buf_free(&app.irc_urgent);
            snag_buf_free(&app.irc_urgent_refs);
        }
    }
    snag_ui_free(&app.ui);
    snag_store_close(&app.store);
    snag_config_free(&config);
}

static void
test_irc_reply_restore_scope(void)
{
    struct snag_config config;
    struct snag_store store;
    char path[4096], error[256] = {0};
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(path, sizeof(path), "%s/snajpagent-irc-replies-XXXXXX",
        tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    snag_config_init(&config);
    strcpy(config.irc.room_name, "#plain");
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET};
    socklen_t address_size = sizeof(address);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(probe >= 0 && bind(probe, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(probe, (struct sockaddr *)&address, &address_size) == 0);
    assert(snprintf(config.irc.listen, sizeof(config.irc.listen), "127.0.0.1:%u",
        (unsigned int)ntohs(address.sin_port)) > 0);
    assert(close(probe) == 0);
    config.irc.listen_explicit = true;
    snag_store_init(&store);
    assert(snag_store_open(&store, path, error, sizeof(error)) == 0);
    for (unsigned int mode = 0u; mode < 9u; ++mode) {
        struct app_state app = {0};
        char session_id[33];
        bool steer_current = mode == 8u;
        bool active = steer_current || (mode & 1u);
        bool current_reply = steer_current || (mode & 2u), cancel_old = mode & 4u;
        app.config = &config;
        snag_session_init(&app.session);
        assert(snag_session_create(&store, &app.session, path, "default", "fixture",
            "medium", error, sizeof(error)) == 0);
        strcpy(session_id, app.session.id);
        struct snag_irc_event event = {.kind = SNAG_IRC_MESSAGE, .timestamp_ms = 1u,
            .room = "#plain", .nick = "operator",
            .local = true, .classified = true, .input = true, .urgent = true, .reply = true};
        strcpy(event.endpoint, config.irc.listen);
        for (unsigned int turn = 1u; turn <= 2u; ++turn) {
            char id[33], text[64];
            assert(snprintf(id, sizeof(id), "%032x", turn) > 0);
            assert(snprintf(text, sizeof(text), "request %u for the agent", turn) > 0);
            json_t *input = json_pack("{s:s,s:o,s:s,s:s,s:b,s:I,s:s}",
                "effort", "medium", "instructions", json_array(), "model", "fixture",
                "provider", "default", "read_only", 0,
                "received_at_ms", (json_int_t)snag_time_ms(), "text", text);
            assert(input);
            if (turn == 1u || (current_reply && !steer_current)) {
                uint64_t received;
                strcpy(event.text, text);
                assert(snag_session_commit(&app.session, "irc_event", snag_irc_event_data(&event),
                    &received, error, sizeof(error)) == 0);
                input = json_pack("{s:[I],s:o}", "sequences", (json_int_t)received, "input", input);
                assert(snag_session_commit(&app.session, "irc_admitted", input,
                    NULL, error, sizeof(error)) == 0);
            } else {
                assert(snag_session_commit(&app.session, "input_received", input,
                    NULL, error, sizeof(error)) == 0);
            }
            if (turn == 1u && cancel_old) {
                assert(snag_session_commit(&app.session, "input_cancelled", json_object(),
                    NULL, error, sizeof(error)) == 0);
                continue;
            }
            if (turn == 1u || active) {
                json_t *started = json_pack("{s:{s:s,s:s,s:n,s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:b},"
                    "s:s,s:b,s:o,s:n,s:n,s:s,s:s,s:I,s:s}",
                    "config", "capability_version", SNAJPAGENT_CAPABILITY_VERSION,
                    "effort", "medium", "max_output_tokens", "model", "fixture",
                    "provider", "default", "profile_id", SNAJPAGENT_PROFILE_ID,
                    "prompt_schema", 1, "replay_schema", 1, "tool_schema", 1,
                    "max_parallel_commands", 4, "parallel_tool_calls", 1,
                    "input_kind", "direct", "read_only", 0, "instructions", json_array(),
                    "queue_id", "queue_seq", "text", text, "turn_id", id,
                    "turn_number", (json_int_t)(turn - (cancel_old ? 1u : 0u)),
                    "cwd", app.session.cwd);
                assert(snag_session_commit(&app.session, "turn_started", started,
                    NULL, error, sizeof(error)) == 0);
            }
            if (turn == 1u)
                assert(snag_session_commit(&app.session, "turn_failed",
                    json_pack("{s:s,s:s,s:s}", "class", "provider", "message", "fixture",
                        "turn_id", id), NULL, error, sizeof(error)) == 0);
            if (turn == 2u && steer_current) {
                uint64_t received;
                strcpy(event.text, "current room steering");
                assert(snag_session_commit(&app.session, "irc_event", snag_irc_event_data(&event),
                    &received, error, sizeof(error)) == 0);
                assert(snag_session_commit(&app.session, "irc_admitted",
                    json_pack("{s:[I],s:{s:s,s:s,s:s}}", "sequences", (json_int_t)received,
                        "steering", "turn_id", id, "steering_id",
                        "33333333333333333333333333333333", "text", event.text),
                    NULL, error, sizeof(error)) == 0);
            }
        }
        snag_session_close(&app.session);
        assert(snag_session_open(&store, &app.session, session_id, error, sizeof(error)) == 0);
        snag_buf_init(&app.irc_background, 4096u);
        snag_buf_init(&app.irc_background_refs, 1024u);
        snag_buf_init(&app.irc_urgent, 4096u);
        snag_buf_init(&app.irc_urgent_refs, 1024u);
        /* An owned loopback listener supplies identity; no pump starts owners. */
        assert(snag_irc_open(&app.irc, &config, path, NULL, NULL, NULL,
            error, sizeof(error)) == 0);
        struct snag_irc_target target;
        assert(snag_irc_event_target(app.irc, &event, &target));
        assert(snag_app_irc_restore(&app, error, sizeof(error)) == 0);
        assert(app.irc_turn_replies.count == (current_reply ? 1u : 0u));
        if (current_reply) assert(app.irc_turn_replies.targets[0].id == target.id);
        assert(!app.irc_urgent.len && !app.irc_background.len);
        snag_irc_close(app.irc);
        snag_buf_free(&app.irc_background);
        snag_buf_free(&app.irc_background_refs);
        snag_buf_free(&app.irc_urgent);
        snag_buf_free(&app.irc_urgent_refs);
        snag_session_close(&app.session);
    }
    snag_store_close(&store);
    snag_config_free(&config);
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

static int
audio_cancel(void *opaque, unsigned int wait_ms)
{
    (void)opaque; (void)wait_ms; return 2;
}

struct video_audio_fixture {
    struct snag_session *session;
    const struct snag_config *config;
    int root_fd;
    const char *original;
    unsigned int calls;
    char source[SNAG_ID_HEX_LEN + 1u];
};

static int
fixture_video_transcribe(void *opaque, const json_t *source, uint64_t start, uint64_t end, json_t **result)
{
    struct video_audio_fixture *fixture = opaque;
    assert(start == 0u && end == 1u && ++fixture->calls == 1u);
    strcpy(fixture->source, snag_json_string(source, "id"));
    /* The source snapshot serves both operations after the original is gone. */
    assert(unlink(fixture->original) == 0);
    return snag_tools_transcribe_asset(fixture->session, source, start, end, fixture->root_fd,
        fixture->config, NULL, NULL, SNAG_WAKE_INVALID, result);
}

static void
test_audio_transport(void)
{
    struct snag_config config;
    struct snag_credential credential;
    struct snag_buf wav, out;
    struct local_server server;
    char endpoint[128], error[256];
    make_fixture_wav();
    snag_config_init(&config);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    config.providers[0].request_timeout_ms = 1500u;
    strcpy(config.providers[0].base_url, "https://openrouter.ai/api/v1");
    strcpy(config.providers[0].openrouter_referer, "https://github.com/snajpa/snajpagent");
    strcpy(config.providers[0].openrouter_title, "snajpagent");
    credential_set(&credential, "transport-secret");
    snag_buf_init(&wav, 2048u); snag_buf_init(&out, 4096u);
    assert(snag_buf_append(&wav, fixture_wav, sizeof(fixture_wav)) == 0);
    json_t *request = json_pack("{s:s,s:s,s:s,s:s}", "model", "audio-fixture",
        "response_format", "wav", "voice", "alloy", "input", "fixture text");
    assert(json_object_set_new(request, "question", json_string("What sounds?")) == 0);
    for (int mode = MODEL_AUDIO_LISTEN; mode <= MODEL_AUDIO_FAILURE; ++mode) {
        enum snag_audio_operation op = mode == MODEL_AUDIO_TRANSCRIBE ? SNAG_AUDIO_TRANSCRIBE :
            mode == MODEL_AUDIO_SPEAK ? SNAG_AUDIO_SPEAK : SNAG_AUDIO_LISTEN;
        start_server(&server, (enum model_fixture)mode, false, "");
        snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u/v1", server.port);
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", endpoint, 1) == 0);
        snag_buf_reset(&out);
        assert(snag_buf_append(&out, "kept:", 5u) == 0);
        int rc = snag_provider_audio(op, request, &wav, &config, &config.providers[0],
            &credential, NULL, NULL, &out, error, sizeof(error));
        if (mode == MODEL_AUDIO_FAILURE) {
            assert(rc < 0 && out.len == 5u && strstr(error, "HTTP 503") && strstr(error, "not retried"));
            assert(!strstr(error, "private audio"));
        } else {
            assert(rc == 0 && out.len > 5u);
            if (mode == MODEL_AUDIO_SPEAK) assert(out.len == 5u + wav.len && !memcmp(out.data + 5u, wav.data, wav.len));
        }
        stop_server(&server);
    }
    /* Callback cancellation is before transport; no server/device necessary. */
    size_t kept = out.len;
    assert(snag_provider_audio(SNAG_AUDIO_LISTEN, request, &wav, &config, &config.providers[0],
        &credential, audio_cancel, NULL, &out, error, sizeof(error)) == 2 && out.len == kept);
    config.providers[0].auth = SNAG_AUTH_CHATGPT;
    assert(snag_provider_audio(SNAG_AUDIO_LISTEN, request, &wav, &config, &config.providers[0],
        &credential, NULL, NULL, &out, error, sizeof(error)) < 0 && out.len == kept);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    /* Cross curl's upload-buffer boundary, exercise every base64 remainder,
     * and verify escaped question metadata without materialized base64 input. */
    struct snag_buf large;
    snag_buf_init(&large, 24000u);
    for (size_t i = 0; i < 20003u; ++i) assert(snag_buf_putc(&large, (unsigned char)i) == 0);
    assert(json_object_set_new(request, "question", json_string("What \"sounds\"?\n\\details")) == 0);
    for (size_t n = 20001u; n <= 20003u; ++n) {
        large.len = n; audio_expected = large.data; audio_expected_len = n;
        start_server(&server, MODEL_AUDIO_LISTEN, false, "");
        snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u/v1", server.port);
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", endpoint, 1) == 0);
        snag_buf_reset(&out);
        assert(snag_provider_audio(SNAG_AUDIO_LISTEN, request, &large, &config, &config.providers[0],
            &credential, NULL, NULL, &out, error, sizeof(error)) == 0);
        stop_server(&server);
    }
    audio_expected = fixture_wav; audio_expected_len = sizeof(fixture_wav);
    snag_buf_free(&large);
    if (getenv("SNAJPAGENT_TEST_MEDIA")) {
        struct snag_store store;
        struct snag_session session;
        char *temp = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", "snag-audio-XXXXXX");
        assert(temp && mkdtemp(temp));
        snag_store_init(&store); snag_session_init(&session);
        assert(snag_store_open(&store, temp, error, sizeof(error)) == 0);
        assert(snag_session_create(&store, &session, temp, "default", "audio-fixture", "medium", error, sizeof(error)) == 0);
        char *file = snag_path_join(temp, "fixture audio.wav");
        int fd = open(file, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0 && write(fd, fixture_wav, sizeof(fixture_wav)) == (ssize_t)sizeof(fixture_wav) && close(fd) == 0);
        struct snag_buf decoded;
        snag_buf_init(&decoded, 96004u);
        assert(snag_buf_append(&decoded, "kept", 4u) == 0);
        assert(snag_av_pcm(file, 0u, 1u, &decoded, audio_cancel, NULL, error, sizeof(error)) == 2 && decoded.len == 4u);
        assert(snag_av_pcm(file, 1u, 0u, &decoded, NULL, NULL, error, sizeof(error)) < 0 && decoded.len == 4u);
        char *resample_path = snag_path_join(temp, "stereo48.wav");
        FILE *pcm_file = fopen(resample_path, "wb");
        unsigned char header48[44]; memcpy(header48, fixture_wav, sizeof(header48));
        const uint32_t values[] = {384036u, 48000u, 192000u, 384000u};
        const size_t offsets[] = {4u, 24u, 28u, 40u};
        for (size_t k = 0; k < 4u; ++k)
            for (size_t b = 0; b < 4u; ++b) header48[offsets[k] + b] = (unsigned char)(values[k] >> (b * 8u));
        assert(pcm_file && fwrite(header48, 1u, sizeof(header48), pcm_file) == sizeof(header48));
        for (size_t i = 0; i < 96000u; ++i) {
            uint16_t left = i < 48000u ? 1000u : 2000u, right = (uint16_t)(0u - left);
            unsigned char pair[] = {(unsigned char)left, (unsigned char)(left >> 8),
                                    (unsigned char)right, (unsigned char)(right >> 8)};
            assert(fwrite(pair, 1u, 4u, pcm_file) == 4u);
        }
        assert(fclose(pcm_file) == 0);
        assert(snag_av_pcm(resample_path, 1u, 2u, &decoded, NULL, NULL, error, sizeof(error)) == 0);
        assert(decoded.len == 96004u && !memcmp(decoded.data, "kept", 4u));
        assert(decoded.data[48004] == 0xd0 && decoded.data[48005] == 0x07 &&
               decoded.data[48006] == 0x30 && decoded.data[48007] == 0xf8);
        snag_buf_reset(&decoded); assert(snag_buf_append(&decoded, "kept", 4u) == 0);
        decoded.max = 20u;
        assert(snag_av_pcm(file, 0u, 1u, &decoded, NULL, NULL, error, sizeof(error)) < 0 && decoded.len == 4u);
        struct snag_av_audio *reader = NULL;
        assert(snag_av_audio_open(resample_path, 1u, 2u, NULL, NULL, &reader, error, sizeof(error)) == 0);
        assert(unlink(resample_path) == 0); /* Open decoder retains its descriptor. */
        uint32_t frames = 0, total = 0, chunks = 0;
        int16_t samples[16384];
        do {
            assert(snag_av_audio_read(reader, samples, &frames, error, sizeof(error)) == 0);
            assert(frames <= 8192u);
            total += frames; ++chunks;
        } while (frames);
        assert(total == 24000u && chunks > 2u);
        assert(snag_av_audio_read(reader, samples, &frames, error, sizeof(error)) == 0 && !frames);
        snag_av_audio_close(reader);
        free(resample_path); snag_buf_free(&decoded);
        strcpy(config.audio.provider, config.providers[0].name);
        strcpy(config.audio.listen_model, "audio-fixture"); strcpy(config.audio.transcribe_model, "audio-fixture");
        strcpy(config.audio.speech_model, "audio-fixture"); strcpy(config.audio.voice, "alloy");
        assert(snag_secret_source_parse(&config.providers[0].api_key, "\"transport-secret\"", NULL, error, sizeof(error)) == 0);
        const char *names[] = {"listen_audio", "transcribe_audio", "speak_text"};
        /* The real graph and context surface must accept every declared tool. */
        json_t *started=json_pack("{s:{s:s,s:s,s:n,s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:b},"
            "s:s,s:b,s:o,s:n,s:n,s:s,s:s,s:I,s:s}",
            "config","capability_version",SNAJPAGENT_CAPABILITY_VERSION,"effort","medium",
            "max_output_tokens","model","audio-fixture","provider","default","profile_id",SNAJPAGENT_PROFILE_ID,
            "prompt_schema",1,"replay_schema",1,"tool_schema",1,"max_parallel_commands",4,"parallel_tool_calls",1,
            "input_kind","direct","read_only",0,"instructions",json_array(),"queue_id","queue_seq",
            "text","Inspect fixture sound","turn_id","01010101010101010101010101010101","turn_number",(json_int_t)1,
            "cwd",session.cwd);
        assert(started && snag_session_commit(&session, "turn_started", started,
            NULL, error, sizeof(error)) == 0);
        struct snag_context_projection projection;
        memset(&projection,0,sizeof(projection));
        json_t *steering = json_array();
        assert(snag_context_build(&session, "audio-fixture", "medium", 1u, steering, 2048u, true,
            &config, NULL, NULL, "Fixture: tool details hidden; report meaningful progress.",
            &projection, error, sizeof(error), NULL) == 0);
        json_t *input = json_object_get(projection.create_request.value, "input");
        assert(input == json_object_get(projection.count_request.value, "input"));
        bool visible = false;
        for (size_t i = 0; i < json_array_size(input); ++i) {
            json_t *item = json_array_get(input, i);
            const char *role = snag_json_string(item, "role");
            const char *content = snag_json_string(item, "content");
            if (role && !strcmp(role, "system") && content &&
                !strcmp(content, "Fixture: tool details hidden; report meaningful progress."))
                visible = true;
        }
        assert(visible);
        json_t *tools = json_object_get(projection.create_request.value, "tools");
        for (size_t i = 0; i < 3u; ++i) {
            bool declared = false;
            for (size_t j = 0; j < json_array_size(tools); ++j) {
                const char *name = snag_json_string(json_array_get(tools, j), "name");
                if (name && !strcmp(name, names[i])) {
                    declared = true;
                    json_t *properties = json_object_get(json_object_get(json_array_get(tools, j),
                        "parameters"), "properties");
                    for (void *it = json_object_iter(properties); it;
                         it = json_object_iter_next(properties, it)) {
                        const char *description = snag_json_string(json_object_iter_value(it), "description");
                        assert(description && *description);
                    }
                }
            }
            assert(declared);
        }
        snag_context_projection_free(&projection); json_decref(steering);
        /* Invalid arguments must fail locally, with no provider fixture running. */
        const char *bad_audio[] = {
            "{}", "{\"path\":null}", "{\"path\":\"x\",\"start_s\":\"null\"}",
            "{\"path\":\"x\",\"start_s\":86401}", "{\"path\":\"x\",\"end_s\":0}",
            "{\"path\":\"x\",\"end_s\":61}", "{\"path\":\"x\",\"extra\":null}"
        };
        for (size_t i = 0; i < sizeof(bad_audio) / sizeof(bad_audio[0]); ++i) {
            struct snag_response_item invalid = {.name = "transcribe_audio",
                .arguments = json_loadb(bad_audio[i], strlen(bad_audio[i]), 0, NULL)};
            json_t *failure = NULL;
            assert(snag_tools_audio(&invalid, &session, store.root_fd, &config, NULL, NULL,
                                    SNAG_WAKE_INVALID, &failure) == 0);
            assert(!strcmp(snag_json_string(failure, "status"), "failed"));
            json_decref(failure); json_decref(invalid.arguments);
        }
        for (unsigned invalid_mode = 0; invalid_mode < 3u; ++invalid_mode) {
            struct snag_response_item invalid = {.name = invalid_mode ? "speak_text" : "listen_audio"};
            invalid.arguments = invalid_mode ? json_pack("{s:s}", "text", "x") : json_pack("{s:s}", "path", file);
            if (invalid_mode == 1u)
                assert(json_object_set_new(invalid.arguments, "extra", json_null()) == 0);
            if (invalid_mode == 2u)
                assert(json_object_set_new(invalid.arguments, "text", json_stringn("x\0y", 3u)) == 0);
            json_t *failure = NULL;
            assert(snag_tools_audio(&invalid, &session, store.root_fd, &config, NULL, NULL,
                                    SNAG_WAKE_INVALID, &failure) == 0);
            assert(!strcmp(snag_json_string(failure, "status"), "failed"));
            json_decref(failure); json_decref(invalid.arguments);
        }
        for (int mode = MODEL_AUDIO_LISTEN; mode <= MODEL_AUDIO_SPEAK; ++mode) {
            start_server(&server, (enum model_fixture)mode, false, "");
            snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u/v1", server.port);
            assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", endpoint, 1) == 0);
            struct snag_response_item call = {.name = (char *)names[mode - MODEL_AUDIO_LISTEN]};
            call.arguments = mode == MODEL_AUDIO_SPEAK ? json_pack("{s:s}", "text", "fixture text") :
                json_pack("{s:s,s:i,s:i}", "path", file, "start_s", 0, "end_s", 1);
            if (mode == MODEL_AUDIO_LISTEN) {
                assert(json_object_set_new(call.arguments, "question", json_string("What sounds?")) == 0);
                assert(json_object_del(call.arguments, "start_s") == 0);
            }
            if (mode == MODEL_AUDIO_TRANSCRIBE) {
                assert(json_object_del(call.arguments, "start_s") == 0);
                assert(json_object_del(call.arguments, "end_s") == 0);
            }
            struct snag_response_graph graph;
            memset(&graph,0,sizeof(graph));
            assert(snag_response_graph_add_call(&graph, "audio-item", "audio-call", call.name,
                json_incref(call.arguments)) == 0);
            snag_response_graph_free(&graph);
            json_t *result = NULL;
            assert(snag_tools_audio(&call, &session, store.root_fd, &config, NULL, NULL, SNAG_WAKE_INVALID, &result) == 0);
            if (strcmp(snag_json_string(result, "status"), "succeeded")) fprintf(stderr, "audio: %s\n", snag_json_string(result, "model_text"));
            assert(!strcmp(snag_json_string(result, "status"), "succeeded") && snag_tool_result_valid(result) == 0);
            assert(strstr(snag_json_string(result, "model_text"), "audio-fixture"));
            json_t *parts = json_object_get(result, "content");
            assert(json_array_size(parts) == (mode == MODEL_AUDIO_SPEAK ? 1u : 5u));
            if (mode != MODEL_AUDIO_SPEAK) {
                assert(strstr(snag_json_string(json_array_get(parts, 3u), "text"), "Auxiliary audio API usage"));
                const char *reported = snag_json_string(json_array_get(parts, 4u), "text");
                assert(reported && strstr(reported, mode == MODEL_AUDIO_LISTEN ? "total_tokens" : "seconds"));
            }
            json_decref(result); json_decref(call.arguments);
            stop_server(&server);
        }
        char *video = snag_path_join(temp, "sound.mkv");
        const char *args[] = {"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i",
            "color=red:s=64x64:r=4:d=1", "-i", file, "-map", "0:v:0", "-map", "1:a:0",
            "-c:v", "mpeg4", "-c:a", "copy", "-threads", "1", video, NULL};
        snag_buf_reset(&out);
        assert(snag_convert(args, temp, &out, NULL, NULL, SNAG_WAKE_INVALID, error, sizeof(error)) == 0);
        struct snag_response_item call = {.kind = SNAG_ITEM_TOOL_CALL, .name = "view_video"};
        call.arguments = json_pack("{s:s,s:i,s:i,s:i}", "path", video, "start_s", 0, "end_s", 1, "frames", 2);
        struct video_audio_fixture fixture = {.session = &session, .config = &config,
            .root_fd = store.root_fd, .original = video};
        start_server(&server, MODEL_AUDIO_TRANSCRIBE, false, "");
        snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u/v1", server.port);
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", endpoint, 1) == 0);
        json_t *result = NULL;
        assert(snag_tools_video(&call, &session, NULL, &fixture, SNAG_WAKE_INVALID,
            fixture_video_transcribe, &result) == 0);
        if (strcmp(snag_json_string(result, "status"), "succeeded")) fprintf(stderr, "video audio: %s\n", snag_json_string(result, "model_text"));
        assert(!strcmp(snag_json_string(result, "status"), "succeeded") && snag_tool_result_valid(result) == 0);
        json_t *parts = json_object_get(result, "content");
        assert(fixture.calls == 1u && !strcmp(fixture.source,
            snag_json_string(json_object_get(json_array_get(parts, 0u), "asset"), "id")));
        bool transcript_seen = false;
        for (size_t i = 0; i < json_array_size(parts); ++i) {
            const char *text = snag_json_string(json_array_get(parts, i), "text");
            if (text && !strcmp(text, "fixture transcript")) transcript_seen = true;
        }
        assert(transcript_seen);
        json_decref(result); json_decref(call.arguments); free(video); stop_server(&server);
        assert(unlink(file) == 0); free(file);
        snag_session_close(&session); snag_store_close(&store); free(temp);
    }
    assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
    json_decref(request); snag_buf_free(&out); snag_buf_free(&wav); snag_config_free(&config);
}

/* Small RFC 6455 wire fixture only; production framing belongs to libcurl. */
static void
ws_read_full(int fd,void *data,size_t length)
{
    unsigned char *p=data;
    while(length) {
        ssize_t n=read(fd,p,length);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)server_fail("WebSocket fixture read failed");
        p+=n;length-=(size_t)n;
    }
}

static unsigned int
ws_read_client(int fd,size_t expected,unsigned char byte)
{
    unsigned char h[2],mask[4],extended[8],buffer[4096];
    ws_read_full(fd,h,2u);
    if(!(h[0]&0x80u) || !(h[1]&0x80u))server_fail("WebSocket fixture expected final masked frame");
    uint64_t length=h[1]&127u;
    if(length>=126u) {
        size_t n=length==126u?2u:8u;length=0;
        ws_read_full(fd,extended,n);
        for(size_t i=0;i<n;++i)length=(length<<8u)|extended[i];
    }
    if(length!=expected)server_fail("WebSocket fixture payload length mismatch");
    ws_read_full(fd,mask,4u);
    for(size_t offset=0;offset<expected;) {
        size_t n=expected-offset;if(n>sizeof(buffer))n=sizeof(buffer);
        ws_read_full(fd,buffer,n);
        for(size_t i=0;i<n;++i)
            if((buffer[i]^mask[(offset+i)%4u])!=byte)server_fail("WebSocket fixture duplicated or changed payload");
        offset+=n;
    }
    return h[0]&15u;
}

static void
ws_server(unsigned int mode,int listen_fd,bool native)
{
    alarm(15u);
    int fd=accept(listen_fd,NULL,NULL);if(fd<0)server_fail("WebSocket fixture accept failed");
    struct http_request request;read_request(fd,&request);
    const char *path = native ? "/backend-api/codex/rtc_native" :
        "/v1/realtime?model=fixture%20voice";
    if (strcmp(request.method,"GET") || strcmp(request.path,path) ||
        !header_contains(request.headers,"Authorization: Bearer transport-secret") ||
        (native && !header_contains(request.headers,"openai-alpha: quicksilver=v2")) ||
        !header_contains(request.headers,"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ=="))
        server_fail("WebSocket handshake URL/nonce mismatch");
    if(mode==4u) {
        send_response(fd,401u,"application/json","{\"error\":\"private denied body\"}");close(fd);_Exit(0);
    }
    if (mode == 5u || mode == 9u) {
        char discard;
        while (read(fd, &discard, 1u) > 0) {}
        close(fd);
        _Exit(0);
    }
    const char *upgrade="HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    write_all_or_die(fd,upgrade,strlen(upgrade));
    if (mode == 6u) {
        close(fd);
        _Exit(0);
    }
    if (mode == 7u) {
        /* Server frames must not be masked. libcurl reports RECV_ERROR here. */
        const unsigned char invalid[] = {0x81, 0x81, 0, 0, 0, 0, 'x'};
        write_all_or_die(fd, (const char *)invalid, sizeof(invalid));
        char discard;
        while (read(fd, &discard, 1u) > 0) {}
        close(fd);
        _Exit(0);
    }
    if(mode) {
        const char *frame=mode==1u?"\202\001x":mode==2u?"\201\176\020\000x":"\201\002\377\377";
        write_all_or_die(fd,frame,mode==1u?3u:mode==2u?5u:4u);
        char discard;while(read(fd,&discard,1u)>0){}close(fd);_Exit(0);
    }
    if(ws_read_client(fd,4u*1024u*1024u,'q')!=1u)server_fail("Expected one complete client text message");
    /* Fragmented text, interleaved ping, and a UTF-8 sequence across fragments. */
    const unsigned char frames[]={0x01,2,'A',0xe2,0x89,1,'p',0x80,3,0x82,0xac,'B'};
    for(size_t i=0;i<sizeof(frames);++i)write_all_or_die(fd,(const char *)frames+i,1u);
    if(ws_read_client(fd,1u,'p')!=10u)server_fail("WebSocket automatic pong missing");
    const unsigned char large[]={0x81,126,0x80,1};
    write_all_or_die(fd,(const char *)large,sizeof(large));
    char block[4096];memset(block,'x',sizeof(block));
    for(size_t left=32769u;left;) {
        size_t n=left>sizeof(block)?sizeof(block):left;
        write_all_or_die(fd,block,n);left-=n;
    }
    write_all_or_die(fd,"\210\002\003\350",4u);
    char discard;while(read(fd,&discard,1u)>0){}close(fd);_Exit(0);
}

static int
ws_cancel(void *opaque,unsigned int timeout)
{
    (void)timeout;
    return snag_monotonic_ms()>=*(uint64_t *)opaque?2:0;
}

static bool
websocket_supported(void)
{
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    for (const char *const *protocol = version ? version->protocols : NULL;
         protocol && *protocol; ++protocol) {
        if (!strcmp(*protocol, "ws")) return true;
    }
    return false;
}

static void
test_voice_socket(void)
{
    struct snag_provider_config provider;struct snag_credential credential;
    snag_config_provider_init(&provider,"default");snag_credential_clear(&credential);
    provider.auth=SNAG_AUTH_API_KEY;provider.connect_timeout_ms=2000u;
    strcpy(provider.openrouter_referer,"https://github.com/snajpa/snajpagent");
    strcpy(provider.openrouter_title,"snajpagent");
    strcpy(credential.value,"transport-secret");credential.len=strlen(credential.value);
    struct snag_voice_socket *voice=NULL;char error[256];
    strcpy(provider.base_url,"ftp://remote.invalid");
    assert(snag_provider_voice_open(&provider,&credential,"fixture",NULL,NULL,&voice,error,sizeof(error))<0 && !voice);
    strcpy(provider.base_url,"http://127.0.0.1:1/v1/");
    assert(snag_provider_voice_open(&provider,&credential,"fixture",NULL,NULL,
        &voice,error,sizeof(error))<0 && !voice);
    if (!websocket_supported()) {
        assert(strstr(error, "lacks WebSocket support"));
        fprintf(stderr, "test_voice_socket: skipped (linked libcurl lacks WebSocket support)\n");
        snag_credential_clear(&credential);
        return;
    }
    assert(strstr(error,"/backend-api/codex"));
    for (unsigned int test = 0; test < 30u; ++test) {
        unsigned int mode = test % 10u;
        unsigned int route = test / 10u;
        bool native = route == 2u;
        provider.connect_timeout_ms = mode == 9u ? 100u : 2000u;
        fprintf(stderr,"WebSocket fixture route %u mode %u\n",route,mode);
        struct local_server server;
        struct sockaddr_in address; socklen_t address_size=sizeof(address);
        memset(&server,0,sizeof(server));memset(&address,0,sizeof(address));
        server.fd=socket(AF_INET,SOCK_STREAM,0);assert(server.fd>=0);
        address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        assert(bind(server.fd, (struct sockaddr *)&address, sizeof(address)) == 0);
        /* A bound, non-listening socket keeps refusal on an owned port. */
        if (mode != 8u) assert(listen(server.fd, 1) == 0);
        assert(getsockname(server.fd,(struct sockaddr *)&address,&address_size)==0);
        server.port=ntohs(address.sin_port);
        if (mode != 8u) {
            server.pid = fork();
            assert(server.pid >= 0);
            if (!server.pid) ws_server(mode, server.fd, native);
            close(server.fd);
            server.fd = -1;
        }
        snprintf(provider.base_url,sizeof(provider.base_url),"http://%s:%u/%s",
            route ? "localhost" : "127.0.0.1",server.port,native ? "backend-api/codex" : "v1/");
        uint64_t started = snag_monotonic_ms();
        uint64_t deadline = started + 200u;
        int rc = native ? snag_provider_voice_attach(&provider,&credential,"rtc_native",
            mode==5u?ws_cancel:NULL,&deadline,&voice,error,sizeof(error)) :
            snag_provider_voice_open(&provider,&credential,"fixture voice",mode==5u?ws_cancel:NULL,
                &deadline,&voice,error,sizeof(error));
        if (mode == 8u) {
            assert(rc == SNAG_PROVIDER_VOICE_RETRY && !voice);
            if (native) {
                struct snag_buf answer = {.max = 32768u};
                char call[257] = {0};
                json_t *session = json_pack("{s:s}", "model", "gpt-live-1-codex");
                assert(session);
                rc = snag_provider_voice_call(NULL, &provider, &credential,
                    "v=0\r\n", session, NULL, NULL, &answer, call, NULL, error, sizeof(error));
                assert(rc == SNAG_PROVIDER_VOICE_RETRY && !call[0] && !answer.len);
                json_decref(session);
                snag_buf_free(&answer);
            }
            close(server.fd);
            continue;
        }
        if (mode == 4u || mode == 5u || mode == 9u) {
            int expected = mode == 9u ? SNAG_PROVIDER_VOICE_RETRY : mode == 5u ? 2 : -1;
            uint64_t elapsed = snag_monotonic_ms() - started;
            if (mode == 9u) assert(elapsed >= provider.connect_timeout_ms && elapsed < 1000u);
            if (rc != expected || voice) {
                fprintf(stderr, "WebSocket handshake rc=%d expected=%d elapsed=%" PRIu64 ": %s\n",
                    rc, expected, elapsed, error);
            }
            assert(rc == expected && !voice);
            if(mode==4u)assert(strstr(error,"401") && !strstr(error,"private denied body"));
            stop_server(&server);continue;
        }
        if (rc) {
            fprintf(stderr,"WebSocket fixture: %s\n",error);
            int status;
            (void)kill(server.pid,SIGTERM);
            assert(waitpid(server.pid,&status,0)==server.pid);
        }
        assert(rc==0 && voice);
        struct snag_buf received;snag_buf_init(&received,mode?64u:65536u);
        deadline=snag_monotonic_ms()+10000u;
        if(!mode) {
            size_t length=4u*1024u*1024u,offset=0u;char *payload=malloc(length);assert(payload);
            memset(payload,'q',length);
            while(offset<length) {
                assert(snag_monotonic_ms()<deadline);
                assert(snag_provider_voice_send(voice,payload,length,&offset,error,sizeof(error))==0);
                if(offset<length)assert(snag_provider_voice_wait(voice,true,20u)==0);
            }
            free(payload);
        }
        unsigned int messages=0;
        do {
            assert(snag_monotonic_ms()<deadline);
            rc=snag_provider_voice_receive(voice,&received,error,sizeof(error));
            if(rc==1) {
                assert(!mode && messages<2u);
                if(!messages)assert(received.len==5u && !memcmp(received.data,"A\342\202\254B",5u));
                else {
                    assert(received.len==32769u);
                    for(size_t i=0;i<received.len;++i)assert(received.data[i]=='x');
                }
                ++messages;snag_buf_reset(&received);
            }
            if(rc==0)assert(snag_provider_voice_wait(voice,false,20u)==0);
        }while(rc>=0);
        assert(messages==(mode?0u:2u));
        assert(rc == (mode == 6u ? SNAG_PROVIDER_VOICE_RETRY : -1));
        snag_provider_voice_close(voice);voice=NULL;snag_buf_free(&received);
        stop_server(&server);
    }
    snag_credential_clear(&credential);
}

struct voice_fixture {
    json_t *sent,*notices;
    uint32_t played,frames,interrupts,ends;
    const char *audio_item;
    bool busy;
};
static int voice_send(void *opaque,const json_t *event)
{
    return json_array_append(((struct voice_fixture *)opaque)->sent,(json_t *)event);
}
static int voice_notice(void *opaque,const json_t *event)
{
    struct voice_fixture *f = opaque;
    const char *type = snag_json_string(event, "type");
    if (f->busy && type && !strcmp(type, "voice_handoff")) return 1;
    return json_array_append(((struct voice_fixture *)opaque)->notices,(json_t *)event);
}
static int voice_play(void *opaque,const char *item,const int16_t *pcm,uint32_t frames)
{
    struct voice_fixture *f=opaque;
    assert(!strcmp(item,f->audio_item?f->audio_item:"audio-1"));
    if(!frames) {assert(!pcm);++f->ends;return 0;}
    assert(frames==24u);
    for(uint32_t i=0;i<frames;++i)assert(pcm[i]==(int16_t)i-12);
    f->frames+=frames;return 0;
}
static uint32_t voice_interrupt(void *opaque)
{
    struct voice_fixture *f=opaque;++f->interrupts;return f->played;
}
static json_t *voice_last(json_t *events)
{
    assert(json_array_size(events));return json_array_get(events,json_array_size(events)-1u);
}
static int voice_deliver(struct snag_voice *voice,json_t *event)
{
    char error[256];assert(event);
    int rc=snag_voice_event(voice,event,error,sizeof(error));json_decref(event);return rc;
}
static struct snag_voice *
voice_fixture_new(const struct snag_voice_io *io, void *opaque, const char *model,
    const char *transcribe, const char *voice)
{
    struct snag_buf help = {.max = 64u * 1024u};
    assert(snag_app_help_text(&help, NULL) == 0);
    struct snag_voice *protocol = snag_voice_new(io, opaque, model, transcribe,
        voice, (const char *)help.data);
    snag_buf_free(&help);
    return protocol;
}

static void
voice_orientation(const json_t *session)
{
    const char *instructions = snag_json_string(session, "instructions");
    assert(instructions && strstr(instructions, "voice model"));
    assert(strstr(instructions, "The model means the working model"));
    assert(strstr(instructions, "The CLI is another interface to this same session"));
    assert(strstr(instructions, "do not automatically request new work"));
    assert(strstr(instructions, "do not authenticate a speaker or add permissions"));
    struct snag_buf help = {.max = 64u * 1024u};
    assert(snag_app_help_text(&help, NULL) == 0 && help.len);
    const char *commands = strstr(instructions, (const char *)help.data);
    assert(commands && commands > instructions && !strcmp(commands, (const char *)help.data));
    snag_buf_free(&help);
}

static struct snag_voice *voice_start(struct voice_fixture *f)
{
    memset(f,0,sizeof(*f));f->sent=json_array();f->notices=json_array();assert(f->sent && f->notices);
    struct snag_voice_io io={voice_send,voice_notice,voice_play,voice_interrupt};
    struct snag_voice *voice=voice_fixture_new(&io,f,"fixture-model","fixture-asr","fixture-voice");
    char error[256];assert(voice && !snag_voice_ready(voice));
    assert(snag_voice_begin(voice,error,sizeof(error))==0);
    json_t *session=json_object_get(voice_last(f->sent),"session");assert(json_is_object(session));
    voice_orientation(session);
    assert(json_array_size(json_object_get(session,"tools"))==1u);
    assert(voice_deliver(voice,json_pack("{s:s,s:O}","type","session.updated","session",session))==0);
    assert(snag_voice_ready(voice));return voice;
}
static void voice_end(struct snag_voice *voice,struct voice_fixture *f)
{
    snag_voice_free(voice);json_decref(f->sent);json_decref(f->notices);
}

static size_t voice_notice_count(const struct voice_fixture *,const char *);

static int
voice_error_record(void *opaque, const struct snag_session *session, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t size)
{
    (void)session;
    (void)seq;
    (void)error;
    (void)size;
    if (strcmp(type, "voice_event")) return 0;
    const json_t *event = json_object_get(data, "event");
    const char *operation = snag_json_string(event, "operation");
    if (!operation || strcmp(operation, "provider_error")) return 0;
    const json_t *failure = json_object_get(event, "error");
    assert(!strcmp(snag_json_string(failure, "code"), "context_length_exceeded"));
    assert(!strcmp(snag_json_string(failure, "message"), "reflected <redacted:secret>"));
    assert(json_integer_value(json_object_get(failure, "max_context_tokens")) == 12345);
    assert(json_integer_value(json_object_get(failure, "input_tokens")) == 12346);
    ++*(unsigned int *)opaque;
    return 0;
}

static void
test_voice_provider_errors(void)
{
    for (unsigned int native = 0u; native < 2u; ++native) {
        for (unsigned int shape = 0u; shape < 7u; ++shape) {
            struct voice_fixture f;
            struct snag_voice *voice;
            char error[256];
            if (!native) {
                voice = voice_start(&f);
            } else {
                f = (struct voice_fixture){.sent = json_array(), .notices = json_array()};
                struct snag_voice_io io = {voice_send, voice_notice, voice_play, voice_interrupt};
                voice = voice_fixture_new(&io, &f, "fixture", "fixture-asr", "fixture-voice");
                assert(voice);
                json_t *session = snag_voice_native_session(voice);
                assert(session);
                json_decref(session);
                assert(snag_voice_begin(voice, error, sizeof(error)) == 0);
            }
            const char *code = shape == 1u ? "server_error" :
                shape == 2u ? "invalid_request_error" :
                shape == 5u ? "content_filter" : "context_length_exceeded";
            const char *category = shape == 1u ? "server_error" :
                shape >= 5u ? "content_filter" : "invalid_request_error";
            json_t *event = json_pack("{s:s,s:{s:s,s:s,s:s}}", "type", "error", "error",
                "code", code, "type", category,
                "message", !shape ? "reflected voice-error-secret" : "context window exhausted");
            json_t *detail = json_object_get(event, "error");
            assert(event && detail);
            if (!shape) {
                assert(json_object_set_new(detail, "max_context_tokens", json_integer(12345)) == 0);
                assert(json_object_set_new(detail, "input_tokens", json_integer(12346)) == 0);
            } else if (shape == 3u) {
                assert(json_object_set_new(detail, "code", json_array()) == 0);
            } else if (shape == 4u) {
                assert(json_object_set_new(detail, "max_context_tokens", json_integer(-1)) == 0);
            }
            size_t sent = json_array_size(f.sent);
            int outcome = snag_voice_event(voice, event, error, sizeof(error));
            assert(outcome == (!shape ? SNAG_VOICE_CAPACITY :
                shape == 1u ? SNAG_VOICE_RETRY : -1));
            if (shape >= 5u) assert(strstr(error, "policy"));
            assert(!snag_voice_ready(voice) && f.interrupts == 1u);
            assert(json_array_size(f.sent) == sent && !voice_notice_count(&f, "voice_handoff"));
            bool valid = shape < 3u || shape >= 5u;
            assert(json_array_size(f.notices) == (size_t)valid);
            if (valid) {
                const json_t *report = voice_last(f.notices);
                assert(!strcmp(snag_json_string(report, "type"), "voice_response"));
                assert(!strcmp(snag_json_string(report, "operation"), "provider_error"));
                struct snag_provider_failure failure;
                assert(snag_provider_failure_from_json(report, &failure) == 0);
                assert(!strcmp(failure.code, code));
                assert(snag_provider_failure_is_capacity(&failure) == !shape);
                assert(snag_provider_failure_is_policy(&failure) == (shape >= 5u));
                assert(snag_provider_failure_retryable(0, failure.code, failure.type) ==
                    (shape == 1u));
                assert(failure.context_limit_tokens == (!shape ? 12345u : 0u));
                assert(failure.requested_input_tokens == (!shape ? 12346u : 0u));
                if (shape) {
                    assert(json_is_null(json_object_get(json_object_get(report, "error"),
                        "max_context_tokens")));
                    assert(json_is_null(json_object_get(json_object_get(report, "error"),
                        "input_tokens")));
                }
            }
            assert(snag_voice_event(voice, event, error, sizeof(error)) < 0);
            assert(json_array_size(f.notices) == (size_t)valid && f.interrupts == 1u);
            json_decref(event);
            if (native && !shape) {
                char path[4096];
                const char *tmp = getenv("TMPDIR");
                assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-error-XXXXXX",
                    tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
                struct snag_config config;
                snag_config_init(&config);
                assert(snag_secret_source_parse(&config.providers[0].api_key,
                    "\"voice-error-secret\"", NULL, error, sizeof(error)) == 0);
                struct app_state app = {.config = &config};
                snag_store_init(&app.store);
                snag_session_init(&app.session);
                assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
                assert(snag_session_create(&app.store, &app.session, path, "default",
                    "fixture", "medium", error, sizeof(error)) == 0);
                assert(snag_ui_init(&app.ui) == 0);
                assert(snag_app_voice_fixture(&app, f.notices, true) == 0);
                assert(snag_app_voice_service(&app) == 0 && !app.voice);
                unsigned int records = 0u;
                assert(snag_session_each_event(&app.session, voice_error_record, &records,
                    error, sizeof(error)) == 0 && records == 1u);
                assert(!app.session.active_turn && !app.session.pending_queue_count &&
                    app.session.usage_totals.responses == 0u);
                snag_ui_free(&app.ui);
                snag_session_close(&app.session);
                snag_store_close(&app.store);
                snag_config_free(&config);
            }
            voice_end(voice, &f);
        }
    }
}

static void test_audio_provider_selection(void)
{
    struct snag_config config;snag_config_init(&config);
    struct snag_audio_config resolved;
    const struct snag_provider_config *provider=snag_provider_audio_config(&config,NULL,&resolved);
    assert(provider && !strcmp(resolved.provider,provider->name));
    assert(!strcmp(resolved.realtime_model,"gpt-realtime") && !strcmp(resolved.voice,"marin"));
    assert(!config.audio.provider[0] && !config.audio.realtime_model[0]);
    strcpy(config.providers[0].base_url,SNAG_CHATGPT_BASE);
    config.providers[0].auth=SNAG_AUTH_CHATGPT;
    provider=snag_provider_audio_config(&config,NULL,&resolved);
    assert(provider && snag_provider_native_audio(provider));
    assert(!strcmp(resolved.realtime_model,"gpt-live-1-codex") && !strcmp(resolved.voice,"cove"));
    assert(!strcmp(resolved.transcribe_model,"gpt-4o-transcribe"));
    config.providers[0].auth=SNAG_AUTH_API_KEY;
    strcpy(config.providers[0].base_url,"http://127.0.0.1:2455/backend-api/codex/");
    assert(snag_provider_native_audio(snag_provider_audio_config(&config,NULL,&resolved)));
    config.provider_count=2u;snag_config_provider_init(&config.providers[1],"byok");
    strcpy(config.audio.provider,"byok");strcpy(config.audio.realtime_model,"custom-voice-model");
    strcpy(config.audio.voice,"custom-voice");
    provider=snag_provider_audio_config(&config,config.providers[0].name,&resolved);
    assert(provider==&config.providers[1] && !snag_provider_native_audio(provider));
    assert(!strcmp(resolved.realtime_model,"custom-voice-model") &&
        !strcmp(resolved.voice,"custom-voice"));
    snag_config_free(&config);
}

static void
voice_preview(const struct voice_fixture *f, const char *user, const char *assistant)
{
    json_t *captions = snag_app_voice_fixture_captions(f->notices);
    assert(captions);
    assert(!strcmp(snag_json_string(captions, "user"), user));
    assert(!strcmp(snag_json_string(captions, "assistant"), assistant));
    json_decref(captions);
}

static void
test_native_voice_caption_mirrors(void)
{
    for (unsigned int who = 0u; who < 2u; ++who) {
        for (unsigned int first = 0u; first < 2u; ++first) {
            struct voice_fixture f = {.sent = json_array(), .notices = json_array(),
                .audio_item = "native-output"};
            struct snag_voice_io io = {voice_send, voice_notice, voice_play, voice_interrupt};
            struct snag_voice *v = voice_fixture_new(&io, &f, "gpt-live-1-codex",
                "gpt-4o-transcribe", "cove");
            assert(v);
            json_decref(snag_voice_native_session(v));
            char error[256];
            assert(snag_voice_begin(v, error, sizeof(error)) == 0);
            const char *role = who ? "assistant" : "user";
            const char *stream = who ? "output_transcript.added" : "input_transcript.added";
            /* Either feed may arrive first. Turn snapshots/deltas may mirror
             * the same words, but may not replace or double the caption stream. */
            for (unsigned int i = 0u; i < 2u; ++i) {
                json_t *event = i == first ? json_pack("{s:s,s:{s:s}}",
                    "type", stream, "item", "text", "one") :
                    json_pack("{s:s,s:{s:s,s:s,s:s}}", "type", "turn.created", "turn",
                        "id", "preview-turn", "role", role, "transcript", "one");
                assert(voice_deliver(v, event) == 0);
            }
            voice_preview(&f, who ? "" : "one", who ? "one" : "");
            assert(voice_deliver(v, json_pack("{s:s,s:s,s:s}", "type", "turn.delta",
                "turn_id", "preview-turn", "delta", " two")) == 0);
            assert(voice_deliver(v, json_pack("{s:s,s:{s:s}}", "type", stream,
                "item", "text", " two")) == 0);
            voice_preview(&f, who ? "" : "one two", who ? "one two" : "");
            assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}", "type", "turn.created",
                "turn", "id", "preview-turn", "role", role, "transcript", "one two")) == 0);
            assert(voice_deliver(v, json_pack("{s:s,s:{s:s}}", "type", stream,
                "item", "text", " three.")) == 0);
            voice_preview(&f, who ? "" : "one two three.", who ? "one two three." : "");
            assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}", "type", "turn.done",
                "turn", "id", "preview-turn", "role", role,
                "transcript", "one two three.")) == 0);
            voice_preview(&f, "", "");
            voice_end(v, &f);
        }
    }
}

static void
test_native_voice_transcript_events(void)
{
    struct voice_fixture f = {.sent = json_array(), .notices = json_array(),
        .audio_item = "native-output"};
    struct snag_voice_io io = {voice_send, voice_notice, voice_play, voice_interrupt};
    struct snag_voice *v = voice_fixture_new(&io, &f, "gpt-live-1-codex",
        "gpt-4o-transcribe", "cove");
    assert(v);
    json_decref(snag_voice_native_session(v));
    char error[256];
    assert(snag_voice_begin(v, error, sizeof(error)) == 0);
    /* Native transcript deltas precede turn.done and have no utterance identity. */
    assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
        "type", "input_transcript.added", "item", "id", "input-1",
        "type", "input_transcript", "text", "hello")) == 0);
    assert(voice_notice_count(&f, "voice_caption") == 1u);
    assert(!strcmp(snag_json_string(voice_last(f.notices), "speaker"), "user"));
    assert(!strcmp(snag_json_string(voice_last(f.notices), "text"), "hello"));
    assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
        "type", "output_transcript.added", "item", "id", "output-1",
        "type", "output_transcript", "text", "Hello there.")) == 0);
    assert(voice_notice_count(&f, "voice_caption") == 2u);
    assert(!strcmp(snag_json_string(voice_last(f.notices), "speaker"), "assistant"));
    assert(!strcmp(snag_json_string(voice_last(f.notices), "text"), "Hello there."));
    assert(voice_notice_count(&f, "voice_handoff") == 0u);
    voice_preview(&f, "hello", "Hello there.");
    assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s}}",
        "type", "input_transcript.added", "item", "id", "input-2",
        "text", " world.")) == 0);
    voice_preview(&f, "hello world.", "Hello there.");
    /* Reusing an optional item ID must not discard a new delta. */
    assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s}}",
        "type", "input_transcript.added", "item", "id", "input-2",
        "text", " Next")) == 0);
    assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s}}",
        "type", "output_transcript.added", "item", "id", "output-2",
        "text", " Next response.")) == 0);
    voice_preview(&f, "hello world. Next", "Hello there. Next response.");
    assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
        "type", "turn.done", "turn", "id", "user-turn-1", "role", "user",
        "transcript", "hello world.")) == 0);
    voice_preview(&f, " Next", "Hello there. Next response.");
    for (unsigned int i = 0; i < 2u; ++i) {
        assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
            "type", "turn.done", "turn", "id", "assistant-turn-1", "role", "assistant",
            "transcript", "Hello there.")) == 0);
    }
    assert(voice_notice_count(&f, "voice_transcript") == 2u && f.ends == 1u);
    voice_preview(&f, " Next", " Next response.");
    /* A repeated creation snapshot cannot resurrect a completed turn. */
    size_t before = json_array_size(f.notices);
    assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
        "type", "turn.created", "turn", "id", "assistant-turn-1", "role", "assistant",
        "transcript", "Hello there.")) == 0);
    assert(json_array_size(f.notices) == before);
    assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
        "type", "turn.done", "turn", "id", "user-turn-2", "role", "user",
        "transcript", " Next")) == 0);
    assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
        "type", "turn.done", "turn", "id", "assistant-turn-2", "role", "assistant",
        "transcript", " Next response.")) == 0);
    voice_preview(&f, "", "");
    assert(snag_voice_mute(v, true, error, sizeof(error)) == 0);
    before = json_array_size(f.notices);
    assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s}}",
        "type", "input_transcript.added", "item", "id", "muted-input",
        "text", "Must not appear")) == 0);
    assert(json_array_size(f.notices) == before);
    assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s}}",
        "type", "output_transcript.added", "item", "id", "unmuted-output",
        "text", "Still speaking")) == 0);
    voice_preview(&f, "", "Still speaking");
    assert(snag_voice_mute(v, false, error, sizeof(error)) == 0);
    /* Missing optional segment IDs do not invent an utterance association. */
    assert(voice_deliver(v, json_pack("{s:s,s:{s:s}}",
        "type", "input_transcript.added", "item", "text", "More speech")) == 0);
    voice_preview(&f, "More speech", "Still speaking");
    assert(voice_notice_count(&f, "voice_handoff") == 0u);
    /* A long UTF-8 segment keeps a valid display tail; its final retires that tail. */
    char long_text[1008];
    memcpy(long_text, "Start ", 6u);
    for (size_t i = 0; i < 500u; ++i) {
        memcpy(long_text + 6u + 2u * i, "\xc5\xbe", 2u);
    }
    memcpy(long_text + 1006u, "!", 2u);
    assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s}}",
        "type", "input_transcript.added", "item", "id", "long-input",
        "text", long_text)) == 0);
    voice_preview(&f, long_text + 624u, "Still speaking");
    assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
        "type", "turn.done", "turn", "id", "long-turn", "role", "user",
        "transcript", long_text)) == 0);
    voice_preview(&f, "", "Still speaking");
    voice_end(v, &f);
    /* A fresh identified-only connection retains the other caption path. A
     * connection already supplying native segments does not switch to mirrors. */
    f = (struct voice_fixture){.sent = json_array(), .notices = json_array(),
        .audio_item = "native-output"};
    v = voice_fixture_new(&io, &f, "gpt-live-1-codex", "gpt-4o-transcribe", "cove");
    assert(v);
    json_decref(snag_voice_native_session(v));
    assert(snag_voice_begin(v, error, sizeof(error)) == 0);
    /* Public deltas stay item-scoped; native creation text is a replacement snapshot. */
    assert(json_array_append_new(f.notices, json_pack("{s:s,s:s,s:s,s:s}",
        "type", "voice_caption", "speaker", "user", "item_id", "public-1",
        "text", "public")) == 0);
    assert(json_array_append_new(f.notices, json_pack("{s:s,s:s,s:s,s:s}",
        "type", "voice_caption", "speaker", "user", "item_id", "public-1",
        "text", " delta")) == 0);
    voice_preview(&f, "public delta", "");
    assert(json_array_append_new(f.notices, json_pack("{s:s,s:s,s:s,s:s}",
        "type", "voice_transcript", "speaker", "user", "item_id", "public-1",
        "text", "public delta")) == 0);
    for (unsigned int i = 0; i < 2u; ++i) {
        assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
            "type", "turn.created", "turn", "id", "snapshot", "role", "assistant",
            "transcript", "Snapshot")) == 0);
    }
    voice_preview(&f, "", "Snapshot");
    assert(voice_deliver(v, json_pack("{s:s,s:{s:s}}",
        "type", "input_transcript.added", "item", "id", "missing-text")) < 0);
    voice_end(v, &f);
}

static void test_native_voice_playback(void)
{
    struct voice_fixture f={.sent=json_array(),.notices=json_array(),.audio_item="native-output"};
    struct snag_voice_io io={.send=voice_send,.notice=voice_notice,
        .play=voice_play,.interrupt=voice_interrupt};
    struct snag_voice *v=voice_fixture_new(&io,&f,"gpt-live-1-codex","gpt-4o-transcribe","marin");
    assert(v);
    json_decref(snag_voice_native_session(v));
    char error[256];int16_t pcm[24];
    for (uint32_t i=0;i<24u;++i)pcm[i]=(int16_t)i-12;
    assert(snag_voice_native_output(v,pcm,24u)<0);
    assert(snag_voice_begin(v,error,sizeof(error))==0);
    /* RTC has no turn ID and can precede sideband assistant metadata. */
    assert(snag_voice_native_output(v,pcm,24u)==0 && f.frames==24u);
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s}}","type","turn.created",
        "turn","id","user-1","role","user"))==0 && f.interrupts==1u);
    /* Barge-in flushes queued speech, not the entire future RTC stream. */
    assert(snag_voice_native_output(v,pcm,24u)==0 && f.frames==48u);
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s}}","type","turn.created",
        "turn","id","audio-1","role","assistant"))==0);
    assert(snag_voice_native_output(v,pcm,24u)==0 && f.frames==72u);
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s,s:s}}","type","turn.done",
        "turn","id","audio-1","role","assistant","transcript","Hello."))==0);
    assert(f.ends==1u);
    assert(snag_voice_native_output(v,pcm,24u)==0 && f.frames==96u);
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s}}","type","turn.created",
        "turn","id","user-2","role","user"))==0 && f.interrupts==2u);
    assert(snag_voice_mute(v,true,error,sizeof(error))==0);
    assert(snag_voice_native_output(v,pcm,24u)==0 && f.frames==120u);
    voice_end(v,&f);
}

static void test_native_voice_protocol(void)
{
    struct voice_fixture f={.sent=json_array(),.notices=json_array()};char error[256];
    struct snag_voice_io io={voice_send,voice_notice,voice_play,voice_interrupt};
    struct snag_voice *v=voice_fixture_new(&io,&f,"gpt-live-1-codex","gpt-4o-transcribe","cove");
    json_t *session=snag_voice_native_session(v);
    assert(session &&
        !strcmp(snag_json_string(json_object_get(session,"delegation"),"type"),"client"));
    voice_orientation(session);
    json_decref(session);
    assert(snag_voice_begin(v,error,sizeof(error))==0 && snag_voice_ready(v));
    assert(json_array_size(f.sent)==0u); /* Existing native call, no public session.update. */
    json_t *context = json_pack("{s:s}", "active_turn_id", "fixture");
    assert(snag_voice_context(v, context, error, sizeof(error)) == 0);
    json_decref(context);
    assert(!strcmp(snag_json_string(voice_last(f.sent), "channel"), "commentary"));
    json_t *created=json_pack("{s:s,s:{s:s,s:s}}","type","turn.created",
        "turn","id","native-input","role","user");
    assert(voice_deliver(v,created)==0 && f.interrupts==1u);
    json_t *delegation=json_pack("{s:s,s:{s:s,s:s,s:s,s:s,s:[{s:s,s:s}]}}",
        "type","delegation.created",
        "item","id","native-call","type","delegation","target","client",
        "user_bidi_turn_id","native-input",
        "content","type","input_text","text","inspect the worktree");
    assert(voice_deliver(v,json_incref(delegation))==0);
    assert(voice_notice_count(&f,"voice_handoff")==0u); /* Wait for final user transcript. */
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s,s:s}}","type","turn.done","turn",
        "id","native-input","role","user","transcript","please inspect the worktree"))==0);
    assert(voice_notice_count(&f,"voice_handoff")==1u &&
        voice_notice_count(&f,"voice_transcript")==1u);
    assert(voice_deliver(v,json_incref(delegation))==0 &&
        voice_notice_count(&f,"voice_handoff")==1u);
    /* Coding can remain pending while a later spoken instruction arrives. */
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s,s:s}}","type","turn.done","turn",
        "id","native-followup","role","user","transcript","also check the tests"))==0);
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s,s:s,s:[{s:s,s:s}]}}",
        "type","delegation.created","item","id","native-call-2","target","client",
        "user_bidi_turn_id","native-followup","content","type","input_text",
        "text","check the tests"))==0);
    assert(voice_notice_count(&f,"voice_handoff")==2u && snag_voice_ready(v));
    assert(snag_voice_progress(v, "native-call-2", "request accepted",
        error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(voice_last(f.sent), "type"), "delegation.context.append"));
    assert(snag_voice_output(v, "Model output:\nThe build finished.",
        error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(voice_last(f.sent), "type"), "session.context.append"));
    assert(!strcmp(snag_json_string(voice_last(f.sent), "channel"), "speakable"));
    assert(!json_object_get(voice_last(f.sent), "delegation_item_id"));
    assert(snag_voice_result(v, "native-call-2", "tests checked", error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(voice_last(f.sent),"delegation_item_id"),"native-call-2"));
    assert(voice_deliver(v,json_incref(delegation))==0 &&
        voice_notice_count(&f,"voice_handoff")==2u);
    assert(snag_voice_result(v,"native-call","inspection complete",error,sizeof(error))==0);
    assert(!strcmp(snag_json_string(voice_last(f.sent),"delegation_item_id"),"native-call"));
    assert(voice_deliver(v,json_incref(delegation))==0 &&
        voice_notice_count(&f,"voice_handoff")==2u && snag_voice_ready(v));
    assert(snag_voice_result(v,"wrong-call","result",error,sizeof(error))<0);
    voice_end(v,&f);json_decref(delegation);

    memset(&f,0,sizeof(f));f.sent=json_array();f.notices=json_array();
    v=voice_fixture_new(&io,&f,"gpt-live-1-codex","gpt-4o-transcribe","cove");
    json_decref(snag_voice_native_session(v));assert(snag_voice_begin(v,error,sizeof(error))==0);
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s,s:s}}","type","turn.done","turn",
        "id","native-input","role","user","transcript","please inspect the worktree"))==0);
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s,s:s,s:[{s:s,s:s}]}}",
        "type","delegation.created",
        "item","id","native-call","target","client","user_bidi_turn_id","native-input",
        "content","type","input_text","text","inspect the worktree"))==0);
    assert(voice_notice_count(&f,"voice_handoff")==1u);
    assert(snag_voice_result(v,"native-call","inspection complete",error,sizeof(error))==0);
    assert(!strcmp(snag_json_string(voice_last(f.sent),"type"),"delegation.context.append"));
    assert(!strcmp(snag_json_string(voice_last(f.sent),"channel"),"speakable"));
    assert(snag_voice_mute(v,true,error,sizeof(error))==0);
    size_t sent=json_array_size(f.sent);
    assert(snag_voice_respond(v,true,error,sizeof(error))==0 && json_array_size(f.sent)==sent);
    voice_end(v,&f);
}

static json_t *
native_delegation(const char *call, const char *input)
{
    return json_pack("{s:s,s:{s:s,s:s,s:s,s:[{s:s,s:s}]}}",
        "type", "delegation.created", "item", "id", call, "target", "client",
        "user_bidi_turn_id", input, "content", "type", "input_text", "text", input);
}

static void
test_native_voice_concurrency(void)
{
    for (unsigned int delayed = 0; delayed < 2u; ++delayed) {
        struct voice_fixture f = {.sent = json_array(), .notices = json_array()};
        struct snag_voice_io io = {voice_send, voice_notice, voice_play, voice_interrupt};
        struct snag_voice *v = voice_fixture_new(&io, &f, "voice", "transcribe", "cove");
        char error[256], ids[SNAG_VOICE_HANDOFFS + 2u][32];

        assert(v);
        json_decref(snag_voice_native_session(v));
        assert(snag_voice_begin(v, error, sizeof(error)) == 0);
        f.busy = true;
        assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
            "type", "turn.done", "turn", "id", "host-busy", "role", "user",
            "transcript", "deferred request")) == 0);
        assert(voice_deliver(v, native_delegation("host-busy", "host-busy")) == 0);
        assert(snag_voice_ready(v) && !voice_notice_count(&f, "voice_handoff"));
        assert(!strcmp(snag_json_string(voice_last(f.sent), "delegation_item_id"), "host-busy"));
        f.busy = false;
        assert(voice_deliver(v, native_delegation("host-busy", "host-busy")) == 0);
        assert(!voice_notice_count(&f, "voice_handoff"));
        for (size_t i = 0; i < SNAG_VOICE_HANDOFFS + 2u; ++i) {
            assert(snprintf(ids[i], sizeof(ids[i]), "native-%zu", i) > 0);
        }
        for (size_t i = 0; i < SNAG_VOICE_HANDOFFS; ++i) {
            if (!delayed) {
                assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
                    "type", "turn.done", "turn", "id", ids[i], "role", "user",
                    "transcript", ids[i])) == 0);
            }
            assert(voice_deliver(v, native_delegation(ids[i], ids[i])) == 0);
        }
        if (delayed) {
            assert(voice_notice_count(&f, "voice_handoff") == 0u);
            for (size_t i = SNAG_VOICE_HANDOFFS; i-- > 0u;) {
                assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
                    "type", "turn.done", "turn", "id", ids[i], "role", "user",
                    "transcript", ids[i])) == 0);
                json_t *handoff = voice_last(f.notices);
                assert(!strcmp(snag_json_string(handoff, "call_id"), ids[i]));
                assert(!strcmp(snag_json_string(handoff, "transcript"), ids[i]));
            }
        }
        assert(voice_notice_count(&f, "voice_handoff") == SNAG_VOICE_HANDOFFS);
        /* A full coding map must not stop conversation or reassign old work. */
        const char *refused = ids[SNAG_VOICE_HANDOFFS];
        assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
            "type", "turn.done", "turn", "id", refused, "role", "user",
            "transcript", refused)) == 0);
        assert(voice_deliver(v, native_delegation(refused, refused)) == 0);
        assert(snag_voice_ready(v));
        assert(!strcmp(snag_json_string(voice_last(f.notices), "status"), "refused"));
        assert(!strcmp(snag_json_string(voice_last(f.sent), "delegation_item_id"), refused));
        assert(snag_voice_result(v, ids[0], "first result", error, sizeof(error)) == 0);
        assert(voice_deliver(v, native_delegation(refused, refused)) == 0);
        assert(voice_notice_count(&f, "voice_handoff") == SNAG_VOICE_HANDOFFS);
        const char *fresh = ids[SNAG_VOICE_HANDOFFS + 1u];
        assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
            "type", "turn.done", "turn", "id", fresh, "role", "user",
            "transcript", fresh)) == 0);
        assert(voice_deliver(v, native_delegation(fresh, fresh)) == 0);
        assert(voice_notice_count(&f, "voice_handoff") == SNAG_VOICE_HANDOFFS + 1u);
        /* A reused ID with different source authority is still an integrity error. */
        assert(voice_deliver(v, native_delegation(ids[0], fresh)) < 0);
        voice_end(v, &f);
    }
}

static void
test_native_voice_credential_snapshot(void)
{
    struct local_server server;
    struct snag_config config;
    struct snag_store store;
    struct snag_credential credential;
    struct snag_buf output = {.max = 65536u};
    char path[4096], error[256] = {0}, call[257] = {0};
    const char *tmp = getenv("TMPDIR");

    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-auth-XXXXXX",
        tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    snag_store_init(&store);
    assert(snag_store_open(&store, path, error, sizeof(error)) == 0);
    start_server(&server, MODEL_NATIVE_CALL, false, "/backend-api/codex");
    struct snag_provider_connection conn = transport_connection(&config,
        &credential, server.endpoint);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    assert(snag_secret_source_parse(&config.providers[0].api_key,
        "\"transport-secret\"", NULL, error, sizeof(error)) == 0);
    assert(snag_auth_read(store.root_fd, conn.provider, false, NULL,
        &credential, NULL, NULL, error, sizeof(error)) == 0);
    assert(credential.root_fd == store.root_fd);
    /* The voice owner keeps the resolved credential, not the mutable source.
     * No login file exists for a provider authenticated by a configured key. */
    snag_secret_source_free(&config.providers[0].api_key);
    json_t *session = json_pack("{s:s}", "model", "gpt-live-1-codex");
    int rc = snag_provider_voice_call(NULL, conn.provider, &credential,
        "v=0\r\n", session, NULL, NULL, &output, call, NULL, error, sizeof(error));
    bool valid = rc == 0 && !strcmp(call, "rtc_native") && output.len != 0u;
    bool unchanged = credential.root_fd == store.root_fd &&
        !strcmp(credential.value, "transport-secret");
    json_decref(session);
    snag_buf_free(&output);
    snag_credential_clear(&credential);
    snag_config_free(&config);
    snag_store_close(&store);
    stop_server(&server);
    if (!valid) fprintf(stderr, "native voice credential snapshot: %s\n", error);
    assert(valid && unchanged);
}

static void test_native_voice_transport(void)
{
    for (unsigned int direct=0;direct<2u;++direct)
        for (int mode = MODEL_NATIVE_TRANSCRIBE; mode <= MODEL_NATIVE_CALL_PARTIAL; ++mode) {
        struct local_server server;struct snag_config config;struct snag_credential credential;
        char error[256]={0},call[257]={0};struct snag_buf output={.max=65536u};
        start_server(&server,(enum model_fixture)mode,false,"/backend-api/codex");
        struct snag_provider_connection conn=transport_connection(&config,&credential,
            direct?SNAG_CHATGPT_BASE:server.endpoint);
        config.providers[0].auth=direct?SNAG_AUTH_CHATGPT:SNAG_AUTH_API_KEY;
        if (direct)strcpy(credential.account_id,"acct-test");
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE",server.endpoint,1)==0);
        int rc;
        if (mode==MODEL_NATIVE_TRANSCRIBE) {
            make_fixture_wav();
            struct snag_buf wav={.data=fixture_wav,.len=sizeof(fixture_wav),
                .max=sizeof(fixture_wav)};
            json_t *request=json_pack("{s:s}","model","gpt-4o-transcribe");
            rc=snag_provider_audio(SNAG_AUDIO_TRANSCRIBE,request,&wav,&config,
                conn.provider,&credential,
                NULL,NULL,&output,error,sizeof(error));json_decref(request);
            assert(!rc && output.len && !memcmp(output.data,"{\"text\":",8u));
        } else {
            json_t *session=json_pack("{s:s}","model","gpt-live-1-codex");
            struct snag_provider_failure failure;
            rc=snag_provider_voice_call(&config,conn.provider,&credential,
                "v=0\r\n",session,NULL,NULL,
                &output, call, &failure, error, sizeof(error));json_decref(session);
            if (mode==MODEL_NATIVE_CALL)assert(!rc && !strcmp(call,"rtc_native") && output.len);
            else assert(rc<0 && !call[0] && error[0]);
            if (mode >= MODEL_NATIVE_CALL_DENIED) {
                bool invalid = mode == MODEL_NATIVE_CALL_MALFORMED ||
                    mode == MODEL_NATIVE_CALL_INVALID || mode == MODEL_NATIVE_CALL_PARTIAL;
                bool retry = mode == MODEL_NATIVE_CALL_TEMPORARY || mode == MODEL_NATIVE_CALL_EMPTY;
                long status = invalid ? 0 : mode == MODEL_NATIVE_CALL_DENIED ? 403 :
                    mode == MODEL_NATIVE_CALL_AUTH_ERROR ? 401 :
                    mode == MODEL_NATIVE_CALL_EMPTY ? 429 : 503;
                assert(rc == (retry ? SNAG_PROVIDER_VOICE_RETRY : -1));
                assert(failure.http_status == status && !output.len);
                assert(failure.retry_after_ms == (status && status != 403 ? 2000u : 0u));
                assert(snag_provider_failure_is_policy(&failure) ==
                    (mode == MODEL_NATIVE_CALL_POLICY));
                assert(snag_provider_failure_is_capacity(&failure) ==
                    (mode == MODEL_NATIVE_CALL_CAPACITY));
                bool counts = mode == MODEL_NATIVE_CALL_POLICY ||
                    mode == MODEL_NATIVE_CALL_CAPACITY;
                assert(failure.context_limit_tokens == (counts ? 12345u : 0u));
                assert(failure.requested_input_tokens == (counts ? 12346u : 0u));
                if (invalid) assert(!failure.code[0] && !failure.type[0] && !failure.message[0]);
                if (mode == MODEL_NATIVE_CALL_TEMPORARY) {
                    assert(strstr(failure.message, "<redacted:secret>"));
                    assert(!strstr(failure.message, "transport-secret"));
                }
                if (status) {
                    struct voice_fixture f = {.sent = json_array(), .notices = json_array()};
                    struct snag_voice_io io = {
                        voice_send, voice_notice, voice_play, voice_interrupt};
                    struct snag_voice *voice = voice_fixture_new(&io, &f,
                        "fixture", "asr", "voice");
                    assert(voice);
                    json_t *native = snag_voice_native_session(voice);
                    assert(native);
                    json_decref(native);
                    int reported = snag_voice_failure(voice, &failure, error, sizeof(error));
                    assert(reported == (retry ? SNAG_VOICE_RETRY :
                        mode == MODEL_NATIVE_CALL_CAPACITY ? SNAG_VOICE_CAPACITY : -1));
                    assert(f.interrupts == 1u && !json_array_size(f.sent));
                    assert(json_array_size(f.notices) == 1u);
                    const json_t *notice = voice_last(f.notices);
                    assert(json_integer_value(json_object_get(notice, "http_status")) == status);
                    assert(json_integer_value(json_object_get(notice, "retry_after_ms")) ==
                        failure.retry_after_ms);
                    assert(!voice_notice_count(&f, "voice_handoff"));
                    snag_voice_free(voice);
                    json_decref(f.sent);
                    json_decref(f.notices);
                }
            }
        }
        snag_buf_free(&output);
        snag_config_free(&config);
        snag_credential_clear(&credential);stop_server(&server);
    }
    assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE")==0);
}

#if SNAJPAGENT_AUDIO_DEVICE
static void
test_native_media_loss_burst(void)
{
    struct snag_voice_rtc *media = NULL;
    char error[256] = {0};
    assert(snag_voice_rtc_open(&media, error, sizeof(error)) == 0);
    int code;
    OpusEncoder *encoder = opus_encoder_create(24000, 1, OPUS_APPLICATION_VOIP, &code);
    assert(encoder && code == OPUS_OK);
    int16_t input[480];
    int16_t output[2880];
    for (size_t i = 0; i < 480u; ++i) input[i] = i % 48u < 24u ? 8000 : -8000;
    unsigned char packet[1512] = {0x80, 111, 0, 100};
    int bytes = opus_encode(encoder, input, 480, packet + 12u, 1500);
    assert(bytes > 0);
    assert(snag_voice_rtc_output(media, output, 2880u) == 0);
    snag_voice_rtc_fixture_packet(media, packet, bytes + 12, false);
    assert(snag_voice_rtc_output(media, output, 2880u) == 480);
    packet[3] = 103;
    snag_voice_rtc_fixture_packet(media, packet, bytes + 12, false);
    assert(snag_voice_rtc_output(media, output, 2880u) == 0);
    snag_voice_rtc_fixture_packet(media, packet, bytes + 12, true);
    /* One expired reorder interval covers the whole known loss burst. */
    assert(snag_voice_rtc_output(media, output, 2880u) == 480);
    assert(snag_voice_rtc_output(media, output, 2880u) == 480);
    assert(snag_voice_rtc_output(media, output, 2880u) == 480);
    assert(snag_voice_rtc_output(media, output, 2880u) == 0);
    /* Late/duplicate packets stay retired; a fresh hole still gets its wait. */
    packet[3] = 101;
    snag_voice_rtc_fixture_packet(media, packet, bytes + 12, false);
    assert(snag_voice_rtc_output(media, output, 2880u) == 0);
    packet[3] = 105;
    snag_voice_rtc_fixture_packet(media, packet, bytes + 12, false);
    assert(snag_voice_rtc_output(media, output, 2880u) == 0);
    packet[3] = 104;
    snag_voice_rtc_fixture_packet(media, packet, bytes + 12, false);
    assert(snag_voice_rtc_output(media, output, 2880u) == 480);
    assert(snag_voice_rtc_output(media, output, 2880u) == 480);
    assert(snag_voice_rtc_output(media, output, 2880u) == 0);
    /* The same loss rule applies across sequence wrap after interruption. */
    snag_voice_rtc_flush(media);
    packet[2] = packet[3] = 255;
    snag_voice_rtc_fixture_packet(media, packet, bytes + 12, false);
    assert(snag_voice_rtc_output(media, output, 2880u) == 480);
    packet[2] = 0;
    packet[3] = 2;
    snag_voice_rtc_fixture_packet(media, packet, bytes + 12, true);
    assert(snag_voice_rtc_output(media, output, 2880u) == 480);
    assert(snag_voice_rtc_output(media, output, 2880u) == 480);
    assert(snag_voice_rtc_output(media, output, 2880u) == 480);
    assert(snag_voice_rtc_output(media, output, 2880u) == 0);
    opus_encoder_destroy(encoder);
    snag_voice_rtc_close(media);
}

static void native_echo(int id,const char *data,int length,void *opaque)
{
    (void)opaque;
    if (length<12 || length>2048 || (((const unsigned char *)data)[1]&127u)!=111u)return;
    unsigned char packet[2048];memcpy(packet,data,(size_t)length);
    packet[8]=packet[9]=packet[10]=0;packet[11]=88;
    (void)rtcSendMessage(id,(const char *)packet,length);
}
static void native_gathered(int id,rtcGatheringState state,void *opaque)
{(void)id;if (state==RTC_GATHERING_COMPLETE)atomic_store((atomic_bool *)opaque,true);}
static void test_native_media(void)
{
    struct snag_voice_rtc *media=NULL;char error[256]={0},answer[32768];
    struct snag_buf offer={.max=32768u};atomic_bool gathered;atomic_init(&gathered,false);
    assert(snag_voice_rtc_open(&media,error,sizeof(error))==0);
    uint64_t deadline=snag_monotonic_ms()+10000u;int rc=0;
    while (!rc && snag_monotonic_ms()<deadline) {
            rc=snag_voice_rtc_offer(media,&offer);snag_sleep_ms(5u);}
    assert(rc==1);
    rtcConfiguration configuration={.disableAutoNegotiation=true,.forceMediaTransport=true};
    int pc=rtcCreatePeerConnection(&configuration);assert(pc>=0);rtcSetUserPointer(pc,&gathered);
    assert(rtcSetGatheringStateChangeCallback(pc,native_gathered)==0);
    rtcTrackInit init={.direction=RTC_DIRECTION_SENDRECV,
        .codec=RTC_CODEC_OPUS,.payloadType=111,.ssrc=88,.mid="0"};
    int track=rtcAddTrackEx(pc,&init);assert(track>=0);rtcSetUserPointer(track,&gathered);
    assert(rtcSetMessageCallback(track,native_echo)==0);
    assert(rtcSetRemoteDescription(pc,(char *)offer.data,"offer")==0 &&
        rtcSetLocalDescription(pc,"answer")==0);
    while (!atomic_load(&gathered) && snag_monotonic_ms()<deadline)snag_sleep_ms(5u);
    assert(atomic_load(&gathered) && rtcGetLocalDescription(pc,answer,sizeof(answer))>0);
    assert(snag_voice_rtc_answer(media,answer)==0);
    while (!snag_voice_rtc_ready(media) && snag_monotonic_ms()<deadline)snag_sleep_ms(5u);
    assert(snag_voice_rtc_ready(media));
    int16_t input[480],output[2880];for (unsigned int i=0;i<480u;++i)input[i]=(i/24u)%2u?8000:-8000;
    unsigned int samples=0,peak=0;uint64_t next=snag_monotonic_ms();deadline=next+1200u;
    while (snag_monotonic_ms()<deadline) {
        if (snag_monotonic_ms()>=next) {
            assert(snag_voice_rtc_input(media,input,480u)==0);next+=20u;}
        int n=snag_voice_rtc_output(media,output,2880u);assert(n>=0);
        samples+=(unsigned int)n;
        for (int i = 0; i < n; ++i) {
            unsigned int v = output[i] < 0 ? -output[i] : output[i];
            if (v > peak) {
                peak = v;
            }
        }
        snag_sleep_ms(2u);
    }
    assert(samples>12000u && peak>1000u);
    assert(snag_voice_rtc_input(media,input,200u)==0 && snag_voice_rtc_input(media,NULL,0u)==0);
    snag_voice_rtc_flush(media);snag_voice_rtc_close(media);
    rtcSetMessageCallback(track,NULL);rtcDeleteTrack(track);
    rtcDeletePeerConnection(pc);snag_buf_free(&offer);
}
#endif
static void voice_commit(struct snag_voice *voice,const char *id,const char *transcript)
{
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.committed","item_id",id))==0);
    if(transcript)assert(voice_deliver(voice,json_pack("{s:s,s:s,s:s,s:i}","type",
        "conversation.item.input_audio_transcription.completed","item_id",id,"transcript",transcript,"content_index",0))==0);
}
static void voice_created(struct snag_voice *voice,struct voice_fixture *f,const char *id)
{
    json_t *request=voice_last(f->sent);assert(!strcmp(snag_json_string(request,"type"),"response.create"));
    json_t *meta=json_object_get(json_object_get(request,"response"),"metadata");
    assert(voice_deliver(voice,json_pack("{s:s,s:{s:s,s:O}}","type","response.created","response","id",id,"metadata",meta))==0);
}
static json_t *voice_call_response(const char *id,const char *status)
{
    return json_pack("{s:s,s:{s:s,s:s,s:[{s:s,s:s,s:s,s:s,s:s}]}}","type","response.done","response",
        "id",id,"status",status,"output","id","function-1","type","function_call","name","ask_agent",
        "call_id","call-1","arguments","{\"request\":\"voice paraphrase\"}");
}
static size_t voice_notice_count(const struct voice_fixture *f,const char *type)
{
    size_t count=0;
    for(size_t i=0;i<json_array_size(f->notices);++i)
        if(!strcmp(snag_json_string(json_array_get(f->notices,i),"type"),type))++count;
    return count;
}
static void
test_voice_concurrent_results(void)
{
    struct voice_fixture f;
    struct snag_voice *voice = voice_start(&f);
    char error[256];

    voice_commit(voice, "first", "first request");
    assert(snag_voice_respond(voice, true, error, sizeof(error)) == 0);
    voice_created(voice, &f, "response-1");
    assert(voice_deliver(voice, voice_call_response("response-1", "completed")) == 0);
    voice_commit(voice, "second", "second request");
    assert(snag_voice_respond(voice, true, error, sizeof(error)) == 0);
    voice_created(voice, &f, "response-2");
    json_t *response = voice_call_response("response-2", "completed");
    json_t *item = json_array_get(json_object_get(json_object_get(response, "response"),
        "output"), 0u);
    assert(json_object_set_new(item, "call_id", json_string("call-2")) == 0);
    assert(voice_deliver(voice, response) == 0);
    assert(voice_notice_count(&f, "voice_handoff") == 2u);
    assert(snag_voice_progress(voice, "call-2", "request accepted", error, sizeof(error)) == 0);
    item = json_object_get(voice_last(f.sent), "item");
    assert(!strcmp(snag_json_string(item, "type"), "message"));
    assert(snag_voice_output(voice, "Model output:\nThe build finished.",
        error, sizeof(error)) == 0);
    item = json_object_get(voice_last(f.sent), "item");
    assert(!strcmp(snag_json_string(item, "type"), "message"));
    assert(!json_object_get(item, "call_id"));
    assert(snag_voice_result(voice, "call-2", "second result", error, sizeof(error)) == 0);
    item = json_object_get(voice_last(f.sent), "item");
    assert(!strcmp(snag_json_string(item, "call_id"), "call-2"));
    const char *second_id = snag_json_string(item, "id");
    assert(second_id);
    assert(snag_voice_result(voice, "call-1", "first result", error, sizeof(error)) == 0);
    item = json_object_get(voice_last(f.sent), "item");
    assert(!strcmp(snag_json_string(item, "call_id"), "call-1"));
    assert(strcmp(snag_json_string(item, "id"), second_id));
    assert(snag_voice_respond(voice, true, error, sizeof(error)) == 0);
    response = json_object_get(voice_last(f.sent), "response");
    assert(!strcmp(snag_json_string(response, "tool_choice"), "none"));
    assert(!strcmp(snag_json_string(json_object_get(response, "metadata"),
        "voice_input_item"), ""));
    voice_end(voice, &f);
}

static void test_voice_protocol(void)
{
    /* An older connection may still own all logical helper slots. */
    struct voice_fixture busy;
    struct snag_voice *waiting = voice_start(&busy);
    busy.busy = true;
    char busy_error[256];
    voice_commit(waiting, "busy-input", "new request");
    assert(snag_voice_respond(waiting, true, busy_error, sizeof(busy_error)) == 0);
    voice_created(waiting, &busy, "busy-response");
    assert(voice_deliver(waiting, voice_call_response("busy-response", "completed")) == 0);
    assert(snag_voice_ready(waiting) && !voice_notice_count(&busy, "voice_handoff"));
    json_t *busy_item = json_object_get(voice_last(busy.sent), "item");
    assert(strstr(snag_json_string(busy_item, "output"), "not submitted"));
    voice_end(waiting, &busy);

    struct voice_fixture f;char error[256];struct snag_voice *voice=voice_start(&f);
    voice_commit(voice,"input-1",NULL);voice_commit(voice,"input-2","later words");
    size_t sent=json_array_size(f.sent);
    /* Audio can be answered before independent ASR completes. The later
     * transcript must not hold up speech or become input-1's coding request. */
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0 && json_array_size(f.sent)==sent+1u);
    json_t *request=json_object_get(voice_last(f.sent),"response");
    assert(!strcmp(snag_json_string(json_object_get(request,"metadata"),"voice_input_item"),"input-1"));
    json_t *input=json_object_get(request,"input");
    assert(json_array_size(input)==1u && !strcmp(snag_json_string(json_array_get(input,0u),"id"),"input-1"));
    voice_created(voice,&f,"response-1");
    int16_t continuing_pcm[480]={0};
    assert(snag_voice_input(voice,continuing_pcm,480u,error,sizeof(error))==0);
    assert(!strcmp(snag_json_string(voice_last(f.sent),"type"),"input_audio_buffer.append"));
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","response.function_call_arguments.done","response_id","response-1"))==0);
    assert(!voice_notice_count(&f,"voice_handoff"));
    assert(voice_deliver(voice,voice_call_response("response-1","completed"))==0);
    assert(!voice_notice_count(&f,"voice_handoff"));
    assert(voice_deliver(voice,voice_call_response("response-1","completed"))==0);
    assert(!voice_notice_count(&f,"voice_handoff"));
    assert(voice_deliver(voice,json_pack("{s:s,s:s,s:s,s:i}","type","conversation.item.input_audio_transcription.completed",
        "item_id","input-1","transcript","original words","content_index",0))==0);
    assert(voice_notice_count(&f,"voice_handoff")==1u);
    json_t *handoff=voice_last(f.notices);
    assert(!strcmp(snag_json_string(handoff,"transcript"),"original words"));
    assert(!strcmp(snag_json_string(handoff,"request"),"voice paraphrase"));
    assert(voice_deliver(voice,voice_call_response("response-1","completed"))==0 && voice_notice_count(&f,"voice_handoff")==1u);
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0);
    request=json_object_get(voice_last(f.sent),"response");
    assert(!strcmp(snag_json_string(request,"tool_choice"),"auto"));
    assert(!strcmp(snag_json_string(json_object_get(request,"metadata"),"voice_input_item"),"input-2"));
    voice_created(voice,&f,"response-2");
    assert(snag_voice_result(voice,"call-1","coding still has one owner",error,sizeof(error))==0);
    assert(!strcmp(snag_json_string(json_object_get(voice_last(f.sent),"item"),"call_id"),"call-1"));
    assert(voice_deliver(voice,json_pack("{s:s,s:{s:s,s:s,s:[]}}","type","response.done","response",
        "id","response-2","status","completed","output"))==0);
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0);
    assert(!strcmp(snag_json_string(json_object_get(voice_last(f.sent),"response"),"tool_choice"),"none"));
    voice_end(voice,&f);

    for(unsigned int mode=0;mode<3u;++mode) {
        voice=voice_start(&f);voice_commit(voice,"input-1","do the work");
        assert(snag_voice_respond(voice,true,error,sizeof(error))==0);voice_created(voice,&f,"response-1");
        if(mode) {
            unsigned char pcm[48];
            for(unsigned int i=0;i<24u;++i) {uint16_t v=(uint16_t)((int)i-12);pcm[2u*i]=(unsigned char)v;pcm[2u*i+1u]=(unsigned char)(v>>8u);}
            struct snag_buf encoded;snag_buf_init(&encoded,128u);
            assert(snag_base64_append(&encoded,pcm,sizeof(pcm))==0 && snag_buf_terminate(&encoded)==0);
            json_t *audio=json_pack("{s:s,s:s,s:s,s:i,s:s}","type","response.output_audio.delta","response_id","response-1",
                "item_id","audio-1","content_index",0,"delta",(char *)encoded.data);
            assert(voice_deliver(voice,json_incref(audio))==0 && f.frames==24u);
            f.played=100u;
            assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_started","item_id","barge-in"))==0);
            assert(!strcmp(snag_json_string(voice_last(f.sent),"type"),"conversation.item.truncate"));
            assert(json_integer_value(json_object_get(voice_last(f.sent),"audio_end_ms"))==1);
            assert(voice_deliver(voice,audio)==0 && f.frames==24u);snag_buf_free(&encoded);
        }
        assert(voice_deliver(voice,voice_call_response("response-1",mode==2u?"completed":"cancelled"))==0);
        assert(!voice_notice_count(&f,"voice_handoff"));
        if(mode)assert(voice_notice_count(&f,"voice_interrupted")==1u);
        voice_end(voice,&f);
    }
    voice=voice_start(&f);voice_commit(voice,"input-1","earlier utterance");
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_started","item_id","input-2"))==0);
    sent=json_array_size(f.sent);
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0 && json_array_size(f.sent)==sent);
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_stopped","item_id","input-2"))==0);
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0 && json_array_size(f.sent)==sent+1u);
    voice_end(voice,&f);
    voice=voice_start(&f);int16_t pcm[480]={0};
    assert(snag_voice_mute(voice,true,error,sizeof(error))==0);
    sent=json_array_size(f.sent);
    assert(snag_voice_input(voice,pcm,480u,error,sizeof(error))==0 && json_array_size(f.sent)==sent);
    assert(snag_voice_mute(voice,false,error,sizeof(error))==0 && snag_voice_input(voice,pcm,480u,error,sizeof(error))==0);
    assert(json_array_size(f.sent)==sent+1u);
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","response.output_audio.delta","response_id","unknown"))<0);
    assert(!snag_voice_ready(voice));voice_end(voice,&f);
    voice=voice_start(&f);
    for(unsigned int i=0;i<8u;++i) {char id[32];snprintf(id,sizeof(id),"input-%u",i);voice_commit(voice,id,NULL);}
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.committed","item_id","input-overflow"))<0);
    voice_end(voice,&f);
}

static void test_voice_async_asr(void)
{
    struct voice_fixture f;char error[256];
    /* Failed ASR can arrive on either side of response.done. Neither ordering
     * submits work; the pending remote call gets one explicit failure result. */
    for(unsigned int early=0;early<2u;++early) {
        struct snag_voice *voice=voice_start(&f);
        voice_commit(voice,"input-1",NULL);
        assert(snag_voice_respond(voice,true,error,sizeof(error))==0);
        voice_created(voice,&f,"response-1");
        json_t *failed=json_pack("{s:s,s:s,s:i}","type",
            "conversation.item.input_audio_transcription.failed","item_id","input-1","content_index",0);
        if(early)assert(voice_deliver(voice,json_incref(failed))==0);
        assert(voice_deliver(voice,voice_call_response("response-1","completed"))==0);
        if(!early)assert(voice_deliver(voice,json_incref(failed))==0);
        json_decref(failed);
        assert(!voice_notice_count(&f,"voice_handoff") && voice_notice_count(&f,"voice_asr_failed")==1u);
        json_t *item=json_object_get(voice_last(f.sent),"item");
        assert(!strcmp(snag_json_string(item,"call_id"),"call-1"));
        assert(strstr(snag_json_string(item,"output"),"no coding work was submitted"));
        size_t sent=json_array_size(f.sent);
        assert(voice_deliver(voice,voice_call_response("response-1","completed"))==0);
        assert(json_array_size(f.sent)==sent);
        voice_end(voice,&f);
    }
    struct snag_voice *voice=voice_start(&f);
    voice_commit(voice,"input-1",NULL);
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0);
    voice_created(voice,&f,"response-1");
    /* Output audio is consumed before the separate transcript arrives; capture
     * can append on both sides of that output event without ending the input. */
    int16_t pcm[480]={0};unsigned char raw[48];
    for(unsigned int i=0;i<24u;++i) {uint16_t v=(uint16_t)((int)i-12);raw[i*2u]=(unsigned char)v;raw[i*2u+1u]=(unsigned char)(v>>8u);}
    struct snag_buf encoded;snag_buf_init(&encoded,128u);
    assert(snag_base64_append(&encoded,raw,sizeof(raw))==0 && snag_buf_terminate(&encoded)==0);
    assert(snag_voice_input(voice,pcm,480u,error,sizeof(error))==0);
    assert(voice_deliver(voice,json_pack("{s:s,s:s,s:s,s:i,s:s}","type","response.output_audio.delta",
        "response_id","response-1","item_id","audio-1","content_index",0,"delta",(char *)encoded.data))==0);
    assert(f.frames==24u && !voice_notice_count(&f,"voice_transcript"));
    assert(snag_voice_input(voice,pcm,480u,error,sizeof(error))==0);
    snag_buf_free(&encoded);
    json_t *audio_end=json_pack("{s:s,s:s,s:s,s:i}","type","response.output_audio.done",
        "response_id","response-1","item_id","audio-1","content_index",0);
    assert(voice_deliver(voice,json_incref(audio_end))==0 && f.ends==1u);
    assert(voice_deliver(voice,audio_end)==0 && f.ends==1u);
    assert(voice_deliver(voice,json_pack("{s:s,s:{s:s,s:s,s:[]}}","type","response.done","response",
        "id","response-1","status","completed","output"))==0);
    assert(f.ends==1u);
    /* Finished responses retain only unresolved input slots; late ASR releases
     * them. More than eight sequential late captions must not exhaust slots. */
    for(unsigned int i=0;i<12u;++i) {
        char input[32],response[32];snprintf(input,sizeof(input),"input-%u",i+2u);
        snprintf(response,sizeof(response),"response-%u",i+2u);
        voice_commit(voice,input,NULL);
        assert(snag_voice_respond(voice,true,error,sizeof(error))==0);
        voice_created(voice,&f,response);
        assert(voice_deliver(voice,json_pack("{s:s,s:{s:s,s:s,s:[]}}","type","response.done","response",
            "id",response,"status","completed","output"))==0);
        assert(voice_deliver(voice,json_pack("{s:s,s:s,s:s,s:i}","type",
            "conversation.item.input_audio_transcription.completed","item_id",input,"transcript","late words","content_index",0))==0);
    }
    assert(voice_deliver(voice,json_pack("{s:s,s:s,s:s,s:i}","type",
        "conversation.item.input_audio_transcription.completed","item_id","input-1","transcript","oldest words","content_index",0))==0);
    assert(voice_notice_count(&f,"voice_transcript")==13u && !voice_notice_count(&f,"voice_handoff"));
    voice_end(voice,&f);
}

static void test_voice_muted_input(void)
{
    struct voice_fixture f;char error[256];struct snag_voice *voice=voice_start(&f);
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_started","item_id","muted-input"))==0);
    assert(snag_voice_mute(voice,true,error,sizeof(error))==0);
    assert(voice_notice_count(&f,"voice_asr_failed")==1u);
    assert(!strcmp(snag_json_string(voice_last(f.notices),"reason"),"muted_incomplete"));
    assert(snag_voice_mute(voice,false,error,sizeof(error))==0);
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_stopped","item_id","muted-input"))==0);
    voice_commit(voice,"muted-input","truncated request must not execute");
    size_t sent=json_array_size(f.sent);
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0 && json_array_size(f.sent)==sent);
    assert(!voice_notice_count(&f,"voice_handoff") && !voice_notice_count(&f,"voice_transcript"));
    for(unsigned int i=0;i<12u;++i) {
        char item[32];snprintf(item,sizeof(item),"cleared-%u",i);
        assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_started","item_id",item))==0);
        assert(snag_voice_mute(voice,true,error,sizeof(error))==0);
        assert(voice_deliver(voice,json_pack("{s:s}","type","input_audio_buffer.cleared"))==0);
        assert(snag_voice_mute(voice,false,error,sizeof(error))==0);
    }
    sent=json_array_size(f.sent);
    voice_commit(voice,"fresh-input","new words");
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0 && json_array_size(f.sent)==sent+1u);
    assert(voice_notice_count(&f,"voice_transcript")==1u);
    voice_end(voice,&f);
}

static void
test_voice_observe(void)
{
    char error[256];
    for (unsigned int native = 0u; native < 2u; ++native) {
        struct voice_fixture f;
        struct snag_voice *voice;
        if (native) {
            memset(&f, 0, sizeof(f));
            f.sent = json_array();
            f.notices = json_array();
            assert(f.sent && f.notices);
            const struct snag_voice_io io = {voice_send, voice_notice, voice_play, voice_interrupt};
            voice = voice_fixture_new(&io, &f, "gpt-live-1-codex", "gpt-4o-transcribe", "cove");
            assert(voice);
            json_t *session = snag_voice_native_session(voice);
            assert(session);
            json_decref(session);
            assert(snag_voice_begin(voice, error, sizeof(error)) == 0);
        } else voice = voice_start(&f);
        size_t sent = json_array_size(f.sent);
        for (unsigned int i = 0u; i < 2u; ++i) {
            json_t *packet = json_pack("{s:s,s:i,s:s}", "kind", "session_observation",
                "seq", (int)i + 1, "text", i ? "second progress" : "first progress");
            assert(packet && snag_voice_observe(voice, packet, error, sizeof(error)) == 0);
            json_decref(packet);
            const json_t *event = voice_last(f.sent);
            assert(!strcmp(snag_json_string(event, "type"), native ?
                "session.context.append" : "conversation.item.create"));
            if (native) assert(!strcmp(snag_json_string(event, "channel"), "commentary"));
        }
        assert(json_array_size(f.sent) == sent + 2u && !voice_notice_count(&f, "voice_handoff"));
        assert(snag_voice_respond(voice, true, error, sizeof(error)) == 0);
        assert(json_array_size(f.sent) == sent + 2u);
        if (!native) {
            voice_commit(voice, "question", "what happened?");
            assert(snag_voice_respond(voice, true, error, sizeof(error)) == 0);
            const json_t *input = json_object_get(json_object_get(voice_last(f.sent), "response"),
                "input");
            assert(json_array_size(input) == 3u);
            assert(!strcmp(snag_json_string(json_array_get(input, 0u), "id"),
                "snag-observation-1"));
            assert(!strcmp(snag_json_string(json_array_get(input, 1u), "id"),
                "snag-observation-2"));
        }
        voice_end(voice, &f);
    }
}

static void test_voice_context(void)
{
    struct voice_fixture f;char error[256];struct snag_voice *voice=voice_start(&f);
    size_t sent=json_array_size(f.sent);json_t *context=json_pack("{s:s,s:s}","kind","session_context","status","old state");
    assert(context && snag_voice_context(voice,context,error,sizeof(error))==0);
    assert(json_object_set_new(context,"status",json_string("completed earlier"))==0);
    assert(snag_voice_context(voice,context,error,sizeof(error))==0);json_decref(context);
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0 && json_array_size(f.sent)==sent);
    assert(!voice_notice_count(&f,"voice_handoff"));
    voice_commit(voice,"input-1","what happened earlier?");
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0);
    const json_t *response=json_object_get(voice_last(f.sent),"response"),*input=json_object_get(response,"input");
    assert(json_array_size(input)==2u);
    const json_t *item=json_array_get(input,0u);
    const char *text=snag_json_string(json_array_get(json_object_get(item,"content"),0u),"text");
    assert(!strcmp(snag_json_string(item,"type"),"message") && !snag_json_string(item,"call_id"));
    assert(text && strstr(text,"completed earlier") && strstr(text,"not a new request") && !strstr(text,"old state"));
    assert(!strcmp(snag_json_string(json_array_get(input,1u),"id"),"input-1"));
    voice_end(voice,&f);
}

static void test_voice_captions(void)
{
    struct voice_fixture f;char error[256];struct snag_voice *voice=voice_start(&f);
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_started","item_id","input-1"))==0);
    assert(voice_deliver(voice,json_pack("{s:s,s:s,s:s,s:i}","type","conversation.item.input_audio_transcription.delta",
        "item_id","input-1","delta","still speaking","content_index",0))==0);
    assert(voice_notice_count(&f,"voice_caption")==1u && !voice_notice_count(&f,"voice_transcript"));
    assert(!voice_notice_count(&f,"voice_handoff"));
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_stopped","item_id","input-1"))==0);
    voice_commit(voice,"input-1","final words");
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0);voice_created(voice,&f,"response-1");
    json_t *caption=json_pack("{s:s,s:s,s:s,s:s,s:i}","type","response.output_audio_transcript.delta",
        "response_id","response-1","item_id","audio-1","delta","answer so far","content_index",0);
    assert(voice_deliver(voice,json_incref(caption))==0 && voice_notice_count(&f,"voice_caption")==2u);
    assert(!strcmp(snag_json_string(voice_last(f.notices),"speaker"),"assistant"));
    assert(!voice_notice_count(&f,"voice_interrupted"));
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_started","item_id","input-2"))==0);
    assert(voice_notice_count(&f,"voice_interrupted")==1u);
    assert(!snag_json_string(voice_last(f.notices),"item_id"));
    assert(!strcmp(snag_json_string(voice_last(f.sent),"type"),"response.cancel"));
    assert(voice_deliver(voice,caption)==0 && voice_notice_count(&f,"voice_caption")==2u);
    assert(!voice_notice_count(&f,"voice_handoff"));
    voice_end(voice,&f);
}

static void
test_context_preview_retains_catalog(void)
{
    struct app_state app = {0};
    struct snag_config config;
    struct snag_model_capacity capacity;
    struct snag_context_choice choice = {SNAG_CONTEXT_MODE_DEFAULT, 0u};
    char error[256] = {0};

    snag_config_init(&config);
    snag_store_init(&app.store);
    app.config = &config;
    const struct snag_provider_config *provider = &config.providers[0];
    json_t *limits = json_pack("{s:n,s:n,s:n,s:n,s:n,s:i,s:n}",
        "auto_compact_input_tokens", "context_window_tokens", "effective_context_window_percent",
        "input_context_window_tokens", "max_context_window_tokens", "max_input_tokens", 258400,
        "max_output_tokens");
    assert(limits);
    app.model_cache.providers = json_pack("[{s:s,s:s,s:s,s:[{s:s,s:o,s:s,s:i}]}]",
        "name", provider->name, "base_url", provider->base_url,
        "protocol", snag_provider_catalog_protocol(provider), "models",
        "id", "catalog-test", "limits", limits, "count_capability", "unknown",
        "observed_hard_input_tokens", 0);
    assert(app.model_cache.providers);
    json_t *original = app.model_cache.providers;

    /* A preview must work without access to the on-disk catalog at all. */
    for (unsigned int i = 0u; i < 3u; ++i) {
        assert(snag_app_context_preview(&app, provider, "catalog-test", &choice,
            &capacity, error, sizeof(error)) == 0);
        assert(capacity.hard_input_known && capacity.hard_input_tokens == 258400u);
        assert(app.model_cache.providers == original);
    }
    snag_model_cache_free(&app.model_cache);
    snag_config_free(&config);
}

#if defined(__linux__) && !defined(_WIN32)
static pid_t native_ui_owner;

static void
native_ui_failed(void)
{
    /* This PID came directly from this fixture's owner fork. A stopped owner
     * cannot receive its alarm; terminate/reap it before an assertion exits. */
    assert(native_ui_owner > 0);
    (void)kill(native_ui_owner, SIGKILL);
    int status;
    pid_t done;
    do { done = waitpid(native_ui_owner, &status, 0); } while (done < 0 && errno == EINTR);
    assert(done == native_ui_owner);
    native_ui_owner = 0;
}

static void
native_ui_frame(int fd, enum snag_session_message type, const void *data, size_t length)
{
    struct snag_session_packet packet = {0};
    if (type == SNAG_SESSION_COMMIT) {
        struct snag_terminal_profile profile = {.term = "xterm"};
        assert(length == 4u && snag_session_commit_set(&packet, data, &profile) == 0);
    } else assert(snag_session_packet_set(&packet, type, data, length) == 0);
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    int rc;
    do {
        rc = snag_session_packet_write(fd, &packet);
        bool okay = rc >= 0 && snag_monotonic_ms() < deadline;
        if (!okay) native_ui_failed();
        assert(okay);
        if (!rc) (void)snag_sleep_ms(1u);
    } while (!rc);
}

static void
native_ui_expect(int fd, enum snag_session_message expected, struct snag_buf *output)
{
    struct snag_session_packet packet = {0};
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    for (;;) {
        int rc = snag_session_packet_read(fd, &packet);
        bool okay = rc >= 0 && snag_monotonic_ms() < deadline;
        if (!okay) native_ui_failed();
        assert(okay);
        if (!rc) { (void)snag_sleep_ms(1u); continue; }
        enum snag_session_message type = snag_session_packet_type(&packet);
        if (type != SNAG_SESSION_OUTPUT) {
            assert(type == expected);
            return;
        }
        if (output) assert(snag_buf_append(output, packet.bytes + SNAG_SESSION_HEADER,
                                           snag_session_packet_length(&packet)) == 0);
        size_t length = snag_session_packet_length(&packet);
        unsigned char offset[2] = {(unsigned char)length, (unsigned char)(length >> 8u)};
        native_ui_frame(fd, SNAG_SESSION_OUTPUT_ACK, offset, sizeof(offset));
        if (type == expected) return;
        packet = (struct snag_session_packet){0};
    }
}

struct voice_renewal_records {
    uint64_t first;
    char queue[33];
    unsigned int queued, settled, replies, retired;
};

static int
voice_renewal_record(void *opaque, const struct snag_session *session, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t size)
{
    (void)session;
    (void)error;
    (void)size;
    struct voice_renewal_records *seen = opaque;
    if (seq < seen->first) return 0;
    if (!strcmp(type, "future_turn_queued")) {
        assert(!strcmp(snag_json_string(data, "queue_id"), seen->queue));
        ++seen->queued;
    }
    if (strcmp(type, "voice_event")) return 0;
    const json_t *event = json_object_get(data, "event");
    const char *call = snag_json_string(event, "call_id");
    if (!call || strcmp(call, "first")) return 0;
    assert(!strcmp(snag_json_string(data, "connection_id"), "0123456789abcdef0123456789abcdef"));
    const char *op = snag_json_string(event, "operation");
    assert(op && strcmp(op, "interface_failed"));
    if (!strcmp(op, "interface_request_settled")) {
        assert(!strcmp(snag_json_string(event, "status"), "completed"));
        assert(!json_object_get(event, "disposition"));
        ++seen->settled;
    } else if (!strcmp(op, "interface_reply")) ++seen->replies;
    else if (!strcmp(op, "interface_connection_retired")) ++seen->retired;
    return 0;
}

static void
test_voice_renewal(struct app_state *app, struct snag_config *config,
    const char *endpoint, int received, int release)
{
    char error[256];
    bool handled = false;
    json_t *empty = json_array();
    json_t *failure = json_pack("{s:s,s:{s:s,s:s}}", "type", "error", "error",
        "code", "server_error", "type", "server_error");
    assert(empty && failure && snag_app_voice_fixture(app, empty, false) == 0);
    json_decref(empty);
    assert(snag_app_voice_fixture_failure(app, NULL, NULL) == SNAG_VOICE_RETRY);
    assert(snag_app_voice_service(app) == 0 && app->voice);
    uint64_t stopping = snag_monotonic_ms();
    assert(snag_app_voice_command(app, "/voice off", &handled) == 0 && handled && !app->voice);
    assert(snag_monotonic_ms() - stopping < 1000u);

    /* A native HTTP hint schedules the existing backoff and expires with its
     * attempt. Replacement credentials still wait on the parent's auth lock. */
    {
        assert(setenv("SNAJPAGENT_TEST_AUTH_BASE", "http://127.0.0.1:1", 1) == 0);
        struct snag_provider_failure http = {.http_status = 503,
            .code = "server_error", .type = "server_error", .retry_after_ms = 2000u};
        empty = json_array();
        assert(empty && snag_app_voice_fixture(app, empty, false) == 0);
        json_decref(empty);
        assert(snag_app_voice_fixture_failure(app, NULL, &http) == SNAG_VOICE_RETRY);
        assert(snag_app_voice_service(app) == 0 && app->voice);
        json_t *state = snag_app_voice_fixture_state(app);
        assert(state);
        char old[33];
        strcpy(old, snag_json_string(state, "connection_id"));
        uint64_t now = snag_monotonic_ms();
        uint64_t scheduled = (uint64_t)json_integer_value(json_object_get(state, "reconnect_at"));
        assert(scheduled >= now + 1000u && scheduled <= now + 2000u);
        assert(json_integer_value(json_object_get(state, "retry_after_ms")) == 2000);
        json_decref(state);
        uint64_t deadline = now + 3000u;
        for (;;) {
            assert(snag_monotonic_ms() < deadline);
            assert(snag_app_voice_service(app) == 0 && app->voice);
            assert(!snag_app_voice_fixture_capture_ready(app));
            state = snag_app_voice_fixture_state(app);
            assert(state);
            bool replaced = strcmp(old, snag_json_string(state, "connection_id")) != 0;
            if (replaced) assert(json_integer_value(json_object_get(state, "retry_after_ms")) == 0);
            json_decref(state);
            if (replaced) break;
            (void)snag_sleep_ms(1u);
        }
        stopping = snag_monotonic_ms();
        assert(snag_app_voice_command(app, "/voice off", &handled) == 0 && handled && !app->voice);
        assert(snag_monotonic_ms() - stopping < 1000u);
        assert(unsetenv("SNAJPAGENT_TEST_AUTH_BASE") == 0);
    }

    struct snag_provider_config *provider = &config->providers[2];
    snag_config_provider_init(provider, "renewal");
    config->provider_count = 3u;
    provider->auth = SNAG_AUTH_API_KEY;
    strcpy(provider->base_url, endpoint);
    strcpy(provider->openrouter_referer, "https://github.com/snajpa/snajpagent");
    strcpy(provider->openrouter_title, "snajpagent");
    assert(snag_secret_source_parse(&provider->api_key, "\"transport-secret\"", NULL,
        error, sizeof(error)) == 0);
    assert(voice_fixture_transition(app, "model_selection_changed",
        json_pack("{s:s,s:s,s:s,s:s,s:s,s:s}", "old_provider", app->session.default_provider,
            "old_model", "fixture", "old_effort", app->session.default_effort,
            "new_provider", "renewal", "new_model", "fixture",
            "new_effort", app->session.default_effort), false) == 0u);
    strcpy(config->audio.provider, "default");
    assert(setenv("SNAJPAGENT_TEST_AUTH_BASE", "http://127.0.0.1:1", 1) == 0);
    assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", endpoint, 1) == 0);
    struct voice_renewal_records seen = {.first = app->session.next_seq};
    json_t *request = json_pack("[{s:s,s:s,s:s,s:s,s:s,s:s}]", "type", "voice_handoff",
        "input_id", "first", "response_id", "first", "call_id", "first",
        "transcript", "retained-first-utterance", "request", "first");
    assert(request && snag_app_voice_fixture(app, request, false) == 0);
    json_decref(request);
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    struct pollfd ready = {.fd = received, .events = POLLIN};
    while (poll(&ready, 1u, 0) == 0) {
        assert(snag_monotonic_ms() < deadline);
        assert(snag_app_voice_service(app) == 0 && app->voice);
        snag_sleep_ms(1u);
    }
    char marker;
    assert(read(received, &marker, 1u) == 1 && marker == 'R');
    assert(close(received) == 0 && app->session.pending_queue_count == 1u);
    strcpy(seen.queue, app->session.pending_queue[0].queue_id);
    /* Leave a partly sent PCM frame, then fail while its helper request waits. */
    assert(snag_app_voice_fixture_mute(app) == 0);
    assert(snag_app_voice_fixture_failure(app, failure, NULL) == SNAG_VOICE_RETRY);
    json_decref(failure);
    assert(snag_app_voice_service(app) == 0 && app->voice);
    for (;;) {
        assert(snag_monotonic_ms() < deadline);
        assert(snag_app_voice_service(app) == 0 && app->voice);
        json_t *state = snag_app_voice_fixture_state(app);
        assert(state && json_is_true(json_object_get(state, "muted")));
        bool replaced = strcmp(snag_json_string(state, "connection_id"),
            "0123456789abcdef0123456789abcdef") != 0;
        if (replaced) {
            assert(json_is_true(json_object_get(state, "transport_empty")));
            assert(json_integer_value(json_object_get(state, "pending_handoffs")) == 1);
        }
        json_decref(state);
        if (replaced) break;
        snag_sleep_ms(1u);
    }
    /* Replacement auth waits on the parent's lock. The old helper still settles. */
    assert(write(release, "T", 1u) == 1 && close(release) == 0);
    for (;;) {
        assert(snag_monotonic_ms() < deadline);
        assert(snag_app_voice_service(app) == 0 && app->voice);
        json_t *state = snag_app_voice_fixture_state(app);
        assert(state && json_is_true(json_object_get(state, "transport_empty")));
        bool settled = json_integer_value(json_object_get(state, "pending_handoffs")) == 0;
        json_decref(state);
        if (settled) break;
        snag_sleep_ms(1u);
    }
    assert(snag_session_each_event(&app->session, voice_renewal_record, &seen,
        error, sizeof(error)) == 0);
    assert(seen.queued == 1u && seen.settled == 2u && seen.replies == 1u && seen.retired == 1u);
    assert(app->session.pending_queue_count == 1u && !app->session.active_turn);
    assert(app->session.usage_totals.responses == 0u);
    stopping = snag_monotonic_ms();
    assert(snag_app_voice_command(app, "/voice off", &handled) == 0 && handled && !app->voice);
    assert(snag_monotonic_ms() - stopping < 1000u);
    assert(app->session.pending_queue_count == 1u);
    assert(snag_session_commit(&app->session, "future_turn_cancelled",
        json_pack("{s:[s],s:s}", "queue_ids", seen.queue, "reason", "user"),
        NULL, error, sizeof(error)) == 0);
    assert(voice_fixture_transition(app, "model_selection_changed",
        json_pack("{s:s,s:s,s:s,s:s,s:s,s:s}", "old_provider", "renewal",
            "old_model", "fixture", "old_effort", app->session.default_effort,
            "new_provider", "default", "new_model", "fixture",
            "new_effort", app->session.default_effort), false) == 0u);
    config->audio.provider[0] = '\0';
    assert(unsetenv("SNAJPAGENT_TEST_AUTH_BASE") == 0);
    assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
}

static void
test_voice_network_renewal(struct app_state *app, struct snag_config *config)
{
    if (!websocket_supported()) {
        fprintf(stderr, "voice network renewal: skipped (libcurl lacks WebSocket support)\n");
        return;
    }
    int refused = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(address);
    assert(refused >= 0 && bind(refused, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(refused, (struct sockaddr *)&address, &length) == 0);
    struct snag_provider_config *provider = &config->providers[1];
    char previous[SNAG_CONFIG_URL_MAX], connection[33];
    strcpy(previous, provider->base_url);
    snprintf(provider->base_url, sizeof(provider->base_url), "http://127.0.0.1:%u/v1",
        ntohs(address.sin_port));
    strcpy(config->audio.provider, "native-key");
    bool handled = false;
    assert(snag_app_voice_command(app, "/voice on", &handled) == 0 && handled && app->voice);
    json_t *state = snag_app_voice_fixture_state(app);
    assert(state);
    strcpy(connection, snag_json_string(state, "connection_id"));
    json_decref(state);
    uint64_t deadline = snag_monotonic_ms() + 4000u;
    for (;;) {
        assert(snag_monotonic_ms() < deadline);
        assert(snag_app_voice_service(app) == 0 && app->voice);
        assert(!snag_app_voice_fixture_capture_ready(app));
        state = snag_app_voice_fixture_state(app);
        bool replaced = state && json_integer_value(json_object_get(state, "reconnects")) >= 2;
        if (replaced) assert(strcmp(connection, snag_json_string(state, "connection_id")));
        json_decref(state);
        if (replaced) break;
        (void)snag_sleep_ms(1u);
    }
    uint64_t stopping = snag_monotonic_ms();
    assert(snag_app_voice_command(app, "/voice off", &handled) == 0 && handled && !app->voice);
    assert(snag_monotonic_ms() - stopping < 1000u);
    assert(!app->session.active_turn && !app->session.pending_queue_count);
    assert(close(refused) == 0);
    strcpy(provider->base_url, previous);
    config->audio.provider[0] = '\0';
}

static void
test_native_ui(void)
{
    char *saved_term = snag_environment("TERM");
    assert(setenv("TERM", "xterm", 1) == 0);
    char path[4096], directory[4096], id[33], error[256];
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(path, sizeof(path), "%s/snajpagent-native-ui-XXXXXX", tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    /* Hold a loopback handshake open so the acknowledged context stays inspectable. */
    int credential_listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in credential_address = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t credential_size = sizeof(credential_address);
    char credential_endpoint[128];
    assert(credential_listener >= 0);
    assert(bind(credential_listener, (struct sockaddr *)&credential_address,
        sizeof(credential_address)) == 0 && listen(credential_listener, 1) == 0);
    assert(getsockname(credential_listener, (struct sockaddr *)&credential_address,
        &credential_size) == 0);
    assert(snprintf(credential_endpoint, sizeof(credential_endpoint), "http://127.0.0.1:%u",
        ntohs(credential_address.sin_port)) > 0);
    struct snag_store auth_store;
    struct snag_config auth_config;
    struct snag_auth_tokens tokens = {0};
    snag_store_init(&auth_store);
    snag_config_init(&auth_config);
    strcpy(auth_config.providers[0].name, "default");
    strcpy(auth_config.providers[0].base_url, SNAG_CHATGPT_BASE);
    auth_config.providers[0].auth = SNAG_AUTH_CHATGPT;
    snag_secret_source_free(&auth_config.providers[0].api_key);
    assert(snag_store_open(&auth_store, path, error, sizeof(error)) == 0);
    assert(snag_auth_key(&tokens, "native-old-access", error, sizeof(error)) == 0);
    strcpy(tokens.refresh_token, "native-old-refresh");
    strcpy(tokens.credential.account_id, "native-fixture-account");
    tokens.expires_at_ms = 1u;
    assert(snag_auth_save(auth_store.root_fd, &auth_config.providers[0], &tokens, NULL,
        NULL, NULL, error, sizeof(error)) == 0);
    snag_config_provider_init(&auth_config.providers[1], "native-key");
    auth_config.provider_count = 2u;
    strcpy(auth_config.providers[1].base_url, credential_endpoint);
    assert(snag_auth_key(&tokens, "native-stored-secret", error, sizeof(error)) == 0);
    assert(snag_auth_save(auth_store.root_fd, &auth_config.providers[1], &tokens, NULL,
        NULL, NULL, error, sizeof(error)) == 0);
    int auth_lock = openat(auth_store.root_fd, "auth/default.lock",
        O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    assert(auth_lock >= 0 && snag_lock_file(auth_lock, false) == 0);
    snag_auth_clear(&tokens);
    snag_config_free(&auth_config);
    snag_store_close(&auth_store);
    struct local_server renewal_server;
    int renewal_received[2], renewal_release[2];
    assert(pipe(renewal_received) == 0 && pipe(renewal_release) == 0);
    voice_request_ready_fd = renewal_received[1];
    voice_request_release_fd = renewal_release[0];
    start_server(&renewal_server, MODEL_VOICE_CLOSE, false, "/v1");
    assert(close(renewal_received[1]) == 0 && close(renewal_release[0]) == 0);
    voice_request_ready_fd = voice_request_release_fd = -1;
    int saved[3], report[2], proceed[2];
    assert(pipe(report) == 0 && pipe(proceed) == 0 && fflush(NULL) == 0);
    for (int fd = 0; fd < 3; ++fd) {
        saved[fd] = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        assert(saved[fd] >= 0);
    }
    int outer = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(outer >= 0 && grantpt(outer) == 0 && unlockpt(outer) == 0);
    int screen = open(ptsname(outer), O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(screen >= 0);
    for (int fd = 0; fd < 3; ++fd) assert(dup2(screen, fd) == fd);
    struct snag_session_process process;
    int frontend = snag_session_process_start(&process);
    if (!frontend) {
        (void)alarm(10u);
        assert(close(auth_lock) == 0);
        assert(close(credential_listener) == 0);
        for (int fd = 0; fd < 3; ++fd) assert(close(saved[fd]) == 0);
        assert(close(outer) == 0 && close(screen) == 0);
        assert(close(report[0]) == 0 && close(proceed[1]) == 0);
        struct app_state app = {0};
        struct snag_config config;
        snag_config_init(&config);
        strcpy(config.providers[0].name, "default");
        strcpy(config.providers[0].base_url, SNAG_CHATGPT_BASE);
        config.providers[0].auth = SNAG_AUTH_CHATGPT;
        snag_secret_source_free(&config.providers[0].api_key);
        snag_config_provider_init(&config.providers[1], "native-key");
        config.provider_count = 2u;
        strcpy(config.providers[1].base_url, credential_endpoint);
        assert(setenv("SNAJPAGENT_TEST_AUTH_BASE", "http://127.0.0.1:1", 1) == 0);
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", "http://127.0.0.1:1", 1) == 0);
        app.config = &config;
        struct snag_store *store = &app.store;
        struct snag_session *session = &app.session;
        struct snag_ui *ui = &app.ui;
        snag_store_init(store);
        snag_session_init(session);
        assert(snag_store_open(store, path, error, sizeof(error)) == 0);
        assert(snag_session_create(store, session, path, "default", "fixture", "medium",
                                    error, sizeof(error)) == 0);
        assert(snag_ui_init(ui) == 0 && snag_ui_session_start(ui, &process) == 0);
        assert(snag_ui_session_attachment(ui) == 1u);
        assert(process.master == -1 && process.peer == -1 && process.slave == -1);
        /* Startup output can exceed the PTY buffer before input is opened. */
        char startup[65537];
        memset(startup, 's', sizeof(startup) - 1u);
        startup[sizeof(startup) - 1u] = '\0';
        assert(snag_ui_text(ui, SNAG_UI_HOST, startup) == 0);
        assert(snag_ui_open(ui, error, sizeof(error)) == 0);
        assert(snag_ui_session_listen(ui, session) == 0);
        assert(snag_ui_simple_prompt(ui, false) == 0);
        assert(snag_ui_insert_draft(ui, "retained draft") == 0);
        assert(snag_ui_input(ui, "/status", 2u) < 0 && errno == ESTALE);
        assert(snag_ui_input(ui, "/verbose 2", 1u) == 0);
        assert(snag_ui_verbosity(ui) == 2u);
        enum snag_term_action admitted = SNAG_TERM_NONE;
        char *command = NULL;
        assert(snag_ui_poll(ui, 100, &admitted, &command) == 0 && !command);
        unsigned int admitted_count = 0u;
        while (admitted_count < 64u && snag_ui_input(ui, "/status", 1u) == 0)
            ++admitted_count;
        assert(admitted_count && admitted_count < 64u && errno == EAGAIN);
        for (unsigned int i = 0u; i < admitted_count; ++i) {
            assert(snag_ui_poll(ui, 0, &admitted, &command) == 1);
            assert(admitted == SNAG_TERM_SUBMIT && command && !strcmp(command, "/status"));
            free(command);
            command = NULL;
        }
        /* The frontend owns the credential lock. No provider or device can open. */
        bool auth_handled = false;
        uint64_t auth_start = snag_monotonic_ms();
        assert(snag_app_voice_command(&app, "/voice on", &auth_handled) == 0);
        assert(auth_handled && app.voice && snag_monotonic_ms() - auth_start < 1000u);
        assert(!snag_app_voice_fixture_capture_ready(&app));
        assert(snag_ui_input(ui, "/status", 1u) == 0);
        assert(snag_ui_poll(ui, 0, &admitted, &command) == 1 && command);
        assert(admitted == SNAG_TERM_SUBMIT && !strcmp(command, "/status"));
        free(command);
        command = NULL;
        assert(snag_app_voice_service(&app) == 0 && app.voice);
        uint64_t auth_stop = snag_monotonic_ms();
        assert(snag_app_voice_command(&app, "/voice off", &auth_handled) == 0);
        assert(auth_handled && !app.voice && snag_monotonic_ms() - auth_stop < 1000u);
        assert(!session->active_turn && !session->pending_queue_count);
        assert(snag_session_commit(session, "voice_event",
            json_pack("{s:s,s:s,s:s,s:{s:s,s:s,s:s,s:s}}",
                "connection_id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "provider", "native-key",
                "model", "fixture", "event", "type", "voice_transcript", "speaker", "user",
                "item_id", "credential-context", "text", "credential marker native-stored-secret"),
            NULL, error, sizeof(error)) == 0);
        strcpy(config.audio.provider, "native-key");
        assert(snag_app_voice_command(&app, "/voice on", &auth_handled) == 0 && app.voice);
        uint64_t credential_deadline = snag_monotonic_ms() + 3000u;
        while (!snag_app_voice_fixture_credential_ready(&app)) {
            assert(snag_monotonic_ms() < credential_deadline);
            (void)snag_sleep_ms(1u);
        }
        assert(!snag_app_voice_fixture_capture_ready(&app));
        assert(snag_app_voice_service(&app) == 0 && app.voice);
        json_t *credential_context = snag_app_voice_fixture_context(&app);
        assert(credential_context && !strcmp(snag_json_string(credential_context, "recent_asr"),
            "credential marker <redacted:secret>"));
        json_decref(credential_context);
        assert(!snag_app_voice_fixture_capture_ready(&app));
        auth_stop = snag_monotonic_ms();
        assert(snag_app_voice_command(&app, "/voice off", &auth_handled) == 0 && !app.voice);
        assert(snag_monotonic_ms() - auth_stop < 1000u);
        config.audio.provider[0] = '\0';
        /* Later device-free fixtures build their protection directly from configuration. */
        assert(snag_secret_source_parse(&config.providers[1].api_key, "\"native-stored-secret\"",
            NULL, error, sizeof(error)) == 0);
        assert(unsetenv("SNAJPAGENT_TEST_AUTH_BASE") == 0);
        assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
        const char *goal = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        char history_text[3073];
        for (size_t i = 0u; i < sizeof(history_text) - 1u; i += 2u) {
            memcpy(history_text + i, "λ", 2u);
        }
        history_text[sizeof(history_text) - 1u] = '\0';
        assert(snag_session_commit(session, "goal_started",
            json_pack("{s:s,s:s}", "goal_id", goal, "prompt", history_text),
            NULL, error, sizeof(error)) == 0);
        assert(snag_session_commit(session, "goal_cancelled", json_pack("{s:s}", "goal_id", goal),
            NULL, error, sizeof(error)) == 0);
        for (unsigned int cancelled = 1u; ; --cancelled) {
            json_t *ready = json_pack("[{s:s}]", "type", "voice_ready");
            assert(ready && snag_app_voice_fixture(&app, ready, false) == 0);
            json_decref(ready);
            assert(snag_app_voice_service(&app) == 0 && app.voice);
            assert(!snag_app_voice_fixture_capture_ready(&app));
            /* Repeated service cannot bypass an unconsumed initial packet. */
            assert(snag_app_voice_service(&app) == 0 && app.voice);
            assert(!snag_app_voice_fixture_capture_ready(&app));
            if (!cancelled) {
                /* Ongoing model changes must not move the initial history boundary. */
                assert(voice_fixture_model_changes(&app, false) == 0u);
                const char *types[] = {"voice_event", "goal_started", "goal_cancelled"};
                for (size_t i = 0u; i < 3u; ++i) {
                    bool complete = false;
                    for (size_t fragments = 0u; !complete && fragments < 8u; ++fragments) {
                        json_t *history = snag_app_voice_fixture_observation(&app);
                        assert(history && !strcmp(snag_json_string(history, "event_type"),
                            types[i]));
                        complete = json_is_true(json_object_get(history, "complete"));
                        if (i == 1u && !fragments) assert(!complete);
                        json_decref(history);
                        assert(!snag_app_voice_fixture_capture_ready(&app));
                        assert(snag_app_voice_service(&app) == 0 && app.voice);
                    }
                    assert(complete);
                }
                assert(!snag_app_voice_fixture_capture_ready(&app));
                json_t *context = snag_app_voice_fixture_context(&app);
                assert(context && json_is_true(json_object_get(context, "history_complete")));
                assert((uint64_t)json_integer_value(json_object_get(context,
                    "history_as_of_seq")) == session->next_seq - 1u);
                json_decref(context);
                assert(!snag_app_voice_fixture_observation(&app));
                assert(snag_app_voice_fixture_capture_ready(&app));
                for (size_t i = 0u; i < 2u; ++i) {
                    assert(snag_app_voice_service(&app) == 0);
                    json_t *history = snag_app_voice_fixture_observation(&app);
                    assert(history && !strcmp(snag_json_string(history, "event_type"),
                        "model_selection_changed"));
                    json_decref(history);
                }
            }
            snag_app_voice_close(&app);
            assert(!app.voice && !session->active_turn && !session->pending_queue_count);
            if (!cancelled) break;
        }
        test_voice_renewal(&app, &config, renewal_server.endpoint,
            renewal_received[0], renewal_release[1]);
        test_voice_network_renewal(&app, &config);
        assert(snag_ui_voice(ui, "[VOICE MIC ON] ") == 0);
        json_t *notices = json_pack("[{s:s,s:s,s:s,s:s},{s:s}]",
            "type", "voice_transcript", "speaker", "user", "item_id", "input-1",
            "text", "final words", "type", "voice_handoff");
        assert(notices && snag_app_voice_fixture(&app, notices, false) == 0);
        json_decref(notices);
        struct snag_response_item control = {.name = "ui_input",
            .arguments = json_pack("{s:s}", "text", "/status")};
        json_t *control_result = NULL;
        assert(control.arguments && snag_app_voice_ui_input(&app, &control,
            &control_result, error, sizeof(error)) == 0);
        assert(!strcmp(snag_json_string(control_result, "status"), "succeeded"));
        assert(strstr(snag_json_string(control_result, "model_text"),
            "does not establish command completion"));
        json_decref(control_result);
        control_result = NULL;
        assert(snag_ui_poll(ui, 0, &admitted, &command) == 1 && command);
        bool handled = false, prompt_ready = false;
        assert(snag_app_input_command(&app, command, false, &handled, &prompt_ready) == 0);
        assert(handled && !app.session.pending_queue_count && !app.session.active_turn);
        free(command);
        command = NULL;
        assert(snag_ui_input(ui, "/status", 1u) == 0);
        assert(snag_app_voice_fixture_checkpoint(&app) == 0);
        assert(write(report[1], session->id, sizeof(id)) == (ssize_t)sizeof(id));
        char release;
        assert(read(proceed[0], &release, 1u) == 1);
        assert(snag_ui_text(ui, SNAG_UI_HOST, "work after disconnect") == 0);
        assert(snag_ui_session_attachment(ui) == 0u);
        assert(snag_ui_input(ui, "/status", 1u) < 0 && errno == ESTALE);
        assert(snag_ui_poll(ui, 0, &admitted, &command) == 0 && !command);
        assert(snag_app_voice_ui_input(&app, &control, &control_result,
            error, sizeof(error)) == 0);
        assert(!strcmp(snag_json_string(control_result, "status"), "failed"));
        json_decref(control_result);
        json_decref(control.arguments);
        char capture = (char)('0' + snag_ui_voice(ui, "[VOICE MIC ON] "));
        assert(write(report[1], &capture, 1u) == 1);
        assert(capture == '1');
        assert(snag_ui_audio(ui, "[MIC ON] ", true) == 1);
        assert(snag_ui_audio(ui, "[playing] ", false) == 1);
        assert(snag_app_voice_fixture_checkpoint(&app) == 2);
        assert(snag_app_voice_service(&app) == 0 && !app.voice);
        unsigned int voice_counts[2] = {0};
        assert(snag_session_each_event(session, voice_close_record, voice_counts,
                                        error, sizeof(error)) == 0);
        unsigned int expected_stops = websocket_supported() ? 9u : 8u;
        assert(voice_counts[0] == 2u && voice_counts[1] == expected_stops &&
            !session->pending_queue_count);
        for (unsigned int playing = 0u; playing < 2u; ++playing) {
            assert(snag_app_audio_fixture(&app, playing != 0u) == 0);
            assert(snag_app_audio_fixture_checkpoint(&app) == 2);
            assert(snag_app_audio_service(&app) == 0 && !app.audio);
        }
        assert(write(report[1], "D", 1u) == 1);
        uint64_t generation, deadline = snag_monotonic_ms() + 5000u;
        while (!(generation = snag_ui_session_pending(ui))) {
            assert(snag_monotonic_ms() < deadline);
            (void)snag_sleep_ms(1u);
        }
        assert(snag_ui_session_attachment(ui) == 0u);
        assert(snag_ui_session_rebind(ui, generation) == 0);
        assert(snag_ui_session_attachment(ui) == generation);
        assert(!app.voice && !app.audio);
        assert(snag_app_audio_fixture(&app, false) == 0);
        assert(snag_app_audio_fixture_checkpoint(&app) == 0);
        assert(snag_ui_text(ui, SNAG_UI_HOST, "semantic catch-up") == 0);
        assert(write(report[1], "B", 1u) == 1);
        assert(read(proceed[0], &release, 1u) == 1);
        enum snag_term_action action = SNAG_TERM_NONE;
        char *text = NULL;
        assert(snag_ui_poll(ui, 0, &action, &text) == 0);
        assert(snag_ui_session_ready(ui, generation) == 0);
        deadline = snag_monotonic_ms() + 2000u;
        int rc;
        do {
            uint64_t resumed = snag_ui_session_pending(ui);
            if (resumed) {
                generation = resumed;
                assert(snag_ui_session_rebind(ui, generation) == 0);
                assert(snag_ui_session_ready(ui, generation) == 0);
            }
            rc = snag_ui_poll(ui, 20, &action, &text);
            assert(rc >= 0 && snag_monotonic_ms() < deadline);
            assert(snag_app_audio_service(&app) == 0);
        } while (!rc);
        assert(!app.audio && !app.voice);
        assert(action == SNAG_TERM_SUBMIT && text && !strcmp(text, "retained draftz"));
        free(text);
        assert(snag_ui_session_rebind(ui, generation - 1u) < 0 && errno == ESTALE);
        assert(snag_ui_text(ui, SNAG_UI_HOST, "owner-ok") == 0);
        unsigned char success = 0u;
        assert(snag_ui_session_control(ui, SNAG_SESSION_EXIT, &success, 1u) == 0);
        snag_ui_free(ui);
        snag_session_close(session);
        snag_store_close(store);
        snag_config_free(&config);
        assert(close(report[1]) == 0 && close(proceed[0]) == 0);
        _exit(0);
    }
    for (int fd = 0; fd < 3; ++fd) {
        assert(dup2(saved[fd], fd) == fd && close(saved[fd]) == 0);
    }
    assert(frontend == 1);
    native_ui_owner = (pid_t)process.child;
    assert(close(outer) == 0 && close(screen) == 0);
    assert(close(report[1]) == 0 && close(proceed[0]) == 0);
    /* Act as the initial frontend while startup output exceeds one frame. */
    for (;;) {
        struct pollfd ready[] = {{report[0], POLLIN, 0}, {process.peer, POLLIN, 0}};
        int rc = poll(ready, 2u, 5000);
        if (rc <= 0) native_ui_failed();
        assert(rc > 0);
        if (ready[0].revents) break;
        native_ui_expect(process.peer, SNAG_SESSION_OUTPUT, NULL);
    }
    ssize_t identified = read(report[0], id, sizeof(id));
    if (identified != (ssize_t)sizeof(id)) {
        int failed;
        assert(waitpid((pid_t)process.child, &failed, 0) == (pid_t)process.child);
    }
    assert(identified == (ssize_t)sizeof(id));
    snag_session_process_close(&process);
    assert(write(proceed[1], "d", 1u) == 1);
    char phase;
    assert(read(report[0], &phase, 1u) == 1);
    if (phase != '1') {
        fprintf(stderr, "detached voice activation returned %c, expected 1 (refused)\n", phase);
        native_ui_failed();
    }
    assert(phase == '1');
    assert(read(report[0], &phase, 1u) == 1 && phase == 'D');
    assert(snprintf(directory, sizeof(directory), "%s/sessions/%s", path, id) > 0);
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    assert(dir >= 0);
    int peer = snag_session_endpoint_connect(dir, directory);
    assert(peer >= 0 && close(dir) == 0);
    /* Catch-up may precede the commit ACK; retain the whole new attachment. */
    struct snag_buf output = {.max = 64u * 1024u};
    native_ui_frame(peer, SNAG_SESSION_RESERVE, NULL, 0u);
    native_ui_expect(peer, SNAG_SESSION_READY, &output);
    unsigned char geometry[4] = {31u, 0u, 97u, 0u};
    native_ui_frame(peer, SNAG_SESSION_COMMIT, geometry, sizeof(geometry));
    native_ui_expect(peer, SNAG_SESSION_READY, &output);
    assert(read(report[0], &phase, 1u) == 1 && phase == 'B');
    /* Ctrl-Z must suspend only the replaceable frontend. The following byte
     * was already admitted and must survive suspension and recommit. */
    native_ui_frame(peer, SNAG_SESSION_INPUT, "\032z", 2u);
    (void)snag_sleep_ms(40u);
    assert(write(proceed[1], "r", 1u) == 1);
    native_ui_expect(peer, SNAG_SESSION_SUSPEND, &output);
    geometry[0] = 33u;
    native_ui_frame(peer, SNAG_SESSION_COMMIT, geometry, sizeof(geometry));
    native_ui_expect(peer, SNAG_SESSION_READY, &output);
    native_ui_frame(peer, SNAG_SESSION_INPUT, "\r", 1u);
    native_ui_expect(peer, SNAG_SESSION_EXIT, &output);
    assert(snag_buf_terminate(&output) == 0);
    assert(strstr((const char *)output.data, "semantic catch-up"));
    assert(strstr((const char *)output.data, "owner-ok"));
    assert(!strstr((const char *)output.data, "work after disconnect"));
    snag_buf_free(&output);
    assert(close(peer) == 0 && close(report[0]) == 0 && close(proceed[1]) == 0);
    int status;
    assert(waitpid((pid_t)process.child, &status, 0) == (pid_t)process.child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    native_ui_owner = 0;
    assert(close(renewal_received[0]) == 0 && close(renewal_release[1]) == 0);
    stop_server(&renewal_server);
    assert(close(auth_lock) == 0);
    assert(close(credential_listener) == 0);
    if (saved_term) {
        assert(setenv("TERM", saved_term, 1) == 0);
        free(saved_term);
    } else {
        assert(unsetenv("TERM") == 0);
    }
}
#endif /* __linux__ && !_WIN32 */

int
main(void)
{
    test_native_voice_caption_mirrors();
    test_native_voice_transcript_events();
#if SNAJPAGENT_AUDIO_DEVICE
    test_native_media_loss_burst();
#endif
#if defined(__linux__) && !defined(_WIN32)
    test_native_ui();
#endif
    test_context_preview_retains_catalog();
    /* Every fixture is a forked copy of this process and is stopped with
     * SIGTERM: install the handler before the first fork so all of them
     * inherit it, whatever path starts them, and exit with status 0. */
    (void)signal(SIGTERM, fixture_stop);
    test_http_continue();
    test_audio_provider_selection();
    test_native_voice_protocol();
    test_native_voice_concurrency();
    test_native_voice_playback();
    test_native_voice_credential_snapshot();
    test_native_voice_transport();
#if SNAJPAGENT_AUDIO_DEVICE
    test_native_media();
#endif
    test_irc_steering_mode();
    test_plain_irc_pending_resume();
    test_irc_reply_restore_scope();
    test_irc_failed_intent_retains_pending();
#if SNAJPAGENT_AUDIO_DEVICE && defined(MA_NO_RUNTIME_LINKING) && defined(MA_ENABLE_ALSA)
    test_static_alsa_config();
#endif
    test_voice_protocol();
    test_voice_provider_errors();
    test_voice_concurrent_results();
    test_voice_async_asr();
    test_voice_captions();
    test_voice_observe();
    test_voice_context();
    test_voice_muted_input();
#if SNAJPAGENT_AUDIO_DEVICE
    assert(snag_audio_fixture_capture()==0);
#endif
    test_voice_close();
    test_voice_transcript_labels();
    test_voice_observation_cursor();
    for (enum model_fixture mode = MODEL_VOICE_NATIVE_SUMMARY;
            mode <= MODEL_VOICE_NATIVE_HANDOFF; ++mode) test_voice_native_capacity(mode);
    test_voice_concurrent_owner(MODEL_VOICE_QUEUE);
    test_voice_concurrent_owner(MODEL_VOICE_COMPACT);
    test_voice_concurrent_owner(MODEL_VOICE_COMPACT_RETRY);
    test_voice_concurrent_owner(MODEL_VOICE_COMPACT_TOOL);
    test_voice_concurrent_owner(MODEL_VOICE_COMPACT_ACTIVE);
    test_voice_close_settlement();
    test_voice_initial_backlog();
    test_voice_read_tools();
    test_voice_queue_inspection();
    test_voice_interface_read();
    test_voice_independent_request();
    test_public_item_growth();
    test_voice_session_controls();
    test_voice_owner_mute();
    test_voice_output_tool();
    test_voice_socket();
    test_audio_transport();
    test_provider_auth();
    test_media_count_fallback();
    test_native_compaction_probe();
    test_local_audio_admission();
    test_ui_output_order_and_failure();
    test_ui_bounded_history();
    test_output_cache_failure();
    test_read_only_dispatch();
    test_goal_tool_manipulates_unfinished_goals();
    test_history_and_goal_list_tools();
    test_local_provider_transport();
    test_session_identity_header();
    test_openrouter_search_transport();
    test_codex_path_selection();
    test_structured_create_failures();
    test_typeless_create_diagnostic();
    test_create_retries();
    test_policy_clarification_after_reasoning();
    test_count_capability_statuses();
    test_count_modes();
    puts("test_provider_transport: ok");
    return 0;
}
