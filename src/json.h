/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_JSON_H
#define SNAJPAGENT_JSON_H

#include "base.h"
#include "snag_jansson.h"

/* Owned canonical document. Refresh its measurement after any mutation. */
struct snag_json_document {
    json_t *value;
    size_t bytes;
    char sha256[SNAG_SHA256_HEX_LEN + 1u];
};

void snag_json_document_free(struct snag_json_document *document);
/* Consumes value; failure leaves an empty document. Initialize with {0}. */
int snag_json_document_set(struct snag_json_document *document, json_t *value, size_t max);
int snag_json_document_measure(struct snag_json_document *document, size_t max);

int snag_json_canonical(const json_t *value, struct snag_buf *out);
/* Diagnostic JSON accepts real numbers; durable canonical JSON does not. */
int snag_json_diagnostic(const json_t *value, struct snag_buf *out);
json_t *snag_json_load_strict(const unsigned char *data, size_t len,
                             size_t max_len, char *error, size_t error_size);
json_t *snag_json_load_canonical(const unsigned char *data, size_t len,
                                char *error, size_t error_size);
int snag_json_digest(const json_t *value,
                    char out[SNAG_SHA256_HEX_LEN + 1u]);
int snag_json_digest_bounded(const json_t *value, size_t max,
                            char out[SNAG_SHA256_HEX_LEN + 1u],
                            size_t *bytes);
/* Fixed field names separated by single spaces; empty means an empty object. */
bool snag_json_exact_keys(const json_t *object, const char *keys);
const char *snag_json_string(const json_t *object, const char *key);
int snag_json_set_new(json_t *object, const char *key, json_t *value);
int snag_json_integer_u64(const json_t *object, const char *key, uint64_t *out);

#endif
