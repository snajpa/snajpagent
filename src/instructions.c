/* SPDX-License-Identifier: GPL-2.0-only */
#include "instructions.h"
#include "json.h"
#include "snajpagent.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

static char *
join_path(const char *left, const char *right)
{
    size_t a = strlen(left);
    size_t b = strlen(right);
    size_t need;
    char *path;

    if (!snag_size_add(a, b, &need) || !snag_size_add(need, 2u, &need) ||
        need > SNAG_PATH_MAX_BYTES + 1u) {
        errno = EOVERFLOW;
        return NULL;
    }
    path = malloc(need);
    if (!path)
        return NULL;
    if (strcmp(left, "/") == 0)
        (void)snprintf(path, need, "/%s", right);
    else
        (void)snprintf(path, need, "%s/%s", left, right);
    return path;
}

void
snag_instructions_init(struct snag_instruction_set *set)
{
    memset(set, 0, sizeof(*set));
}

void
snag_instructions_free(struct snag_instruction_set *set)
{
    for (size_t i = 0; i < set->count; ++i) {
        free(set->sources[i].path);
        free(set->sources[i].text);
    }
    snag_instructions_init(set);
}

static int
append_source(struct snag_instruction_set *set, const char *path,
              const unsigned char *data, size_t len,
              char *error, size_t error_size)
{
    struct snag_instruction_source *src;
    char *canonical;
    char *text;

    if (set->count >= SNAG_MAX_INSTRUCTION_SOURCES ||
        len > SNAG_MAX_INSTRUCTION_FILE ||
        set->bytes > SNAG_MAX_INSTRUCTION_BYTES - len) {
        snag_errorf(error, error_size,
                  "instruction discovery exceeds 16 files or 128 KiB");
        errno = EOVERFLOW;
        return -1;
    }
    canonical = realpath(path, NULL);
    if (!canonical || strlen(canonical) > SNAG_PATH_MAX_BYTES ||
        !snag_utf8_valid((const unsigned char *)canonical, strlen(canonical), true)) {
        free(canonical);
        snag_errorf(error, error_size, "instruction path cannot be canonicalized");
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < set->count; ++i) {
        if (strcmp(set->sources[i].path, canonical) == 0) {
            free(canonical);
            snag_errorf(error, error_size, "duplicate instruction path discovered");
            errno = EINVAL;
            return -1;
        }
    }
    text = malloc(len + 1u);
    if (!text) {
        free(canonical);
        return -1;
    }
    memcpy(text, data, len);
    text[len] = '\0';
    src = &set->sources[set->count++];
    src->path = canonical;
    src->text = text;
    src->bytes = len;
    snag_sha256_hex(data, len, src->sha256);
    set->bytes += len;
    return 0;
}

static int
try_candidate(struct snag_instruction_set *set, const char *path,
              bool *added, char *error, size_t error_size)
{
    struct stat st;
    struct snag_buf text;
    int fd = -1;
    int rc = -1;

    *added = false;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT)
            return 0;
        snag_errorf(error, error_size, "cannot inspect instruction %s: %s",
                  path, strerror(errno));
        return -1;
    }
    if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) {
        snag_errorf(error, error_size,
                  "instruction %s must be a non-symlink regular file", path);
        errno = EINVAL;
        return -1;
    }
    if (st.st_size == 0)
        return 0;
    if (st.st_size < 0 || (uintmax_t)st.st_size > SNAG_MAX_INSTRUCTION_FILE) {
        snag_errorf(error, error_size, "instruction %s exceeds 32 KiB", path);
        errno = EOVERFLOW;
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        snag_errorf(error, error_size, "cannot open instruction %s: %s",
                  path, strerror(errno));
        return -1;
    }
    snag_buf_init(&text, SNAG_MAX_INSTRUCTION_FILE + 1u);
    for (;;) {
        unsigned char chunk[4096];
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            snag_errorf(error, error_size, "cannot read instruction %s: %s",
                      path, strerror(errno));
            goto out;
        }
        if (got == 0)
            break;
        if (snag_buf_append(&text, chunk, (size_t)got) < 0 ||
            text.len > SNAG_MAX_INSTRUCTION_FILE) {
            snag_errorf(error, error_size, "instruction %s exceeds 32 KiB", path);
            errno = EOVERFLOW;
            goto out;
        }
    }
    if (text.len == 0u) {
        rc = 0;
        goto out;
    }
    if (!snag_utf8_valid(text.data, text.len, true)) {
        snag_errorf(error, error_size,
                  "instruction %s must be valid UTF-8 without NUL bytes", path);
        errno = EILSEQ;
        goto out;
    }
    if (append_source(set, path, text.data, text.len, error, error_size) < 0)
        goto out;
    *added = true;
    rc = 0;
