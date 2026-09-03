/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_internal.h"
#include "instructions.h"
#include "snajpagent.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define SNJ_LOG_HARD_LIMIT ((off_t)2 * 1024 * 1024 * 1024)
#define SNJ_LOG_RESERVE ((off_t)32 * 1024 * 1024)
#define SNJ_EVENT_LIMIT UINT64_C(1000000)
#define SNJ_EVENT_RESERVE UINT64_C(256)
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
static int
set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        return -1;
    return 0;
}
static int
open_dir_path(const char *path)
{
    int flags = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    return open(path, flags);
}
static bool
private_dir_stat(const struct stat *st)
{
    return S_ISDIR(st->st_mode) && st->st_uid == getuid() &&
           (st->st_mode & 077u) == 0;
}
static bool
private_file_stat(const struct stat *st)
{
    return S_ISREG(st->st_mode) && st->st_uid == getuid() &&
           (st->st_mode & 077u) == 0;
}
int
snj_store_verify_private_fd(int fd, bool directory, const char *name,
                  char *error, size_t error_size)
{
    struct stat st;
    bool valid;
    if (fstat(fd, &st) < 0) {
        set_error(error, error_size, "cannot inspect %s: %s", name,
                  strerror(errno));
        return -1;
    }
    valid = directory ? private_dir_stat(&st) : private_file_stat(&st);
    if (!valid) {
        set_error(error, error_size, "%s must be private and user-owned", name);
        errno = EACCES;
        return -1;
    }
    return 0;
}
static int
ensure_directory(const char *path, mode_t mode, bool require_private,
                 char *error, size_t error_size)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno != ENOENT) {
            set_error(error, error_size, "cannot inspect %s: %s", path,
                      strerror(errno));
            return -1;
        }
        if (mkdir(path, mode) < 0) {
            set_error(error, error_size, "cannot create %s: %s", path,
                      strerror(errno));
            return -1;
        }
        if (lstat(path, &st) < 0) {
            set_error(error, error_size, "cannot verify %s: %s", path,
                      strerror(errno));
            return -1;
        }
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        set_error(error, error_size, "%s is not a real directory", path);
        errno = EINVAL;
        return -1;
    }
    if (require_private && !private_dir_stat(&st)) {
        set_error(error, error_size, "%s must be private (mode 0700)", path);
        errno = EACCES;
        return -1;
    }
    return 0;
}
char *
snj_store_path_join(const char *left, const char *right)
{
    size_t a = strlen(left);
    size_t b = strlen(right);
    size_t need;
    char *path;
    if (!snj_size_add(a, b, &need) || !snj_size_add(need, 2u, &need) ||
        need > SNJ_PATH_MAX_BYTES + 1u) {
        errno = EOVERFLOW;
        return NULL;
    }
    path = malloc(need);
    if (!path)
        return NULL;
    (void)snprintf(path, need, "%s/%s", left, right);
    return path;
}
static int
mkdir_parents(const char *path, char *error, size_t error_size)
{
    char *copy = snj_strdup_checked(path, SNJ_PATH_MAX_BYTES);
    char *p;
    if (!copy)
        return -1;
    for (p = copy + 1; *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (ensure_directory(copy, 0700, false, error, error_size) < 0) {
            free(copy);
            return -1;
        }
        *p = '/';
    }
    free(copy);
    return 0;
}
void
snj_store_init(struct snj_store *store)
{
    memset(store, 0, sizeof(*store));
    store->root_fd = -1;
    store->sessions_fd = -1;
    store->trash_fd = -1;
}
void
snj_store_close(struct snj_store *store)
{
    if (store->trash_fd >= 0)
        (void)close(store->trash_fd);
    if (store->sessions_fd >= 0)
        (void)close(store->sessions_fd);
    if (store->root_fd >= 0)
        (void)close(store->root_fd);
    free(store->root_path);
    snj_store_init(store);
}
int
snj_store_open(struct snj_store *store, char *error, size_t error_size)
{
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    char *base = NULL;
    char *sessions = NULL;
    char *trash = NULL;
    int rc = -1;
    if (xdg && *xdg) {
        if (xdg[0] != '/' || strlen(xdg) > SNJ_PATH_MAX_BYTES) {
            set_error(error, error_size, "XDG_STATE_HOME must be an absolute UTF-8 path");
            errno = EINVAL;
            return -1;
        }
        base = snj_strdup_checked(xdg, SNJ_PATH_MAX_BYTES);
    } else {
        if (!home || home[0] != '/') {
            set_error(error, error_size, "HOME is unavailable for the state path");
            errno = EINVAL;
            return -1;
        }
        base = snj_store_path_join(home, ".local/state");
    }
    if (!base || !snj_utf8_valid((const unsigned char *)base, strlen(base), true))
        goto out;
    if (mkdir_parents(base, error, error_size) < 0 ||
        ensure_directory(base, 0700, false, error, error_size) < 0)
        goto out;
    store->root_path = snj_store_path_join(base, "snajpagent");
    if (!store->root_path || mkdir_parents(store->root_path, error, error_size) < 0 ||
        ensure_directory(store->root_path, 0700, true, error, error_size) < 0)
        goto out;
    store->root_fd = open_dir_path(store->root_path);
    if (store->root_fd < 0)
        goto io_error;
    if (snj_store_verify_private_fd(store->root_fd, true, "state root",
                          error, error_size) < 0)
        goto out;
    sessions = snj_store_path_join(store->root_path, "sessions");
    trash = snj_store_path_join(store->root_path, "trash");
    if (!sessions || !trash ||
        ensure_directory(sessions, 0700, true, error, error_size) < 0 ||
        ensure_directory(trash, 0700, true, error, error_size) < 0)
        goto out;
    store->sessions_fd = open_dir_path(sessions);
    store->trash_fd = open_dir_path(trash);
    if (store->sessions_fd < 0 || store->trash_fd < 0)
        goto io_error;
    if (snj_store_verify_private_fd(store->sessions_fd, true, "sessions directory",
                          error, error_size) < 0 ||
        snj_store_verify_private_fd(store->trash_fd, true, "trash directory",
                          error, error_size) < 0)
        goto out;
    rc = 0;
    goto out;
io_error:
    set_error(error, error_size, "cannot open state directory: %s", strerror(errno));
out:
    free(base);
    free(sessions);
    free(trash);
    if (rc < 0)
        snj_store_close(store);
    return rc;
}
static void
free_pending_user_state(struct snj_session *session)
{
    for (size_t i = 0; i < session->pending_steering_count; ++i) {
        free(session->pending_steering[i].text);
        session->pending_steering[i].text = NULL;
    }
    for (size_t i = 0; i < session->pending_queue_count; ++i) {
        free(session->pending_queue[i].text);
        session->pending_queue[i].text = NULL;
    }
    session->pending_steering_count = 0;
    session->pending_steering_bytes = 0;
    session->pending_queue_count = 0;
    session->pending_queue_bytes = 0;
}
void
snj_session_init(struct snj_session *session)
{
    memset(session, 0, sizeof(*session));
    session->dir_fd = -1;
    session->log_fd = -1;
    session->lock_fd = -1;
    memset(session->prev_sha256, '0', SNJ_SHA256_HEX_LEN);
    session->prev_sha256[SNJ_SHA256_HEX_LEN] = '\0';
    session->next_seq = 1;
}
void
snj_session_close(struct snj_session *session)
{
    if (session->log_fd >= 0)
        (void)close(session->log_fd);
    if (session->lock_fd >= 0)
        (void)close(session->lock_fd);
    if (session->dir_fd >= 0)
        (void)close(session->dir_fd);
    free_pending_user_state(session);
    free(session->workspace);
    free(session->dir_path);
    free(session->first_user);
    free(session->last_user);
    free(session->last_assistant);
    if (session->compact_output)
        json_decref(session->compact_output);
    snj_session_init(session);
}
static int
lock_session(int dir_fd, int *fd_out, char *error, size_t error_size)
{
    struct flock lock;
    int flags = O_RDWR | O_CREAT;
    int fd;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = openat(dir_fd, "lock", flags, 0600);
    if (fd < 0) {
        set_error(error, error_size, "cannot open session lock: %s", strerror(errno));
        return -1;
    }
    if (set_cloexec(fd) < 0 ||
        snj_store_verify_private_fd(fd, false, "session lock", error, error_size) < 0) {
        (void)close(fd);
        return -1;
    }
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) < 0) {
        set_error(error, error_size, errno == EACCES || errno == EAGAIN ?
                  "session is already open" : "cannot lock session: %s",
                  strerror(errno));
        (void)close(fd);
        return -1;
    }
    *fd_out = fd;
    return 0;
}
int
snj_store_open_session_files(struct snj_session *session, bool create,
                   char *error, size_t error_size)
{
    int flags = O_RDWR | O_APPEND;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    if (create)
        flags |= O_CREAT | O_EXCL;
    if (lock_session(session->dir_fd, &session->lock_fd, error, error_size) < 0)
        return -1;
    session->log_fd = openat(session->dir_fd, "events.jsonl", flags, 0600);
    if (session->log_fd < 0) {
        set_error(error, error_size, "cannot open event log: %s", strerror(errno));
        return -1;
    }
    if (set_cloexec(session->log_fd) < 0 ||
        snj_store_verify_private_fd(session->log_fd, false, "event log",
                          error, error_size) < 0)
        return -1;
    session->log_end = lseek(session->log_fd, 0, SEEK_END);
    if (session->log_end < 0) {
        set_error(error, error_size, "cannot seek event log: %s", strerror(errno));
        return -1;
    }
    return 0;
}
static int
json_set_new(json_t *object, const char *key, json_t *value)
{
    if (!value) {
        errno = ENOMEM;
        return -1;
    }
    if (json_object_set_new(object, key, value) < 0) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}
