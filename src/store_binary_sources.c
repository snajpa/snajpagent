/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"
#include "json.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Current working origins. No completed lifetime source dictionary is retained. */
void
snag_binary_checkpoint_sources_free(struct snag_binary_checkpoint_sources *sources)
{
    if (!sources) return;
    free(sources->processes);
    free(sources->queue);
    free(sources->downloads);
    memset(sources, 0, sizeof(*sources));
}

int
snag_binary_checkpoint_sources_clone(struct snag_binary_checkpoint_sources *destination,
    const struct snag_binary_checkpoint_sources *source)
{
    if (!destination || !source || destination == source ||
        (source->process_count && !source->processes) ||
        (source->queue_count && !source->queue) ||
        (source->download_count && !source->downloads)) return snag_errno(EINVAL);
    if (source->process_count > SIZE_MAX / sizeof(*source->processes) ||
        source->queue_count > SIZE_MAX / sizeof(*source->queue) ||
        source->download_count > SIZE_MAX / sizeof(*source->downloads))
        return snag_errno(EOVERFLOW);
    struct snag_binary_checkpoint_sources staged = *source;
    staged.processes = NULL;
    staged.queue = NULL;
    staged.downloads = NULL;
    staged.process_capacity = source->process_count;
    staged.queue_capacity = source->queue_count;
    staged.download_capacity = source->download_count;
    if (source->process_count) {
        staged.processes = malloc(source->process_count * sizeof(*source->processes));
        if (!staged.processes) goto fail;
        memcpy(staged.processes, source->processes,
            source->process_count * sizeof(*source->processes));
    }
    if (source->queue_count) {
        staged.queue = malloc(source->queue_count * sizeof(*source->queue));
        if (!staged.queue) goto fail;
        memcpy(staged.queue, source->queue, source->queue_count * sizeof(*source->queue));
    }
    if (source->download_count) {
        staged.downloads = malloc(source->download_count * sizeof(*source->downloads));
        if (!staged.downloads) goto fail;
        memcpy(staged.downloads, source->downloads,
            source->download_count * sizeof(*source->downloads));
    }
    snag_binary_checkpoint_sources_free(destination);
    *destination = staged;
    return 0;
fail:
    {
        int code = errno;
        snag_binary_checkpoint_sources_free(&staged);
        errno = code;
    }
    return -1;
}

static int
update_process_sources(struct snag_binary_checkpoint_sources *sources,
    const struct snag_session *state, uint64_t sequence, enum snag_binary_kind kind)
{
    size_t old_count = sources->process_count;
    if (state->process_count > old_count) {
        if (old_count == SIZE_MAX || state->process_count != old_count + 1u ||
            kind != SNAG_BINARY_TOOL_STARTED || !sources->calls.graph ||
            sources->calls.graph >= sequence) return snag_errno(EINVAL);
        if (state->process_count > sources->process_capacity) {
            size_t capacity = state->process_capacity;
            if (capacity < state->process_count ||
                capacity > SIZE_MAX / sizeof(*sources->processes))
                return snag_errno(EOVERFLOW);
            void *grown = realloc(sources->processes, capacity * sizeof(*sources->processes));
            if (!grown) return -1;
            sources->processes = grown;
            sources->process_capacity = capacity;
        }
        struct snag_binary_checkpoint_process_source *added = &sources->processes[old_count];
        *added = (struct snag_binary_checkpoint_process_source){
            .started = sequence, .call = sources->calls};
        memcpy(added->handle, state->processes[old_count].handle, sizeof(added->handle));
        ++old_count;
    }
    size_t previous = 0u;
    for (size_t i = 0u; i < state->process_count; ++i) {
        while (previous < old_count &&
            strcmp(sources->processes[previous].handle, state->processes[i].handle)) ++previous;
        if (previous == old_count) return snag_errno(EINVAL);
        sources->processes[i] = sources->processes[previous++];
    }
    sources->process_count = state->process_count;
    return 0;
}

static int
update_download_sources(struct snag_binary_checkpoint_sources *sources,
    const struct snag_session *state, uint64_t sequence, enum snag_binary_kind kind)
{
    if (kind != SNAG_BINARY_DOWNLOAD_QUEUED && kind != SNAG_BINARY_DOWNLOAD_REMOVED &&
        kind != SNAG_BINARY_DOWNLOADS_CLEARED) return 0;
    size_t count = json_array_size(state->download_queue), previous = 0u;
    size_t old_count = sources->download_count;
    if (kind == SNAG_BINARY_DOWNLOAD_QUEUED) {
        if (old_count == SIZE_MAX || count != old_count + 1u) return snag_errno(EINVAL);
        if (count > sources->download_capacity) {
            size_t capacity = sources->download_capacity;
            if (capacity > SIZE_MAX / 2u) return snag_errno(EOVERFLOW);
            capacity = capacity ? capacity * 2u : 8u;
            if (capacity > SIZE_MAX / sizeof(*sources->downloads)) return snag_errno(EOVERFLOW);
            void *grown = realloc(sources->downloads, capacity * sizeof(*sources->downloads));
            if (!grown) return -1;
            sources->downloads = grown;
            sources->download_capacity = capacity;
        }
        const char *id = snag_json_string(json_array_get(state->download_queue, old_count), "id");
        if (!id || !snag_hex_is_lower(id, SNAG_ID_HEX_LEN)) return snag_errno(EINVAL);
        sources->downloads[old_count].receipt = sequence;
        memcpy(sources->downloads[old_count].id, id, sizeof(sources->downloads[old_count].id));
        ++old_count;
    }
    for (size_t i = 0u; i < count; ++i) {
        const char *id = snag_json_string(json_array_get(state->download_queue, i), "id");
        if (!id) return snag_errno(EINVAL);
        while (previous < old_count && strcmp(id, sources->downloads[previous].id)) ++previous;
        if (previous == old_count) return snag_errno(EINVAL);
        sources->downloads[i] = sources->downloads[previous++];
    }
    sources->download_count = count;
    return 0;
}

