/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_JSON_H
#define SNAJPAGENT_JSON_H

#include "base.h"
#include "snag_jansson.h"

int snag_json_canonical(const json_t *value, struct snag_buf *out);
json_t *snag_json_load_strict(const unsigned char *data, size_t len,
                             size_t max_len, char *error, size_t error_size);
json_t *snag_json_load_canonical(const unsigned char *data, size_t len,
                                char *error, size_t error_size);
int snag_json_digest(const json_t *value,
                    char out[SNAG_SHA256_HEX_LEN + 1u]);
int snag_json_digest_bounded(const json_t *value, size_t max,
                            char out[SNAG_SHA256_HEX_LEN + 1u],
                            size_t *bytes);
bool snag_json_exact_keys(const json_t *object, const char *const *keys,
                         size_t count);
const char *snag_json_string(const json_t *object, const char *key);
int snag_json_set_new(json_t *object, const char *key, json_t *value);
int snag_json_integer_u64(const json_t *object, const char *key, uint64_t *out);

#endif
