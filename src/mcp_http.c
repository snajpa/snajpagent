/* SPDX-License-Identifier: GPL-2.0-only */
#include "mcp_internal.h"
#include "http.h"
#include "snajpagent.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *
snag_mcp_string(const json_t *object, const char *key)
{
    const char *s = json_string_value(json_object_get(object, key));
    return s ? s : "";
}

char *
snag_mcp_url_part(const char *url, CURLUPart part)
{
    CURLU *parsed = curl_url();
    char *value = NULL;
    char *copy = NULL;
    if (parsed && curl_url_set(parsed, CURLUPART_URL, url, 0u) == CURLUE_OK &&
        curl_url_get(parsed, part, &value, 0u) == CURLUE_OK)
        copy = strdup(value);
    curl_free(value);
    curl_url_cleanup(parsed);
    return copy;
}

bool
snag_mcp_url_valid(const char *url)
{
    if (!url || !*url) return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
        if (*p <= 0x20u || *p >= 0x7fu || *p == '\\') return false;
    char *scheme = snag_mcp_url_part(url, CURLUPART_SCHEME);
    char *host = snag_mcp_url_part(url, CURLUPART_HOST);
    char *user = snag_mcp_url_part(url, CURLUPART_USER);
    char *pass = snag_mcp_url_part(url, CURLUPART_PASSWORD);
    char *fragment = snag_mcp_url_part(url, CURLUPART_FRAGMENT);
    bool valid = scheme && host && !user && !pass && !fragment &&
        (!strcmp(scheme, "https") || (!strcmp(scheme, "http") &&
            (!strcmp(host, "127.0.0.1") || !strcmp(host, "[::1]") ||
                !strcmp(host, "localhost"))));
    free(scheme);
    free(host);
    free(user);
    free(pass);
    free(fragment);
    return valid;
}

int
snag_mcp_header(struct curl_slist **headers, const char *name, const char *value)
{
    if (!value) return snag_errno(EINVAL);
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 0x20u || *p > 0x7eu) return snag_errno(EINVAL);
    struct snag_buf line = {.max = SNAG_MAX_PROVIDER_WIRE};
    int rc = *value ? snag_buf_printf(&line, "%s: %s", name, value)
        : snag_buf_printf(&line, "%s;", name);
    if (!rc) rc = snag_buf_terminate(&line);
    struct curl_slist *next = rc ? NULL : curl_slist_append(*headers, (char *)line.data);
    snag_buf_free(&line);
    if (!next) return -1;
    *headers = next;
    return 0;
}

int
snag_mcp_encoded_header(struct curl_slist **headers, const char *name, const char *value)
{
    size_t length = strlen(value);
    bool encode = !strncmp(value, "=?base64?", 9u) ||
        (length && (isspace((unsigned char)value[0]) ||
            isspace((unsigned char)value[length - 1u])));
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 0x20u || *p > 0x7eu) encode = true;
    if (!encode) return snag_mcp_header(headers, name, value);
    struct snag_buf text = {.max = SNAG_MAX_PROVIDER_WIRE};
    int rc = snag_buf_printf(&text, "=?base64?");
    if (!rc) rc = snag_base64_append(&text, (const unsigned char *)value, strlen(value));
    if (!rc) rc = snag_buf_printf(&text, "?=");
    if (!rc) rc = snag_buf_terminate(&text);
    if (!rc) rc = snag_mcp_header(headers, name, (char *)text.data);
    snag_buf_free(&text);
    return rc;
}

struct transfer {
    struct snag_mcp_http *out;
    struct snag_sse_parser parser;
    json_int_t id;
    bool sse, invalid, subscription, legacy, acknowledged;
};