int
snj_session_append(struct snj_session *session, const char *type, json_t *data,
                   uint64_t *written_seq, char *error, size_t error_size)
{
    json_t *event = NULL;
    json_t *digest_event = NULL;
    struct snj_buf line;
    char digest[SNJ_SHA256_HEX_LEN + 1u];
    off_t actual_end;
    uint64_t seq = session->next_seq;
    int rc = -1;
    snj_buf_init(&line, SNJ_MAX_EVENT_LINE);
    if (!data || seq > SNJ_EVENT_LIMIT - SNJ_EVENT_RESERVE ||
        session->log_end > SNJ_LOG_HARD_LIMIT - SNJ_LOG_RESERVE) {
        set_error(error, error_size, "session log has no admission reserve");
        errno = ENOSPC;
        goto out;
    }
    event = json_object();
    if (!event)
        goto memory_error;
    if (json_set_new(event, "data", data) < 0) {
        data = NULL;
        goto memory_error;
    }
    data = NULL;
    if (json_set_new(event, "prev_sha256", json_string(session->prev_sha256)) < 0 ||
        json_set_new(event, "seq", json_integer((json_int_t)seq)) < 0 ||
        json_set_new(event, "session_id", json_string(session->id)) < 0 ||
        json_set_new(event, "time_ms", json_integer((json_int_t)snj_time_ms())) < 0 ||
        json_set_new(event, "type", json_string(type)) < 0 ||
        json_set_new(event, "v", json_integer(1)) < 0)
        goto memory_error;
    digest_event = json_deep_copy(event);
    if (!digest_event || snj_json_digest(digest_event, digest) < 0)
        goto memory_error;
    json_decref(digest_event);
    digest_event = NULL;
    if (json_set_new(event, "event_sha256", json_string(digest)) < 0 ||
        snj_json_canonical(event, &line) < 0 || snj_buf_putc(&line, '\n') < 0)
        goto memory_error;
    if ((off_t)line.len > SNJ_LOG_HARD_LIMIT - SNJ_LOG_RESERVE - session->log_end) {
        set_error(error, error_size, "event would consume session closure reserve");
        errno = ENOSPC;
        goto out;
    }
    actual_end = lseek(session->log_fd, 0, SEEK_END);
    if (actual_end < 0 || actual_end != session->log_end) {
        set_error(error, error_size, "event log end changed unexpectedly");
        errno = EIO;
        goto out;
    }
    if (snj_write_full(session->log_fd, line.data, line.len) < 0 ||
        snj_sync_file(session->log_fd) < 0) {
        set_error(error, error_size, "cannot durably append %s: %s", type,
                  strerror(errno));
        goto out;
    }
    session->log_end += (off_t)line.len;
    session->next_seq++;
    memcpy(session->prev_sha256, digest, sizeof(digest));
    session->last_time_ms = snj_time_ms();
    if (written_seq)
        *written_seq = seq;
    rc = 0;
    goto out;
memory_error:
    set_error(error, error_size, "cannot encode %s event", type);
out:
    if (data)
        json_decref(data);
    if (digest_event)
        json_decref(digest_event);
    if (event)
        json_decref(event);
    snj_buf_free(&line);
    return rc;
}
static bool
copy_small(char *dst, size_t size, const char *src)
{
    size_t len;
    if (!src)
        return false;
    len = strlen(src);
    if (len >= size)
        return false;
    memcpy(dst, src, len + 1u);
    return true;
}
static int
replace_text(char **slot, const char *text, size_t max)
{
    char *copy = snj_strdup_checked(text, max);
    if (!copy)
        return -1;
    free(*slot);
    *slot = copy;
    return 0;
}
static bool
common_event_valid(json_t *event, struct snj_session *session, uint64_t seq,
                   const char *prev, const char **type_out, json_t **data_out,
                   char *error, size_t error_size)
{
    static const char *const keys[] = {
        "data", "event_sha256", "prev_sha256", "seq", "session_id",
        "time_ms", "type", "v"
    };
    const char *event_hash;
    const char *prev_hash;
    const char *session_id;
    const char *type;
    uint64_t n;
    json_t *copy;
    char computed[SNJ_SHA256_HEX_LEN + 1u];
    if (!snj_json_exact_keys(event, keys, sizeof(keys) / sizeof(keys[0])) ||
        snj_json_integer_u64(event, "v", &n) < 0 || n != 1 ||
        snj_json_integer_u64(event, "seq", &n) < 0 || n != seq ||
        snj_json_integer_u64(event, "time_ms", &n) < 0 ||
        !(event_hash = snj_json_string(event, "event_sha256")) ||
        !(prev_hash = snj_json_string(event, "prev_sha256")) ||
        !(session_id = snj_json_string(event, "session_id")) ||
        !(type = snj_json_string(event, "type")) ||
        !snj_hex_is_lower(event_hash, SNJ_SHA256_HEX_LEN) ||
        !snj_hex_is_lower(prev_hash, SNJ_SHA256_HEX_LEN) ||
        strcmp(prev_hash, prev) != 0 || strcmp(session_id, session->id) != 0 ||
        !json_is_object(json_object_get(event, "data"))) {
        set_error(error, error_size, "invalid event envelope at sequence %llu",
                  (unsigned long long)seq);
        return false;
    }
    copy = json_deep_copy(event);
    if (!copy || json_object_del(copy, "event_sha256") < 0 ||
        snj_json_digest(copy, computed) < 0) {
        if (copy)
            json_decref(copy);
        set_error(error, error_size, "cannot verify event digest");
        return false;
    }
    json_decref(copy);
    if (strcmp(event_hash, computed) != 0) {
        set_error(error, error_size, "event digest mismatch at sequence %llu",
                  (unsigned long long)seq);
        return false;
    }
    memcpy(session->prev_sha256, event_hash, SNJ_SHA256_HEX_LEN + 1u);
    session->last_time_ms = n;
    *type_out = type;
    *data_out = json_object_get(event, "data");
    return true;
}
static void
clear_response_state(struct snj_session *session)
{
    session->response_open = false;
    session->response_complete = false;
    session->response_terminal = SNJ_RESPONSE_TERMINAL_NONE;
    session->active_response_id[0] = '\0';
    session->final_item_id[0] = '\0';
    session->final_response_id[0] = '\0';
    session->pending_call_count = 0;
    memset(session->pending_calls, 0, sizeof(session->pending_calls));
}
static bool
all_pending_finished(const struct snj_session *session)
{
    for (size_t i = 0; i < session->pending_call_count; ++i)
        if (!session->pending_calls[i].finished)
            return false;
    return true;
}
static bool
process_close_status(const char *status)
{
    return status &&
           (strcmp(status, "succeeded") == 0 ||
            strcmp(status, "failed") == 0 ||
            strcmp(status, "signaled") == 0 ||
            strcmp(status, "timed_out") == 0 ||
            strcmp(status, "cancelled") == 0 ||
            strcmp(status, "outcome_unknown") == 0 ||
            strcmp(status, "io_failed") == 0);
}

static int
compact_output_digest(const json_t *output,
                      char out[SNJ_SHA256_HEX_LEN + 1u], size_t *bytes)
{
    struct snj_buf encoded;
    int rc = -1;

    if (!json_is_array(output) || json_array_size(output) == 0u ||
        json_array_size(output) > 128u) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < json_array_size(output); ++i) {
        json_t *item = json_array_get(output, i);
        if (!json_is_object(item) || !snj_json_string(item, "type")) {
            errno = EINVAL;
            return -1;
        }
    }
    snj_buf_init(&encoded, 12u * 1024u * 1024u);
    if (snj_json_canonical(output, &encoded) == 0) {
        snj_sha256_hex(encoded.data, encoded.len, out);
        if (bytes)
            *bytes = encoded.len;
        rc = 0;
    }
    snj_buf_free(&encoded);
    return rc;
}
static bool
valid_trash_name(const struct snj_session *session, const char *name)
{
    if (!name || strlen(name) != SNJ_TRASH_NAME_LEN)
        return false;
    if (memcmp(name, session->id, SNJ_ID_HEX_LEN) != 0 ||
        name[SNJ_ID_HEX_LEN] != '.')
        return false;
    return snj_hex_is_lower(name + SNJ_ID_HEX_LEN + 1u,
                            SNJ_TRASH_SUFFIX_HEX_LEN);
}
static struct snj_pending_call *
find_pending_call(struct snj_session *session, const char *call_id,
                  size_t *index_out)
{
    for (size_t i = 0; i < session->pending_call_count; ++i) {
        if (strcmp(session->pending_calls[i].call_id, call_id) == 0) {
            if (index_out)
                *index_out = i;
            return &session->pending_calls[i];
        }
    }
    return NULL;
}
static void
clear_pending_steering(struct snj_session *session)
{
    for (size_t i = 0; i < session->pending_steering_count; ++i) {
        free(session->pending_steering[i].text);
        session->pending_steering[i].text = NULL;
    }
    session->pending_steering_count = 0;
    session->pending_steering_bytes = 0;
}
static bool
pending_user_id_exists(const struct snj_session *session, const char *id)
{
    for (size_t i = 0; i < session->pending_steering_count; ++i)
        if (strcmp(session->pending_steering[i].steering_id, id) == 0)
            return true;
    for (size_t i = 0; i < session->pending_queue_count; ++i)
        if (strcmp(session->pending_queue[i].queue_id, id) == 0)
            return true;
    return false;
}
static int
consume_oldest_queue(struct snj_session *session)
{
    size_t len;
    if (session->pending_queue_count == 0u ||
        !session->pending_queue[0].text) {
        errno = EINVAL;
        return -1;
    }
    len = strlen(session->pending_queue[0].text);
    free(session->pending_queue[0].text);
    if (session->pending_queue_count > 1u)
        memmove(&session->pending_queue[0], &session->pending_queue[1],
                (session->pending_queue_count - 1u) *
                sizeof(session->pending_queue[0]));
    --session->pending_queue_count;
    memset(&session->pending_queue[session->pending_queue_count], 0,
           sizeof(session->pending_queue[0]));
    session->pending_queue_bytes -= len;
    return 0;
}

