/* SPDX-License-Identifier: GPL-2.0-only */
#include "fixture_store_binary.h"
#include "fixture_store_legacy.h"
#include "store_binary_replay.h"
#include "store_binary_import.h"
#include "store_binary_producer.h"
#include "fs.h"
#include "context.h"
#include "irc.h"
#include "store_binary_legacy.h"
#include "store_internal.h"
#include "checked_json.h"
#include "snajpagent.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#endif

void test_store_binary_core_state(int, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_sources *, const struct snag_session *);
void test_store_binary_processes_state(int, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_sources *, const struct snag_session *);
void test_store_binary_payloads_state(int, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_sources *, const struct snag_session *);
void test_store_binary_inputs_state(int, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_sources *, const struct snag_session *);
void test_store_binary_calls_state(int, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_call_source *, const struct snag_session *);
void test_store_binary_texts_state(int, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_texts *, const struct snag_session *);
void test_store_binary_texts_bad(int, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_texts *);

#define GOAL_ID "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define OTHER_ID "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define ZERO_HASH "0000000000000000000000000000000000000000000000000000000000000000"

static void
test_live_result_coordinates(enum snag_binary_kind kind, unsigned int variant)
{
    struct snag_binary_producer producer = {.field = {.max = SNAG_MAX_EVENT_LINE}};
    struct snag_process_state process = {.log_offset = 100u, .log_seq = 3u};
    strcpy(process.handle, GOAL_ID);
    strcpy(process.log_hash, ZERO_HASH);
    struct snag_session committed = {.log_end = 200, .next_seq = 9u,
        .processes = &process, .process_count = 1u};
    uint64_t start = 100u, end = 200u;
    if (variant == 1u) { start = 200u; process.log_offset = 200u; process.log_seq = 9u; }
    if (variant == 2u) { start = end = 0u; committed.process_count = 0u; }
    if (variant == 3u) committed.process_count = 0u;
    if (variant == 4u) strcpy(process.handle, OTHER_ID);
    if (variant == 5u) process.log_offset = 99u;
    if (variant == 6u) committed.log_end = 201;
    if (variant == 7u) process.log_seq = 0u;
    if (variant == 8u) process.log_seq = 10u;
    if (variant == 9u) process.log_hash[0] = 'x';
    if (variant == 10u) process.log_seq = 9u; /* Nonempty bytes cannot name an empty range. */
    if (variant == 11u) { start = 200u; process.log_offset = 200u; }
    json_t *result = snag_tool_result_terminal(true, "fixture result");
    assert(result);
    assert(!snag_json_set_new(result, "max_output_tokens", json_integer(16000)));
    assert(!snag_json_set_new(result, "output_ref",
        json_pack("{s:s,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:b,s:I,s:I}",
            "handle", GOAL_ID, "stdout_start", 0, "stdout_end", 0,
            "stderr_start", 0, "stderr_end", 0, "stdin_accepted", 0,
            "stdin_written", 0, "stdin_pending", 0, "stdin_open", 0,
            "log_start", (json_int_t)start, "log_end", (json_int_t)end)));
    const char *type = kind == SNAG_BINARY_TOOL_FINISHED ? "tool_finished" : "process_closed";
    json_t *data = kind == SNAG_BINARY_TOOL_FINISHED ?
        json_pack("{s:s,s:s,s:o}", "turn_id", OTHER_ID, "call_id", GOAL_ID, "result", result) :
        json_pack("{s:s,s:s,s:s,s:o}", "turn_id", OTHER_ID, "handle", GOAL_ID,
            "cause", "user_interrupt", "result", result);
    if (variant == 12u && kind == SNAG_BINARY_PROCESS_CLOSED)
        assert(!snag_json_set_new(data, "handle", json_string(OTHER_ID)));
    if (variant == 12u && kind == SNAG_BINARY_TOOL_FINISHED) committed.log_end = -1;
    json_t *original = json_deep_copy(data);
    enum snag_binary_kind encoded;
    assert(data && original && !snag_binary_legacy_encode(&producer.field, type, data, &encoded));
    assert(encoded == kind);
    struct snag_binary_record record = {.kind = (uint16_t)kind,
        .version = snag_binary_event_version(kind), .payload = producer.field.data,
        .size = producer.field.len};
    struct snag_binary_record saved_record = record;
    struct snag_buf saved = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_buf_append(&saved, producer.field.data, producer.field.len));
    struct snag_process_state saved_process = process;
    int rc = snag_binary_producer_live_result(&producer, &committed, &record);
    assert(!memcmp(&process, &saved_process, sizeof(process)) && json_equal(data, original));
    if (variant >= 3u) {
        assert(rc < 0 && errno == EINVAL);
        assert(!memcmp(&record, &saved_record, sizeof(record)) && producer.field.len == saved.len &&
            !memcmp(producer.field.data, saved.data, saved.len));
    } else {
        assert(!rc);
        struct snag_binary_event event;
        assert(!snag_binary_event_decode(&record, &event));
        struct snag_binary_tool_output_ref *ref = kind == SNAG_BINARY_TOOL_FINISHED ?
            &event.data.tool_finished.result.output_ref :
            &event.data.process_closed.result.output_ref;
        assert(ref->native && ref->log_start == start && ref->log_end == end);
        assert(ref->first_sequence == (variant == 2u ? 0u : process.log_seq) &&
            ref->end_sequence == (variant == 2u ? 0u : committed.next_seq));
        const char *decoded_type = NULL;
        json_t *decoded_data = NULL;
        assert(!snag_binary_legacy_decode(&record, &decoded_type, &decoded_data));
        assert(!strcmp(type, decoded_type) && json_equal(data, decoded_data));
        json_decref(decoded_data);
        /* This entrypoint accepts literal admission only, never a second projection. */
        assert(snag_binary_producer_live_result(&producer, &committed, &record) < 0 &&
            errno == EINVAL);
    }
    json_decref(data);
    json_decref(original);
    snag_buf_free(&saved);
    snag_binary_producer_free(&producer);
}

static void
test_producer_candidate_ownership(void)
{
    struct snag_binary_producer source = {.field = {.max = SNAG_MAX_EVENT_LINE}};
    source.input.creation = 7u;
    source.input.text = (struct snag_binary_input_reference){
        .field = SNAG_BINARY_INPUT_TEXT, .target = {7u, 20u, 5u}};
    source.input.paths = json_pack("[s]", "AGENTS.md");
    source.queue_count = source.queue_capacity = 1u;
    source.queue = calloc(1u, sizeof(*source.queue));
    source.output_count = source.output_capacity = 1u;
    source.outputs = calloc(1u, sizeof(*source.outputs));
    source.public = json_pack("[{s:s,s:s}]", "provider_item_id", "part", "text", "hello");
    assert(source.input.paths && source.queue && source.outputs && source.public);
    source.queue[0] = (struct snag_binary_input_source){.creation = 8u,
        .text = {.field = SNAG_BINARY_INPUT_TEXT, .target = {10u, 40u, 5u}},
        .content = {.field = SNAG_BINARY_INPUT_CONTENT, .target = {8u, 60u, 4u}},
        .paths = json_incref(source.input.paths)};
    source.outputs[0].value.source = (struct snag_binary_output_span){
        .first = {11u, 30u, 2u}, .last_sequence = 12u, .bytes = 5u};
    assert(!snag_buf_append(&source.field, "scratch", 7u));
    struct snag_binary_producer staged = {.field = {.max = 16u}};
    assert(!snag_buf_append(&staged.field, "canary", 6u));
    struct snag_buf saved = staged.field;
    assert(snag_binary_producer_clone(&staged, NULL) < 0 && errno == EINVAL);
    assert(!memcmp(&saved, &staged.field, sizeof(saved)) &&
        !memcmp(staged.field.data, "canary", 6u));
    assert(snag_binary_producer_clone(&source, &source) < 0 && errno == EINVAL);
    assert(!snag_binary_producer_clone(&staged, &source));
    assert(!staged.field.data && !staged.field.len && staged.field.max == SNAG_MAX_EVENT_LINE);
    assert(staged.queue != source.queue && staged.outputs != source.outputs);
    assert(staged.input.paths == source.input.paths && staged.public == source.public);
    staged.queue[0].text.target.sequence = 99u;
    staged.outputs[0].value.source.bytes = 99u;
    assert(source.queue[0].text.target.sequence == 10u &&
        source.outputs[0].value.source.bytes == 5u);
    snag_binary_producer_free(&staged); /* Abandoned candidate leaves its owner intact. */
    assert(source.input.creation == 7u && source.queue_count == 1u && source.output_count == 1u);
    assert(!snag_binary_producer_clone(&staged, &source));
    snag_binary_producer_free(&source);
    assert(staged.input.creation == 7u && staged.queue[0].text.target.sequence == 10u &&
        staged.queue[0].content.target.sequence == 8u);
    assert(staged.outputs[0].value.source.last_sequence == 12u);
    assert(!strcmp(json_string_value(json_array_get(staged.input.paths, 0u)), "AGENTS.md"));
    assert(!strcmp(snag_json_string(json_array_get(staged.public, 0u), "text"), "hello"));
    snag_binary_producer_free(&staged);
    snag_binary_producer_free(&staged);
}

/* A small fixture builder, not the production converter. The source is verified
 * by the existing strict reader; three records per batch exercise partial-batch
 * state adoption. Derived checkpoints get empty, optional test metadata. */
struct replay_fixture {
    struct snag_buf file, payload[3];
    struct snag_binary_record records[3];
    struct snag_binary_identity identity;
    struct snag_binary_anchor anchor;
    size_t count, first_end, batch_records;
    uint64_t turns;
    json_t *creation, *events;
};

static int
temporary_fd(void)
{
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snajpagent-native-replay-XXXXXX");
    assert(path);
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(!unlink(path));
    free(path);
    return fd;
}

static void
fixture_flush(struct replay_fixture *fixture)
{
    if (!fixture->count) return;
    struct snag_buf bytes = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_anchor next;
    struct snag_binary_batch batch;
    assert(!snag_binary_batch_encode(&bytes, &fixture->anchor, fixture->records,
        (uint32_t)fixture->count, fixture->turns, NULL));
    assert(!snag_binary_batch_decode(bytes.data, bytes.len, &fixture->anchor, &batch, &next));
    assert(!snag_binary_wire_encode(&fixture->file, bytes.data, bytes.len));
    if (!fixture->first_end) fixture->first_end = fixture->file.len;
    fixture->anchor = next;
    fixture->count = 0u;
    snag_buf_free(&bytes);
    for (size_t i = 0u; i < 3u; ++i) snag_buf_reset(&fixture->payload[i]);
}

static int
fixture_event(void *opaque, const struct snag_session *state, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t error_size)
{
    (void)error;
    (void)error_size;
    struct replay_fixture *fixture = opaque;
    assert(state && state->next_seq == seq + 1u);
    if (fixture->events) {
        assert(json_array_size(fixture->events) == seq - 1u);
        assert(!json_array_append_new(fixture->events,
            json_pack("{s:I,s:s,s:O}", "time", (json_int_t)state->last_time_ms,
                "type", type, "data", (json_t *)data)));
    }
    if (seq == 1u) {
        const char *hex = "0123456789abcdef";
        for (size_t i = 0u; i < 16u; ++i) {
            unsigned int high = (unsigned int)(strchr(hex, state->id[i * 2u]) - hex);
            unsigned int low = (unsigned int)(strchr(hex, state->id[i * 2u + 1u]) - hex);
            fixture->identity.id[i] = (unsigned char)(high * 16u + low);
        }
        fixture->identity.created_ms = state->last_time_ms;
        fixture->creation = json_incref((json_t *)data);
        unsigned char header[SNAG_BINARY_HEADER_SIZE];
        snag_binary_header_encode(header, &fixture->identity);
        struct snag_binary_identity identity;
        assert(!snag_binary_header_decode(header, sizeof(header), &identity, &fixture->anchor));
        assert(!snag_buf_append(&fixture->file, header, sizeof(header)));
    }
    size_t slot = fixture->count;
    struct snag_binary_record record = {.version = 1u, .timestamp_ms = state->last_time_ms};
    if (!strcmp(type, "session_checkpoint")) {
        record.kind = 0x8fffu;
        record.flags = SNAG_BINARY_RECORD_OPTIONAL;
    } else {
        enum snag_binary_kind kind;
        assert(!snag_binary_legacy_encode(&fixture->payload[slot], type, data, &kind));
        record.kind = (uint16_t)kind;
        record.version = snag_binary_event_version(kind);
        record.payload = fixture->payload[slot].data;
        record.size = fixture->payload[slot].len;
    }
    fixture->records[slot] = record;
    ++fixture->count;
    fixture->turns = state->turn_count;
    if (fixture->count == (fixture->batch_records ? fixture->batch_records : 3u))
        fixture_flush(fixture);
    return 0;
}

static uint64_t
commit_data(struct snag_session *session, const char *type, json_t *data)
{
    char error[512];
    assert(data);
    uint64_t sequence;
    int rc = snag_session_commit(session, type, data, &sequence, error, sizeof(error));
    if (rc < 0) fprintf(stderr, "native replay source fixture %s: %s\n", type, error);
    assert(!rc);
    return sequence;
}

static void
write_fixture(struct snag_session *source, const void *bytes, size_t size)
{
    assert(!snag_truncate(source->log_fd, 0));
    assert(snag_seek(source->log_fd, 0, SEEK_SET) == 0);
    assert(!snag_write_full(source->log_fd, bytes, size));
    assert(snag_seek(source->log_fd, 7, SEEK_SET) == 7);
}

static unsigned int prefix_comparisons;

static void
prefix_matches(struct snag_session *source, const struct snag_session *expected,
    const struct snag_binary_anchor *anchor)
{
    struct snag_session prefix;
    struct snag_binary_recovery recovery = {.verified = *anchor};
    struct snag_binary_checkpoint_sources origins = {0};
    char error[512] = {0};
    snag_session_init(&prefix);
    /* Exercise an input anchor aliasing the diagnostic output. */
    assert(snag_store_reconcile_binary_prefix(source, &prefix, &recovery.verified,
        NULL, NULL, &recovery, &origins, error, sizeof(error)) == 0);
    assert(!recovery.incomplete_tail_bytes && !recovery.problem_seq);
    json_t *left = snag_checkpoint_state_encode(expected);
    json_t *right = snag_checkpoint_state_encode(&prefix);
    assert(left && right && json_equal(left, right));
    json_decref(left);
    json_decref(right);
    test_store_binary_core_state(source->log_fd, &recovery.verified, &origins, &prefix);
    snag_binary_checkpoint_sources_free(&origins);
    snag_session_close(&prefix);
    ++prefix_comparisons;
}

static int
replay_visit(struct snag_session *source, struct snag_session *restored,
    struct snag_binary_recovery *recovery, const void *bytes, size_t size,
    snag_session_event_fn fn, void *opaque)
{
    write_fixture(source, bytes, size);
    struct snag_session saved_source = *source, saved_restored = *restored;
    char error[512] = {0};
    struct snag_binary_checkpoint_sources sources = {.response_start = UINT64_MAX};
    unsigned char saved_sources[sizeof(sources)];
    memcpy(saved_sources, &sources, sizeof(sources));
    int rc = snag_store_reconcile_binary(source, restored, fn, opaque,
        recovery, &sources, error, sizeof(error));
    int code = errno;
    if (rc < 0) {
        assert(*error);
        assert(!memcmp(restored, &saved_restored, sizeof(saved_restored)));
        assert(!recovery->incomplete_tail_bytes);
        assert(!memcmp(&sources, saved_sources, sizeof(sources)));
    } else {
        test_store_binary_texts_state(source->log_fd, &recovery->verified,
            &sources.texts, restored);
        test_store_binary_calls_state(source->log_fd, &recovery->verified,
            &sources.calls, restored);
        test_store_binary_processes_state(source->log_fd, &recovery->verified, &sources, restored);
        test_store_binary_inputs_state(source->log_fd, &recovery->verified, &sources, restored);
        test_store_binary_payloads_state(source->log_fd, &recovery->verified, &sources, restored);
        test_store_binary_core_state(source->log_fd, &recovery->verified, &sources, restored);
        prefix_matches(source, restored, &recovery->verified);
        snag_binary_checkpoint_sources_free(&sources);
    }
    assert(!memcmp(source, &saved_source, sizeof(saved_source)));
    assert(snag_seek(source->log_fd, 0, SEEK_CUR) == 7);
    snag_file_info info;
    assert(!snag_fstat(source->log_fd, &info) && (uint64_t)info.st_size == size);
    unsigned char *copy = malloc(size ? size : 1u);
    assert(copy);
    assert(snag_pread(source->log_fd, copy, size, 0) == (ssize_t)size);
    assert(!memcmp(copy, bytes, size));
    free(copy);
    errno = code;
    return rc;
}

static int
replay_check(struct snag_session *source, struct snag_session *restored,
    struct snag_binary_recovery *recovery, const void *bytes, size_t size)
{
    return replay_visit(source, restored, recovery, bytes, size, NULL, NULL);
}

static void
append_record(struct replay_fixture *fixture, struct snag_buf *file,
    const struct snag_binary_record *record, uint64_t turns)
{
    snag_buf_reset(file);
    assert(!snag_buf_append(file, fixture->file.data, fixture->file.len));
    assert(!binary_fixture_append(file, &fixture->anchor, record, 1u, turns, NULL));
}

static struct snag_binary_record
encode_record(struct snag_buf *payload, const char *type, const json_t *data)
{
    snag_buf_reset(payload);
    enum snag_binary_kind kind;
    assert(!snag_binary_legacy_encode(payload, type, data, &kind));
    return (struct snag_binary_record){.kind = (uint16_t)kind,
        .version = snag_binary_event_version(kind), .payload = payload->data, .size = payload->len};
}

static void
test_reference_rejection(struct replay_fixture *fixture, struct snag_session *source,
    struct snag_session *restored)
{
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf file = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    struct snag_binary_recovery recovery;
    json_t *data = checked_json(json_pack("{s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:s}",
        "transfer_id", GOAL_ID, "source_session_id", OTHER_ID, "target_session_id", source->id,
        "source_as_of_seq", 1, "count", 1, "begin_offset", 123, "begin_seq", 2,
        "begin_sha256", ZERO_HASH));
    struct snag_binary_record record = encode_record(&payload, "voice_transfer_adopted", data);
    json_decref(data);
    append_record(fixture, &file, &record, fixture->turns);
    assert(replay_check(source, restored, &recovery, file.data, file.len) < 0 && errno == ENOTSUP);

    struct snag_binary_event adoption;
    assert(!snag_binary_event_decode(&record, &adoption));
    struct snag_binary_voice_adopted *voice = &adoption.data.voice_transfer_adopted;
    voice->native = true;
    voice->begin_offset = 0u;
    memset(voice->begin_sha256, 0, sizeof(voice->begin_sha256));
    for (unsigned int bad = 0u; bad < 4u; ++bad) {
        voice->begin_seq = bad < 2u ? fixture->anchor.next_seq + bad : 2u;
        voice->transfer.count = bad == 2u ? fixture->anchor.next_seq : 1u;
        if (bad == 3u) voice->transfer.target[0] ^= 1u;
        snag_buf_reset(&payload);
        assert(!snag_binary_event_encode(&payload, &adoption));
        record.payload = payload.data;
        record.size = payload.len;
        append_record(fixture, &file, &record, fixture->turns);
        assert(replay_check(source, restored, &recovery, file.data, file.len) < 0);
        assert(errno == EINVAL && recovery.verified.end == fixture->anchor.end);
    }

    json_t *result = checked_json(json_pack("{s:s,s:n,s:n,s:i,s:i,s:n,s:s,s:i}",
        "status", "succeeded", "reason", "handle", "duration_ms", 0, "exit_code", 0,
        "signal", "model_text", "", "max_output_tokens", 1));
    json_t *excerpt = checked_json(json_pack("{s:s,s:s,s:i,s:i,s:i}",
        "encoding", "utf8", "retained", "", "retained_bytes", 0,
        "original_bytes", 0, "discarded_bytes", 0));
    assert(!json_object_set(result, "stdout", excerpt));
    assert(!json_object_set(result, "stderr", excerpt));
    json_decref(excerpt);
    assert(!snag_json_set_new(result, "output_ref", json_pack(
        "{s:s,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:b,s:i,s:i}", "handle", OTHER_ID,
        "stdout_start", 5, "stdout_end", 5, "stderr_start", 8, "stderr_end", 8,
        "stdin_accepted", 9, "stdin_written", 3, "stdin_pending", 5, "stdin_open", 1,
        "log_start", 123, "log_end", 456)));
    for (unsigned int i = 0u; i < 2u; ++i) {
        data = checked_json(json_pack("{s:s,s:s,s:O}", "turn_id", GOAL_ID,
            i ? "handle" : "call_id", OTHER_ID, "result", result));
        if (i) assert(!snag_json_set_new(data, "cause", json_string("user_interrupt")));
        record = encode_record(&payload, i ? "process_closed" : "tool_finished", data);
        json_decref(data);
        append_record(fixture, &file, &record, fixture->turns);
        assert(replay_check(source, restored, &recovery, file.data, file.len) < 0);
        assert(errno == ENOTSUP && recovery.verified.end == fixture->anchor.end);
    }
    json_decref(result);

    struct snag_buf literal = {.max = SNAG_MAX_EVENT_LINE};
    data = checked_json(json_pack("{s:s,s:s,s:s,s:s,s:[],s:b,s:i}",
        "provider", "p", "model", "m", "effort", "e", "text", "t",
        "instructions", "read_only", 0, "received_at_ms", 0));
    record = encode_record(&literal, "input_received", data);
    json_decref(data);
    struct snag_binary_event event;
    assert(!snag_binary_event_decode(&record, &event));
    event.data.input.text = (struct snag_binary_text){0};
    event.data.input.text_ref = (struct snag_binary_input_reference){
        .field = SNAG_BINARY_INPUT_TEXT, .target = {1u, 0u, 1u}};
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record.payload = payload.data;
    record.size = payload.len;
    append_record(fixture, &file, &record, fixture->turns);
    assert(replay_check(source, restored, &recovery, file.data, file.len) < 0 && errno == EINVAL);
    snag_buf_free(&literal);
    snag_buf_free(&payload);
    snag_buf_free(&file);
}

static void
test_creation_rejection(struct replay_fixture *fixture, struct snag_session *source,
    struct snag_session *restored)
{
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf file = {.max = SNAG_BINARY_WIRE_BATCH_MAX + SNAG_BINARY_HEADER_SIZE};
    struct snag_binary_recovery recovery;
    struct snag_binary_anchor root;
    struct snag_binary_identity identity;
    assert(!snag_binary_header_decode(fixture->file.data, SNAG_BINARY_HEADER_SIZE,
        &identity, &root));
    struct snag_binary_record records[2];
    records[0] = encode_record(&payload, "session_created", fixture->creation);
    records[0].timestamp_ms = identity.created_ms + 1u;
    assert(!snag_buf_append(&file, fixture->file.data, SNAG_BINARY_HEADER_SIZE));
    assert(!binary_fixture_append(&file, &root, records, 1u, 0u, NULL));
    assert(replay_check(source, restored, &recovery, file.data, file.len) < 0 && errno == EINVAL);
    assert(recovery.problem_seq == 1u && !recovery.batches);
    records[0].timestamp_ms = identity.created_ms;
    records[0].flags = SNAG_BINARY_RECORD_OPTIONAL;
    file.len = SNAG_BINARY_HEADER_SIZE;
    assert(!binary_fixture_append(&file, &root, records, 1u, 0u, NULL));
    assert(replay_check(source, restored, &recovery, file.data, file.len) < 0 && errno == EINVAL);

    /* Format-2 semantics remain strict here: an invalid transition cannot use
     * the ordinary legacy reader's historical downgrade. The entire first
     * batch is provisional, including its otherwise valid creation. */
    json_t *created = checked_json(json_deep_copy(fixture->creation));
    assert(!snag_json_set_new(created, "format", json_integer(2)));
    assert(!json_object_set(created, "workspace", json_object_get(created, "cwd")));
    assert(!json_object_del(created, "cwd"));
    records[0] = encode_record(&payload, "session_created", created);
    records[0].timestamp_ms = identity.created_ms;
    json_decref(created);
    struct snag_buf bad_payload = {.max = SNAG_MAX_EVENT_LINE};
    json_t *bad = checked_json(json_pack("{s:s,s:s,s:s}", "goal_id", OTHER_ID,
        "actor", "user", "prompt", "no such goal"));
    records[1] = encode_record(&bad_payload, "goal_reworded", bad);
    json_decref(bad);
    file.len = SNAG_BINARY_HEADER_SIZE;
    assert(!binary_fixture_append(&file, &root, records, 2u, 0u, NULL));
    assert(replay_check(source, restored, &recovery, file.data, file.len) < 0 && errno == EINVAL);
    assert(recovery.problem_seq == 2u && recovery.verified.end == SNAG_BINARY_HEADER_SIZE);
    snag_buf_free(&bad_payload);
    snag_buf_free(&payload);
    snag_buf_free(&file);
}

enum turn_ref_case {
    TURN_REF_BOTH, TURN_REF_TEXT, TURN_REF_CONTENT,
    TURN_REF_OLD_TEXT, TURN_REF_OTHER_TEXT, TURN_REF_FUTURE, TURN_REF_SELF,
    TURN_REF_EDIT_CONTENT, TURN_REF_OTHER_CONTENT,
    TURN_REF_OFFSET, TURN_REF_SIZE, TURN_REF_ROLE, TURN_REF_INSTRUCTIONS,
    TURN_REF_DIRECT, TURN_REF_OWNER, TURN_REF_CREATION, TURN_REF_READ_ONLY,
    TURN_REF_ORIGIN, TURN_REF_MODEL, TURN_REF_GOAL, TURN_REF_INNER_OFFSET,
    TURN_REF_ALL, TURN_REF_PATHS, TURN_REF_OTHER_PATHS, TURN_REF_FUTURE_PATHS,
    TURN_REF_PATH_OFFSET, TURN_REF_PATH_SIZE, TURN_REF_PATH_LEAF, TURN_REF_INNER_PATH,
    TURN_REF_RECEIPT_CHAIN, TURN_REF_VOICE_ALIAS
};

struct turn_sources {
    uint64_t creation, text, other, future, turn, canonical, alternate;
};

static struct snag_binary_ref
fixture_reference(const struct replay_fixture *fixture, uint64_t wanted,
    enum snag_binary_input_leaf field)
{
    struct snag_buf decoded = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_identity identity;
    struct snag_binary_anchor anchor;
    assert(!snag_binary_header_decode(fixture->file.data, fixture->file.len, &identity, &anchor));
    while (anchor.end < fixture->file.len) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor next;
        assert(!binary_fixture_read(fixture->file.data + anchor.end,
            fixture->file.len - anchor.end, &anchor, &decoded, &batch, &next));
        if (wanted >= batch.first_seq && wanted < next.next_seq) {
            struct snag_binary_ref reference;
            assert(!snag_binary_input_ref_create(&batch, wanted, field, &reference));
            snag_buf_free(&decoded);
            return reference;
        }
        anchor = next;
    }
    assert(false);
    return (struct snag_binary_ref){0};
}

/* Turn references reuse the receipt's declared original directly. They never
 * reference the receipt's encoded reference marker or follow a chain. */
static struct snag_binary_input_reference
fixture_declaration(const struct replay_fixture *fixture, uint64_t wanted,
    enum snag_binary_input_leaf field)
{
    struct snag_buf decoded = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_identity identity;
    struct snag_binary_anchor anchor;
    assert(!snag_binary_header_decode(fixture->file.data, fixture->file.len, &identity, &anchor));
    while (anchor.end < fixture->file.len) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor next;
        assert(!binary_fixture_read(fixture->file.data + anchor.end,
            fixture->file.len - anchor.end, &anchor, &decoded, &batch, &next));
        size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
        for (uint32_t i = 0u; i < batch.count; ++i) {
            struct snag_binary_record record;
            uint64_t sequence;
            assert(!snag_binary_record_next(&batch, &cursor, &record, &sequence));
            if (sequence != wanted) continue;
            struct snag_binary_event event;
            assert(!snag_binary_event_decode(&record, &event));
            if (event.kind == SNAG_BINARY_IRC_ADMITTED) {
                struct snag_binary_record child = event.data.irc_admitted.input;
                assert(!snag_binary_event_decode(&child, &event));
            }
            struct snag_binary_input_reference declared = {0};
            if (event.kind == SNAG_BINARY_INPUT_RECEIVED) {
                declared = field == SNAG_BINARY_INPUT_TEXT ? event.data.input.text_ref :
                    field == SNAG_BINARY_INPUT_CONTENT ? event.data.input.content_ref :
                    event.data.input.instructions_ref;
            } else if (event.kind == SNAG_BINARY_TURN_STARTED) {
                declared = field == SNAG_BINARY_INPUT_TEXT ? event.data.started.text_ref :
                    field == SNAG_BINARY_INPUT_CONTENT ? event.data.started.content_ref :
                    event.data.started.instructions_ref;
            } else if (event.kind == SNAG_BINARY_STEERING_ADDED ||
                event.kind == SNAG_BINARY_IRC_REPLY_REMINDER) {
                declared = field == SNAG_BINARY_INPUT_TEXT ? event.data.steering_input.text_ref :
                    event.data.steering_input.content_ref;
            } else if (event.kind == SNAG_BINARY_FUTURE_TURN_QUEUED ||
                event.kind == SNAG_BINARY_FUTURE_TURN_EDITED) {
                declared = field == SNAG_BINARY_INPUT_TEXT ? event.data.queued.text_ref :
                    field == SNAG_BINARY_INPUT_VOICE_TRANSCRIPT ?
                    event.data.queued.voice.transcript_ref :
                    field == SNAG_BINARY_INPUT_VOICE_REQUEST ? event.data.queued.voice.request_ref :
                    event.data.queued.content_ref;
            }
            snag_buf_free(&decoded);
            if (declared.field) return declared;
            return (struct snag_binary_input_reference){.field = field,
                .target = fixture_reference(fixture, wanted, field)};
        }
        anchor = next;
    }
    assert(false);
    return (struct snag_binary_input_reference){0};
}

static void
reference_turn(struct snag_binary_turn_start *turn, const struct replay_fixture *fixture,
    const struct turn_sources *sequences, enum turn_ref_case mode)
{
    if (mode != TURN_REF_CONTENT && mode != TURN_REF_PATHS) {
        uint64_t source = sequences->text;
        if (mode == TURN_REF_OLD_TEXT) source = sequences->creation;
        if (mode == TURN_REF_OTHER_TEXT) source = sequences->other;
        if (mode == TURN_REF_FUTURE) source = sequences->future;
        if (mode == TURN_REF_SELF) source = sequences->turn;
        turn->text_ref = fixture_declaration(fixture, source, SNAG_BINARY_INPUT_TEXT);
        turn->text = (struct snag_binary_text){0};
    }
    if (mode != TURN_REF_TEXT && mode != TURN_REF_PATHS) {
        uint64_t source = sequences->creation;
        if (mode == TURN_REF_EDIT_CONTENT) source = sequences->text;
        if (mode == TURN_REF_OTHER_CONTENT) source = sequences->other;
        turn->content_ref = fixture_declaration(fixture, source, SNAG_BINARY_INPUT_CONTENT);
        turn->content = (struct snag_binary_content){0};
    }
    if (mode == TURN_REF_OFFSET) ++turn->text_ref.target.offset;
    if (mode == TURN_REF_SIZE) --turn->content_ref.target.size;
    if (mode == TURN_REF_ROLE) turn->text_ref.field = SNAG_BINARY_INPUT_VOICE_REQUEST;
    if (mode == TURN_REF_VOICE_ALIAS) {
        enum snag_binary_input_leaf field =
            turn->text_ref.field == SNAG_BINARY_INPUT_VOICE_REQUEST ?
            SNAG_BINARY_INPUT_VOICE_TRANSCRIPT : SNAG_BINARY_INPUT_VOICE_REQUEST;
        turn->text_ref.target = fixture_reference(fixture, turn->text_ref.target.sequence, field);
        turn->text_ref.field = field;
    }
    if (mode == TURN_REF_INSTRUCTIONS ||
        (mode >= TURN_REF_ALL && mode <= TURN_REF_INNER_PATH)) {
        uint64_t source = sequences->creation;
        if (mode == TURN_REF_INSTRUCTIONS) source = sequences->turn;
        if (mode == TURN_REF_OTHER_PATHS) source = sequences->other;
        if (mode == TURN_REF_FUTURE_PATHS) source = sequences->future;
        enum snag_binary_input_leaf field = mode == TURN_REF_PATH_LEAF ?
            SNAG_BINARY_INPUT_TEXT : SNAG_BINARY_INPUT_INSTRUCTIONS;
        turn->instructions_ref = fixture_declaration(fixture, source, field);
        turn->instructions_ref.field = SNAG_BINARY_INPUT_INSTRUCTIONS;
        turn->instructions = (struct snag_binary_instructions){0};
        if (mode == TURN_REF_PATH_OFFSET) ++turn->instructions_ref.target.offset;
        if (mode == TURN_REF_PATH_SIZE) ++turn->instructions_ref.target.size;
        if (mode == TURN_REF_INNER_PATH) {
            struct snag_binary_ref inner = fixture_reference(fixture, sequences->alternate,
                SNAG_BINARY_INPUT_INSTRUCTIONS);
            assert(inner.offset < turn->instructions_ref.target.offset);
            turn->instructions_ref.target.offset = inner.offset;
        }
    }
    if (mode == TURN_REF_RECEIPT_CHAIN) turn->text_ref.target.sequence = sequences->text;
    if (mode == TURN_REF_DIRECT) turn->origin = SNAG_BINARY_TURN_DIRECT;
    if (mode == TURN_REF_OWNER) turn->queue_id[0] ^= 1u;
    if (mode == TURN_REF_CREATION) turn->queue_seq = sequences->other;
    if (mode == TURN_REF_READ_ONLY) turn->read_only = !turn->read_only;
    if (mode == TURN_REF_ORIGIN) {
        turn->origin = turn->origin == SNAG_BINARY_TURN_DIRECT ?
            SNAG_BINARY_TURN_TIMER : SNAG_BINARY_TURN_DIRECT;
    }
    if (mode == TURN_REF_MODEL) {
        turn->config.selection.model = (struct snag_binary_text){
            (const unsigned char *)"different", 9u};
    }
    if (mode == TURN_REF_GOAL) turn->origin = SNAG_BINARY_TURN_GOAL;
    if (mode == TURN_REF_INNER_OFFSET) {
        struct snag_binary_ref inner = fixture_reference(fixture, sequences->alternate,
            SNAG_BINARY_INPUT_TEXT);
        assert(inner.offset < turn->text_ref.target.offset);
        turn->text_ref.target.offset = inner.offset;
    }
}

