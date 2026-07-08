/* SPDX-License-Identifier: GPL-2.0-only */
#include "tools_patch.h"

#include "base.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include "snj_jansson.h"
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#define PATCH_TEXT_MAX (2u * 1024u * 1024u)
#define PATCH_LINE_MAX (1024u * 1024u)
#define PATCH_PATH_MAX 4096u
#define PATCH_OP_MAX 256u
#define PATCH_HUNK_MAX 2048u
#define PATCH_FILE_MAX (16u * 1024u * 1024u)
#define PATCH_TOTAL_MAX (48u * 1024u * 1024u)
#define PATCH_MODEL_MAX (512u * 1024u)
#define PATCH_PREVIEW_MAX (128u * 1024u)

struct line_vec {
    char **v;
    size_t n;
    size_t cap;
};

enum patch_op_type {
    OP_ADD,
    OP_UPDATE,
    OP_DELETE
};

enum hunk_type {
    HUNK_NORMAL,
    HUNK_START,
    HUNK_END
};

struct patch_hunk {
    enum hunk_type type;
    struct line_vec old_lines;
    struct line_vec new_lines;
    struct line_vec preview_lines;
    bool changed;
};

struct patch_op {
    enum patch_op_type type;
    char *path;
    struct line_vec add_lines;
    struct patch_hunk *hunks;
    size_t hunk_count;
    size_t hunk_cap;
    char *old_bytes;
    size_t old_len;
    struct snj_buf new_bytes;
    bool eol_crlf;
    bool final_nl;
    mode_t mode;
    struct stat st;
    size_t added_lines;
    size_t removed_lines;
};

struct patch_set {
    struct patch_op *ops;
    size_t count;
    size_t cap;
    size_t hunk_total;
    size_t total_file_bytes;
};

static void
set_error(char *error, size_t size, const char *fmt, ...)
{
    va_list ap;
    if (!size)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(error, size, fmt, ap);
    va_end(ap);
}

static void
line_vec_free(struct line_vec *vec)
{
    if (!vec)
        return;
    for (size_t i = 0; i < vec->n; ++i)
        free(vec->v[i]);
    free(vec->v);
    memset(vec, 0, sizeof(*vec));
}

