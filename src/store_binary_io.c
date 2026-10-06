/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_io.h"
#include "fs.h"
#include "store_binary_wire.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum io_phase { IO_IDLE, IO_QUEUED, IO_RUNNING, IO_DONE, IO_FAILED };

struct io_request {
    struct snag_binary_record *records;
    unsigned char *payloads;
    uint32_t count;
    uint64_t turns;
    struct snag_binary_anchor before;
    struct snag_binary_anchor after;
    struct snag_buf bytes;
    struct snag_buf decoded;
    bool attempted_io;
    bool retried;
    bool checkpoint_receipt;
};

struct snag_binary_io {
    int fd;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    snag_wake_fd wake[2];
    struct snag_binary_io_ops ops;
    enum io_phase phase;
    bool stopping;
    struct io_request *request;
    struct snag_binary_io_result result;
    int directory;
    uint64_t generations[2];
    uint64_t sequences[2];
    bool awaiting_receipt;
    struct snag_binary_publication_result published;
    enum io_phase checkpoint_phase;
    struct snag_binary_publication *checkpoint;
};

static int
write_native(void *opaque, int fd, const void *bytes, size_t size)
{
    (void)opaque;
    return snag_write_full(fd, bytes, size);
}

static int
sync_native(void *opaque, int fd)
{
    (void)opaque;
    return snag_sync_file(fd);
}

static int
create_native(void *opaque, int directory, const char *name)
{
    (void)opaque;
    return snag_create_private_at(directory, name, true);
}

static int
rename_native(void *opaque, int from_directory, const char *from, int to_directory, const char *to)
{
    (void)opaque;
    return snag_rename_at(from_directory, from, to_directory, to);
}

static int
sync_directory(void *opaque, int fd)
{
    (void)opaque;
    return snag_sync_dir(fd);
}

/* Both completion streams share a level-triggered wakeup. Consuming one must
 * preserve readiness for an unconsumed completion on the other stream. */
static void
refresh_wake(struct snag_binary_io *io)
{
    snag_wakeup_drain(io->wake[0]);
    if (io->phase == IO_DONE || io->checkpoint_phase == IO_DONE) {
        snag_wakeup_send(io->wake[1]);
    }
}

static void
request_free(struct io_request *request)
{
    if (!request) return;
    free(request->records);
    free(request->payloads);
    snag_buf_free(&request->bytes);
    snag_buf_free(&request->decoded);
    free(request);
}

static struct io_request *
request_copy(const struct snag_binary_record *records, uint32_t count, uint64_t turns)
{
    if (!records || !count) {
        errno = EINVAL;
        return NULL;
    }
    size_t limit = count == 1u ? SNAG_BINARY_BATCH_MAX : SNAG_BINARY_BATCH_TARGET;
    size_t total = SNAG_BINARY_BATCH_HEADER_SIZE + SNAG_BINARY_BATCH_FOOTER_SIZE;
    if (count > (limit - total) / SNAG_BINARY_RECORD_HEADER_SIZE) {
        errno = E2BIG;
        return NULL;
    }
    size_t payload_size = 0u;
    total += (size_t)count * SNAG_BINARY_RECORD_HEADER_SIZE;
    for (uint32_t i = 0u; i < count; ++i) {
        if ((!records[i].payload && records[i].size) ||
            records[i].size > SNAG_MAX_EVENT_LINE || records[i].size > limit - total) {
            errno = EINVAL;
            return NULL;
        }
        total += records[i].size;
        payload_size += records[i].size;
    }
    struct io_request *request = calloc(1u, sizeof(*request));
    if (!request) return NULL;
    request->records = calloc(count, sizeof(*request->records));
    request->payloads = malloc(payload_size ? payload_size : 1u);
    if (!request->records || !request->payloads) {
        request_free(request);
        return NULL;
    }
    size_t offset = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        request->records[i] = records[i];
        request->records[i].payload = request->payloads + offset;
        if (records[i].size) {
            memcpy(request->payloads + offset, records[i].payload, records[i].size);
        }
        offset += records[i].size;
    }
    request->count = count;
    request->turns = turns;
    request->bytes.max = SNAG_BINARY_WIRE_BATCH_MAX;
    return request;
}

