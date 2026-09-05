/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_INSTRUCTIONS_H
#define SNAJPAGENT_INSTRUCTIONS_H

#include "base.h"

#include "snag_jansson.h"
#include <stddef.h>

#define SNAG_MAX_INSTRUCTION_SOURCES 16u
#define SNAG_MAX_INSTRUCTION_FILE (32u * 1024u)
#define SNAG_MAX_INSTRUCTION_BYTES (128u * 1024u)

struct snag_instruction_source {
    char *path;
    char sha256[SNAG_SHA256_HEX_LEN + 1u];
    size_t bytes;
    char *text;
};

struct snag_instruction_set {
    struct snag_instruction_source sources[SNAG_MAX_INSTRUCTION_SOURCES];
    size_t count;
    size_t bytes;
};

void snag_instructions_init(struct snag_instruction_set *set);
void snag_instructions_free(struct snag_instruction_set *set);
int snag_instructions_discover(struct snag_instruction_set *set,
                              const char *workspace,
                              char *error, size_t error_size);
json_t *snag_instructions_metadata_json(const struct snag_instruction_set *set);
int snag_instructions_metadata_valid(const json_t *array,
                                    char *error, size_t error_size);
int snag_instructions_match_metadata(const struct snag_instruction_set *set,
                                    const json_t *array,
                                    char *error, size_t error_size);

#endif
