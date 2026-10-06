/* SPDX-License-Identifier: GPL-2.0-only */
#include "fixture_store_binary.h"
#include "store_binary_io.h"
#include "store_binary_index.h"
#include "store_binary_wire.h"
#include "fs.h"
#include "irc.h"
#include "snajpagent.h"
#include "store_internal.h"
#include "store_binary_replay.h"
#include "store_binary_legacy.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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
    bool index_enabled;
    int index_fd;
    unsigned int index_failures;
    size_t index_writes;
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
    if (probe->index_enabled && fd == probe->index_fd) {
        ++probe->index_writes;
        if (probe->index_failures) {
            --probe->index_failures;
            assert(!snag_write_full(fd, data, size / 2u));
            return snag_errno(EIO);
        }
    }
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

static void
producer_access(struct snag_session *session, const struct snag_binary_anchor *boundary,
    const struct snag_binary_index_tree *tree, struct snag_binary_checkpoint_index *access,
    struct snag_buf *entries)
{
    struct snag_binary_identity identity = {.created_ms = 42u};
    identity.id[0] = 17u;
    assert(tree->count <= SIZE_MAX / SNAG_BINARY_INDEX_ENTRY_SIZE);
    size_t size = (size_t)tree->count * SNAG_BINARY_INDEX_ENTRY_SIZE;
    assert(!snag_buf_reserve(entries, size));
    entries->len = size;
    struct snag_binary_anchor cursor = *boundary;
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf batch_entries = {.max = SNAG_BINARY_INDEX_BATCH_MAX};
    while (cursor.end > SNAG_BINARY_HEADER_SIZE) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor before;
        assert(!snag_binary_batch_previous(session->log_fd, &cursor, &scratch, &batch, &before));
        snag_buf_reset(&batch_entries);
        assert(!snag_binary_index_append_batch(&batch_entries, &identity, &before,
            &cursor, batch.data, batch.size));
        assert(batch_entries.len == (cursor.next_seq - before.next_seq) *
            SNAG_BINARY_INDEX_ENTRY_SIZE);
        memcpy(entries->data + (before.next_seq - 1u) * SNAG_BINARY_INDEX_ENTRY_SIZE,
            batch_entries.data, batch_entries.len);
        cursor = before;
    }
    *access = (struct snag_binary_checkpoint_index){.identity = identity,
        .boundary = *boundary, .tree = *tree, .entries = entries->data,
        .entry_count = (size_t)tree->count};
    snag_buf_free(&scratch);
    snag_buf_free(&batch_entries);
}

struct producer_cancel {
    size_t calls, at;
};

static bool
producer_cancelled(void *opaque)
{
    struct producer_cancel *cancel = opaque;
    return ++cancel->calls == cancel->at;
}

static void
producer_matches(struct snag_session *session, const struct snag_binary_anchor *boundary,
    const struct snag_binary_index_tree *tree, const struct snag_buf *core)
{
    struct snag_binary_checkpoint_index access;
    struct snag_buf entries = {.max = SIZE_MAX};
    producer_access(session, boundary, tree, &access, &entries);
    struct snag_binary_checkpoint_frame frame = {.identity = access.identity,
        .boundary = *boundary, .core = {.version = SNAG_BINARY_CORE_VERSION,
            .data = core->data, .size = core->len}};
    struct snag_session state;
    struct snag_binary_checkpoint_sources sources = {0};
    assert(!snag_binary_checkpoint_core_read(session->log_fd, &frame, &access, &state, &sources));
    struct snag_binary_producer producer = {0};
    struct producer_cancel cancel = {0};
    int64_t position = snag_seek(session->log_fd, 0, SEEK_CUR);
    int rc = snag_binary_producer_restore(&producer, session->log_fd, boundary, &access,
        &sources, &state, producer_cancelled, &cancel);
    if (rc < 0) fprintf(stderr, "producer restore at %llu: %s\n",
        (unsigned long long)boundary->next_seq, strerror(errno));
    assert(!rc && snag_seek(session->log_fd, 0, SEEK_CUR) == position);
    assert(producer.input.creation == sources.input && producer.queue_count == sources.queue_count);
    for (size_t i = 0u; i < producer.queue_count; ++i) {
        assert(producer.queue[i].creation == sources.queue[i].creation);
        assert(producer.queue[i].text.target.sequence == sources.queue[i].text);
        assert(!producer.queue[i].paths);
        assert(producer.queue[i].content.target.sequence ==
            (state.pending_queue[i].content ? sources.queue[i].creation : 0u));
    }
    if (state.response_open) {
        assert(producer.output_count == json_array_size(state.response_public));
        assert((!producer.public && !state.response_public) ||
            json_equal(producer.public, state.response_public));
        for (size_t i = 0u; i < producer.output_count; ++i) {
            const struct snag_binary_output_source *source = &producer.outputs[i];
            assert(source->index == i &&
                source->value.source.first.sequence > sources.response_start);
            assert(source->value.source.last_sequence <= sources.response_end);
            assert(!source->value.item.text.data && !source->value.item.provider_id.data);
            const char *text = snag_json_string(json_array_get(state.response_public, i), "text");
            assert(text && source->value.source.bytes == strlen(text));
        }
    } else {
        assert(!producer.output_count && !producer.public);
    }
    unsigned char saved[sizeof(producer)];
    memcpy(saved, &producer, sizeof(producer));
    assert(snag_binary_producer_restore(&producer, session->log_fd, boundary, NULL,
        &sources, &state, NULL, NULL) < 0 && errno == EINVAL);
    assert(!memcmp(saved, &producer, sizeof(producer)));
    struct snag_binary_anchor bad_clock = *boundary;
    bad_clock.next_seq = 0u;
    assert(snag_binary_producer_restore(&producer, session->log_fd, &bad_clock, &access,
        &sources, &state, NULL, NULL) < 0 && errno == EINVAL);
    if (sources.input) {
        struct snag_binary_checkpoint_sources wrong = sources;
        wrong.input = 1u; /* A canonical creation record is not an input declaration. */
        assert(snag_binary_producer_restore(&producer, session->log_fd, boundary, &access,
            &wrong, &state, NULL, NULL) < 0 && errno == EINVAL);
    }
    if (state.response_open) {
        struct snag_session wrong = state;
        wrong.active_cycle ^= 1u;
        assert(snag_binary_producer_restore(&producer, session->log_fd, boundary, &access,
            &sources, &wrong, NULL, NULL) < 0 && errno == EINVAL);
        wrong = state;
        wrong.response_public_bytes ^= 1u;
        assert(snag_binary_producer_restore(&producer, session->log_fd, boundary, &access,
            &sources, &wrong, NULL, NULL) < 0 && errno == EINVAL);
        if (producer.output_count > 1u) {
            struct snag_binary_checkpoint_sources early = sources;
            early.response_end = producer.outputs[0].value.source.last_sequence;
            assert(snag_binary_producer_restore(&producer, session->log_fd, boundary, &access,
                &early, &state, NULL, NULL) < 0 && errno == EINVAL);
        }
    }
    assert(!memcmp(saved, &producer, sizeof(producer)));
    size_t calls = cancel.calls;
    for (size_t at = 1u; at <= calls; ++at) {
        cancel = (struct producer_cancel){.at = at};
        assert(snag_binary_producer_restore(&producer, session->log_fd, boundary, &access,
            &sources, &state, producer_cancelled, &cancel) < 0 && errno == ECANCELED);
        assert(!memcmp(saved, &producer, sizeof(producer)));
    }
    uint64_t missing = sources.input ? sources.input : sources.queue_count ?
        sources.queue[0].creation : state.response_open ? sources.response_start : 0u;
    if (missing) {
        struct snag_buf sparse = {.max = SIZE_MAX};
        size_t offset = (size_t)(missing - 1u) * SNAG_BINARY_INDEX_ENTRY_SIZE;
        assert(!snag_buf_append(&sparse, entries.data, offset));
        assert(!snag_buf_append(&sparse, entries.data + offset + SNAG_BINARY_INDEX_ENTRY_SIZE,
            entries.len - offset - SNAG_BINARY_INDEX_ENTRY_SIZE));
        struct snag_binary_checkpoint_index incomplete = access;
        incomplete.entries = sparse.data;
        --incomplete.entry_count;
        assert(snag_binary_producer_restore(&producer, session->log_fd, boundary, &incomplete,
            &sources, &state, NULL, NULL) < 0 && errno == ENOENT);
        assert(!memcmp(saved, &producer, sizeof(producer)));
        snag_buf_free(&sparse);
    }
    assert(snag_seek(session->log_fd, 0, SEEK_CUR) == position);
    snag_binary_producer_free(&producer);
    snag_binary_checkpoint_sources_free(&sources);
    snag_session_close(&state);
    snag_buf_free(&entries);
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
    producer_matches(session, &boundary, &tree, &live);
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
producer_resume_owner(struct snag_session *session, struct probe *probe)
{
    struct snag_binary_anchor boundary;
    struct snag_binary_index_tree tree;
    struct snag_binary_checkpoint_sources captured = {0}, sources = {0};
    char error[256];
    assert(!snag_session_binary_checkpoint_capture(session, &boundary, &tree,
        &captured, error, sizeof(error)));
    struct snag_buf core = {.max = SIZE_MAX}, entries = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_core_encode(&core, &captured, session));
    struct snag_binary_checkpoint_index access;
    producer_access(session, &boundary, &tree, &access, &entries);
    struct snag_binary_checkpoint_frame frame = {.identity = access.identity,
        .boundary = boundary, .core = {.version = SNAG_BINARY_CORE_VERSION,
            .data = core.data, .size = core.len}};
    struct snag_session restored;
    assert(!snag_binary_checkpoint_core_read(session->log_fd, &frame, &access,
        &restored, &sources));
    struct snag_binary_producer producer = {0};
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    struct snag_binary_checkpoint_index suffix = {0};
    assert(snag_pread(session->log_fd, header, sizeof(header), 0) == (ssize_t)sizeof(header));
    assert(!snag_binary_header_decode(header, sizeof(header), &suffix.identity, &suffix.boundary));
    /* The entire small fixture prefix is this caller-bounded suffix. No NULL
     * access or unbounded repair/oracle fallback is passed to restore. */
    assert(!snag_binary_producer_restore(&producer, session->log_fd, &boundary,
        &suffix, &sources, &restored, NULL, NULL));
    int fd = dup(session->log_fd);
    assert(fd >= 0);
    snag_session_close(session); /* Join the former owner before replacing it. */
    *session = restored;
    session->log_fd = fd;
    session->lock_fd = dup(fd);
    assert(session->lock_fd >= 0 && !snag_lock_file(session->lock_fd, true));
    probe->owner_seen = false;
    struct snag_binary_io_ops ops = {.write_full = probe_write, .sync_file = probe_sync,
        .opaque = probe};
    assert(!snag_session_bind_binary(session, &access.identity, &boundary, &tree,
        &producer, &sources, &ops, error, sizeof(error)));
    session->on_commit = native_effect;
    session->on_commit_opaque = probe;
    snag_binary_producer_free(&producer);
    snag_binary_checkpoint_sources_free(&sources);
    snag_binary_checkpoint_sources_free(&captured);
    snag_buf_free(&core);
    snag_buf_free(&entries);
}