out:
    {
        int saved = errno;
        if (fd >= 0)
            (void)close(fd);
        snag_buf_free(&text);
        errno = saved;
    }
    return rc;
}

static int
try_instruction_dir(struct snag_instruction_set *set, const char *dir,
                    char *error, size_t error_size)
{
    static const char *const names[] = {"AGENTS.override.md", "AGENTS.md"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char *path = join_path(dir, names[i]);
        bool added = false;
        int rc;

        if (!path)
            return -1;
        rc = try_candidate(set, path, &added, error, error_size);
        free(path);
        if (rc < 0)
            return -1;
        if (added)
            return 0;
    }
    return 0;
}

static char *
config_instruction_root(char *error, size_t error_size)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char *base;
    char *root;

    if (xdg && *xdg) {
        if (xdg[0] != '/') {
            snag_errorf(error, error_size, "XDG_CONFIG_HOME must be absolute");
            errno = EINVAL;
            return NULL;
        }
        base = snag_strdup_checked(xdg, SNAG_PATH_MAX_BYTES);
    } else {
        if (!home || home[0] != '/') {
            snag_errorf(error, error_size,
                      "HOME is unavailable for instruction discovery");
            errno = EINVAL;
            return NULL;
        }
        base = join_path(home, ".config");
    }
    if (!base)
        return NULL;
    root = join_path(base, SNAJPAGENT_NAME);
    free(base);
    return root;
}

static int
find_project_root(const char *workspace, char **root,
                  char *error, size_t error_size)
{
    char *current = snag_strdup_checked(workspace, SNAG_PATH_MAX_BYTES);

    *root = NULL;
    if (!current)
        return -1;
    for (;;) {
        char *git = join_path(current, ".git");
        struct stat st;

        if (!git) {
            free(current);
            return -1;
        }
        if (lstat(git, &st) == 0) {
            if (S_ISLNK(st.st_mode) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
                snag_errorf(error, error_size,
                          ".git at %s must be a non-symlink file or directory", git);
                free(git);
                free(current);
                errno = EINVAL;
                return -1;
            }
            free(git);
            *root = current;
            return 0;
        }
        if (errno != ENOENT) {
            snag_errorf(error, error_size, "cannot inspect %s: %s",
                      git, strerror(errno));
            free(git);
            free(current);
            return -1;
        }
        free(git);
        if (strcmp(current, "/") == 0)
            break;
        {
            char *slash = strrchr(current, '/');
            if (!slash || slash == current)
                current[1] = '\0';
            else
                *slash = '\0';
        }
    }
    free(current);
    *root = snag_strdup_checked(workspace, SNAG_PATH_MAX_BYTES);
    return *root ? 0 : -1;
}

static int
walk_project_chain(struct snag_instruction_set *set,
                   const char *root, const char *workspace,
                   char *error, size_t error_size)
{
    char *current = snag_strdup_checked(root, SNAG_PATH_MAX_BYTES);

    if (!current)
        return -1;
    if (try_instruction_dir(set, current, error, error_size) < 0)
        goto fail;
    while (strcmp(current, workspace) != 0) {
        const char *rest = workspace + strlen(current);
        const char *end;
        char segment[SNAG_PATH_MAX_BYTES + 1u];
        char *next;
        size_t len;

        if (strcmp(current, "/") == 0)
            rest = workspace + 1u;
        else if (*rest == '/')
            ++rest;
        else {
            snag_errorf(error, error_size,
                      "project root is not an ancestor of workspace");
            errno = EINVAL;
            goto fail;
        }
        end = strchr(rest, '/');
        len = end ? (size_t)(end - rest) : strlen(rest);
        if (!len || len > SNAG_PATH_MAX_BYTES) {
            errno = EINVAL;
            goto fail;
        }
        memcpy(segment, rest, len);
        segment[len] = '\0';
        next = join_path(current, segment);
        if (!next)
            goto fail;
        free(current);
        current = next;
        if (try_instruction_dir(set, current, error, error_size) < 0)
            goto fail;
    }
    free(current);
    return 0;
fail:
    free(current);
    return -1;
}

