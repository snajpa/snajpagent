/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_io.h"
#include "fs.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void test_store_binary_publish(void);

enum fault {
    FAULT_NONE, FAULT_CREATE, FAULT_PARTIAL, FAULT_WRITTEN, FAULT_FILE_SYNC,
    FAULT_FILE_SYNCED, FAULT_RENAME, FAULT_RENAMED, FAULT_DIR_SYNC,
    FAULT_DIR_SYNCED, FAULT_UNSUPPORTED
};
enum gate { GATE_NONE, GATE_FILE_SYNC, GATE_RENAME, GATE_DIR_SYNC };

struct probe {
    int journal;
    pthread_t caller;
    pthread_t owner;
    bool owner_seen;
    enum fault fault;
    unsigned int failures;
    size_t fail_write;
    enum gate gate;
    bool terminate_at_gate;
    int notice;
    atomic_bool entered;
    atomic_bool released;
    bool priority;
    atomic_bool first_entered;
    atomic_bool first_released;
    atomic_bool second_entered;
    atomic_bool second_released;
    atomic_bool journal_synced;
    size_t writes;
    size_t largest_write;
    size_t creates;
    size_t file_syncs;
    size_t renames;
    size_t dir_syncs;
};

struct fixture {
    char *path;
    int directory;
    int journal;
    struct snag_binary_identity identity;
    struct snag_binary_anchor before;
    struct snag_buf slots[2];
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

static bool
fail(struct probe *probe, enum fault fault)
{
    if (probe->fault != fault || !probe->failures) return false;
    --probe->failures;
    return true;
}

static void
pause_stage(struct probe *probe, enum gate gate)
{
    if (probe->gate != gate) return;
    if (probe->terminate_at_gate) assert(write(probe->notice, "x", 1u) == 1);
    atomic_store(&probe->entered, true);
    wait_flag(&probe->released);
}

static int
probe_create(void *opaque, int directory, const char *name)
{
    struct probe *probe = opaque;
    check_owner(probe);
    ++probe->creates;
    if (fail(probe, FAULT_CREATE)) return snag_errno(ENOSPC);
    return snag_create_private_at(directory, name, true);
}

static int
probe_write(void *opaque, int fd, const void *bytes, size_t size)
{
    struct probe *probe = opaque;
    check_owner(probe);
    if (fd == probe->journal) return snag_write_full(fd, bytes, size);
    ++probe->writes;
    if (size > probe->largest_write) probe->largest_write = size;
    if (probe->priority && probe->writes == 1u) {
        atomic_store(&probe->first_entered, true);
        wait_flag(&probe->first_released);
    }
    if (probe->priority && probe->writes == 2u) {
        assert(atomic_load(&probe->journal_synced));
        atomic_store(&probe->second_entered, true);
        wait_flag(&probe->second_released);
    }
    if ((!probe->fail_write || probe->writes == probe->fail_write) && fail(probe, FAULT_PARTIAL)) {
        assert(!snag_write_full(fd, bytes, size / 2u));
        return snag_errno(EIO);
    }
    int rc = snag_write_full(fd, bytes, size);
    if (!rc && fail(probe, FAULT_WRITTEN)) return snag_errno(EIO);
    return rc;
}

static int
probe_sync(void *opaque, int fd)
{
    struct probe *probe = opaque;
    check_owner(probe);
    if (fd == probe->journal) {
        int rc = snag_sync_file(fd);
        if (!rc) atomic_store(&probe->journal_synced, true);
        return rc;
    }
    ++probe->file_syncs;
    pause_stage(probe, GATE_FILE_SYNC);
    if (fail(probe, FAULT_FILE_SYNC)) return snag_errno(EIO);
    int rc = snag_sync_file(fd);
    if (!rc && fail(probe, FAULT_FILE_SYNCED)) return snag_errno(EIO);
    return rc;
}

static int
probe_rename(void *opaque, int from_directory, const char *from, int to_directory, const char *to)
{
    struct probe *probe = opaque;
    check_owner(probe);
    ++probe->renames;
    pause_stage(probe, GATE_RENAME);
    assert(probe->file_syncs);
    if (fail(probe, FAULT_RENAME)) return snag_errno(EIO);
    int rc = snag_rename_at(from_directory, from, to_directory, to);
    if (!rc && fail(probe, FAULT_RENAMED)) return snag_errno(EIO);
    return rc;
}

static int
probe_directory(void *opaque, int fd)
{
    struct probe *probe = opaque;
    check_owner(probe);
    ++probe->dir_syncs;
    pause_stage(probe, GATE_DIR_SYNC);
    assert(probe->renames);
    if (fail(probe, FAULT_DIR_SYNC)) return snag_errno(EIO);
    if (fail(probe, FAULT_UNSUPPORTED)) return 1;
    int rc = snag_sync_dir(fd);
    if (!rc && fail(probe, FAULT_DIR_SYNCED)) return snag_errno(EIO);
    return rc;
}

static struct snag_binary_io_snapshot
snapshot(const struct fixture *fixture, const struct snag_binary_anchor *boundary, size_t size)
{
    struct snag_binary_io_snapshot out = {
        .identity = fixture->identity, .boundary = *boundary,
        .core_version = 1u, .provider_version = 1u, .access_version = 1u,
        .core = {.max = SIZE_MAX}, .provider = {.max = SIZE_MAX},
        .access = {.max = SIZE_MAX}
    };
    /* Opaque framing fixtures; section semantics have separate codec coverage. */
    unsigned char bytes[4096u];
    for (size_t i = 0u; i < sizeof(bytes); ++i) {
        bytes[i] = (unsigned char)(i * 17u + 3u);
    }
    while (size) {
        size_t count = size < sizeof(bytes) ? size : sizeof(bytes);
        assert(!snag_buf_append(&out.core, bytes, count));
        size -= count;
    }
    assert(!snag_buf_append(&out.provider, "provider section", 16u));
    assert(!snag_buf_append(&out.access, "access section", 14u));
    return out;
}

static struct snag_buf
image(const struct snag_binary_io_snapshot *snapshot, uint64_t generation)
{
    struct snag_buf bytes = {.max = SIZE_MAX};
    struct snag_binary_checkpoint_frame frame = {
        .identity = snapshot->identity, .boundary = snapshot->boundary,
        .generation = generation,
        .core = {snapshot->core_version, snapshot->core.data, snapshot->core.len},
        .provider = {snapshot->provider_version, snapshot->provider.data, snapshot->provider.len},
        .access = {snapshot->access_version, snapshot->access.data, snapshot->access.len}
    };
    assert(!snag_binary_checkpoint_frame_encode(&bytes, &frame));
    return bytes;
}

static void
free_snapshot(struct snag_binary_io_snapshot *snapshot)
{
    snag_buf_free(&snapshot->core);
    snag_buf_free(&snapshot->provider);
    snag_buf_free(&snapshot->access);
}

static void
fixture_init(struct fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snag-publication-XXXXXX");
    assert(fixture->path && mkdtemp(fixture->path));
    fixture->directory = snag_open_read(fixture->path, true);
    assert(fixture->directory >= 0);
    fixture->journal = snag_create_private_at(fixture->directory, "journal.bin", true);
    assert(fixture->journal >= 0 && !snag_lock_file(fixture->journal, false));
    fixture->identity.created_ms = 42u;
    fixture->identity.id[0] = 17u;
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &fixture->identity);
    assert(!snag_binary_header_decode(header, sizeof(header),
        &fixture->identity, &fixture->before));
    assert(!snag_write_full(fixture->journal, header, sizeof(header)));
    assert(!snag_sync_file(fixture->journal));
    struct snag_binary_io_snapshot initial = snapshot(fixture, &fixture->before, 7u);
    fixture->slots[0] = image(&initial, 5u);
    fixture->slots[1] = image(&initial, 8u);
    free_snapshot(&initial);
    for (unsigned int slot = 0u; slot < 2u; ++slot) {
        int fd = snag_create_private_at(fixture->directory,
            slot ? "checkpoint.1" : "checkpoint.0", true);
        assert(fd >= 0);
        assert(!snag_write_full(fd, fixture->slots[slot].data, fixture->slots[slot].len));
        assert(!snag_sync_file(fd) && !close(fd));
    }
    assert(!snag_sync_dir(fixture->directory));
}

