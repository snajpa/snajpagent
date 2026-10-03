/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"
#include "store_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define CALL_HEADER 42u

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
valid_source(const struct snag_binary_checkpoint_call_source *source, size_t count)
{
    if (!source || source->cwd.original.field) return false;
    const struct snag_binary_ref *ref = &source->cwd.original.target;
    if (!count) {
        return !source->graph && !source->cwd.declaration &&
            !ref->sequence && !ref->offset && !ref->size;
    }
    unsigned char encoded[SNAG_BINARY_REF_SIZE];
    return source->graph && source->graph != UINT64_MAX && source->cwd.declaration &&
        source->cwd.declaration < source->graph && ref->sequence == source->cwd.declaration &&
        !snag_binary_ref_encode(encoded, ref);
}

int
snag_binary_checkpoint_calls_encode(struct snag_buf *out,
    const struct snag_binary_checkpoint_call_source *source, const struct snag_session *state)
{
    if (!out || !state || !valid_source(source, state->pending_call_count) ||
        (state->pending_call_count && !state->pending_calls)) return snag_errno(EINVAL);
    if (state->pending_call_count > SIZE_MAX - CALL_HEADER) return snag_errno(EOVERFLOW);
    unsigned char header[CALL_HEADER] = {1u};
    put_number(header + 2u, source->graph, 8u);
    put_number(header + 10u, source->cwd.declaration, 8u);
    if (state->pending_call_count &&
        snag_binary_ref_encode(header + 18u, &source->cwd.original.target) < 0) return -1;
    put_number(header + 34u, state->pending_call_count, 8u);
    struct snag_buf encoded = {.max = CALL_HEADER + state->pending_call_count};
    int rc = snag_buf_append(&encoded, header, sizeof(header));
    for (size_t i = 0u; !rc && i < state->pending_call_count; ++i) {
        const struct snag_pending_call *call = &state->pending_calls[i];
        rc = snag_buf_putc(&encoded, (call->started ? 1u : 0u) | (call->finished ? 2u : 0u));
    }
    if (!rc) rc = snag_buf_append(out, encoded.data, encoded.len);
    snag_buf_free(&encoded);
    return rc;
}

int
snag_binary_checkpoint_calls_decode(const void *data, size_t size,
    struct snag_binary_checkpoint_calls *out)
{
    if (!data || !out || size < CALL_HEADER) return snag_errno(EINVAL);
    const unsigned char *bytes = data;
    uint64_t count = get_number(bytes + 34u, 8u);
    if (get_number(bytes, 2u) != 1u || count != size - CALL_HEADER)
        return snag_errno(EINVAL);
    struct snag_binary_checkpoint_calls value = {.count = (size_t)count,
        .flags = bytes + CALL_HEADER};
    value.source.graph = get_number(bytes + 2u, 8u);
    value.source.cwd.declaration = get_number(bytes + 10u, 8u);
    if (count) {
        if (snag_binary_ref_decode(bytes + 18u, SNAG_BINARY_REF_SIZE,
                &value.source.cwd.original.target) < 0) return -1;
    } else {
        for (size_t i = 18u; i < 34u; ++i)
            if (bytes[i]) return snag_errno(EINVAL);
    }
    if (!valid_source(&value.source, value.count)) return snag_errno(EINVAL);
    for (size_t i = 0u; i < value.count; ++i)
        if (value.flags[i] & ~3u) return snag_errno(EINVAL);
    *out = value;
    return 0;
}

static int
find_record(int fd, const struct snag_binary_anchor *anchor, uint64_t wanted,
    struct snag_buf *scratch, struct snag_binary_batch *batch, struct snag_binary_record *record)
{
    struct snag_binary_anchor before;
    if (snag_binary_batch_find(fd, anchor, wanted, scratch, batch, &before) < 0) return -1;
    size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
    uint64_t sequence;
    int rc;
    while ((rc = snag_binary_record_next(batch, &offset, record, &sequence)) == 0)
        if (sequence == wanted) return record->flags ? snag_errno(EINVAL) : 0;
    return rc < 0 ? -1 : snag_errno(EINVAL);
}

static void
bytes_hex(char *out, const unsigned char *id, size_t size)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < size; ++i) {
        out[i * 2u] = hex[id[i] >> 4u];
        out[i * 2u + 1u] = hex[id[i] & 15u];
    }
    out[size * 2u] = 0;
}

static int
read_call(const struct snag_binary_call *source, const char *cwd, struct snag_pending_call *out)
{
    char name[sizeof(out->tool_name)];
    if (source->name.size >= sizeof(name)) return snag_errno(EINVAL);
    memcpy(name, source->name.data, source->name.size);
    name[source->name.size] = 0;
    char id[33];
    bytes_hex(id, source->id, 16u);
    json_t *arguments = snag_json_load_canonical_bounded(source->arguments.data,
        source->arguments.size, SNAG_MAX_TOOL_ARGUMENTS, NULL, 0u);
    if (!arguments) return -1;
    struct snag_response_item item = {.kind = SNAG_ITEM_TOOL_CALL, .call_id = id,
        .name = name, .arguments = arguments};
    int rc = snag_pending_call_from_item(&item, cwd, out);
    json_decref(arguments);
    return rc;
}