static int
rpc_message(struct transfer *t, json_t *message)
{
    if (strcmp(snag_mcp_string(message, "jsonrpc"), "2.0")) return -1;
    const json_t *id = json_object_get(message, "id");
    if (!id) {
        if (t->subscription && !t->legacy) {
            const json_t *params = json_object_get(message, "params");
            const json_t *meta = json_object_get(params, "_meta");
            const json_t *subscription = json_object_get(meta,
                "io.modelcontextprotocol/subscriptionId");
            if (!json_is_integer(subscription) || json_integer_value(subscription) != t->id)
                return -1;
            const char *method = snag_mcp_string(message, "method");
            if (!strcmp(method, "notifications/subscriptions/acknowledged")) {
                if (t->acknowledged || !json_is_true(json_object_get(
                        json_object_get(params, "notifications"), "toolsListChanged"))) return -1;
                t->acknowledged = true;
                return 0;
            }
            if (!t->acknowledged || strcmp(method, "notifications/tools/list_changed")) return -1;
        }
        if (!strcmp(snag_mcp_string(message, "method"), "notifications/tools/list_changed"))
            t->out->list_changed = true;
        return 0;
    }
    if (!json_is_integer(id) || json_integer_value(id) != t->id || t->out->reply ||
        (!!json_object_get(message, "result") == !!json_object_get(message, "error")))
        return -1;
    t->out->reply = json_incref(message);
    return 0;
}

static int
sse_record(void *opaque, const struct snag_sse_record *record)
{
    struct transfer *t = opaque;
    if (record->kind != SNAG_SSE_EVENT || !record->data_len) return 0;
    json_t *message = snag_json_load_strict(record->data, record->data_len,
        SNAG_MAX_PROVIDER_WIRE, NULL, 0u);
    int rc = message ? rpc_message(t, message) : -1;
    json_decref(message);
    return rc;
}

static size_t
receive_body(char *data, size_t size, size_t count, void *opaque)
{
    struct transfer *t = opaque;
    if (size && count > SIZE_MAX / size) return 0u;
    size_t bytes = size * count;
    if (t->sse) {
        if (snag_sse_feed(&t->parser, data, bytes, NULL, 0u) < 0) {
            t->invalid = true;
            return 0u;
        }
        /* The response owns this request's stream; completion ends our wait. */
        return t->out->reply ? 0u : bytes;
    }
    return snag_buf_append(&t->out->body, data, bytes) < 0 ? 0u : bytes;
}

