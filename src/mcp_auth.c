/* SPDX-License-Identifier: GPL-2.0-only */
#include "mcp_internal.h"
#include "auth.h"
#include "fs.h"
#include "net.h"
#include "term_host.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static int
private_fd(int fd, bool directory)
{
    snag_file_info st;
    struct snag_file_privacy privacy;
    if (snag_fstat(fd, &st) < 0 || snag_fd_privacy(fd, &privacy) < 0 ||
        !privacy.effective_owner || !privacy.private_access ||
        (directory ? !S_ISDIR(st.st_mode) : (!S_ISREG(st.st_mode) || st.st_nlink != 1u)))
        return snag_errno(EACCES);
    return 0;
}

static int
auth_directory(struct snag_mcp *client, bool create)
{
    if (private_fd(client->root_fd, true) < 0) return -1;
    if (create && snag_mkdir_private_at(client->root_fd, "mcp-auth") < 0 && errno != EEXIST)
        return -1;
    int fd = snag_open_read_security_at(client->root_fd, "mcp-auth", true);
    if (fd >= 0 && private_fd(fd, true) < 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

int
snag_mcp_auth_save(struct snag_mcp *client, struct snag_mcp_server *server)
{
    int dir = auth_directory(client, true);
    if (dir < 0) return -1;
    char id[SNAG_ID_HEX_LEN + 1u];
    char temp[SNAG_ID_HEX_LEN + 5u] = {0};
    struct snag_buf text = {.max = SNAG_CONFIG_FILE_MAX};
    int fd = -1;
    int rc = -1;
    if (json_object_set_new(server->auth, "binding", json_string(server->binding)) < 0 ||
        snag_json_canonical(server->auth, &text) < 0 || snag_random_id(id) < 0) goto done;
    (void)snprintf(temp, sizeof(temp), "%s.tmp", id);
    fd = snag_create_private_at(dir, temp, true);
    if (fd < 0 || private_fd(fd, false) < 0 || snag_write_full(fd, text.data, text.len) < 0 ||
        snag_fsync(fd) < 0 || snag_rename_at(dir, temp, dir, server->binding) < 0) goto done;
    temp[0] = '\0';
    rc = snag_sync_dir(dir);
done:
    if (fd >= 0) (void)close(fd);
    if (*temp) (void)snag_unlink_at(dir, temp, false);
    (void)close(dir);
    if (text.data) snag_secret_clear(text.data, text.len);
    snag_buf_free(&text);
    return rc;
}

static void
expiry_deadline(struct snag_mcp_server *server)
{
    uint64_t now = snag_time_ms();
    uint64_t expires = (uint64_t)json_integer_value(json_object_get(server->auth, "expires_at_ms"));
    uint64_t remaining = expires > now ? expires - now : 0u;
    server->token_deadline_ms = expires ? snag_monotonic_ms() + remaining : 0u;
}

int
snag_mcp_auth_load(struct snag_mcp *client, struct snag_mcp_server *server)
{
    int dir = auth_directory(client, false);
    if (dir < 0) return errno == ENOENT ? 0 : -1;
    int fd = snag_open_read_security_at(dir, server->binding, false);
    (void)close(dir);
    if (fd < 0) {
        if (errno != ENOENT) return -1;
        snag_auth_json_free(server->auth);
        server->auth = NULL;
        return 0;
    }
    struct snag_buf text = {.max = SNAG_CONFIG_FILE_MAX};
    json_t *value = NULL;
    int rc = -1;
    if (private_fd(fd, false) < 0 || snag_buf_read(&text, fd) < 0) goto done;
    value = snag_json_load_strict(text.data, text.len, text.max, NULL, 0u);
    const char *token = snag_mcp_string(value, "access_token");
    if (strcmp(snag_mcp_string(value, "binding"), server->binding) || !*token ||
        strlen(token) > SNAG_SECRET_MAX ||
        !snag_mcp_url_valid(snag_mcp_string(value, "token_endpoint")) ||
        strcmp(snag_mcp_string(value, "issuer"), snag_mcp_string(server->config, "issuer")))
        goto done;
    const char *refresh = snag_mcp_string(value, "refresh_token");
    json_t *issued = json_object_get(value, "issued_at_ms");
    json_t *expires = json_object_get(value, "expires_at_ms");
    if (strlen(refresh) > SNAG_SECRET_MAX || !json_is_integer(issued) ||
        !json_is_integer(expires) || json_integer_value(issued) < 0 ||
        json_integer_value(expires) < 0 ||
        !snag_hex_is_lower(snag_mcp_string(value, "grant_id"), SNAG_ID_HEX_LEN)) goto done;
    for (const unsigned char *p = (const unsigned char *)token; *p; ++p)
        if (*p <= 0x20u || *p > 0x7eu) goto done;
    snag_auth_json_free(server->auth);
    server->auth = value;
    value = NULL;
    expiry_deadline(server);
    rc = snag_mcp_protect(client, server);
done:
    (void)close(fd);
    snag_auth_json_free(value);
    if (text.data) snag_secret_clear(text.data, text.len);
    snag_buf_free(&text);
    if (rc < 0)
        snag_errorf(server->error, sizeof(server->error), "unsafe or invalid MCP credential file");
    return rc;
}

static int
auth_lock(struct snag_mcp *client, struct snag_mcp_server *server,
    snag_mcp_pump_fn pump, void *opaque)
{
    int dir = auth_directory(client, true);
    if (dir < 0) return -1;
    char path[SNAG_SHA256_HEX_LEN + 6u];
    (void)snprintf(path, sizeof(path), "%s.lock", server->binding);
    int fd = snag_create_private_at(dir, path, false);
    (void)close(dir);
    if (fd < 0) return -1;
    uint64_t deadline = snag_monotonic_ms() + 30000u;
    if (private_fd(fd, false) < 0) goto fail;
    while (snag_lock_file(fd, false) < 0) {
        if ((errno != EAGAIN && errno != EACCES && errno != EINTR) ||
            snag_monotonic_ms() >= deadline ||
            ((pump && pump(opaque, 0u) != 0) || snag_sleep_ms(20u) < 0)) goto fail;
    }
    return fd;
fail:
    (void)close(fd);
    return -1;
}

static int
form_field(struct snag_buf *form, const char *name, const char *value)
{
    char *escaped = curl_easy_escape(NULL, value, 0);
    int rc = escaped ? snag_buf_printf(form, "%s%s=%s", form->len ? "&" : "", name, escaped) : -1;
    curl_free(escaped);
    return rc;
}

static int
json_http(struct snag_mcp_server *server, const char *url, const char *body,
    struct curl_slist *headers, snag_mcp_pump_fn pump, void *opaque, json_t **result)
{
    struct snag_mcp_http response = {0};
    *result = NULL;
    int rc = snag_mcp_http(server, url, body ? "POST" : "GET", body, headers, -1,
        pump, opaque, &response);
    if (rc < 0) goto done;
    if (response.status != 200L) {
        json_t *failure = snag_json_load_strict(response.body.data, response.body.len,
            SNAG_CONFIG_FILE_MAX, NULL, 0u);
        const char *code = snag_mcp_string(failure, "error");
        bool rejected = snag_string_in(code,
            "invalid_grant invalid_client invalid_scope unauthorized_client access_denied");
        if (rejected) {
            rc = snag_errorf(server->error, sizeof(server->error),
                "OAuth rejected (%s); login/configuration required", code);
            server->auth_failed =
                !strcmp(code, "invalid_grant") || !strcmp(code, "invalid_client");
        } else {
            rc = snag_errorf(server->error, sizeof(server->error),
                "OAuth HTTP %ld", response.status);
        }
        snag_auth_json_free(failure);
        goto done;
    }
    *result = snag_json_load_strict(response.body.data, response.body.len,
        SNAG_CONFIG_FILE_MAX, NULL, 0u);
    if (!json_is_object(*result)) {
        json_decref(*result);
        *result = NULL;
        rc = snag_errorf(server->error, sizeof(server->error), "invalid OAuth metadata/response");
    }
done:
    snag_mcp_http_free(&response);
    return rc;
}

static char *
well_known(const char *url, const char *name, bool append, bool root)
{
    CURLU *parsed = curl_url();
    char *path = NULL;
    char *joined = NULL;
    char *copy = NULL;
    struct snag_buf target = {.max = SNAG_CONFIG_URL_MAX};
    if (!parsed || curl_url_set(parsed, CURLUPART_URL, url, 0u) != CURLUE_OK ||
        curl_url_get(parsed, CURLUPART_PATH, &path, 0u) != CURLUE_OK) goto done;
    size_t len = strlen(path);
    while (len && path[len - 1u] == '/') path[--len] = '\0';
    if (snag_buf_printf(&target, "%s/.well-known/%s%s", append && !root ? path : "", name,
            !append && !root ? path : "") < 0 || snag_buf_terminate(&target) < 0 ||
        curl_url_set(parsed, CURLUPART_PATH, (char *)target.data, 0u) != CURLUE_OK ||
        curl_url_set(parsed, CURLUPART_QUERY, NULL, 0u) != CURLUE_OK ||
        curl_url_get(parsed, CURLUPART_URL, &joined, 0u) != CURLUE_OK) goto done;
    copy = strdup(joined);
done:
    curl_free(path);
    curl_free(joined);
    curl_url_cleanup(parsed);
    snag_buf_free(&target);
    return copy;
}

static char *
challenge_parameter(const char *header, const char *key)
{
    if (!header) return NULL;
    const char *p = header;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        const char *begin = p;
        while (*p && *p != '=' && *p != ' ' && *p != ',') ++p;
        size_t n = (size_t)(p - begin);
        while (*p == ' ') ++p;
        if (*p != '=') {
            if (*p) ++p;
            continue;
        }
        ++p;
        while (*p == ' ') ++p;
        bool quoted = *p == '"';
        if (quoted) ++p;
        const char *value = p;
        while (*p && (quoted ? *p != '"' : *p != ',' && *p != ' ')) {
            if (*p == '\\' || (unsigned char)*p < 0x20u) return NULL;
            ++p;
        }
        if (strlen(key) == n && !strncasecmp(begin, key, n))
            return snag_mcp_slice(value, (size_t)(p - value));
        if (*p) ++p;
    }
    return NULL;
}

static bool
array_has(const json_t *array, const char *value)
{
    for (size_t i = 0u; i < json_array_size(array); ++i) {
        const char *item = json_string_value(json_array_get(array, i));
        if (item && !strcmp(item, value)) return true;
    }
    return false;
}

static int
discover(struct snag_mcp_server *server, snag_mcp_pump_fn pump, void *opaque,
    json_t **metadata, char **scope)
{
    const char *url = snag_mcp_string(server->config, "url");
    const char *resource = snag_mcp_string(server->config, "resource");
    const char *issuer = snag_mcp_string(server->config, "issuer");
    if (!*resource) resource = url;
    if (!snag_mcp_url_valid(issuer) || !*snag_mcp_string(server->config, "client_id"))
        return snag_errorf(server->error, sizeof(server->error),
            "configure client_id and its registered issuer before MCP login");
    struct snag_mcp_http challenge = {0};
    json_t *protected = NULL;
    *metadata = NULL;
    *scope = NULL;
    int rc = snag_mcp_http(server, url, "GET", NULL, NULL, -1, pump, opaque, &challenge);
    if (rc < 0) goto done;
    char *location = challenge_parameter(challenge.challenge, "resource_metadata");
    const char *configured_scope = snag_mcp_string(server->config, "scope");
    *scope = *configured_scope ? strdup(configured_scope)
        : challenge_parameter(challenge.challenge, "scope");
    if (!*scope) *scope = strdup("");
    if (!*scope) goto done;
    if (location) {
        rc = json_http(server, location, NULL, NULL, pump, opaque, &protected);
        free(location);
        if (rc < 0) goto done;
    } else {
        for (int i = 0; i < 2 && !protected; ++i) {
            location = well_known(resource, "oauth-protected-resource", false, i == 1);
            if (location) (void)json_http(server, location, NULL, NULL, pump, opaque, &protected);
            free(location);
        }
    }
    if (!protected || strcmp(snag_mcp_string(protected, "resource"), resource) ||
        !array_has(json_object_get(protected, "authorization_servers"), issuer)) {
        rc = snag_errorf(server->error, sizeof(server->error), "OAuth resource/issuer mismatch");
        goto done;
    }
    for (int i = 0; i < 3 && !*metadata; ++i) {
        location = well_known(issuer, i ? "openid-configuration" : "oauth-authorization-server",
            i == 2, false);
        if (location) (void)json_http(server, location, NULL, NULL, pump, opaque, metadata);
        free(location);
    }
    if (!*metadata || strcmp(snag_mcp_string(*metadata, "issuer"), issuer) ||
        !snag_mcp_url_valid(snag_mcp_string(*metadata, "authorization_endpoint")) ||
        !snag_mcp_url_valid(snag_mcp_string(*metadata, "token_endpoint")) ||
        !array_has(json_object_get(*metadata, "code_challenge_methods_supported"), "S256")) {
        rc = snag_errorf(server->error, sizeof(server->error),
            "invalid OAuth issuer/endpoints or PKCE S256 unavailable");
        goto done;
    }
    const json_t *methods = json_object_get(*metadata, "token_endpoint_auth_methods_supported");
    bool confidential = server->secret && *server->secret;
    if (methods && ((!confidential && !array_has(methods, "none")) ||
        (confidential && !array_has(methods, "client_secret_basic") &&
            !array_has(methods, "client_secret_post")))) {
        rc = snag_errorf(server->error, sizeof(server->error),
            "OAuth client authentication does not match configured client_secret");
        goto done;
    }
    rc = 0;
done:
    json_decref(protected);
    snag_mcp_http_free(&challenge);
    return rc;
}

static int
client_authenticate(struct snag_mcp_server *server, const json_t *metadata,
    const char *method_field, struct snag_buf *form, struct curl_slist **headers)
{
    const char *client_id = snag_mcp_string(server->config, "client_id");
    if (form_field(form, "client_id", client_id) < 0) return -1;
    if (server->secret && *server->secret) {
        const json_t *methods = json_object_get(metadata, method_field);
        if (!methods || array_has(methods, "client_secret_basic")) {
            char *id = curl_easy_escape(NULL, client_id, 0);
            char *secret = curl_easy_escape(NULL, server->secret, 0);
            struct snag_buf pair = {.max = SNAG_CONFIG_FILE_MAX};
            struct snag_buf authorization = {.max = SNAG_CONFIG_FILE_MAX};
            int h = id && secret ? snag_buf_printf(&pair, "%s:%s", id, secret) : -1;
            if (!h) h = snag_buf_printf(&authorization, "Basic ");
            if (!h) h = snag_base64_append(&authorization, pair.data, pair.len);
            if (!h) h = snag_buf_terminate(&authorization);
            if (!h) h = snag_mcp_header(headers, "Authorization", (char *)authorization.data);
            if (secret) snag_secret_clear(secret, strlen(secret));
            curl_free(secret);
            curl_free(id);
            if (pair.data) snag_secret_clear(pair.data, pair.len);
            if (authorization.data) snag_secret_clear(authorization.data, authorization.len);
            snag_buf_free(&pair);
            snag_buf_free(&authorization);
            if (h < 0) return -1;
        } else if (array_has(methods, "client_secret_post")) {
            if (form_field(form, "client_secret", server->secret) < 0) return -1;
        } else {
            snag_errorf(server->error, sizeof(server->error),
                "unsupported OAuth client authentication");
            return -1;
        }
    }
    return 0;
}

static int
token_request(struct snag_mcp *client, struct snag_mcp_server *server,
    const json_t *metadata, struct snag_buf *form, snag_mcp_pump_fn pump, void *opaque)
{
    const char *resource = snag_mcp_string(server->config, "resource");
    if (!*resource) resource = snag_mcp_string(server->config, "url");
    struct curl_slist *headers = NULL;
    json_t *result = NULL;
    int rc = -1;
    if (snag_mcp_header(&headers, "Content-Type", "application/x-www-form-urlencoded") < 0 ||
        client_authenticate(server, metadata, "token_endpoint_auth_methods_supported",
            form, &headers) < 0 ||
        form_field(form, "resource", resource) < 0) goto done;
    if (snag_buf_terminate(form) < 0 || json_http(server,
            snag_mcp_string(metadata, "token_endpoint"), (char *)form->data, headers,
            pump, opaque, &result) < 0) goto done;
    const char *token = snag_mcp_string(result, "access_token");
    if (!*token || strlen(token) > SNAG_SECRET_MAX ||
        strcasecmp(snag_mcp_string(result, "token_type"), "Bearer") ||
        json_object_get(result, "error") || json_is_false(json_object_get(result, "ok"))) {
        snag_errorf(server->error, sizeof(server->error), "OAuth token rejected; login required");
        goto done;
    }
    for (const unsigned char *p = (const unsigned char *)token; *p; ++p)
        if (*p <= 0x20u || *p > 0x7eu) goto done;
    const char *audience = snag_mcp_string(result, "resource");
    if (*audience && strcmp(audience, resource)) {
        snag_errorf(server->error, sizeof(server->error), "OAuth token resource mismatch");
        goto done;
    }
    json_t *next = json_deep_copy(metadata);
    uint64_t now = snag_time_ms();
    json_t *seconds = json_object_get(result, "expires_in");
    json_int_t lifetime = json_integer_value(seconds);
    if (seconds && (!json_is_integer(seconds) || lifetime <= 0 ||
        (uint64_t)lifetime > (INT64_MAX - now) / 1000u)) {
        json_decref(next);
        snag_errorf(server->error, sizeof(server->error), "invalid OAuth token expiry");
        goto done;
    }
    if (!next || json_object_set_new(next, "access_token",
            json_deep_copy(json_object_get(result, "access_token"))) < 0 ||
        json_object_set_new(next, "issued_at_ms", json_integer((json_int_t)now)) < 0 ||
        json_object_set_new(next, "expires_at_ms",
            json_integer(seconds ? (json_int_t)(now + (uint64_t)lifetime * 1000u) : 0)) < 0) {
        json_decref(next);
        goto done;
    }
    const char *fields[] = {"refresh_token", "scope", "team", "authed_user"};
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        json_t *field = json_object_get(result, fields[i]);
        if (!field) field = json_object_get(metadata, fields[i]);
        if (field && json_object_set_new(next, fields[i], json_deep_copy(field)) < 0) {
            snag_auth_json_free(next);
            goto done;
        }
    }
    json_t *previous = server->auth;
    server->auth = next;
    if (snag_mcp_protect(client, server) < 0 || snag_mcp_auth_save(client, server) < 0) {
        snag_auth_json_free(server->auth);
        server->auth = previous;
        snag_errorf(server->error, sizeof(server->error), "cannot save private MCP credentials");
        goto done;
    }
    snag_auth_json_free(previous);
    snag_mcp_watch_close(server);
    expiry_deadline(server);
    server->dirty = true;
    server->force_refresh = false;
    server->auth_failed = false;
    server->error[0] = '\0';
    rc = 0;
done:
    snag_auth_json_free(result);
    curl_slist_free_all(headers);
    return rc;
}

