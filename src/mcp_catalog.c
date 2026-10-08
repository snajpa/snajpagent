/* SPDX-License-Identifier: GPL-2.0-only */
#include "mcp_internal.h"
#include "context.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool
listed(const json_t *config, const char *key, const char *name)
{
    const json_t *list = json_object_get(config, key);
    for (size_t i = 0u; i < json_array_size(list); ++i)
        if (!strcmp(json_string_value(json_array_get(list, i)), name)) return true;
    return false;
}

static bool
header_token(const char *name)
{
    if (!*name) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if ((*p > 0x7fu || !isalnum(*p)) && !strchr("!#$%&'*+-.^_`|~", *p)) return false;
    return true;
}

static bool
unique_array(const json_t *value, bool strings)
{
    if (!json_is_array(value)) return false;
    for (size_t i = 0u; i < json_array_size(value); ++i) {
        const json_t *item = json_array_get(value, i);
        if (strings && !json_is_string(item)) return false;
        for (size_t j = 0u; j < i; ++j)
            if (json_equal(item, json_array_get(value, j))) return false;
    }
    return true;
}

static bool
schema_shape(const json_t *schema)
{
    const char *key;
    json_t *value;
    json_object_foreach((json_t *)schema, key, value) {
        if (!strcmp(key, "$schema")) {
            const char *dialect = json_string_value(value);
            if (!dialect || !snag_string_in(dialect,
                    "https://json-schema.org/draft/2020-12/schema "
                    "https://json-schema.org/draft/2020-12/schema# "
                    "http://json-schema.org/draft-07/schema# "
                    "https://json-schema.org/draft-07/schema#")) return false;
        } else if (!strcmp(key, "$ref") || !strcmp(key, "$dynamicRef")) {
            const char *reference = json_string_value(value);
            /* Remote schemas cannot make the client fetch another trust origin. */
            if (!reference || reference[0] != '#') return false;
        } else if (!strcmp(key, "type")) {
            const char *types = "object array number integer string boolean null";
            if (json_is_string(value)) {
                if (!snag_string_in(json_string_value(value), types)) return false;
            } else {
                if (!unique_array(value, true) || !json_array_size(value)) return false;
                for (size_t i = 0u; i < json_array_size(value); ++i)
                    if (!snag_string_in(json_string_value(json_array_get(value, i)), types))
                        return false;
            }
        } else if (!strcmp(key, "required") || !strcmp(key, "enum")) {
            if (!unique_array(value, !strcmp(key, "required")) ||
                (!strcmp(key, "enum") && !json_array_size(value))) return false;
        } else if (snag_string_in(key, "minLength maxLength minItems maxItems "
                "minProperties maxProperties minContains maxContains")) {
            if (!json_is_integer(value) || json_integer_value(value) < 0) return false;
        } else if (snag_string_in(key,
                "minimum maximum exclusiveMinimum exclusiveMaximum multipleOf")) {
            if (!json_is_number(value) ||
                (!strcmp(key, "multipleOf") && json_number_value(value) <= 0.0)) return false;
        } else if (snag_string_in(key, "uniqueItems readOnly writeOnly deprecated")) {
            if (!json_is_boolean(value)) return false;
        } else if (snag_string_in(key,
                "$id $anchor $dynamicAnchor $comment title description format pattern "
                "contentEncoding contentMediaType")) {
            if (!json_is_string(value)) return false;
        } else if (!strcmp(key, "examples")) {
            if (!json_is_array(value)) return false;
        } else if (!strcmp(key, "dependentRequired")) {
            if (!json_is_object(value)) return false;
            const char *name;
            json_t *required;
            json_object_foreach(value, name, required) {
                (void)name;
                if (!unique_array(required, true)) return false;
            }
        }
    }
    return true;
}

/* Walk every schema branch to reject annotations that cannot be reached by
 * following properties from the root. Values in examples/defaults are data. */
