/* SPDX-License-Identifier: GPL-2.0-only */
#include "secret_source.h"
#include "fs.h"
#include "base.h"
#include "snag_jansson.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void
snag_secret_clear(void *data, size_t len)
{
    volatile unsigned char *p = data;
    for (size_t i = 0; i < len; ++i)
        p[i] = 0;
}

void
snag_secret_bytes_free(char *value)
{
    if (value) {
        snag_secret_clear(value, strlen(value));
        free(value);
    }
}

void
snag_secret_source_free(struct snag_secret_source *source)
{
    snag_secret_bytes_free(source->expression);
    snag_secret_bytes_free(source->value);
    snag_secret_bytes_free(source->path);
    memset(source, 0, sizeof(*source));
}

static bool
environment_name(const char *name)
{
    for (size_t i = 0; name[i]; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              c == '_' || (i && c >= '0' && c <= '9')))
            return false;
    }
    return name[0] && strlen(name) <= 255u;
}

int
snag_secret_source_parse(struct snag_secret_source *out, const char *expression,
                        const char *config_path, char *error, size_t error_size)
{
    struct snag_secret_source source = {0};
    struct snag_buf path;
    json_t *literal = NULL;
    char *home = NULL;
    size_t len = strlen(expression);
    int rc = -1;

    snag_buf_init(&path, SNAG_SECRET_MAX + 1u);
    if (!len || len > 65536u)
        goto invalid;
    source.expression = snag_strdup_checked(expression, 65536u);
    if (!source.expression)
        goto invalid;
    if (expression[0] == '"') {
        literal = json_loadb(expression, len, JSON_DECODE_ANY, NULL);
        if (!json_is_string(literal) || !json_string_length(literal) ||
            json_string_length(literal) > SNAG_SECRET_MAX ||
            strlen(json_string_value(literal)) != json_string_length(literal))
            goto invalid;
        source.kind = SNAG_SECRET_LITERAL;
        source.value = snag_strdup_checked(json_string_value(literal), SNAG_SECRET_MAX);
    } else if (strncmp(expression, "${", 2u) == 0) {
        if (len < 4u || len > 258u || expression[len - 1u] != '}')
            goto invalid;
        source.kind = SNAG_SECRET_ENV;
        source.value = snag_strdup_checked(expression + 2u, 256u);
        if (!source.value)
            goto invalid;
        source.value[len - 3u] = '\0';
        if (!environment_name(source.value))
            goto invalid;
    } else {
        const char *base = NULL;
        source.kind = SNAG_SECRET_FILE;
        source.value = snag_strdup_checked(expression, SNAG_SECRET_MAX);
        if (!source.value)
            goto invalid;
        if (snag_path_root_len(expression)) {
            if (snag_buf_printf(&path, "%s", expression) < 0)
                goto invalid;
        } else if (strncmp(expression, "~/", 2u) == 0) {
            home = snag_home_directory();
            base = home;
            if (!snag_path_root_len(base) ||
                snag_buf_printf(&path, "%s/%s", base, expression + 2u) < 0)
                goto invalid;
        } else {
            const char *slash = config_path ? strrchr(config_path, '/') : NULL;
            if (!slash || !snag_path_root_len(config_path) ||
                snag_buf_append(&path, config_path, (size_t)(slash - config_path)) < 0 ||
                snag_buf_printf(&path, "/%s", expression) < 0)
                goto invalid;
        }
        if (snag_buf_terminate(&path) < 0)
            goto invalid;
        source.path = (char *)path.data;
        path.data = NULL;
    }
    if (!source.value)
        goto invalid;
    snag_secret_source_free(out);
    *out = source;
    memset(&source, 0, sizeof(source));
    rc = 0;
    goto done;
invalid:
    errno = EINVAL;
    snag_errorf(error, error_size,
               "invalid secret source; use ${ENV}, a double-quoted literal, or a file path");
done:
    free(home);
    if (json_is_string(literal)) {
        volatile char *p = (volatile char *)json_string_value(literal);
        for (size_t i = 0; i < json_string_length(literal); ++i)
            p[i] = 0;
    }
    json_decref(literal);
    snag_secret_source_free(&source);
    snag_buf_free(&path);
    return rc;
}

int
snag_secret_source_resolve(const struct snag_secret_source *source, char **out,
                          char *error, size_t error_size)
{
    char *value = NULL;
    size_t len = 0u;
    int fd = -1, rc = -1;

    *out = NULL;
    if (source->kind == SNAG_SECRET_ENV) {
        value = snag_environment(source->value);
        if (value)
            len = strlen(value);
    } else if (source->kind == SNAG_SECRET_LITERAL) {
        const char *text = source->value;
        if (!text || !(len = strnlen(text, SNAG_SECRET_MAX + 1u)) || len > SNAG_SECRET_MAX)
            goto done;
        value = snag_strdup_checked(text, SNAG_SECRET_MAX);
    } else if (source->kind == SNAG_SECRET_FILE) {
        snag_file_info st;
        /* Reject special files without hanging; symlinks are intentional sources. */
        fd = snag_open_secret_file(source->path);
        if (fd < 0 || snag_fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
            st.st_size < 0 || (uintmax_t)st.st_size > SNAG_SECRET_MAX + 2u)
            goto done;
        value = calloc(SNAG_SECRET_MAX + 4u, 1u);
        if (!value)
            goto done;
        while (len < SNAG_SECRET_MAX + 3u) {
            ssize_t n = read(fd, value + len, SNAG_SECRET_MAX + 3u - len);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                goto done;
            if (!n)
                break;
            len += (size_t)n;
        }
        if (len && value[len - 1u] == '\n') {
            value[--len] = '\0';
            if (len && value[len - 1u] == '\r')
                value[--len] = '\0';
        }
    }
    if (!value || !len || len > SNAG_SECRET_MAX ||
        !snag_utf8_valid((const unsigned char *)value, len, true))
        goto done;
    *out = value;
    value = NULL;
    rc = 0;
done:
    if (value) {
        volatile char *p = value;
        for (size_t i = 0; i < len; ++i)
            p[i] = 0;
        free(value);
    }
    if (fd >= 0)
        (void)close(fd);
    if (rc < 0) {
        errno = EINVAL;
        snag_errorf(error, error_size, "%s secret source%s%s is unavailable, empty or invalid",
                   snag_secret_source_kind(source),
                   source->kind == SNAG_SECRET_ENV || source->kind == SNAG_SECRET_FILE ? " " : "",
                   source->kind == SNAG_SECRET_ENV ? source->value :
                   source->kind == SNAG_SECRET_FILE ? source->path : "");
    }
    return rc;
}

const char *
snag_secret_source_kind(const struct snag_secret_source *source)
{
    switch (source->kind) {
    case SNAG_SECRET_NONE: return "managed";
    case SNAG_SECRET_ENV: return "environment";
    case SNAG_SECRET_LITERAL: return "literal";
    case SNAG_SECRET_FILE: return "file";
    }
    return "invalid";
}