static bool
token_due(const struct snag_mcp_server *server)
{
    uint64_t expires = (uint64_t)json_integer_value(json_object_get(server->auth, "expires_at_ms"));
    uint64_t issued = (uint64_t)json_integer_value(json_object_get(server->auth, "issued_at_ms"));
    uint64_t now = snag_time_ms();
    return expires && (now < issued || now + 30000u >= expires ||
        snag_monotonic_ms() + 30000u >= server->token_deadline_ms);
}

int
snag_mcp_auth_refresh(struct snag_mcp *client, struct snag_mcp_server *server,
    snag_mcp_pump_fn pump, void *opaque)
{
    if (server->auth_failed) return -1;
    if (!server->auth || (!server->force_refresh && !token_due(server))) return 0;
    int lock = auth_lock(client, server, pump, opaque);
    if (lock < 0) return -1;
    char grant[SNAG_ID_HEX_LEN + 1u];
    (void)snag_strcpy(grant, sizeof(grant), snag_mcp_string(server->auth, "grant_id"));
    int rc = snag_mcp_auth_load(client, server);
    if (rc < 0) goto done;
    if (!server->auth || strcmp(grant, snag_mcp_string(server->auth, "grant_id"))) {
        server->auth_failed = true;
        rc = snag_errorf(server->error, sizeof(server->error),
            "MCP login changed; use /configure to adopt the new grant");
        goto done;
    }
    if (!server->force_refresh && !token_due(server)) goto done;
    const char *refresh = snag_mcp_string(server->auth, "refresh_token");
    if (!*refresh) {
        server->auth_failed = true;
        rc = snag_errorf(server->error, sizeof(server->error),
            "MCP credential expired; login required");
        goto done;
    }
    struct snag_buf form = {.max = SNAG_CONFIG_FILE_MAX};
    rc = form_field(&form, "grant_type", "refresh_token");
    if (!rc) rc = form_field(&form, "refresh_token", refresh);
    if (!rc) rc = token_request(client, server, server->auth, &form, pump, opaque);
    if (form.data) snag_secret_clear(form.data, form.len);
    snag_buf_free(&form);
done:
    (void)close(lock);
    return rc;
}

