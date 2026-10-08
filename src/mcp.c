/* SPDX-License-Identifier: GPL-2.0-only */
#include "mcp_internal.h"
#include "auth.h"
#include "fs.h"
#include "store.h"
#include "wire.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct snag_mcp *
snag_mcp_open(int root_fd, struct snag_config *config)
{
    struct snag_mcp *client = calloc(1u, sizeof(*client));
    if (!client) return NULL;
    client->root_fd = root_fd;
    if (!config->mcp_credentials) config->mcp_credentials = json_array();
    client->secrets = json_incref(config->mcp_credentials);
    if (!client->secrets) goto fail;
    client->pending = json_object();
    client->approved = json_object();
    client->inspected = json_object();
    client->uncertain = json_object();
    size_t count = json_object_size(config->mcp_servers);
    if (count) client->servers = calloc(count, sizeof(*client->servers));
    if (!client->pending || !client->approved || !client->inspected || !client->uncertain ||
        (count && !client->servers)) {
        goto fail;
    }
    const char *name;
    json_t *value;
    json_object_foreach(config->mcp_servers, name, value) {
        struct snag_mcp_server *server = &client->servers[client->count++];
        server->name = strdup(name);
        server->config = json_deep_copy(value);
        server->enabled = !json_is_false(json_object_get(value, "enabled"));
        server->dirty = true;
        (void)snag_strcpy(server->version, sizeof(server->version), SNAG_MCP_CURRENT);
        if (!server->name || !server->config) goto fail;
        const char *url = snag_mcp_string(value, "url");
        if (!snag_mcp_url_valid(url)) {
            server->enabled = false;
            (void)snag_errorf(server->error, sizeof(server->error), "invalid or unsafe MCP URL");
        }
        json_t *identity = json_pack("[s,s,s,s,s]", name, url,
            snag_mcp_string(value, "resource"), snag_mcp_string(value, "issuer"),
            snag_mcp_string(value, "client_id"));
        int hashed = identity ? snag_json_digest_bounded(identity, SNAG_CONFIG_FILE_MAX,
            server->binding, NULL) : -1;
        json_decref(identity);
        if (hashed < 0) goto fail;
        const char *secret = snag_mcp_string(value, "client_secret");
        if (*secret) {
            struct snag_secret_source source = {0};
            int rc = snag_secret_source_parse(&source, secret, config->source_path,
                server->error, sizeof(server->error));
            if (!rc) rc = snag_secret_source_resolve(&source, &server->secret,
                server->error, sizeof(server->error));
            snag_secret_source_free(&source);
            if (rc < 0) server->enabled = false;
        }
        if (snag_mcp_protect(client, server) < 0) goto fail;
        if (server->enabled && snag_mcp_auth_load(client, server) < 0) server->enabled = false;
    }
    return client;
fail:
    snag_mcp_close(client);
    return NULL;
}

int
snag_mcp_protect(struct snag_mcp *client, const struct snag_mcp_server *server)
{
    const char *values[] = {server->secret ? server->secret : "",
        snag_mcp_string(server->auth, "access_token"),
        snag_mcp_string(server->auth, "refresh_token")};
    for (size_t i = 0u; i < 3u; ++i) {
        if (!*values[i]) continue;
        bool present = false;
        for (size_t j = 0u; j < json_array_size(client->secrets); ++j)
            if (!strcmp(values[i], json_string_value(json_array_get(client->secrets, j))))
                present = true;
        if (!present && json_array_append_new(client->secrets, json_string(values[i])) < 0)
            return -1;
    }
    return 0;
}