static int
read_graph(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_call_source *source, struct snag_buf *scratch,
    struct snag_binary_event *event, char **cwd)
{
    struct snag_binary_batch batch;
    struct snag_binary_record record;
    if (find_record(fd, anchor, source->cwd.declaration, scratch, &batch, &record) < 0) return -1;
    if (record.kind != SNAG_BINARY_SESSION_CREATED && record.kind != SNAG_BINARY_CWD_CHANGED)
        return snag_errno(EINVAL);
    struct snag_binary_control_text_source directory;
    if (snag_binary_control_text_ref_resolve(&source->cwd.original.target,
            &batch, (enum snag_binary_kind)record.kind, &directory) < 0) return -1;
    *cwd = malloc(directory.text.size + 1u);
    if (!*cwd) return -1;
    memcpy(*cwd, directory.text.data, directory.text.size);
    (*cwd)[directory.text.size] = 0;
    if (find_record(fd, anchor, source->graph, scratch, &batch, &record) < 0) return -1;
    if (record.kind != SNAG_BINARY_RESPONSE_COMPLETED) return snag_errno(EINVAL);
    int decoded = snag_binary_event_decode(&record, event);
    return decoded > 0 ? snag_errno(EINVAL) : decoded;
}

/* Digests are mutable: accepted rule transformations replace them before
 * dispatch while labels keep the original graph's previews. Walk only this
 * graph's causal window, with bounded batch scratch and no session-wide table. */
static int
apply_transforms(int fd, const struct snag_binary_anchor *anchor, uint64_t graph,
    uint64_t end, struct snag_pending_call *calls, size_t count)
{
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor before, next;
    int rc = snag_binary_batch_find(fd, anchor, graph, &scratch, &batch, &before);
    if (rc < 0) goto done;
    do {
        int batch_rc = snag_binary_batch_read(fd, anchor->end, &before, &scratch, &batch, &next);
        if (batch_rc != 0) {
            rc = batch_rc < 0 ? -1 : snag_errno(EIO);
            goto done;
        }
        size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
        struct snag_binary_record record;
        uint64_t sequence;
        int record_rc;
        while ((record_rc = snag_binary_record_next(&batch, &offset, &record, &sequence)) == 0) {
            if (sequence <= graph) continue;
            if (sequence >= end) goto done;
            if (record.kind == SNAG_BINARY_TURN_STARTED ||
                record.kind == SNAG_BINARY_RESPONSE_STARTED ||
                record.kind == SNAG_BINARY_RESPONSE_COMPLETED ||
                record.kind == SNAG_BINARY_TURN_COMPLETED ||
                record.kind == SNAG_BINARY_TURN_COMPLETED_SILENT ||
                record.kind == SNAG_BINARY_TURN_INTERRUPTED ||
                record.kind == SNAG_BINARY_TURN_FAILED) {
                rc = snag_errno(EINVAL);
                goto done;
            }
            if (record.kind != SNAG_BINARY_RULE_TRANSFORM) continue;
            if (record.flags) { rc = snag_errno(EINVAL); goto done; }
            struct snag_binary_event event;
            int decoded = snag_binary_event_decode(&record, &event);
            if (decoded != 0) { rc = decoded < 0 ? -1 : snag_errno(EINVAL); goto done; }
            const struct snag_binary_rule_transform *transform = &event.data.rule_transform;
            char id[33], original[65];
            bytes_hex(id, transform->call, 16u);
            bytes_hex(original, transform->original_sha256, 32u);
            for (size_t i = 0u; i < count; ++i) {
                if (strcmp(calls[i].call_id, id)) continue;
                if (strcmp(calls[i].action_sha256, original)) {
                    rc = snag_errno(EINVAL);
                    goto done;
                }
                bytes_hex(calls[i].action_sha256, transform->effective_sha256, 32u);
            }
        }
        if (record_rc < 0) { rc = -1; goto done; }
        before = next;
    } while (next.next_seq < end);
done:
    snag_buf_free(&scratch);
    return rc;
}

