/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_BASE_H
#define SNAJPAGENT_BASE_H

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifndef EOVERFLOW
#define EOVERFLOW ERANGE /* Old BSD reports range overflow without a distinct errno. */
#endif

#ifndef ENOTSUP
#define ENOTSUP EOPNOTSUPP
#endif

#ifndef EPROTO
#define EPROTO EIO /* Old BSD has no distinct protocol-error number. */
#endif

#define SNAG_VERBOSITY_MAX 6u

bool snag_verbosity_command(const char *text, size_t len);
/* Decimal count; values beyond uint64_t saturate to cover any stored history. */
int snag_parse_count(const char *text, uint64_t *count);

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
int snag_command_argument(struct snag_buf *command, const char *argument);
int snag_command_finish(struct snag_buf *command);
const char *snag_command_shell_note(void);
/* Append through EOF, without closing or terminating: -1 read, -2 buffer error. */
int snag_buf_read(struct snag_buf *buf, int fd);

int snag_errorf(char *error, size_t size, const char *fmt, ...);
int snag_fail(char *error, size_t size, int code, const char *fmt, ...);

/* Return a system-style failure with the specified errno. */
static inline int
snag_errno(int code)
{
    errno = code;
    return -1;
}

bool snag_size_add(size_t a, size_t b, size_t *out);
size_t snag_utf8_size(unsigned char first);
int snag_key_ref_compare(const void *left, const void *right);
int snag_fd_cloexec(int fd);
int snag_isatty(int fd);
char *snag_default_shell(void);
char *snag_program_path(const char *program);
int snag_file_executable(const char *path);
int snag_editor_run(const char *path, bool *success, void (*service)(void *), void *opaque);
int snag_hostname(char *out, size_t size);
/* Owned UTF-8 copies; absent environment variables return NULL/ENOENT. */
char *snag_environment(const char *name);
char *snag_home_directory(void);
char **snag_environment_entries(void);
void snag_environment_entries_free(char **entries);
bool snag_environment_prefix(const char *entry, const char *prefix);
#ifdef _WIN32
/* Explicit UTF-8/UTF-16 conversion; caller owns the result. */
wchar_t *snag_utf8_to_wide(const char *text);
char *snag_wide_to_utf8(const wchar_t *text);
/* Convert exactly count UTF-16 units; NULL output queries the byte count. */
ssize_t snag_utf16_to_utf8(const wchar_t *text, size_t count, char *out, size_t capacity);
char **snag_wide_arguments(int argc, wchar_t **wide);
void snag_arguments_free(char **argv);
#endif
size_t snag_utf8_decode(const unsigned char *text, size_t len, uint32_t *out);
bool snag_utf8_valid(const unsigned char *s, size_t len, bool reject_nul);
/* NUL-terminated UTF-8, with inclusive byte-length bounds; NULL is invalid. */
bool snag_text_valid(const char *text, size_t min, size_t max);
int snag_char_width(uint32_t cp);
bool snag_text_blank(const char *text);
unsigned char snag_irc_fold(unsigned char c);
bool snag_irc_nick_char(unsigned char c);
bool snag_irc_nick_mentioned(const char *text, const char *nick);
enum snag_irc_target_command {
    SNAG_IRC_TARGET_INVALID = -1, SNAG_IRC_TARGET_NONE,
    SNAG_IRC_TARGET_SELECT, SNAG_IRC_TARGET_SEND, SNAG_IRC_TARGET_ALL };
enum snag_irc_target_command snag_irc_target_parse(const char *text, size_t len, uint32_t *id, size_t *body);
int snag_random_id(char out[SNAG_ID_HEX_LEN + 1u]);
int snag_random_bytes(unsigned char *out, size_t len);
uint64_t snag_time_ms(void);
uint64_t snag_monotonic_ms(void);
int snag_sleep_ms(unsigned int milliseconds);
void snag_ignore_sigpipe(void);
bool snag_text_locale_init(void);
struct tm *snag_localtime(const time_t *seconds, struct tm *out);
struct tm *snag_gmtime(const time_t *seconds, struct tm *out);
int snag_write_full(int fd, const void *data, size_t len);
int snag_sync_file(int fd);
int snag_sync_dir(int fd);
/* Full data/metadata synchronization; unlike sync_dir, unsupported is failure. */
int snag_fsync(int fd);
struct snag_file_privacy {
    bool real_owner, effective_owner, private_access;
};
int snag_fd_privacy(int fd, struct snag_file_privacy *out);
char *snag_strdup_checked(const char *s, size_t max);
char *snag_path_join(const char *left, const char *right);
size_t snag_path_root_len(const char *path);
char *snag_realpath(const char *path);
int snag_mkdir_private(const char *path);
int snag_mkdir_private_at(int dirfd, const char *path);
int snag_create_private_at(int dirfd, const char *path, bool exclusive);
bool snag_strcpy(char *dst, size_t size, const char *src);
/* Exact membership in a space-separated list of nonempty words. */
bool snag_string_in(const char *value, const char *choices);
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
void snag_sha256_hex(const void *data, size_t len, char out[SNAG_SHA256_HEX_LEN + 1u]);
bool snag_hex_is_lower(const char *s, size_t len);
int snag_base64_append(struct snag_buf *out, const unsigned char *data, size_t len);
int snag_base64_decode(struct snag_buf *out, const char *text);

#endif
