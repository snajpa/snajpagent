/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"
#include "store_binary_legacy.h"
#include "store_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define PAYLOAD_HEADER 59u

static uint64_t
number(const unsigned char *data)
{
    uint64_t value = 0u;
    for (size_t i = 0u; i < 8u; ++i) value |= (uint64_t)data[i] << (i * 8u);
    return value;
}

static void
put_number(unsigned char *data, uint64_t value)
{
    for (size_t i = 0u; i < 8u; ++i) data[i] = (unsigned char)(value >> (i * 8u));
}

static bool
valid(const struct snag_binary_checkpoint_payloads *view)
{
    if (!view || view->turn == UINT64_MAX || view->compact_end == UINT64_MAX ||
        view->response_end == UINT64_MAX || view->resume_options == UINT64_MAX ||
        (!!view->compact_start != !!view->compact_end) ||
        (view->compact_end && view->compact_start >= view->compact_end) ||
        (!!view->response_start != !!view->response_end) ||
        (view->response_end && (!view->turn || view->response_start <= view->turn ||
            view->response_end <= view->response_start)) ||
        view->download_count > (SIZE_MAX - PAYLOAD_HEADER) / 8u ||
        (view->download_count && (!view->downloads || !view->downloads_present))) return false;
    uint64_t previous = 0u;
    for (size_t i = 0u; i < view->download_count; ++i) {
        uint64_t receipt = number(view->downloads + i * 8u);
        if (receipt <= previous || receipt == UINT64_MAX) return false;
        previous = receipt;
    }
    return true;
}

int
snag_binary_checkpoint_payloads_decode(const void *data, size_t size,
    struct snag_binary_checkpoint_payloads *out)
{
    if (!data || !out || size < PAYLOAD_HEADER) return snag_errno(EINVAL);
    const unsigned char *bytes = data;
    uint64_t count = number(bytes + 42u);
    if (bytes[0] != 1u || bytes[1] || bytes[50] > 1u ||
        count > (SIZE_MAX - PAYLOAD_HEADER) / 8u ||
        size != PAYLOAD_HEADER + (size_t)count * 8u) return snag_errno(EINVAL);
    struct snag_binary_checkpoint_payloads value = {
        .turn = number(bytes + 2u), .compact_start = number(bytes + 10u),
        .compact_end = number(bytes + 18u), .response_start = number(bytes + 26u),
        .response_end = number(bytes + 34u), .download_count = (size_t)count,
        .downloads_present = bytes[50] != 0u, .downloads = bytes + PAYLOAD_HEADER,
        .resume_options = number(bytes + 51u)};
    if (!valid(&value)) return snag_errno(EINVAL);
    *out = value;
    return 0;
}

int
snag_binary_checkpoint_payloads_encode(struct snag_buf *out,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state)
{
    if (!out || !sources || !state) return snag_errno(EINVAL);
    json_t *options = json_object_get(state->strings, "resume_options");
    if ((options && !snag_session_options_valid(options)) ||
        (!!options != !!sources->resume_options) ||
        sources->download_count != json_array_size(state->download_queue) ||
        (sources->download_count && !sources->downloads) ||
        (state->download_queue && !json_is_array(state->download_queue)) ||
        (!!sources->compact_end != !!state->compact_output) ||
        (!!sources->response_end != !!state->response_public) ||
        (!!state->active_instructions != state->active_turn) ||
        sources->download_count > (SIZE_MAX - PAYLOAD_HEADER) / 8u) return snag_errno(EINVAL);
    uint64_t turn = state->active_instructions ?
        sources->texts.slots[SNAG_BINARY_TEXT_ACTIVE_PROMPT].declaration : 0u;
    if (!!turn != state->active_turn) return snag_errno(EINVAL);
    unsigned char header[PAYLOAD_HEADER] = {1u};
    put_number(header + 2u, turn);
    put_number(header + 10u, sources->compact_start);
    put_number(header + 18u, sources->compact_end);
    put_number(header + 26u, sources->response_end ? sources->response_start : 0u);
    put_number(header + 34u, sources->response_end);
    put_number(header + 42u, sources->download_count);
    header[50] = state->download_queue != NULL;
    put_number(header + 51u, sources->resume_options);
    struct snag_buf staged = {.max = out->max};
    int rc = snag_buf_append(&staged, header, sizeof(header));
    for (size_t i = 0u; rc == 0 && i < sources->download_count; ++i) {
        unsigned char entry[8];
        put_number(entry, sources->downloads[i].receipt);
        rc = snag_buf_append(&staged, entry, sizeof(entry));
    }
    struct snag_binary_checkpoint_payloads view;
    if (rc == 0) rc = snag_binary_checkpoint_payloads_decode(staged.data, staged.len, &view);
    if (rc == 0) rc = snag_buf_append(out, staged.data, staged.len);
    snag_buf_free(&staged);
    return rc;
}

