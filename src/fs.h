/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_FS_H
#define SNAJPAGENT_FS_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>

#ifdef _WIN32
typedef struct {
    uint64_t st_dev, st_ino, st_nlink, st_rdev;
    int64_t st_size, st_mtime;
    unsigned int st_mode;
} snag_file_info;

#ifndef S_IFLNK
#define S_IFLNK 0120000
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)
#endif
#else
typedef struct stat snag_file_info;
#endif

int snag_fstat(int fd, snag_file_info *out);
int snag_stat(const char *path, snag_file_info *out);
int snag_lstat(const char *path, snag_file_info *out);
int snag_lstat_at(int dirfd, const char *path, snag_file_info *out);
int snag_unlink_at(int dirfd, const char *path, bool directory);
int snag_rename_at(int from_dir, const char *from, int to_dir, const char *to);

#endif
