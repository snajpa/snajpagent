/* SPDX-License-Identifier: GPL-2.0-only */
#include "skills.h"
#include "config.h"
#include "base.h"
#include "fs.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
snag_skills_settings_save(const char *dotdir, const json_t *settings, char *error, size_t size)
{
    int parent = snag_open_inspect_path(NULL, dotdir);
    if (parent < 0) return snag_errorf(error, size, "%s: %s", dotdir, strerror(errno));
    char id[SNAG_ID_HEX_LEN + 1u];
    char name[64] = {0};
    json_t *policy = json_deep_copy(settings);
    if (!policy) { close(parent); return -1; }
    json_object_del(policy, "roots");
    json_object_del(policy, "inspect_all");
    if (json_object_set_new(policy, "version", json_integer(1)) < 0) {
        json_decref(policy);
        close(parent);
        return -1;
    }
    char *text = json_dumps(policy, JSON_INDENT(2) | JSON_SORT_KEYS);
    json_decref(policy);
    int rc = -1;
    int fd = -1;
    if (!text || snag_random_id(id) < 0) goto done;
    (void)snprintf(name, sizeof(name), ".skills-%s.tmp", id);
    fd = snag_create_private_at(parent, name, true);
    if (fd < 0 || snag_write_full(fd, text, strlen(text)) < 0 || snag_sync_file(fd) < 0) {
        goto done;
    }
    if (close(fd) < 0) { fd = -1; goto done; }
    fd = -1;
    if (snag_rename_at(parent, name, parent, "skills.json") < 0) goto done;
    if (snag_sync_dir(parent) < 0) goto done;
    rc = 0;
done:
    if (fd >= 0) close(fd);
    if (rc < 0 && *name) (void)snag_unlink_at(parent, name, false);
    if (rc < 0) {
        (void)snag_errorf(error, size, "cannot save local skills settings: %s", strerror(errno));
    }
    free(text);
    close(parent);
    return rc;
}

static int
set_membership(json_t *settings, const char *key, const char *value, bool present)
{
    json_t *array = json_object_get(settings, key);
    if (!array) {
        array = json_array();
        if (!array || json_object_set_new(settings, key, array) < 0) return -1;
    }
    if (!json_is_array(array)) return snag_errno(EINVAL);
    for (size_t i = 0; i < json_array_size(array); ++i) {
        const char *text = json_string_value(json_array_get(array, i));
        if (text && !strcmp(text, value)) return present ? 0 : json_array_remove(array, i);
    }
    return present ? json_array_append_new(array, json_string(value)) : 0;
}

static json_t *
scaffold(const char *destination, char *error, size_t size)
{
    const char *name = strrchr(destination, '/');
    name = name ? name + 1u : destination;
    size_t length = strlen(name);
    bool valid = length > 0u && length <= 64u && name[0] != '-' && name[length - 1u] != '-';
    for (size_t i = 0; valid && i < length; ++i) {
        unsigned char ch = (unsigned char)name[i];
        valid = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            (ch == '-' && (!i || name[i - 1u] != '-'));
    }
    if (!valid) {
        (void)snag_errorf(error, size,
            "create requires a lowercase hyphenated skill directory name");
        return NULL;
    }
    if (snag_mkdir_private(destination) < 0) {
        (void)snag_errorf(error, size, "%s: %s", destination, strerror(errno));
        return NULL;
    }
    char *path = snag_path_join(destination, "SKILL.md");
    struct snag_buf content = {.max = SNAG_MAX_EVENT_LINE};
    int fd = path ? snag_open_inspect_path(NULL, destination) : -1;
    int output = fd >= 0 ? snag_create_private_at(fd, "SKILL.md", true) : -1;
    json_t *result = NULL;
    json_t *name_json = json_string(name);
    char *quoted = name_json ? json_dumps(name_json, JSON_ENCODE_ANY) : NULL;
    json_decref(name_json);
    if (quoted && output >= 0 && snag_buf_printf(&content,
        "---\nname: %s\ndescription: Describe the task and when this skill applies.\n---\n\n"
        "# %s\n\nDescribe the workflow, required inputs and how to verify the result.\n"
        "Use scripts/, references/ and assets/ for supporting resources.\n", quoted, name) == 0 &&
        snag_write_full(output, content.data, content.len) == 0 && snag_sync_file(output) == 0) {
        result = snag_skill_load(destination, true, error, size);
    }
    if (output >= 0) close(output);
    if (fd >= 0) close(fd);
    free(quoted);
    free(path);
    snag_buf_free(&content);
    return result;
}

