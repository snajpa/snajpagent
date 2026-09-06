/* SPDX-License-Identifier: GPL-2.0-only */
#include "tools.h"
#include "base.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RO_OUTPUT (256u * 1024u)
#define RO_LINE (64u * 1024u)
#define RO_SCAN (128u * 1024u * 1024u)
#define RO_ENTRIES 100000u
#define RO_DEPTH 32u

struct read_query {
    struct snag_buf output;
    snag_tool_pump_fn pump;
    void *opaque;
    regex_t regex;
    const char *pattern;
    const char *problem;
    size_t bytes, entries, seen, emitted, offset, limit;
    size_t skipped;
    uint64_t start, end;
    bool grep, read, recursive, literal, ignore_case, compiled, more;
    int interrupted;
};

static int
checkpoint(struct read_query *q)
{
    if (q->pump && (q->interrupted = q->pump(q->opaque, 0u)) != 0) {
        q->problem = "Inspection interrupted; results are incomplete.";
        return -1;
    }
    return 0;
}

/* Reject special files before opening; O_NONBLOCK also prevents FIFO races.
 * O_NOFOLLOW on every component prevents symlink traversal, including parents. */
static int
open_entry(int parent, const char *name)
{
    struct stat before, after;
    int fd;

    if (fstatat(parent, name, &before, AT_SYMLINK_NOFOLLOW) < 0)
        return -1;
    if (!S_ISREG(before.st_mode) && !S_ISDIR(before.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    fd = openat(parent, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return -1;
    if (fstat(fd, &after) < 0 || before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino ||
        (!S_ISREG(after.st_mode) && !S_ISDIR(after.st_mode))) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}

static int
open_path(const char *workspace, const char *path)
{
    char *copy, *part, *save = NULL;
    struct snag_buf full;
    int fd = -1;
    bool absolute = snag_path_root_len(path) != 0u;

    snag_buf_init(&full, 8192u);
    if (snag_buf_printf(&full, "%s%s%s", absolute ? "" : workspace,
                       absolute ? "" : "/", path) < 0 ||
        snag_buf_terminate(&full) < 0)
        goto out;
    copy = (char *)full.data;
    fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (part = strtok_r(copy, "/", &save); fd >= 0 && part;
         part = strtok_r(NULL, "/", &save)) {
        int next = open_entry(fd, part);
        close(fd);
        fd = next;
    }
out:
    snag_buf_free(&full);
    return fd;
}

static bool
number_arg(const json_t *args, const char *name, uint64_t fallback,
            uint64_t min, uint64_t max, uint64_t *value)
{
    json_t *v = json_object_get(args, name);

    *value = fallback;
    if (json_is_null(v))
        return true;
    return snag_json_integer_u64(args, name, value) == 0 &&
           *value >= min && *value <= max;
}

static bool
bool_arg(const json_t *args, const char *name, bool fallback, bool *value)
{
    json_t *v = json_object_get(args, name);

    *value = json_is_null(v) ? fallback : json_is_true(v);
    return json_is_null(v) || json_is_boolean(v);
}

static bool
literal_match(const char *line, const char *pattern, bool ignore_case)
{
    if (!ignore_case)
        return strstr(line, pattern) != NULL;
    do {
        size_t i = 0;
        while (pattern[i] && line[i] &&
               tolower((unsigned char)line[i]) ==
               tolower((unsigned char)pattern[i]))
            ++i;
        if (!pattern[i])
            return true;
    } while (*line++);
    return false;
}

static int
emit_line(struct read_query *q, const char *path, uint64_t line,
           const char *text)
{
    size_t needed = strlen(path) + strlen(text) + 48u;

    if (q->seen++ < q->offset)
        return 0;
    if (q->emitted == q->limit || needed > RO_OUTPUT - q->output.len) {
        q->more = true;
        return 1;
    }
    if (snag_buf_printf(&q->output, "%s:%llu:%s\n", path,
                       (unsigned long long)line, text) < 0)
        return -1;
    ++q->emitted;
    return 0;
}

static int
scan_file(struct read_query *q, int fd, const char *path)
{
    FILE *file = fdopen(fd, "r");
    char *line = malloc(RO_LINE + 1u);
    uint64_t number = 0;
    int rc = 0;
    size_t saved_len = q->output.len, saved_seen = q->seen;
    size_t saved_emitted = q->emitted;

    if (!file || !line) {
        if (file) fclose(file);
        else close(fd);
        free(line);
        return -1;
    }
    for (;;) {
        size_t len = 0;
        int c = EOF;

        if (checkpoint(q) < 0) { rc = -1; break; }
        while (len < RO_LINE && (c = fgetc(file)) != EOF) {
            if (++q->bytes > RO_SCAN) {
                q->problem = "Scan byte limit reached; narrow the path or range.";
                rc = -1;
                break;
            }
            if (c == '\n')
                break;
            line[len++] = (char)c;
        }
        if (rc < 0)
            break;
        if (ferror(file)) { rc = -1; break; }
        if (c == EOF && !len)
            break;
        if (memchr(line, 0, len) || (len < RO_LINE &&
            !snag_utf8_valid((unsigned char *)line, len, true))) {
            if (q->grep) {
                q->output.len = saved_len;
                q->seen = saved_seen;
                q->emitted = saved_emitted;
                ++q->skipped;
                break;
            }
            q->problem = "Non-text file (NUL or invalid UTF-8); inspection is incomplete.";
            rc = -1;
            break;
        }
        if (len == RO_LINE) {
            q->problem = "Line exceeds 64 KiB; inspection is incomplete.";
            rc = -1;
            break;
        }
        line[len] = '\0';
        ++number;
        if (q->read) {
            if (number >= q->start) {
                if (strlen(path) + len + 48u > RO_OUTPUT - q->output.len) {
                    q->problem = "File output exceeds 256 KiB; request a narrower line range.";
                    rc = -1;
                    break;
                }
                rc = snag_buf_printf(&q->output, "%llu:%s%s",
                    (unsigned long long)number, line, c == '\n' ? "\n" : "");
            }
            if (number == q->end || rc < 0)
                break;
        } else {
            bool match = q->literal ? literal_match(line, q->pattern, q->ignore_case) :
                         regexec(&q->regex, line, 0u, NULL, 0) == 0;
            if (match && (rc = emit_line(q, path, number, line)) != 0)
                break;
        }
        if (c == EOF)
            break;
    }
    if (q->read && q->start > 1u && number < q->start && rc == 0) {
        q->problem = "Requested start_line is beyond end of file.";
        rc = -1;
    }
    if (fclose(file) != 0 && rc == 0)
        rc = -1;
    free(line);
    return rc;
}

static int
compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int
walk(struct read_query *q, int fd, const char *path, unsigned int depth)
{
    struct stat st;
    DIR *dir;
    struct dirent *entry;
    char **names = NULL;
    size_t count = 0;
    int rc = -1;

    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    if (S_ISREG(st.st_mode) && (q->grep || q->read))
        return scan_file(q, fd, path);
    if (q->read || !S_ISDIR(st.st_mode)) {
        q->problem = "Expected a regular file for read_file or a directory for list_files.";
        close(fd);
        return -1;
    }
    dir = fdopendir(fd);
    if (!dir) { close(fd); return -1; }
    while (checkpoint(q) == 0) {
        char **grown;

        errno = 0;
        entry = readdir(dir);
        if (!entry) { rc = errno ? -1 : 0; break; }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (++q->entries > RO_ENTRIES) {
            q->problem = "Directory entry limit reached; narrow the path.";
            break;
        }
        if (!snag_utf8_valid((unsigned char *)entry->d_name,
                            strlen(entry->d_name), true)) {
            q->problem = "Non-UTF-8 filename; directory inspection is incomplete.";
            break;
        }
        grown = realloc(names, (count + 1u) * sizeof(*names));
        if (!grown)
            break;
        names = grown;
        names[count] = strdup(entry->d_name);
        if (!names[count])
            break;
        ++count;
    }
    if (rc != 0)
        goto out;
    if (count)
        qsort(names, count, sizeof(*names), compare_names);
    for (size_t i = 0; i < count && rc == 0; ++i) {
        struct snag_buf child;
        const char *type;

        if (checkpoint(q) < 0 ||
            fstatat(fd, names[i], &st, AT_SYMLINK_NOFOLLOW) < 0) {
            rc = -1;
            break;
        }
        snag_buf_init(&child, 8192u);
        rc = snag_buf_printf(&child, "%s%s%s", path,
                            path[strlen(path) - 1u] == '/' ? "" : "/", names[i]);
        if (rc == 0)
            rc = snag_buf_terminate(&child);
        type = S_ISDIR(st.st_mode) ? "directory" : S_ISREG(st.st_mode) ? "file" :
               S_ISLNK(st.st_mode) ? "symlink (not followed)" : "special (not opened)";
        if (q->grep && !S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
            ++q->skipped;
        if (rc == 0 && !q->grep) {
            if (q->seen++ >= q->offset) {
                if (q->emitted == q->limit ||
                    child.len + strlen(type) + 4u > RO_OUTPUT - q->output.len) {
                    q->more = true;
                    rc = 1;
                } else {
                    rc = snag_buf_printf(&q->output, "%s\t%s\n", child.data, type);
                    ++q->emitted;
                }
            }
        }
        if (rc == 0 && ((S_ISDIR(st.st_mode) && q->recursive) ||
                        (S_ISREG(st.st_mode) && q->grep))) {
            int next;

            if (depth >= RO_DEPTH) {
                q->problem = "Directory depth limit reached; narrow the path.";
                rc = -1;
            } else if ((next = open_entry(fd, names[i])) < 0) {
                rc = -1;
            } else rc = walk(q, next, (char *)child.data, depth + 1u);
        }
        snag_buf_free(&child);
    }
out:
    for (size_t i = 0; i < count; ++i)
        free(names[i]);
    free(names);
    closedir(dir);
    return rc;
}

int
snag_tools_read_only(const struct snag_response_item *call, const char *workspace,
                    snag_tool_pump_fn pump, void *opaque, json_t **result)
{
    static const char *const read_keys[] = {"path", "start_line", "end_line"};
    static const char *const list_keys[] = {"path", "recursive", "offset", "limit"};
    static const char *const grep_keys[] = {
        "path", "pattern", "recursive", "ignore_case", "literal", "offset", "limit"
    };
    struct read_query q = {0};
    const json_t *args;
    const char *path;
    uint64_t offset = 0, limit = 200u;
    char failure[256];
    int fd, rc = -1;

    if (!call || !call->name || !result || !workspace) {
        errno = EINVAL;
        return -1;
    }
    *result = NULL;
    args = call->arguments;
    path = snag_json_string(args, "path");
    q.read = strcmp(call->name, "read_file") == 0;
    q.grep = strcmp(call->name, "grep") == 0;
    q.pump = pump;
    q.opaque = opaque;
    snag_buf_init(&q.output, RO_OUTPUT + 1024u);
    q.problem = "Invalid native inspection arguments.";
    if (!snag_read_only_tool(call->name) || !workspace || !path || !*path ||
        strlen(path) > 4096u ||
        !snag_json_exact_keys(args, q.read ? read_keys : q.grep ? grep_keys : list_keys,
                             q.read ? 3u : q.grep ? 7u : 4u))
        goto out;
    if (q.read) {
        if (!number_arg(args, "start_line", 1u, 1u, INT32_MAX, &q.start) ||
            !number_arg(args, "end_line", UINT64_MAX, 1u, INT32_MAX, &q.end) ||
            q.end < q.start)
            goto out;
    } else if (!bool_arg(args, "recursive", q.grep, &q.recursive) ||
               !number_arg(args, "offset", 0u, 0u, 1000000u, &offset) ||
               !number_arg(args, "limit", 200u, 1u, 1000u, &limit)) {
        goto out;
    }
    q.offset = (size_t)offset;
    q.limit = (size_t)limit;
    if (q.grep) {
        q.pattern = snag_json_string(args, "pattern");
        if (!q.pattern || strlen(q.pattern) > 4096u ||
            !bool_arg(args, "ignore_case", false, &q.ignore_case) ||
            !bool_arg(args, "literal", false, &q.literal))
            goto out;
        if (!q.literal) {
            int regex_rc = regcomp(&q.regex, q.pattern,
                REG_EXTENDED | REG_NOSUB | (q.ignore_case ? REG_ICASE : 0));
            if (regex_rc) {
                regerror(regex_rc, &q.regex, failure, sizeof(failure));
                q.problem = failure;
                goto out;
            }
            q.compiled = true;
        }
    }
    q.problem = NULL;
    fd = open_path(workspace, path);
    if (fd < 0) {
        q.problem = "Cannot open path: missing, inaccessible, symlink, or non-regular special file.";
        goto out;
    }
    rc = walk(&q, fd, path, 0u);
out:
    if (rc < 0) {
        *result = snag_tool_result_terminal(false, q.problem ? q.problem :
            "Native inspection failed; results are incomplete (path changed, I/O or resource error).");
    } else {
        if (!q.read && snag_buf_printf(&q.output,
                "\n%s; returned=%zu; next_offset=%zu; skipped_nontext_or_special=%zu\n",
                q.more ? "More results (repeat with next_offset)" : "Complete",
                q.emitted, q.offset + q.emitted, q.skipped) < 0)
            goto cleanup;
        if (snag_buf_terminate(&q.output) == 0)
            *result = snag_tool_result_terminal(true, (char *)q.output.data);
    }
cleanup:
    if (q.compiled)
        regfree(&q.regex);
    snag_buf_free(&q.output);
    return !*result ? -1 : q.interrupted == 2 ? 2 : 0;
}