static void
rewrite_turn(const struct replay_fixture *fixture, const struct turn_sources *sequences,
    enum turn_ref_case mode, struct snag_buf *file)
{
    struct snag_buf decoded = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_identity identity;
    struct snag_binary_anchor input;
    assert(!snag_binary_header_decode(fixture->file.data, fixture->file.len, &identity, &input));
    struct snag_binary_anchor output = input;
    snag_buf_reset(file);
    assert(!snag_buf_append(file, fixture->file.data, SNAG_BINARY_HEADER_SIZE));
    while (input.end < fixture->file.len) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor next;
        assert(!binary_fixture_read(fixture->file.data + input.end,
            fixture->file.len - input.end, &input, &decoded, &batch, &next));
        assert(batch.count <= 3u);
        struct snag_binary_record records[3];
        struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
        size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
        for (uint32_t i = 0u; i < batch.count; ++i) {
            uint64_t sequence;
            assert(!snag_binary_record_next(&batch, &cursor, &records[i], &sequence));
            if (sequence != sequences->turn) continue;
            if (fixture->batch_records == 3u && sequences->text != sequences->creation) {
                assert(sequences->text >= batch.first_seq && sequences->future < next.next_seq);
            }
            struct snag_binary_event event;
            assert(!snag_binary_event_decode(&records[i], &event));
            assert(event.kind == SNAG_BINARY_TURN_STARTED);
            reference_turn(&event.data.started, fixture, sequences, mode);
            assert(!snag_binary_event_encode(&payload, &event));
            records[i].payload = payload.data;
            records[i].size = payload.len;
            const char *literal_type = NULL;
            json_t *literal_data = NULL;
            assert(snag_binary_legacy_decode(&records[i], &literal_type, &literal_data) < 0 &&
                errno == ENOTSUP && !literal_type && !literal_data);
        }
        assert(!binary_fixture_append(file, &output, records, batch.count, next.turns, &output));
        snag_buf_free(&payload);
        input = next;
    }
    snag_buf_free(&decoded);
}

enum receipt_fault {
    RECEIPT_VALID, RECEIPT_ROLE, RECEIPT_OFFSET, RECEIPT_SIZE,
    RECEIPT_CONTENT_LEAF, RECEIPT_INSTRUCTION_LEAF,
    RECEIPT_CONTENT_SIZE, RECEIPT_INSTRUCTION_SIZE, RECEIPT_CHAIN,
    RECEIPT_TRANSCRIPT, RECEIPT_REQUEST, RECEIPT_TURN, RECEIPT_ID, RECEIPT_REMINDER,
    RECEIPT_GOAL, RECEIPT_READ_ONLY,
    RECEIPT_VOICE_TEXT, RECEIPT_VOICE_ALIAS, RECEIPT_VOICE_ROLE,
    RECEIPT_VOICE_OFFSET, RECEIPT_VOICE_SIZE, RECEIPT_VOICE_CHAIN,
    RECEIPT_VOICE_CONNECTION, RECEIPT_VOICE_INPUT, RECEIPT_VOICE_ID, RECEIPT_VOICE_TURN,
    RECEIPT_INSTRUCTION_OFFSET, RECEIPT_QUEUE_OWNER, RECEIPT_QUEUE_CREATION
};

static void
rewrite_receipt(const struct replay_fixture *fixture, uint64_t receipt, uint64_t canonical,
    unsigned int fields, enum receipt_fault fault, struct snag_buf *file)
{
    struct snag_buf decoded = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_identity identity;
    struct snag_binary_anchor input;
    assert(!snag_binary_header_decode(fixture->file.data, fixture->file.len, &identity, &input));
    struct snag_binary_anchor output = input;
    snag_buf_reset(file);
    assert(!snag_buf_append(file, fixture->file.data, SNAG_BINARY_HEADER_SIZE));
    while (input.end < fixture->file.len) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor next;
        assert(!binary_fixture_read(fixture->file.data + input.end,
            fixture->file.len - input.end, &input, &decoded, &batch, &next));
        assert(batch.count <= 3u);
        struct snag_binary_record records[3];
        struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
        struct snag_buf child = {.max = SNAG_MAX_EVENT_LINE};
        size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
        for (uint32_t i = 0u; i < batch.count; ++i) {
            uint64_t sequence;
            assert(!snag_binary_record_next(&batch, &cursor, &records[i], &sequence));
            if (sequence != receipt) continue;
            struct snag_binary_event event, outer;
            assert(!snag_binary_event_decode(&records[i], &event));
            bool embedded = event.kind == SNAG_BINARY_IRC_ADMITTED;
            if (embedded) {
                outer = event;
                assert(!snag_binary_event_decode(&outer.data.irc_admitted.input, &event));
            }
            bool direct = event.kind == SNAG_BINARY_INPUT_RECEIVED;
            bool started = event.kind == SNAG_BINARY_TURN_STARTED;
            bool steering = event.kind == SNAG_BINARY_STEERING_ADDED ||
                event.kind == SNAG_BINARY_IRC_REPLY_REMINDER;
            assert(direct || started || steering || event.kind == SNAG_BINARY_FUTURE_TURN_QUEUED ||
                event.kind == SNAG_BINARY_FUTURE_TURN_EDITED);
            struct snag_binary_input_reference *text = direct ? &event.data.input.text_ref :
                started ? &event.data.started.text_ref :
                steering ? &event.data.steering_input.text_ref : &event.data.queued.text_ref;
            struct snag_binary_input_reference *content = direct ? &event.data.input.content_ref :
                started ? &event.data.started.content_ref :
                steering ? &event.data.steering_input.content_ref : &event.data.queued.content_ref;
            struct snag_binary_input_reference *instructions = direct ?
                &event.data.input.instructions_ref : started ?
                &event.data.started.instructions_ref : NULL;
            if (fields & 1u) {
                enum snag_binary_input_leaf field = fault == RECEIPT_TRANSCRIPT ?
                    SNAG_BINARY_INPUT_VOICE_TRANSCRIPT : fault == RECEIPT_REQUEST ?
                    SNAG_BINARY_INPUT_VOICE_REQUEST : SNAG_BINARY_INPUT_TEXT;
                *text = fixture_declaration(fixture, canonical, field);
                if (direct) event.data.input.text = (struct snag_binary_text){0};
                else if (started) event.data.started.text = (struct snag_binary_text){0};
                else if (steering) event.data.steering_input.text = (struct snag_binary_text){0};
                else event.data.queued.text = (struct snag_binary_text){0};
            }
            if (fields & 2u) {
                *content = fixture_declaration(fixture, canonical, SNAG_BINARY_INPUT_CONTENT);
                if (direct) event.data.input.content = (struct snag_binary_content){0};
                else if (started) event.data.started.content = (struct snag_binary_content){0};
                else if (steering)
                    event.data.steering_input.content = (struct snag_binary_content){0};
                else event.data.queued.content = (struct snag_binary_content){0};
            }
            if (fields & 4u) {
                assert(instructions);
                *instructions = fixture_declaration(fixture, canonical,
                    SNAG_BINARY_INPUT_INSTRUCTIONS);
                if (direct) event.data.input.instructions = (struct snag_binary_instructions){0};
                else event.data.started.instructions = (struct snag_binary_instructions){0};
            }
            if (fault == RECEIPT_ROLE) text->field = SNAG_BINARY_INPUT_VOICE_REQUEST;
            if (fault == RECEIPT_OFFSET) ++text->target.offset;
            if (fault == RECEIPT_SIZE) --text->target.size;
            if (fault == RECEIPT_CONTENT_LEAF) content->target = text->target;
            if (fault == RECEIPT_INSTRUCTION_LEAF) {
                instructions->target =
                    fixture_reference(fixture, canonical, SNAG_BINARY_INPUT_TEXT);
            }
            if (fault == RECEIPT_INSTRUCTION_OFFSET) {
                ++instructions->target.offset;
            }
            if (fault == RECEIPT_CONTENT_SIZE) ++content->target.size;
            if (fault == RECEIPT_INSTRUCTION_SIZE) ++instructions->target.size;
            if (fault == RECEIPT_CHAIN) {
                if (fields & 1u) text->target.sequence = canonical;
                else instructions->target.sequence = canonical;
            }
            if (fault == RECEIPT_TURN) memset(event.data.steering_input.turn, 0xdd, 16u);
            if (fault == RECEIPT_ID) memset(event.data.steering_input.id, 0xbb, 16u);
            if (fault == RECEIPT_REMINDER) event.kind = SNAG_BINARY_IRC_REPLY_REMINDER;
            if (fault == RECEIPT_GOAL) event.data.started.origin = SNAG_BINARY_TURN_GOAL;
            if (fault == RECEIPT_READ_ONLY) event.data.started.read_only = true;
            if (fault == RECEIPT_QUEUE_OWNER) {
                event.data.started.queue_id[0] ^= 1u;
            }
            if (fault == RECEIPT_QUEUE_CREATION) {
                ++event.data.started.queue_seq;
            }
            if (fields & 24u) {
                assert(event.kind == SNAG_BINARY_FUTURE_TURN_QUEUED && event.data.queued.has_voice);
                struct snag_binary_voice_source *voice = &event.data.queued.voice;
                for (unsigned int i = 0u; i < 2u; ++i) {
                    if (!(fields & (8u << i))) continue;
                    enum snag_binary_input_leaf role = i ? SNAG_BINARY_INPUT_VOICE_REQUEST :
                        SNAG_BINARY_INPUT_VOICE_TRANSCRIPT;
                    if (fault == RECEIPT_VOICE_TEXT) role = SNAG_BINARY_INPUT_TEXT;
                    if (fault == RECEIPT_VOICE_ALIAS) role = i ?
                        SNAG_BINARY_INPUT_VOICE_TRANSCRIPT : SNAG_BINARY_INPUT_VOICE_REQUEST;
                    struct snag_binary_input_reference *ref = i ?
                        &voice->request_ref : &voice->transcript_ref;
                    *ref = fixture_declaration(fixture, canonical, role);
                    if (i) voice->request = (struct snag_binary_text){0};
                    else voice->transcript = (struct snag_binary_text){0};
                }
                struct snag_binary_input_reference *ref = fields & 8u ?
                    &voice->transcript_ref : &voice->request_ref;
                if (fault == RECEIPT_VOICE_ROLE) ref->field = fields & 8u ?
                    SNAG_BINARY_INPUT_VOICE_REQUEST : SNAG_BINARY_INPUT_VOICE_TRANSCRIPT;
                if (fault == RECEIPT_VOICE_OFFSET) ++ref->target.offset;
                if (fault == RECEIPT_VOICE_SIZE) --ref->target.size;
                if (fault == RECEIPT_VOICE_CHAIN) ref->target.sequence = canonical;
                if (fault == RECEIPT_VOICE_CONNECTION) voice->connection_id[0] ^= 1u;
                if (fault == RECEIPT_VOICE_INPUT) voice->input_id =
                    (struct snag_binary_text){(const unsigned char *)"foreign-input", 13u};
                if (fault == RECEIPT_VOICE_ID) event.data.queued.id[0] ^= 1u;
                if (fault == RECEIPT_VOICE_TURN) {
                    event.data.queued.while_kind = SNAG_BINARY_WHILE_ID;
                    memset(event.data.queued.while_id, 0xdd, 16u);
                }
            }
            assert(!snag_binary_event_encode(embedded ? &child : &payload, &event));
            if (embedded) {
                outer.data.irc_admitted.input.payload = child.data;
                outer.data.irc_admitted.input.size = child.len;
                outer.data.irc_admitted.input.version = snag_binary_event_version(event.kind);
                assert(!snag_binary_event_encode(&payload, &outer));
            }
            records[i].payload = payload.data;
            records[i].size = payload.len;
            if (!embedded) {
                records[i].kind = event.kind;
                records[i].version = snag_binary_event_version(event.kind);
            }
            const char *type = NULL;
            json_t *data = NULL;
            assert(snag_binary_legacy_decode(&records[i], &type, &data) < 0 && errno == ENOTSUP);
            assert(!type && !data);
        }
        assert(!binary_fixture_append(file, &output, records, batch.count, next.turns, &output));
        snag_buf_free(&child);
        snag_buf_free(&payload);
        input = next;
    }
    snag_buf_free(&decoded);
}

void test_store_binary_checkpoint_state(const struct snag_session *);

static void
same_core_state(const struct snag_session *expected, const struct snag_session *actual)
{
    test_store_binary_checkpoint_state(expected);
    test_store_binary_checkpoint_state(actual);
    json_t *left = snag_checkpoint_state_encode(expected);
    json_t *right = snag_checkpoint_state_encode(actual);
    assert(left && right);
    const char *storage[] = {"prev_sha256", "log_end", "checkpoint_offset", "checkpoint_seq"};
    for (size_t i = 0u; i < sizeof(storage) / sizeof(storage[0]); ++i) {
        json_object_del(left, storage[i]);
        json_object_del(right, storage[i]);
    }
    assert(json_equal(left, right));
    json_decref(left);
    json_decref(right);
    assert(expected->next_seq == actual->next_seq);
    assert(expected->last_time_ms == actual->last_time_ms);
}

static unsigned int bounded_reference_suffixes;

static void
bounded_reference_suffix(struct snag_session *source, const struct snag_session *expected,
    const struct snag_binary_anchor *stop, uint64_t turn_sequence)
{
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    assert(snag_pread(source->log_fd, header, sizeof(header), 0) == (ssize_t)sizeof(header));
    struct snag_binary_identity identity;
    struct snag_binary_anchor start;
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &start));
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    for (;;) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor next;
        assert(!snag_binary_batch_read(source->log_fd, stop->end, &start, &scratch, &batch, &next));
        if (turn_sequence < next.next_seq) break;
        start = next;
    }
    snag_buf_free(&scratch);
    assert(start.next_seq > 1u && start.next_seq <= turn_sequence);
    struct snag_session prefix, loaded;
    snag_session_init(&prefix);
    snag_session_init(&loaded);
    struct snag_binary_checkpoint_sources origins = {0}, restored = {0};
    struct snag_binary_recovery recovery = {0};
    char error[128] = {0};
    assert(!snag_store_reconcile_binary_prefix(source, &prefix, &start, NULL, NULL,
        &recovery, &origins, error, sizeof(error)));
    struct snag_buf core = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_core_encode(&core, &origins, &prefix));
    struct snag_binary_checkpoint_frame frame = {.identity = identity, .boundary = start,
        .generation = 1u, .core = {.version = SNAG_BINARY_CORE_VERSION,
            .data = (const unsigned char *)core.data, .size = core.len}};
    struct snag_buf locations = {.max = SIZE_MAX};
    struct snag_binary_checkpoint_index access;
    binary_fixture_access(source->log_fd, &start, &locations, &access);
    assert(!snag_binary_checkpoint_core_read(source->log_fd, &frame, &access, &loaded, &restored));
    int64_t position = snag_seek(source->log_fd, 0, SEEK_CUR);
    assert(!snag_store_reduce_binary_suffix_prefix(source, &loaded, &start, stop, &access,
        NULL, NULL, NULL, NULL, &recovery, &restored, error, sizeof(error)));
    same_core_state(expected, &loaded);
    assert(loaded.log_end == (int64_t)stop->end && !recovery.incomplete_tail_bytes);
    snag_session_close(&loaded);
    snag_binary_checkpoint_sources_free(&restored);
    unsigned char root[32];
    assert(!snag_binary_index_tree_root(&access.tree, root));
    struct snag_buf empty_bytes = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_index_encode(&empty_bytes, &identity, &start,
        &access.tree, NULL, 0u));
    struct snag_binary_checkpoint_index empty;
    assert(!snag_binary_checkpoint_index_decode(empty_bytes.data, empty_bytes.len,
        &identity, &start, root, &empty));
    snag_session_init(&loaded);
    assert(!snag_binary_checkpoint_core_read(source->log_fd, &frame, &access, &loaded, &restored));
    assert(snag_store_reduce_binary_suffix_prefix(source, &loaded, &start, stop, &empty,
        NULL, NULL, NULL, NULL, &recovery, &restored, error, sizeof(error)) < 0 && errno == ENOENT);
    assert(recovery.problem_seq == turn_sequence && !recovery.incomplete_tail_bytes &&
        snag_seek(source->log_fd, 0, SEEK_CUR) == position);
    ++bounded_reference_suffixes;
    snag_session_close(&loaded);
    snag_session_close(&prefix);
    snag_binary_checkpoint_sources_free(&restored);
    snag_binary_checkpoint_sources_free(&origins);
    snag_buf_free(&empty_bytes);
    snag_buf_free(&locations);
    snag_buf_free(&core);
}

static void
file_digest(int fd, unsigned char digest[32])
{
    struct snag_sha256 hash;
    snag_sha256_init(&hash);
    unsigned char bytes[65536];
    int64_t offset = 0;
    for (;;) {
        ssize_t got = snag_pread(fd, bytes, sizeof(bytes), offset);
        assert(got >= 0);
        if (!got) break;
        snag_sha256_update(&hash, bytes, (size_t)got);
        offset += got;
    }
    snag_sha256_final(&hash, digest);
}

static int
import_checked(struct snag_session *source, int output, struct snag_session *restored,
    struct snag_binary_import_result *result)
{
    struct snag_session before = *source, saved = *restored;
    unsigned char hash[32], after[32];
    file_digest(source->log_fd, hash);
    int64_t position = snag_seek(source->log_fd, 0, SEEK_CUR);
    assert(position >= 0);
    char error[512] = {0};
    snag_binary_checkpoint_sources_free(&result->sources);
    int rc = snag_store_import_binary_journal(source, output, restored, result,
        error, sizeof(error));
    int code = errno;
    if (rc < 0) {
        assert(*error && !memcmp(&saved, restored, sizeof(saved)));
    } else {
        test_store_binary_texts_state(output, &result->native.verified,
            &result->sources.texts, restored);
        test_store_binary_calls_state(output, &result->native.verified,
            &result->sources.calls, restored);
        test_store_binary_processes_state(output, &result->native.verified,
            &result->sources, restored);
        test_store_binary_payloads_state(output, &result->native.verified,
            &result->sources, restored);
        test_store_binary_inputs_state(output, &result->native.verified,
            &result->sources, restored);
        test_store_binary_core_state(output, &result->native.verified,
            &result->sources, restored);
    }
    assert(!memcmp(&before, source, sizeof(before)));
    assert(snag_seek(source->log_fd, 0, SEEK_CUR) == position);
    file_digest(source->log_fd, after);
    assert(!memcmp(hash, after, sizeof(hash)));
    errno = code;
    return rc;
}

struct import_counts {
    size_t references, literals, cross_batch;
    uint64_t referenced_bytes;
    size_t input_fields[3], plain_turns, input_cross_batch;
    struct snag_binary_input_reference last_input[3]; /* Last reference-bearing turn. */
};

static void
count_import_output(struct import_counts *counts, const struct snag_binary_public_value *value,
    uint64_t batch_first)
{
    if (value->source.first.sequence) {
        assert(!value->item.text.data && !value->item.text.size);
        ++counts->references;
        counts->referenced_bytes += value->source.bytes;
        counts->cross_batch += value->source.first.sequence < batch_first;
    } else {
        ++counts->literals;
    }
}

static void
count_import_snapshot(struct import_counts *counts, const struct snag_binary_record *record,
    uint64_t batch_first)
{
    if (record->kind < SNAG_BINARY_RESPONSE_INTERRUPTED ||
        record->kind > SNAG_BINARY_RESPONSE_COMPLETED) {
        return;
    }
    struct snag_binary_event event;
    assert(!snag_binary_event_decode(record, &event));
    struct snag_binary_public_items *partial = NULL;
    size_t offset = 0u;
    int rc;
    if (record->kind == SNAG_BINARY_RESPONSE_COMPLETED) {
        struct snag_binary_graph_item item;
        while ((rc = snag_binary_graph_items_next(&event.data.response_completed.items,
            &offset, &item)) == 0) {
            if (item.kind != SNAG_BINARY_ITEM_TOOL_CALL) {
                count_import_output(counts, &item.data.output, batch_first);
            }
        }
    } else {
        if (record->kind == SNAG_BINARY_RESPONSE_INTERRUPTED) {
            partial = &event.data.response_interrupted.partial;
        } else if (record->kind == SNAG_BINARY_RESPONSE_FAILED) {
            partial = &event.data.response_failed.partial;
        } else {
            partial = &event.data.response_correction.partial;
        }
        struct snag_binary_public_value value;
        while ((rc = snag_binary_public_items_next(partial, &offset, &value)) == 0) {
            count_import_output(counts, &value, batch_first);
        }
    }
    assert(rc == 1);
}

static void
count_import_turn(struct import_counts *counts, const struct snag_binary_record *record,
    uint64_t sequence, uint64_t batch_first, int fd, const struct snag_binary_anchor *verified)
{
    if (record->kind != SNAG_BINARY_TURN_STARTED) return;
    struct snag_binary_event event;
    assert(!snag_binary_event_decode(record, &event));
    struct snag_binary_turn_start *turn = &event.data.started;
    const enum snag_binary_input_leaf roles[] = {
        SNAG_BINARY_INPUT_TEXT, SNAG_BINARY_INPUT_CONTENT, SNAG_BINARY_INPUT_INSTRUCTIONS};
    if (!turn->text_ref.field && !turn->content_ref.field && !turn->instructions_ref.field) {
        ++counts->plain_turns;
        return;
    }
    counts->last_input[0] = turn->text_ref;
    counts->last_input[1] = turn->content_ref;
    counts->last_input[2] = turn->instructions_ref;
    for (size_t i = 0u; i < 3u; ++i) {
        const struct snag_binary_input_reference *reference = &counts->last_input[i];
        if (!reference->field) continue;
        assert(reference->field == roles[i]);
        assert(reference->target.sequence && reference->target.sequence < sequence);
        assert(i != 0u || (!turn->text.data && !turn->text.size));
        assert(i != 1u || (!turn->content.data && !turn->content.size));
        assert(i != 2u || (!turn->instructions.data && !turn->instructions.size));
        ++counts->input_fields[i];
        counts->input_cross_batch += reference->target.sequence < batch_first;
        struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
        struct snag_binary_batch batch;
        struct snag_binary_anchor before;
        assert(!snag_binary_batch_find(fd, verified, reference->target.sequence,
            &scratch, &batch, &before));
        struct snag_binary_ref original;
        assert(!snag_binary_input_ref_create(&batch, reference->target.sequence,
            reference->field, &original));
        assert(original.sequence == reference->target.sequence &&
            original.offset == reference->target.offset && original.size == reference->target.size);
        snag_buf_free(&scratch);
    }
}

static struct import_counts
check_import(struct snag_session *source, const struct snag_session *expected)
{
    struct import_counts counts = {0};
    int fd = temporary_fd();
    struct snag_session restored;
    snag_session_init(&restored);
    struct snag_binary_import_result result = {0};
    int rc = import_checked(source, fd, &restored, &result);
    if (rc < 0) {
        fprintf(stderr, "binary import failed: %s, legacy=%llu native=%llu\n",
            strerror(errno), (unsigned long long)result.legacy.problem_seq,
            (unsigned long long)result.native.problem_seq);
    }
    assert(!rc);
    same_core_state(expected, &restored);
    assert(!result.legacy.problem_seq && !result.native.problem_seq);
    assert(result.legacy.verified_records == expected->next_seq - 1u);
    assert(result.native.verified.next_seq == expected->next_seq);
    assert(result.native.verified.turns == expected->turn_count);
    assert(!strcmp(result.source_sha256, expected->prev_sha256));
    assert(!result.native.incomplete_tail_bytes);
    assert(snag_seek(fd, 0, SEEK_CUR) == restored.log_end);

    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    assert(snag_pread(fd, header, sizeof(header), 0) == sizeof(header));
    struct snag_binary_identity identity;
    struct snag_binary_anchor anchor;
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &anchor));
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    uint64_t markers = 0u;
    while (anchor.end < result.native.verified.end) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor next;
        assert(!snag_binary_batch_read(fd, result.native.verified.end, &anchor,
            &scratch, &batch, &next));
        size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
        struct snag_binary_record record;
        uint64_t sequence;
        while (snag_binary_record_next(&batch, &cursor, &record, &sequence) == 0) {
            count_import_snapshot(&counts, &record, anchor.next_seq);
            count_import_turn(&counts, &record, sequence, anchor.next_seq, fd,
                &result.native.verified);
            if (record.kind != SNAG_BINARY_LEGACY_CHECKPOINT) continue;
            struct snag_binary_legacy_checkpoint marker;
            assert(!snag_binary_legacy_checkpoint_decode(&record, &marker));
            assert(marker.end <= (uint64_t)result.legacy.verified_end && sequence > 1u);
            size_t size = (size_t)(marker.end - marker.start);
            unsigned char *bytes = malloc(size);
            assert(bytes && snag_pread(source->log_fd, bytes, size, (int64_t)marker.start) ==
                (ssize_t)size && bytes[size - 1u] == '\n');
            char error[512];
            json_t *event = snag_json_load_canonical_bounded(bytes, size - 1u,
                size - 1u, error, sizeof(error));
            free(bytes);
            assert(event && !strcmp(snag_json_string(event, "type"), "session_checkpoint"));
            assert((uint64_t)json_integer_value(json_object_get(event, "seq")) == sequence);
            const char *hash = snag_json_string(event, "event_sha256");
            for (size_t i = 0u; i < sizeof(marker.digest); ++i) {
                const char *hex = "0123456789abcdef";
                assert(hex[marker.digest[i] >> 4u] == hash[i * 2u]);
                assert(hex[marker.digest[i] & 15u] == hash[i * 2u + 1u]);
            }
            json_decref(event);
            ++markers;
        }
        anchor = next;
    }
    assert(markers == result.legacy.discarded_checkpoints);
    snag_buf_free(&scratch);
    snag_binary_checkpoint_sources_free(&result.sources);
    snag_session_close(&restored);
    assert(!close(fd));
    return counts;
}

static bool
stopped_stage_cancel(void *opaque)
{
    uint64_t *remaining = opaque;
    if (!*remaining) return true;
    --*remaining;
    return false;
}

static void
check_stopped_native_stage(struct snag_session *source)
{
    struct snag_session before = *source;
    struct snag_session target;
    snag_session_init(&target);
    target.log_fd = snag_create_private_at(source->dir_fd, "stage-journal", true);
    target.lock_fd = snag_create_private_at(source->dir_fd, "stage-lock", true);
    int index = snag_create_private_at(source->dir_fd, "stage-index", true);
    assert(target.log_fd >= 0 && target.lock_fd >= 0 && index >= 0);
    assert(!snag_lock_file(target.lock_fd, false));
    struct snag_session empty = target;
    unsigned char hash[32], after[32];
    file_digest(source->log_fd, hash);
    int64_t position = snag_seek(source->log_fd, 0, SEEK_CUR);
    assert(position >= 0);
    struct snag_binary_import_result result = {0};
    char error[512] = {0};
    assert(snag_store_stage_binary_session(source, &target, source->lock_fd, &result,
        NULL, error, sizeof(error)) < 0 && errno == EINVAL);
    assert(!memcmp(&target, &empty, sizeof(empty)));
    assert(snag_store_stage_binary_session(source, &target, target.log_fd, &result,
        NULL, error, sizeof(error)) < 0 && errno == EINVAL);
    assert(!memcmp(&target, &empty, sizeof(empty)));
    uint64_t attempts[] = {0u, 4u, source->next_seq + 2u};
    for (size_t i = 0u; i < sizeof(attempts) / sizeof(attempts[0]); ++i) {
        struct snag_context_control control = {.cancelled = stopped_stage_cancel,
            .opaque = &attempts[i]};
        assert(snag_store_stage_binary_session(source, &target, index, &result,
            &control, error, sizeof(error)) < 0 && errno == ECANCELED);
        assert(!memcmp(&target, &empty, sizeof(empty)));
        assert(!memcmp(source, &before, sizeof(before)));
        assert(snag_seek(source->log_fd, 0, SEEK_CUR) == position);
        file_digest(source->log_fd, after);
        assert(!memcmp(hash, after, sizeof(hash)));
        assert(!snag_truncate(target.log_fd, 0) && !snag_truncate(index, 0));
        assert(!result.sources.texts.through);
    }
    int rc = snag_store_stage_binary_session(source, &target, index, &result,
        NULL, error, sizeof(error));
    if (rc < 0) fprintf(stderr, "stopped native stage failed: %s\n", error);
    assert(!rc && target.binary && result.native.batches == 4u);
    assert(!strcmp(source->id, target.id) && source->next_seq == target.next_seq);
    assert(!memcmp(&before, source, sizeof(before)));
    assert(snag_seek(source->log_fd, 0, SEEK_CUR) == position);
    file_digest(source->log_fd, after);
    assert(!memcmp(hash, after, sizeof(hash)));
    test_store_binary_core_state(target.log_fd, &result.native.verified,
        &result.sources, &target);
    test_store_binary_payloads_state(target.log_fd, &result.native.verified,
        &result.sources, &target);
    test_store_binary_inputs_state(target.log_fd, &result.native.verified,
        &result.sources, &target);
    struct snag_binary_anchor boundary;
    struct snag_binary_index_tree tree;
    assert(!snag_session_binary_checkpoint_capture(&target, &boundary, &tree, NULL,
        error, sizeof(error)));
    assert(boundary.next_seq == source->next_seq && tree.count == source->next_seq - 1u);
    assert(!snag_session_binary_index_setup(&target, index, error, sizeof(error)));
    snag_binary_checkpoint_sources_free(&result.sources);
    snag_session_close(&target);
    assert(!close(index));
    assert(!snag_unlink_at(source->dir_fd, "stage-journal", false));
    assert(!snag_unlink_at(source->dir_fd, "stage-lock", false));
    assert(!snag_unlink_at(source->dir_fd, "stage-index", false));
    assert(!memcmp(&before, source, sizeof(before)));
}