static char *
base64url(const unsigned char *bytes, size_t length)
{
    struct snag_buf buffer = {.max = SNAG_CONFIG_FILE_MAX};
    if (snag_base64_append(&buffer, bytes, length) < 0) {
        snag_buf_free(&buffer);
        return NULL;
    }
    while (buffer.len && buffer.data[buffer.len - 1u] == '=') --buffer.len;
    for (size_t i = 0u; i < buffer.len; ++i) {
        if (buffer.data[i] == '+') buffer.data[i] = '-';
        if (buffer.data[i] == '/') buffer.data[i] = '_';
    }
    if (snag_buf_terminate(&buffer) < 0) {
        snag_buf_free(&buffer);
        return NULL;
    }
    return (char *)buffer.data;
}

static snag_socket
callback_listener(struct snag_mcp_server *server, char **redirect)
{
    const char *configured = snag_mcp_string(server->config, "redirect_uri");
    const char *url = *configured ? configured : "http://127.0.0.1:0/callback";
    char *host = snag_mcp_url_part(url, CURLUPART_HOST);
    char *scheme = snag_mcp_url_part(url, CURLUPART_SCHEME);
    char *port = snag_mcp_url_part(url, CURLUPART_PORT);
    char *query = snag_mcp_url_part(url, CURLUPART_QUERY);
    snag_socket fd = SNAG_SOCKET_INVALID;
    if (!snag_mcp_url_valid(url) || !host || !scheme || strcmp(scheme, "http") || query ||
        (strcmp(host, "127.0.0.1") && strcmp(host, "[::1]") && strcmp(host, "localhost"))) {
        goto done;
    }
    unsigned long number = port ? strtoul(port, NULL, 10) : 80u;
    if (number > 65535u) goto done;
    bool ipv6 = !strcmp(host, "[::1]");
    fd = snag_socket_open(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    if (fd == SNAG_SOCKET_INVALID) goto done;
    struct sockaddr_in a4 = {0};
    struct sockaddr_in6 a6 = {0};
    a4.sin_family = AF_INET;
    a4.sin_port = htons((unsigned short)number);
    a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a6.sin6_family = AF_INET6;
    a6.sin6_port = htons((unsigned short)number);
    a6.sin6_addr = in6addr_loopback;
    struct sockaddr *address = ipv6 ? (struct sockaddr *)&a6 : (struct sockaddr *)&a4;
#ifdef _WIN32
    int length = ipv6 ? sizeof(a6) : sizeof(a4);
#else
    socklen_t length = ipv6 ? sizeof(a6) : sizeof(a4);
#endif
    if (snag_socket_bind(fd, address, (size_t)length) < 0 || snag_socket_listen(fd, 1) < 0 ||
        getsockname(fd, address, &length) < 0) goto fail;
    CURLU *parsed = curl_url();
    char service[6];
    (void)snprintf(service, sizeof(service), "%u", ntohs(ipv6 ? a6.sin6_port : a4.sin_port));
    char *actual = NULL;
    if (!parsed || curl_url_set(parsed, CURLUPART_URL, url, 0u) != CURLUE_OK ||
        curl_url_set(parsed, CURLUPART_PORT, service, 0u) != CURLUE_OK ||
        curl_url_get(parsed, CURLUPART_URL, &actual, 0u) != CURLUE_OK) {
        curl_url_cleanup(parsed);
        goto fail;
    }
    *redirect = strdup(actual);
    curl_free(actual);
    curl_url_cleanup(parsed);
    if (!*redirect) goto fail;
    goto done;
fail:
    (void)snag_socket_close(fd);
    fd = SNAG_SOCKET_INVALID;
done:
    free(host);
    free(scheme);
    free(port);
    free(query);
    if (fd == SNAG_SOCKET_INVALID) {
        snag_errorf(server->error, sizeof(server->error),
            "cannot bind exact loopback OAuth callback");
    }
    return fd;
}

static char *
form_decode(const char *value, size_t length)
{
    char *copy = snag_mcp_slice(value, length);
    if (!copy) return NULL;
    for (size_t i = 0u; i < length; ++i)
        if (copy[i] == '+') copy[i] = ' ';
    int decoded_len = 0;
    char *decoded = curl_easy_unescape(NULL, copy, (int)length, &decoded_len);
    free(copy);
    char *result = decoded && decoded_len >= 0 && !memchr(decoded, 0, (size_t)decoded_len)
        ? snag_mcp_slice(decoded, (size_t)decoded_len) : NULL;
    if (decoded) snag_secret_clear(decoded, (size_t)decoded_len);
    curl_free(decoded);
    return result;
}

static char *
callback_code(const char *callback, const char *redirect, const char *state,
    const json_t *metadata)
{
    size_t prefix = strlen(redirect);
    if (strncmp(callback, redirect, prefix) || callback[prefix] != '?' || strchr(callback, '#'))
        return NULL;
    json_t *params = json_object();
    char *code = NULL;
    if (!params) return NULL;
    for (const char *p = callback + prefix + 1u; *p;) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *equals = memchr(p, '=', (size_t)(end - p));
        if (!equals) goto done;
        char *key = form_decode(p, (size_t)(equals - p));
        char *value = form_decode(equals + 1u, (size_t)(end - equals - 1u));
        bool valid = key && value && !json_object_get(params, key) &&
            json_object_set_new(params, key, json_string(value)) == 0;
        free(key);
        snag_secret_bytes_free(value);
        if (!valid) goto done;
        p = *end ? end + 1u : end;
    }
    const char *iss = snag_mcp_string(params, "iss");
    if (strcmp(snag_mcp_string(params, "state"), state) || json_object_get(params, "error") ||
        (*iss && strcmp(iss, snag_mcp_string(metadata, "issuer"))) ||
        (!*iss && json_is_true(json_object_get(metadata,
            "authorization_response_iss_parameter_supported")))) goto done;
    const char *value = snag_mcp_string(params, "code");
    if (*value) code = strdup(value);
done:
    snag_auth_json_free(params);
    return code;
}