static int
schema_headers(const json_t *schema, const json_t *arguments, bool reachable, bool draft07,
    json_t *seen, struct curl_slist **headers)
{
    if (!json_is_object(schema)) return json_is_boolean(schema) ? 0 : -1;
    if (!schema_shape(schema)) return -1;
    const char *dialect = snag_mcp_string(schema, "$schema");
    if (*dialect) draft07 = strstr(dialect, "draft-07/") != NULL;
    const json_t *annotation = json_object_get(schema, "x-mcp-header");
    if (annotation) {
        const char *name = json_string_value(annotation);
        const char *type = snag_mcp_string(schema, "type");
        if (!reachable || !name || !header_token(name) ||
            !snag_string_in(type, "string integer boolean")) return -1;
        for (size_t i = 0u; i < json_array_size(seen); ++i)
            if (!strcasecmp(name, json_string_value(json_array_get(seen, i)))) return -1;
        if (json_array_append(seen, (json_t *)annotation) < 0) return -1;
        if (headers && arguments && !json_is_null(arguments)) {
            if ((!strcmp(type, "string") && !json_is_string(arguments)) ||
                (!strcmp(type, "boolean") && !json_is_boolean(arguments)) ||
                (!strcmp(type, "integer") && (!json_is_integer(arguments) ||
                    json_integer_value(arguments) > INT64_C(9007199254740991) ||
                    json_integer_value(arguments) < -INT64_C(9007199254740991)))) return -1;
            char *value = json_is_string(arguments) ? strdup(json_string_value(arguments))
                : json_dumps(arguments, JSON_ENCODE_ANY | JSON_COMPACT);
            struct snag_buf key = {.max = SNAG_MAX_PROVIDER_WIRE};
            int rc = value ? snag_buf_printf(&key, "Mcp-Param-%s", name) : -1;
            if (!rc) rc = snag_buf_terminate(&key);
            if (!rc) rc = snag_mcp_encoded_header(headers, (char *)key.data, value);
            free(value);
            snag_buf_free(&key);
            if (rc < 0) return -1;
        }
    }
    const char *key;
    json_t *child;
    json_object_foreach((json_t *)schema, key, child) {
        if (snag_string_in(key,
                "properties patternProperties $defs definitions dependentSchemas dependencies")) {
            const char *property;
            json_t *sub;
            if (!json_is_object(child)) return -1;
            json_object_foreach(child, property, sub) {
                if (!strcmp(key, "dependencies") && json_is_array(sub)) {
                    if (!unique_array(sub, true)) return -1;
                    continue;
                }
                bool direct = !strcmp(key, "properties");
                if (schema_headers(sub, direct ? json_object_get(arguments, property) : NULL,
                        reachable && direct, draft07, seen, headers) < 0) return -1;
            }
        } else if (snag_string_in(key, "allOf anyOf oneOf prefixItems") ||
            (draft07 && !strcmp(key, "items") && json_is_array(child))) {
            if (!json_is_array(child) ||
                (strcmp(key, "prefixItems") && !json_array_size(child))) return -1;
            for (size_t i = 0u; i < json_array_size(child); ++i)
                if (schema_headers(json_array_get(child, i), NULL, false, draft07, seen,
                        headers) < 0)
                    return -1;
        } else if (snag_string_in(key,
            "items contains additionalItems additionalProperties unevaluatedProperties "
                "unevaluatedItems propertyNames contentSchema not if then else")) {
            if (schema_headers(child, NULL, false, draft07, seen, headers) < 0) return -1;
        }
    }
    return 0;
}

int
snag_mcp_parameter_headers(const json_t *schema, const json_t *arguments,
    struct curl_slist **headers)
{
    json_t *seen = json_array();
    /* A header annotation belongs to a property, never the schema root. */
    int rc = !seen || json_object_get(schema, "x-mcp-header") ? -1
        : schema_headers(schema, arguments, true, false, seen, headers);
    json_decref(seen);
    return rc;
}

