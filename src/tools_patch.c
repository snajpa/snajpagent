/* SPDX-License-Identifier: GPL-2.0-only */
#include "tools_patch.h"
#include "tools_file.h"
#include "fs.h"

#include "base.h"
#include "json.h"
#include "snajpagent.h"

#include <errno.h>
#include "snag_jansson.h"
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PATCH_TEXT_MAX (2u * 1024u * 1024u)
#define PATCH_LINE_MAX (1024u * 1024u)
#define PATCH_PATH_MAX 4096u
#define PATCH_OP_MAX 256u
#define PATCH_HUNK_MAX 2048u
#define PATCH_FILE_MAX (16u * 1024u * 1024u)
#define PATCH_TOTAL_MAX (48u * 1024u * 1024u)
#define PATCH_MODEL_MAX (512u * 1024u)
#define PATCH_PREVIEW_MAX (128u * 1024u)

/* Vectors borrow NUL-terminated lines from the patch or target buffer. */
struct line_vec {
    char **v;
    size_t n;
};

enum patch_op_type {
    OP_ADD, OP_UPDATE, OP_DELETE };

enum hunk_type {
    HUNK_NORMAL, HUNK_START, HUNK_END };

struct patch_hunk {
    enum hunk_type type;
    char **lines; /* Borrowed slice, including context/add/remove markers. */
    size_t count;
    size_t old_count;
};

struct patch_op {
    enum patch_op_type type;
    const char *path;
    char **add_lines;
    struct patch_hunk *hunks;
    size_t hunk_count;
    struct snag_buf old_bytes, new_bytes;
    bool eol_crlf;
    bool final_nl;
    struct snag_permissions permissions;
    snag_file_info st;
    size_t added_lines;
    size_t removed_lines;
};

struct patch_set {
    struct patch_op *ops;
    size_t count;
    struct patch_hunk *hunks;
    size_t hunk_total;
    size_t total_file_bytes;
};

static void
op_free(struct patch_op *op)
{
    if (!op) return;
    snag_buf_free(&op->old_bytes);
    snag_permissions_free(&op->permissions);
    snag_buf_free(&op->new_bytes);
    memset(op, 0, sizeof(*op));
}

static void
patch_set_free(struct patch_set *set)
{
    if (!set) return;
    for (size_t i = 0; i < set->count; ++i) op_free(&set->ops[i]);
    free(set->ops);
    free(set->hunks);
    memset(set, 0, sizeof(*set));
}

static bool
starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool
is_file_header(const char *line)
{
    return starts_with(line, "*** Add File: ") || starts_with(line, "*** Update File: ") ||
           starts_with(line, "*** Delete File: ");
}

static bool
is_hunk_header(const char *line)
{
    return strcmp(line, "@@") == 0 || starts_with(line, "@@ ");
}

int
snag_file_path_valid(const char *path, char *error, size_t error_size)
{
    size_t len = strlen(path);
    const char *p = path;
    const char *component = path;

    if (!len || len > PATCH_PATH_MAX || path[0] == '/' ||
        !snag_utf8_valid((const unsigned char *)path, len, true)) {
        return snag_fail(error, error_size, EINVAL, "patch paths must be bounded relative UTF-8 paths inside workdir (no absolute paths or .. components)");
    }
    if (len >= 2u && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':') {
        return snag_fail(error, error_size, EINVAL, "patch path uses a drive-prefix form");
    }
    if (starts_with(path, "//"))
        return snag_fail(error, error_size, EINVAL, "patch path uses a UNC-like form");
    while (*p) {
        if ((unsigned char)*p < 0x20u || (unsigned char)*p == 0x7fu ||
            *p == '\\') {
            return snag_fail(error, error_size, EINVAL, "patch path contains a rejected byte");
        }
        if (*p == '/') {
            if (p == component || (p - component == 1 && component[0] == '.') ||
                (p - component == 2 && component[0] == '.' && component[1] == '.')) {
                return snag_fail(error, error_size, EINVAL, "patch path contains an invalid component");
            }
            component = p + 1;
        }
        ++p;
    }
    if (p == component || (p - component == 1 && component[0] == '.') ||
        (p - component == 2 && component[0] == '.' && component[1] == '.')) {
        return snag_fail(error, error_size, EINVAL, "patch path contains an invalid final component");
    }
    return 0;
}