static bool
string_in(const char *value, const char *const *choices, size_t count)
{
    if (!value)
        return false;
    for (size_t i = 0; i < count; ++i)
        if (strcmp(value, choices[i]) == 0)
            return true;
    return false;
}

static int
apply_event(struct snj_session *session, const char *type, json_t *data,
            uint64_t seq, char *error, size_t error_size)
{
    uint64_t n;

    if (strcmp(type, "session_created") == 0) {
        static const char *const keys[] = {
            "default_effort", "default_model", "format", "protocol", "workspace"
        };
        static const char *const efforts[] = {
            "default", "none", "minimal", "low", "medium", "high", "xhigh"
        };
        const char *effort = snj_json_string(data, "default_effort");
        const char *model = snj_json_string(data, "default_model");
        const char *protocol = snj_json_string(data, "protocol");
        const char *workspace = snj_json_string(data, "workspace");

        if (seq != 1 || !snj_json_exact_keys(data, keys, 5u) ||
            snj_json_integer_u64(data, "format", &n) < 0 || n != 1 ||
            !protocol || strcmp(protocol, "responses") != 0 ||
            !string_in(effort, efforts, sizeof(efforts) / sizeof(efforts[0])) ||
            !model || !*model || strlen(model) >= sizeof(session->default_model) ||
            !snj_utf8_valid((const unsigned char *)model, strlen(model), true) ||
            !workspace || workspace[0] != '/' ||
            strlen(workspace) > SNJ_PATH_MAX_BYTES ||
            !snj_utf8_valid((const unsigned char *)workspace,
                            strlen(workspace), true))
            goto invalid;
        if (replace_text(&session->workspace, workspace,
                         SNJ_PATH_MAX_BYTES) < 0)
            return -1;
        if (!copy_small(session->default_effort,
                        sizeof(session->default_effort), effort) ||
            !copy_small(session->default_model,
                        sizeof(session->default_model), model))
            goto invalid;
    } else if (session->delete_requested) {
        goto invalid;
    } else if (strcmp(type, "workspace_changed") == 0) {
        static const char *const keys[] = {"new_workspace", "old_workspace"};
        const char *old_workspace = snj_json_string(data, "old_workspace");
        const char *new_workspace = snj_json_string(data, "new_workspace");
        if (session->active_turn || session->active_process_handle[0] != '\0' ||
            !snj_json_exact_keys(data, keys, 2u) ||
            !old_workspace || !new_workspace ||
            strcmp(old_workspace, session->workspace) != 0 ||
            strcmp(old_workspace, new_workspace) == 0 ||
            new_workspace[0] != '/' || strlen(new_workspace) > SNJ_PATH_MAX_BYTES ||
            !snj_utf8_valid((const unsigned char *)new_workspace,
                            strlen(new_workspace), true) ||
            replace_text(&session->workspace, new_workspace,
                         SNJ_PATH_MAX_BYTES) < 0)
            goto invalid;
    } else if (strcmp(type, "session_archived") == 0) {
        static const char *const keys[] = {"origin"};
        const char *origin = snj_json_string(data, "origin");
        if (!snj_json_exact_keys(data, keys, 1u) || session->active_turn ||
            session->active_process_handle[0] != '\0' || session->archived ||
            !origin || strcmp(origin, "user") != 0)
            goto invalid;
        session->archived = true;
    } else if (strcmp(type, "session_unarchived") == 0) {
        static const char *const keys[] = {"origin"};
        const char *origin = snj_json_string(data, "origin");
        if (!snj_json_exact_keys(data, keys, 1u) || session->active_turn ||
            session->active_process_handle[0] != '\0' || !session->archived ||
            !origin || strcmp(origin, "user") != 0)
            goto invalid;
        session->archived = false;
    } else if (strcmp(type, "session_delete_requested") == 0) {
        static const char *const keys[] = {"confirmed_id_prefix", "trash_name"};
        const char *prefix = snj_json_string(data, "confirmed_id_prefix");
        const char *trash = snj_json_string(data, "trash_name");
        if (!snj_json_exact_keys(data, keys, 2u) || session->active_turn ||
            session->active_process_handle[0] != '\0' ||
            !prefix || strlen(prefix) != 8u ||
            !snj_hex_is_lower(prefix, 8u) ||
            memcmp(prefix, session->id, 8u) != 0 ||
            !valid_trash_name(session, trash))
            goto invalid;
        memcpy(session->trash_name, trash, SNJ_TRASH_NAME_LEN + 1u);
        session->delete_requested = true;
    } else if (strcmp(type, "compaction_started") == 0) {
        static const char *const keys[] = {
            "capability_version", "compact_id", "count_method",
            "count_request_sha256", "input_tokens_bound", "model",
            "predecessor_compact_id", "profile_id", "reason",
            "request_sha256", "source_seq", "source_sha256"
        };
        static const char *const methods[] = {"exact", "qualified_upper_bound"};
        static const char *const reasons[] = {"manual", "automatic"};
        const char *compact_id = snj_json_string(data, "compact_id");
        const char *predecessor = snj_json_string(data, "predecessor_compact_id");
        const char *reason = snj_json_string(data, "reason");
        const char *method = snj_json_string(data, "count_method");
        const char *source_hash = snj_json_string(data, "source_sha256");
        const char *request_hash = snj_json_string(data, "request_sha256");
        const char *count_hash = snj_json_string(data, "count_request_sha256");
        const char *model = snj_json_string(data, "model");
        const char *profile = snj_json_string(data, "profile_id");
        const char *capability = snj_json_string(data, "capability_version");
        uint64_t source_seq;
        uint64_t tokens;
        bool active_prefix;

        active_prefix = reason && strcmp(reason, "automatic") == 0 &&
            session->active_turn && !session->response_open;
        if (!snj_json_exact_keys(data, keys, sizeof(keys) / sizeof(keys[0])) ||
            (session->active_turn && !active_prefix) || session->response_open ||
            session->active_process_handle[0] != '\0' ||
            session->active_compact_id[0] != '\0' ||
            !compact_id || !snj_hex_is_lower(compact_id, SNJ_ID_HEX_LEN) ||
            strcmp(compact_id, session->id) == 0 ||
            !string_in(method, methods, sizeof(methods) / sizeof(methods[0])) ||
            !string_in(reason, reasons, sizeof(reasons) / sizeof(reasons[0])) ||
            !source_hash || !snj_hex_is_lower(source_hash, SNJ_SHA256_HEX_LEN) ||
            !request_hash || !snj_hex_is_lower(request_hash, SNJ_SHA256_HEX_LEN) ||
            !count_hash || !snj_hex_is_lower(count_hash, SNJ_SHA256_HEX_LEN) ||
            !model || strcmp(model, session->default_model) != 0 ||
            !profile || strcmp(profile, SNAJPAGENT_PROFILE_ID) != 0 ||
            !capability ||
            strcmp(capability, SNAJPAGENT_CAPABILITY_VERSION) != 0 ||
            snj_json_integer_u64(data, "source_seq", &source_seq) < 0 ||
            source_seq == 0u || source_seq >= seq || source_seq <= session->compact_seq ||
            snj_json_integer_u64(data, "input_tokens_bound", &tokens) < 0 ||
            tokens == 0u)
            goto invalid;
        if (session->compact_id[0] == '\0') {
            if (!json_is_null(json_object_get(data, "predecessor_compact_id")))
                goto invalid;
        } else if (!predecessor || strcmp(predecessor, session->compact_id) != 0) {
            goto invalid;
        }
        memcpy(session->active_compact_id, compact_id,
               sizeof(session->active_compact_id));
        memcpy(session->active_compact_source_sha256, source_hash,
               sizeof(session->active_compact_source_sha256));
        session->active_compact_source_seq = source_seq;
    } else if (strcmp(type, "compaction_completed") == 0) {
        static const char *const keys[] = {
            "compact_id", "count_method", "input_tokens_bound", "output",
            "output_count_method", "output_count_request_sha256",
            "output_sha256", "output_tokens_bound", "source_sha256"
        };
        static const char *const methods[] = {"exact", "qualified_upper_bound"};
        const char *compact_id = snj_json_string(data, "compact_id");
        const char *method = snj_json_string(data, "count_method");
        const char *output_method = snj_json_string(data, "output_count_method");
        const char *output_count_hash =
            snj_json_string(data, "output_count_request_sha256");
        const char *source_hash = snj_json_string(data, "source_sha256");
        const char *output_hash = snj_json_string(data, "output_sha256");
        json_t *output = json_object_get(data, "output");
        char computed[SNJ_SHA256_HEX_LEN + 1u];
        size_t bytes = 0u;
        uint64_t in_tokens;
        uint64_t out_tokens;

        if (!snj_json_exact_keys(data, keys, sizeof(keys) / sizeof(keys[0])) ||
            session->active_compact_id[0] == '\0' ||
            !compact_id || strcmp(compact_id, session->active_compact_id) != 0 ||
            !string_in(method, methods, sizeof(methods) / sizeof(methods[0])) ||
            !string_in(output_method, methods, sizeof(methods) / sizeof(methods[0])) ||
            !output_count_hash ||
            !snj_hex_is_lower(output_count_hash, SNJ_SHA256_HEX_LEN) ||
            !source_hash || strcmp(source_hash, session->active_compact_source_sha256) != 0 ||
            !output_hash || !snj_hex_is_lower(output_hash, SNJ_SHA256_HEX_LEN) ||
            snj_json_integer_u64(data, "input_tokens_bound", &in_tokens) < 0 ||
            snj_json_integer_u64(data, "output_tokens_bound", &out_tokens) < 0 ||
            in_tokens == 0u || out_tokens == 0u ||
            compact_output_digest(output, computed, &bytes) < 0 || bytes == 0u ||
            strcmp(output_hash, computed) != 0)
            goto invalid;
        if (session->compact_output)
            json_decref(session->compact_output);
        session->compact_output = json_deep_copy(output);
        if (!session->compact_output)
            return -1;
        memcpy(session->compact_id, compact_id, sizeof(session->compact_id));
        session->compact_seq = session->active_compact_source_seq;
        session->active_compact_id[0] = '\0';
        session->active_compact_source_sha256[0] = '\0';
        session->active_compact_source_seq = 0u;
    } else if (strcmp(type, "model_changed") == 0) {
        static const char *const keys[] = {"new_model", "old_model"};
        const char *old_model = snj_json_string(data, "old_model");
        const char *new_model = snj_json_string(data, "new_model");
        if (session->active_turn || !snj_json_exact_keys(data, keys, 2u) ||
            !old_model || !new_model || !*new_model ||
            strcmp(old_model, session->default_model) != 0 ||
            strcmp(old_model, new_model) == 0 ||
            strlen(new_model) >= sizeof(session->default_model) ||
            !snj_utf8_valid((const unsigned char *)new_model,
                            strlen(new_model), true) ||
            !copy_small(session->default_model,
                        sizeof(session->default_model), new_model))
            goto invalid;
    } else if (strcmp(type, "effort_changed") == 0) {
        static const char *const keys[] = {"new_effort", "old_effort"};
        static const char *const efforts[] = {
            "default", "none", "minimal", "low", "medium", "high", "xhigh"
        };
        const char *old_effort = snj_json_string(data, "old_effort");
        const char *new_effort = snj_json_string(data, "new_effort");
        if (session->active_turn || !snj_json_exact_keys(data, keys, 2u) ||
            !string_in(old_effort, efforts, sizeof(efforts) / sizeof(efforts[0])) ||
            !string_in(new_effort, efforts, sizeof(efforts) / sizeof(efforts[0])) ||
            strcmp(old_effort, session->default_effort) != 0 ||
            strcmp(old_effort, new_effort) == 0 ||
            !copy_small(session->default_effort,
                        sizeof(session->default_effort), new_effort))
            goto invalid;
    } else if (strcmp(type, "steering_added") == 0) {
        static const char *const keys[] = {"steering_id", "text", "turn_id"};
        const char *steering_id = snj_json_string(data, "steering_id");
        const char *text = snj_json_string(data, "text");
        const char *turn_id = snj_json_string(data, "turn_id");
        size_t len;
        char *copy;
        struct snj_pending_steering *pending;

        if (!snj_json_exact_keys(data, keys, 3u) || !session->active_turn ||
            session->response_terminal == SNJ_RESPONSE_TERMINAL_FAILED ||
            session->response_terminal == SNJ_RESPONSE_TERMINAL_INTERRUPTED ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !steering_id || !snj_hex_is_lower(steering_id, SNJ_ID_HEX_LEN) ||
            pending_user_id_exists(session, steering_id) || !text || !*text ||
            (len = strlen(text)) > SNJ_MAX_STEERING_TEXT ||
            session->pending_steering_count >= SNJ_MAX_STEERING_PER_TURN ||
            session->pending_steering_bytes >
                SNJ_MAX_STEERING_PER_TURN * SNJ_MAX_STEERING_TEXT - len)
            goto invalid;
        copy = snj_strdup_checked(text, SNJ_MAX_STEERING_TEXT);
        if (!copy)
            return -1;
        pending = &session->pending_steering[session->pending_steering_count++];
        memset(pending, 0, sizeof(*pending));
        memcpy(pending->steering_id, steering_id, sizeof(pending->steering_id));
        pending->seq = seq;
        pending->text = copy;
        session->pending_steering_bytes += len;
    } else if (strcmp(type, "future_turn_queued") == 0) {
        static const char *const keys[] = {"queue_id", "text", "while_turn_id"};
        const char *queue_id = snj_json_string(data, "queue_id");
        const char *text = snj_json_string(data, "text");
        const char *turn_id = snj_json_string(data, "while_turn_id");
        size_t len;
        char *copy;
        struct snj_queued_turn *queued;

        if (!snj_json_exact_keys(data, keys, 3u) || !session->active_turn ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !queue_id || !snj_hex_is_lower(queue_id, SNJ_ID_HEX_LEN) ||
            pending_user_id_exists(session, queue_id) || !text || !*text ||
            (len = strlen(text)) > SNJ_MAX_QUEUED_TEXT ||
            session->pending_queue_count >= SNJ_MAX_PENDING_TURNS ||
            session->pending_queue_bytes > SNJ_MAX_PENDING_QUEUE_TEXT - len)
            goto invalid;
        copy = snj_strdup_checked(text, SNJ_MAX_QUEUED_TEXT);
        if (!copy)
            return -1;
        queued = &session->pending_queue[session->pending_queue_count++];
        memset(queued, 0, sizeof(*queued));
        memcpy(queued->queue_id, queue_id, sizeof(queued->queue_id));
        queued->seq = seq;
        queued->text = copy;
        session->pending_queue_bytes += len;
    } else if (strcmp(type, "future_turn_cancelled") == 0) {
        static const char *const keys[] = {"queue_ids", "reason"};
        const char *reason = snj_json_string(data, "reason");
        json_t *ids = json_object_get(data, "queue_ids");
        bool remove[SNJ_MAX_PENDING_TURNS] = {false};
        size_t previous = 0;
        bool have_previous = false;
        size_t count;

        if (!snj_json_exact_keys(data, keys, 2u) || !reason ||
            strcmp(reason, "user") != 0 || !json_is_array(ids) ||
            !(count = json_array_size(ids)) || count > SNJ_MAX_PENDING_TURNS)
            goto invalid;
        for (size_t i = 0; i < count; ++i) {
            json_t *value = json_array_get(ids, i);
            const char *id = json_is_string(value) ? json_string_value(value) : NULL;
            size_t index;

            if (!id || !snj_hex_is_lower(id, SNJ_ID_HEX_LEN))
                goto invalid;
            for (index = 0; index < session->pending_queue_count; ++index)
                if (strcmp(session->pending_queue[index].queue_id, id) == 0)
                    break;
            if (index == session->pending_queue_count || remove[index] ||
                (have_previous && index <= previous))
                goto invalid;
            remove[index] = true;
            previous = index;
            have_previous = true;
        }
        {
            size_t out = 0;
            for (size_t i = 0; i < session->pending_queue_count; ++i) {
                if (remove[i]) {
                    session->pending_queue_bytes -=
                        strlen(session->pending_queue[i].text);
                    free(session->pending_queue[i].text);
                    session->pending_queue[i].text = NULL;
                    continue;
                }
                if (out != i)
                    session->pending_queue[out] = session->pending_queue[i];
                ++out;
            }
            memset(&session->pending_queue[out], 0,
                   (session->pending_queue_count - out) *
                   sizeof(session->pending_queue[0]));
            session->pending_queue_count = out;
        }
    } else if (strcmp(type, "turn_started") == 0) {
        static const char *const keys[] = {
            "config", "input_kind", "instructions", "queue_id", "queue_seq",
            "text", "turn_id", "turn_number", "workspace"
        };
        const char *turn_id;
        const char *text;
        const char *workspace;
        const char *kind;
        const char *model;
        json_t *config = json_object_get(data, "config");
        bool queued;
        uint64_t queue_seq = 0;

        if (session->active_turn || session->active_process_handle[0] != '\0' ||
            session->pending_steering_count != 0u ||
            !snj_json_exact_keys(data, keys, 9u) ||
            !(turn_id = snj_json_string(data, "turn_id")) ||
            !snj_hex_is_lower(turn_id, SNJ_ID_HEX_LEN) ||
            snj_json_integer_u64(data, "turn_number", &n) < 0 ||
            n != session->turn_count + 1u ||
            !(kind = snj_json_string(data, "input_kind")) ||
            !json_is_object(config) ||
            !(model = snj_json_string(config, "model")) || !*model ||
            strlen(model) >= sizeof(session->active_turn_model) ||
            !snj_utf8_valid((const unsigned char *)model, strlen(model), true) ||
            snj_instructions_metadata_valid(json_object_get(data, "instructions"),
                                            error, error_size) < 0 ||
            !(workspace = snj_json_string(data, "workspace")) ||
            strcmp(workspace, session->workspace) != 0 ||
            !(text = snj_json_string(data, "text")) || !*text)
            goto invalid;
        queued = strcmp(kind, "queued") == 0;
        if (!queued && strcmp(kind, "direct") != 0)
            goto invalid;
        if (!queued) {
            if (!json_is_null(json_object_get(data, "queue_id")) ||
                !json_is_null(json_object_get(data, "queue_seq")) ||
                strlen(text) > SNJ_MAX_DIRECT_PROMPT)
                goto invalid;
        } else {
            const char *queue_id = snj_json_string(data, "queue_id");
            if (session->pending_queue_count == 0u ||
                !session->pending_queue[0].text || !queue_id ||
                snj_json_integer_u64(data, "queue_seq", &queue_seq) < 0 ||
                strcmp(queue_id, session->pending_queue[0].queue_id) != 0 ||
                queue_seq != session->pending_queue[0].seq ||
                strcmp(text, session->pending_queue[0].text) != 0 ||
                strlen(text) > SNJ_MAX_QUEUED_TEXT)
                goto invalid;
        }
        memcpy(session->active_turn_id, turn_id, sizeof(session->active_turn_id));
        if (!copy_small(session->active_turn_model,
                        sizeof(session->active_turn_model), model))
            goto invalid;
        session->active_turn = true;
        session->turn_count = n;
        session->active_cycle = 0;
        session->tool_invocations = 0;
        clear_response_state(session);
        if ((!session->first_user &&
             replace_text(&session->first_user, text, SNJ_MAX_DIRECT_PROMPT) < 0) ||
            replace_text(&session->last_user, text, SNJ_MAX_DIRECT_PROMPT) < 0)
            return -1;
        if (queued && consume_oldest_queue(session) < 0)
            goto invalid;
    } else if (strcmp(type, "response_started") == 0) {
        static const char *const keys[] = {
            "baseline_sha256", "capability_version", "compact_id",
            "count_method", "count_request_sha256", "cycle",
            "input_tokens_bound", "model", "model_input_sha256",
            "profile_id", "request_sha256", "response_id",
            "steering_ids", "turn_id"
        };
        const char *response_id = snj_json_string(data, "response_id");
        const char *turn_id = snj_json_string(data, "turn_id");
        const char *method = snj_json_string(data, "count_method");
        const char *compact_id = snj_json_string(data, "compact_id");
        const char *capability = snj_json_string(data, "capability_version");
        const char *model = snj_json_string(data, "model");
        const char *profile = snj_json_string(data, "profile_id");
        const char *input_hash = snj_json_string(data, "model_input_sha256");
        const char *request_hash = snj_json_string(data, "request_sha256");
        const char *count_hash = snj_json_string(data, "count_request_sha256");
        json_t *steering_ids = json_object_get(data, "steering_ids");
        uint64_t cycle;
        uint64_t token_bound;
        bool state_allows_start;

        state_allows_start = !session->response_open &&
            session->response_terminal != SNJ_RESPONSE_TERMINAL_FAILED &&
            session->response_terminal != SNJ_RESPONSE_TERMINAL_INTERRUPTED &&
            (session->response_terminal != SNJ_RESPONSE_TERMINAL_STEERED ||
             session->pending_steering_count != 0u) &&
            (!session->response_complete ||
             (session->pending_steering_count != 0u &&
              all_pending_finished(session) &&
              session->response_outcome != SNJ_GRAPH_CONFLICT));
        if (!snj_json_exact_keys(data, keys, 14u) || !session->active_turn ||
            !state_allows_start || !response_id ||
            !snj_hex_is_lower(response_id, SNJ_ID_HEX_LEN) || !turn_id ||
            strcmp(turn_id, session->active_turn_id) != 0 || !method ||
            (strcmp(method, "exact") != 0 &&
            strcmp(method, "qualified_upper_bound") != 0) ||
            !capability || strcmp(capability, SNAJPAGENT_CAPABILITY_VERSION) != 0 ||
            !model || strcmp(model, session->active_turn_model) != 0 ||
            !profile || strcmp(profile, SNAJPAGENT_PROFILE_ID) != 0 ||
            !json_is_null(json_object_get(data, "baseline_sha256")) ||
            (session->compact_id[0] == '\0' ?
             !json_is_null(json_object_get(data, "compact_id")) :
             (!compact_id || strcmp(compact_id, session->compact_id) != 0)) ||
            !json_is_array(steering_ids) ||
            json_array_size(steering_ids) != session->pending_steering_count ||
            !input_hash || !snj_hex_is_lower(input_hash, SNJ_SHA256_HEX_LEN) ||
            !request_hash || !snj_hex_is_lower(request_hash, SNJ_SHA256_HEX_LEN) ||
            !count_hash || !snj_hex_is_lower(count_hash, SNJ_SHA256_HEX_LEN) ||
            snj_json_integer_u64(data, "input_tokens_bound", &token_bound) < 0 ||
            snj_json_integer_u64(data, "cycle", &cycle) < 0 ||
            cycle != (uint64_t)session->active_cycle + 1u ||
            cycle > SNJ_MAX_RESPONSE_CYCLES)
            goto invalid;
        for (size_t i = 0; i < session->pending_steering_count; ++i) {
            json_t *value = json_array_get(steering_ids, i);
            const char *id = json_is_string(value) ? json_string_value(value) : NULL;
            if (!id || strcmp(id, session->pending_steering[i].steering_id) != 0)
                goto invalid;
        }
        clear_response_state(session);
        clear_pending_steering(session);
        memcpy(session->active_response_id, response_id,
               sizeof(session->active_response_id));
        session->active_cycle = (unsigned int)cycle;
        session->response_open = true;
    } else if (strcmp(type, "response_interrupted") == 0) {
        static const char *const keys[] = {
            "cycle", "origin", "partial_public", "reason", "response_id",
            "turn_id"
        };
        static const char *const origins[] = {"user", "steering", "recovery", "output"};
        static const char *const reasons[] = {"cancelled", "steered", "process_lost", "output_lost"};
        const char *response_id = snj_json_string(data, "response_id");
        const char *turn_id = snj_json_string(data, "turn_id");
        const char *origin = snj_json_string(data, "origin");
        const char *reason = snj_json_string(data, "reason");
        json_t *partial = json_object_get(data, "partial_public");
        uint64_t cycle;
        if (!snj_json_exact_keys(data, keys, 6u) || !session->response_open ||
            !response_id || strcmp(response_id, session->active_response_id) != 0 ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !string_in(origin, origins, sizeof(origins) / sizeof(origins[0])) ||
            !string_in(reason, reasons, sizeof(reasons) / sizeof(reasons[0])) ||
            ((strcmp(origin, "steering") == 0) !=
             (strcmp(reason, "steered") == 0)) ||
            (strcmp(origin, "steering") == 0 &&
             session->pending_steering_count == 0u) ||
            snj_partial_public_validate(partial, error, error_size) < 0 ||
            snj_json_integer_u64(data, "cycle", &cycle) < 0 ||
            cycle != session->active_cycle)
            goto invalid;
        session->response_open = false;
        session->response_complete = false;
        session->response_terminal = strcmp(origin, "steering") == 0 ?
            SNJ_RESPONSE_TERMINAL_STEERED : SNJ_RESPONSE_TERMINAL_INTERRUPTED;
    } else if (strcmp(type, "response_failed") == 0) {
        static const char *const keys[] = {
            "class", "cycle", "message", "partial_public", "response_id",
            "retry_count", "turn_id"
        };
        static const char *const classes[] = {
            "context", "provider", "protocol", "resource", "output", "internal"
        };
        const char *response_id = snj_json_string(data, "response_id");
        const char *turn_id = snj_json_string(data, "turn_id");
        const char *class_name = snj_json_string(data, "class");
        const char *message = snj_json_string(data, "message");
        json_t *partial = json_object_get(data, "partial_public");
        uint64_t cycle;
        uint64_t retry_count;
        if (!snj_json_exact_keys(data, keys, 7u) || !session->response_open ||
            !response_id || strcmp(response_id, session->active_response_id) != 0 ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !string_in(class_name, classes, sizeof(classes) / sizeof(classes[0])) ||
            !message || strlen(message) > 8192u ||
            snj_partial_public_validate(partial, error, error_size) < 0 ||
            snj_json_integer_u64(data, "cycle", &cycle) < 0 ||
            cycle != session->active_cycle ||
            snj_json_integer_u64(data, "retry_count", &retry_count) < 0 ||
            retry_count > 2u)
            goto invalid;
        session->response_open = false;
        session->response_complete = false;
        session->response_terminal = SNJ_RESPONSE_TERMINAL_FAILED;
    } else if (strcmp(type, "response_completed") == 0) {
        static const char *const keys[] = {
            "cycle", "items", "provider_response_id", "response_id", "status",
            "turn_id", "usage"
        };
        json_t *items = json_object_get(data, "items");
        const char *provider_response_id = snj_json_string(data, "provider_response_id");
        const char *response_id = snj_json_string(data, "response_id");
        const char *turn_id = snj_json_string(data, "turn_id");
        const char *status = snj_json_string(data, "status");
        struct snj_response_graph graph;
        struct snj_graph_decision decision;
        uint64_t cycle;
        int graph_rc;

        snj_response_graph_init(&graph);
        if (!snj_json_exact_keys(data, keys, 7u) || !session->response_open ||
            !response_id || strcmp(response_id, session->active_response_id) != 0 ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !status || strcmp(status, "completed") != 0 ||
            !provider_response_id ||
            snj_response_graph_set_provider_id(&graph, provider_response_id) < 0 ||
            snj_response_usage_from_json(json_object_get(data, "usage"),
                                         &graph.usage) < 0 ||
            snj_json_integer_u64(data, "cycle", &cycle) < 0 ||
            cycle != session->active_cycle ||
            snj_response_graph_from_json(&graph, items, error, error_size) < 0) {
            snj_response_graph_free(&graph);
            goto invalid;
        }
        graph_rc = snj_response_graph_classify(&graph, &decision,
                                               error, error_size);
        if (graph_rc < 0) {
            snj_response_graph_free(&graph);
            goto invalid;
        }
        session->response_open = false;
        session->response_complete = true;
        session->response_terminal = SNJ_RESPONSE_TERMINAL_NONE;
        session->response_outcome = decision.outcome;
        session->pending_call_count = 0;
        session->final_item_id[0] = '\0';
        session->final_response_id[0] = '\0';
        for (size_t i = 0; i < graph.count; ++i) {
            const struct snj_response_item *item = &graph.items[i];
            if (item->kind == SNJ_ITEM_ASSISTANT ||
                item->kind == SNJ_ITEM_REFUSAL) {
                if (replace_text(&session->last_assistant, item->text,
                                 SNJ_MAX_PUBLIC_ITEM) < 0) {
                    snj_response_graph_free(&graph);
                    return -1;
                }
            }
            if (item->kind == SNJ_ITEM_TOOL_CALL) {
                struct snj_pending_call *pending;
                if (session->pending_call_count >= SNJ_MAX_CALLS_PER_RESPONSE) {
                    snj_response_graph_free(&graph);
                    goto invalid;
                }
                pending = &session->pending_calls[session->pending_call_count++];
                memset(pending, 0, sizeof(*pending));
                memcpy(pending->call_id, item->call_id,
                       sizeof(pending->call_id));
                if (!copy_small(pending->tool_name,
                                sizeof(pending->tool_name), item->name)) {
                    snj_response_graph_free(&graph);
                    goto invalid;
                }
                if (strcmp(item->name, "write_stdin") == 0) {
                    const char *handle = snj_json_string(item->arguments, "handle");
                    if (handle && snj_hex_is_lower(handle, SNJ_ID_HEX_LEN))
                        memcpy(pending->process_handle, handle,
                               sizeof(pending->process_handle));
                }
                if (snj_tool_action_digest(item, session->workspace,
                                           pending->action_sha256) < 0) {
                    snj_response_graph_free(&graph);
                    return -1;
                }
            }
        }
        if (decision.outcome == SNJ_GRAPH_FINAL ||
            decision.outcome == SNJ_GRAPH_REFUSAL) {
            const struct snj_response_item *item = &graph.items[decision.final_index];
            memcpy(session->final_item_id, item->local_item_id,
                   sizeof(session->final_item_id));
            memcpy(session->final_response_id, response_id,
                   sizeof(session->final_response_id));
        }
        snj_response_graph_free(&graph);
    } else if (strcmp(type, "tool_started") == 0) {
        static const char *const keys[] = {
            "action_sha256", "call_id", "resolved_workdir", "turn_id"
        };
        const char *action = snj_json_string(data, "action_sha256");
        const char *call_id = snj_json_string(data, "call_id");
        const char *workspace = snj_json_string(data, "resolved_workdir");
        const char *turn_id = snj_json_string(data, "turn_id");
        struct snj_pending_call *call;
        size_t index;
        if (!snj_json_exact_keys(data, keys, 4u) || !session->active_turn ||
            !session->response_complete || session->response_outcome != SNJ_GRAPH_CALLS ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !action || !snj_hex_is_lower(action, SNJ_SHA256_HEX_LEN) ||
            !workspace || strcmp(workspace, session->workspace) != 0 ||
            !call_id || !(call = find_pending_call(session, call_id, &index)) ||
            strcmp(action, call->action_sha256) != 0 ||
            call->started || call->finished ||
            session->tool_invocations >= SNJ_MAX_TOOL_INVOCATIONS)
            goto invalid;
        for (size_t i = 0; i < index; ++i)
            if (!session->pending_calls[i].finished)
                goto invalid;
        call->started = true;
        ++session->tool_invocations;
    } else if (strcmp(type, "tool_finished") == 0) {
        static const char *const keys[] = {"call_id", "result", "turn_id"};
        const char *call_id = snj_json_string(data, "call_id");
        const char *turn_id = snj_json_string(data, "turn_id");
        json_t *result = json_object_get(data, "result");
        const char *status = snj_json_string(result, "status");
        const char *handle = snj_json_string(result, "handle");
        struct snj_pending_call *call;
        size_t index;
        if (!snj_json_exact_keys(data, keys, 3u) || !session->active_turn ||
            !session->response_complete || !turn_id ||
            strcmp(turn_id, session->active_turn_id) != 0 || !call_id ||
            !(call = find_pending_call(session, call_id, &index)) || call->finished ||
            snj_tool_result_valid(result) < 0 || !status)
            goto invalid;
        for (size_t i = 0; i < index; ++i)
            if (!session->pending_calls[i].finished)
                goto invalid;
        if (call->started) {
            if (strcmp(status, "not_run") == 0 || strcmp(status, "denied") == 0)
                goto invalid;
        } else if (strcmp(status, "not_run") != 0 &&
                   strcmp(status, "denied") != 0) {
            goto invalid;
        }
        if (strcmp(status, "running") == 0) {
            if (!handle)
                goto invalid;
            if (strcmp(call->tool_name, "exec_command") == 0) {
                if (session->active_process_handle[0] != '\0')
                    goto invalid;
                memcpy(session->active_process_handle, handle,
                       sizeof(session->active_process_handle));
            } else if (strcmp(call->tool_name, "write_stdin") == 0) {
                if (session->active_process_handle[0] == '\0' ||
                    call->process_handle[0] == '\0' ||
                    strcmp(call->process_handle,
                           session->active_process_handle) != 0 ||
                    strcmp(handle, session->active_process_handle) != 0)
                    goto invalid;
            } else {
                goto invalid;
            }
        } else if (strcmp(call->tool_name, "write_stdin") == 0 &&
                   call->started && call->process_handle[0] != '\0' &&
                   session->active_process_handle[0] != '\0' &&
                   strcmp(call->process_handle,
                          session->active_process_handle) == 0) {
            session->active_process_handle[0] = '\0';
        }
        call->finished = true;
        if (all_pending_finished(session) &&
            session->response_outcome == SNJ_GRAPH_CALLS) {
            session->response_complete = false;
            session->pending_call_count = 0;
            memset(session->pending_calls, 0, sizeof(session->pending_calls));
            session->active_response_id[0] = '\0';
        }
    } else if (strcmp(type, "process_closed") == 0) {
        static const char *const keys[] = {"cause", "handle", "result", "turn_id"};
        static const char *const causes[] = {
            "user_interrupt", "provider_failure", "protocol_failure",
            "tool_failure", "output_failure", "internal_failure"
        };
        const char *cause = snj_json_string(data, "cause");
        const char *handle = snj_json_string(data, "handle");
        const char *turn_id = snj_json_string(data, "turn_id");
        json_t *result = json_object_get(data, "result");
        const char *status = snj_json_string(result, "status");
        if (!snj_json_exact_keys(data, keys, 4u) || !session->active_turn ||
            session->response_open ||
            (session->response_complete && !all_pending_finished(session)) ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !handle || !snj_hex_is_lower(handle, SNJ_ID_HEX_LEN) ||
            session->active_process_handle[0] == '\0' ||
            strcmp(handle, session->active_process_handle) != 0 ||
            !string_in(cause, causes, sizeof(causes) / sizeof(causes[0])) ||
            snj_tool_result_valid(result) < 0 ||
            !process_close_status(status))
            goto invalid;
        session->active_process_handle[0] = '\0';
    } else if (strcmp(type, "turn_completed") == 0) {
        static const char *const keys[] = {
            "final_item_id", "final_response_id", "turn_id"
        };
        const char *turn_id = snj_json_string(data, "turn_id");
        const char *item_id = snj_json_string(data, "final_item_id");
        const char *response_id = snj_json_string(data, "final_response_id");
        if (!snj_json_exact_keys(data, keys, 3u) || !session->active_turn ||
            !session->response_complete ||
            (session->response_outcome != SNJ_GRAPH_FINAL &&
             session->response_outcome != SNJ_GRAPH_REFUSAL) ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 || !item_id ||
            strcmp(item_id, session->final_item_id) != 0 || !response_id ||
            strcmp(response_id, session->final_response_id) != 0 ||
            session->active_process_handle[0] != '\0' ||
            session->pending_call_count != 0u ||
            session->pending_steering_count != 0u)
            goto invalid;
        session->active_turn = false;
        session->active_turn_id[0] = '\0';
        session->active_turn_model[0] = '\0';
        clear_response_state(session);
    } else if (strcmp(type, "turn_interrupted") == 0) {
        static const char *const keys[] = {"origin", "reason", "turn_id"};
        static const char *const origins[] = {"user", "recovery", "output"};
        static const char *const reasons[] = {
            "cancelled", "process_lost", "output_lost", "session_recovered"
        };
        const char *turn_id = snj_json_string(data, "turn_id");
        const char *origin = snj_json_string(data, "origin");
        const char *reason = snj_json_string(data, "reason");
        if (!snj_json_exact_keys(data, keys, 3u) || !session->active_turn ||
            session->active_process_handle[0] != '\0' ||
            session->response_open ||
            (session->response_complete && !all_pending_finished(session)) ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !string_in(origin, origins, sizeof(origins) / sizeof(origins[0])) ||
            !string_in(reason, reasons, sizeof(reasons) / sizeof(reasons[0])))
            goto invalid;
        session->active_turn = false;
        session->active_turn_id[0] = '\0';
        session->active_turn_model[0] = '\0';
        clear_response_state(session);
        clear_pending_steering(session);
    } else if (strcmp(type, "turn_failed") == 0) {
        static const char *const keys[] = {"class", "message", "turn_id"};
        static const char *const classes[] = {
            "context", "provider", "protocol", "tool", "persistence",
            "resource", "output", "internal"
        };
        const char *turn_id = snj_json_string(data, "turn_id");
        const char *class_name = snj_json_string(data, "class");
        const char *message = snj_json_string(data, "message");
        if (!snj_json_exact_keys(data, keys, 3u) || !session->active_turn ||
            session->active_process_handle[0] != '\0' ||
            session->response_open ||
            (session->response_complete && !all_pending_finished(session)) ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !string_in(class_name, classes, sizeof(classes) / sizeof(classes[0])) ||
            !message || strlen(message) > 8192u)
            goto invalid;
        session->active_turn = false;
        session->active_turn_id[0] = '\0';
        session->active_turn_model[0] = '\0';
        clear_response_state(session);
        clear_pending_steering(session);
    } else {
        set_error(error, error_size,
                  "event type %s is not implemented by this checkpoint", type);
        errno = ENOTSUP;
        return -1;
    }
    return 0;
invalid:
    set_error(error, error_size, "invalid %s transition at sequence %llu", type,
              (unsigned long long)seq);
    errno = EINVAL;
    return -1;
}

