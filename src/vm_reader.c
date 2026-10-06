/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_reader.h"
#include "history_view.h"
#include "json.h"
#include "vm_public.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct snag_vm_reader {
    struct snag_store *store;
    struct snag_wire_secrets secrets;
    char **secret_values;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    snag_wake_fd wake[2];
    atomic_uint_fast64_t generation;
    atomic_bool stop;
    uint64_t working_generation;
    struct snag_vm_read_result *pending, *completed;
    struct snag_session view;
    bool best_effort, incomplete;
};

void
snag_vm_read_result_free(struct snag_vm_read_result *result)
{
    if (!result) return;
    json_decref(result->events);
    json_decref(result->catalog);
    free(result);
}

static bool
read_canceled(void *opaque)
{
    struct snag_vm_reader *reader = opaque;
    return atomic_load(&reader->stop) ||
        reader->working_generation != atomic_load(&reader->generation);
}

static void
view_close(struct snag_vm_reader *reader)
{
    snag_session_close(&reader->view);
    reader->view.history_cancel = read_canceled;
    reader->view.history_cancel_opaque = reader;
}

static struct snag_journal_cursor
view_tail(const struct snag_session *view)
{
    struct snag_journal_cursor tail = {.offset = view->log_end, .next_seq = view->next_seq};
    memcpy(tail.prev_sha256, view->prev_sha256, sizeof(tail.prev_sha256));
    return tail;
}

static int
view_open(struct snag_vm_reader *reader, const struct snag_vm_read_request *request,
    char *error, size_t size)
{
    struct snag_session *view = &reader->view;
    if (strcmp(view->id, request->session_id) ||
        (!request->trusted_tail && request->refresh) ||
        reader->best_effort == request->trusted_tail) view_close(reader);
    if (view->log_fd >= 0) {
        struct snag_journal_cursor tail = request->trusted_tail ? request->tail : view_tail(view);
        return snag_session_history_refresh(view, &tail, error, size);
    }
    reader->best_effort = !request->trusted_tail;
    reader->incomplete = false;
    if (request->trusted_tail) {
        return snag_session_history_open(reader->store, view, request->session_id,
            &request->tail, error, size);
    }
    return snag_session_history_snapshot(reader->store, view, request->session_id,
        &reader->incomplete, error, size);
}

struct read_page {
    struct snag_vm_reader *reader;
    json_t *events;
};

static int
read_event(void *opaque, const struct snag_session *state, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t size)
{
    struct read_page *page = opaque;
    (void)state;
    if (read_canceled(page->reader)) {
        return snag_fail(error, size, ECANCELED, "history read canceled");
    }
    char *text = snag_history_event_data(seq, type, data, &page->reader->secrets, error, size);
    if (!text) return -1;
    json_error_t parse;
    json_t *filtered = json_loads(text, JSON_REJECT_DUPLICATES, &parse);
    free(text);
    if (!filtered) return snag_fail(error, size, EINVAL, "invalid public history projection");
    json_t *event = json_pack("{s:I,s:s,s:o}", "seq", (json_int_t)seq,
        "type", type, "data", filtered);
    if (event && snag_vm_public_source_bytes(event, data) < 0) {
        json_decref(event);
        return -1;
    }
    if (!event || json_array_append_new(page->events, event) < 0) return -1;
    return 0;
}

