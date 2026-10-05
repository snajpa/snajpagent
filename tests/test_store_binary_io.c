/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_io.h"
#include "store_binary_index.h"
#include "store_binary_wire.h"
#include "fs.h"
#include "store_internal.h"
#include "store_binary_replay.h"
#include "store_binary_legacy.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

void test_store_binary_io(void);

struct probe {
    pthread_t caller;
    pthread_t owner;
    bool owner_seen;
    bool block_write;
    bool block_sync;
    atomic_bool write_entered;
    atomic_bool sync_entered;
    atomic_bool release_write;
    atomic_bool release_sync;
    unsigned int partial_failures;
    unsigned int sync_failures;
    size_t writes;
    size_t syncs;
    size_t bytes;
    atomic_uint effects;
};

static void
wait_flag(atomic_bool *flag)
{
    for (unsigned int i = 0u; i < 10000u && !atomic_load(flag); ++i) {
        assert(!snag_sleep_ms(1u));
    }
    assert(atomic_load(flag));
}

static void
check_owner(struct probe *probe)
{
    assert(!pthread_equal(probe->caller, pthread_self()));
    if (probe->owner_seen) {
        assert(pthread_equal(probe->owner, pthread_self()));
    } else {
        probe->owner = pthread_self();
        probe->owner_seen = true;
    }
}

static int
probe_write(void *opaque, int fd, const void *data, size_t size)
{
    struct probe *probe = opaque;
    check_owner(probe);
    ++probe->writes;
    atomic_store(&probe->write_entered, true);
    if (probe->block_write) wait_flag(&probe->release_write);
    if (probe->partial_failures) {
        --probe->partial_failures;
        size_t partial = size / 2u;
        assert(!snag_write_full(fd, data, partial));
        probe->bytes += partial;
        return snag_errno(EIO);
    }
    int rc = snag_write_full(fd, data, size);
    if (!rc) probe->bytes += size;
    return rc;
}

static int
probe_sync(void *opaque, int fd)
{
    struct probe *probe = opaque;
    check_owner(probe);
    ++probe->syncs;
    atomic_store(&probe->sync_entered, true);
    if (probe->block_sync) wait_flag(&probe->release_sync);
    int rc = snag_sync_file(fd);
    if (!rc && probe->sync_failures) {
        /* Model the ambiguous case: the bytes reached storage despite failure. */
        --probe->sync_failures;
        return snag_errno(EIO);
    }
    return rc;
}

static struct snag_binary_io *
start_owner(int fd, const struct snag_binary_anchor *before, struct probe *probe)
{
    probe->caller = pthread_self();
    struct snag_binary_io_ops ops = {
        .write_full = probe_write, .sync_file = probe_sync, .opaque = probe
    };
    struct snag_binary_io *io = snag_binary_io_start(fd, before, &ops);
    assert(io && snag_binary_io_wake(io) != SNAG_WAKE_INVALID);
    return io;
}

static int
journal_fd(struct snag_binary_anchor *before)
{
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snag-io-XXXXXX");
    assert(path);
    int fd = mkstemp(path);
    assert(fd >= 0 && !unlink(path));
    free(path);
    assert(!snag_lock_file(fd, false));
    struct snag_binary_identity identity = {.created_ms = 42u};
    identity.id[0] = 17u;
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &identity);
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, before));
    assert(!snag_write_full(fd, header, sizeof(header)) && !snag_sync_file(fd));
    return fd;
}

static struct snag_binary_record
record(const char *text)
{
    return (struct snag_binary_record){.kind = 0x8001u, .version = 1u,
        .flags = SNAG_BINARY_RECORD_OPTIONAL, .timestamp_ms = 100u,
        .payload = (const unsigned char *)text, .size = strlen(text)};
}

static bool
same_anchor(const struct snag_binary_anchor *left, const struct snag_binary_anchor *right)
{
    return left->end == right->end && left->next_seq == right->next_seq &&
        left->turns == right->turns && left->previous == right->previous &&
        !memcmp(left->digest, right->digest, sizeof(left->digest));
}

static int
await_result(struct snag_binary_io *io, struct snag_binary_io_result *out)
{
    assert(snag_wakeup_wait(snag_binary_io_wake(io), 10000) == 1);
    int rc = snag_binary_io_take(io, out);
    assert(rc != 1);
    return rc;
}

static int
await_batch(struct snag_binary_io *io, struct snag_binary_io_result *out,
    struct snag_buf *batch)
{
    assert(snag_wakeup_wait(snag_binary_io_wake(io), 10000) == 1);
    int rc = snag_binary_io_take_batch(io, out, batch);
    assert(rc != 1);
    return rc;
}

static void
verify_commit_bytes(int fd, const struct snag_binary_anchor *before,
    const struct snag_binary_anchor *after, const struct snag_buf *committed)
{
    struct snag_buf bytes = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor found;
    assert(!snag_binary_batch_read(fd, after->end, before, &bytes, &batch, &found));
    assert(same_anchor(&found, after) && bytes.len == committed->len &&
        !memcmp(bytes.data, committed->data, bytes.len));
    struct snag_binary_identity identity = {.created_ms = 42u};
    identity.id[0] = 17u;
    struct snag_binary_index_tree tree = {0};
    assert(before->next_seq == 1u);
    assert(!snag_binary_index_tree_append_batch(NULL, &tree, &identity, before,
        after, committed->data, committed->len));
    assert(tree.count == after->next_seq - 1u);
    snag_buf_free(&bytes);
}

static void
verify_batch(int fd, const struct snag_binary_anchor *before,
    const struct snag_binary_anchor *after, const struct snag_binary_record *records,
    uint32_t count)
{
    struct snag_buf bytes = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor found;
    assert(!snag_binary_batch_read(fd, after->end, before, &bytes, &batch, &found));
    assert(same_anchor(&found, after) && batch.count == count);
    size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
    for (uint32_t i = 0u; i < count; ++i) {
        struct snag_binary_record found_record;
        uint64_t sequence;
        assert(!snag_binary_record_next(&batch, &offset, &found_record, &sequence));
        assert(sequence == before->next_seq + i && found_record.kind == records[i].kind &&
            found_record.size == records[i].size &&
            !memcmp(found_record.payload, records[i].payload, records[i].size));
    }
    snag_buf_free(&bytes);
}