static bool
callback_headers(const char *request, const char *redirect)
{
    char *host = snag_mcp_url_part(redirect, CURLUPART_HOST);
    char *port = snag_mcp_url_part(redirect, CURLUPART_PORT);
    struct snag_buf authority = {.max = SNAG_CONFIG_URL_MAX};
    struct snag_buf origin = {.max = SNAG_CONFIG_URL_MAX};
    bool valid = false;
    bool found = false;
    if (!host || snag_buf_printf(&authority, "%s%s%s", host, port ? ":" : "",
            port ? port : "") < 0 || snag_buf_terminate(&authority) < 0 ||
        snag_buf_printf(&origin, "http://%s", authority.data) < 0 ||
        snag_buf_terminate(&origin) < 0) goto done;
    const char *p = strstr(request, "\r\n");
    while (p && *(p += 2u)) {
        const char *end = strstr(p, "\r\n");
        if (!end || end == p) break;
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (!colon) goto done;
        const char *value = colon + 1u;
        while (value < end && (*value == ' ' || *value == '\t')) ++value;
        size_t size = (size_t)(end - value);
        if (colon - p == 4 && !strncasecmp(p, "Host", 4u)) {
            if (found || size != authority.len || strncasecmp(value,
                    (char *)authority.data, size)) goto done;
            found = true;
        } else if (colon - p == 6 && !strncasecmp(p, "Origin", 6u)) {
            if (size != origin.len || memcmp(value, origin.data, size)) goto done;
        }
        p = end;
    }
    valid = found;
done:
    free(host);
    free(port);
    snag_buf_free(&authority);
    snag_buf_free(&origin);
    return valid;
}

