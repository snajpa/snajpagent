/* SPDX-License-Identifier: GPL-2.0-only */
#include "instructions.h"
#include "fs.h"
#include "json.h"
#include "snajpagent.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void
snag_instructions_init(struct snag_instruction_set *set)
{
    memset(set, 0, sizeof(*set));
}

void
snag_instructions_free(struct snag_instruction_set *set)
{
    for (size_t i = 0; i < set->count; ++i)
        free(set->paths[i]);
    snag_instructions_init(set);
}

static int
try_candidate(struct snag_instruction_set *set, const char *path,
              bool *added, char *error, size_t error_size)
{
    snag_file_info st;
    char *canonical;

    *added = false;
    if (snag_lstat(path, &st) < 0) {
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
    canonical = snag_realpath(path);
    if (!canonical || strlen(canonical) > SNAG_PATH_MAX_BYTES ||
        !snag_utf8_valid((const unsigned char *)canonical, strlen(canonical), true)) {
        free(canonical);
        snag_errorf(error, error_size, "instruction path cannot be canonicalized");
        errno = EINVAL;
        return -1;
    }
    *added = true;
    for (size_t i = 0; i < set->count; ++i) {
        if (strcmp(set->paths[i], canonical) == 0) {
            free(canonical);
            return 0;
        }
    }
    if (set->count == SNAG_MAX_INSTRUCTION_SOURCES) {
        free(canonical);
        snag_errorf(error, error_size, "instruction discovery exceeds %u files",
                    SNAG_MAX_INSTRUCTION_SOURCES);
        errno = EOVERFLOW;
        return -1;
    }
    set->paths[set->count++] = canonical;
    return 0;
}

int
snag_instructions_add_file(struct snag_instruction_set *set, const char *path,
                           char *error, size_t error_size)
{
    bool added;
    if (try_candidate(set, path, &added, error, error_size) < 0)
        return -1;
    if (added)
        return 0;
    snag_errorf(error, error_size, "instruction file is missing: %s", path);
    errno = ENOENT;
    return -1;
}

static int
try_instruction_dir(struct snag_instruction_set *set, const char *dir,
                    char *error, size_t error_size)
{
    static const char *const names[] = {"AGENTS.override.md", "AGENTS.md"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char *path = snag_path_join(dir, names[i]);
        bool added = false;
        int rc;

        if (!path)
            return -1;
        rc = try_candidate(set, path, &added, error, error_size);
        free(path);
        if (rc < 0)
            return -1;
        if (added)
            return 1;
    }
    return 0;
}

int
snag_instructions_add_directory(struct snag_instruction_set *set, const char *dir,
                                char *error, size_t error_size)
{
    char *canonical = dir && *dir ? snag_realpath(dir) : NULL;
    snag_file_info st;
    int rc = -1;

    if (!canonical || strlen(canonical) > SNAG_PATH_MAX_BYTES ||
        !snag_utf8_valid((const unsigned char *)canonical, strlen(canonical), true) ||
        snag_stat(canonical, &st) < 0 || !S_ISDIR(st.st_mode)) {
        snag_errorf(error, error_size, "-d requires an existing UTF-8 directory: %s",
                    dir ? dir : "");
        errno = EINVAL;
        goto out;
    }
    rc = try_instruction_dir(set, canonical, error, error_size);
    if (rc == 0) {
        snag_errorf(error, error_size, "-d directory has no AGENTS.md or AGENTS.override.md: %s",
                    canonical);
        errno = ENOENT;
        rc = -1;
    } else if (rc > 0) {
        rc = 0;
    }
out:
    free(canonical);
    return rc;
}

static char *
config_instruction_root(char *error, size_t error_size)
{
    char *xdg = snag_environment("XDG_CONFIG_HOME");
    char *base;
    char *root;

    if (xdg && *xdg) {
        if (!snag_path_root_len(xdg)) {
            free(xdg);
            snag_errorf(error, error_size, "XDG_CONFIG_HOME must be absolute");
            errno = EINVAL;
            return NULL;
        }
        base = snag_strdup_checked(xdg, SNAG_PATH_MAX_BYTES);
    } else {
        char *home = snag_home_directory();
        if (!snag_path_root_len(home)) {
            free(home);
            free(xdg);
            snag_errorf(error, error_size,
                      "HOME is unavailable for instruction discovery");
            errno = EINVAL;
            return NULL;
        }
        base = snag_path_join(home, ".config");
        free(home);
    }
    free(xdg);
    if (!base)
        return NULL;
    root = snag_path_join(base, SNAJPAGENT_NAME);
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
        char *git = snag_path_join(current, ".git");
        snag_file_info st;

        if (!git) {
            free(current);
            return -1;
        }
        if (snag_lstat(git, &st) == 0) {
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
        next = snag_path_join(current, segment);
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
    snag_file_info st;
    int rc = -1;

    snag_instructions_free(set);
    if (!workspace) {
        errno = EINVAL;
        return -1;
    }
    global = config_instruction_root(error, error_size);
    if (!global)
        goto out;
    if (snag_lstat(global, &st) == 0) {
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
    canonical_workspace = snag_realpath(workspace);
    if (!canonical_workspace || strlen(canonical_workspace) > SNAG_PATH_MAX_BYTES ||
        !snag_utf8_valid((const unsigned char *)canonical_workspace,
                        strlen(canonical_workspace), true) ||
        snag_stat(canonical_workspace, &st) < 0 || !S_ISDIR(st.st_mode)) {
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
        if (json_array_append_new(array, json_string(set->paths[i])) < 0) {
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
    size_t count;

    if (!json_is_array(array) ||
        (count = json_array_size(array)) > SNAG_MAX_INSTRUCTION_SOURCES)
        goto invalid;
    for (size_t i = 0; i < count; ++i) {
        const json_t *value = json_array_get(array, i);
        const char *path = json_string_value(value);
        if (!snag_path_root_len(path) || strlen(path) > SNAG_PATH_MAX_BYTES ||
            json_string_length(value) != strlen(path) ||
            !snag_utf8_valid((const unsigned char *)path, strlen(path), true))
            goto invalid;
        for (size_t j = 0; j < i; ++j)
            if (strcmp(json_string_value(json_array_get(array, j)), path) == 0)
                goto invalid;
    }
    return 0;
invalid:
    snag_errorf(error, error_size, "invalid or duplicate instruction path metadata");
    errno = EINVAL;
    return -1;
}

int
snag_instructions_match_metadata(const struct snag_instruction_set *set,
                                const json_t *array,
                                char *error, size_t error_size)
{
    size_t count;

    if (snag_instructions_metadata_valid(array, error, error_size) < 0)
        return -1;
    count = json_array_size(array);
    if ((!set && count != 0u) || (set && count != set->count))
        goto mismatch;
    for (size_t i = 0; set && i < set->count; ++i)
        if (strcmp(json_string_value(json_array_get(array, i)), set->paths[i]) != 0)
            goto mismatch;
    return 0;
mismatch:
    snag_errorf(error, error_size,
                "active turn instruction paths no longer match advertised paths");
    errno = EINVAL;
    return -1;
}
