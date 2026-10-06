/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_workspace.h"
#include "json.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif

static void
write_file(int directory, const char *name, const char *text)
{
    int fd = snag_create_private_at(directory, name, true);
    assert(fd >= 0);
    assert(snag_write_full(fd, text, strlen(text)) == 0);
    assert(close(fd) == 0);
}

static void
assert_catalog(struct snag_store *store, uint64_t limit, size_t count, const char *first)
{
    char error[256];
    json_t *rows = snag_vm_workspace_list(store, limit, error, sizeof(error));
    assert(rows && json_array_size(rows) == count);
    if (first) assert(!strcmp(snag_json_string(json_array_get(rows, 0u), "name"), first));
    json_decref(rows);
}

#ifndef _WIN32
static void
assert_busy_elsewhere(struct snag_store *store, const char *id)
{
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        struct snag_vm_workspace other;
        snag_vm_workspace_init(&other);
        char error[256];
        assert(snag_vm_workspace_open(&other, store, id, error, sizeof(error)) < 0);
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
        snag_vm_workspace_close(&other);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void
owner_exit_test(struct snag_store *store, json_t *state)
{
    int ready[2], release[2];
    assert(pipe(ready) == 0 && pipe(release) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        (void)close(ready[0]);
        (void)close(release[1]);
        struct snag_vm_workspace workspace;
        snag_vm_workspace_init(&workspace);
        char error[256];
        assert(snag_vm_workspace_create(&workspace, store, "exited-owner", state, 1u,
            error, sizeof(error)) == 0);
        assert(write(ready[1], "y", 1u) == 1);
        char byte;
        assert(read(release[0], &byte, 1u) == 1);
        /* Simulate process loss without workspace cleanup or another snapshot. */
        _exit(0);
    }
    assert(close(ready[1]) == 0 && close(release[0]) == 0);
    char byte, error[256];
    assert(read(ready[0], &byte, 1u) == 1 && close(ready[0]) == 0);
    struct snag_vm_workspace workspace;
    snag_vm_workspace_init(&workspace);
    assert(snag_vm_workspace_open(&workspace, store, "exited-owner", error, sizeof(error)) < 0);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    assert(write(release[1], "y", 1u) == 1 && close(release[1]) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(snag_vm_workspace_open(&workspace, store, "exited-owner", error, sizeof(error)) == 0);
    assert(json_equal(json_object_get(workspace.snapshot, "state"), state));
    snag_vm_workspace_close(&workspace);
}
#endif

int
main(void)
{
    char id[SNAG_ID_HEX_LEN + 1u], name[64];
    assert(snag_random_id(id) == 0);
    (void)snprintf(name, sizeof(name), "snajpagent-vm-workspace-%s", id);
    char *root = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        name);
    assert(root && snag_mkdir_private(root) == 0);
    char *resolved = snag_realpath(root);
    free(root);
    root = resolved;
    assert(root);
    struct snag_store store;
    struct snag_vm_workspace first, second, opened;
    snag_store_init(&store);
    snag_vm_workspace_init(&first);
    snag_vm_workspace_init(&second);
    snag_vm_workspace_init(&opened);
    char error[256];
    assert(snag_store_open(&store, root, error, sizeof(error)) == 0);
    assert_catalog(&store, 10u, 0u, NULL);
    snag_file_info info;
    assert(snag_lstat_at(store.root_fd, "workspaces", &info) < 0 && errno == ENOENT);
    json_t *state = json_pack("{s:i,s:[s],s:s}", "layout_v", 1, "buffers", "session-id",
        "draft", "unsent text\n:qa");
    assert(state);
    assert(snag_vm_workspace_create(&first, &store, "operations", state, 10u,
        error, sizeof(error)) == 0);
    char first_id[SNAG_ID_HEX_LEN + 1u], second_id[SNAG_ID_HEX_LEN + 1u];
    memcpy(first_id, first.id, sizeof(first_id));
    assert_catalog(&store, 0u, 1u, "operations");
    assert(snag_vm_workspace_open(&opened, &store, first_id, error, sizeof(error)) < 0 &&
        errno == EAGAIN);
    /* Listing/probing must not release this process's existing ownership. */
#ifndef _WIN32
    assert_busy_elsewhere(&store, first_id);
#endif
    assert(snag_vm_workspace_create(&second, &store, "operations", state, 20u,
        error, sizeof(error)) < 0 && errno == EEXIST);
    assert(snag_vm_workspace_create(&second, &store, "newer", state, 20u,
        error, sizeof(error)) == 0);
    memcpy(second_id, second.id, sizeof(second_id));
    assert_catalog(&store, 0u, 2u, "newer");
    assert(snag_vm_workspace_save(&first, "newer", state, 30u,
        error, sizeof(error)) < 0 && errno == EEXIST);
    /* Caller edits cannot mutate the retained last-saved snapshot. */
    assert(json_object_set_new(state, "draft", json_string("edited")) == 0);
    assert(!strcmp(snag_json_string(json_object_get(first.snapshot, "state"), "draft"),
        "unsent text\n:qa"));
    assert(snag_vm_workspace_save(&first, "renamed", state, 30u, error, sizeof(error)) == 0);
    snag_vm_workspace_close(&first);
    /* Open rows precede newer stored rows; limit counts only stored workspaces. */
    assert_catalog(&store, 0u, 1u, "newer");
    assert_catalog(&store, 1u, 2u, "newer");
    snag_vm_workspace_close(&second);
    assert_catalog(&store, 1u, 1u, "renamed");
    assert(snag_vm_workspace_open(&opened, &store, NULL, error, sizeof(error)) == 0);
    assert(!strcmp(opened.id, first_id));
    assert(!strcmp(snag_json_string(json_object_get(opened.snapshot, "state"), "draft"), "edited"));
    snag_vm_workspace_close(&opened);
    assert(snag_vm_workspace_open(&opened, &store, "operations", error, sizeof(error)) < 0 &&
        errno == ENOENT);
    assert(snag_vm_workspace_open(&opened, &store, "renamed", error, sizeof(error)) == 0);
    assert(!strcmp(opened.id, first_id));
    snag_vm_workspace_close(&opened);
    char prefix[9];
    memcpy(prefix, second_id, 8u);
    prefix[8] = 0;
    assert(snag_vm_workspace_open(&opened, &store, prefix, error, sizeof(error)) == 0);
    assert(!strcmp(opened.id, second_id));
    /* An exact name wins over an ID-looking prefix. */
    assert(snag_vm_workspace_save(&opened, first_id, state, 40u, error, sizeof(error)) == 0);
    snag_vm_workspace_close(&opened);
    assert(snag_vm_workspace_open(&opened, &store, first_id, error, sizeof(error)) == 0);
    assert(!strcmp(opened.id, second_id));
    assert(snag_vm_workspace_save(&opened, "newer", state, 40u, error, sizeof(error)) == 0);
    assert(snag_vm_workspace_save(&opened, "bad\nname", state, 50u,
        error, sizeof(error)) < 0);
    assert(!strcmp(snag_json_string(opened.snapshot, "name"), "newer"));
    json_t *invalid_state = json_pack("{s:f}", "noncanonical", 1.5);
    assert(invalid_state && snag_vm_workspace_save(&opened, "newer", invalid_state, 50u,
        error, sizeof(error)) < 0);
    json_decref(invalid_state);
    assert(json_integer_value(json_object_get(opened.snapshot, "activity_ms")) == 40);
    /* Replaced directory paths cannot redirect a held workspace's writes. */
    assert(snag_rename_at(opened.root_fd, second_id, opened.root_fd, "retained") == 0);
    assert(snag_mkdir_private_at(opened.root_fd, second_id) == 0);
    assert(snag_vm_workspace_save(&opened, "redirected", state, 50u,
        error, sizeof(error)) < 0 && errno == ESTALE);
    assert(snag_unlink_at(opened.root_fd, second_id, true) == 0);
    assert(snag_rename_at(opened.root_fd, "retained", opened.root_fd, second_id) == 0);
    assert(snag_rename_at(store.root_fd, "workspaces", store.root_fd, "old-workspaces") == 0);
    assert(snag_mkdir_private_at(store.root_fd, "workspaces") == 0);
    assert(snag_vm_workspace_save(&opened, "redirected", state, 50u,
        error, sizeof(error)) < 0 && errno == ESTALE);
    assert(snag_unlink_at(store.root_fd, "workspaces", true) == 0);
    assert(snag_rename_at(store.root_fd, "old-workspaces", store.root_fd, "workspaces") == 0);
    /* Corrupt and newer-version snapshots stay byte-identical during listing. */
    assert(snag_rename_at(opened.dir_fd, "workspace.json", opened.dir_fd, "saved.json") == 0);
    const char *corrupt = "{\"v\":999,\"draft\":\"keep this\"}";
    write_file(opened.dir_fd, "workspace.json", corrupt);
    json_t *rows = snag_vm_workspace_list(&store, 0u, error, sizeof(error));
    assert(rows && json_array_size(rows) == 1u &&
        json_object_get(json_array_get(rows, 0u), "error"));
    json_decref(rows);
    int fd = snag_open_read_at(opened.dir_fd, "workspace.json", false);
    char bytes[128] = {0};
    assert(fd >= 0 && read(fd, bytes, sizeof(bytes)) == (ssize_t)strlen(corrupt));
    assert(!strcmp(bytes, corrupt) && close(fd) == 0);
    snag_vm_workspace_close(&opened);
    assert(snag_vm_workspace_open(&opened, &store, second_id, error, sizeof(error)) < 0);
    assert(strstr(error, "snapshot"));
    int workspaces = snag_open_read_at(store.root_fd, "workspaces", true);
    int directory = snag_open_read_at(workspaces, second_id, true);
    assert(directory >= 0);
    assert(snag_unlink_at(directory, "workspace.json", false) == 0);
    assert(snag_rename_at(directory, "saved.json", directory, "workspace.json") == 0);
    assert(close(directory) == 0 && close(workspaces) == 0);
    assert(snag_vm_workspace_open(&opened, &store, second_id, error, sizeof(error)) == 0);
    /* Listing ignores an unrelated file and leaves it in place. */
    write_file(opened.root_fd, "unrelated", "keep");
    snag_vm_workspace_close(&opened);
    assert_catalog(&store, 10u, 2u, "newer");
#ifndef _WIN32
    owner_exit_test(&store, state);
#endif
    json_decref(state);
    snag_store_close(&store);
    free(root);
    puts("test_vm_workspace: ok");
    return 0;
}