static struct snag_binary_io *
start_owner(struct fixture *fixture, struct probe *probe)
{
    probe->caller = pthread_self();
    probe->journal = fixture->journal;
    struct snag_binary_io_ops ops = {
        .write_full = probe_write, .sync_file = probe_sync, .opaque = probe,
        .create_private = probe_create, .rename_at = probe_rename, .sync_dir = probe_directory
    };
    struct snag_binary_io *io = snag_binary_io_start(fixture->journal, &fixture->before, &ops);
    assert(io);
    uint64_t generations[] = {5u, 8u};
    assert(!snag_binary_io_checkpoint_setup(io, fixture->directory, generations));
    return io;
}

static void
check_file(struct fixture *fixture, const char *name, const struct snag_buf *expected)
{
    int fd = snag_open_read_at(fixture->directory, name, false);
    assert(fd >= 0);
    struct snag_buf actual = {.max = expected->len};
    assert(!snag_buf_read(&actual, fd));
    assert(actual.len == expected->len && !memcmp(actual.data, expected->data, expected->len));
    assert(!close(fd));
    snag_buf_free(&actual);
}

static void
fixture_free(struct fixture *fixture)
{
    DIR *directory = fdopendir(dup(fixture->directory));
    assert(directory);
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }
        assert(!snag_unlink_at(fixture->directory, entry->d_name, false));
    }
    assert(!closedir(directory) && !close(fixture->journal) && !close(fixture->directory));
    assert(!rmdir(fixture->path));
    free(fixture->path);
    snag_buf_free(&fixture->slots[0]);
    snag_buf_free(&fixture->slots[1]);
}

