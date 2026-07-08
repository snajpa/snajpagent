/* SPDX-License-Identifier: GPL-2.0-only */
#include "render.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static size_t
count_text(const char *haystack, const char *needle)
{
    size_t count = 0u;
    size_t n = strlen(needle);
    for (const char *p = haystack; (p = strstr(p, needle)); p += n)
        ++count;
    return count;
}

static size_t
capture(unsigned int verbosity, char *out, size_t out_size)
{
    int fds[2];
    int saved;
    struct snj_render render;
    ssize_t n;
    size_t used = 0u;

    assert(pipe(fds) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0);
    assert(dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snj_render_init(&render, verbosity);
    assert(snj_render_protocol(&render, "request JSON", "{\"x\":1}", 7u) == 0);
    assert(snj_render_protocol(&render, "response JSON", "{}", 2u) == 0);
    assert(snj_render_transport(&render, '>', "POST https://example.test", 25u) == 0);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    while ((n = read(fds[0], out + used, out_size - used - 1u)) > 0)
        used += (size_t)n;
    assert(n == 0);
    close(fds[0]);
    out[used] = '\0';
    return used;
}

int
main(void)
{
    char output[4096];
    struct snj_render render;

    assert(capture(4u, output, sizeof(output)) == 0u);
    assert(capture(5u, output, sizeof(output)) > 0u);
    assert(count_text(output, "verbosity 5 exposes") == 1u);
    assert(strstr(output, "protocol › request JSON\n{\"x\":1}\n"));
    assert(strstr(output, "protocol › response JSON\n{}\n"));
    assert(!strstr(output, "> POST"));

    assert(capture(6u, output, sizeof(output)) > 0u);
    assert(count_text(output, "verbosity 5 exposes") == 1u);
    assert(strstr(output, "> POST https://example.test\n"));

    snj_render_init(&render, 6u);
    errno = 0;
    assert(snj_render_transport(&render, '>', "bad\rline", 8u) < 0);
    assert(errno == EINVAL);
    puts("test_render: ok");
    return 0;
}
