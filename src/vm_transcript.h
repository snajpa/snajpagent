/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_TRANSCRIPT_H
#define SNAJPAGENT_VM_TRANSCRIPT_H

#include "json.h"
#include "wire.h"

/* Chronological validated journal events, held privately by the reader. The
 * result owns display blocks with key, seq, last_seq, kind, label, text and
 * source byte ranges. No raw provider continuation or encoded process payload
 * is returned. Model/tool text stays data; the grid renders controls inert.
 * Reproject the source page to change verbosity. Missing earlier call metadata
 * is identified by needs_call, for a dependency read without reducer replay. */
json_t *snag_vm_transcript_blocks(const json_t *events, unsigned int verbosity,
    unsigned int columns, const struct snag_wire_secrets *, bool (*cancel)(void *),
    void *cancel_opaque, char *, size_t);

#endif