static int
normalize_patch_text(const char *patch, size_t len, char **out, char *error, size_t error_size)
{
    char *text;
    size_t written = 0;
    size_t line_len = 0;

    *out = NULL;
    if (len > PATCH_TEXT_MAX || !snag_utf8_valid((const unsigned char *)patch, len, true))
        return snag_fail(error, error_size, EINVAL, "patch must be bounded UTF-8 without NUL");
    text = malloc(len + 1u);
    if (!text) return -1;
    for (size_t i = 0; i < len; ++i) {
        char c = patch[i];

        if (c == '\r') {
            if (i + 1u >= len || patch[++i] != '\n') {
                (void)snag_fail(error, error_size, EINVAL, "patch contains a bare carriage return");
                free(text);
                return -1;
            }
            c = '\n';
        }
        line_len = c == '\n' ? 0u : line_len + 1u;
        if (line_len > PATCH_LINE_MAX) {
            (void)snag_fail(error, error_size, EOVERFLOW, "patch line exceeds 1 MiB");
            free(text);
            return -1;
        }
        text[written++] = c;
    }
    text[written] = '\0';
    *out = text;
    return 0;
}

static int
split_lines(char *text, size_t len, struct line_vec *lines)
{
    size_t count = len && text[len - 1u] != '\n';
    for (size_t i = 0u; i < len; ++i) count += text[i] == '\n';
    if (count > PATCH_LINE_MAX) return snag_errno(EOVERFLOW);
    lines->v = malloc((count ? count : 1u) * sizeof(*lines->v));
    if (!lines->v) return -1;
    for (size_t start = 0u, i = 0u; i <= len; ++i) {
        if (i != len && text[i] != '\n') continue;
        text[i] = '\0';
        if (i != len || i != start) lines->v[lines->n++] = text + start;
        start = i + 1u;
    }
    return 0;
}

static int
check_duplicate_path(const struct patch_set *set, const char *path, char *error, size_t error_size)
{
    for (size_t i = 0; i < set->count; ++i) {
        if (strcmp(set->ops[i].path, path) == 0) {
            return snag_fail(error, error_size, EINVAL, "patch contains duplicate target path");
        }
    }
    return 0;
}

static int
parse_hunk_header(const char *line, enum hunk_type *type, char *error, size_t error_size)
{
    *type = HUNK_NORMAL;
    if (strcmp(line, "@@") == 0) return 0;
    if (!starts_with(line, "@@ ") || line[3] == '\0')
        return snag_fail(error, error_size, EINVAL, "invalid hunk header");
    if (strcmp(line + 3, "@start") == 0) *type = HUNK_START;
    else if (strcmp(line + 3, "@end") == 0) *type = HUNK_END;
    return 0;
}

