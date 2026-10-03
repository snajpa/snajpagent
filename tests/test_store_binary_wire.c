/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_wire.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void test_store_binary_wire(void);

static void
check_header(const unsigned char *data, size_t size, const struct snag_buf *wire)
{
    unsigned char header[SNAG_BINARY_BATCH_HEADER_SIZE];
    unsigned char saved[sizeof(header)];
    memset(saved, 0x5a, sizeof(saved));
    if (size < sizeof(header)) {
        memcpy(header, saved, sizeof(header));
        assert(snag_binary_wire_header(wire->data, wire->len, header) < 0);
        assert(!memcmp(header, saved, sizeof(header)));
        return;
    }
    for (size_t cut = 0u; cut <= sizeof(header) + 1u; ++cut) {
        memcpy(header, saved, sizeof(header));
        int rc = snag_binary_wire_header(wire->data, cut, header);
        assert(rc == (cut < sizeof(header) + 1u ? 1 : 0));
        assert(!memcmp(header, rc ? saved : data, sizeof(header)));
    }
    assert(!snag_binary_wire_header(wire->data, wire->len, header));
    assert(!memcmp(header, data, sizeof(header)));
}

static void
round_trip(const unsigned char *data, size_t size, const char *golden)
{
    struct snag_buf wire = {.max = SNAG_BINARY_WIRE_BATCH_MAX};
    struct snag_buf decoded = {.max = SNAG_BINARY_BATCH_MAX};
    size_t needed;
    assert(!snag_binary_wire_size(size, &needed));
    assert(!snag_binary_wire_encode(&wire, data, size) && wire.len == needed);
    assert(!wire.data[wire.len - 1u] && !memchr(wire.data, 0, wire.len - 1u));
    if (golden) {
        struct snag_sha256 hash;
        unsigned char digest[32];
        snag_sha256_init(&hash);
        snag_sha256_update(&hash, wire.data, wire.len);
        snag_sha256_final(&hash, digest);
        for (size_t i = 0u; i < sizeof(digest); ++i) {
            unsigned int value;
            assert(sscanf(golden + 2u * i, "%2x", &value) == 1 && value == digest[i]);
        }
    }
    assert(!snag_binary_wire_decode(&decoded, wire.data, wire.len));
    assert(decoded.len == size && (!size || !memcmp(decoded.data, data, size)));
    check_header(data, size, &wire);
    snag_buf_free(&wire);
    snag_buf_free(&decoded);
}

static void
test_vectors(void)
{
    static const struct { size_t size; const char *hash; } cases[] = {
        {0u, "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d"},
        {1u, "9f44ac6acb8a37b00b615dc89259d306035be82dee7db2b472e4c48326195440"},
        {2u, "78fcafd6eb07895c571a6c1a7c036e56e409f62f813dcc91c684dc80f222bb5a"},
        {112u, "9ae805a252f760fd2029bde8030e26dce106ca2d3b43d4649de515e91c5940d6"},
        {253u, "9890cb055e97d9ed07f33a621ef22888125e9f9a568cd074fcaac570ae85bc24"},
        {254u, "cad9234527cacf031475915645c5327f1844e1f93b8db68c9884a159aeaba422"},
        {255u, "6cef0718454e0d19a1349eca6183280ff1fdb64f7ec2b85adccc5c5288725d7b"},
        {256u, "f21a14110528a6a700d5d09a73849279ba3faf3d74d7930fc25ca9be6aaeba90"},
        {507u, "f39ae145135c4db188ae9b432c41054cb9d4cd74bc0d88e059009446e1c7eb8b"},
        {508u, "cb5f953115ac5efcae6a6732e9684a4364bfd3bd7a395ba7d89db987bdfdc4fd"},
        {509u, "7e6ceb55b78e3475ca45347d33a1b23c242d8f7d721f2574020f621e9351decb"},
        {1024u, "2840f1d3ed5b0227f941b2662464d7dcd80fd3ffbd4cbd4809add27827a2374c"}
    };
    unsigned char data[1024];
    for (size_t i = 0u; i < sizeof(data); ++i) data[i] = (unsigned char)i;
    /* Independent Python split-at-zero encoding and hashlib expected values. */
    for (size_t i = 0u; i < sizeof(cases) / sizeof(*cases); ++i) {
        round_trip(data, cases[i].size, cases[i].hash);
    }
    for (unsigned int pattern = 0u; pattern < 3u; ++pattern) {
        for (size_t i = 0u; i < sizeof(data); ++i) {
            data[i] = pattern == 0u ? 0u : pattern == 1u ? 0xffu : (unsigned char)i;
        }
        for (size_t size = 0u; size <= sizeof(data); ++size) {
            round_trip(data, size, NULL);
        }
    }
    unsigned char all[SNAG_BINARY_WIRE_BLOCK];
    memset(all, 0xff, sizeof(all));
    struct snag_buf wire = {.max = SNAG_BINARY_WIRE_BATCH_MAX};
    assert(!snag_binary_wire_encode(&wire, all, sizeof(all)));
    assert(wire.len == sizeof(all) + 2u && wire.data[0] == 0xffu &&
        !memcmp(wire.data + 1u, all, sizeof(all)) && !wire.data[wire.len - 1u]);
    snag_buf_free(&wire);
    static const unsigned char plain[] = {1u, 2u, 0u, 3u, 0u, 0u, 4u, 255u};
    static const unsigned char expected[] = {3u, 1u, 2u, 2u, 3u, 1u, 3u, 4u, 255u, 0u};
    wire.max = SNAG_BINARY_WIRE_BATCH_MAX;
    assert(!snag_binary_wire_encode(&wire, plain, sizeof(plain)));
    assert(wire.len == sizeof(expected) && !memcmp(wire.data, expected, sizeof(expected)));
    snag_buf_free(&wire);
}

