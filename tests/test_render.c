/* SPDX-License-Identifier: GPL-2.0-only */
#include "render.h"
#include "snajpagent.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
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

static bool
term_history_has(const struct snj_history *term, const char *text)
{
    for (size_t i = 0u; i < term->snapshot.count; ++i)
        if (strcmp(term->snapshot.items[i], text) == 0)
            return true;
    return false;
}

static void
test_prompt_history(void)
{
    const char sample[] = "one\\two\nthree\r\t\x01z";
    char build[4096], temp[4096], path[4096], subdir[4096], entry[64];
    char bytes[4096];
    struct snj_history term;
    struct stat st;
    ssize_t got;
    int fd, status;

    assert(mkdir("build", 0700) == 0 || errno == EEXIST);
    assert(realpath("build", build));
    assert(snprintf(temp, sizeof(temp), "%s/test-history-XXXXXX", build) > 0);
    assert(mkdtemp(temp));
    assert(snprintf(path, sizeof(path), "%s/prompt_history", temp) > 0);
    memset(&term, 0, sizeof(term));
    assert(snj_history_open(&term, temp) == 0);
    assert(snj_history_add(&term, sample) == 0);
    assert(snj_history_add(&term, "duplicate") == 0);
    assert(snj_history_add(&term, "duplicate") == 0);
    snj_history_free(&term);
    assert(stat(path, &st) == 0 && (st.st_mode & 0777u) == 0600u);
    fd = open(path, O_RDONLY);
    assert(fd >= 0 && (got = read(fd, bytes, sizeof(bytes) - 1u)) > 0);
    assert(close(fd) == 0);
    bytes[got] = '\0';
    assert(count_text(bytes, "\n") == 3u);
    assert(strstr(bytes, "one\\\\two\\nthree\\r\\t\\x01z\n"));

    fd = open(path, O_WRONLY | O_APPEND);
    assert(fd >= 0 && snj_write_full(fd, "bad\\q\nunfinished", 17u) == 0);
    assert(close(fd) == 0 && chmod(path, 0644) == 0);
    memset(&term, 0, sizeof(term));
    assert(snj_history_open(&term, temp) == 0);
    assert(snj_history_take_warning(&term));
    assert(!snj_history_take_warning(&term));
    assert(term.snapshot.count == 3u);
    assert(strcmp(term.snapshot.items[0], sample) == 0);
    assert(strcmp(term.snapshot.items[1], "duplicate") == 0);
    assert(strcmp(term.snapshot.items[2], "duplicate") == 0);
    assert(stat(path, &st) == 0 && (st.st_mode & 0777u) == 0600u);
    snj_history_free(&term);

    assert(unlink(path) == 0);
    for (unsigned int process = 0u; process < 2u; ++process) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            struct snj_history writer;
            int rc = 0;
            memset(&writer, 0, sizeof(writer));
            if (snj_history_open(&writer, temp) < 0)
                rc = 1;
            for (unsigned int i = 0u; !rc && i < 10u; ++i) {
                (void)snprintf(entry, sizeof(entry), "child-%u-%u", process, i);
                if (snj_history_add(&writer, entry) < 0)
                    rc = 1;
            }
            snj_history_free(&writer);
            _exit(rc);
        }
    }
    for (unsigned int i = 0u; i < 2u; ++i) {
        assert(wait(&status) > 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    memset(&term, 0, sizeof(term));
    assert(snj_history_open(&term, temp) == 0);
    assert(term.snapshot.count == 20u);
    for (unsigned int process = 0u; process < 2u; ++process)
        for (unsigned int i = 0u; i < 10u; ++i) {
            (void)snprintf(entry, sizeof(entry), "child-%u-%u", process, i);
            assert(term_history_has(&term, entry));
        }
    for (unsigned int i = 0u; i < 105u; ++i) {
        (void)snprintf(entry, sizeof(entry), "bounded-%03u", i);
        assert(snj_history_add(&term, entry) == 0);
    }
    assert(term.snapshot.count == SNJ_HISTORY_COUNT);
    assert(strcmp(term.snapshot.items[0], "bounded-005") == 0);
    assert(strcmp(term.snapshot.items[99], "bounded-104") == 0);
    snj_history_free(&term);
    assert(unlink(path) == 0);

    assert(snprintf(subdir, sizeof(subdir), "%s/symlink", temp) > 0);
    assert(mkdir(subdir, 0700) == 0);
    assert(snprintf(path, sizeof(path), "%s/prompt_history", subdir) > 0);
    assert(symlink("../target", path) == 0);
    memset(&term, 0, sizeof(term));
    assert(snj_history_open(&term, subdir) < 0);
    assert(snj_history_take_warning(&term));
    snj_history_free(&term);
    assert(unlink(path) == 0 && rmdir(subdir) == 0);

    if (geteuid() == 0u) {
        assert(snprintf(subdir, sizeof(subdir), "%s/wrong-owner", temp) > 0);
        assert(mkdir(subdir, 0700) == 0);
        assert(snprintf(path, sizeof(path), "%s/prompt_history", subdir) > 0);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0 && close(fd) == 0);
        assert(chown(path, 65534u, 65534u) == 0);
        memset(&term, 0, sizeof(term));
        assert(snj_history_open(&term, subdir) < 0);
        assert(snj_history_take_warning(&term));
        snj_history_free(&term);
        assert(unlink(path) == 0 && rmdir(subdir) == 0);
    }

    assert(snprintf(subdir, sizeof(subdir), "%s/nonregular", temp) > 0);
    assert(mkdir(subdir, 0700) == 0);
    assert(snprintf(path, sizeof(path), "%s/prompt_history", subdir) > 0);
    assert(mkdir(path, 0700) == 0);
    memset(&term, 0, sizeof(term));
    assert(snj_history_open(&term, subdir) < 0);
    snj_history_free(&term);
    assert(rmdir(path) == 0 && rmdir(subdir) == 0);

    assert(snprintf(subdir, sizeof(subdir), "%s/oversized", temp) > 0);
    assert(mkdir(subdir, 0700) == 0);
    assert(snprintf(path, sizeof(path), "%s/prompt_history", subdir) > 0);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)(SNJ_HISTORY_BYTES * 4u +
                                 SNJ_HISTORY_COUNT + 1u)) == 0);
    assert(close(fd) == 0);
    memset(&term, 0, sizeof(term));
    assert(snj_history_open(&term, subdir) < 0);
    snj_history_free(&term);
    assert(unlink(path) == 0 && rmdir(subdir) == 0);
    assert(rmdir(temp) == 0);
}

