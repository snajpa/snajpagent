/* SPDX-License-Identifier: GPL-2.0-only */
#include "skills.h"
#include "base.h"
#include "fs.h"
#include "json.h"
#include "process_host.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool
within_package(const char *path, const char *root)
{
    size_t n = strlen(root);
    return !strncmp(path, root, n) && (!path[n] || root[n - 1u] == '/' || path[n] == '/');
}

/* Staging is private and exclusively created here. Never follow a link during cleanup. */
static int
remove_stage(int parent, const char *name)
{
    snag_file_info info;
    if (snag_lstat_at(parent, name, &info) < 0) return -1;
    if (!S_ISDIR(info.st_mode)) return snag_unlink_at(parent, name, false);
    int fd = snag_open_inspect_at(parent, name);
    struct snag_directory *directory = fd >= 0 ? snag_directory_open(fd) : NULL;
    if (!directory) { if (fd >= 0) close(fd); return -1; }
    int rc = 0;
    const char *child;
    while ((child = snag_directory_next(directory))) {
        if (!strcmp(child, ".") || !strcmp(child, "..")) continue;
        if (remove_stage(fd, child) < 0) { rc = -1; break; }
    }
    if (errno) rc = -1;
    if (snag_directory_close(directory) < 0) rc = -1;
    return rc < 0 ? -1 : snag_unlink_at(parent, name, true);
}

static int
copy_package(const char *path, const char *root, int parent, const char *name,
    json_t *ancestors, char *error, size_t size)
{
    char *canonical = snag_realpath(path);
    if (!canonical || !within_package(canonical, root)) {
        free(canonical);
        return snag_errorf(error, size, "resource escapes or is absent from selected package");
    }
    for (size_t i = 0; i < json_array_size(ancestors); ++i) {
        if (!strcmp(canonical, json_string_value(json_array_get(ancestors, i)))) {
            free(canonical);
            return snag_errorf(error, size, "cyclic package resource link");
        }
    }
    snag_file_info kind;
    if (snag_lstat(canonical, &kind) < 0 ||
        (!S_ISDIR(kind.st_mode) && !S_ISREG(kind.st_mode))) {
        free(canonical);
        return snag_errorf(error, size, "special files are unsupported in skill packages");
    }
    int input = snag_open_inspect_path(NULL, canonical);
    snag_file_info before;
    snag_file_info after;
    int rc = -1;
    if (input < 0 || snag_fstat(input, &before) < 0) goto done;
    if (S_ISDIR(before.st_mode)) {
        if (snag_mkdir_private_at(parent, name) < 0 ||
            json_array_append_new(ancestors, json_string(canonical)) < 0) goto done;
        int output = snag_open_inspect_at(parent, name);
        struct snag_directory *directory = snag_directory_open(input);
        if (!directory) { if (output >= 0) close(output); goto done; }
        int source_fd = input;
        input = -1;
        rc = output < 0 ? -1 : 0;
        const char *child;
        while (rc == 0 && (child = snag_directory_next(directory))) {
            if (!strcmp(child, ".") || !strcmp(child, "..") || !strcmp(child, ".git")) continue;
            char *next = snag_path_join(canonical, child);
            rc = next ? copy_package(next, root, output, child, ancestors, error, size) : -1;
            free(next);
        }
        if (rc == 0 && errno) rc = -1;
        if (output >= 0) {
            if (rc == 0 && snag_sync_dir(output) < 0) rc = -1;
            close(output);
        }
        if (rc == 0 && (snag_fstat(source_fd, &after) < 0 ||
            !snag_file_unchanged(&before, &after))) rc = -1;
        if (snag_directory_close(directory) < 0) rc = -1;
        (void)json_array_remove(ancestors, json_array_size(ancestors) - 1u);
    } else if (S_ISREG(before.st_mode)) {
        int output = snag_create_private_at(parent, name, true);
        rc = output < 0 ? -1 : 0;
        unsigned char buffer[65536];
        ssize_t amount;
        while (rc == 0 && (amount = read(input, buffer, sizeof(buffer))) != 0) {
            if (amount < 0 && errno == EINTR) continue;
            if (amount < 0 || snag_write_full(output, buffer, (size_t)amount) < 0) rc = -1;
        }
        if (rc == 0 && (snag_fstat(input, &after) < 0 ||
            !snag_file_unchanged(&before, &after))) {
            rc = snag_errorf(error, size, "package resource changed while copying");
        }
#ifndef _WIN32
        if (rc == 0) {
            struct snag_permissions permissions = {.mode = 0600 | (before.st_mode & 0111)};
            if (snag_permissions_apply(output, &permissions) < 0) rc = -1;
        }
#endif
        if (rc == 0 && snag_sync_file(output) < 0) rc = -1;
        if (output >= 0 && close(output) < 0) rc = -1;
    } else {
        rc = snag_errorf(error, size, "special files are unsupported in skill packages");
    }
done:
    if (input >= 0) close(input);
    free(canonical);
    if (rc < 0 && !*error) {
        (void)snag_errorf(error, size, "cannot copy package: %s", strerror(errno));
    }
    return rc;
}

