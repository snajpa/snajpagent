/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_INTERNAL_H
#define SNAJPAGENT_STORE_INTERNAL_H

#include "store.h"

#include <stdbool.h>
#include <stddef.h>

#define SNJ_TRASH_SUFFIX_HEX_LEN SNJ_ID_HEX_LEN
#define SNJ_TRASH_NAME_LEN (SNJ_ID_HEX_LEN + 1u + SNJ_TRASH_SUFFIX_HEX_LEN)

enum snj_tail_policy {
    SNJ_TAIL_REJECT,
    SNJ_TAIL_TRUNCATE,
    SNJ_TAIL_IGNORE
};

char *snj_store_path_join(const char *left, const char *right);
int snj_store_verify_private_fd(int fd, bool directory, const char *name,
                                char *error, size_t error_size);
int snj_store_open_session_files(struct snj_session *session, bool create,
                                 char *error, size_t error_size);
int snj_store_scan_log(struct snj_session *session,
                       enum snj_tail_policy tail_policy,
                       bool allow_active, char *error, size_t error_size);
int snj_store_complete_trash_delete(struct snj_store *store,
                                    const char *trash_name,
                                    char *error, size_t error_size);

#endif
