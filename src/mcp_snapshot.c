/* SPDX-License-Identifier: GPL-2.0-only */
#include "mcp_internal.h"
#include "context.h"
#include "fs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* One replaceable file per session pins the remote contract through a crash.
 * Credentials and approvals are never included. The session owns its writer lock. */
int
snag_mcp_snapshot(struct snag_mcp *client, int dir, const char *turn, bool write_snapshot)
{
    if (dir < 0) return client->count ? -1 : 0;
    const char *name = "mcp-turn.json";
    struct snag_buf bytes = {.max = SNAG_CONTEXT_MAX_REQUEST};
    json_t *value = NULL;
    int fd = -1;
    int rc = -1;
    char temporary[SNAG_ID_HEX_LEN + 1u] = {0};
    if (!write_snapshot) {
        fd = snag_open_read_security_at(dir, name, false);
        if (fd < 0 && errno == ENOENT) return 0;
        struct snag_file_privacy privacy;
        snag_file_info st;
        if (fd < 0 || snag_fd_privacy(fd, &privacy) < 0 || !privacy.effective_owner ||
            !privacy.private_access || snag_fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
            st.st_nlink != 1u || snag_buf_read(&bytes, fd) < 0) goto done;
        value = snag_json_load_strict(bytes.data, bytes.len, bytes.max, NULL, 0u);
        if (!value) goto done;
        if (strcmp(snag_mcp_string(value, "turn_id"), turn)) {
            rc = 0;
            goto done;
        }
        json_t *servers = json_object_get(value, "servers");
        if (!json_is_object(servers)) goto done;
        for (size_t i = 0u; i < client->count; ++i) {
            struct snag_mcp_server *server = &client->servers[i];
            json_t *saved = json_object_get(servers, server->binding);
            if (!saved) {
                server->error[0] = '\0';
                continue;
            }
            json_t *catalog = json_object_get(saved, "catalog");
            if (!json_is_array(catalog) || snag_mcp_contains_secret(server, saved) ||
                !json_is_integer(json_object_get(saved, "generation")) ||
                json_integer_value(json_object_get(saved, "generation")) < 0) goto done;
            for (size_t t = 0u; t < json_array_size(catalog); ++t) {
                json_t *tool = json_array_get(catalog, t);
                json_t *schema = json_object_get(tool, "inputSchema");
                if (!*snag_mcp_string(tool, "name") || !json_is_object(schema) ||
                    strcmp(snag_mcp_string(schema, "type"), "object") ||
                    snag_mcp_parameter_headers(schema, NULL, NULL) < 0) goto done;
                for (size_t previous = 0u; previous < t; ++previous)
                    if (!strcmp(snag_mcp_string(tool, "name"),
                            snag_mcp_string(json_array_get(catalog, previous), "name"))) goto done;
            }
            json_decref(server->catalog);
            server->catalog = json_incref(catalog);
            json_decref(server->filtered);
            server->filtered = json_incref(json_object_get(saved, "filtered"));
            server->generation = (uint64_t)json_integer_value(json_object_get(saved, "generation"));
            server->legacy = json_is_true(json_object_get(saved, "legacy"));
            const char *version = snag_mcp_string(saved, "version");
            if (!snag_string_in(version, "2026-07-28 2025-03-26 2025-06-18 2025-11-25")) goto done;
            (void)snag_strcpy(server->version, sizeof(server->version), version);
        }
        rc = 1;
        goto done;
    }
    value = json_pack("{s:s,s:{}}", "turn_id", turn, "servers");
    if (!value) goto done;
    for (size_t i = 0u; i < client->count; ++i) {
        struct snag_mcp_server *server = &client->servers[i];
        if (!server->catalog) continue;
        json_t *saved = json_pack("{s:O,s:O,s:I,s:b,s:s}", "catalog", server->catalog,
            "filtered", server->filtered ? server->filtered : json_null(),
            "generation", (json_int_t)server->generation, "legacy", server->legacy,
            "version", server->version);
        if (!saved || json_object_set_new(json_object_get(value, "servers"), server->binding,
                saved) < 0) goto done;
    }
    if (snag_json_diagnostic(value, &bytes) < 0 || snag_random_id(temporary) < 0) goto done;
    fd = snag_create_private_at(dir, temporary, true);
    if (fd < 0 || snag_write_full(fd, bytes.data, bytes.len) < 0 || snag_fsync(fd) < 0 ||
        snag_rename_at(dir, temporary, dir, name) < 0) goto done;
    temporary[0] = '\0';
    rc = snag_sync_dir(dir);
done:
    if (*temporary) (void)snag_unlink_at(dir, temporary, false);
    if (fd >= 0) (void)close(fd);
    json_decref(value);
    snag_buf_free(&bytes);
    return rc;
}