void
snag_mcp_close(struct snag_mcp *client)
{
    if (!client) return;
    uint64_t deadline = snag_monotonic_ms() + 2000u;
    for (size_t i = 0u; i < client->count; ++i) {
        struct snag_mcp_server *server = &client->servers[i];
        uint64_t now = snag_monotonic_ms();
        snag_mcp_watch_close(server);
        snag_mcp_end_session(server, now < deadline ? deadline - now : 0u);
        free(server->name);
        json_decref(server->config);
        snag_auth_json_free(server->auth);
        json_decref(server->metadata);
        json_decref(server->catalog);
        json_decref(server->filtered);
        snag_secret_bytes_free(server->secret);
        free(server->session);
    }
    free(client->servers);
    json_decref(client->tools);
    json_decref(client->routes);
    json_decref(client->pending);
    json_decref(client->approved);
    json_decref(client->inspected);
    json_decref(client->uncertain);
    json_decref(client->secrets);
    snag_mcp_close(client->replacement);
    free(client);
}

void
snag_mcp_configure(struct snag_mcp *client, struct snag_mcp *replacement)
{
    /* The current turn can still refresh its old grant before the swap. Both
     * clients share the new config's redaction set throughout that interval. */
    json_decref(client->secrets);
    client->secrets = json_incref(replacement->secrets);
    snag_mcp_close(client->replacement);
    client->replacement = replacement;
}

int
snag_mcp_prepare(struct snag_mcp *client, const struct snag_session *session,
    snag_mcp_pump_fn pump, void *opaque)
{
    int session_fd = session->dir_fd;
    const char *turn_id = session->active_turn_id;
    bool read_only = session->active_read_only;
    if (!client || !strcmp(client->turn_id, turn_id)) return 0;
    if (client->replacement) {
        struct snag_mcp *next = client->replacement;
        client->replacement = NULL;
        struct snag_mcp old = *client;
        *client = *next;
        *next = old;
        snag_mcp_close(next);
    }
    client->session = session;
    int restored = client->count && !*client->turn_id
        ? snag_mcp_snapshot(client, session_fd, turn_id, false) : 0;
    if (restored < 0) {
        for (size_t i = 0u; i < client->count; ++i) {
            json_decref(client->servers[i].catalog);
            client->servers[i].catalog = NULL;
            snag_errorf(client->servers[i].error, sizeof(client->servers[i].error),
                "invalid MCP turn snapshot; MCP tools withheld");
        }
        if (snag_mcp_project(client, read_only) < 0) return -1;
        (void)snag_strcpy(client->turn_id, sizeof(client->turn_id), turn_id);
        return 0;
    }
    client->busy = true;
    for (size_t i = 0u; i < client->count; ++i) {
        struct snag_mcp_server *server = &client->servers[i];
        if (!server->enabled) continue;
        if (snag_mcp_auth_refresh(client, server, pump, opaque) < 0 ||
            (!restored && (server->dirty || snag_monotonic_ms() >= server->expires_ms) &&
                snag_mcp_catalog(server, pump, opaque) < 0)) {
            /* A failed refresh cannot expose a stale remote contract. */
            json_decref(server->catalog);
            server->catalog = NULL;
        }
        if (server->catalog) snag_mcp_watch_start(server);
    }
    client->busy = false;
    if (snag_mcp_project(client, read_only) < 0 ||
        (client->count && !restored && snag_mcp_snapshot(client, session_fd, turn_id, true) < 0))
        return -1;
    (void)snag_strcpy(client->turn_id, sizeof(client->turn_id), turn_id);
    return 0;
}

const json_t *
snag_mcp_tools(const struct snag_mcp *client)
{
    return client ? client->tools : NULL;
}

bool
snag_mcp_contains_secret(struct snag_mcp_server *server, const json_t *value)
{
    const char *tokens[] = {snag_mcp_string(server->auth, "access_token"),
        snag_mcp_string(server->auth, "refresh_token"), server->secret ? server->secret : ""};
    if (json_is_string(value)) {
        const char *text = json_string_value(value);
        for (size_t i = 0u; i < 3u; ++i)
            if (*tokens[i] && strstr(text, tokens[i])) return true;
    } else if (json_is_array(value)) {
        for (size_t i = 0u; i < json_array_size(value); ++i)
            if (snag_mcp_contains_secret(server, json_array_get(value, i))) return true;
    } else if (json_is_object(value)) {
        const char *key;
        json_t *child;
        json_object_foreach((json_t *)value, key, child) {
            for (size_t i = 0u; i < 3u; ++i)
                if (*tokens[i] && strstr(key, tokens[i])) return true;
            if (snag_mcp_contains_secret(server, child)) return true;
        }
    }
    return false;
}

