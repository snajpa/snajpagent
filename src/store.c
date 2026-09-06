/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_internal.h"
#include "fs.h"
#include "instructions.h"
#include "irc.h"
#include "snajpagent.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define SNAG_LOG_HARD_LIMIT ((int64_t)2 * 1024 * 1024 * 1024)
#define SNAG_LOG_RESERVE ((int64_t)32 * 1024 * 1024)
#define SNAG_EVENT_LIMIT UINT64_C(1000000)
#define SNAG_EVENT_RESERVE UINT64_C(256)
static int
open_dir_path(const char *path)
{
    return snag_open_read_security_at(-1, path, true);
}
int
snag_store_verify_private_fd(int fd, bool directory, const char *name,
                  char *error, size_t error_size)
{
    snag_file_info st;
    struct snag_file_privacy privacy;
    bool valid;
    if (snag_fstat(fd, &st) < 0 || snag_fd_privacy(fd, &privacy) < 0) {
        snag_errorf(error, error_size, "cannot inspect %s: %s", name,
                  strerror(errno));
        return -1;
    }
    valid = (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode)) &&
            privacy.real_owner && privacy.private_access;
    if (!valid) {
        snag_errorf(error, error_size, "%s must be private and user-owned", name);
        errno = EACCES;
        return -1;
    }
    return 0;
}
static int
ensure_directory(const char *path, bool require_private,
                 char *error, size_t error_size)
{
    snag_file_info st;
    if (snag_lstat(path, &st) < 0) {
        if (errno != ENOENT) {
            snag_errorf(error, error_size, "cannot inspect %s: %s", path,
                      strerror(errno));
            return -1;
        }
        if (snag_mkdir_private(path) < 0) {
            snag_errorf(error, error_size, "cannot create %s: %s", path,
                      strerror(errno));
            return -1;
        }
        if (snag_lstat(path, &st) < 0) {
            snag_errorf(error, error_size, "cannot verify %s: %s", path,
                      strerror(errno));
            return -1;
        }
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        snag_errorf(error, error_size, "%s is not a real directory", path);
        errno = EINVAL;
        return -1;
    }
    if (require_private) {
        int fd = open_dir_path(path), rc;
        if (fd < 0)
            return -1;
        rc = snag_store_verify_private_fd(fd, true, path, error, error_size);
        int saved = errno;
        if (close(fd) < 0 && rc == 0)
            return -1;
        errno = saved;
        if (rc < 0) {
            snag_errorf(error, error_size, "%s must be private (mode 0700)", path);
            return -1;
        }
    }
    return 0;
}
static int
mkdir_parents(const char *path, char *error, size_t error_size)
{
    char *copy = snag_strdup_checked(path, SNAG_PATH_MAX_BYTES);
    char *p;
    if (!copy)
        return -1;
    for (p = copy + snag_path_root_len(path); *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (ensure_directory(copy, false, error, error_size) < 0) {
            free(copy);
            return -1;
        }
        *p = '/';
    }
    free(copy);
    return 0;
}
void
snag_store_init(struct snag_store *store)
{
    memset(store, 0, sizeof(*store));
    store->root_fd = -1;
    store->sessions_fd = -1;
    store->trash_fd = -1;
}
void
snag_store_close(struct snag_store *store)
{
    if (store->trash_fd >= 0)
        (void)close(store->trash_fd);
    if (store->sessions_fd >= 0)
        (void)close(store->sessions_fd);
    if (store->root_fd >= 0)
        (void)close(store->root_fd);
    free(store->root_path);
    snag_store_init(store);
}
int
snag_store_open(struct snag_store *store, const char *dotdir,
               char *error, size_t error_size)
{
    char *sessions = NULL;
    char *trash = NULL;
    int rc = -1;
    if (!snag_path_root_len(dotdir) || strlen(dotdir) > SNAG_PATH_MAX_BYTES ||
        !snag_utf8_valid((const unsigned char *)dotdir, strlen(dotdir), true)) {
        snag_errorf(error, error_size,
                  "dotdir must be an absolute UTF-8 path within the supported limit");
        errno = EINVAL;
        return -1;
    }
    store->root_path = snag_strdup_checked(dotdir, SNAG_PATH_MAX_BYTES);
    if (!store->root_path ||
        mkdir_parents(store->root_path, error, error_size) < 0 ||
        ensure_directory(store->root_path, true, error, error_size) < 0)
        goto out;
    store->root_fd = open_dir_path(store->root_path);
    if (store->root_fd < 0)
        goto io_error;
    if (snag_store_verify_private_fd(store->root_fd, true, "state root",
                          error, error_size) < 0)
        goto out;
    sessions = snag_path_join(store->root_path, "sessions");
    trash = snag_path_join(store->root_path, "trash");
    if (!sessions || !trash ||
        ensure_directory(sessions, true, error, error_size) < 0 ||
        ensure_directory(trash, true, error, error_size) < 0)
        goto out;
    store->sessions_fd = open_dir_path(sessions);
    store->trash_fd = open_dir_path(trash);
    if (store->sessions_fd < 0 || store->trash_fd < 0)
        goto io_error;
    if (snag_store_verify_private_fd(store->sessions_fd, true, "sessions directory",
                          error, error_size) < 0 ||
        snag_store_verify_private_fd(store->trash_fd, true, "trash directory",
                          error, error_size) < 0)
        goto out;
    rc = 0;
    goto out;
io_error:
    snag_errorf(error, error_size, "cannot open state directory: %s", strerror(errno));
out:
    free(sessions);
    free(trash);
    if (rc < 0)
        snag_store_close(store);
    return rc;
}
static void
free_pending_user_state(struct snag_session *session)
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
snag_session_init(struct snag_session *session)
{
    memset(session, 0, sizeof(*session));
    session->dir_fd = -1;
    session->log_fd = -1;
    session->lock_fd = -1;
    memset(session->prev_sha256, '0', SNAG_SHA256_HEX_LEN);
    session->prev_sha256[SNAG_SHA256_HEX_LEN] = '\0';
    session->next_seq = 1;
}
static void
free_session_state(struct snag_session *session)
{
    free_pending_user_state(session);
    free(session->workspace);
    free(session->first_user);
    free(session->last_user);
    free(session->last_assistant);
    free(session->goal_prompt);
    free(session->goal_blocker);
    if (session->compact_output)
        json_decref(session->compact_output);
}

void
snag_session_close(struct snag_session *session)
{
    if (session->log_fd >= 0)
        (void)close(session->log_fd);
    if (session->lock_fd >= 0)
        (void)close(session->lock_fd);
    if (session->dir_fd >= 0)
        (void)close(session->dir_fd);
    free_session_state(session);
    free(session->dir_path);
    snag_session_init(session);
}
static int
lock_session(int dir_fd, int *fd_out, char *error, size_t error_size)
{
    int fd;
    fd = snag_create_private_at(dir_fd, "lock", false);
    if (fd < 0) {
        snag_errorf(error, error_size, "cannot open session lock: %s", strerror(errno));
        return -1;
    }
    if (snag_fd_cloexec(fd) < 0 ||
        snag_store_verify_private_fd(fd, false, "session lock", error, error_size) < 0) {
        (void)close(fd);
        return -1;
    }
    if (snag_lock_file(fd, false) < 0) {
        snag_errorf(error, error_size, errno == EACCES || errno == EAGAIN ?
                  "session is already open" : "cannot lock session: %s",
                  strerror(errno));
        (void)close(fd);
        return -1;
    }
    *fd_out = fd;
    return 0;
}
int
snag_store_open_session_files(struct snag_session *session, bool create,
                   char *error, size_t error_size)
{
    if (lock_session(session->dir_fd, &session->lock_fd, error, error_size) < 0)
        return -1;
    session->log_fd = snag_open_private_append_at(session->dir_fd, "events.jsonl", create);
    if (session->log_fd < 0) {
        snag_errorf(error, error_size, "cannot open event log: %s", strerror(errno));
        return -1;
    }
    if (snag_fd_cloexec(session->log_fd) < 0 ||
        snag_store_verify_private_fd(session->log_fd, false, "event log",
                          error, error_size) < 0)
        return -1;
    session->log_end = snag_seek(session->log_fd, 0, SEEK_END);
    if (session->log_end < 0) {
        snag_errorf(error, error_size, "cannot seek event log: %s", strerror(errno));
        return -1;
    }
    return 0;
}
static int
snag_session_append(struct snag_session *session, const char *type, json_t *data,
                   uint64_t *written_seq, char *error, size_t error_size)
{
    json_t *event = NULL;
    struct snag_buf line;
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    int64_t actual_end;
    uint64_t seq = session->next_seq;
    int rc = -1;
    snag_buf_init(&line, SNAG_MAX_EVENT_LINE);
    if (!data || seq > SNAG_EVENT_LIMIT - SNAG_EVENT_RESERVE ||
        session->log_end > SNAG_LOG_HARD_LIMIT - SNAG_LOG_RESERVE) {
        snag_errorf(error, error_size, "session log has no admission reserve");
        errno = ENOSPC;
        goto out;
    }
    event = json_pack("{s:O,s:s,s:I,s:s,s:I,s:s,s:i}",
        "data", data, "prev_sha256", session->prev_sha256,
        "seq", (json_int_t)seq, "session_id", session->id,
        "time_ms", (json_int_t)snag_time_ms(), "type", type, "v", 1);
    if (!event || snag_json_digest(event, digest) < 0)
        goto memory_error;
    if (snag_json_set_new(event, "event_sha256", json_string(digest)) < 0 ||
        snag_json_canonical(event, &line) < 0 || snag_buf_putc(&line, '\n') < 0)
        goto memory_error;
    if ((int64_t)line.len > SNAG_LOG_HARD_LIMIT - SNAG_LOG_RESERVE - session->log_end) {
        snag_errorf(error, error_size, "event would consume session closure reserve");
        errno = ENOSPC;
        goto out;
    }
    actual_end = snag_seek(session->log_fd, 0, SEEK_END);
    if (actual_end < 0 || actual_end != session->log_end) {
        snag_errorf(error, error_size, "event log end changed unexpectedly");
        errno = EIO;
        goto out;
    }
    if (snag_write_full(session->log_fd, line.data, line.len) < 0 ||
        snag_sync_file(session->log_fd) < 0) {
        snag_errorf(error, error_size, "cannot durably append %s: %s", type,
                  strerror(errno));
        goto out;
    }
    session->log_end += (int64_t)line.len;
    session->next_seq++;
    memcpy(session->prev_sha256, digest, sizeof(digest));
    session->last_time_ms = snag_time_ms();
    if (written_seq)
        *written_seq = seq;
    rc = 0;
    goto out;
memory_error:
    snag_errorf(error, error_size, "cannot encode %s event", type);
out:
    if (event)
        json_decref(event);
    snag_buf_free(&line);
    return rc;
}
static int
replace_text(char **slot, const char *text, size_t max)
{
    char *copy = snag_strdup_checked(text, max);
    if (!copy)
        return -1;
    free(*slot);
    *slot = copy;
    return 0;
}
static bool
common_event_valid(json_t *event, struct snag_session *session, uint64_t seq,
                   const char **type_out, json_t **data_out,
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
    char computed[SNAG_SHA256_HEX_LEN + 1u];
    if (!snag_json_exact_keys(event, keys, sizeof(keys) / sizeof(keys[0])) ||
        snag_json_integer_u64(event, "v", &n) < 0 || n != 1 ||
        snag_json_integer_u64(event, "seq", &n) < 0 || n != seq ||
        snag_json_integer_u64(event, "time_ms", &n) < 0 ||
        !(event_hash = snag_json_string(event, "event_sha256")) ||
        !(prev_hash = snag_json_string(event, "prev_sha256")) ||
        !(session_id = snag_json_string(event, "session_id")) ||
        !(type = snag_json_string(event, "type")) ||
        !snag_hex_is_lower(event_hash, SNAG_SHA256_HEX_LEN) ||
        !snag_hex_is_lower(prev_hash, SNAG_SHA256_HEX_LEN) ||
        strcmp(prev_hash, session->prev_sha256) != 0 ||
        strcmp(session_id, session->id) != 0 ||
        !json_is_object(json_object_get(event, "data"))) {
        snag_errorf(error, error_size, "invalid event envelope at sequence %llu",
                  (unsigned long long)seq);
        return false;
    }
    copy = json_copy(event);
    if (!copy || json_object_del(copy, "event_sha256") < 0 ||
        snag_json_digest(copy, computed) < 0) {
        if (copy)
            json_decref(copy);
        snag_errorf(error, error_size, "cannot verify event digest");
        return false;
    }
    json_decref(copy);
    if (strcmp(event_hash, computed) != 0) {
        snag_errorf(error, error_size, "event digest mismatch at sequence %llu",
                  (unsigned long long)seq);
        return false;
    }
    memcpy(session->prev_sha256, event_hash, SNAG_SHA256_HEX_LEN + 1u);
    session->last_time_ms = n;
    *type_out = type;
    *data_out = json_object_get(event, "data");
    return true;
}
static void
clear_response_state(struct snag_session *session)
{
    session->response_open = false;
    session->response_complete = false;
    session->response_terminal = SNAG_RESPONSE_TERMINAL_NONE;
    session->active_response_id[0] = '\0';
    session->active_response_model_input_sha256[0] = '\0';
    session->active_response_request_input_sha256[0] = '\0';
    session->active_response_request_sha256[0] = '\0';
    session->active_response_provider_source_sha256[0] = '\0';
    session->active_response_model_input_bytes = 0u;
    session->active_response_request_input_bytes = 0u;
    session->active_response_request_input_count = 0u;
    session->active_response_requested_output_tokens = 0u;
    session->active_response_requested_output_known = false;
    session->final_item_id[0] = '\0';
    session->final_response_id[0] = '\0';
    session->pending_call_count = 0;
    memset(session->pending_calls, 0, sizeof(session->pending_calls));
}

