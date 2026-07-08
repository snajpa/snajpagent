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
snj_size_add(size_t a, size_t b, size_t *out)
{
    if (b > SIZE_MAX - a)
        return false;
    *out = a + b;
    return true;
}

void
snj_buf_init(struct snj_buf *buf, size_t max)
{
    memset(buf, 0, sizeof(*buf));
    buf->max = max;
}

void
snj_buf_reset(struct snj_buf *buf)
{
    buf->len = 0;
}

void
snj_buf_free(struct snj_buf *buf)
{
    free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

int
snj_buf_reserve(struct snj_buf *buf, size_t extra)
{
    size_t need;
    size_t cap;
    unsigned char *next;

    if (!snj_size_add(buf->len, extra, &need) || need > buf->max) {
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
snj_buf_append(struct snj_buf *buf, const void *data, size_t len)
{
    if ((len && !data) || snj_buf_reserve(buf, len) < 0)
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
snj_buf_putc(struct snj_buf *buf, unsigned char c)
{
    return snj_buf_append(buf, &c, 1u);
}

int
snj_buf_printf(struct snj_buf *buf, const char *fmt, ...)
{
    va_list ap;
    va_list copy;
    int n;

    va_start(ap, fmt);
    va_copy(copy, ap);
    n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0 || (size_t)n > buf->max - buf->len) {
        va_end(ap);
        errno = EOVERFLOW;
        return -1;
    }
    if (snj_buf_reserve(buf, (size_t)n + 1u) < 0) {
        va_end(ap);
        return -1;
    }
    if (vsnprintf((char *)buf->data + buf->len, (size_t)n + 1u, fmt, ap) != n) {
        va_end(ap);
        errno = EIO;
        return -1;
    }
    va_end(ap);
    buf->len += (size_t)n;
    return 0;
}

int
snj_buf_terminate(struct snj_buf *buf)
{
    if (snj_buf_reserve(buf, 1u) < 0)
        return -1;
    buf->data[buf->len] = '\0';
    return 0;
}

bool
snj_utf8_valid(const unsigned char *s, size_t len, bool reject_nul)
{
    size_t i = 0;

    while (i < len) {
        uint32_t cp;
        unsigned char c = s[i++];
        unsigned int need;

        if (c < 0x80u) {
            if (reject_nul && c == 0)
                return false;
            continue;
        }
        if (c >= 0xc2u && c <= 0xdfu) {
            cp = (uint32_t)(c & 0x1fu);
            need = 1;
        } else if (c >= 0xe0u && c <= 0xefu) {
            cp = (uint32_t)(c & 0x0fu);
            need = 2;
        } else if (c >= 0xf0u && c <= 0xf4u) {
            cp = (uint32_t)(c & 0x07u);
            need = 3;
        } else {
            return false;
        }
        if (len - i < need)
            return false;
        for (unsigned int j = 0; j < need; ++j) {
            unsigned char d = s[i++];
            if ((d & 0xc0u) != 0x80u)
                return false;
            cp = (cp << 6) | (uint32_t)(d & 0x3fu);
        }
        if ((need == 1 && cp < 0x80u) ||
            (need == 2 && cp < 0x800u) ||
            (need == 3 && cp < 0x10000u) ||
            cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu))
            return false;
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
snj_random_id(char out[SNJ_ID_HEX_LEN + 1u])
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
snj_time_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
        return 0;
    if ((uint64_t)ts.tv_sec > UINT64_MAX / 1000u)
        return UINT64_MAX;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int
snj_write_full(int fd, const void *data, size_t len)
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
snj_sync_file(int fd)
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
snj_sync_dir(int fd)
{
    if (fsync(fd) == 0)
        return 0;
    if (errno == EINVAL || errno == ENOTSUP || errno == EROFS)
        return 1;
    return -1;
}

char *
snj_strdup_checked(const char *s, size_t max)
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
snj_join_words(char *const *words, size_t count, size_t max)
{
    struct snj_buf buf;

    snj_buf_init(&buf, max + 1u);
    for (size_t i = 0; i < count; ++i) {
        size_t len = strlen(words[i]);
        if (!snj_utf8_valid((const unsigned char *)words[i], len, true) ||
            (i && snj_buf_putc(&buf, ' ') < 0) ||
            snj_buf_append(&buf, words[i], len) < 0) {
            snj_buf_free(&buf);
            errno = EINVAL;
            return NULL;
        }
    }
    if (snj_buf_terminate(&buf) < 0) {
        snj_buf_free(&buf);
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
sha256_block(struct snj_sha256 *ctx, const unsigned char block[64])
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
snj_sha256_init(struct snj_sha256 *ctx)
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
snj_sha256_update(struct snj_sha256 *ctx, const void *data, size_t len)
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
snj_sha256_final(struct snj_sha256 *ctx, unsigned char out[32])
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
snj_sha256_hex(const void *data, size_t len, char out[SNJ_SHA256_HEX_LEN + 1u])
{
    struct snj_sha256 ctx;
    unsigned char digest[32];

    snj_sha256_init(&ctx);
    snj_sha256_update(&ctx, data, len);
    snj_sha256_final(&ctx, digest);
    hex_encode(digest, sizeof(digest), out);
}

bool
snj_hex_is_lower(const char *s, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return s[len] == '\0';
}