static int
encode_request(struct io_request *request)
{
    if (request->bytes.len) return 0;
    struct snag_buf decoded = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_anchor after;
    int rc = snag_binary_batch_encode(&decoded, &request->before,
        request->records, request->count, request->turns, &after);
    if (rc == 0) rc = snag_binary_wire_encode(&request->bytes, decoded.data, decoded.len);
    if (rc < 0) {
        int code = errno;
        snag_buf_free(&decoded);
        return snag_errno(code);
    }
    request->decoded = decoded;
    request->after = after;
    free(request->records);
    request->records = NULL;
    free(request->payloads);
    request->payloads = NULL;
    return 0;
}

static int
matching_tail(int fd, const struct io_request *request, size_t size)
{
    unsigned char bytes[8192u];
    size_t offset = 0u;
    while (offset < size) {
        size_t count = size - offset;
        if (count > sizeof(bytes)) count = sizeof(bytes);
        ssize_t got = snag_pread(fd, bytes, count,
            (int64_t)(request->before.end + offset));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return -1;
        if (!got || memcmp(bytes, request->bytes.data + offset, (size_t)got)) {
            return snag_errno(ESTALE);
        }
        offset += (size_t)got;
    }
    return 0;
}

static int
commit_request(struct snag_binary_io *io, struct io_request *request)
{
    if (encode_request(request) < 0) return -1;
    request->attempted_io = true;
    snag_file_info info;
    if (snag_fstat(io->fd, &info) < 0) return -1;
    if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
        (uint64_t)info.st_size < request->before.end ||
        (uint64_t)info.st_size > request->after.end ||
        (!request->retried && (uint64_t)info.st_size != request->before.end)) {
        return snag_errno(ESTALE);
    }
    size_t present = (size_t)((uint64_t)info.st_size - request->before.end);
    if (matching_tail(io->fd, request, present) < 0) return -1;
    int64_t end = snag_seek(io->fd, 0, SEEK_END);
    if (end < 0) return -1;
    if (end != info.st_size) return snag_errno(ESTALE);
    if (present < request->bytes.len &&
        io->ops.write_full(io->ops.opaque, io->fd, request->bytes.data + present,
            request->bytes.len - present) < 0) {
        return -1;
    }
    if (snag_fstat(io->fd, &info) < 0) return -1;
    if (info.st_size < 0 || (uint64_t)info.st_size != request->after.end) {
        return snag_errno(ESTALE);
    }
    pthread_mutex_lock(&io->mutex);
    io->result.written = request->after;
    pthread_mutex_unlock(&io->mutex);
    return io->ops.sync_file(io->ops.opaque, io->fd);
}

static void *
run_owner(void *opaque)
{
    struct snag_binary_io *io = opaque;
    pthread_mutex_lock(&io->mutex);
    for (;;) {
        while (io->phase != IO_QUEUED && io->checkpoint_phase != IO_QUEUED && !io->stopping) {
            pthread_cond_wait(&io->changed, &io->mutex);
        }
        if (io->stopping) break;
        if (io->phase != IO_QUEUED) {
            io->checkpoint_phase = IO_RUNNING;
            pthread_mutex_unlock(&io->mutex);
            int rc = snag_binary_publication_step(io->checkpoint, &io->ops);
            pthread_mutex_lock(&io->mutex);
            io->checkpoint_phase = rc > 0 ? IO_QUEUED : IO_DONE;
            if (rc <= 0) snag_wakeup_send(io->wake[1]);
            continue;
        }
        struct io_request *request = io->request;
        io->phase = IO_RUNNING;
        pthread_mutex_unlock(&io->mutex);
        int rc = commit_request(io, request);
        int error = rc < 0 ? (errno ? errno : EIO) : 0;
        pthread_mutex_lock(&io->mutex);
        io->result.error = error;
        io->result.retryable = error && request->attempted_io && !request->retried;
        if (!error) {
            io->result.durable = request->after;
            if (request->checkpoint_receipt) {
                unsigned int slot = io->published.slot;
                io->generations[slot] = io->published.generation;
                io->sequences[slot] = request->before.next_seq;
                io->awaiting_receipt = false;
                io->result.checkpoint_receipt = true;
            }
        }
        io->phase = IO_DONE;
        snag_wakeup_send(io->wake[1]);
    }
    pthread_mutex_unlock(&io->mutex);
    snag_binary_publication_close(io->checkpoint);
    return NULL;
}