static int
await_checkpoint(struct snag_binary_io *io, struct snag_binary_publication_result *out)
{
    assert(snag_wakeup_wait(snag_binary_io_wake(io), 10000) == 1);
    int rc = snag_binary_io_checkpoint_take(io, out);
    assert(rc != 1);
    return rc;
}

static struct snag_binary_io_result
journal_commit(struct snag_binary_io *io)
{
    struct snag_binary_record record = {
        .kind = 0x8fffu, .version = 1u, .flags = SNAG_BINARY_RECORD_OPTIONAL,
        .timestamp_ms = 100u, .payload = (const unsigned char *)"later", .size = 5u
    };
    assert(!snag_binary_io_submit(io, &record, 1u, 0u));
    assert(snag_wakeup_wait(snag_binary_io_wake(io), 10000) == 1);
    struct snag_binary_io_result result;
    assert(!snag_binary_io_take(io, &result));
    return result;
}

static void
test_publication_gates(enum gate gate)
{
    struct fixture fixture;
    fixture_init(&fixture);
    struct probe probe = {.gate = gate};
    struct snag_binary_io *io = start_owner(&fixture, &probe);
    struct snag_binary_io_snapshot capture = snapshot(&fixture, &fixture.before, 200003u);
    if (gate == GATE_DIR_SYNC) {
        snag_buf_free(&capture.access);
        capture.access_version = 0u;
    }
    struct snag_buf expected = image(&capture, 9u);
    assert(!snag_binary_io_checkpoint_submit(io, &capture));
    assert(!capture.core.data && !capture.provider.data && !capture.access.data);
    wait_flag(&probe.entered);
    struct snag_binary_publication_result result;
    memset(&result, 0x5a, sizeof(result));
    struct snag_binary_publication_result sentinel = result;
    assert(snag_binary_io_checkpoint_take(io, &result) == 1);
    assert(!memcmp(&result, &sentinel, sizeof(result)));
    assert(snag_binary_io_close(io) < 0 && errno == EBUSY);
    check_file(&fixture, "checkpoint.1", &fixture.slots[1]);
    check_file(&fixture, "checkpoint.0", gate == GATE_DIR_SYNC ? &expected : &fixture.slots[0]);
    struct snag_binary_io_snapshot extra = snapshot(&fixture, &fixture.before, 1u);
    struct snag_binary_io_snapshot saved = extra;
    assert(snag_binary_io_checkpoint_submit(io, &extra) < 0 && errno == EBUSY);
    assert(!memcmp(&extra, &saved, sizeof(extra)));
    free_snapshot(&extra);
    atomic_store(&probe.released, true);
    assert(!await_checkpoint(io, &result));
    assert(result.published && result.renamed && !result.error && result.generation == 9u);
    assert(!memcmp(result.image_digest, expected.data + expected.len - 32u, 32u));
    assert(result.slot == 0u && probe.largest_write == 65536u);
    assert(probe.file_syncs == 1u && probe.renames == 1u && probe.dir_syncs == 1u);
    check_file(&fixture, "checkpoint.0", &expected);
    snag_file_info info;
    assert(snag_lstat_at(fixture.directory, result.temporary, &info) < 0 && errno == ENOENT);
    assert(snag_binary_io_checkpoint_take(io, &result) < 0 && errno == ENOENT);
    assert(!snag_binary_io_close(io));
    snag_buf_free(&expected);
    fixture_free(&fixture);
}

