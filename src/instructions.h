/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_INSTRUCTIONS_H
#define SNAJPAGENT_INSTRUCTIONS_H

#include "base.h"

#include "snag_jansson.h"
#include <stddef.h>

/* The single local work note: resolved per request, separate from the instruction sources. */
#define SNAG_WORKNOTE_NAME "WORKNOTE.md"
#define SNAG_WORKNOTE_MAX_BYTES (64u * 1024u)
struct snag_instruction_set {
    char **paths;
    size_t count, capacity;
};

void snag_instructions_free(struct snag_instruction_set *set);
int snag_instructions_add_directory(struct snag_instruction_set *set, const char *dir,
                                   char *error, size_t error_size);
int snag_instructions_add_file(struct snag_instruction_set *set, const char *path,
                              char *error, size_t error_size);
/* Takes ownership of an already canonical path; grows the set as needed. */
int snag_instructions_add_owned(struct snag_instruction_set *set, char *path,
                               char *error, size_t error_size);
int snag_instructions_discover(struct snag_instruction_set *set, const char *workspace,
                              char *error, size_t error_size);
int snag_instructions_worknote(const char *workspace, char **note,
                              char *error, size_t error_size);
json_t *snag_instructions_metadata_json(const struct snag_instruction_set *set);
int snag_instructions_metadata_valid(const json_t *array, char *error, size_t error_size);
int snag_instructions_match_metadata(const struct snag_instruction_set *set, const json_t *array,
                                    char *error, size_t error_size);

#endif