static void
test_group_ack(void)
{
    struct snag_binary_anchor before;
    int fd = journal_fd(&before);
    struct probe probe = {.block_write = true, .block_sync = true};
    struct snag_binary_io *io = start_owner(fd, &before, &probe);
    char first[] = "one";
    char second[] = "two";
    struct snag_binary_record records[] = {record(first), record(second)};
    assert(!snag_binary_io_submit(io, records, 2u, 0u));
    memset(first, 'x', 3u);
    memset(second, 'y', 3u);
    records[0].kind++;
    struct snag_buf committed = {.max = 16u};
    assert(!snag_buf_append(&committed, "canary", 6u));
    struct snag_buf original;
    memcpy(&original, &committed, sizeof(original));
    wait_flag(&probe.write_entered);
    struct snag_binary_io_result result;
    memset(&result, 0xa5, sizeof(result));
    struct snag_binary_io_result saved;
    memcpy(&saved, &result, sizeof(saved));
    assert(snag_binary_io_take_batch(io, &result, &committed) == 1 &&
        !memcmp(&saved, &result, sizeof(saved)));
    assert(!memcmp(&committed, &original, sizeof(original)) &&
        !memcmp(committed.data, "canary", 6u));
    assert(snag_binary_io_take_batch(io, &result, NULL) < 0 && errno == EINVAL);
    assert(snag_binary_io_take_batch(NULL, &result, &committed) < 0 && errno == EINVAL);
    assert(snag_binary_io_take_batch(io, NULL, &committed) < 0 && errno == EINVAL);
    assert(snag_binary_io_submit(io, records, 2u, 0u) < 0 && errno == EBUSY);
    assert(snag_binary_io_close(io) < 0 && errno == EBUSY);
    atomic_store(&probe.release_write, true);
    wait_flag(&probe.sync_entered);
    assert(snag_binary_io_take_batch(io, &result, &committed) == 1 &&
        !memcmp(&saved, &result, sizeof(saved)));
    assert(!memcmp(&committed, &original, sizeof(original)) &&
        !memcmp(committed.data, "canary", 6u));
    atomic_store(&probe.release_sync, true);
    assert(snag_wakeup_wait(snag_binary_io_wake(io), 10000) == 1);
    assert(snag_binary_io_close(io) < 0 && errno == EBUSY);
    assert(!snag_binary_io_take_batch(io, &result, &committed));
    assert(!result.error && !result.retryable && result.durable.next_seq == 3u &&
        same_anchor(&result.written, &result.durable));
    assert(snag_binary_io_take(io, &saved) < 0 && errno == ENOENT);
    struct snag_binary_anchor middle = result.durable;
    struct snag_binary_record last = record("three");
    assert(!snag_binary_io_submit(io, &last, 1u, 0u));
    assert(!await_result(io, &result) && result.durable.next_seq == 4u);
    assert(!snag_binary_io_close(io));
    assert(probe.writes == 2u && probe.syncs == 2u &&
        probe.bytes == result.durable.end - before.end);
    struct snag_binary_record originals[] = {record("one"), record("two")};
    verify_batch(fd, &before, &middle, originals, 2u);
    verify_batch(fd, &middle, &result.durable, &last, 1u);
    verify_commit_bytes(fd, &before, &middle, &committed);
    snag_buf_free(&committed);
    assert(!close(fd));
}

static void
test_failed_commit(bool partial, unsigned int failures)
{
    struct snag_binary_anchor before;
    int fd = journal_fd(&before);
    struct probe probe = {.partial_failures = partial ? failures : 0u,
        .sync_failures = partial ? 0u : failures};
    struct snag_binary_io *io = start_owner(fd, &before, &probe);
    struct snag_binary_record records[] = {record("alpha"), record("beta")};
    assert(!snag_binary_io_submit(io, records, 2u, 0u));
    struct snag_buf committed = {.max = 16u};
    assert(!snag_buf_append(&committed, "canary", 6u));
    struct snag_buf original;
    memcpy(&original, &committed, sizeof(original));
    struct snag_binary_io_result result;
    assert(await_batch(io, &result, &committed) < 0 && errno == EIO && result.error == EIO);
    assert(result.retryable && same_anchor(&result.durable, &before));
    assert(!memcmp(&committed, &original, sizeof(original)) &&
        !memcmp(committed.data, "canary", 6u));
    assert(partial ? same_anchor(&result.written, &before) : result.written.next_seq == 3u);
    assert(snag_binary_io_submit(io, records, 2u, 0u) < 0 && errno == EBUSY);
    assert(!snag_binary_io_retry(io));
    if (failures == 1u) {
        assert(!await_batch(io, &result, &committed) && result.durable.next_seq == 3u &&
            same_anchor(&result.written, &result.durable));
        assert(!snag_binary_io_close(io));
        assert(probe.writes == (partial ? 2u : 1u));
        assert(probe.syncs == (partial ? 1u : 2u));
        assert(probe.bytes == result.durable.end - before.end);
        verify_batch(fd, &before, &result.durable, records, 2u);
        verify_commit_bytes(fd, &before, &result.durable, &committed);
        assert(snag_seek(fd, 0, SEEK_END) == (int64_t)result.durable.end);
    } else {
        assert(await_batch(io, &result, &committed) < 0 && errno == EIO && !result.retryable);
        assert(same_anchor(&result.durable, &before));
        assert(!memcmp(&committed, &original, sizeof(original)) &&
            !memcmp(committed.data, "canary", 6u));
        assert(snag_binary_io_retry(io) < 0 && errno == EALREADY);
        assert(snag_binary_io_submit(io, records, 2u, 0u) < 0 && errno == EBUSY);
        assert(!snag_binary_io_close(io));
        assert(probe.writes == (partial ? 2u : 1u));
        assert(probe.syncs == (partial ? 0u : 2u));
        assert(snag_seek(fd, 0, SEEK_END) > (int64_t)before.end);
    }
    snag_buf_free(&committed);
    assert(!close(fd));
}

