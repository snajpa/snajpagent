/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TOOLS_FILE_H
#define SNAJPAGENT_TOOLS_FILE_H

#include "base.h"
#include "fs.h"

#include <stdbool.h>
#include <stddef.h>

/* File paths are absolute or relative to cwd (an initial ./ is accepted).
 * All remaining components are bounded UTF-8 without . or .., control bytes
 * or symlink traversal. */
int snag_file_path_valid(const char *path, char *error, size_t error_size);
/* Open the appropriate filesystem root for a validated path. */
int snag_file_root_open(const char *cwd, const char *path, char *error, size_t error_size);

/* Open the parent directory of a validated path relative to root_fd without
 * following symlinks; leaf receives the final component. */
int snag_file_parent(int root_fd, const char *path, char leaf[SNAG_NAME_MAX_BYTES + 1u],
                     char *error, size_t error_size);

/* Stage bytes in a fresh temporary file inside parent_fd and return its name in
 * temp; the caller owns cleanup on later failure. */
int snag_file_stage(int parent_fd, const struct snag_buf *bytes, const struct snag_permissions *permissions,
                    char temp[SNAG_NAME_MAX_BYTES + 1u]);

#endif