void
snag_binary_checkpoint_payloads_free(struct snag_binary_checkpoint_payloads_state *value)
{
    if (!value) return;
    json_decref(value->instructions);
    json_decref(value->compact_output);
    json_decref(value->response_public);
    json_decref(value->downloads);
    json_decref(value->resume_options);
    memset(value, 0, sizeof(*value));
}

static bool
hex_equal(const unsigned char *bytes, size_t size, const char *text)
{
    static const char hex[] = "0123456789abcdef";
    if (!text || strlen(text) != size * 2u) return false;
    for (size_t i = 0u; i < size; ++i) {
        if (text[i * 2u] != hex[bytes[i] >> 4u] ||
            text[i * 2u + 1u] != hex[bytes[i] & 15u]) return false;
    }
    return true;
}

static int
find_record(int fd, const struct snag_binary_anchor *through, uint64_t wanted,
    enum snag_binary_kind kind, struct snag_buf *scratch, struct snag_binary_batch *batch,
    struct snag_binary_record *record, struct snag_binary_event *event)
{
    struct snag_binary_anchor before;
    if (!wanted || wanted >= through->next_seq) return snag_errno(EINVAL);
    if (snag_binary_batch_find(fd, through, wanted, scratch, batch, &before) < 0) return -1;
    size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
    uint64_t sequence;
    int rc;
    while ((rc = snag_binary_record_next(batch, &offset, record, &sequence)) == 0) {
        if (sequence != wanted) continue;
        if (record->kind != kind || record->flags) return snag_errno(EINVAL);
        return snag_binary_event_decode(record, event);
    }
    return rc < 0 ? -1 : snag_errno(EINVAL);
}

int
snag_binary_checkpoint_epochs_check(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state)
{
    if (fd < 0 || !through || !sources || !state ||
        !!sources->response_start != !!state->active_response_id[0] ||
        !!sources->active_compact != !!state->active_compact_id[0] ||
        sources->response_start > sources->texts.through ||
        sources->active_compact > sources->texts.through) return snag_errno(EINVAL);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_record record;
    struct snag_binary_event event;
    int rc = -1;
    if (sources->response_start) {
        uint64_t turn = sources->texts.slots[SNAG_BINARY_TEXT_ACTIVE_PROMPT].declaration;
        if (!turn || sources->response_start <= turn) { snag_errno(EINVAL); goto done; }
        if (find_record(fd, through, sources->response_start, SNAG_BINARY_RESPONSE_STARTED,
                &scratch, &batch, &record, &event) < 0) goto done;
        const struct snag_binary_response_start *start = &event.data.response_started;
        if (!hex_equal(start->turn, sizeof(start->turn), state->active_turn_id) ||
            !hex_equal(start->response, sizeof(start->response), state->active_response_id) ||
            start->cycle != state->active_cycle) { snag_errno(EINVAL); goto done; }
    }
    if (sources->active_compact) {
        if (sources->active_compact <= sources->compact_end) { snag_errno(EINVAL); goto done; }
        if (find_record(fd, through, sources->active_compact, SNAG_BINARY_COMPACTION_STARTED,
                &scratch, &batch, &record, &event) < 0) goto done;
        const struct snag_binary_compact_start *start = &event.data.compaction_started;
        if (!hex_equal(start->id, sizeof(start->id), state->active_compact_id) ||
            !hex_equal(start->source_sha256, sizeof(start->source_sha256),
                state->active_compact_source_sha256) ||
            start->source_seq != state->active_compact_source_seq ||
            start->source_seq >= sources->active_compact ||
            (start->has_scope ? !hex_equal(start->scope, sizeof(start->scope),
                state->active_compact_scope) : state->active_compact_scope[0] != '\0')) {
            snag_errno(EINVAL);
            goto done;
        }
    }
    rc = 0;
done:
    snag_buf_free(&scratch);
    return rc;
}

