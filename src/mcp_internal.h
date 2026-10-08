/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_MCP_INTERNAL_H
#define SNAJPAGENT_MCP_INTERNAL_H

#include "mcp.h"
#include "sse.h"

#include <curl/curl.h>

#define SNAG_MCP_CURRENT "2026-07-28"
#define SNAG_MCP_LEGACY "2025-11-25"

struct snag_mcp_server {
    char *name;
    json_t *config;
    json_t *auth;
    json_t *metadata;
    json_t *catalog;
    json_t *filtered;
    char *secret;
    char *session;
    struct snag_mcp_watch *watch;
    char version[16];
    char binding[SNAG_SHA256_HEX_LEN + 1u];
    char error[256];
    uint64_t request_id, generation, contact_ms, expires_ms, token_deadline_ms;
    uint64_t operation_deadline_ms;
    bool legacy, dirty, enabled, auth_failed, force_refresh;
};
struct snag_mcp {
    int root_fd;
    const struct snag_session *session;
    struct snag_mcp *replacement;
    struct snag_mcp_server *servers;
    size_t count;
    char turn_id[SNAG_ID_HEX_LEN + 1u];
    json_t *tools, *routes, *pending, *approved, *inspected, *uncertain, *secrets;
    bool busy;
};
struct snag_mcp_http {
    struct snag_buf body;
    json_t *reply;
    char *session, *challenge, *content_type;
    long status;
    CURLcode code;
    bool sent, cancelled, list_changed;
};

const char *snag_mcp_string(const json_t *object, const char *key);
char *snag_mcp_slice(const char *, size_t);
bool snag_mcp_url_valid(const char *url);
char *snag_mcp_url_part(const char *url, CURLUPart part);
void snag_mcp_http_free(struct snag_mcp_http *response);
int snag_mcp_http(struct snag_mcp_server *server, const char *url, const char *method,
    const char *body, struct curl_slist *headers, json_int_t id, snag_mcp_pump_fn pump,
    void *opaque, struct snag_mcp_http *response);
void snag_mcp_watch_start(struct snag_mcp_server *);
void snag_mcp_watch_close(struct snag_mcp_server *);
void snag_mcp_end_session(struct snag_mcp_server *, uint64_t);
int snag_mcp_rpc(struct snag_mcp_server *server, const char *method, json_t *params,
    struct curl_slist *extra, snag_mcp_pump_fn pump, void *opaque, json_t **result,
    bool *sent);
bool snag_mcp_contains_secret(struct snag_mcp_server *, const json_t *);
int snag_mcp_catalog(struct snag_mcp_server *server, snag_mcp_pump_fn pump, void *opaque);
int snag_mcp_snapshot(struct snag_mcp *, int, const char *, bool);
int snag_mcp_initialize(struct snag_mcp_server *, snag_mcp_pump_fn, void *);
int snag_mcp_project(struct snag_mcp *client, bool read_only);
int snag_mcp_parameter_headers(const json_t *schema, const json_t *arguments,
    struct curl_slist **headers);
int snag_mcp_header(struct curl_slist **headers, const char *name, const char *value);
int snag_mcp_encoded_header(struct curl_slist **headers, const char *name, const char *value);
int snag_mcp_protect(struct snag_mcp *, const struct snag_mcp_server *);
int snag_mcp_auth_load(struct snag_mcp *client, struct snag_mcp_server *server);
int snag_mcp_auth_refresh(struct snag_mcp *client, struct snag_mcp_server *server,
    snag_mcp_pump_fn pump, void *opaque);
int snag_mcp_login(struct snag_mcp *client, struct snag_mcp_server *server,
    snag_mcp_pump_fn pump, void *opaque);
int snag_mcp_logout(struct snag_mcp *client, struct snag_mcp_server *server,
    snag_mcp_pump_fn pump, void *opaque);
int snag_mcp_auth_save(struct snag_mcp *client, struct snag_mcp_server *server);

#endif