static int
parse_patch_lines(char **lines, size_t line_count, struct patch_set *set, char *error, size_t error_size)
{
    size_t i = 1, ops = 0u, hunks = 0u;

    if (line_count < 2u) return snag_fail(error, error_size, EINVAL, "patch is missing required frame");
    if (strcmp(lines[0], "*** Begin Patch") != 0 || strcmp(lines[line_count - 1u], "*** End Patch") != 0)
        return snag_fail(error, error_size, EINVAL, "patch frame must begin and end exactly");
    for (size_t n = 1u; n + 1u < line_count; ++n) {
        if (is_file_header(lines[n]) && ops < PATCH_OP_MAX) ++ops;
        if (is_hunk_header(lines[n]) && hunks < PATCH_HUNK_MAX) ++hunks;
    }
    set->ops = calloc(ops ? ops : 1u, sizeof(*set->ops));
    set->hunks = calloc(hunks ? hunks : 1u, sizeof(*set->hunks));
    if (!set->ops || !set->hunks) return -1;

    while (i + 1u < line_count) {
        struct patch_op *op;
        const char *path;
        enum patch_op_type type;

        if (starts_with(lines[i], "*** Add File: ")) {
            type = OP_ADD;
            path = lines[i] + strlen("*** Add File: ");
        } else if (starts_with(lines[i], "*** Delete File: ")) {
            type = OP_DELETE;
            path = lines[i] + strlen("*** Delete File: ");
        } else if (starts_with(lines[i], "*** Update File: ")) {
            type = OP_UPDATE;
            path = lines[i] + strlen("*** Update File: ");
        } else {
            return snag_fail(error, error_size, EINVAL, "expected a file operation header");
        }
        if (snag_file_path_valid(path, error, error_size) < 0 || check_duplicate_path(set, path, error, error_size) < 0)
            return -1;
        if (set->count >= PATCH_OP_MAX) return snag_errno(EOVERFLOW);
        op = &set->ops[set->count++];
        op->old_bytes.max = PATCH_FILE_MAX + 1u;
        op->new_bytes.max = PATCH_FILE_MAX;
        op->hunks = set->hunks + set->hunk_total;
        op->type = type;
        op->path = path;
        ++i;
        if (type == OP_ADD) {
            op->add_lines = lines + i;
            while (i + 1u < line_count && !is_file_header(lines[i])) {
                if (lines[i][0] != '+') {
                    return snag_fail(error, error_size, EINVAL, "add-file body lines must start with +");
                }
                ++op->added_lines;
                ++i;
            }
        } else if (type == OP_DELETE) {
            if (i + 1u < line_count && !is_file_header(lines[i])) {
                return snag_fail(error, error_size, EINVAL, "delete-file sections cannot have a body");
            }
        } else {
            while (i + 1u < line_count && !is_file_header(lines[i])) {
                struct patch_hunk *hunk;
                bool changed = false;
                if (!is_hunk_header(lines[i])) return -1;
                if (set->hunk_total >= PATCH_HUNK_MAX) return snag_errno(EOVERFLOW);
                hunk = &set->hunks[set->hunk_total++];
                ++op->hunk_count;
                if (parse_hunk_header(lines[i], &hunk->type, error, error_size) < 0) return -1;
                ++i;
                hunk->lines = lines + i;
                while (i + 1u < line_count && !is_file_header(lines[i]) && !is_hunk_header(lines[i])) {
                    if (hunk->type == HUNK_START || hunk->type == HUNK_END) {
                        if (lines[i][0] != '+') {
                            return snag_fail(error, error_size, EINVAL,
                                      "anchored hunks may contain only + lines");
                        }
                    } else if (lines[i][0] != ' ' && lines[i][0] != '-' && lines[i][0] != '+') {
                        return snag_fail(error, error_size, EINVAL,
                                  "update hunk body lines must start with space, -, or +");
                    }
                    ++hunk->count;
                    if (lines[i][0] != '+') ++hunk->old_count;
                    if (lines[i][0] == '+' || lines[i][0] == '-') {
                        changed = true;
                        if (lines[i][0] == '+') ++op->added_lines;
                        else ++op->removed_lines;
                    }
                    ++i;
                }
                if ((hunk->type == HUNK_START || hunk->type == HUNK_END) && hunk->count == 0u) {
                    return snag_fail(error, error_size, EINVAL,
                              "anchored hunks must insert at least one line");
                }
                if (hunk->type == HUNK_NORMAL && (!changed || hunk->old_count == 0u)) {
                    return snag_fail(error, error_size, EINVAL,
                              "normal hunks need a nonempty old pattern and a change");
                }
            }
            if (op->hunk_count == 0u) {
                return snag_fail(error, error_size, EINVAL, "update-file sections need at least one hunk");
            }
        }
    }
    if (set->count == 0u) return snag_fail(error, error_size, EINVAL, "patch contains no file operations");
    return 0;
}

int
snag_file_parent(int root_fd, const char *path, char leaf[SNAG_NAME_MAX_BYTES + 1u],
                char *error, size_t error_size)
{
    int dir_fd = snag_dup_read(root_fd);
    const char *p = path;
    const char *slash;

    if (dir_fd < 0) return -1;
    for (;;) {
        size_t len;
        slash = strchr(p, '/');
        len = slash ? (size_t)(slash - p) : strlen(p);
        if (len > SNAG_NAME_MAX_BYTES) {
            close(dir_fd);
            return snag_fail(error, error_size, ENAMETOOLONG, "patch path component is too long");
        }
        memcpy(leaf, p, len);
        leaf[len] = '\0';
        if (!slash) return dir_fd;
        int next_fd = snag_open_read_at(dir_fd, leaf, true);
        if (next_fd < 0) {
            close(dir_fd);
            return snag_errorf(error, error_size,
                      "patch parent directory cannot be opened without following symlinks");
        }
        close(dir_fd);
        dir_fd = next_fd;
        p = slash + 1;
    }
}