static void
test_stopped_directory_conversion(struct snag_store *store, const char *cwd)
{
    for (unsigned int variant = 0u; variant < 2u; ++variant) {
        struct snag_session source, resumed;
        snag_session_init(&source);
        snag_session_init(&resumed);
        char error[512] = {0};
        assert(!legacy_fixture_create(store, &source, cwd, "default", "fixture", "default",
            error, sizeof(error)));
        commit_data(&source, "goal_started", json_pack("{s:s,s:s}",
            "goal_id", GOAL_ID, "prompt", "retained conversion goal"));
        assert(snag_seek(source.log_fd, 7, SEEK_SET) == 7);
        unsigned char hash[32], after[32];
        file_digest(source.log_fd, hash);
        char id[SNAG_ID_HEX_LEN + 1u];
        memcpy(id, source.id, sizeof(id));
        uint64_t next = source.next_seq;
        if (variant) {
            /* Recreate interruption after retention but before native selection. */
            assert(!snag_mkdir_private_at(source.dir_fd, ".legacy-source"));
            int old = snag_open_read_security_at(source.dir_fd, ".legacy-source", true);
            assert(old >= 0);
            assert(!snag_rename_at(source.dir_fd, "events.jsonl", old, "events.jsonl"));
            assert(!snag_sync_dir(old) && !snag_sync_dir(source.dir_fd));
            assert(!close(old));
        }
        struct snag_session before = source;
        struct snag_binary_import_result result = {0};
        uint64_t stop = 0u;
        struct snag_context_control control = {.cancelled = stopped_stage_cancel, .opaque = &stop};
        assert(snag_store_convert_binary_directory(&source, &result, &control,
            error, sizeof(error)) < 0 && errno == ECANCELED);
        assert(!memcmp(&source, &before, sizeof(before)));
        snag_file_info info;
        assert(snag_lstat_at(source.dir_fd, "journal.bin", &info) < 0 && errno == ENOENT);
        int rc = snag_store_convert_binary_directory(&source, &result, NULL,
            error, sizeof(error));
        if (rc < 0) fprintf(stderr, "stopped directory conversion failed: %s\n", error);
        assert(!rc);
        assert(!memcmp(&source, &before, sizeof(before)));
        assert(snag_seek(source.log_fd, 0, SEEK_CUR) == 7);
        file_digest(source.log_fd, after);
        assert(!memcmp(hash, after, sizeof(hash)));
        int old = snag_open_read_security_at(source.dir_fd, ".legacy-source", true);
        assert(old >= 0);
        int retained = snag_open_read_security_at(old, "events.jsonl", false);
        assert(retained >= 0);
        file_digest(retained, after);
        assert(!memcmp(hash, after, sizeof(hash)));
        assert(!close(retained) && !close(old));
        assert(snag_open_private_append_at(source.dir_fd, "events.jsonl", false) < 0 &&
            errno == ENOENT);
        snag_binary_checkpoint_sources_free(&result.sources);
        assert(snag_store_convert_binary_directory(&source, &result, NULL,
            error, sizeof(error)) < 0 && errno == EEXIST);
        snag_session_close(&source);
        assert(!snag_session_open(store, &resumed, id, error, sizeof(error)));
        /* Bootstrap ACKs one optional receipt after the unchanged imported prefix. */
        assert(resumed.binary && resumed.next_seq == next + 1u);
        assert(resumed.checkpoint_seq == next);
        assert(!strcmp(resumed.goal_id, GOAL_ID));
        assert(resumed.goal_prompt && !strcmp(resumed.goal_prompt, "retained conversion goal"));
        snag_session_close(&resumed);
    }
}

static void
test_import_batches(struct snag_store *store, const char *cwd)
{
    char error[512];
    struct snag_session original, expected, restored;
    snag_session_init(&original);
    snag_session_init(&expected);
    snag_session_init(&restored);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    size_t size = SNAG_BINARY_BATCH_TARGET + 31u;
    char *text = malloc(size + 1u);
    assert(text);
    memset(text, 'x', size);
    text[size] = '\0';
    for (unsigned int i = 0u; i < 70u; ++i) {
        /* Cross the ordinary batch target, grow its record table, then emit
         * one above-target record between ordinary batches. */
        size_t length = i == 35u ? size : (i < 35u ? 32000u : 17u);
        commit_data(&original, "rule_log", json_pack("{s:n,s:o,s:i}", "chain", "message",
            json_stringn(text, length), "rule", (int)i));
    }
    free(text);
    assert(!snag_session_checkpoint(&original, error, sizeof(error)));
    commit_data(&original, "goal_started", json_pack("{s:s,s:s}",
        "goal_id", GOAL_ID, "prompt", "staged goal authority"));
    struct snag_legacy_recovery recovery;
    assert(!snag_store_reconcile_legacy(&original, &expected, NULL, NULL,
        &recovery, error, sizeof(error)));
    assert(snag_seek(original.log_fd, 7, SEEK_SET) == 7);
    check_stopped_native_stage(&original);
    check_import(&original, &expected);
    int fd = temporary_fd();
    struct snag_binary_import_result result = {0};
    assert(!import_checked(&original, fd, &restored, &result));
    assert(result.native.batches == 4u);
    unsigned char first[32], second[32];
    file_digest(fd, first);
    assert(!snag_truncate(fd, 0));
    assert(!import_checked(&original, fd, &restored, &result));
    file_digest(fd, second);
    assert(!memcmp(first, second, sizeof(first)));

    int duplicate = dup(original.log_fd);
    assert(duplicate >= 0);
    assert(import_checked(&original, original.log_fd, &restored, &result) < 0 && errno == EINVAL);
    assert(import_checked(&original, duplicate, &restored, &result) < 0 && errno == EINVAL);
    assert(!close(duplicate));
    assert(import_checked(&original, original.lock_fd, &restored, &result) < 0 && errno == EINVAL);
    assert(import_checked(&original, fd, &restored, &result) < 0 && errno == EINVAL);
    file_digest(fd, second);
    assert(!memcmp(first, second, sizeof(first)));
    int pipes[2];
    assert(!pipe(pipes));
    assert(import_checked(&original, pipes[1], &restored, &result) < 0 && errno == EINVAL);
    assert(!close(pipes[0]) && !close(pipes[1]));

    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snajpagent-stage-readonly-XXXXXX");
    assert(path);
    int writable = mkstemp(path);
    int readonly = open(path, O_RDONLY);
    assert(writable >= 0 && readonly >= 0);
    assert(!unlink(path) && !close(writable));
    free(path);
    assert(import_checked(&original, readonly, &restored, &result) < 0 && errno == EBADF);
    assert(!close(readonly));

#ifndef _WIN32
    /* Fail inside a batch write after the header succeeded. The caller retains
     * the partial output, while imported state and original bytes stay intact. */
    assert(!snag_truncate(fd, 0));
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        struct rlimit limit = {SNAG_BINARY_HEADER_SIZE + 16u, SNAG_BINARY_HEADER_SIZE + 16u};
        assert(signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
        assert(!setrlimit(RLIMIT_FSIZE, &limit));
        assert(import_checked(&original, fd, &restored, &result) < 0 && errno == EFBIG);
        snag_file_info partial;
        assert(!snag_fstat(fd, &partial) && partial.st_size == SNAG_BINARY_HEADER_SIZE + 16u);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif /* !_WIN32 */

    /* An incomplete source suffix remains intact and is reported, while the
     * private output contains only the verified committed prefix. */
    assert(snag_seek(original.log_fd, 0, SEEK_END) == original.log_end);
    assert(!snag_write_full(original.log_fd, "torn", 4u));
    assert(!snag_truncate(fd, 0));
    assert(!import_checked(&original, fd, &restored, &result));
    assert(result.legacy.incomplete_tail_bytes == 4u);
    same_core_state(&expected, &restored);
    file_digest(fd, second);
    assert(!memcmp(first, second, sizeof(first)));

    /* Completing those invalid bytes makes the suffix a corrupt committed
     * record. Earlier staged batches stay provisional, state is not adopted. */
    assert(!snag_write_full(original.log_fd, "\n", 1u));
    assert(!snag_truncate(fd, 0));
    assert(import_checked(&original, fd, &restored, &result) < 0);
    assert(result.legacy.problem_seq == expected.next_seq);
    assert(result.legacy.problem_start == expected.log_end);
    snag_file_info info;
    assert(!snag_fstat(fd, &info) && info.st_size > SNAG_BINARY_HEADER_SIZE);
    assert(!close(fd));
    snag_binary_checkpoint_sources_free(&result.sources);
    snag_session_close(&restored);
    snag_session_close(&expected);
    snag_session_close(&original);
}

static void
test_import_markers(struct replay_fixture *fixture, struct snag_session *source,
    struct snag_session *restored)
{
    struct snag_binary_legacy_checkpoint marker = {.start = 1u, .end = 2u};
    struct snag_buf bytes = {.max = SNAG_BINARY_LEGACY_CHECKPOINT_SIZE};
    struct snag_buf file = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    assert(!snag_binary_legacy_checkpoint_encode(&bytes, &marker));
    struct snag_binary_record record = {.kind = SNAG_BINARY_LEGACY_CHECKPOINT,
        .version = 1u, .flags = SNAG_BINARY_RECORD_OPTIONAL,
        .timestamp_ms = fixture->identity.created_ms + 100u,
        .payload = bytes.data, .size = bytes.len};
    struct snag_binary_recovery recovery;
    append_record(fixture, &file, &record, fixture->turns);
    assert(!replay_check(source, restored, &recovery, file.data, file.len));
    assert(restored->next_seq == fixture->anchor.next_seq + 1u);
    for (unsigned int fault = 0u; fault < 6u; ++fault) {
        struct snag_binary_record bad = record;
        unsigned char payload[SNAG_BINARY_LEGACY_CHECKPOINT_SIZE];
        memcpy(payload, bytes.data, sizeof(payload));
        bad.payload = payload;
        if (fault == 0u) bad.version++;
        if (fault == 1u) bad.flags = 0u;
        if (fault == 2u) bad.size--;
        if (fault == 3u) payload[0] = 0u;
        if (fault == 4u) payload[8] = 1u;
        if (fault == 5u) payload[15] = 0x80u;
        append_record(fixture, &file, &bad, fixture->turns);
        assert(replay_check(source, restored, &recovery, file.data, file.len) < 0);
        assert(errno == EINVAL && recovery.problem_seq == fixture->anchor.next_seq);
    }
    snag_buf_free(&bytes);
    snag_buf_free(&file);
}

static void
prefix_through(const struct snag_buf *file, uint64_t wanted, struct snag_buf *prefix)
{
    struct snag_buf decoded = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_identity identity;
    struct snag_binary_anchor anchor;
    assert(!snag_binary_header_decode(file->data, file->len, &identity, &anchor));
    while (anchor.end < file->len) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor next;
        assert(!binary_fixture_read(file->data + anchor.end,
            file->len - anchor.end, &anchor, &decoded, &batch, &next));
        if (wanted >= batch.first_seq && wanted < next.next_seq) {
            size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
            uint64_t turns = anchor.turns;
            uint32_t count = (uint32_t)(wanted - batch.first_seq + 1u);
            struct snag_binary_record *records = calloc(count, sizeof(*records));
            assert(records);
            for (uint32_t i = 0u; i < count; ++i) {
                uint64_t sequence;
                assert(!snag_binary_record_next(&batch, &cursor, &records[i], &sequence));
                if (records[i].kind == SNAG_BINARY_TURN_STARTED) ++turns;
            }
            snag_buf_reset(prefix);
            assert(!snag_buf_append(prefix, file->data, (size_t)anchor.end));
            assert(!binary_fixture_append(prefix, &anchor, records, count, turns, NULL));
            free(records);
            snag_buf_free(&decoded);
            return;
        }
        anchor = next;
    }
    assert(false);
}

static void
receipt_references(const struct replay_fixture *fixture, const struct turn_sources *sequences,
    struct snag_session *source, struct snag_session *restored, const struct snag_session *expected,
    const json_t *pending, const json_t *paths)
{
    bool edited = !pending && sequences->text != sequences->creation;
    struct replay_fixture receipts = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX},
        .batch_records = fixture->batch_records};
    struct replay_fixture edits = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX},
        .batch_records = fixture->batch_records};
    struct snag_buf file = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    struct snag_buf prefix = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    struct snag_binary_recovery recovery;
    unsigned int all = pending ? 7u : 3u;
    for (unsigned int fields = 1u; fields <= all; ++fields) {
        rewrite_receipt(fixture, sequences->creation, sequences->canonical,
            fields, RECEIPT_VALID, &receipts.file);
        assert(!replay_check(source, restored, &recovery, receipts.file.data, receipts.file.len));
        same_core_state(expected, restored);
        prefix_through(&receipts.file, sequences->creation, &prefix);
        assert(!replay_check(source, restored, &recovery, prefix.data, prefix.len));
        if (pending) {
            assert(json_equal(restored->pending_input, pending));
        } else {
            assert(restored->pending_queue_count == 2u);
            assert(restored->pending_queue[1].seq == sequences->creation);
            assert(!strcmp(restored->pending_queue[1].queue_id, GOAL_ID));
            assert(!restored->pending_queue[1].read_only);
        }
        const struct replay_fixture *current = &receipts;
        if (edited) {
            /* A new edit explicitly reuses another queue's literal text. Its
             * own receipt/queue identity remains the authoritative transition. */
            rewrite_receipt(&receipts, sequences->text, sequences->other,
                1u, RECEIPT_VALID, &edits.file);
            assert(!replay_check(source, restored, &recovery, edits.file.data, edits.file.len));
            same_core_state(expected, restored);
            current = &edits;
        }
        bool same_paths = pending && json_equal(paths, json_object_get(pending, "instructions"));
        rewrite_turn(current, sequences, same_paths ? TURN_REF_ALL : TURN_REF_BOTH, &file);
        assert(!replay_check(source, restored, &recovery, file.data, file.len));
        same_core_state(expected, restored);
        prefix_through(&file, sequences->turn, &prefix);
        assert(!replay_check(source, restored, &recovery, prefix.data, prefix.len));
        assert(restored->active_turn && restored->turn_count == 1u);
        if (paths) assert(json_equal(restored->active_instructions, paths));
        struct turn_sources wrong = *sequences;
        wrong.other = pending ? sequences->alternate :
            edited ? sequences->creation : sequences->other;
        rewrite_turn(current, &wrong, TURN_REF_OTHER_TEXT, &file);
        assert(replay_check(source, restored, &recovery, file.data, file.len) < 0);
        assert(errno == EINVAL && recovery.problem_seq == sequences->turn);
        if (edited || (fields & 1u)) {
            rewrite_turn(current, sequences, TURN_REF_RECEIPT_CHAIN, &file);
            assert(replay_check(source, restored, &recovery, file.data, file.len) < 0);
            assert(errno == EINVAL && recovery.problem_seq == sequences->turn);
        }
        if (fields & 1u) {
            /* A later receipt can reuse the original again, but cannot point
             * at this receipt's reference marker as though it were a literal. */
            rewrite_receipt(current, sequences->future, sequences->creation,
                1u, RECEIPT_VALID, &file);
            assert(!replay_check(source, restored, &recovery, file.data, file.len));
            same_core_state(expected, restored);
            rewrite_receipt(current, sequences->future, sequences->creation,
                1u, RECEIPT_CHAIN, &file);
            assert(replay_check(source, restored, &recovery, file.data, file.len) < 0);
            assert(errno == EINVAL && recovery.problem_seq == sequences->future);
        }
    }
    for (enum receipt_fault fault = RECEIPT_ROLE; fault <= RECEIPT_INSTRUCTION_SIZE; ++fault) {
        if (!pending && (fault == RECEIPT_INSTRUCTION_LEAF || fault == RECEIPT_INSTRUCTION_SIZE))
            continue;
        rewrite_receipt(fixture, sequences->creation, sequences->canonical,
            all, fault, &file);
        assert(replay_check(source, restored, &recovery, file.data, file.len) < 0);
        assert(errno == EINVAL && recovery.problem_seq == sequences->creation);
    }
    const uint64_t noncausal[] = {sequences->creation, sequences->future};
    for (size_t i = 0u; i < sizeof(noncausal) / sizeof(noncausal[0]); ++i) {
        rewrite_receipt(fixture, sequences->creation, noncausal[i], 1u, RECEIPT_VALID, &file);
        assert(replay_check(source, restored, &recovery, file.data, file.len) < 0);
        assert(errno == EINVAL && recovery.problem_seq == sequences->creation);
    }
    if (pending) {
        /* This also exercises literal fields inside an older IRC admission as
         * canonical sources for a subsequent plain receipt. */
        rewrite_receipt(fixture, sequences->future, sequences->creation,
            all, RECEIPT_VALID, &file);
        assert(!replay_check(source, restored, &recovery, file.data, file.len));
        same_core_state(expected, restored);
        prefix_through(&file, sequences->future, &prefix);
        assert(!replay_check(source, restored, &recovery, prefix.data, prefix.len));
        assert(json_equal(restored->pending_input, pending));
    }
    snag_buf_free(&prefix);
    snag_buf_free(&file);
    snag_buf_free(&edits.file);
    snag_buf_free(&receipts.file);
}

static json_t *
queue_data(const char *id, const char *text, const char *while_turn)
{
    return checked_json(json_pack("{s:s,s:s,s:b,s:s,s:[{s:s,s:s}]}",
        "queue_id", id, "text", text, "read_only", 0, "while_turn_id", while_turn,
        "content", "type", "input_text", "text", "original content"));
}

static void
test_queued_references(struct snag_store *store, const char *cwd, bool edited)
{
    const char *turn_id = "cccccccccccccccccccccccccccccccc";
    const char *cancelled_id = "dddddddddddddddddddddddddddddddd";
    const char *text = "The same queue text deliberately appears in different receipts, "
        "including an unchanged edit; equal bytes do not establish original field ownership.";
    char error[512];
    struct snag_session original, expected, source, restored;
    snag_session_init(&original);
    snag_session_init(&expected);
    snag_session_init(&source);
    snag_session_init(&restored);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    struct turn_sources sequences;
    json_t *canonical = queue_data(cancelled_id, text, "");
    assert(!snag_json_set_new(canonical, "read_only", json_true()));
    sequences.canonical = commit_data(&original, "future_turn_queued", canonical);
    sequences.creation = commit_data(&original, "future_turn_queued",
        queue_data(GOAL_ID, text, ""));
    sequences.other = commit_data(&original, "future_turn_queued", queue_data(OTHER_ID, text, ""));
    commit_data(&original, "future_turn_cancelled", json_pack("{s:s,s:[s]}",
        "reason", "user", "queue_ids", cancelled_id));
    commit_data(&original, "banner_updated", json_pack("{s:s}", "text", "queue provenance"));
    sequences.text = sequences.creation;
    if (edited) {
        sequences.text = commit_data(&original, "future_turn_edited",
            json_pack("{s:s,s:s,s:b,s:b,s:[{s:s,s:s}]}", "queue_id", GOAL_ID,
                "text", text, "read_only", 0, "armed", 1, "content", "type", "input_text",
                "text", "ignored edited content"));
        assert(original.pending_queue[0].seq == sequences.creation);
        assert(!strcmp(snag_json_string(json_array_get(original.pending_queue[0].content, 0u),
            "text"), "original content"));
    }
    sequences.turn = commit_data(&original, "turn_started", json_pack(
        "{s:s,s:i,s:s,s:s,s:I,s:s,s:s,s:b,s:[],s:[{s:s,s:s}],s:{s:s,s:s,s:s,s:i,s:b}}",
        "turn_id", turn_id, "turn_number", 1, "input_kind", "queued", "queue_id", GOAL_ID,
        "queue_seq", (json_int_t)sequences.creation, "cwd", cwd, "text", text, "read_only", 0,
        "instructions", "content", "type", "input_text", "text", "original content",
        "config", "provider", "default", "model", "fixture", "effort", "default",
        "max_parallel_commands", 4, "parallel_tool_calls", 1));
    sequences.future = commit_data(&original, "future_turn_edited", json_pack(
        "{s:s,s:s,s:b,s:b}", "queue_id", OTHER_ID, "text", text, "read_only", 0, "armed", 0));
    commit_data(&original, "future_turn_cancelled", json_pack("{s:s,s:[s]}",
        "reason", "user", "queue_ids", OTHER_ID));
    commit_data(&original, "future_turn_queued", queue_data(OTHER_ID, text, turn_id));

    struct replay_fixture fixture = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX},
        .batch_records = edited ? 3u : 1u};
    for (size_t i = 0u; i < 3u; ++i) fixture.payload[i].max = SNAG_MAX_EVENT_LINE;
    struct snag_legacy_recovery legacy;
    assert(!snag_store_reconcile_legacy(&original, &expected, fixture_event, &fixture,
        &legacy, error, sizeof(error)));
    fixture_flush(&fixture);
    struct import_counts counts = check_import(&original, &expected);
    assert(counts.input_fields[0] == 1u && counts.input_fields[1] == 1u &&
        !counts.input_fields[2] && !counts.plain_turns);
    assert(counts.last_input[0].target.sequence == sequences.text);
    assert(counts.last_input[1].target.sequence == sequences.creation);
    memcpy(source.id, original.id, sizeof(source.id));
    source.log_fd = temporary_fd();
    source.lock_fd = temporary_fd();
    assert(!snag_lock_file(source.lock_fd, false));
    struct snag_binary_recovery recovery;
    assert(!replay_check(&source, &restored, &recovery, fixture.file.data, fixture.file.len));
    same_core_state(&expected, &restored);
    struct snag_buf file = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    receipt_references(&fixture, &sequences, &source, &restored, &expected, NULL, NULL);
    for (enum turn_ref_case mode = TURN_REF_BOTH; mode <= TURN_REF_READ_ONLY; ++mode) {
        if (!edited && (mode == TURN_REF_OLD_TEXT || mode == TURN_REF_EDIT_CONTENT)) continue;
        rewrite_turn(&fixture, &sequences, mode, &file);
        int rc = replay_check(&source, &restored, &recovery, file.data, file.len);
        if (mode <= TURN_REF_CONTENT) {
            assert(!rc && !recovery.problem_seq && !recovery.incomplete_tail_bytes);
            same_core_state(&expected, &restored);
            assert(restored.log_end == (int64_t)file.len);
            if (mode == TURN_REF_BOTH) assert(file.len < fixture.file.len);
        } else {
            assert(rc < 0 && errno == EINVAL && recovery.problem_seq == sequences.turn);
        }
    }
    snag_buf_free(&file);
    for (size_t i = 0u; i < 3u; ++i) snag_buf_free(&fixture.payload[i]);
    snag_buf_free(&fixture.file);
    json_decref(fixture.creation);
    snag_session_close(&restored);
    snag_session_close(&source);
    snag_session_close(&expected);
    snag_session_close(&original);
}

static json_t *
input_data(const char *text, bool timer, json_t *instructions)
{
    json_t *data = checked_json(json_pack(
        "{s:s,s:s,s:s,s:s,s:[],s:b,s:i,s:[{s:s,s:s}]}",
        "provider", "default", "model", "fixture", "effort", "default", "text", text,
        "instructions", "read_only", 0, "received_at_ms", 1, "content",
        "type", "input_text", "text", "original content"));
    if (timer) assert(!snag_json_set_new(data, "origin", json_string("timer")));
    assert(!json_object_set(data, "instructions", instructions));
    return data;
}

static json_t *
direct_turn_data(const char *cwd, const char *text, const char *id, int number, bool timer,
    json_t *instructions)
{
    json_t *data = checked_json(json_pack(
        "{s:s,s:i,s:s,s:n,s:n,s:s,s:s,s:b,s:[],s:[{s:s,s:s}],s:{s:s,s:s,s:s,s:i,s:b}}",
        "turn_id", id, "turn_number", number, "input_kind", timer ? "timer" : "direct",
        "queue_id", "queue_seq", "cwd", cwd, "text", text, "read_only", 0,
        "instructions", "content", "type", "input_text", "text", "original content",
        "config", "provider", "default", "model", "fixture", "effort", "default",
        "max_parallel_commands", 4, "parallel_tool_calls", 1));
    assert(!json_object_set(data, "instructions", instructions));
    return data;
}

static void
test_import_input_sources(struct snag_store *store, const char *cwd)
{
    char error[512], ids[20][SNAG_ID_HEX_LEN + 1u];
    char *text = malloc(65537u);
    assert(text);
    memset(text, 'p', 65536u);
    text[65536u] = '\0';
    json_t *paths = checked_json(json_array());
    struct snag_session original, expected;
    snag_session_init(&original);
    snag_session_init(&expected);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    for (unsigned int i = 0u; i < 20u; ++i) {
        assert(snprintf(ids[i], sizeof(ids[i]), "%032x", i + 1u) == SNAG_ID_HEX_LEN);
        json_t *data = queue_data(ids[i], text, "");
        char content[32];
        int length = snprintf(content, sizeof(content), "original content %u", i);
        assert(length > 0 && (size_t)length < sizeof(content));
        assert(!snag_json_set_new(json_array_get(json_object_get(data, "content"), 0u),
            "text", json_string(content)));
        commit_data(&original, "future_turn_queued", data);
    }
    for (size_t i = 1u; i <= 10u; i += 9u) {
        commit_data(&original, "future_turn_edited",
            json_pack("{s:s,s:s,s:b,s:b,s:[{s:s,s:s}]}", "queue_id", ids[i],
                "text", i == 1u ? text : "new edited text", "read_only", 1, "armed", 1,
                "content", "type", "input_text", "text", "ignored edited content"));
    }
    commit_data(&original, "future_turn_cancelled", json_pack("{s:s,s:[s,s,s]}",
        "reason", "user", "queue_ids", ids[0], ids[7], ids[19]));
    uint64_t reused = commit_data(&original, "future_turn_queued", queue_data(ids[0], text, ""));
    commit_data(&original, "input_received", input_data(text, false, paths));
    commit_data(&original, "input_cancelled", json_object());
    commit_data(&original, "turn_started", direct_turn_data(cwd, text, GOAL_ID, 1, false, paths));
    commit_data(&original, "turn_interrupted", json_pack("{s:s,s:s,s:s}",
        "turn_id", GOAL_ID, "origin", "user", "reason", "cancelled"));
    commit_data(&original, "input_received", input_data(text, false, paths));
    assert(!snag_session_checkpoint(&original, error, sizeof(error)));

    /* The pending receipt and all surviving queue entries are still unconsumed. */
    struct snag_legacy_recovery recovery;
    assert(!snag_store_reconcile_legacy(&original, &expected, NULL, NULL,
        &recovery, error, sizeof(error)));
    struct import_counts counts = check_import(&original, &expected);
    assert(counts.plain_turns == 1u && !counts.input_fields[0] &&
        !counts.input_fields[1] && !counts.input_fields[2]);
    snag_session_close(&expected);
    snag_session_init(&expected);

    commit_data(&original, "turn_started", direct_turn_data(cwd, text, GOAL_ID, 2, false, paths));
    commit_data(&original, "turn_interrupted", json_pack("{s:s,s:s,s:s}",
        "turn_id", GOAL_ID, "origin", "user", "reason", "cancelled"));
    while (original.pending_queue_count) {
        const struct snag_queued_turn *queued = &original.pending_queue[0];
        json_t *turn = direct_turn_data(cwd, queued->text, OTHER_ID,
            (int)original.turn_count + 1, false, paths);
        assert(!snag_json_set_new(turn, "input_kind", json_string("queued")));
        assert(!snag_json_set_new(turn, "queue_id", json_string(queued->queue_id)));
        assert(!snag_json_set_new(turn, "queue_seq", json_integer((json_int_t)queued->seq)));
        assert(!snag_json_set_new(turn, "read_only", json_boolean(queued->read_only)));
        assert(!json_object_set(turn, "content", queued->content));
        commit_data(&original, "turn_started", turn);
        commit_data(&original, "turn_interrupted", json_pack("{s:s,s:s,s:s}",
            "turn_id", OTHER_ID, "origin", "user", "reason", "cancelled"));
    }
    commit_data(&original, "future_turn_queued", queue_data(ids[0], text, ""));
    commit_data(&original, "future_turn_edited", json_pack("{s:s,s:s,s:b,s:b}",
        "queue_id", ids[0], "text", "unfinished queue", "read_only", 0, "armed", 0));
    commit_data(&original, "input_received", input_data(text, false, paths));
    assert(!snag_store_reconcile_legacy(&original, &expected, NULL, NULL,
        &recovery, error, sizeof(error)));
    counts = check_import(&original, &expected);
    assert(counts.input_fields[0] == 19u && counts.input_fields[1] == 19u &&
        counts.input_fields[2] == 1u && counts.plain_turns == 1u && counts.input_cross_batch);
    assert(counts.last_input[0].target.sequence == reused &&
        counts.last_input[1].target.sequence == reused && !counts.last_input[2].field);
    assert(expected.pending_input && expected.pending_queue_count == 1u);
    snag_session_close(&expected);
    snag_session_close(&original);
    json_decref(paths);
    free(text);
}

static void
test_direct_references(struct snag_store *store, const char *cwd, bool timer, bool irc,
    unsigned int paths)
{
    const char *text = "Identical input text is retained across cancellation and new receipts; "
        "only the currently pending receipt supplies the turn's canonical fields.";
    char error[512];
    struct snag_session original, expected, source, restored;
    snag_session_init(&original);
    snag_session_init(&expected);
    snag_session_init(&source);
    snag_session_init(&restored);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    struct turn_sources sequences;
    json_t *receipt_paths = checked_json(json_array());
    if (paths) {
        char *path = snag_path_join(cwd, "receipt instructions.md");
        assert(path && !json_array_append_new(receipt_paths, json_string(path)));
        free(path);
        path = snag_path_join(cwd, "second instruction.md");
        assert(path && !json_array_append_new(receipt_paths, json_string(path)));
        free(path);
    }
    json_t *turn_paths = checked_json(json_deep_copy(receipt_paths));
    if (paths == 2u) {
        char *path = snag_path_join(cwd, "turn-discovered instructions.md");
        assert(path && !json_array_set_new(turn_paths, 0u, json_string(path)));
        free(path);
    }
    json_t *canonical = input_data(text, !timer, receipt_paths);
    assert(!snag_json_set_new(canonical, "provider", json_string("source-provider")));
    assert(!snag_json_set_new(canonical, "model", json_string("source-model")));
    assert(!snag_json_set_new(canonical, "effort", json_string("source-effort")));
    assert(!snag_json_set_new(canonical, "read_only", json_true()));
    sequences.other = commit_data(&original, "input_received", canonical);
    sequences.canonical = sequences.other;
    commit_data(&original, "input_cancelled", json_object());
    sequences.alternate = commit_data(&original, "input_received",
        input_data(text, timer, receipt_paths));
    commit_data(&original, "input_cancelled", json_object());
    struct snag_irc_event observed = {.kind = SNAG_IRC_MESSAGE, .timestamp_ms = 1u,
        .stream = GOAL_ID, .sequence = 1u, .input = true, .endpoint = "fixture",
        .room = "#fixture", .nick = "operator", .op = true};
    assert(snag_strcpy(observed.text, sizeof(observed.text), text));
    uint64_t observation = commit_data(&original, "irc_event", snag_irc_event_data(&observed));
    json_t *input = input_data(text, timer, receipt_paths);
    sequences.creation = commit_data(&original, irc ? "irc_admitted" : "input_received",
        irc ? json_pack("{s:[I],s:o}", "sequences", (json_int_t)observation,
            "input", input) : input);
    sequences.text = sequences.creation;
    json_t *pending = checked_json(json_deep_copy(original.pending_input));
    /* A watermark-only admission must leave the pending receipt identity alone. */
    commit_data(&original, "irc_admitted", json_pack("{s:[I]}",
        "sequences", (json_int_t)observation));
    sequences.turn = commit_data(&original, "turn_started",
        direct_turn_data(cwd, text, GOAL_ID, 1, timer, turn_paths));
    assert(json_equal(original.active_instructions, turn_paths));
    commit_data(&original, "turn_interrupted", json_pack("{s:s,s:s,s:s}",
        "turn_id", GOAL_ID, "origin", "user", "reason", "cancelled"));
    sequences.future = commit_data(&original, "input_received",
        input_data(text, timer, receipt_paths));
    commit_data(&original, "input_cancelled", json_object());
    /* Literal direct turns without receipts remain in the existing reducer domain. */
    uint64_t last_turn = commit_data(&original, "turn_started",
        direct_turn_data(cwd, text, OTHER_ID, 2, false, turn_paths));

    struct replay_fixture fixture = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX},
        .batch_records = irc ? 3u : 1u};
    for (size_t i = 0u; i < 3u; ++i) fixture.payload[i].max = SNAG_MAX_EVENT_LINE;
    struct snag_legacy_recovery legacy;
    assert(!snag_store_reconcile_legacy(&original, &expected, fixture_event, &fixture,
        &legacy, error, sizeof(error)));
    fixture_flush(&fixture);
    struct import_counts counts = check_import(&original, &expected);
    assert(counts.input_fields[0] == 1u && counts.input_fields[1] == 1u &&
        counts.input_fields[2] == (paths == 2u ? 0u : 1u) && counts.plain_turns == 1u);
    assert(counts.last_input[0].target.sequence == sequences.creation);
    assert(counts.last_input[1].target.sequence == sequences.creation);
    assert(counts.last_input[2].target.sequence == (paths == 2u ? 0u : sequences.creation));
    memcpy(source.id, original.id, sizeof(source.id));
    source.log_fd = temporary_fd();
    source.lock_fd = temporary_fd();
    assert(!snag_lock_file(source.lock_fd, false));
    struct snag_binary_recovery recovery;
    assert(!replay_check(&source, &restored, &recovery, fixture.file.data, fixture.file.len));
    same_core_state(&expected, &restored);
    const enum turn_ref_case modes[] = {
        TURN_REF_BOTH, TURN_REF_TEXT, TURN_REF_CONTENT, TURN_REF_OTHER_TEXT,
        TURN_REF_FUTURE, TURN_REF_SELF, TURN_REF_OTHER_CONTENT, TURN_REF_OFFSET,
        TURN_REF_SIZE, TURN_REF_ROLE, TURN_REF_INSTRUCTIONS, TURN_REF_READ_ONLY,
        TURN_REF_ORIGIN, TURN_REF_MODEL, TURN_REF_GOAL, TURN_REF_INNER_OFFSET,
        TURN_REF_ALL, TURN_REF_PATHS, TURN_REF_OTHER_PATHS, TURN_REF_FUTURE_PATHS,
        TURN_REF_PATH_OFFSET, TURN_REF_PATH_SIZE, TURN_REF_PATH_LEAF, TURN_REF_INNER_PATH
    };
    struct snag_buf file = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    struct snag_buf prefix = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    for (size_t i = 0u; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        enum turn_ref_case mode = modes[i];
        if ((mode == TURN_REF_INNER_OFFSET || mode == TURN_REF_INNER_PATH) && !irc) continue;
        bool paths_reference = mode == TURN_REF_ALL || mode == TURN_REF_PATHS;
        /* Discovered turn paths can differ from the receipt. Retain their literal
         * value instead of replacing it with a non-equivalent reference. */
        if (paths == 2u && paths_reference) continue;
        rewrite_turn(&fixture, &sequences, mode, &file);
        int rc = replay_check(&source, &restored, &recovery, file.data, file.len);
        if (mode <= TURN_REF_CONTENT || paths_reference) {
            assert(!rc && !recovery.problem_seq && !recovery.incomplete_tail_bytes);
            same_core_state(&expected, &restored);
            assert(restored.log_end == (int64_t)file.len);
            if (mode == TURN_REF_BOTH) assert(file.len < fixture.file.len);
            if (mode == TURN_REF_BOTH && !irc) {
                bounded_reference_suffix(&source, &restored, &recovery.verified, sequences.turn);
            }
            /* Inspect the referenced turn before later turns replace its state. */
            prefix_through(&file, sequences.turn, &prefix);
            assert(!replay_check(&source, &restored, &recovery, prefix.data, prefix.len));
            assert(restored.active_turn && restored.turn_count == 1u);
            assert(json_equal(restored.active_instructions, turn_paths));
        } else {
            assert(rc < 0 && errno == EINVAL && recovery.problem_seq == sequences.turn);
        }
    }
    receipt_references(&fixture, &sequences, &source, &restored, &expected, pending, turn_paths);
    json_decref(pending);
    sequences.turn = last_turn;
    sequences.creation = sequences.text = sequences.future;
    const enum turn_ref_case missing[] = {TURN_REF_BOTH, TURN_REF_ALL, TURN_REF_PATHS};
    for (size_t i = 0u; i < sizeof(missing) / sizeof(missing[0]); ++i) {
        rewrite_turn(&fixture, &sequences, missing[i], &file);
        assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0);
        assert(errno == EINVAL && recovery.problem_seq == last_turn);
    }
    json_decref(receipt_paths);
    json_decref(turn_paths);
    snag_buf_free(&prefix);
    snag_buf_free(&file);
    for (size_t i = 0u; i < 3u; ++i) snag_buf_free(&fixture.payload[i]);
    snag_buf_free(&fixture.file);
    json_decref(fixture.creation);
    snag_session_close(&restored);
    snag_session_close(&source);
    snag_session_close(&expected);
    snag_session_close(&original);
}