static void
producer_last_event(struct snag_session *session, struct snag_binary_event *event,
    struct snag_buf *scratch)
{
    struct snag_binary_anchor boundary, before;
    struct snag_binary_index_tree tree;
    struct snag_binary_checkpoint_sources sources = {0};
    char error[256];
    assert(!snag_session_binary_checkpoint_capture(session, &boundary, &tree,
        &sources, error, sizeof(error)));
    struct snag_binary_batch batch;
    assert(!snag_binary_batch_previous(session->log_fd, &boundary, scratch, &batch, &before));
    assert(batch.count == 1u);
    size_t position = SNAG_BINARY_BATCH_HEADER_SIZE;
    uint64_t sequence;
    struct snag_binary_record record;
    assert(!snag_binary_record_next(&batch, &position, &record, &sequence));
    assert(sequence + 1u == boundary.next_seq && !snag_binary_event_decode(&record, event));
    snag_binary_checkpoint_sources_free(&sources);
}

static void
producer_output(struct snag_session *session, unsigned int index, unsigned int offset,
    const char *text)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_RESPONSE_OUTPUT};
    struct snag_binary_response_output *output = &event.data.response_output;
    memset(output->turn, 0x11, sizeof(output->turn));
    memset(output->response, 0x33, sizeof(output->response));
    memset(output->item.id, index ? 0x55 : 0x44, sizeof(output->item.id));
    output->cycle = 1u;
    output->index = index;
    output->offset = offset;
    output->item.kind = SNAG_BINARY_ITEM_ASSISTANT;
    output->item.phase = SNAG_BINARY_PHASE_COMMENTARY;
    output->item.provider_id = (struct snag_binary_text){(const unsigned char *)"p", 1u};
    output->item.text = (struct snag_binary_text){(const unsigned char *)text, strlen(text)};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    struct snag_binary_record record = {.kind = (uint16_t)event.kind,
        .version = snag_binary_event_version(event.kind), .payload = payload.data,
        .size = payload.len};
    const char *type;
    json_t *data = NULL;
    assert(!snag_binary_legacy_decode(&record, &type, &data));
    native_checkpoint_commit(session, type, data);
    snag_buf_free(&payload);
}

static void native_query_closure(struct snag_session *, uint64_t, size_t, const char *);