static void
reject_wire(const void *data, size_t size)
{
    struct snag_buf decoded = {.max = SNAG_BINARY_BATCH_MAX};
    assert(!snag_buf_append(&decoded, "kept", 4u));
    assert(snag_binary_wire_decode(&decoded, data, size) < 0);
    assert(decoded.len == 4u && !memcmp(decoded.data, "kept", 4u));
    snag_buf_free(&decoded);
}

static void
test_rejections(void)
{
    unsigned char raw[1024];
    for (size_t i = 0u; i < sizeof(raw); ++i) raw[i] = (unsigned char)i;
    struct snag_buf wire = {.max = SNAG_BINARY_WIRE_BATCH_MAX};
    assert(!snag_binary_wire_encode(&wire, raw, sizeof(raw)));
    for (size_t cut = 0u; cut < wire.len; ++cut) reject_wire(wire.data, cut);
    for (size_t i = 0u; i < wire.len - 1u; ++i) {
        unsigned char saved = wire.data[i];
        wire.data[i] = 0u;
        reject_wire(wire.data, wire.len);
        wire.data[i] = saved;
    }
    wire.data[wire.len - 1u] = 1u;
    reject_wire(wire.data, wire.len);
    wire.data[wire.len - 1u] = 0u;
    const unsigned char invalid[][5] = {
        {1u, 0u}, {3u, 7u, 0u}, {2u, 0u, 0u}, {2u, 7u, 3u, 9u, 0u}
    };
    const size_t lengths[] = {2u, 3u, 3u, 5u};
    for (size_t i = 0u; i < 4u; ++i) reject_wire(invalid[i], lengths[i]);
    reject_wire(NULL, 1u);
    reject_wire(wire.data, SNAG_BINARY_WIRE_BATCH_MAX + 1u);
    assert(errno == EOVERFLOW);
    assert(snag_binary_wire_decode(NULL, wire.data, wire.len) < 0);
    struct snag_buf small = {.max = sizeof(raw) - 1u};
    assert(snag_binary_wire_decode(&small, wire.data, wire.len) < 0 && errno == EOVERFLOW);
    assert(!small.len && !small.data);
    assert(snag_binary_wire_encode(&small, raw, sizeof(raw)) < 0 && errno == EOVERFLOW);
    assert(!small.len && !small.data);
    assert(snag_binary_wire_encode(&small, NULL, 1u) < 0);
    assert(snag_binary_wire_encode(NULL, raw, sizeof(raw)) < 0);
    assert(snag_binary_wire_encode(&small, raw, SNAG_BINARY_BATCH_MAX + 1u) < 0);
    assert(errno == EOVERFLOW && !small.len && !small.data);
    size_t kept = 17u;
    assert(snag_binary_wire_size(SIZE_MAX, &kept) < 0 && errno == EOVERFLOW && kept == 17u);
    assert(snag_binary_wire_size(0u, NULL) < 0);
    unsigned char header[SNAG_BINARY_BATCH_HEADER_SIZE];
    memset(header, 0x5a, sizeof(header));
    unsigned char saved[sizeof(header)];
    memcpy(saved, header, sizeof(saved));
    static const unsigned char bad_prefix[] = {1u, 255u};
    assert(snag_binary_wire_header(bad_prefix, sizeof(bad_prefix), header) < 0);
    assert(snag_binary_wire_header(NULL, 1u, header) < 0);
    assert(snag_binary_wire_header(wire.data, wire.len, NULL) < 0);
    assert(!memcmp(header, saved, sizeof(header)));
    snag_buf_free(&wire);
}