static json_t *
goal_turn_data(const char *cwd, const char *id, int number, json_t *paths, bool goal)
{
    json_t *data = direct_turn_data(cwd, SNAG_GOAL_CONTINUATION_TEXT, id, number, false, paths);
    assert(!json_object_del(data, "content"));
    if (goal) assert(!snag_json_set_new(data, "input_kind", json_string("goal")));
    return data;
}

static void
test_goal_references(struct snag_store *store, const char *cwd, unsigned int variant, bool nonempty)
{
    const char *replacement = "cccccccccccccccccccccccccccccccc";
    const char *later = "dddddddddddddddddddddddddddddddd";
    char error[512];
    struct snag_session original, expected, source, restored;
    snag_session_init(&original);
    snag_session_init(&expected);
    snag_session_init(&source);
    snag_session_init(&restored);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    json_t *paths = checked_json(json_array());
    if (nonempty) {
        char *path = snag_path_join(cwd, "goal instruction.md");
        assert(path && !json_array_append_new(paths, json_string(path)));
        free(path);
    }
    json_t *input = input_data(SNAG_GOAL_CONTINUATION_TEXT, false, paths);
    assert(!snag_json_set_new(input, "read_only", json_true()));
    uint64_t canonical = commit_data(&original, "input_received", input);
    commit_data(&original, "input_cancelled", json_object());
    uint64_t wrong = commit_data(&original, "input_received",
        input_data("Different text is not a goal continuation.", false, paths));
    commit_data(&original, "input_cancelled", json_object());
    if (variant != 5u) commit_data(&original, "goal_started", json_pack("{s:s,s:s}",
        "goal_id", OTHER_ID, "prompt", "Original goal wording."));
    uint64_t prior_turn = commit_data(&original, "turn_started",
        goal_turn_data(cwd, GOAL_ID, 1, paths, variant != 5u));
    commit_data(&original, "turn_interrupted", json_pack("{s:s,s:s,s:s}",
        "turn_id", GOAL_ID, "origin", "user", "reason", "cancelled"));
    if (variant != 5u) commit_data(&original, "goal_replaced", json_pack("{s:s,s:s,s:s,s:s}",
        "actor", "user", "goal_id", OTHER_ID, "new_goal_id", replacement,
        "prompt", "Replacement goal wording."));
    if (variant == 0u) commit_data(&original, "goal_lock_changed", json_pack("{s:s,s:b}",
        "goal_id", replacement, "locked", 1));
    if (variant == 1u) commit_data(&original, "goal_paused", json_pack("{s:s,s:s}",
        "goal_id", replacement, "reason", "user"));
    if (variant == 2u) commit_data(&original, "goal_blocked", json_pack("{s:s,s:s,s:s}",
        "goal_id", replacement, "actor", "model", "reason", "Fixture dependency."));
    if (variant == 3u) commit_data(&original, "goal_completed", json_pack("{s:s,s:s}",
        "goal_id", replacement, "actor", "user"));
    if (variant == 4u) commit_data(&original, "goal_cancelled", json_pack("{s:s}",
        "goal_id", replacement));
    if (variant == 6u) {
        input = input_data(SNAG_GOAL_CONTINUATION_TEXT, false, paths);
        assert(!json_object_del(input, "content"));
        commit_data(&original, "input_received", input);
    }
    uint64_t turn = commit_data(&original, "turn_started",
        goal_turn_data(cwd, later, 2, paths, variant == 0u));
    commit_data(&original, "turn_interrupted", json_pack("{s:s,s:s,s:s}",
        "turn_id", later, "origin", "user", "reason", "cancelled"));
    uint64_t future = commit_data(&original, "input_received",
        input_data(SNAG_GOAL_CONTINUATION_TEXT, false, paths));
    struct replay_fixture fixture = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX},
        .batch_records = 3u};
    for (size_t i = 0u; i < 3u; ++i) fixture.payload[i].max = SNAG_MAX_EVENT_LINE;
    struct snag_legacy_recovery legacy;
    assert(!snag_store_reconcile_legacy(&original, &expected, fixture_event, &fixture,
        &legacy, error, sizeof(error)));
    fixture_flush(&fixture);
    check_import(&original, &expected);
    memcpy(source.id, original.id, sizeof(source.id));
    source.log_fd = temporary_fd();
    source.lock_fd = temporary_fd();
    assert(!snag_lock_file(source.lock_fd, false));
    struct replay_fixture referenced = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX}};
    struct snag_buf file = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    struct snag_binary_recovery recovery;
    assert(!replay_check(&source, &restored, &recovery, fixture.file.data, fixture.file.len));
    same_core_state(&expected, &restored);
    for (unsigned int origin = 0u; origin < 2u; ++origin) {
        for (unsigned int fields = 1u; fields <= 5u; ++fields) {
            if (fields & 2u) continue;
            rewrite_receipt(&fixture, turn, origin ? prior_turn : canonical, fields,
                RECEIPT_GOAL, &referenced.file);
            int rc = replay_check(&source, &restored, &recovery,
                referenced.file.data, referenced.file.len);
            if (variant) {
                assert(rc < 0 && errno == EINVAL && recovery.problem_seq == turn);
                continue;
            }
            assert(!rc);
            same_core_state(&expected, &restored);
            prefix_through(&referenced.file, turn, &file);
            assert(!replay_check(&source, &restored, &recovery, file.data, file.len));
            assert(restored.active_goal && restored.goal_status == SNAG_GOAL_ACTIVE);
            assert(!strcmp(restored.goal_id, replacement) && restored.goal_locked);
            assert(!strcmp(restored.goal_prompt, "Replacement goal wording."));
            assert(restored.goal_turn_count == 1u &&
                json_equal(restored.active_instructions, paths));
            rewrite_receipt(&referenced, future, turn, fields, RECEIPT_VALID, &file);
            assert(!replay_check(&source, &restored, &recovery, file.data, file.len));
            same_core_state(&expected, &restored);
            rewrite_receipt(&referenced, future, turn, fields, RECEIPT_CHAIN, &file);
            assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0);
            assert(errno == EINVAL && recovery.problem_seq == future);
        }
    }
    if (!variant) {
        const enum receipt_fault faults[] = {RECEIPT_ROLE, RECEIPT_OFFSET, RECEIPT_SIZE,
            RECEIPT_INSTRUCTION_LEAF, RECEIPT_INSTRUCTION_SIZE, RECEIPT_READ_ONLY};
        for (size_t i = 0u; i < sizeof(faults) / sizeof(faults[0]); ++i) {
            rewrite_receipt(&fixture, turn, canonical, 5u, faults[i], &file);
            assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0);
            assert(errno == EINVAL && recovery.problem_seq == turn);
        }
        const uint64_t invalid[] = {wrong, turn, future};
        for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            rewrite_receipt(&fixture, turn, invalid[i], 5u, RECEIPT_VALID, &file);
            assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0);
            assert(errno == EINVAL && recovery.problem_seq == turn);
        }
        for (unsigned int fields = 2u; fields <= 7u; ++fields) {
            if (!(fields & 2u)) continue;
            rewrite_receipt(&fixture, turn, canonical, fields, RECEIPT_VALID, &file);
            assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0);
            assert(errno == EINVAL && recovery.problem_seq == turn);
        }
    }
    snag_buf_free(&file);
    snag_buf_free(&referenced.file);
    for (size_t i = 0u; i < 3u; ++i) snag_buf_free(&fixture.payload[i]);
    snag_buf_free(&fixture.file);
    json_decref(fixture.creation);
    json_decref(paths);
    snag_session_close(&restored);
    snag_session_close(&source);
    snag_session_close(&expected);
    snag_session_close(&original);
}

static json_t *
steering_data(const char *id, const char *text, unsigned int received)
{
    return checked_json(json_pack("{s:s,s:s,s:s,s:i,s:[{s:s,s:s}]}",
        "steering_id", id, "turn_id", GOAL_ID, "text", text, "received_at_ms", received,
        "content", "type", "input_text", "text", "original content"));
}

static json_t *
response_data(void)
{
    return checked_json(json_pack(
        "{s:i,s:n,s:s,s:n,s:s,s:s,s:s,s:i,s:s,s:n,s:i,s:s,s:i,s:s,s:i,s:i,s:s,"
        "s:s,s:s,s:s,s:s,s:n,s:s,s:b,s:[],s:s}",
        "irc_seq", 0, "baseline_sha256", "capability_version", SNAJPAGENT_CAPABILITY_VERSION,
        "compact_id", "count_method", "exact", "capacity_source", "unknown",
        "count_request_sha256", ZERO_HASH, "cycle", 1, "effort", "default", "hard_input_tokens",
        "input_tokens_bound", 1000, "model", "fixture", "model_input_bytes", 4000,
        "model_input_sha256", ZERO_HASH, "request_input_bytes", 3000, "request_input_count", 1,
        "request_input_sha256", ZERO_HASH, "profile_id", SNAJPAGENT_PROFILE_ID,
        "provider", "default", "provider_source_sha256", ZERO_HASH,
        "request_sha256", ZERO_HASH, "requested_output_tokens", "response_id", OTHER_ID,
        "source_bound", 0, "steering_ids", "turn_id", GOAL_ID));
}

static void
test_steering_references(struct snag_store *store, const char *cwd, unsigned int variant)
{
    bool irc = variant == 1u, reminder = variant == 2u, correction = variant == 3u;
    const char *id = reminder ? OTHER_ID : "cccccccccccccccccccccccccccccccc";
    const char *later_id = "dddddddddddddddddddddddddddddddd";
    const char *text = correction ? SNAG_EMPTY_OUTPUT_CORRECTION : SNAG_IRC_REPLY_REMINDER_TEXT;
    char error[512];
    struct snag_session original, expected, source, restored;
    snag_session_init(&original);
    snag_session_init(&expected);
    snag_session_init(&source);
    snag_session_init(&restored);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    json_t *paths = checked_json(json_array());
    json_t *input = input_data(text, false, paths);
    assert(!snag_json_set_new(input, "read_only", json_true()));
    uint64_t canonical = commit_data(&original, "input_received", input);
    commit_data(&original, "input_cancelled", json_object());
    uint64_t oversized = 0u;
    if (variant == 0u) {
        char *large = malloc(SNAG_MAX_STEERING_TEXT + 2u);
        assert(large);
        memset(large, 'x', SNAG_MAX_STEERING_TEXT + 1u);
        large[SNAG_MAX_STEERING_TEXT + 1u] = '\0';
        oversized = commit_data(&original, "input_received", input_data(large, false, paths));
        free(large);
        commit_data(&original, "input_cancelled", json_object());
    }
    uint64_t turn = commit_data(&original, "turn_started",
        direct_turn_data(cwd, "Active turn with its own input.", GOAL_ID, 1, false, paths));
    json_decref(paths);
    uint64_t alternative = 0u;
    if (reminder || correction) {
        commit_data(&original, "response_started", response_data());
        if (correction) {
            alternative = commit_data(&original, "response_output_correction", json_pack(
                "{s:s,s:s,s:i,s:s,s:s,s:[]}", "turn_id", GOAL_ID, "response_id", OTHER_ID,
                "cycle", 1, "correction_id", OTHER_ID, "text", text, "partial_public"));
        } else {
            commit_data(&original, "response_completed", json_pack(
                "{s:s,s:s,s:i,s:s,s:s,s:[],s:{s:i,s:i,s:n,s:i}}", "turn_id", GOAL_ID,
                "response_id", OTHER_ID, "cycle", 1, "status", "completed",
                "provider_response_id", "fixture", "items", "usage", "input_tokens", 0,
                "output_tokens", 0, "reasoning_tokens", "total_tokens", 0));
        }
    } else {
        alternative = commit_data(&original, "steering_added", steering_data(OTHER_ID, text, 2u));
    }
    struct snag_irc_event observed = {.kind = SNAG_IRC_MESSAGE, .timestamp_ms = 3u,
        .stream = GOAL_ID, .sequence = 1u, .input = true, .endpoint = "fixture",
        .room = "#fixture", .nick = "operator", .op = true};
    assert(snag_strcpy(observed.text, sizeof(observed.text), text));
    uint64_t observation = commit_data(&original, "irc_event", snag_irc_event_data(&observed));
    json_t *steering = steering_data(id, text, 17u);
    uint64_t receipt = commit_data(&original, irc ? "irc_admitted" :
        reminder ? "irc_reply_reminder" : "steering_added", irc ?
        json_pack("{s:[I],s:o}", "sequences", (json_int_t)observation, "steering", steering) :
        steering);
    uint64_t future = commit_data(&original, "steering_added", steering_data(later_id, text, 23u));
    struct replay_fixture fixture = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX},
        .batch_records = 3u};
    for (size_t i = 0u; i < 3u; ++i) fixture.payload[i].max = SNAG_MAX_EVENT_LINE;
    struct snag_legacy_recovery legacy;
    assert(!snag_store_reconcile_legacy(&original, &expected, fixture_event, &fixture,
        &legacy, error, sizeof(error)));
    fixture_flush(&fixture);
    check_import(&original, &expected);
    memcpy(source.id, original.id, sizeof(source.id));
    source.log_fd = temporary_fd();
    source.lock_fd = temporary_fd();
    assert(!snag_lock_file(source.lock_fd, false));
    struct replay_fixture referenced = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX}};
    struct snag_buf file = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    struct snag_binary_recovery recovery;
    assert(!replay_check(&source, &restored, &recovery, fixture.file.data, fixture.file.len));
    same_core_state(&expected, &restored);
    for (unsigned int fields = 1u; fields <= 3u; ++fields) {
        rewrite_receipt(&fixture, receipt, canonical, fields, RECEIPT_VALID, &referenced.file);
        assert(!replay_check(&source, &restored, &recovery,
            referenced.file.data, referenced.file.len));
        same_core_state(&expected, &restored);
        size_t target = restored.pending_steering_count - 2u;
        assert(restored.pending_steering[target].seq == receipt);
        assert(restored.pending_steering[target].received_ms == 17u);
        assert(!strcmp(restored.pending_steering[target].steering_id, id));
        assert(restored.irc_reply_reminded == reminder && !restored.active_read_only);
        rewrite_receipt(&referenced, future, receipt, fields, RECEIPT_VALID, &file);
        assert(!replay_check(&source, &restored, &recovery, file.data, file.len));
        same_core_state(&expected, &restored);
        if (fields & 1u) {
            rewrite_receipt(&referenced, future, receipt, fields, RECEIPT_CHAIN, &file);
            assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0);
            assert(errno == EINVAL && recovery.problem_seq == future);
        }
        /* Literal steering/reminder originals include outer-relative IRC leaves. */
        rewrite_receipt(&fixture, future, receipt, fields, RECEIPT_VALID, &file);
        assert(!replay_check(&source, &restored, &recovery, file.data, file.len));
        same_core_state(&expected, &restored);
    }
    if (alternative) {
        rewrite_receipt(&fixture, receipt, alternative, 1u, RECEIPT_VALID, &file);
        assert(!replay_check(&source, &restored, &recovery, file.data, file.len));
        same_core_state(&expected, &restored);
    }
    const enum receipt_fault faults[] = {RECEIPT_ROLE, RECEIPT_OFFSET, RECEIPT_SIZE,
        RECEIPT_CONTENT_LEAF, RECEIPT_CONTENT_SIZE, RECEIPT_TURN, RECEIPT_ID, RECEIPT_REMINDER};
    for (size_t i = 0u; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        uint64_t bad = faults[i] == RECEIPT_REMINDER || (reminder && faults[i] == RECEIPT_ID) ?
            future : receipt;
        rewrite_receipt(&fixture, bad, canonical, 3u, faults[i], &file);
        assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0);
        assert(errno == EINVAL && recovery.problem_seq == bad);
    }
    for (unsigned int i = 0u; i < 2u; ++i) {
        rewrite_receipt(&fixture, receipt, i ? future : receipt, 3u, RECEIPT_VALID, &file);
        assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0);
        assert(errno == EINVAL && recovery.problem_seq == receipt);
    }
    if (reminder) {
        /* A valid original text field is still not the required reminder prompt. */
        rewrite_receipt(&fixture, receipt, turn, 1u, RECEIPT_VALID, &file);
        assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0);
        assert(errno == EINVAL && recovery.problem_seq == receipt);
    }
    if (oversized) {
        /* A destination-sized prefix cannot borrow an oversized original. */
        rewrite_receipt(&fixture, receipt, oversized, 1u, RECEIPT_SIZE, &file);
        assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0);
        assert(errno == EINVAL && recovery.problem_seq == receipt);
    }
    snag_buf_free(&file);
    snag_buf_free(&referenced.file);
    for (size_t i = 0u; i < 3u; ++i) snag_buf_free(&fixture.payload[i]);
    snag_buf_free(&fixture.file);
    json_decref(fixture.creation);
    snag_session_close(&restored);
    snag_session_close(&source);
    snag_session_close(&expected);
    snag_session_close(&original);
}

struct replay_observer {
    const json_t *events;
    uint64_t reject;
    size_t seen;
};

static int
observe_event(void *opaque, const struct snag_session *state, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t error_size)
{
    struct replay_observer *observer = opaque;
    const json_t *event = json_array_get(observer->events, (size_t)(seq - 1u));
    assert(event && state && state->next_seq == seq + 1u);
    assert(strcmp(type, "session_checkpoint"));
    assert(!strcmp(type, snag_json_string(event, "type")));
    assert(state->last_time_ms == (uint64_t)json_integer_value(json_object_get(event, "time")));
    if (!json_equal(data, json_object_get(event, "data"))) {
        fprintf(stderr, "resolved native event %llu (%s) differs from legacy data\n",
            (unsigned long long)seq, type);
        assert(false);
    }
    if (!strcmp(type, "future_turn_queued")) {
        assert(state->pending_queue_count);
        const struct snag_queued_turn *queue =
            &state->pending_queue[state->pending_queue_count - 1u];
        assert(queue->seq == seq && !strcmp(queue->queue_id, snag_json_string(data, "queue_id")));
        assert(!strcmp(queue->text, snag_json_string(data, "text")));
    }
    ++observer->seen;
    if (seq == observer->reject) {
        snprintf(error, error_size, "fixture callback refused sequence %llu",
            (unsigned long long)seq);
        errno = ECANCELED;
        return -1;
    }
    return 0;
}

static void
observed_replay(struct snag_session *source, struct snag_session *restored,
    const struct snag_session *expected, const json_t *events, const struct snag_buf *file,
    uint64_t failure, bool reject)
{
    struct replay_observer observer = {.events = events, .reject = reject ? failure : 0u};
    struct snag_binary_recovery recovery;
    int rc = replay_visit(source, restored, &recovery, file->data, file->len,
        observe_event, &observer);
    if (failure) {
        assert(rc < 0 && errno == (reject ? ECANCELED : EINVAL));
        assert(recovery.problem_seq == failure);
    } else {
        if (rc) fprintf(stderr, "observed replay failed at %llu: errno %d\n",
            (unsigned long long)recovery.problem_seq, errno);
        assert(!rc);
        if (expected) same_core_state(expected, restored);
    }
    uint64_t through = failure ? failure - (reject ? 0u : 1u) : restored->next_seq - 1u;
    size_t count = 0u;
    for (size_t i = 0u; i < json_array_size(events) && i < through; ++i) {
        if (strcmp(snag_json_string(json_array_get(events, i), "type"), "session_checkpoint"))
            ++count;
    }
    assert(observer.seen == count);
}

struct snapshot_sources {
    uint64_t first[3][2], last[3][2], gap[3][2], target;
    size_t bytes[2];
};

enum snapshot_fault {
    SNAPSHOT_VALID, SNAPSHOT_OLD, SNAPSHOT_FUTURE, SNAPSHOT_SELF,
    SNAPSHOT_OFFSET, SNAPSHOT_SIZE, SNAPSHOT_BYTES, SNAPSHOT_LAST, SNAPSHOT_RECORD,
    SNAPSHOT_ID, SNAPSHOT_PROVIDER, SNAPSHOT_KIND, SNAPSHOT_PHASE,
    SNAPSHOT_TURN, SNAPSHOT_RESPONSE, SNAPSHOT_CYCLE, SNAPSHOT_DUPLICATE, SNAPSHOT_FANOUT
};

struct snapshot_fields {
    struct snag_binary_public_items *items;
    unsigned char *turn, *response;
    uint32_t *cycle;
    struct snag_binary_graph_items *graph;
};

static struct snapshot_fields
snapshot_fields(struct snag_binary_event *event)
{
    switch (event->kind) {
    case SNAG_BINARY_RESPONSE_INTERRUPTED:
        return (struct snapshot_fields){&event->data.response_interrupted.partial,
            event->data.response_interrupted.turn, event->data.response_interrupted.response,
            &event->data.response_interrupted.cycle, NULL};
    case SNAG_BINARY_RESPONSE_FAILED:
        return (struct snapshot_fields){&event->data.response_failed.partial,
            event->data.response_failed.turn, event->data.response_failed.response,
            &event->data.response_failed.cycle, NULL};
    case SNAG_BINARY_RESPONSE_COMPLETED:
        return (struct snapshot_fields){NULL, event->data.response_completed.turn,
            event->data.response_completed.response, &event->data.response_completed.cycle,
            &event->data.response_completed.items};
    default:
        assert(event->kind == SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION);
        return (struct snapshot_fields){&event->data.response_correction.partial,
            event->data.response_correction.turn, event->data.response_correction.response,
            &event->data.response_correction.cycle, NULL};
    }
}

static struct snag_binary_response_output
fixture_output(const struct replay_fixture *fixture, uint64_t first,
    struct snag_binary_ref *reference, struct snag_buf *decoded)
{
    struct snag_binary_identity identity;
    struct snag_binary_anchor position, next;
    assert(!snag_binary_header_decode(fixture->file.data, SNAG_BINARY_HEADER_SIZE,
        &identity, &position));
    while (position.end < fixture->file.len) {
        struct snag_binary_batch batch;
        assert(!binary_fixture_read(fixture->file.data + position.end,
            fixture->file.len - position.end, &position, decoded, &batch, &next));
        size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
        struct snag_binary_record record;
        uint64_t sequence;
        while (snag_binary_record_next(&batch, &cursor, &record, &sequence) == 0) {
            if (sequence != first) continue;
            struct snag_binary_event event;
            assert(!snag_binary_event_decode(&record, &event));
            assert(event.kind == SNAG_BINARY_RESPONSE_OUTPUT);
            const struct snag_binary_text *text = &event.data.response_output.item.text;
            *reference = (struct snag_binary_ref){.sequence = first,
                .offset = text->data - record.payload, .size = text->size};
            return event.data.response_output;
        }
        position = next;
    }
    assert(false);
    return (struct snag_binary_response_output){0};
}

static struct snag_binary_output_span
fixture_span(const struct replay_fixture *fixture, uint64_t first, uint64_t last, size_t bytes)
{
    struct snag_binary_ref reference;
    struct snag_buf decoded = {.max = SNAG_BINARY_BATCH_MAX};
    (void)fixture_output(fixture, first, &reference, &decoded);
    snag_buf_free(&decoded);
    return (struct snag_binary_output_span){.first = reference,
        .last_sequence = bytes == reference.size ? first : last, .bytes = bytes};
}

static void
snapshot_public(const struct snag_binary_graph_items *graph, struct snag_buf *encoded,
    struct snag_binary_public_items *public)
{
    struct snag_binary_public_value *values = calloc(graph->count, sizeof(*values));
    assert(values);
    struct snag_binary_graph_item value;
    size_t offset = 0u, count = 0u;
    int rc;
    while ((rc = snag_binary_graph_items_next(graph, &offset, &value)) == 0) {
        if (value.kind != SNAG_BINARY_ITEM_TOOL_CALL) {
            values[count++] = value.data.output;
        }
    }
    assert(rc == 1 && !snag_binary_public_items_encode(encoded, values, count));
    free(values);
    assert(!snag_binary_public_items_decode(encoded->data, encoded->len, public));
}

static void
snapshot_graph(struct snag_binary_graph_items *graph,
    const struct snag_binary_public_value *public, size_t count, struct snag_buf *encoded)
{
    /* Keep calls in their original slots, including when public items move.
     * Fanout-negative fixtures append any extra repeated public values. */
    struct snag_binary_graph_item *values = calloc(graph->count + count, sizeof(*values));
    assert(values);
    size_t offset = 0u, used = 0u, item = 0u;
    struct snag_binary_graph_item value;
    int rc;
    while ((rc = snag_binary_graph_items_next(graph, &offset, &value)) == 0) {
        if (value.kind != SNAG_BINARY_ITEM_TOOL_CALL) {
            assert(item < count);
            value.kind = public[item].item.kind;
            value.data.output = public[item++];
        }
        values[used++] = value;
    }
    assert(rc == 1);
    while (item < count) {
        values[used++] = (struct snag_binary_graph_item){
            .kind = public[item].item.kind, .data.output = public[item]};
        ++item;
    }
    assert(!snag_binary_graph_items_encode(encoded, values, used));
    free(values);
    assert(!snag_binary_graph_items_decode(encoded->data, encoded->len, graph));
}

static void
rewrite_snapshot(const struct replay_fixture *fixture, const struct snapshot_sources *sources,
    unsigned int mask, enum snapshot_fault fault, bool single, struct snag_buf *file)
{
    struct snag_buf decoded = {.max = SNAG_BINARY_BATCH_MAX};
    size_t count = json_array_size(fixture->events), used = 0u;
    struct snag_binary_record *records = calloc(count, sizeof(*records));
    assert(records);
    unsigned char *retained = malloc(fixture->file.len);
    assert(retained);
    size_t retained_size = 0u;
    struct snag_binary_identity identity;
    struct snag_binary_anchor input, output, next;
    assert(!snag_binary_header_decode(fixture->file.data, SNAG_BINARY_HEADER_SIZE,
        &identity, &input));
    output = input;
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf public = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_binary_ref reference;
    unsigned char original_ids[2][16];
    for (size_t i = 0u; i < 2u; ++i) {
        struct snag_binary_response_output original =
            fixture_output(fixture, sources->first[1][i], &reference, &decoded);
        memcpy(original_ids[i], original.item.id, sizeof(original_ids[i]));
    }
    while (input.end < fixture->file.len) {
        struct snag_binary_batch batch;
        assert(!binary_fixture_read(fixture->file.data + input.end,
            fixture->file.len - input.end, &input, &decoded, &batch, &next));
        size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
        uint64_t sequence;
        struct snag_binary_record record;
        while (snag_binary_record_next(&batch, &cursor, &record, &sequence) == 0) {
            assert(used < count && sequence == used + 1u);
            assert(record.size <= fixture->file.len - retained_size);
            memcpy(retained + retained_size, record.payload, record.size);
            records[used] = record;
            records[used++].payload = retained + retained_size;
            retained_size += record.size;
            if (sequence != sources->target) continue;
            struct snag_binary_event event;
            assert(!snag_binary_event_decode(&record, &event));
            struct snapshot_fields fields = snapshot_fields(&event);
            struct snag_binary_public_items public_items;
            if (fields.graph) {
                snapshot_public(fields.graph, &public, &public_items);
                fields.items = &public_items;
            }
            size_t item_count = 0u, offset = 0u;
            struct snag_binary_public_value value;
            while (!snag_binary_public_items_next(fields.items, &offset, &value)) {
                ++item_count;
            }
            size_t fanout = SNAG_MAX_EVENT_LINE / SNAG_MAX_PUBLIC_ITEM + 1u;
            size_t capacity = item_count > fanout ? item_count : fanout;
            struct snag_binary_public_value *values = calloc(capacity, sizeof(*values));
            assert(values && item_count >= 2u);
            offset = 0u;
            unsigned int generation = fault == SNAPSHOT_OLD ? 0u :
                fault == SNAPSHOT_FUTURE ? 2u : 1u;
            for (size_t i = 0u; i < item_count; ++i) {
                assert(!snag_binary_public_items_next(fields.items, &offset, &values[i]));
                if (i >= 2u || !(mask & (1u << i))) continue;
                size_t source_index = memcmp(values[i].item.id, original_ids[0], 16u) != 0;
                assert(!memcmp(values[i].item.id, original_ids[source_index], 16u));
                values[i].source = fixture_span(fixture, sources->first[generation][source_index],
                    sources->last[generation][source_index], values[i].item.text.size);
                values[i].item.text = (struct snag_binary_text){0};
            }
            size_t index = mask & 1u ? 0u : 1u;
            if (fault == SNAPSHOT_PHASE && values[index].item.kind == SNAG_BINARY_ITEM_REFUSAL) {
                index = 1u - index;
            }
            struct snag_binary_public_value *selected = &values[index];
            size_t source_index = memcmp(selected->item.id, original_ids[0], 16u) != 0;
            struct snag_binary_output_span *span = &selected->source;
            if (fault == SNAPSHOT_SELF) {
                span->first.sequence = sources->target;
                span->last_sequence = sources->target + (span->bytes > span->first.size);
            }
            if (fault == SNAPSHOT_OFFSET) ++span->first.offset;
            if (fault == SNAPSHOT_SIZE) {
                --span->first.size;
                --span->bytes;
            }
            if (fault == SNAPSHOT_BYTES) {
                span->last_sequence = sources->last[1][source_index];
                span->bytes = sources->bytes[source_index] + 1u;
            }
            if (fault == SNAPSHOT_LAST) {
                span->last_sequence = sources->gap[1][source_index];
                if (span->bytes == span->first.size) ++span->bytes;
            }
            if (fault == SNAPSHOT_RECORD) {
                if (span->last_sequence == span->first.sequence) {
                    span->last_sequence = sources->gap[1][source_index];
                }
                span->first.sequence = sources->gap[1][source_index];
            }
            if (fault == SNAPSHOT_ID) selected->item.id[0] ^= 1u;
            if (fault == SNAPSHOT_PROVIDER) {
                selected->item.provider_id =
                    (struct snag_binary_text){(const unsigned char *)"foreign-provider", 16u};
            }
            if (fault == SNAPSHOT_KIND) {
                bool refusal = selected->item.kind == SNAG_BINARY_ITEM_REFUSAL;
                selected->item.kind = refusal ?
                    SNAG_BINARY_ITEM_ASSISTANT : SNAG_BINARY_ITEM_REFUSAL;
                selected->item.phase = refusal ?
                    SNAG_BINARY_PHASE_COMMENTARY : SNAG_BINARY_PHASE_FINAL_ANSWER;
            }
            if (fault == SNAPSHOT_PHASE) {
                selected->item.phase = selected->item.phase == SNAG_BINARY_PHASE_COMMENTARY ?
                    SNAG_BINARY_PHASE_FINAL_ANSWER : SNAG_BINARY_PHASE_COMMENTARY;
            }
            if (fault == SNAPSHOT_TURN) fields.turn[0] ^= 1u;
            if (fault == SNAPSHOT_RESPONSE) fields.response[0] ^= 1u;
            if (fault == SNAPSHOT_CYCLE) ++*fields.cycle;
            if (fault == SNAPSHOT_DUPLICATE) values[1u - index] = *selected;
            if (fault == SNAPSHOT_FANOUT) {
                selected->source.last_sequence = sources->last[1][source_index];
                selected->source.bytes = SNAG_MAX_PUBLIC_ITEM;
                value = *selected;
                item_count = fanout;
                for (size_t i = 0u; i < item_count; ++i) values[i] = value;
            }
            if (fields.graph) {
                snapshot_graph(fields.graph, values, item_count, &encoded);
            } else {
                assert(!snag_binary_public_items_encode(&encoded, values, item_count));
                *fields.items = (struct snag_binary_public_items){encoded.data, encoded.len};
            }
            free(values);
            assert(!snag_binary_event_encode(&payload, &event));
            records[used - 1u].payload = payload.data;
            records[used - 1u].size = payload.len;
            const char *type = NULL;
            json_t *data = NULL;
            assert(snag_binary_legacy_decode(&records[used - 1u], &type, &data) < 0);
            assert(errno == ENOTSUP && !type && !data);
        }
        input = next;
    }
    assert(used == count);
    snag_buf_reset(file);
    assert(!snag_buf_append(file, fixture->file.data, SNAG_BINARY_HEADER_SIZE));
    uint64_t turns = 0u;
    for (size_t begin = 0u; begin < count;) {
        size_t n = single || count - begin < 3u ? count - begin : 3u;
        for (size_t i = 0u; i < n; ++i) {
            if (records[begin + i].kind == SNAG_BINARY_TURN_STARTED) ++turns;
        }
        assert(!binary_fixture_append(file, &output, records + begin, n, turns, &output));
        begin += n;
    }
    assert(turns == input.turns);
    snag_buf_free(&public);
    snag_buf_free(&encoded);
    snag_buf_free(&payload);
    free(records);
    free(retained);
    snag_buf_free(&decoded);
}