static char *
wait_callback(struct snag_mcp_server *server, snag_socket listener, const char *redirect,
    const char *state, const json_t *metadata, snag_mcp_pump_fn pump, void *opaque)
{
    struct snag_term_host terminal = {0};
    struct snag_buf input = {.max = SNAG_CONFIG_FILE_MAX};
    struct snag_buf request = {.max = SNAG_CONFIG_FILE_MAX};
    snag_socket peer = SNAG_SOCKET_INVALID;
    bool hidden = false;
    bool stdin_open = true;
    char *code = NULL;
    uint64_t deadline = snag_monotonic_ms() + 600000u;
    uint64_t peer_deadline = 0u;
    if (snag_isatty(STDIN_FILENO)) {
        if (snag_term_input_capture(&terminal) < 0 ||
            snag_term_input_raw(&terminal, false) < 0) {
            goto done;
        }
        hidden = true;
    }
    (void)fprintf(stderr, "Waiting for the loopback callback. You can also paste the complete "
        "redirected URL here (hidden); Ctrl-C cancels.\n");
    while (snag_monotonic_ms() < deadline) {
        if (pump ? pump(opaque, 20u) != 0 : snag_sleep_ms(20u) < 0) goto done;
        int input_ready = stdin_open
            ? snag_term_input_wait(&terminal, SNAG_WAKE_INVALID, 0) : 0;
        if (input_ready & SNAG_TERM_WAIT_INPUT) {
            char bytes[256];
            ssize_t n = snag_term_input_read(&terminal, bytes, sizeof(bytes));
            if (!n) stdin_open = false;
            for (ssize_t i = 0; i < n; ++i) {
                if (bytes[i] == 3) goto done;
                if (bytes[i] == 21) {
                    if (input.data) snag_secret_clear(input.data, input.len);
                    snag_buf_reset(&input);
                    continue;
                }
                if (bytes[i] == 8 || bytes[i] == 127) {
                    if (input.len) {
                        size_t before = input.len;
                        do { --input.len; }
                        while (input.len && (input.data[input.len] & 0xc0u) == 0x80u);
                        snag_secret_clear(input.data + input.len, before - input.len);
                    }
                    continue;
                }
                if (bytes[i] == '\n' || bytes[i] == '\r') {
                    if (snag_buf_terminate(&input) < 0) goto done;
                    code = callback_code((char *)input.data, redirect, state, metadata);
                    goto done;
                }
                if (!bytes[i] || snag_buf_putc(&input, (unsigned char)bytes[i]) < 0) goto done;
            }
        }
        if ((input_ready & SNAG_TERM_WAIT_END) && !(input_ready & SNAG_TERM_WAIT_INPUT))
            stdin_open = false;
        if (peer == SNAG_SOCKET_INVALID) {
            peer = snag_socket_accept(listener);
            if (peer != SNAG_SOCKET_INVALID) peer_deadline = snag_monotonic_ms() + 5000u;
        }
        if (peer == SNAG_SOCKET_INVALID) continue;
        char bytes[2048];
        ssize_t n = snag_socket_recv(peer, bytes, sizeof(bytes));
        if (n > 0) {
            if (snag_buf_append(&request, bytes, (size_t)n) < 0 ||
                snag_buf_terminate(&request) < 0) goto done;
            char *end = strstr((char *)request.data, "\r\n\r\n");
            if (end) {
                char *target = (char *)request.data + 4u;
                char *space = strchr(target, ' ');
                if (strncmp((char *)request.data, "GET ", 4u) || !space) goto done;
                bool trusted = callback_headers((char *)request.data, redirect);
                *space = '\0';
                char *path = snag_mcp_url_part(redirect, CURLUPART_PATH);
                struct snag_buf full = {.max = SNAG_CONFIG_FILE_MAX};
                if (trusted && path && target[0] == '/' &&
                    snag_buf_printf(&full, "%.*s%s", (int)(strlen(redirect) - strlen(path)),
                        redirect, target) == 0 && snag_buf_terminate(&full) == 0)
                    code = callback_code((char *)full.data, redirect, state, metadata);
                free(path);
                if (full.data) snag_secret_clear(full.data, full.len);
                snag_buf_free(&full);
                const char *reply = code
                    ? "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 26\r\n\r\n"
                      "Return to your terminal.\r\n"
                    : "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
                (void)snag_socket_send(peer, reply, strlen(reply));
                goto done;
            }
        }
        if (!n || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) ||
            snag_monotonic_ms() >= peer_deadline) {
            (void)snag_socket_close(peer);
            peer = SNAG_SOCKET_INVALID;
            snag_buf_reset(&request);
        }
    }