static int
read_instructions(int fd, const struct snag_binary_anchor *through, uint64_t sequence,
    const struct snag_session *state, struct snag_buf *scratch, json_t **out)
{
    struct snag_binary_batch batch;
    struct snag_binary_record record;
    struct snag_binary_event event;
    if (find_record(fd, through, sequence, SNAG_BINARY_TURN_STARTED,
        scratch, &batch, &record, &event) < 0) return -1;
    struct snag_binary_turn_start *turn = &event.data.started;
    if (!hex_equal(turn->id, sizeof(turn->id), state->active_turn_id) ||
        turn->number != state->turn_count) return snag_errno(EINVAL);
    struct snag_binary_instructions instructions = turn->instructions;
    struct snag_binary_input_reference reference = turn->instructions_ref;
    if (reference.field) {
        if (reference.field != SNAG_BINARY_INPUT_INSTRUCTIONS ||
            reference.target.sequence >= sequence) return snag_errno(EINVAL);
        struct snag_binary_anchor before;
        if (snag_binary_batch_find(fd, through, reference.target.sequence,
            scratch, &batch, &before) < 0) return -1;
        const unsigned char *bytes;
        if (snag_binary_input_ref_resolve(&reference.target, &batch, reference.field, &bytes) < 0 ||
            snag_binary_instructions_decode(bytes, reference.target.size, &instructions) < 0) {
            return -1;
        }
    }
    return snag_binary_instructions_legacy(&instructions, true, out);
}

static int
read_options(int fd, const struct snag_binary_anchor *through, uint64_t sequence,
    struct snag_buf *scratch, json_t **out)
{
    struct snag_binary_batch batch;
    struct snag_binary_record record;
    struct snag_binary_event event;
    if (find_record(fd, through, sequence, SNAG_BINARY_SESSION_OPTIONS,
        scratch, &batch, &record, &event) < 0) return -1;
    const char *type;
    json_t *data = NULL;
    if (snag_binary_legacy_decode(&record, &type, &data) < 0) return -1;
    *out = json_incref(json_object_get(data, "args"));
    json_decref(data);
    return 0;
}

static int
read_compaction(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_payloads *view, const struct snag_session *state,
    struct snag_buf *scratch, json_t **out)
{
    struct snag_binary_batch batch;
    struct snag_binary_record record;
    struct snag_binary_event event;
    if (find_record(fd, through, view->compact_start, SNAG_BINARY_COMPACTION_STARTED,
        scratch, &batch, &record, &event) < 0) return -1;
    /* Only fixed metadata survives the next scratch read. */
    struct snag_binary_compact_start start = event.data.compaction_started;
    if (!hex_equal(start.id, sizeof(start.id), state->compact_id) ||
        start.source_seq != state->compact_seq || start.source_seq >= view->compact_start) {
        return snag_errno(EINVAL);
    }
    if (find_record(fd, through, view->compact_end, SNAG_BINARY_COMPACTION_COMPLETED,
        scratch, &batch, &record, &event) < 0) return -1;
    struct snag_binary_ref reference;
    struct snag_binary_compact_complete complete;
    if (snag_binary_compact_ref_create(&batch, view->compact_end, &reference) < 0 ||
        snag_binary_compact_ref_resolve(&reference, &batch, &complete) < 0) return -1;
    if (memcmp(start.id, complete.id, sizeof(start.id)) ||
        memcmp(start.source_sha256, complete.source_sha256, sizeof(start.source_sha256)) ||
        (start.has_scope && (!complete.has_scope ||
            memcmp(start.scope, complete.scope, sizeof(start.scope)))) ||
        (complete.has_scope ?
            !hex_equal(complete.scope, sizeof(complete.scope), state->compact_scope) :
            state->compact_scope[0] != '\0')) return snag_errno(EINVAL);
    const char *type;
    json_t *data = NULL;
    if (snag_binary_legacy_decode(&record, &type, &data) < 0) return -1;
    *out = json_incref(json_object_get(data, "output"));
    json_decref(data);
    return *out ? 0 : snag_errno(EINVAL);
}

static bool
response_boundary(uint16_t kind)
{
    return kind == SNAG_BINARY_RESPONSE_STARTED || kind == SNAG_BINARY_RESPONSE_COMPLETED ||
        kind == SNAG_BINARY_RESPONSE_FAILED || kind == SNAG_BINARY_RESPONSE_INTERRUPTED ||
        kind == SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION || kind == SNAG_BINARY_TURN_STARTED ||
        kind == SNAG_BINARY_TURN_COMPLETED || kind == SNAG_BINARY_TURN_COMPLETED_SILENT ||
        kind == SNAG_BINARY_TURN_INTERRUPTED || kind == SNAG_BINARY_TURN_FAILED;
}

