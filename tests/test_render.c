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

static size_t
capture_wrapped(char *out, size_t out_size, struct snj_buf *delivered)
{
    static const char first[] = "alpha beta gamm";
    static const char second[] = "a delta";
    struct snj_render render;
    struct snj_term term;
    int fds[2];
    int saved;
    ssize_t n;
    size_t used = 0u;

    assert(pipe(fds) == 0);
    saved = dup(STDOUT_FILENO);
    assert(saved >= 0);
    assert(dup2(fds[1], STDOUT_FILENO) >= 0);
    close(fds[1]);
    snj_term_init(&term);
    term.columns = 20u;
    snj_render_init(&render, 0u);
    render.stdout_terminal = true;
    snj_render_attach_term(&render, &term);
    assert(snj_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snj_render_public(&render, first, sizeof(first) - 1u, delivered) == 0);
    assert(snj_render_public(&render, second, sizeof(second) - 1u, delivered) == 0);
    assert(snj_render_public_end(&render) == 0);
    assert(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    while ((n = read(fds[0], out + used, out_size - used - 1u)) > 0)
        used += (size_t)n;
    assert(n == 0);
    close(fds[0]);
    out[used] = '\0';
    snj_term_close(&term);
    return used;
}

int
main(void)
{
    char output[4096];
    struct snj_render render;
    struct snj_buf delivered;

    assert(capture(4u, output, sizeof(output)) == 0u);
    assert(capture(5u, output, sizeof(output)) > 0u);
    assert(count_text(output, "verbosity 5 exposes") == 1u);
    assert(strstr(output, "protocol › request JSON\n{\"x\":1}\n"));
    assert(strstr(output, "protocol › response JSON\n{}\n"));
    assert(!strstr(output, "> POST"));

    assert(capture(6u, output, sizeof(output)) > 0u);
    assert(count_text(output, "verbosity 5 exposes") == 1u);
    assert(strstr(output, "> POST https://example.test\n"));

    snj_buf_init(&delivered, 1024u);
    assert(capture_wrapped(output, sizeof(output), &delivered) > 0u);
    assert(strcmp(output, "alpha beta gamma\ndelta") == 0);
    assert(snj_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data,
                  "alpha beta gamma delta") == 0);
    snj_buf_free(&delivered);

    snj_render_init(&render, 6u);
    errno = 0;
    assert(snj_render_transport(&render, '>', "bad\rline", 8u) < 0);
    assert(errno == EINVAL);
    puts("test_render: ok");
    return 0;
}