static void
test_native_producer_resume(bool queued)
{
    struct snag_session session;
    struct probe probe = {0};
    native_fixture(&session, &probe);
    uint64_t creation = session.next_seq;
    if (queued) {
        native_checkpoint_commit(&session, "future_turn_queued", json_pack(
            "{s:s,s:s,s:b,s:s,s:[{s:s,s:s}]}", "queue_id",
            "22222222222222222222222222222222", "text", "before edit", "read_only", 0,
            "while_turn_id", "", "content", "type", "input_text", "text", "original content"));
        native_checkpoint_commit(&session, "future_turn_edited", json_pack(
            "{s:s,s:s,s:b,s:[{s:s,s:s}]}", "queue_id", "22222222222222222222222222222222",
            "text", "native reference", "read_only", 0, "content", "type", "input_text",
            "text", "inert edited content"));
    } else {
        native_checkpoint_commit(&session, "input_received", json_pack(
            "{s:s,s:s,s:s,s:s,s:[],s:b,s:i,s:[{s:s,s:s}]}", "provider", "openai",
            "model", "gpt-5", "effort", "medium", "text", "native reference",
            "instructions", "read_only", 0, "received_at_ms", 0,
            "content", "type", "input_text", "text", "original content"));
    }
    uint64_t text_sequence = session.next_seq - 1u;
    producer_resume_owner(&session, &probe);
    json_t *turn = json_pack("{s:s,s:i,s:s,s:n,s:n,s:s,s:s,s:[],s:{s:s,s:s,s:s},s:O}",
        "turn_id", "11111111111111111111111111111111", "turn_number", 1,
        "input_kind", queued ? "queued" : "direct", "queue_id", "queue_seq", "cwd", "/",
        "text", "native reference", "instructions", "config", "provider", "openai",
        "model", "gpt-5", "effort", "medium", "content", queued ?
            session.pending_queue[0].content : json_object_get(session.pending_input, "content"));
    assert(turn);
    assert(!json_object_set_new(turn, "read_only", json_false()));
    assert(!json_object_set_new(turn, "received_at_ms", json_integer(queued ?
        (json_int_t)session.pending_queue[0].received_ms : 0)));
    json_t *config = json_object_get(turn, "config");
    assert(!json_object_set_new(config, "max_parallel_commands", json_integer(4)));
    assert(!json_object_set_new(config, "parallel_tool_calls", json_true()));
    if (queued) {
        assert(!json_object_set_new(turn, "queue_id",
            json_string("22222222222222222222222222222222")));
        assert(!json_object_set_new(turn, "queue_seq", json_integer((json_int_t)creation)));
    }
    native_checkpoint_commit(&session, "turn_started", turn);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_event event;
    producer_last_event(&session, &event, &scratch);
    assert(event.kind == SNAG_BINARY_TURN_STARTED &&
        event.data.started.text_ref.target.sequence == text_sequence &&
        event.data.started.content_ref.target.sequence == creation);
    assert(queued ? !event.data.started.instructions_ref.field :
        event.data.started.instructions_ref.target.sequence == creation);
    static const char hash[] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    native_checkpoint_commit(&session, "response_started", json_pack(
        "{s:i,s:n,s:s,s:n,s:s,s:s,s:s,s:i,s:s,s:n,s:i,s:s,s:i,s:s,s:i,s:i,s:s,"
        "s:s,s:s,s:s,s:s,s:n,s:s,s:b,s:[],s:s}",
        "irc_seq", 0, "baseline_sha256", "capability_version", SNAJPAGENT_CAPABILITY_VERSION,
        "compact_id", "count_method", "exact", "capacity_source", "unknown",
        "count_request_sha256", hash, "cycle", 1, "effort", "medium", "hard_input_tokens",
        "input_tokens_bound", 1000, "model", "gpt-5", "model_input_bytes", 4000,
        "model_input_sha256", hash, "request_input_bytes", 3000, "request_input_count", 1,
        "request_input_sha256", hash, "profile_id", SNAJPAGENT_PROFILE_ID,
        "provider", "openai", "provider_source_sha256", hash,
        "request_sha256", hash, "requested_output_tokens", "response_id",
        "33333333333333333333333333333333", "source_bound", 0, "steering_ids",
        "turn_id", "11111111111111111111111111111111"));
    producer_resume_owner(&session, &probe); /* Empty open response has zero spans. */
    uint64_t first = session.next_seq;
    producer_output(&session, 0u, 0u, "hel");
    native_checkpoint_commit(&session, "banner_updated", json_pack("{s:s}", "text", "between"));
    producer_output(&session, 0u, 3u, "lo");
    uint64_t second = session.next_seq;
    producer_output(&session, 1u, 0u, "sec");
    producer_resume_owner(&session, &probe);
    uint64_t last = session.next_seq;
    producer_output(&session, 1u, 3u, "ond");
    native_checkpoint_commit(&session, "response_interrupted", json_pack(
        "{s:s,s:s,s:i,s:s,s:s,s:O}", "turn_id", "11111111111111111111111111111111",
        "response_id", "33333333333333333333333333333333", "cycle", 1,
        "origin", "user", "reason", "cancelled", "partial_public", session.response_public));
    producer_last_event(&session, &event, &scratch);
    assert(event.kind == SNAG_BINARY_RESPONSE_INTERRUPTED);
    struct snag_binary_public_value value;
    size_t position = 0u;
    assert(!snag_binary_public_items_next(&event.data.response_interrupted.partial,
        &position, &value));
    assert(value.source.first.sequence == first && value.source.bytes == 5u);
    assert(!snag_binary_public_items_next(&event.data.response_interrupted.partial,
        &position, &value));
    assert(value.source.first.sequence == second && value.source.last_sequence == last &&
        value.source.bytes == 6u);
    producer_resume_owner(&session, &probe); /* Closed response never resurrects spans. */
    native_checkpoint_commit(&session, "banner_updated", json_pack("{s:s}", "text", "after"));
    native_query_closure(&session, text_sequence + 1u, queued ? 3u : 2u, "turn_started");
    native_query_closure(&session, last + 1u, 5u, "response_interrupted");
    snag_buf_free(&scratch);
    snag_session_close(&session);
}

static void
test_native_irc_payloads(void)
{
    const struct {
        const char *type, *valid, *invalid;
    } cases[] = {
        {"irc_sleep_set", "{\"until_ms\":1,\"messages\":1}",
            "{\"until_ms\":1,\"messages\":0}"},
        {"irc_sleep_woke", "{\"reason\":\"mention\"}", "{\"reason\":\"unknown\"}"},
        {"irc_compact_configured", "{\"after_updates\":20,\"instruction\":\"\"}",
            "{\"after_updates\":4294967296,\"instruction\":\"\"}"},
        {"irc_compacted", "{\"through_seq\":1,\"count\":1,\"summary\":\"data\"}",
            "{\"through_seq\":1,\"count\":1,\"summary\":\"\"}"}
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct snag_buf wire = {.max = SIZE_MAX};
        enum snag_binary_kind kind;
        json_t *good = json_loads(cases[i].valid, 0, NULL);
        json_t *bad = json_loads(cases[i].invalid, 0, NULL), *projected = NULL;
        const char *type = NULL;
        assert(good && bad && !snag_binary_legacy_encode(&wire, cases[i].type, good, &kind));
        struct snag_binary_record record = {.kind = (uint16_t)kind,
            .version = snag_binary_event_version(kind), .payload = wire.data, .size = wire.len};
        assert(!snag_binary_legacy_decode(&record, &type, &projected));
        assert(!strcmp(type, cases[i].type) && json_equal(good, projected));
        json_decref(projected);
        if (i < 2u) {
            if (!i) memset(wire.data + wire.len - 4u, 0, 4u);
            else wire.data[wire.len - 1u] = 4u;
            struct snag_binary_event event, saved;
            memset(&event, 0xa5, sizeof(event));
            memcpy(&saved, &event, sizeof(saved));
            assert(snag_binary_event_decode(&record, &event) < 0 && errno == EINVAL);
            assert(!memcmp(&event, &saved, sizeof(event)));
        }
        snag_buf_reset(&wire);
        assert(!snag_buf_append(&wire, "keep", 4u));
        enum snag_binary_kind saved_kind = kind;
        assert(snag_binary_legacy_encode(&wire, cases[i].type, bad, &kind) < 0 &&
            errno == EINVAL && kind == saved_kind && wire.len == 4u &&
            !memcmp(wire.data, "keep", 4u));
        assert(!json_object_set_new(good, "unknown", json_true()));
        assert(snag_binary_legacy_encode(&wire, cases[i].type, good, &kind) < 0 &&
            errno == EINVAL && kind == saved_kind && wire.len == 4u);
        json_decref(good);
        json_decref(bad);
        snag_buf_free(&wire);
    }
}