static size_t
receive_header(char *data, size_t size, size_t count, void *opaque)
{
    struct transfer *t = opaque;
    if (size && count > SIZE_MAX / size) return 0u;
    size_t bytes = size * count;
    if (bytes >= 5u && !memcmp(data, "HTTP/", 5u)) {
        free(t->out->content_type);
        free(t->out->session);
        free(t->out->challenge);
        t->out->content_type = t->out->session = t->out->challenge = NULL;
        t->sse = false;
        return bytes;
    }
    const char *colon = memchr(data, ':', bytes);
    if (!colon) return bytes;
    size_t key_size = (size_t)(colon - data);
    const char *begin = colon + 1u;
    const char *end = data + bytes;
    while (begin < end && (*begin == ' ' || *begin == '\t')) ++begin;
    while (end > begin && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ')) --end;
    char **target = NULL;
    if (key_size == 12u && !strncasecmp(data, "Content-Type", key_size))
        target = &t->out->content_type;
    else if (key_size == 14u && !strncasecmp(data, "Mcp-Session-Id", key_size))
        target = &t->out->session;
    else if (key_size == 16u && !strncasecmp(data, "WWW-Authenticate", key_size))
        target = &t->out->challenge;
    if (target) {
        if (*target) return 0u;
        *target = strndup(begin, (size_t)(end - begin));
        if (!*target) return 0u;
        if (target == &t->out->content_type)
            t->sse = !strncasecmp(*target, "text/event-stream", 17u);
    }
    return bytes;
}

static int
request_progress(CURL *easy, curl_infotype kind, char *data, size_t size, void *opaque)
{
    (void)easy;
    (void)data;
    (void)size;
    struct transfer *transfer = opaque;
    /* This callback replaces libcurl's diagnostic output. It records only the
     * dispatch boundary, never headers or payload (which may contain tokens).
     * CURLINFO_REQUEST_SIZE can be zero after an empty-response failure. */
    if (kind == CURLINFO_HEADER_OUT) transfer->out->sent = true;
    return 0;
}

void
snag_mcp_http_free(struct snag_mcp_http *response)
{
    if (response->body.data) snag_secret_clear(response->body.data, response->body.len);
    snag_buf_free(&response->body);
    json_decref(response->reply);
    free(response->session);
    free(response->challenge);
    free(response->content_type);
    memset(response, 0, sizeof(*response));
}

static int
configure_http(CURL *easy, struct transfer *t, const char *url, const char *method,
    const char *body, struct curl_slist *headers, uint64_t timeout)
{
#define OPTION(k, v) do { if (curl_easy_setopt(easy, k, v) != CURLE_OK) return -1; } while (0)
    OPTION(CURLOPT_URL, url);
    OPTION(CURLOPT_NOSIGNAL, 1L);
    OPTION(CURLOPT_FOLLOWLOCATION, 0L);
    OPTION(CURLOPT_TIMEOUT_MS, (long)timeout);
    OPTION(CURLOPT_CONNECTTIMEOUT_MS, (long)(timeout && timeout < 30000u ? timeout : 30000u));
    OPTION(CURLOPT_HTTPHEADER, headers);
    OPTION(CURLOPT_CUSTOMREQUEST, method);
    OPTION(CURLOPT_WRITEFUNCTION, receive_body);
    OPTION(CURLOPT_WRITEDATA, t);
    OPTION(CURLOPT_HEADERFUNCTION, receive_header);
    OPTION(CURLOPT_HEADERDATA, t);
    OPTION(CURLOPT_USERAGENT, SNAJPAGENT_IDENTITY);
    OPTION(CURLOPT_VERBOSE, 1L);
    OPTION(CURLOPT_DEBUGFUNCTION, request_progress);
    OPTION(CURLOPT_DEBUGDATA, t);
    if (body) {
        OPTION(CURLOPT_POSTFIELDS, body);
        OPTION(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(body));
    }
#undef OPTION
    return 0;
}

int
snag_mcp_http(struct snag_mcp_server *server, const char *url, const char *method,
    const char *body, struct curl_slist *headers, json_int_t id, snag_mcp_pump_fn pump,
    void *opaque, struct snag_mcp_http *out)
{
    memset(out, 0, sizeof(*out));
    out->body.max = SNAG_MAX_PROVIDER_WIRE;
    if (!snag_mcp_url_valid(url))
        return snag_errorf(server->error, sizeof(server->error), "unsafe MCP/OAuth URL");
    uint64_t timeout = (uint64_t)json_integer_value(json_object_get(server->config, "timeout_ms"));
    if (!timeout) timeout = 60000u;
    uint64_t now = snag_monotonic_ms();
    if (server->operation_deadline_ms) {
        if (now >= server->operation_deadline_ms)
            return snag_errorf(server->error, sizeof(server->error), "MCP operation timed out");
        uint64_t remaining = server->operation_deadline_ms - now;
        if (remaining < timeout) timeout = remaining;
    }
    uint64_t deadline = now + timeout;
    if (pump && pump(opaque, 0u)) {
        out->cancelled = true;
        return snag_errno(ECANCELED);
    }
    CURL *easy = NULL;
    CURLM *multi = NULL;
    struct transfer t = {.out = out, .id = id};
    snag_sse_init(&t.parser, sse_record, &t);
    int rc = -1;
    if (snag_http_init() != CURLE_OK || !(easy = curl_easy_init()) ||
        !(multi = curl_multi_init()) || snag_http_trust(easy) != CURLE_OK)
        goto done;
    if (configure_http(easy, &t, url, method, body, headers, timeout) < 0) goto done;
    if (curl_multi_add_handle(multi, easy) != CURLM_OK) goto done;
    int running = 1;
    while (running) {
        if (curl_multi_perform(multi, &running) != CURLM_OK) goto done;
        if (!running) break;
        if (snag_monotonic_ms() >= deadline) {
            out->code = CURLE_OPERATION_TIMEDOUT;
            break;
        }
        int ready = 0;
        if (curl_multi_wait(multi, NULL, 0u, 20, &ready) != CURLM_OK) goto done;
        if (!ready && snag_sleep_ms(1u) < 0) goto done;
        if (pump && pump(opaque, 0u) != 0) {
            out->cancelled = true;
            out->code = CURLE_ABORTED_BY_CALLBACK;
            break;
        }
    }
    int messages;
    CURLMsg *message;
    while ((message = curl_multi_info_read(multi, &messages)))
        if (message->msg == CURLMSG_DONE) out->code = message->data.result;
    (void)curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &out->status);
    long request_bytes = 0L;
    (void)curl_easy_getinfo(easy, CURLINFO_REQUEST_SIZE, &request_bytes);
    out->sent = out->sent || request_bytes > 0L;
    if (t.sse && out->reply && !t.invalid) out->code = CURLE_OK;
    if (out->code == CURLE_OK && !t.sse && id >= 0 && out->body.len) {
        json_t *value = snag_json_load_strict(out->body.data, out->body.len,
            SNAG_MAX_PROVIDER_WIRE, NULL, 0u);
        int parsed = -1;
        if (out->status >= 400L && json_is_object(value) &&
            !strcmp(snag_mcp_string(value, "jsonrpc"), "2.0") &&
            json_is_object(json_object_get(value, "error"))) {
            const json_t *reply_id = json_object_get(value, "id");
            if (reply_id && !json_is_null(reply_id) &&
                (!json_is_integer(reply_id) || json_integer_value(reply_id) != id)) {
                json_decref(value);
                snag_errorf(server->error, sizeof(server->error), "invalid MCP JSON-RPC error ID");
                goto done;
            }
            out->reply = json_incref(value);
            parsed = 0;
        } else if (value) {
            parsed = rpc_message(&t, value);
        }
        if (parsed < 0 &&
            out->status >= 200L && out->status < 300L) {
            json_decref(value);
            snag_errorf(server->error, sizeof(server->error), "invalid MCP JSON-RPC response");
            goto done;
        }
        json_decref(value);
    }
    if (out->code != CURLE_OK) {
        snag_errorf(server->error, sizeof(server->error), "MCP transport: %s",
            out->cancelled ? "cancelled" : curl_easy_strerror(out->code));
        goto done;
    }
    if (out->status >= 300L && out->status < 400L) {
        snag_errorf(server->error, sizeof(server->error), "MCP/OAuth redirect refused");
        goto done;
    }
    rc = 0;
done:
    if (multi && easy) (void)curl_multi_remove_handle(multi, easy);
    curl_easy_cleanup(easy);
    curl_multi_cleanup(multi);
    snag_sse_free(&t.parser);
    return rc;
}

