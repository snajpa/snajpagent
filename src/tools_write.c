/* SPDX-License-Identifier: GPL-2.0-only */
#include "tools_write.h"
#include "tools_file.h"

#include "base.h"
#include "fs.h"
#include "json.h"

#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* One ceiling for a whole file, matching apply_patch's per-file bound, and a
 * smaller bound for a single edit pattern so a replace cannot grow without
 * limit through repetition. */
#define TOOL_FILE_MAX (16u * 1024u * 1024u)
#define TOOL_EDIT_PATTERN_MAX (1024u * 1024u)
#define TOOL_PATH_MAX 4096u

static json_t *
rejected(const char *text)
{
    /* Invalid or refused arguments keep the factual not-run reason. */
    return snag_tool_result("not_run", "invalid_arguments", text, -1, 0u);
}

static json_t *
failed(const char *text)
{
    return snag_tool_result("failed", NULL, text, 1, 0u);
}

static json_t *
finish(char *message, size_t size, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(message, size, format, arguments);
    va_end(arguments);
    return snag_tool_result_terminal(true, message);
}

static size_t
count_occurrences(const unsigned char *hay, size_t hay_len, const char *needle, size_t needle_len)
{
    size_t count = 0u;
    for (size_t i = 0u; needle_len && i + needle_len <= hay_len; ) {
        if (memcmp(hay + i, needle, needle_len) == 0) {
            ++count;
            i += needle_len;
        } else {
            ++i;
        }
    }
    return count;
}

static int
replace_all(const struct snag_buf *source, const char *needle, size_t needle_len,
            const char *replacement, size_t replacement_len, struct snag_buf *out)
{
    size_t i = 0u;

    while (i < source->len) {
        if (needle_len && i + needle_len <= source->len &&
            memcmp(source->data + i, needle, needle_len) == 0) {
            if (snag_buf_append(out, replacement, replacement_len) < 0) return -1;
            i += needle_len;
        } else {
            if (snag_buf_putc(out, source->data[i]) < 0) return -1;
            ++i;
        }
    }
    return 0;
}

static int
read_target(int parent_fd, const char *leaf, const char *path, struct snag_buf *bytes,
            struct snag_permissions *permissions, char *error, size_t error_size)
{
    snag_file_info info;
    int fd;

    fd = snag_open_read_security_at(parent_fd, leaf, false);
    if (fd < 0) {
        snag_errorf(error, error_size, "target %s cannot be opened", path);
        return -1;
    }
    if (snag_fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || (uint64_t)info.st_size > TOOL_FILE_MAX) {
        close(fd);
        snag_errorf(error, error_size, "target %s is not a regular file within 16 MiB", path);
        return -1;
    }
    if (snag_permissions_capture(fd, permissions) < 0 || snag_buf_read(bytes, fd) < 0 ||
        snag_buf_terminate(bytes) < 0) {
        close(fd);
        snag_errorf(error, error_size, "target %s cannot be read", path);
        return -1;
    }
    close(fd);
    return 0;
}

static int
install_bytes(int parent_fd, const char *leaf, const char *path, const struct snag_buf *bytes,
              const struct snag_permissions *permissions, char *error, size_t error_size)
{
    char temp[SNAG_NAME_MAX_BYTES + 1u] = {0};
    int saved;

    if (snag_file_stage(parent_fd, bytes, permissions, temp) < 0) {
        snag_errorf(error, error_size, "target %s could not be staged", path);
        return -1;
    }
    if (snag_rename_at(parent_fd, temp, parent_fd, leaf) < 0) {
        saved = errno;
        (void)snag_unlink_at(parent_fd, temp, false);
        errno = saved;
        snag_errorf(error, error_size, "target %s could not be installed", path);
        return -1;
    }
    if (snag_sync_dir(parent_fd) < 0) {
        snag_errorf(error, error_size, "target %s directory sync failed", path);
        return -1;
    }
    return 0;
}

/* Returns 0 with *create set, or -1 after filling *error. */
static int
prepare(int root_fd, const char *path, char leaf[SNAG_NAME_MAX_BYTES + 1u],
        char *error, size_t error_size)
{
    if (snag_file_path_valid(path, error, error_size) < 0) return -1;
    if (snag_file_parent(root_fd, path, leaf, error, error_size) < 0) return -1;
    return 0;
}