static char *
package_storage(const char *dotdir)
{
    if (snag_mkdir_private(dotdir) < 0 && errno != EEXIST) return NULL;
    char *skills = snag_path_join(dotdir, "skills");
    char *storage = skills ? snag_path_join(skills, ".store") : NULL;
    int rc = skills ? snag_mkdir_private(skills) : -1;
    if (rc < 0 && errno == EEXIST) rc = 0;
    if (rc == 0 && storage && snag_mkdir_private(storage) < 0 && errno != EEXIST) rc = -1;
    free(skills);
    int fd = rc == 0 && storage ? snag_open_inspect_path(NULL, storage) : -1;
    struct snag_file_privacy privacy;
    if (fd < 0 || snag_fd_privacy(fd, &privacy) < 0 ||
        !privacy.effective_owner || !privacy.private_access) rc = -1;
    if (fd >= 0) close(fd);
    if (rc < 0) { free(storage); storage = NULL; }
    return storage;
}

static volatile sig_atomic_t retrieval_canceled;

static void
cancel_retrieval(int signal_number)
{
    retrieval_canceled = signal_number;
}

/* Git receives no repository/global filters or hooks. Its output may contain credentials. */
static int
git_run(const char *const *argv, struct snag_buf *output, char *error, size_t size)
{
    struct snag_child child;
    snag_child_init(&child);
    char **inherited = snag_environment_entries();
    size_t count = 0;
    while (inherited && inherited[count]) ++count;
    char **environment = calloc(count + 6u, sizeof(*environment));
    if (!environment || !inherited) {
        free(environment);
        snag_environment_entries_free(inherited);
        return -1;
    }
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!snag_environment_prefix(inherited[i], "GIT_")) environment[used++] = inherited[i];
    }
#ifdef _WIN32
    environment[used++] = "GIT_CONFIG_GLOBAL=NUL";
#else
    environment[used++] = "GIT_CONFIG_GLOBAL=/dev/null";
#endif
    environment[used++] = "GIT_CONFIG_NOSYSTEM=1";
    environment[used++] = "GIT_ATTR_NOSYSTEM=1";
    environment[used++] = "GIT_TERMINAL_PROMPT=0";
    environment[used++] = "GIT_LFS_SKIP_SMUDGE=1";
    retrieval_canceled = 0;
    int rc = -1;
    void (*previous_term)(int) = signal(SIGTERM, cancel_retrieval);
    if (previous_term == SIG_ERR) goto done;
    void (*previous_int)(int) = signal(SIGINT, cancel_retrieval);
    if (previous_int == SIG_ERR) {
        (void)signal(SIGTERM, previous_term);
        goto done;
    }
    if (snag_child_spawn_host_env(&child, argv, environment) < 0) goto restore;
    snag_child_close_stream(&child, 2u);
    bool reading[2] = {true, true};
    while (!retrieval_canceled) {
        for (unsigned int stream = 0; stream < 2u; ++stream) {
            if (!reading[stream]) continue;
            unsigned char buffer[65536];
            ssize_t amount = snag_child_read(&child, stream, buffer, sizeof(buffer));
            if (amount > 0 && stream == 0 && output &&
                snag_buf_append(output, buffer, (size_t)amount) < 0) goto restore;
            if (amount == 0 || (amount < 0 && errno != EINTR &&
                errno != EAGAIN && errno != EWOULDBLOCK)) {
                reading[stream] = false;
                snag_child_close_stream(&child, stream);
            }
        }
        int exited = snag_child_exited(&child);
        if (exited < 0) break;
        if (exited > 0 && !reading[0] && !reading[1]) {
            if (snag_child_reap(&child) == 0 && child.exit_code == 0) rc = 0;
            break;
        }
        struct snag_child_event events[] = {
            {.child = &child, .stream = 0, .events = reading[0] ? SNAG_CHILD_READ : 0u},
            {.child = &child, .stream = 1, .events = reading[1] ? SNAG_CHILD_READ : 0u}
        };
        if (snag_child_wait(events, 2u, SNAG_WAKE_INVALID, 100) < 0 && errno != EINTR) break;
    }
