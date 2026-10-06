/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_workspace.h"
#include "json.h"
#include "store_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Windows directory mutexes are recursive within their owning thread. Track
 * our frontend's handles so a probe/open cannot mistake recursion for vacancy. */
static _Thread_local struct snag_vm_workspace *owned_workspaces;

void
snag_vm_workspace_init(struct snag_vm_workspace *workspace)
{
    memset(workspace, 0, sizeof(*workspace));
    workspace->root_fd = workspace->dir_fd = workspace->ownership.fd = -1;
}

void
snag_vm_workspace_close(struct snag_vm_workspace *workspace)
{
    for (struct snag_vm_workspace **p = &owned_workspaces; *p; p = &(*p)->next_owned) {
        if (*p == workspace) {
            *p = workspace->next_owned;
            break;
        }
    }
    (void)snag_directory_lock_release(&workspace->ownership);
    if (workspace->dir_fd >= 0) (void)close(workspace->dir_fd);
    if (workspace->root_fd >= 0) (void)close(workspace->root_fd);
    json_decref(workspace->snapshot);
    snag_vm_workspace_init(workspace);
}

static bool
owned_directory(int fd)
{
    snag_file_info candidate, held;
    if (snag_fstat(fd, &candidate) < 0) return false;
    for (struct snag_vm_workspace *p = owned_workspaces; p; p = p->next_owned) {
        if (snag_fstat(p->dir_fd, &held) == 0 && held.st_dev == candidate.st_dev &&
            held.st_ino == candidate.st_ino) return true;
    }
    return false;
}

static int
own_workspace(struct snag_vm_workspace *workspace)
{
    if (owned_directory(workspace->dir_fd)) return snag_errno(EAGAIN);
    if (snag_directory_lock_acquire(workspace->dir_fd, &workspace->ownership) < 0) return -1;
    workspace->next_owned = owned_workspaces;
    owned_workspaces = workspace;
    return 0;
}

static bool
valid_id(const char *text)
{
    return text && strlen(text) == SNAG_ID_HEX_LEN &&
        snag_hex_is_lower(text, SNAG_ID_HEX_LEN);
}