int
snj_store_scan_log(struct snj_session *session,
                   enum snj_tail_policy tail_policy, bool allow_active,
                   char *error, size_t error_size)
{
    struct snj_buf line;
    unsigned char chunk[8192];
    off_t complete_end = 0;
    off_t read_off = 0;
    uint64_t seq = 1;
    char prev[SNJ_SHA256_HEX_LEN + 1u];
    int rc = -1;

    memset(prev, '0', SNJ_SHA256_HEX_LEN);
    prev[SNJ_SHA256_HEX_LEN] = '\0';
    snj_buf_init(&line, SNJ_MAX_EVENT_LINE);
    for (;;) {
        ssize_t got = pread(session->log_fd, chunk, sizeof(chunk), read_off);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            set_error(error, error_size, "cannot read event log: %s", strerror(errno));
            goto out;
        }
        if (got == 0)
            break;
        read_off += got;
        for (ssize_t i = 0; i < got; ++i) {
            if (chunk[i] != '\n') {
                if (snj_buf_putc(&line, chunk[i]) < 0) {
                    set_error(error, error_size, "event line exceeds 16 MiB");
                    goto out;
                }
                continue;
            }
            if (!line.len) {
                set_error(error, error_size, "blank event line at sequence %llu",
                          (unsigned long long)seq);
                errno = EINVAL;
                goto out;
            }
            char jerr[192];
            json_t *event = snj_json_load_canonical(line.data, line.len,
                                                    jerr, sizeof(jerr));
            const char *type;
            json_t *data;
            if (!event) {
                set_error(error, error_size, "corrupt event %llu: %s",
                          (unsigned long long)seq, jerr);
                goto out;
            }
            if (!common_event_valid(event, session, seq, prev, &type, &data,
                                    error, error_size) ||
                apply_event(session, type, data, seq, error, error_size) < 0) {
                json_decref(event);
                goto out;
            }
            memcpy(prev, session->prev_sha256, sizeof(prev));
            json_decref(event);
            complete_end = read_off - (off_t)(got - i - 1);
            ++seq;
            snj_buf_reset(&line);
        }
    }
    if (line.len && tail_policy == SNJ_TAIL_REJECT) {
        set_error(error, error_size, "event log has an incomplete final suffix");
        errno = EINVAL;
        goto out;
    }
    if (line.len && tail_policy == SNJ_TAIL_TRUNCATE &&
        (ftruncate(session->log_fd, complete_end) < 0 ||
         snj_sync_file(session->log_fd) < 0)) {
        set_error(error, error_size, "cannot truncate incomplete log tail: %s",
                  strerror(errno));
        goto out;
    }
    session->log_end = complete_end;
    session->next_seq = seq;
    if (seq == 1) {
        set_error(error, error_size, "session event log is empty");
        errno = EINVAL;
        goto out;
    }
    if (session->active_turn && !allow_active) {
        set_error(error, error_size,
                  "active-turn recovery is unavailable in this scan mode");
        errno = ENOTSUP;
        goto out;
    }
    session->active_compact_id[0] = '\0';
    session->active_compact_source_sha256[0] = '\0';
    session->active_compact_source_seq = 0u;
    rc = 0;