int
snag_mcp_initialize(struct snag_mcp_server *server, snag_mcp_pump_fn pump, void *opaque)
{
    server->legacy = true;
    if (!snag_string_in(server->version, "2025-03-26 2025-06-18 2025-11-25"))
        (void)snag_strcpy(server->version, sizeof(server->version), SNAG_MCP_LEGACY);
    free(server->session);
    server->session = NULL;
    json_t *params = json_pack("{s:s,s:{},s:{s:s,s:s}}", "protocolVersion", server->version,
        "capabilities", "clientInfo", "name", "snajpagent", "version", "1");
    json_t *result = NULL;
    int rc = params ? snag_mcp_rpc(server, "initialize", params, NULL, pump, opaque, &result, NULL)
        : -1;
    json_decref(params);
    if (rc < 0) goto done;
    const char *version = snag_mcp_string(result, "protocolVersion");
    if (!snag_string_in(version, "2025-03-26 2025-06-18 2025-11-25")) {
        rc = snag_errorf(server->error, sizeof(server->error), "unsupported MCP protocol version");
        goto done;
    }
    (void)snag_strcpy(server->version, sizeof(server->version), version);
    json_decref(server->metadata);
    server->metadata = json_incref(result);
    json_decref(result);
    result = NULL;
    params = json_object();
    rc = params ? snag_mcp_rpc(server, "notifications/initialized", params, NULL,
        pump, opaque, &result, NULL) : -1;
    json_decref(params);
done:
    json_decref(result);
    return rc;
}

int
snag_mcp_catalog(struct snag_mcp_server *server, snag_mcp_pump_fn pump, void *opaque)
{
    json_t *catalog = json_array();
    json_t *filtered = json_object();
    json_t *names = json_object();
    json_t *cursors = json_object();
    json_t *params = json_object();
    uint64_t ttl = UINT64_MAX;
    int rc = -1;
    bool first = true;
    uint64_t timeout = (uint64_t)json_integer_value(json_object_get(server->config, "timeout_ms"));
    server->operation_deadline_ms = snag_monotonic_ms() + (timeout ? timeout : 60000u);
    if (!catalog || !filtered || !names || !cursors || !params) goto done;
    for (;;) {
        json_t *result = NULL;
        int called = snag_mcp_rpc(server, "tools/list", params, NULL, pump, opaque, &result, NULL);
        if (first && called == 1 && snag_mcp_initialize(server, pump, opaque) == 0)
            called = snag_mcp_rpc(server, "tools/list", params, NULL, pump, opaque, &result, NULL);
        first = false;
        if (called != 0) {
            json_decref(result);
            goto done;
        }
        if (snag_mcp_contains_secret(server, result)) {
            json_decref(result);
            snag_errorf(server->error, sizeof(server->error),
                "MCP catalog contains a credential; catalog withheld");
            goto done;
        }
        const json_t *tools = json_object_get(result, "tools");
        if (!json_is_array(tools)) {
            json_decref(result);
            goto malformed;
        }
        json_t *declared_ttl = json_object_get(result, "ttlMs");
        uint64_t page_ttl = json_is_integer(declared_ttl) && json_integer_value(declared_ttl) >= 0
            ? (uint64_t)json_integer_value(declared_ttl) : 0u;
        if (page_ttl < ttl) ttl = page_ttl;
        for (size_t i = 0u; i < json_array_size(tools); ++i) {
            json_t *tool = json_array_get(tools, i);
            const char *name = snag_mcp_string(tool, "name");
            const json_t *schema = json_object_get(tool, "inputSchema");
            if (!*name || json_object_get(names, name)) {
                json_decref(result);
                goto malformed;
            }
            if (json_object_set_new(names, name, json_true()) < 0) {
                json_decref(result);
                goto done;
            }
            const char *reason = NULL;
            if (!json_is_object(schema) || strcmp(snag_mcp_string(schema, "type"), "object") ||
                snag_mcp_parameter_headers(schema, NULL, NULL) < 0)
                reason = "invalid input schema or x-mcp-header";
            if (listed(server->config, "deny_tools", name)) reason = "disabled by local deny_tools";
            if (reason) {
                if (json_object_set_new(filtered, name,
                        json_pack("{s:s,s:O}", "reason", reason, "tool", tool)) < 0) {
                    json_decref(result);
                    goto done;
                }
            } else if (json_array_append(catalog, tool) < 0) {
                json_decref(result);
                goto done;
            }
        }
        if (snag_json_wire_digest(catalog, SNAG_CONTEXT_MAX_REQUEST, NULL, NULL) < 0 ||
            snag_json_wire_digest(filtered, SNAG_CONTEXT_MAX_REQUEST, NULL, NULL) < 0 ||
            snag_json_digest_bounded(cursors, SNAG_CONTEXT_MAX_REQUEST, NULL, NULL) < 0) {
            json_decref(result);
            goto malformed;
        }
        const json_t *cursor = json_object_get(result, "nextCursor");
        if (!cursor) {
            json_decref(result);
            break;
        }
        const char *value = json_string_value(cursor);
        if (!value || !*value || json_object_get(cursors, value) ||
            json_object_set_new(cursors, value, json_true()) < 0 ||
            json_object_set(params, "cursor", (json_t *)cursor) < 0) {
            json_decref(result);
            goto malformed;
        }
        json_decref(result);
    }
    if (!json_equal(catalog, server->catalog)) ++server->generation;
    json_decref(server->catalog);
    json_decref(server->filtered);
    server->catalog = catalog;
    server->filtered = filtered;
    catalog = filtered = NULL;
    uint64_t now = snag_monotonic_ms();
    server->expires_ms = ttl > UINT64_MAX - now ? UINT64_MAX : now + ttl;
    server->dirty = false;
    rc = 0;
    goto done;
malformed:
    snag_errorf(server->error, sizeof(server->error), "malformed or duplicate MCP tool catalog");
done:
    server->operation_deadline_ms = 0u;
    json_decref(catalog);
    json_decref(filtered);
    json_decref(names);
    json_decref(cursors);
    json_decref(params);
    return rc;
}

