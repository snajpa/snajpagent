/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_UPLOAD_MD5_H
#define SNAJPAGENT_UPLOAD_MD5_H

#include <stddef.h>
#include <stdint.h>

/* Legacy trzsz wire integrity only; retained assets still use SHA-256. */
struct snag_upload_md5 {
    uint32_t state[4];
    uint64_t bytes;
    unsigned char block[64];
    size_t used;
};

void snag_upload_md5_init(struct snag_upload_md5 *hash);
void snag_upload_md5_update(struct snag_upload_md5 *hash, const void *data, size_t length);
void snag_upload_md5_finish(struct snag_upload_md5 *hash, unsigned char digest[16]);

#endif
