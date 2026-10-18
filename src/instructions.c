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
snag_instructions_free(struct snag_instruction_set *set)
{
    for (size_t i = 0; i < set->count; ++i) {
        free(set->paths[i]);
        free(set->labels[i]);
    }
    free(set->paths);
    free(set->labels);
    *set = (struct snag_instruction_set){0};
}

int
snag_instructions_add_owned(
    struct snag_instruction_set *set, char *path, char *error, size_t error_size)
{
    char **grown_paths;
    char **grown_labels;

    if (!set || !path) {
        free(path);
        return snag_fail(error, error_size, EINVAL, "invalid instruction path");
    }
    if (set->count == set->capacity) {
        size_t capacity = set->capacity ? set->capacity * 2u : 8u;
        if (capacity < set->capacity) {
            free(path);
            return snag_fail(error, error_size, EOVERFLOW, "instruction path capacity overflow");
        }
        grown_paths = malloc(capacity * sizeof(*grown_paths));
        grown_labels = calloc(capacity, sizeof(*grown_labels));
        if (!grown_paths || !grown_labels) {
            free(grown_paths);
            free(grown_labels);
            free(path);
            return snag_fail(error, error_size, ENOMEM, "cannot retain instruction path");
        }
        if (set->count) {
            memcpy(grown_paths, set->paths, set->count * sizeof(*grown_paths));
            memcpy(grown_labels, set->labels, set->count * sizeof(*grown_labels));
        }
        free(set->paths);
        free(set->labels);
        set->paths = grown_paths;
        set->labels = grown_labels;
        set->capacity = capacity;
    }
    set->paths[set->count++] = path;
    return 0;
}

int
snag_instructions_label_last(
    struct snag_instruction_set *set, const char *label, char *error, size_t error_size)
{
    char *copy;

    if (!set || !set->count)
        return snag_fail(error, error_size, EINVAL, "-dl requires a preceding -d directory");
    if (set->labels[set->count - 1u])
        return snag_fail(
            error, error_size, EINVAL, "duplicate label for the preceding -d directory");
    if (!snag_text_valid(label, 1u, SNAG_TERM_LABEL_BYTES))
        return snag_fail(error, error_size, EINVAL,
            "-dl label must be nonempty UTF-8 text within %u bytes", SNAG_TERM_LABEL_BYTES);
    copy = snag_strdup_checked(label, SNAG_TERM_LABEL_BYTES);
    if (!copy) return snag_errorf(error, error_size, "cannot retain -dl label");
    set->labels[set->count - 1u] = copy;
    return 0;
}

static int
try_candidate(struct snag_instruction_set *set, const char *path, bool follow_symlink, bool *added,
    char *error, size_t error_size)
{
    snag_file_info st;
    char *canonical;

    *added = false;
    if (snag_lstat(path, &st) < 0) {
        if (errno == ENOENT) return 0;
        return snag_errorf(
            error, error_size, "cannot inspect instruction %s: %s", path, strerror(errno));
    }
    if ((!follow_symlink || !S_ISLNK(st.st_mode)) && !S_ISREG(st.st_mode)) {
        return snag_fail(
            error, error_size, EINVAL, "instruction %s must resolve to a regular file", path);
    }
    canonical = snag_realpath(path);
    if (!canonical)
        return snag_errorf(
            error, error_size, "cannot resolve instruction %s: %s", path, strerror(errno));
    if (!snag_text_valid(canonical, 0u, SNAG_PATH_MAX_BYTES)) {
        free(canonical);
        return snag_fail(error, error_size, EINVAL, "instruction path cannot be canonicalized");
    }
    if (snag_stat(canonical, &st) < 0) {
        free(canonical);
        return snag_errorf(
            error, error_size, "cannot inspect instruction %s: %s", path, strerror(errno));
    }
    if (!S_ISREG(st.st_mode)) {
        free(canonical);
        return snag_fail(
            error, error_size, EINVAL, "instruction %s must resolve to a regular file", path);
    }
    *added = true;
    for (size_t i = 0; i < set->count; ++i) {
        if (strcmp(set->paths[i], canonical) == 0) {
            free(canonical);
            return 0;
        }
    }
    return snag_instructions_add_owned(set, canonical, error, error_size);
}