out:
    snj_buf_free(&line);
    return rc;
}


int
snj_session_each_event(struct snj_session *session, snj_session_event_fn fn,
                       void *opaque, char *error, size_t error_size)
{
    struct snj_session verifier;
    struct snj_buf line;
    unsigned char chunk[8192];
    off_t read_off = 0;
    uint64_t seq = 1;
    char prev[SNJ_SHA256_HEX_LEN + 1u];
    int rc = -1;

    if (!session || !fn || session->log_fd < 0 || session->log_end < 0 ||
        !snj_hex_is_lower(session->id, SNJ_ID_HEX_LEN)) {
        set_error(error, error_size, "invalid session event iterator");
        errno = EINVAL;
        return -1;
    }
    snj_session_init(&verifier);
    memcpy(verifier.id, session->id, sizeof(verifier.id));
    memset(prev, '0', SNJ_SHA256_HEX_LEN);
    prev[SNJ_SHA256_HEX_LEN] = '\0';
    snj_buf_init(&line, SNJ_MAX_EVENT_LINE);
    while (read_off < session->log_end) {
        size_t want = sizeof(chunk);
        ssize_t got;
        if ((off_t)want > session->log_end - read_off)
            want = (size_t)(session->log_end - read_off);
        got = pread(session->log_fd, chunk, want, read_off);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            set_error(error, error_size, "cannot read event log: %s", strerror(errno));
            goto out;
        }
        if (got == 0) {
            set_error(error, error_size, "event log ended before recorded boundary");
            errno = EIO;
            goto out;
        }
        read_off += got;
        for (ssize_t i = 0; i < got; ++i) {
            json_t *event;
            const char *type;
            json_t *data;
            char jerr[192];

            if (chunk[i] != '\n') {
                if (snj_buf_putc(&line, chunk[i]) < 0) {
                    set_error(error, error_size, "event line exceeds 16 MiB");
                    goto out;
                }
                continue;
            }
            if (!line.len) {
                set_error(error, error_size, "blank event line at sequence %llu",
                          (unsigned long long)seq);
                errno = EINVAL;
                goto out;
            }
            event = snj_json_load_canonical(line.data, line.len,
                                            jerr, sizeof(jerr));
            if (!event) {
                set_error(error, error_size, "corrupt event %llu: %s",
                          (unsigned long long)seq, jerr);
                goto out;
            }
            if (!common_event_valid(event, &verifier, seq, prev, &type, &data,
                                    error, error_size) ||
                fn(opaque, seq, type, data, error, error_size) < 0) {
                json_decref(event);
                goto out;
            }
            memcpy(prev, verifier.prev_sha256, sizeof(prev));
            json_decref(event);
            ++seq;
            snj_buf_reset(&line);
        }
    }
    if (line.len) {
        set_error(error, error_size, "event log has an incomplete final suffix");
        errno = EINVAL;
        goto out;
    }
    rc = 0;