static int
authorize(struct snag_mcp_server *server, struct curl_slist **headers)
{
    const char *token = snag_mcp_string(server->auth, "access_token");
    if (!*token) return 0;
    struct snag_buf value = {.max = SNAG_SECRET_MAX + 8u};
    int rc = snag_buf_printf(&value, "Bearer %s", token);
    if (!rc) rc = snag_buf_terminate(&value);
    if (!rc) rc = snag_mcp_header(headers, "Authorization", (char *)value.data);
    if (value.data) snag_secret_clear(value.data, value.len);
    snag_buf_free(&value);
    return rc;
}

void
snag_mcp_end_session(struct snag_mcp_server *server, uint64_t remaining_ms)
{
    if (!server->session || !remaining_ms) return;
    struct curl_slist *headers = NULL;
    struct snag_mcp_http response = {0};
    if (authorize(server, &headers) == 0 &&
        snag_mcp_header(&headers, "Mcp-Session-Id", server->session) == 0 &&
        snag_mcp_header(&headers, "MCP-Protocol-Version", server->version) == 0 &&
        json_object_set_new(server->config, "timeout_ms",
            json_integer((json_int_t)remaining_ms)) == 0)
        (void)snag_mcp_http(server, snag_mcp_string(server->config, "url"), "DELETE", NULL,
            headers, -1, NULL, NULL, &response);
    curl_slist_free_all(headers);
    snag_mcp_http_free(&response);
}