static void
test_changed_tail(bool append)
{
    struct snag_binary_anchor before;
    int fd = journal_fd(&before);
    struct probe probe = {.sync_failures = 1u};
    struct snag_binary_io *io = start_owner(fd, &before, &probe);
    struct snag_binary_record data = record("retained batch");
    assert(!snag_binary_io_submit(io, &data, 1u, 0u));
    struct snag_binary_io_result result;
    assert(await_result(io, &result) < 0 && result.retryable);
    /* Deliberately violate exclusive access to simulate conflicting/corrupt bytes. */
    int64_t offset = append ? (int64_t)result.written.end : (int64_t)before.end;
    assert(snag_seek(fd, offset, SEEK_SET) == offset);
    assert(!snag_write_full(fd, "!", 1u));
    int64_t size = snag_seek(fd, 0, SEEK_END);
    assert(!snag_binary_io_retry(io));
    assert(await_result(io, &result) < 0 && errno == ESTALE && !result.retryable);
    assert(same_anchor(&result.durable, &before));
    assert(!snag_binary_io_close(io));
    assert(probe.writes == 1u && probe.syncs == 1u && snag_seek(fd, 0, SEEK_END) == size);
    char kept = 0;
    assert(snag_pread(fd, &kept, 1u, offset) == 1 && kept == '!');
    assert(!close(fd));
}

static void
test_native_partial_write(void)
{
    struct snag_binary_anchor before;
    int fd = journal_fd(&before);
    struct snag_binary_io *io = snag_binary_io_start(fd, &before, NULL);
    assert(io);
    struct rlimit saved;
    assert(!getrlimit(RLIMIT_FSIZE, &saved));
    struct rlimit limited = saved;
    limited.rlim_cur = (rlim_t)before.end + 16u;
    void (*old_signal)(int) = signal(SIGXFSZ, SIG_IGN);
    assert(old_signal != SIG_ERR && !setrlimit(RLIMIT_FSIZE, &limited));
    struct snag_binary_record data = record("native partial write");
    assert(!snag_binary_io_submit(io, &data, 1u, 0u));
    struct snag_binary_io_result result;
    int rc = await_result(io, &result);
    assert(!setrlimit(RLIMIT_FSIZE, &saved) && signal(SIGXFSZ, old_signal) != SIG_ERR);
    assert(rc < 0 && result.error == EFBIG && result.retryable &&
        same_anchor(&result.durable, &before));
    assert(!snag_binary_io_retry(io) && !await_result(io, &result));
    assert(!snag_binary_io_close(io));
    verify_batch(fd, &before, &result.durable, &data, 1u);
    assert(snag_seek(fd, 0, SEEK_END) == (int64_t)result.durable.end);
    assert(!close(fd));
}

static void
test_batch_limits(void)
{
    for (unsigned int large = 0u; large < 2u; ++large) {
        struct snag_binary_anchor before;
        int fd = journal_fd(&before);
        struct snag_binary_io *io = snag_binary_io_start(fd, &before, NULL);
        assert(io);
        uint32_t count = large ? 1u : (SNAG_BINARY_BATCH_TARGET -
            SNAG_BINARY_BATCH_HEADER_SIZE - SNAG_BINARY_BATCH_FOOTER_SIZE) /
            SNAG_BINARY_RECORD_HEADER_SIZE;
        struct snag_binary_record *records = calloc(count, sizeof(*records));
        unsigned char *payload = large ? calloc(1u, SNAG_MAX_EVENT_LINE) : NULL;
        assert(records && (!large || payload));
        if (large) {
            payload[0] = 17u;
            payload[SNAG_MAX_EVENT_LINE - 1u] = 29u;
        }
        for (uint32_t i = 0u; i < count; ++i) {
            records[i] = record("");
            records[i].payload = payload;
            records[i].size = large ? SNAG_MAX_EVENT_LINE : 0u;
        }
        if (!large) {
            assert(snag_binary_io_submit(io, records, count + 1u, 0u) < 0);
        }
        assert(!snag_binary_io_submit(io, records, count, 0u));
        free(records);
        free(payload);
        struct snag_binary_io_result result;
        assert(!await_result(io, &result));
        assert(!snag_binary_io_close(io));
        size_t wire_size;
        assert(!snag_binary_wire_size(large ? SNAG_BINARY_BATCH_MAX :
            SNAG_BINARY_BATCH_TARGET, &wire_size));
        assert(result.durable.next_seq == count + 1u &&
            result.durable.end - before.end == wire_size);
        struct snag_buf bytes = {.max = SNAG_BINARY_BATCH_MAX};
        struct snag_binary_batch batch;
        struct snag_binary_anchor after;
        assert(!snag_binary_batch_read(fd, result.durable.end, &before, &bytes, &batch, &after));
        assert(same_anchor(&result.durable, &after) && batch.count == count);
        if (large) {
            struct snag_binary_record found;
            size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
            uint64_t sequence;
            assert(!snag_binary_record_next(&batch, &offset, &found, &sequence));
            assert(found.size == SNAG_MAX_EVENT_LINE && found.payload[0] == 17u &&
                found.payload[found.size - 1u] == 29u);
        }
        snag_buf_free(&bytes);
        assert(!close(fd));
    }
}

static void
test_rejected_input(void)
{
    struct snag_binary_anchor before;
    int fd = journal_fd(&before);
    assert(!snag_binary_io_start(-1, &before, NULL));
    struct probe probe = {0};
    struct snag_binary_io *io = start_owner(fd, &before, &probe);
    struct snag_binary_record data = record("valid");
    assert(snag_binary_io_submit(io, NULL, 1u, 0u) < 0);
    assert(snag_binary_io_submit(io, &data, 0u, 0u) < 0);
    assert(snag_binary_io_submit(io, &data, UINT32_MAX, 0u) < 0);
    data.size = SNAG_MAX_EVENT_LINE + 1u;
    assert(snag_binary_io_submit(io, &data, 1u, 0u) < 0);
    data = record("valid");
    data.kind = 0u;
    assert(!snag_binary_io_submit(io, &data, 1u, 0u));
    struct snag_binary_io_result result;
    assert(await_result(io, &result) < 0 && !result.retryable &&
        same_anchor(&result.written, &before) && same_anchor(&result.durable, &before));
    data = record("valid");
    assert(!snag_binary_io_submit(io, &data, 1u, 0u));
    assert(!await_result(io, &result));
    assert(!snag_binary_io_close(io));
    assert(probe.writes == 1u && probe.syncs == 1u && result.durable.next_seq == 2u);
    verify_batch(fd, &before, &result.durable, &data, 1u);
    assert(!close(fd));
}

