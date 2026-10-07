/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"
#include "store_binary_event.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define PROCESS_HEADER 10u
#define PROCESS_ENTRY_V1 97u
#define PROCESS_ENTRY 105u

static size_t
entry_size(uint16_t version)
{
    return version == 1u ? PROCESS_ENTRY_V1 : version == 2u ? PROCESS_ENTRY : 0u;
}

static const size_t counters[] = {
    offsetof(struct snag_process_state, output_bytes[0]),
    offsetof(struct snag_process_state, output_bytes[1]),
    offsetof(struct snag_process_state, collected_bytes[0]),
    offsetof(struct snag_process_state, collected_bytes[1]),
    offsetof(struct snag_process_state, input_accepted),
    offsetof(struct snag_process_state, input_written),
    offsetof(struct snag_process_state, input_pending)
};

static void
put_number(unsigned char *out, uint64_t value, size_t size)
{
    for (size_t i = 0u; i < size; ++i) out[i] = (unsigned char)(value >> (i * 8u));
}

static uint64_t
get_number(const unsigned char *data, size_t size)
{
    uint64_t value = 0u;
    for (size_t i = 0u; i < size; ++i) value |= (uint64_t)data[i] << (i * 8u);
    return value;
}

static bool
valid_source(const struct snag_binary_checkpoint_process_source *source, uint64_t previous)
{
    const struct snag_binary_ref *ref = &source->call.cwd.original.target;
    unsigned char wire[SNAG_BINARY_REF_SIZE];
    return source->started > previous && source->started != UINT64_MAX &&
        source->started > source->call.graph && source->call.graph > source->call.cwd.declaration &&
        source->call.cwd.declaration && !source->call.cwd.original.field &&
        ref->sequence == source->call.cwd.declaration && !snag_binary_ref_encode(wire, ref);
}

static int
read_entry(const unsigned char *bytes, uint64_t previous, uint16_t version,
    struct snag_binary_checkpoint_process_source *source, struct snag_process_state *state)
{
    struct snag_binary_checkpoint_process_source origin = {.started = get_number(bytes, 8u)};
    origin.scan = version == 1u ? origin.started : get_number(bytes + PROCESS_ENTRY_V1, 8u);
    origin.call.graph = get_number(bytes + 8u, 8u);
    origin.call.cwd.declaration = get_number(bytes + 16u, 8u);
    if (snag_binary_ref_decode(bytes + 24u, SNAG_BINARY_REF_SIZE,
            &origin.call.cwd.original.target) < 0) return -1;
    if (!valid_source(&origin, previous) || origin.scan < origin.started ||
        origin.scan == UINT64_MAX || bytes[96] & ~3u) return snag_errno(EINVAL);
    struct snag_process_state value = {0};
    for (size_t i = 0u; i < sizeof(counters) / sizeof(*counters); ++i) {
        uint64_t number = get_number(bytes + 40u + i * 8u, 8u);
        memcpy((unsigned char *)&value + counters[i], &number, sizeof(number));
    }
    value.ready = (bytes[96] & 1u) != 0u;
    value.draining = (bytes[96] & 2u) != 0u;
    *source = origin;
    *state = value;
    return 0;
}

