/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

bool
snag_verbosity_command(const char *text, size_t len)
{
    return text && len >= 8u && memcmp(text, "/verbose", 8u) == 0 &&
           (len == 8u || text[8] == ' ' || text[8] == '\t' || text[8] == '\n');
}

unsigned char
snag_irc_fold(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return (unsigned char)(c + ('a' - 'A'));
    if (c == '[') return '{';
    if (c == ']') return '}';
    if (c == '\\') return '|';
    if (c == '^') return '~';
    return c;
}

bool
snag_irc_nick_char(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '[' || c == ']' || c == '\\' ||
           c == '`' || c == '_' || c == '^' || c == '{' || c == '}' ||
           c == '|' || c == '-';
}

enum snag_irc_target_command
snag_irc_target_parse(const char *text, size_t len, uint32_t *id, size_t *body)
{
    size_t i = 1u;
    bool all;

    *id = 0u;
    *body = 0u;
    if (!text || len < 2u || text[0] != '/')
        return SNAG_IRC_TARGET_NONE;
    all = len >= 4u && memcmp(text, "/all", 4u) == 0 &&
          (len == 4u || text[4] == ' ' || text[4] == '\t' || text[4] == '\n');
    if (all) {
        i = 4u;
    } else {
        if (text[i] < '0' || text[i] > '9')
            return SNAG_IRC_TARGET_NONE;
        while (i < len && text[i] >= '0' && text[i] <= '9') {
            unsigned int digit = (unsigned int)(text[i++] - '0');
            if (*id > (UINT32_MAX - digit) / 10u)
                return SNAG_IRC_TARGET_INVALID;
            *id = *id * 10u + digit;
        }
        if (!*id || (i < len && text[i] != ' ' && text[i] != '\t' && text[i] != '\n'))
            return SNAG_IRC_TARGET_INVALID;
    }
    while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n'))
        ++i;
    *body = i;
    if (all)
        return i < len ? SNAG_IRC_TARGET_ALL : SNAG_IRC_TARGET_INVALID;
    return i < len ? SNAG_IRC_TARGET_SEND : SNAG_IRC_TARGET_SELECT;
}

char *
snag_path_join(const char *left, const char *right)
{
    size_t need;
    char *path;

    if (!snag_size_add(strlen(left), strlen(right), &need) ||
        !snag_size_add(need, 2u, &need) || need > SNAG_PATH_MAX_BYTES + 1u) {
        errno = EOVERFLOW;
        return NULL;
    }
    path = malloc(need);
    if (path)
        (void)snprintf(path, need, "%s%s%s", left,
                       strcmp(left, "/") == 0 ? "" : "/", right);
    return path;
}

void
snag_errorf(char *error, size_t size, const char *fmt, ...)
{
    va_list ap;

    if (!size)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(error, size, fmt, ap);
    va_end(ap);
}

size_t
snag_utf8_size(unsigned char first)
{
    if (first < 0x80u)
        return 1u;
    if (first >= 0xc2u && first <= 0xdfu)
        return 2u;
    if (first >= 0xe0u && first <= 0xefu)
        return 3u;
    return first >= 0xf0u && first <= 0xf4u ? 4u : 0u;
}

int
snag_key_ref_compare(const void *left, const void *right)
{
    const struct snag_key_ref *a = left;
    const struct snag_key_ref *b = right;
    size_t n = a->len < b->len ? a->len : b->len;
    int cmp = memcmp(a->name, b->name, n);

    return cmp ? cmp : (a->len > b->len) - (a->len < b->len);
}

int
snag_fd_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);

    return flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0 ? -1 : 0;
}

bool
snag_size_add(size_t a, size_t b, size_t *out)
{
    if (b > SIZE_MAX - a)
        return false;
    *out = a + b;
    return true;
}

void
snag_buf_init(struct snag_buf *buf, size_t max)
{
    memset(buf, 0, sizeof(*buf));
    buf->max = max;
}

void
snag_buf_reset(struct snag_buf *buf)
{
    buf->len = 0;
}