int
snag_tools_write_file(const struct snag_response_item *call, const char *session_workspace,
                      json_t **result, char *error, size_t error_size)
{
    const char *path, *content;
    char leaf[SNAG_NAME_MAX_BYTES + 1u], message[512];
    struct snag_buf bytes;
    struct snag_permissions permissions;
    snag_file_info info;
    int root_fd = -1, parent_fd = -1, fd = -1;

    if (!result || !session_workspace) return snag_fail(error, error_size, EINVAL, "invalid write_file destination");
    *result = NULL;
    snag_buf_init(&bytes, TOOL_FILE_MAX);
    memset(&permissions, 0, sizeof(permissions));
    if (!snag_json_arg_keys(call->arguments, "path content", "", error, error_size) ||
        !snag_json_arg_text(call->arguments, "path", 1u, TOOL_PATH_MAX, false, &path, error, error_size) ||
        !snag_json_arg_text(call->arguments, "content", 0u, TOOL_FILE_MAX, false, &content, error, error_size) ||
        snag_buf_append(&bytes, content, strlen(content)) < 0) {
        *result = rejected(*error ? error : "write_file requires bounded path and content strings.");
        goto out;
    }
    root_fd = snag_open_read(session_workspace, true);
    if (root_fd < 0 || prepare(root_fd, path, leaf, error, error_size) < 0) {
        *result = failed(*error ? error : "workspace cannot be opened");
        goto out;
    }
    parent_fd = snag_file_parent(root_fd, path, leaf, error, error_size);
    if (parent_fd < 0) {
        *result = failed(*error ? error : "target parent cannot be opened");
        goto out;
    }
    if (snag_lstat_at(parent_fd, leaf, &info) == 0) {
        bool have = false;
        if (!S_ISREG(info.st_mode) || info.st_size < 0 || (uint64_t)info.st_size > TOOL_FILE_MAX) {
            (void)snprintf(message, sizeof(message), "Target %s is not a regular file within 16 MiB.", path);
            *result = failed(message);
            goto out;
        }
        fd = snag_open_read_security_at(parent_fd, leaf, false);
        if (fd < 0 || snag_permissions_capture(fd, &permissions) < 0) {
            (void)snprintf(message, sizeof(message), "Target %s cannot be opened.", path);
            *result = failed(message);
            goto out;
        }
        close(fd);
        fd = -1;
        have = true;
        if (install_bytes(parent_fd, leaf, path, &bytes, &permissions, error, error_size) < 0) {
            *result = failed(*error ? error : "write_file failed");
            goto out;
        }
        (void)have;
    } else if (errno != ENOENT) {
        (void)snprintf(message, sizeof(message), "Target %s cannot be checked.", path);
        *result = failed(message);
        goto out;
    } else if (install_bytes(parent_fd, leaf, path, &bytes, NULL, error, error_size) < 0) {
        *result = failed(*error ? error : "write_file failed");
        goto out;
    }
    *result = finish(message, sizeof(message), "Wrote %s (%llu bytes).", path, (unsigned long long)bytes.len);
out:
    if (fd >= 0) close(fd);
    if (parent_fd >= 0) close(parent_fd);
    if (root_fd >= 0) close(root_fd);
    snag_permissions_free(&permissions);
    snag_buf_free(&bytes);
    return *result ? 0 : -1;
}

int
snag_tools_edit_file(const struct snag_response_item *call, const char *session_workspace,
                     json_t **result, char *error, size_t error_size)
{
    const char *path, *old, *replacement;
    char leaf[SNAG_NAME_MAX_BYTES + 1u], message[512];
    struct snag_buf source, updated;
    struct snag_permissions permissions;
    uint64_t expected = 1u, found;
    size_t old_len;
    int root_fd = -1, parent_fd = -1;

    if (!result || !session_workspace) return snag_fail(error, error_size, EINVAL, "invalid edit_file destination");
    *result = NULL;
    snag_buf_init(&source, TOOL_FILE_MAX + 1u);
    snag_buf_init(&updated, TOOL_FILE_MAX);
    memset(&permissions, 0, sizeof(permissions));
    if (!snag_json_arg_keys(call->arguments, "path old new", "count", error, error_size) ||
        !snag_json_arg_text(call->arguments, "path", 1u, TOOL_PATH_MAX, false, &path, error, error_size) ||
        !snag_json_arg_text(call->arguments, "old", 1u, TOOL_EDIT_PATTERN_MAX, false, &old, error, error_size) ||
        !snag_json_arg_text(call->arguments, "new", 0u, TOOL_FILE_MAX, false, &replacement, error, error_size) ||
        !snag_json_arg_uint(call->arguments, "count", 1u, 1u, 1000000u, &expected, error, error_size)) {
        *result = rejected(*error ? error : "edit_file requires bounded path, old and new strings.");
        goto out;
    }
    old_len = strlen(old);
    root_fd = snag_open_read(session_workspace, true);
    if (root_fd < 0 || prepare(root_fd, path, leaf, error, error_size) < 0) {
        *result = failed(*error ? error : "workspace cannot be opened");
        goto out;
    }
    parent_fd = snag_file_parent(root_fd, path, leaf, error, error_size);
    if (parent_fd < 0 || read_target(parent_fd, leaf, path, &source, &permissions, error, error_size) < 0) {
        *result = failed(*error ? error : "target cannot be read");
        goto out;
    }
    found = count_occurrences(source.data, source.len, old, old_len);
    if (found != expected) {
        (void)snprintf(message, sizeof(message),
                       "Target %s contains %llu exact match(es); expected %llu. Nothing changed.",
                       path, (unsigned long long)found, (unsigned long long)expected);
        *result = rejected(message);
        goto out;
    }
    if (replace_all(&source, old, old_len, replacement, strlen(replacement), &updated) < 0) {
        (void)snprintf(message, sizeof(message), "Resulting %s exceeds 16 MiB. Nothing changed.", path);
        *result = rejected(message);
        goto out;
    }
    if (install_bytes(parent_fd, leaf, path, &updated, &permissions, error, error_size) < 0) {
        *result = failed(*error ? error : "edit_file failed");
        goto out;
    }
    *result = finish(message, sizeof(message), "Replaced %llu occurrence(s) in %s.",
                     (unsigned long long)found, path);
out:
    if (parent_fd >= 0) close(parent_fd);
    if (root_fd >= 0) close(root_fd);
    snag_permissions_free(&permissions);
    snag_buf_free(&source);
    snag_buf_free(&updated);
    return *result ? 0 : -1;
}
