/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_INSTRUCTIONS_H
#define SNAJPAGENT_INSTRUCTIONS_H

#include "base.h"

#include "snj_jansson.h"
#include <stddef.h>

#define SNJ_MAX_INSTRUCTION_SOURCES 16u
#define SNJ_MAX_INSTRUCTION_FILE (32u * 1024u)
#define SNJ_MAX_INSTRUCTION_BYTES (128u * 1024u)

struct snj_instruction_source {
    char *path;
    char sha256[SNJ_SHA256_HEX_LEN + 1u];
    size_t bytes;
    char *text;
};

struct snj_instruction_set {
    struct snj_instruction_source sources[SNJ_MAX_INSTRUCTION_SOURCES];
    size_t count;
    size_t bytes;
};

void snj_instructions_init(struct snj_instruction_set *set);
void snj_instructions_free(struct snj_instruction_set *set);
int snj_instructions_discover(struct snj_instruction_set *set,
                              const char *workspace,
                              char *error, size_t error_size);
json_t *snj_instructions_metadata_json(const struct snj_instruction_set *set);
int snj_instructions_metadata_valid(const json_t *array,
                                    char *error, size_t error_size);
int snj_instructions_match_metadata(const struct snj_instruction_set *set,
                                    const json_t *array,
                                    char *error, size_t error_size);

#endif
