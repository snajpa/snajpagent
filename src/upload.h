/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_UPLOAD_H
#define SNAJPAGENT_UPLOAD_H

#include "base.h"
#include "fs.h"

#define SNAG_UPLOAD_FILES_MAX 8u

struct snag_upload_file {
    char name[SNAG_NAME_MAX_BYTES + 1u];
    char leaf[SNAG_ID_HEX_LEN + 1u];
    uint64_t bytes;
    int fd; /* Open only during the wire phase; -1 after verified close. */
};

struct snag_upload_result {
    struct snag_upload_file files[SNAG_UPLOAD_FILES_MAX];
    size_t count;
    unsigned char tail[4096];
    size_t tail_len;
};

/* The caller exclusively owns a private, nonblocking controlling-terminal fd
 * in raw mode and a private staging directory. Return 0 for verified EXIT,
 * 1 for sender cancellation, -1 for an error. Files belong to the caller until
 * snag_upload_cleanup removes precisely this operation's staging leaves. */
int snag_upload_receive(int tty, int stage_fd, size_t slots, bool directory,
                        int (*checkpoint)(void *), void *opaque,
                        struct snag_upload_result *result, char *error, size_t error_size);
void snag_upload_cleanup(int stage_fd, struct snag_upload_result *result);

/* Send one already-open regular file over the same terminal lease. The caller
 * owns file_fd. Result carries only post-EXIT input; no attachment is created. */
int snag_download_send(int tty, int file_fd, const char *name,
                        int (*checkpoint)(void *), void *opaque,
                        struct snag_upload_result *result, char *error, size_t error_size);

#endif