static void
test_native_checkpoint_origins(void)
{
    test_native_irc_payloads();
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
    struct snag_irc_event irc = {.kind = SNAG_IRC_MESSAGE, .timestamp_ms = 1u,
        .endpoint = "127.0.0.1:6667", .room = "#lab", .nick = "peer",
        .text = "covered-detail", .stream = "11111111111111111111111111111111",
        .sequence = 1u, .input = true};
    native_checkpoint_commit(&session, "irc_event", snag_irc_event_data(&irc));
    uint64_t admitted = session.irc_received_seq;
    native_checkpoint_commit(&session, "irc_admitted",
        json_pack("{s:[I]}", "sequences", (json_int_t)admitted));
    assert(session.irc_message_count == 1u && session.irc_admitted_count == 1u);
    native_checkpoint_commit(&session, "irc_sleep_set",
        json_pack("{s:I,s:I}", "until_ms", (json_int_t)2000000000000LL,
            "messages", (json_int_t)12));
    assert(session.irc_sleep_until_ms == 2000000000000ULL &&
        session.irc_sleep_messages == 12u && session.irc_sleep_start_count == 1u);
    const char *wake_reasons[] = {"timeout", "mention", "messages"};
    for (size_t i = 0u; i < sizeof(wake_reasons) / sizeof(wake_reasons[0]); ++i) {
        native_checkpoint_commit(&session, "irc_sleep_woke",
            json_pack("{s:s}", "reason", wake_reasons[i]));
        assert(!session.irc_sleep_until_ms && session.irc_sleep_messages == 12u);
    }
    native_checkpoint_commit(&session, "irc_compact_configured",
        json_pack("{s:I,s:s}", "after_updates", (json_int_t)20,
            "instruction", "retain decisions"));
    assert(session.irc_compact_updates == 20u &&
        !strcmp(snag_json_string(session.strings, "irc_compact_instruction"), "retain decisions"));
    uint64_t irc_boundary = session.next_seq - 1u;
    native_checkpoint_commit(&session, "irc_compacted",
        json_pack("{s:I,s:I,s:s}", "through_seq", (json_int_t)irc_boundary,
            "count", (json_int_t)1, "summary", "{\"type\":\"goal_started\",\"prompt\":\"inert\"}"));
    assert(session.irc_compact_seq == irc_boundary && session.irc_compact_count == 1u &&
        session.irc_summary_seq == session.next_seq - 1u && !session.goal_id[0]);
    assert(!snag_session_binary_checkpoint_capture(&session, &boundary, &tree,
        &captured, error, sizeof(error)));
    snag_buf_init(&service_wire, SIZE_MAX);
    assert(!snag_binary_checkpoint_texts_encode(&service_wire, &captured.texts) &&
        service_wire.len == 335u && service_wire.data[0] == 3u);
    snag_buf_free(&service_wire);
    snag_binary_checkpoint_sources_free(&captured);
    native_checkpoint_commit(&session, "irc_compact_configured",
        json_pack("{s:I,s:s}", "after_updates", (json_int_t)0, "instruction", ""));
    assert(!session.irc_compact_updates &&
        !strcmp(snag_json_string(session.strings, "irc_compact_instruction"), ""));
    native_checkpoint_commit(&session, "irc_sleep_set",
        json_pack("{s:I,s:I}", "until_ms", (json_int_t)0, "messages", (json_int_t)0));
    assert(!session.irc_sleep_until_ms && !session.irc_sleep_messages);
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

static int
native_query_index(struct snag_session *session)
{
    struct snag_binary_anchor through, before;
    struct snag_binary_index_tree expected, tree = {0};
    struct snag_binary_checkpoint_sources sources = {0};
    char error[256];
    assert(!snag_session_binary_checkpoint_capture(session, &through, &expected,
        &sources, error, sizeof(error)));
    snag_binary_checkpoint_sources_free(&sources);
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    struct snag_binary_identity identity;
    assert(snag_pread(session->log_fd, header, sizeof(header), 0) == sizeof(header));
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &before));
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snag-query-index-XXXXXX");
    assert(path);
    int fd = mkstemp(path);
    assert(fd >= 0 && !unlink(path));
    free(path);
    unsigned char index_header[SNAG_BINARY_INDEX_HEADER_SIZE];
    snag_binary_index_header_encode(index_header, &identity);
    assert(!snag_write_full(fd, index_header, sizeof(index_header)));
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf bytes = {.max = SNAG_BINARY_INDEX_TREE_BATCH_MAX};
    /* Independent oracle over this explicitly small admitted fixture, never a
     * production lifetime rebuild or an index supplied root as authority. */
    while (before.next_seq < through.next_seq) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor after;
        assert(!snag_binary_batch_read(session->log_fd, through.end, &before,
            &scratch, &batch, &after));
        snag_buf_reset(&bytes);
        assert(!snag_binary_index_tree_append_batch(&bytes, &tree, &identity,
            &before, &after, batch.data, batch.size));
        assert(!snag_write_full(fd, bytes.data, bytes.len));
        before = after;
    }
    unsigned char actual[32], wanted[32];
    assert(!snag_binary_index_tree_root(&tree, actual));
    assert(!snag_binary_index_tree_root(&expected, wanted) && !memcmp(actual, wanted, 32u));
    snag_buf_free(&bytes);
    snag_buf_free(&scratch);
    return fd;
}

struct native_query_cancel {
    size_t calls, at;
};

static bool
native_query_cancelled(void *opaque)
{
    struct native_query_cancel *cancel = opaque;
    return ++cancel->calls == cancel->at;
}

static void
native_query_closure(struct snag_session *session, uint64_t wanted, size_t count, const char *type)
{
    struct snag_binary_anchor through;
    struct snag_binary_index_tree tree;
    struct snag_binary_checkpoint_sources sources = {0};
    char error[256];
    assert(!snag_session_binary_checkpoint_capture(session, &through, &tree,
        &sources, error, sizeof(error)));
    snag_binary_checkpoint_sources_free(&sources);
    unsigned char header[SNAG_BINARY_HEADER_SIZE], root[32];
    struct snag_binary_identity identity;
    struct snag_binary_anchor begin;
    assert(snag_pread(session->log_fd, header, sizeof(header), 0) == sizeof(header));
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &begin));
    assert(!snag_binary_index_tree_root(&tree, root));
    struct snag_buf bytes = {.max = SIZE_MAX}, query = {.max = SIZE_MAX};
    struct snag_binary_checkpoint_index available, selected;
    /* Empty old location custody at the independently known ACK frontier:
     * neither a core image nor a claim that the unselected history is absent. */
    assert(!snag_binary_checkpoint_index_encode(&bytes, &identity, &through, &tree, NULL, 0u));
    assert(!snag_binary_checkpoint_index_decode(bytes.data, bytes.len, &identity,
        &through, root, &available));
    int index = native_query_index(session);
    assert(snag_seek(session->log_fd, 13, SEEK_SET) == 13 && snag_seek(index, 31, SEEK_SET) == 31);
    const char *name = "unchanged";
    json_t *data = NULL;
    assert(snag_binary_checkpoint_projection_read(session->log_fd, &through, &available,
        wanted, &name, &data) < 0 && errno == ENOENT && !data && !strcmp(name, "unchanged"));
    struct native_query_cancel cancel = {.at = SIZE_MAX};
    uint64_t roots[3] = {wanted, wanted, wanted};
    assert(!snag_buf_append(&query, "canary", 6u));
    assert(!snag_binary_checkpoint_query_read(session->log_fd, index, &through, &available,
        &tree, roots, 3u, native_query_cancelled, &cancel, &query));
    assert(!memcmp(query.data, "canary", 6u) && cancel.calls > 1u);
    assert(!snag_binary_checkpoint_index_decode(query.data + 6u, query.len - 6u,
        &identity, &through, root, &selected) && selected.entry_count == count);
    assert(!snag_binary_checkpoint_projection_read(session->log_fd, &through, &selected,
        wanted, &name, &data) && !strcmp(name, type));
    if (!strcmp(type, "turn_started")) {
        assert(!strcmp(snag_json_string(data, "text"), "native reference"));
        assert(!strcmp(snag_json_string(json_array_get(json_object_get(data, "content"), 0u),
            "text"), "original content"));
    } else {
        json_t *items = json_object_get(data, "partial_public");
        assert(json_array_size(items) == 2u &&
            !strcmp(snag_json_string(json_array_get(items, 0u), "text"), "hello") &&
            !strcmp(snag_json_string(json_array_get(items, 1u), "text"), "second"));
    }
    json_decref(data);
    size_t checks = cancel.calls;
    for (size_t at = 1u; at <= checks; ++at) {
        query.len = 6u;
        cancel = (struct native_query_cancel){.at = at};
        assert(snag_binary_checkpoint_query_read(session->log_fd, index, &through, &available,
            &tree, &wanted, 1u, native_query_cancelled, &cancel, &query) < 0 &&
            errno == ECANCELED && query.len == 6u && !memcmp(query.data, "canary", 6u));
    }
    uint64_t invalid[2] = {0u, through.next_seq};
    for (size_t i = 0u; i < 2u; ++i) {
        assert(snag_binary_checkpoint_query_read(session->log_fd, index, &through, &available,
            &tree, &invalid[i], 1u, NULL, NULL, &query) < 0 && errno == EINVAL);
        assert(query.len == 6u && !memcmp(query.data, "canary", 6u));
    }
    assert(snag_binary_checkpoint_query_read(session->log_fd, index, &through, &available,
        &tree, &wanted, SIZE_MAX, NULL, NULL, &query) < 0 && errno == EOVERFLOW && query.len == 6u);
    struct snag_binary_index_tree wrong = tree;
    ++wrong.count;
    assert(snag_binary_checkpoint_query_read(session->log_fd, index, &through, &available,
        &wrong, &wanted, 1u, NULL, NULL, &query) < 0 && errno == EINVAL && query.len == 6u);
    wrong = tree;
    unsigned int peak = 0u;
    while (!(wrong.count & (UINT64_C(1) << peak))) ++peak;
    wrong.peaks[peak][0] ^= 1u;
    assert(snag_binary_checkpoint_query_read(session->log_fd, index, &through, &available,
        &wrong, &wanted, 1u, NULL, NULL, &query) < 0 && errno == EINVAL && query.len == 6u);
    struct snag_binary_checkpoint_index foreign = available;
    ++foreign.identity.created_ms;
    assert(snag_binary_checkpoint_query_read(session->log_fd, index, &through, &foreign,
        &tree, &wanted, 1u, NULL, NULL, &query) < 0 && errno == EINVAL && query.len == 6u);
    assert(snag_binary_checkpoint_query_read(session->log_fd, -1, &through, &available,
        &tree, &wanted, 1u, NULL, NULL, &query) < 0 && errno == EINVAL && query.len == 6u);
    query.max = query.len;
    assert(snag_binary_checkpoint_query_read(session->log_fd, index, &through, &available,
        &tree, &wanted, 1u, NULL, NULL, &query) < 0 && query.len == 6u &&
        !memcmp(query.data, "canary", 6u));
    assert(snag_seek(session->log_fd, 0, SEEK_CUR) == 13 && snag_seek(index, 0, SEEK_CUR) == 31);
    assert(session->next_seq == through.next_seq && !close(index));
    snag_buf_free(&query);
    snag_buf_free(&bytes);
}