int
snag_mcp_rpc(struct snag_mcp_server *server, const char *method, json_t *params,
    struct curl_slist *headers, snag_mcp_pump_fn pump, void *opaque, json_t **result,
    bool *sent)
{
    bool notification = !strncmp(method, "notifications/", 14u);
    json_int_t id = (json_int_t)++server->request_id;
    *result = NULL;
    if (sent) *sent = false;
    json_t *request = json_pack("{s:s,s:s,s:O}", "jsonrpc", "2.0", "method", method,
        "params", params);
    struct snag_mcp_http response = {0};
    char *body = NULL;
    int rc = -1;
    if (!request || (!notification && json_object_set_new(request, "id", json_integer(id)) < 0))
        goto done;
    if (!server->legacy) {
        json_t *meta = json_pack("{s:s,s:{s:s,s:s},s:{}}",
            "io.modelcontextprotocol/protocolVersion", SNAG_MCP_CURRENT,
            "io.modelcontextprotocol/clientInfo", "name", SNAJPAGENT_NAME,
            "version", SNAJPAGENT_VERSION, "io.modelcontextprotocol/clientCapabilities");
        if (!meta || json_object_set_new(params, "_meta", meta) < 0) goto done;
    }
    if (snag_mcp_header(&headers, "Content-Type", "application/json") < 0 ||
        snag_mcp_header(&headers, "Accept", "application/json, text/event-stream") < 0 ||
        snag_mcp_header(&headers, "MCP-Protocol-Version", server->version) < 0)
        goto done;
    if (!server->legacy && (snag_mcp_header(&headers, "Mcp-Method", method) < 0 ||
        (*snag_mcp_string(params, "name") && snag_mcp_encoded_header(&headers, "Mcp-Name",
            snag_mcp_string(params, "name")) < 0)))
        goto done;
    if (server->session && snag_mcp_header(&headers, "Mcp-Session-Id", server->session) < 0)
        goto done;
    if (authorize(server, &headers) < 0) goto done;
    body = json_dumps(request, JSON_COMPACT | JSON_ENSURE_ASCII);
    if (!body) goto done;
    rc = snag_mcp_http(server, snag_mcp_string(server->config, "url"), "POST", body,
        headers, notification ? -1 : id, pump, opaque, &response);
    if (sent) *sent = response.sent;
    if (response.list_changed) server->dirty = true;
    if (response.session && !strcmp(method, "initialize")) {
        free(server->session);
        server->session = response.session;
        response.session = NULL;
    }
    if (rc < 0) goto done;
    /* Only discovery may fall back; a tool call is never replayed. */
    json_t *rpc_error = json_object_get(response.reply, "error");
    json_int_t error_code = json_integer_value(json_object_get(rpc_error, "code"));
    if (!server->legacy && response.status == 400L &&
        error_code != -32020 && error_code != -32021 && error_code != -32022) {
        rc = 1;
        goto done;
    }
    if (!server->legacy && response.status == 400L && error_code == -32022 &&
        !strcmp(method, "tools/list")) {
        const json_t *supported = json_object_get(json_object_get(rpc_error, "data"), "supported");
        for (size_t i = 0u; i < json_array_size(supported); ++i) {
            const char *version = json_string_value(json_array_get(supported, i));
            if (version && snag_string_in(version, "2025-03-26 2025-06-18 2025-11-25")) {
                (void)snag_strcpy(server->version, sizeof(server->version), version);
                rc = 1;
                goto done;
            }
        }
    }
    if (response.status == 401L || response.status == 403L) {
        if (sent) *sent = false;
        if (response.status == 401L) server->force_refresh = true;
    }
    if (error_code == -32020 || error_code == -32021 || error_code == -32022) {
        if (sent) *sent = false;
        rc = snag_errorf(server->error, sizeof(server->error),
            "MCP protocol error %lld: %s", (long long)error_code,
            error_code == -32022 ? "no supported protocol version" :
                error_code == -32021 ? "required client capability unavailable" :
                    "header/schema mismatch; reload catalog at next turn");
        if (error_code == -32020) server->dirty = true;
        goto done;
    }
    if (response.status != 200L && !(notification && response.status == 202L)) {
        rc = snag_errorf(server->error, sizeof(server->error), "MCP HTTP %ld: %s",
            response.status, response.status == 401L ? "login required" :
                response.status == 403L ? "access denied or missing scope" : "server failure");
        goto done;
    }
    if (notification) goto done;
    if (!response.content_type ||
        (strncasecmp(response.content_type, "application/json", 16u) &&
            strncasecmp(response.content_type, "text/event-stream", 17u))) {
        rc = snag_errorf(server->error, sizeof(server->error), "invalid MCP Content-Type");
        goto done;
    }
    const json_t *remote_error = json_object_get(response.reply, "error");
    if (remote_error) {
        rc = snag_errorf(server->error, sizeof(server->error), "MCP protocol error %lld",
            (long long)json_integer_value(json_object_get(remote_error, "code")));
        goto done;
    }
    json_t *value = json_object_get(response.reply, "result");
    if (!json_is_object(value)) {
        rc = snag_errorf(server->error, sizeof(server->error), "missing MCP result");
        goto done;
    }
    const char *type = snag_mcp_string(value, "resultType");
    if ((json_object_get(value, "resultType") &&
            !json_is_string(json_object_get(value, "resultType"))) ||
        (*type && !snag_string_in(type, "complete input_required")) ||
        (!strcmp(type, "input_required") && strcmp(method, "tools/call"))) {
        rc = snag_errorf(server->error, sizeof(server->error), "unsupported MCP result type");
        goto done;
    }
    const char *version = snag_mcp_string(json_object_get(value, "_meta"),
        "io.modelcontextprotocol/protocolVersion");
    if (*version && strcmp(version, server->version)) {
        rc = snag_errorf(server->error, sizeof(server->error), "MCP protocol version mismatch");
        goto done;
    }
    *result = json_incref(value);
    server->contact_ms = snag_time_ms();
    server->error[0] = '\0';
done:
    free(body);
    json_decref(request);
    snag_mcp_http_free(&response);
    curl_slist_free_all(headers);
    return rc;
}