static json_t *
redacted(struct snag_mcp_server *server, const json_t *value)
{
    const char *tokens[] = {snag_mcp_string(server->auth, "access_token"),
        snag_mcp_string(server->auth, "refresh_token"), server->secret ? server->secret : ""};
    const char *present[3];
    size_t count = 0u;
    for (size_t i = 0u; i < 3u; ++i)
        if (*tokens[i]) present[count++] = tokens[i];
    struct snag_wire_secrets secrets = {.values = present, .count = count};
    struct snag_buf raw = {.max = SNAG_MAX_PROVIDER_WIRE};
    struct snag_buf clean = {.max = SNAG_MAX_PROVIDER_WIRE};
    json_t *result = NULL;
    if (snag_json_diagnostic(value, &raw) == 0 &&
        snag_wire_json_redact_bounded(raw.data, raw.len, raw.max, &secrets, &clean, NULL, 0u) == 0)
        result = snag_json_load_strict(clean.data, clean.len, clean.max, NULL, 0u);
    if (raw.data) snag_secret_clear(raw.data, raw.len);
    snag_buf_free(&raw);
    snag_buf_free(&clean);
    return result;
}

static char *
retain_result(struct snag_mcp *client, const char *call_id, const char *text, json_t *receipt)
{
    const struct snag_session *session = client->session;
    if (!session || !snag_hex_is_lower(call_id, SNAG_ID_HEX_LEN)) return NULL;
    if (snag_mkdir_private_at(session->dir_fd, "mcp-results") < 0 && errno != EEXIST) return NULL;
    int dir = snag_open_read_security_at(session->dir_fd, "mcp-results", true);
    if (dir < 0) return NULL;
    struct snag_file_privacy privacy;
    char name[SNAG_ID_HEX_LEN + 6u];
    char temporary[SNAG_ID_HEX_LEN + 1u] = {0};
    (void)snprintf(name, sizeof(name), "%s.json", call_id);
    int fd = -1;
    char *summary = NULL;
    struct snag_buf path = {.max = SNAG_PATH_MAX_BYTES};
    size_t size = strlen(text);
    if (snag_fd_privacy(dir, &privacy) < 0 || !privacy.effective_owner ||
        !privacy.private_access || snag_random_id(temporary) < 0) goto done;
    fd = snag_create_private_at(dir, temporary, true);
    if (fd < 0 || snag_write_full(fd, text, size) < 0 || snag_fsync(fd) < 0 ||
        snag_rename_at(dir, temporary, dir, name) < 0 || snag_sync_dir(dir) < 0) goto done;
    temporary[0] = '\0';
    if (snag_buf_printf(&path, "%s/mcp-results/%s", session->dir_path, name) < 0 ||
        snag_buf_terminate(&path) < 0) goto done;
    char hex[SNAG_SHA256_HEX_LEN + 1u];
    snag_sha256_hex(text, size, hex);
    (void)json_object_del(receipt, "result");
    if (json_object_set_new(receipt, "result_file", json_string((char *)path.data)) < 0 ||
        json_object_set_new(receipt, "result_bytes", json_integer((json_int_t)size)) < 0 ||
        json_object_set_new(receipt, "result_sha256", json_string(hex)) < 0) goto done;
    summary = json_dumps(receipt, JSON_COMPACT | JSON_SORT_KEYS);
done:
    if (*temporary) (void)snag_unlink_at(dir, temporary, false);
    if (fd >= 0) (void)close(fd);
    (void)close(dir);
    snag_buf_free(&path);
    return summary;
}