int
snag_binary_checkpoint_processes_encode(struct snag_buf *out,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state)
{
    if (!out || !sources || !state || sources->process_count != state->process_count ||
        (state->process_count && (!state->processes || !sources->processes)))
        return snag_errno(EINVAL);
    if (state->process_count > (SIZE_MAX - PROCESS_HEADER) / PROCESS_ENTRY)
        return snag_errno(EOVERFLOW);
    struct snag_buf encoded = {.max = PROCESS_HEADER + state->process_count * PROCESS_ENTRY};
    unsigned char bytes[PROCESS_ENTRY] = {2u};
    put_number(bytes + 2u, state->process_count, 8u);
    int rc = snag_buf_append(&encoded, bytes, PROCESS_HEADER);
    uint64_t previous = 0u;
    for (size_t i = 0u; !rc && i < state->process_count; ++i) {
        const struct snag_binary_checkpoint_process_source *source = &sources->processes[i];
        const struct snag_process_state *process = &state->processes[i];
        if (!valid_source(source, previous) || !memchr(source->handle, 0, sizeof(source->handle)) ||
            !snag_hex_is_lower(source->handle, SNAG_ID_HEX_LEN) ||
            memcmp(source->handle, process->handle, sizeof(source->handle))) {
            rc = snag_errno(EINVAL);
            break;
        }
        put_number(bytes, source->started, 8u);
        put_number(bytes + 8u, source->call.graph, 8u);
        put_number(bytes + 16u, source->call.cwd.declaration, 8u);
        rc = snag_binary_ref_encode(bytes + 24u, &source->call.cwd.original.target);
        for (size_t j = 0u; j < sizeof(counters) / sizeof(*counters); ++j) {
            uint64_t value;
            memcpy(&value, (const unsigned char *)process + counters[j], sizeof(value));
            put_number(bytes + 40u + j * 8u, value, 8u);
        }
        bytes[96] = (process->ready ? 1u : 0u) | (process->draining ? 2u : 0u);
        uint64_t scan = source->scan ? source->scan : source->started;
        if (scan < source->started || scan == UINT64_MAX) rc = snag_errno(EINVAL);
        put_number(bytes + PROCESS_ENTRY_V1, scan, 8u);
        if (!rc) rc = snag_buf_append(&encoded, bytes, sizeof(bytes));
        previous = source->started;
    }
    if (!rc) rc = snag_buf_append(out, encoded.data, encoded.len);
    snag_buf_free(&encoded);
    return rc;
}

int
snag_binary_checkpoint_processes_decode(const void *data, size_t size,
    struct snag_binary_checkpoint_processes *out)
{
    if (!data || !out || size < PROCESS_HEADER) return snag_errno(EINVAL);
    const unsigned char *bytes = data;
    uint64_t count = get_number(bytes + 2u, 8u);
    uint16_t version = (uint16_t)get_number(bytes, 2u);
    size_t entry = entry_size(version);
    if (!entry || (size - PROCESS_HEADER) % entry ||
        count != (size - PROCESS_HEADER) / entry) return snag_errno(EINVAL);
    struct snag_binary_checkpoint_processes value = {.data = bytes + PROCESS_HEADER,
        .count = (size_t)count, .version = version};
    uint64_t previous = 0u;
    for (size_t i = 0u; i < value.count; ++i) {
        struct snag_binary_checkpoint_process_source source;
        struct snag_process_state state;
        if (read_entry(value.data + i * entry, previous, version, &source, &state) < 0) return -1;
        previous = source.started;
    }
    *out = value;
    return 0;
}

int
snag_binary_checkpoint_processes_origin(const struct snag_binary_checkpoint_processes *view,
    size_t index, struct snag_binary_checkpoint_process_source *out)
{
    if (!view || !out || !view->data || index >= view->count) return snag_errno(EINVAL);
    size_t entry = entry_size(view->version);
    if (!entry || view->count > SIZE_MAX / entry) return snag_errno(EINVAL);
    struct snag_process_state metadata;
    return read_entry(view->data + index * entry, 0u, view->version, out, &metadata);
}

int
snag_binary_checkpoint_processes_read(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_index *access,
    const struct snag_binary_checkpoint_processes *view, const struct snag_session *state,
    struct snag_process_state **out)
{
    if (fd < 0 || !anchor || !anchor->next_seq || !view || !state || !out ||
        (view->count && !view->data)) return snag_errno(EINVAL);
    size_t entry = entry_size(view->version);
    if (!entry) return snag_errno(EINVAL);
    if (view->count > SIZE_MAX / sizeof(struct snag_process_state) ||
        view->count > SIZE_MAX / entry) return snag_errno(EOVERFLOW);
    struct snag_process_state *processes = view->count ?
        calloc(view->count, sizeof(*processes)) : NULL;
    if (view->count && !processes) return -1;
    uint64_t previous = 0u;
    for (size_t i = 0u; i < view->count; ++i) {
        struct snag_binary_checkpoint_process_source source;
        struct snag_process_state metadata;
        if (read_entry(view->data + i * entry, previous, view->version, &source, &metadata) < 0 ||
            snag_binary_checkpoint_process_source_read(fd, anchor, access, &source, state,
                &processes[i]) < 0) goto fail;
        for (size_t j = 0u; j < i; ++j) {
            if (!strcmp(processes[j].handle, processes[i].handle)) {
                snag_errno(EINVAL);
                goto fail;
            }
        }
        for (size_t j = 0u; j < sizeof(counters) / sizeof(*counters); ++j) {
            memcpy((unsigned char *)&processes[i] + counters[j],
                (const unsigned char *)&metadata + counters[j], sizeof(uint64_t));
        }
        processes[i].ready = metadata.ready;
        processes[i].draining = metadata.draining;
        previous = source.started;
    }
    *out = processes;
    return 0;
fail:
    free(processes);
    return -1;
}