static void
test_prompt_clock(void)
{
    struct snj_term term;
    char *saved_tz = getenv("TZ") ? strdup(getenv("TZ")) : NULL;

    assert(setenv("TZ", "UTC0", 1) == 0);
    tzset();
    snj_term_init(&term);
    snj_term_capture_prompt_clock(&term, 86399);
    assert(term.prompt_clock.captured && term.prompt_clock.valid);
    assert(term.prompt_clock.hour == 23 && term.prompt_clock.minute == 59 &&
           term.prompt_clock.second == 59);
    snj_term_capture_prompt_clock(&term, 86400);
    assert(term.prompt_clock.hour == 23 && term.prompt_clock.second == 59);
    term.prompt_clock.captured = false;
    snj_term_capture_prompt_clock(&term, 86400);
    assert(term.prompt_clock.valid && term.prompt_clock.hour == 0 &&
           term.prompt_clock.minute == 0 && term.prompt_clock.second == 0);
    term.prompt_clock.captured = false;
    snj_term_capture_prompt_clock(&term, (time_t)-1);
    assert(term.prompt_clock.captured && !term.prompt_clock.valid);
    snj_term_capture_prompt_clock(&term, 0);
    assert(!term.prompt_clock.valid);
    snj_term_close(&term);
    assert((saved_tz ? setenv("TZ", saved_tz, 1) : unsetenv("TZ")) == 0);
    free(saved_tz);
    tzset();
}