static int
prepare_target(int root_fd, struct patch_op *op, char *error, size_t error_size)
{
    char leaf[SNAG_NAME_MAX_BYTES + 1u];
    int parent_fd = snag_file_parent(root_fd, op->path, leaf, error, error_size);
    int fd = -1, rc = -1;

    if (parent_fd < 0) return -1;
    if (op->type == OP_ADD) {
        snag_file_info st;
        if (snag_lstat_at(parent_fd, leaf, &st) == 0) {
            (void)snag_fail(error, error_size, EEXIST, "add target %s already exists", op->path);
            goto out;
        }
        if (errno != ENOENT) {
            snag_errorf(error, error_size, "add target %s cannot be checked", op->path);
            goto out;
        }
    } else if (op->type == OP_DELETE) {
        if (snag_lstat_at(parent_fd, leaf, &op->st) < 0) {
            snag_errorf(error, error_size, "delete target %s cannot be checked", op->path);
            goto out;
        }
        if (!S_ISREG(op->st.st_mode)) {
            (void)snag_fail(error, error_size, EINVAL, "delete target %s is not a regular file", op->path);
            goto out;
        }
    } else {
        fd = snag_open_read_security_at(parent_fd, leaf, false);
        if (fd < 0) {
            snag_errorf(error, error_size, "patch target %s cannot be opened", op->path);
            goto out;
        }
        if (snag_fstat(fd, &op->st) < 0) goto out;
        if (!S_ISREG(op->st.st_mode) || op->st.st_size > (int64_t)PATCH_FILE_MAX) {
            (void)snag_fail(error, error_size, EINVAL,
                "patch target %s is not a regular file within 16 MiB", op->path);
            goto out;
        }
        if (snag_permissions_capture(fd, &op->permissions) < 0) goto out;
        if (snag_buf_read(&op->old_bytes, fd) < 0 || snag_buf_terminate(&op->old_bytes) < 0) {
            snag_errorf(error, error_size, "patch target %s cannot be read", op->path);
            goto out;
        }
    }
    rc = 0;
out:
    if (fd >= 0) close(fd);
    close(parent_fd);
    return rc;
}

static int
parse_file_lines(char *bytes, size_t len, struct line_vec *lines,
                 bool *crlf, bool *final_nl, char *error, size_t error_size)
{
    enum { STYLE_NONE, STYLE_LF, STYLE_CRLF } style = STYLE_NONE;

    if (!snag_utf8_valid((const unsigned char *)bytes, len, true))
        return snag_fail(error, error_size, EINVAL, "update target is not strict UTF-8 without NUL");
    *final_nl = len && bytes[len - 1u] == '\n';
    for (size_t i = 0; i < len; ++i) {
        bool is_crlf = bytes[i] == '\r';

        if (!is_crlf && bytes[i] != '\n') continue;
        if ((is_crlf && (i + 1u >= len || bytes[i + 1u] != '\n')) ||
            (style != STYLE_NONE && (style == STYLE_CRLF) != is_crlf)) {
            return snag_fail(error, error_size, EINVAL,
                        "update target has mixed or bare carriage-return line endings");
        }
        style = is_crlf ? STYLE_CRLF : STYLE_LF;
        if (is_crlf) bytes[i++] = '\0';
    }
    *crlf = style == STYLE_CRLF;
    return split_lines(bytes, len, lines);
}

static bool
line_range_matches(const struct line_vec *lines, size_t pos, const struct patch_hunk *pattern)
{
    if (pos > lines->n || pattern->old_count > lines->n - pos) return false;
    for (size_t i = 0; i < pattern->count; ++i)
        if (pattern->lines[i][0] != '+' && strcmp(lines->v[pos++], pattern->lines[i] + 1u) != 0) return false;
    return true;
}

static size_t
find_unique_match(const struct line_vec *lines, size_t cursor, const struct patch_hunk *pattern,
                  char *error, size_t error_size)
{
    size_t matches = 0;
    size_t found = 0;

    for (size_t pos = cursor; pos <= lines->n; ++pos) {
        if (line_range_matches(lines, pos, pattern)) {
            ++matches;
            found = pos;
            if (matches > 1u) break;
        }
        if (pos == lines->n) break;
    }
    if (matches != 1u) {
        (void)snag_fail(error, error_size, EINVAL, matches ? "update hunk match is ambiguous" :
                            "update hunk did not match");
        return SIZE_MAX;
    }
    return found;
}

