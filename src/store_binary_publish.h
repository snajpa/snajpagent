/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_PUBLISH_H
#define SNAJPAGENT_STORE_BINARY_PUBLISH_H

#include "store_binary.h"

#define SNAG_BINARY_CP_TEMP_SIZE (SNAG_ID_HEX_LEN + sizeof(".checkpoint..tmp"))

/* Owned immutable section buffers captured at one already durable boundary.
 * Their semantic validity is established by the engine before submission.
 * Core/provider are required; access version/length may both be zero for an
 * image requiring independent full-prefix verification instead of bounded resume. */
struct snag_binary_io_snapshot {
    struct snag_binary_identity identity;
    struct snag_binary_anchor boundary;
    struct snag_buf core;
    struct snag_buf provider;
    struct snag_buf access;
    uint16_t core_version;
    uint16_t provider_version;
    uint16_t access_version;
};

/* Renamed means confirmed replacement; false does not exclude an ambiguous
 * rename. The temporary name identifies this request's created file and may
 * already be absent after rename, including an ambiguously failed rename.
 * image_digest/image_size are available only when published, otherwise zero.
 * They identify exact frame bytes; publication alone does not bind them to
 * canonical state. */
struct snag_binary_publication_result {
    struct snag_binary_anchor boundary;
    uint64_t generation, image_size;
    unsigned char image_digest[32];
    unsigned int slot;
    int error;
    bool renamed;
    bool published;
    char temporary[SNAG_BINARY_CP_TEMP_SIZE];
};

/* Internal cooperative publisher driven only by the session I/O owner. New
 * moves snapshot ownership on success; failure leaves it untouched. Step performs
 * one framing/write quantum or publication stage (1 more, 0 published, -1 error).
 * A failed step retains its phase and exact bytes for an explicitly paced retry.
 * Close preserves any provisional file and closes only its own descriptor.
 * Free releases memory after close or successful publication, with no file I/O. */
struct snag_binary_publication;
struct snag_binary_io_ops;
struct snag_binary_publication *snag_binary_publication_new(int journal, int directory,
    const uint64_t generations[2], struct snag_binary_io_snapshot *snapshot);
int snag_binary_publication_step(struct snag_binary_publication *,
    const struct snag_binary_io_ops *);
void snag_binary_publication_result(const struct snag_binary_publication *,
    struct snag_binary_publication_result *);
void snag_binary_publication_close(struct snag_binary_publication *);
void snag_binary_publication_free(struct snag_binary_publication *);

#endif
