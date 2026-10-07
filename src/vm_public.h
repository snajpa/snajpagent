/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_PUBLIC_H
#define SNAJPAGENT_VM_PUBLIC_H

#include "json.h"
#include "wire.h"

/* Project chronological public history events into response text blocks.
 * Identity is response ID plus public-item ordinal: final graph local IDs may
 * differ from streaming local IDs, and tool calls do not consume ordinals.
 * Final/terminal snapshots replace streamed text, preserving block identity.
 * Native reader annotations provide canonical snapshots and prior stream ends
 * so a loaded page emits only its own part of the completed body.
 * Input comes from the public history reader. Source byte metadata preserves
 * offsets through redaction; joined chunks receive a second redaction pass. */
json_t *snag_vm_public_blocks(
    const json_t *events, const struct snag_wire_secrets *, char *, size_t);
/* Terminal response state, or NULL for other event types. */
const char *snag_vm_public_state(const char *type);
/* Add source byte counts before the reader drops its unredacted event data. */
int snag_vm_public_source_bytes(json_t *event, const json_t *source_data);

#endif