void
snag_buf_free(struct snag_buf *buf)
{
    free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

int
snag_buf_reserve(struct snag_buf *buf, size_t extra)
{
    size_t need;
    size_t cap;
    unsigned char *next;

    if (!snag_size_add(buf->len, extra, &need) || need > buf->max) {
        errno = EOVERFLOW;
        return -1;
    }
    if (need <= buf->cap)
        return 0;
    cap = buf->cap ? buf->cap : 256u;
    while (cap < need) {
        if (cap > buf->max / 2u) {
            cap = buf->max;
            break;
        }
        cap *= 2u;
    }
    if (cap < need) {
        errno = EOVERFLOW;
        return -1;
    }
    next = realloc(buf->data, cap);
    if (!next)
        return -1;
    buf->data = next;
    buf->cap = cap;
    return 0;
}

int
snag_buf_append(struct snag_buf *buf, const void *data, size_t len)
{
    if ((len && !data) || snag_buf_reserve(buf, len) < 0)
        return -1;
    if (len) {
        if (!buf->data) {
            errno = EFAULT;
            return -1;
        }
        memcpy(buf->data + buf->len, data, len);
    }
    buf->len += len;
    return 0;
}

int
snag_buf_putc(struct snag_buf *buf, unsigned char c)
{
    return snag_buf_append(buf, &c, 1u);
}

int
snag_buf_vprintf(struct snag_buf *buf, const char *fmt, va_list ap)
{
    va_list copy;
    int n;

    va_copy(copy, ap);
    n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0 || (size_t)n > buf->max - buf->len) {
        errno = EOVERFLOW;
        return -1;
    }
    if (snag_buf_reserve(buf, (size_t)n + 1u) < 0)
        return -1;
    if (vsnprintf((char *)buf->data + buf->len, (size_t)n + 1u, fmt, ap) != n) {
        errno = EIO;
        return -1;
    }
    buf->len += (size_t)n;
    return 0;
}

int
snag_buf_printf(struct snag_buf *buf, const char *fmt, ...)
{
    va_list ap;
    int rc;

    va_start(ap, fmt);
    rc = snag_buf_vprintf(buf, fmt, ap);
    va_end(ap);
    return rc;
}

int
snag_buf_terminate(struct snag_buf *buf)
{
    if (snag_buf_reserve(buf, 1u) < 0)
        return -1;
    buf->data[buf->len] = '\0';
    return 0;
}

bool
snag_text_blank(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            return false;
    return true;
}

/* Return the first scalar's byte length, or zero for invalid/incomplete UTF-8. */
size_t
snag_utf8_decode(const unsigned char *text, size_t len, uint32_t *out)
{
    size_t size = len ? snag_utf8_size(text[0]) : 0u;
    uint32_t cp;

    if (!size || size > len)
        return 0u;
    cp = text[0] & (size == 1u ? 0x7fu : size == 2u ? 0x1fu :
                   size == 3u ? 0x0fu : 0x07u);
    for (size_t i = 1u; i < size; ++i) {
        if ((text[i] & 0xc0u) != 0x80u)
            return 0u;
        cp = (cp << 6) | (text[i] & 0x3fu);
    }
    if ((size == 2u && cp < 0x80u) || (size == 3u && cp < 0x800u) ||
        (size == 4u && cp < 0x10000u) || cp > 0x10ffffu ||
        (cp >= 0xd800u && cp <= 0xdfffu))
        return 0u;
    *out = cp;
    return size;
}

bool
snag_utf8_valid(const unsigned char *s, size_t len, bool reject_nul)
{
    for (size_t i = 0u; i < len;) {
        uint32_t cp;
        size_t size = snag_utf8_decode(s + i, len - i, &cp);
        if (!size || (reject_nul && cp == 0u))
            return false;
        i += size;
    }
    return true;
}

static void
hex_encode(const unsigned char *in, size_t len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[i * 2u] = hex[in[i] >> 4];
        out[i * 2u + 1u] = hex[in[i] & 15u];
    }
    out[len * 2u] = '\0';
}