static void
read_page(struct snag_vm_reader *reader, struct snag_vm_read_result *result)
{
    const struct snag_vm_read_request *request = &result->request;
    if (request->kind == SNAG_VM_READ_SESSIONS) {
        struct snag_session owned = {.lock_fd = -1};
        if (request->owned_session_id[0]) {
            memcpy(owned.id, request->owned_session_id, sizeof(owned.id));
            owned.lock_fd = 0;
        }
        json_t *catalog = snag_store_catalog(reader->store, &owned, request->stored_limit,
            read_canceled, reader, result->error, sizeof(result->error));
        char *encoded = catalog ? json_dumps(catalog, JSON_COMPACT) : NULL;
        json_decref(catalog);
        if (!encoded) goto failed;
        struct snag_buf filtered = {.max = SNAG_MEMORY_LIMIT / 4u};
        int rc = snag_wire_json_redact_bounded((const unsigned char *)encoded, strlen(encoded),
            filtered.max, &reader->secrets, &filtered, result->error, sizeof(result->error));
        snag_secret_bytes_free(encoded);
        json_error_t parse;
        if (!rc) result->catalog = json_loadb((const char *)filtered.data, filtered.len, 0u, &parse);
        snag_buf_free(&filtered);
        if (!result->catalog) goto failed;
        return;
    }
    if (view_open(reader, request, result->error, sizeof(result->error)) < 0) goto failed;
    result->tail = view_tail(&reader->view);
    result->best_effort = reader->best_effort;
    result->incomplete = reader->incomplete;
    result->events = json_array();
    if (!result->events) goto failed;
    struct read_page page = {.reader = reader, .events = result->events};
    if (request->reverse) {
        uint64_t before;
        if (snag_session_each_event_reverse(&reader->view, request->before_seq,
            SNAG_JOURNAL_PAGE_BYTES, read_event, &page, &before,
            result->error, sizeof(result->error)) < 0) goto failed;
        result->cursor = reader->view.history_cursor;
        result->more = before != 0u;
    } else {
        result->cursor = request->cursor;
        if (snag_session_each_event_forward(&reader->view, &result->cursor,
            SNAG_JOURNAL_PAGE_BYTES, read_event, &page,
            result->error, sizeof(result->error)) < 0) goto failed;
        result->more = result->cursor.offset < result->tail.offset;
    }
    return;
failed:
    result->error_number = errno ? errno : EIO;
    if (!result->error[0]) {
        (void)snag_errorf(result->error, sizeof(result->error), "history read: %s",
            strerror(result->error_number));
    }
    json_decref(result->events);
    result->events = NULL;
    view_close(reader);
}

static void *
reader_main(void *opaque)
{
    struct snag_vm_reader *reader = opaque;
    for (;;) {
        (void)pthread_mutex_lock(&reader->lock);
        while (!reader->pending && !atomic_load(&reader->stop)) {
            (void)pthread_cond_wait(&reader->condition, &reader->lock);
        }
        if (atomic_load(&reader->stop)) {
            (void)pthread_mutex_unlock(&reader->lock);
            break;
        }
        /* Request allocation happens in the caller so OOM is synchronous. */
        struct snag_vm_read_result *result = reader->pending;
        reader->pending = NULL;
        reader->working_generation = result->generation;
        (void)pthread_mutex_unlock(&reader->lock);
        read_page(reader, result);
        (void)pthread_mutex_lock(&reader->lock);
        if (!read_canceled(reader)) {
            reader->completed = result;
            result = NULL;
            snag_wakeup_send(reader->wake[1]);
        }
        (void)pthread_mutex_unlock(&reader->lock);
        snag_vm_read_result_free(result);
    }
    view_close(reader);
    return NULL;
}

static void
free_reader(struct snag_vm_reader *reader)
{
    for (size_t i = 0u; i < reader->secrets.count; ++i) {
        snag_secret_clear(reader->secret_values[i], strlen(reader->secret_values[i]));
        free(reader->secret_values[i]);
    }
    free(reader->secret_values);
    snag_wakeup_close(reader->wake);
    snag_vm_read_result_free(reader->pending);
    snag_vm_read_result_free(reader->completed);
    free(reader);
}

