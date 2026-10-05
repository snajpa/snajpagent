/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary.h"
#include "fs.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void test_store_binary_image(void);

struct image_fixture {
    int fd;
    struct snag_binary_identity identity;
    struct snag_binary_checkpoint_receipt receipt;
    struct snag_buf bytes;
};

static void
write_image(struct image_fixture *fixture, size_t size)
{
    assert(!snag_truncate(fixture->fd, 0));
    assert(snag_seek(fixture->fd, 0, SEEK_SET) == 0);
    assert(!snag_write_full(fixture->fd, fixture->bytes.data, size));
    assert(snag_seek(fixture->fd, 13, SEEK_SET) == 13);
}

static void
fixture_init(struct image_fixture *fixture)
{
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snag-binary-image-XXXXXX");
    assert(path);
    fixture->fd = mkstemp(path);
    assert(fixture->fd >= 0 && !unlink(path));
    free(path);
    fixture->identity.created_ms = 42u;
    fixture->identity.id[0] = 17u;
    struct snag_binary_checkpoint_frame frame = {.identity = fixture->identity,
        .generation = 9u, .boundary = {.end = 512u, .next_seq = 7u, .turns = 2u, .previous = 96u}};
    size_t size = 150009u;
    unsigned char *core = malloc(size);
    assert(core);
    for (size_t i = 0u; i < size; ++i) core[i] = (unsigned char)i;
    frame.core = (struct snag_binary_checkpoint_section){.version = 1u, .data = core, .size = size};
    frame.provider = (struct snag_binary_checkpoint_section){.version = 2u,
        .data = (const unsigned char *)"provider", .size = 8u};
    frame.access = (struct snag_binary_checkpoint_section){.version = 1u,
        .data = (const unsigned char *)"access", .size = 6u};
    fixture->bytes.max = SIZE_MAX;
    assert(!snag_binary_checkpoint_frame_encode(&fixture->bytes, &frame));
    free(core);
    fixture->receipt = (struct snag_binary_checkpoint_receipt){.generation = frame.generation,
        .boundary = frame.boundary, .image_size = fixture->bytes.len};
    memcpy(fixture->receipt.image_digest, fixture->bytes.data + fixture->bytes.len - 32u, 32u);
    write_image(fixture, fixture->bytes.len);
}

struct interference {
    struct image_fixture *fixture;
    unsigned calls, stop, fault, at;
};

static bool
interfere(void *opaque)
{
    struct interference *event = opaque;
    ++event->calls;
    if (event->calls == (event->at ? event->at : 3u) && event->fault) {
        int fd = event->fixture->fd;
        size_t size = event->fixture->bytes.len;
        if (event->fault == 1u) assert(!snag_truncate(fd, (int64_t)size - 1));
        if (event->fault == 2u) assert(!snag_truncate(fd, (int64_t)size + 1));
        if (event->fault == 3u) {
            assert(snag_seek(fd, (int64_t)size - 80, SEEK_SET) == (int64_t)size - 80);
            unsigned char changed = event->fixture->bytes.data[size - 80u] ^ 1u;
            assert(!snag_write_full(fd, &changed, 1u));
            assert(snag_seek(fd, 13, SEEK_SET) == 13);
        }
    }
    return event->calls == event->stop;
}

static void
reject_read(struct image_fixture *fixture, struct snag_buf *out,
    struct interference *event, int expected_errno)
{
    struct snag_binary_checkpoint_frame frame, saved_frame;
    memset(&frame, 0x5a, sizeof(frame));
    memcpy(&saved_frame, &frame, sizeof(frame));
    struct snag_buf saved = *out;
    assert(snag_binary_checkpoint_image_read(fixture->fd, &fixture->identity,
        &fixture->receipt, out, &frame, event ? interfere : NULL, event) < 0);
    assert(errno == expected_errno);
    assert(!memcmp(&frame, &saved_frame, sizeof(frame)));
    assert(!memcmp(out, &saved, sizeof(saved)));
    assert(out->len == 4u && !memcmp(out->data, "kept", 4u));
    assert(snag_seek(fixture->fd, 0, SEEK_CUR) == 13);
}