static bool
all_pending_finished(const struct snag_session *session)
{
    for (size_t i = 0; i < session->pending_call_count; ++i)
        if (!session->pending_calls[i].finished)
            return false;
    return true;
}

struct snag_process_state *
snag_session_process(struct snag_session *session, const char *handle)
{
    for (size_t i = 0u; i < session->process_count; ++i)
        if (handle && !strcmp(session->processes[i].handle, handle))
            return &session->processes[i];
    return NULL;
}

static void
remove_process(struct snag_session *session, struct snag_process_state *process)
{
    size_t i = (size_t)(process - session->processes);
    --session->process_count;
    memmove(process, process + 1, (session->process_count - i) * sizeof(*process));
    memset(&session->processes[session->process_count], 0, sizeof(*process));
}

static void
process_label(char out[257], const char *text)
{
    size_t n = text ? strlen(text) : 0u;
    if (n > 256u)
        n = 256u;
    while (n && !snag_utf8_valid((const unsigned char *)text, n, true))
        --n;
    if (n)
        memcpy(out, text, n);
    out[n] = '\0';
}

int
snag_process_output_decode(const json_t *data, struct snag_buf *bytes)
{
    static const char *const keys[] = {"turn_id", "handle", "stream", "offset", "encoding", "data"};
    const char *encoding = snag_json_string(data, "encoding");
    const char *text = snag_json_string(data, "data");
    if (!snag_json_exact_keys(data, keys, 6u) || !encoding || !text)
        return -1;
    if (!strcmp(encoding, "utf8"))
        return snag_buf_append(bytes, text, strlen(text));
    return !strcmp(encoding, "base64") ? snag_base64_decode(bytes, text) : -1;
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
                      char out[SNAG_SHA256_HEX_LEN + 1u], size_t *bytes)
{
    if (!json_is_array(output) || json_array_size(output) == 0u ||
        json_array_size(output) > 128u) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < json_array_size(output); ++i) {
        json_t *item = json_array_get(output, i);
        if (!json_is_object(item) || !snag_json_string(item, "type")) {
            errno = EINVAL;
            return -1;
        }
    }
    return snag_json_digest_bounded(output, 12u * 1024u * 1024u,
                                   out, bytes);
}
static bool
valid_trash_name(const struct snag_session *session, const char *name)
{
    if (!name || strlen(name) != SNAG_TRASH_NAME_LEN)
        return false;
    if (memcmp(name, session->id, SNAG_ID_HEX_LEN) != 0 ||
        name[SNAG_ID_HEX_LEN] != '.')
        return false;
    return snag_hex_is_lower(name + SNAG_ID_HEX_LEN + 1u,
                            SNAG_TRASH_SUFFIX_HEX_LEN);
}
static struct snag_pending_call *
find_pending_call(struct snag_session *session, const char *call_id,
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
clear_pending_steering(struct snag_session *session)
{
    for (size_t i = 0; i < session->pending_steering_count; ++i) {
        free(session->pending_steering[i].text);
        session->pending_steering[i].text = NULL;
    }
    session->pending_steering_count = 0;
    session->pending_steering_bytes = 0;
}
static bool
pending_user_id_exists(const struct snag_session *session, const char *id)
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
add_pending_steering(struct snag_session *session, const char *id,
                     const char *text, size_t len, uint64_t seq)
{
    struct snag_pending_steering *pending;
    char *copy = snag_strdup_checked(text, SNAG_MAX_STEERING_TEXT);

    if (!copy)
        return -1;
    pending = &session->pending_steering[session->pending_steering_count++];
    memset(pending, 0, sizeof(*pending));
    memcpy(pending->steering_id, id, sizeof(pending->steering_id));
    pending->seq = seq;
    pending->text = copy;
    session->pending_steering_bytes += len;
    return 0;
}
static int
consume_oldest_queue(struct snag_session *session)
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

static bool
preference_text_valid(const char *value, size_t size)
{
    size_t len;

    return value && (len = strlen(value)) != 0u && len < size &&
           snag_utf8_valid((const unsigned char *)value, len, true);
}

static bool
irc_field_valid(const char *value, size_t max, bool allow_empty)
{
    size_t len;

    if (!value || (!(len = strlen(value)) && !allow_empty) || len > max ||
        !snag_utf8_valid((const unsigned char *)value, len, true))
        return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20u || c == 0x7fu)
            return false;
    }
    return true;
}

static bool
irc_kind_valid(const char *kind)
{
    static const char *const kinds[] = {
        "connected", "disconnected", "join", "part", "quit", "nick",
        "message", "notice", "topic", "mode", "history_ready"
    };

    return string_in(kind, kinds, sizeof(kinds) / sizeof(kinds[0]));
}

const char *
snag_goal_status_name(enum snag_goal_status status)
{
    switch (status) {
    case SNAG_GOAL_NONE: return "none";
    case SNAG_GOAL_ACTIVE: return "active";
    case SNAG_GOAL_PAUSED: return "paused";
    case SNAG_GOAL_BLOCKED: return "blocked";
    case SNAG_GOAL_COMPLETED: return "completed";
    case SNAG_GOAL_CANCELLED: return "cancelled";
    }
    return "unknown";
}

bool
snag_goal_unfinished(enum snag_goal_status status)
{
    return status == SNAG_GOAL_ACTIVE || status == SNAG_GOAL_PAUSED ||
           status == SNAG_GOAL_BLOCKED;
}

