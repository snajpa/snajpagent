/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_context.h"

#include "base.h"
#include "fs.h"
#include "store_binary_legacy.h"

#include <errno.h>
#include <string.h>

struct source_walk {
    int fd;
    const struct snag_context_control *control;
    const struct snag_binary_anchor *verified;
};

static int
source_header(int fd, struct snag_binary_identity *identity, struct snag_binary_anchor *anchor)
{
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    size_t position = 0u;
    while (position < sizeof(header)) {
        ssize_t got = snag_pread(fd, header + position, sizeof(header) - position,
            (int64_t)position);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return -1;
        if (!got) return snag_errno(EIO);
        position += (size_t)got;
    }
    return snag_binary_header_decode(header, sizeof(header), identity, anchor);
}

/* This is lookup under the already verified full-prefix anchor, not another
 * semantic replay. Import markers supply only the old adjacent-IRC lookup seam. */
static int
walk_sources(void *opaque, snag_session_event_fn fn, void *argument,
    char *error, size_t error_size)
{
    const struct source_walk *source = opaque;
    struct snag_binary_identity identity;
    struct snag_binary_anchor anchor;
    if (source_header(source->fd, &identity, &anchor) < 0) return -1;
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    int rc = -1;
    while (anchor.end < source->verified->end) {
        if (source->control && source->control->cancelled &&
            source->control->cancelled(source->control->opaque)) {
            snag_fail(error, error_size, ECANCELED, "context source lookup cancelled");
            goto done;
        }
        struct snag_binary_batch batch;
        struct snag_binary_anchor next;
        int read = snag_binary_batch_read(source->fd, source->verified->end, &anchor,
            &scratch, &batch, &next);
        if (read != 0) {
            if (read > 0) snag_fail(error, error_size, EAGAIN, "context source prefix changed");
            goto done;
        }
        size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
        for (uint32_t i = 0u; i < batch.count; ++i) {
            struct snag_binary_record record;
            uint64_t seq;
            if (snag_binary_record_next(&batch, &cursor, &record, &seq) != 0) goto done;
            const char *type = NULL;
            json_t *data = NULL;
            if (record.kind == SNAG_BINARY_IRC_EVENT) {
                if (snag_binary_legacy_decode(&record, &type, &data) < 0) goto done;
            } else if (record.kind == SNAG_BINARY_LEGACY_CHECKPOINT) {
                type = "session_checkpoint";
                data = json_object();
                if (!data) { errno = ENOMEM; goto done; }
            } else {
                continue;
            }
            int called = fn(argument, NULL, seq, type, data, error, error_size);
            json_decref(data);
            if (called < 0) goto done;
        }
        anchor = next;
    }
    if (anchor.end != source->verified->end || anchor.next_seq != source->verified->next_seq ||
        anchor.previous != source->verified->previous || anchor.turns != source->verified->turns ||
        memcmp(anchor.digest, source->verified->digest, sizeof(anchor.digest))) {
        errno = EAGAIN;
        goto done;
    }
    rc = 0;
 done:
    snag_buf_free(&scratch);
    return rc;
}

/* Replaying the exact boundary is deliberately the independent semantic oracle.
 * Checksums alone cannot prove that an otherwise valid older control/reference
 * remains current, or that a saved seam includes every required event. */
static int
verify_checkpoint(int fd, const struct snag_binary_anchor *boundary,
    struct snag_session *state, struct snag_binary_checkpoint_sources *origins,
    const void *bytes, size_t size, const struct snag_context_control *control,
    char *error, size_t error_size)
{
    struct snag_binary_identity identity;
    struct snag_binary_anchor root;
    struct snag_binary_checkpoint_frame frame;
    if (source_header(fd, &identity, &root) < 0) return -1;
    if (snag_binary_checkpoint_frame_decode(bytes, size, &identity, boundary, &frame) != 0 ||
        frame.core.version != 1u || frame.provider.version != 1u) {
        return snag_fail(error, error_size, EINVAL, "invalid native checkpoint frame");
    }
    struct snag_binary_checkpoint_provider view;
    if (snag_binary_checkpoint_provider_decode(frame.provider.data, frame.provider.size,
        &view) < 0) return snag_fail(error, error_size, EINVAL, "invalid provider recipe");
    const json_t *recent, *history;
    if (snag_context_capture_seam(state, &recent, &history) < 0) return -1;
    struct snag_buf core = {.max = SIZE_MAX}, provider = {.max = SIZE_MAX};
    struct snag_session loaded;
    snag_session_init(&loaded);
    struct snag_binary_checkpoint_sources loaded_origins = {0};
    struct snag_context_capture *capture = NULL;
    json_t *events = NULL, *history_events = NULL;
    int rc = -1;
    if (snag_binary_checkpoint_core_encode(&core, origins, state) < 0 ||
        snag_binary_checkpoint_provider_encode(&provider, state, recent, history) < 0) goto done;
    if (core.len != frame.core.size || provider.len != frame.provider.size ||
        memcmp(core.data, frame.core.data, core.len) ||
        memcmp(provider.data, frame.provider.data, provider.len)) {
        snag_fail(error, error_size, EINVAL, "checkpoint differs from its committed prefix");
        goto done;
    }
    /* Keep strict replay as the source/epoch authority, while materializing the
     * returned state from the candidate sections. Payload values are shared from
     * its verified canonical pool; no weaker inert record decoder is needed. */
    if (snag_binary_checkpoint_core_read(fd, &frame, &loaded, &loaded_origins) < 0 ||
        snag_binary_checkpoint_provider_materialize(frame.provider.data, frame.provider.size,
            recent, history, &events, &history_events) < 0) goto done;
    capture = snag_context_capture_new(control);
    if (!capture) { errno = ENOMEM; goto done; }
    if (snag_context_capture_seed(capture, events, history_events) < 0 ||
        snag_context_capture_bind(&capture, &loaded, error, error_size) < 0) goto done;
    snag_session_close(state);
    *state = loaded;
    snag_session_init(&loaded);
    snag_binary_checkpoint_sources_free(origins);
    *origins = loaded_origins;
    loaded_origins = (struct snag_binary_checkpoint_sources){0};
    rc = 0;
done:
    snag_session_close(&loaded);
    snag_binary_checkpoint_sources_free(&loaded_origins);
    snag_context_capture_free(capture);
    json_decref(events);
    json_decref(history_events);
    snag_buf_free(&core);
    snag_buf_free(&provider);
    return rc;
}