static void
test_alias_and_maximum(void)
{
    struct snag_buf source = {.max = 8192u};
    unsigned char raw[1024];
    for (size_t i = 0u; i < sizeof(raw); ++i) raw[i] = (unsigned char)(i * 7u);
    assert(!snag_buf_append(&source, raw, sizeof(raw)));
    assert(!snag_binary_wire_encode(&source, source.data, source.len));
    size_t wire_start = sizeof(raw);
    size_t decoded_start = source.len;
    assert(!snag_binary_wire_decode(&source, source.data + wire_start, source.len - wire_start));
    assert(source.len == decoded_start + sizeof(raw));
    assert(!memcmp(source.data, raw, sizeof(raw)) &&
        !memcmp(source.data + decoded_start, raw, sizeof(raw)));
    assert(!snag_binary_wire_header(source.data + wire_start, decoded_start - wire_start,
        source.data + wire_start));
    assert(!memcmp(source.data + wire_start, raw, SNAG_BINARY_BATCH_HEADER_SIZE));
    snag_buf_free(&source);
    unsigned char *large = malloc(SNAG_BINARY_BATCH_MAX);
    assert(large);
    for (size_t i = 0u; i < SNAG_BINARY_BATCH_MAX; ++i) {
        large[i] = (unsigned char)i;
    }
    round_trip(large, SNAG_BINARY_BATCH_MAX, NULL);
    memset(large, 0, SNAG_BINARY_BATCH_MAX);
    round_trip(large, SNAG_BINARY_BATCH_MAX, NULL);
    memset(large, 0xff, SNAG_BINARY_BATCH_MAX);
    round_trip(large, SNAG_BINARY_BATCH_MAX, NULL);
    free(large);
    size_t size;
    assert(!snag_binary_wire_size(SNAG_BINARY_BATCH_MAX, &size));
    assert(size == SNAG_BINARY_WIRE_BATCH_MAX);
}

static void
test_batch_envelope(void)
{
    struct snag_binary_identity identity = {.created_ms = 42u};
    identity.id[0] = 17u;
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &identity);
    struct snag_binary_anchor before;
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &before));
    static const unsigned char payload[] = "SNAGBAT\0SNAGEND\0arbitrary\0payload";
    struct snag_binary_record record = {.kind = 0x8fffu, .version = 1u,
        .flags = SNAG_BINARY_RECORD_OPTIONAL, .payload = payload, .size = sizeof(payload)};
    struct snag_buf batch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf wire = {.max = SNAG_BINARY_WIRE_BATCH_MAX};
    struct snag_buf plain = {.max = SNAG_BINARY_BATCH_MAX};
    assert(!snag_binary_batch_encode(&batch, &before, &record, 1u, 0u));
    assert(!snag_binary_wire_encode(&wire, batch.data, batch.len));
    assert(!memchr(wire.data, 0, wire.len - 1u));
    unsigned char prefix[SNAG_BINARY_BATCH_HEADER_SIZE];
    assert(!snag_binary_wire_header(wire.data, wire.len, prefix));
    assert(!memcmp(prefix, batch.data, sizeof(prefix)));
    struct snag_binary_batch decoded;
    struct snag_binary_anchor after;
    assert(snag_binary_batch_decode(prefix, sizeof(prefix), &before, &decoded, &after) == 1);
    assert(!snag_binary_wire_decode(&plain, wire.data, wire.len));
    assert(!snag_binary_batch_decode(plain.data, plain.len, &before, &decoded, &after));
    assert(plain.len == batch.len && !memcmp(plain.data, batch.data, batch.len));
    snag_buf_free(&batch);
    snag_buf_free(&wire);
    snag_buf_free(&plain);
}

void
test_store_binary_wire(void)
{
    test_vectors();
    test_rejections();
    test_alias_and_maximum();
    test_batch_envelope();
}