static int
append_line_with_eol(struct snag_buf *out, const char *line, bool crlf)
{
    if (snag_buf_append(out, line, strlen(line)) < 0) return -1;
    return crlf ? snag_buf_append(out, "\r\n", 2u) : snag_buf_putc(out, '\n');
}

static int
append_line_range(struct snag_buf *out, const struct line_vec *lines, size_t begin, size_t end, bool crlf)
{
    for (size_t i = begin; i < end; ++i)
        if (append_line_with_eol(out, lines->v[i], crlf) < 0) return -1;
    return 0;
}

static int
append_new_lines(struct snag_buf *out, char *const *lines, size_t count, bool crlf)
{
    for (size_t i = 0; i < count; ++i)
        if (lines[i][0] != '-' && append_line_with_eol(out, lines[i] + 1u, crlf) < 0) return -1;
    return 0;
}

static void
remove_final_eol(struct snag_buf *out, bool crlf)
{
    size_t n = crlf ? 2u : 1u;
    if (out->len >= n) out->len -= n;
}

static int
apply_update_hunks(struct patch_op *op, char *error, size_t error_size)
{
    struct line_vec lines = {0};
    size_t cursor = 0;
    bool start_seen = false;
    bool end_seen = false;
    int rc = -1;

    if (parse_file_lines((char *)op->old_bytes.data, op->old_bytes.len, &lines, &op->eol_crlf,
                         &op->final_nl, error, error_size) < 0) goto out;
    snag_buf_reset(&op->new_bytes);
    for (size_t i = 0; i < op->hunk_count; ++i) {
        struct patch_hunk *hunk = &op->hunks[i];
        if (end_seen) {
            (void)snag_fail(error, error_size, EINVAL, "hunks cannot follow an @end insertion");
            goto out;
        }
        size_t match = cursor;
        if (hunk->type == HUNK_START) {
            if (start_seen || cursor != 0u) {
                (void)snag_fail(error, error_size, EINVAL, "conflicting @start insertion");
                goto out;
            }
            start_seen = true;
        } else if (hunk->type == HUNK_END) {
            if (lines.n == 0u && start_seen) {
                (void)snag_fail(error, error_size, EINVAL, "conflicting @end insertion");
                goto out;
            }
            match = lines.n;
            end_seen = true;
        } else {
            match = find_unique_match(&lines, cursor, hunk, error, error_size);
            if (match == SIZE_MAX) goto out;
        }
        if (append_line_range(&op->new_bytes, &lines, cursor, match, op->eol_crlf) < 0 ||
            append_new_lines(&op->new_bytes, hunk->lines, hunk->count, op->eol_crlf) < 0) goto out;
        cursor = match + hunk->old_count;
    }
    if (append_line_range(&op->new_bytes, &lines, cursor, lines.n, op->eol_crlf) < 0) goto out;
    if (!op->final_nl) remove_final_eol(&op->new_bytes, op->eol_crlf);
    rc = 0;
out: free(lines.v);
    return rc;
}

static int
validate_and_compute(struct patch_set *set, int root_fd, char *error, size_t error_size)
{
    for (size_t i = 0; i < set->count; ++i) {
        struct patch_op *op = &set->ops[i];
        if (prepare_target(root_fd, op, error, error_size) < 0) return -1;
        if (op->type == OP_ADD) {
            if (append_new_lines(&op->new_bytes, op->add_lines, op->added_lines, false) < 0) return -1;
            set->total_file_bytes += op->new_bytes.len;
        } else if (op->type == OP_UPDATE) {
            if (apply_update_hunks(op, error, error_size) < 0) return -1;
            set->total_file_bytes += op->old_bytes.len + op->new_bytes.len;
        }
        if (set->total_file_bytes > PATCH_TOTAL_MAX) {
            return snag_fail(error, error_size, EOVERFLOW,
                      "patch input and output files exceed 48 MiB total");
        }
    }
    return 0;
}

static bool
same_identity(const snag_file_info *a, const snag_file_info *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_mtime == b->st_mtime && a->st_size == b->st_size && a->st_mode == b->st_mode;
}

static bool
unchanged_target(int parent_fd, const char *leaf, const struct patch_op *op)
{
    int fd = snag_open_read_security_at(parent_fd, leaf, false);
    snag_file_info current;

    if (fd < 0) return false;
    bool same = snag_fstat(fd, &current) == 0 && same_identity(&op->st, &current) &&
                snag_permissions_match(fd, &op->permissions) == 1;
    int saved = errno;
    (void)close(fd);
    errno = saved;
    return same;
}