static void
test_retry(enum fault fault)
{
    struct fixture fixture;
    fixture_init(&fixture);
    struct probe probe = {.fault = fault, .failures = 1u};
    struct snag_binary_io *io = start_owner(&fixture, &probe);
    struct snag_binary_io_snapshot capture = snapshot(&fixture, &fixture.before, 220009u);
    struct snag_buf expected = image(&capture, 9u);
    assert(!snag_binary_io_checkpoint_submit(io, &capture));
    struct snag_binary_publication_result result;
    assert(await_checkpoint(io, &result) < 0);
    assert(!result.published && result.error && result.generation == 9u && result.slot == 0u);
    static const unsigned char zero[32] = {0};
    assert(!memcmp(result.image_digest, zero, sizeof(zero)));
    if (fault == FAULT_UNSUPPORTED) assert(result.error == ENOTSUP);
    check_file(&fixture, "checkpoint.1", &fixture.slots[1]);
    bool replaced = fault >= FAULT_RENAMED;
    check_file(&fixture, "checkpoint.0", replaced ? &expected : &fixture.slots[0]);
    struct snag_binary_publication_result failed = result;
    struct snag_binary_io_result committed = journal_commit(io);
    assert(committed.durable.next_seq == fixture.before.next_seq + 1u);
    assert(!snag_binary_io_checkpoint_retry(io));
    assert(!await_checkpoint(io, &result));
    assert(result.published && result.generation == failed.generation &&
        result.slot == failed.slot);
    assert(result.boundary.end == fixture.before.end);
    assert(!memcmp(result.image_digest, expected.data + expected.len - 32u, 32u));
    assert(fault == FAULT_CREATE || !strcmp(result.temporary, failed.temporary));
    assert(probe.creates == (fault == FAULT_CREATE ? 2u : 1u));
    assert(probe.renames == (fault == FAULT_RENAME ? 2u : 1u));
    check_file(&fixture, "checkpoint.0", &expected);
    check_file(&fixture, "checkpoint.1", &fixture.slots[1]);
    capture = snapshot(&fixture, &fixture.before, 1u);
    struct snag_binary_io_snapshot saved = capture;
    assert(snag_binary_io_checkpoint_submit(io, &capture) < 0 && errno == ESTALE);
    assert(!memcmp(&capture, &saved, sizeof(capture)));
    capture.boundary = committed.durable;
    struct snag_buf newer = image(&capture, 10u);
    assert(!snag_binary_io_checkpoint_submit(io, &capture));
    assert(!await_checkpoint(io, &result));
    assert(result.generation == 10u && result.slot == 1u);
    assert(!memcmp(result.image_digest, newer.data + newer.len - 32u, 32u));
    check_file(&fixture, "checkpoint.0", &expected);
    check_file(&fixture, "checkpoint.1", &newer);
    assert(!snag_binary_io_close(io));
    snag_buf_free(&expected);
    snag_buf_free(&newer);
    fixture_free(&fixture);
}