restore:
    snag_child_free(&child);
    (void)signal(SIGINT, previous_int);
    (void)signal(SIGTERM, previous_term);
done:
    free(environment);
    snag_environment_entries_free(inherited);
    if (rc < 0) (void)snag_errorf(error, size, retrieval_canceled ?
        "Git retrieval canceled; published installation unchanged" :
        "Git retrieval failed; check source, revision and existing Git/SSH authentication");
    return rc;
}

static bool
safe_subdir(const char *path)
{
    if (!path || !*path) return true;
    if (snag_path_root_len(path) || strchr(path, '\\')) return false;
    const char *part = path;
    for (;;) {
        const char *end = strchr(part, '/');
        size_t length = end ? (size_t)(end - part) : strlen(part);
        if (!length || (length == 1u && *part == '.') ||
            (length == 2u && !memcmp(part, "..", 2u))) return false;
        if (!end) return true;
        part = end + 1u;
    }
}

static bool
safe_source(const char *source)
{
    if (!source || !*source || source[0] == '-' || strchr(source, '\n') ||
        strchr(source, '\r') || strchr(source, '?') || strchr(source, '#')) return false;
    const char *scheme = strstr(source, "://");
    if (scheme) {
        if (strncmp(source, "https://", 8u) && strncmp(source, "ssh://", 6u) &&
            strncmp(source, "file://", 7u)) return false;
        const char *authority = scheme + 3u;
        const char *end = strchr(authority, '/');
        const char *at = strchr(authority, '@');
        if (at && (!end || at < end) &&
            (!strncmp(source, "https://", 8u) ||
                memchr(authority, ':', (size_t)(at - authority)))) {
            return false;
        }
    } else if (strstr(source, "::")) return false;
    return true;
}

static json_t *
receipt_for(const json_t *settings, const char *target, const char **name)
{
    json_t *installed = json_object_get(settings, "installed");
    json_t *receipt;
    const char *key;
    json_object_foreach(installed, key, receipt) {
        const char *id = snag_json_string(receipt, "id");
        if (target && (!strcmp(target, key) || (id && !strcmp(target, id)))) {
            *name = key;
            return receipt;
        }
    }
    return NULL;
}