done:
    if (peer != SNAG_SOCKET_INVALID) (void)snag_socket_close(peer);
    if (hidden) (void)snag_term_input_restore(&terminal, true);
    snag_term_host_close(&terminal);
    if (input.data) snag_secret_clear(input.data, input.len);
    if (request.data) snag_secret_clear(request.data, request.len);
    snag_buf_free(&input);
    snag_buf_free(&request);
    if (!code) {
        snag_errorf(server->error, sizeof(server->error),
            "OAuth callback invalid, cancelled or expired");
    }
    return code;
}

int
snag_mcp_login(struct snag_mcp *client, struct snag_mcp_server *server,
    snag_mcp_pump_fn pump, void *opaque)
{
    json_t *metadata = NULL;
    char *scope = NULL;
    char *redirect = NULL;
    char *verifier = NULL;
    char *challenge = NULL;
    char *state = NULL;
    char *code = NULL;
    struct snag_buf query = {.max = SNAG_CONFIG_FILE_MAX};
    struct snag_buf form = {.max = SNAG_CONFIG_FILE_MAX};
    snag_socket listener = SNAG_SOCKET_INVALID;
    int lock = -1;
    int rc = -1;
    unsigned char random[32];
    unsigned char digest[32];
    struct snag_sha256 hash;
    bool network_ready = snag_network_init() == 0;
    if (!network_ready || discover(server, pump, opaque, &metadata, &scope) < 0 ||
        snag_random_bytes(random, sizeof(random)) < 0 ||
        !(verifier = base64url(random, sizeof(random)))) goto done;
    snag_sha256_init(&hash);
    snag_sha256_update(&hash, verifier, strlen(verifier));
    snag_sha256_final(&hash, digest);
    challenge = base64url(digest, sizeof(digest));
    if (!challenge || snag_random_bytes(random, sizeof(random)) < 0 ||
        !(state = base64url(random, sizeof(random)))) goto done;
    listener = callback_listener(server, &redirect);
    if (listener == SNAG_SOCKET_INVALID) goto done;
    const char *resource = snag_mcp_string(server->config, "resource");
    if (!*resource) resource = snag_mcp_string(server->config, "url");
    if (form_field(&query, "response_type", "code") < 0 ||
        form_field(&query, "client_id", snag_mcp_string(server->config, "client_id")) < 0 ||
        form_field(&query, "redirect_uri", redirect) < 0 ||
        form_field(&query, "scope", scope) < 0 || form_field(&query, "resource", resource) < 0 ||
        form_field(&query, "state", state) < 0 ||
        form_field(&query, "code_challenge", challenge) < 0 ||
        form_field(&query, "code_challenge_method", "S256") < 0 ||
        snag_buf_terminate(&query) < 0) goto done;
    const char *authorize = snag_mcp_string(metadata, "authorization_endpoint");
    (void)fprintf(stderr, "Authorize MCP server %s, resource %s, requested scopes: %s\n%s%c%s\n",
        server->name, resource, *scope ? scope : "server default",
        authorize, strchr(authorize, '?') ? '&' : '?', query.data);
    char grant[SNAG_ID_HEX_LEN + 1u];
    if (snag_random_id(grant) < 0 ||
        json_object_set_new(metadata, "grant_id", json_string(grant)) < 0 ||
        json_object_set_new(metadata, "scope", json_string(scope)) < 0) goto done;
    code = wait_callback(server, listener, redirect, state, metadata, pump, opaque);
    if (!code) goto done;
    lock = auth_lock(client, server, pump, opaque);
    if (lock < 0 || form_field(&form, "grant_type", "authorization_code") < 0 ||
        form_field(&form, "code", code) < 0 || form_field(&form, "redirect_uri", redirect) < 0 ||
        form_field(&form, "code_verifier", verifier) < 0) goto done;
    rc = token_request(client, server, metadata, &form, pump, opaque);
done:
    if (lock >= 0) (void)close(lock);
    if (listener != SNAG_SOCKET_INVALID) (void)snag_socket_close(listener);
    if (network_ready) snag_network_free();
    snag_secret_clear(random, sizeof(random));
    snag_secret_clear(digest, sizeof(digest));
    snag_secret_bytes_free(verifier);
    snag_secret_bytes_free(state);
    snag_secret_bytes_free(code);
    free(challenge);
    free(scope);
    free(redirect);
    json_decref(metadata);
    if (form.data) snag_secret_clear(form.data, form.len);
    snag_buf_free(&form);
    snag_buf_free(&query);
    return rc;
}

