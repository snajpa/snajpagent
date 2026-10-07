/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_FIXTURE_STORE_HISTORY_H
#define SNAJPAGENT_FIXTURE_STORE_HISTORY_H

#include "base.h"
#include "fs.h"
#include "store.h"
#include "store_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Python fixtures use the production decoder instead of a second native codec.
 * Buffer callbacks until whole-prefix verification succeeds. */
static inline int
fixture_history_event(void *opaque, const struct snag_session *state, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t error_size)
{
    struct snag_buf *rows = opaque;
    if (!state || seq > INT64_MAX || state->last_time_ms > INT64_MAX) {
        return snag_fail(error, error_size, EINVAL, "invalid fixture event coordinates");
    }
    json_t *row = json_pack("{s:I,s:I,s:s,s:O}", "seq", (json_int_t)seq,
        "time_ms", (json_int_t)state->last_time_ms, "type", type, "data", data);
    if (!row) return snag_fail(error, error_size, ENOMEM, "cannot allocate fixture event");
    char *text = json_dumps(row, JSON_COMPACT | JSON_SORT_KEYS);
    json_decref(row);
    if (!text) return snag_fail(error, error_size, ENOMEM, "cannot encode fixture event");
    int rc = snag_buf_append(rows, text, strlen(text));
    free(text);
    if (!rc) rc = snag_buf_putc(rows, '\n');
    return rc;
}

static inline int
fixture_history_read(const char *dotdir, const char *id, bool coordinates)
{
    struct snag_store store;
    snag_store_init(&store);
    struct snag_session source;
    snag_session_init(&source);
    struct snag_buf rows = {.max = SIZE_MAX};
    char error[1024] = "";
    bool incomplete = false;
    int rc = -1;

    /* Open only existing directories. This fixture acquires no writer lock,
     * repairs no tail and creates no store or derived files. */
    store.root_path = snag_strdup_checked(dotdir, SNAG_PATH_MAX_BYTES);
    if (!store.root_path || (store.root_fd = snag_open_read(dotdir, true)) < 0 ||
        (store.sessions_fd = snag_open_read_at(store.root_fd, "sessions", true)) < 0 ||
        snag_session_history_snapshot(&store, &source, id, &incomplete,
            error, sizeof(error)) < 0 ||
        snag_session_each_event(&source, fixture_history_event, &rows,
            error, sizeof(error)) < 0) goto done;
    if (coordinates) {
        if (!source.next_seq || source.next_seq - 1u > INT64_MAX || source.log_end < 0) {
            (void)snag_fail(error, sizeof(error), EINVAL, "invalid fixture boundary");
            goto done;
        }
        json_t *boundary = json_pack("{s:I,s:I,s:s}", "seq",
            (json_int_t)(source.next_seq - 1u), "end", (json_int_t)source.log_end,
            "sha256", source.prev_sha256);
        if (!boundary) goto done;
        char *text = json_dumps(boundary, JSON_COMPACT | JSON_SORT_KEYS);
        json_decref(boundary);
        if (!text) goto done;
        snag_buf_reset(&rows);
        int appended = snag_buf_append(&rows, text, strlen(text));
        free(text);
        if (appended < 0 || snag_buf_putc(&rows, '\n') < 0) goto done;
    }
    if (fwrite(rows.data, 1u, rows.len, stdout) != rows.len || fflush(stdout)) goto done;
    rc = 0;
done:
    if (rc < 0) fprintf(stderr, "fixture history: %s\n", error[0] ? error : strerror(errno));
    snag_buf_free(&rows);
    snag_session_close(&source);
    snag_store_close(&store);
    return rc < 0 ? 1 : 0;
}

#endif
