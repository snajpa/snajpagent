/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define PROCESS_HEADER 10u
#define PROCESS_ENTRY 97u

static const size_t counters[] = {
    offsetof(struct snag_process_state, output_bytes[0]),
    offsetof(struct snag_process_state, output_bytes[1]),
    offsetof(struct snag_process_state, collected_bytes[0]),
    offsetof(struct snag_process_state, collected_bytes[1]),
    offsetof(struct snag_process_state, input_accepted),
    offsetof(struct snag_process_state, input_written),
    offsetof(struct snag_process_state, input_pending)
};

void
snag_binary_checkpoint_sources_free(struct snag_binary_checkpoint_sources *sources)
{
    if (!sources) return;
    free(sources->processes);
    free(sources->queue);
    free(sources->downloads);
    memset(sources, 0, sizeof(*sources));
}

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
read_entry(const unsigned char *bytes, uint64_t previous,
    struct snag_binary_checkpoint_process_source *source, struct snag_process_state *state)
{
    struct snag_binary_checkpoint_process_source origin = {.started = get_number(bytes, 8u)};
    origin.call.graph = get_number(bytes + 8u, 8u);
    origin.call.cwd.declaration = get_number(bytes + 16u, 8u);
    if (snag_binary_ref_decode(bytes + 24u, SNAG_BINARY_REF_SIZE,
            &origin.call.cwd.original.target) < 0) return -1;
    if (!valid_source(&origin, previous) || bytes[96] & ~3u) return snag_errno(EINVAL);
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
    unsigned char bytes[PROCESS_ENTRY] = {1u};
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
    if (get_number(bytes, 2u) != 1u || (size - PROCESS_HEADER) % PROCESS_ENTRY ||
        count != (size - PROCESS_HEADER) / PROCESS_ENTRY) return snag_errno(EINVAL);
    struct snag_binary_checkpoint_processes value = {.data = bytes + PROCESS_HEADER,
        .count = (size_t)count};
    uint64_t previous = 0u;
    for (size_t i = 0u; i < value.count; ++i) {
        struct snag_binary_checkpoint_process_source source;
        struct snag_process_state state;
        if (read_entry(value.data + i * PROCESS_ENTRY, previous, &source, &state) < 0) return -1;
        previous = source.started;
    }
    *out = value;
    return 0;
}

int
snag_binary_checkpoint_processes_origin(const struct snag_binary_checkpoint_processes *view,
    size_t index, struct snag_binary_checkpoint_process_source *out)
{
    if (!view || !out || !view->data || index >= view->count ||
        view->count > SIZE_MAX / PROCESS_ENTRY) return snag_errno(EINVAL);
    struct snag_process_state metadata;
    return read_entry(view->data + index * PROCESS_ENTRY, 0u, out, &metadata);
}

int
snag_binary_checkpoint_processes_read(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_index *access,
    const struct snag_binary_checkpoint_processes *view, const struct snag_session *state,
    struct snag_process_state **out)
{
    if (fd < 0 || !anchor || !anchor->next_seq || !view || !state || !out ||
        (view->count && !view->data)) return snag_errno(EINVAL);
    if (view->count > SIZE_MAX / sizeof(struct snag_process_state) ||
        view->count > SIZE_MAX / PROCESS_ENTRY) return snag_errno(EOVERFLOW);
    struct snag_process_state *processes = view->count ?
        calloc(view->count, sizeof(*processes)) : NULL;
    if (view->count && !processes) return -1;
    uint64_t previous = 0u;
    for (size_t i = 0u; i < view->count; ++i) {
        struct snag_binary_checkpoint_process_source source;
        struct snag_process_state metadata;
        if (read_entry(view->data + i * PROCESS_ENTRY, previous, &source, &metadata) < 0 ||
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