int
snag_random_id(char out[SNAG_ID_HEX_LEN + 1u])
{
    unsigned char raw[16];
    size_t done = 0;
    int fd;

    fd = open("/dev/urandom", O_RDONLY
#ifdef O_CLOEXEC
        | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
        | O_NOFOLLOW
#endif
    );
    if (fd < 0)
        return -1;
    while (done < sizeof(raw)) {
        ssize_t n = read(fd, raw + done, sizeof(raw) - done);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        (void)close(fd);
        errno = n == 0 ? EIO : errno;
        return -1;
    }
    if (close(fd) < 0)
        return -1;
    hex_encode(raw, sizeof(raw), out);
    return 0;
}

uint64_t
snag_time_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
        return 0;
    if ((uint64_t)ts.tv_sec > UINT64_MAX / 1000u)
        return UINT64_MAX;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t
snag_monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0u;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

int
snag_write_full(int fd, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0)
            errno = EIO;
        return -1;
    }
    return 0;
}

int
snag_sync_file(int fd)
{
#if defined(__APPLE__)
    return fsync(fd);
#else
    if (fdatasync(fd) == 0)
        return 0;
    if (errno != EINVAL && errno != ENOSYS)
        return -1;
    return fsync(fd);
#endif
}

int
snag_sync_dir(int fd)
{
    if (fsync(fd) == 0)
        return 0;
    if (errno == EINVAL || errno == ENOTSUP || errno == EROFS)
        return 1;
    return -1;
}

bool
snag_strcpy(char *dst, size_t size, const char *src)
{
    size_t len;

    if (!src || (len = strlen(src)) >= size)
        return false;
    memcpy(dst, src, len + 1u);
    return true;
}

char *
snag_strdup_checked(const char *s, size_t max)
{
    size_t len = strlen(s);
    char *copy;

    if (len > max) {
        errno = EOVERFLOW;
        return NULL;
    }
    copy = malloc(len + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, s, len + 1u);
    return copy;
}

char *
snag_join_words(char *const *words, size_t count, size_t max)
{
    struct snag_buf buf;

    snag_buf_init(&buf, max + 1u);
    for (size_t i = 0; i < count; ++i) {
        size_t len = strlen(words[i]);
        if (!snag_utf8_valid((const unsigned char *)words[i], len, true) ||
            (i && snag_buf_putc(&buf, ' ') < 0) ||
            snag_buf_append(&buf, words[i], len) < 0) {
            snag_buf_free(&buf);
            errno = EINVAL;
            return NULL;
        }
    }
    if (snag_buf_terminate(&buf) < 0) {
        snag_buf_free(&buf);
        return NULL;
    }
    return (char *)buf.data;
}

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32u - (n))))