out:
    snj_buf_free(&line);
    return rc;
}

static char *
clone_optional(const char *value, size_t max)
{
    return value ? snj_strdup_checked(value, max) : NULL;
}

static void
free_staged_state(struct snj_session *session)
{
    free_pending_user_state(session);
    free(session->workspace);
    free(session->first_user);
    free(session->last_user);
    free(session->last_assistant);
    if (session->compact_output)
        json_decref(session->compact_output);
    session->workspace = NULL;
    session->first_user = NULL;
    session->last_user = NULL;
    session->last_assistant = NULL;
    session->compact_output = NULL;
}

static int
clone_session_state(const struct snj_session *source,
                    struct snj_session *staged)
{
    *staged = *source;
    staged->workspace = NULL;
    staged->first_user = NULL;
    staged->last_user = NULL;
    staged->last_assistant = NULL;
    staged->compact_output = NULL;
    for (size_t i = 0; i < staged->pending_steering_count; ++i)
        staged->pending_steering[i].text = NULL;
    for (size_t i = 0; i < staged->pending_queue_count; ++i)
        staged->pending_queue[i].text = NULL;

    staged->workspace = clone_optional(source->workspace, SNJ_PATH_MAX_BYTES);
    staged->first_user = clone_optional(source->first_user, SNJ_MAX_DIRECT_PROMPT);
    staged->last_user = clone_optional(source->last_user, SNJ_MAX_DIRECT_PROMPT);
    staged->last_assistant = clone_optional(source->last_assistant,
                                            SNJ_MAX_PUBLIC_ITEM);
    if (source->compact_output)
        staged->compact_output = json_deep_copy(source->compact_output);
    if ((source->workspace && !staged->workspace) ||
        (source->first_user && !staged->first_user) ||
        (source->last_user && !staged->last_user) ||
        (source->last_assistant && !staged->last_assistant) ||
        (source->compact_output && !staged->compact_output))
        goto fail;
    for (size_t i = 0; i < source->pending_steering_count; ++i) {
        staged->pending_steering[i].text =
            clone_optional(source->pending_steering[i].text,
                           SNJ_MAX_STEERING_TEXT);
        if (!staged->pending_steering[i].text)
            goto fail;
    }
    for (size_t i = 0; i < source->pending_queue_count; ++i) {
        staged->pending_queue[i].text =
            clone_optional(source->pending_queue[i].text,
                           SNJ_MAX_QUEUED_TEXT);
        if (!staged->pending_queue[i].text)
            goto fail;
    }
    return 0;
fail:
    free_staged_state(staged);
    return -1;
}

