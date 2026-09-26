/* SPDX-License-Identifier: GPL-2.0-only */
#include "upload_md5.h"

#include <string.h>

static const uint32_t constants[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

static uint32_t
load32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void
store32(unsigned char *bytes, uint32_t value)
{
    for (unsigned int i = 0; i < 4u; ++i) bytes[i] = (unsigned char)(value >> (8u * i));
}

static void
compress_block(struct snag_upload_md5 *hash)
{
    static const unsigned char rotations[4][4] = {
        {7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}
    };
    uint32_t words[16];
    uint32_t a = hash->state[0];
    uint32_t b = hash->state[1];
    uint32_t c = hash->state[2];
    uint32_t d = hash->state[3];

    for (unsigned int i = 0; i < 16u; ++i) words[i] = load32(hash->block + 4u * i);
    for (unsigned int i = 0; i < 64u; ++i) {
        unsigned int round = i / 16u;
        unsigned int index;
        uint32_t value;

        switch (round) {
        case 0: value = (b & c) | (~b & d); index = i; break;
        case 1: value = (d & b) | (~d & c); index = (5u * i + 1u) % 16u; break;
        case 2: value = b ^ c ^ d; index = (3u * i + 5u) % 16u; break;
        default: value = c ^ (b | ~d); index = (7u * i) % 16u; break;
        }
        value += a + constants[i] + words[index];
        unsigned int shift = rotations[round][i % 4u];
        uint32_t rotated = (value << shift) | (value >> (32u - shift));
        a = d;
        d = c;
        c = b;
        b += rotated;
    }
    hash->state[0] += a;
    hash->state[1] += b;
    hash->state[2] += c;
    hash->state[3] += d;
}

void
snag_upload_md5_init(struct snag_upload_md5 *hash)
{
    *hash = (struct snag_upload_md5){
        .state = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u}
    };
}

void
snag_upload_md5_update(struct snag_upload_md5 *hash, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    hash->bytes += length;

    while (length) {
        size_t amount = sizeof(hash->block) - hash->used;
        if (amount > length) amount = length;
        memcpy(hash->block + hash->used, bytes, amount);
        hash->used += amount;
        bytes += amount;
        length -= amount;
        if (hash->used == sizeof(hash->block)) {
            compress_block(hash);
            hash->used = 0u;
        }
    }
}

void
snag_upload_md5_finish(struct snag_upload_md5 *hash, unsigned char digest[16])
{
    uint64_t bits = hash->bytes * 8u;
    unsigned char tail[64] = {0x80};
    size_t padding = hash->used < 56u ? 56u - hash->used : 120u - hash->used;
    unsigned char length[8];

    for (unsigned int i = 0; i < sizeof(length); ++i) {
        length[i] = (unsigned char)(bits >> (8u * i));
    }
    snag_upload_md5_update(hash, tail, padding);
    snag_upload_md5_update(hash, length, sizeof(length));
    for (unsigned int i = 0; i < 4u; ++i) store32(digest + i * 4u, hash->state[i]);
}
