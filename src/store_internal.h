/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_INTERNAL_H
#define SNAJPAGENT_STORE_INTERNAL_H

#include "store.h"

#include <stdbool.h>
#include <stddef.h>

#define SNAG_TRASH_SUFFIX_HEX_LEN SNAG_ID_HEX_LEN
#define SNAG_TRASH_NAME_LEN (SNAG_ID_HEX_LEN + 1u + SNAG_TRASH_SUFFIX_HEX_LEN)

enum snag_tail_policy {
    SNAG_TAIL_REJECT,
    SNAG_TAIL_TRUNCATE,
    SNAG_TAIL_IGNORE
};

bool snag_store_trash_id(const char *name, char id[SNAG_ID_HEX_LEN + 1u]);
int snag_store_verify_private_fd(int fd, bool directory, const char *name,
                                char *error, size_t error_size);
int snag_store_open_session_files(struct snag_session *session, bool create,
                                 char *error, size_t error_size);
int snag_store_scan_log(struct snag_session *session,
                       enum snag_tail_policy tail_policy,
                       bool allow_active, char *error, size_t error_size);
int snag_store_complete_trash_delete(struct snag_store *store,
                                    const char *trash_name,
                                    char *error, size_t error_size);

#endif