json_t *
snag_skills_manage(const char *dotdir, const char *action, const char *target,
    const char *git_source, const char *ref, const char *subdir, char *error, size_t size)
{
    bool install = !strcmp(action, "install");
    bool remove = !strcmp(action, "remove");
    json_t *settings = snag_skills_settings_load(dotdir, error, size);
    json_t *original = NULL;
    json_t *skill = NULL;
    json_t *result = NULL;
    const char *selected_name = NULL;
    char *name = NULL;
    char *source = NULL;
    char *revision = NULL;
    char *relative = NULL;
    char *storage = NULL;
    char *stage = NULL;
    char *checkout = NULL;
    char *selected = NULL;
    char *package = NULL;
    char *generation = NULL;
    char *root = NULL;
    int parent = -1;
    int stage_fd = -1;
    int local = -1;
    struct snag_directory_lock lock = {.fd = -1};
    char random[SNAG_ID_HEX_LEN + 1u];
    char stage_name[64] = {0};
    struct snag_buf commit = {.max = SNAG_MAX_EVENT_LINE};
    if (!settings) goto done;
    if (!install) {
        original = receipt_for(settings, target, &selected_name);
        if (!original) {
            (void)snag_errorf(error, size,
                "unknown managed installation: %s", target ? target : "");
            goto done;
        }
        json_incref(original);
        name = strdup(selected_name);
    }
    if (remove) goto publish;
    source = strdup(git_source ? git_source : install ? (target ? target : "") :
        snag_json_string(original, "source"));
    revision = strdup(ref ? ref : original ? snag_json_string(original, "ref") : "HEAD");
    relative = strdup(subdir ? subdir : original ? snag_json_string(original, "subdir") : "");
    bool git = git_source || (original && json_is_true(json_object_get(original, "git")));
    if (!source || !revision || !relative || !safe_subdir(relative) ||
        !*revision || revision[0] == '-' || (git && !safe_source(source))) {
        (void)snag_errorf(error, size,
            "invalid source, credential-bearing URL, revision or package subdir");
        goto done;
    }
    storage = package_storage(dotdir);
    if (!storage) goto done;
    parent = snag_open_inspect_path(NULL, storage);
    struct snag_file_privacy privacy;
    if (parent < 0 || snag_fd_privacy(parent, &privacy) < 0 ||
        !privacy.effective_owner || !privacy.private_access) {
        (void)snag_errorf(error, size, "managed packages require private application storage");
        goto done;
    }
    if (snag_random_id(random) < 0) goto done;
    (void)snprintf(stage_name, sizeof(stage_name), ".stage-%s", random);
    if (snag_mkdir_private_at(parent, stage_name) < 0) goto done;
    stage = snag_path_join(storage, stage_name);
    stage_fd = snag_open_inspect_at(parent, stage_name);
    if (!stage || stage_fd < 0) goto done;
    if (git) {
        checkout = snag_path_join(stage, "checkout");
        char *program = snag_program_path("git");
        if (!checkout || !program) { free(program); goto done; }
        const char *clone[] = {program, "-c", "protocol.ext.allow=never", "clone",
            "--no-checkout", "--no-recurse-submodules", "--template=",
            "--", source, checkout, NULL};
        int rc = git_run(clone, NULL, error, size);
        struct snag_buf commitish = {.max = SNAG_MAX_EVENT_LINE};
        if (rc == 0 && (snag_buf_printf(&commitish, "%s^{commit}", revision) < 0 ||
            snag_buf_terminate(&commitish) < 0)) rc = -1;
        const char *resolve[] = {program, "-C", checkout, "rev-parse", "--verify",
            "--end-of-options", (char *)commitish.data, NULL};
        if (rc == 0) rc = git_run(resolve, &commit, error, size);
        snag_buf_free(&commitish);
        while (commit.len && (commit.data[commit.len - 1u] == '\n' ||
            commit.data[commit.len - 1u] == '\r')) --commit.len;
        if (rc == 0) rc = snag_buf_terminate(&commit);
        if (rc == 0 && ((commit.len != 40u && commit.len != 64u) ||
            !snag_hex_is_lower((char *)commit.data, commit.len))) rc = -1;
        const char *checkout_argv[] = {program, "-C", checkout, "-c", "core.hooksPath=",
            "-c", "submodule.recurse=false", "checkout", "--detach",
            (char *)commit.data, "--", NULL};
        if (rc == 0) rc = git_run(checkout_argv, NULL, error, size);
        free(program);
        if (rc < 0) goto done;
        selected = *relative ? snag_path_join(checkout, relative) : strdup(checkout);
        char *canonical = selected ? snag_realpath(selected) : NULL;
        char *checkout_root = snag_realpath(checkout);
        bool inside = canonical && checkout_root && within_package(canonical, checkout_root);
        free(checkout_root);
        free(selected);
        selected = canonical;
        if (!inside) {
            (void)snag_errorf(error, size, "selected package escapes Git checkout");
            goto done;
        }
    } else {
        selected = snag_realpath(source);
        if (selected) { free(source); source = strdup(selected); }
    }
    skill = selected ? snag_skill_load(selected, false, error, size) : NULL;
    if (!skill) goto done;
    const char *skill_name = snag_json_string(skill, "name");
    if (install) name = strdup(skill_name);
    if (!name || strcmp(name, skill_name) || strchr(name, '/') || strchr(name, '\\') ||
        !strcmp(name, ".") || !strcmp(name, "..")) {
        (void)snag_errorf(error, size, "managed package name must remain unchanged and path-safe");
        goto done;
    }
    json_t *ancestors = json_array();
    int copied = ancestors ? copy_package(selected, selected, stage_fd, name,
        ancestors, error, size) : -1;
    json_decref(ancestors);
    if (copied < 0) goto done;
    package = snag_path_join(stage, name);
    json_decref(skill);
    skill = package ? snag_skill_load(package, true, error, size) : NULL;
    if (!skill) goto done;
    if (checkout && remove_stage(stage_fd, "checkout") < 0) goto done;
    if (snag_sync_dir(stage_fd) < 0) goto done;
    generation = snag_path_join(storage, random);
    root = generation ? snag_path_join(generation, name) : NULL;
    char *identity_path = snag_path_join(storage, name);
    char identity[SNAG_SHA256_HEX_LEN + 1u];
    if (!identity_path || !root) { free(identity_path); goto done; }
    snag_sha256_hex(identity_path, strlen(identity_path), identity);
    free(identity_path);
    struct snag_buf id = {.max = SNAG_PATH_MAX_BYTES};
    if (snag_buf_printf(&id, "%s@%.16s", name, identity) == 0 && snag_buf_terminate(&id) == 0) {
        result = json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:b,s:s}",
            "id", (char *)id.data, "name", name, "root", root, "source", source,
            "ref", revision, "subdir", relative, "commit", git ? (char *)commit.data : "",
            "digest", snag_json_string(skill, "digest"), "git", git,
            "generation", random);
    }
    snag_buf_free(&id);
    if (!result) goto done;