static void
test_priority_and_wake(bool checkpoint_first)
{
    struct fixture fixture;
    fixture_init(&fixture);
    struct probe probe = {.priority = true};
    struct snag_binary_io *io = start_owner(&fixture, &probe);
    struct snag_binary_io_snapshot capture = snapshot(&fixture, &fixture.before,
        (size_t)SNAG_MAX_EVENT_LINE + 1u);
    if (checkpoint_first) {
        struct snag_buf swap = capture.core;
        capture.core = capture.access;
        capture.access = swap;
    }
    struct snag_buf expected = image(&capture, 9u);
    assert(!snag_binary_io_checkpoint_submit(io, &capture));
    wait_flag(&probe.first_entered);
    struct snag_binary_record record = {
        .kind = 0x8fffu, .version = 1u, .flags = SNAG_BINARY_RECORD_OPTIONAL
    };
    assert(!snag_binary_io_submit(io, &record, 1u, 0u));
    atomic_store(&probe.first_released, true);
    wait_flag(&probe.second_entered);
    assert(snag_wakeup_wait(snag_binary_io_wake(io), 10000) == 1);
    /* Drain the journal notification while its completion remains unconsumed.
     * The checkpoint cannot complete until the second chunk is released. */
    snag_wakeup_drain(snag_binary_io_wake(io));
    atomic_store(&probe.second_released, true);
    assert(snag_wakeup_wait(snag_binary_io_wake(io), 10000) == 1);
    struct snag_binary_io_result committed;
    struct snag_binary_publication_result published;
    if (checkpoint_first) {
        assert(!snag_binary_io_checkpoint_take(io, &published));
    } else {
        assert(!snag_binary_io_take(io, &committed));
    }
    assert(snag_wakeup_wait(snag_binary_io_wake(io), 0) == 1);
    if (checkpoint_first) {
        assert(!snag_binary_io_take(io, &committed));
    } else {
        assert(!snag_binary_io_checkpoint_take(io, &published));
    }
    assert(snag_wakeup_wait(snag_binary_io_wake(io), 0) == 0);
    assert(published.boundary.end == fixture.before.end);
    assert(committed.durable.end > published.boundary.end && probe.largest_write == 65536u);
    check_file(&fixture, "checkpoint.0", &expected);
    assert(!snag_binary_io_close(io));
    snag_buf_free(&expected);
    fixture_free(&fixture);
}

static void
test_failed_close(void)
{
    struct fixture fixture;
    fixture_init(&fixture);
    struct probe probe = {.fault = FAULT_FILE_SYNC, .failures = 3u};
    struct snag_binary_io *io = start_owner(&fixture, &probe);
    struct snag_binary_io_snapshot capture = snapshot(&fixture, &fixture.before, 200u);
    struct snag_buf expected = image(&capture, 9u);
    assert(!snag_binary_io_checkpoint_submit(io, &capture));
    struct snag_binary_publication_result result;
    for (unsigned int attempt = 0u; attempt < 3u; ++attempt) {
        assert(await_checkpoint(io, &result) < 0 && result.error == EIO);
        assert(probe.file_syncs == attempt + 1u);
        assert(!probe.renames && !probe.dir_syncs);
        if (attempt < 2u) assert(!snag_binary_io_checkpoint_retry(io));
    }
    assert(!snag_binary_io_close(io));
    check_file(&fixture, result.temporary, &expected);
    check_file(&fixture, "checkpoint.0", &fixture.slots[0]);
    check_file(&fixture, "checkpoint.1", &fixture.slots[1]);
    snag_buf_free(&expected);
    fixture_free(&fixture);
}

