/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_io.h"
#include "store_binary_wire.h"
#include "fs.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
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
    wait_flag(&probe.write_entered);
    struct snag_binary_io_result result;
    memset(&result, 0xa5, sizeof(result));
    struct snag_binary_io_result saved;
    memcpy(&saved, &result, sizeof(saved));
    assert(snag_binary_io_take(io, &result) == 1 && !memcmp(&saved, &result, sizeof(saved)));
    assert(snag_binary_io_submit(io, records, 2u, 0u) < 0 && errno == EBUSY);
    assert(snag_binary_io_close(io) < 0 && errno == EBUSY);
    atomic_store(&probe.release_write, true);
    wait_flag(&probe.sync_entered);
    assert(snag_binary_io_take(io, &result) == 1 && !memcmp(&saved, &result, sizeof(saved)));
    atomic_store(&probe.release_sync, true);
    assert(snag_wakeup_wait(snag_binary_io_wake(io), 10000) == 1);
    assert(snag_binary_io_close(io) < 0 && errno == EBUSY);
    assert(!snag_binary_io_take(io, &result));
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
    struct snag_binary_io_result result;
    assert(await_result(io, &result) < 0 && errno == EIO && result.error == EIO);
    assert(result.retryable && same_anchor(&result.durable, &before));
    assert(partial ? same_anchor(&result.written, &before) : result.written.next_seq == 3u);
    assert(snag_binary_io_submit(io, records, 2u, 0u) < 0 && errno == EBUSY);
    assert(!snag_binary_io_retry(io));
    if (failures == 1u) {
        assert(!await_result(io, &result) && result.durable.next_seq == 3u &&
            same_anchor(&result.written, &result.durable));
        assert(!snag_binary_io_close(io));
        assert(probe.writes == (partial ? 2u : 1u));
        assert(probe.syncs == (partial ? 1u : 2u));
        assert(probe.bytes == result.durable.end - before.end);
        verify_batch(fd, &before, &result.durable, records, 2u);
        assert(snag_seek(fd, 0, SEEK_END) == (int64_t)result.durable.end);
    } else {
        assert(await_result(io, &result) < 0 && errno == EIO && !result.retryable);
        assert(same_anchor(&result.durable, &before));
        assert(snag_binary_io_retry(io) < 0 && errno == EALREADY);
        assert(snag_binary_io_submit(io, records, 2u, 0u) < 0 && errno == EBUSY);
        assert(!snag_binary_io_close(io));
        assert(probe.writes == (partial ? 2u : 1u));
        assert(probe.syncs == (partial ? 0u : 2u));
        assert(snag_seek(fd, 0, SEEK_END) > (int64_t)before.end);
    }
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

void
test_store_binary_io(void)
{
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