int
snag_mcp_logout(struct snag_mcp *client, struct snag_mcp_server *server,
    snag_mcp_pump_fn pump, void *opaque)
{
    snag_mcp_watch_close(server);
    int lock = auth_lock(client, server, pump, opaque);
    if (lock < 0) return -1;
    int rc = snag_mcp_auth_load(client, server);
    const char *endpoint = snag_mcp_string(server->auth, "revocation_endpoint");
    bool remote_failed = false;
    bool has_revocation = *endpoint != '\0';
    if (!rc && *endpoint) {
        struct snag_buf form = {.max = SNAG_CONFIG_FILE_MAX};
        struct curl_slist *headers = NULL;
        struct snag_mcp_http response = {0};
        const char *token = snag_mcp_string(server->auth, "refresh_token");
        if (!*token) token = snag_mcp_string(server->auth, "access_token");
        int prepared = form_field(&form, "token", token);
        if (!prepared) prepared = client_authenticate(server, server->auth,
            "revocation_endpoint_auth_methods_supported", &form, &headers);
        if (!prepared) prepared = snag_buf_terminate(&form);
        if (!prepared) prepared = snag_mcp_header(&headers, "Content-Type",
            "application/x-www-form-urlencoded");
        remote_failed = prepared || snag_mcp_http(server, endpoint, "POST", (char *)form.data,
            headers, -1, pump, opaque, &response) < 0 || response.status != 200L;
        curl_slist_free_all(headers);
        snag_mcp_http_free(&response);
        if (form.data) snag_secret_clear(form.data, form.len);
        snag_buf_free(&form);
    }
    int dir = auth_directory(client, false);
    if (dir < 0) rc = -1;
    else {
        if (snag_unlink_at(dir, server->binding, false) < 0 && errno != ENOENT) rc = -1;
        if (snag_sync_dir(dir) < 0) rc = -1;
        (void)close(dir);
    }
    if (!rc) {
        snag_auth_json_free(server->auth);
        server->auth = NULL;
        server->enabled = false;
        json_decref(server->catalog);
        server->catalog = NULL;
        server->dirty = true;
        snag_errorf(server->error, sizeof(server->error), remote_failed
            ? "local credential removed; remote revocation failed" :
                has_revocation ? "credential removed and revoked" :
                    "local credential removed; server offers no revocation endpoint");
    }
    (void)close(lock);
    return rc;
}