static void
test_temporary_conflict(unsigned int mode)
{
    struct fixture fixture;
    fixture_init(&fixture);
    struct probe probe = {.fault = FAULT_PARTIAL, .failures = 1u, .fail_write = 2u};
    struct snag_binary_io *io = start_owner(&fixture, &probe);
    struct snag_binary_io_snapshot capture = snapshot(&fixture, &fixture.before, 500u);
    assert(!snag_binary_io_checkpoint_submit(io, &capture));
    struct snag_binary_publication_result result;
    assert(await_checkpoint(io, &result) < 0 && result.error == EIO);
    int fd = openat(fixture.directory, result.temporary, O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    if (mode == 0u) {
        /* The retained prefix of the incomplete chunk must match exactly. */
        assert(snag_seek(fd, SNAG_BINARY_CHECKPOINT_HEADER_SIZE, SEEK_SET) >= 0);
        assert(!snag_write_full(fd, "!", 1u));
    } else if (mode == 1u) {
        assert(!snag_truncate(fd, SNAG_BINARY_CHECKPOINT_HEADER_SIZE - 1u));
    } else {
        assert(!snag_truncate(fd, SNAG_BINARY_CHECKPOINT_HEADER_SIZE + 501u));
    }
    assert(snag_seek(fd, 0, SEEK_SET) == 0);
    struct snag_buf changed = {.max = 4096u};
    assert(!snag_buf_read(&changed, fd) && !close(fd));
    assert(!snag_binary_io_checkpoint_retry(io));
    assert(await_checkpoint(io, &result) < 0 && result.error == ESTALE);
    assert(!probe.renames && !probe.file_syncs && probe.writes == 2u);
    assert(!snag_binary_io_close(io));
    check_file(&fixture, result.temporary, &changed);
    check_file(&fixture, "checkpoint.0", &fixture.slots[0]);
    check_file(&fixture, "checkpoint.1", &fixture.slots[1]);
    snag_buf_free(&changed);
    fixture_free(&fixture);
}

static void
test_source_identity(unsigned int mode)
{
    struct fixture fixture;
    fixture_init(&fixture);
    struct probe probe = {0};
    struct snag_binary_io *io = start_owner(&fixture, &probe);
    struct snag_binary_io_snapshot capture = snapshot(&fixture, &fixture.before, 1u);
    if (mode) {
        ++capture.identity.created_ms;
    } else {
        capture.identity.id[0] ^= 1u;
    }
    assert(!snag_binary_io_checkpoint_submit(io, &capture));
    struct snag_binary_publication_result result;
    assert(await_checkpoint(io, &result) < 0 && result.error == ESTALE);
    assert(!probe.creates && !result.temporary[0] && !result.published && !result.renamed);
    assert(!snag_binary_io_close(io));
    check_file(&fixture, "checkpoint.0", &fixture.slots[0]);
    check_file(&fixture, "checkpoint.1", &fixture.slots[1]);
    fixture_free(&fixture);
}

static void
test_termination(enum gate gate)
{
    struct fixture fixture;
    fixture_init(&fixture);
    struct snag_binary_io_snapshot capture = snapshot(&fixture, &fixture.before, 200u);
    struct snag_buf expected = image(&capture, 9u);
    int notice[2];
    assert(!pipe(notice));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        assert(!close(notice[0]));
        struct probe probe = {.gate = gate, .terminate_at_gate = true, .notice = notice[1]};
        struct snag_binary_io *io = start_owner(&fixture, &probe);
        assert(!snag_binary_io_checkpoint_submit(io, &capture));
        struct snag_binary_publication_result result;
        (void)await_checkpoint(io, &result);
        _exit(1);
    }
    assert(!close(notice[1]));
    assert(snag_wakeup_wait(notice[0], 10000) == 1);
    unsigned char ready;
    assert(read(notice[0], &ready, 1u) == 1 && ready == 'x');
    assert(!kill(child, SIGKILL));
    int status;
    assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGKILL && !close(notice[0]));
    check_file(&fixture, "checkpoint.0", gate == GATE_DIR_SYNC ? &expected : &fixture.slots[0]);
    check_file(&fixture, "checkpoint.1", &fixture.slots[1]);
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    struct snag_binary_identity identity;
    struct snag_binary_anchor anchor;
    assert(snag_pread(fixture.journal, header, sizeof(header), 0) == (ssize_t)sizeof(header));
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &anchor));
    assert(anchor.end == fixture.before.end &&
        !memcmp(anchor.digest, fixture.before.digest, sizeof(anchor.digest)));
    snag_buf_free(&expected);
    free_snapshot(&capture);
    fixture_free(&fixture);
}

static void
test_empty_slots(void)
{
    struct fixture fixture;
    fixture_init(&fixture);
    assert(!snag_unlink_at(fixture.directory, "checkpoint.0", false));
    assert(!snag_unlink_at(fixture.directory, "checkpoint.1", false));
    assert(!snag_sync_dir(fixture.directory));
    struct snag_binary_io *io = snag_binary_io_start(fixture.journal, &fixture.before, NULL);
    assert(io);
    uint64_t generations[] = {0u, 0u};
    assert(!snag_binary_io_checkpoint_setup(io, fixture.directory, generations));
    for (unsigned int slot = 0u; slot < 2u; ++slot) {
        struct snag_binary_io_snapshot capture = snapshot(&fixture, &fixture.before, slot + 1u);
        struct snag_buf expected = image(&capture, slot + 1u);
        assert(!snag_binary_io_checkpoint_submit(io, &capture));
        struct snag_binary_publication_result result;
        assert(!await_checkpoint(io, &result));
        assert(result.slot == slot && result.generation == slot + 1u);
        const char *name = slot ? "checkpoint.1" : "checkpoint.0";
        check_file(&fixture, name, &expected);
        snag_file_info info;
        assert(!snag_lstat_at(fixture.directory, name, &info));
        assert(S_ISREG(info.st_mode) && !(info.st_mode & 077u));
        snag_buf_free(&expected);
    }
    assert(!snag_binary_io_close(io));
    fixture_free(&fixture);
}