int
snag_instructions_discover(struct snag_instruction_set *set,
                          const char *workspace,
                          char *error, size_t error_size)
{
    char *global = NULL;
    char *canonical_workspace = NULL;
    char *project_root = NULL;
    struct stat st;
    int rc = -1;

    snag_instructions_free(set);
    if (!workspace) {
        errno = EINVAL;
        return -1;
    }
    global = config_instruction_root(error, error_size);
    if (!global)
        goto out;
    if (lstat(global, &st) == 0) {
        if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
            snag_errorf(error, error_size,
                      "instruction config root must be a real directory");
            errno = EINVAL;
            goto out;
        }
        if (try_instruction_dir(set, global, error, error_size) < 0)
            goto out;
    } else if (errno != ENOENT) {
        snag_errorf(error, error_size, "cannot inspect instruction config root: %s",
                  strerror(errno));
        goto out;
    }
    canonical_workspace = realpath(workspace, NULL);
    if (!canonical_workspace || strlen(canonical_workspace) > SNAG_PATH_MAX_BYTES ||
        !snag_utf8_valid((const unsigned char *)canonical_workspace,
                        strlen(canonical_workspace), true) ||
        stat(canonical_workspace, &st) < 0 || !S_ISDIR(st.st_mode)) {
        snag_errorf(error, error_size,
                  "workspace must be an existing UTF-8 directory for instruction discovery");
        errno = EINVAL;
        goto out;
    }
    if (find_project_root(canonical_workspace, &project_root,
                          error, error_size) < 0 ||
        walk_project_chain(set, project_root, canonical_workspace,
                           error, error_size) < 0)
        goto out;
    rc = 0;
out:
    free(global);
    free(canonical_workspace);
    free(project_root);
    if (rc < 0)
        snag_instructions_free(set);
    return rc;
}

json_t *
snag_instructions_metadata_json(const struct snag_instruction_set *set)
{
    json_t *array = json_array();

    if (!array)
        return NULL;
    for (size_t i = 0; set && i < set->count; ++i) {
        const struct snag_instruction_source *src = &set->sources[i];
        if (json_array_append_new(array,
            json_pack("{s:I,s:s,s:s}", "bytes", (json_int_t)src->bytes,
                      "path", src->path, "sha256", src->sha256)) < 0) {
            json_decref(array);
            return NULL;
        }
    }
    return array;
}

int
snag_instructions_metadata_valid(const json_t *array,
                                char *error, size_t error_size)
{
    static const char *const keys[] = {"bytes", "path", "sha256"};
    size_t total = 0;
    size_t count;

    if (!json_is_array(array) ||
        (count = json_array_size((json_t *)array)) > SNAG_MAX_INSTRUCTION_SOURCES) {
        snag_errorf(error, error_size, "invalid instruction metadata array");
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        json_t *item = json_array_get((json_t *)array, i);
        const char *path = snag_json_string(item, "path");
        const char *sha = snag_json_string(item, "sha256");
        uint64_t bytes;

        if (!snag_json_exact_keys(item, keys, 3u) || !path || path[0] != '/' ||
            strlen(path) > SNAG_PATH_MAX_BYTES ||
            !snag_utf8_valid((const unsigned char *)path, strlen(path), true) ||
            !sha || !snag_hex_is_lower(sha, SNAG_SHA256_HEX_LEN) ||
            snag_json_integer_u64(item, "bytes", &bytes) < 0 || bytes == 0u ||
            bytes > SNAG_MAX_INSTRUCTION_FILE ||
            total > SNAG_MAX_INSTRUCTION_BYTES - (size_t)bytes) {
            snag_errorf(error, error_size, "invalid instruction metadata entry");
            errno = EINVAL;
            return -1;
        }
        for (size_t j = 0; j < i; ++j) {
            const char *prev = snag_json_string(json_array_get((json_t *)array, j),
                                               "path");
            if (prev && strcmp(prev, path) == 0) {
                snag_errorf(error, error_size, "duplicate instruction metadata path");
                errno = EINVAL;
                return -1;
            }
        }
        total += (size_t)bytes;
    }
    return 0;
}

int
snag_instructions_match_metadata(const struct snag_instruction_set *set,
                                const json_t *array,
                                char *error, size_t error_size)
{
    size_t count;

    if (snag_instructions_metadata_valid(array, error, error_size) < 0)
        return -1;
    count = json_array_size((json_t *)array);
    if ((!set && count != 0u) || (set && count != set->count))
        goto mismatch;
    for (size_t i = 0; set && i < set->count; ++i) {
        json_t *item = json_array_get((json_t *)array, i);
        uint64_t bytes;
        if (snag_json_integer_u64(item, "bytes", &bytes) < 0 ||
            bytes != set->sources[i].bytes ||
            strcmp(snag_json_string(item, "path"), set->sources[i].path) != 0 ||
            strcmp(snag_json_string(item, "sha256"), set->sources[i].sha256) != 0)
            goto mismatch;
    }
    return 0;
mismatch:
    snag_errorf(error, error_size,
              "active turn instruction metadata no longer matches frozen contents");
    errno = EINVAL;
    return -1;
}
