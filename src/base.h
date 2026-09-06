/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_BASE_H
#define SNAJPAGENT_BASE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNAG_VERBOSITY_MAX 6u

bool snag_verbosity_command(const char *text, size_t len);

#define SNAG_ID_HEX_LEN 32u
#define SNAG_SHA256_HEX_LEN 64u
#define SNAG_PATH_MAX_BYTES (16u * 1024u)
#define SNAG_MAX_DIRECT_PROMPT (1024u * 1024u)
#define SNAG_TERM_LABEL_BYTES 512u
#define SNAG_MAX_EVENT_LINE (16u * 1024u * 1024u)
#define SNAG_MEMORY_LIMIT (192u * 1024u * 1024u)

struct snag_buf {
    unsigned char *data;
    size_t len;
    size_t cap;
    size_t max;
};

struct snag_key_ref {
    const char *name;
    size_t len;
};

void snag_buf_init(struct snag_buf *buf, size_t max);
void snag_buf_reset(struct snag_buf *buf);
void snag_buf_free(struct snag_buf *buf);
int snag_buf_reserve(struct snag_buf *buf, size_t extra);
int snag_buf_append(struct snag_buf *buf, const void *data, size_t len);
int snag_buf_putc(struct snag_buf *buf, unsigned char c);
int snag_buf_vprintf(struct snag_buf *buf, const char *fmt, va_list ap);
int snag_buf_printf(struct snag_buf *buf, const char *fmt, ...);
int snag_buf_terminate(struct snag_buf *buf);

void snag_errorf(char *error, size_t size, const char *fmt, ...);

bool snag_size_add(size_t a, size_t b, size_t *out);
size_t snag_utf8_size(unsigned char first);
int snag_key_ref_compare(const void *left, const void *right);
int snag_fd_cloexec(int fd);
size_t snag_utf8_decode(const unsigned char *text, size_t len, uint32_t *out);
bool snag_utf8_valid(const unsigned char *s, size_t len, bool reject_nul);
bool snag_text_blank(const char *text);
unsigned char snag_irc_fold(unsigned char c);
bool snag_irc_nick_char(unsigned char c);
enum snag_irc_target_command {
    SNAG_IRC_TARGET_INVALID = -1, SNAG_IRC_TARGET_NONE,
    SNAG_IRC_TARGET_SELECT, SNAG_IRC_TARGET_SEND, SNAG_IRC_TARGET_ALL
};
enum snag_irc_target_command snag_irc_target_parse(const char *text, size_t len,
                                                uint32_t *id, size_t *body);
int snag_random_id(char out[SNAG_ID_HEX_LEN + 1u]);
uint64_t snag_time_ms(void);
uint64_t snag_monotonic_ms(void);
int snag_write_full(int fd, const void *data, size_t len);
int snag_sync_file(int fd);
int snag_sync_dir(int fd);
char *snag_strdup_checked(const char *s, size_t max);
char *snag_path_join(const char *left, const char *right);
bool snag_strcpy(char *dst, size_t size, const char *src);
char *snag_join_words(char *const *words, size_t count, size_t max);

struct snag_sha256 {
    uint32_t state[8];
    uint64_t bit_count;
    unsigned char block[64];
    size_t block_len;
};

void snag_sha256_init(struct snag_sha256 *ctx);
void snag_sha256_update(struct snag_sha256 *ctx, const void *data, size_t len);
void snag_sha256_final(struct snag_sha256 *ctx, unsigned char out[32]);
void snag_sha256_hex(const void *data, size_t len,
                    char out[SNAG_SHA256_HEX_LEN + 1u]);
bool snag_hex_is_lower(const char *s, size_t len);
int snag_base64_append(struct snag_buf *out, const unsigned char *data, size_t len);
int snag_base64_decode(struct snag_buf *out, const char *text);

#endif