static int
apply_event(struct snag_session *session, const char *type, const json_t *data,
            uint64_t seq, char *error, size_t error_size)
{
    uint64_t n;

    if (strcmp(type, "session_created") == 0) {
        static const char *const keys[] = {
            "default_effort", "default_model", "default_provider", "format",
            "protocol", "workspace"
        };
        const char *effort = snag_json_string(data, "default_effort");
        const char *model = snag_json_string(data, "default_model");
        const char *provider = snag_json_string(data, "default_provider");
        const char *protocol = snag_json_string(data, "protocol");
        const char *workspace = snag_json_string(data, "workspace");

        if (seq != 1 ||
            snag_json_integer_u64(data, "format", &n) < 0 ||
            n != 2u || !snag_json_exact_keys(data, keys, 6u) ||
            !protocol || strcmp(protocol, "responses") != 0 ||
            !preference_text_valid(effort, sizeof(session->default_effort)) ||
            !preference_text_valid(model, sizeof(session->default_model)) ||
            !provider || !*provider ||
            strlen(provider) > SNAG_CONFIG_PROVIDER_NAME_MAX ||
            !snag_utf8_valid((const unsigned char *)provider, strlen(provider), true) ||
            !snag_path_root_len(workspace) ||
            strlen(workspace) > SNAG_PATH_MAX_BYTES ||
            !snag_utf8_valid((const unsigned char *)workspace,
                            strlen(workspace), true))
            goto invalid;
        if (replace_text(&session->workspace, workspace,
                         SNAG_PATH_MAX_BYTES) < 0)
            return -1;
        if (!snag_strcpy(session->default_effort,
                        sizeof(session->default_effort), effort) ||
            !snag_strcpy(session->default_model,
                        sizeof(session->default_model), model) ||
            !snag_strcpy(session->default_provider,
                         sizeof(session->default_provider), provider))
            goto invalid;
    } else if (session->delete_requested) {
        goto invalid;
    } else if (strcmp(type, "irc_event") == 0) {
        static const char *const keys[] = {
            "endpoint", "historical", "kind", "local", "nick", "op",
            "room", "text", "timestamp_ms"
        };
        const char *endpoint = snag_json_string(data, "endpoint");
        const char *kind = snag_json_string(data, "kind");
        const char *room = snag_json_string(data, "room");
        const char *nick = snag_json_string(data, "nick");
        const char *text = snag_json_string(data, "text");
        json_t *historical = json_object_get(data, "historical");
        json_t *local = json_object_get(data, "local");
        json_t *op = json_object_get(data, "op");
        uint64_t timestamp_ms;

        if (!snag_json_exact_keys(data, keys, sizeof(keys) / sizeof(keys[0])) ||
            !irc_field_valid(endpoint, SNAG_CONFIG_IRC_ENDPOINT_MAX, false) ||
            !irc_kind_valid(kind) ||
            !irc_field_valid(room, SNAG_CONFIG_IRC_ROOM_MAX + 1u, true) ||
            !irc_field_valid(nick, SNAG_CONFIG_IRC_NICK_MAX, true) ||
            !irc_field_valid(text, SNAG_IRC_TEXT_MAX, true) ||
            !json_is_boolean(historical) || !json_is_boolean(local) ||
            !json_is_boolean(op) ||
            snag_json_integer_u64(data, "timestamp_ms", &timestamp_ms) < 0 ||
            timestamp_ms == 0u)
            goto invalid;
    } else if (strcmp(type, "irc_snapshot") == 0) {
        static const char *const keys[] = {"reason", "text", "timestamp_ms"};
        const char *reason = snag_json_string(data, "reason");
        const char *text = snag_json_string(data, "text");
        uint64_t timestamp_ms;

        if (!snag_json_exact_keys(data, keys, 3u) || !reason ||
            (strcmp(reason, "join") != 0 &&
             strcmp(reason, "nick") != 0 &&
             strcmp(reason, "topology") != 0 &&
             strcmp(reason, "compaction") != 0) ||
            !text || !*text || strlen(text) > SNAG_MAX_IRC_SNAPSHOT ||
            !snag_utf8_valid((const unsigned char *)text, strlen(text), true) ||
            snag_json_integer_u64(data, "timestamp_ms", &timestamp_ms) < 0 ||
            timestamp_ms == 0u)
            goto invalid;
    } else if (strcmp(type, "workspace_changed") == 0) {
        static const char *const keys[] = {"new_workspace", "old_workspace"};
        const char *old_workspace = snag_json_string(data, "old_workspace");
        const char *new_workspace = snag_json_string(data, "new_workspace");
        if (session->active_turn || session->process_count != 0u ||
            !snag_json_exact_keys(data, keys, 2u) ||
            !old_workspace || !new_workspace ||
            strcmp(old_workspace, session->workspace) != 0 ||
            strcmp(old_workspace, new_workspace) == 0 ||
            !snag_path_root_len(new_workspace) || strlen(new_workspace) > SNAG_PATH_MAX_BYTES ||
            !snag_utf8_valid((const unsigned char *)new_workspace,
                            strlen(new_workspace), true) ||
            replace_text(&session->workspace, new_workspace,
                         SNAG_PATH_MAX_BYTES) < 0)
            goto invalid;
    } else if (strcmp(type, "session_archived") == 0) {
        static const char *const keys[] = {"origin"};
        const char *origin = snag_json_string(data, "origin");
        if (!snag_json_exact_keys(data, keys, 1u) || session->active_turn ||
            session->process_count != 0u || session->archived ||
            !origin || strcmp(origin, "user") != 0)
            goto invalid;
        session->archived = true;
    } else if (strcmp(type, "session_unarchived") == 0) {
        static const char *const keys[] = {"origin"};
        const char *origin = snag_json_string(data, "origin");
        if (!snag_json_exact_keys(data, keys, 1u) || session->active_turn ||
            session->process_count != 0u || !session->archived ||
            !origin || strcmp(origin, "user") != 0)
            goto invalid;
        session->archived = false;
    } else if (strcmp(type, "session_delete_requested") == 0) {
        static const char *const keys[] = {"confirmed_id_prefix", "trash_name"};
        const char *prefix = snag_json_string(data, "confirmed_id_prefix");
        const char *trash = snag_json_string(data, "trash_name");
        if (!snag_json_exact_keys(data, keys, 2u) || session->active_turn ||
            session->process_count != 0u ||
            !prefix || strlen(prefix) != 8u ||
            !snag_hex_is_lower(prefix, 8u) ||
            memcmp(prefix, session->id, 8u) != 0 ||
            !valid_trash_name(session, trash))
            goto invalid;
        memcpy(session->trash_name, trash, SNAG_TRASH_NAME_LEN + 1u);
        session->delete_requested = true;
    } else if (strcmp(type, "goal_started") == 0) {
        static const char *const keys[] = {"goal_id", "prompt"};
        const char *goal_id = snag_json_string(data, "goal_id");
        const char *prompt = snag_json_string(data, "prompt");
        size_t len;
        if (!snag_json_exact_keys(data, keys, 2u) || snag_goal_unfinished(session->goal_status) ||
            !goal_id || !snag_hex_is_lower(goal_id, SNAG_ID_HEX_LEN) ||
            strcmp(goal_id, session->id) == 0 || !prompt || !*prompt ||
            (session->goal_id[0] && strcmp(goal_id, session->goal_id) == 0) ||
            snag_text_blank(prompt) ||
            (len = strlen(prompt)) > SNAG_MAX_GOAL_PROMPT ||
            !snag_utf8_valid((const unsigned char *)prompt, len, true) ||
            replace_text(&session->goal_prompt, prompt,
                         SNAG_MAX_GOAL_PROMPT) < 0)
            goto invalid;
        if ((!session->first_user &&
             replace_text(&session->first_user, prompt,
                          SNAG_MAX_GOAL_PROMPT) < 0) ||
            replace_text(&session->last_user, prompt,
                         SNAG_MAX_GOAL_PROMPT) < 0)
            return -1;
        free(session->goal_blocker);
        session->goal_blocker = NULL;
        memcpy(session->goal_id, goal_id, sizeof(session->goal_id));
        session->goal_status = SNAG_GOAL_ACTIVE;
        session->goal_locked = false;
        session->goal_revision = 1u;
        session->goal_turn_count = 0u;
    } else if (strcmp(type, "goal_reworded") == 0) {
        static const char *const keys[] = {"actor", "goal_id", "prompt"};
        static const char *const actors[] = {"model", "user"};
        const char *actor = snag_json_string(data, "actor");
        const char *goal_id = snag_json_string(data, "goal_id");
        const char *prompt = snag_json_string(data, "prompt");
        size_t len;
        if (!snag_json_exact_keys(data, keys, 3u) || !snag_goal_unfinished(session->goal_status) ||
            !goal_id || strcmp(goal_id, session->goal_id) != 0 ||
            !string_in(actor, actors, sizeof(actors) / sizeof(actors[0])) ||
            (strcmp(actor, "model") == 0 &&
             session->goal_status != SNAG_GOAL_ACTIVE) ||
            (strcmp(actor, "model") == 0 && session->goal_locked) ||
            !prompt || !*prompt || snag_text_blank(prompt) ||
            (len = strlen(prompt)) > SNAG_MAX_GOAL_PROMPT ||
            !snag_utf8_valid((const unsigned char *)prompt, len, true) ||
            strcmp(prompt, session->goal_prompt) == 0 ||
            replace_text(&session->goal_prompt, prompt,
                         SNAG_MAX_GOAL_PROMPT) < 0)
            goto invalid;
        ++session->goal_revision;
        if (strcmp(actor, "user") == 0 &&
            replace_text(&session->last_user, prompt,
                         SNAG_MAX_GOAL_PROMPT) < 0)
            return -1;
    } else if (strcmp(type, "goal_lock_changed") == 0) {
        static const char *const keys[] = {"goal_id", "locked"};
        const char *goal_id = snag_json_string(data, "goal_id");
        json_t *locked = json_object_get(data, "locked");
        if (!snag_json_exact_keys(data, keys, 2u) || !snag_goal_unfinished(session->goal_status) ||
            !goal_id || strcmp(goal_id, session->goal_id) != 0 ||
            !json_is_boolean(locked) ||
            (json_is_true(locked) == session->goal_locked))
            goto invalid;
        session->goal_locked = json_is_true(locked);
    } else if (strcmp(type, "goal_paused") == 0) {
        static const char *const keys[] = {"goal_id", "reason"};
        static const char *const reasons[] = {
            "input_closed", "refusal", "session_resumed", "turn_stopped", "user"
        };
        const char *goal_id = snag_json_string(data, "goal_id");
        const char *reason = snag_json_string(data, "reason");
        if (!snag_json_exact_keys(data, keys, 2u) ||
            session->goal_status != SNAG_GOAL_ACTIVE ||
            !goal_id || strcmp(goal_id, session->goal_id) != 0 ||
            !string_in(reason, reasons, sizeof(reasons) / sizeof(reasons[0])))
            goto invalid;
        session->goal_status = SNAG_GOAL_PAUSED;
    } else if (strcmp(type, "goal_resumed") == 0) {
        static const char *const keys[] = {"goal_id"};
        const char *goal_id = snag_json_string(data, "goal_id");
        if (!snag_json_exact_keys(data, keys, 1u) ||
            (session->goal_status != SNAG_GOAL_PAUSED &&
             session->goal_status != SNAG_GOAL_BLOCKED) ||
            !goal_id || strcmp(goal_id, session->goal_id) != 0)
            goto invalid;
        free(session->goal_blocker);
        session->goal_blocker = NULL;
        session->goal_status = SNAG_GOAL_ACTIVE;
    } else if (strcmp(type, "goal_blocked") == 0) {
        static const char *const keys[] = {"actor", "goal_id", "reason"};
        const char *actor = snag_json_string(data, "actor");
        const char *goal_id = snag_json_string(data, "goal_id");
        const char *reason = snag_json_string(data, "reason");
        size_t len;
        if (!snag_json_exact_keys(data, keys, 3u) ||
            session->goal_status != SNAG_GOAL_ACTIVE ||
            !actor || strcmp(actor, "model") != 0 ||
            !goal_id || strcmp(goal_id, session->goal_id) != 0 ||
            !reason || !*reason || snag_text_blank(reason) ||
            (len = strlen(reason)) > SNAG_MAX_GOAL_BLOCKER ||
            !snag_utf8_valid((const unsigned char *)reason, len, true) ||
            replace_text(&session->goal_blocker, reason,
                         SNAG_MAX_GOAL_BLOCKER) < 0)
            goto invalid;
        session->goal_status = SNAG_GOAL_BLOCKED;
    } else if (strcmp(type, "goal_completed") == 0) {
        static const char *const keys[] = {"actor", "goal_id"};
        static const char *const actors[] = {"model", "user"};
        const char *actor = snag_json_string(data, "actor");
        const char *goal_id = snag_json_string(data, "goal_id");
        if (!snag_json_exact_keys(data, keys, 2u) || !snag_goal_unfinished(session->goal_status) ||
            !goal_id || strcmp(goal_id, session->goal_id) != 0 ||
            !string_in(actor, actors, sizeof(actors) / sizeof(actors[0])) ||
            (strcmp(actor, "model") == 0 &&
             (session->goal_status != SNAG_GOAL_ACTIVE || session->process_count)))
            goto invalid;
        free(session->goal_blocker);
        session->goal_blocker = NULL;
        session->goal_status = SNAG_GOAL_COMPLETED;
    } else if (strcmp(type, "goal_cancelled") == 0) {
        static const char *const keys[] = {"goal_id"};
        const char *goal_id = snag_json_string(data, "goal_id");
        if (!snag_json_exact_keys(data, keys, 1u) || !snag_goal_unfinished(session->goal_status) ||
            !goal_id || strcmp(goal_id, session->goal_id) != 0)
            goto invalid;
        free(session->goal_blocker);
        session->goal_blocker = NULL;
        session->goal_status = SNAG_GOAL_CANCELLED;
    } else if (strcmp(type, "compaction_started") == 0) {
        static const char *const keys[] = {
            "capability_version", "compact_id", "count_method",
            "count_request_sha256", "input_tokens_bound", "model",
            "predecessor_compact_id", "profile_id", "reason",
            "request_sha256", "source_seq", "source_sha256"
        };
        static const char *const methods[] = {
            "exact", "anchored_upper_bound", "statistical_upper_estimate", "qualified_upper_bound"
        };
        static const char *const reasons[] = {
            "manual", "proactive", "hard_budget", "provider_rejection"
        };
        const char *compact_id = snag_json_string(data, "compact_id");
        const char *predecessor = snag_json_string(data, "predecessor_compact_id");
        const char *reason = snag_json_string(data, "reason");
        const char *method = snag_json_string(data, "count_method");
        const char *source_hash = snag_json_string(data, "source_sha256");
        const char *request_hash = snag_json_string(data, "request_sha256");
        const char *count_hash = snag_json_string(data, "count_request_sha256");
        const char *model = snag_json_string(data, "model");
        const char *profile = snag_json_string(data, "profile_id");
        const char *capability = snag_json_string(data, "capability_version");
        uint64_t source_seq;
        uint64_t tokens;
        bool active_prefix;
        const char *expected_model;

        active_prefix = reason && strcmp(reason, "manual") != 0 &&
            session->active_turn && !session->response_open;
        expected_model = active_prefix ? session->active_turn_model :
                                         session->default_model;
        if (!snag_json_exact_keys(data, keys, sizeof(keys) / sizeof(keys[0])) ||
            (session->active_turn && !active_prefix) || session->response_open ||
            session->pending_call_count || (!active_prefix && session->process_count) ||
            session->active_compact_id[0] != '\0' ||
            !compact_id || !snag_hex_is_lower(compact_id, SNAG_ID_HEX_LEN) ||
            strcmp(compact_id, session->id) == 0 ||
            !string_in(method, methods, sizeof(methods) / sizeof(methods[0])) ||
            !string_in(reason, reasons, sizeof(reasons) / sizeof(reasons[0])) ||
            !source_hash || !snag_hex_is_lower(source_hash, SNAG_SHA256_HEX_LEN) ||
            !request_hash || !snag_hex_is_lower(request_hash, SNAG_SHA256_HEX_LEN) ||
            !count_hash || !snag_hex_is_lower(count_hash, SNAG_SHA256_HEX_LEN) ||
            !model || strcmp(model, expected_model) != 0 ||
            !profile || strcmp(profile, SNAJPAGENT_PROFILE_ID) != 0 ||
            !capability ||
            strcmp(capability, SNAJPAGENT_CAPABILITY_VERSION) != 0 ||
            snag_json_integer_u64(data, "source_seq", &source_seq) < 0 ||
            source_seq == 0u || source_seq >= seq || source_seq <= session->compact_seq ||
            snag_json_integer_u64(data, "input_tokens_bound", &tokens) < 0 ||
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
    } else if (strcmp(type, "compaction_interrupted") == 0) {
        static const char *const keys[] = {"compact_id", "reason"};
        static const char *const reasons[] = {"steering", "user", "endpoint_unavailable"};
        const char *compact_id = snag_json_string(data, "compact_id");
        const char *reason = snag_json_string(data, "reason");

        if (!snag_json_exact_keys(data, keys, 2u) ||
            session->active_compact_id[0] == '\0' || !compact_id ||
            strcmp(compact_id, session->active_compact_id) != 0 ||
            !string_in(reason, reasons,
                       sizeof(reasons) / sizeof(reasons[0])))
            goto invalid;
        session->active_compact_id[0] = '\0';
        session->active_compact_source_sha256[0] = '\0';
        session->active_compact_source_seq = 0u;
    } else if (strcmp(type, "compaction_completed") == 0) {
        static const char *const keys[] = {
            "compact_id", "count_method", "input_tokens_bound", "output",
            "output_count_method", "output_count_request_sha256",
            "output_sha256", "output_tokens_bound", "source_sha256"
        };
        static const char *const methods[] = {
            "exact", "statistical_upper_estimate", "qualified_upper_bound"
        };
        const char *compact_id = snag_json_string(data, "compact_id");
        const char *method = snag_json_string(data, "count_method");
        const char *output_method = snag_json_string(data, "output_count_method");
        const char *output_count_hash =
            snag_json_string(data, "output_count_request_sha256");
        const char *source_hash = snag_json_string(data, "source_sha256");
        const char *output_hash = snag_json_string(data, "output_sha256");
        json_t *output = json_object_get(data, "output");
        char computed[SNAG_SHA256_HEX_LEN + 1u];
        size_t bytes = 0u;
        uint64_t in_tokens;
        uint64_t out_tokens;

        if (!snag_json_exact_keys(data, keys, sizeof(keys) / sizeof(keys[0])) ||
            session->active_compact_id[0] == '\0' ||
            !compact_id || strcmp(compact_id, session->active_compact_id) != 0 ||
            !string_in(method, methods, sizeof(methods) / sizeof(methods[0])) ||
            !string_in(output_method, methods, sizeof(methods) / sizeof(methods[0])) ||
            !output_count_hash ||
            !snag_hex_is_lower(output_count_hash, SNAG_SHA256_HEX_LEN) ||
            !source_hash || strcmp(source_hash, session->active_compact_source_sha256) != 0 ||
            !output_hash || !snag_hex_is_lower(output_hash, SNAG_SHA256_HEX_LEN) ||
            snag_json_integer_u64(data, "input_tokens_bound", &in_tokens) < 0 ||
            snag_json_integer_u64(data, "output_tokens_bound", &out_tokens) < 0 ||
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
    } else if (strcmp(type, "model_selection_changed") == 0) {
        static const char *const keys[] = {
            "new_effort", "new_model", "new_provider",
            "old_effort", "old_model", "old_provider"
        };
        const char *old_provider = snag_json_string(data, "old_provider");
        const char *new_provider = snag_json_string(data, "new_provider");
        const char *old_model = snag_json_string(data, "old_model");
        const char *new_model = snag_json_string(data, "new_model");
        const char *old_effort = snag_json_string(data, "old_effort");
        const char *new_effort = snag_json_string(data, "new_effort");
        if (session->active_turn || !snag_json_exact_keys(data, keys, 6u) ||
            !old_provider || !new_provider || !*new_provider ||
            strcmp(old_provider, session->default_provider) != 0 ||
            strlen(new_provider) > SNAG_CONFIG_PROVIDER_NAME_MAX ||
            !snag_utf8_valid((const unsigned char *)new_provider,
                            strlen(new_provider), true) ||
            !old_model || strcmp(old_model, session->default_model) != 0 ||
            !preference_text_valid(new_model, sizeof(session->default_model)) ||
            !old_effort || strcmp(old_effort, session->default_effort) != 0 ||
            !preference_text_valid(new_effort, sizeof(session->default_effort)) ||
            (strcmp(old_provider, new_provider) == 0 &&
             strcmp(old_model, new_model) == 0 &&
             strcmp(old_effort, new_effort) == 0) ||
            !snag_strcpy(session->default_provider,
                        sizeof(session->default_provider), new_provider) ||
            !snag_strcpy(session->default_model,
                        sizeof(session->default_model), new_model) ||
            !snag_strcpy(session->default_effort,
                        sizeof(session->default_effort), new_effort))
            goto invalid;
    } else if (strcmp(type, "effort_changed") == 0) {
        static const char *const keys[] = {"new_effort", "old_effort"};
        const char *old_effort = snag_json_string(data, "old_effort");
        const char *new_effort = snag_json_string(data, "new_effort");
        if (session->active_turn || !snag_json_exact_keys(data, keys, 2u) ||
            !preference_text_valid(old_effort, sizeof(session->default_effort)) ||
            !preference_text_valid(new_effort, sizeof(session->default_effort)) ||
            strcmp(old_effort, session->default_effort) != 0 ||
            strcmp(old_effort, new_effort) == 0 ||
            !snag_strcpy(session->default_effort,
                        sizeof(session->default_effort), new_effort))
            goto invalid;
    } else if (strcmp(type, "steering_added") == 0 ||
               strcmp(type, "irc_reply_reminder") == 0) {
        static const char *const keys[] = {"steering_id", "text", "turn_id"};
        const char *steering_id = snag_json_string(data, "steering_id");
        const char *text = snag_json_string(data, "text");
        const char *turn_id = snag_json_string(data, "turn_id");
        size_t len;
        bool reminder = strcmp(type, "irc_reply_reminder") == 0;

        if (!snag_json_exact_keys(data, keys, 3u) || !session->active_turn ||
            session->response_terminal == SNAG_RESPONSE_TERMINAL_FAILED ||
            session->response_terminal == SNAG_RESPONSE_TERMINAL_INTERRUPTED ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !steering_id || !snag_hex_is_lower(steering_id, SNAG_ID_HEX_LEN) ||
            pending_user_id_exists(session, steering_id) || !text || !*text ||
            (len = strlen(text)) > SNAG_MAX_STEERING_TEXT ||
            session->pending_steering_count >= SNAG_MAX_STEERING_PER_TURN ||
            session->pending_steering_bytes >
                SNAG_MAX_STEERING_PER_TURN * SNAG_MAX_STEERING_TEXT - len ||
            (reminder && (!session->response_complete ||
                          (session->response_outcome != SNAG_GRAPH_NONPRODUCTIVE &&
                           session->response_outcome != SNAG_GRAPH_FINAL &&
                           session->response_outcome != SNAG_GRAPH_REFUSAL) ||
                          session->irc_reply_reminded ||
                          session->pending_steering_count != 0u ||
                          strcmp(text, SNAG_IRC_REPLY_REMINDER_TEXT) != 0)))
            goto invalid;
        if (add_pending_steering(session, steering_id, text, len, seq) < 0)
            return -1;
        if (reminder)
            session->irc_reply_reminded = true;
    } else if (strcmp(type, "future_turn_queued") == 0) {
        static const char *const keys[] = {"queue_id", "read_only", "text", "while_turn_id"};
        const char *queue_id = snag_json_string(data, "queue_id");
        const char *text = snag_json_string(data, "text");
        const char *turn_id = snag_json_string(data, "while_turn_id");
        size_t len;
        char *copy;
        struct snag_queued_turn *queued;

        if (!snag_json_exact_keys(data, keys, 4u) || !session->active_turn ||
            !json_is_boolean(json_object_get(data, "read_only")) ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !queue_id || !snag_hex_is_lower(queue_id, SNAG_ID_HEX_LEN) ||
            pending_user_id_exists(session, queue_id) || !text || !*text ||
            (len = strlen(text)) > SNAG_MAX_QUEUED_TEXT ||
            session->pending_queue_count >= SNAG_MAX_PENDING_TURNS ||
            session->pending_queue_bytes > SNAG_MAX_PENDING_QUEUE_TEXT - len)
            goto invalid;
        copy = snag_strdup_checked(text, SNAG_MAX_QUEUED_TEXT);
        if (!copy)
            return -1;
        queued = &session->pending_queue[session->pending_queue_count++];
        memset(queued, 0, sizeof(*queued));
        memcpy(queued->queue_id, queue_id, sizeof(queued->queue_id));
        queued->seq = seq;
        queued->text = copy;
        queued->read_only = json_is_true(json_object_get(data, "read_only"));
        session->pending_queue_bytes += len;
    } else if (strcmp(type, "future_turn_edited") == 0) {
        static const char *const keys[] = {"queue_id", "read_only", "text"};
        const char *queue_id = snag_json_string(data, "queue_id");
        const char *text = snag_json_string(data, "text");
        struct snag_queued_turn *queued = NULL;
        size_t old_len;
        size_t len;
        char *copy;

        if (!snag_json_exact_keys(data, keys, 3u) || !queue_id || !text || !*text ||
            !json_is_boolean(json_object_get(data, "read_only")) ||
            !snag_hex_is_lower(queue_id, SNAG_ID_HEX_LEN) ||
            (len = strlen(text)) > SNAG_MAX_QUEUED_TEXT)
            goto invalid;
        for (size_t i = 0; i < session->pending_queue_count; ++i) {
            if (strcmp(session->pending_queue[i].queue_id, queue_id) == 0) {
                queued = &session->pending_queue[i];
                break;
            }
        }
        if (!queued || (strcmp(queued->text, text) == 0 &&
            queued->read_only == json_is_true(json_object_get(data, "read_only"))))
            goto invalid;
        old_len = strlen(queued->text);
        if (len > old_len &&
            session->pending_queue_bytes >
                SNAG_MAX_PENDING_QUEUE_TEXT - (len - old_len))
            goto invalid;
        copy = snag_strdup_checked(text, SNAG_MAX_QUEUED_TEXT);
        if (!copy)
            return -1;
        free(queued->text);
        queued->text = copy;
        queued->read_only = json_is_true(json_object_get(data, "read_only"));
        session->pending_queue_bytes =
            session->pending_queue_bytes - old_len + len;
    } else if (strcmp(type, "future_turn_cancelled") == 0) {
        static const char *const keys[] = {"queue_ids", "reason"};
        const char *reason = snag_json_string(data, "reason");
        json_t *ids = json_object_get(data, "queue_ids");
        bool remove[SNAG_MAX_PENDING_TURNS] = {false};
        size_t previous = 0;
        bool have_previous = false;
        size_t count;

        if (!snag_json_exact_keys(data, keys, 2u) || !reason ||
            strcmp(reason, "user") != 0 || !json_is_array(ids) ||
            !(count = json_array_size(ids)) || count > SNAG_MAX_PENDING_TURNS)
            goto invalid;
        for (size_t i = 0; i < count; ++i) {
            json_t *value = json_array_get(ids, i);
            const char *id = json_is_string(value) ? json_string_value(value) : NULL;
            size_t index;

            if (!id || !snag_hex_is_lower(id, SNAG_ID_HEX_LEN))
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
            "read_only", "text", "turn_id", "turn_number", "workspace"
        };
        const char *turn_id;
        const char *text;
        const char *workspace;
        const char *kind;
        const char *model;
        const char *provider;
        const char *effort;
        json_t *config = json_object_get(data, "config");
        bool queued;
        bool goal;
        uint64_t queue_seq = 0;
        uint64_t max_parallel;

        if (session->active_turn || session->process_count != 0u ||
            session->pending_steering_count != 0u ||
            !snag_json_exact_keys(data, keys, 10u) ||
            !json_is_boolean(json_object_get(data, "read_only")) ||
            !(turn_id = snag_json_string(data, "turn_id")) ||
            !snag_hex_is_lower(turn_id, SNAG_ID_HEX_LEN) ||
            snag_json_integer_u64(data, "turn_number", &n) < 0 ||
            n != session->turn_count + 1u ||
            !(kind = snag_json_string(data, "input_kind")) ||
            !json_is_object(config) ||
            snag_json_integer_u64(config, "max_parallel_commands", &max_parallel) < 0 ||
            max_parallel < 1u || max_parallel > SNAG_MAX_PROCESSES ||
            !json_is_boolean(json_object_get(config, "parallel_tool_calls")) ||
            !(model = snag_json_string(config, "model")) || !*model ||
            !(provider = snag_json_string(config, "provider")) || !*provider ||
            strlen(provider) > SNAG_CONFIG_PROVIDER_NAME_MAX ||
            !(effort = snag_json_string(config, "effort")) || !*effort ||
            strlen(effort) >= sizeof(session->active_turn_effort) ||
            strlen(model) >= sizeof(session->active_turn_model) ||
            !snag_utf8_valid((const unsigned char *)model, strlen(model), true) ||
            snag_instructions_metadata_valid(json_object_get(data, "instructions"),
                                            error, error_size) < 0 ||
            !(workspace = snag_json_string(data, "workspace")) ||
            strcmp(workspace, session->workspace) != 0 ||
            !(text = snag_json_string(data, "text")) || !*text)
            goto invalid;
        queued = strcmp(kind, "queued") == 0;
        goal = strcmp(kind, "goal") == 0;
        if (!queued && !goal && strcmp(kind, "direct") != 0)
            goto invalid;
        if (goal) {
            if (session->goal_status != SNAG_GOAL_ACTIVE ||
                json_is_true(json_object_get(data, "read_only")) ||
                !json_is_null(json_object_get(data, "queue_id")) ||
                !json_is_null(json_object_get(data, "queue_seq")) ||
                strcmp(text, SNAG_GOAL_CONTINUATION_TEXT) != 0)
                goto invalid;
        } else if (!queued) {
            if (!json_is_null(json_object_get(data, "queue_id")) ||
                !json_is_null(json_object_get(data, "queue_seq")) ||
                strlen(text) > SNAG_MAX_DIRECT_PROMPT)
                goto invalid;
        } else {
            const char *queue_id = snag_json_string(data, "queue_id");
            if (session->pending_queue_count == 0u ||
                !session->pending_queue[0].text || !queue_id ||
                snag_json_integer_u64(data, "queue_seq", &queue_seq) < 0 ||
                strcmp(queue_id, session->pending_queue[0].queue_id) != 0 ||
                queue_seq != session->pending_queue[0].seq ||
                strcmp(text, session->pending_queue[0].text) != 0 ||
                session->pending_queue[0].read_only !=
                    json_is_true(json_object_get(data, "read_only")) ||
                strlen(text) > SNAG_MAX_QUEUED_TEXT)
                goto invalid;
        }
        memcpy(session->active_turn_id, turn_id, sizeof(session->active_turn_id));
        if (!snag_strcpy(session->active_turn_model,
                        sizeof(session->active_turn_model), model) ||
            !snag_strcpy(session->active_turn_provider,
                        sizeof(session->active_turn_provider), provider) ||
            !snag_strcpy(session->active_turn_effort,
                        sizeof(session->active_turn_effort), effort))
            goto invalid;
        session->active_turn = true;
        session->last_turn_failed = false;
        session->max_parallel_commands = (uint32_t)max_parallel;
        session->parallel_tool_calls = json_is_true(json_object_get(config, "parallel_tool_calls"));
        session->active_read_only = json_is_true(json_object_get(data, "read_only"));
        session->active_queued = queued;
        session->turn_count = n;
        if (goal)
            ++session->goal_turn_count;
        session->active_cycle = 0;
        session->irc_reply_reminded = false;
        session->output_correction_used = false;
        session->cyber_clarifications = 0u;
        clear_response_state(session);
        if (!goal && ((!session->first_user &&
             replace_text(&session->first_user, text, SNAG_MAX_DIRECT_PROMPT) < 0) ||
            replace_text(&session->last_user, text, SNAG_MAX_DIRECT_PROMPT) < 0))
            return -1;
        if (queued && consume_oldest_queue(session) < 0)
            goto invalid;
    } else if (strcmp(type, "response_started") == 0) {
        static const char *const keys[] = {
            "baseline_sha256", "capability_version", "compact_id",
            "capacity_source", "count_method", "count_request_sha256", "cycle",
            "effort", "hard_input_tokens", "input_tokens_bound", "model",
            "model_input_bytes", "model_input_sha256", "profile_id", "provider",
            "provider_source_sha256", "request_input_bytes",
            "request_input_count", "request_input_sha256", "request_sha256",
            "requested_output_tokens", "response_id", "source_bound",
            "steering_ids", "turn_id"
        };
        const char *response_id = snag_json_string(data, "response_id");
        const char *turn_id = snag_json_string(data, "turn_id");
        const char *method = snag_json_string(data, "count_method");
        const char *compact_id = snag_json_string(data, "compact_id");
        const char *capability = snag_json_string(data, "capability_version");
        const char *model = snag_json_string(data, "model");
        const char *provider = snag_json_string(data, "provider");
        const char *provider_source_sha256 =
            snag_json_string(data, "provider_source_sha256");
        const char *effort = snag_json_string(data, "effort");
        const char *capacity_source = snag_json_string(data, "capacity_source");
        const char *profile = snag_json_string(data, "profile_id");
        const char *input_hash = snag_json_string(data, "model_input_sha256");
        const char *request_input_hash =
            snag_json_string(data, "request_input_sha256");
        const char *request_hash = snag_json_string(data, "request_sha256");
        const char *count_hash = snag_json_string(data, "count_request_sha256");
        json_t *steering_ids = json_object_get(data, "steering_ids");
        uint64_t cycle;
        uint64_t token_bound;
        uint64_t model_input_bytes;
        uint64_t request_input_bytes;
        uint64_t request_input_count;
        uint64_t requested_output_tokens = 0u;
        bool requested_output_known =
            !json_is_null(json_object_get(data, "requested_output_tokens"));
        bool state_allows_start;

        state_allows_start = !session->response_open &&
            session->response_terminal != SNAG_RESPONSE_TERMINAL_FAILED &&
            session->response_terminal != SNAG_RESPONSE_TERMINAL_INTERRUPTED &&
            (session->response_terminal != SNAG_RESPONSE_TERMINAL_STEERED ||
             session->pending_steering_count != 0u) &&
            (!session->response_complete ||
             (session->pending_steering_count != 0u &&
              all_pending_finished(session) &&
              session->response_outcome != SNAG_GRAPH_CONFLICT));
        if (!snag_json_exact_keys(data, keys, 25u) || !session->active_turn ||
            !state_allows_start || !response_id ||
            !snag_hex_is_lower(response_id, SNAG_ID_HEX_LEN) || !turn_id ||
            strcmp(turn_id, session->active_turn_id) != 0 || !method ||
            (strcmp(method, "exact") != 0 &&
             strcmp(method, "anchored_upper_bound") != 0 &&
             strcmp(method, "statistical_upper_estimate") != 0 &&
             strcmp(method, "qualified_upper_bound") != 0) ||
            !capability || strcmp(capability, SNAJPAGENT_CAPABILITY_VERSION) != 0 ||
            !model || strcmp(model, session->active_turn_model) != 0 ||
            !provider || strcmp(provider, session->active_turn_provider) != 0 ||
            !provider_source_sha256 ||
            !snag_hex_is_lower(provider_source_sha256,
                              SNAG_SHA256_HEX_LEN) ||
            !effort || strcmp(effort, session->active_turn_effort) != 0 ||
            (!capacity_source ||
             (strcmp(capacity_source, "unknown") != 0 &&
              strcmp(capacity_source, "advertised") != 0 &&
              strcmp(capacity_source, "configured") != 0 &&
              strcmp(capacity_source, "observed") != 0 &&
              strcmp(capacity_source, "stale-catalog-ignored") != 0)) ||
            (!json_is_true(json_object_get(data, "source_bound")) &&
             !json_is_false(json_object_get(data, "source_bound"))) ||
            (!json_is_null(json_object_get(data, "hard_input_tokens")) &&
             snag_json_integer_u64(data, "hard_input_tokens", &n) < 0) ||
            (requested_output_known &&
             (snag_json_integer_u64(data, "requested_output_tokens",
                                   &requested_output_tokens) < 0 ||
              requested_output_tokens == 0u ||
              requested_output_tokens > SNAG_CONFIG_TOKEN_LIMIT_MAX)) ||
            !profile || strcmp(profile, SNAJPAGENT_PROFILE_ID) != 0 ||
            (strcmp(method, "anchored_upper_bound") == 0 ?
                (!session->usage_anchor_valid ||
                 !snag_json_string(data, "baseline_sha256") ||
                 !snag_hex_is_lower(snag_json_string(data, "baseline_sha256"),
                                   SNAG_SHA256_HEX_LEN) ||
                 strcmp(snag_json_string(data, "baseline_sha256"),
                        session->usage_anchor_model_input_sha256) != 0 ||
                 strcmp(session->usage_anchor_provider,
                        session->active_turn_provider) != 0 ||
                 strcmp(session->usage_anchor_model,
                        session->active_turn_model) != 0 ||
                 strcmp(session->usage_anchor_effort,
                        session->active_turn_effort) != 0 ||
                 strcmp(session->usage_anchor_provider_source_sha256,
                        provider_source_sha256) != 0 ||
                 strcmp(session->usage_anchor_compact_id,
                        session->compact_id) != 0) :
                !json_is_null(json_object_get(data, "baseline_sha256"))) ||
            (session->compact_id[0] == '\0' ?
             !json_is_null(json_object_get(data, "compact_id")) :
             (!compact_id || strcmp(compact_id, session->compact_id) != 0)) ||
            !json_is_array(steering_ids) ||
            json_array_size(steering_ids) != session->pending_steering_count ||
            !input_hash || !snag_hex_is_lower(input_hash, SNAG_SHA256_HEX_LEN) ||
            !request_input_hash ||
            !snag_hex_is_lower(request_input_hash, SNAG_SHA256_HEX_LEN) ||
            !request_hash || !snag_hex_is_lower(request_hash, SNAG_SHA256_HEX_LEN) ||
            !count_hash || !snag_hex_is_lower(count_hash, SNAG_SHA256_HEX_LEN) ||
            snag_json_integer_u64(data, "input_tokens_bound", &token_bound) < 0 ||
            (strcmp(method, "anchored_upper_bound") == 0 &&
             token_bound < session->usage_anchor_input_tokens) ||
            snag_json_integer_u64(data, "model_input_bytes",
                                 &model_input_bytes) < 0 ||
            model_input_bytes == 0u ||
            snag_json_integer_u64(data, "request_input_bytes",
                                 &request_input_bytes) < 0 ||
            request_input_bytes == 0u ||
            snag_json_integer_u64(data, "request_input_count",
                                 &request_input_count) < 0 ||
            request_input_count > SNAG_EVENT_LIMIT ||
            (strcmp(method, "anchored_upper_bound") == 0 &&
             (request_input_bytes <
                  session->usage_anchor_request_input_bytes ||
              request_input_count <
                  session->usage_anchor_request_input_count)) ||
            snag_json_integer_u64(data, "cycle", &cycle) < 0 ||
            cycle != (uint64_t)session->active_cycle + 1u ||
            cycle > UINT_MAX)
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
        memcpy(session->active_response_model_input_sha256, input_hash,
               sizeof(session->active_response_model_input_sha256));
        memcpy(session->active_response_request_input_sha256,
               request_input_hash,
               sizeof(session->active_response_request_input_sha256));
        memcpy(session->active_response_request_sha256, request_hash,
               sizeof(session->active_response_request_sha256));
        memcpy(session->active_response_provider_source_sha256,
               provider_source_sha256,
               sizeof(session->active_response_provider_source_sha256));
        session->active_response_model_input_bytes = model_input_bytes;
        session->active_response_request_input_bytes = request_input_bytes;
        session->active_response_request_input_count = request_input_count;
        session->active_response_requested_output_tokens =
            requested_output_tokens;
        session->active_response_requested_output_known =
            requested_output_known;
        memcpy(session->context_meter_provider,
               session->active_turn_provider,
               sizeof(session->context_meter_provider));
        memcpy(session->context_meter_model, session->active_turn_model,
               sizeof(session->context_meter_model));
        memcpy(session->context_meter_effort, session->active_turn_effort,
               sizeof(session->context_meter_effort));
        memcpy(session->context_meter_compact_id, session->compact_id,
               sizeof(session->context_meter_compact_id));
        memcpy(session->context_meter_provider_source_sha256,
               provider_source_sha256,
               sizeof(session->context_meter_provider_source_sha256));
        session->context_meter_input_tokens = token_bound;
        session->context_meter_valid = true;
        session->active_cycle = (unsigned int)cycle;
        session->response_open = true;
    } else if (strcmp(type, "response_capacity_rejected") == 0) {
        static const char *const keys[] = {
            "code", "context_limit_tokens", "cycle", "message",
            "observed_hard_input_tokens", "provider_source_sha256",
            "request_sha256", "requested_input_tokens", "response_id",
            "turn_id"
        };
        const char *code = snag_json_string(data, "code");
        const char *message = snag_json_string(data, "message");
        const char *provider_source_sha256 =
            snag_json_string(data, "provider_source_sha256");
        const char *request_hash = snag_json_string(data, "request_sha256");
        const char *response_id = snag_json_string(data, "response_id");
        const char *turn_id = snag_json_string(data, "turn_id");
        json_t *context_limit =
            json_object_get(data, "context_limit_tokens");
        json_t *requested_input =
            json_object_get(data, "requested_input_tokens");
        json_t *observed_ceiling =
            json_object_get(data, "observed_hard_input_tokens");
        uint64_t context_limit_tokens = 0u;
        uint64_t requested_input_tokens = 0u;
        uint64_t expected_ceiling = 0u;
        uint64_t recorded_ceiling = 0u;
        uint64_t cycle;
        bool expected_ceiling_known;
        bool same_binding;

        if (!snag_json_exact_keys(data, keys, 10u) || !session->response_open ||
            !code || strcmp(code, "context_length_exceeded") != 0 ||
            !message || strlen(message) > 255u ||
            !provider_source_sha256 ||
            !snag_hex_is_lower(provider_source_sha256,
                              SNAG_SHA256_HEX_LEN) ||
            !request_hash || !snag_hex_is_lower(request_hash,
                                                SNAG_SHA256_HEX_LEN) ||
            strcmp(request_hash,
                   session->active_response_request_sha256) != 0 ||
            !response_id || strcmp(response_id,
                                    session->active_response_id) != 0 ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            (!json_is_null(context_limit) &&
             (snag_json_integer_u64(data, "context_limit_tokens",
                                   &context_limit_tokens) < 0 ||
              context_limit_tokens == 0u ||
              context_limit_tokens >
                  SNAG_CONFIG_TOKEN_LIMIT_MAX)) ||
            (!json_is_null(requested_input) &&
             (snag_json_integer_u64(data, "requested_input_tokens",
                                   &requested_input_tokens) < 0 ||
              requested_input_tokens == 0u ||
              requested_input_tokens >
                  SNAG_CONFIG_TOKEN_LIMIT_MAX)) ||
            (!json_is_null(observed_ceiling) &&
             (snag_json_integer_u64(data, "observed_hard_input_tokens",
                                   &recorded_ceiling) < 0 ||
              recorded_ceiling == 0u ||
              recorded_ceiling > SNAG_CONFIG_TOKEN_LIMIT_MAX)) ||
            snag_json_integer_u64(data, "cycle", &cycle) < 0 ||
            cycle != session->active_cycle)
            goto invalid;
        expected_ceiling = snag_capacity_safety_ceiling(
            context_limit_tokens, requested_input_tokens,
            session->active_response_requested_output_known ?
                session->active_response_requested_output_tokens : 0u);
        expected_ceiling_known = expected_ceiling != 0u;
        if (expected_ceiling_known != !json_is_null(observed_ceiling) ||
            (expected_ceiling_known && expected_ceiling != recorded_ceiling))
            goto invalid;
        same_binding = session->capacity_ceiling_valid &&
            strcmp(session->capacity_ceiling_provider,
                   session->active_turn_provider) == 0 &&
            strcmp(session->capacity_ceiling_model,
                   session->active_turn_model) == 0 &&
            strcmp(session->capacity_ceiling_source_sha256,
                   provider_source_sha256) == 0;
        if (expected_ceiling_known &&
            (!same_binding || expected_ceiling <
                              session->capacity_ceiling_input_tokens)) {
            if (!snag_strcpy(session->capacity_ceiling_provider,
                            sizeof(session->capacity_ceiling_provider),
                            session->active_turn_provider) ||
                !snag_strcpy(session->capacity_ceiling_model,
                            sizeof(session->capacity_ceiling_model),
                            session->active_turn_model) ||
                !snag_strcpy(session->capacity_ceiling_source_sha256,
                            sizeof(session->capacity_ceiling_source_sha256),
                            provider_source_sha256))
                goto invalid;
            session->capacity_ceiling_input_tokens = expected_ceiling;
            session->capacity_ceiling_valid = true;
        }
        clear_response_state(session);
    } else if (strcmp(type, "response_output_correction") == 0) {
        static const char *const keys[] = {
            "correction_id", "cycle", "partial_public", "response_id",
            "text", "turn_id"
        };
        const char *correction_id = snag_json_string(data, "correction_id");
        const char *response_id = snag_json_string(data, "response_id");
        const char *text = snag_json_string(data, "text");
        const char *turn_id = snag_json_string(data, "turn_id");
        json_t *partial = json_object_get(data, "partial_public");
        uint64_t cycle;
        size_t len;
        bool cyber = text && strcmp(text, SNAG_CYBER_CLARIFICATION) == 0;

        if (!snag_json_exact_keys(data, keys, 6u) || !session->response_open ||
            (cyber ? session->cyber_clarifications >= SNAG_CYBER_CLARIFICATIONS_MAX :
                     session->output_correction_used) ||
            !correction_id ||
            !snag_hex_is_lower(correction_id, SNAG_ID_HEX_LEN) ||
            pending_user_id_exists(session, correction_id) ||
            !response_id ||
            strcmp(response_id, session->active_response_id) != 0 ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !text ||
            (!cyber && strcmp(text, SNAG_EMPTY_OUTPUT_CORRECTION) != 0 &&
             strcmp(text, SNAG_OVERSIZED_OUTPUT_CORRECTION) != 0) ||
            snag_partial_public_validate(partial, error, error_size) < 0 ||
            (cyber && json_array_size(partial) != 0u) ||
            snag_json_integer_u64(data, "cycle", &cycle) < 0 ||
            cycle != session->active_cycle ||
            session->pending_steering_count >= SNAG_MAX_STEERING_PER_TURN ||
            session->pending_steering_bytes >
                SNAG_MAX_STEERING_PER_TURN * SNAG_MAX_STEERING_TEXT -
                (len = strlen(text)))
            goto invalid;
        if (add_pending_steering(session, correction_id, text, len, seq) < 0)
            return -1;
        clear_response_state(session);
        if (cyber)
            ++session->cyber_clarifications;
        else
            session->output_correction_used = true;
    } else if (strcmp(type, "response_interrupted") == 0) {
        static const char *const keys[] = {
            "cycle", "origin", "partial_public", "reason", "response_id",
            "turn_id"
        };
        static const char *const origins[] = {"user", "steering", "recovery", "output"};
        static const char *const reasons[] = {"cancelled", "steered", "process_lost", "output_lost"};
        const char *response_id = snag_json_string(data, "response_id");
        const char *turn_id = snag_json_string(data, "turn_id");
        const char *origin = snag_json_string(data, "origin");
        const char *reason = snag_json_string(data, "reason");
        json_t *partial = json_object_get(data, "partial_public");
        uint64_t cycle;
        if (!snag_json_exact_keys(data, keys, 6u) || !session->response_open ||
            !response_id || strcmp(response_id, session->active_response_id) != 0 ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !string_in(origin, origins, sizeof(origins) / sizeof(origins[0])) ||
            !string_in(reason, reasons, sizeof(reasons) / sizeof(reasons[0])) ||
            ((strcmp(origin, "steering") == 0) !=
             (strcmp(reason, "steered") == 0)) ||
            (strcmp(origin, "steering") == 0 &&
             session->pending_steering_count == 0u) ||
            snag_partial_public_validate(partial, error, error_size) < 0 ||
            snag_json_integer_u64(data, "cycle", &cycle) < 0 ||
            cycle != session->active_cycle)
            goto invalid;
        session->response_open = false;
        session->response_complete = false;
        session->response_terminal = strcmp(origin, "steering") == 0 ?
            SNAG_RESPONSE_TERMINAL_STEERED : SNAG_RESPONSE_TERMINAL_INTERRUPTED;
    } else if (strcmp(type, "response_failed") == 0) {
        static const char *const keys[] = {
            "class", "cycle", "message", "partial_public", "response_id",
            "retry_count", "turn_id"
        };
        static const char *const classes[] = {
            "context", "provider", "protocol", "resource", "output", "internal"
        };
        const char *response_id = snag_json_string(data, "response_id");
        const char *turn_id = snag_json_string(data, "turn_id");
        const char *class_name = snag_json_string(data, "class");
        const char *message = snag_json_string(data, "message");
        json_t *partial = json_object_get(data, "partial_public");
        uint64_t cycle;
        uint64_t retry_count;
        if (!snag_json_exact_keys(data, keys, 7u) || !session->response_open ||
            !response_id || strcmp(response_id, session->active_response_id) != 0 ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !string_in(class_name, classes, sizeof(classes) / sizeof(classes[0])) ||
            !message || strlen(message) > 8192u ||
            snag_partial_public_validate(partial, error, error_size) < 0 ||
            snag_json_integer_u64(data, "cycle", &cycle) < 0 ||
            cycle != session->active_cycle ||
            snag_json_integer_u64(data, "retry_count", &retry_count) < 0 ||
            retry_count > 2u)
            goto invalid;
        session->response_open = false;
        session->response_complete = false;
        session->response_terminal = SNAG_RESPONSE_TERMINAL_FAILED;
    } else if (strcmp(type, "response_completed") == 0) {
        static const char *const keys[] = {
            "cycle", "items", "provider_response_id", "response_id", "status",
            "turn_id", "usage"
        };
        json_t *items = json_object_get(data, "items");
        const char *provider_response_id = snag_json_string(data, "provider_response_id");
        const char *response_id = snag_json_string(data, "response_id");
        const char *turn_id = snag_json_string(data, "turn_id");
        const char *status = snag_json_string(data, "status");
        struct snag_response_graph graph;
        struct snag_graph_decision decision;
        uint64_t cycle;
        int graph_rc;

        snag_response_graph_init(&graph);
        if (!snag_json_exact_keys(data, keys, 7u) || !session->response_open ||
            !response_id || strcmp(response_id, session->active_response_id) != 0 ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !status || strcmp(status, "completed") != 0 ||
            !provider_response_id ||
            snag_response_graph_set_provider_id(&graph, provider_response_id) < 0 ||
            snag_response_usage_from_json(json_object_get(data, "usage"),
                                         &graph.usage) < 0 ||
            snag_json_integer_u64(data, "cycle", &cycle) < 0 ||
            cycle != session->active_cycle ||
            snag_response_graph_from_json(&graph, items, error, error_size) < 0) {
            snag_response_graph_free(&graph);
            goto invalid;
        }
        graph_rc = snag_response_graph_classify(&graph, &decision,
                                               error, error_size);
        if (graph_rc < 0) {
            snag_response_graph_free(&graph);
            goto invalid;
        }
        if (graph.usage.input_known) {
            memcpy(session->usage_anchor_provider,
                   session->active_turn_provider,
                   sizeof(session->usage_anchor_provider));
            memcpy(session->usage_anchor_model, session->active_turn_model,
                   sizeof(session->usage_anchor_model));
            memcpy(session->usage_anchor_effort, session->active_turn_effort,
                   sizeof(session->usage_anchor_effort));
            memcpy(session->usage_anchor_compact_id, session->compact_id,
                   sizeof(session->usage_anchor_compact_id));
            memcpy(session->usage_anchor_provider_source_sha256,
                   session->active_response_provider_source_sha256,
                   sizeof(session->usage_anchor_provider_source_sha256));
            memcpy(session->usage_anchor_model_input_sha256,
                   session->active_response_model_input_sha256,
                   sizeof(session->usage_anchor_model_input_sha256));
            memcpy(session->usage_anchor_request_input_sha256,
                   session->active_response_request_input_sha256,
                   sizeof(session->usage_anchor_request_input_sha256));
            session->usage_anchor_model_input_bytes =
                session->active_response_model_input_bytes;
            session->usage_anchor_request_input_bytes =
                session->active_response_request_input_bytes;
            session->usage_anchor_request_input_count =
                session->active_response_request_input_count;
            session->usage_anchor_input_tokens = graph.usage.input_tokens;
            session->usage_anchor_valid = true;
        }
        session->response_open = false;
        session->response_complete = true;
        session->response_terminal = SNAG_RESPONSE_TERMINAL_NONE;
        session->response_outcome = decision.outcome;
        session->pending_call_count = 0;
        session->final_item_id[0] = '\0';
        session->final_response_id[0] = '\0';
        for (size_t i = 0; i < graph.count; ++i) {
            const struct snag_response_item *item = &graph.items[i];
            if (item->kind == SNAG_ITEM_ASSISTANT ||
                item->kind == SNAG_ITEM_REFUSAL) {
                if (replace_text(&session->last_assistant, item->text,
                                 SNAG_MAX_PUBLIC_ITEM) < 0) {
                    snag_response_graph_free(&graph);
                    return -1;
                }
            }
            if (item->kind == SNAG_ITEM_TOOL_CALL) {
                struct snag_pending_call *pending;
                if (session->pending_call_count >= SNAG_MAX_CALLS_PER_RESPONSE) {
                    snag_response_graph_free(&graph);
                    goto invalid;
                }
                pending = &session->pending_calls[session->pending_call_count++];
                memset(pending, 0, sizeof(*pending));
                memcpy(pending->call_id, item->call_id,
                       sizeof(pending->call_id));
                if (!snag_strcpy(pending->tool_name,
                                sizeof(pending->tool_name), item->name)) {
                    snag_response_graph_free(&graph);
                    goto invalid;
                }
                if (strcmp(item->name, "write_stdin") == 0) {
                    const char *handle = snag_json_string(item->arguments, "handle");
                    if (handle && snag_hex_is_lower(handle, SNAG_ID_HEX_LEN))
                        memcpy(pending->process_handle, handle,
                               sizeof(pending->process_handle));
                }
                if (strcmp(item->name, "exec_command") == 0) {
                    memcpy(pending->process_handle, item->call_id, sizeof(pending->process_handle));
                    process_label(pending->command, snag_json_string(item->arguments, "command"));
                    process_label(pending->workdir, snag_json_string(item->arguments, "workdir"));
                }
                if (snag_tool_action_digest(item, session->workspace,
                                           pending->action_sha256) < 0) {
                    snag_response_graph_free(&graph);
                    return -1;
                }
            }
        }
        if (decision.outcome == SNAG_GRAPH_FINAL ||
            decision.outcome == SNAG_GRAPH_REFUSAL) {
            const struct snag_response_item *item = &graph.items[decision.final_index];
            memcpy(session->final_item_id, item->local_item_id,
                   sizeof(session->final_item_id));
            memcpy(session->final_response_id, response_id,
                   sizeof(session->final_response_id));
        }
        snag_response_graph_free(&graph);
    } else if (strcmp(type, "tool_started") == 0) {
        static const char *const keys[] = {
            "action_sha256", "call_id", "resolved_workdir", "turn_id"
        };
        const char *action = snag_json_string(data, "action_sha256");
        const char *call_id = snag_json_string(data, "call_id");
        const char *workspace = snag_json_string(data, "resolved_workdir");
        const char *turn_id = snag_json_string(data, "turn_id");
        struct snag_pending_call *call;
        size_t index;
        if (!snag_json_exact_keys(data, keys, 4u) || !session->active_turn ||
            !session->response_complete || session->response_outcome != SNAG_GRAPH_CALLS ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !action || !snag_hex_is_lower(action, SNAG_SHA256_HEX_LEN) ||
            !workspace || strcmp(workspace, session->workspace) != 0 ||
            !call_id || !(call = find_pending_call(session, call_id, &index)) ||
            strcmp(action, call->action_sha256) != 0 ||
            call->started || call->finished)
            goto invalid;
        if (session->active_read_only && !snag_read_only_tool(call->tool_name))
            goto invalid;
        if (!strcmp(call->tool_name, "exec_command")) {
            struct snag_process_state *process;
            if (session->process_count >= SNAG_MAX_PROCESSES ||
                session->process_count >= session->max_parallel_commands ||
                snag_session_process(session, call->process_handle))
                goto invalid;
            process = &session->processes[session->process_count++];
            memset(process, 0, sizeof(*process));
            memcpy(process->handle, call->process_handle, sizeof(process->handle));
            memcpy(process->command, call->command, sizeof(process->command));
            memcpy(process->workdir, call->workdir, sizeof(process->workdir));
        } else if (!strcmp(call->tool_name, "write_stdin")) {
            if (!snag_session_process(session, call->process_handle))
                goto invalid;
            for (size_t i = 0u; i < session->pending_call_count; ++i)
                if (session->pending_calls[i].started &&
                    !strcmp(session->pending_calls[i].process_handle, call->process_handle))
                    goto invalid;
        }
        call->started = true;
    } else if (strcmp(type, "tool_finished") == 0) {
        static const char *const keys[] = {"call_id", "result", "turn_id"};
        const char *call_id = snag_json_string(data, "call_id");
        const char *turn_id = snag_json_string(data, "turn_id");
        json_t *result = json_object_get(data, "result");
        const char *status = snag_json_string(result, "status");
        const char *handle = snag_json_string(result, "handle");
        struct snag_pending_call *call;
        size_t index;
        if (!snag_json_exact_keys(data, keys, 3u) || !session->active_turn ||
            !session->response_complete || !turn_id ||
            strcmp(turn_id, session->active_turn_id) != 0 || !call_id ||
            !(call = find_pending_call(session, call_id, &index)) || call->finished ||
            snag_tool_result_valid(result) < 0 || !status)
            goto invalid;
        if (call->started) {
            if (strcmp(status, "not_run") == 0 || strcmp(status, "denied") == 0)
                goto invalid;
        } else if (strcmp(status, "not_run") != 0 &&
                   strcmp(status, "denied") != 0) {
            goto invalid;
        }
        struct snag_process_state *process = snag_session_process(session, call->process_handle);
        if (call->started && call->process_handle[0]) {
            if (!process)
                goto invalid;
            json_t *ref = json_object_get(result, "output_ref");
            if (ref) {
                const char *ref_handle = snag_json_string(ref, "handle");
                const char *const begin[] = {"stdout_start", "stderr_start"};
                const char *const end[] = {"stdout_end", "stderr_end"};
                if (!ref_handle || strcmp(ref_handle, process->handle))
                    goto invalid;
                for (unsigned int s = 0u; s < 2u; ++s) {
                    uint64_t from, to;
                    if (snag_json_integer_u64(ref, begin[s], &from) < 0 ||
                        snag_json_integer_u64(ref, end[s], &to) < 0 ||
                        from != process->collected_bytes[s] || to != process->output_bytes[s])
                        goto invalid;
                    process->collected_bytes[s] = to;
                }
            } else if ((process->output_bytes[0] || process->output_bytes[1]) &&
                       strcmp(status, "outcome_unknown")) {
                goto invalid;
            }
            if (!strcmp(status, "running")) {
                if (!handle || strcmp(handle, process->handle))
                    goto invalid;
            } else if (strcmp(status, "outcome_unknown")) {
                remove_process(session, process);
            }
        } else if (!strcmp(status, "running")) {
            goto invalid;
        }
        call->finished = true;
        if (all_pending_finished(session) &&
            session->response_outcome == SNAG_GRAPH_CALLS) {
            session->response_complete = false;
            session->pending_call_count = 0;
            memset(session->pending_calls, 0, sizeof(session->pending_calls));
            session->active_response_id[0] = '\0';
        }
    } else if (strcmp(type, "process_output") == 0) {
        const char *handle = snag_json_string(data, "handle");
        const char *turn_id = snag_json_string(data, "turn_id");
        struct snag_process_state *process = snag_session_process(session, handle);
        struct snag_buf bytes;
        uint64_t stream, offset;
        int rc;
        if (!session->active_turn || !turn_id ||
            strcmp(turn_id, session->active_turn_id) || !process ||
            snag_json_integer_u64(data, "stream", &stream) < 0 || stream > 1u ||
            snag_json_integer_u64(data, "offset", &offset) < 0 ||
            offset != process->output_bytes[stream])
            goto invalid;
        snag_buf_init(&bytes, 16384u);
        rc = snag_process_output_decode(data, &bytes);
        if (rc == 0 && bytes.len && offset <= INT64_MAX - bytes.len)
            process->output_bytes[stream] += bytes.len;
        else
            rc = -1;
        snag_buf_free(&bytes);
        if (rc < 0)
            goto invalid;
    } else if (strcmp(type, "process_closed") == 0) {
        static const char *const keys[] = {"cause", "handle", "result", "turn_id"};
        static const char *const causes[] = {
            "user_interrupt", "provider_failure", "protocol_failure",
            "tool_failure", "output_failure", "internal_failure"
        };
        const char *cause = snag_json_string(data, "cause");
        const char *handle = snag_json_string(data, "handle");
        const char *turn_id = snag_json_string(data, "turn_id");
        json_t *result = json_object_get(data, "result");
        const char *status = snag_json_string(result, "status");
        struct snag_process_state *process = snag_session_process(session, handle);
        if (!snag_json_exact_keys(data, keys, 4u) || !session->active_turn ||
            session->response_open ||
            (session->response_complete && !all_pending_finished(session)) ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !handle || !snag_hex_is_lower(handle, SNAG_ID_HEX_LEN) ||
            !process ||
            !string_in(cause, causes, sizeof(causes) / sizeof(causes[0])) ||
            snag_tool_result_valid(result) < 0 ||
            !process_close_status(status))
            goto invalid;
        remove_process(session, process);
    } else if (strcmp(type, "turn_completed") == 0) {
        static const char *const keys[] = {
            "final_item_id", "final_response_id", "turn_id"
        };
        const char *turn_id = snag_json_string(data, "turn_id");
        const char *item_id = snag_json_string(data, "final_item_id");
        const char *response_id = snag_json_string(data, "final_response_id");
        if (!snag_json_exact_keys(data, keys, 3u) || !session->active_turn ||
            !session->response_complete ||
            (session->response_outcome != SNAG_GRAPH_FINAL &&
             session->response_outcome != SNAG_GRAPH_REFUSAL) ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 || !item_id ||
            strcmp(item_id, session->final_item_id) != 0 || !response_id ||
            strcmp(response_id, session->final_response_id) != 0 ||
            session->process_count != 0u ||
            session->pending_call_count != 0u ||
            session->pending_steering_count != 0u)
            goto invalid;
        session->active_turn = false;
        session->active_read_only = false;
        session->active_queued = false;
        session->active_turn_id[0] = '\0';
        session->active_turn_model[0] = '\0';
        clear_response_state(session);
    } else if (strcmp(type, "turn_completed_silent") == 0) {
        static const char *const keys[] = {"reason", "response_id", "turn_id"};
        static const char *const reasons[] = {
            "room_update_quiet", "reply_reminder_exhausted"
        };
        const char *turn_id = snag_json_string(data, "turn_id");
        const char *response_id = snag_json_string(data, "response_id");
        const char *reason = snag_json_string(data, "reason");

        if (!snag_json_exact_keys(data, keys, 3u) || !session->active_turn ||
            !session->response_complete ||
            session->response_outcome != SNAG_GRAPH_NONPRODUCTIVE ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !response_id || strcmp(response_id,
                                   session->active_response_id) != 0 ||
            !string_in(reason, reasons,
                       sizeof(reasons) / sizeof(reasons[0])) ||
            (strcmp(reason, "reply_reminder_exhausted") == 0 &&
             !session->irc_reply_reminded) ||
            session->process_count != 0u ||
            session->pending_call_count != 0u ||
            session->pending_steering_count != 0u)
            goto invalid;
        session->active_turn = false;
        session->active_read_only = false;
        session->active_queued = false;
        session->active_turn_id[0] = '\0';
        session->active_turn_model[0] = '\0';
        clear_response_state(session);
    } else if (strcmp(type, "turn_interrupted") == 0) {
        static const char *const keys[] = {"origin", "reason", "turn_id"};
        static const char *const origins[] = {"user", "recovery", "output"};
        static const char *const reasons[] = {
            "cancelled", "process_lost", "output_lost", "session_recovered"
        };
        const char *turn_id = snag_json_string(data, "turn_id");
        const char *origin = snag_json_string(data, "origin");
        const char *reason = snag_json_string(data, "reason");
        if (!snag_json_exact_keys(data, keys, 3u) || !session->active_turn ||
            session->process_count != 0u ||
            session->response_open ||
            (session->response_complete && !all_pending_finished(session)) ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !string_in(origin, origins, sizeof(origins) / sizeof(origins[0])) ||
            !string_in(reason, reasons, sizeof(reasons) / sizeof(reasons[0])))
            goto invalid;
        session->active_turn = false;
        session->active_read_only = false;
        session->active_queued = false;
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
        const char *turn_id = snag_json_string(data, "turn_id");
        const char *class_name = snag_json_string(data, "class");
        const char *message = snag_json_string(data, "message");
        if (!snag_json_exact_keys(data, keys, 3u) || !session->active_turn ||
            session->process_count != 0u ||
            session->response_open ||
            (session->response_complete && !all_pending_finished(session)) ||
            !turn_id || strcmp(turn_id, session->active_turn_id) != 0 ||
            !string_in(class_name, classes, sizeof(classes) / sizeof(classes[0])) ||
            !message || strlen(message) > 8192u)
            goto invalid;
        session->last_turn_failed = true;
        session->retry_read_only = session->active_read_only;
        session->active_turn = false;
        session->active_read_only = false;
        session->active_queued = false;
        session->active_turn_id[0] = '\0';
        session->active_turn_model[0] = '\0';
        clear_response_state(session);
        clear_pending_steering(session);
    } else {
        snag_errorf(error, error_size,
                  "event type %s is not implemented by this checkpoint", type);
        errno = ENOTSUP;
        return -1;
    }
    return 0;
invalid:
    snag_errorf(error, error_size, "invalid %s transition at sequence %llu", type,
              (unsigned long long)seq);
    errno = EINVAL;
    return -1;
}

static int
read_event_log(struct snag_session *source, struct snag_session *verifier,
               int64_t boundary, enum snag_tail_policy tail_policy,
               snag_session_event_fn fn, void *opaque,
               const struct snag_process_state *cursor,
               int64_t *complete_end_out, uint64_t *next_seq_out,
               char *error, size_t error_size)
{
    struct snag_buf line;
    unsigned char chunk[8192];
    int64_t complete_end = cursor ? (int64_t)cursor->log_offset : 0;
    int64_t read_off = complete_end;
    uint64_t seq = cursor ? cursor->log_seq : 1u;
    int rc = -1;

    snag_buf_init(&line, SNAG_MAX_EVENT_LINE);
    for (;;) {
        size_t want = sizeof(chunk);
        ssize_t got;

        if (boundary >= 0) {
            if (read_off == boundary)
                break;
            if (read_off > boundary) {
                errno = EIO;
                goto boundary_error;
            }
            if ((int64_t)want > boundary - read_off)
                want = (size_t)(boundary - read_off);
        }
        got = snag_pread(source->log_fd, chunk, want, read_off);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            snag_errorf(error, error_size, "cannot read event log: %s",
                       strerror(errno));
            goto out;
        }
        if (got == 0) {
            if (boundary >= 0 && read_off != boundary)
                goto boundary_error;
            break;
        }
        read_off += got;
        for (ssize_t i = 0; i < got; ++i) {
            json_t *event;
            const char *type;
            json_t *data;
            char jerr[192];

            if (chunk[i] != '\n') {
                if (snag_buf_putc(&line, chunk[i]) < 0) {
                    snag_errorf(error, error_size,
                               "event line exceeds 16 MiB");
                    goto out;
                }
                continue;
            }
            if (!line.len) {
                snag_errorf(error, error_size,
                           "blank event line at sequence %llu",
                           (unsigned long long)seq);
                errno = EINVAL;
                goto out;
            }
            event = snag_json_load_canonical(line.data, line.len,
                                            jerr, sizeof(jerr));
            if (!event) {
                snag_errorf(error, error_size, "corrupt event %llu: %s",
                           (unsigned long long)seq, jerr);
                goto out;
            }
            if (!common_event_valid(event, verifier, seq, &type, &data,
                                    error, error_size) ||
                (fn ? fn(opaque, seq, type, data, error, error_size) :
                      apply_event(verifier, type, data, seq,
                                  error, error_size)) < 0) {
                json_decref(event);
                goto out;
            }
            json_decref(event);
            complete_end = read_off - (int64_t)(got - i - 1);
            ++seq;
            snag_buf_reset(&line);
        }
    }
    if (line.len && (boundary >= 0 || tail_policy == SNAG_TAIL_REJECT)) {
        snag_errorf(error, error_size,
                   "event log has an incomplete final suffix");
        errno = EINVAL;
        goto out;
    }
    if (line.len && tail_policy == SNAG_TAIL_TRUNCATE &&
        (snag_truncate(source->log_fd, complete_end) < 0 ||
         snag_sync_file(source->log_fd) < 0)) {
        snag_errorf(error, error_size,
                   "cannot truncate incomplete log tail: %s",
                   strerror(errno));
        goto out;
    }
    if (complete_end_out)
        *complete_end_out = complete_end;
    if (next_seq_out)
        *next_seq_out = seq;
    rc = 0;
    goto out;

boundary_error:
    snag_errorf(error, error_size,
               "event log ended before recorded boundary");
    errno = EIO;
out:
    snag_buf_free(&line);
    return rc;
}

int
snag_store_scan_log(struct snag_session *session,
                   enum snag_tail_policy tail_policy, bool allow_active,
                   char *error, size_t error_size)
{
    int64_t complete_end;
    uint64_t next_seq;

    if (read_event_log(session, session, -1, tail_policy, NULL, NULL, NULL,
                       &complete_end, &next_seq, error, error_size) < 0)
        return -1;
    session->log_end = complete_end;
    session->next_seq = next_seq;
    if (next_seq == 1) {
        snag_errorf(error, error_size, "session event log is empty");
        errno = EINVAL;
        return -1;
    }
    if (session->active_turn && !allow_active) {
        snag_errorf(error, error_size,
                   "active-turn recovery is unavailable in this scan mode");
        errno = ENOTSUP;
        return -1;
    }
    session->active_compact_id[0] = '\0';
    session->active_compact_source_sha256[0] = '\0';
    session->active_compact_source_seq = 0u;
    return 0;
}

int
snag_session_each_event(struct snag_session *session, snag_session_event_fn fn,
                       void *opaque, char *error, size_t error_size)
{
    struct snag_session verifier;

    if (!session || !fn || session->log_fd < 0 || session->log_end < 0 ||
        !snag_hex_is_lower(session->id, SNAG_ID_HEX_LEN)) {
        snag_errorf(error, error_size, "invalid session event iterator");
        errno = EINVAL;
        return -1;
    }
    snag_session_init(&verifier);
    memcpy(verifier.id, session->id, sizeof(verifier.id));
    return read_event_log(session, &verifier, session->log_end,
                          SNAG_TAIL_REJECT, fn, opaque, NULL, NULL, NULL,
                          error, error_size);
}

int
snag_session_each_event_since(struct snag_session *session,
                              const struct snag_process_state *cursor,
                              snag_session_event_fn fn, void *opaque,
                              char *error, size_t error_size)
{
    struct snag_session verifier;
    if (!cursor || !cursor->log_seq)
        return snag_session_each_event(session, fn, opaque, error, error_size);
    if (cursor->log_offset > (uint64_t)session->log_end ||
        !snag_hex_is_lower(cursor->log_hash, SNAG_SHA256_HEX_LEN))
        return -1;
    snag_session_init(&verifier);
    memcpy(verifier.id, session->id, sizeof(verifier.id));
    memcpy(verifier.prev_sha256, cursor->log_hash, sizeof(verifier.prev_sha256));
    return read_event_log(session, &verifier, session->log_end,
                          SNAG_TAIL_REJECT, fn, opaque, cursor, NULL, NULL,
                          error, error_size);
}

static char *
clone_optional(const char *value, size_t max)
{
    return value ? snag_strdup_checked(value, max) : NULL;
}

static int
clone_session_state(const struct snag_session *source,
                    struct snag_session *staged)
{
    *staged = *source;
    for (size_t i = 0; i < staged->pending_steering_count; ++i)
        staged->pending_steering[i].text = NULL;
    for (size_t i = 0; i < staged->pending_queue_count; ++i)
        staged->pending_queue[i].text = NULL;

    staged->workspace = clone_optional(source->workspace, SNAG_PATH_MAX_BYTES);
    staged->first_user = clone_optional(source->first_user, SNAG_MAX_DIRECT_PROMPT);
    staged->last_user = clone_optional(source->last_user, SNAG_MAX_DIRECT_PROMPT);
    staged->last_assistant = clone_optional(source->last_assistant,
                                            SNAG_MAX_PUBLIC_ITEM);
    staged->goal_prompt = clone_optional(source->goal_prompt,
                                         SNAG_MAX_GOAL_PROMPT);
    staged->goal_blocker = clone_optional(source->goal_blocker,
                                          SNAG_MAX_GOAL_BLOCKER);
    staged->compact_output = json_incref(source->compact_output);
    if ((source->workspace && !staged->workspace) ||
        (source->first_user && !staged->first_user) ||
        (source->last_user && !staged->last_user) ||
        (source->last_assistant && !staged->last_assistant) ||
        (source->goal_prompt && !staged->goal_prompt) ||
        (source->goal_blocker && !staged->goal_blocker) ||
        (source->compact_output && !staged->compact_output))
        goto fail;
    for (size_t i = 0; i < source->pending_steering_count; ++i) {
        staged->pending_steering[i].text =
            clone_optional(source->pending_steering[i].text,
                           SNAG_MAX_STEERING_TEXT);
        if (!staged->pending_steering[i].text)
            goto fail;
    }
    for (size_t i = 0; i < source->pending_queue_count; ++i) {
        staged->pending_queue[i].text =
            clone_optional(source->pending_queue[i].text,
                           SNAG_MAX_QUEUED_TEXT);
        if (!staged->pending_queue[i].text)
            goto fail;
    }
    return 0;
fail:
    return -1;
}

int
snag_session_commit(struct snag_session *session, const char *type, json_t *data,
                   uint64_t *written_seq, char *error, size_t error_size)
{
    struct snag_session staged = {0};
    int rc = -1;

    if (!data || clone_session_state(session, &staged) < 0) {
        snag_errorf(error, error_size, "cannot stage %s event", type);
        errno = ENOMEM;
    } else if (apply_event(&staged, type, data, session->next_seq,
                          error, error_size) == 0) {
        /* Append updates the staged metadata too. No live state is adopted
         * until durable append succeeds; descriptors and dir_path are borrowed. */
        rc = snag_session_append(&staged, type, data, written_seq,
                                 error, error_size);
    }
    json_decref(data);
    if (rc < 0)
        free_session_state(&staged);
    else {
        free_session_state(session);
        *session = staged;
    }
    return rc;
}

static char *
canonical_workspace(const char *workspace, char *error, size_t error_size)
{
    char *resolved = snag_realpath(workspace);
    snag_file_info st;

    if (!resolved) {
        snag_errorf(error, error_size, "cannot resolve workspace %s: %s", workspace,
                  strerror(errno));
        return NULL;
    }
    if (strlen(resolved) > SNAG_PATH_MAX_BYTES ||
        !snag_utf8_valid((const unsigned char *)resolved, strlen(resolved), true) ||
        snag_stat(resolved, &st) < 0 || !S_ISDIR(st.st_mode)) {
        snag_errorf(error, error_size, "workspace must be an existing UTF-8 directory");
        free(resolved);
        errno = EINVAL;
        return NULL;
    }
    return resolved;
}

static json_t *
session_created_data(const char *workspace, const char *provider,
                     const char *model,
                     const char *effort)
{
    return json_pack("{s:s,s:s,s:s,s:i,s:s,s:s}",
        "default_effort", effort, "default_model", model,
        "default_provider", provider, "format", 2, "protocol", "responses",
        "workspace", workspace);
}

int
snag_session_create(struct snag_store *store, struct snag_session *session,
                   const char *workspace, const char *provider,
                   const char *model, const char *effort,
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
        if (snag_random_id(session->id) < 0) {
            snag_errorf(error, error_size, "cryptographic session id generation failed");
            goto out;
        }
        if (snag_mkdir_private_at(store->sessions_fd, session->id) == 0) {
            created = 1;
            break;
        }
        if (errno != EEXIST) {
            snag_errorf(error, error_size, "cannot create session directory: %s",
                      strerror(errno));
            goto out;
        }
    }
    if (!created) {
        snag_errorf(error, error_size, "could not allocate a unique session id");
        errno = EEXIST;
        goto out;
    }
    dir = snag_path_join(store->root_path, "sessions");
    if (dir) {
        char *full = snag_path_join(dir, session->id);
        free(dir);
        dir = full;
    }
    if (!dir)
        goto out;
    session->dir_path = dir;
    dir = NULL;
    session->dir_fd = snag_open_read_security_at(store->sessions_fd, session->id, true);
    if (session->dir_fd < 0) {
        snag_errorf(error, error_size, "cannot open new session directory: %s",
                  strerror(errno));
        goto out;
    }
    if (snag_store_verify_private_fd(session->dir_fd, true, "session directory",
                          error, error_size) < 0 ||
        snag_store_open_session_files(session, true, error, error_size) < 0)
        goto out;
    session->workspace = resolved;
    resolved = NULL;
    if (!snag_strcpy(session->default_provider,
                    sizeof(session->default_provider), provider) ||
        !snag_strcpy(session->default_model, sizeof(session->default_model), model) ||
        !snag_strcpy(session->default_effort, sizeof(session->default_effort), effort)) {
        errno = EOVERFLOW;
        goto out;
    }
    if (snag_session_commit(session, "session_created",
                           session_created_data(session->workspace, provider,
                                                model, effort),
                           NULL, error, error_size) < 0)
        goto out;
    if (snag_sync_dir(session->dir_fd) < 0 || snag_sync_dir(store->sessions_fd) < 0) {
        snag_errorf(error, error_size, "cannot sync new session directory: %s",
                  strerror(errno));
        goto out;
    }
    rc = 0;
out:
    free(resolved);
    free(dir);
    if (rc < 0 && created) {
        char failed_id[SNAG_ID_HEX_LEN + 1u];
        memcpy(failed_id, session->id, sizeof(failed_id));
        if (session->dir_fd >= 0) {
            (void)snag_unlink_at(session->dir_fd, "events.jsonl", false);
            (void)snag_unlink_at(session->dir_fd, "lock", false);
        }
        snag_session_close(session);
        (void)snag_unlink_at(store->sessions_fd, failed_id, true);
    }
    return rc;
}