int
snag_instructions_add_file(
    struct snag_instruction_set *set, const char *path, char *error, size_t error_size)
{
    bool added;
    if (try_candidate(set, path, true, &added, error, error_size) < 0) return -1;
    if (added) return 0;
    return snag_fail(error, error_size, ENOENT, "instruction file is missing: %s", path);
}

static int
try_instruction_dir(
    struct snag_instruction_set *set, const char *dir, char *error, size_t error_size)
{
    static const char *const names[] = {"AGENTS.override.md", "AGENTS.md"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char *path = snag_path_join(dir, names[i]);
        bool added = false;
        int rc;

        if (!path) return -1;
        rc = try_candidate(set, path, true, &added, error, error_size);
        free(path);
        if (rc < 0) return -1;
        if (added) return 1;
    }
    return 0;
}

int
snag_instructions_add_directory(
    struct snag_instruction_set *set, const char *dir, char *error, size_t error_size)
{
    char *canonical = dir && *dir ? snag_realpath(dir) : NULL;
    snag_file_info st;
    int rc = -1;

    if (!snag_text_valid(canonical, 0u, SNAG_PATH_MAX_BYTES) || snag_stat(canonical, &st) < 0 ||
        !S_ISDIR(st.st_mode)) {
        (void)snag_fail(error, error_size, EINVAL, "-d requires an existing UTF-8 directory: %s",
            dir ? dir : "");
        goto out;
    }
    rc = try_instruction_dir(set, canonical, error, error_size);
    if (rc == 0) {
        (void)snag_fail(error, error_size, ENOENT,
            "-d directory has no AGENTS.md or AGENTS.override.md: %s", canonical);
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

    if (xdg) snag_path_slashes(xdg);
    if (xdg && *xdg) {
        if (!snag_path_root_len(xdg)) {
            free(xdg);
            (void)snag_fail(error, error_size, EINVAL, "XDG_CONFIG_HOME must be absolute");
            return NULL;
        }
        base = snag_strdup_checked(xdg, SNAG_PATH_MAX_BYTES);
    } else {
        char *home = snag_home_directory();
        if (!snag_path_root_len(home)) {
            free(home);
            free(xdg);
            (void)snag_fail(
                error, error_size, EINVAL, "HOME is unavailable for instruction discovery");
            return NULL;
        }
        base = snag_path_join(home, ".config");
        free(home);
    }
    free(xdg);
    if (!base) return NULL;
    root = snag_path_join(base, SNAJPAGENT_NAME);
    free(base);
    return root;
}

int
snag_instructions_project_root(const char *cwd, char **root, char *error, size_t error_size)
{
    char *current = snag_strdup_checked(cwd, SNAG_PATH_MAX_BYTES);

    *root = NULL;
    if (!current) return -1;
    for (;;) {
        char *git = snag_path_join(current, ".git");
        snag_file_info st;

        if (!git) {
            free(current);
            return -1;
        }
        if (snag_lstat(git, &st) == 0) {
            if (S_ISLNK(st.st_mode) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
                snag_errorf(
                    error, error_size, ".git at %s must be a non-symlink file or directory", git);
                free(git);
                free(current);
                return snag_errno(EINVAL);
            }
            free(git);
            *root = current;
            return 0;
        }
        if (errno != ENOENT) {
            snag_errorf(error, error_size, "cannot inspect %s: %s", git, strerror(errno));
            free(git);
            free(current);
            return -1;
        }
        free(git);
        if (strcmp(current, "/") == 0) break;
        {
            char *slash = strrchr(current, '/');
            if (!slash || slash == current)
                current[1] = '\0';
            else
                *slash = '\0';
        }
    }
    free(current);
    *root = snag_strdup_checked(cwd, SNAG_PATH_MAX_BYTES);
    return *root ? 0 : -1;
}

static int
walk_project_chain(struct snag_instruction_set *set, const char *root, const char *cwd, char *error,
    size_t error_size)
{
    char *current = snag_strdup_checked(cwd, SNAG_PATH_MAX_BYTES);
    size_t end = strlen(root);
    int rc = -1;

    if (!current) return -1;
    if (strncmp(root, cwd, end) != 0 || (strcmp(root, "/") && cwd[end] && cwd[end] != '/')) {
        (void)snag_fail(error, error_size, EINVAL, "project root is not an ancestor of cwd");
        goto out;
    }
    for (;;) {
        char saved = current[end];
        current[end] = '\0';
        if (try_instruction_dir(set, current, error, error_size) < 0) goto out;
        current[end] = saved;
        if (!saved) break;
        if (saved == '/') ++end;
        end += strcspn(current + end, "/");
    }
    rc = 0;
out:
    free(current);
    return rc;
}

static int
add_project(struct snag_instruction_set *set, const char *cwd, char *error, size_t error_size)
{
    char *canonical_cwd = NULL, *project_root = NULL;
    snag_file_info st;
    int rc = -1;

    canonical_cwd = snag_realpath(cwd);
    /* Keep global guidance when the saved cwd vanished; cd can recover it. */
    if (!canonical_cwd && (errno == ENOENT || errno == ENOTDIR)) {
        rc = 0;
        goto out;
    }
    if (!snag_text_valid(canonical_cwd, 0u, SNAG_PATH_MAX_BYTES) ||
        snag_stat(canonical_cwd, &st) < 0) {
        (void)snag_fail(error, error_size, EINVAL,
            "cwd must be an existing UTF-8 directory for instruction discovery");
        goto out;
    }
    if (!S_ISDIR(st.st_mode)) {
        rc = 0;
        goto out;
    }
    if (snag_instructions_project_root(canonical_cwd, &project_root, error, error_size) < 0 ||
        walk_project_chain(set, project_root, canonical_cwd, error, error_size) < 0)
        goto out;
    rc = 0;
out:
    free(canonical_cwd);
    free(project_root);
    return rc;
}

int
snag_instructions_discover_context(struct snag_instruction_set *set, const char *cwd,
    const char *visit_root, char *error, size_t error_size)
{
    char *global = NULL;
    snag_file_info st;
    int rc = -1;

    snag_instructions_free(set);
    if (!snag_text_valid(cwd, 1u, SNAG_PATH_MAX_BYTES))
        return snag_fail(error, error_size, EINVAL,
            "cwd must be a nonempty UTF-8 path for instruction discovery");
    global = config_instruction_root(error, error_size);
    if (!global) goto out;
    if (snag_lstat(global, &st) == 0) {
        if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
            (void)snag_fail(
                error, error_size, EINVAL, "instruction config root must be a real directory");
            goto out;
        }
        if (try_instruction_dir(set, global, error, error_size) < 0) goto out;
    } else if (errno != ENOENT) {
        snag_errorf(
            error, error_size, "cannot inspect instruction config root: %s", strerror(errno));
        goto out;
    }
    if (add_project(set, cwd, error, error_size) < 0 ||
        (visit_root && strcmp(visit_root, cwd) &&
            add_project(set, visit_root, error, error_size) < 0)) goto out;
    rc = 0;
out:
    free(global);
    if (rc < 0) snag_instructions_free(set);
    return rc;
}

