/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_JSON_H
#define SNAJPAGENT_JSON_H

#include "base.h"
#include "snj_jansson.h"

int snj_json_canonical(const json_t *value, struct snj_buf *out);
json_t *snj_json_load_strict(const unsigned char *data, size_t len,
                             size_t max_len, char *error, size_t error_size);
json_t *snj_json_load_canonical(const unsigned char *data, size_t len,
                                char *error, size_t error_size);
int snj_json_digest(const json_t *value,
                    char out[SNJ_SHA256_HEX_LEN + 1u]);
bool snj_json_exact_keys(const json_t *object, const char *const *keys,
                         size_t count);
const char *snj_json_string(const json_t *object, const char *key);
int snj_json_set_new(json_t *object, const char *key, json_t *value);
int snj_json_integer_u64(const json_t *object, const char *key, uint64_t *out);

#endif