static json_t *
snapshot_item(const json_t *template, const char *text)
{
    json_t *item = checked_json(json_deep_copy(template));
    assert(!json_object_set_new(item, "text", json_string(text)));
    return item;
}

static void
test_import_output_sources(struct snag_store *store, const char *cwd)
{
    char error[512];
    struct snag_session original, expected;
    snag_session_init(&original);
    snag_session_init(&expected);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    struct snag_response_graph templates = {0};
    for (size_t i = 0u; i < 20u; ++i) {
        assert(!snag_response_graph_add_public(&templates, SNAG_ITEM_ASSISTANT,
            SNAG_PHASE_FINAL_ANSWER, "stream-provider", "template"));
    }
    json_t *paths = checked_json(json_array());
    char *large = malloc(400001u);
    assert(large);
    large[400000u] = '\0';
    const char *chunks[] = {"First ", "middle ", "last."};
    for (unsigned int variation = 0u; variation < 13u; ++variation) {
        /* Reuse scope IDs across lifetimes, including a later snapshot with no stream. */
        commit_data(&original, "turn_started", direct_turn_data(cwd,
            "Import snapshot provenance.", GOAL_ID, variation + 1u, false, paths));
        commit_data(&original, "response_started", response_data());
        size_t count = variation == 11u ? 20u : variation == 9u ? 0u : 1u;
        for (size_t item = 0u; item < count; ++item) {
            size_t offset = 0u;
            for (size_t fragment = 0u; fragment < 3u; ++fragment) {
                memset(large, (int)('a' + fragment), 400000u);
                const char *text = variation == 10u ? large : chunks[fragment];
                commit_data(&original, "response_output",
                    json_pack("{s:s,s:s,s:i,s:I,s:I,s:o}", "turn_id", GOAL_ID,
                        "response_id", OTHER_ID, "cycle", 1, "index", (json_int_t)item,
                        "offset", (json_int_t)offset, "item",
                        snapshot_item(json_array_get(templates.items, item), text)));
                offset += strlen(text);
                if (variation == 10u && !fragment) {
                    assert(!snag_session_checkpoint(&original, error, sizeof(error)));
                }
            }
        }
        if (variation == 11u) {
            struct snag_legacy_recovery prefix;
            assert(original.response_open && json_array_size(original.response_public) == 20u);
            assert(!snag_store_reconcile_legacy(&original, &expected, NULL, NULL,
                &prefix, error, sizeof(error)));
            struct import_counts counts = check_import(&original, &expected);
            assert(counts.references == 3u && counts.literals == 8u);
            assert(counts.referenced_bytes == 1200024u && counts.cross_batch >= 1u);
            snag_session_close(&expected);
            snag_session_init(&expected);
        }
        json_t *partial = checked_json(variation == 9u ? json_pack("[o]",
            snapshot_item(json_array_get(templates.items, 0u), "First middle last.")) :
            json_deep_copy(original.response_public));
        json_t *item = json_array_get(partial, 0u);
        const char *key = NULL, *value = NULL;
        switch (variation) {
        case 1u: key = "text"; value = "First middle LAST."; break;
        case 2u: key = "provider_item_id"; value = "different-provider"; break;
        case 3u:
            assert(!json_array_set_new(partial, 0u,
                snapshot_item(json_array_get(templates.items, 1u), "First middle last.")));
            break;
        case 4u: key = "kind"; value = "refusal"; break;
        case 5u: key = "phase"; value = "commentary"; break;
        case 6u: key = "text"; value = "First middle "; break;
        case 7u: key = "text"; value = "First"; break;
        case 8u: key = "text"; value = "First "; break;
        case 11u:
            for (size_t i = 0u; i < count / 2u; ++i) {
                json_t *first = json_incref(json_array_get(partial, i));
                assert(!json_array_set(partial, i, json_array_get(partial, count - i - 1u)));
                assert(!json_array_set_new(partial, count - i - 1u, first));
            }
            break;
        case 12u: assert(!json_array_clear(partial)); break;
        default: break;
        }
        if (key) assert(!json_object_set_new(item, key, json_string(value)));
        commit_data(&original, "response_interrupted",
            json_pack("{s:s,s:s,s:i,s:s,s:s,s:o}", "turn_id", GOAL_ID, "response_id", OTHER_ID,
                "cycle", 1, "origin", "user", "reason", "cancelled", "partial_public", partial));
        commit_data(&original, "turn_interrupted", json_pack("{s:s,s:s,s:s}",
            "turn_id", GOAL_ID, "origin", "user", "reason", "cancelled"));
    }
    free(large);
    json_decref(paths);
    snag_response_graph_free(&templates);
    struct snag_legacy_recovery legacy;
    assert(!snag_store_reconcile_legacy(&original, &expected, NULL, NULL,
        &legacy, error, sizeof(error)));
    struct import_counts counts = check_import(&original, &expected);
    assert(counts.references == 23u && counts.literals == 8u && counts.cross_batch >= 1u);
    assert(counts.referenced_bytes == 1200384u);
    snag_session_close(&expected);
    snag_session_close(&original);
}

static void
test_snapshot_references(struct snag_store *store, const char *cwd,
    unsigned int ending, bool shortened)
{
    static const char *const chunks[2][2] = {{"Snapshot text: \"alpha\".\n", "Final segment."},
        {"Refusal prefix: ", "no."}};
    const char *correction = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    const char *types[] = {"response_interrupted", "response_failed", "response_output_correction"};
    bool completed = ending >= 3u, calls = ending == 4u || ending == 5u;
    bool refusal = ending < 5u;
    char error[512];
    struct snag_session original, expected, source, restored;
    snag_session_init(&original);
    snag_session_init(&expected);
    snag_session_init(&source);
    snag_session_init(&restored);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    struct snapshot_sources sequences = {0};
    json_t *paths = checked_json(json_array());
    struct snag_response_graph templates = {0};
    for (unsigned int i = 0u; i < 3u; ++i) {
        assert(!snag_response_graph_add_public(&templates,
            i == 1u && refusal ? SNAG_ITEM_REFUSAL : SNAG_ITEM_ASSISTANT,
            i == 1u && ending != 5u ? SNAG_PHASE_FINAL_ANSWER : SNAG_PHASE_COMMENTARY,
            i == 1u ? "refusal-provider" : "assistant-provider", "template"));
    }
    if (calls) {
        assert(!snag_response_graph_add_call(&templates, "call-item-a", "provider-call-a",
            "read_file", checked_json(json_pack("{s:s,s:i}",
                "path", "fixture.txt", "start_line", 1))));
        assert(!snag_response_graph_add_call(&templates, "call-item-b", "provider-call-b",
            "exec_command", checked_json(json_pack("{s:s}", "command", "fixture only"))));
    }
    for (unsigned int generation = 0u; generation < 3u; ++generation) {
        /* Reuse all scope IDs and cycle numbers across distinct turn lifetimes.
         * Byte/ID equality cannot authorize old or future source fragments. */
        commit_data(&original, "turn_started", direct_turn_data(cwd,
            "Snapshot response lifetime.", GOAL_ID, generation + 1u, false, paths));
        commit_data(&original, "response_started", response_data());
        for (unsigned int item = 0u; item < 2u; ++item) {
            for (unsigned int fragment = 0u; fragment < 2u; ++fragment) {
                uint64_t sequence = commit_data(&original, "response_output",
                    json_pack("{s:s,s:s,s:i,s:i,s:I,s:o}", "turn_id", GOAL_ID,
                        "response_id", OTHER_ID, "cycle", 1, "index", item,
                        "offset", (json_int_t)(fragment ? strlen(chunks[item][0]) : 0u),
                        "item", snapshot_item(json_array_get(templates.items, item),
                            chunks[item][fragment])));
                if (!fragment) {
                    sequences.first[generation][item] = sequence;
                    sequences.gap[generation][item] = original.next_seq;
                    assert(!snag_session_checkpoint(&original, error, sizeof(error)));
                    assert(original.next_seq == sequences.gap[generation][item] + 1u);
                } else {
                    sequences.last[generation][item] = sequence;
                }
            }
            sequences.bytes[item] = strlen(chunks[item][0]) + strlen(chunks[item][1]);
        }
        json_t *partial = checked_json(json_deep_copy(original.response_public));
        if (generation == 1u && shortened) {
            assert(!json_object_set_new(json_array_get(partial, 0u), "text",
                json_string(chunks[0][0])));
            assert(!json_array_append_new(partial,
                snapshot_item(json_array_get(templates.items, 2u),
                    "Independently recorded snapshot-only text.")));
            json_t *first = json_incref(json_array_get(partial, 0u));
            assert(!json_array_set(partial, 0u, json_array_get(partial, 1u)));
            assert(!json_array_set_new(partial, 1u, first));
        }
        unsigned int kind = generation == 1u ? ending : 0u;
        json_t *data = checked_json(json_pack("{s:s,s:s,s:i,s:o}", "turn_id", GOAL_ID,
            "response_id", OTHER_ID, "cycle", 1, kind >= 3u ? "items" : "partial_public", partial));
        if (!kind) {
            assert(!json_object_set_new(data, "origin", json_string("user")));
            assert(!json_object_set_new(data, "reason", json_string("cancelled")));
        } else if (kind == 1u) {
            assert(!json_object_set_new(data, "class", json_string("internal")));
            assert(!json_object_set_new(data, "retry_count", json_integer(0)));
            assert(!json_object_set_new(data, "message", json_string("fixture failure")));
        } else if (kind == 2u) {
            assert(!json_object_set_new(data, "correction_id", json_string(correction)));
            assert(!json_object_set_new(data, "text", json_string(SNAG_EMPTY_OUTPUT_CORRECTION)));
        } else {
            if (calls) {
                assert(!json_array_insert(partial, 0u, json_array_get(templates.items, 3u)));
                assert(!json_array_insert(partial, 2u, json_array_get(templates.items, 4u)));
            }
            assert(!json_object_set_new(data, "status", json_string("completed")));
            assert(!json_object_set_new(data, "provider_response_id",
                json_string("graph-provider")));
            json_t *usage = checked_json(json_pack("{s:i,s:i,s:i,s:i}", "input_tokens", 17,
                "output_tokens", 7, "reasoning_tokens", 3, "total_tokens", 24));
            if (kind != 3u) {
                assert(!json_object_set_new(usage, "cached_tokens",
                    kind == 4u ? json_null() : json_integer(kind == 5u ? 4 : 20)));
                assert(!json_object_set_new(data, "continuation_scope", json_string(ZERO_HASH)));
                json_t *continuation = kind == 4u ? json_null() : json_array();
                assert(continuation);
                if (kind == 6u) {
                    size_t before[] = {0u, 1u, json_array_size(partial)};
                    for (size_t i = 0u; i < 3u; ++i) {
                        assert(!json_array_append_new(continuation,
                            json_pack("{s:I,s:{s:s,s:n,s:{s:b}}}", "before", (json_int_t)before[i],
                                "item", "type", "reasoning", "encrypted_content", "x", "kept", 1)));
                    }
                }
                assert(!json_object_set_new(data, "continuation", continuation));
            }
            assert(!json_object_set_new(data, "usage", usage));
        }
        uint64_t sequence = commit_data(&original,
            kind >= 3u ? "response_completed" : types[kind], data);
        if (generation == 1u) sequences.target = sequence;
        if (kind >= 3u && calls) {
            for (size_t i = 3u; i < 5u; ++i) {
                struct snag_response_item call = snag_response_graph_item(&templates, i);
                commit_data(&original, "tool_finished", json_pack("{s:s,s:s,s:o}",
                    "turn_id", GOAL_ID, "call_id", call.call_id,
                    "result", snag_tool_result_not_run("turn_cancelled")));
            }
        }
        commit_data(&original, "turn_interrupted", json_pack("{s:s,s:s,s:s}",
            "turn_id", GOAL_ID, "origin", "user", "reason", "cancelled"));
        assert(!original.pending_steering_count);
    }
    json_decref(paths);
    snag_response_graph_free(&templates);
    struct replay_fixture fixture = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX},
        .batch_records = 3u, .events = checked_json(json_array())};
    for (size_t i = 0u; i < 3u; ++i) {
        fixture.payload[i].max = SNAG_MAX_EVENT_LINE;
    }
    struct snag_legacy_recovery legacy;
    assert(!snag_store_reconcile_legacy(&original, &expected, fixture_event, &fixture,
        &legacy, error, sizeof(error)));
    fixture_flush(&fixture);
    struct import_counts imported = check_import(&original, &expected);
    assert(imported.references == 6u && imported.literals == (shortened ? 1u : 0u));
    memcpy(source.id, original.id, sizeof(source.id));
    source.log_fd = temporary_fd();
    source.lock_fd = temporary_fd();
    assert(!snag_lock_file(source.lock_fd, false));
    struct snag_buf file = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    struct snag_buf prefix = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    observed_replay(&source, &restored, &expected, fixture.events, &fixture.file, 0u, false);
    for (unsigned int single = 0u; single < 2u; ++single) {
        for (unsigned int mask = 1u; mask < 4u; ++mask) {
            rewrite_snapshot(&fixture, &sequences, mask, SNAPSHOT_VALID, single != 0u, &file);
            observed_replay(&source, &restored, &expected, fixture.events, &file, 0u, false);
            prefix_through(&file, sequences.target, &prefix);
            observed_replay(&source, &restored, NULL, fixture.events, &prefix, 0u, false);
            assert(restored.active_turn && restored.turn_count == 2u && !restored.response_open);
            assert(restored.pending_steering_count == (ending == 2u ? 1u : 0u));
            if (completed) {
                assert(restored.response_complete);
                assert(restored.pending_call_count == (calls ? 2u : 0u));
                enum snag_graph_outcome outcome = ending == 5u ? SNAG_GRAPH_CALLS :
                    ending == 4u || shortened ? SNAG_GRAPH_CONFLICT :
                    ending == 3u ? SNAG_GRAPH_REFUSAL : SNAG_GRAPH_FINAL;
                assert(restored.response_outcome == outcome);
                assert(restored.usage_totals.responses == 1u &&
                    restored.usage_totals.input_tokens == 17u);
                assert(restored.usage_totals.cached_seen == (ending == 5u));
            }
            if (single && mask == 3u) {
                observed_replay(&source, &restored, NULL, fixture.events,
                    &file, sequences.target, true);
                assert(!snag_buf_append(&file, "\xff", 1u));
                observed_replay(&source, &restored, &expected, fixture.events, &file, 0u, false);
            }
            for (enum snapshot_fault fault = SNAPSHOT_OLD; fault <= SNAPSHOT_FANOUT; ++fault) {
                if (fault == SNAPSHOT_PHASE && refusal && mask == (shortened ? 1u : 2u)) {
                    continue;
                }
                rewrite_snapshot(&fixture, &sequences, mask, fault, single != 0u, &file);
                if (fault == SNAPSHOT_FANOUT) {
                    struct snag_binary_recovery recovery;
                    assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0);
                    assert(errno == EFBIG && recovery.problem_seq == sequences.target);
                } else {
                    observed_replay(&source, &restored, NULL, fixture.events,
                        &file, sequences.target, false);
                }
            }
        }
    }
    snag_buf_free(&prefix);
    snag_buf_free(&file);
    for (size_t i = 0u; i < 3u; ++i) snag_buf_free(&fixture.payload[i]);
    snag_buf_free(&fixture.file);
    json_decref(fixture.events);
    json_decref(fixture.creation);
    snag_session_close(&restored);
    snag_session_close(&source);
    snag_session_close(&expected);
    snag_session_close(&original);
}

static void
test_queued_instructions(struct snag_store *store, const char *cwd, bool edited, bool nonempty)
{
    const char *text = "Queued payload authority is independent of instruction-list storage.";
    const char *old_id = "cccccccccccccccccccccccccccccccc";
    const char *turn_id = "dddddddddddddddddddddddddddddddd";
    char error[512];
    struct snag_session original, expected, source, restored;
    snag_session_init(&original);
    snag_session_init(&expected);
    snag_session_init(&source);
    snag_session_init(&restored);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    json_t *paths = checked_json(json_array());
    if (nonempty) {
        char *path = snag_path_join(cwd, "queued instructions.md");
        assert(path && !json_array_append_new(paths, json_string(path)));
        free(path);
        path = snag_path_join(cwd, "additional queued instructions.md");
        assert(path && !json_array_append_new(paths, json_string(path)));
        free(path);
    }
    uint64_t originals[3];
    json_t *input = input_data("Cancelled input with independent metadata.", false, paths);
    assert(!json_object_set_new(input, "read_only", json_true()));
    assert(!json_object_set_new(input, "provider", json_string("source-provider")));
    assert(!json_object_set_new(input, "model", json_string("source-model")));
    assert(!json_object_set_new(input, "effort", json_string("source-effort")));
    originals[0] = commit_data(&original, "input_received", input);
    commit_data(&original, "input_cancelled", json_object());
    struct snag_irc_event observed = {.kind = SNAG_IRC_MESSAGE, .timestamp_ms = 1u,
        .stream = GOAL_ID, .sequence = 1u, .input = true, .endpoint = "fixture",
        .room = "#fixture", .nick = "operator", .op = true};
    assert(snag_strcpy(observed.text, sizeof(observed.text), text));
    uint64_t observation = commit_data(&original, "irc_event", snag_irc_event_data(&observed));
    originals[1] = commit_data(&original, "irc_admitted",
        json_pack("{s:[I],s:o}", "sequences", (json_int_t)observation,
            "input", input_data(text, false, paths)));
    commit_data(&original, "input_cancelled", json_object());
    originals[2] = commit_data(&original, "turn_started",
        direct_turn_data(cwd, "Earlier turn instruction discovery.", old_id, 1, false, paths));
    commit_data(&original, "turn_interrupted", json_pack("{s:s,s:s,s:s}",
        "turn_id", old_id, "origin", "user", "reason", "cancelled"));

    struct turn_sources sequences = {0};
    sequences.creation = commit_data(&original, "future_turn_queued",
        queue_data(GOAL_ID, text, ""));
    sequences.other = commit_data(&original, "future_turn_queued", queue_data(OTHER_ID, text, ""));
    sequences.text = sequences.creation;
    if (edited) {
        sequences.text = commit_data(&original, "future_turn_edited",
            json_pack("{s:s,s:s,s:b,s:b,s:[{s:s,s:s}]}", "queue_id", GOAL_ID,
                "text", text, "read_only", 0, "armed", 1, "content", "type", "input_text",
                "text", "ignored edited content"));
    }
    json_t *turn = direct_turn_data(cwd, text, turn_id, 2, false, paths);
    assert(!json_object_set_new(turn, "input_kind", json_string("queued")));
    assert(!json_object_set_new(turn, "queue_id", json_string(GOAL_ID)));
    assert(!json_object_set_new(turn, "queue_seq", json_integer((json_int_t)sequences.creation)));
    sequences.turn = commit_data(&original, "turn_started", turn);
    commit_data(&original, "turn_interrupted", json_pack("{s:s,s:s,s:s}",
        "turn_id", turn_id, "origin", "user", "reason", "cancelled"));
    sequences.future = commit_data(&original, "input_received", input_data(text, false, paths));

    struct replay_fixture fixture = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX},
        .batch_records = 3u, .events = checked_json(json_array())};
    for (size_t i = 0u; i < 3u; ++i) {
        fixture.payload[i].max = SNAG_MAX_EVENT_LINE;
    }
    struct snag_legacy_recovery legacy;
    assert(!snag_store_reconcile_legacy(&original, &expected, fixture_event, &fixture,
        &legacy, error, sizeof(error)));
    fixture_flush(&fixture);
    check_import(&original, &expected);
    memcpy(source.id, original.id, sizeof(source.id));
    source.log_fd = temporary_fd();
    source.lock_fd = temporary_fd();
    assert(!snag_lock_file(source.lock_fd, false));
    struct replay_fixture current = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX}};
    struct replay_fixture combined = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX}};
    struct snag_buf file = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    struct snag_buf prefix = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    observed_replay(&source, &restored, &expected, fixture.events, &fixture.file, 0u, false);
    const enum turn_ref_case modes[] = {TURN_REF_TEXT, TURN_REF_CONTENT, TURN_REF_BOTH};
    const enum receipt_fault faults[] = {RECEIPT_INSTRUCTION_OFFSET, RECEIPT_INSTRUCTION_SIZE,
        RECEIPT_INSTRUCTION_LEAF, RECEIPT_QUEUE_OWNER, RECEIPT_QUEUE_CREATION,
        RECEIPT_READ_ONLY, RECEIPT_GOAL};
    for (size_t i = 0u; i < sizeof(originals) / sizeof(originals[0]); ++i) {
        rewrite_receipt(&fixture, sequences.turn, originals[i], 4u, RECEIPT_VALID, &current.file);
        for (size_t j = 0u; j <= sizeof(modes) / sizeof(modes[0]); ++j) {
            const struct replay_fixture *referenced = &current;
            if (j) {
                rewrite_turn(&current, &sequences, modes[j - 1u], &combined.file);
                referenced = &combined;
            }
            observed_replay(&source, &restored, &expected, fixture.events,
                &referenced->file, 0u, false);
            prefix_through(&referenced->file, sequences.turn, &prefix);
            observed_replay(&source, &restored, NULL, fixture.events, &prefix, 0u, false);
            assert(restored.active_turn && !strcmp(restored.active_turn_id, turn_id));
            assert(json_equal(restored.active_instructions, paths));
            assert(!restored.active_read_only && restored.pending_queue_count == 1u);
            assert(!strcmp(restored.pending_queue[0].queue_id, OTHER_ID));
            rewrite_receipt(referenced, sequences.future, sequences.turn, 4u,
                RECEIPT_VALID, &file);
            observed_replay(&source, &restored, &expected, fixture.events, &file, 0u, false);
            rewrite_receipt(referenced, sequences.future, sequences.turn, 4u,
                RECEIPT_CHAIN, &file);
            observed_replay(&source, &restored, NULL, fixture.events,
                &file, sequences.future, false);
        }
        for (size_t j = 0u; j < sizeof(faults) / sizeof(faults[0]); ++j) {
            rewrite_receipt(&fixture, sequences.turn, originals[i], 4u, faults[j], &file);
            observed_replay(&source, &restored, NULL, fixture.events, &file, sequences.turn, false);
        }
    }
    const uint64_t noncausal[] = {sequences.turn, sequences.future};
    for (size_t i = 0u; i < sizeof(noncausal) / sizeof(noncausal[0]); ++i) {
        rewrite_receipt(&fixture, sequences.turn, noncausal[i], 4u, RECEIPT_VALID, &file);
        observed_replay(&source, &restored, NULL, fixture.events, &file, sequences.turn, false);
    }
    snag_buf_free(&prefix);
    snag_buf_free(&file);
    snag_buf_free(&combined.file);
    snag_buf_free(&current.file);
    for (size_t i = 0u; i < 3u; ++i) snag_buf_free(&fixture.payload[i]);
    snag_buf_free(&fixture.file);
    json_decref(fixture.events);
    json_decref(fixture.creation);
    json_decref(paths);
    snag_session_close(&restored);
    snag_session_close(&source);
    snag_session_close(&expected);
    snag_session_close(&original);
}

static void
test_voice_references(struct snag_store *store, const char *cwd, bool active, bool equal)
{
    const char *transcript = "Keep the late tool result with the original source session.";
    const char *request = equal ? transcript : "Preserve the source ownership of that late result.";
    const char *connections[] = {GOAL_ID, OTHER_ID, "cccccccccccccccccccccccccccccccc"};
    const char *labels[] = {"canonical", "accepted", "later"};
    char error[512], ids[3][SNAG_ID_HEX_LEN + 1u];
    uint64_t plain[2], voices[3];
    struct snag_session original, expected, source, restored;
    snag_session_init(&original);
    snag_session_init(&expected);
    snag_session_init(&source);
    snag_session_init(&restored);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    json_t *paths = checked_json(json_array());
    for (size_t i = 0u; i < 2u; ++i) {
        plain[i] = commit_data(&original, "input_received",
            input_data(i ? request : transcript, true, paths));
        commit_data(&original, "input_cancelled", json_object());
    }
    for (size_t i = 0u; i < 3u; ++i) {
        json_t *voice = checked_json(json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s}",
            "connection_id", connections[i], "input_id", labels[i], "response_id", labels[i],
            "call_id", labels[i], "provider", labels[i], "model", labels[i],
            "transcript", transcript, "request", request));
        bool duplicate;
        voices[i] = original.next_seq;
        assert(!snag_session_voice_queue(&original, voice, ids[i], &duplicate,
            error, sizeof(error)));
        assert(!duplicate && original.next_seq == voices[i] + 1u);
        json_decref(voice);
        if (i == 0u) {
            commit_data(&original, "future_turn_cancelled", json_pack("{s:s,s:[s]}",
                "reason", "user", "queue_ids", ids[i]));
            if (active) commit_data(&original, "turn_started",
                direct_turn_data(cwd, "Existing work.", GOAL_ID, 1, false, paths));
        }
        if (i == 1u) assert(!snag_session_checkpoint(&original, error, sizeof(error)));
    }
    if (active) commit_data(&original, "turn_interrupted",
        json_pack("{s:s,s:s,s:s}", "turn_id", GOAL_ID, "origin", "user", "reason", "cancelled"));
    json_t *turn = direct_turn_data(cwd, original.pending_queue[0].text, OTHER_ID,
        active ? 2 : 1, false, paths);
    assert(!json_object_del(turn, "content"));
    assert(!json_object_set_new(turn, "input_kind", json_string("queued")));
    assert(!json_object_set_new(turn, "queue_id", json_string(ids[1])));
    assert(!json_object_set_new(turn, "queue_seq", json_integer((json_int_t)voices[1])));
    struct turn_sources sequences = {.creation = voices[1], .text = voices[1],
        .other = voices[0], .future = voices[2]};
    sequences.turn = commit_data(&original, "turn_started", turn);
    json_decref(paths);

    struct replay_fixture fixture = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX},
        .batch_records = 3u, .events = checked_json(json_array())};
    for (size_t i = 0u; i < 3u; ++i) fixture.payload[i].max = SNAG_MAX_EVENT_LINE;
    struct snag_legacy_recovery legacy;
    assert(!snag_store_reconcile_legacy(&original, &expected, fixture_event, &fixture,
        &legacy, error, sizeof(error)));
    fixture_flush(&fixture);
    check_import(&original, &expected);
    memcpy(source.id, original.id, sizeof(source.id));
    source.log_fd = temporary_fd();
    source.lock_fd = temporary_fd();
    assert(!snag_lock_file(source.lock_fd, false));
    struct replay_fixture current = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX},
        .batch_records = 3u};
    struct snag_buf file = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    struct snag_buf prefix = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    observed_replay(&source, &restored, &expected, fixture.events, &fixture.file, 0u, false);

    for (unsigned int voice_fields = 8u; voice_fields <= 24u; voice_fields += 8u) {
        for (unsigned int text = 0u; text < 2u; ++text) {
            unsigned int fields = voice_fields | text;
            rewrite_receipt(&fixture, voices[1], voices[0], fields, RECEIPT_VALID, &current.file);
            observed_replay(&source, &restored, &expected, fixture.events,
                &current.file, 0u, false);
            prefix_through(&current.file, voices[1], &prefix);
            observed_replay(&source, &restored, NULL, fixture.events, &prefix, 0u, false);
            assert(restored.pending_queue_count == 1u && restored.active_turn == active);
            assert(restored.pending_queue[0].seq == voices[1]);
            assert(!strcmp(restored.pending_queue[0].queue_id, ids[1]));
            rewrite_receipt(&current, voices[2], voices[1], fields, RECEIPT_VALID, &file);
            observed_replay(&source, &restored, &expected, fixture.events, &file, 0u, false);
            rewrite_receipt(&current, voices[2], voices[1], fields, RECEIPT_VOICE_CHAIN, &file);
            observed_replay(&source, &restored, NULL, fixture.events, &file, voices[2], false);
            rewrite_turn(&current, &sequences, TURN_REF_TEXT, &file);
            observed_replay(&source, &restored, &expected, fixture.events, &file, 0u, false);
            /* A later queue's equal prompt is not this turn's declared original. */
            rewrite_receipt(&current, sequences.turn, voices[2], 1u, RECEIPT_VALID, &file);
            observed_replay(&source, &restored, NULL, fixture.events, &file, sequences.turn, false);
            if (equal) {
                rewrite_receipt(&fixture, voices[1], voices[0], fields, RECEIPT_VOICE_ALIAS, &file);
                observed_replay(&source, &restored, &expected, fixture.events, &file, 0u, false);
            }
        }
    }
    for (size_t i = 0u; i < 2u; ++i) {
        rewrite_receipt(&fixture, voices[1], plain[i], 8u << i, RECEIPT_VOICE_TEXT, &file);
        observed_replay(&source, &restored, &expected, fixture.events, &file, 0u, false);
    }
    rewrite_receipt(&fixture, voices[1], plain[0], 8u, RECEIPT_VOICE_TEXT, &current.file);
    rewrite_receipt(&current, voices[1], plain[1], 16u, RECEIPT_VOICE_TEXT, &file);
    observed_replay(&source, &restored, &expected, fixture.events, &file, 0u, false);

    const enum receipt_fault faults[] = {RECEIPT_VOICE_ROLE, RECEIPT_VOICE_OFFSET,
        RECEIPT_VOICE_SIZE, RECEIPT_VOICE_CONNECTION, RECEIPT_VOICE_INPUT,
        RECEIPT_VOICE_ID, RECEIPT_VOICE_TURN};
    for (unsigned int fields = 8u; fields <= 24u; fields += 8u) {
        for (size_t i = 0u; i < sizeof(faults) / sizeof(faults[0]); ++i) {
            rewrite_receipt(&fixture, voices[1], voices[0], fields, faults[i], &file);
            observed_replay(&source, &restored, NULL, fixture.events, &file, voices[1], false);
        }
        for (size_t i = 1u; i < 3u; ++i) {
            rewrite_receipt(&fixture, voices[1], voices[i], fields, RECEIPT_VALID, &file);
            observed_replay(&source, &restored, NULL, fixture.events, &file, voices[1], false);
        }
    }
    /* Visitor rejection is an unrepaired semantic failure, including with a tail. */
    const uint64_t stops[] = {1u, voices[1], sequences.turn};
    for (size_t i = 0u; i < sizeof(stops) / sizeof(stops[0]); ++i)
        observed_replay(&source, &restored, NULL, fixture.events, &fixture.file, stops[i], true);
    snag_buf_reset(&file);
    assert(!snag_buf_append(&file, fixture.file.data, fixture.file.len));
    assert(!snag_buf_append(&file, "\xff", 1u));
    observed_replay(&source, &restored, &expected, fixture.events, &file, 0u, false);
    observed_replay(&source, &restored, NULL, fixture.events, &file, voices[1], true);

    snag_buf_free(&prefix);
    snag_buf_free(&file);
    snag_buf_free(&current.file);
    for (size_t i = 0u; i < 3u; ++i) snag_buf_free(&fixture.payload[i]);
    snag_buf_free(&fixture.file);
    json_decref(fixture.events);
    json_decref(fixture.creation);
    snag_session_close(&restored);
    snag_session_close(&source);
    snag_session_close(&expected);
    snag_session_close(&original);
}

