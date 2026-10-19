/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int renameat(int olddir, const char *old, int newdir, const char *name);
int unlinkat(int directory, const char *name, int flags);

static void
stop_at(int point)
{
    const char *selected = getenv("SNAJPAGENT_TEST_OPTIMIZATION_STOP");
    if (selected && atoi(selected) == point) _exit(86);
}

int
renameat(int olddir, const char *old, int newdir, const char *name)
{
    int (*real)(int, const char *, int, const char *) = dlsym(RTLD_NEXT, "renameat");
    if (getenv("SNAJPAGENT_TEST_OPTIMIZATION_RENAME_FAIL") &&
        !strcmp(old, ".optimize-install")) {
        errno = EIO;
        return -1;
    }
    int journal = !strcmp(old, "journal.bin") && !strcmp(name, "journal.bin");
    if (journal) stop_at(4);
    int rc = real(olddir, old, newdir, name);
    if (!rc && !strcmp(old, ".optimize-install")) {
        static int caches;
        stop_at(++caches);
    }
    if (!rc && journal) stop_at(5);
    return rc;
}

int
unlinkat(int directory, const char *name, int flags)
{
    int (*real)(int, const char *, int) = dlsym(RTLD_NEXT, "unlinkat");
    int rc = real(directory, name, flags);
    if (!rc && !strcmp(name, "ready")) stop_at(6);
    return rc;
}
