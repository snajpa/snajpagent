/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Test-only slow disk barriers across all program storage. */
static void
delay_flush(int fd)
{
    const char *armed = getenv("SNAJPAGENT_TEST_SYNC_ARM");
    if (!armed || access(armed, F_OK)) return;
    char descriptor[64], path[4096];
    (void)snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
    ssize_t size = readlink(descriptor, path, sizeof(path) - 1u);
    if (size < 0) return;
    path[size] = '\0';
    const char *record = getenv("SNAJPAGENT_TEST_SYNC_LOG");
    int output = record ? open(record, O_WRONLY | O_CREAT | O_APPEND, 0600) : -1;
    if (output >= 0) {
        ssize_t written = write(output, path, (size_t)size);
        if (written == size) written = write(output, "\n", 1u);
        (void)written;
        (void)close(output);
    }
    struct timespec delay = {.tv_sec = 3};
    (void)nanosleep(&delay, NULL);
}

int
fsync(int fd)
{
    int (*original)(int) = dlsym(RTLD_NEXT, "fsync");
    delay_flush(fd);
    return original(fd);
}

int
fdatasync(int fd)
{
    int (*original)(int) = dlsym(RTLD_NEXT, "fdatasync");
    delay_flush(fd);
    return original(fd);
}