static void
test_voice_original(struct snag_store *store, const char *cwd)
{
    const char *text = "Equal transcript and paraphrase bytes still have distinct original roles.";
    char error[512], queue_id[SNAG_ID_HEX_LEN + 1u];
    struct snag_session original, expected, source, restored;
    snag_session_init(&original);
    snag_session_init(&expected);
    snag_session_init(&source);
    snag_session_init(&restored);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    json_t *voice = checked_json(json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s}",
        "connection_id", GOAL_ID, "input_id", "voice-input", "response_id", "voice-response",
        "call_id", "voice-call", "provider", "voice-provider", "model", "voice-model",
        "transcript", text, "request", text));
    bool duplicate;
    uint64_t canonical = original.next_seq;
    assert(!snag_session_voice_queue(&original, voice, queue_id, &duplicate, error, sizeof(error)));
    assert(!duplicate && original.next_seq == canonical + 1u);
    json_decref(voice);
    commit_data(&original, "future_turn_cancelled", json_pack("{s:s,s:[s]}",
        "reason", "user", "queue_ids", queue_id));
    json_t *paths = checked_json(json_array());
    struct turn_sources sequences = {0};
    sequences.creation = commit_data(&original, "input_received", input_data(text, false, paths));
    sequences.text = sequences.creation;
    sequences.turn = commit_data(&original, "turn_started",
        direct_turn_data(cwd, text, OTHER_ID, 1, false, paths));
    json_decref(paths);
    struct replay_fixture fixture = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX},
        .batch_records = 3u};
    for (size_t i = 0u; i < 3u; ++i) fixture.payload[i].max = SNAG_MAX_EVENT_LINE;
    struct snag_legacy_recovery legacy;
    assert(!snag_store_reconcile_legacy(&original, &expected, fixture_event, &fixture,
        &legacy, error, sizeof(error)));
    fixture_flush(&fixture);
    check_import(&original, &expected);
    memcpy(source.id, original.id, sizeof(source.id));
    source.log_fd = temporary_fd();
    source.lock_fd = temporary_fd();
    assert(!snag_lock_file(source.lock_fd, false));
    struct replay_fixture receipt = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX},
        .batch_records = 3u};
    struct snag_buf file = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    struct snag_binary_recovery recovery;
    for (enum receipt_fault role = RECEIPT_TRANSCRIPT; role <= RECEIPT_REQUEST; ++role) {
        rewrite_receipt(&fixture, sequences.creation, canonical, 1u, role, &receipt.file);
        assert(!replay_check(&source, &restored, &recovery, receipt.file.data, receipt.file.len));
        same_core_state(&expected, &restored);
        rewrite_turn(&receipt, &sequences, TURN_REF_BOTH, &file);
        assert(!replay_check(&source, &restored, &recovery, file.data, file.len));
        same_core_state(&expected, &restored);
        rewrite_turn(&receipt, &sequences, TURN_REF_VOICE_ALIAS, &file);
        assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0);
        assert(errno == EINVAL && recovery.problem_seq == sequences.turn);
    }
    snag_buf_free(&file);
    snag_buf_free(&receipt.file);
    for (size_t i = 0u; i < 3u; ++i) snag_buf_free(&fixture.payload[i]);
    snag_buf_free(&fixture.file);
    json_decref(fixture.creation);
    snag_session_close(&restored);
    snag_session_close(&source);
    snag_session_close(&expected);
    snag_session_close(&original);
}

static void
start_compact_fixture(struct snag_session *session, const char *id, const char *scope)
{
    const char *hash = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    json_t *data = checked_json(json_pack("{s:s,s:s,s:s,s:s,s:i,s:s,s:o,s:s,s:s,s:s,s:I,s:s}",
        "capability_version", SNAJPAGENT_CAPABILITY_VERSION, "compact_id", id,
        "count_method", "exact", "count_request_sha256", hash, "input_tokens_bound", 1,
        "model", session->default_model, "predecessor_compact_id",
        session->compact_id[0] ? json_string(session->compact_id) : json_null(),
        "profile_id", SNAJPAGENT_PROFILE_ID, "reason", "manual", "request_sha256", hash,
        "source_seq", (json_int_t)(session->next_seq - 1u), "source_sha256", hash));
    if (scope) assert(!json_object_set_new(data, "continuation_scope", json_string(scope)));
    commit_data(session, "compaction_started", data);
}

static void
finish_compact_fixture(struct snag_session *session, const char *scope, const char *text)
{
    json_t *output = checked_json(json_pack("[{s:s,s:s,s:{s:[n,b,i]}}]", "type", "compaction",
        "encrypted_content", text, "vendor", "mixed", 1, 7));
    char hash[65];
    assert(!snag_json_digest(output, hash));
    json_t *data = checked_json(json_pack("{s:s,s:s,s:i,s:o,s:s,s:s,s:s,s:i,s:s}",
        "compact_id", session->active_compact_id, "count_method", "exact", "input_tokens_bound", 1,
        "output", output, "output_count_method", "exact", "output_count_request_sha256", hash,
        "output_sha256", hash, "output_tokens_bound", 1,
        "source_sha256", session->active_compact_source_sha256));
    if (scope) assert(!json_object_set_new(data, "continuation_scope", json_string(scope)));
    commit_data(session, "compaction_completed", data);
}

static void
test_compact_origins(struct snag_store *store, const char *cwd, bool scoped)
{
    char error[512];
    struct snag_session original;
    snag_session_init(&original);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    const char *scope = scoped ?
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee" : NULL;
    start_compact_fixture(&original, GOAL_ID, scope);
    check_import(&original, &original);
    finish_compact_fixture(&original, scope, "original opaque payload");
    check_import(&original, &original);
    start_compact_fixture(&original, OTHER_ID, scope);
    check_import(&original, &original);
    commit_data(&original, "compaction_interrupted", json_pack("{s:s,s:s}",
        "compact_id", OTHER_ID, "reason", "user"));
    assert(!strcmp(snag_json_string(json_array_get(original.compact_output, 0u),
        "encrypted_content"), "original opaque payload"));
    check_import(&original, &original);
    /* Reuse a completed ID; the new start and source boundary still distinguish it. */
    start_compact_fixture(&original, GOAL_ID, scope);
    finish_compact_fixture(&original, scope, "replacement opaque payload");
    check_import(&original, &original);
    snag_session_close(&original);
}

static json_t *
download_data(unsigned number, uint64_t bytes)
{
    char id[33];
    snprintf(id, sizeof(id), "%032x", number);
    return checked_json(json_pack("{s:s,s:s,s:s,s:I,s:I,s:s,s:I}", "id", id,
        "path", "/tmp/native-checkpoint-download", "name", "download", "bytes", (json_int_t)bytes,
        "mtime", (json_int_t)0, "sha256",
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
        "queued_ms", (json_int_t)17));
}

static void
test_download_origins(struct snag_store *store, const char *cwd)
{
    char error[512];
    struct snag_session original;
    snag_session_init(&original);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    assert(!original.download_queue);
    check_import(&original, &original);
    for (unsigned i = 1u; i <= 10u; ++i) {
        commit_data(&original, "download_queued", download_data(i, i));
    }
    check_import(&original, &original);
    commit_data(&original, "download_removed", json_pack("{s:s,s:s}",
        "id", "00000000000000000000000000000004", "reason", "fixture"));
    assert(json_array_size(original.download_queue) == 9u);
    check_import(&original, &original);
    commit_data(&original, "download_queued", download_data(4u, 777u));
    assert(json_integer_value(json_object_get(json_array_get(original.download_queue, 9u),
        "bytes")) == 777);
    check_import(&original, &original);
    commit_data(&original, "downloads_cleared", json_pack("{s:s}", "reason", "fixture"));
    assert(original.download_queue && !json_array_size(original.download_queue));
    check_import(&original, &original);
    snag_session_close(&original);
}

static void
test_input_context_origins(struct snag_store *store, const char *cwd)
{
    char error[512];
    struct snag_session original;
    snag_session_init(&original);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    json_t *paths = checked_json(json_array());
    uint64_t input = commit_data(&original, "input_received", input_data("pending", true, paths));
    check_import(&original, &original);
    commit_data(&original, "input_cancelled", json_object());
    commit_data(&original, "turn_started",
        direct_turn_data(cwd, "active", GOAL_ID, 1, false, paths));
    json_decref(paths);
    const char *later = "cccccccccccccccccccccccccccccccc";
    uint64_t first = commit_data(&original, "steering_added",
        steering_data(OTHER_ID, "first", 17u));
    json_t *steering = steering_data(later, "later", 23u);
    assert(!json_object_del(steering, "received_at_ms"));
    uint64_t second = commit_data(&original, "steering_added", steering);
    commit_data(&original, "input_admitted", json_pack("{s:s,s:i,s:[s]}",
        "turn_id", GOAL_ID, "time_ms", 42, "steering_ids", OTHER_ID));
    assert(original.pending_steering_count == 2u);
    assert(original.pending_steering[0].first_context_ms == 42u);
    assert(!original.pending_steering[1].first_context_ms);
    check_import(&original, &original);
    json_t *response = response_data();
    assert(!json_array_append_new(json_object_get(response, "steering_ids"),
        json_string(OTHER_ID)));
    commit_data(&original, "response_started", response);
    assert(original.pending_steering_count == 1u && original.pending_steering[0].seq == second);
    check_import(&original, &original);
    uint64_t correction = commit_data(&original, "response_output_correction", json_pack(
        "{s:s,s:s,s:i,s:s,s:s,s:[]}", "turn_id", GOAL_ID, "response_id", OTHER_ID,
        "cycle", 1, "correction_id", "dddddddddddddddddddddddddddddddd",
        "text", SNAG_EMPTY_OUTPUT_CORRECTION, "partial_public"));
    assert(original.pending_steering_count == 2u &&
        original.pending_steering[1].seq == correction && !original.pending_steering[1].content);
    check_import(&original, &original);
    assert(input < first && first < second);
    snag_session_close(&original);
}

static uint64_t
start_process_call(struct snag_session *session, size_t index)
{
    const struct snag_pending_call *call = &session->pending_calls[index];
    return commit_data(session, "tool_started", json_pack("{s:s,s:s,s:s,s:s}",
        "turn_id", GOAL_ID, "call_id", call->call_id, "action_sha256", call->action_sha256,
        "resolved_workdir", session->cwd));
}

static void
test_checkpoint_after_streamed_tools(struct snag_store *store, const char *cwd, bool legacy)
{
    struct snag_session state;
    snag_session_init(&state);
    char error[512] = "";
    int rc = legacy ? legacy_fixture_create(store, &state, cwd,
        "default", "fixture", "default", error, sizeof(error)) :
        snag_session_create(store, &state, cwd, "default", "fixture", "default",
            error, sizeof(error));
    assert(!rc);
    json_t *paths = checked_json(json_array());
    commit_data(&state, "turn_started",
        direct_turn_data(cwd, "Checkpoint completed tools.", GOAL_ID, 1u, false, paths));
    json_decref(paths);
    commit_data(&state, "response_started", response_data());

    struct snag_response_graph graph = {0};
    assert(!snag_response_graph_add_public(&graph, SNAG_ITEM_ASSISTANT,
        SNAG_PHASE_COMMENTARY, "commentary", "Before the tools."));
    commit_data(&state, "response_output", json_pack(
        "{s:s,s:s,s:i,s:i,s:i,s:O}", "turn_id", GOAL_ID, "response_id", OTHER_ID,
        "cycle", 1, "index", 0, "offset", 0, "item", json_array_get(graph.items, 0u)));
    for (size_t i = 0u; i < 2u; ++i) {
        const char *id = i ? "second" : "first";
        assert(!snag_response_graph_add_call(&graph, id, id, "read_file",
            json_pack("{s:s}", "path", "fixture.txt")));
    }
    commit_data(&state, "response_completed", json_pack(
        "{s:s,s:s,s:i,s:s,s:s,s:O,s:{s:i,s:i,s:i,s:i}}", "turn_id", GOAL_ID,
        "response_id", OTHER_ID, "cycle", 1, "status", "completed",
        "provider_response_id", "fixture", "items", graph.items, "usage",
        "input_tokens", 17, "output_tokens", 7, "reasoning_tokens", 3, "total_tokens", 24));
    for (size_t i = 0u; i < 2u; ++i) {
        start_process_call(&state, i);
        commit_data(&state, "tool_finished", json_pack("{s:s,s:s,s:o}",
            "turn_id", GOAL_ID, "call_id", state.pending_calls[i].call_id,
            "result", snag_tool_result_terminal(true, "tool result")));
        rc = snag_session_checkpoint(&state, error, sizeof(error));
        if (rc < 0) fprintf(stderr, "checkpoint after tool %zu: %s\n", i + 1u, error);
        assert(!rc);
        assert(state.active_turn);
        if (!i) {
            assert(state.pending_call_count == 2u && state.response_complete &&
                state.active_response_id[0] && state.response_public);
        } else {
            assert(!state.pending_call_count && !state.response_complete &&
                !state.active_response_id[0] && !state.response_public &&
                !state.response_public_bytes);
        }
    }
    char id[sizeof(state.id)];
    memcpy(id, state.id, sizeof(id));
    uint64_t next = state.next_seq;
    snag_session_close(&state);
    snag_session_init(&state);
    assert(!snag_session_open(store, &state, id, error, sizeof(error)));
    assert(state.next_seq == next && state.active_turn && !state.pending_call_count &&
        !state.active_response_id[0] && !state.response_public);
    assert(!snag_session_checkpoint(&state, error, sizeof(error)));
    if (legacy) check_import(&state, &state);
    snag_session_close(&state);
    snag_response_graph_free(&graph);
}

static void
finish_process_call(struct snag_session *session, size_t index, bool started)
{
    commit_data(session, "tool_finished", json_pack("{s:s,s:s,s:o}", "turn_id", GOAL_ID,
        "call_id", session->pending_calls[index].call_id, "result", started ?
        snag_tool_result_outcome_unknown("owner_lost") :
        snag_tool_result_not_run("turn_cancelled")));
}

static void
close_fixture_process(struct snag_session *session)
{
    commit_data(session, "process_closed", json_pack("{s:s,s:s,s:s,s:o}", "turn_id", GOAL_ID,
        "handle", session->processes[0].handle, "cause", "user_interrupt", "result",
        snag_tool_result_outcome_unknown("owner_lost")));
}

static void
reject_result_ranges(struct snag_session *source, struct snag_session *restored,
    int fd, struct snag_binary_import_result *imported, uint64_t sequence)
{
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor before;
    assert(!snag_binary_batch_find(fd, &imported->native.verified, sequence,
        &scratch, &batch, &before));
    /* This small fixture deliberately keeps the result and its range in one
     * batch: a committed batch boundary alone cannot prove result causality. */
    assert(before.end == SNAG_BINARY_HEADER_SIZE && before.next_seq == 1u);
    struct snag_binary_record *records = calloc(batch.count, sizeof(*records));
    assert(records);
    size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
    for (uint32_t i = 0u; i < batch.count; ++i) {
        uint64_t found;
        assert(!snag_binary_record_next(&batch, &offset, &records[i], &found));
        assert(found == i + 1u);
    }
    struct snag_binary_record *record = &records[sequence - 1u];
    struct snag_binary_event event;
    assert(!snag_binary_event_decode(record, &event));
    struct snag_binary_tool_output_ref *ref = event.kind == SNAG_BINARY_TOOL_FINISHED ?
        &event.data.tool_finished.result.output_ref : &event.data.process_closed.result.output_ref;
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    assert(snag_pread(fd, header, sizeof(header), 0) == (ssize_t)sizeof(header));
    json_t *old_state = checked_json(snag_checkpoint_state_encode(restored));
    struct snag_binary_checkpoint_sources old_sources = imported->sources;
    for (unsigned int literal = 0u; literal < 2u; ++literal) {
        if (literal) {
            ref->native = false;
            ref->first_sequence = ref->end_sequence = 0u;
        } else {
            ref->end_sequence = sequence + 1u; /* Includes its own result. */
        }
        struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
        struct snag_buf file = {.max = SNAG_BINARY_WIRE_BATCH_MAX + SNAG_BINARY_HEADER_SIZE};
        assert(!snag_binary_event_encode(&payload, &event));
        record->payload = payload.data;
        record->size = payload.len;
        assert(!snag_buf_append(&file, header, sizeof(header)));
        assert(!binary_fixture_append(&file, &before, records, batch.count,
            imported->native.verified.turns, NULL));
        int bad_fd = temporary_fd();
        assert(!snag_write_full(bad_fd, file.data, file.len));
        struct snag_session view = *source; /* Borrowed source identity/lock only. */
        view.log_fd = bad_fd;
        struct snag_binary_recovery recovery;
        char error[256];
        int rc = snag_store_reconcile_binary(&view, restored, NULL, NULL,
            &recovery, &imported->sources, error, sizeof(error));
        assert(rc < 0 && errno == (literal ? ENOTSUP : EINVAL));
        assert(recovery.problem_seq == sequence && recovery.verified.end == before.end);
        assert(strstr(error, "cannot project native record"));
        json_t *after = checked_json(snag_checkpoint_state_encode(restored));
        assert(json_equal(old_state, after));
        assert(!memcmp(&old_sources, &imported->sources, sizeof(old_sources)));
        json_decref(after);
        assert(!close(bad_fd));
        snag_buf_free(&file);
        snag_buf_free(&payload);
    }
    json_decref(old_state);
    free(records);
    snag_buf_free(&scratch);
}

static void
test_output_references(struct snag_store *store, const char *cwd, unsigned int mode)
{
    struct snag_session original, restored;
    snag_session_init(&original);
    snag_session_init(&restored);
    char error[512] = {0};
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    struct snag_response_graph graph = {0};
    assert(!snag_response_graph_add_call(&graph, "run", "run", "exec_command",
        json_pack("{s:s}", "command", "fixture only")));
    json_t *paths = checked_json(json_array());
    commit_data(&original, "turn_started",
        direct_turn_data(cwd, "Preserve collected output.", GOAL_ID, 1u, false, paths));
    json_decref(paths);
    commit_data(&original, "response_started", response_data());
    commit_data(&original, "response_completed", json_pack(
        "{s:s,s:s,s:i,s:s,s:s,s:O,s:{s:i,s:i,s:i,s:i}}", "turn_id", GOAL_ID,
        "response_id", OTHER_ID, "cycle", 1, "status", "completed",
        "provider_response_id", "fixture", "items", graph.items, "usage",
        "input_tokens", 17, "output_tokens", 7, "reasoning_tokens", 3, "total_tokens", 24));
    int64_t start = original.log_end;
    uint64_t first_sequence = original.next_seq;
    start_process_call(&original, 0u);
    char handle[SNAG_ID_HEX_LEN + 1u], call[SNAG_ID_HEX_LEN + 1u];
    memcpy(handle, original.processes[0].handle, sizeof(handle));
    memcpy(call, original.pending_calls[0].call_id, sizeof(call));
    if (mode != 1u) commit_data(&original, "process_output", json_pack("{s:s,s:s,s:i,s:i,s:s,s:s}",
        "turn_id", GOAL_ID, "handle", handle, "stream", 0, "offset", 0,
        "encoding", "utf8", "data", "abc"));
    for (unsigned int closing = 0u; closing < 2u; ++closing) {
        int bytes = mode == 1u ? 0 : 3;
        int64_t from = mode == 1u ? original.log_end : mode == 2u || mode == 3u ? 0 : start;
        int64_t to = mode == 2u ? 0 : original.log_end;
        uint64_t sequence = original.next_seq;
        bool bad = closing && mode >= 4u;
        if (bad && mode == 4u) from++;
        if (bad && mode == 5u) to--;
        if (bad && mode == 6u) to++;

        json_t *result = snag_tool_result_outcome_unknown("owner_lost");
        assert(result);
        assert(!snag_json_set_new(result, "max_output_tokens", json_integer(16000)));
        assert(!snag_json_set_new(result, "stdout", json_pack("{s:s,s:s,s:i,s:i,s:i}",
            "encoding", "utf8", "retained", bytes ? "abc" : "", "retained_bytes", bytes,
            "original_bytes", bytes, "discarded_bytes", 0)));
        assert(!snag_json_set_new(result, "output_ref", json_pack(
            "{s:s,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:b,s:I,s:I}", "handle", handle,
            "stdout_start", 0, "stdout_end", bytes, "stderr_start", 0, "stderr_end", 0,
            "stdin_accepted", 0, "stdin_written", 0, "stdin_pending", 0, "stdin_open", false,
            "log_start", (json_int_t)from, "log_end", (json_int_t)to)));
        json_t *data = json_pack("{s:s,s:s,s:O}", "turn_id", GOAL_ID,
            closing ? "handle" : "call_id", closing ? handle : call, "result", result);
        assert(data);
        if (closing) assert(!snag_json_set_new(data, "cause", json_string("user_interrupt")));
        commit_data(&original, closing ? "process_closed" : "tool_finished", data);
        int fd = temporary_fd();
        struct snag_binary_import_result imported = {0};
        json_t *before_state = bad ? checked_json(snag_checkpoint_state_encode(&restored)) : NULL;
        int64_t position = snag_seek(original.log_fd, 0, SEEK_CUR);
        int rc = snag_store_import_binary_journal(&original, fd, &restored, &imported,
            error, sizeof(error));
        assert(snag_seek(original.log_fd, 0, SEEK_CUR) == position);
        if (bad) {
            assert(rc < 0 && errno == EINVAL);
            json_t *after_state = checked_json(snag_checkpoint_state_encode(&restored));
            assert(json_equal(before_state, after_state));
            json_decref(before_state);
            json_decref(after_state);
        } else {
            if (rc) fprintf(stderr, "output reference import: %s\n", error);
            assert(!rc);
            test_store_binary_core_state(fd, &imported.native.verified,
                &imported.sources, &restored);
            struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
            struct snag_binary_batch batch;
            struct snag_binary_anchor before;
            assert(!snag_binary_batch_find(fd, &imported.native.verified, sequence,
                &scratch, &batch, &before));
            struct snag_binary_record record;
            uint64_t found;
            size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
            do { assert(!snag_binary_record_next(&batch, &offset, &record, &found)); }
            while (found != sequence);
            struct snag_binary_event event;
            assert(!snag_binary_event_decode(&record, &event));
            const struct snag_binary_tool_output_ref *ref = closing ?
                &event.data.process_closed.result.output_ref :
                &event.data.tool_finished.result.output_ref;
            assert(ref->native && ref->log_start == (uint64_t)from && ref->log_end == (uint64_t)to);
            assert(ref->first_sequence == (mode == 1u ? sequence : mode == 2u ? 0u :
                mode == 3u ? 1u : first_sequence));
            assert(ref->end_sequence == (mode == 2u ? 0u : sequence));
            const char *type;
            json_t *projected = NULL;
            assert(!snag_binary_legacy_decode(&record, &type, &projected));
            assert(json_equal(json_object_get(projected, "result"), result));
            json_decref(projected);
            snag_buf_free(&scratch);
            if (!mode) reject_result_ranges(&original, &restored, fd, &imported, sequence);
        }
        snag_binary_checkpoint_sources_free(&imported.sources);
        assert(!close(fd));
        json_decref(result);
    }
    snag_response_graph_free(&graph);
    snag_session_close(&restored);
    snag_session_close(&original);
}

static void
test_process_origins(struct snag_store *store, const char *cwd)
{
    char error[512] = {0};
    struct snag_session original, restored;
    snag_session_init(&original);
    snag_session_init(&restored);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    struct snag_response_graph graph = {0};
    assert(!snag_response_graph_add_call(&graph, "first", "first", "exec_command",
        json_pack("{s:s,s:s}", "command", "first command", "workdir", "./first")));
    assert(!snag_response_graph_add_call(&graph, "second", "second", "exec_command",
        json_pack("{s:s,s:s}", "command", "second command", "workdir", "./second")));
    json_t *paths = checked_json(json_array());
    commit_data(&original, "turn_started", direct_turn_data(cwd,
        "Retain process origins across responses.", GOAL_ID, 1u, false, paths));
    commit_data(&original, "response_started", response_data());
    json_t *data = checked_json(json_pack("{s:s,s:s,s:i,s:s,s:s,s:O,s:{s:i,s:i,s:i,s:i}}",
        "turn_id", GOAL_ID, "response_id", OTHER_ID, "cycle", 1, "status", "completed",
        "provider_response_id", "fixture", "items", graph.items, "usage",
        "input_tokens", 17, "output_tokens", 7, "reasoning_tokens", 3, "total_tokens", 24));
    uint64_t first_graph = commit_data(&original, "response_completed", data);
    int output = temporary_fd();
    struct snag_binary_import_result result = {0};
    char effective[65];
    for (size_t i = 0u; i < 3u; ++i) {
        const struct snag_pending_call *call = &original.pending_calls[i == 2u ? 1u : 0u];
        memset(effective, (int)('a' + i), 64u);
        effective[64] = 0;
        commit_data(&original, "rule_transform", json_pack("{s:s,s:s,s:s,s:s}",
            "rule", "fixture", "call_id", call->call_id, "original_sha256", call->action_sha256,
            "effective_sha256", effective));
    }
    assert(!import_checked(&original, output, &restored, &result));
    assert(!snag_truncate(output, 0));
    uint64_t starts[2];
    starts[0] = start_process_call(&original, 0u);
    starts[1] = start_process_call(&original, 1u);
    for (size_t i = 0u; i < 2u; ++i) {
        commit_data(&original, "process_output", json_pack("{s:s,s:s,s:i,s:i,s:s,s:s}",
            "turn_id", GOAL_ID, "handle", original.processes[i].handle, "stream", (int)i,
            "offset", 0, "encoding", "utf8", "data", "abc"));
    }
    assert(!import_checked(&original, output, &restored, &result));
    assert(result.sources.process_count == 2u);
    for (size_t i = 0u; i < 2u; ++i) {
        assert(result.sources.processes[i].started == starts[i]);
        assert(result.sources.processes[i].call.graph == first_graph);
        assert(result.sources.processes[i].call.cwd.declaration == 1u);
    }
    finish_process_call(&original, 0u, true);
    finish_process_call(&original, 1u, true);
    assert(!original.pending_call_count && original.process_count == 2u);
    assert(!snag_truncate(output, 0));
    assert(!import_checked(&original, output, &restored, &result));
    assert(!result.sources.calls.graph && result.sources.process_count == 2u);
    close_fixture_process(&original);
    assert(!snag_truncate(output, 0));
    assert(!import_checked(&original, output, &restored, &result));
    assert(result.sources.process_count == 1u && result.sources.processes[0].started == starts[1]);
    struct snag_binary_checkpoint_process_source old_source = result.sources.processes[0];
    uint64_t directory = commit_data(&original, "cwd_changed", json_pack("{s:s,s:s}",
        "old_cwd", original.cwd, "new_cwd", "/native-process-new-cwd"));
    data = response_data();
    assert(!json_object_set_new(data, "cycle", json_integer(2)));
    commit_data(&original, "response_started", data);
    data = checked_json(json_pack("{s:s,s:s,s:i,s:s,s:s,s:O,s:{s:i,s:i,s:i,s:i}}",
        "turn_id", GOAL_ID, "response_id", OTHER_ID, "cycle", 2, "status", "completed",
        "provider_response_id", "fixture", "items", graph.items, "usage",
        "input_tokens", 17, "output_tokens", 7, "reasoning_tokens", 3, "total_tokens", 24));
    uint64_t second_graph = commit_data(&original, "response_completed", data);
    uint64_t restarted = start_process_call(&original, 0u);
    finish_process_call(&original, 0u, true);
    finish_process_call(&original, 1u, false);
    assert(!snag_truncate(output, 0));
    assert(!import_checked(&original, output, &restored, &result));
    assert(result.sources.process_count == 2u);
    assert(old_source.started == result.sources.processes[0].started);
    assert(!strcmp(old_source.handle, result.sources.processes[0].handle));
    assert(old_source.call.graph == result.sources.processes[0].call.graph);
    assert(old_source.call.cwd.declaration == result.sources.processes[0].call.cwd.declaration);
    assert(result.sources.processes[1].started == restarted);
    assert(result.sources.processes[1].call.graph == second_graph);
    assert(result.sources.processes[1].call.cwd.declaration == directory);
    struct snag_binary_checkpoint_process_source bad = result.sources.processes[1];
    bad.call = old_source.call; /* Reused call ID, but an obsolete response lifetime. */
    struct snag_process_state canary, saved;
    memset(&canary, 0xa5, sizeof(canary));
    saved = canary;
    assert(snag_binary_checkpoint_process_source_read(output, &result.native.verified, NULL,
        &bad, &restored, &canary) < 0);
    assert(!memcmp(&canary, &saved, sizeof(canary)));
    snag_response_graph_free(&graph);
    assert(!snag_response_graph_add_call(&graph, "poll", "poll", "write_stdin",
        json_pack("{s:s}", "handle", original.processes[0].handle)));
    data = response_data();
    assert(!json_object_set_new(data, "cycle", json_integer(3)));
    commit_data(&original, "response_started", data);
    data = checked_json(json_pack("{s:s,s:s,s:i,s:s,s:s,s:O,s:{s:i,s:i,s:i,s:i}}",
        "turn_id", GOAL_ID, "response_id", OTHER_ID, "cycle", 3, "status", "completed",
        "provider_response_id", "fixture", "items", graph.items, "usage",
        "input_tokens", 17, "output_tokens", 7, "reasoning_tokens", 3, "total_tokens", 24));
    commit_data(&original, "response_completed", data);
    start_process_call(&original, 0u);
    assert(!snag_truncate(output, 0));
    assert(!import_checked(&original, output, &restored, &result));
    assert(result.sources.process_count == 2u);
    assert(old_source.started == result.sources.processes[0].started);
    assert(!strcmp(old_source.handle, result.sources.processes[0].handle));
    assert(old_source.call.graph == result.sources.processes[0].call.graph);
    assert(old_source.call.cwd.declaration == result.sources.processes[0].call.cwd.declaration);
    finish_process_call(&original, 0u, true);
    close_fixture_process(&original);
    assert(!snag_truncate(output, 0));
    assert(!import_checked(&original, output, &restored, &result));
    assert(result.sources.process_count == 1u && result.sources.processes[0].started == restarted);
    close_fixture_process(&original);
    commit_data(&original, "turn_interrupted", json_pack("{s:s,s:s,s:s}",
        "turn_id", GOAL_ID, "origin", "user", "reason", "cancelled"));
    assert(!snag_truncate(output, 0));
    assert(!import_checked(&original, output, &restored, &result));
    assert(!result.sources.process_count && !result.sources.processes);
    snag_binary_checkpoint_sources_free(&result.sources);
    assert(!close(output));
    json_decref(paths);
    snag_response_graph_free(&graph);
    snag_session_close(&restored);
    snag_session_close(&original);
}

/* Build an admitted format2 fixture directly from strict reducer transitions;
 * no process is dispatched and no operator journal is converted. */