static void
native_voice_access(struct snag_session *session, int *directory, char **path, bool sparse)
{
    *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snag-native-voice-XXXXXX");
    assert(*path && mkdtemp(*path));
    *directory = open(*path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(*directory >= 0);
    unsigned char header[SNAG_BINARY_HEADER_SIZE], hash[32];
    struct snag_binary_identity identity;
    struct snag_binary_anchor root;
    assert(snag_pread(session->log_fd, header, sizeof(header), 0) == sizeof(header));
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &root));
    struct snag_binary_index_tree tree = {0};
    struct snag_buf bytes = {.max = SIZE_MAX};
    struct snag_binary_checkpoint_index access;
    struct snag_binary_index_entry entry;
    size_t count = 0u;
    if (sparse) {
        struct snag_binary_anchor first = root, after;
        struct snag_binary_checkpoint_sources sources = {0};
        char error[256];
        assert(!snag_session_binary_checkpoint_capture(session, &root, &tree,
            &sources, error, sizeof(error)));
        snag_binary_checkpoint_sources_free(&sources);
        struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
        struct snag_buf flat = {.max = SNAG_BINARY_INDEX_BATCH_MAX};
        struct snag_binary_batch batch;
        assert(!snag_binary_batch_read(session->log_fd, root.end, &first,
            &scratch, &batch, &after));
        assert(batch.count == 1u && first.next_seq == 1u);
        assert(!snag_binary_index_append_batch(&flat, &identity, &first,
            &after, batch.data, batch.size));
        assert(!snag_binary_index_entry_decode(flat.data, flat.len, &identity, 1u, &entry));
        count = 1u; /* Core creation retained; old voice observations are not. */
        snag_buf_free(&flat);
        snag_buf_free(&scratch);
    }
    assert(!snag_binary_checkpoint_index_encode(&bytes, &identity, &root, &tree,
        count ? &entry : NULL, count));
    assert(!snag_binary_index_tree_root(&tree, hash));
    assert(!snag_binary_checkpoint_index_decode(bytes.data, bytes.len,
        &identity, &root, hash, &access));
    /* Independently decoded empty custody plus this explicitly bounded small
     * fixture suffix, not production admission of an arbitrary old prefix. */
    uint64_t generations[2] = {0}, sequences[2] = {0};
    char error[256];
    assert(!snag_session_binary_checkpoint_setup(session, *directory,
        generations, sequences, &access, error, sizeof(error)));
    snag_buf_free(&bytes);
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
    int directory = -1;
    char *path = NULL;
    if (!variant) {
        native_voice_access(&session, &directory, &path, false);
        json_t *context = NULL;
        assert(!snag_session_voice_context(&session, &context, error, sizeof(error)));
        assert(context && json_is_true(json_object_get(context, "history_complete")) &&
            !strcmp(snag_json_string(context, "recent_asr"), "") && !session.goal_id[0]);
        json_decref(context);
    }
    snag_session_close(&session);
    if (directory >= 0) assert(!close(directory) && !rmdir(path));
    free(path);
}

static json_t *
native_voice_data(const char *speaker, const char *text)
{
    return json_pack("{s:s,s:s,s:s,s:{s:s,s:s,s:s}}",
        "connection_id", "0123456789abcdef0123456789abcdef", "provider", "default",
        "model", "fixture", "event", "type", "voice_transcript", "speaker", speaker,
        "text", text);
}

static void
test_native_voice_context(bool sparse)
{
    struct snag_session session;
    struct probe probe = {0};
    native_fixture(&session, &probe);
    char error[256];
    json_t *context = NULL;
    assert(snag_session_voice_context(&session, &context, error, sizeof(error)) < 0 &&
        errno == ENOTSUP && !context && !session.voice_projection);
    if (sparse) {
        assert(!snag_session_commit(&session, "voice_event", native_voice_data("user",
            "Old ASR outside the core working set"), NULL, error, sizeof(error)));
    }
    int directory;
    char *path;
    native_voice_access(&session, &directory, &path, sparse);
    assert(snag_seek(session.log_fd, 13, SEEK_SET) == 13);
    if (sparse) {
        uint64_t sequence = session.next_seq;
        int64_t end = session.log_end;
        for (unsigned retry = 0u; retry < 2u; ++retry) {
            assert(snag_session_voice_context(&session, &context, error, sizeof(error)) < 0 &&
                errno == ENOENT && !context && !session.voice_projection);
            assert(session.next_seq == sequence && session.log_end == end &&
                !session.voice_history.adopted_seq && atomic_load(&probe.effects) == 2u);
        }
    } else {
        assert(!snag_session_voice_context(&session, &context, error, sizeof(error)));
        assert(json_is_true(json_object_get(context, "history_complete")) &&
            json_integer_value(json_object_get(context, "history_as_of_seq")) == 1);
        json_decref(context);
        assert(!snag_session_commit(&session, "voice_event", native_voice_data("user",
            "Original ASR, not an approved request"), NULL, error, sizeof(error)));
        assert(!snag_session_voice_context(&session, &context, error, sizeof(error)));
        assert(!strcmp(snag_json_string(context, "recent_asr"),
            "Original ASR, not an approved request") &&
            !strcmp(snag_json_string(context, "recent_asr_origin_session_id"), session.id) &&
            json_integer_value(json_object_get(context, "recent_asr_origin_seq")) == 2);
        json_decref(context);
        assert(!snag_session_commit(&session, "voice_event", native_voice_data("assistant",
            "Generated observation, not user intent"), NULL, error, sizeof(error)));
        char *padding = malloc(1024u * 1024u + 1u);
        assert(padding);
        memset(padding, 'x', 1024u * 1024u);
        padding[1024u * 1024u] = '\0';
        for (unsigned i = 0u; i < 4u; ++i) {
            assert(!snag_session_commit(&session, "voice_event", json_pack(
                "{s:s,s:s,s:s,s:{s:s,s:s}}", "connection_id",
                "0123456789abcdef0123456789abcdef", "provider", "default", "model", "fixture",
                "event", "type", "voice_usage", "padding", padding), NULL, error, sizeof(error)));
        }
        free(padding);
        assert(!snag_session_commit(&session, "voice_event", native_voice_data("user",
            "New original ASR"), NULL, error, sizeof(error)));
        assert(snag_seek(session.log_fd, 13, SEEK_SET) == 13);
        assert(!snag_session_voice_context(&session, &context, error, sizeof(error)));
        assert(json_is_false(json_object_get(context, "history_complete")) &&
            json_integer_value(json_object_get(context, "history_as_of_seq")) == 7 &&
            json_integer_value(json_object_get(context, "state_as_of_seq")) == 8 &&
            !strcmp(snag_json_string(context, "recent_asr"),
                "Original ASR, not an approved request"));
        json_decref(context);
        assert(!snag_session_voice_context(&session, &context, error, sizeof(error)));
        assert(json_is_true(json_object_get(context, "history_complete")) &&
            json_integer_value(json_object_get(context, "history_as_of_seq")) == 8 &&
            !strcmp(snag_json_string(context, "recent_asr"), "New original ASR") &&
            !strcmp(snag_json_string(context, "recent_generated_reply"),
                "Generated observation, not user intent") &&
            json_integer_value(json_object_get(context, "recent_asr_origin_seq")) == 8 &&
            json_integer_value(json_object_get(context, "recent_generated_reply_origin_seq")) == 3);
        json_t *again = NULL;
        assert(!snag_session_voice_context(&session, &again, error, sizeof(error)) &&
            json_equal(context, again));
        json_decref(again);
        json_decref(context);
    }
    assert(snag_seek(session.log_fd, 0, SEEK_CUR) == 13);
    snag_session_close(&session);
    assert(!close(directory) && !rmdir(path));
    free(path);
}