struct snag_vm_reader *
snag_vm_reader_open(struct snag_store *store, const struct snag_wire_secrets *secrets,
    char *error, size_t size)
{
    if (!store || store->sessions_fd < 0 || !store->root_path) {
        (void)snag_fail(error, size, EINVAL, "history reader requires an open store");
        return NULL;
    }
    struct snag_vm_reader *reader = calloc(1u, sizeof(*reader));
    if (!reader) return NULL;
    reader->store = store;
    reader->wake[0] = reader->wake[1] = SNAG_WAKE_INVALID;
    snag_session_init(&reader->view);
    reader->view.history_cancel = read_canceled;
    reader->view.history_cancel_opaque = reader;
    atomic_init(&reader->generation, 0u);
    atomic_init(&reader->stop, false);
    if (secrets && secrets->count) {
        if (secrets->count > SIZE_MAX / sizeof(char *)) { errno = EOVERFLOW; goto failed; }
        reader->secret_values = calloc(secrets->count, sizeof(char *));
        if (!reader->secret_values) goto failed;
        reader->secrets.values = (const char *const *)reader->secret_values;
        for (size_t i = 0u; i < secrets->count; ++i) {
            char *copy = snag_strdup_checked(secrets->values[i], SNAG_WIRE_SECRET_MAX);
            if (!copy) goto failed;
            reader->secret_values[reader->secrets.count++] = copy;
        }
    }
    if (snag_wakeup_create(reader->wake) < 0) goto failed;
    int rc = pthread_mutex_init(&reader->lock, NULL);
    if (rc) { errno = rc; goto failed; }
    rc = pthread_cond_init(&reader->condition, NULL);
    if (rc) {
        (void)pthread_mutex_destroy(&reader->lock);
        errno = rc;
        goto failed;
    }
    rc = pthread_create(&reader->thread, NULL, reader_main, reader);
    if (rc) {
        (void)pthread_cond_destroy(&reader->condition);
        (void)pthread_mutex_destroy(&reader->lock);
        errno = rc;
        goto failed;
    }
    return reader;
failed: {
    int saved = errno;
    free_reader(reader);
    (void)snag_fail(error, size, saved, "cannot start history reader: %s", strerror(saved));
    return NULL;
}
}

void
snag_vm_reader_cancel(struct snag_vm_reader *reader)
{
    (void)pthread_mutex_lock(&reader->lock);
    (void)atomic_fetch_add(&reader->generation, 1u);
    struct snag_vm_read_result *pending = reader->pending, *completed = reader->completed;
    reader->pending = reader->completed = NULL;
    (void)pthread_mutex_unlock(&reader->lock);
    snag_vm_read_result_free(pending);
    snag_vm_read_result_free(completed);
}

uint64_t
snag_vm_reader_request(struct snag_vm_reader *reader, const struct snag_vm_read_request *request)
{
    if (!reader || !request ||
        (request->kind != SNAG_VM_READ_HISTORY && request->kind != SNAG_VM_READ_SESSIONS) ||
        (request->kind == SNAG_VM_READ_HISTORY &&
         !snag_hex_is_lower(request->session_id, SNAG_ID_HEX_LEN)) ||
        (request->owned_session_id[0] &&
         !snag_hex_is_lower(request->owned_session_id, SNAG_ID_HEX_LEN))) {
        errno = EINVAL;
        return 0u;
    }
    struct snag_vm_read_result *result = calloc(1u, sizeof(*result));
    if (!result) return 0u;
    (void)pthread_mutex_lock(&reader->lock);
    uint64_t generation = atomic_fetch_add(&reader->generation, 1u) + 1u;
    result->request = *request;
    result->generation = generation;
    struct snag_vm_read_result *pending = reader->pending, *completed = reader->completed;
    reader->pending = result;
    reader->completed = NULL;
    (void)pthread_cond_signal(&reader->condition);
    (void)pthread_mutex_unlock(&reader->lock);
    snag_vm_read_result_free(pending);
    snag_vm_read_result_free(completed);
    return generation;
}

snag_wake_fd
snag_vm_reader_fd(const struct snag_vm_reader *reader)
{
    return reader->wake[0];
}

struct snag_vm_read_result *
snag_vm_reader_take(struct snag_vm_reader *reader)
{
    snag_wakeup_drain(reader->wake[0]);
    (void)pthread_mutex_lock(&reader->lock);
    struct snag_vm_read_result *result = reader->completed;
    reader->completed = NULL;
    (void)pthread_mutex_unlock(&reader->lock);
    return result;
}

void
snag_vm_reader_close(struct snag_vm_reader *reader)
{
    if (!reader) return;
    atomic_store(&reader->stop, true);
    (void)pthread_mutex_lock(&reader->lock);
    (void)pthread_cond_signal(&reader->condition);
    (void)pthread_mutex_unlock(&reader->lock);
    (void)pthread_join(reader->thread, NULL);
    (void)pthread_cond_destroy(&reader->condition);
    (void)pthread_mutex_destroy(&reader->lock);
    free_reader(reader);
}
