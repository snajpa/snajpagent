/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_publish.h"
#include "fs.h"
#include "store_binary_io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A maintenance yield quantum, independent of total snapshot size. */
#define CHECKPOINT_CHUNK 65536u

enum publication_phase { CP_CREATE, CP_WRITE, CP_FILE_SYNC, CP_RENAME, CP_DIR_SYNC, CP_COMPLETE };

struct snag_binary_publication {
    int journal;
    int directory;
    int file;
    enum publication_phase phase;
    struct snag_binary_io_snapshot snapshot;
    struct snag_binary_checkpoint_encoder encoder;
    struct snag_binary_publication_result result;
};

struct snag_binary_publication *
snag_binary_publication_new(int journal, int directory, const uint64_t generations[2],
    struct snag_binary_io_snapshot *snapshot)
{
    if (journal < 0 || directory < 0 || !generations || !snapshot ||
        snapshot->core.data == snapshot->provider.data ||
        (snapshot->access.data && (snapshot->access.data == snapshot->core.data ||
            snapshot->access.data == snapshot->provider.data)) ||
        snapshot->core.len > snapshot->core.max || snapshot->core.len > snapshot->core.cap ||
        snapshot->provider.len > snapshot->provider.max ||
        snapshot->provider.len > snapshot->provider.cap ||
        snapshot->access.len > snapshot->access.max ||
        snapshot->access.len > snapshot->access.cap) {
        errno = EINVAL;
        return NULL;
    }
    uint64_t latest = generations[0] > generations[1] ? generations[0] : generations[1];
    if (latest == UINT64_MAX) {
        errno = EOVERFLOW;
        return NULL;
    }
    struct snag_binary_publication *publication = calloc(1u, sizeof(*publication));
    if (!publication) return NULL;
    publication->journal = journal;
    publication->directory = directory;
    publication->file = -1;
    publication->result.boundary = snapshot->boundary;
    publication->result.generation = latest + 1u;
    publication->result.slot = generations[0] <= generations[1] ? 0u : 1u;
    struct snag_binary_checkpoint_frame frame = {
        .identity = snapshot->identity, .boundary = snapshot->boundary,
        .generation = publication->result.generation,
        .core = {snapshot->core_version, snapshot->core.data, snapshot->core.len},
        .provider = {snapshot->provider_version, snapshot->provider.data, snapshot->provider.len},
        .access = {snapshot->access_version, snapshot->access.data, snapshot->access.len}
    };
    if (snag_binary_checkpoint_encoder_init(&publication->encoder, &frame) < 0) {
        free(publication);
        return NULL;
    }
    publication->snapshot = *snapshot;
    memset(snapshot, 0, sizeof(*snapshot));
    return publication;
}

static int
check_identity(struct snag_binary_publication *publication)
{
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    size_t offset = 0u;
    while (offset < sizeof(header)) {
        ssize_t count = snag_pread(publication->journal, header + offset,
            sizeof(header) - offset, (int64_t)offset);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return -1;
        if (!count) return snag_errno(ESTALE);
        offset += (size_t)count;
    }
    struct snag_binary_identity identity;
    struct snag_binary_anchor start;
    if (snag_binary_header_decode(header, sizeof(header), &identity, &start) != 0) {
        return snag_errno(EINVAL);
    }
    snag_file_info info;
    if (snag_fstat(publication->journal, &info) < 0) return -1;
    if (memcmp(identity.id, publication->snapshot.identity.id, sizeof(identity.id)) ||
        identity.created_ms != publication->snapshot.identity.created_ms ||
        info.st_size < 0 || (uint64_t)info.st_size < publication->snapshot.boundary.end) {
        return snag_errno(ESTALE);
    }
    return 0;
}

static int
create_temporary(struct snag_binary_publication *publication,
    const struct snag_binary_io_ops *ops)
{
    if (check_identity(publication) < 0) return -1;
    char id[SNAG_ID_HEX_LEN + 1u];
    if (snag_random_id(id) < 0) return -1;
    char name[SNAG_BINARY_CP_TEMP_SIZE];
    int length = snprintf(name, sizeof(name), ".checkpoint.%s.tmp", id);
    if (length < 0 || (size_t)length >= sizeof(name)) {
        return snag_errno(EOVERFLOW);
    }
    int fd = ops->create_private(ops->opaque, publication->directory, name);
    if (fd < 0) return -1;
    publication->file = fd;
    memcpy(publication->result.temporary, name, (size_t)length + 1u);
    publication->phase = CP_WRITE;
    return 1;
}

