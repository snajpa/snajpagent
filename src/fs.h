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
int snag_open_read(const char *path, bool directory);
int snag_open_read_at(int dirfd, const char *path, bool directory);
/* Also permit querying native ownership/permissions; callers decide privacy. */
int snag_open_read_security_at(int dirfd, const char *path, bool directory);
/* Exclusive-create a new journal, or append to an existing private journal. */
int snag_open_private_append_at(int dirfd, const char *path, bool create);
/* Exclusive whole-file lock, released by close. Busy nonblocking locks use EAGAIN. */
int snag_lock_file(int fd, bool wait);
struct snag_directory_lock {
    int fd; /* Initialize to -1; the caller retains descriptor ownership. */
#ifdef _WIN32
    void *mutex;
#endif
};
int snag_directory_lock_acquire(int fd, struct snag_directory_lock *lock);
int snag_directory_lock_release(struct snag_directory_lock *lock);
void snag_path_slashes(char *path);

/* Initialize to zero before capture; release after the replacement finishes. */
struct snag_permissions {
#ifdef _WIN32
    void *native;
    bool readonly;
#else
    mode_t mode;
#endif
};
int snag_permissions_capture(int fd, struct snag_permissions *out);
int snag_permissions_apply(int fd, const struct snag_permissions *permissions);
/* Returns 1 for equal, 0 for changed, -1 for an inspection error. */
int snag_permissions_match(int fd, const struct snag_permissions *permissions);
void snag_permissions_free(struct snag_permissions *permissions);

struct snag_directory;
/* Open takes fd ownership only on success; close releases it. */
struct snag_directory *snag_directory_open(int fd);
/* Borrowed UTF-8 name; NULL with errno=0 means EOF, otherwise an error. */
const char *snag_directory_next(struct snag_directory *dir);
int snag_directory_close(struct snag_directory *dir);

#endif