static int
call_remote(struct snag_mcp *client, const struct snag_response_item *call,
    snag_mcp_pump_fn pump, void *opaque, json_t **result)
{
    *result = NULL;
    json_t *route = client ? json_object_get(client->routes, call->name) : NULL;
    if (!route) {
        *result = snag_tool_result_not_run("mcp_tool_not_in_turn_catalog");
        return *result ? 0 : -1;
    }
    size_t index = (size_t)json_integer_value(json_object_get(route, "server"));
    struct snag_mcp_server *server = &client->servers[index];
    if (!server->enabled) {
        *result = snag_tool_result_not_run("mcp_tool_not_in_turn_catalog");
        return *result ? 0 : -1;
    }
    json_t *tool = json_object_get(route, "tool");
    const char *name = snag_mcp_string(tool, "name");
    json_t *request = json_pack("{s:s,s:s,s:O,s:O}", "server", server->name,
        "endpoint", snag_mcp_string(server->config, "url"), "tool", tool,
        "arguments", call->arguments);
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    if (!request || snag_json_wire_digest(request, SNAG_MAX_PROVIDER_WIRE, digest, NULL) < 0) {
        json_decref(request);
        return -1;
    }
    bool admitted = (json_is_true(json_object_get(route, "allowed")) &&
        !json_object_get(client->uncertain, digest)) ||
        json_is_true(json_object_get(client->approved, digest));
    if (!admitted) {
        if (json_object_set(client->pending, digest, request) < 0) {
            json_decref(request);
            return -1;
        }
        char message[256];
        (void)snprintf(message, sizeof(message),
            "MCP call requires exact operator approval. Inspect /mcp pending %s; "
            "approve with /mcp approve %s, then repeat these exact arguments.", digest, digest);
        *result = snag_tool_result("not_run", "mcp_approval_required", message, -1, 0u);
        json_decref(request);
        return *result ? 0 : -1;
    }
    (void)json_object_del(client->approved, digest);
    (void)json_object_del(client->inspected, digest);
    (void)json_object_del(client->uncertain, digest);
    (void)json_object_del(client->pending, digest);
    json_decref(request);
    struct curl_slist *headers = NULL;
    if (snag_mcp_parameter_headers(json_object_get(tool, "inputSchema"), call->arguments,
            &headers) < 0) {
        curl_slist_free_all(headers);
        *result = snag_tool_result_not_run("mcp_invalid_header_arguments");
        return *result ? 0 : -1;
    }
    client->busy = true;
    bool sent = false;
    json_t *response = NULL;
    uint64_t started = snag_monotonic_ms();
    int rc = snag_mcp_auth_refresh(client, server, pump, opaque);
    json_t *params = json_pack("{s:s,s:O}", "name", name, "arguments", call->arguments);
    if (!rc && server->legacy && !server->session)
        rc = snag_mcp_initialize(server, pump, opaque);
    if (!rc && params) {
        rc = snag_mcp_rpc(server, "tools/call", params, headers, pump, opaque, &response, &sent);
        headers = NULL;
    } else {
        rc = -1;
    }
    client->busy = false;
    json_decref(params);
    curl_slist_free_all(headers);
    if (rc || !response) {
        if (sent && !json_is_true(json_object_get(route, "read_only")) &&
            json_object_set_new(client->uncertain, digest, json_true()) < 0) {
            json_decref(response);
            return -1;
        }
        *result = sent ? snag_tool_result("outcome_unknown", "mcp_outcome_unknown",
            server->error, -1, 0u)
            : snag_tool_result("not_run", "mcp_transport", server->error, -1, 0u);
    } else {
        bool incomplete = !strcmp(snag_mcp_string(response, "resultType"), "input_required");
        bool failed = incomplete || json_is_true(json_object_get(response, "isError"));
        if (incomplete && !json_is_true(json_object_get(route, "read_only")))
            (void)json_object_set_new(client->uncertain, digest, json_true());
        json_t *clean = redacted(server, response);
        json_t *receipt = clean ? json_pack("{s:s,s:s,s:s,s:s,s:I,s:s,s:s,s:O}",
            "server", server->name, "tool", name, "action_sha256", digest,
            "endpoint", snag_mcp_string(server->config, "url"),
            "received_at_ms", (json_int_t)snag_time_ms(), "status",
            incomplete ? "input_required" : failed ? "failed" : "server_accepted",
            "meaning", "MCP result; external delivery/read-back is described only by server data",
            "result", clean) : NULL;
        json_decref(clean);
        char *text = receipt ? json_dumps(receipt, JSON_COMPACT | JSON_SORT_KEYS) : NULL;
        if (text && strlen(text) > SNAG_MAX_PUBLIC_ITEM) {
            char *summary = retain_result(client, call->call_id, text, receipt);
            free(text);
            text = summary;
        }
        json_decref(receipt);
        if (text && strlen(text) <= SNAG_MAX_PUBLIC_ITEM) {
            *result = snag_tool_result(failed ? "failed" : "succeeded", NULL, text, 0,
                snag_monotonic_ms() - started);
        } else {
            if (!json_is_true(json_object_get(route, "read_only")))
                (void)json_object_set_new(client->uncertain, digest, json_true());
            *result = snag_tool_result("outcome_unknown", "mcp_outcome_unknown",
                "MCP result could not be retained; inspect remote state before repeating", -1, 0u);
        }
        free(text);
    }
    json_decref(response);
    return *result ? 0 : -1;
}