static int
make_temp_file(int parent_fd, const struct snag_permissions *permissions, char temp[SNAG_NAME_MAX_BYTES + 1u])
{
    char id[SNAG_ID_HEX_LEN + 1u];
    int fd;

    for (unsigned int attempt = 0; attempt < 32u; ++attempt) {
        if (snag_random_id(id) < 0) return -1;
        (void)snprintf(temp, SNAG_NAME_MAX_BYTES + 1u, "." SNAJPAGENT_NAME "-patch-%s.tmp", id);
        fd = permissions ? snag_create_private_at(parent_fd, temp, true) :
                           snag_create_output_at(parent_fd, temp);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    return snag_errno(EEXIST);
}

int
snag_file_stage(int parent_fd, const struct snag_buf *bytes, const struct snag_permissions *permissions,
                char temp[SNAG_NAME_MAX_BYTES + 1u])
{
    int fd = make_temp_file(parent_fd, permissions, temp);
    int saved;

    if (fd < 0) return -1;
    if (snag_write_full(fd, bytes->data, bytes->len) < 0 ||
        (permissions && snag_permissions_apply(fd, permissions) < 0) || snag_sync_file(fd) < 0) {
        saved = errno;
        close(fd);
        (void)snag_unlink_at(parent_fd, temp, false);
        errno = saved;
        return -1;
    }
    if (close(fd) < 0) {
        saved = errno;
        (void)snag_unlink_at(parent_fd, temp, false);
        errno = saved;
        return -1;
    }
    return 0;
}

static int
install_op(int root_fd, const struct patch_op *op, char *error, size_t error_size)
{
    char leaf[SNAG_NAME_MAX_BYTES + 1u];
    char temp[SNAG_NAME_MAX_BYTES + 1u] = {0};
    const char *kind = op->type == OP_ADD ? "add" : op->type == OP_UPDATE ? "update" : "delete";
    const char *failure = "changed before install";
    snag_file_info st;
    int parent_fd = snag_file_parent(root_fd, op->path, leaf, error, error_size);
    int rc = -1, saved;

    if (parent_fd < 0) return -1;
    if (op->type == OP_ADD) {
        if (snag_lstat_at(parent_fd, leaf, &st) == 0) {
            failure = "appeared before install";
            errno = EEXIST;
            goto fail;
        }
        if (errno != ENOENT) goto out;
    } else if (op->type == OP_UPDATE ? !unchanged_target(parent_fd, leaf, op) :
               (snag_lstat_at(parent_fd, leaf, &st) < 0 || !same_identity(&op->st, &st))) {
        errno = ESTALE;
        goto fail;
    }
    if (op->type == OP_DELETE) {
        failure = "could not be removed";
        if (snag_unlink_at(parent_fd, leaf, false) < 0 || snag_sync_dir(parent_fd) < 0) goto fail;
    } else {
        failure = "could not be staged";
        if (snag_file_stage(parent_fd, &op->new_bytes,
                             op->type == OP_UPDATE ? &op->permissions : NULL, temp) < 0) {
            temp[0] = '\0'; /* The stage writer owns cleanup on failure. */
            goto fail;
        }
        if (op->type == OP_UPDATE && !unchanged_target(parent_fd, leaf, op)) {
            failure = "changed before rename";
            if (!errno) errno = ESTALE;
            goto fail;
        }
        failure = "could not be installed";
        if ((op->type == OP_ADD ? snag_link_at(parent_fd, temp, parent_fd, leaf) :
             snag_rename_at(parent_fd, temp, parent_fd, leaf)) < 0) goto fail;
        failure = "directory sync failed";
        if (op->type == OP_ADD && snag_unlink_at(parent_fd, temp, false) < 0) {
            temp[0] = '\0'; /* Preserve the failed-unlink result, without retry. */
            goto fail;
        }
        temp[0] = '\0';
        if (snag_sync_dir(parent_fd) < 0) goto fail;
    }
    rc = 0;
    goto out;
fail: saved = errno;
    if (temp[0]) (void)snag_unlink_at(parent_fd, temp, false);
    errno = saved;
    snag_errorf(error, error_size, "%s target %s %s", kind, op->path, failure);
out: close(parent_fd);
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
install_patch(struct patch_set *set, int root_fd, char *error, size_t error_size)
{
    struct patch_op **order = calloc(set->count, sizeof(*order));
    int rc = -1;

    if (!order) return -1;
    for (size_t i = 0; i < set->count; ++i) order[i] = &set->ops[i];
    qsort(order, set->count, sizeof(*order), op_compare);
    for (size_t i = 0; i < set->count; ++i) {
        if (install_op(root_fd, order[i], error, error_size) < 0) goto out;
    }
    rc = 0;
out: free(order);
    return rc;
}

static json_t *
patch_result_buf(const char *status, struct snag_buf *text, uint64_t duration_ms)
{
    if (snag_buf_terminate(text) < 0) return NULL;
    return snag_tool_result(status, NULL, (const char *)text->data, strcmp(status, "succeeded") == 0 ? 0 : -1,
                           duration_ms);
}

static int
preview_appendn(struct snag_buf *out, size_t *used, bool *truncated, const char *data, size_t len)
{
    static const char marker[] = "… diff preview truncated\n";

    if (*truncated) return 0;
    if (len > PATCH_PREVIEW_MAX - *used) {
        size_t marker_len = sizeof(marker) - 1u;
        if (marker_len <= PATCH_PREVIEW_MAX - *used) {
            if (snag_buf_append(out, marker, marker_len) < 0) return -1;
            *used += marker_len;
        }
        *truncated = true;
        return 0;
    }
    if (len && snag_buf_append(out, data, len) < 0) return -1;
    *used += len;
    return 0;
}

static int
preview_printf(struct snag_buf *out, size_t *used, bool *truncated, const char *fmt, ...)
{
    va_list ap;
    int rc;

    if (*truncated) return 0;
    struct snag_buf text = {.max = SIZE_MAX};
    va_start(ap, fmt);
    rc = snag_buf_vprintf(&text, fmt, ap);
    va_end(ap);
    if (rc == 0) rc = preview_appendn(out, used, truncated, (const char *)text.data, text.len);
    snag_buf_free(&text);
    return rc;
}

static int
append_hunk_preview(struct snag_buf *out, size_t *used, bool *truncated, const struct patch_hunk *hunk)
{
    const char *header = hunk->type == HUNK_START ? "@@ @start" : hunk->type == HUNK_END ? "@@ @end" : "@@";

    if (preview_printf(out, used, truncated, "%s\n", header) < 0) return -1;
    for (size_t i = 0; i < hunk->count; ++i)
        if (preview_printf(out, used, truncated, "%s\n", hunk->lines[i]) < 0) return -1;
    return 0;
}

static int
append_patch_preview(struct snag_buf *out, const struct patch_set *set)
{
    size_t used = 0;
    bool truncated = false;

    if (preview_printf(out, &used, &truncated, "\nDiff preview (bounded to %u bytes):\n",
            (unsigned int)PATCH_PREVIEW_MAX) < 0) return -1;
    for (size_t i = 0; i < set->count; ++i) {
        const struct patch_op *op = &set->ops[i];
        if (op->type == OP_ADD) {
            if (preview_printf(out, &used, &truncated, "*** Add File: %s\n", op->path) < 0) return -1;
            for (size_t j = 0; j < op->added_lines; ++j)
                if (preview_printf(out, &used, &truncated, "%s\n", op->add_lines[j]) < 0) return -1;
        } else if (op->type == OP_UPDATE) {
            if (preview_printf(out, &used, &truncated, "*** Update File: %s\n", op->path) < 0) return -1;
            for (size_t j = 0; j < op->hunk_count; ++j)
                if (append_hunk_preview(out, &used, &truncated, &op->hunks[j]) < 0) return -1;
        } else if (preview_printf(out, &used, &truncated, "*** Delete File: %s\n", op->path) < 0) {
            return -1;
        }
        if (truncated) break;
    }
    return 0;
}

static int
append_summary(struct snag_buf *out, const struct patch_set *set)
{
    size_t adds = 0, updates = 0, deletes = 0;
    size_t lines_add = 0, lines_del = 0;

    for (size_t i = 0; i < set->count; ++i) {
        if (set->ops[i].type == OP_ADD) ++adds;
        else if (set->ops[i].type == OP_UPDATE) ++updates;
        else ++deletes;
        lines_add += set->ops[i].added_lines;
        lines_del += set->ops[i].removed_lines;
    }
    if (snag_buf_printf(out,
            "Patch applied. files=%zu added=%zu updated=%zu deleted=%zu lines_added=%zu lines_removed=%zu\n",
            set->count, adds, updates, deletes, lines_add, lines_del) < 0) return -1;
    for (size_t i = 0; i < set->count; ++i) {
        const char *kind = set->ops[i].type == OP_ADD ? "add" :
                           set->ops[i].type == OP_UPDATE ? "update" : "delete";
        if (snag_buf_printf(out, "%s %s", kind, set->ops[i].path) < 0) return -1;
        if (set->ops[i].type != OP_DELETE &&
            snag_buf_printf(out, " (%zu bytes)", set->ops[i].new_bytes.len) < 0) return -1;
        if (snag_buf_putc(out, '\n') < 0) return -1;
    }
    return append_patch_preview(out, set);
}

static int
workdir_valid(const char *workdir, size_t len, const char *session_workspace, char *error, size_t error_size)
{
    snag_file_info st;
    if (!len || len > SNAG_PATH_MAX_BYTES || workdir[0] != '/' || strcmp(workdir, session_workspace) != 0 ||
        !snag_utf8_valid((const unsigned char *)workdir, len, true) ||
        snag_stat(workdir, &st) < 0 || !S_ISDIR(st.st_mode)) {
        return snag_fail(error, error_size, EINVAL,
                  "apply_patch workdir must be the session workspace directory");
    }
    return 0;
}

int
snag_tools_apply_patch(const struct snag_response_item *call, const char *session_workspace,
                      json_t **result, char *error, size_t error_size)
{
    const char *patch;
    const char *workdir;
    char *normalized = NULL;
    struct line_vec lines = {0};
    struct patch_set set = {0};
    const char *status = "patch_rejected";
    uint64_t started = snag_time_ms();
    int root_fd = -1;
    int rc = -1;

    if (!result) return snag_fail(error, error_size, EINVAL, "invalid apply_patch result destination");
    *result = NULL;
    struct snag_buf summary = {.max = PATCH_MODEL_MAX};
    if (!call || !session_workspace ||
        !snag_json_arg_keys(call->arguments, "patch", "workdir", error, error_size) ||
        !snag_json_arg_text(call->arguments, "patch", 0u, PATCH_TEXT_MAX, false, &patch, error, error_size) ||
        !snag_json_arg_text(call->arguments, "workdir", 1u, SNAG_PATH_MAX_BYTES,
                            true, &workdir, error, error_size)) {
        if (snag_buf_printf(&summary, "Patch rejected: %s\n", *error ? error : "invalid arguments") < 0)
            goto out;
        goto result;
    }
    if (!workdir) workdir = session_workspace;
    if (workdir_valid(workdir, strlen(workdir), session_workspace, error, error_size) < 0 ||
        normalize_patch_text(patch, strlen(patch), &normalized, error, error_size) < 0 ||
        split_lines(normalized, strlen(normalized), &lines) < 0 ||
        parse_patch_lines(lines.v, lines.n, &set, error, error_size) < 0) {
        if (snag_buf_printf(&summary, "Patch rejected: %s.\n", error[0] ? error : "invalid patch") < 0)
            goto out;
        goto result;
    }
    root_fd = snag_open_read(workdir, true);
    if (root_fd < 0) {
        snag_errorf(error, error_size, "patch workdir cannot be opened safely");
        if (snag_buf_printf(&summary, "Patch failed during I/O: %s.\n", error) < 0) goto out;
        status = "io_failed";
        goto result;
    }
    if (validate_and_compute(&set, root_fd, error, error_size) < 0) {
        if (snag_buf_printf(&summary, "Patch rejected: %s.\n", error[0] ? error : "validation failed") < 0)
            goto out;
        goto result;
    }
    if (install_patch(&set, root_fd, error, error_size) < 0) {
        if (snag_buf_printf(&summary, "Patch failed during I/O: %s.\n",
                           error[0] ? error : "installation failed") < 0) goto out;
        status = "io_failed";
        goto result;
    }
    if (append_summary(&summary, &set) < 0) goto out;
    status = "succeeded";
result: *result = patch_result_buf(status, &summary, snag_time_ms() - started);
    rc = *result ? 0 : -1;
out:
    if (root_fd >= 0) close(root_fd);
    patch_set_free(&set);
    free(lines.v);
    free(normalized);
    snag_buf_free(&summary);
    return rc;
}
