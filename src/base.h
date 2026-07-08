/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_BASE_H
#define SNAJPAGENT_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNJ_ID_HEX_LEN 32u
#define SNJ_SHA256_HEX_LEN 64u
#define SNJ_PATH_MAX_BYTES (16u * 1024u)
#define SNJ_MAX_DIRECT_PROMPT (1024u * 1024u)
#define SNJ_MAX_EVENT_LINE (16u * 1024u * 1024u)
#define SNJ_MEMORY_LIMIT (192u * 1024u * 1024u)

struct snj_buf {
    unsigned char *data;
    size_t len;
    size_t cap;
    size_t max;
};

void snj_buf_init(struct snj_buf *buf, size_t max);
void snj_buf_reset(struct snj_buf *buf);
void snj_buf_free(struct snj_buf *buf);
int snj_buf_reserve(struct snj_buf *buf, size_t extra);
int snj_buf_append(struct snj_buf *buf, const void *data, size_t len);
int snj_buf_putc(struct snj_buf *buf, unsigned char c);
int snj_buf_printf(struct snj_buf *buf, const char *fmt, ...);
int snj_buf_terminate(struct snj_buf *buf);

bool snj_size_add(size_t a, size_t b, size_t *out);
bool snj_utf8_valid(const unsigned char *s, size_t len, bool reject_nul);
int snj_random_id(char out[SNJ_ID_HEX_LEN + 1u]);
uint64_t snj_time_ms(void);
int snj_write_full(int fd, const void *data, size_t len);
int snj_sync_file(int fd);
int snj_sync_dir(int fd);
char *snj_strdup_checked(const char *s, size_t max);
char *snj_join_words(char *const *words, size_t count, size_t max);

struct snj_sha256 {
    uint32_t state[8];
    uint64_t bit_count;
    unsigned char block[64];
    size_t block_len;
};

void snj_sha256_init(struct snj_sha256 *ctx);
void snj_sha256_update(struct snj_sha256 *ctx, const void *data, size_t len);
void snj_sha256_final(struct snj_sha256 *ctx, unsigned char out[32]);
void snj_sha256_hex(const void *data, size_t len,
                    char out[SNJ_SHA256_HEX_LEN + 1u]);
bool snj_hex_is_lower(const char *s, size_t len);

#endif