struct snag_mcp_watch {
    struct snag_mcp_http response;
    struct transfer transfer;
    CURL *easy;
    CURLM *multi;
    struct curl_slist *headers;
    char *body;
    uint64_t deadline_ms;
};

void
snag_mcp_watch_close(struct snag_mcp_server *server)
{
    struct snag_mcp_watch *watch = server->watch;
    if (!watch) return;
    if (watch->multi && watch->easy)
        (void)curl_multi_remove_handle(watch->multi, watch->easy);
    curl_easy_cleanup(watch->easy);
    curl_multi_cleanup(watch->multi);
    snag_sse_free(&watch->transfer.parser);
    snag_mcp_http_free(&watch->response);
    curl_slist_free_all(watch->headers);
    free(watch->body);
    free(watch);
    server->watch = NULL;
}

void
snag_mcp_watch_start(struct snag_mcp_server *server)
{
    if (server->watch || !server->enabled) return;
    if (server->legacy && !json_is_true(json_object_get(json_object_get(
            json_object_get(server->metadata, "capabilities"), "tools"), "listChanged"))) return;
    struct snag_mcp_watch *watch = calloc(1u, sizeof(*watch));
    if (!watch) return;
    server->watch = watch;
    watch->response.body.max = SNAG_MAX_PROVIDER_WIRE;
    watch->transfer = (struct transfer){.out = &watch->response,
        .id = (json_int_t)++server->request_id, .subscription = true, .legacy = server->legacy};
    snag_sse_init(&watch->transfer.parser, sse_record, &watch->transfer);
    uint64_t timeout = (uint64_t)json_integer_value(json_object_get(server->config, "timeout_ms"));
    watch->deadline_ms = snag_monotonic_ms() + (timeout ? timeout : 60000u);
    if (authorize(server, &watch->headers) < 0 ||
        snag_mcp_header(&watch->headers, "Accept", "application/json, text/event-stream") < 0 ||
        snag_mcp_header(&watch->headers, "MCP-Protocol-Version", server->version) < 0) goto fail;
    if (server->legacy) {
        if (server->session &&
            snag_mcp_header(&watch->headers, "Mcp-Session-Id", server->session) < 0) goto fail;
    } else {
        json_t *request = json_pack("{s:s,s:I,s:s,s:{s:{s:b},s:{s:s,s:{s:s,s:s},s:{}}}}",
            "jsonrpc", "2.0", "id", watch->transfer.id, "method", "subscriptions/listen",
            "params", "notifications", "toolsListChanged", 1, "_meta",
            "io.modelcontextprotocol/protocolVersion", server->version,
            "io.modelcontextprotocol/clientInfo", "name", SNAJPAGENT_NAME,
            "version", SNAJPAGENT_VERSION, "io.modelcontextprotocol/clientCapabilities");
        watch->body = request ? json_dumps(request, JSON_COMPACT) : NULL;
        json_decref(request);
        if (!watch->body ||
            snag_mcp_header(&watch->headers, "Content-Type", "application/json") < 0 ||
            snag_mcp_header(&watch->headers, "Mcp-Method", "subscriptions/listen") < 0) goto fail;
    }
    if (snag_http_init() != CURLE_OK || !(watch->easy = curl_easy_init()) ||
        !(watch->multi = curl_multi_init()) || snag_http_trust(watch->easy) != CURLE_OK ||
        configure_http(watch->easy, &watch->transfer, snag_mcp_string(server->config, "url"),
            server->legacy ? "GET" : "POST", watch->body, watch->headers, 0u) < 0 ||
        curl_multi_add_handle(watch->multi, watch->easy) != CURLM_OK) goto fail;
    return;
fail:
    snag_mcp_watch_close(server);
    server->dirty = true;
}

bool
snag_mcp_watching(const struct snag_mcp *client)
{
    if (client) {
        for (size_t i = 0u; i < client->count; ++i)
            if (client->servers[i].watch) return true;
    }
    return false;
}

void
snag_mcp_poll(struct snag_mcp *client)
{
    if (!client) return;
    for (size_t i = 0u; i < client->count; ++i) {
        struct snag_mcp_server *server = &client->servers[i];
        struct snag_mcp_watch *watch = server->watch;
        if (!watch) continue;
        int running = 0;
        CURLMcode rc = curl_multi_perform(watch->multi, &running);
        if (watch->response.list_changed) {
            server->dirty = true;
            watch->response.list_changed = false;
        }
        bool started = server->legacy ? watch->transfer.sse : watch->transfer.acknowledged;
        if (rc != CURLM_OK || !running || watch->transfer.invalid ||
            (!started && snag_monotonic_ms() >= watch->deadline_ms) ||
            (server->token_deadline_ms && snag_monotonic_ms() >= server->token_deadline_ms)) {
            snag_mcp_watch_close(server);
            /* Refresh at the next turn also repairs notifications lost on disconnect. */
            server->dirty = true;
        }
    }
}