int
snag_skills_main(int argc, char **argv)
{
    const char *command = "list";
    const char *operand = NULL;
    const char *dotdir_arg = NULL;
    const char *config_path = NULL;
    const char *cwd_arg = NULL;
    const char *query = NULL;
    const char *root = NULL;
    const char *git = NULL;
    const char *ref = NULL;
    const char *subdir = NULL;
    bool compact = false;
    bool help = false;
    bool all = false;
    bool seen_command = false;
    size_t offset = 0;
    size_t limit = 50u;
    char error[512] = "invalid skills command";
    for (int i = 0; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help")) help = true;
        else if (!strcmp(arg, "--json")) compact = true;
        else if (!strcmp(arg, "--all-skills")) all = true;
        else if ((!strcmp(arg, "--dotdir") || !strcmp(arg, "--config") ||
            !strcmp(arg, "--cwd") ||
            !strcmp(arg, "--query") || !strcmp(arg, "--root") ||
            !strcmp(arg, "--git") || !strcmp(arg, "--ref") || !strcmp(arg, "--subdir") ||
            !strcmp(arg, "--offset") || !strcmp(arg, "--limit")) && i + 1 < argc) {
            const char *value = argv[++i];
            if (!strcmp(arg, "--dotdir")) dotdir_arg = value;
            else if (!strcmp(arg, "--config")) config_path = value;
            else if (!strcmp(arg, "--cwd")) cwd_arg = value;
            else if (!strcmp(arg, "--query")) query = value;
            else if (!strcmp(arg, "--root")) root = value;
            else if (!strcmp(arg, "--git")) git = value;
            else if (!strcmp(arg, "--ref")) ref = value;
            else if (!strcmp(arg, "--subdir")) subdir = value;
            else {
                uint64_t number;
                if (snag_parse_count(value, &number) < 0 || number > SIZE_MAX) goto usage;
                if (!strcmp(arg, "--offset")) offset = (size_t)number;
                else limit = (size_t)number;
            }
        } else if (!seen_command && arg[0] != '-') {
            command = arg;
            seen_command = true;
        } else if (!operand && arg[0] != '-') operand = arg;
        else goto usage;
    }
    if (help) goto usage;
    char *cwd = cwd_arg ? strdup(cwd_arg) : snag_realpath(".");
    const char *home = getenv("HOME");
    char *dotdir = dotdir_arg ? strdup(dotdir_arg) :
        home ? snag_path_join(home, ".snajpagent") : NULL;
    if (dotdir && !snag_path_root_len(dotdir)) {
        char *launch = snag_realpath(".");
        char *absolute = launch ? snag_path_join(launch, dotdir) : NULL;
        free(launch);
        free(dotdir);
        dotdir = absolute;
    }
    struct snag_config config;
    snag_config_init(&config);
    json_t *settings = NULL;
    json_t *catalog = NULL;
    json_t *result = NULL;
    struct snag_directory_lock lock = {.fd = -1};
    int parent = -1;
    int rc = 1;
    if (!dotdir) goto done;
    bool changing = !strcmp(command, "enable") || !strcmp(command, "disable") ||
        !strcmp(command, "ban") || !strcmp(command, "unban");
    if (changing) {
        if (snag_mkdir_private(dotdir) < 0 && errno != EEXIST) goto done;
        parent = snag_open_inspect_path(NULL, dotdir);
        if (parent < 0 || snag_directory_lock_acquire(parent, &lock) < 0) goto done;
    }
    settings = snag_skills_settings_load(dotdir, error, sizeof(error));
    if (!settings) goto done;
    if (snag_config_load(&config, config_path, dotdir, error, sizeof(error)) < 0) goto done;
    if (config.skills_roots && json_object_set(settings, "roots", config.skills_roots) < 0) {
        goto done;
    }
    if (root) {
        json_t *roots = json_pack("[s]", root);
        if (!roots || json_object_set_new(settings, "roots", roots) < 0) goto done;
    }
    bool inspecting = all || changing || !strcmp(command, "review") || !strcmp(command, "paths");
    if (json_object_set_new(settings, "inspect_all", json_boolean(inspecting)) < 0) goto done;
    if (!strcmp(command, "install") || !strcmp(command, "update") || !strcmp(command, "remove")) {
        result = snag_skills_manage(dotdir, command, operand, git, ref, subdir,
            error, sizeof(error));
    } else if (!strcmp(command, "validate")) {
        if (!operand) goto done;
        result = snag_skill_load(operand, true, error, sizeof(error));
    } else if (!strcmp(command, "create")) {
        if (!operand) goto done;
        result = scaffold(operand, error, sizeof(error));
    } else {
        catalog = snag_skills_discover(cwd, dotdir, settings, error, sizeof(error));
        if (!catalog) goto done;
        if (!strcmp(command, "paths")) {
            json_t *managed = json_array();
            char *storage = snag_path_join(dotdir, "skills/.store");
            const char *name;
            json_t *receipt;
            json_object_foreach(json_object_get(settings, "installed"), name, receipt) {
                (void)name;
                if (managed && json_array_append(managed, receipt) < 0) {
                    json_decref(managed);
                    managed = NULL;
                    break;
                }
            }
            if (managed && storage) result = json_pack("{s:O,s:O,s:s,s:O,s:O}",
                "roots", json_object_get(catalog, "roots"), "managed", managed,
                "excluded_retained_storage", storage,
                "linked_origins", json_object_get(catalog, "linked_origins"),
                "diagnostics", json_object_get(catalog, "diagnostics"));
            json_decref(managed);
            free(storage);
        } else if (operand && (!strcmp(command, "ban") || !strcmp(command, "unban"))) {
            const json_t *entry = snag_skills_resolve(catalog, operand, error, sizeof(error));
            json_t *known = json_object_get(json_object_get(settings, "approvals"), operand);
            if (!known) known = json_object_get(json_object_get(settings, "reviews"), operand);
            const char *name = entry ? snag_json_string(entry, "name") :
                known ? snag_json_string(known, "name") : operand;
            if (!snag_skill_name_valid(name)) goto done;
            if (set_membership(settings, "banned", name, !strcmp(command, "ban")) < 0) goto done;
            json_t *approvals = json_object_get(settings, "approvals");
            const char *id;
            json_t *approval;
            void *next;
            json_object_foreach_safe(approvals, next, id, approval) {
                const char *approved_name = snag_json_string(approval, "name");
                if (approved_name && !strcmp(approved_name, name)) json_object_del(approvals, id);
            }
            if (!strcmp(command, "ban")) {
                json_t *reviews = json_object_get(settings, "reviews");
                json_object_foreach_safe(reviews, next, id, approval) {
                    const char *review_name = snag_json_string(approval, "name");
                    if (review_name && !strcmp(review_name, name)) json_object_del(reviews, id);
                }
            }
            if (snag_skills_settings_save(dotdir, settings, error, sizeof(error)) == 0) {
                result = json_pack("{s:s,s:s}", "action", command, "name", name);
            }
        } else if (!strcmp(command, "list") || !strcmp(command, "refresh")) {
            result = snag_skills_list(catalog, query ? query : operand, offset, limit);
        } else if (operand && (!strcmp(command, "show") || !strcmp(command, "review") ||
            !strcmp(command, "enable") ||
            !strcmp(command, "disable"))) {
            const json_t *entry = snag_skills_resolve(catalog, operand, error, sizeof(error));
            if (!entry) goto done;
            if (!strcmp(command, "review")) {
                result = snag_skills_review(dotdir, entry, error, sizeof(error));
            } else if (!strcmp(command, "show")) {
                result = snag_skill_snapshot(entry, error, sizeof(error));
                if (result && json_object_set_new(result, "adversarial",
                    json_boolean(!json_is_true(json_object_get(entry, "approved")))) < 0) {
                    json_decref(result);
                    result = NULL;
                }
            } else if (!strcmp(command, "enable") &&
                !json_is_true(json_object_get(entry, "approved"))) {
                (void)snag_errorf(error, sizeof(error),
                    "enable requires current operator approval");
            } else if (set_membership(settings, "disabled", snag_json_string(entry, "id"),
                !strcmp(command, "disable")) == 0 &&
                snag_skills_settings_save(dotdir, settings, error, sizeof(error)) == 0) {
                result = json_pack("{s:s,s:s}", "action", command,
                    "id", snag_json_string(entry, "id"));
            }
        }
    }
    if (result) {
        char *output = json_dumps(result,
            JSON_SORT_KEYS | (compact ? JSON_COMPACT : JSON_INDENT(2)));
        if (output) {
            rc = printf("%s\n", output) < 0 ? 1 : 0;
            free(output);
        }
    }
done:
    if (rc) (void)fprintf(stderr, "snajpagent skills: %s\n", error);
    if (lock.fd >= 0) (void)snag_directory_lock_release(&lock);
    if (parent >= 0) close(parent);
    snag_config_free(&config);
    json_decref(result);
    json_decref(catalog);
    json_decref(settings);
    free(cwd);
    free(dotdir);
    return rc;
usage:
    (void)fprintf(stderr,
        "usage: snajpagent skills [--dotdir DIR] [--config FILE] [--cwd DIR] "
        "[--json] [--all-skills] "
        "list|show|paths|review|validate|create|enable|disable|ban|unban|refresh|"
        "install|update|remove [TARGET] [--git SOURCE] [--ref REF] [--subdir PATH]\n");
    return help ? 0 : 2;
}