static const uint32_t sha256_k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static void
sha256_block(struct snag_sha256 *ctx, const unsigned char block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;

    for (size_t i = 0; i < 16; ++i)
        w[i] = ((uint32_t)block[i * 4u] << 24) |
               ((uint32_t)block[i * 4u + 1u] << 16) |
               ((uint32_t)block[i * 4u + 2u] << 8) |
               (uint32_t)block[i * 4u + 3u];
    for (size_t i = 16; i < 64; ++i) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (size_t i = 0; i < 64; ++i) {
        uint32_t s1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + sha256_k[i] + w[i];
        uint32_t s0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void
snag_sha256_init(struct snag_sha256 *ctx)
{
    static const uint32_t init[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    memcpy(ctx->state, init, sizeof(init));
    ctx->bit_count = 0;
    ctx->block_len = 0;
}

void
snag_sha256_update(struct snag_sha256 *ctx, const void *data, size_t len)
{
    const unsigned char *p = data;

    while (len) {
        size_t take = 64u - ctx->block_len;
        if (take > len)
            take = len;
        memcpy(ctx->block + ctx->block_len, p, take);
        ctx->block_len += take;
        p += take;
        len -= take;
        ctx->bit_count += (uint64_t)take * 8u;
        if (ctx->block_len == 64u) {
            sha256_block(ctx, ctx->block);
            ctx->block_len = 0;
        }
    }
}

void
snag_sha256_final(struct snag_sha256 *ctx, unsigned char out[32])
{
    uint64_t bits = ctx->bit_count;
    ctx->block[ctx->block_len++] = 0x80u;
    if (ctx->block_len > 56u) {
        memset(ctx->block + ctx->block_len, 0, 64u - ctx->block_len);
        sha256_block(ctx, ctx->block);
        ctx->block_len = 0;
    }
    memset(ctx->block + ctx->block_len, 0, 56u - ctx->block_len);
    for (size_t i = 0; i < 8u; ++i)
        ctx->block[63u - i] = (unsigned char)(bits >> (i * 8u));
    sha256_block(ctx, ctx->block);
    for (size_t i = 0; i < 8u; ++i) {
        out[i * 4u] = (unsigned char)(ctx->state[i] >> 24);
        out[i * 4u + 1u] = (unsigned char)(ctx->state[i] >> 16);
        out[i * 4u + 2u] = (unsigned char)(ctx->state[i] >> 8);
        out[i * 4u + 3u] = (unsigned char)ctx->state[i];
    }
    memset(ctx, 0, sizeof(*ctx));
}

void
snag_sha256_hex(const void *data, size_t len, char out[SNAG_SHA256_HEX_LEN + 1u])
{
    struct snag_sha256 ctx;
    unsigned char digest[32];

    snag_sha256_init(&ctx);
    snag_sha256_update(&ctx, data, len);
    snag_sha256_final(&ctx, digest);
    hex_encode(digest, sizeof(digest), out);
}

bool
snag_hex_is_lower(const char *s, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return s[len] == '\0';
}

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int
snag_base64_append(struct snag_buf *out, const unsigned char *data, size_t len)
{
    size_t i = 0;
    while (i + 3u <= len) {
        unsigned int v = ((unsigned int)data[i] << 16) |
                         ((unsigned int)data[i + 1u] << 8) |
                         (unsigned int)data[i + 2u];
        unsigned char enc[4] = {
            (unsigned char)b64[(v >> 18) & 63u],
            (unsigned char)b64[(v >> 12) & 63u],
            (unsigned char)b64[(v >> 6) & 63u],
            (unsigned char)b64[v & 63u]
        };
        if (snag_buf_append(out, enc, sizeof(enc)) < 0)
            return -1;
        i += 3u;
    }
    if (i < len) {
        unsigned int v = (unsigned int)data[i] << 16;
        unsigned char enc[4];
        if (i + 1u < len)
            v |= (unsigned int)data[i + 1u] << 8;
        enc[0] = (unsigned char)b64[(v >> 18) & 63u];
        enc[1] = (unsigned char)b64[(v >> 12) & 63u];
        enc[2] = (i + 1u < len) ? (unsigned char)b64[(v >> 6) & 63u] : '=';
        enc[3] = '=';
        if (snag_buf_append(out, enc, sizeof(enc)) < 0)
            return -1;
    }
    return 0;
}

int
snag_base64_decode(struct snag_buf *out, const char *text)
{
    size_t len = strlen(text);
    if (len % 4u)
        return -1;
    for (size_t i = 0u; i < len; i += 4u) {
        unsigned int v = 0u, pad = 0u;
        for (size_t j = 0u; j < 4u; ++j) {
            const char *p = strchr(b64, text[i + j]);
            if (text[i + j] == '=') {
                if (j < 2u || i + 4u != len)
                    return -1;
                ++pad;
                v <<= 6;
            } else {
                if (!p || pad)
                    return -1;
                v = (v << 6) | (unsigned int)(p - b64);
            }
        }
        if ((pad == 1u && (v & 0xffu)) || (pad == 2u && (v & 0xffffu)))
            return -1;
        for (unsigned int j = 0u; j < 3u - pad; ++j)
            if (snag_buf_putc(out, (unsigned char)(v >> (16u - 8u * j))) < 0)
                return -1;
    }
    return 0;
}
