/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_RECORD_H
#define SNAJPAGENT_STORE_RECORD_H

#include "json.h"

#include <sys/types.h>

typedef ssize_t (*snag_record_read_fn)(void *, void *, size_t, int64_t);

/* Consume one canonical checkpoint without materializing its state/context.
 * The returned envelope has only display metadata in data. digest is computed
 * over the original payload with event_sha256 omitted, for normal verification.
 * end includes the terminating newline; reads never extend beyond that range. */
json_t *snag_store_checkpoint_metadata(snag_record_read_fn, void *, int64_t start,
    int64_t end, char digest[SNAG_SHA256_HEX_LEN + 1u]);

#endif