struct snag_binary_io *
snag_binary_io_start(int fd, const struct snag_binary_anchor *boundary,
    const struct snag_binary_io_ops *ops)
{
    if (fd < 0 || !boundary || boundary->end < SNAG_BINARY_HEADER_SIZE ||
        boundary->end > INT64_MAX || !boundary->next_seq || boundary->next_seq > INT64_MAX ||
        boundary->turns >= boundary->next_seq || boundary->previous >= boundary->end) {
        errno = EINVAL;
        return NULL;
    }
    struct snag_binary_io *io = calloc(1u, sizeof(*io));
    if (!io) return NULL;
    io->fd = fd;
    io->directory = -1;
    io->wake[0] = io->wake[1] = SNAG_WAKE_INVALID;
    io->result.written = io->result.durable = *boundary;
    if (ops) io->ops = *ops;
    if (!io->ops.write_full) io->ops.write_full = write_native;
    if (!io->ops.sync_file) io->ops.sync_file = sync_native;
    if (!io->ops.create_private) io->ops.create_private = create_native;
    if (!io->ops.rename_at) io->ops.rename_at = rename_native;
    if (!io->ops.sync_dir) io->ops.sync_dir = sync_directory;
    int rc = pthread_mutex_init(&io->mutex, NULL);
    if (rc) {
        free(io);
        errno = rc;
        return NULL;
    }
    rc = pthread_cond_init(&io->changed, NULL);
    if (rc) {
        pthread_mutex_destroy(&io->mutex);
        free(io);
        errno = rc;
        return NULL;
    }
    if (snag_wakeup_create(io->wake) < 0) goto failed;
    rc = pthread_create(&io->thread, NULL, run_owner, io);
    if (rc) {
        errno = rc;
        goto failed;
    }
    return io;
failed:
    rc = errno;
    snag_wakeup_close(io->wake);
    pthread_cond_destroy(&io->changed);
    pthread_mutex_destroy(&io->mutex);
    free(io);
    errno = rc;
    return NULL;
}

static int
submit_request(struct snag_binary_io *io, const struct snag_binary_record *records,
    uint32_t count, uint64_t turns, bool checkpoint_receipt)
{
    if (io->phase != IO_IDLE) return snag_errno(EBUSY);
    struct io_request *request = request_copy(records, count, turns);
    if (!request) return -1;
    request->before = io->result.durable;
    request->checkpoint_receipt = checkpoint_receipt;
    io->request = request;
    io->result.error = 0;
    io->result.retryable = false;
    io->result.checkpoint_receipt = false;
    io->phase = IO_QUEUED;
    pthread_cond_signal(&io->changed);
    return 0;
}

int
snag_binary_io_submit(struct snag_binary_io *io,
    const struct snag_binary_record *records, uint32_t count, uint64_t turns)
{
    if (!io) return snag_errno(EINVAL);
    pthread_mutex_lock(&io->mutex);
    int rc = submit_request(io, records, count, turns, false);
    pthread_mutex_unlock(&io->mutex);
    return rc;
}

snag_wake_fd
snag_binary_io_wake(const struct snag_binary_io *io)
{
    return io ? io->wake[0] : SNAG_WAKE_INVALID;
}

static int
take_result(struct snag_binary_io *io, struct snag_binary_io_result *out,
    struct snag_buf *batch)
{
    if (!io || !out) return snag_errno(EINVAL);
    pthread_mutex_lock(&io->mutex);
    if (io->phase != IO_DONE) {
        bool pending = io->phase == IO_QUEUED || io->phase == IO_RUNNING;
        pthread_mutex_unlock(&io->mutex);
        return pending ? 1 : snag_errno(ENOENT);
    }
    *out = io->result;
    if (!out->error && batch) {
        snag_buf_free(batch);
        *batch = io->request->decoded;
        io->request->decoded = (struct snag_buf){0};
    }
    if (out->error && io->request->attempted_io) {
        io->phase = IO_FAILED;
    } else {
        request_free(io->request);
        io->request = NULL;
        io->phase = IO_IDLE;
    }
    refresh_wake(io);
    pthread_mutex_unlock(&io->mutex);
    return out->error ? snag_errno(out->error) : 0;
}