static void
native_effect(void *opaque, const struct snag_session *session, uint64_t sequence,
    const char *type, const json_t *data)
{
    struct probe *probe = opaque;
    assert(pthread_equal(probe->caller, pthread_self()));
    assert(sequence + 1u == session->next_seq && type && data);
    struct snag_binary_anchor boundary;
    struct snag_binary_index_tree tree;
    struct snag_binary_checkpoint_sources sources = {0};
    char error[256];
    assert(!snag_session_binary_checkpoint_capture(session, &boundary, &tree,
        &sources, error, sizeof(error)));
    assert(boundary.next_seq == session->next_seq && tree.count == sequence &&
        sources.texts.through == sequence);
    snag_binary_checkpoint_sources_free(&sources);
    atomic_fetch_add(&probe->effects, 1u);
}

static void
native_fixture(struct snag_session *session, struct probe *probe)
{
    struct snag_binary_anchor before;
    snag_session_init(session);
    session->log_fd = journal_fd(&before);
    session->lock_fd = dup(session->log_fd);
    assert(session->lock_fd >= 0);
    session->log_end = (int64_t)before.end;
    strcpy(session->id, "11000000000000000000000000000000");
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0u; i < sizeof(before.digest); ++i) {
        session->prev_sha256[i * 2u] = digits[before.digest[i] >> 4u];
        session->prev_sha256[i * 2u + 1u] = digits[before.digest[i] & 15u];
    }
    session->prev_sha256[SNAG_SHA256_HEX_LEN] = '\0';
    struct snag_binary_identity identity = {.created_ms = 42u};
    identity.id[0] = 17u;
    struct snag_binary_index_tree tree = {0};
    struct snag_binary_producer producer = {0};
    struct snag_binary_checkpoint_sources sources = {0};
    struct snag_binary_io_ops ops = {
        .write_full = probe_write, .sync_file = probe_sync, .opaque = probe};
    char error[256];
    struct snag_binary_anchor bad = before;
    ++bad.end;
    assert(snag_session_bind_binary(session, &identity, &bad, &tree, &producer, &sources,
        &ops, error, sizeof(error)) < 0 && errno == EINVAL && !session->binary);
    probe->caller = pthread_self();
    assert(!snag_session_bind_binary(session, &identity, &before, &tree, &producer, &sources,
        &ops, error, sizeof(error)));
    session->on_commit = native_effect;
    session->on_commit_opaque = probe;
    assert(!snag_session_commit(session, "session_created",
        json_pack("{s:s,s:s,s:s,s:i,s:s,s:s}", "default_effort", "medium",
            "default_model", "gpt-5", "default_provider", "openai", "format", 4,
            "protocol", "responses", "cwd", "/"), NULL, error, sizeof(error)));
    assert(session->next_seq == 2u && session->last_time_ms == 42u);
    assert(session->committed_start == (int64_t)before.end &&
        session->committed_end == session->log_end);
    assert(atomic_load(&probe->effects) == 1u);
}

/* Compare the ACK snapshot with independent prefix replay, not the provisional
 * producer's own staged state. This also works beside a failed physical suffix. */
static void
native_checkpoint_matches(struct snag_session *session)
{
    struct snag_binary_anchor boundary;
    struct snag_binary_index_tree tree;
    struct snag_binary_checkpoint_sources captured = {0}, replayed = {0};
    struct snag_session restored;
    snag_session_init(&restored);
    struct snag_binary_recovery recovery;
    char error[256];
    assert(!snag_session_binary_checkpoint_capture(session, &boundary, &tree,
        &captured, error, sizeof(error)));
    assert(boundary.next_seq == session->next_seq && tree.count == session->next_seq - 1u &&
        captured.texts.through == session->next_seq - 1u);
    assert(!snag_store_reconcile_binary_prefix(session, &restored, &boundary, NULL,
        NULL, &recovery, &replayed, error, sizeof(error)));
    struct snag_buf live = {.max = SIZE_MAX}, replay = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_core_encode(&live, &captured, session));
    assert(!snag_binary_checkpoint_core_encode(&replay, &replayed, &restored));
    assert(live.len == replay.len && !memcmp(live.data, replay.data, live.len));
    json_t *left = snag_checkpoint_state_encode(session);
    json_t *right = snag_checkpoint_state_encode(&restored);
    assert(left && right && json_equal(left, right));
    json_decref(left);
    json_decref(right);
    snag_buf_free(&live);
    snag_buf_free(&replay);
    snag_binary_checkpoint_sources_free(&captured);
    snag_binary_checkpoint_sources_free(&replayed);
    snag_session_close(&restored);
}

static void
native_checkpoint_commit(struct snag_session *session, const char *type, json_t *data)
{
    char error[256];
    assert(data);
    int rc = snag_session_commit(session, type, data, NULL, error, sizeof(error));
    if (rc < 0) fprintf(stderr, "native checkpoint fixture %s: %s\n", type, error);
    assert(!rc);
    native_checkpoint_matches(session);
}