static int
write_chunk(struct snag_binary_publication *publication, const struct snag_binary_io_ops *ops)
{
    struct snag_binary_checkpoint_encoder next = publication->encoder;
    const unsigned char *bytes;
    size_t size;
    int rc = snag_binary_checkpoint_encoder_next(&next, CHECKPOINT_CHUNK, &bytes, &size);
    if (rc < 0) return -1;
    if (rc) {
        publication->phase = CP_FILE_SYNC;
        return 1;
    }
    snag_file_info info;
    if (snag_fstat(publication->file, &info) < 0) return -1;
    size_t start = publication->encoder.position;
    if (info.st_size < 0 || (uint64_t)info.st_size < start ||
        (uint64_t)info.st_size > next.position) {
        return snag_errno(ESTALE);
    }
    size_t present = (size_t)((uint64_t)info.st_size - start);
    unsigned char scratch[8192u];
    size_t checked = 0u;
    while (checked < present) {
        size_t count = present - checked;
        if (count > sizeof(scratch)) count = sizeof(scratch);
        ssize_t got = snag_pread(publication->file, scratch, count, (int64_t)(start + checked));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return -1;
        if (!got || memcmp(scratch, bytes + checked, (size_t)got)) {
            return snag_errno(ESTALE);
        }
        checked += (size_t)got;
    }
    int64_t end = snag_seek(publication->file, 0, SEEK_END);
    if (end < 0) return -1;
    if (end != info.st_size) return snag_errno(ESTALE);
    if (present < size && ops->write_full(ops->opaque, publication->file,
        bytes + present, size - present) < 0) {
        return -1;
    }
    if (snag_fstat(publication->file, &info) < 0) return -1;
    if (info.st_size < 0 || (uint64_t)info.st_size != next.position) {
        return snag_errno(ESTALE);
    }
    publication->encoder = next;
    return 1;
}

static int
same_file(struct snag_binary_publication *publication, const char *name)
{
    snag_file_info held;
    snag_file_info named;
    if (snag_fstat(publication->file, &held) < 0) return -1;
    if (snag_lstat_at(publication->directory, name, &named) < 0) {
        return errno == ENOENT ? 0 : -1;
    }
    return S_ISREG(named.st_mode) && held.st_dev == named.st_dev && held.st_ino == named.st_ino;
}

static int
rename_checkpoint(struct snag_binary_publication *publication,
    const struct snag_binary_io_ops *ops)
{
    const char *slot = publication->result.slot ? "checkpoint.1" : "checkpoint.0";
    int matched = same_file(publication, slot);
    if (matched < 0) return -1;
    /* Reconcile a rename that happened despite its reported failure. */
    if (!matched) {
        matched = same_file(publication, publication->result.temporary);
        if (matched < 0) return -1;
        if (!matched) return snag_errno(ESTALE);
        if (ops->rename_at(ops->opaque, publication->directory, publication->result.temporary,
            publication->directory, slot) < 0) {
            return -1;
        }
    }
    publication->result.renamed = true;
    publication->phase = CP_DIR_SYNC;
    return 1;
}

int
snag_binary_publication_step(struct snag_binary_publication *publication,
    const struct snag_binary_io_ops *ops)
{
    int rc;
    switch (publication->phase) {
    case CP_CREATE:
        rc = create_temporary(publication, ops);
        break;
    case CP_WRITE:
        rc = write_chunk(publication, ops);
        break;
    case CP_FILE_SYNC:
        rc = ops->sync_file(ops->opaque, publication->file);
        if (!rc) {
            publication->phase = CP_RENAME;
            rc = 1;
        }
        break;
    case CP_RENAME:
        rc = rename_checkpoint(publication, ops);
        break;
    case CP_DIR_SYNC:
        rc = ops->sync_dir(ops->opaque, publication->directory);
        if (rc > 0) rc = snag_errno(ENOTSUP);
        if (!rc) {
            publication->result.published = true;
            publication->result.image_size = publication->encoder.total;
            memcpy(publication->result.image_digest,
                publication->encoder.footer + SNAG_BINARY_CHECKPOINT_FOOTER_SIZE - 32u, 32u);
            publication->phase = CP_COMPLETE;
            snag_binary_publication_close(publication);
        }
        break;
    case CP_COMPLETE:
        rc = 0;
        break;
    default:
        rc = snag_errno(EINVAL);
        break;
    }
    publication->result.error = rc < 0 ? (errno ? errno : EIO) : 0;
    return rc;
}

void
snag_binary_publication_result(const struct snag_binary_publication *publication,
    struct snag_binary_publication_result *out)
{
    *out = publication->result;
}

void
snag_binary_publication_close(struct snag_binary_publication *publication)
{
    if (publication && publication->file >= 0) {
        (void)close(publication->file);
        publication->file = -1;
    }
}

void
snag_binary_publication_free(struct snag_binary_publication *publication)
{
    if (!publication) return;
    snag_buf_free(&publication->snapshot.core);
    snag_buf_free(&publication->snapshot.provider);
    snag_buf_free(&publication->snapshot.access);
    free(publication);
}
