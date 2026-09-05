/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_SECRET_SOURCE_H
#define SNAJPAGENT_SECRET_SOURCE_H

#include <stddef.h>

#define SNAG_SECRET_MAX 16384u

enum snag_secret_kind {
    SNAG_SECRET_NONE,
    SNAG_SECRET_ENV,
    SNAG_SECRET_LITERAL,
    SNAG_SECRET_FILE
};

/* Owns its strings. Copies are borrowed unless explicitly parsed anew. */
struct snag_secret_source {
    enum snag_secret_kind kind;
    char *expression;
    char *value;
    char *path;
};

void snag_secret_bytes_free(char *value);
void snag_secret_clear(void *data, size_t len);
void snag_secret_source_free(struct snag_secret_source *source);
int snag_secret_source_parse(struct snag_secret_source *out, const char *expression,
                            const char *config_path, char *error, size_t error_size);
int snag_secret_source_resolve(const struct snag_secret_source *source, char **out,
                              char *error, size_t error_size);
const char *snag_secret_source_kind(const struct snag_secret_source *source);

#endif