static int
reconcile_context(struct snag_session *source, struct snag_session *restored,
    const struct snag_binary_anchor *prefix, const void *checkpoint, size_t checkpoint_size,
    struct snag_binary_recovery *recovery,
    struct snag_binary_checkpoint_sources *sources, const struct snag_context_control *control,
    char *error, size_t error_size)
{
    if (!source || !restored || source == restored || !recovery ||
        restored->dir_fd >= 0 || restored->log_fd >= 0 || restored->lock_fd >= 0 ||
        restored->pending_log) {
        return snag_fail(error, error_size, EINVAL, "invalid native context replay target");
    }
    snag_file_info before, after;
    if (source->log_fd < 0)
        return snag_fail(error, error_size, EINVAL, "cannot inspect native context source");
    if (snag_fstat(source->log_fd, &before) < 0)
        return snag_fail(error, error_size, errno, "cannot inspect native context source");
    struct snag_context_capture *capture = snag_context_capture_new(control);
    if (!capture) return snag_fail(error, error_size, ENOMEM, "cannot capture provider context");
    struct snag_session candidate;
    snag_session_init(&candidate);
    struct snag_binary_checkpoint_sources origins = {0};
    int rc = prefix ?
        snag_store_reconcile_binary_prefix(source, &candidate, prefix, snag_context_capture_event,
            capture, recovery, sources || checkpoint ? &origins : NULL, error, error_size) :
        snag_store_reconcile_binary(source, &candidate, snag_context_capture_event, capture,
            recovery, sources || checkpoint ? &origins : NULL, error, error_size);
    struct source_walk walk = {.fd = source->log_fd, .control = control,
        .verified = &recovery->verified};
    if (rc == 0) {
        rc = snag_context_capture_sources(capture, &candidate, walk_sources, &walk,
            error, error_size);
    }
    if (rc == 0) rc = snag_context_capture_bind(&capture, &candidate, error, error_size);
    if (rc == 0 && checkpoint) {
        rc = verify_checkpoint(source->log_fd, &recovery->verified, &candidate, &origins,
            checkpoint, checkpoint_size, control, error, error_size);
        if (rc == 0 && control && control->cancelled && control->cancelled(control->opaque)) {
            rc = snag_fail(error, error_size, ECANCELED, "checkpoint verification cancelled");
        }
    }
    if (rc == 0 && snag_fstat(source->log_fd, &after) < 0)
        rc = snag_fail(error, error_size, errno, "cannot recheck native context source");
    if (rc == 0 && (before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino || before.st_size != after.st_size ||
        before.st_mtime != after.st_mtime)) {
        rc = snag_fail(error, error_size, EAGAIN, "native source changed during context capture");
    }
    if (rc == 0) {
        snag_session_close(restored);
        *restored = candidate;
        snag_session_init(&candidate);
        if (sources) {
            *sources = origins;
            origins = (struct snag_binary_checkpoint_sources){0};
        }
    }
    snag_session_close(&candidate);
    snag_binary_checkpoint_sources_free(&origins);
    snag_context_capture_free(capture);
    return rc;
}

int
snag_store_reconcile_binary_context(struct snag_session *source,
    struct snag_session *restored, struct snag_binary_recovery *recovery,
    struct snag_binary_checkpoint_sources *sources, const struct snag_context_control *control,
    char *error, size_t error_size)
{
    return reconcile_context(source, restored, NULL, NULL, 0u, recovery, sources, control,
        error, error_size);
}

int
snag_store_reconcile_binary_context_prefix(struct snag_session *source,
    struct snag_session *restored, const struct snag_binary_anchor *prefix,
    struct snag_binary_recovery *recovery, struct snag_binary_checkpoint_sources *sources,
    const struct snag_context_control *control, char *error, size_t error_size)
{
    if (!prefix) return snag_fail(error, error_size, EINVAL, "missing native context boundary");
    return reconcile_context(source, restored, prefix, NULL, 0u, recovery, sources, control,
        error, error_size);
}

int
snag_store_verify_binary_context_checkpoint(struct snag_session *source,
    struct snag_session *restored, const struct snag_binary_anchor *prefix,
    const void *checkpoint, size_t checkpoint_size, struct snag_binary_recovery *recovery,
    struct snag_binary_checkpoint_sources *sources, const struct snag_context_control *control,
    char *error, size_t error_size)
{
    if (!prefix || !checkpoint || !checkpoint_size)
        return snag_fail(error, error_size, EINVAL, "missing native checkpoint candidate");
    return reconcile_context(source, restored, prefix, checkpoint, checkpoint_size,
        recovery, sources, control, error, error_size);
}