static void
test_native_checkpoint_origins(void)
{
    struct snag_session session;
    struct probe probe = {0};
    native_fixture(&session, &probe);
    native_checkpoint_matches(&session);
    char id[SNAG_ID_HEX_LEN + 1u];
    for (unsigned int i = 1u; i <= 12u; ++i) {
        assert(snprintf(id, sizeof(id), "%032x", i) == SNAG_ID_HEX_LEN);
        native_checkpoint_commit(&session, "future_turn_queued", json_pack(
            "{s:s,s:s,s:b,s:s,s:[{s:s,s:s}]}", "queue_id", id, "text", "original text",
            "read_only", 0, "while_turn_id", "", "content", "type", "input_text",
            "text", "original content"));
    }
    struct snag_binary_anchor boundary;
    struct snag_binary_index_tree tree;
    struct snag_binary_checkpoint_sources captured = {0}, again = {0};
    char error[256];
    assert(!snag_session_binary_checkpoint_capture(&session, &boundary, &tree,
        &captured, error, sizeof(error)));
    assert(captured.queue_count == 12u && captured.queue[0].creation == 2u &&
        captured.queue[0].text == 2u);
    assert(!snag_binary_checkpoint_sources_clone(&again, &captured));
    assert(again.queue != captured.queue && again.queue_count == captured.queue_count &&
        !memcmp(again.queue, captured.queue, captured.queue_count * sizeof(*captured.queue)));
    struct snag_binary_checkpoint_sources saved = again, bad = captured;
    bad.queue_count = SIZE_MAX;
    assert(snag_binary_checkpoint_sources_clone(&again, &bad) < 0 && errno == EOVERFLOW &&
        !memcmp(&saved, &again, sizeof(saved)));
    assert(snag_binary_checkpoint_sources_clone(&again, &again) < 0 && errno == EINVAL &&
        !memcmp(&saved, &again, sizeof(saved)));
    captured.queue[0].text = UINT64_MAX; /* Returned copies never mutate the live owner. */
    native_checkpoint_commit(&session, "future_turn_edited", json_pack("{s:s,s:s,s:b,s:b}",
        "queue_id", "00000000000000000000000000000001", "text", "edited text",
        "read_only", 0, "armed", 1));
    assert(!snag_session_binary_checkpoint_capture(&session, &boundary, &tree,
        &captured, error, sizeof(error)));
    assert(captured.queue[0].creation == 2u && captured.queue[0].text == 14u &&
        again.queue[0].text == 2u);
    snag_binary_checkpoint_sources_free(&again);
    snag_binary_checkpoint_sources_free(&captured);
    for (unsigned int i = 1u; i <= 12u; ++i) {
        assert(snprintf(id, sizeof(id), "%032x", i) == SNAG_ID_HEX_LEN);
        native_checkpoint_commit(&session, "download_queued", json_pack(
            "{s:s,s:s,s:s,s:I,s:I,s:s,s:I}", "id", id, "path", "/tmp/native-checkpoint",
            "name", "download", "bytes", (json_int_t)i, "mtime", (json_int_t)0, "sha256",
            "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
            "queued_ms", (json_int_t)17));
    }
    assert(!snag_session_binary_checkpoint_capture(&session, &boundary, &tree,
        &captured, error, sizeof(error)));
    assert(captured.download_count == 12u && captured.downloads[0].receipt == 15u);
    assert(!snag_binary_checkpoint_sources_clone(&again, &captured));
    assert(again.downloads != captured.downloads &&
        !memcmp(again.downloads, captured.downloads,
            captured.download_count * sizeof(*captured.downloads)));
    snag_binary_checkpoint_sources_free(&again);
    snag_binary_checkpoint_sources_free(&captured);
    native_checkpoint_commit(&session, "download_removed", json_pack("{s:s,s:s}",
        "id", "00000000000000000000000000000003", "reason", "fixture"));
    native_checkpoint_commit(&session, "downloads_cleared",
        json_pack("{s:s}", "reason", "fixture"));
    for (unsigned int i = 1u; i <= 12u; ++i) {
        assert(snprintf(id, sizeof(id), "%032x", i) == SNAG_ID_HEX_LEN);
        native_checkpoint_commit(&session, "future_turn_cancelled",
            json_pack("{s:[s],s:s}", "queue_ids", id, "reason", "user"));
    }
    for (unsigned int i = 0u; i < 2u; ++i) {
        native_checkpoint_commit(&session, "banner_updated", json_pack("{s:s}",
            "text", "same bytes, distinct declarations"));
        assert(!snag_session_binary_checkpoint_capture(&session, &boundary, &tree,
            &captured, error, sizeof(error)));
        assert(captured.texts.slots[SNAG_BINARY_TEXT_BANNER].declaration == session.next_seq - 1u);
        snag_binary_checkpoint_sources_free(&captured);
    }
    native_checkpoint_commit(&session, "banner_updated", json_pack("{s:s}", "text", ""));
    assert(!snag_session_binary_checkpoint_capture(&session, &boundary, &tree,
        &captured, error, sizeof(error)));
    assert(!captured.queue_count && !captured.download_count &&
        !captured.texts.slots[SNAG_BINARY_TEXT_BANNER].declaration);
    snag_binary_checkpoint_sources_free(&captured);
    native_checkpoint_commit(&session, "service_tier_changed",
        json_pack("{s:s}", "value", "priority"));
    assert(!strcmp(session.service_tier, "priority"));
    assert(!snag_session_binary_checkpoint_capture(&session, &boundary, &tree,
        &captured, error, sizeof(error)));
    assert(captured.texts.slots[SNAG_BINARY_TEXT_SERVICE_TIER].declaration ==
        session.next_seq - 1u);
    struct snag_buf service_wire = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_texts_encode(&service_wire, &captured.texts));
    assert(service_wire.len == 310u && service_wire.data[0] == 2u);
    snag_buf_free(&service_wire);
    snag_binary_checkpoint_sources_free(&captured);
    struct snag_buf bad_tier = {.max = SIZE_MAX};
    enum snag_binary_kind kind;
    json_t *bad_value = json_pack("{s:s}", "value", "unknown");
    assert(snag_binary_legacy_encode(&bad_tier, "service_tier_changed", bad_value, &kind) < 0 &&
        errno == EINVAL && !bad_tier.len);
    json_decref(bad_value);
    json_t *valid_value = json_pack("{s:s}", "value", "priority");
    assert(!snag_binary_legacy_encode(&bad_tier, "service_tier_changed", valid_value, &kind));
    assert(kind == SNAG_BINARY_SERVICE_TIER_CHANGED && bad_tier.len >= 8u);
    struct snag_binary_record tier_record = {.kind = (uint16_t)kind,
        .version = snag_binary_event_version(kind), .payload = bad_tier.data, .size = bad_tier.len};
    struct snag_binary_event tier_event;
    assert(!snag_binary_event_decode(&tier_record, &tier_event));
    memcpy(bad_tier.data + bad_tier.len - 8u, "invalid!", 8u);
    assert(snag_binary_event_decode(&tier_record, &tier_event) < 0 && errno == EINVAL);
    json_decref(valid_value);
    snag_buf_free(&bad_tier);
    native_checkpoint_commit(&session, "service_tier_changed",
        json_pack("{s:s}", "value", "default"));
    assert(!strcmp(session.service_tier, "default"));
    snag_session_close(&session);
    memset(&boundary, 0xa5, sizeof(boundary));
    memset(&tree, 0x5a, sizeof(tree));
    struct snag_binary_anchor saved_boundary = boundary;
    struct snag_binary_index_tree saved_tree = tree;
    captured.texts.through = UINT64_MAX;
    assert(snag_session_binary_checkpoint_capture(&session, &boundary, &tree,
        &captured, error, sizeof(error)) < 0 && errno == ENOTSUP &&
        !memcmp(&saved_boundary, &boundary, sizeof(boundary)) &&
        !memcmp(&saved_tree, &tree, sizeof(tree)) && captured.texts.through == UINT64_MAX);
    snag_binary_checkpoint_sources_free(&captured);
}