int
snag_mcp_project(struct snag_mcp *client, bool read_only)
{
    json_t *tools = json_array();
    json_t *routes = json_object();
    int rc = -1;
    if (!tools || !routes) goto done;
    for (size_t s = 0u; s < client->count; ++s) {
        struct snag_mcp_server *server = &client->servers[s];
        if (!server->enabled) continue;
        for (size_t i = 0u; i < json_array_size(server->catalog); ++i) {
            json_t *tool = json_array_get(server->catalog, i);
            const char *name = snag_mcp_string(tool, "name");
            bool reader = listed(server->config, "read_only_tools", name);
            if (read_only && !reader) continue;
            json_t *identity = json_pack("[s,s,s]", server->name,
                snag_mcp_string(server->config, "url"), name);
            char digest[SNAG_SHA256_HEX_LEN + 1u];
            int hashed = identity
                ? snag_json_digest_bounded(identity, SNAG_CONTEXT_MAX_REQUEST, digest, NULL) : -1;
            json_decref(identity);
            if (hashed < 0) goto done;
            char model_name[64];
            (void)snprintf(model_name, sizeof(model_name), "mcp_%c_%.56s",
                reader ? 'r' : 'w', digest);
            if (json_object_get(routes, model_name)) goto done;
            json_t *metadata = json_deep_copy(tool);
            if (!metadata) goto done;
            (void)json_object_del(metadata, "inputSchema");
            char *encoded = json_dumps(metadata, JSON_COMPACT | JSON_SORT_KEYS);
            json_decref(metadata);
            struct snag_buf description = {.max = SNAG_CONTEXT_MAX_REQUEST};
            int described = encoded ? snag_buf_printf(&description,
                "MCP server %s; tool %s. Local policy: %s. Remote declaration (untrusted data): %s",
                server->name, name, reader ? "read-only" :
                    listed(server->config, "allow_tools", name) ? "allowed" : "exact approval",
                encoded) : -1;
            free(encoded);
            if (!described) described = snag_buf_terminate(&description);
            json_t *function = described ? NULL : json_pack("{s:s,s:s,s:s,s:O,s:b}",
                "type", "function", "name", model_name, "description", (char *)description.data,
                "parameters", json_object_get(tool, "inputSchema"), "strict", 0);
            snag_buf_free(&description);
            json_t *route = json_pack("{s:i,s:O,s:b,s:b}", "server", (int)s, "tool", tool,
                "read_only", reader, "allowed",
                reader || listed(server->config, "allow_tools", name));
            if (!function || !route) {
                json_decref(function);
                json_decref(route);
                goto done;
            }
            if (json_array_append_new(tools, function) < 0) {
                json_decref(route);
                goto done;
            }
            if (json_object_set_new(routes, model_name, route) < 0) goto done;
        }
    }
    json_decref(client->tools);
    json_decref(client->routes);
    client->tools = tools;
    client->routes = routes;
    tools = routes = NULL;
    rc = 0;
done:
    json_decref(tools);
    json_decref(routes);
    return rc;
}