static void
test_invalid(void)
{
    struct fixture fixture;
    fixture_init(&fixture);
    struct snag_binary_io *io = snag_binary_io_start(fixture.journal, &fixture.before, NULL);
    assert(io);
    struct snag_binary_io_snapshot capture = snapshot(&fixture, &fixture.before, 10u);
    struct snag_binary_io_snapshot saved = capture;
    assert(snag_binary_io_checkpoint_submit(io, &capture) < 0 && errno == EINVAL);
    assert(!memcmp(&capture, &saved, sizeof(capture)));
    uint64_t generations[] = {5u, 8u};
    assert(snag_binary_io_checkpoint_setup(NULL, fixture.directory, generations) < 0);
    assert(snag_binary_io_checkpoint_setup(io, -1, generations) < 0);
    assert(snag_binary_io_checkpoint_setup(io, fixture.directory, NULL) < 0);
    assert(!snag_binary_io_checkpoint_setup(io, fixture.directory, generations));
    assert(snag_binary_io_checkpoint_setup(io, fixture.directory, generations) < 0);
    assert(snag_binary_io_checkpoint_submit(NULL, &capture) < 0);
    assert(snag_binary_io_checkpoint_submit(io, NULL) < 0);
    assert(snag_binary_io_checkpoint_retry(NULL) < 0);
    assert(snag_binary_io_checkpoint_retry(io) < 0 && errno == EBUSY);
    for (unsigned int mode = 0u; mode < 11u; ++mode) {
        capture = saved;
        if (mode == 0u) capture.boundary.digest[0] ^= 1u;
        if (mode == 1u) capture.provider = capture.core;
        if (mode == 2u) capture.provider_version = 0u;
        if (mode == 3u) capture.core.len = capture.core.cap + 1u;
        if (mode == 4u) capture.access = capture.core;
        if (mode == 5u) capture.access = capture.provider;
        if (mode == 6u) capture.access_version = 0u;
        if (mode == 7u) capture.access.len = capture.access.cap + 1u;
        if (mode == 8u) capture.access.max = 0u;
        if (mode == 9u) capture.access.data = NULL;
        if (mode == 10u) capture.access.len = 0u;
        struct snag_binary_io_snapshot bad = capture;
        assert(snag_binary_io_checkpoint_submit(io, &capture) < 0);
        assert(!memcmp(&capture, &bad, sizeof(capture)));
    }
    capture = saved;
    assert(!snag_binary_io_checkpoint_submit(io, &capture));
    struct snag_binary_publication_result result;
    assert(snag_binary_io_checkpoint_take(NULL, &result) < 0);
    assert(snag_binary_io_checkpoint_take(io, NULL) < 0);
    assert(!await_checkpoint(io, &result));
    assert(!snag_binary_io_close(io));
    io = snag_binary_io_start(fixture.journal, &fixture.before, NULL);
    assert(io);
    generations[1] = UINT64_MAX;
    assert(!snag_binary_io_checkpoint_setup(io, fixture.directory, generations));
    capture = snapshot(&fixture, &fixture.before, 1u);
    saved = capture;
    assert(snag_binary_io_checkpoint_submit(io, &capture) < 0 && errno == EOVERFLOW);
    assert(!memcmp(&capture, &saved, sizeof(capture)));
    free_snapshot(&capture);
    assert(!snag_binary_io_close(io));
    fixture_free(&fixture);
}

void
test_store_binary_publish(void)
{
    test_invalid();
    test_empty_slots();
    for (enum gate gate = GATE_FILE_SYNC; gate <= GATE_DIR_SYNC; ++gate) {
        test_publication_gates(gate);
        test_termination(gate);
    }
    for (enum fault fault = FAULT_CREATE; fault <= FAULT_UNSUPPORTED; ++fault) {
        test_retry(fault);
    }
    test_priority_and_wake(false);
    test_priority_and_wake(true);
    test_failed_close();
    for (unsigned int mode = 0u; mode < 3u; ++mode) {
        test_temporary_conflict(mode);
    }
    test_source_identity(0u);
    test_source_identity(1u);
}