int
snag_mcp_call(struct snag_mcp *client, const struct snag_response_item *call,
    snag_mcp_pump_fn pump, void *opaque, json_t **result)
{
    struct snag_response_item decoded = *call;
    decoded.arguments = snag_response_arguments(call);
    if (!decoded.arguments) {
        *result = snag_tool_result_not_run("invalid_arguments");
        return *result ? 0 : -1;
    }
    int rc = call_remote(client, &decoded, pump, opaque, result);
    json_decref(decoded.arguments);
    return rc;
}

static struct snag_mcp_server *
server_named(struct snag_mcp *client, const char *name)
{
    for (size_t i = 0u; i < client->count; ++i)
        if (!strcmp(client->servers[i].name, name)) return &client->servers[i];
    return NULL;
}

int
snag_mcp_command(struct snag_mcp *client, const char *command, struct snag_buf *report,
    snag_mcp_pump_fn pump, void *opaque)
{
    while (isspace((unsigned char)*command)) ++command;
    const char *space = command;
    while (*space && !isspace((unsigned char)*space)) ++space;
    size_t length = (size_t)(space - command);
    const char *argument = space;
    while (isspace((unsigned char)*argument)) ++argument;
    char verb[16];
    if (length >= sizeof(verb)) return snag_errno(EINVAL);
    memcpy(verb, command, length);
    verb[length] = '\0';
    if (!*verb || !strcmp(verb, "list")) {
        for (size_t i = 0u; i < client->count; ++i) {
            struct snag_mcp_server *s = &client->servers[i];
            if (snag_buf_printf(report, "%s: %s; %zu tools; generation %llu%s%s\n", s->name,
                    s->enabled ? "enabled" : "disabled", json_array_size(s->catalog),
                    (unsigned long long)s->generation, *s->error ? "; " : "", s->error) < 0) {
                return -1;
            }
        }
        return client->count ? 0 : snag_buf_printf(report, "No MCP servers configured.\n");
    }
    if (!strcmp(verb, "pending")) {
        json_t *pending = *argument ? json_object_get(client->pending, argument) : client->pending;
        if (!pending) return snag_buf_printf(report, "No pending call with that digest.\n");
        char *text = json_dumps(pending, JSON_INDENT(2) | JSON_SORT_KEYS);
        int rc = text ? snag_buf_printf(report, "%s\n", text) : -1;
        if (!rc && *argument) rc = json_object_set_new(client->inspected, argument, json_true());
        free(text);
        return rc;
    }
    if (!strcmp(verb, "approve") || !strcmp(verb, "deny")) {
        json_t *pending = json_object_get(client->pending, argument);
        if (!pending) return snag_buf_printf(report, "No pending call with that digest.\n");
        if (!strcmp(verb, "approve")) {
            if (!json_is_true(json_object_get(client->inspected, argument))) {
                return snag_buf_printf(report,
                    "Inspect /mcp pending %s before approving.\n", argument);
            }
            if (json_object_set_new(client->approved, argument, json_true()) < 0) return -1;
            return snag_buf_printf(report,
                "Approved exactly one matching MCP call: %s\n", argument);
        }
        (void)json_object_del(client->approved, argument);
        (void)json_object_del(client->inspected, argument);
        (void)json_object_del(client->pending, argument);
        return snag_buf_printf(report, "Denied MCP call: %s\n", argument);
    }
    struct snag_mcp_server *s = server_named(client, argument);
    if (!s) return snag_buf_printf(report, "Unknown MCP server. Use /mcp list.\n");
    if (!strcmp(verb, "reload")) {
        s->dirty = true;
        return snag_buf_printf(report,
            "%s: catalog reload scheduled for the next turn.\n", s->name);
    }
    if (!strcmp(verb, "login")) {
        return snag_buf_printf(report,
            "Run snajpagent mcp login %s in a shell, then /configure here. "
            "OAuth callback input stays outside session history.\n", s->name);
    }
    if (!strcmp(verb, "logout")) {
        if (client->busy) {
            return snag_buf_printf(report, "MCP operation active; retry logout when idle.\n");
        }
        int rc = snag_mcp_logout(client, s, pump, opaque);
        return snag_buf_printf(report, "%s: %s\n", s->name,
            *s->error ? s->error : rc ? "logout failed" : "logged out");
    }
    if (!strcmp(verb, "tools")) {
        char *tools = json_dumps(s->catalog ? s->catalog : json_null(), JSON_INDENT(2));
        char *filtered = json_dumps(s->filtered ? s->filtered : json_null(), JSON_INDENT(2));
        int rc = tools && filtered ? snag_buf_printf(report, "%s tools:\n%s\nFiltered:\n%s\n",
            s->name, tools, filtered) : -1;
        free(tools);
        free(filtered);
        return rc;
    }
    if (!strcmp(verb, "status")) {
        const char *scope = snag_mcp_string(s->auth, "scope");
        const char *identity = snag_mcp_string(s->config, "identity");
        const char *team = snag_mcp_string(json_object_get(s->auth, "team"), "id");
        const char *user = snag_mcp_string(json_object_get(s->auth, "authed_user"), "id");
        uint64_t expiry = (uint64_t)json_integer_value(json_object_get(s->auth, "expires_at_ms"));
        return snag_buf_printf(report,
            "server: %s\nendpoint: %s\ntransport: Streamable HTTP\nprotocol: %s%s\n"
            "identity label: %s\nauthenticated workspace: %s\nauthenticated user: %s\n"
            "auth: %s\nexpires (Unix ms): %llu\nscopes: %s\ntools: %zu\n"
            "generation: %llu\nfiltered: %zu\nnotifications: %s\n"
            "last contact (Unix ms): %llu\nerror: %s\n",
            s->name, snag_mcp_string(s->config, "url"), s->version,
            s->legacy ? " initialized" : " stateless",
            *identity ? identity : "not configured", *team ? team : "not disclosed by grant",
            *user ? user : "not disclosed by grant",
            s->auth_failed ? "login/configuration required" : !s->auth ? "no OAuth credential" :
                expiry && expiry <= snag_time_ms() ? "expired; refresh needed" : "OAuth loaded",
            (unsigned long long)expiry, *scope ? scope : "server does not disclose scopes",
            json_array_size(s->catalog), (unsigned long long)s->generation,
            json_object_size(s->filtered), s->watch ? "listening" : "refresh at turn boundary",
            (unsigned long long)s->contact_ms, *s->error ? s->error : "none");
    }
    return snag_buf_printf(report, "usage: /mcp list|status|tools|reload|login|logout SERVER; "
        "pending [DIGEST]; approve|deny DIGEST\n");
}