static void
legacy_process_step(struct replay_fixture *fixture, struct snag_session *state,
    const char *type, json_t *data)
{
    char error[512] = {0};
    uint64_t sequence = state->next_seq++;
    ++state->last_time_ms;
    assert(data);
    int rc = snag_store_reduce_event(state, type, data, sequence, error, sizeof(error));
    if (rc < 0) fprintf(stderr, "legacy process fixture %s: %s\n", type, error);
    assert(!rc);
    assert(!fixture_event(fixture, state, sequence, type, data, error, sizeof(error)));
    json_decref(data);
}

static void
test_legacy_process_origin(struct snag_store *store, const char *cwd)
{
    char error[512] = {0};
    struct snag_session source, expected, restored;
    snag_session_init(&source);
    snag_session_init(&expected);
    snag_session_init(&restored);
    assert(!legacy_fixture_create(store, &source, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    memcpy(expected.id, source.id, sizeof(expected.id));
    expected.last_time_ms = source.last_time_ms;
    struct replay_fixture fixture = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX}};
    for (size_t i = 0u; i < 3u; ++i) fixture.payload[i].max = SNAG_MAX_EVENT_LINE;
    legacy_process_step(&fixture, &expected, "session_created", json_pack(
        "{s:s,s:s,s:s,s:i,s:s,s:s}", "default_effort", "default", "default_model", "fixture",
        "default_provider", "default", "format", 2, "protocol", "responses", "workspace", cwd));
    json_t *paths = checked_json(json_array());
    legacy_process_step(&fixture, &expected, "turn_started", direct_turn_data(cwd,
        "Retain a workspace-era process origin.", GOAL_ID, 1u, false, paths));
    legacy_process_step(&fixture, &expected, "response_started", response_data());
    struct snag_response_graph graph = {0};
    assert(!snag_response_graph_add_call(&graph, "legacy", "legacy", "write_stdin",
        json_pack("{s:s}", "handle", OTHER_ID)));
    legacy_process_step(&fixture, &expected, "response_completed", json_pack(
        "{s:s,s:s,s:i,s:s,s:s,s:O,s:{s:i,s:i,s:i,s:i}}", "turn_id", GOAL_ID,
        "response_id", OTHER_ID, "cycle", 1, "status", "completed",
        "provider_response_id", "fixture", "items", graph.items, "usage",
        "input_tokens", 17, "output_tokens", 7, "reasoning_tokens", 3, "total_tokens", 24));
    char effective[65];
    memset(effective, 'd', 64u);
    effective[64] = 0;
    legacy_process_step(&fixture, &expected, "rule_transform", json_pack("{s:s,s:s,s:s,s:s}",
        "rule", "legacy fixture", "call_id", expected.pending_calls[0].call_id, "original_sha256",
        expected.pending_calls[0].action_sha256, "effective_sha256", effective));
    legacy_process_step(&fixture, &expected, "tool_started", json_pack("{s:s,s:s,s:s,s:s}",
        "turn_id", GOAL_ID, "call_id", expected.pending_calls[0].call_id, "action_sha256",
        expected.pending_calls[0].action_sha256, "resolved_workdir", expected.cwd));
    legacy_process_step(&fixture, &expected, "tool_finished", json_pack("{s:s,s:s,s:o}",
        "turn_id", GOAL_ID, "call_id", expected.pending_calls[0].call_id, "result",
        snag_tool_result_outcome_unknown("owner_lost")));
    fixture_flush(&fixture);
    struct snag_binary_recovery recovery;
    assert(!replay_check(&source, &restored, &recovery, fixture.file.data, fixture.file.len));
    assert(restored.legacy_journal && restored.process_count == 1u && !restored.pending_call_count);
    same_core_state(&expected, &restored);
    assert(!strcmp(restored.processes[0].handle, OTHER_ID));
    assert(!*restored.processes[0].command && !*restored.processes[0].workdir);
    struct snag_binary_checkpoint_sources origins = {0};
    assert(!snag_store_reconcile_binary(&source, &restored, NULL, NULL, &recovery, &origins,
        error, sizeof(error)));
    assert(origins.process_count == 1u && origins.processes[0].started == 6u);
    assert(origins.processes[0].call.graph == 4u && !origins.calls.graph);
    struct snag_session wrong = restored;
    wrong.legacy_journal = false;
    struct snag_process_state process, saved;
    memset(&process, 0xa5, sizeof(process));
    saved = process;
    assert(snag_binary_checkpoint_process_source_read(source.log_fd, &recovery.verified, NULL,
        &origins.processes[0], &wrong, &process) < 0);
    assert(!memcmp(&process, &saved, sizeof(process)));
    snag_binary_checkpoint_sources_free(&origins);
    snag_response_graph_free(&graph);
    json_decref(paths);
    json_decref(fixture.creation);
    for (size_t i = 0u; i < 3u; ++i) snag_buf_free(&fixture.payload[i]);
    snag_buf_free(&fixture.file);
    snag_session_close(&restored);
    snag_session_close(&expected);
    snag_session_close(&source);
}

static void
test_call_origins(struct snag_store *store, const char *cwd)
{
    char error[512] = {0};
    struct snag_session original, restored;
    snag_session_init(&original);
    snag_session_init(&restored);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    struct snag_response_graph graph = {0};
    assert(!snag_response_graph_add_public(&graph, SNAG_ITEM_ASSISTANT, SNAG_PHASE_COMMENTARY,
        "commentary", "These calls are fixture records; no command is executed."));
    const char *names[] = {"exec_command", "write_stdin", "get_cwd", "read_file"};
    char command[300];
    memset(command, 'x', 255u);
    memcpy(command + 255u, "\xc3\xa9 retained original", 21u);
    for (size_t i = 0u; i < 4u; ++i) {
        json_t *args = i == 0u ? json_pack("{s:s,s:s}", "command", command, "workdir", "./sub") :
            i == 1u ? json_pack("{s:s}", "handle", OTHER_ID) : json_object();
        assert(args);
        assert(!snag_response_graph_add_call(&graph, names[i], names[i], names[i], args));
    }
    json_t *paths = checked_json(json_array());
    uint64_t directory = 1u;
    for (unsigned int lifetime = 0u; lifetime < 2u; ++lifetime) {
        commit_data(&original, "turn_started", direct_turn_data(original.cwd,
            "Retain pending call origins.", GOAL_ID, lifetime + 1u, false, paths));
        commit_data(&original, "response_started", response_data());
        json_t *data = checked_json(json_pack("{s:s,s:s,s:i,s:s,s:s,s:O,s:{s:i,s:i,s:i,s:i}}",
            "turn_id", GOAL_ID, "response_id", OTHER_ID, "cycle", 1, "status", "completed",
            "provider_response_id", "fixture", "items", graph.items, "usage",
            "input_tokens", 17, "output_tokens", 7, "reasoning_tokens", 3, "total_tokens", 24));
        uint64_t completed = commit_data(&original, "response_completed", data);
        assert(original.pending_call_count == 4u);
        int output = temporary_fd();
        struct snag_binary_import_result result = {0};
        assert(!import_checked(&original, output, &restored, &result));
        assert(result.sources.calls.graph == completed);
        assert(result.sources.calls.cwd.declaration == directory);
        assert(!close(output));
        const char *changed = lifetime ? "/native-call-cwd-second" : "/native-call-cwd-first";
        directory = commit_data(&original, "cwd_changed", json_pack("{s:s,s:s}",
            "old_cwd", original.cwd, "new_cwd", changed));
        for (size_t i = 2u; i < 4u; ++i) {
            const struct snag_pending_call *call = &original.pending_calls[i];
            commit_data(&original, "tool_started", json_pack("{s:s,s:s,s:s,s:s}",
                "turn_id", GOAL_ID, "call_id", call->call_id,
                "action_sha256", call->action_sha256, "resolved_workdir", original.cwd));
        }
        commit_data(&original, "tool_finished", json_pack("{s:s,s:s,s:o}",
            "turn_id", GOAL_ID, "call_id", original.pending_calls[2].call_id,
            "result", snag_tool_result_terminal(true, "fixture result")));
        commit_data(&original, "tool_finished", json_pack("{s:s,s:s,s:o}",
            "turn_id", GOAL_ID, "call_id", original.pending_calls[1].call_id,
            "result", snag_tool_result_not_run("turn_cancelled")));
        assert(!snag_session_checkpoint(&original, error, sizeof(error)));
        output = temporary_fd();
        assert(!import_checked(&original, output, &restored, &result));
        assert(result.sources.calls.graph == completed);
        assert(result.sources.calls.cwd.declaration < completed);
        assert(result.sources.texts.slots[SNAG_BINARY_TEXT_CWD].declaration == directory);
        assert(!close(output));
        commit_data(&original, "tool_finished", json_pack("{s:s,s:s,s:o}",
            "turn_id", GOAL_ID, "call_id", original.pending_calls[3].call_id,
            "result", snag_tool_result_terminal(true, "fixture result")));
        commit_data(&original, "tool_finished", json_pack("{s:s,s:s,s:o}",
            "turn_id", GOAL_ID, "call_id", original.pending_calls[0].call_id,
            "result", snag_tool_result_not_run("turn_cancelled")));
        assert(!original.pending_call_count);
        output = temporary_fd();
        assert(!import_checked(&original, output, &restored, &result));
        assert(!result.sources.calls.graph);
        snag_binary_checkpoint_sources_free(&result.sources);
        assert(!close(output));
        commit_data(&original, "turn_interrupted", json_pack("{s:s,s:s,s:s}",
            "turn_id", GOAL_ID, "origin", "user", "reason", "cancelled"));
    }
    json_decref(paths);
    snag_response_graph_free(&graph);
    snag_session_close(&restored);
    snag_session_close(&original);
}

static void
test_text_origins(struct snag_store *store, const char *cwd)
{
    char error[512];
    struct snag_session original, restored;
    snag_session_init(&original);
    snag_session_init(&restored);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    commit_data(&original, "banner_updated", json_pack("{s:s}", "text", "same"));
    uint64_t banner = commit_data(&original, "banner_updated", json_pack("{s:s}", "text", "same"));
    uint64_t steering = commit_data(&original, "steering_updated",
        json_pack("{s:s}", "mode", "mentions"));
    uint64_t timer = commit_data(&original, "timer_scheduled", json_pack("{s:s,s:I,s:s}",
        "timer_id", OTHER_ID, "due_ms", (json_int_t)(original.last_time_ms + 60000u),
        "text", "same"));
    json_t *paths = json_array();
    uint64_t turn = commit_data(&original, "turn_started", direct_turn_data(cwd, "same",
        "dddddddddddddddddddddddddddddddd", 1u, false, paths));
    json_decref(paths);
    uint64_t goal = commit_data(&original, "goal_started", json_pack("{s:s,s:s}",
        "goal_id", GOAL_ID, "prompt", "same"));
    uint64_t reword = commit_data(&original, "goal_reworded", json_pack("{s:s,s:s,s:s}",
        "goal_id", GOAL_ID, "actor", "model", "prompt", "model wording"));
    int fd = temporary_fd();
    struct snag_binary_import_result result = {0};
    assert(!import_checked(&original, fd, &restored, &result));
    assert(result.sources.texts.through == reword);
    assert(result.sources.texts.slots[SNAG_BINARY_TEXT_FIRST_USER].declaration == turn);
    assert(result.sources.texts.slots[SNAG_BINARY_TEXT_LAST_USER].declaration == goal);
    assert(result.sources.texts.slots[SNAG_BINARY_TEXT_ACTIVE_PROMPT].declaration == turn);
    assert(result.sources.texts.slots[SNAG_BINARY_TEXT_GOAL_PROMPT].declaration == reword);
    assert(result.sources.texts.slots[SNAG_BINARY_TEXT_BANNER].declaration == banner);
    assert(result.sources.texts.slots[SNAG_BINARY_TEXT_STEERING].declaration == steering);
    assert(result.sources.texts.slots[SNAG_BINARY_TEXT_TIMER].declaration == timer);
    test_store_binary_texts_bad(fd, &result.native.verified, &result.sources.texts);
    assert(!close(fd));
    uint64_t blocker = commit_data(&original, "goal_blocked", json_pack("{s:s,s:s,s:s}",
        "goal_id", GOAL_ID, "actor", "model", "reason", "retained blocker"));
    commit_data(&original, "goal_replaced", json_pack("{s:s,s:s,s:s,s:s}",
        "goal_id", GOAL_ID, "new_goal_id", OTHER_ID, "actor", "user", "prompt", "replacement one"));
    const char *third = "cccccccccccccccccccccccccccccccc";
    uint64_t replace = commit_data(&original, "goal_replaced", json_pack("{s:s,s:s,s:s,s:s}",
        "goal_id", OTHER_ID, "new_goal_id", third, "actor", "user", "prompt", "replacement two"));
    assert(!snag_session_checkpoint(&original, error, sizeof(error)));
    fd = temporary_fd();
    assert(!import_checked(&original, fd, &restored, &result));
    assert(result.sources.texts.through == replace &&
        result.native.verified.next_seq == replace + 2u);
    assert(result.sources.texts.slots[SNAG_BINARY_TEXT_GOAL_BLOCKER].declaration == blocker);
    assert(result.sources.texts.slots[SNAG_BINARY_TEXT_GOAL_PROMPT].declaration == replace);
    assert(result.sources.texts.slots[SNAG_BINARY_TEXT_LAST_USER].declaration == replace);
    assert(result.sources.texts.slots[SNAG_BINARY_TEXT_FIRST_USER].declaration == turn);
    assert(!strcmp(restored.goal_blocker, "retained blocker") &&
        !strcmp(restored.goal_id, third) && restored.goal_status == SNAG_GOAL_BLOCKED);
    assert(!close(fd));
    commit_data(&original, "goal_resumed", json_pack("{s:s}", "goal_id", third));
    reword = commit_data(&original, "goal_reworded", json_pack("{s:s,s:s,s:s}",
        "goal_id", third, "actor", "model", "prompt", "later model wording"));
    commit_data(&original, "goal_cancelled", json_pack("{s:s}", "goal_id", third));
    commit_data(&original, "timer_cancelled", json_pack("{s:s}", "timer_id", OTHER_ID));
    commit_data(&original, "banner_updated", json_pack("{s:s}", "text", ""));
    uint64_t last = commit_data(&original, "steering_updated", json_pack("{s:s}", "mode", ""));
    fd = temporary_fd();
    assert(!import_checked(&original, fd, &restored, &result));
    assert(result.sources.texts.through == last);
    assert(!result.sources.texts.slots[SNAG_BINARY_TEXT_GOAL_BLOCKER].declaration);
    assert(!result.sources.texts.slots[SNAG_BINARY_TEXT_TIMER].declaration);
    assert(!result.sources.texts.slots[SNAG_BINARY_TEXT_BANNER].declaration);
    assert(!result.sources.texts.slots[SNAG_BINARY_TEXT_STEERING].declaration);
    assert(result.sources.texts.slots[SNAG_BINARY_TEXT_GOAL_PROMPT].declaration == reword);
    assert(result.sources.texts.slots[SNAG_BINARY_TEXT_LAST_USER].declaration == replace);
    assert(result.sources.texts.slots[SNAG_BINARY_TEXT_ACTIVE_PROMPT].declaration == turn);
    assert(!close(fd));
    snag_binary_checkpoint_sources_free(&result.sources);
    snag_session_close(&restored);
    snag_session_close(&original);
}

static void
test_metadata_origins(struct snag_store *store, const char *cwd)
{
    char error[512];
    struct snag_session original, restored;
    snag_session_init(&original);
    snag_session_init(&restored);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    struct snag_binary_import_result result = {0};
    uint64_t name_sequence = 0u, options_sequence = 0u;
    for (unsigned int phase = 0u; phase < 4u; ++phase) {
        if (phase) {
            name_sequence = commit_data(&original, "session_named", json_pack("{s:s}", "name",
                phase == 3u ? "renamed é" : "same"));
            options_sequence = commit_data(&original, "session_options", phase == 2u ?
                json_pack("{s:[]}", "args") :
                json_pack("{s:[s,s,s,s]}", "args", "--config", "--markdown", "-v", "-v"));
        }
        int fd = temporary_fd();
        assert(!import_checked(&original, fd, &restored, &result));
        assert(!!original.name == !!restored.name);
        if (original.name) assert(!strcmp(original.name, restored.name));
        json_t *before = json_object_get(original.strings, "resume_options");
        json_t *after = json_object_get(restored.strings, "resume_options");
        assert(!!before == !!after && (!before || json_equal(before, after)));
        assert(result.sources.texts.slots[SNAG_BINARY_TEXT_NAME].declaration == name_sequence);
        assert(result.sources.resume_options == options_sequence);
        test_store_binary_texts_bad(fd, &result.native.verified, &result.sources.texts);
        test_store_binary_core_state(fd, &result.native.verified, &result.sources, &restored);
        assert(!close(fd));
    }
    snag_binary_checkpoint_sources_free(&result.sources);
    snag_session_close(&restored);
    snag_session_close(&original);
}

static void
test_voice_adoption(struct snag_store *store, const char *cwd, unsigned int bad)
{
    struct snag_session original, restored;
    snag_session_init(&original);
    snag_session_init(&restored);
    char error[512] = {0};
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    commit_data(&original, "banner_updated", json_pack("{s:s}", "text", "destination"));
    for (unsigned int phase = 0u; phase < 2u; ++phase) {
        const char *id = phase ? GOAL_ID : OTHER_ID;
        struct snag_journal_cursor begin = {.offset = original.log_end,
            .next_seq = original.next_seq};
        memcpy(begin.prev_sha256, original.prev_sha256, sizeof(begin.prev_sha256));
        commit_data(&original, "voice_transfer_record",
            json_pack("{s:s,s:s,s:s,s:i,s:s,s:{s:s,s:s}}", "transfer_id", id,
                "target_session_id", original.id, "source_session_id", OTHER_ID,
                "source_seq", 1, "source_type", "goal_started", "data",
                "goal_id", OTHER_ID, "prompt", "inert archived goal"));
        commit_data(&original, "banner_updated",
            json_pack("{s:s}", "text", "destination continues"));
        assert(!snag_session_checkpoint(&original, error, sizeof(error)));
        if (phase) {
            if (bad == 1u) ++begin.offset;
            if (bad == 2u) begin.prev_sha256[0] = begin.prev_sha256[0] == '0' ? '1' : '0';
            if (bad == 3u) ++begin.next_seq;
        }
        commit_data(&original, "voice_transfer_adopted",
            json_pack("{s:s,s:s,s:s,s:i,s:I,s:I,s:s,s:i}", "transfer_id", id,
                "target_session_id", original.id, "source_session_id", OTHER_ID,
                "source_as_of_seq", 1, "begin_offset", (json_int_t)begin.offset,
                "begin_seq", (json_int_t)begin.next_seq, "begin_sha256", begin.prev_sha256,
                "count", 1));
        int fd = temporary_fd();
        int64_t position = snag_seek(original.log_fd, 0, SEEK_CUR);
        struct snag_binary_import_result result;
        struct snag_session saved = restored, saved_source = original;
        json_t *old = snag_checkpoint_state_encode(&restored);
        assert(old);
        int rc = snag_store_import_binary_journal(&original, fd, &restored, &result,
            error, sizeof(error));
        assert(!memcmp(&saved_source, &original, sizeof(original)));
        assert(snag_seek(original.log_fd, 0, SEEK_CUR) == position);
        if (bad && phase) {
            assert(rc < 0 && errno == EINVAL && *error);
            assert(!memcmp(&saved, &restored, sizeof(restored)));
            json_t *unchanged = snag_checkpoint_state_encode(&restored);
            assert(unchanged && json_equal(old, unchanged));
            json_decref(unchanged);
        } else {
            if (rc) fprintf(stderr, "voice adoption import: %s\n", error);
            assert(!rc);
            assert(restored.voice_history.adopted_seq == original.voice_history.adopted_seq);
            assert(restored.voice_history.begin.next_seq == begin.next_seq);
            assert(!strcmp(restored.voice_history.transfer_id, id));
            assert(restored.voice_history.begin.offset == SNAG_BINARY_HEADER_SIZE);
            assert(strcmp(restored.voice_history.begin.prev_sha256, begin.prev_sha256));
            assert(!restored.goal_prompt && !restored.active_turn);
            assert(!strcmp(restored.banner_text, "destination continues"));
            struct snag_session source;
            snag_session_init(&source);
            memcpy(source.id, original.id, sizeof(source.id));
            source.log_fd = fd;
            source.lock_fd = original.lock_fd;
            prefix_matches(&source, &restored, &result.native.verified);
            struct snag_voice_history_root root = restored.voice_history, previous = root;
            assert(snag_binary_checkpoint_voice_read(fd, &result.native.verified, NULL,
                root.adopted_seq, OTHER_ID, &root) < 0);
            assert(!memcmp(&root, &previous, sizeof(root)));
        }
        json_decref(old);
        snag_binary_checkpoint_sources_free(&result.sources);
        assert(!close(fd));
    }
    snag_session_close(&restored);
    snag_session_close(&original);
}

struct factory_observer {
    struct snag_store *store;
    struct snag_session *owner;
    unsigned int freed;
    bool published;
    bool drop_parent;
};

static void
factory_observer_free(void *opaque)
{
    struct factory_observer *observer = opaque;
    ++observer->freed;
    if (!observer->published) return;
    struct snag_session *owner = observer->owner;
    assert(owner->binary && !owner->pending_log && owner->on_checkpoint);
    int fd = snag_open_read_security_at(observer->store->sessions_fd, owner->id, true);
    snag_file_info expected;
    snag_file_info actual;
    assert(fd >= 0 && !snag_fstat(owner->dir_fd, &expected) && !snag_fstat(fd, &actual));
    assert(expected.st_dev == actual.st_dev && expected.st_ino == actual.st_ino);
    assert(!close(fd));
    if (observer->drop_parent) {
        assert(!close(observer->store->sessions_fd));
        observer->store->sessions_fd = -1;
    }
}

static void
test_native_factory(const char *cwd)
{
    enum {FRESH, EMPTY_COLLISION, FULL_COLLISION, FILE_COLLISION,
        BAD_PREFIX, PARENT_BUSY, PARENT_SYNC, SYMLINK_COLLISION, PENDING_TAIL,
        FRESH_INPUT, FRESH_LARGE, CASE_COUNT};
    for (unsigned int variant = 0u; variant < CASE_COUNT; ++variant) {
#ifdef _WIN32
        if (variant == PARENT_BUSY || variant == SYMLINK_COLLISION) continue;
#endif
        const char *tmp = getenv("TMPDIR");
        if (!tmp || !*tmp) tmp = "/tmp";
        char *path = snag_path_join(tmp, "snag-native-factory-XXXXXX");
        assert(path && mkdtemp(path));
        struct snag_store store;
        snag_store_init(&store);
        char error[512];
        assert(!snag_store_open(&store, path, error, sizeof(error)));
        struct snag_session prepared;
        snag_session_init(&prepared);
        assert(!snag_session_prepare(&prepared, cwd, "openai", "factory", "medium",
            error, sizeof(error)));
        bool fresh = variant == FRESH || variant == FRESH_INPUT || variant == FRESH_LARGE;
        if (variant == FRESH_INPUT || variant == FRESH_LARGE) {
            json_t *instructions = json_array();
            assert(instructions);
            size_t length = variant == FRESH_LARGE ? SNAG_BINARY_BATCH_TARGET : 24u;
            char *text = malloc(length + 1u);
            assert(text);
            memset(text, 'q', length);
            text[length] = '\0';
            commit_data(&prepared, "input_received", input_data(text, false, instructions));
            free(text);
            json_decref(instructions);
        }
        json_t *retry = json_pack("{s:s}", "value", "on");
        assert(retry);
        (void)commit_data(&prepared, "retry_auto_changed", retry);
        struct factory_observer observer = {.store = &store, .owner = &prepared,
            .published = fresh || variant == PARENT_SYNC,
            .drop_parent = variant == PARENT_SYNC};
        prepared.on_commit_free = factory_observer_free;
        prepared.on_commit_opaque = &observer;
        char id[SNAG_ID_HEX_LEN + 1u];
        memcpy(id, prepared.id, sizeof(id));
        snag_file_info before_name = {0};
        int other = -1;
        struct snag_directory_lock held = {.fd = -1};
        if (variant == EMPTY_COLLISION || variant == FULL_COLLISION) {
            assert(!snag_mkdir_private_at(store.sessions_fd, id));
            if (variant == FULL_COLLISION) {
                int dir = snag_open_read_security_at(store.sessions_fd, id, true);
                assert(dir >= 0);
                int file = snag_create_private_at(dir, "foreign", true);
                assert(file >= 0 && !snag_write_full(file, "keep", 4u));
                assert(!close(file) && !close(dir));
            }
            assert(!snag_lstat_at(store.sessions_fd, id, &before_name));
        } else if (variant == FILE_COLLISION) {
            int file = snag_create_private_at(store.sessions_fd, id, true);
            assert(file >= 0 && !snag_write_full(file, "keep", 4u) && !close(file));
            assert(!snag_lstat_at(store.sessions_fd, id, &before_name));
        } else if (variant == BAD_PREFIX) {
            prepared.pending_log->data[0] ^= 1u;
        } else if (variant == PENDING_TAIL) {
            assert(!snag_buf_append(prepared.pending_log, "not-json\n", 9u));
#ifndef _WIN32
        } else if (variant == PARENT_BUSY) {
            char *parent = snag_path_join(path, "sessions");
            assert(parent);
            other = snag_open_read_security_at(AT_FDCWD, parent, true);
            free(parent);
            assert(other >= 0 && !snag_directory_lock_acquire(other, &held));
        } else if (variant == SYMLINK_COLLISION) {
            char *parent = snag_path_join(path, "sessions");
            char *name = parent ? snag_path_join(parent, id) : NULL;
            assert(name && !symlink("foreign-target", name));
            free(parent);
            free(name);
            assert(!snag_lstat_at(store.sessions_fd, id, &before_name));
#endif
        }
        struct snag_session original = prepared;
        struct snag_buf pending = {.max = SIZE_MAX};
        assert(!snag_buf_append(&pending, prepared.pending_log->data, prepared.pending_log->len));
        int persisted = fresh ?
            snag_session_persist(&store, &prepared, error, sizeof(error)) :
            snag_store_persist_binary_session(&store, &prepared, error, sizeof(error));
        if (observer.published) {
            if (fresh && persisted < 0) fprintf(stderr, "native factory: %s\n", error);
            assert(fresh ? !persisted : persisted < 0);
            if (variant == PARENT_SYNC) {
                assert(errno == EBADF && strstr(error, "published") && strstr(error, "uncertain"));
            }
            assert(prepared.binary && !prepared.pending_log && !strcmp(prepared.id, id));
            assert(observer.freed == 1u && prepared.on_commit && prepared.on_checkpoint);
            assert(!strcmp(prepared.retry_auto, "on"));
            const char *files[] = {"journal.bin", "checkpoint.0", "checkpoint.1",
                "history.idx", "lock"};
            const char *unused = NULL;
            for (size_t i = 0u; i < sizeof(files) / sizeof(files[0]); ++i) {
                snag_file_info st;
                assert(!snag_lstat_at(prepared.dir_fd, files[i], &st) && S_ISREG(st.st_mode));
                if ((i == 1u || i == 2u) && !st.st_size) unused = files[i];
            }
            assert(unused);
            snag_file_info st;
            assert(snag_lstat_at(prepared.dir_fd, "events.jsonl", &st) < 0 && errno == ENOENT);
            if (fresh) {
                int slot = snag_open_private_append_at(prepared.dir_fd, unused, false);
                assert(slot >= 0 && !snag_write_full(slot, "x", 1u));
                snag_session_close(&prepared);
                snag_session_init(&prepared);
                assert(snag_session_open(&store, &prepared, id, error, sizeof(error)) < 0);
                assert(!snag_fstat(slot, &st) && st.st_size == 1);
                assert(!snag_truncate(slot, 0) && !close(slot));
                int opened = snag_session_open(&store, &prepared, id, error, sizeof(error));
                if (opened < 0) fprintf(stderr, "native factory recovery: %s\n", error);
                assert(!opened && prepared.binary && !strcmp(prepared.default_model, "factory"));
                assert(!strcmp(prepared.retry_auto, "on"));
                if (variant == FRESH_INPUT || variant == FRESH_LARGE) {
                    const char *text = snag_json_string(prepared.pending_input, "text");
                    assert(text && strlen(text) == (variant == FRESH_LARGE ?
                        SNAG_BINARY_BATCH_TARGET : 24u));
                    for (size_t n = 0u; text[n]; ++n) assert(text[n] == 'q');
                }
            }
            retry = json_pack("{s:s}", "value", "off");
            assert(retry);
            (void)commit_data(&prepared, "retry_auto_changed", retry);
            assert(!snag_session_checkpoint(&prepared, error, sizeof(error)));
            assert(!snag_session_binary_index_status(&prepared, error, sizeof(error)));
            assert(!strcmp(prepared.retry_auto, "off") && observer.freed == 1u);
        } else {
            assert(persisted < 0 && !observer.freed);
            assert(!memcmp(&prepared, &original, sizeof(prepared)));
            assert(prepared.pending_log->len == pending.len &&
                !memcmp(prepared.pending_log->data, pending.data, pending.len));
            if (before_name.st_ino) {
                snag_file_info after;
                assert(!snag_lstat_at(store.sessions_fd, id, &after));
                assert(before_name.st_dev == after.st_dev && before_name.st_ino == after.st_ino &&
                    before_name.st_mode == after.st_mode && before_name.st_size == after.st_size);
            } else {
                snag_file_info absent;
                assert(snag_lstat_at(store.sessions_fd, id, &absent) < 0 && errno == ENOENT);
            }
            if (variant == BAD_PREFIX) assert(strstr(error, "provisional native session"));
        }
        if (held.fd >= 0) assert(!snag_directory_lock_release(&held));
        if (other >= 0) assert(!close(other));
        snag_session_close(&prepared);
        assert(observer.freed == 1u);
        if (fresh) {
            snag_session_init(&prepared);
            assert(!snag_session_open(&store, &prepared, id, error, sizeof(error)));
            assert(prepared.binary && !strcmp(prepared.retry_auto, "off"));
            snag_session_close(&prepared);
        }
        snag_store_close(&store);
        snag_buf_free(&pending);
        free(path);
    }
}

static bool
seed_access_cancel(void *opaque)
{
    (void)opaque;
    return true;
}

static void
seed_index_checks(struct snag_session *target, const struct snag_buf *index,
    const struct snag_binary_anchor *boundary, const struct snag_binary_index_tree *tree,
    const struct snag_binary_checkpoint_sources *sources)
{
    unsigned char header[SNAG_BINARY_HEADER_SIZE], root[32];
    struct snag_binary_identity identity;
    struct snag_binary_anchor initial;
    assert(snag_pread(target->log_fd, header, sizeof(header), 0) == sizeof(header));
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &initial));
    int fd = temporary_fd();
    assert(!snag_write_full(fd, index->data, index->len));
    assert(!snag_binary_index_header_read(fd, &identity));
    assert(!snag_binary_index_tree_root(tree, root));
    int64_t end;
    assert(!snag_binary_index_end(tree->count, &end) && (uint64_t)end == index->len);
    struct snag_binary_index_tree loaded;
    assert(!snag_binary_index_tree_load(fd, &identity, tree->count, root, &loaded));
    assert(!memcmp(tree, &loaded, sizeof(loaded)));
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    for (uint64_t sequence = 1u; sequence < boundary->next_seq; ++sequence) {
        struct snag_binary_index_entry entry;
        struct snag_binary_record record;
        assert(!snag_binary_index_read_verified(fd, &identity, tree->count, root,
            sequence, &entry));
        assert(!snag_binary_index_load_record(target->log_fd, boundary, &entry, &scratch, &record));
    }
    struct snag_buf empty = {.max = SIZE_MAX}, provider = {.max = SIZE_MAX};
    struct snag_buf selected = {.max = SIZE_MAX};
    struct snag_binary_checkpoint_index available;
    assert(!snag_binary_checkpoint_index_encode(&empty, &identity, boundary, tree, NULL, 0u));
    assert(!snag_binary_checkpoint_index_decode(empty.data, empty.len, &identity,
        boundary, root, &available));
    const json_t *recent = NULL, *history = NULL;
    assert(!snag_context_capture_seam(target, &recent, &history));
    assert(!snag_binary_checkpoint_provider_encode(&provider, target, recent, history));
    struct snag_binary_checkpoint_access_plan plan = {0};
    assert(!snag_binary_checkpoint_access_plan_build(&plan, boundary, sources, target,
        provider.data, provider.len, NULL, NULL));
    assert(!snag_binary_checkpoint_access_plan_read(target->log_fd, fd, &plan, &available,
        tree, NULL, NULL, &selected));
    struct snag_binary_checkpoint_index captured;
    assert(!snag_binary_checkpoint_index_decode(selected.data, selected.len, &identity,
        boundary, root, &captured));
    assert(captured.entry_count);
    struct snag_buf retained = {.max = SIZE_MAX};
    assert(!snag_buf_append(&retained, selected.data, selected.len));
    off_t journal_position = lseek(target->log_fd, 0, SEEK_CUR);
    off_t index_position = lseek(fd, 0, SEEK_CUR);
    assert(journal_position >= 0 && index_position >= 0);
    assert(snag_binary_checkpoint_access_plan_read(target->log_fd, -1, &plan, &available,
        tree, NULL, NULL, &selected) < 0);
    assert(snag_binary_checkpoint_access_plan_read(target->log_fd, -2, &plan, &available,
        tree, NULL, NULL, &selected) < 0 && errno == EINVAL);
    assert(snag_binary_checkpoint_access_plan_read(target->log_fd, fd, &plan, &available,
        tree, seed_access_cancel, NULL, &selected) < 0 && errno == ECANCELED);
    assert(!snag_truncate(fd, SNAG_BINARY_INDEX_HEADER_SIZE));
    assert(snag_binary_checkpoint_access_plan_read(target->log_fd, fd, &plan, &available,
        tree, NULL, NULL, &selected) < 0);
    assert(selected.len == retained.len && !memcmp(selected.data, retained.data, retained.len));
    assert(lseek(target->log_fd, 0, SEEK_CUR) == journal_position);
    assert(lseek(fd, 0, SEEK_CUR) == index_position);
    snag_buf_free(&retained);
    snag_binary_checkpoint_access_plan_free(&plan);
    snag_buf_free(&empty);
    snag_buf_free(&provider);
    snag_buf_free(&selected);
    snag_buf_free(&scratch);
    close(fd);
}