static void
native_voice_fixture_record(struct snag_buf *payload, const char *type, json_t *data,
    struct snag_binary_record *record)
{
    assert(data);
    payload->max = SNAG_MAX_EVENT_LINE;
    enum snag_binary_kind kind;
    assert(!snag_binary_legacy_encode(payload, type, data, &kind));
    json_decref(data);
    *record = (struct snag_binary_record){.kind = (uint16_t)kind,
        .version = snag_binary_event_version(kind), .timestamp_ms = 42u,
        .payload = payload->data, .size = payload->len};
}

static void
test_native_voice_grouped(void)
{
    struct snag_session source, session;
    snag_session_init(&source);
    snag_session_init(&session);
    struct snag_binary_anchor before, after;
    source.log_fd = journal_fd(&before);
    source.lock_fd = dup(source.log_fd);
    assert(source.lock_fd >= 0);
    strcpy(source.id, "11000000000000000000000000000000");
    struct snag_buf payload[5] = {0};
    struct snag_binary_record records[6];
    native_voice_fixture_record(&payload[0], "session_created", json_pack(
        "{s:s,s:s,s:s,s:i,s:s,s:s}", "default_effort", "medium", "default_model", "gpt-5",
        "default_provider", "openai", "format", 4, "protocol", "responses", "cwd", "/"),
        &records[0]);
    native_voice_fixture_record(&payload[1], "voice_event",
        native_voice_data("user", "Destination observation before adoption"), &records[1]);
    for (unsigned i = 0u; i < 2u; ++i) {
        native_voice_fixture_record(&payload[i + 2u], "voice_transfer_record", json_pack(
            "{s:s,s:s,s:s,s:i,s:s,s:o}", "transfer_id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "target_session_id", source.id, "source_session_id",
            "dddddddddddddddddddddddddddddddd", "source_seq", (int)i + 1,
            "source_type", "voice_event", "data", native_voice_data(i ? "assistant" : "user",
                i ? "Imported generated reply" : "Imported original ASR")), &records[i + 2u]);
    }
    struct snag_binary_event adoption = {.kind = SNAG_BINARY_VOICE_TRANSFER_ADOPTED};
    struct snag_binary_voice_adopted *value = &adoption.data.voice_transfer_adopted;
    memset(value->transfer.id, 0xaa, sizeof(value->transfer.id));
    value->transfer.target[0] = 17u;
    memset(value->transfer.source, 0xbb, sizeof(value->transfer.source));
    value->transfer.source_as_of = value->transfer.count = 2u;
    value->begin_seq = 2u; /* Inside the one containing batch, not its predecessor. */
    value->native = true;
    payload[4].max = SNAG_MAX_EVENT_LINE;
    assert(!snag_binary_event_encode(&payload[4], &adoption));
    records[4] = (struct snag_binary_record){.kind = (uint16_t)adoption.kind,
        .version = snag_binary_event_version(adoption.kind), .timestamp_ms = 42u,
        .payload = payload[4].data, .size = payload[4].len};
    static const unsigned char metadata = 0xff;
    records[5] = (struct snag_binary_record){.kind = 77u, .version = 1u,
        .flags = SNAG_BINARY_RECORD_OPTIONAL, .timestamp_ms = 42u,
        .payload = &metadata, .size = 1u};
    struct snag_buf raw = {.max = SNAG_BINARY_BATCH_MAX};
    assert(!snag_binary_batch_encode(&raw, &before, records, 6u, 0u, &after));
    assert(!binary_fixture_write(source.log_fd, raw.data, raw.len) &&
        !snag_sync_file(source.log_fd));
    char error[256];
    struct snag_binary_recovery recovery;
    struct snag_binary_checkpoint_sources sources = {0};
    /* Independent complete interpretation of this stopped six-record fixture;
     * no claim of application admission or default four-file resume. */
    int replay = snag_store_reconcile_binary(&source, &session, NULL, NULL,
        &recovery, &sources, error, sizeof(error));
    if (replay < 0) fprintf(stderr, "grouped voice replay at %llu: %s\n",
        (unsigned long long)recovery.problem_seq, error);
    assert(!replay);
    assert(session.next_seq == 7u && session.voice_history.adopted_seq == 5u &&
        session.voice_history.begin.next_seq == 2u &&
        session.voice_history.begin.offset == SNAG_BINARY_HEADER_SIZE);
    struct snag_binary_identity identity = {.created_ms = 42u};
    identity.id[0] = 17u;
    struct snag_binary_index_tree tree = {0};
    assert(!snag_binary_index_tree_append_batch(NULL, &tree, &identity,
        &before, &after, raw.data, raw.len));
    struct snag_binary_producer producer = {0};
    struct probe probe = {.caller = pthread_self()};
    struct snag_binary_io_ops ops = {.write_full = probe_write, .sync_file = probe_sync,
        .opaque = &probe};
    session.log_fd = dup(source.log_fd);
    session.lock_fd = dup(source.log_fd);
    assert(session.log_fd >= 0 && session.lock_fd >= 0);
    assert(!snag_session_bind_binary(&session, &identity, &after, &tree, &producer,
        &sources, &ops, error, sizeof(error)));
    session.on_commit = native_effect;
    session.on_commit_opaque = &probe;
    int directory;
    char *path;
    native_voice_access(&session, &directory, &path, false);
    assert(snag_seek(session.log_fd, 13, SEEK_SET) == 13);
    const char *name = "canary";
    json_t *canary = json_object(), *output = canary;
    assert(canary);
    assert(snag_binary_checkpoint_record_project(session.log_fd, &after, NULL,
        &records[5], 6u, &name, &output) == 1 && output == canary && !strcmp(name, "canary"));
    json_decref(canary);
    json_t *context = NULL;
    assert(!snag_session_voice_context(&session, &context, error, sizeof(error)));
    assert(json_is_true(json_object_get(context, "history_complete")) &&
        json_integer_value(json_object_get(context, "history_as_of_seq")) == 6 &&
        !strcmp(snag_json_string(context, "recent_asr"), "Imported original ASR") &&
        !strcmp(snag_json_string(context, "recent_generated_reply"), "Imported generated reply") &&
        !strcmp(snag_json_string(context, "recent_asr_origin_session_id"),
            "dddddddddddddddddddddddddddddddd") &&
        json_integer_value(json_object_get(context, "recent_asr_seq")) == 3 &&
        json_integer_value(json_object_get(context, "recent_asr_origin_seq")) == 1 &&
        json_integer_value(json_object_get(context, "recent_generated_reply_origin_seq")) == 2);
    json_decref(context);
    assert(snag_seek(session.log_fd, 0, SEEK_CUR) == 13);
    assert(!snag_session_commit(&session, "voice_event", native_voice_data("user",
        "Live original ASR after adoption"), NULL, error, sizeof(error)));
    assert(!snag_session_voice_context(&session, &context, error, sizeof(error)));
    assert(json_is_true(json_object_get(context, "history_complete")) &&
        !strcmp(snag_json_string(context, "recent_asr"), "Live original ASR after adoption") &&
        !strcmp(snag_json_string(context, "recent_asr_origin_session_id"), session.id) &&
        json_integer_value(json_object_get(context, "recent_asr_origin_seq")) == 7 &&
        !strcmp(snag_json_string(context, "recent_generated_reply"), "Imported generated reply"));
    json_decref(context);
    assert(!session.goal_id[0] && !session.active_turn && !session.pending_queue_count);
    snag_session_close(&session);
    snag_session_close(&source);
    assert(!close(directory) && !rmdir(path));
    free(path);
    snag_binary_checkpoint_sources_free(&sources);
    snag_buf_free(&raw);
    for (unsigned i = 0u; i < 5u; ++i) snag_buf_free(&payload[i]);
}