int
snag_binary_io_take(struct snag_binary_io *io, struct snag_binary_io_result *out)
{
    return take_result(io, out, NULL);
}

int
snag_binary_io_take_batch(struct snag_binary_io *io, struct snag_binary_io_result *out,
    struct snag_buf *batch)
{
    if (!batch) return snag_errno(EINVAL);
    return take_result(io, out, batch);
}

int
snag_binary_io_retry(struct snag_binary_io *io)
{
    if (!io) return snag_errno(EINVAL);
    pthread_mutex_lock(&io->mutex);
    int error = io->phase != IO_FAILED ? EBUSY : io->request->retried ? EALREADY : 0;
    if (error) {
        pthread_mutex_unlock(&io->mutex);
        return snag_errno(error);
    }
    io->request->retried = true;
    io->result.error = 0;
    io->result.retryable = false;
    io->phase = IO_QUEUED;
    pthread_cond_signal(&io->changed);
    pthread_mutex_unlock(&io->mutex);
    return 0;
}

int
snag_binary_io_checkpoint_setup(struct snag_binary_io *io, int directory,
    const uint64_t generations[2], const uint64_t sequences[2])
{
    if (!io || directory < 0 || !generations || !sequences) return snag_errno(EINVAL);
    pthread_mutex_lock(&io->mutex);
    if (io->directory >= 0 || io->phase != IO_IDLE) {
        pthread_mutex_unlock(&io->mutex);
        return snag_errno(EBUSY);
    }
    for (size_t i = 0u; i < 2u; ++i) {
        if (!!generations[i] != !!sequences[i] || sequences[i] >= io->result.durable.next_seq) {
            pthread_mutex_unlock(&io->mutex);
            return snag_errno(EINVAL);
        }
    }
    if (sequences[0] == sequences[1] && generations[0] != generations[1]) {
        pthread_mutex_unlock(&io->mutex);
        return snag_errno(EINVAL);
    }
    io->directory = directory;
    memcpy(io->generations, generations, sizeof(io->generations));
    memcpy(io->sequences, sequences, sizeof(io->sequences));
    pthread_mutex_unlock(&io->mutex);
    return 0;
}

static bool
same_anchor(const struct snag_binary_anchor *left, const struct snag_binary_anchor *right)
{
    return left->end == right->end && left->next_seq == right->next_seq &&
        left->turns == right->turns && left->previous == right->previous &&
        !memcmp(left->digest, right->digest, sizeof(left->digest));
}

int
snag_binary_io_checkpoint_submit(struct snag_binary_io *io,
    struct snag_binary_io_snapshot *snapshot)
{
    if (!io || !snapshot) return snag_errno(EINVAL);
    pthread_mutex_lock(&io->mutex);
    int error = io->checkpoint || io->awaiting_receipt ? EBUSY : io->directory < 0 ? EINVAL :
        !same_anchor(&snapshot->boundary, &io->result.durable) ? ESTALE : 0;
    if (error) {
        pthread_mutex_unlock(&io->mutex);
        return snag_errno(error);
    }
    unsigned int slot = io->sequences[0] <= io->sequences[1] ? 0u : 1u;
    struct snag_binary_publication *publication = snag_binary_publication_new(io->fd,
        io->directory, io->generations, slot, snapshot);
    if (!publication) {
        pthread_mutex_unlock(&io->mutex);
        return -1;
    }
    io->checkpoint = publication;
    io->checkpoint_phase = IO_QUEUED;
    pthread_cond_signal(&io->changed);
    pthread_mutex_unlock(&io->mutex);
    return 0;
}