static void
test_prompt_spinners(void)
{
    struct snj_term term;
    const char prompt[] = {'x', (char)0xfd, (char)0xfe, '>', '\0'};
    char oversized[SNJ_TERM_LABEL_BYTES];
    const char *spinners[SNJ_TERM_SPINNER_COUNT] = {"\\0◆", " |/-", "\\0"};
    const char *bad[SNJ_TERM_SPINNER_COUNT] = {"\x80", " ", " "};
    const char *wide[SNJ_TERM_SPINNER_COUNT] = {" 😀", " ", " "};
    char saved[SNJ_TERM_LABEL_BYTES];

    snj_term_init(&term);
    assert(snj_term_set_prompt_template(&term, false, prompt, spinners, 8u, 0u)
           == 0);
    assert(strcmp(term.label, "x >") == 0);
    assert(term.spinner[SNJ_TERM_SPINNER_GOAL].inactive_len == 0u);
    assert(term.spinner[SNJ_TERM_SPINNER_PROVIDER].inactive_len == 1u);
    assert(term.spinner[SNJ_TERM_SPINNER_TOOL].inactive_len == 0u);
    assert(snj_term_set_spinner_states(&term,
        1u << SNJ_TERM_SPINNER_GOAL) == 0);
    assert(strcmp(term.label, "x◆ >") == 0);
    assert(snj_term_set_spinner_states(&term,
        1u << SNJ_TERM_SPINNER_PROVIDER) == 0);
    assert(strcmp(term.label, "x|>") == 0);
    memcpy(saved, term.label, sizeof(saved));
    assert(snj_term_set_prompt_template(&term, false, prompt, bad, 8u, 0u) < 0);
    assert(memcmp(saved, term.label, sizeof(saved)) == 0);
    assert(term.spinner_states == (1u << SNJ_TERM_SPINNER_PROVIDER));
    assert(snj_term_set_prompt_template(&term, false, prompt, spinners, 8u,
                                        1u << SNJ_TERM_SPINNER_COUNT) < 0);
    memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized) - 3u] = (char)SNJ_TERM_SPINNER_MARKER_BASE;
    oversized[sizeof(oversized) - 2u] = '\0';
    assert(snj_term_set_prompt_template(&term, false, oversized, wide, 8u,
                                        0u) < 0);
    assert(memcmp(saved, term.label, sizeof(saved)) == 0);
    {
        const char padded[] = "\xfd\xfe  9%> ";
        const char *stable[] = {" ⚑", " P", " ⠋"};
        const char *compact[] = {"\\0◆", "\\0P", "\\0T"};
        const char *blank[] = {" ", "\\0 P", "\\0"};

        for (unsigned int state = 0u; state < 8u; ++state) {
            assert(snj_term_set_prompt_template(&term, true, padded, stable,
                                                8u, state) == 0);
            char expected[32];

            assert(snprintf(expected, sizeof(expected), "%s%s  9%%> ",
                state & 1u ? "⚑" : " ",
                state & 4u ? "⠋" : state & 2u ? "P" : " ") > 0);
            assert(strcmp(term.label, expected) == 0);
            assert(snj_term_text_width(term.label, strlen(term.label)) == 8u);
            assert(snj_term_set_spinner_states(&term, 2u) == 0);
            assert(strcmp(term.label, " P  9%> ") == 0);
            assert(snj_term_set_spinner_states(&term, 6u) == 0);
            assert(strcmp(term.label, " ⠋  9%> ") == 0);
            assert(snj_term_set_spinner_states(&term, 0u) == 0);
            assert(strcmp(term.label, "    9%> ") == 0);
        }
        assert(snj_term_set_prompt_template(&term, true, padded, compact,
                                            8u, 0u) == 0);
        assert(strcmp(term.label, "  9%> ") == 0);
        assert(snj_term_set_spinner_states(&term, 2u) == 0);
        assert(strcmp(term.label, "P  9%> ") == 0);
        assert(snj_term_set_spinner_states(&term, 4u) == 0);
        assert(strcmp(term.label, "T  9%> ") == 0);
        assert(term.spinner[SNJ_TERM_SPINNER_PROVIDER].current_len == 0u);
        assert(term.spinner[SNJ_TERM_SPINNER_TOOL].current_len == 1u);
        assert(snj_term_set_prompt_template(&term, true, padded, blank,
                                            8u, 2u) == 0);
        assert(strcmp(term.label, "    9%> ") == 0);
        assert(term.spinner[SNJ_TERM_SPINNER_PROVIDER].current_len == 1u);
        assert(snj_term_set_spinner_states(&term, 0u) == 0);
        assert(strcmp(term.label, "   9%> ") == 0);
        assert(snj_term_set_prompt_template(&term, true, "  9%> ", compact,
                                            8u, 7u) == 0);
        assert(strcmp(term.label, "  9%> ") == 0);
    }
    {
        const char *frames[] = {" ⚑", " P", " ⠋"};
        int fds[2], saved_fd;
        char output[512];
        ssize_t bytes;

        /* The longest activity variant must fit even while the slot is idle. */
        memset(oversized, 'x', sizeof(oversized));
        oversized[sizeof(oversized) - 3u] = (char)0xfe;
        oversized[sizeof(oversized) - 2u] = '\0';
        assert(snj_term_set_prompt_template(&term, false, oversized, frames, 8u, 0u) < 0);
        assert(snj_term_set_prompt_template(&term, true, "\xfd\xfe  9%> ",
                                            frames, 8u, 2u) == 0);
        assert(snj_buf_append(&term.draft, "draft", 5u) == 0);
        term.cursor = term.draft.len;
        term.columns = 80u;
        term.prompt_visible = term.capable = true;
        term.rendered_cursor_col = 13u;
        assert(pipe(fds) == 0);
        saved_fd = dup(STDERR_FILENO);
        assert(saved_fd >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
        close(fds[1]);
        assert(snj_term_set_spinner_states(&term, 6u) == 0);
        assert(snj_term_set_spinner_states(&term, 2u) == 0);
        assert(term.cursor == 5u && term.rendered_cursor_col == 13u);
        assert(dup2(saved_fd, STDERR_FILENO) >= 0);
        close(saved_fd);
        bytes = read(fds[0], output, sizeof(output) - 1u);
        assert(bytes > 0);
        output[bytes] = '\0';
        close(fds[0]);
        assert(strstr(output, "⠋") && strstr(output, "P"));
        assert(!strstr(output, "9%") && !strstr(output, "draft"));
        assert(!strstr(output, "\033[2K") && !strchr(output, '\n'));
        term.prompt_visible = false;
    }
    snj_term_close(&term);
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
capture_orientation(bool resumed, char *out, size_t out_size)
{
    struct snj_render render;
    struct snj_session session = {0};
    int fds[2];
    int saved;
    ssize_t n;
    size_t used = 0u;

    assert(pipe(fds) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snj_render_init(&render, 0u);
    memcpy(session.id, "0123456789abcdef0123456789abcdef",
           sizeof(session.id));
    assert(snprintf(session.default_model, sizeof(session.default_model),
                    "model-must-not-appear") > 0);
    session.workspace = "/work/tree";
    session.turn_count = 3u;
    session.pending_queue_count = 2u;
    assert(snj_render_orientation(&render, session.workspace, session.id,
                                  session.turn_count, session.pending_queue_count,
                                  resumed) == 0);
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
drain_available(int fd, char *out, size_t out_size, size_t used)
{
    ssize_t n;

    while ((n = read(fd, out + used, out_size - used - 1u)) > 0)
        used += (size_t)n;
    assert(n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)));
    out[used] = '\0';
    return used;
}

static size_t
capture_wrapped(const char *first, const char *second, unsigned int columns,
                bool markdown, const char *first_output,
                const char *second_output, char *out, size_t out_size,
                struct snj_buf *delivered)
{
    struct snj_render render;
    struct snj_term term;
    int fds[2];
    int saved;
    ssize_t n;
    size_t used = 0u;

    assert(pipe(fds) == 0);
    assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    saved = dup(STDOUT_FILENO);
    assert(saved >= 0);
    assert(dup2(fds[1], STDOUT_FILENO) >= 0);
    close(fds[1]);
    snj_term_init(&term);
    term.columns = columns;
    snj_render_init(&render, 0u);
    render.stdout_terminal = true;
    snj_render_set_markdown(&render, markdown);
    snj_render_attach_term(&render, &term);
    assert(snj_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snj_render_public(&render, first, strlen(first), delivered) == 0);
    used = drain_available(fds[0], out, out_size, used);
    assert(strcmp(out, first_output) == 0);
    assert(snj_render_public(&render, second, strlen(second), delivered) == 0);
    used = drain_available(fds[0], out, out_size, used);
    assert(strcmp(out, second_output) == 0);
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

static void
test_punctuation_wrapping(void)
{
    static const char *const punctuation[] = {
        "-", ",", ".", ";", ":", "!", "?", "/", ")", "]", "}",
        "..", "\")",
        "\xe2\x80\x90", "\xe2\x80\x92", "\xe2\x80\x93",
        "\xe2\x80\x94", "\xe2\x80\xa6"
    };
    static const char first[] = "1234567890 word";
    char second[32];
    char first_output[64];
    char second_output[128];
    char delivered_output[128];
    char output[256];

    for (size_t enabled = 0u; enabled < 2u; ++enabled) {
        const char *prefix = enabled ? "• " : "";

        assert(snprintf(first_output, sizeof(first_output), "%s%s",
                        prefix, first) > 0);
        for (size_t i = 0u; i < sizeof(punctuation) / sizeof(punctuation[0]);
             ++i) {
            struct snj_buf delivered;

            assert(snprintf(second, sizeof(second), "%smores",
                            punctuation[i]) > 0);
            assert(snprintf(second_output, sizeof(second_output),
                            "%s%s%s\n%smores", prefix, first,
                            punctuation[i], enabled ? "  " : "") > 0);
            assert(snprintf(delivered_output, sizeof(delivered_output),
                            "%s%smores", first, punctuation[i]) > 0);
            snj_buf_init(&delivered, sizeof(delivered_output));
            assert(capture_wrapped(first, second, 20u, enabled != 0u,
                                   first_output, second_output, output,
                                   sizeof(output), &delivered) > 0u);
            assert(snj_buf_terminate(&delivered) == 0);
            assert(strcmp((const char *)delivered.data, delivered_output) == 0);
            snj_buf_free(&delivered);
        }
        {
            struct snj_buf delivered;

            assert(snprintf(first_output, sizeof(first_output),
                            "%s1234567890 ", prefix) > 0);
            assert(snprintf(second_output, sizeof(second_output),
                            "%s1234567890 \n%s-something", prefix,
                            enabled ? "  " : "") > 0);
            snj_buf_init(&delivered, 32u);
            assert(capture_wrapped("1234567890 ", "-something", 20u,
                                   enabled != 0u, first_output, second_output,
                                   output, sizeof(output), &delivered) > 0u);
            assert(snj_buf_terminate(&delivered) == 0);
            assert(strcmp((const char *)delivered.data,
                          "1234567890 -something") == 0);
            snj_buf_free(&delivered);
        }
    }
}

static size_t capture_markdown_width(const char *text, bool enabled,
                                     bool split, enum snj_color_mode color,
                                     unsigned int columns, char *out,
                                     size_t out_size,
                                     struct snj_buf *delivered);

static size_t
capture_markdown(const char *text, bool enabled, bool split,
                 enum snj_color_mode color, char *out, size_t out_size,
                 struct snj_buf *delivered)
{
    return capture_markdown_width(text, enabled, split, color, 120u, out,
                                  out_size, delivered);
}

static size_t
capture_markdown_width(const char *text, bool enabled, bool split,
                       enum snj_color_mode color, unsigned int columns,
                       char *out, size_t out_size, struct snj_buf *delivered)
{
    struct snj_render render;
    struct snj_term term;
    size_t len = strlen(text);
    size_t used = 0u;
    ssize_t n;
    int fds[2];
    int saved;

    assert(pipe(fds) == 0);
    saved = dup(STDOUT_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDOUT_FILENO) >= 0);
    close(fds[1]);
    snj_term_init(&term);
    term.columns = columns;
    snj_render_init(&render, 0u);
    render.stdout_terminal = true;
    snj_render_set_color(&render, color);
    snj_render_set_markdown(&render, enabled);
    snj_render_attach_term(&render, &term);
    assert(snj_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    if (split) {
        for (size_t i = 0u; i < len; ++i)
            assert(snj_render_public(&render, text + i, 1u, delivered) == 0);
    } else {
        assert(snj_render_public(&render, text, len, delivered) == 0);
    }
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

static size_t
capture_prompt_boundary(const char *text, bool markdown,
                        char *out, size_t out_size)
{
    struct snj_render render;
    struct snj_term term;
    size_t used = 0u;
    ssize_t n;
    int fds[2];
    int saved_stdout;
    int saved_stderr;

    assert(pipe(fds) == 0);
    saved_stdout = dup(STDOUT_FILENO);
    saved_stderr = dup(STDERR_FILENO);
    assert(saved_stdout >= 0 && saved_stderr >= 0);
    assert(dup2(fds[1], STDOUT_FILENO) >= 0);
    assert(dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snj_term_init(&term);
    term.columns = 120u;
    snj_render_init(&render, 0u);
    render.stdout_terminal = true;
    render.stderr_terminal = true;
    snj_render_set_color(&render, SNJ_COLOR_NEVER);
    snj_render_set_markdown(&render, markdown);
    snj_render_attach_term(&render, &term);
    assert(snj_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snj_render_public(&render, text, strlen(text), NULL) == 0);
    assert(snj_render_public_end(&render) == 0);
    assert(snj_render_before_prompt(&render) == 0);
    assert(snj_render_before_prompt(&render) == 0);
    assert(snj_render_prompt(&render, "model/low › ") == 0);
    snj_render_free(&render);
    snj_term_close(&term);
    assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stdout);
    close(saved_stderr);
    while ((n = read(fds[0], out + used, out_size - used - 1u)) > 0)
        used += (size_t)n;
    assert(n == 0);
    close(fds[0]);
    out[used] = '\0';
    return used;
}

static void
test_model_prompt_boundaries(void)
{
    static const struct {
        const char *source;
        const char *rendered;
        bool markdown;
    } cases[] = {
        { "plain prose", "• plain prose", true },
        { "# heading", "heading", true },
        { "- unordered", "• unordered", true },
        { "7. ordered", "7. ordered", true },
        { "> quotation", "│ quotation", true },
        { "```c\nint x;\n```", "┌─ c\n│ int x;\n└─", true },
        { "| A |\n| --- |\n| B |",
          "┌───┐\n│ A │\n├───┤\n│ B │\n└───┘", true },
        { "**inline**", "• inline", true },
        { "- literal", "- literal", false },
    };
    static const char *const suffixes[] = { "", "\n", "\n\n" };
    char source[256];
    char expected[256];
    char output[512];

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        for (size_t j = 0u; j < sizeof(suffixes) / sizeof(suffixes[0]); ++j) {
            assert(snprintf(source, sizeof(source), "%s%s",
                            cases[i].source, suffixes[j]) > 0);
            assert(snprintf(expected, sizeof(expected),
                            "%s\n\nmodel/low › ", cases[i].rendered) > 0);
            assert(capture_prompt_boundary(source, cases[i].markdown, output,
                                           sizeof(output)) > 0u);
            assert(strcmp(output, expected) == 0);
        }
    }
}

static void
test_input_model_boundaries(void)
{
    static const char *const suffixes[] = { "", "\n", "\n\n" };
    char output[256];
    char question[32];

    for (size_t enabled = 0u; enabled < 2u; ++enabled) {
        for (size_t suffix = 0u;
             suffix < sizeof(suffixes) / sizeof(suffixes[0]); ++suffix) {
            struct snj_render render;
            struct snj_term term;
            int fds[2];
            int saved_stdout;
            int saved_stderr;
            ssize_t n;
            size_t used = 0u;

            assert(pipe(fds) == 0);
            saved_stdout = dup(STDOUT_FILENO);
            saved_stderr = dup(STDERR_FILENO);
            assert(saved_stdout >= 0 && saved_stderr >= 0);
            assert(dup2(fds[1], STDOUT_FILENO) >= 0);
            assert(dup2(fds[1], STDERR_FILENO) >= 0);
            close(fds[1]);
            snj_term_init(&term);
            term.columns = 120u;
            snj_render_init(&render, 0u);
            render.stdout_terminal = true;
            render.stderr_terminal = true;
            snj_render_set_color(&render, SNJ_COLOR_NEVER);
            snj_render_set_markdown(&render, enabled != 0u);
            snj_render_attach_term(&render, &term);
            assert(snprintf(question, sizeof(question), "question%s",
                            suffixes[suffix]) > 0);
            assert(snj_render_input_submitted(&render, "model/low › ",
                                              question) == 0);
            assert(snj_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
            assert(snj_render_public(&render, "answer", 6u, NULL) == 0);
            assert(snj_render_public_end(&render) == 0);
            assert(snj_render_before_prompt(&render) == 0);
            assert(snj_render_before_prompt(&render) == 0);
            assert(snj_render_prompt(&render, "model/low › ") == 0);
            snj_render_free(&render);
            snj_term_close(&term);
            assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
            assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
            close(saved_stdout);
            close(saved_stderr);
            while ((n = read(fds[0], output + used,
                             sizeof(output) - used - 1u)) > 0)
                used += (size_t)n;
            assert(n == 0);
            close(fds[0]);
            output[used] = '\0';
            assert(strcmp(output, enabled ?
                          "model/low › question\n\n• answer\n\nmodel/low › " :
                          "model/low › question\n\nanswer\n\nmodel/low › ") == 0);
        }
    }
    {
        struct snj_render render;
        struct snj_term term;
        int fds[2];
        int saved;
        ssize_t n;
        size_t used = 0u;

        assert(pipe(fds) == 0);
        saved = dup(STDERR_FILENO);
        assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
        close(fds[1]);
        snj_term_init(&term);
        memcpy(term.label, "model/low › ", strlen("model/low › ") + 1u);
        term.line_submission_echoed = true;
        snj_render_init(&render, 0u);
        render.stderr_terminal = true;
        snj_render_attach_term(&render, &term);
        assert(snj_render_input_submitted(&render, "model/low › ",
                                          "question") == 0);
        snj_render_free(&render);
        snj_term_close(&term);
        assert(dup2(saved, STDERR_FILENO) >= 0);
        close(saved);
        while ((n = read(fds[0], output + used,
                         sizeof(output) - used - 1u)) > 0)
            used += (size_t)n;
        assert(n == 0);
        close(fds[0]);
        output[used] = '\0';
        assert(strcmp(output, "\n") == 0);
    }
}

static size_t
capture_static_markdown(unsigned int verbosity, char *out, size_t out_size)
{
    struct snj_render render;
    struct snj_irc_event event;
    struct snj_session session;
    size_t used = 0u;
    ssize_t n;
    int fds[2];
    int saved;

    assert(pipe(fds) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snj_render_init(&render, verbosity);
    render.stderr_terminal = true;
    snj_render_set_color(&render, SNJ_COLOR_NEVER);
    snj_render_set_networked(&render, true, "agent");
    memset(&event, 0, sizeof(event));
    event.kind = SNJ_IRC_MESSAGE;
    event.timestamp_ms = 1000u;
    memcpy(event.endpoint, "local", 6u);
    memcpy(event.nick, "agent", 6u);
    memcpy(event.text, "**answer** and `code`", 22u);
    event.local = true;
    assert(snj_render_irc_event(&render, &event) == 0);
    memcpy(event.text, "- actual list item", 19u);
    assert(snj_render_irc_event(&render, &event) == 0);
    memcpy(event.nick, "operator", 9u);
    memcpy(event.text, "**literal operator**", 21u);
    event.op = true;
    assert(snj_render_irc_event(&render, &event) == 0);
    event.op = false;
    memcpy(event.nick, "remote", 7u);
    memcpy(event.text, "```c", 5u);
    assert(snj_render_irc_event(&render, &event) == 0);
    memcpy(event.text, "int value = 1;", 15u);
    assert(snj_render_irc_event(&render, &event) == 0);
    memcpy(event.text, "```", 4u);
    assert(snj_render_irc_event(&render, &event) == 0);
    event.kind = SNJ_IRC_NOTICE;
    memcpy(event.text, "**literal notice**", 19u);
    assert(snj_render_irc_event(&render, &event) == 0);
    event.kind = SNJ_IRC_MESSAGE;
    memcpy(event.text, "```c", 5u);
    assert(snj_render_irc_event(&render, &event) == 0);
    event.kind = SNJ_IRC_QUIT;
    memcpy(event.text, "gone", 5u);
    assert(snj_render_irc_event(&render, &event) == 0);
    event.kind = SNJ_IRC_MESSAGE;
    memcpy(event.text, "plain after quit", 17u);
    assert(snj_render_irc_event(&render, &event) == 0);
    memset(&session, 0, sizeof(session));
    session.last_user = "**literal user**";
    session.last_assistant = "## Saved *answer*";
    assert(snj_render_history(&render, session.last_user, session.last_assistant) == 0);
    snj_render_set_markdown(&render, false);
    event.kind = SNJ_IRC_MESSAGE;
    memcpy(event.text, "**literal agent**", 18u);
    assert(snj_render_irc_event(&render, &event) == 0);
    session.last_user = NULL;
    session.last_assistant = "## Literal assistant";
    assert(snj_render_history(&render, session.last_user, session.last_assistant) == 0);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    while ((n = read(fds[0], out + used, out_size - used - 1u)) > 0)
        used += (size_t)n;
    assert(n == 0);
    close(fds[0]);
    out[used] = '\0';
    return used;
}

static void
test_history_failure(void)
{
    struct snj_render render;
    struct snj_session session = {0};
    int fds[2];
    int saved;

    assert(pipe(fds) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snj_render_init(&render, 0u);
    render.stderr_terminal = true;
    session.last_assistant = "\xff";
    errno = 0;
    assert(snj_render_history(&render, session.last_user, session.last_assistant) < 0);
    assert(errno == EILSEQ && !render.public_item_open);
    assert(snj_render_public_begin(&render, STDERR_FILENO, NULL) == 0);
    assert(snj_render_public_end(&render) == 0);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    close(fds[0]);
}

static void
test_markdown_streaming(void)
{
    static const char first[] = "# **Live";
    static const char second[] = "** café [docs](";
    static const char third[] = "https://example.test) and `co";
    static const char fourth[] = "de`\n";
    struct snj_render render;
    struct snj_term term;
    struct snj_buf delivered;
    char output[4096] = {0};
    size_t used = 0u;
    int fds[2];
    int saved;

    assert(pipe(fds) == 0);
    assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    saved = dup(STDOUT_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDOUT_FILENO) >= 0);
    close(fds[1]);
    snj_term_init(&term);
    term.columns = 80u;
    snj_render_init(&render, 0u);
    render.stdout_terminal = true;
    snj_render_set_color(&render, SNJ_COLOR_NEVER);
    snj_render_attach_term(&render, &term);
    snj_buf_init(&delivered, 1024u);
    assert(snj_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snj_render_public(&render, first, sizeof(first) - 1u,
                             &delivered) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strcmp(output, "Live") == 0);
    for (size_t i = 0u; i < sizeof(second) - 1u; ++i)
        assert(snj_render_public(&render, second + i, 1u, &delivered) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strcmp(output, "Live café [docs] <") == 0);
    assert(snj_render_public(&render, third, sizeof(third) - 1u,
                             &delivered) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strcmp(output,
                  "Live café [docs] <https://example.test> and co") == 0);
    assert(snj_render_public(&render, fourth, sizeof(fourth) - 1u,
                             &delivered) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strcmp(output,
                  "Live café [docs] <https://example.test> and code\n") == 0);
    assert(snj_render_public_end(&render) == 0);
    assert(snj_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data,
                  "# **Live** café [docs](https://example.test) and `code`\n") == 0);
    snj_buf_free(&delivered);

    assert(snj_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snj_render_public(&render, "**aborted", 9u, NULL) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strcmp(output,
                  "Live café [docs] <https://example.test> and code\n\n"
                  "• aborted") == 0);
    assert(snj_render_public_abort(&render) == 0);
    assert(snj_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snj_render_public(&render, "literal", 7u, NULL) == 0);
    assert(snj_render_public_end(&render) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strcmp(output,
                  "Live café [docs] <https://example.test> and code\n\n"
                  "• aborted\n\n• literal") == 0);
    assert(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    while (read(fds[0], output, sizeof(output)) > 0)
        ;
    close(fds[0]);
    snj_term_close(&term);
}

static void
test_markdown_tables(void)
{
    static const char markdown[] =
        "| Name | State | Count\n"
        "| :--- | :---: | ---:\n"
        "| **alpha** | `ready` | 7\n"
        "| escaped \\| pipe | [docs](https://example.test) | 42 |\n"
        "after table\n";
    static const char rendered[] =
        "┌────────────────┬───────────────────────────────┬───────┐\n"
        "│ Name           │             State             │ Count │\n"
        "├────────────────┼───────────────────────────────┼───────┤\n"
        "│ alpha          │             ready             │     7 │\n"
        "│ escaped | pipe │ [docs] <https://example.test> │    42 │\n"
        "└────────────────┴───────────────────────────────┴───────┘\n"
        "• after table\n";
    static const char narrow[] =
        "┌─ table\n"
        "├─ row\n"
        "│ Name: alpha\n"
        "│ State: ready\n"
        "│ Count: 7\n"
        "├─ row\n"
        "│ Name: escaped | pipe\n"
        "│ State: [docs] <https://example.test>\n"
        "│ Count: 42\n"
        "└─\n"
        "• after table\n";
    static const char malformed[] =
        "| Name | State |\n"
        "| -- | nope |\n"
        "after\n";
    static const char code_pipe[] =
        "| Code | Other\n"
        "| --- | ---\n"
        "| `a|b` | tail\n";
    static const char code_pipe_rendered[] =
        "┌──────┬───────┐\n"
        "│ Code │ Other │\n"
        "├──────┼───────┤\n"
        "│ a|b  │ tail  │\n"
        "└──────┴───────┘\n";
    char output[8192];
    struct snj_buf delivered;

    snj_buf_init(&delivered, sizeof(markdown));
    assert(capture_markdown(markdown, true, true, SNJ_COLOR_NEVER,
                            output, sizeof(output), &delivered) > 0u);
    assert(strcmp(output, rendered) == 0);
    assert(snj_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data, markdown) == 0);
    snj_buf_free(&delivered);
    assert(capture_markdown(markdown, true, false, SNJ_COLOR_ALWAYS,
                            output, sizeof(output), NULL) > 0u);
    assert(strstr(output, "\033[0;1mName") != NULL);
    assert(strstr(output, "\033[0;1malpha") != NULL);
    assert(strstr(output, "\033[0;33mready") != NULL);
    assert(strstr(output, "\033[0;4;34mhttps://example.test") != NULL);

    assert(capture_markdown_width(markdown, true, true, SNJ_COLOR_NEVER,
                                  28u, output, sizeof(output), NULL) > 0u);
    assert(strcmp(output, narrow) == 0);

    assert(capture_markdown(malformed, true, true, SNJ_COLOR_NEVER,
                            output, sizeof(output), NULL) > 0u);
    assert(strcmp(output, "• | Name | State |\n| -- | nope |\nafter\n") == 0);
    assert(capture_markdown(code_pipe, true, true, SNJ_COLOR_NEVER,
                            output, sizeof(output), NULL) > 0u);
    assert(strcmp(output, code_pipe_rendered) == 0);
    assert(capture_markdown(markdown, false, false, SNJ_COLOR_NEVER,
                            output, sizeof(output), NULL) > 0u);
    assert(strcmp(output, markdown) == 0);
}

static size_t
capture_color(enum snj_color_mode mode, bool networked,
              unsigned int verbosity, int timeout_ms,
              uint32_t default_timeout_ms, uint32_t max_output_bytes,
              char *out, size_t out_size)
{
    struct snj_render render;
    struct snj_irc_event event;
    struct snj_response_item call;
    json_t *arguments;
    json_t *result;
    int fds[2];
    int saved;
    ssize_t n;
    size_t used = 0u;

    assert(pipe(fds) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snj_render_init(&render, verbosity);
    snj_render_set_networked(&render, networked, "agent");
    snj_render_set_color(&render, mode);
    assert(snj_render_submitted(&render, "› ", "plain") == 0);
    assert(snj_render_warning_ctx(&render, "careful") == 0);
    assert(snj_render_error_ctx(&render, "broken") == 0);
    assert(snj_render_host(&render, "status") == 0);
    assert(snj_render_event(&render, 7u, "compaction_completed") == 0);
    memset(&call, 0, sizeof(call));
    arguments = json_object();
    assert(arguments != NULL);
    assert(json_object_set_new(arguments, "command",
                               json_string("printf plain")) == 0);
    assert(json_object_set_new(arguments, "timeout_ms",
                               timeout_ms < 0 ? json_null() :
                                                json_integer(timeout_ms)) == 0);
    call.name = "exec_command";
    call.arguments = arguments;
    {
        struct snj_render_block block;
        assert(snj_render_prepare_tool_start(&block, &call, "/tmp",
                                             default_timeout_ms) == 0);
        assert(snj_render_tool_block(&render, &block) == 0);
        snj_buf_free(&block.text);
    }
    json_decref(arguments);
    result = json_object();
    assert(result != NULL);
    assert(json_object_set_new(result, "duration_ms", json_integer(12)) == 0);
    assert(json_object_set_new(result, "exit_code", json_integer(0)) == 0);
    assert(json_object_set_new(result, "model_text",
                               json_string("fixture tool output: café\n")) == 0);
    assert(json_object_set_new(result, "reason", json_null()) == 0);
    assert(json_object_set_new(result, "status", json_string("succeeded")) == 0);
    {
        struct snj_render_block block;
        assert(snj_render_prepare_tool_finish(&block, call.name, result,
                                              max_output_bytes) == 0);
        assert(snj_render_tool_block(&render, &block) == 0);
        snj_buf_free(&block.text);
    }
    json_decref(result);
    memset(&event, 0, sizeof(event));
    event.kind = SNJ_IRC_MESSAGE;
    event.timestamp_ms = 1000u;
    memcpy(event.nick, "agent", 6u);
    memcpy(event.text, "answer", 7u);
    event.local = true;
    assert(snj_render_irc_event(&render, &event) == 0);
    if (networked)
        assert(snj_render_set_view(&render, SNJ_RENDER_ROLLOUT) == 0);
    snj_render_free(&render);
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
capture_lifecycle(unsigned int verbosity, enum snj_color_mode color,
                  char *out, size_t out_size)
{
    static const char *const events[] = {
        "compaction_completed", "goal_started", "goal_reworded",
        "goal_completed", "goal_cancelled", "turn_completed"
    };
    struct snj_render render;
    size_t used = 0u;
    ssize_t n;
    int fds[2];
    int saved;

    assert(pipe(fds) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snj_render_init(&render, verbosity);
    snj_render_set_color(&render, color);
    for (size_t i = 0u; i < sizeof(events) / sizeof(events[0]); ++i)
        assert(snj_render_event(&render, i + 1u, events[i]) == 0);
    snj_render_free(&render);
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
capture_resume_hint(enum snj_color_mode color, char *out, size_t out_size)
{
    static const char command[] = "'snajpagent' --resume '0123'";
    struct snj_render render;
    size_t used = 0u;
    ssize_t n;
    int fds[2];
    int saved;

    assert(pipe(fds) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snj_render_init(&render, 0u);
    snj_render_set_color(&render, color);
    assert(snj_render_resume_hint(&render, command, sizeof(command) - 1u) == 0);
    snj_render_free(&render);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    while ((n = read(fds[0], out + used, out_size - used - 1u)) > 0)
        used += (size_t)n;
    assert(n == 0);
    close(fds[0]);
    out[used] = '\0';
    return used;
}

static void
test_append_only_views(unsigned int verbosity)
{
    struct snj_render render;
    struct snj_irc_event event = {0};
    struct snj_buf delivered;
    char output[8192] = {0};
    size_t used = 0u;
    int fds[2];
    int saved;

    assert(pipe(fds) == 0);
    assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snj_render_init(&render, verbosity);
    snj_render_set_color(&render, SNJ_COLOR_NEVER);
    snj_render_set_networked(&render, true, "agent");
    render.verbosity = 1u;
    event.kind = SNJ_IRC_MESSAGE;
    event.timestamp_ms = 1000u;
    memcpy(event.nick, "peer", 5u);
    memcpy(event.text, "chat-one", 9u);
    assert(snj_render_irc_event(&render, &event) == 0);
    assert(snj_render_event(&render, 1u, "goal_started") == 0);
    assert(snj_render_event(&render, 2u, "compaction_completed") == 0);
    snj_buf_init(&delivered, 1024u);
    assert(snj_render_rollout_begin(&render, STDERR_FILENO, "agent › ") == 0);
    assert(snj_render_rollout(&render, "hidden-prefix ", 14u, &delivered) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strstr(output, "chat-one") != NULL);
    assert(strstr(output, "Goal set") == NULL);
    assert(strstr(output, "Compacted") == NULL);
    assert(strstr(output, "hidden-prefix") == NULL);

    assert(snj_render_set_view(&render, SNJ_RENDER_ROLLOUT) == 0);
    assert(snj_render_rollout(&render, "live-suffix ", 12u, &delivered) == 0);
    render.verbosity = 3u;
    assert(snj_render_runtime(&render, "queued-runtime") == 0);
    memcpy(event.text, "chat-two", 9u);
    assert(snj_render_irc_event(&render, &event) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strstr(output,
                  "── rollout ──\n• Goal set\n• Compacted\n"
                  "agent › hidden-prefix live-suffix ") != NULL);
    assert(strstr(output, "queued-runtime") == NULL);
    assert(strstr(output, "chat-two") == NULL);

    assert(snj_render_set_view(&render, SNJ_RENDER_CHAT) == 0);
    assert(snj_render_rollout(&render, "hidden-tail", 11u, &delivered) == 0);
    assert(snj_render_rollout_end(&render) == 0);
    assert(snj_render_set_view(&render, SNJ_RENDER_ROLLOUT) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strstr(output, "── chat ──\n") != NULL);
    assert(strstr(output, "chat-two") != NULL);
    assert(strstr(output, "── rollout ──\nhidden-tail\n") != NULL);
    assert(strstr(output, "hidden-tail\nqueued-runtime\n") != NULL);
    assert(count_text(output, "hidden-prefix") == 1u);
    assert(count_text(output, "live-suffix") == 1u);
    assert(count_text(output, "hidden-tail") == 1u);
    assert(count_text(output, "chat-two") == 1u);
    assert(count_text(output, "• Goal set") == 1u);
    assert(count_text(output, "• Compacted") == 1u);
    assert(count_text(output, "── rollout ──") == 2u);
    assert(snj_render_set_view(&render, SNJ_RENDER_ROLLOUT) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(count_text(output, "── rollout ──") == 2u);
    errno = 0;
    assert(snj_render_set_view(&render, (enum snj_render_view)-1) < 0);
    assert(errno == EINVAL);
    render.verbosity = verbosity;
    event.local = true;
    memcpy(event.nick, "agent", 6u);
    memcpy(event.text, "public-before-rename", 21u);
    assert(snj_render_irc_event(&render, &event) == 0);
    memcpy(render.model_nick, "agent2", 7u);
    memcpy(event.nick, "agent2", 7u);
    memcpy(event.text, "public-after-rename", 20u);
    assert(snj_render_irc_event(&render, &event) == 0);
    event.local = false;
    memcpy(event.nick, "agent", 6u);
    memcpy(event.text, "peer-with-old-nick", 19u);
    assert(snj_render_irc_event(&render, &event) == 0);
    assert(snj_render_set_view(&render, SNJ_RENDER_CHAT) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(count_text(output, "agent › public-before-rename") == 1u);
    assert(count_text(output, "agent2 › public-after-rename") == 1u);
    assert(count_text(output, "peer-with-old-nick") == 1u);
    event.historical = true;
    memcpy(event.nick, "agent2", 7u);
    memcpy(event.text, "retained-own-message", 21u);
    assert(snj_render_irc_event(&render, &event) == 0);
    event.historical = false;
    event.local = true;
    event.kind = SNJ_IRC_NOTICE;
    memcpy(event.text, "own-public-notice", 18u);
    assert(snj_render_irc_event(&render, &event) == 0);
    event.local = false;
    event.nick[0] = '\0';
    event.kind = SNJ_IRC_TOPIC;
    memcpy(event.text, "/workspace", 11u);
    assert(snj_render_irc_event(&render, &event) == 0);
    event.kind = SNJ_IRC_HISTORY_READY;
    event.text[0] = '\0';
    assert(snj_render_irc_event(&render, &event) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(count_text(output, "history agent2 › retained-own-message") == 1u);
    assert(count_text(output, "-agent2 - own-public-notice") == 1u);
    assert(strstr(output, "· topic · /workspace\n") != NULL);
    assert(strstr(output, "· history synchronized\n") != NULL);
    assert(snj_render_set_view(&render, SNJ_RENDER_ROLLOUT) == 0);
    assert(snj_render_public_begin(&render, STDERR_FILENO, NULL) == 0);
    errno = 0;
    assert(snj_render_rollout_begin(&render, STDERR_FILENO, NULL) < 0);
    assert(errno == EBUSY);
    assert(snj_render_public_abort(&render) == 0);
    assert(snj_render_rollout_begin(&render, STDERR_FILENO, NULL) == 0);
    assert(snj_render_rollout_abort(&render) == 0);
    assert(snj_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data,
                  "hidden-prefix live-suffix hidden-tail") == 0);
    snj_buf_free(&delivered);
    snj_render_free(&render);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    close(fds[0]);
}

int
main(void)
{
    static const char markdown[] =
        "# **Live** _Markdown_\n"
        "- item with `code` and [docs](https://example.test)\n"
        "> ~~old~~ new\n"
        "````c\nint main(void) { return 0; }\n````\n"
        "~~~text\ntilde fence\n~~~\n"
        "First prose line\ncontinued prose\n\nsecond paragraph\n";
    static const char rendered[] =
        "Live Markdown\n"
        "• item with code and [docs] <https://example.test>\n"
        "│ old new\n"
        "┌─ c\n│ int main(void) { return 0; }\n└─\n"
        "┌─ text\n│ tilde fence\n└─\n"
        "• First prose line\ncontinued prose\n\n• second paragraph\n";
    char output[4096];
    struct snj_render render;
    struct snj_buf delivered;

    assert(setlocale(LC_ALL, "") != NULL);
    assert(setenv("TZ", "UTC0", 1) == 0);
    tzset();
    assert(capture_orientation(false, output, sizeof(output)) > 0u);
    assert(strcmp(output, SNAJPAGENT_IDENTITY
                  " · /work/tree · session id 01234567\n") == 0);
    assert(strstr(output, "model-must-not-appear") == NULL);
    assert(capture_orientation(true, output, sizeof(output)) > 0u);
    assert(strcmp(output, SNAJPAGENT_IDENTITY
                  " · resumed · /work/tree · session id 01234567"
                  " · 3 turns · 2 queued paused\n") == 0);
    assert(strstr(output, "model-must-not-appear") == NULL);
    assert(capture(4u, output, sizeof(output)) == 0u);
    assert(capture(5u, output, sizeof(output)) > 0u);
    assert(count_text(output, "verbosity 5 exposes") == 1u);
    assert(strstr(output, "protocol › request JSON\n{\"x\":1}\n"));
    assert(strstr(output, "protocol › response JSON\n{}\n"));
    assert(!strstr(output, "> POST"));

    assert(capture(6u, output, sizeof(output)) > 0u);
    assert(count_text(output, "verbosity 5 exposes") == 1u);
    assert(strstr(output, "> POST https://example.test\n"));

    test_input_model_boundaries();
    test_model_prompt_boundaries();

    snj_buf_init(&delivered, 1024u);
    assert(capture_wrapped("alpha beta gamm", "a delta", 20u, true,
                           "• alpha beta gamm", "• alpha beta gamma\n  delta",
                           output, sizeof(output), &delivered) > 0u);
    assert(strcmp(output, "• alpha beta gamma\n  delta") == 0);
    assert(snj_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data,
                  "alpha beta gamma delta") == 0);
    snj_buf_free(&delivered);
    snj_buf_init(&delivered, 64u);
    assert(capture_wrapped("123456789012345678 ", "next", 20u, true,
                           "• 123456789012345678",
                           "• 123456789012345678\n  next",
                           output, sizeof(output), &delivered) > 0u);
    assert(snj_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data,
                  "123456789012345678 next") == 0);
    snj_buf_free(&delivered);
    test_punctuation_wrapping();

    snj_buf_init(&delivered, sizeof(markdown));
    assert(capture_markdown(markdown, true, true, SNJ_COLOR_NEVER,
                            output, sizeof(output), &delivered) > 0u);
    assert(strcmp(output, rendered) == 0);
    assert(snj_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data, markdown) == 0);
    snj_buf_free(&delivered);
    assert(capture_markdown(markdown, false, false, SNJ_COLOR_NEVER,
                            output, sizeof(output), NULL) > 0u);
    assert(strcmp(output, markdown) == 0);
    assert(capture_markdown(markdown, true, false, SNJ_COLOR_ALWAYS,
                            output, sizeof(output), NULL) > 0u);
    assert(strstr(output, "\033[0;1;36mLive") != NULL);
    assert(strstr(output, "\033[0;33mcode") != NULL);
    assert(strstr(output, "\033[0;4;34mhttps://example.test") != NULL);
    assert(strstr(output, "\033[0;34;2mold") != NULL);
    assert(capture_markdown("**", true, true, SNJ_COLOR_NEVER,
                            output, sizeof(output), NULL) == strlen("• **"));
    assert(strcmp(output, "• **") == 0);

    assert(capture_static_markdown(0u, output, sizeof(output)) > 0u);
    for (unsigned int verbosity = 1u; verbosity <= 6u; ++verbosity) {
        char other[8192u];

        assert(capture_static_markdown(verbosity, other, sizeof(other)) > 0u);
        assert(strcmp(output, other) == 0);
    }
    assert(strstr(output, "00:00:01 agent › answer and code\n") != NULL);
    assert(strstr(output, "00:00:01 agent › • actual list item\n") != NULL);
    assert(strstr(output, "@operator › **literal operator**\n") != NULL);
    assert(strstr(output, "remote › ┌─ c\n") != NULL);
    assert(strstr(output, "remote › │ int value = 1;\n") != NULL);
    assert(strstr(output, "remote › └─\n") != NULL);
    assert(strstr(output,
                  "00:00:01 -remote - **literal notice**\n") != NULL);
    assert(strstr(output, "remote › plain after quit\n") != NULL);
    assert(strstr(output, "remote › │ plain after quit\n") == NULL);
    assert(strstr(output, "user: **literal user**\n") != NULL);
    assert(strstr(output, "assistant: Saved answer\n") != NULL);
    assert(strstr(output, "remote › **literal agent**\n") != NULL);
    assert(strstr(output, "assistant: ## Literal assistant\n") != NULL);
    test_history_failure();
    test_prompt_history();
    test_prompt_clock();
    test_prompt_spinners();
    test_markdown_streaming();
    test_markdown_tables();
    for (unsigned int verbosity = 0u; verbosity <= 6u; ++verbosity)
        test_append_only_views(verbosity);

    assert(capture_lifecycle(0u, SNJ_COLOR_NEVER,
                             output, sizeof(output)) > 0u);
    assert(strcmp(output,
        "• Compacted\n"
        "• Goal set\n"
        "• Goal set\n"
        "• Goal cleared\n"
        "• Goal cleared\n") == 0);
    assert(capture_lifecycle(4u, SNJ_COLOR_NEVER,
                             output, sizeof(output)) > 0u);
    assert(strstr(output,
                  "• Compacted · event › 1 compaction_completed synced\n"));
    assert(strstr(output,
                  "• Goal cleared · event › 5 goal_cancelled synced\n"));
    assert(strstr(output, "event › 6 turn_completed synced\n"));
    assert(capture_lifecycle(0u, SNJ_COLOR_ALWAYS,
                             output, sizeof(output)) > 0u);
    assert(count_text(output, "\033[1;32m• ") == 5u);
    assert(count_text(output, "\n\033[0m") == 5u);
    assert(capture_resume_hint(SNJ_COLOR_NEVER,
                               output, sizeof(output)) > 0u);
    assert(strcmp(output,
        "• You can resume this session with the following command:\n"
        "'snajpagent' --resume '0123'\n") == 0);
    assert(capture_resume_hint(SNJ_COLOR_ALWAYS,
                               output, sizeof(output)) > 0u);
    assert(strcmp(output,
        "\033[1;32m• You can resume this session with the following command:"
        "\033[0m\n'snajpagent' --resume '0123'\n") == 0);

    snj_render_init(&render, 6u);
    errno = 0;
    assert(snj_render_transport(&render, '>', "bad\rline", 8u) < 0);
    assert(errno == EINVAL);

    assert(capture_color(SNJ_COLOR_ALWAYS, false, 6u, 2500, 0u, 0u,
                         output, sizeof(output)) > 0u);
    assert(strstr(output, "\033[1;36m› \033[0mplain\n") != NULL);
    assert(strstr(output, "\033[1;33m" SNAJPAGENT_NAME
                  ": careful\n\033[0m") != NULL);
    assert(strstr(output, "\033[1;31m" SNAJPAGENT_NAME
                  ": broken\n\033[0m") != NULL);
    assert(strstr(output, "\033[34mstatus\n\033[0m") != NULL);
    assert(strstr(output,
                  "\033[1;32m• Compacted · event › 7 "
                  "compaction_completed synced\n\033[0m") != NULL);
    assert(strstr(output,
                  "\033[33m→ exec\033[0m  timeout=2500ms  'printf plain'\n") != NULL);
    assert(strstr(output, "\033[1;36magent \033[0m› answer") != NULL);
    assert(capture_color(SNJ_COLOR_NEVER, true, 6u, -1, 0u, 0u,
                         output, sizeof(output)) > 0u);
    assert(strchr(output, '\033') == NULL);
    assert(strstr(output, "→ exec  timeout=none  'printf plain'\n") != NULL);
    assert(capture_color(SNJ_COLOR_NEVER, false, 1u, -1, 0u, 0u,
                         output, sizeof(output)) > 0u);
    assert(strstr(output, "→ exec  timeout=none  'printf plain'\n") != NULL);
    assert(strstr(output, "  arguments: {\"command\":\"printf plain\"") != NULL);
    assert(strstr(output, "  output:\nfixture tool output: café\n") != NULL);
    assert(capture_color(SNJ_COLOR_NEVER, true, 1u, -1, 0u, 0u,
                         output, sizeof(output)) > 0u);
    assert(strstr(output, "→ exec  timeout=none  'printf plain'\n") != NULL);
    assert(strstr(output, "  arguments:") != NULL);
    assert(strstr(output, "fixture tool output: café") != NULL);
    assert(capture_color(SNJ_COLOR_NEVER, false, 1u, -1, 4000u, 8u,
                         output, sizeof(output)) > 0u);
    assert(strstr(output,
                  "→ exec  timeout=4000ms  'printf plain'\n") != NULL);
    assert(strstr(output, "  arguments:") != NULL);
    assert(strstr(output, "  output:\nfixture ") != NULL);
    assert(strstr(output, "output bytes hidden by max_output_bytes") != NULL);
    assert(strstr(output, "fixture tool output: café") == NULL);

    assert(setenv("NO_COLOR", "1", 1) == 0);
    snj_render_init(&render, 0u);
    render.stderr_terminal = true;
    snj_render_set_color(&render, SNJ_COLOR_AUTO);
    assert(!render.color_stderr);
    assert(unsetenv("NO_COLOR") == 0);
    puts("test_render: ok");
    return 0;
}