static void
adopt_session_state(struct snj_session *session, struct snj_session *staged)
{
    char *dir_path = session->dir_path;
    int dir_fd = session->dir_fd;
    int log_fd = session->log_fd;
    int lock_fd = session->lock_fd;
    off_t log_end = session->log_end;
    uint64_t next_seq = session->next_seq;
    uint64_t last_time_ms = session->last_time_ms;
    char prev_sha256[SNJ_SHA256_HEX_LEN + 1u];

    memcpy(prev_sha256, session->prev_sha256, sizeof(prev_sha256));
    free_pending_user_state(session);
    free(session->workspace);
    free(session->first_user);
    free(session->last_user);
    free(session->last_assistant);
    if (session->compact_output)
        json_decref(session->compact_output);
    *session = *staged;
    session->dir_path = dir_path;
    session->dir_fd = dir_fd;
    session->log_fd = log_fd;
    session->lock_fd = lock_fd;
    session->log_end = log_end;
    session->next_seq = next_seq;
    session->last_time_ms = last_time_ms;
    memcpy(session->prev_sha256, prev_sha256, sizeof(prev_sha256));
    staged->workspace = NULL;
    staged->first_user = NULL;
    staged->last_user = NULL;
    staged->last_assistant = NULL;
    staged->compact_output = NULL;
    for (size_t i = 0; i < staged->pending_steering_count; ++i)
        staged->pending_steering[i].text = NULL;
    for (size_t i = 0; i < staged->pending_queue_count; ++i)
        staged->pending_queue[i].text = NULL;
    staged->pending_steering_count = 0;
    staged->pending_queue_count = 0;
}