static int
take_checkpoint_result(struct snag_binary_io *io,
    struct snag_binary_publication_result *out, struct snag_buf *access)
{
    if (!io || !out) return snag_errno(EINVAL);
    pthread_mutex_lock(&io->mutex);
    if (io->checkpoint_phase != IO_DONE) {
        bool pending = io->checkpoint_phase == IO_QUEUED || io->checkpoint_phase == IO_RUNNING;
        pthread_mutex_unlock(&io->mutex);
        return pending ? 1 : snag_errno(ENOENT);
    }
    snag_binary_publication_result(io->checkpoint, out);
    if (out->error) {
        io->checkpoint_phase = IO_FAILED;
    } else {
        io->published = *out;
        io->awaiting_receipt = true;
        if (access) snag_binary_publication_access_move(io->checkpoint, access);
        snag_binary_publication_free(io->checkpoint);
        io->checkpoint = NULL;
        io->checkpoint_phase = IO_IDLE;
    }
    refresh_wake(io);
    pthread_mutex_unlock(&io->mutex);
    return out->error ? snag_errno(out->error) : 0;
}

int
snag_binary_io_checkpoint_take(struct snag_binary_io *io,
    struct snag_binary_publication_result *out)
{
    return take_checkpoint_result(io, out, NULL);
}

int
snag_binary_io_checkpoint_take_access(struct snag_binary_io *io,
    struct snag_binary_publication_result *out, struct snag_buf *access)
{
    if (!access) return snag_errno(EINVAL);
    return take_checkpoint_result(io, out, access);
}

int
snag_binary_io_checkpoint_receipt_submit(struct snag_binary_io *io,
    const unsigned char index_root[32], uint64_t timestamp)
{
    if (!io || !index_root) return snag_errno(EINVAL);
    pthread_mutex_lock(&io->mutex);
    int error = !io->awaiting_receipt ? ENOENT : io->phase != IO_IDLE ? EBUSY : 0;
    if (error) {
        pthread_mutex_unlock(&io->mutex);
        return snag_errno(error);
    }
    struct snag_binary_checkpoint_receipt receipt = {
        .generation = io->published.generation, .image_size = io->published.image_size,
        .boundary = io->published.boundary
    };
    memcpy(receipt.image_digest, io->published.image_digest, sizeof(receipt.image_digest));
    memcpy(receipt.index_root, index_root, sizeof(receipt.index_root));
    struct snag_buf payload = {.max = SIZE_MAX};
    int rc = snag_binary_checkpoint_receipt_encode(&payload, &receipt);
    if (!rc) {
        struct snag_binary_record record = {.kind = SNAG_BINARY_CHECKPOINT_RECEIPT,
            .version = SNAG_BINARY_CHECKPOINT_RECEIPT_VERSION, .flags = SNAG_BINARY_RECORD_OPTIONAL,
            .timestamp_ms = timestamp, .payload = payload.data, .size = payload.len};
        rc = submit_request(io, &record, 1u, io->result.durable.turns, true);
    }
    snag_buf_free(&payload);
    pthread_mutex_unlock(&io->mutex);
    return rc;
}

int
snag_binary_io_checkpoint_retry(struct snag_binary_io *io)
{
    if (!io) return snag_errno(EINVAL);
    pthread_mutex_lock(&io->mutex);
    if (io->checkpoint_phase != IO_FAILED) {
        pthread_mutex_unlock(&io->mutex);
        return snag_errno(EBUSY);
    }
    io->checkpoint_phase = IO_QUEUED;
    pthread_cond_signal(&io->changed);
    pthread_mutex_unlock(&io->mutex);
    return 0;
}

int
snag_binary_io_close(struct snag_binary_io *io)
{
    if (!io) return 0;
    pthread_mutex_lock(&io->mutex);
    if ((io->phase != IO_IDLE && io->phase != IO_FAILED) ||
        (io->checkpoint_phase != IO_IDLE && io->checkpoint_phase != IO_FAILED)) {
        pthread_mutex_unlock(&io->mutex);
        return snag_errno(EBUSY);
    }
    io->stopping = true;
    pthread_cond_signal(&io->changed);
    pthread_mutex_unlock(&io->mutex);
    pthread_join(io->thread, NULL);
    request_free(io->request);
    snag_binary_publication_free(io->checkpoint);
    snag_wakeup_close(io->wake);
    pthread_cond_destroy(&io->changed);
    pthread_mutex_destroy(&io->mutex);
    free(io);
    return 0;
}
