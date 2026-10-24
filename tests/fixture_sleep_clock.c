/* SPDX-License-Identifier: GPL-2.0-only */
/* Model Darwin sleep in isolated test processes without suspending the host. */
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int64_t
sleep_offset(void)
{
    const char *path = getenv("SNAJPAGENT_TEST_SLEEP_CLOCK");
    if (!path) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    int64_t offset = 0;
    ssize_t count = read(fd, &offset, sizeof(offset));
    (void)close(fd);
    return count == sizeof(offset) ? offset : 0;
}

static int
sleep_clock_gettime(clockid_t clock, struct timespec *out)
{
    int result = clock_gettime(clock, out);
    if (!result && clock == CLOCK_MONOTONIC) out->tv_sec += sleep_offset();
    return result;
}

__attribute__((used))
static const struct {
    int (*replacement)(clockid_t, struct timespec *);
    int (*original)(clockid_t, struct timespec *);
} interpose __attribute__((section("__DATA,__interpose"))) = {
    sleep_clock_gettime, clock_gettime
};