int
snj_session_commit(struct snj_session *session, const char *type, json_t *data,
                   uint64_t *written_seq, char *error, size_t error_size)
{
    struct snj_session staged;
    json_t *apply = data ? json_deep_copy(data) : NULL;
    uint64_t seq = session->next_seq;

    memset(&staged, 0, sizeof(staged));
    if (!apply || clone_session_state(session, &staged) < 0) {
        if (data)
            json_decref(data);
        if (apply)
            json_decref(apply);
        free_staged_state(&staged);
        set_error(error, error_size, "cannot stage %s event", type);
        errno = ENOMEM;
        return -1;
    }
    if (apply_event(&staged, type, apply, seq, error, error_size) < 0) {
        json_decref(data);
        json_decref(apply);
        free_staged_state(&staged);
        return -1;
    }
    json_decref(apply);
    if (snj_session_append(session, type, data, written_seq,
                           error, error_size) < 0) {
        free_staged_state(&staged);
        return -1;
    }
    adopt_session_state(session, &staged);
    return 0;
}

static char *
canonical_workspace(const char *workspace, char *error, size_t error_size)
{
    char *resolved = realpath(workspace, NULL);
    struct stat st;

    if (!resolved) {
        set_error(error, error_size, "cannot resolve workspace %s: %s", workspace,
                  strerror(errno));
        return NULL;
    }
    if (strlen(resolved) > SNJ_PATH_MAX_BYTES ||
        !snj_utf8_valid((const unsigned char *)resolved, strlen(resolved), true) ||
        stat(resolved, &st) < 0 || !S_ISDIR(st.st_mode)) {
        set_error(error, error_size, "workspace must be an existing UTF-8 directory");
        free(resolved);
        errno = EINVAL;
        return NULL;
    }
    return resolved;
}

static json_t *
session_created_data(const char *workspace, const char *model,
                     const char *effort)
{
    json_t *data = json_object();
    if (!data ||
        json_set_new(data, "default_effort", json_string(effort)) < 0 ||
        json_set_new(data, "default_model", json_string(model)) < 0 ||
        json_set_new(data, "format", json_integer(1)) < 0 ||
        json_set_new(data, "protocol", json_string("responses")) < 0 ||
        json_set_new(data, "workspace", json_string(workspace)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

int
snj_session_create(struct snj_store *store, struct snj_session *session,
                   const char *workspace, const char *model, const char *effort,
                   char *error, size_t error_size)
{
    char *resolved = NULL;
    char *dir = NULL;
    int created = 0;
    int rc = -1;

    resolved = canonical_workspace(workspace, error, error_size);
    if (!resolved)
        return -1;
    for (unsigned int attempt = 0; attempt < 32u; ++attempt) {
        if (snj_random_id(session->id) < 0) {
            set_error(error, error_size, "cryptographic session id generation failed");
            goto out;
        }
        if (mkdirat(store->sessions_fd, session->id, 0700) == 0) {
            created = 1;
            break;
        }
        if (errno != EEXIST) {
            set_error(error, error_size, "cannot create session directory: %s",
                      strerror(errno));
            goto out;
        }
    }
    if (!created) {
        set_error(error, error_size, "could not allocate a unique session id");
        errno = EEXIST;
        goto out;
    }
    dir = snj_store_path_join(store->root_path, "sessions");
    if (dir) {
        char *full = snj_store_path_join(dir, session->id);
        free(dir);
        dir = full;
    }
    if (!dir)
        goto out;
    session->dir_path = dir;
    dir = NULL;
    session->dir_fd = openat(store->sessions_fd, session->id,
                             O_RDONLY | O_DIRECTORY
#ifdef O_CLOEXEC
                             | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                             | O_NOFOLLOW
#endif
    );
    if (session->dir_fd < 0) {
        set_error(error, error_size, "cannot open new session directory: %s",
                  strerror(errno));
        goto out;
    }
    if (snj_store_verify_private_fd(session->dir_fd, true, "session directory",
                          error, error_size) < 0 ||
        snj_store_open_session_files(session, true, error, error_size) < 0)
        goto out;
    session->workspace = resolved;
    resolved = NULL;
    if (!copy_small(session->default_model, sizeof(session->default_model), model) ||
        !copy_small(session->default_effort, sizeof(session->default_effort), effort)) {
        errno = EOVERFLOW;
        goto out;
    }
    if (snj_session_commit(session, "session_created",
                           session_created_data(session->workspace, model, effort),
                           NULL, error, error_size) < 0)
        goto out;
    if (snj_sync_dir(session->dir_fd) < 0 || snj_sync_dir(store->sessions_fd) < 0) {
        set_error(error, error_size, "cannot sync new session directory: %s",
                  strerror(errno));
        goto out;
    }
    rc = 0;
out:
    free(resolved);
    free(dir);
    if (rc < 0 && created) {
        char failed_id[SNJ_ID_HEX_LEN + 1u];
        memcpy(failed_id, session->id, sizeof(failed_id));
        if (session->dir_fd >= 0) {
            (void)unlinkat(session->dir_fd, "events.jsonl", 0);
            (void)unlinkat(session->dir_fd, "lock", 0);
        }
        snj_session_close(session);
        (void)unlinkat(store->sessions_fd, failed_id, AT_REMOVEDIR);
    }
    return rc;
}