static bool
id_matches(const unsigned char id[16], const char *hex)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0u; i < 16u; ++i) {
        if (hex[i * 2u] != digits[id[i] >> 4u] ||
            hex[i * 2u + 1u] != digits[id[i] & 15u]) return false;
    }
    return !hex[SNAG_ID_HEX_LEN];
}

int
snag_binary_checkpoint_processes_cursors(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access,
    const struct snag_binary_checkpoint_sources *sources, struct snag_session *state,
    bool (*cancelled)(void *), void *opaque)
{
    if (fd < 0 || !through || !access || !sources || !state ||
        sources->process_count != state->process_count ||
        (state->process_count && (!sources->processes || !state->processes)))
        return snag_errno(EINVAL);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    int rc = -1;
    for (size_t i = 0u; i < state->process_count; ++i) {
        if (cancelled && cancelled(opaque)) { snag_errno(ECANCELED); goto done; }
        const struct snag_binary_checkpoint_process_source *source = &sources->processes[i];
        struct snag_process_state *process = &state->processes[i];
        uint64_t scan = source->scan ? source->scan : source->started;
        if (!source->started || scan < source->started || scan >= through->next_seq ||
            strcmp(source->handle, process->handle)) { snag_errno(EINVAL); goto done; }
        struct snag_binary_batch batch, checked;
        struct snag_binary_anchor before, after;
        if (snag_binary_checkpoint_batch_find(fd, through, access, scan,
                &scratch, &batch, &before) < 0 ||
            snag_binary_batch_decode(batch.data, batch.size, &before, &checked, &after) != 0)
            goto done;
        struct snag_binary_record record;
        uint64_t sequence;
        size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
        bool found = false;
        while (snag_binary_record_next(&checked, &offset, &record, &sequence) == 0) {
            if (sequence != scan) continue;
            struct snag_binary_event event;
            if (record.flags || snag_binary_event_decode(&record, &event) != 0) {
                snag_errno(EINVAL); goto done;
            }
            if (scan == source->started) {
                struct snag_process_state original;
                if (snag_binary_checkpoint_process_source_read(fd, through, access, source,
                    state, &original) < 0) goto done;
                /* Imported write_stdin starts use a call ID distinct from the
                 * process handle. The original graph proves that association. */
                found = event.kind == SNAG_BINARY_TOOL_STARTED &&
                    !strcmp(original.handle, process->handle);
            } else if (event.kind == SNAG_BINARY_TOOL_FINISHED) {
                const struct snag_binary_tool_finish *finish = &event.data.tool_finished;
                found = id_matches(finish->call, process->handle) ||
                    (finish->result.has_handle &&
                        id_matches(finish->result.handle, process->handle)) ||
                    (finish->result.has_output_ref &&
                        id_matches(finish->result.output_ref.handle, process->handle));
            }
            break;
        }
        if (!found) { snag_errno(EINVAL); goto done; }
        struct snag_binary_cursor cursor;
        if (snag_binary_cursor_capture(&before, &after, &checked, scan + 1u, &cursor) < 0)
            goto done;
        process->log_offset = cursor.before.end;
        process->log_seq = cursor.next_seq;
        static const char digits[] = "0123456789abcdef";
        for (size_t j = 0u; j < sizeof(cursor.before.digest); ++j) {
            process->log_hash[j * 2u] = digits[cursor.before.digest[j] >> 4u];
            process->log_hash[j * 2u + 1u] = digits[cursor.before.digest[j] & 15u];
        }
        process->log_hash[SNAG_SHA256_HEX_LEN] = '\0';
    }
    rc = 0;
done:
    snag_buf_free(&scratch);
    return rc;
}
