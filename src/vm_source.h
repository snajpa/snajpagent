/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_SOURCE_H
#define SNAJPAGENT_VM_SOURCE_H

#include "json.h"
#include "wire.h"

/* Nonlinear runs: display start, source start, display/source bytes per unit,
 * unit count. Plain bytes between runs map directly. Equal adjacent units
 * coalesce, including long runs of escaped binary or repeated redactions. */
const char *snag_vm_block_text(const json_t *, bool heading);
bool snag_vm_source_mapped(const json_t *block, uint64_t display_byte);

int snag_vm_source_replace(json_t *runs, uint64_t display, uint64_t source,
    uint64_t display_bytes, uint64_t source_bytes);
uint64_t snag_vm_source_unformatted(const json_t *block, uint64_t byte);
uint64_t snag_vm_source_position(const json_t *block, uint64_t byte, bool display_to_source);
json_t *snag_vm_source_redactions(const char *source, const char *display,
    const struct snag_wire_secrets *);

#endif