int
snag_instructions_discover(
    struct snag_instruction_set *set, const char *cwd, char *error, size_t error_size)
{
    return snag_instructions_discover_context(set, cwd, NULL, error, error_size);
}

json_t *
snag_instructions_metadata_json(const struct snag_instruction_set *set)
{
    json_t *array = json_array();

    if (!array) return NULL;
    for (size_t i = 0; set && i < set->count; ++i) {
        json_t *entry = set->labels[i]
                            ? json_pack("{s:s,s:s}", "label", set->labels[i], "path",
                                  set->paths[i])
                            : json_string(set->paths[i]);
        if (!entry || json_array_append(array, entry) < 0) {
            json_decref(entry);
            json_decref(array);
            return NULL;
        }
        json_decref(entry);
    }
    return array;
}

int
snag_instructions_metadata_valid(const json_t *array, char *error, size_t error_size)
{
    size_t count;

    if (!json_is_array(array)) goto invalid;
    count = json_array_size(array);
    for (size_t i = 0; i < count; ++i) {
        const json_t *value = json_array_get(array, i);
        const char *path = json_is_string(value) ? json_string_value(value)
                                                  : snag_json_string(value, "path");
        const char *label = json_is_object(value) ? snag_json_string(value, "label") : NULL;
        if ((json_is_object(value) && !snag_json_exact_keys(value, "label path")) ||
            (json_is_object(value) && !label) ||
            !path || !snag_path_root_len(path) || strlen(path) > SNAG_PATH_MAX_BYTES ||
            (json_is_string(value) && json_string_length(value) != strlen(path)) ||
            !snag_utf8_valid((const unsigned char *)path, strlen(path), true) ||
            (label && !snag_text_valid(label, 1u, SNAG_TERM_LABEL_BYTES)))
            goto invalid;
        for (size_t j = 0; j < i; ++j) {
            const json_t *other = json_array_get(array, j);
            const char *other_path = json_is_string(other)
                                         ? json_string_value(other)
                                         : snag_json_string(other, "path");
            if (other_path && strcmp(other_path, path) == 0) goto invalid;
        }
    }
    return 0;
invalid:
    return snag_fail(error, error_size, EINVAL, "invalid or duplicate instruction path metadata");
}