static json_t *
native_goal(const char *prompt)
{
    return json_pack("{s:s,s:s}", "goal_id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "prompt", prompt);
}

struct native_call {
    struct snag_session *session;
    struct probe *probe;
    uint64_t sequence;
    int rc;
};

static void *
native_call_main(void *opaque)
{
    struct native_call *call = opaque;
    call->probe->caller = pthread_self();
    char error[256];
    call->rc = snag_session_commit(call->session, "goal_started", native_goal("native goal"),
        &call->sequence, error, sizeof(error));
    return NULL;
}

static void
test_native_session_ack(void)
{
    struct snag_session session;
    struct probe probe = {0};
    native_fixture(&session, &probe);
    uint64_t before_seq = session.next_seq;
    int64_t before_end = session.log_end;
    enum snag_goal_status before_goal = session.goal_status;
    int64_t before_start = session.committed_start;
    probe.block_sync = true;
    atomic_store(&probe.sync_entered, false);
    struct native_call call = {.session = &session, .probe = &probe};
    pthread_t caller;
    assert(!pthread_create(&caller, NULL, native_call_main, &call));
    wait_flag(&probe.sync_entered);
    assert(session.next_seq == before_seq && session.log_end == before_end &&
        session.goal_status == before_goal && atomic_load(&probe.effects) == 1u);
    assert(session.committed_start == before_start && session.committed_end == before_end);
    atomic_store(&probe.release_sync, true);
    assert(!pthread_join(caller, NULL));
    assert(!call.rc && call.sequence == before_seq && session.next_seq == before_seq + 1u);
    assert(session.log_end > before_end && !strcmp(session.goal_prompt, "native goal"));
    assert(session.committed_start == before_end && session.committed_end == session.log_end);
    assert(atomic_load(&probe.effects) == 2u);
    struct snag_session restored;
    snag_session_init(&restored);
    struct snag_binary_recovery recovery;
    struct snag_binary_checkpoint_sources sources = {0};
    char error[256];
    assert(!snag_store_reconcile_binary(&session, &restored, NULL, NULL, &recovery,
        &sources, error, sizeof(error)));
    assert(restored.next_seq == session.next_seq && restored.log_end == session.log_end &&
        !strcmp(restored.prev_sha256, session.prev_sha256) &&
        !strcmp(restored.goal_prompt, session.goal_prompt));
    snag_binary_checkpoint_sources_free(&sources);
    snag_session_close(&restored);
    snag_session_close(&session);
}

static void
test_native_session_retry(bool partial, unsigned int failures)
{
    struct snag_session session;
    struct probe probe = {0};
    native_fixture(&session, &probe);
    uint64_t before_seq = session.next_seq;
    int64_t before_end = session.log_end;
    json_t *data = native_goal("retained native goal");
    int64_t before_start = session.committed_start;
    assert(data);
    if (partial) probe.partial_failures = failures;
    else probe.sync_failures = failures;
    char error[256];
    uint64_t sequence = 999u;
    assert(snag_session_commit(&session, "goal_started", json_incref(data), &sequence,
        error, sizeof(error)) < 0 && errno == EIO);
    assert(session.next_seq == before_seq && session.log_end == before_end &&
        sequence == 999u && atomic_load(&probe.effects) == 1u);
    assert(session.committed_start == before_start && session.committed_end == before_end);
    native_checkpoint_matches(&session);
    size_t writes = probe.writes, syncs = probe.syncs;
    assert(snag_session_commit(&session, "goal_started", native_goal("different"), &sequence,
        error, sizeof(error)) < 0 && errno == EBUSY);
    assert(probe.writes == writes && probe.syncs == syncs);
    assert(!json_object_set_new(data, "prompt", json_string("mutated caller")));
    assert(snag_session_commit(&session, "goal_started", json_incref(data), &sequence,
        error, sizeof(error)) < 0 && errno == EBUSY);
    assert(probe.writes == writes && probe.syncs == syncs);
    assert(!json_object_set_new(data, "prompt", json_string("retained native goal")));
    int rc = snag_session_commit(&session, "goal_started", json_incref(data), &sequence,
        error, sizeof(error));
    if (failures == 1u) {
        assert(!rc && sequence == before_seq && session.next_seq == before_seq + 1u);
        assert(session.committed_start == before_end && session.committed_end == session.log_end);
        assert(!strcmp(session.goal_prompt, "retained native goal"));
        assert(atomic_load(&probe.effects) == 2u);
        native_checkpoint_matches(&session);
    } else {
        assert(rc < 0 && errno == EIO && session.next_seq == before_seq &&
            session.log_end == before_end && atomic_load(&probe.effects) == 1u);
        assert(session.committed_start == before_start && session.committed_end == before_end);
        writes = probe.writes;
        syncs = probe.syncs;
        assert(snag_session_commit(&session, "goal_started", json_incref(data), &sequence,
            error, sizeof(error)) < 0 && errno == EIO);
        assert(probe.writes == writes && probe.syncs == syncs);
    }
    json_decref(data);
    snag_session_close(&session);
}

static void
test_native_result_retry(void)
{
    struct snag_session session;
    struct probe probe = {0};
    native_fixture(&session, &probe);
    /* Inject accepted engine-owned process state to isolate result admission
     * and ACK ownership. This fixture does not qualify production lifecycle. */
    session.active_turn = true;
    strcpy(session.active_turn_id, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    session.processes = calloc(1u, sizeof(*session.processes));
    assert(session.processes);
    session.process_count = session.process_capacity = 1u;
    struct snag_process_state *process = session.processes;
    strcpy(process->handle, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    process->log_offset = (uint64_t)session.log_end;
    process->log_seq = session.next_seq;
    strcpy(process->log_hash, session.prev_sha256);
    json_t *result = snag_tool_result_terminal(true, "fixture result");
    assert(result && !snag_json_set_new(result, "max_output_tokens", json_integer(16000)));
    assert(result && !snag_json_set_new(result, "output_ref",
        json_pack("{s:s,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:b,s:I,s:I}",
            "handle", process->handle, "stdout_start", 0, "stdout_end", 0,
            "stderr_start", 0, "stderr_end", 0, "stdin_accepted", 0,
            "stdin_written", 0, "stdin_pending", 0, "stdin_open", 0,
            "log_start", (json_int_t)session.log_end, "log_end", (json_int_t)session.log_end)));
    json_t *data = json_pack("{s:s,s:s,s:s,s:o}", "turn_id", session.active_turn_id,
        "handle", process->handle, "cause", "user_interrupt", "result", result);
    assert(data);
    probe.sync_failures = 1u;
    char error[256];
    int64_t end = session.log_end;
    int rc = snag_session_commit(&session, "process_closed", json_incref(data), NULL,
        error, sizeof(error));
    if (rc >= 0 || errno != EIO)
        fprintf(stderr, "native result failure: rc=%d errno=%d %s\n", rc, errno, error);
    assert(rc < 0 && errno == EIO);
    assert(session.next_seq == 2u && session.log_end == end && session.process_count == 1u &&
        atomic_load(&probe.effects) == 1u);
    assert(!snag_session_commit(&session, "process_closed", data, NULL, error, sizeof(error)));
    assert(session.next_seq == 3u && !session.process_count && atomic_load(&probe.effects) == 2u);
    struct snag_buf bytes = {.max = SNAG_BINARY_BATCH_MAX};
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    struct snag_binary_identity identity;
    struct snag_binary_anchor before, created, after;
    struct snag_binary_batch batch;
    assert(snag_pread(session.log_fd, header, sizeof(header), 0) == (ssize_t)sizeof(header));
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &before));
    assert(!snag_binary_batch_read(session.log_fd, (uint64_t)session.log_end,
        &before, &bytes, &batch, &created));
    assert(!snag_binary_batch_read(session.log_fd, (uint64_t)session.log_end,
        &created, &bytes, &batch, &after));
    size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
    uint64_t sequence;
    struct snag_binary_record record;
    struct snag_binary_event event;
    assert(!snag_binary_record_next(&batch, &offset, &record, &sequence));
    assert(sequence == 2u && !snag_binary_event_decode(&record, &event));
    assert(event.kind == SNAG_BINARY_PROCESS_CLOSED &&
        event.data.process_closed.result.output_ref.native);
    struct snag_binary_tool_output_ref *ref = &event.data.process_closed.result.output_ref;
    assert(ref->first_sequence == 2u && ref->end_sequence == 2u &&
        ref->log_start == (uint64_t)end && ref->log_end == (uint64_t)end);
    snag_buf_free(&bytes);
    snag_session_close(&session);
}

static json_t *
native_voice_record(const struct snag_session *session)
{
    /* The offered archive can contain observations originating in an older
     * source session. Its enclosed goal is data, never a destination goal. */
    return json_pack("{s:s,s:s,s:s,s:i,s:s,s:{s:s,s:s}}",
        "transfer_id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "target_session_id", session->id,
        "source_session_id", "dddddddddddddddddddddddddddddddd", "source_seq", 1,
        "source_type", "goal_started", "data",
        "goal_id", "cccccccccccccccccccccccccccccccc", "prompt", "inert archived goal");
}

static json_t *
native_voice_adoption(const struct snag_session *session, const struct snag_journal_cursor *begin,
    uint64_t count)
{
    return json_pack("{s:s,s:s,s:s,s:i,s:I,s:I,s:s,s:I}",
        "transfer_id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "target_session_id", session->id,
        "source_session_id", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "source_as_of_seq", 2,
        "begin_offset", (json_int_t)begin->offset, "begin_seq", (json_int_t)begin->next_seq,
        "begin_sha256", begin->prev_sha256, "count", (json_int_t)count);
}

static void
test_native_voice_import(unsigned int variant)
{
    struct snag_session session;
    struct probe probe = {0};
    native_fixture(&session, &probe);
    const char *id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *source = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    struct snag_journal_cursor begin = {.offset = session.log_end, .next_seq = session.next_seq};
    strcpy(begin.prev_sha256, session.prev_sha256);
    char error[256];
    uint64_t count = variant == 7u ? 2u : 1u;
    struct snag_journal_cursor canary = {.offset = -42, .next_seq = 99u}, saved = canary;
    assert(snag_session_voice_import_cursor(&session, id, session.id, 2u, 1u,
        &canary, error, sizeof(error)) < 0 && errno == EINVAL);
    assert(!memcmp(&canary, &saved, sizeof(canary)));
    if (variant != 5u) {
        assert(!snag_session_voice_import_cursor(&session, id, source, 2u, count,
            &begin, error, sizeof(error)));
        assert(begin.offset == session.log_end && begin.next_seq == session.next_seq &&
            !strcmp(begin.prev_sha256, session.prev_sha256));
        assert(snag_session_voice_import_cursor(&session, id, source, 2u, count,
            &canary, error, sizeof(error)) < 0 && errno == EBUSY);
        assert(!memcmp(&canary, &saved, sizeof(canary)));
    }
    assert(!snag_session_commit(&session, "banner_updated", json_pack("{s:s}",
        "text", "destination continues"), NULL, error, sizeof(error)));
    /* The captured start stays before intervening ordinary metadata. */
    if (variant == 1u || variant == 4u) probe.sync_failures = 1u;
    if (variant == 2u) probe.partial_failures = 1u;
    json_t *record = native_voice_record(&session);
    assert(record);
    if (variant == 1u || variant == 2u || variant == 4u) {
        int64_t end = session.log_end;
        assert(snag_session_commit(&session, "voice_transfer_record", json_incref(record),
            NULL, error, sizeof(error)) < 0 && errno == EIO);
        assert(session.next_seq == 3u && session.log_end == end &&
            !session.voice_history.adopted_seq && !session.goal_id[0] &&
            atomic_load(&probe.effects) == 2u);
        if (variant == 4u) snag_session_voice_import_abandon(&session, id);
    }
    assert(!snag_session_commit(&session, "voice_transfer_record", record,
        NULL, error, sizeof(error)));
    assert(session.next_seq == 4u && !session.goal_id[0] && !session.active_turn &&
        !session.pending_queue_count && !session.voice_history.adopted_seq &&
        atomic_load(&probe.effects) == 3u);
    if (variant == 6u) {
        /* An old close must not discard another operation's capture. */
        snag_session_voice_import_abandon(&session, "cccccccccccccccccccccccccccccccc");
    }
    size_t writes = probe.writes, syncs = probe.syncs;
    int64_t before_end = session.log_end;
    if (variant == 4u || variant == 5u || variant == 7u) {
        assert(snag_session_commit(&session, "voice_transfer_adopted",
            native_voice_adoption(&session, &begin, count), NULL,
            error, sizeof(error)) < 0 && errno == EINVAL);
        assert(probe.writes == writes && probe.syncs == syncs &&
            session.next_seq == 4u && session.log_end == before_end &&
            !session.voice_history.adopted_seq && atomic_load(&probe.effects) == 3u);
        snag_session_voice_import_abandon(&session, id);
        /* Abandoned/unregistered archive bytes stay inert; a fresh operation
         * starts at the current ACK instead of acquiring an old prefix. */
        assert(!snag_session_voice_import_cursor(&session, id, source, 2u, 1u,
            &canary, error, sizeof(error)));
        assert(canary.next_seq == 4u && canary.offset == session.log_end);
        snag_session_voice_import_abandon(&session, id);
        snag_session_close(&session);
        return;
    }
    for (unsigned int bad = 0u; bad < 8u; ++bad) {
        json_t *data = native_voice_adoption(&session, &begin, count);
        assert(data);
        const char *field = NULL;
        json_t *replacement = NULL;
        switch (bad) {
        case 0u:
            field = "transfer_id";
            replacement = json_string("cccccccccccccccccccccccccccccccc");
            break;
        case 1u:
            field = "source_session_id";
            replacement = json_string("cccccccccccccccccccccccccccccccc");
            break;
        case 2u:
            field = "target_session_id";
            replacement = json_string("cccccccccccccccccccccccccccccccc");
            break;
        case 3u:
            field = "source_as_of_seq";
            replacement = json_integer(3);
            break;
        case 4u:
            field = "count";
            replacement = json_integer(2);
            break;
        case 5u:
            field = "begin_offset";
            replacement = json_integer((json_int_t)begin.offset + 1);
            break;
        case 6u:
            field = "begin_seq";
            replacement = json_integer((json_int_t)begin.next_seq + 1);
            break;
        case 7u:
            field = "begin_sha256";
            replacement = json_string(
                "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
            break;
        }
        assert(field && replacement && !snag_json_set_new(data, field, replacement));
        assert(snag_session_commit(&session, "voice_transfer_adopted", data,
            NULL, error, sizeof(error)) < 0 && errno == EINVAL);
        assert(probe.writes == writes && probe.syncs == syncs &&
            session.next_seq == 4u && session.log_end == before_end &&
            !session.voice_history.adopted_seq && atomic_load(&probe.effects) == 3u);
    }
    if (variant == 3u) {
        probe.sync_failures = 1u;
        assert(snag_session_commit(&session, "voice_transfer_adopted",
            native_voice_adoption(&session, &begin, count), NULL,
            error, sizeof(error)) < 0 && errno == EIO);
        assert(session.next_seq == 4u && session.log_end == before_end &&
            !session.voice_history.adopted_seq && atomic_load(&probe.effects) == 3u);
    }
    assert(!snag_session_commit(&session, "voice_transfer_adopted",
        native_voice_adoption(&session, &begin, count), NULL, error, sizeof(error)));
    assert(session.next_seq == 5u && session.voice_history.adopted_seq == 4u &&
        !strcmp(session.voice_history.transfer_id, id) &&
        session.voice_history.begin.offset == begin.offset &&
        session.voice_history.begin.next_seq == begin.next_seq &&
        !strcmp(session.voice_history.begin.prev_sha256, begin.prev_sha256) &&
        !session.goal_id[0] && !session.active_turn && atomic_load(&probe.effects) == 4u);
    struct snag_session restored;
    snag_session_init(&restored);
    struct snag_binary_recovery recovery;
    struct snag_binary_checkpoint_sources sources = {0};
    assert(!snag_store_reconcile_binary(&session, &restored, NULL, NULL,
        &recovery, &sources, error, sizeof(error)));
    assert(restored.voice_history.adopted_seq == 4u &&
        !strcmp(restored.voice_history.transfer_id, id) &&
        restored.voice_history.begin.offset == begin.offset &&
        restored.voice_history.begin.next_seq == begin.next_seq &&
        !strcmp(restored.voice_history.begin.prev_sha256, begin.prev_sha256) &&
        !restored.goal_id[0] && !restored.active_turn && !restored.pending_queue_count);
    snag_binary_checkpoint_sources_free(&sources);
    snag_session_close(&restored);
    assert(!snag_session_voice_import_cursor(&session, id, source, 2u, 1u,
        &canary, error, sizeof(error)));
    assert(canary.next_seq == 5u && canary.offset == session.log_end);
    snag_session_voice_import_abandon(&session, id);
    snag_session_close(&session);
}

static void
test_native_clone_failure(void)
{
    struct snag_session session;
    struct probe probe = {0};
    native_fixture(&session, &probe);
    session.processes = calloc(1u, sizeof(*session.processes));
    session.pending_calls = calloc(1u, sizeof(*session.pending_calls));
    assert(session.processes && session.pending_calls);
    session.process_count = session.process_capacity = 1u;
    session.pending_call_count = 1u;
    /* Force the first vector reservation to fail before the process vector is
     * copied. No invalid geometry is dereferenced on this failure path. */
    session.pending_call_capacity = SIZE_MAX / sizeof(*session.pending_calls);
    char error[256];
    assert(snag_session_commit(&session, "goal_started", native_goal("uncommitted"), NULL,
        error, sizeof(error)) < 0 && errno == ENOMEM);
    assert(session.processes && session.process_count == 1u && session.next_seq == 2u &&
        atomic_load(&probe.effects) == 1u);
    session.pending_call_capacity = 1u;
    snag_session_close(&session); /* Owning process storage was never freed by the failed clone. */
}

void
test_store_binary_io(void)
{
    test_native_checkpoint_origins();
    test_native_session_ack();
    test_native_session_retry(true, 1u);
    test_native_session_retry(false, 1u);
    test_native_session_retry(true, 2u);
    test_native_session_retry(false, 2u);
    test_native_clone_failure();
    test_native_result_retry();
    for (unsigned int variant = 0u; variant < 8u; ++variant)
        test_native_voice_import(variant);
    test_group_ack();
    test_failed_commit(true, 1u);
    test_failed_commit(false, 1u);
    test_failed_commit(true, 2u);
    test_failed_commit(false, 2u);
    test_changed_tail(false);
    test_changed_tail(true);
    test_native_partial_write();
    test_batch_limits();
    test_rejected_input();
}