static int
private_directory(int parent, const char *name, bool create, char *error, size_t size)
{
    if (create && snag_mkdir_private_at(parent, name) < 0 && errno != EEXIST)
        return snag_errorf(error, size, "cannot create workspace directory: %s", strerror(errno));
    int fd = snag_open_read_security_at(parent, name, true);
    if (fd < 0) return -1;
    if (snag_store_verify_private_fd(fd, true, "workspace directory", error, size) < 0) {
        int saved = errno;
        (void)close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static bool
snapshot_valid(const json_t *value, const char *id)
{
    uint64_t activity;
    const char *saved_id = snag_json_string(value, "id");
    const char *name = snag_json_string(value, "name");
    return snag_json_exact_keys(value, "v id name activity_ms state") &&
        json_integer_value(json_object_get(value, "v")) == 1 &&
        saved_id && strlen(saved_id) == json_string_length(json_object_get(value, "id")) &&
        !strcmp(saved_id, id) && name &&
        strlen(name) == json_string_length(json_object_get(value, "name")) &&
        (!*name || snag_session_name_valid(name)) &&
        snag_json_integer_u64(value, "activity_ms", &activity) == 0 &&
        json_is_object(json_object_get(value, "state"));
}

static json_t *
read_snapshot(int directory, const char *id, char *error, size_t size)
{
    int fd = snag_open_read_security_at(directory, "workspace.json", false);
    if (fd < 0) {
        (void)snag_errorf(error, size, "cannot read workspace snapshot: %s", strerror(errno));
        return NULL;
    }
    snag_file_info info;
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    json_t *value = NULL;
    if (snag_store_verify_private_fd(fd, false, "workspace snapshot", error, size) < 0)
        goto out;
    if (snag_fstat(fd, &info) < 0 || info.st_nlink != 1 ||
        info.st_size < 0 || info.st_size > SNAG_MAX_EVENT_LINE) {
        (void)snag_fail(error, size, EINVAL, "invalid workspace snapshot file");
        goto out;
    }
    if (snag_buf_read(&bytes, fd) < 0) goto out;
    value = snag_json_load_strict(bytes.data, bytes.len, bytes.max, error, size);
    if (value && !snapshot_valid(value, id)) {
        (void)snag_fail(error, size, EINVAL, "unsupported or invalid workspace snapshot");
        json_decref(value);
        value = NULL;
    }
out: {
        int saved = errno;
        (void)close(fd);
        snag_buf_free(&bytes);
        errno = saved;
        return value;
    }
}

static int
same_directory(int parent, const char *name, int held)
{
    snag_file_info expected, actual;
    if (snag_fstat(held, &expected) < 0 || snag_lstat_at(parent, name, &actual) < 0)
        return -1;
    if (!S_ISDIR(actual.st_mode) || expected.st_dev != actual.st_dev ||
        expected.st_ino != actual.st_ino) return snag_errno(ESTALE);
    return 0;
}

static int
workspace_identity(struct snag_vm_workspace *workspace, char *error, size_t size)
{
    if (workspace->ownership.fd < 0)
        return snag_fail(error, size, EINVAL, "workspace is not open");
    if (same_directory(workspace->store->root_fd, "workspaces", workspace->root_fd) < 0 ||
        same_directory(workspace->root_fd, workspace->id, workspace->dir_fd) < 0)
        return snag_errorf(error, size, "workspace directory changed: %s", strerror(errno));
    return 0;
}

static struct snag_directory *
scan_directory(int root)
{
    int fd = snag_open_read_security_at(root, ".", true);
    if (fd < 0) return NULL;
    struct snag_directory *scan = snag_directory_open(fd);
    if (!scan) (void)close(fd);
    return scan;
}

static int
unique_name(int root, const char *name, const char *except, char *error, size_t size)
{
    if (!*name) return 0;
    struct snag_directory *scan = scan_directory(root);
    if (!scan) return -1;
    int rc = 0;
    for (;;) {
        errno = 0;
        const char *id = snag_directory_next(scan);
        if (!id) {
            if (errno) rc = -1;
            break;
        }
        if (!valid_id(id) || !strcmp(id, except)) continue;
        int fd = private_directory(root, id, false, error, size);
        json_t *snapshot = fd < 0 ? NULL : read_snapshot(fd, id, error, size);
        if (fd >= 0) (void)close(fd);
        if (!snapshot) {
            rc = snag_errorf(error, size, "cannot verify workspace names; inspect %s", id);
            break;
        }
        bool same = !strcmp(snag_json_string(snapshot, "name"), name);
        json_decref(snapshot);
        if (same) {
            rc = snag_fail(error, size, EEXIST, "workspace name already exists: %s", id);
            break;
        }
    }
    int saved = errno;
    if (snag_directory_close(scan) < 0 && !rc) return -1;
    errno = saved;
    return rc;
}

static int
write_snapshot(struct snag_vm_workspace *workspace, const char *name, const json_t *state,
    uint64_t activity, char *error, size_t size)
{
    if (!name || (*name && !snag_session_name_valid(name)) || !json_is_object(state) ||
        activity > INT64_MAX)
        return snag_fail(error, size, EINVAL, "invalid workspace name, state or activity");
    json_t *copy = json_deep_copy(state);
    json_t *snapshot = copy ? json_pack("{s:i,s:s,s:s,s:I,s:o}", "v", 1, "id", workspace->id,
        "name", name, "activity_ms", (json_int_t)activity, "state", copy) : NULL;
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    char suffix[SNAG_ID_HEX_LEN + 1u], temporary[64];
    int fd = -1, rc = -1;
    bool created = false;
    if (!snapshot || snag_json_canonical(snapshot, &bytes) < 0 ||
        snag_random_id(suffix) < 0) goto out;
    (void)snprintf(temporary, sizeof(temporary), ".workspace-%s.tmp", suffix);
    fd = snag_create_private_at(workspace->dir_fd, temporary, true);
    if (fd < 0) goto out;
    created = true;
    if (snag_write_full(fd, bytes.data, bytes.len) < 0 || snag_sync_file(fd) < 0) goto out;
    if (close(fd) < 0) {
        fd = -1;
        goto out;
    }
    fd = -1;
    if (workspace_identity(workspace, error, size) < 0 ||
        snag_rename_at(workspace->dir_fd, temporary, workspace->dir_fd, "workspace.json") < 0)
        goto out;
    created = false;
    if (snag_sync_dir(workspace->dir_fd) < 0 || snag_sync_dir(workspace->root_fd) < 0 ||
        snag_sync_dir(workspace->store->root_fd) < 0)
        goto out;
    json_decref(workspace->snapshot);
    workspace->snapshot = snapshot;
    snapshot = NULL;
    rc = 0;
out: {
        int saved = errno;
        if (fd >= 0) (void)close(fd);
        if (created) (void)snag_unlink_at(workspace->dir_fd, temporary, false);
        json_decref(snapshot);
        snag_buf_free(&bytes);
        if (rc < 0) (void)snag_errorf(error, size, "cannot save workspace: %s", strerror(saved));
        errno = saved;
        return rc;
    }
}

int
snag_vm_workspace_save(struct snag_vm_workspace *workspace, const char *name,
    const json_t *state, uint64_t activity, char *error, size_t size)
{
    if (workspace_identity(workspace, error, size) < 0) return -1;
    if (!name) return snag_fail(error, size, EINVAL, "workspace name is required");
    struct snag_directory_lock names = {.fd = -1};
    if (snag_directory_lock_acquire(workspace->root_fd, &names) < 0)
        return snag_errorf(error, size, "workspace names are busy: %s", strerror(errno));
    const char *old_name = snag_json_string(workspace->snapshot, "name");
    int rc = old_name && !strcmp(old_name, name) ? 0 :
        unique_name(workspace->root_fd, name, workspace->id, error, size);
    if (!rc) rc = write_snapshot(workspace, name, state, activity, error, size);
    int saved = errno;
    (void)snag_directory_lock_release(&names);
    errno = saved;
    return rc;
}

int
snag_vm_workspace_create(struct snag_vm_workspace *workspace, struct snag_store *store,
    const char *name, const json_t *state, uint64_t activity, char *error, size_t size)
{
    if (workspace->root_fd >= 0 || !name || !json_is_object(state) ||
        (*name && !snag_session_name_valid(name)))
        return snag_fail(error, size, EINVAL, "invalid new workspace");
    workspace->store = store;
    workspace->root_fd = private_directory(store->root_fd, "workspaces", true, error, size);
    if (workspace->root_fd < 0) return -1;
    struct snag_directory_lock names = {.fd = -1};
    bool created = false;
    int rc = -1;
    if (snag_directory_lock_acquire(workspace->root_fd, &names) < 0 ||
        unique_name(workspace->root_fd, name, "", error, size) < 0 ||
        snag_random_id(workspace->id) < 0) goto out;
    if (snag_mkdir_private_at(workspace->root_fd, workspace->id) < 0) goto out;
    created = true;
    workspace->dir_fd = private_directory(workspace->root_fd, workspace->id, false, error, size);
    if (workspace->dir_fd < 0 || own_workspace(workspace) < 0) goto out;
    rc = write_snapshot(workspace, name, state, activity, error, size);
out: {
        int saved = errno;
        if (rc < 0 && created) {
            /* Remove only our empty incomplete directory. A post-rename sync
             * failure leaves its valid snapshot available for explicit resume. */
            (void)snag_unlink_at(workspace->root_fd, workspace->id, true);
        }
        (void)snag_directory_lock_release(&names);
        if (rc < 0) snag_vm_workspace_close(workspace);
        errno = saved;
        return rc;
    }
}

static int
compare_rows(const void *left, const void *right)
{
    const json_t *a = *(const json_t *const *)left;
    const json_t *b = *(const json_t *const *)right;
    int ar = json_is_true(json_object_get(a, "open")) ? 0 : json_object_get(a, "error") ? 1 : 2;
    int br = json_is_true(json_object_get(b, "open")) ? 0 : json_object_get(b, "error") ? 1 : 2;
    if (ar != br) return ar < br ? -1 : 1;
    json_int_t at = json_integer_value(json_object_get(a, "activity_ms"));
    json_int_t bt = json_integer_value(json_object_get(b, "activity_ms"));
    if (at != bt) return at > bt ? -1 : 1;
    return strcmp(snag_json_string(a, "id"), snag_json_string(b, "id"));
}

static json_t *
catalog_row(int root, const char *id)
{
    char error[256] = "cannot inspect workspace";
    int fd = private_directory(root, id, false, error, sizeof(error));
    json_t *snapshot = fd < 0 ? NULL : read_snapshot(fd, id, error, sizeof(error));
    struct snag_directory_lock probe = {.fd = -1};
    bool open = fd >= 0 && owned_directory(fd);
    if (fd >= 0 && !open && snag_directory_lock_acquire(fd, &probe) < 0) {
        open = errno == EAGAIN || errno == EWOULDBLOCK;
        if (!open) {
            json_decref(snapshot);
            snapshot = NULL;
            (void)snag_errorf(error, sizeof(error), "cannot inspect workspace ownership");
        }
    }
    (void)snag_directory_lock_release(&probe);
    if (fd >= 0) (void)close(fd);
    json_t *row = json_pack("{s:s,s:b}", "id", id, "open", open);
    if (row && snapshot) {
        if (json_object_set(row, "name", json_object_get(snapshot, "name")) < 0 ||
            json_object_set(row, "activity_ms", json_object_get(snapshot, "activity_ms")) < 0) {
            json_decref(row);
            row = NULL;
        }
    } else if (row && json_object_set_new(row, "error", json_string(error)) < 0) {
        json_decref(row);
        row = NULL;
    }
    json_decref(snapshot);
    return row;
}

json_t *
snag_vm_workspace_list(struct snag_store *store, uint64_t limit, char *error, size_t size)
{
    int root = private_directory(store->root_fd, "workspaces", false, error, size);
    if (root < 0) return errno == ENOENT ? json_array() : NULL;
    struct snag_directory *scan = scan_directory(root);
    json_t **rows = NULL, *result = NULL;
    size_t count = 0u, retained = 0u;
    if (!scan) goto out;
    for (;;) {
        errno = 0;
        const char *id = snag_directory_next(scan);
        if (!id) {
            if (errno) goto out;
            break;
        }
        if (!valid_id(id)) continue;
        if (count >= SNAG_MEMORY_LIMIT / sizeof(*rows)) goto out;
        json_t **grown = realloc(rows, (count + 1u) * sizeof(*rows));
        if (!grown) goto out;
        rows = grown;
        rows[count] = catalog_row(root, id);
        if (!rows[count]) goto out;
        ++count;
        const char *name = snag_json_string(rows[count - 1u], "name");
        const char *problem = snag_json_string(rows[count - 1u], "error");
        size_t cost = 1024u + (name ? strlen(name) : 0u) + (problem ? strlen(problem) : 0u);
        if (cost > SNAG_MEMORY_LIMIT / 4u - retained) {
            (void)snag_fail(error, size, EOVERFLOW, "workspace listing exceeds its memory budget");
            goto out;
        }
        retained += cost;
    }
    if (count > 1u) qsort(rows, count, sizeof(*rows), compare_rows);
    result = json_array();
    for (size_t i = 0u; result && i < count; ++i) {
        bool stored = !json_is_true(json_object_get(rows[i], "open")) &&
            !json_object_get(rows[i], "error");
        if (stored && !limit) continue;
        if (stored) --limit;
        if (json_array_append(result, rows[i]) < 0) {
            json_decref(result);
            result = NULL;
        }
    }
out: {
        int saved = errno;
        if (scan) (void)snag_directory_close(scan);
        (void)close(root);
        for (size_t i = 0u; i < count; ++i) json_decref(rows[i]);
        free(rows);
        errno = saved;
        return result;
    }
}

int
snag_vm_workspace_open(struct snag_vm_workspace *workspace, struct snag_store *store,
    const char *selector, char *error, size_t size)
{
    if (workspace->root_fd >= 0 || (selector && !*selector))
        return snag_fail(error, size, EINVAL, "invalid workspace selector");
    workspace->store = store;
    workspace->root_fd = private_directory(store->root_fd, "workspaces", false, error, size);
    if (workspace->root_fd < 0) return -1;
    struct snag_directory_lock names = {.fd = -1};
    if (snag_directory_lock_acquire(workspace->root_fd, &names) < 0) {
        snag_vm_workspace_close(workspace);
        return snag_errorf(error, size, "workspace names are busy: %s", strerror(errno));
    }
    json_t *rows = snag_vm_workspace_list(store, UINT64_MAX, error, size);
    if (!rows) {
        (void)snag_directory_lock_release(&names);
        snag_vm_workspace_close(workspace);
        return -1;
    }
    const char *selected = NULL;
    size_t matches = 0u;
    uint64_t newest = 0u;
    struct snag_buf candidates = {.max = 4096u};
    /* Exact names have priority even when they look like an ID prefix. */
    for (unsigned int pass = 0u; pass < 2u && !matches; ++pass) {
        for (size_t i = 0u; i < json_array_size(rows); ++i) {
            const json_t *row = json_array_get(rows, i);
            if (json_object_get(row, "error") && (!selector || !pass)) continue;
            const char *id = snag_json_string(row, "id");
            bool match = !selector;
            if (selector) {
                match = pass == 0u ? !strcmp(selector, snag_json_string(row, "name")) :
                    strlen(selector) <= SNAG_ID_HEX_LEN &&
                    !strncmp(selector, id, strlen(selector));
            }
            if (!match) continue;
            uint64_t activity = (uint64_t)json_integer_value(json_object_get(row, "activity_ms"));
            if (!selector) {
                if (selected && (activity < newest ||
                    (activity == newest && strcmp(id, selected) > 0))) continue;
                selected = id;
                newest = activity;
                matches = 1u;
            } else {
                selected = id;
                ++matches;
                (void)snag_buf_printf(&candidates, "%s%s", matches > 1u ? " " : "", id);
            }
        }
    }
    int rc = -1;
    if (matches != 1u) {
        (void)snag_buf_terminate(&candidates);
        (void)snag_fail(error, size, matches ? EEXIST : ENOENT,
            matches ? "ambiguous workspace: %s" : "workspace was not found%s",
            matches && candidates.data ? (const char *)candidates.data : "");
        goto out;
    }
    memcpy(workspace->id, selected, sizeof(workspace->id));
    workspace->dir_fd = private_directory(workspace->root_fd, selected, false, error, size);
    if (workspace->dir_fd < 0) goto out;
    if (own_workspace(workspace) < 0) {
        (void)snag_errorf(error, size, "workspace is already open or unavailable: %s",
            strerror(errno));
        goto out;
    }
    if (workspace_identity(workspace, error, size) < 0) goto out;
    workspace->snapshot = read_snapshot(workspace->dir_fd, selected, error, size);
    if (!workspace->snapshot) goto out;
    rc = 0;
out: {
        int saved = errno;
        json_decref(rows);
        snag_buf_free(&candidates);
        (void)snag_directory_lock_release(&names);
        if (rc < 0) snag_vm_workspace_close(workspace);
        errno = saved;
        return rc;
    }
}