static int
line_vec_pushn(struct line_vec *vec, const char *s, size_t len)
{
    char *copy;
    char **newv;
    size_t newcap;

    if (vec->n == vec->cap) {
        newcap = vec->cap ? vec->cap * 2u : 8u;
        if (newcap < vec->cap || newcap > PATCH_LINE_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        newv = realloc(vec->v, newcap * sizeof(*newv));
        if (!newv)
            return -1;
        vec->v = newv;
        vec->cap = newcap;
    }
    copy = malloc(len + 1u);
    if (!copy)
        return -1;
    memcpy(copy, s, len);
    copy[len] = '\0';
    vec->v[vec->n++] = copy;
    return 0;
}

static int
line_vec_push(struct line_vec *vec, const char *s)
{
    return line_vec_pushn(vec, s, strlen(s));
}

static void
hunk_free(struct patch_hunk *hunk)
{
    line_vec_free(&hunk->old_lines);
    line_vec_free(&hunk->new_lines);
    line_vec_free(&hunk->preview_lines);
    memset(hunk, 0, sizeof(*hunk));
}

static void
op_free(struct patch_op *op)
{
    if (!op)
        return;
    free(op->path);
    line_vec_free(&op->add_lines);
    for (size_t i = 0; i < op->hunk_count; ++i)
        hunk_free(&op->hunks[i]);
    free(op->hunks);
    free(op->old_bytes);
    snj_buf_free(&op->new_bytes);
    memset(op, 0, sizeof(*op));
}

static void
patch_set_free(struct patch_set *set)
{
    if (!set)
        return;
    for (size_t i = 0; i < set->count; ++i)
        op_free(&set->ops[i]);
    free(set->ops);
    memset(set, 0, sizeof(*set));
}

static int
patch_set_add(struct patch_set *set, struct patch_op **out)
{
    struct patch_op *newops;
    size_t newcap;

    if (set->count >= PATCH_OP_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    if (set->count == set->cap) {
        newcap = set->cap ? set->cap * 2u : 8u;
        if (newcap > PATCH_OP_MAX)
            newcap = PATCH_OP_MAX;
        newops = realloc(set->ops, newcap * sizeof(*newops));
        if (!newops)
            return -1;
        set->ops = newops;
        set->cap = newcap;
    }
    *out = &set->ops[set->count++];
    memset(*out, 0, sizeof(**out));
    snj_buf_init(&(*out)->new_bytes, PATCH_FILE_MAX);
    return 0;
}

static int
op_add_hunk(struct patch_set *set, struct patch_op *op,
            struct patch_hunk **out)
{
    struct patch_hunk *newhunks;
    size_t newcap;

    if (set->hunk_total >= PATCH_HUNK_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    if (op->hunk_count == op->hunk_cap) {
        newcap = op->hunk_cap ? op->hunk_cap * 2u : 4u;
        newhunks = realloc(op->hunks, newcap * sizeof(*newhunks));
        if (!newhunks)
            return -1;
        op->hunks = newhunks;
        op->hunk_cap = newcap;
    }
    *out = &op->hunks[op->hunk_count++];
    memset(*out, 0, sizeof(**out));
    ++set->hunk_total;
    return 0;
}

static bool
starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool
is_file_header(const char *line)
{
    return starts_with(line, "*** Add File: ") ||
           starts_with(line, "*** Update File: ") ||
           starts_with(line, "*** Delete File: ");
}

static bool
is_hunk_header(const char *line)
{
    return strcmp(line, "@@") == 0 || starts_with(line, "@@ ");
}

static int
path_valid(const char *path, char *error, size_t error_size)
{
    size_t len = strlen(path);
    const char *p = path;
    const char *component = path;

    if (!len || len > PATCH_PATH_MAX || path[0] == '/' ||
        !snj_utf8_valid((const unsigned char *)path, len, true)) {
        set_error(error, error_size, "patch path is not a bounded relative UTF-8 path");
        errno = EINVAL;
        return -1;
    }
    if (len >= 2u && ((path[0] >= 'A' && path[0] <= 'Z') ||
                      (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':') {
        set_error(error, error_size, "patch path uses a drive-prefix form");
        errno = EINVAL;
        return -1;
    }
    if (starts_with(path, "//")) {
        set_error(error, error_size, "patch path uses a UNC-like form");
        errno = EINVAL;
        return -1;
    }
    while (*p) {
        if ((unsigned char)*p < 0x20u || (unsigned char)*p == 0x7fu ||
            *p == '\\') {
            set_error(error, error_size, "patch path contains a rejected byte");
            errno = EINVAL;
            return -1;
        }
        if (*p == '/') {
            if (p == component || (p - component == 1 && component[0] == '.') ||
                (p - component == 2 && component[0] == '.' && component[1] == '.')) {
                set_error(error, error_size, "patch path contains an invalid component");
                errno = EINVAL;
                return -1;
            }
            component = p + 1;
        }
        ++p;
    }
    if (p == component || (p - component == 1 && component[0] == '.') ||
        (p - component == 2 && component[0] == '.' && component[1] == '.')) {
        set_error(error, error_size, "patch path contains an invalid final component");
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int
normalize_patch_text(const char *patch, size_t len, char **out,
                     char *error, size_t error_size)
{
    struct snj_buf buf;
    size_t line_len = 0;
    int rc = -1;

    *out = NULL;
    if (len > PATCH_TEXT_MAX || !snj_utf8_valid((const unsigned char *)patch,
                                                len, true)) {
        set_error(error, error_size, "patch must be bounded UTF-8 without NUL");
        errno = EINVAL;
        return -1;
    }
    snj_buf_init(&buf, PATCH_TEXT_MAX + 1u);
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)patch[i];
        if (c == '\r') {
            if (i + 1u >= len || patch[i + 1u] != '\n') {
                set_error(error, error_size, "patch contains a bare carriage return");
                errno = EINVAL;
                goto out_free;
            }
            if (line_len > PATCH_LINE_MAX) {
                set_error(error, error_size, "patch line exceeds 1 MiB");
                errno = EOVERFLOW;
                goto out_free;
            }
            if (snj_buf_putc(&buf, '\n') < 0)
                goto out_free;
            line_len = 0;
            ++i;
        } else {
            if (snj_buf_putc(&buf, c) < 0)
                goto out_free;
            if (c == '\n') {
                if (line_len > PATCH_LINE_MAX) {
                    set_error(error, error_size, "patch line exceeds 1 MiB");
                    errno = EOVERFLOW;
                    goto out_free;
                }
                line_len = 0;
            } else if (++line_len > PATCH_LINE_MAX) {
                set_error(error, error_size, "patch line exceeds 1 MiB");
                errno = EOVERFLOW;
                goto out_free;
            }
        }
    }
    if (snj_buf_terminate(&buf) < 0)
        goto out_free;
    *out = (char *)buf.data;
    memset(&buf, 0, sizeof(buf));
    rc = 0;
out_free:
    snj_buf_free(&buf);
    return rc;
}

static int
split_lines(char *text, char ***out_lines, size_t *out_count,
            char *error, size_t error_size)
{
    char **lines = NULL;
    size_t count = 0;
    size_t cap = 0;
    char *start = text;

    *out_lines = NULL;
    *out_count = 0;
    for (char *p = text;; ++p) {
        if (*p != '\n' && *p != '\0')
            continue;
        if (count == cap) {
            size_t newcap = cap ? cap * 2u : 16u;
            char **newlines = realloc(lines, newcap * sizeof(*newlines));
            if (!newlines) {
                free(lines);
                return -1;
            }
            lines = newlines;
            cap = newcap;
        }
        if (*p == '\n') {
            *p = '\0';
            lines[count++] = start;
            start = p + 1;
            continue;
        }
        if (p != start)
            lines[count++] = start;
        break;
    }
    if (count < 2u) {
        free(lines);
        set_error(error, error_size, "patch is missing required frame");
        errno = EINVAL;
        return -1;
    }
    *out_lines = lines;
    *out_count = count;
    return 0;
}

static int
check_duplicate_path(const struct patch_set *set, const char *path,
                     char *error, size_t error_size)
{
    for (size_t i = 0; i < set->count; ++i) {
        if (strcmp(set->ops[i].path, path) == 0) {
            set_error(error, error_size, "patch contains duplicate target path");
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

static int
parse_hunk_header(const char *line, enum hunk_type *type,
                  char *error, size_t error_size)
{
    *type = HUNK_NORMAL;
    if (strcmp(line, "@@") == 0)
        return 0;
    if (!starts_with(line, "@@ ") || line[3] == '\0') {
        set_error(error, error_size, "invalid hunk header");
        errno = EINVAL;
        return -1;
    }
    if (strcmp(line + 3, "@start") == 0)
        *type = HUNK_START;
    else if (strcmp(line + 3, "@end") == 0)
        *type = HUNK_END;
    return 0;
}

static int
parse_patch_lines(char **lines, size_t line_count, struct patch_set *set,
                  char *error, size_t error_size)
{
    size_t i = 1;

    if (strcmp(lines[0], "*** Begin Patch") != 0 ||
        strcmp(lines[line_count - 1u], "*** End Patch") != 0) {
        set_error(error, error_size, "patch frame must begin and end exactly");
        errno = EINVAL;
        return -1;
    }
    while (i + 1u < line_count) {
        struct patch_op *op;
        const char *path;

        if (starts_with(lines[i], "*** Add File: ")) {
            path = lines[i] + strlen("*** Add File: ");
            if (path_valid(path, error, error_size) < 0 ||
                check_duplicate_path(set, path, error, error_size) < 0 ||
                patch_set_add(set, &op) < 0)
                return -1;
            op->type = OP_ADD;
            op->path = snj_strdup_checked(path, PATCH_PATH_MAX);
            if (!op->path)
                return -1;
            ++i;
            while (i + 1u < line_count && !is_file_header(lines[i])) {
                if (lines[i][0] != '+') {
                    set_error(error, error_size, "add-file body lines must start with +");
                    errno = EINVAL;
                    return -1;
                }
                if (line_vec_push(&op->add_lines, lines[i] + 1) < 0)
                    return -1;
                ++op->added_lines;
                ++i;
            }
        } else if (starts_with(lines[i], "*** Delete File: ")) {
            path = lines[i] + strlen("*** Delete File: ");
            if (path_valid(path, error, error_size) < 0 ||
                check_duplicate_path(set, path, error, error_size) < 0 ||
                patch_set_add(set, &op) < 0)
                return -1;
            op->type = OP_DELETE;
            op->path = snj_strdup_checked(path, PATCH_PATH_MAX);
            if (!op->path)
                return -1;
            ++i;
            if (i + 1u < line_count && !is_file_header(lines[i])) {
                set_error(error, error_size, "delete-file sections cannot have a body");
                errno = EINVAL;
                return -1;
            }
        } else if (starts_with(lines[i], "*** Update File: ")) {
            path = lines[i] + strlen("*** Update File: ");
            if (path_valid(path, error, error_size) < 0 ||
                check_duplicate_path(set, path, error, error_size) < 0 ||
                patch_set_add(set, &op) < 0)
                return -1;
            op->type = OP_UPDATE;
            op->path = snj_strdup_checked(path, PATCH_PATH_MAX);
            if (!op->path)
                return -1;
            ++i;
            while (i + 1u < line_count && !is_file_header(lines[i])) {
                struct patch_hunk *hunk;
                if (!is_hunk_header(lines[i]) ||
                    op_add_hunk(set, op, &hunk) < 0 ||
                    parse_hunk_header(lines[i], &hunk->type,
                                      error, error_size) < 0)
                    return -1;
                ++i;
                while (i + 1u < line_count && !is_file_header(lines[i]) &&
                       !is_hunk_header(lines[i])) {
                    if (hunk->type == HUNK_START || hunk->type == HUNK_END) {
                        if (lines[i][0] != '+') {
                            set_error(error, error_size,
                                      "anchored hunks may contain only + lines");
                            errno = EINVAL;
                            return -1;
                        }
                    } else if (lines[i][0] != ' ' && lines[i][0] != '-' &&
                               lines[i][0] != '+') {
                        set_error(error, error_size,
                                  "update hunk body lines must start with space, -, or +");
                        errno = EINVAL;
                        return -1;
                    }
                    if (line_vec_push(&hunk->preview_lines, lines[i]) < 0)
                        return -1;
                    if (lines[i][0] == ' ' || lines[i][0] == '-') {
                        if (line_vec_push(&hunk->old_lines, lines[i] + 1) < 0)
                            return -1;
                    }
                    if (lines[i][0] == ' ' || lines[i][0] == '+') {
                        if (line_vec_push(&hunk->new_lines, lines[i] + 1) < 0)
                            return -1;
                    }
                    if (lines[i][0] == '+' || lines[i][0] == '-') {
                        hunk->changed = true;
                        if (lines[i][0] == '+')
                            ++op->added_lines;
                        else
                            ++op->removed_lines;
                    }
                    ++i;
                }
                if ((hunk->type == HUNK_START || hunk->type == HUNK_END) &&
                    hunk->new_lines.n == 0u) {
                    set_error(error, error_size,
                              "anchored hunks must insert at least one line");
                    errno = EINVAL;
                    return -1;
                }
                if (hunk->type == HUNK_NORMAL &&
                    (!hunk->changed || hunk->old_lines.n == 0u)) {
                    set_error(error, error_size,
                              "normal hunks need a nonempty old pattern and a change");
                    errno = EINVAL;
                    return -1;
                }
            }
            if (op->hunk_count == 0u) {
                set_error(error, error_size, "update-file sections need at least one hunk");
                errno = EINVAL;
                return -1;
            }
        } else {
            set_error(error, error_size, "expected a file operation header");
            errno = EINVAL;
            return -1;
        }
    }
    if (set->count == 0u) {
        set_error(error, error_size, "patch contains no file operations");
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int
append_joined_lines(struct snj_buf *out, const struct line_vec *lines)
{
    for (size_t i = 0; i < lines->n; ++i) {
        if (snj_buf_append(out, lines->v[i], strlen(lines->v[i])) < 0 ||
            snj_buf_putc(out, '\n') < 0)
            return -1;
    }
    return 0;
}

static int
read_fd_all(int fd, char **out, size_t *out_len)
{
    struct snj_buf buf;
    unsigned char tmp[8192];
    int rc = -1;

    *out = NULL;
    *out_len = 0;
    snj_buf_init(&buf, PATCH_FILE_MAX + 1u);
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n > 0) {
            if (snj_buf_append(&buf, tmp, (size_t)n) < 0)
                goto out_free;
            continue;
        }
        if (n == 0)
            break;
        if (errno == EINTR)
            continue;
        goto out_free;
    }
    if (snj_buf_terminate(&buf) < 0)
        goto out_free;
    *out = (char *)buf.data;
    *out_len = buf.len;
    memset(&buf, 0, sizeof(buf));
    rc = 0;
out_free:
    snj_buf_free(&buf);
    return rc;
}

static int
open_parent_dir(int root_fd, const char *path, char leaf[NAME_MAX + 1u],
                char *error, size_t error_size)
{
    int dir_fd = dup(root_fd);
    const char *p = path;
    const char *slash;

    if (dir_fd < 0)
        return -1;
    for (;;) {
        size_t len;
        slash = strchr(p, '/');
        len = slash ? (size_t)(slash - p) : strlen(p);
        if (!slash) {
            if (len > NAME_MAX) {
                close(dir_fd);
                set_error(error, error_size, "patch path component is too long");
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(leaf, p, len);
            leaf[len] = '\0';
            return dir_fd;
        }
        if (len > NAME_MAX) {
            close(dir_fd);
            set_error(error, error_size, "patch path component is too long");
            errno = ENAMETOOLONG;
            return -1;
        }
        {
            char component[NAME_MAX + 1u];
            int next_fd;
            memcpy(component, p, len);
            component[len] = '\0';
            next_fd = openat(dir_fd, component,
                             O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
            if (next_fd < 0) {
                close(dir_fd);
                set_error(error, error_size,
                          "patch parent directory cannot be opened without following symlinks");
                return -1;
            }
            close(dir_fd);
            dir_fd = next_fd;
        }
        p = slash + 1;
    }
}

static int
read_target_file(int root_fd, struct patch_op *op,
                 char *error, size_t error_size)
{
    char leaf[NAME_MAX + 1u];
    int parent_fd = -1;
    int fd = -1;
    int rc = -1;

    parent_fd = open_parent_dir(root_fd, op->path, leaf, error, error_size);
    if (parent_fd < 0)
        return -1;
    fd = openat(parent_fd, leaf, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        set_error(error, error_size, "patch target %s cannot be opened", op->path);
        goto out;
    }
    if (fstat(fd, &op->st) < 0)
        goto out;
    if (!S_ISREG(op->st.st_mode) || op->st.st_size > (off_t)PATCH_FILE_MAX) {
        set_error(error, error_size,
                  "patch target %s is not a regular file within 16 MiB", op->path);
        errno = EINVAL;
        goto out;
    }
    op->mode = op->st.st_mode & 07777u;
    if (read_fd_all(fd, &op->old_bytes, &op->old_len) < 0) {
        set_error(error, error_size, "patch target %s cannot be read", op->path);
        goto out;
    }
    rc = 0;
out:
    if (fd >= 0)
        close(fd);
    close(parent_fd);
    return rc;
}

static int
validate_add_target(int root_fd, struct patch_op *op,
                    char *error, size_t error_size)
{
    char leaf[NAME_MAX + 1u];
    struct stat st;
    int parent_fd = open_parent_dir(root_fd, op->path, leaf, error, error_size);
    int rc = -1;

    if (parent_fd < 0)
        return -1;
    if (fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        set_error(error, error_size, "add target %s already exists", op->path);
        errno = EEXIST;
        goto out;
    }
    if (errno != ENOENT) {
        set_error(error, error_size, "add target %s cannot be checked", op->path);
        goto out;
    }
    rc = 0;
out:
    close(parent_fd);
    return rc;
}

static int
validate_delete_target(int root_fd, struct patch_op *op,
                       char *error, size_t error_size)
{
    char leaf[NAME_MAX + 1u];
    int parent_fd = open_parent_dir(root_fd, op->path, leaf, error, error_size);
    int rc = -1;

    if (parent_fd < 0)
        return -1;
    if (fstatat(parent_fd, leaf, &op->st, AT_SYMLINK_NOFOLLOW) < 0) {
        set_error(error, error_size, "delete target %s cannot be checked", op->path);
        goto out;
    }
    if (!S_ISREG(op->st.st_mode)) {
        set_error(error, error_size, "delete target %s is not a regular file", op->path);
        errno = EINVAL;
        goto out;
    }
    rc = 0;
out:
    close(parent_fd);
    return rc;
}

static int
parse_file_lines(const char *bytes, size_t len, struct line_vec *lines,
                 bool *crlf, bool *final_nl, char *error, size_t error_size)
{
    enum { STYLE_NONE, STYLE_LF, STYLE_CRLF } style = STYLE_NONE;
    size_t start = 0;

    if (!snj_utf8_valid((const unsigned char *)bytes, len, true)) {
        set_error(error, error_size, "update target is not strict UTF-8 without NUL");
        errno = EINVAL;
        return -1;
    }
    *final_nl = false;
    for (size_t i = 0; i < len; ++i) {
        if (bytes[i] == '\r') {
            if (i + 1u >= len || bytes[i + 1u] != '\n' || style == STYLE_LF) {
                set_error(error, error_size,
                          "update target has mixed or bare carriage-return line endings");
                errno = EINVAL;
                return -1;
            }
            style = STYLE_CRLF;
            if (line_vec_pushn(lines, bytes + start, i - start) < 0)
                return -1;
            i += 1u;
            start = i + 1u;
            *final_nl = true;
        } else if (bytes[i] == '\n') {
            if (style == STYLE_CRLF) {
                set_error(error, error_size, "update target has mixed line endings");
                errno = EINVAL;
                return -1;
            }
            style = STYLE_LF;
            if (line_vec_pushn(lines, bytes + start, i - start) < 0)
                return -1;
            start = i + 1u;
            *final_nl = true;
        } else {
            *final_nl = false;
        }
    }
    if (start < len && line_vec_pushn(lines, bytes + start, len - start) < 0)
        return -1;
    *crlf = style == STYLE_CRLF;
    return 0;
}

static bool
line_range_matches(const struct line_vec *lines, size_t pos,
                   const struct line_vec *pattern)
{
    if (pos > lines->n || pattern->n > lines->n - pos)
        return false;
    for (size_t i = 0; i < pattern->n; ++i)
        if (strcmp(lines->v[pos + i], pattern->v[i]) != 0)
            return false;
    return true;
}

static int
find_unique_match(const struct line_vec *lines, size_t cursor,
                  const struct line_vec *pattern, size_t *match,
                  char *error, size_t error_size)
{
    size_t matches = 0;
    size_t found = 0;

    for (size_t pos = cursor; pos <= lines->n; ++pos) {
        if (line_range_matches(lines, pos, pattern)) {
            ++matches;
            found = pos;
            if (matches > 1u)
                break;
        }
        if (pos == lines->n)
            break;
    }
    if (matches != 1u) {
        set_error(error, error_size,
                  matches ? "update hunk match is ambiguous" :
                            "update hunk did not match");
        errno = EINVAL;
        return -1;
    }
    *match = found;
    return 0;
}

static int
append_line_with_eol(struct snj_buf *out, const char *line, bool crlf)
{
    if (snj_buf_append(out, line, strlen(line)) < 0)
        return -1;
    return crlf ? snj_buf_append(out, "\r\n", 2u) : snj_buf_putc(out, '\n');
}

static int
append_line_range(struct snj_buf *out, const struct line_vec *lines,
                  size_t begin, size_t end, bool crlf)
{
    for (size_t i = begin; i < end; ++i)
        if (append_line_with_eol(out, lines->v[i], crlf) < 0)
            return -1;
    return 0;
}

static int
append_new_lines(struct snj_buf *out, const struct line_vec *lines, bool crlf)
{
    return append_line_range(out, lines, 0u, lines->n, crlf);
}

static int
remove_final_eol(struct snj_buf *out, bool crlf)
{
    size_t n = crlf ? 2u : 1u;
    if (out->len < n)
        return 0;
    out->len -= n;
    return 0;
}

static int
apply_update_hunks(struct patch_op *op, char *error, size_t error_size)
{
    struct line_vec lines = {0};
    size_t cursor = 0;
    bool start_seen = false;
    bool end_seen = false;
    int rc = -1;

    if (parse_file_lines(op->old_bytes, op->old_len, &lines, &op->eol_crlf,
                         &op->final_nl, error, error_size) < 0)
        goto out;
    snj_buf_reset(&op->new_bytes);
    for (size_t i = 0; i < op->hunk_count; ++i) {
        struct patch_hunk *hunk = &op->hunks[i];
        if (end_seen) {
            set_error(error, error_size, "hunks cannot follow an @end insertion");
            errno = EINVAL;
            goto out;
        }
        if (hunk->type == HUNK_START) {
            if (start_seen || cursor != 0u || (lines.n == 0u && end_seen)) {
                set_error(error, error_size, "conflicting @start insertion");
                errno = EINVAL;
                goto out;
            }
            start_seen = true;
            if (append_new_lines(&op->new_bytes, &hunk->new_lines,
                                 op->eol_crlf) < 0)
                goto out;
            continue;
        }
        if (hunk->type == HUNK_END) {
            if (end_seen || (lines.n == 0u && start_seen)) {
                set_error(error, error_size, "conflicting @end insertion");
                errno = EINVAL;
                goto out;
            }
            if (append_line_range(&op->new_bytes, &lines, cursor, lines.n,
                                  op->eol_crlf) < 0 ||
                append_new_lines(&op->new_bytes, &hunk->new_lines,
                                 op->eol_crlf) < 0)
                goto out;
            cursor = lines.n;
            end_seen = true;
            continue;
        }
        {
            size_t match;
            if (find_unique_match(&lines, cursor, &hunk->old_lines, &match,
                                  error, error_size) < 0)
                goto out;
            if (append_line_range(&op->new_bytes, &lines, cursor, match,
                                  op->eol_crlf) < 0 ||
                append_new_lines(&op->new_bytes, &hunk->new_lines,
                                 op->eol_crlf) < 0)
                goto out;
            cursor = match + hunk->old_lines.n;
        }
    }
    if (append_line_range(&op->new_bytes, &lines, cursor, lines.n,
                          op->eol_crlf) < 0)
        goto out;
    if (!op->final_nl)
        remove_final_eol(&op->new_bytes, op->eol_crlf);
    rc = 0;
out:
    line_vec_free(&lines);
    return rc;
}

static int
compute_add_bytes(struct patch_op *op)
{
    snj_buf_reset(&op->new_bytes);
    return append_joined_lines(&op->new_bytes, &op->add_lines);
}

static int
validate_and_compute(struct patch_set *set, int root_fd,
                     char *error, size_t error_size)
{
    for (size_t i = 0; i < set->count; ++i) {
        struct patch_op *op = &set->ops[i];
        if (op->type == OP_ADD) {
            if (validate_add_target(root_fd, op, error, error_size) < 0 ||
                compute_add_bytes(op) < 0)
                return -1;
            set->total_file_bytes += op->new_bytes.len;
        } else if (op->type == OP_UPDATE) {
            if (read_target_file(root_fd, op, error, error_size) < 0 ||
                apply_update_hunks(op, error, error_size) < 0)
                return -1;
            set->total_file_bytes += op->old_len + op->new_bytes.len;
        } else {
            if (validate_delete_target(root_fd, op, error, error_size) < 0)
                return -1;
        }
        if (set->total_file_bytes > PATCH_TOTAL_MAX) {
            set_error(error, error_size,
                      "patch input and output files exceed 48 MiB total");
            errno = EOVERFLOW;
            return -1;
        }
    }
    return 0;
}

static bool
same_identity(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_mtime == b->st_mtime && a->st_size == b->st_size &&
           a->st_mode == b->st_mode;
}

static int
make_temp_file(int parent_fd, char temp[NAME_MAX + 1u])
{
    char id[SNJ_ID_HEX_LEN + 1u];
    int fd;

    for (unsigned int attempt = 0; attempt < 32u; ++attempt) {
        if (snj_random_id(id) < 0)
            return -1;
        (void)snprintf(temp, NAME_MAX + 1u, ".snajpagent-patch-%s.tmp", id);
        fd = openat(parent_fd, temp,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0600);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return -1;
    }
    errno = EEXIST;
    return -1;
}

static int
write_temp_file(int parent_fd, const struct snj_buf *bytes, mode_t mode,
                char temp[NAME_MAX + 1u])
{
    int fd = make_temp_file(parent_fd, temp);
    int saved;

    if (fd < 0)
        return -1;
    if (fchmod(fd, mode) < 0 || snj_write_full(fd, bytes->data, bytes->len) < 0 ||
        snj_sync_file(fd) < 0) {
        saved = errno;
        close(fd);
        (void)unlinkat(parent_fd, temp, 0);
        errno = saved;
        return -1;
    }
    if (close(fd) < 0) {
        saved = errno;
        (void)unlinkat(parent_fd, temp, 0);
        errno = saved;
        return -1;
    }
    return 0;
}

static mode_t
add_mode_from_umask(void)
{
    mode_t old = umask(0);
    (void)umask(old);
    return 0666u & ~old;
}

static int
install_add(int root_fd, const struct patch_op *op,
            char *error, size_t error_size)
{
    char leaf[NAME_MAX + 1u];
    char temp[NAME_MAX + 1u];
    struct stat st;
    int parent_fd = open_parent_dir(root_fd, op->path, leaf, error, error_size);
    int rc = -1;
    int saved;

    if (parent_fd < 0)
        return -1;
    if (fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        set_error(error, error_size, "add target %s appeared before install", op->path);
        errno = EEXIST;
        goto out;
    }
    if (errno != ENOENT)
        goto out;
    if (write_temp_file(parent_fd, &op->new_bytes, add_mode_from_umask(), temp) < 0) {
        set_error(error, error_size, "add target %s could not be staged", op->path);
        goto out;
    }
    if (linkat(parent_fd, temp, parent_fd, leaf, 0) < 0) {
        saved = errno;
        (void)unlinkat(parent_fd, temp, 0);
        errno = saved;
        set_error(error, error_size, "add target %s could not be installed", op->path);
        goto out;
    }
    if (unlinkat(parent_fd, temp, 0) < 0 || snj_sync_dir(parent_fd) < 0) {
        set_error(error, error_size, "add target %s directory sync failed", op->path);
        goto out;
    }
    rc = 0;
out:
    close(parent_fd);
    return rc;
}

static int
install_update(int root_fd, const struct patch_op *op,
               char *error, size_t error_size)
{
    char leaf[NAME_MAX + 1u];
    char temp[NAME_MAX + 1u];
    struct stat st;
    int parent_fd = open_parent_dir(root_fd, op->path, leaf, error, error_size);
    int rc = -1;
    int saved;

    if (parent_fd < 0)
        return -1;
    if (fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
        !same_identity(&op->st, &st)) {
        set_error(error, error_size, "update target %s changed before install", op->path);
        errno = ESTALE;
        goto out;
    }
    if (write_temp_file(parent_fd, &op->new_bytes, op->mode, temp) < 0) {
        set_error(error, error_size, "update target %s could not be staged", op->path);
        goto out;
    }
    if (fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
        !same_identity(&op->st, &st)) {
        saved = errno;
        (void)unlinkat(parent_fd, temp, 0);
        errno = saved ? saved : ESTALE;
        set_error(error, error_size, "update target %s changed before rename", op->path);
        goto out;
    }
    if (renameat(parent_fd, temp, parent_fd, leaf) < 0) {
        saved = errno;
        (void)unlinkat(parent_fd, temp, 0);
        errno = saved;
        set_error(error, error_size, "update target %s could not be installed", op->path);
        goto out;
    }
    if (snj_sync_dir(parent_fd) < 0) {
        set_error(error, error_size, "update target %s directory sync failed", op->path);
        goto out;
    }
    rc = 0;
out:
    close(parent_fd);
    return rc;
}

static int
install_delete(int root_fd, const struct patch_op *op,
               char *error, size_t error_size)
{
    char leaf[NAME_MAX + 1u];
    struct stat st;
    int parent_fd = open_parent_dir(root_fd, op->path, leaf, error, error_size);
    int rc = -1;

    if (parent_fd < 0)
        return -1;
    if (fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
        !same_identity(&op->st, &st)) {
        set_error(error, error_size, "delete target %s changed before install", op->path);
        errno = ESTALE;
        goto out;
    }
    if (unlinkat(parent_fd, leaf, 0) < 0 || snj_sync_dir(parent_fd) < 0) {
        set_error(error, error_size, "delete target %s could not be removed", op->path);
        goto out;
    }
    rc = 0;
out:
    close(parent_fd);
    return rc;
}

static int
op_compare(const void *a, const void *b)
{
    const struct patch_op *const *pa = a;
    const struct patch_op *const *pb = b;
    return strcmp((*pa)->path, (*pb)->path);
}

static int
install_patch(struct patch_set *set, int root_fd,
              char *error, size_t error_size)
{
    struct patch_op **order = calloc(set->count, sizeof(*order));
    int rc = -1;

    if (!order)
        return -1;
    for (size_t i = 0; i < set->count; ++i)
        order[i] = &set->ops[i];
    qsort(order, set->count, sizeof(*order), op_compare);
    for (size_t i = 0; i < set->count; ++i) {
        if (order[i]->type == OP_ADD) {
            if (install_add(root_fd, order[i], error, error_size) < 0)
                goto out;
        } else if (order[i]->type == OP_UPDATE) {
            if (install_update(root_fd, order[i], error, error_size) < 0)
                goto out;
        } else if (install_delete(root_fd, order[i], error, error_size) < 0) {
            goto out;
        }
    }
    rc = 0;
out:
    free(order);
    return rc;
}

static json_t *
empty_excerpt(void)
{
    json_t *out = json_object();
    if (!out ||
        snj_json_set_new(out, "discarded_bytes", json_integer(0)) < 0 ||
        snj_json_set_new(out, "encoding", json_string("utf8")) < 0 ||
        snj_json_set_new(out, "original_bytes", json_integer(0)) < 0 ||
        snj_json_set_new(out, "retained", json_string("")) < 0 ||
        snj_json_set_new(out, "retained_bytes", json_integer(0)) < 0) {
        if (out)
            json_decref(out);
        return NULL;
    }
    return out;
}

static json_t *
patch_result(const char *status, const char *model_text, uint64_t duration_ms)
{
    json_t *out = json_object();
    if (!out ||
        snj_json_set_new(out, "duration_ms", json_integer((json_int_t)duration_ms)) < 0 ||
        snj_json_set_new(out, "exit_code",
                         strcmp(status, "succeeded") == 0 ? json_integer(0) : json_null()) < 0 ||
        snj_json_set_new(out, "handle", json_null()) < 0 ||
        snj_json_set_new(out, "model_text", json_string(model_text)) < 0 ||
        snj_json_set_new(out, "reason", json_null()) < 0 ||
        snj_json_set_new(out, "signal", json_null()) < 0 ||
        snj_json_set_new(out, "status", json_string(status)) < 0 ||
        snj_json_set_new(out, "stderr", empty_excerpt()) < 0 ||
        snj_json_set_new(out, "stdout", empty_excerpt()) < 0) {
        if (out)
            json_decref(out);
        return NULL;
    }
    return out;
}

static json_t *
patch_result_buf(const char *status, struct snj_buf *text,
                 uint64_t duration_ms)
{
    if (snj_buf_terminate(text) < 0)
        return NULL;
    return patch_result(status, (const char *)text->data, duration_ms);
}


static int
preview_appendn(struct snj_buf *out, size_t *used, bool *truncated,
                const char *data, size_t len)
{
    static const char marker[] = "... diff preview truncated ...\n";

    if (*truncated)
        return 0;
    if (len > PATCH_PREVIEW_MAX - *used) {
        size_t marker_len = sizeof(marker) - 1u;
        if (marker_len <= PATCH_PREVIEW_MAX - *used) {
            if (snj_buf_append(out, marker, marker_len) < 0)
                return -1;
            *used += marker_len;
        }
        *truncated = true;
        return 0;
    }
    if (len && snj_buf_append(out, data, len) < 0)
        return -1;
    *used += len;
    return 0;
}

static int
preview_printf(struct snj_buf *out, size_t *used, bool *truncated,
               const char *fmt, ...)
{
    va_list ap;
    va_list copy;
    char *tmp;
    int n;
    int rc;

    if (*truncated)
        return 0;
    va_start(ap, fmt);
    va_copy(copy, ap);
    n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) {
        va_end(ap);
        errno = EINVAL;
        return -1;
    }
    tmp = malloc((size_t)n + 1u);
    if (!tmp) {
        va_end(ap);
        return -1;
    }
    if (vsnprintf(tmp, (size_t)n + 1u, fmt, ap) != n) {
        va_end(ap);
        free(tmp);
        errno = EIO;
        return -1;
    }
    va_end(ap);
    rc = preview_appendn(out, used, truncated, tmp, (size_t)n);
    free(tmp);
    return rc;
}

static int
append_hunk_preview(struct snj_buf *out, size_t *used, bool *truncated,
                    const struct patch_hunk *hunk)
{
    const char *header = hunk->type == HUNK_START ? "@@ @start" :
                         hunk->type == HUNK_END ? "@@ @end" : "@@";

    if (preview_printf(out, used, truncated, "%s\n", header) < 0)
        return -1;
    for (size_t i = 0; i < hunk->preview_lines.n; ++i)
        if (preview_printf(out, used, truncated, "%s\n",
                           hunk->preview_lines.v[i]) < 0)
            return -1;
    return 0;
}

static int
append_patch_preview(struct snj_buf *out, const struct patch_set *set)
{
    size_t used = 0;
    bool truncated = false;

    if (preview_printf(out, &used, &truncated,
            "\nDiff preview (bounded to %u bytes):\n",
            (unsigned int)PATCH_PREVIEW_MAX) < 0)
        return -1;
    for (size_t i = 0; i < set->count; ++i) {
        const struct patch_op *op = &set->ops[i];
        if (op->type == OP_ADD) {
            if (preview_printf(out, &used, &truncated,
                               "*** Add File: %s\n", op->path) < 0)
                return -1;
            for (size_t j = 0; j < op->add_lines.n; ++j)
                if (preview_printf(out, &used, &truncated, "+%s\n",
                                   op->add_lines.v[j]) < 0)
                    return -1;
        } else if (op->type == OP_UPDATE) {
            if (preview_printf(out, &used, &truncated,
                               "*** Update File: %s\n", op->path) < 0)
                return -1;
            for (size_t j = 0; j < op->hunk_count; ++j)
                if (append_hunk_preview(out, &used, &truncated,
                                        &op->hunks[j]) < 0)
                    return -1;
        } else if (preview_printf(out, &used, &truncated,
                                  "*** Delete File: %s\n", op->path) < 0) {
            return -1;
        }
        if (truncated)
            break;
    }
    return 0;
}

static int
append_summary(struct snj_buf *out, const struct patch_set *set)
{
    size_t adds = 0, updates = 0, deletes = 0;
    size_t lines_add = 0, lines_del = 0;

    for (size_t i = 0; i < set->count; ++i) {
        if (set->ops[i].type == OP_ADD)
            ++adds;
        else if (set->ops[i].type == OP_UPDATE)
            ++updates;
        else
            ++deletes;
        lines_add += set->ops[i].added_lines;
        lines_del += set->ops[i].removed_lines;
    }
    if (snj_buf_printf(out,
            "Patch applied. files=%zu added=%zu updated=%zu deleted=%zu lines_added=%zu lines_removed=%zu\n",
            set->count, adds, updates, deletes, lines_add, lines_del) < 0)
        return -1;
    for (size_t i = 0; i < set->count; ++i) {
        const char *kind = set->ops[i].type == OP_ADD ? "add" :
                           set->ops[i].type == OP_UPDATE ? "update" : "delete";
        if (snj_buf_printf(out, "%s %s", kind, set->ops[i].path) < 0)
            return -1;
        if (set->ops[i].type != OP_DELETE &&
            snj_buf_printf(out, " (%zu bytes)", set->ops[i].new_bytes.len) < 0)
            return -1;
        if (snj_buf_putc(out, '\n') < 0)
            return -1;
    }
    return append_patch_preview(out, set);
}

static bool
json_bounded_string(const json_t *object, const char *key, size_t max,
                    const char **out, size_t *len)
{
    json_t *value = json_object_get(object, key);
    const char *s;

    if (!json_is_string(value))
        return false;
    s = json_string_value(value);
    *len = json_string_length(value);
    if (!s || *len > max || strlen(s) != *len)
        return false;
    *out = s;
    return true;
}

static int
workdir_valid(const char *workdir, size_t len, const char *session_workspace,
              char *error, size_t error_size)
{
    struct stat st;
    if (!len || len > SNJ_PATH_MAX_BYTES || workdir[0] != '/' ||
        strcmp(workdir, session_workspace) != 0 ||
        !snj_utf8_valid((const unsigned char *)workdir, len, true) ||
        stat(workdir, &st) < 0 || !S_ISDIR(st.st_mode)) {
        set_error(error, error_size,
                  "apply_patch workdir must be the session workspace directory");
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int
snj_tools_apply_patch(const struct snj_response_item *call,
                      const char *session_workspace,
                      json_t **result,
                      char *error, size_t error_size)
{
    const char *patch;
    const char *workdir;
    size_t patch_len;
    size_t workdir_len;
    char *normalized = NULL;
    char **lines = NULL;
    size_t line_count = 0;
    struct patch_set set = {0};
    struct snj_buf summary;
    uint64_t started = snj_time_ms();
    int root_fd = -1;
    int rc = -1;

    if (result)
        *result = NULL;
    snj_buf_init(&summary, PATCH_MODEL_MAX);
    if (!call || !session_workspace || !result ||
        !json_bounded_string(call->arguments, "patch", PATCH_TEXT_MAX,
                             &patch, &patch_len) ||
        !json_bounded_string(call->arguments, "workdir", SNJ_PATH_MAX_BYTES,
                             &workdir, &workdir_len)) {
        set_error(error, error_size, "invalid apply_patch arguments");
        if (snj_buf_printf(&summary, "Patch rejected: invalid apply_patch arguments.\n") < 0)
            goto out;
        *result = patch_result_buf("patch_rejected", &summary,
                                   snj_time_ms() - started);
        rc = *result ? 0 : -1;
        goto out;
    }
    if (workdir_valid(workdir, workdir_len, session_workspace,
                      error, error_size) < 0 ||
        normalize_patch_text(patch, patch_len, &normalized,
                             error, error_size) < 0 ||
        split_lines(normalized, &lines, &line_count,
                    error, error_size) < 0 ||
        parse_patch_lines(lines, line_count, &set,
                          error, error_size) < 0) {
        if (snj_buf_printf(&summary, "Patch rejected: %s.\n",
                           error[0] ? error : "invalid patch") < 0)
            goto out;
        *result = patch_result_buf("patch_rejected", &summary,
                                   snj_time_ms() - started);
        rc = *result ? 0 : -1;
        goto out;
    }
    root_fd = open(workdir, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (root_fd < 0) {
        set_error(error, error_size, "patch workdir cannot be opened safely");
        if (snj_buf_printf(&summary, "Patch failed during I/O: %s.\n", error) < 0)
            goto out;
        *result = patch_result_buf("io_failed", &summary,
                                   snj_time_ms() - started);
        rc = *result ? 0 : -1;
        goto out;
    }
    if (validate_and_compute(&set, root_fd, error, error_size) < 0) {
        if (snj_buf_printf(&summary, "Patch rejected: %s.\n",
                           error[0] ? error : "validation failed") < 0)
            goto out;
        *result = patch_result_buf("patch_rejected", &summary,
                                   snj_time_ms() - started);
        rc = *result ? 0 : -1;
        goto out;
    }
    if (install_patch(&set, root_fd, error, error_size) < 0) {
        if (snj_buf_printf(&summary, "Patch failed during I/O: %s.\n",
                           error[0] ? error : "installation failed") < 0)
            goto out;
        *result = patch_result_buf("io_failed", &summary,
                                   snj_time_ms() - started);
        rc = *result ? 0 : -1;
        goto out;
    }
    if (append_summary(&summary, &set) < 0)
        goto out;
    *result = patch_result_buf("succeeded", &summary,
                               snj_time_ms() - started);
    rc = *result ? 0 : -1;
out:
    if (root_fd >= 0)
        close(root_fd);
    free(lines);
    free(normalized);
    patch_set_free(&set);
    snj_buf_free(&summary);
    return rc;
}