void
test_store_binary_image(void)
{
    struct image_fixture fixture = {0};
    fixture_init(&fixture);
    unsigned char hash[32], saved_hash[32];
    assert(!snag_binary_checkpoint_image_probe(fixture.fd, hash));
    assert(!memcmp(hash, fixture.receipt.image_digest, 32u));
    memcpy(saved_hash, hash, 32u);
    struct snag_buf out = {.max = SIZE_MAX};
    assert(!snag_buf_append(&out, "kept", 4u));
    struct snag_binary_checkpoint_frame frame;
    for (unsigned stop = 1u; stop <= 5u; ++stop) {
        struct interference event = {.fixture = &fixture, .stop = stop};
        reject_read(&fixture, &out, &event, ECANCELED);
        assert(event.calls == stop);
    }
    for (unsigned fault = 1u; fault <= 3u; ++fault) {
        struct interference event = {.fixture = &fixture, .fault = fault};
        reject_read(&fixture, &out, &event, fault == 1u ? EIO : fault == 2u ? ESTALE : EINVAL);
        write_image(&fixture, fixture.bytes.len);
    }
    /* A final yield can mutate the source even after all bytes were verified. */
    for (unsigned fault = 1u; fault <= 3u; ++fault) {
        struct interference event = {.fixture = &fixture, .fault = fault, .at = 5u};
        reject_read(&fixture, &out, &event, ESTALE);
        write_image(&fixture, fixture.bytes.len);
    }
    /* Extent mismatch wins over allocation failure, even with a zero budget. */
    out.max = 0u;
    for (int change = -1; change <= 1; change += 2) {
        assert(!snag_truncate(fixture.fd, (int64_t)fixture.bytes.len + change));
        reject_read(&fixture, &out, NULL, EINVAL);
        write_image(&fixture, fixture.bytes.len);
    }
    reject_read(&fixture, &out, NULL, EOVERFLOW);
    out.max = SIZE_MAX;

    int directory = snag_open_read(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", true);
    assert(directory >= 0);
    struct snag_buf saved_out = out;
    memset(&frame, 0x5a, sizeof(frame));
    struct snag_binary_checkpoint_frame saved_frame;
    memcpy(&saved_frame, &frame, sizeof(frame));
    assert(snag_binary_checkpoint_image_probe(directory, hash) < 0 && errno == EINVAL);
    assert(!memcmp(hash, saved_hash, 32u));
    assert(snag_binary_checkpoint_image_read(directory, &fixture.identity,
        &fixture.receipt, &out, &frame, NULL, NULL) < 0 && errno == EINVAL);
    assert(!memcmp(&frame, &saved_frame, sizeof(frame)));
    assert(!memcmp(&out, &saved_out, sizeof(out)) && !memcmp(out.data, "kept", 4u));
    assert(!close(directory));

    struct snag_binary_checkpoint_receipt saved_receipt = fixture.receipt;
    for (unsigned fault = 0u; fault < 4u; ++fault) {
        if (fault == 0u) ++fixture.receipt.generation;
        if (fault == 1u) fixture.receipt.image_digest[0] ^= 1u;
        if (fault == 2u) fixture.receipt.boundary.digest[0] ^= 1u;
        if (fault == 3u) fixture.identity.id[0] ^= 1u;
        reject_read(&fixture, &out, NULL, EINVAL);
        fixture.receipt = saved_receipt;
        if (fault == 3u) fixture.identity.id[0] ^= 1u;
    }

    /* The footer probe supplies only a lookup key; it does not verify the body. */
    fixture.bytes.data[200u] ^= 1u;
    write_image(&fixture, fixture.bytes.len);
    assert(!snag_binary_checkpoint_image_probe(fixture.fd, hash));
    assert(!memcmp(hash, saved_hash, 32u));
    reject_read(&fixture, &out, NULL, EINVAL);
    fixture.bytes.data[200u] ^= 1u;
    write_image(&fixture, fixture.bytes.len);
    for (size_t size = 0u; size < 210u; ++size) {
        assert(!snag_truncate(fixture.fd, (int64_t)size));
        assert(snag_binary_checkpoint_image_probe(fixture.fd, hash) < 0 && errno == EINVAL);
        assert(!memcmp(hash, saved_hash, 32u));
        reject_read(&fixture, &out, NULL, EINVAL);
    }
    write_image(&fixture, fixture.bytes.len);
    struct interference event = {.fixture = &fixture, .stop = 6u};
    assert(!snag_binary_checkpoint_image_read(fixture.fd, &fixture.identity,
        &fixture.receipt, &out, &frame, interfere, &event));
    assert(event.calls == 5u && out.len == fixture.bytes.len);
    assert(!memcmp(out.data, fixture.bytes.data, out.len));
    assert(frame.core.data == out.data + SNAG_BINARY_CHECKPOINT_HEADER_SIZE);
    assert(frame.core.size == 150009u && frame.provider.size == 8u && frame.access.size == 6u);
    assert(!memcmp(frame.provider.data, "provider", 8u));
    assert(!memcmp(frame.access.data, "access", 6u));
    assert(snag_seek(fixture.fd, 0, SEEK_CUR) == 13);
    assert(snag_binary_checkpoint_image_probe(-1, hash) < 0);
    assert(snag_binary_checkpoint_image_probe(fixture.fd, NULL) < 0);
    assert(snag_binary_checkpoint_image_read(fixture.fd, NULL,
        &fixture.receipt, &out, &frame, NULL, NULL) < 0);
    assert(snag_binary_checkpoint_image_read(fixture.fd, &fixture.identity,
        NULL, &out, &frame, NULL, NULL) < 0);
    assert(snag_binary_checkpoint_image_read(fixture.fd, &fixture.identity,
        &fixture.receipt, NULL, &frame, NULL, NULL) < 0);
    assert(snag_binary_checkpoint_image_read(fixture.fd, &fixture.identity,
        &fixture.receipt, &out, NULL, NULL, NULL) < 0);
    assert(!close(fixture.fd));
    assert(snag_binary_checkpoint_image_probe(fixture.fd, hash) < 0 && errno == EBADF);
    assert(!memcmp(hash, saved_hash, 32u));
    assert(snag_binary_checkpoint_image_read(fixture.fd, &fixture.identity,
        &fixture.receipt, &out, &frame, NULL, NULL) < 0 && errno == EBADF);
    snag_buf_free(&out);
    snag_buf_free(&fixture.bytes);
}