int
snag_binary_checkpoint_calls_read(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_calls *calls, const struct snag_session *state,
    struct snag_pending_call **out)
{
    if (fd < 0 || !anchor || !anchor->next_seq || !calls || !state || !out ||
        !valid_source(&calls->source, calls->count) ||
        calls->source.graph >= anchor->next_seq || (calls->count && !calls->flags))
        return snag_errno(EINVAL);
    if (calls->count > SIZE_MAX / sizeof(struct snag_pending_call)) return snag_errno(EOVERFLOW);
    for (size_t i = 0u; i < calls->count; ++i)
        if (calls->flags[i] & ~3u) return snag_errno(EINVAL);
    if (!calls->count) {
        *out = NULL;
        return 0;
    }
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_pending_call *result = NULL;
    char *cwd = NULL;
    int rc = -1;
    struct snag_binary_event event;
    if (read_graph(fd, anchor, &calls->source, &scratch, &event, &cwd) < 0) goto done;
    const struct snag_binary_response_complete *graph = &event.data.response_completed;
    char turn[33], response[33];
    bytes_hex(turn, graph->turn, 16u);
    bytes_hex(response, graph->response, 16u);
    if (strcmp(turn, state->active_turn_id) || strcmp(response, state->active_response_id) ||
        graph->cycle != state->active_cycle || calls->count > graph->items.count) {
        snag_errno(EINVAL);
        goto done;
    }
    result = calloc(calls->count, sizeof(*result));
    if (!result) goto done;
    size_t offset = 0u, count = 0u;
    struct snag_binary_graph_item item;
    int next;
    while ((next = snag_binary_graph_items_next(&graph->items, &offset, &item)) == 0) {
        if (item.kind != SNAG_BINARY_ITEM_TOOL_CALL) continue;
        if (count == calls->count) {
            snag_errno(EINVAL);
            goto done;
        }
        if (read_call(&item.data.call, cwd, &result[count]) < 0) goto done;
        result[count].started = (calls->flags[count] & 1u) != 0u;
        result[count].finished = (calls->flags[count] & 2u) != 0u;
        ++count;
    }
    if (next < 0) goto done;
    if (count != calls->count) {
        snag_errno(EINVAL);
        goto done;
    }
    if (apply_transforms(fd, anchor, calls->source.graph, anchor->next_seq,
            result, calls->count) < 0) goto done;
    *out = result;
    result = NULL;
    rc = 0;
done:
    free(result);
    free(cwd);
    snag_buf_free(&scratch);
    return rc;
}

int
snag_binary_checkpoint_process_source_read(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_process_source *source, const struct snag_session *state,
    struct snag_process_state *out)
{
    if (fd < 0 || !anchor || !source || !state || !state->active_turn || !out ||
        !valid_source(&source->call, 1u) ||
        source->started <= source->call.graph || source->started >= anchor->next_seq ||
        (source->handle[0] && (!memchr(source->handle, 0, sizeof(source->handle)) ||
         !snag_hex_is_lower(source->handle, SNAG_ID_HEX_LEN)))) return snag_errno(EINVAL);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_record record;
    char *cwd = NULL;
    int rc = -1;
    if (find_record(fd, anchor, source->started, &scratch, &batch, &record) < 0) goto done;
    if (record.kind != SNAG_BINARY_TOOL_STARTED) {
        snag_errno(EINVAL);
        goto done;
    }
    struct snag_binary_event event;
    int decoded = snag_binary_event_decode(&record, &event);
    if (decoded != 0) {
        if (decoded > 0) snag_errno(EINVAL);
        goto done;
    }
    struct snag_binary_tool_start start = event.data.tool_started;
    if (read_graph(fd, anchor, &source->call, &scratch, &event, &cwd) < 0) goto done;
    const struct snag_binary_response_complete *graph = &event.data.response_completed;
    char turn[33], action[65];
    bytes_hex(turn, start.turn, 16u);
    bytes_hex(action, start.action_sha256, 32u);
    if (memcmp(start.turn, graph->turn, 16u) || strcmp(turn, state->active_turn_id)) {
        snag_errno(EINVAL);
        goto done;
    }
    size_t offset = 0u;
    struct snag_binary_graph_item item;
    int next;
    while ((next = snag_binary_graph_items_next(&graph->items, &offset, &item)) == 0) {
        if (item.kind != SNAG_BINARY_ITEM_TOOL_CALL || memcmp(item.data.call.id, start.call, 16u))
            continue;
        struct snag_pending_call call;
        if (read_call(&item.data.call, cwd, &call) < 0 ||
            apply_transforms(fd, anchor, source->call.graph, source->started, &call, 1u) < 0)
            goto done;
        if (strcmp(action, call.action_sha256) || !*call.process_handle ||
            (strcmp(call.tool_name, "exec_command") &&
             (!state->legacy_journal || strcmp(call.tool_name, "write_stdin"))) ||
            (*source->handle && strcmp(source->handle, call.process_handle))) {
            snag_errno(EINVAL);
            goto done;
        }
        struct snag_process_state value = {0};
        memcpy(value.handle, call.process_handle, sizeof(value.handle));
        memcpy(value.command, call.command, sizeof(value.command));
        memcpy(value.workdir, call.workdir, sizeof(value.workdir));
        *out = value;
        rc = 0;
        goto done;
    }
    if (next >= 0) snag_errno(EINVAL);
done:
    free(cwd);
    snag_buf_free(&scratch);
    return rc;
}