static void
test_prepared_native_seed(const char *cwd)
{
    char error[512];
    for (unsigned int variant = 0u; variant < 18u; ++variant) {
        struct snag_session prepared;
        struct snag_session target;
        snag_session_init(&prepared);
        snag_session_init(&target);
        assert(!snag_session_prepare(&prepared, cwd, "default", "fixture", "default",
            error, sizeof(error)));
        commit_data(&prepared, "model_selection_changed", json_pack("{s:s,s:s,s:s,s:s,s:s,s:s}",
            "old_provider", "default", "new_provider", "default",
            "old_model", "fixture", "new_model", "selected",
            "old_effort", "default", "new_effort", "low"));
        struct snag_journal_cursor cursor;
        assert(!snag_store_legacy_cursor_at(&prepared, 0, &cursor, error, sizeof(error)));
        assert(cursor.next_seq == 1u);
        assert(!snag_store_legacy_cursor_at(&prepared, prepared.log_end, &cursor,
            error, sizeof(error)) && cursor.next_seq == prepared.next_seq);
        assert(snag_store_legacy_cursor_at(&prepared, 1, &cursor, error, sizeof(error)) < 0);
        if (variant == 1u || variant == 6u) {
            json_t *instructions = json_array();
            assert(instructions);
            size_t length = SNAG_BINARY_BATCH_TARGET;
            char *large = variant == 6u ? malloc(length + 1u) : NULL;
            if (variant == 6u) {
                assert(large);
                memset(large, 'a', length);
                large[length] = '\0';
            }
            commit_data(&prepared, "input_received", input_data(large ? large :
                "pending native input", false, instructions));
            free(large);
            json_decref(instructions);
        }
        if (variant == 11u) {
            struct snag_irc_event event = {.kind = SNAG_IRC_MESSAGE, .timestamp_ms = 123u,
                .sequence = 1u, .input = true, .classified = true, .routed = true};
            assert(snag_strcpy(event.stream, sizeof(event.stream),
                "11111111111111111111111111111111"));
            assert(snag_strcpy(event.endpoint, sizeof(event.endpoint), "127.0.0.1:6667"));
            assert(snag_strcpy(event.room, sizeof(event.room), "#seeded"));
            assert(snag_strcpy(event.nick, sizeof(event.nick), "peer"));
            assert(snag_strcpy(event.text, sizeof(event.text), "original routed message"));
            struct snag_irc_event_route *route = &event.route;
            route->generation = 1u;
            route->kind = SNAG_IRC_CHANNEL;
            route->identity = SNAG_IRC_AGENT;
            assert(snag_strcpy(route->connection, sizeof(route->connection),
                "22222222222222222222222222222222"));
            assert(snag_strcpy(route->conversation, sizeof(route->conversation),
                "33333333333333333333333333333333"));
            assert(snag_strcpy(route->target, sizeof(route->target), event.room));
            commit_data(&prepared, "irc_event_v2", snag_irc_event_data(&event));
            event.kind = SNAG_IRC_NOTICE;
            event.input = false;
            ++event.sequence;
            ++event.timestamp_ms;
            event.room[0] = '\0';
            route->kind = SNAG_IRC_CONNECTION_EVENTS;
            assert(snag_strcpy(route->conversation, sizeof(route->conversation),
                "44444444444444444444444444444444"));
            assert(snag_strcpy(route->target, sizeof(route->target), "accepted-nick"));
            commit_data(&prepared, "irc_event_v2", snag_irc_event_data(&event));
        }
        if (variant == 12u) {
            const char *goal = "55555555555555555555555555555555";
            commit_data(&prepared, "goal_started", json_pack("{s:s,s:s}",
                "goal_id", goal, "prompt", "Finish the recorded objective"));
            commit_data(&prepared, "goal_blocked", json_pack("{s:s,s:s,s:s,s:s}",
                "actor", "model", "goal_id", goal, "reason", "Waiting for an event",
                "wait_for", "operator"));
        }
        if (variant == 16u || variant == 17u) {
            commit_data(&prepared, "retry_auto_changed", json_pack("{s:s}", "value",
                variant == 16u ? "on" : "off"));
        }
        target.log_fd = temporary_fd();
        target.lock_fd = temporary_fd();
        int log_fd = target.log_fd;
        int lock_fd = target.lock_fd;
        json_t *before = snag_checkpoint_state_encode(&prepared);
        assert(before);
        struct snag_buf pending = {.max = SIZE_MAX};
        assert(!snag_buf_append(&pending, prepared.pending_log->data, prepared.pending_log->len));
        void *callback = prepared.on_commit_opaque;
        uint64_t sequence = prepared.next_seq;
        if (variant == 2u) ++prepared.next_seq;
        if (variant == 3u) prepared.pending_log->data[0] ^= 1u;
        if (variant == 4u) target.lock_fd = target.log_fd;
        if (variant == 5u) {
            snag_strcpy(prepared.default_model, sizeof(prepared.default_model), "unrecorded model");
        }
        if (variant == 7u) --prepared.pending_log->len;
        if (variant == 8u) assert(!snag_write_full(target.log_fd, "x", 1u));
        if (variant == 9u) prepared.snapshot_read_only = true;
        if (variant == 10u) target.snapshot_read_only = true;
        struct snag_buf index = {.max = SIZE_MAX};
        if (variant == 13u) index.max = 0u;
        if (variant == 14u) assert(!snag_buf_append(&index, "keep", 4u));
        if (variant == 15u) index.max = SNAG_BINARY_INDEX_HEADER_SIZE;
        int rc = snag_store_seed_binary_session(&prepared, &target, &index, error, sizeof(error));
        if (variant < 2u || variant == 6u || variant == 11u || variant == 12u ||
            variant == 16u || variant == 17u) {
            if (rc < 0) fprintf(stderr, "prepared native seed: %s (%d)\n", error, errno);
            assert(!rc && target.binary && target.log_fd == log_fd && target.lock_fd == lock_fd);
            same_core_state(&prepared, &target);
            const json_t *recent;
            const json_t *history;
            assert(!snag_context_capture_seam(&target, &recent, &history));
            assert(json_is_array(recent) && json_is_array(history));
            struct snag_binary_anchor boundary;
            struct snag_binary_index_tree tree;
            struct snag_binary_checkpoint_sources sources = {0};
            assert(!snag_session_binary_checkpoint_capture(&target, &boundary, &tree, &sources,
                error, sizeof(error)));
            assert(boundary.next_seq == sequence && tree.count == sequence - 1u);
            seed_index_checks(&target, &index, &boundary, &tree, &sources);
            assert(sources.input == (variant == 1u || variant == 6u ? sequence - 1u : 0u));
            if (variant == 16u || variant == 17u) {
                assert(!strcmp(target.retry_auto, variant == 16u ? "on" : "off"));
                test_store_binary_core_state(target.log_fd, &boundary, &sources, &target);
            } else assert(!target.retry_auto);
            if (variant == 11u) {
                assert(target.irc_activity && json_object_size(
                    json_object_get(target.irc_activity, "items")) == 2u);
                test_store_binary_core_state(target.log_fd, &boundary, &sources, &target);
            }
            if (variant == 12u) {
                assert(!strcmp(snag_goal_wait_for(&target), "operator"));
                test_store_binary_core_state(target.log_fd, &boundary, &sources, &target);
            }
            snag_binary_checkpoint_sources_free(&sources);
            commit_data(&target, "model_selection_changed", json_pack("{s:s,s:s,s:s,s:s,s:s,s:s}",
                "old_provider", "default", "new_provider", "default",
                "old_model", "selected", "new_model", "after seed",
                "old_effort", "low", "new_effort", "high"));
            assert(target.next_seq == sequence + 1u && !strcmp(target.default_model, "after seed"));
            if (variant == 16u || variant == 17u) {
                const char *next_retry = variant == 16u ? "off" : "on";
                commit_data(&target, "retry_auto_changed", json_pack("{s:s}",
                    "value", next_retry));
                assert(!strcmp(target.retry_auto, next_retry));
                assert(!strcmp(prepared.retry_auto, variant == 16u ? "on" : "off"));
                assert(!snag_session_binary_checkpoint_capture(&target, &boundary, &tree,
                    &sources, error, sizeof(error)));
                test_store_binary_core_state(target.log_fd, &boundary, &sources, &target);
                snag_binary_checkpoint_sources_free(&sources);
            }
            if (variant == 12u) {
                commit_data(&target, "goal_replaced", json_pack("{s:s,s:s,s:s,s:s}",
                    "actor", "user", "goal_id", target.goal_id,
                    "new_goal_id", "66666666666666666666666666666666",
                    "prompt", "Replacement keeps the blocker"));
                assert(!strcmp(snag_goal_wait_for(&target), "operator"));
                assert(!snag_session_binary_checkpoint_capture(&target, &boundary, &tree,
                    &sources, error, sizeof(error)));
                test_store_binary_core_state(target.log_fd, &boundary, &sources, &target);
                snag_binary_checkpoint_sources_free(&sources);
                commit_data(&target, "goal_resumed", json_pack("{s:s}",
                    "goal_id", target.goal_id));
                assert(!json_object_get(target.strings, "goal_wait_for"));
                assert(!snag_session_binary_checkpoint_capture(&target, &boundary, &tree,
                    &sources, error, sizeof(error)));
                test_store_binary_core_state(target.log_fd, &boundary, &sources, &target);
                snag_binary_checkpoint_sources_free(&sources);
                commit_data(&target, "goal_blocked", json_pack("{s:s,s:s,s:s}",
                    "goal_id", target.goal_id, "actor", "model", "reason", "Older blocker"));
                assert(!json_object_get(target.strings, "goal_wait_for"));
                assert(!snag_session_binary_checkpoint_capture(&target, &boundary, &tree,
                    &sources, error, sizeof(error)));
                test_store_binary_core_state(target.log_fd, &boundary, &sources, &target);
                snag_binary_checkpoint_sources_free(&sources);
            }
        } else {
            assert(rc < 0 && !target.binary && !target.id[0] && target.next_seq == 1u);
            assert(index.len == (variant == 14u ? 4u : 0u));
            if (variant == 14u) assert(!memcmp(index.data, "keep", 4u));
            assert(target.log_fd == log_fd && target.lock_fd == (variant == 4u ? log_fd : lock_fd));
            assert(fcntl(log_fd, F_GETFD) >= 0 && fcntl(lock_fd, F_GETFD) >= 0);
        }
        if (variant == 2u) prepared.next_seq = sequence;
        if (variant == 3u) prepared.pending_log->data[0] ^= 1u;
        if (variant == 4u) target.lock_fd = lock_fd;
        if (variant == 5u) {
            snag_strcpy(prepared.default_model, sizeof(prepared.default_model), "selected");
        }
        if (variant == 7u) ++prepared.pending_log->len;
        if (variant == 9u) prepared.snapshot_read_only = false;
        if (variant == 10u) target.snapshot_read_only = false;
        json_t *after = snag_checkpoint_state_encode(&prepared);
        assert(after && json_equal(before, after));
        assert(prepared.on_commit_opaque == callback && prepared.pending_log &&
            prepared.pending_log->len == pending.len &&
            !memcmp(prepared.pending_log->data, pending.data, pending.len));
        json_decref(before);
        json_decref(after);
        snag_buf_free(&pending);
        snag_buf_free(&index);
        snag_session_close(&target);
        snag_session_close(&prepared);
    }
}

static void
test_irc_admission_after_checkpoint(struct snag_store *store, const char *cwd, bool legacy)
{
    struct snag_session session;
    snag_session_init(&session);
    char error[256] = {0};
    int created = legacy ?
        legacy_fixture_create(store, &session, cwd, "default", "fixture", "default",
            error, sizeof(error)) :
        snag_session_create(store, &session, cwd, "default", "fixture", "default",
            error, sizeof(error));
    assert(!created);
    char id[SNAG_ID_HEX_LEN + 1u];
    memcpy(id, session.id, sizeof(id));
    json_t *paths = checked_json(json_array());
    commit_data(&session, "input_received", input_data("checkpoint admission", false, paths));
    commit_data(&session, "turn_started",
        direct_turn_data(cwd, "checkpoint admission", GOAL_ID, 1, false, paths));
    json_decref(paths);

    struct snag_irc_event event = {.kind = SNAG_IRC_MESSAGE, .timestamp_ms = 1u,
        .stream = OTHER_ID, .sequence = 1u, .input = true, .endpoint = "fixture",
        .room = "#fixture", .nick = "operator", .op = true};
    assert(snag_strcpy(event.text, sizeof(event.text), "waiting at checkpoint"));
    uint64_t observation = commit_data(&session, "irc_event", snag_irc_event_data(&event));
    /* The neighbour is obsolete by the checkpoint and absent from its sparse index. */
    commit_data(&session, "retry_auto_changed", json_pack("{s:s}", "value", "off"));
    int64_t neighbor = session.committed_start +
        (session.committed_end - session.committed_start) / 2;
    for (size_t i = 0u; i <= SNAG_CONTEXT_COMPACT_OVERLAP_EVENTS; ++i) {
        commit_data(&session, "retry_auto_changed", json_pack("{s:s}", "value", "on"));
    }
    commit_data(&session, "context_rebased", json_pack("{s:s,s:s}",
        "reason", "turn_recovery", "turn_id", GOAL_ID));
    assert(!snag_session_checkpoint(&session, error, sizeof(error)));
    commit_data(&session, "irc_admitted", json_pack("{s:[I]}",
        "sequences", (json_int_t)observation));
    uint64_t end = session.next_seq;
    int journal = dup(session.log_fd);
    assert(journal >= 0);
    snag_session_close(&session);
    snag_session_init(&session);
    if (!legacy) {
        unsigned char original;
        assert(snag_pread(journal, &original, 1u, neighbor) == 1);
        unsigned char damaged = original ^ 0x80u;
        assert(snag_seek(journal, neighbor, SEEK_SET) == neighbor);
        assert(!snag_write_full(journal, &damaged, 1u));
        assert(snag_session_open(store, &session, id, error, sizeof(error)) < 0);
        snag_session_close(&session);
        snag_session_init(&session);
        assert(snag_seek(journal, neighbor, SEEK_SET) == neighbor);
        assert(!snag_write_full(journal, &original, 1u));
    }
    assert(!close(journal));
    int opened = snag_session_open(store, &session, id, error, sizeof(error));
    if (opened < 0) fprintf(stderr, "IRC admission after checkpoint: %s\n", error);
    assert(!opened && session.next_seq == end && !!session.binary == !legacy);
    assert(!strcmp(session.retry_auto, "on"));
    assert(!strcmp(session.active_prompt, "checkpoint admission"));
    if (!legacy) {
        const json_t *recent;
        const json_t *history;
        assert(!snag_context_capture_seam(&session, &recent, &history));
        size_t matches = 0u;
        for (size_t i = 0u; i < json_array_size(history); ++i) {
            const json_t *row = json_array_get(history, i);
            if ((uint64_t)json_integer_value(json_object_get(row, "seq")) != observation)
                continue;
            assert(!strcmp(snag_json_string(row, "type"), "irc_event"));
            assert(!strcmp(snag_json_string(json_object_get(row, "data"), "text"), event.text));
            ++matches;
        }
        assert(matches == 1u);
    }
    assert(!snag_session_checkpoint(&session, error, sizeof(error)));
    snag_session_close(&session);
    snag_session_init(&session);
    assert(!snag_session_open(store, &session, id, error, sizeof(error)));
    assert(!strcmp(session.active_prompt, "checkpoint admission"));
    snag_session_close(&session);
}

struct snapshot_append {
    struct snag_session *owner;
    unsigned int calls, at;
    bool cancel, change_mode;
};

static bool
append_during_snapshot(void *opaque)
{
    struct snapshot_append *hook = opaque;
    if (++hook->calls != hook->at) return false;
    if (hook->cancel) return true;
    if (hook->change_mode) {
        assert(!fchmod(hook->owner->log_fd, 0400));
    } else {
        commit_data(hook->owner, "session_named", json_pack("{s:s}", "name", "snapshot-after"));
    }
    return false;
}

static int
open_test_snapshot(struct snag_store *store, struct snag_session *snapshot,
    struct snapshot_append *hook, char *error, size_t size)
{
    snag_session_init(snapshot);
    assert(!snag_store_open_session_directory(store, snapshot, hook->owner->id, error, size));
    snapshot->log_fd = snag_open_read_security_at(snapshot->dir_fd, "journal.bin", false);
    assert(snapshot->log_fd >= 0);
    snapshot->snapshot_read_only = true;
    snapshot->history_cancel = append_during_snapshot;
    snapshot->history_cancel_opaque = hook;
    return snag_store_load_binary_session(snapshot, SNAG_TAIL_IGNORE, error, size);
}

static void
test_live_native_snapshot(struct snag_store *store, const char *cwd)
{
    struct snag_session owner, snapshot;
    snag_session_init(&owner);
    char error[256] = {0};
    assert(!snag_session_create(store, &owner, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    struct snapshot_append hook = {.owner = &owner};
    commit_data(&owner, "session_named", json_pack("{s:s}", "name", "snapshot-before"));
    assert(!snag_session_checkpoint(&owner, error, sizeof(error)));
    for (unsigned int i = 0u; i < 4u; ++i)
        commit_data(&owner, "retry_auto_changed", json_pack("{s:s}", "value", "on"));
    assert(!open_test_snapshot(store, &snapshot, &hook, error, sizeof(error)));
    unsigned int callbacks = hook.calls;
    assert(callbacks > 4u);
    snag_session_close(&snapshot);

    /* Append at every cancellation boundary, including after suffix validation.
     * Each successful snapshot must be one complete committed state. */
    for (unsigned int at = 1u; at <= callbacks; ++at) {
        commit_data(&owner, "session_named", json_pack("{s:s}", "name", "snapshot-before"));
        assert(!snag_session_checkpoint(&owner, error, sizeof(error)));
        for (unsigned int i = 0u; i < 4u; ++i)
            commit_data(&owner, "retry_auto_changed", json_pack("{s:s}", "value", "on"));
        uint64_t before = owner.next_seq;
        hook = (struct snapshot_append){.owner = &owner, .at = at};
        int rc = open_test_snapshot(store, &snapshot, &hook, error, sizeof(error));
        if (rc < 0) fprintf(stderr, "live snapshot callback %u: %s\n", at, error);
        assert(!rc && hook.calls >= at && owner.next_seq == before + 1u);
        assert(snapshot.next_seq == before || snapshot.next_seq == owner.next_seq);
        assert(!strcmp(snapshot.name,
            snapshot.next_seq == before ? "snapshot-before" : "snapshot-after"));
        assert(snapshot.snapshot_read_only && snapshot.lock_fd < 0);
        snag_session_close(&snapshot);
    }

    for (unsigned int fault = 0u; fault < 2u; ++fault) {
        hook = (struct snapshot_append){.owner = &owner};
        assert(!open_test_snapshot(store, &snapshot, &hook, error, sizeof(error)));
        snag_session_close(&snapshot);
        hook.at = hook.calls;
        hook.calls = 0u;
        hook.cancel = fault == 0u;
        hook.change_mode = fault == 1u;
        int rc = open_test_snapshot(store, &snapshot, &hook, error, sizeof(error));
        assert(rc < 0 && errno == (fault ? EAGAIN : ECANCELED));
        assert(!snapshot.name && !snapshot.binary && snapshot.next_seq == 1u);
        snag_session_close(&snapshot);
        if (fault) assert(!fchmod(owner.log_fd, 0600));
    }
    snag_session_close(&owner);
}

void
test_store_binary_replay(struct snag_store *store, const char *cwd)
{
    test_live_native_snapshot(store, cwd);
    test_irc_admission_after_checkpoint(store, cwd, true);
    test_irc_admission_after_checkpoint(store, cwd, false);
    test_checkpoint_after_streamed_tools(store, cwd, false);
    test_checkpoint_after_streamed_tools(store, cwd, true);
    test_prepared_native_seed(cwd);
    test_native_factory(cwd);
    test_producer_candidate_ownership();
    for (unsigned int variant = 0u; variant < 13u; ++variant) {
        test_live_result_coordinates(SNAG_BINARY_TOOL_FINISHED, variant);
        test_live_result_coordinates(SNAG_BINARY_PROCESS_CLOSED, variant);
    }
    for (unsigned int bad = 0u; bad < 4u; ++bad) test_voice_adoption(store, cwd, bad);
    test_compact_origins(store, cwd, false);
    test_compact_origins(store, cwd, true);
    test_download_origins(store, cwd);
    test_input_context_origins(store, cwd);
    test_text_origins(store, cwd);
    test_metadata_origins(store, cwd);
    test_call_origins(store, cwd);
    for (unsigned int mode = 0u; mode < 7u; ++mode) test_output_references(store, cwd, mode);
    test_process_origins(store, cwd);
    test_legacy_process_origin(store, cwd);
    test_import_batches(store, cwd);
    test_stopped_directory_conversion(store, cwd);
    test_import_output_sources(store, cwd);
    test_import_input_sources(store, cwd);
    for (unsigned int ending = 0u; ending < 7u; ++ending) {
        test_snapshot_references(store, cwd, ending, false);
        test_snapshot_references(store, cwd, ending, true);
    }
    for (unsigned int variant = 0u; variant < 4u; ++variant) {
        test_queued_instructions(store, cwd, (variant & 1u) != 0u, (variant & 2u) != 0u);
    }
    for (unsigned int variant = 0u; variant < 4u; ++variant)
        test_voice_references(store, cwd, (variant & 1u) != 0u, (variant & 2u) != 0u);
    test_goal_references(store, cwd, 0u, false);
    for (unsigned int variant = 0u; variant < 7u; ++variant)
        test_goal_references(store, cwd, variant, true);
    for (unsigned int variant = 0u; variant < 4u; ++variant)
        test_steering_references(store, cwd, variant);
    test_voice_original(store, cwd);
    test_queued_references(store, cwd, false);
    test_queued_references(store, cwd, true);
    for (unsigned int variant = 0u; variant < 4u; ++variant) {
        for (unsigned int paths = 0u; paths < 3u; ++paths) {
            test_direct_references(store, cwd, (variant & 1u) != 0u, (variant & 2u) != 0u, paths);
        }
    }
    char error[512];
    struct snag_session original, expected, source, restored;
    snag_session_init(&original);
    snag_session_init(&expected);
    snag_session_init(&source);
    snag_session_init(&restored);
    assert(!legacy_fixture_create(store, &original, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    commit_data(&original, "banner_updated", json_pack("{s:s}", "text", "before checkpoint"));
    commit_data(&original, "goal_started", json_pack("{s:s,s:s}",
        "goal_id", GOAL_ID, "prompt", "original authority"));
    commit_data(&original, "goal_blocked", json_pack("{s:s,s:s,s:s}",
        "goal_id", GOAL_ID, "actor", "model", "reason", "waiting"));
    commit_data(&original, "timer_scheduled", json_pack("{s:s,s:I,s:s}",
        "timer_id", OTHER_ID, "due_ms", (json_int_t)(original.last_time_ms + 60000u),
        "text", "source reminder"));
    commit_data(&original, "voice_transfer_record", json_pack("{s:s,s:s,s:s,s:i,s:s,s:{s:s,s:s}}",
        "transfer_id", OTHER_ID, "target_session_id", original.id, "source_session_id", OTHER_ID,
        "source_seq", 1, "source_type", "goal_started", "data",
        "goal_id", OTHER_ID, "prompt", "inert archived goal"));
    assert(!snag_session_checkpoint(&original, error, sizeof(error)));
    commit_data(&original, "banner_updated", json_pack("{s:s}", "text", "after checkpoint"));

    struct replay_fixture fixture = {.file = {.max = 4u * SNAG_BINARY_BATCH_MAX}};
    for (size_t i = 0u; i < 3u; ++i) fixture.payload[i].max = SNAG_MAX_EVENT_LINE;
    struct snag_legacy_recovery legacy;
    assert(!snag_store_reconcile_legacy(&original, &expected, fixture_event, &fixture,
        &legacy, error, sizeof(error)));
    fixture_flush(&fixture);
    check_import(&original, &expected);
    assert(legacy.discarded_checkpoints == 1u);
    memcpy(source.id, original.id, sizeof(source.id));
    source.log_fd = temporary_fd();
    source.lock_fd = temporary_fd();
    assert(!snag_lock_file(source.lock_fd, false));
    struct snag_binary_recovery recovery;
    assert(!replay_check(&source, &restored, &recovery, fixture.file.data, fixture.file.len));
    assert(recovery.batches == 3u && !recovery.problem_seq && !recovery.incomplete_tail_bytes);
    assert(restored.next_seq == expected.next_seq);
    assert(restored.last_time_ms == expected.last_time_ms);
    assert(restored.log_end == (int64_t)fixture.file.len);
    assert(restored.goal_status == SNAG_GOAL_BLOCKED && !strcmp(restored.goal_id, GOAL_ID));
    assert(!strcmp(restored.goal_prompt, "original authority"));
    assert(!strcmp(restored.timer_text, "source reminder"));
    assert(!strcmp(restored.banner_text, "after checkpoint"));
    assert(!restored.active_turn && !restored.response_open && !restored.process_count);
    assert(!*restored.voice_history.transfer_id);
    struct snag_session earlier;
    struct snag_binary_recovery earlier_recovery;
    snag_session_init(&earlier);
    assert(!replay_check(&source, &earlier, &earlier_recovery,
        fixture.file.data, fixture.first_end));
    assert(earlier_recovery.batches == 1u && earlier.goal_status == SNAG_GOAL_ACTIVE);
    write_fixture(&source, fixture.file.data, fixture.file.len);
    prefix_matches(&source, &earlier, &earlier_recovery.verified);
    snag_session_close(&earlier);
    json_t *left = snag_checkpoint_state_encode(&expected);
    json_t *right = snag_checkpoint_state_encode(&restored);
    assert(left && right);
    /* The storage domains have different offsets/hashes and checkpoint caches. */
    const char *storage[] = {"prev_sha256", "log_end", "checkpoint_offset", "checkpoint_seq"};
    for (size_t i = 0u; i < sizeof(storage) / sizeof(storage[0]); ++i) {
        json_object_del(left, storage[i]);
        json_object_del(right, storage[i]);
    }
    assert(json_equal(left, right));
    json_decref(left);
    json_decref(right);

    /* Every incomplete first batch fails; every incomplete second batch returns
     * only the first complete batch. Both retain every source byte and fd cursor. */
    for (size_t cut = 0u; cut < fixture.first_end; ++cut)
        assert(replay_check(&source, &restored, &recovery, fixture.file.data, cut) < 0);
    struct snag_binary_anchor first;
    struct snag_binary_batch batch;
    struct snag_binary_identity identity;
    struct snag_binary_anchor root;
    assert(!snag_binary_header_decode(fixture.file.data, SNAG_BINARY_HEADER_SIZE,
        &identity, &root));
    struct snag_buf decoded_batch = {.max = SNAG_BINARY_BATCH_MAX};
    assert(!binary_fixture_read(fixture.file.data + root.end,
        fixture.file.len - (size_t)root.end, &root, &decoded_batch, &batch, &first));
    struct snag_binary_anchor second;
    assert(!binary_fixture_read(fixture.file.data + first.end,
        fixture.file.len - (size_t)first.end, &first, &decoded_batch, &batch, &second));
    for (size_t cut = fixture.first_end; cut < second.end; ++cut) {
        assert(!replay_check(&source, &restored, &recovery, fixture.file.data, cut));
        assert(recovery.verified.end == first.end && restored.next_seq == first.next_seq);
        assert(recovery.incomplete_tail_bytes == cut - fixture.first_end);
    }
    assert(!replay_check(&source, &restored, &recovery, fixture.file.data, fixture.file.len));

    struct snag_buf file = {.max = 4u * SNAG_BINARY_BATCH_MAX};
    struct snag_binary_record record = {
        .kind = 0x8002u, .version = 1u, .flags = SNAG_BINARY_RECORD_OPTIONAL,
        .timestamp_ms = fixture.identity.created_ms + 100u
    };
    append_record(&fixture, &file, &record, 0u);
    assert(!replay_check(&source, &restored, &recovery, file.data, file.len));
    assert(restored.next_seq == fixture.anchor.next_seq + 1u);
    assert(restored.last_time_ms == record.timestamp_ms);
    assert(!strcmp(restored.goal_prompt, "original authority"));
    record.flags = 0u;
    append_record(&fixture, &file, &record, 0u);
    assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0);
    assert(recovery.verified.end == fixture.anchor.end);

    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    enum snag_binary_kind kind;
    json_t *bad = checked_json(json_pack("{s:s,s:s,s:s}", "goal_id", OTHER_ID,
        "actor", "user", "prompt", "wrong goal"));
    assert(!snag_binary_legacy_encode(&payload, "goal_reworded", bad, &kind));
    json_decref(bad);
    record.kind = (uint16_t)kind;
    record.version = snag_binary_event_version(kind);
    record.payload = payload.data;
    record.size = payload.len;
    append_record(&fixture, &file, &record, 0u);
    assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0 && errno == EINVAL);
    assert(recovery.problem_seq == fixture.anchor.next_seq);
    assert(recovery.problem_start == fixture.anchor.end);
    record.flags = SNAG_BINARY_RECORD_OPTIONAL;
    append_record(&fixture, &file, &record, 0u);
    assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0 && errno == EINVAL);
    record.flags = 0u;
    record.version = 0xffffu;
    append_record(&fixture, &file, &record, 0u);
    assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0);

    record = (struct snag_binary_record){.kind = 0x8002u, .version = 1u,
        .flags = SNAG_BINARY_RECORD_OPTIONAL};
    append_record(&fixture, &file, &record, 1u);
    assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0 && errno == EINVAL);
    assert(recovery.problem_start == fixture.anchor.end);
    append_record(&fixture, &file, &record, 0u);
    file.data[file.len - 1u] ^= 1u;
    assert(replay_check(&source, &restored, &recovery, file.data, file.len) < 0);
    assert(recovery.verified.end == fixture.anchor.end);
    test_reference_rejection(&fixture, &source, &restored);
    test_creation_rejection(&fixture, &source, &restored);
    test_import_markers(&fixture, &source, &restored);
    source.id[0] = source.id[0] == '0' ? '1' : '0';
    assert(replay_check(&source, &restored, &recovery, fixture.file.data, fixture.file.len) < 0);

    assert(prefix_comparisons);
    printf("native core prefix: %u state/origin comparisons\n", prefix_comparisons);
    assert(bounded_reference_suffixes);
    printf("native bounded referenced suffix: %u complete/missing-old pairs\n",
        bounded_reference_suffixes);

    snag_buf_free(&payload);
    snag_buf_free(&file);
    for (size_t i = 0u; i < 3u; ++i) snag_buf_free(&fixture.payload[i]);
    snag_buf_free(&fixture.file);
    json_decref(fixture.creation);
    snag_session_close(&restored);
    snag_session_close(&source);
    snag_session_close(&expected);
    snag_session_close(&original);
    snag_buf_free(&decoded_batch);
}