int
snag_instructions_match_metadata(
    const struct snag_instruction_set *set, const json_t *array, char *error, size_t error_size)
{
    size_t count;

    if (snag_instructions_metadata_valid(array, error, error_size) < 0) return -1;
    count = json_array_size(array);
    if ((!set && count != 0u) || (set && count != set->count)) goto mismatch;
    for (size_t i = 0; set && i < set->count; ++i) {
        const json_t *value = json_array_get(array, i);
        const char *path = json_is_string(value) ? json_string_value(value)
                                                  : snag_json_string(value, "path");
        const char *label = json_is_object(value) ? snag_json_string(value, "label") : NULL;
        if (strcmp(path, set->paths[i]) != 0 ||
            ((label != NULL) != (set->labels[i] != NULL)) ||
            (label && strcmp(label, set->labels[i]) != 0))
            goto mismatch;
    }
    return 0;
mismatch:
    return snag_fail(error, error_size, EINVAL,
        "active turn instruction paths no longer match advertised paths");
}

int
snag_instructions_worknote(const char *cwd, char **note, char *error, size_t error_size)
{
    struct snag_instruction_set probe = {0};
    char *canonical_cwd = NULL;
    char *global = NULL;
    const char *dirs[2];
    size_t dir_count = 0u;
    char scratch[256];
    int rc = -1;

    (void)error;
    (void)error_size;
    *note = NULL;
    /* Fail-soft by design: absence, invalid candidates and an unavailable global root all read
     * as "no note"; only allocation failure reports -1, and the caller treats it as no note. */
    canonical_cwd = cwd && *cwd ? snag_realpath(cwd) : NULL;
    if (canonical_cwd) dirs[dir_count++] = canonical_cwd;
    global = config_instruction_root(scratch, sizeof(scratch));
    if (global) dirs[dir_count++] = global;
    for (size_t i = 0u; i < dir_count; ++i) {
        char *path = snag_path_join(dirs[i], SNAG_WORKNOTE_NAME);
        bool added = false;
        int inspected;

        if (!path) goto out;
        inspected = try_candidate(&probe, path, false, &added, scratch, sizeof(scratch));
        free(path);
        if (inspected == 0 && added) {
            /* Transfer the canonical path out of the probe set. */
            *note = probe.paths[0];
            probe.paths[0] = NULL;
            probe.count = 0u;
            rc = 0;
            goto out;
        }
        snag_instructions_free(&probe);
    }
    rc = 0;
out:
    snag_instructions_free(&probe);
    free(global);
    free(canonical_cwd);
    return rc;
}