publish:
    local = snag_open_inspect_path(NULL, dotdir);
    if (local < 0 || snag_directory_lock_acquire(local, &lock) < 0) goto done;
    json_decref(settings);
    settings = snag_skills_settings_load(dotdir, error, size);
    if (!settings) goto done;
    json_t *installed = json_object_get(settings, "installed");
    if (!installed) {
        installed = json_object();
        if (!installed || json_object_set_new(settings, "installed", installed) < 0) goto done;
    }
    json_t *current = json_object_get(installed, name);
    if ((install && current) || (!install && !json_equal(current, original))) {
        (void)snag_errorf(error, size, "installation changed concurrently; refresh and retry");
        goto done;
    }
    if (remove) {
        if (json_object_del(installed, name) < 0) goto done;
    } else {
        if (snag_rename_at(parent, stage_name, parent, random) < 0) goto done;
        stage_name[0] = 0;
        if (snag_sync_dir(parent) < 0 || json_object_set(installed, name, result) < 0) goto done;
    }
    if (snag_skills_settings_save(dotdir, settings, error, size) < 0) goto done;
    if (remove) result = json_pack("{s:s,s:s,s:b}", "action", "remove", "name", name,
        "generations_retained", true);
    goto success;
done:
    json_decref(result);
    result = NULL;
    if (!*error) (void)snag_errorf(error, size, "skills management failed: %s", strerror(errno));
success:
    if (lock.fd >= 0) (void)snag_directory_lock_release(&lock);
    if (local >= 0) close(local);
    if (stage_fd >= 0) close(stage_fd);
    if (parent >= 0) {
        if (*stage_name) (void)remove_stage(parent, stage_name);
        close(parent);
    }
    json_decref(settings);
    json_decref(original);
    json_decref(skill);
    free(name);
    free(source);
    free(revision);
    free(relative);
    free(storage);
    free(stage);
    free(checkout);
    free(selected);
    free(package);
    free(generation);
    free(root);
    snag_buf_free(&commit);
    return result;
}