static int
read_response(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_payloads *view, const struct snag_session *state,
    struct snag_buf *scratch, struct snag_session *staged)
{
    struct snag_binary_batch batch;
    struct snag_binary_record record;
    struct snag_binary_event event;
    if (find_record(fd, through, view->response_start, SNAG_BINARY_RESPONSE_STARTED,
        scratch, &batch, &record, &event) < 0) return -1;
    const struct snag_binary_response_start *start = &event.data.response_started;
    if (!hex_equal(start->turn, sizeof(start->turn), state->active_turn_id) ||
        !hex_equal(start->response, sizeof(start->response), state->active_response_id) ||
        start->cycle != state->active_cycle) return snag_errno(EINVAL);
    staged->response_open = true;
    staged->active_cycle = state->active_cycle;
    memcpy(staged->active_turn_id, state->active_turn_id, sizeof(staged->active_turn_id));
    memcpy(staged->active_response_id, state->active_response_id,
        sizeof(staged->active_response_id));
    struct snag_binary_anchor anchor, next;
    if (snag_binary_batch_find(fd, through, view->response_start,
        scratch, &batch, &anchor) < 0) return -1;
    while (anchor.next_seq <= view->response_end) {
        int rc = snag_binary_batch_read(fd, through->end, &anchor, scratch, &batch, &next);
        if (rc != 0) return rc < 0 ? -1 : snag_errno(EINVAL);
        size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
        uint64_t sequence;
        while ((rc = snag_binary_record_next(&batch, &offset, &record, &sequence)) == 0) {
            if (sequence <= view->response_start) continue;
            if (sequence > view->response_end) return snag_errno(EINVAL);
            if (response_boundary(record.kind)) return snag_errno(EINVAL);
            if (record.kind != SNAG_BINARY_RESPONSE_OUTPUT) continue;
            const char *type;
            json_t *data = NULL;
            if (record.flags || snag_binary_legacy_decode(&record, &type, &data) < 0) {
                if (record.flags) (void)snag_errno(EINVAL);
                return -1;
            }
            rc = snag_store_reduce_event(staged, type, data, sequence, NULL, 0u);
            json_decref(data);
            if (rc < 0) return -1;
            if (sequence == view->response_end) {
                return staged->response_public_bytes == state->response_public_bytes ?
                    0 : snag_errno(EINVAL);
            }
        }
        if (rc < 0) return -1;
        anchor = next;
    }
    return snag_errno(EINVAL);
}

int
snag_binary_checkpoint_payloads_read(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_payloads *view, const struct snag_session *state,
    struct snag_binary_checkpoint_payloads_state *out)
{
    if (fd < 0 || !through || through->next_seq <= 1u || !state || !out || !valid(view) ||
        (!!view->turn != state->active_turn) ||
        (!!view->compact_end != !!state->compact_id[0]) ||
        (!!view->response_end != !!state->response_public_bytes) ||
        view->turn >= through->next_seq || view->resume_options >= through->next_seq ||
        view->compact_end >= through->next_seq || view->response_end >= through->next_seq) {
        return snag_errno(EINVAL);
    }
    struct snag_binary_checkpoint_payloads_state value = {0};
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_session *staged = calloc(1u, sizeof(*staged));
    if (!staged) return -1;
    snag_session_init(staged);
    int rc = -1;
    if (view->resume_options && read_options(fd, through, view->resume_options,
        &scratch, &value.resume_options) < 0) goto done;
    if (view->turn && read_instructions(fd, through, view->turn, state,
        &scratch, &value.instructions) < 0) goto done;
    if (view->compact_end && read_compaction(fd, through, view, state,
        &scratch, &value.compact_output) < 0) goto done;
    if (view->response_end && read_response(fd, through, view, state, &scratch, staged) < 0) {
        goto done;
    }
    if (view->downloads_present) {
        staged->download_queue = json_array();
        if (!staged->download_queue) goto done;
    }
    for (size_t i = 0u; i < view->download_count; ++i) {
        struct snag_binary_batch batch;
        struct snag_binary_record record;
        struct snag_binary_event event;
        uint64_t sequence = number(view->downloads + i * 8u);
        if (find_record(fd, through, sequence, SNAG_BINARY_DOWNLOAD_QUEUED,
            &scratch, &batch, &record, &event) < 0) goto done;
        const char *type;
        json_t *data = NULL;
        if (snag_binary_legacy_decode(&record, &type, &data) < 0) goto done;
        int applied = snag_store_reduce_event(staged, type, data, sequence, NULL, 0u);
        json_decref(data);
        if (applied < 0) goto done;
    }
    value.response_public = json_incref(staged->response_public);
    value.response_public_bytes = staged->response_public_bytes;
    value.downloads = json_incref(staged->download_queue);
    *out = value;
    value = (struct snag_binary_checkpoint_payloads_state){0};
    rc = 0;
 done:
    snag_binary_checkpoint_payloads_free(&value);
    snag_session_close(staged);
    free(staged);
    snag_buf_free(&scratch);
    return rc;
}