static int
update_sources(struct snag_binary_checkpoint_sources *sources, const struct snag_session *state,
    uint64_t sequence, enum snag_binary_kind kind, const json_t *data)
{
    if (kind == SNAG_BINARY_SESSION_OPTIONS) sources->resume_options = sequence;
    if (!json_object_get(state->strings, "resume_options")) sources->resume_options = 0u;
    if (kind == SNAG_BINARY_RESPONSE_STARTED) {
        sources->response_start = sequence;
    }
    if (!state->active_response_id[0]) sources->response_start = 0u;
    if (kind == SNAG_BINARY_RESPONSE_OUTPUT) sources->response_end = sequence;
    if (!state->response_public) sources->response_end = 0u;
    if (kind == SNAG_BINARY_COMPACTION_STARTED) sources->active_compact = sequence;
    if (kind == SNAG_BINARY_COMPACTION_COMPLETED) {
        if (!sources->active_compact) return snag_errno(EINVAL);
        sources->compact_start = sources->active_compact;
        sources->compact_end = sequence;
    }
    if (!state->active_compact_id[0]) sources->active_compact = 0u;
    if (update_download_sources(sources, state, sequence, kind) < 0) return -1;
    if (kind == SNAG_BINARY_INPUT_RECEIVED ||
        (kind == SNAG_BINARY_IRC_ADMITTED && json_object_get(data, "input"))) {
        sources->input = sequence;
    }
    if (!state->pending_input) sources->input = 0u;

    if (kind == SNAG_BINARY_FUTURE_TURN_QUEUED) {
        if (sources->queue_count == SIZE_MAX ||
            state->pending_queue_count != sources->queue_count + 1u ||
            state->pending_queue[sources->queue_count].seq != sequence) {
            return snag_errno(EINVAL);
        }
        if (sources->queue_count == sources->queue_capacity) {
            size_t capacity = state->pending_queue_capacity;
            if (capacity <= sources->queue_count || capacity > SIZE_MAX / sizeof(*sources->queue))
                return snag_errno(EOVERFLOW);
            struct snag_binary_checkpoint_queue_source *grown =
                realloc(sources->queue, capacity * sizeof(*grown));
            if (!grown) return -1;
            sources->queue = grown;
            sources->queue_capacity = capacity;
        }
        sources->queue[sources->queue_count++] = (struct snag_binary_checkpoint_queue_source){
            .creation = sequence, .text = sequence};
    } else if (kind == SNAG_BINARY_FUTURE_TURN_EDITED) {
        const char *id = snag_json_string(data, "queue_id");
        if (!id || sources->queue_count != state->pending_queue_count) return snag_errno(EINVAL);
        for (size_t i = 0u; i < sources->queue_count; ++i) {
            if (strcmp(id, state->pending_queue[i].queue_id)) continue;
            if (sources->queue[i].creation != state->pending_queue[i].seq) {
                return snag_errno(EINVAL);
            }
            sources->queue[i].text = sequence;
            return 0;
        }
        return snag_errno(EINVAL);
    } else if (kind == SNAG_BINARY_FUTURE_TURN_CANCELLED || kind == SNAG_BINARY_TURN_STARTED) {
        /* Cancellation preserves order; a queued turn consumes the first item.
         * Non-queued turns leave this list alone. No completed history is kept. */
        size_t previous = 0u;
        for (size_t i = 0u; i < state->pending_queue_count; ++i) {
            uint64_t creation = state->pending_queue[i].seq;
            while (previous < sources->queue_count && sources->queue[previous].creation < creation)
                ++previous;
            if (previous == sources->queue_count || sources->queue[previous].creation != creation)
                return snag_errno(EINVAL);
            sources->queue[i] = sources->queue[previous++];
        }
        sources->queue_count = state->pending_queue_count;
    }
    return 0;
}

int
snag_binary_checkpoint_sources_step(struct snag_binary_checkpoint_sources *sources,
    const struct snag_session *state, const struct snag_binary_record *record,
    uint64_t sequence, const json_t *data)
{
    if (!sources || !state || !record || !data || record->flags ||
        !sequence || sequence == UINT64_MAX) return snag_errno(EINVAL);
    enum snag_binary_kind kind = (enum snag_binary_kind)record->kind;
    if (update_sources(sources, state, sequence, kind, data) < 0 ||
        snag_binary_checkpoint_texts_step(&sources->texts, record, sequence, state) < 0)
        return -1;
    if (!state->pending_call_count) {
        sources->calls = (struct snag_binary_checkpoint_call_source){0};
    } else if (kind == SNAG_BINARY_RESPONSE_COMPLETED) {
        sources->calls = (struct snag_binary_checkpoint_call_source){
            .graph = sequence, .cwd = sources->texts.slots[SNAG_BINARY_TEXT_CWD]};
    }
    return update_process_sources(sources, state, sequence, kind);
}