static void
test_native_historical_point(void)
{
    struct snag_session session;
    struct probe probe = {0};
    native_fixture(&session, &probe);
    char error[256];
    assert(!snag_session_commit(&session, "voice_event", native_voice_data("user",
        "Historical observation outside current core closure"), NULL, error, sizeof(error)));
    int directory;
    char *path;
    native_voice_access(&session, &directory, &path, true);
    int index = native_query_index(&session);
    assert(!snag_session_binary_index_setup(&session, index, error, sizeof(error)));
    assert(snag_seek(session.log_fd, 13, SEEK_SET) == 13 && snag_seek(index, 31, SEEK_SET) == 31);
    const char *type;
    json_t *data = NULL;
    int rc = snag_session_binary_projection_read(&session, 2u, &type, &data,
        error, sizeof(error));
    if (rc < 0) fprintf(stderr, "historical native point: %s\n", error);
    assert(!rc && !strcmp(type, "voice_event") &&
        !strcmp(snag_json_string(json_object_get(data, "event"), "text"),
            "Historical observation outside current core closure"));
    json_decref(data);
    assert(snag_seek(session.log_fd, 0, SEEK_CUR) == 13 && snag_seek(index, 0, SEEK_CUR) == 31);
    assert(session.next_seq == 3u && atomic_load(&probe.effects) == 2u);
    int64_t extent, offset;
    assert(!snag_binary_index_end(2u, &extent) && !snag_binary_index_offset(2u, &offset));
    struct snag_buf saved = {.max = SNAG_BINARY_INDEX_TREE_BATCH_MAX};
    assert(!snag_buf_reserve(&saved, (size_t)extent));
    assert(snag_pread(index, saved.data, (size_t)extent, 0) == extent);
    struct snag_binary_identity identity = {.created_ms = 42u};
    identity.id[0] = 17u;
    for (unsigned int fault = 0u; fault < 3u; ++fault) {
        if (!fault) {
            unsigned char bad = saved.data[0] ^ 1u;
            assert(snag_seek(index, 0, SEEK_SET) == 0 && !snag_write_full(index, &bad, 1u));
        } else if (fault == 1u) {
            assert(!snag_truncate(index, SNAG_BINARY_INDEX_HEADER_SIZE));
        } else {
            struct snag_binary_index_entry entry;
            unsigned char bad[SNAG_BINARY_INDEX_ENTRY_SIZE];
            assert(!snag_binary_index_entry_decode(saved.data + offset,
                sizeof(bad), &identity, 2u, &entry));
            entry.kind = SNAG_BINARY_BANNER_UPDATED;
            assert(!snag_binary_index_entry_encode(bad, &identity, &entry));
            assert(snag_seek(index, offset, SEEK_SET) == offset &&
                !snag_write_full(index, bad, sizeof(bad)));
        }
        assert(snag_seek(index, 31, SEEK_SET) == 31);
        type = "unchanged";
        data = json_pack("{s:i}", "canary", 17);
        assert(data);
        json_t *canary = data;
        assert(snag_session_binary_projection_read(&session, 2u, &type, &data,
            error, sizeof(error)) < 0 && errno == (fault == 1u ? ENOENT : EINVAL));
        assert(data == canary && !strcmp(type, "unchanged") &&
            json_integer_value(json_object_get(data, "canary")) == 17);
        json_decref(data);
        data = NULL;
        /* Installed canonical working-set membership is independent of cache
         * health. A cache failure never makes valid old custody unavailable. */
        assert(!snag_session_binary_projection_read(&session, 1u, &type, &data,
            error, sizeof(error)) && !strcmp(type, "session_created"));
        json_decref(data);
        assert(snag_seek(session.log_fd, 0, SEEK_CUR) == 13 &&
            snag_seek(index, 0, SEEK_CUR) == 31 && session.next_seq == 3u &&
            atomic_load(&probe.effects) == 2u);
        assert(snag_seek(index, 0, SEEK_SET) == 0 &&
            !snag_write_full(index, saved.data, (size_t)extent) &&
            snag_seek(index, 31, SEEK_SET) == 31);
    }
    snag_buf_free(&saved);
    snag_session_close(&session);
    assert(!close(index) && !close(directory) && !rmdir(path));
    free(path);
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

static void
test_native_index_attachment(unsigned int variant)
{
    struct snag_session session;
    struct probe probe = {0};
    char error[256];
    snag_session_init(&session);
    assert(snag_session_binary_index_status(&session, error, sizeof(error)) < 0 &&
        errno == ENOTSUP);
    assert(snag_session_binary_index_setup(&session, -1, error, sizeof(error)) < 0 &&
        errno == EINVAL);
    snag_session_close(&session);
    native_fixture(&session, &probe);
    assert(snag_session_binary_index_status(&session, error, sizeof(error)) < 0 &&
        errno == ENOTSUP);
    struct snag_binary_anchor boundary, before;
    struct snag_binary_index_tree tree, seed = {0};
    struct snag_binary_checkpoint_sources sources = {0};
    assert(!snag_session_binary_checkpoint_capture(&session, &boundary, &tree, &sources,
        error, sizeof(error)));
    snag_binary_checkpoint_sources_free(&sources);
    struct snag_binary_identity identity = {.created_ms = 42u};
    identity.id[0] = 17u;
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf entries = {.max = SNAG_BINARY_INDEX_TREE_BATCH_MAX};
    struct snag_binary_batch batch;
    assert(!snag_binary_batch_previous(session.log_fd, &boundary, &scratch, &batch, &before));
    assert(!snag_binary_index_tree_append_batch(&entries, &seed, &identity, &before,
        &boundary, batch.data, batch.size));
    unsigned char actual[32], expected[32];
    assert(!snag_binary_index_tree_root(&seed, actual));
    assert(!snag_binary_index_tree_root(&tree, expected) && !memcmp(actual, expected, 32u));
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snag-session-index-XXXXXX");
    assert(path);
    int index = mkstemp(path);
    assert(index >= 0 && !unlink(path));
    free(path);
    unsigned char header[SNAG_BINARY_INDEX_HEADER_SIZE];
    snag_binary_index_header_encode(header, &identity);
    assert(!snag_write_full(index, header, sizeof(header)));
    if (variant != 2u) assert(!snag_write_full(index, entries.data, entries.len));
    snag_buf_free(&scratch);
    snag_buf_free(&entries);
    probe.index_enabled = true;
    probe.index_fd = index;
    probe.index_failures = variant == 1u;
    int64_t position = snag_seek(index, 0, SEEK_CUR);
    assert(!snag_session_binary_index_setup(&session, index, error, sizeof(error)));
    assert(snag_seek(index, 0, SEEK_CUR) == position);
    assert(!snag_session_binary_index_status(&session, error, sizeof(error)));
    assert(snag_session_binary_index_setup(&session, index, error, sizeof(error)) < 0 &&
        errno == EBUSY);
    const char *types[2] = {"banner_updated", "goal_started"};
    int cache_error = variant == 1u ? EIO : variant == 2u ? ESTALE : 0;
    for (unsigned int i = 0u; i < 2u; ++i) {
        uint64_t sequence = session.next_seq, written = 0u;
        json_t *data = i ? json_pack("{s:s,s:s}", "goal_id",
            "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", "prompt", "native indexed value") :
            json_pack("{s:s}", "text", "native indexed value");
        int rc = snag_session_commit(&session, types[i], data, &written, error, sizeof(error));
        if (rc < 0) fprintf(stderr, "native index attachment %s: %s\n", types[i], error);
        assert(!rc);
        assert(written == sequence && session.next_seq == sequence + 1u);
        assert(atomic_load(&probe.effects) == i + 2u && !session.write_failures);
        if (cache_error) {
            assert(snag_session_binary_index_status(&session, error, sizeof(error)) < 0 &&
                errno == cache_error);
        } else {
            assert(!snag_session_binary_index_status(&session, error, sizeof(error)));
        }
    }
    if (!cache_error) {
        assert(!snag_session_binary_checkpoint_capture(&session, &boundary, &tree, &sources,
            error, sizeof(error)));
        assert(!snag_binary_index_tree_root(&tree, expected));
        for (uint64_t seq = 1u; seq <= tree.count; ++seq) {
            struct snag_binary_index_entry entry;
            assert(!snag_binary_index_read_verified(index, &identity, tree.count,
                expected, seq, &entry));
        }
        snag_binary_checkpoint_sources_free(&sources);
    }
    assert(probe.index_writes == (variant == 1u ? 1u : variant == 2u ? 0u : 2u));
    snag_session_close(&session);
    snag_file_info info;
    assert(!snag_fstat(index, &info));
    assert(!close(index));
}

static void
test_index_cache(unsigned int variant)
{
    struct snag_binary_anchor before;
    int fd = journal_fd(&before);
    struct snag_binary_identity identity = {.created_ms = 42u};
    identity.id[0] = 17u;
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snag-index-io-XXXXXX");
    assert(path);
    int index = mkstemp(path);
    assert(index >= 0 && !unlink(path));
    free(path);
    unsigned char header[SNAG_BINARY_INDEX_HEADER_SIZE];
    snag_binary_index_header_encode(header, &identity);
    if (variant == 2u) header[0] ^= 1u;
    if (variant == 6u) {
        struct snag_binary_identity other = identity;
        other.id[0] ^= 1u;
        snag_binary_index_header_encode(header, &other);
    }
    assert(!snag_write_full(index, header, sizeof(header)));
    if (variant == 3u) assert(!snag_write_full(index, "x", 1u));
    struct snag_binary_index_tree tree = {0};
    struct probe probe = {.index_enabled = true, .index_fd = index,
        .index_failures = variant == 1u, .sync_failures = variant == 4u};
    struct snag_binary_io *io = start_owner(fd, &before, &probe);
    assert(snag_binary_io_index_setup(io, fd, &identity, &tree) < 0 && errno == EINVAL);
    struct snag_binary_index_tree stale = {.count = 1u};
    assert(snag_binary_io_index_setup(io, index, &identity, &stale) < 0 && errno == ESTALE);
    int64_t setup_position = snag_seek(index, 0, SEEK_CUR);
    assert(!snag_binary_io_index_setup(io, index, &identity, &tree));
    assert(snag_seek(index, 0, SEEK_CUR) == setup_position);
    assert(snag_binary_io_index_setup(io, index, &identity, &tree) < 0 && errno == EBUSY);
    struct snag_binary_io_result result;
    struct snag_buf committed = {0};
    int expected = variant == 1u ? EIO : variant == 2u || variant == 6u ? EINVAL :
        variant == 3u ? ESTALE : 0;
    for (unsigned int turn = 0u; turn < 2u; ++turn) {
        struct snag_binary_record items[2] = {record("indexed canonical payload"),
            record("grouped canonical payload")};
        uint32_t count = turn ? 1u : 2u;
        assert(!snag_binary_io_submit(io, items, count, 0u));
        if (variant == 4u && !turn) {
            assert(await_batch(io, &result, &committed) < 0 && errno == EIO);
            assert(!probe.index_writes && !result.index_error);
            snag_file_info info;
            assert(!snag_fstat(index, &info) && info.st_size == sizeof(header));
            assert(!snag_binary_io_retry(io));
        }
        assert(!await_batch(io, &result, &committed));
        assert(!result.error && !result.retryable && result.index_error == expected);
        assert(!snag_binary_index_tree_append_batch(NULL, &tree, &identity, &before,
            &result.durable, committed.data, committed.len));
        before = result.durable;
        assert(probe.syncs == turn + 1u + (variant == 4u));
        if (!expected) {
            unsigned char root[32];
            assert(!snag_binary_index_tree_root(&tree, root));
            struct snag_binary_index_entry entry;
            int64_t position = snag_seek(index, 0, SEEK_CUR);
            assert(!snag_binary_index_read_verified(index, &identity, tree.count,
                root, tree.count, &entry));
            assert(snag_seek(index, 0, SEEK_CUR) == position);
            struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
            struct snag_binary_record read;
            assert(!snag_binary_index_load_record(fd, &before, &entry, &scratch, &read));
            assert(read.size == items[count - 1u].size &&
                !memcmp(read.payload, items[count - 1u].payload, read.size));
            snag_buf_free(&scratch);
        }
    }
    assert(probe.index_writes == (variant == 1u ? 1u : expected ? 0u : 2u));
    assert(!snag_binary_io_close(io));
    if (!expected) {
        /* Resume only from independently known canonical state/frontier. */
        if (variant == 5u) {
            int64_t end;
            assert(!snag_binary_index_end(tree.count, &end));
            int64_t offset = end - 1;
            unsigned char value;
            assert(snag_pread(index, &value, 1u, offset) == 1);
            value ^= 1u;
            assert(snag_seek(index, offset, SEEK_SET) == offset);
            assert(!snag_write_full(index, &value, 1u));
        }
        io = start_owner(fd, &before, &probe);
        assert(!snag_binary_io_index_setup(io, index, &identity, &tree));
        struct snag_binary_record item = record("resumed canonical payload");
        assert(!snag_binary_io_submit(io, &item, 1u, 0u));
        assert(!await_batch(io, &result, &committed));
        assert(!result.error && result.index_error == (variant == 5u ? EINVAL : 0));
        assert(!snag_binary_index_tree_append_batch(NULL, &tree, &identity, &before,
            &result.durable, committed.data, committed.len));
        if (variant != 5u) {
            unsigned char root[32];
            assert(!snag_binary_index_tree_root(&tree, root));
            for (uint64_t seq = 1u; seq <= tree.count; ++seq) {
                struct snag_binary_index_entry entry;
                assert(!snag_binary_index_read_verified(index, &identity, tree.count,
                    root, seq, &entry));
            }
        }
        assert(!snag_binary_io_close(io));
    }
    snag_buf_free(&committed);
    assert(!close(index) && !close(fd));
}

void
test_store_binary_io(void)
{
    for (unsigned int variant = 0u; variant < 3u; ++variant)
        test_native_index_attachment(variant);
    for (unsigned int variant = 0u; variant < 7u; ++variant) test_index_cache(variant);
    test_native_checkpoint_origins();
    test_native_producer_resume(false);
    test_native_producer_resume(true);
    test_native_session_ack();
    test_native_session_retry(true, 1u);
    test_native_session_retry(false, 1u);
    test_native_session_retry(true, 2u);
    test_native_session_retry(false, 2u);
    test_native_clone_failure();
    test_native_result_retry();
    for (unsigned int variant = 0u; variant < 8u; ++variant)
        test_native_voice_import(variant);
    test_native_voice_context(false);
    test_native_voice_context(true);
    test_native_voice_grouped();
    test_native_historical_point();
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
