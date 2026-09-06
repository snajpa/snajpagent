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
term_history_has(const struct snag_history *term, const char *text)
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
    struct snag_history term;
    struct stat st;
    ssize_t got;
    int fd, status;

    assert(mkdir("build", 0700) == 0 || errno == EEXIST);
    assert(realpath("build", build));
    assert(snprintf(temp, sizeof(temp), "%s/test-history-XXXXXX", build) > 0);
    assert(mkdtemp(temp));
    assert(snprintf(path, sizeof(path), "%s/prompt_history", temp) > 0);
    memset(&term, 0, sizeof(term));
    assert(snag_history_open(&term, temp) == 0);
    assert(snag_history_add(&term, sample) == 0);
    assert(snag_history_add(&term, "duplicate") == 0);
    assert(snag_history_add(&term, "duplicate") == 0);
    snag_history_free(&term);
    assert(stat(path, &st) == 0 && (st.st_mode & 0777u) == 0600u);
    fd = open(path, O_RDONLY);
    assert(fd >= 0 && (got = read(fd, bytes, sizeof(bytes) - 1u)) > 0);
    assert(close(fd) == 0);
    bytes[got] = '\0';
    assert(count_text(bytes, "\n") == 3u);
    assert(strstr(bytes, "one\\\\two\\nthree\\r\\t\\x01z\n"));

    fd = open(path, O_WRONLY | O_APPEND);
    assert(fd >= 0 && snag_write_full(fd, "bad\\q\nunfinished", 17u) == 0);
    assert(close(fd) == 0 && chmod(path, 0644) == 0);
    memset(&term, 0, sizeof(term));
    assert(snag_history_open(&term, temp) == 0);
    assert(snag_history_take_warning(&term));
    assert(!snag_history_take_warning(&term));
    assert(term.snapshot.count == 3u);
    assert(strcmp(term.snapshot.items[0], sample) == 0);
    assert(strcmp(term.snapshot.items[1], "duplicate") == 0);
    assert(strcmp(term.snapshot.items[2], "duplicate") == 0);
    assert(stat(path, &st) == 0 && (st.st_mode & 0777u) == 0600u);
    snag_history_free(&term);

    assert(unlink(path) == 0);
    for (unsigned int process = 0u; process < 2u; ++process) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            struct snag_history writer;
            int rc = 0;
            memset(&writer, 0, sizeof(writer));
            if (snag_history_open(&writer, temp) < 0)
                rc = 1;
            for (unsigned int i = 0u; !rc && i < 10u; ++i) {
                (void)snprintf(entry, sizeof(entry), "child-%u-%u", process, i);
                if (snag_history_add(&writer, entry) < 0)
                    rc = 1;
            }
            snag_history_free(&writer);
            _exit(rc);
        }
    }
    for (unsigned int i = 0u; i < 2u; ++i) {
        assert(wait(&status) > 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    memset(&term, 0, sizeof(term));
    assert(snag_history_open(&term, temp) == 0);
    assert(term.snapshot.count == 20u);
    for (unsigned int process = 0u; process < 2u; ++process)
        for (unsigned int i = 0u; i < 10u; ++i) {
            (void)snprintf(entry, sizeof(entry), "child-%u-%u", process, i);
            assert(term_history_has(&term, entry));
        }
    for (unsigned int i = 0u; i < 105u; ++i) {
        (void)snprintf(entry, sizeof(entry), "bounded-%03u", i);
        assert(snag_history_add(&term, entry) == 0);
    }
    assert(term.snapshot.count == SNAG_HISTORY_COUNT);
    assert(strcmp(term.snapshot.items[0], "bounded-005") == 0);
    assert(strcmp(term.snapshot.items[99], "bounded-104") == 0);
    snag_history_free(&term);
    assert(unlink(path) == 0);

    assert(snprintf(subdir, sizeof(subdir), "%s/symlink", temp) > 0);
    assert(mkdir(subdir, 0700) == 0);
    assert(snprintf(path, sizeof(path), "%s/prompt_history", subdir) > 0);
    assert(symlink("../target", path) == 0);
    memset(&term, 0, sizeof(term));
    assert(snag_history_open(&term, subdir) < 0);
    assert(snag_history_take_warning(&term));
    snag_history_free(&term);
    assert(unlink(path) == 0 && rmdir(subdir) == 0);

    if (geteuid() == 0u) {
        assert(snprintf(subdir, sizeof(subdir), "%s/wrong-owner", temp) > 0);
        assert(mkdir(subdir, 0700) == 0);
        assert(snprintf(path, sizeof(path), "%s/prompt_history", subdir) > 0);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0 && close(fd) == 0);
        assert(chown(path, 65534u, 65534u) == 0);
        memset(&term, 0, sizeof(term));
        assert(snag_history_open(&term, subdir) < 0);
        assert(snag_history_take_warning(&term));
        snag_history_free(&term);
        assert(unlink(path) == 0 && rmdir(subdir) == 0);
    }

    assert(snprintf(subdir, sizeof(subdir), "%s/nonregular", temp) > 0);
    assert(mkdir(subdir, 0700) == 0);
    assert(snprintf(path, sizeof(path), "%s/prompt_history", subdir) > 0);
    assert(mkdir(path, 0700) == 0);
    memset(&term, 0, sizeof(term));
    assert(snag_history_open(&term, subdir) < 0);
    snag_history_free(&term);
    assert(rmdir(path) == 0 && rmdir(subdir) == 0);

    assert(snprintf(subdir, sizeof(subdir), "%s/oversized", temp) > 0);
    assert(mkdir(subdir, 0700) == 0);
    assert(snprintf(path, sizeof(path), "%s/prompt_history", subdir) > 0);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)(SNAG_HISTORY_BYTES * 4u +
                                 SNAG_HISTORY_COUNT + 1u)) == 0);
    assert(close(fd) == 0);
    memset(&term, 0, sizeof(term));
    assert(snag_history_open(&term, subdir) < 0);
    snag_history_free(&term);
    assert(unlink(path) == 0 && rmdir(subdir) == 0);
    assert(rmdir(temp) == 0);
}

static void
test_prompt_clock(void)
{
    struct snag_term term;
    char *saved_tz = getenv("TZ") ? strdup(getenv("TZ")) : NULL;

    assert(setenv("TZ", "UTC0", 1) == 0);
    tzset();
    snag_term_init(&term);
    snag_term_capture_prompt_clock(&term, 86399);
    assert(term.prompt_clock.captured && term.prompt_clock.valid);
    assert(term.prompt_clock.hour == 23 && term.prompt_clock.minute == 59 &&
           term.prompt_clock.second == 59);
    snag_term_capture_prompt_clock(&term, 86400);
    assert(term.prompt_clock.hour == 23 && term.prompt_clock.second == 59);
    term.prompt_clock.captured = false;
    snag_term_capture_prompt_clock(&term, 86400);
    assert(term.prompt_clock.valid && term.prompt_clock.hour == 0 &&
           term.prompt_clock.minute == 0 && term.prompt_clock.second == 0);
    term.prompt_clock.captured = false;
    snag_term_capture_prompt_clock(&term, (time_t)-1);
    assert(term.prompt_clock.captured && !term.prompt_clock.valid);
    snag_term_capture_prompt_clock(&term, 0);
    assert(!term.prompt_clock.valid);
    snag_term_close(&term);
    assert((saved_tz ? setenv("TZ", saved_tz, 1) : unsetenv("TZ")) == 0);
    free(saved_tz);
    tzset();
}

static void
test_prompt_spinners(void)
{
    struct snag_term term;
    const char prompt[] = {'x', (char)0xfd, (char)0xfe, '>', '\0'};
    char oversized[SNAG_TERM_LABEL_BYTES];
    const char *spinners[SNAG_TERM_SPINNER_COUNT] = {"\\0◆", " |/-", "\\0"};
    const char *bad[SNAG_TERM_SPINNER_COUNT] = {"\x80", " ", " "};
    const char *wide[SNAG_TERM_SPINNER_COUNT] = {" 😀", " ", " "};
    char saved[SNAG_TERM_LABEL_BYTES];

    snag_term_init(&term);
    assert(snag_term_set_prompt_template(&term, false, prompt, spinners, 8u, 0u)
           == 0);
    assert(strcmp(term.label, "x >") == 0);
    assert(term.spinner[SNAG_TERM_SPINNER_GOAL].inactive_len == 0u);
    assert(term.spinner[SNAG_TERM_SPINNER_PROVIDER].inactive_len == 1u);
    assert(term.spinner[SNAG_TERM_SPINNER_TOOL].inactive_len == 0u);
    assert(snag_term_set_spinner_states(&term,
        1u << SNAG_TERM_SPINNER_GOAL) == 0);
    assert(strcmp(term.label, "x◆ >") == 0);
    assert(snag_term_set_spinner_states(&term,
        1u << SNAG_TERM_SPINNER_PROVIDER) == 0);
    assert(strcmp(term.label, "x|>") == 0);
    memcpy(saved, term.label, sizeof(saved));
    assert(snag_term_set_prompt_template(&term, false, prompt, bad, 8u, 0u) < 0);
    assert(memcmp(saved, term.label, sizeof(saved)) == 0);
    assert(term.spinner_states == (1u << SNAG_TERM_SPINNER_PROVIDER));
    assert(snag_term_set_prompt_template(&term, false, prompt, spinners, 8u,
                                        1u << SNAG_TERM_SPINNER_COUNT) < 0);
    memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized) - 3u] = (char)SNAG_TERM_SPINNER_MARKER_BASE;
    oversized[sizeof(oversized) - 2u] = '\0';
    assert(snag_term_set_prompt_template(&term, false, oversized, wide, 8u,
                                        0u) < 0);
    assert(memcmp(saved, term.label, sizeof(saved)) == 0);
    {
        const char padded[] = "\xfd\xfe  9%> ";
        const char *stable[] = {" ⚑", " P", " ⠋"};
        const char *compact[] = {"\\0◆", "\\0P", "\\0T"};
        const char *blank[] = {" ", "\\0 P", "\\0"};

        for (unsigned int state = 0u; state < 8u; ++state) {
            assert(snag_term_set_prompt_template(&term, true, padded, stable,
                                                8u, state) == 0);
            char expected[32];

            assert(snprintf(expected, sizeof(expected), "%s%s  9%%> ",
                state & 1u ? "⚑" : " ",
                state & 4u ? "⠋" : state & 2u ? "P" : " ") > 0);
            assert(strcmp(term.label, expected) == 0);
            assert(snag_term_text_width(term.label, strlen(term.label)) == 8u);
            assert(snag_term_set_spinner_states(&term, 2u) == 0);
            assert(strcmp(term.label, " P  9%> ") == 0);
            assert(snag_term_set_spinner_states(&term, 6u) == 0);
            assert(strcmp(term.label, " ⠋  9%> ") == 0);
            assert(snag_term_set_spinner_states(&term, 0u) == 0);
            assert(strcmp(term.label, "    9%> ") == 0);
        }
        assert(snag_term_set_prompt_template(&term, true, padded, compact,
                                            8u, 0u) == 0);
        assert(strcmp(term.label, "  9%> ") == 0);
        assert(snag_term_set_spinner_states(&term, 2u) == 0);
        assert(strcmp(term.label, "P  9%> ") == 0);
        assert(snag_term_set_spinner_states(&term, 4u) == 0);
        assert(strcmp(term.label, "T  9%> ") == 0);
        assert(snag_term_set_prompt_template(&term, true, padded, blank,
                                            8u, 2u) == 0);
        assert(strcmp(term.label, "    9%> ") == 0);
        assert(snag_term_set_spinner_states(&term, 0u) == 0);
        assert(strcmp(term.label, "   9%> ") == 0);
        assert(snag_term_set_prompt_template(&term, true, "  9%> ", compact,
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
        assert(snag_term_set_prompt_template(&term, false, oversized, frames, 8u, 0u) < 0);
        assert(snag_term_set_prompt_template(&term, true, "\xfd\xfe  9%> ",
                                            frames, 8u, 2u) == 0);
        assert(snag_buf_append(&term.draft, "draft", 5u) == 0);
        term.cursor = term.draft.len;
        term.columns = 80u;
        term.opened = term.capable = true;
        assert(pipe(fds) == 0);
        saved_fd = dup(STDERR_FILENO);
        assert(saved_fd >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
        close(fds[1]);
        assert(snag_term_set_prompt_template(&term, true, "\xfd\xfe  9%> ",
                                            frames, 8u, 2u) == 0);
        assert(read(fds[0], output, sizeof(output)) > 0);
        assert(snag_term_set_spinner_states(&term, 6u) == 0);
        assert(snag_term_set_spinner_states(&term, 2u) == 0);
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
    snag_term_close(&term);
}

static size_t
prompt_output(int fd, char *output, size_t size)
{
    ssize_t n = read(fd, output, size - 1u);
    assert(n >= 0 || errno == EAGAIN);
    size_t len = n > 0 ? (size_t)n : 0u;
    output[len] = '\0';
    return len;
}

static void
test_retained_prompt(void)
{
    struct snag_term term;
    const char *frames[SNAG_TERM_SPINNER_COUNT] = {" ⚑", " |/-", " ⠋⠙"};
    char output[4096];
    int fds[2], saved_fd;

    snag_term_init(&term);
    term.opened = term.capable = true;
    term.columns = 24u;
    assert(pipe(fds) == 0);
    assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    saved_fd = dup(STDERR_FILENO);
    assert(saved_fd >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    assert(snag_term_set_prompt_label(&term, true, "  9%> ") == 0);
    assert(snag_term_restore_draft(&term, "first row stays\nsecond row stays") == 0);
    assert(prompt_output(fds[0], output, sizeof(output)) > 0u);
    assert(snag_term_set_prompt_label(&term, true, "  9%> ") == 0);
    assert(prompt_output(fds[0], output, sizeof(output)) == 0u);
    size_t cursor_row = term.rendered_cursor_row, cursor_col = term.rendered_cursor_col;
    assert(snag_term_set_prompt_label(&term, true, " 10%> ") == 0);
    assert(prompt_output(fds[0], output, sizeof(output)) > 0u);
    assert(strstr(output, "10") && !strstr(output, "stays") && !strchr(output, '\n'));
    assert(!strstr(output, "\033[K") && !strstr(output, "\033[2K"));
    assert(term.rendered_cursor_row == cursor_row && term.rendered_cursor_col == cursor_col);
    assert(snag_term_restore_draft(&term, "first row stays\nsecond row short") == 0);
    assert(prompt_output(fds[0], output, sizeof(output)) > 0u);
    assert(!strstr(output, "first") && !strstr(output, "10%"));
    assert(snag_term_restore_draft(&term, "small") == 0);
    assert(prompt_output(fds[0], output, sizeof(output)) > 0u);
    assert(strstr(output, "\033[K") && !strstr(output, "\033[2K"));
    assert(term.rendered_rows == 1u && term.rendered_cursor_col == 11u);
    assert(snag_term_restore_draft(&term, "invalid:\xff") == 0);
    assert(prompt_output(fds[0], output, sizeof(output)) > 0u);
    assert(strstr(output, "invalid:\\xFF"));
    assert(snag_term_set_prompt_label(&term, true, "two\nlines> ") == 0);
    assert(prompt_output(fds[0], output, sizeof(output)) > 0u);
    assert(strstr(output, "two\\nlines> "));
    assert(snag_term_set_prompt_label(&term, true, " 10%> ") == 0);
    (void)prompt_output(fds[0], output, sizeof(output));
    char multiline[141];
    memset(multiline, '\n', sizeof(multiline) - 1u);
    multiline[sizeof(multiline) - 1u] = '\0';
    assert(snag_term_restore_draft(&term, multiline) == 0);
    (void)prompt_output(fds[0], output, sizeof(output));
    assert(snag_term_restore_draft(&term, "") == 0);
    assert(prompt_output(fds[0], output, sizeof(output)) > 0u);
    assert(term.rendered_rows == 1u);

    assert(snag_term_restore_draft(&term, "café界 tail") == 0);
    (void)prompt_output(fds[0], output, sizeof(output));
    assert(snag_term_restore_draft(&term, "cafè界 tail") == 0);
    assert(prompt_output(fds[0], output, sizeof(output)) > 0u);
    assert(strstr(output, "è") && !strstr(output, "caf") && !strstr(output, "tail"));
    assert(snag_term_restore_draft(&term, "cafè語 tail") == 0);
    assert(prompt_output(fds[0], output, sizeof(output)) > 0u);
    assert(strstr(output, "語") && !strstr(output, "tail"));
    assert(snag_term_restore_draft(&term, "aaaaaaaaaaaaaaaaa界") == 0);
    assert(term.rendered_rows == 2u && term.rendered_cursor_col == 2u);
    assert(snag_term_restore_draft(&term, "aaaaaaaaaaaaaaaaaa\nnext") == 0);
    assert(term.rendered_rows == 2u && term.rendered_cursor_col == 10u);
    assert(snag_term_restore_draft(&term, "cafè語 tail") == 0);
    (void)prompt_output(fds[0], output, sizeof(output));
    snag_term_set_color(&term, true);
    assert(snag_term_set_prompt_label(&term, true, " 10%> ") == 0);
    assert(prompt_output(fds[0], output, sizeof(output)) > 0u);
    assert(strstr(output, "\033[1;36m") && !strstr(output, "tail"));
    snag_term_set_color(&term, false);
    assert(snag_term_set_prompt_label(&term, true, " 10%> ") == 0);
    assert(prompt_output(fds[0], output, sizeof(output)) > 0u);
    assert(!strstr(output, "tail"));

    assert(snag_term_restore_draft(&term, "") == 0);
    assert(snag_term_set_prompt_template(&term, true, "\xfd\xfe> ", frames, 8u, 2u) == 0);
    (void)prompt_output(fds[0], output, sizeof(output));
    term.spinner_epoch_ms -= 125u;
    uint64_t epoch = term.spinner_epoch_ms;
    assert(snag_term_set_prompt_template(&term, true, "\xfd\xfe> ", frames, 8u, 2u) == 0);
    assert(term.spinner_epoch_ms == epoch);
    assert(prompt_output(fds[0], output, sizeof(output)) > 0u);
    assert(strchr(output, '/') && !strchr(output, '>'));
    assert(snag_term_set_prompt_template(&term, true, "\xfd\xfe> ", frames, 8u, 2u) == 0);
    assert(prompt_output(fds[0], output, sizeof(output)) == 0u);

    assert(snag_term_output_begin(&term, true) == 0);
    assert(snag_term_note_output(&term, "public", 6u, "") == 0);
    assert(snag_term_output_end(&term) == 0);
    (void)prompt_output(fds[0], output, sizeof(output));
    assert(!term.prompt_visible && term.painted_prompt.len == 0u);
    enum snag_term_action action;
    char *text;
    int input[2], stdin_fd = dup(STDIN_FILENO);
    assert(stdin_fd >= 0 && pipe(input) == 0 && dup2(input[0], STDIN_FILENO) >= 0);
    assert(snag_term_poll(&term, 20, -1, &action, &text) == 0);
    assert(!term.prompt_visible && prompt_output(fds[0], output, sizeof(output)) == 0u);
    term.input[0] = 'x';
    term.input_len = 1u;
    assert(snag_term_poll(&term, 0, -1, &action, &text) == 0);
    assert(term.prompt_visible && prompt_output(fds[0], output, sizeof(output)) > 0u);
    assert(snag_term_output_begin(&term, true) == 0);
    assert(snag_term_note_output(&term, "more", 4u, "") == 0);
    assert(snag_term_output_end(&term) == 0);
    (void)prompt_output(fds[0], output, sizeof(output));
    term.last_output_ms -= 200u;
    assert(snag_term_poll(&term, 20, -1, &action, &text) == 0);
    assert(term.prompt_visible && prompt_output(fds[0], output, sizeof(output)) > 0u);
    assert(snag_term_set_prompt_label(&term, false, "cursor> ") == 0);
    assert(snag_term_restore_draft(&term, "unchanged") == 0);
    (void)prompt_output(fds[0], output, sizeof(output));
    const char *moves[] = {"\033[H", "\033[F", "\033[D"};
    const size_t columns[] = {8u, 17u, 16u};
    for (size_t i = 0u; i < 3u; ++i) {
        memcpy(term.input, moves[i], 3u);
        term.input_pos = 0u;
        term.input_len = 3u;
        assert(snag_term_poll(&term, 0, -1, &action, &text) == 0);
        assert(term.rendered_cursor_col == columns[i] && term.rendered_cursor_row == 0u);
        assert(prompt_output(fds[0], output, sizeof(output)) > 0u);
        assert(!strstr(output, "unchanged") && !strchr(output, '>'));
        assert(!strstr(output, "\033[K") && !strstr(output, "\033[2K"));
    }
    assert(dup2(stdin_fd, STDIN_FILENO) >= 0);
    close(stdin_fd);
    close(input[0]);
    close(input[1]);
    int readonly = open("/dev/null", O_RDONLY);
    assert(readonly >= 0 && dup2(readonly, STDERR_FILENO) >= 0);
    close(readonly);
    assert(snag_term_set_prompt_label(&term, true, "failure> ") < 0);
    assert(term.painted_prompt.len == 0u);
    assert(dup2(saved_fd, STDERR_FILENO) >= 0);
    close(saved_fd);
    close(fds[0]);
    term.opened = false;
    snag_term_close(&term);
}

static void
test_mention_completion(void)
{
    static const struct {
        const char *nicks, *draft, *expected;
        size_t cursor, result_cursor;
    } cases[] = {
        {"agent\n", "@ag", "@agent ", 3u, 7u},
        {"Agent\nagent\n", "hey @AG", "hey @Agent ", 7u, 11u},
        {"agent1\nagent2\n", "@ag", "@agent", 3u, 6u},
        {"agent1\nagent2\n", "@agent", "@agent", 6u, 6u},
        {"agent\n", "@missing", "@missing", 8u, 8u},
        {"", "@", "@", 1u, 1u},
        {"agent\n", "hi @agxxx, bye", "hi @agent , bye", 6u, 10u},
        {"agent\n", "hey\n@ag tail", "hey\n@agent tail", 7u, 11u},
        {"agent\n", "@agent", "@agent ", 6u, 7u},
        {"[bot]\n", "@{bo", "@[bot] ", 4u, 7u},
        {"čenda\n", "@če", "@čenda ", 4u, 8u},
        {"čenda\nčerven\n", "@č", "@če", 3u, 4u},
        {"ač\naď\n", "@a", "@a", 2u, 2u},
    };

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        for (unsigned int active = 0u; active < 2u; ++active) {
            struct snag_term term;
            enum snag_term_action action;
            char *text = NULL;

            snag_term_init(&term);
            term.chat = true;
            term.active = active != 0u;
            term.nicks = snag_strdup_checked(cases[i].nicks, 4096u);
            assert(term.nicks);
            assert(snag_buf_append(&term.draft, cases[i].draft,
                                  strlen(cases[i].draft)) == 0);
            term.cursor = cases[i].cursor;
            term.input[0] = '\t';
            term.input_len = 1u;
            assert(snag_term_poll(&term, 0, -1, &action, &text) == 0);
            assert(action == SNAG_TERM_NONE && !text);
            assert(term.draft.len == strlen(cases[i].expected));
            assert(memcmp(term.draft.data, cases[i].expected, term.draft.len) == 0);
            assert(term.cursor == cases[i].result_cursor);
            snag_term_close(&term);
        }
    }
}

static void
completion_input(struct snag_term *term, const char *bytes)
{
    enum snag_term_action action;
    char *text = NULL;
    term->input_pos = 0u;
    term->input_len = strlen(bytes);
    assert(term->input_len <= sizeof(term->input));
    memcpy(term->input, bytes, term->input_len);
    assert(snag_term_poll(term, 0, -1, &action, &text) == 0);
    assert(action == SNAG_TERM_NONE && !text);
}

static void
test_completion_choices(void)
{
    static const struct snag_term_command commands[] = {
        {"/connect ENDPOINT", NULL}, {"/config FILE", NULL},
        {"/compact", NULL}, {"/help", NULL}, {"/help", NULL}
    };
    static const struct {
        const char *draft, *common, *first, *second;
    } cases[] = {
        {"/c", "/co", "/connect", "/config"},
        {"/1", "/1", "/12", "/17"},
        {"@ag", "@agent", "@agent1", "@agent2"},
        {"/17 @ag", "/17 @Agent", "@Agent1", "@agent2"},
        {"/all @ag", "/all @agent", "@agent1", "@agent2"},
        {"@č", "@če", "@čenda", "@červen"},
    };
    int fds[2], saved = dup(STDERR_FILENO);
    assert(saved >= 0 && pipe(fds) == 0);
    assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    assert(dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    for (unsigned int flags = 0u; flags < 4u; ++flags) {
        for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            struct snag_term term;
            char output[4096];
            struct snag_irc_destinations destinations = {.count = 2u, .items = {
                {.target = {.id = 12u}, .joined = true,
                 .nicks = "agent1\nagent2\nčenda\nčerven\n"},
                {.target = {.id = 17u}, .joined = true,
                 .nicks = "Agent1\nagent2\n"},
            }};
            snag_term_init(&term);
            term.chat = true;
            term.active = (flags & 1u) != 0u;
            term.columns = flags & 2u ? 10u : 80u;
            snag_term_set_commands(&term, commands, sizeof(commands) / sizeof(commands[0]));
            assert(snag_term_set_destinations(&term, &destinations) == 0);
            assert(snag_term_restore_draft(&term, cases[i].draft) == 0);
            completion_input(&term, "\t");
            assert(term.draft.len == strlen(cases[i].common));
            assert(memcmp(term.draft.data, cases[i].common, term.draft.len) == 0);
            assert(prompt_output(fds[0], output, sizeof(output)) == 0u);
            size_t cursor = term.cursor;
            completion_input(&term, "\t");
            assert(prompt_output(fds[0], output, sizeof(output)) > 0u);
            assert(strstr(output, cases[i].first) && strstr(output, cases[i].second));
            assert(count_text(output, cases[i].first) == 1u);
            assert(!strstr(output, "ENDPOINT") && !strstr(output, "FILE"));
            assert(term.cursor == cursor && term.draft.len == strlen(cases[i].common));
            assert(memcmp(term.draft.data, cases[i].common, term.draft.len) == 0);
            /* Cursor movement breaks the consecutive-Tab sequence. */
            completion_input(&term, "\033[D\033[C\t");
            assert(prompt_output(fds[0], output, sizeof(output)) == 0u);
            /* Capture while output is stalled; listing is deferred, not lost. */
            term.input_only = true;
            completion_input(&term, "\t");
            assert(term.completion_output.len && prompt_output(fds[0], output, sizeof(output)) == 0u);
            term.input_only = false;
            completion_input(&term, "x");
            assert(prompt_output(fds[0], output, sizeof(output)) > 0u);
            assert(strstr(output, cases[i].first) && strstr(output, cases[i].second));
            assert(!term.completion_output.len);
            snag_term_close(&term);
        }
    }
    struct snag_term term;
    snag_term_init(&term);
    snag_term_set_commands(&term, commands, sizeof(commands) / sizeof(commands[0]));
    const char *drafts[] = {"/he", "/help", "/he tail"};
    for (size_t i = 0u; i < 3u; ++i) {
        assert(snag_term_restore_draft(&term, drafts[i]) == 0);
        term.cursor = i == 1u ? 5u : 3u;
        completion_input(&term, "\t");
        const char *expected = i == 2u ? "/help tail" : "/help ";
        assert(term.draft.len == strlen(expected));
        assert(memcmp(term.draft.data, expected, term.draft.len) == 0);
        assert(term.cursor == 6u);
        assert(!term.completion_armed);
    }
    const char *submissions[] = {
        "/help ", "/17 ", "/model value ", "ordinary ", "//help ", "/help\n "
    };
    const char *submitted[] = {
        "/help", "/17", "/model value ", "ordinary ", "//help ", "/help\n "
    };
    for (size_t i = 0u; i < sizeof(submissions) / sizeof(submissions[0]); ++i) {
        enum snag_term_action action;
        char *text = NULL;
        assert(snag_term_restore_draft(&term, submissions[i]) == 0);
        term.input[0] = '\r';
        term.input_len = 1u;
        assert(snag_term_poll(&term, 0, -1, &action, &text) == 1);
        assert(action == SNAG_TERM_SUBMIT && text && strcmp(text, submitted[i]) == 0);
        free(text);
        term.input_pos = term.input_len = 0u;
    }
    snag_term_close(&term);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    close(fds[0]);
}

static void
test_destination_editor(void)
{
    static const struct {
        const char *draft, *expected;
        bool chat;
    } cases[] = {
        {"@ag", "@agent2 ", true},
        {"/17 @ag", "/17 @agent17 ", true},
        {"/17 @ag", "/17 @agent17 ", false},
        {"/all @ag", "/all @agent", true},
        {"/9 @ag", "/9 @ag", true},
        {"/2oops @ag", "/2oops @ag", true},
        {"/1", "/17 ", true}
    };
    struct snag_irc_destinations destinations = {0};
    struct snag_irc_route route, frozen;
    struct snag_term term;

    destinations.count = 2u;
    destinations.items[0].target = (struct snag_irc_target){2u, 1u};
    destinations.items[1].target = (struct snag_irc_target){17u, 1u};
    destinations.items[0].joined = destinations.items[1].joined = true;
    strcpy(destinations.items[0].room, "#one");
    strcpy(destinations.items[1].room, "#two");
    strcpy(destinations.items[0].nicks, "agent2\n");
    strcpy(destinations.items[1].nicks, "agent17\n");
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        enum snag_term_action action;
        char *text = NULL;
        snag_term_init(&term);
        term.chat = cases[i].chat;
        term.active = true;
        assert(snag_term_set_destinations(&term, &destinations) == 0);
        assert(term.destination.id == 2u);
        assert(snag_term_restore_draft(&term, cases[i].draft) == 0);
        term.input[0] = '\t';
        term.input_len = 1u;
        assert(snag_term_poll(&term, 0, -1, &action, &text) == 0);
        assert(action == SNAG_TERM_NONE && !text);
        assert(term.draft.len == strlen(cases[i].expected));
        assert(memcmp(term.draft.data, cases[i].expected, term.draft.len) == 0);
        assert(term.destination.id == 2u);
        snag_term_close(&term);
    }
    snag_term_init(&term);
    term.chat = true;
    assert(snag_term_set_destinations(&term, &destinations) == 0);
    char label[128u];
    snag_term_destination_prefix(&term, label, sizeof(label));
    assert(strcmp(label, "[2 #one] ") == 0);
    snag_term_destination_route(&term, "hello", &route);
    assert(route.count == 1u && route.targets[0].id == 2u);
    snag_term_destination_route(&term, "/17 hello", &route);
    assert(route.count == 1u && route.targets[0].id == 17u);
    assert(term.destination.id == 2u);
    snag_term_destination_route(&term, "/all hello", &frozen);
    assert(frozen.count == 2u);
    assert(snag_term_select_destination(&term, 17u) == 0);
    assert(snag_term_select_destination(&term, 9u) < 0);
    assert(snag_term_restore_draft(&term, "keep me") == 0);
    destinations.count = 1u;
    assert(snag_term_set_destinations(&term, &destinations) == 0);
    assert(term.destination.id == 17u && term.draft.len == 7u);
    snag_term_destination_prefix(&term, label, sizeof(label));
    assert(strcmp(label, "[17 unavailable] ") == 0);
    snag_term_destination_route(&term, "keep me", &route);
    assert(route.count == 1u && route.targets[0].id == 17u);
    assert(frozen.count == 2u && frozen.targets[1].id == 17u);
    snag_term_destination_route(&term, "/17 no", &route);
    assert(route.count == 0u);
    snag_term_destination_route(&term, "/all hello", &route);
    assert(route.count == 1u && route.targets[0].id == 2u);
    assert(snag_term_select_destination(&term, 2u) == 0);
    snag_term_destination_prefix(&term, label, sizeof(label));
    assert(!label[0]);
    destinations.items[0].joined = false;
    assert(snag_term_set_destinations(&term, &destinations) == 0);
    snag_term_destination_prefix(&term, label, sizeof(label));
    assert(strcmp(label, "[2 connecting] ") == 0);
    assert(snag_term_select_destination(&term, 2u) == 0);
    destinations.count = 0u;
    assert(snag_term_set_destinations(&term, &destinations) == 0);
    assert(term.destination.id == 2u);
    snag_term_destination_route(&term, "/all hello", &route);
    assert(route.count == 0u);
    snag_term_close(&term);
    snag_term_init(&term);
    assert(snag_term_restore_draft(&term, "/17") == 0);
    term.local_backlog = true;
    term.input[0] = '\r';
    term.input_len = 1u;
    enum snag_term_action action;
    char *text = NULL;
    assert(snag_term_poll(&term, 0, -1, &action, &text) == 0);
    assert(action == SNAG_TERM_NONE && !text && term.draft.len == 3u);
    snag_term_close(&term);
}

static size_t
capture(unsigned int verbosity, char *out, size_t out_size)
{
    int fds[2];
    int saved;
    struct snag_render render;
    ssize_t n;
    size_t used = 0u;

    assert(pipe(fds) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0);
    assert(dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snag_render_init(&render, verbosity);
    assert(snag_render_protocol(&render, "request JSON", "{\"x\":1}", 7u) == 0);
    assert(snag_render_protocol(&render, "response JSON", "{}", 2u) == 0);
    assert(snag_render_transport(&render, '>', "POST https://example.test", 25u) == 0);
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
    struct snag_render render;
    struct snag_session session = {0};
    int fds[2];
    int saved;
    ssize_t n;
    size_t used = 0u;

    assert(pipe(fds) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snag_render_init(&render, 0u);
    memcpy(session.id, "0123456789abcdef0123456789abcdef",
           sizeof(session.id));
    assert(snprintf(session.default_model, sizeof(session.default_model),
                    "model-must-not-appear") > 0);
    session.workspace = "/work/tree";
    session.turn_count = 3u;
    session.pending_queue_count = 2u;
    assert(snag_render_orientation(&render, session.workspace, session.id,
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
                struct snag_buf *delivered)
{
    struct snag_render render;
    struct snag_term term;
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
    snag_term_init(&term);
    term.columns = columns;
    snag_render_init(&render, 0u);
    render.stdout_terminal = true;
    snag_render_set_markdown(&render, markdown);
    snag_render_attach_term(&render, &term);
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snag_render_public(&render, first, strlen(first), delivered) == 0);
    used = drain_available(fds[0], out, out_size, used);
    assert(strcmp(out, first_output) == 0);
    assert(snag_render_public(&render, second, strlen(second), delivered) == 0);
    used = drain_available(fds[0], out, out_size, used);
    assert(strcmp(out, second_output) == 0);
    assert(snag_render_public_end(&render) == 0);
    assert(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    while ((n = read(fds[0], out + used, out_size - used - 1u)) > 0)
        used += (size_t)n;
    assert(n == 0);
    close(fds[0]);
    out[used] = '\0';
    snag_term_close(&term);
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
            struct snag_buf delivered;

            assert(snprintf(second, sizeof(second), "%smores",
                            punctuation[i]) > 0);
            assert(snprintf(second_output, sizeof(second_output),
                            "%s%s%s\n%smores", prefix, first,
                            punctuation[i], enabled ? "  " : "") > 0);
            assert(snprintf(delivered_output, sizeof(delivered_output),
                            "%s%smores", first, punctuation[i]) > 0);
            snag_buf_init(&delivered, sizeof(delivered_output));
            assert(capture_wrapped(first, second, 20u, enabled != 0u,
                                   first_output, second_output, output,
                                   sizeof(output), &delivered) > 0u);
            assert(snag_buf_terminate(&delivered) == 0);
            assert(strcmp((const char *)delivered.data, delivered_output) == 0);
            snag_buf_free(&delivered);
        }
        {
            struct snag_buf delivered;

            assert(snprintf(first_output, sizeof(first_output),
                            "%s1234567890 ", prefix) > 0);
            assert(snprintf(second_output, sizeof(second_output),
                            "%s1234567890 \n%s-something", prefix,
                            enabled ? "  " : "") > 0);
            snag_buf_init(&delivered, 32u);
            assert(capture_wrapped("1234567890 ", "-something", 20u,
                                   enabled != 0u, first_output, second_output,
                                   output, sizeof(output), &delivered) > 0u);
            assert(snag_buf_terminate(&delivered) == 0);
            assert(strcmp((const char *)delivered.data,
                          "1234567890 -something") == 0);
            snag_buf_free(&delivered);
        }
    }
}

static size_t capture_markdown_width(const char *text, bool enabled,
                                     bool split, enum snag_color_mode color,
                                     unsigned int columns, char *out,
                                     size_t out_size,
                                     struct snag_buf *delivered);

static size_t
capture_markdown(const char *text, bool enabled, bool split,
                 enum snag_color_mode color, char *out, size_t out_size,
                 struct snag_buf *delivered)
{
    return capture_markdown_width(text, enabled, split, color, 120u, out,
                                  out_size, delivered);
}

static size_t
capture_markdown_width(const char *text, bool enabled, bool split,
                       enum snag_color_mode color, unsigned int columns,
                       char *out, size_t out_size, struct snag_buf *delivered)
{
    struct snag_render render;
    struct snag_term term;
    size_t len = strlen(text);
    size_t used = 0u;
    ssize_t n;
    int fds[2];
    int saved;

    assert(pipe(fds) == 0);
    saved = dup(STDOUT_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDOUT_FILENO) >= 0);
    close(fds[1]);
    snag_term_init(&term);
    term.columns = columns;
    snag_render_init(&render, 0u);
    render.stdout_terminal = true;
    snag_render_set_color(&render, color);
    snag_render_set_markdown(&render, enabled);
    snag_render_attach_term(&render, &term);
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    if (split) {
        for (size_t i = 0u; i < len; ++i)
            assert(snag_render_public(&render, text + i, 1u, delivered) == 0);
    } else {
        assert(snag_render_public(&render, text, len, delivered) == 0);
    }
    assert(snag_render_public_end(&render) == 0);
    assert(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    while ((n = read(fds[0], out + used, out_size - used - 1u)) > 0)
        used += (size_t)n;
    assert(n == 0);
    close(fds[0]);
    out[used] = '\0';
    snag_term_close(&term);
    return used;
}

static size_t
capture_prompt_boundary(const char *text, bool markdown,
                        char *out, size_t out_size)
{
    struct snag_render render;
    struct snag_term term;
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
    snag_term_init(&term);
    term.columns = 120u;
    snag_render_init(&render, 0u);
    render.stdout_terminal = true;
    render.stderr_terminal = true;
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    snag_render_set_markdown(&render, markdown);
    snag_render_attach_term(&render, &term);
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snag_render_public(&render, text, strlen(text), NULL) == 0);
    assert(snag_render_public_end(&render) == 0);
    assert(snag_render_before_prompt(&render) == 0);
    assert(snag_render_before_prompt(&render) == 0);
    snag_render_free(&render);
    snag_term_close(&term);
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
                            "%s\n\n", cases[i].rendered) > 0);
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
            struct snag_render render;
            struct snag_term term;
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
            snag_term_init(&term);
            term.columns = 120u;
            snag_render_init(&render, 0u);
            render.stdout_terminal = true;
            render.stderr_terminal = true;
            snag_render_set_color(&render, SNAG_COLOR_NEVER);
            snag_render_set_markdown(&render, enabled != 0u);
            snag_render_attach_term(&render, &term);
            assert(snprintf(question, sizeof(question), "question%s",
                            suffixes[suffix]) > 0);
            assert(snag_render_input_submitted(&render, "model/low › ",
                                              question) == 0);
            assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
            assert(snag_render_public(&render, "answer", 6u, NULL) == 0);
            assert(snag_render_public_end(&render) == 0);
            assert(snag_render_before_prompt(&render) == 0);
            assert(snag_render_before_prompt(&render) == 0);
            snag_render_free(&render);
            snag_term_close(&term);
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
                          "model/low › question\n\n• answer\n\n" :
                          "model/low › question\n\nanswer\n\n") == 0);
        }
    }
    {
        struct snag_render render;
        struct snag_term term;
        int fds[2];
        int saved;
        ssize_t n;
        size_t used = 0u;

        assert(pipe(fds) == 0);
        saved = dup(STDERR_FILENO);
        assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
        close(fds[1]);
        snag_term_init(&term);
        memcpy(term.label, "model/low › ", strlen("model/low › ") + 1u);
        term.line_submission_echoed = true;
        snag_render_init(&render, 0u);
        render.stderr_terminal = true;
        snag_render_attach_term(&render, &term);
        assert(snag_render_input_submitted(&render, "model/low › ",
                                          "question") == 0);
        snag_render_free(&render);
        snag_term_close(&term);
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
    struct snag_render render;
    struct snag_irc_event event;
    struct snag_session session;
    size_t used = 0u;
    ssize_t n;
    int fds[2];
    int saved;

    assert(pipe(fds) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snag_render_init(&render, verbosity);
    render.stderr_terminal = true;
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
    memset(&event, 0, sizeof(event));
    event.kind = SNAG_IRC_MESSAGE;
    event.timestamp_ms = 1000u;
    memcpy(event.endpoint, "local", 6u);
    memcpy(event.nick, "agent", 6u);
    memcpy(event.text, "**answer** and `code`", 22u);
    event.local = true;
    assert(snag_render_irc_event(&render, &event) == 0);
    memcpy(event.text, "- actual list item", 19u);
    assert(snag_render_irc_event(&render, &event) == 0);
    memcpy(event.nick, "operator", 9u);
    memcpy(event.text, "**literal operator**", 21u);
    event.op = true;
    assert(snag_render_irc_event(&render, &event) == 0);
    event.op = false;
    memcpy(event.nick, "remote", 7u);
    memcpy(event.text, "```c", 5u);
    assert(snag_render_irc_event(&render, &event) == 0);
    memcpy(event.text, "int value = 1;", 15u);
    assert(snag_render_irc_event(&render, &event) == 0);
    memcpy(event.text, "```", 4u);
    assert(snag_render_irc_event(&render, &event) == 0);
    event.kind = SNAG_IRC_NOTICE;
    memcpy(event.text, "**literal notice**", 19u);
    assert(snag_render_irc_event(&render, &event) == 0);
    event.kind = SNAG_IRC_MESSAGE;
    memcpy(event.text, "```c", 5u);
    assert(snag_render_irc_event(&render, &event) == 0);
    event.kind = SNAG_IRC_QUIT;
    memcpy(event.text, "gone", 5u);
    assert(snag_render_irc_event(&render, &event) == 0);
    event.kind = SNAG_IRC_MESSAGE;
    memcpy(event.text, "plain after quit", 17u);
    assert(snag_render_irc_event(&render, &event) == 0);
    memset(&session, 0, sizeof(session));
    session.last_user = "**literal user**";
    session.last_assistant = "## Saved *answer*";
    assert(snag_render_history(&render, session.last_user, session.last_assistant) == 0);
    snag_render_set_markdown(&render, false);
    event.kind = SNAG_IRC_MESSAGE;
    memcpy(event.text, "**literal agent**", 18u);
    assert(snag_render_irc_event(&render, &event) == 0);
    session.last_user = NULL;
    session.last_assistant = "## Literal assistant";
    assert(snag_render_history(&render, session.last_user, session.last_assistant) == 0);
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
    struct snag_render render;
    struct snag_session session = {0};
    int fds[2];
    int saved;

    assert(pipe(fds) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snag_render_init(&render, 0u);
    render.stderr_terminal = true;
    session.last_assistant = "\xff";
    errno = 0;
    assert(snag_render_history(&render, session.last_user, session.last_assistant) < 0);
    assert(errno == EILSEQ && !render.public_item_open);
    assert(snag_render_public_begin(&render, STDERR_FILENO, NULL) == 0);
    assert(snag_render_public_end(&render) == 0);
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
    struct snag_render render;
    struct snag_term term;
    struct snag_buf delivered;
    char output[4096] = {0};
    size_t used = 0u;
    int fds[2];
    int saved;

    assert(pipe(fds) == 0);
    assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    saved = dup(STDOUT_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDOUT_FILENO) >= 0);
    close(fds[1]);
    snag_term_init(&term);
    term.columns = 80u;
    snag_render_init(&render, 0u);
    render.stdout_terminal = true;
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    snag_render_attach_term(&render, &term);
    snag_buf_init(&delivered, 1024u);
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snag_render_public(&render, first, sizeof(first) - 1u,
                             &delivered) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strcmp(output, "Live") == 0);
    for (size_t i = 0u; i < sizeof(second) - 1u; ++i)
        assert(snag_render_public(&render, second + i, 1u, &delivered) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strcmp(output, "Live café [docs] <") == 0);
    assert(snag_render_public(&render, third, sizeof(third) - 1u,
                             &delivered) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strcmp(output,
                  "Live café [docs] <https://example.test> and co") == 0);
    assert(snag_render_public(&render, fourth, sizeof(fourth) - 1u,
                             &delivered) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strcmp(output,
                  "Live café [docs] <https://example.test> and code\n") == 0);
    assert(snag_render_public_end(&render) == 0);
    assert(snag_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data,
                  "# **Live** café [docs](https://example.test) and `code`\n") == 0);
    snag_buf_free(&delivered);

    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snag_render_public(&render, "**aborted", 9u, NULL) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strcmp(output,
                  "Live café [docs] <https://example.test> and code\n\n"
                  "• aborted") == 0);
    assert(snag_render_public_abort(&render) == 0);
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snag_render_public(&render, "literal", 7u, NULL) == 0);
    assert(snag_render_public_end(&render) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strcmp(output,
                  "Live café [docs] <https://example.test> and code\n\n"
                  "• aborted\n\n• literal") == 0);
    assert(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    while (read(fds[0], output, sizeof(output)) > 0)
        ;
    close(fds[0]);
    snag_term_close(&term);
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
    struct snag_buf delivered;

    snag_buf_init(&delivered, sizeof(markdown));
    assert(capture_markdown(markdown, true, true, SNAG_COLOR_NEVER,
                            output, sizeof(output), &delivered) > 0u);
    assert(strcmp(output, rendered) == 0);
    assert(snag_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data, markdown) == 0);
    snag_buf_free(&delivered);
    assert(capture_markdown(markdown, true, false, SNAG_COLOR_ALWAYS,
                            output, sizeof(output), NULL) > 0u);
    assert(strstr(output, "\033[0;1mName") != NULL);
    assert(strstr(output, "\033[0;1malpha") != NULL);
    assert(strstr(output, "\033[0;33mready") != NULL);
    assert(strstr(output, "\033[0;4;34mhttps://example.test") != NULL);

    assert(capture_markdown_width(markdown, true, true, SNAG_COLOR_NEVER,
                                  28u, output, sizeof(output), NULL) > 0u);
    assert(strcmp(output, narrow) == 0);

    assert(capture_markdown(malformed, true, true, SNAG_COLOR_NEVER,
                            output, sizeof(output), NULL) > 0u);
    assert(strcmp(output, "• | Name | State |\n| -- | nope |\nafter\n") == 0);
    assert(capture_markdown(code_pipe, true, true, SNAG_COLOR_NEVER,
                            output, sizeof(output), NULL) > 0u);
    assert(strcmp(output, code_pipe_rendered) == 0);
    assert(capture_markdown(markdown, false, false, SNAG_COLOR_NEVER,
                            output, sizeof(output), NULL) > 0u);
    assert(strcmp(output, markdown) == 0);
}

static size_t
capture_color(enum snag_color_mode mode, bool chat_view,
              unsigned int verbosity, int timeout_ms,
              uint32_t default_timeout_ms, uint32_t max_output_bytes,
              char *out, size_t out_size)
{
    struct snag_render render;
    struct snag_irc_event event;
    struct snag_response_item call;
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
    snag_render_init(&render, verbosity);
    if (chat_view)
        assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
    snag_render_set_color(&render, mode);
    assert(snag_render_submitted(&render, "› ", "plain") == 0);
    assert(snag_render_warning_ctx(&render, "careful") == 0);
    assert(snag_render_error_ctx(&render, "broken") == 0);
    assert(snag_render_host(&render, "status") == 0);
    assert(snag_render_event(&render, 7u, "compaction_completed") == 0);
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
        struct snag_render_block block;
        assert(snag_render_prepare_tool_start(&block, &call, "/tmp",
                                             default_timeout_ms, verbosity, 0u) == 0);
        assert(snag_render_tool_block(&render, &block) == 0);
        snag_render_block_free(&block);
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
        struct snag_render_block block;
        assert(snag_render_prepare_tool_finish(&block, call.name, result,
                                              max_output_bytes, verbosity, 0u) == 0);
        assert(snag_render_tool_block(&render, &block) == 0);
        snag_render_block_free(&block);
    }
    json_decref(result);
    memset(&event, 0, sizeof(event));
    event.kind = SNAG_IRC_MESSAGE;
    event.timestamp_ms = 1000u;
    memcpy(event.nick, "agent", 6u);
    memcpy(event.text, "answer", 7u);
    /* Immediate and queued chat use the same role colors, independent of
     * locality and history. Notices retain the same sender palette. */
    for (unsigned int flags = 0u; flags < 16u; ++flags) {
        event.local = (flags & 1u) != 0u;
        event.op = (flags & 2u) != 0u;
        event.historical = (flags & 4u) != 0u;
        event.kind = flags & 8u ? SNAG_IRC_NOTICE : SNAG_IRC_MESSAGE;
        assert(snag_render_irc_event(&render, &event) == 0);
    }
    assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
    if (chat_view)
        assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
    snag_render_free(&render);
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
test_local_mention_highlight(void)
{
    static const struct {
        const char *text, *nick, *room;
        enum snag_irc_event_kind kind;
        bool op, highlight;
    } cases[] = {
        {"@ALICE **bold** `code` tail", "alice", "#room", SNAG_IRC_MESSAGE, false, true},
        {"alice: plain tail", "alice", "#room", SNAG_IRC_MESSAGE, true, true},
        {"@alice notice tail", "alice", "#room", SNAG_IRC_NOTICE, false, true},
        {"malice alice2 alice-other éalice aliceé", "alice", "#room", SNAG_IRC_MESSAGE, false, false},
        {"@bob wrong destination", "alice", "#room", SNAG_IRC_MESSAGE, false, false},
        {"@alice wrong room", "alice", "#other", SNAG_IRC_MESSAGE, false, false},
        {"@alice old alias", "alice2", "#room", SNAG_IRC_MESSAGE, false, false},
        {"@alice2 accepted alias", "alice2", "#room", SNAG_IRC_MESSAGE, false, true},
        {"@alice topic", "alice", "#room", SNAG_IRC_TOPIC, false, false},
        {"unregistered nick", "", "#room", SNAG_IRC_MESSAGE, false, false},
        {"@local-other also addressed", "alice", "#room", SNAG_IRC_MESSAGE, false, true},
    };
    for (unsigned int flags = 0u; flags < 8u; ++flags) {
        for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            struct snag_render render;
            struct snag_term term;
            struct snag_irc_destinations destinations = {.count = 2u, .items = {
                {.endpoint = "server", .room = "#room", .operator = "local-other",
                 .model = "local-other", .target = {.id = 1u}},
                {.endpoint = "other", .room = "#room", .operator = "bob",
                 .model = "bob", .target = {.id = 2u}},
            }};
            struct snag_irc_event event = {.endpoint = "server", .nick = "peer"};
            char output[8192] = {0};
            int fds[2], saved = dup(STDERR_FILENO);
            bool color = (flags & 1u) != 0u;
            assert(saved >= 0 && pipe(fds) == 0);
            assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
            assert(dup2(fds[1], STDERR_FILENO) >= 0);
            close(fds[1]);
            snag_term_init(&term);
            term.columns = 40u;
            strcpy(flags & 4u ? destinations.items[0].model :
                               destinations.items[0].operator, cases[i].nick);
            assert(snag_term_set_destinations(&term, &destinations) == 0);
            snag_render_init(&render, 0u);
            render.stderr_terminal = true;
            snag_render_attach_term(&render, &term);
            snag_render_set_color(&render, color ? SNAG_COLOR_ALWAYS : SNAG_COLOR_NEVER);
            if (flags & 2u)
                assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
            strcpy(event.room, cases[i].room);
            strcpy(event.text, cases[i].text);
            event.kind = cases[i].kind;
            event.op = cases[i].op;
            event.historical = !(flags & 2u);
            assert(snag_render_irc_event(&render, &event) == 0);
            assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
            size_t used = drain_available(fds[0], output, sizeof(output), 0u);
            event.kind = SNAG_IRC_MESSAGE;
            event.op = false;
            strcpy(event.text, "ordinary followup");
            assert(snag_render_irc_event(&render, &event) == 0);
            (void)drain_available(fds[0], output, sizeof(output), used);
            snag_render_free(&render);
            snag_term_close(&term);
            assert(dup2(saved, STDERR_FILENO) >= 0);
            close(saved);
            close(fds[0]);
            assert((strstr(output, "\033[1;35m[1] ") != NULL) == (color && cases[i].highlight));
            assert(strstr(output + used, "ordinary") && strstr(output + used, "followup"));
            assert(!strstr(output + used, "35m"));
            if (!color)
                assert(!strchr(output, '\033'));
            if (color && i == 0u) {
                assert(strstr(output, "\033[0;1;1;35m"));
                assert(strstr(output, "\033[0;33;1;35m"));
                assert(strstr(output, "code\033[0m"));
                assert(strstr(output, "\033[0;1;35m"));
                assert(strstr(output, "tail\033[0m"));
            }
        }
    }
}

static size_t
capture_lifecycle(unsigned int verbosity, enum snag_color_mode color,
                  char *out, size_t out_size)
{
    static const char *const events[] = {
        "compaction_completed", "goal_started", "goal_reworded",
        "goal_completed", "goal_cancelled", "turn_completed"
    };
    struct snag_render render;
    size_t used = 0u;
    ssize_t n;
    int fds[2];
    int saved;

    assert(pipe(fds) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snag_render_init(&render, verbosity);
    snag_render_set_color(&render, color);
    for (size_t i = 0u; i < sizeof(events) / sizeof(events[0]); ++i)
        assert(snag_render_event(&render, i + 1u, events[i]) == 0);
    snag_render_free(&render);
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
capture_resume_hint(enum snag_color_mode color, char *out, size_t out_size)
{
    static const char command[] = "'snajpagent' --resume '0123'";
    struct snag_render render;
    size_t used = 0u;
    ssize_t n;
    int fds[2];
    int saved;

    assert(pipe(fds) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snag_render_init(&render, 0u);
    snag_render_set_color(&render, color);
    assert(snag_render_resume_hint(&render, command, sizeof(command) - 1u) == 0);
    snag_render_free(&render);
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
test_tool_previews(void)
{
    char text[1100];
    struct snag_render_block block;
    struct snag_response_item call = {.name = "arbitrary_tool"};

    memset(text, 'x', sizeof(text) - 1u);
    text[sizeof(text) - 1u] = '\0';
    assert(snag_presentation_limit(SNAG_PRESENT_ARGUMENTS, 1u) == 0u);
    assert(snag_presentation_limit(SNAG_PRESENT_OUTPUT, 1u) == 0u);
    assert(snag_presentation_limit(SNAG_PRESENT_ARGUMENTS, 2u) == 1024u);
    assert(snag_presentation_limit(SNAG_PRESENT_OUTPUT, 2u) == 512u);
    assert(snag_presentation_limit(SNAG_PRESENT_OUTPUT, 3u) == SIZE_MAX);
    for (unsigned int level = 0u; level <= SNAG_VERBOSITY_MAX; ++level) {
        assert(snag_presentation_enabled(SNAG_PRESENT_CHAT, level, SNAG_RENDER_CHAT));
        assert(!snag_presentation_enabled(SNAG_PRESENT_TOOL, level, SNAG_RENDER_CHAT));
        assert(snag_presentation_enabled(SNAG_PRESENT_DEBUG, level, SNAG_RENDER_ROLLOUT) ==
               (level >= 4u));
    }
    for (size_t n = 1023u; n <= 1025u; ++n) {
        call.arguments = json_object();
        assert(call.arguments);
        assert(json_object_set_new(call.arguments, "x", json_stringn(text, n - 8u)) == 0);
        for (unsigned int level = 1u; level <= 3u; ++level) {
            assert(snag_render_prepare_tool_start(&block, &call, "/work", 0u, level, 40u) == 0);
            assert(block.body.len == (level == 1u ? 0u : level == 2u && n > 1024u ? 1024u : n));
            assert(block.truncated == (level == 2u && n > 1024u));
            assert(block.context.len == 0u || level == 3u);
            assert(block.text.len <= 512u);
            assert(snag_term_text_width((char *)block.text.data, block.text.len - 1u) < 40u);
            assert(memchr(block.text.data, '\n', block.text.len) == block.text.data + block.text.len - 1u);
            snag_render_block_free(&block);
        }
        json_decref(call.arguments);
    }
    for (size_t n = 511u; n <= 513u; ++n) {
        json_t *result = json_object();
        assert(result);
        assert(json_object_set_new(result, "model_text", json_stringn(text, n)) == 0);
        for (unsigned int level = 1u; level <= 3u; ++level) {
            assert(snag_render_prepare_tool_finish(&block, call.name, result, 0u, level, 0u) == 0);
            assert(block.body.len == (level == 1u ? 0u : level == 2u && n > 512u ? 512u : n));
            assert(block.truncated == (level == 2u && n > 512u));
            assert(snag_buf_terminate(&block.text) == 0);
            assert(!strstr((char *)block.text.data, "0ms"));
            snag_render_block_free(&block);
        }
        json_decref(result);
    }
    call.arguments = json_string("line\n\t界é");
    assert(call.arguments);
    assert(snag_render_prepare_tool_start(&block, &call, "/work", 0u, 2u, 20u) == 0);
    assert(snag_utf8_valid(block.text.data, block.text.len, true));
    assert(snag_utf8_valid(block.body.data, block.body.len, true));
    assert(memchr(block.text.data, '\n', block.text.len) == block.text.data + block.text.len - 1u);
    snag_render_block_free(&block);
    json_decref(call.arguments);
}

static struct snag_render_source
append_event(FILE *file, const char *text)
{
    struct snag_render_source source = {ftello(file), strlen(text)};
    assert(source.offset >= 0);
    assert(fwrite(text, 1u, source.len, file) == source.len);
    assert(fflush(file) == 0);
    return source;
}

static void
test_semantic_history(void)
{
    for (unsigned int level = 0u; level <= 3u; ++level) {
        char path[] = "build/verbosity-log-XXXXXX";
        int log_fd = mkstemp(path);
        assert(log_fd >= 0 && unlink(path) == 0);
        FILE *file = fdopen(log_fd, "w+");
        struct snag_render render;
        struct snag_buf response, finish;
        char args[1401], result[801], output[8192] = {0};
        int fds[2], saved = dup(STDERR_FILENO);
        assert(file && saved >= 0 && pipe(fds) == 0);
        assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
        assert(dup2(fds[1], STDERR_FILENO) >= 0);
        close(fds[1]);
        memset(args, 'A', sizeof(args) - 1u);
        args[sizeof(args) - 1u] = '\0';
        memset(result, 'R', sizeof(result) - 1u);
        result[sizeof(result) - 1u] = '\0';
        snag_buf_init(&response, 4096u);
        snag_buf_init(&finish, 4096u);
        assert(snag_buf_printf(&response,
            "{\"data\":{\"items\":[{\"name\":\"future_tool\",\"call_id\":\"one\","
            "\"arguments\":\"%s\"}]}}\n", args) == 0);
        assert(snag_buf_terminate(&response) == 0);
        assert(snag_buf_printf(&finish, "{\"data\":{\"call_id\":\"one\",\"result\":{"
                              "\"status\":\"failed\",\"model_text\":\"%s\"}}}\n", result) == 0);
        assert(snag_buf_terminate(&finish) == 0);
        snag_render_init(&render, 6u);
        snag_render_set_color(&render, SNAG_COLOR_NEVER);
        assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
        struct snag_render_source source = append_event(file, (char *)response.data);
        assert(snag_render_durable(&render, fileno(file), source, "response_completed", 0u, 0u) == 0);
        source = append_event(file, "{\"data\":{\"call_id\":\"one\",\"resolved_workdir\":\"/work\"}}\n");
        assert(snag_render_durable(&render, fileno(file), source, "tool_started", 0u, 0u) == 0);
        source = append_event(file, (char *)finish.data);
        assert(snag_render_durable(&render, fileno(file), source, "tool_finished", 0u, 0u) == 0);
        assert(snag_render_runtime(&render, "hidden-debug") == 0);
        assert(snag_render_protocol(&render, "hidden", "hidden-protocol", 15u) == 0);
        render.verbosity = level;
        assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
        size_t used = drain_available(fds[0], output, sizeof(output), 0u);
        assert((strstr(output, "future_tool") != NULL) == (level >= 1u));
        assert((strstr(output, "RRRR") != NULL) == (level >= 2u));
        assert((strstr(output, "[arguments truncated]") != NULL) == (level == 2u));
        assert((strstr(output, "[output truncated]") != NULL) == (level == 2u));
        assert(!strstr(output, "hidden-debug") && !strstr(output, "hidden-protocol"));
        render.verbosity = 6u;
        assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
        assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
        (void)drain_available(fds[0], output, sizeof(output), used);
        assert(count_text(output, "future_tool") == (level ? 2u : 0u));
        snag_render_free(&render);
        snag_buf_free(&response);
        snag_buf_free(&finish);
        fclose(file);
        assert(dup2(saved, STDERR_FILENO) >= 0);
        close(saved);
        close(fds[0]);
    }
}

struct downgrade {
    struct snag_render *render;
    unsigned int calls, level;
};

static int
downgrade_checkpoint(void *opaque)
{
    struct downgrade *change = opaque;
    if (++change->calls == 2u)
        change->render->verbosity = change->level;
    return 0;
}

static void
test_live_downgrade(void)
{
    struct snag_render render;
    struct snag_render_block block;
    struct downgrade change = {&render, 0u, 2u};
    char payload[5000], output[8192] = {0};
    int fds[2], saved = dup(STDERR_FILENO);
    assert(saved >= 0 && pipe(fds) == 0);
    assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    assert(dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    memset(payload, 'Q', sizeof(payload));
    memcpy(payload + sizeof(payload) - 12u, "secret-tail", 12u);
    json_t *result = json_object();
    assert(result && json_object_set_new(result, "model_text", json_string(payload)) == 0);
    assert(snag_render_prepare_tool_finish(&block, "future", result, 0u, 3u, 0u) == 0);
    snag_render_init(&render, 3u);
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    render.checkpoint = downgrade_checkpoint;
    render.checkpoint_opaque = &change;
    assert(snag_render_tool_block(&render, &block) == 0);
    (void)drain_available(fds[0], output, sizeof(output), 0u);
    assert(strstr(output, "[output truncated]") && !strstr(output, "secret-tail"));
    assert(block.body.len == sizeof(payload) - 1u);
    snag_render_block_free(&block);
    json_decref(result);
    snag_render_free(&render);

    snag_render_init(&render, 5u);
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    change.calls = 0u;
    change.level = 0u;
    render.checkpoint = downgrade_checkpoint;
    render.checkpoint_opaque = &change;
    assert(snag_render_protocol(&render, "live", payload, strlen(payload)) == 0);
    (void)drain_available(fds[0], output, sizeof(output), 0u);
    assert(strstr(output, "[display omitted]") && !strstr(output, "secret-tail"));
    snag_render_free(&render);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    close(fds[0]);
}

static void
test_append_only_views(unsigned int verbosity)
{
    struct snag_render render;
    struct snag_irc_event event = {0};
    struct snag_buf delivered;
    char output[8192] = {0};
    size_t used = 0u;
    int fds[2];
    int saved;

    assert(pipe(fds) == 0);
    assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snag_render_init(&render, verbosity);
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
    render.verbosity = 1u;
    event.kind = SNAG_IRC_MESSAGE;
    event.timestamp_ms = 1000u;
    memcpy(event.nick, "peer", 5u);
    memcpy(event.text, "chat-one", 9u);
    assert(snag_render_irc_event(&render, &event) == 0);
    assert(snag_render_event(&render, 1u, "goal_started") == 0);
    assert(snag_render_event(&render, 2u, "compaction_completed") == 0);
    snag_buf_init(&delivered, 1024u);
    assert(snag_render_rollout_begin(&render, STDERR_FILENO, "agent › ", SNAG_PRESENT_CONVERSATION) == 0);
    assert(snag_render_rollout(&render, "hidden-prefix ", 14u, &delivered) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strstr(output, "chat-one") != NULL);
    assert(strstr(output, "Goal set") == NULL);
    assert(strstr(output, "Compacted") == NULL);
    assert(strstr(output, "hidden-prefix") == NULL);

    assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
    assert(snag_render_rollout(&render, "live-suffix ", 12u, &delivered) == 0);
    render.verbosity = 4u;
    assert(snag_render_runtime(&render, "queued-runtime") == 0);
    memcpy(event.text, "chat-two", 9u);
    assert(snag_render_irc_event(&render, &event) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strstr(output,
                  "── rollout ──\n• Goal set\n• Compacted\n"
                  "agent › hidden-prefix live-suffix ") != NULL);
    assert(strstr(output, "queued-runtime") != NULL);
    assert(strstr(output, "chat-two") == NULL);

    assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
    assert(snag_render_rollout(&render, "hidden-tail", 11u, &delivered) == 0);
    assert(snag_render_rollout_end(&render) == 0);
    assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(strstr(output, "── chat ──\n") != NULL);
    assert(strstr(output, "chat-two") != NULL);
    assert(strstr(output, "── rollout ──\nhidden-tail\n") != NULL);
    assert(count_text(output, "queued-runtime") == 1u);
    assert(count_text(output, "hidden-prefix") == 1u);
    assert(count_text(output, "live-suffix") == 1u);
    assert(count_text(output, "hidden-tail") == 1u);
    assert(count_text(output, "chat-two") == 1u);
    assert(count_text(output, "• Goal set") == 1u);
    assert(count_text(output, "• Compacted") == 1u);
    assert(count_text(output, "── rollout ──") == 2u);
    assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(count_text(output, "── rollout ──") == 2u);
    errno = 0;
    assert(snag_render_set_view(&render, (enum snag_render_view)-1) < 0);
    assert(errno == EINVAL);
    render.verbosity = verbosity;
    event.local = true;
    memcpy(event.nick, "agent", 6u);
    memcpy(event.text, "public-before-rename", 21u);
    assert(snag_render_irc_event(&render, &event) == 0);
    memcpy(event.nick, "agent2", 7u);
    memcpy(event.text, "public-after-rename", 20u);
    assert(snag_render_irc_event(&render, &event) == 0);
    event.local = false;
    memcpy(event.nick, "agent", 6u);
    memcpy(event.text, "peer-with-old-nick", 19u);
    assert(snag_render_irc_event(&render, &event) == 0);
    assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(count_text(output, "agent › public-before-rename") == 1u);
    assert(count_text(output, "agent2 › public-after-rename") == 1u);
    assert(count_text(output, "peer-with-old-nick") == 1u);
    event.historical = true;
    memcpy(event.nick, "agent2", 7u);
    memcpy(event.text, "retained-own-message", 21u);
    assert(snag_render_irc_event(&render, &event) == 0);
    event.historical = false;
    event.local = true;
    event.kind = SNAG_IRC_NOTICE;
    memcpy(event.text, "own-public-notice", 18u);
    assert(snag_render_irc_event(&render, &event) == 0);
    event.local = false;
    event.nick[0] = '\0';
    event.kind = SNAG_IRC_TOPIC;
    memcpy(event.text, "/workspace", 11u);
    assert(snag_render_irc_event(&render, &event) == 0);
    event.kind = SNAG_IRC_HISTORY_READY;
    event.text[0] = '\0';
    assert(snag_render_irc_event(&render, &event) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(count_text(output, "history agent2 › retained-own-message") == 1u);
    assert(count_text(output, "-agent2 - own-public-notice") == 1u);
    assert(strstr(output, "· topic · /workspace\n") != NULL);
    assert(strstr(output, "· history synchronized\n") != NULL);
    assert(snag_render_view(&render) == SNAG_RENDER_CHAT);
    assert(snag_render_rollout_begin(&render, STDERR_FILENO, NULL,
                                     SNAG_PRESENT_CONVERSATION) == 0);
    assert(snag_render_rollout(&render, "offline-private", 15u, NULL) == 0);
    assert(snag_render_rollout_end(&render) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(!strstr(output, "offline-private"));
    event.kind = SNAG_IRC_MESSAGE;
    memcpy(event.nick, "peer", 5u);
    memcpy(event.text, "offline-retained", 17u);
    assert(snag_render_irc_event(&render, &event) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(count_text(output, "offline-retained") == 1u);
    assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
    used = drain_available(fds[0], output, sizeof(output), used);
    assert(count_text(output, "offline-private") == 1u);
    assert(snag_render_view(&render) == SNAG_RENDER_ROLLOUT);
    assert(snag_render_public_begin(&render, STDERR_FILENO, NULL) == 0);
    errno = 0;
    assert(snag_render_rollout_begin(&render, STDERR_FILENO, NULL, SNAG_PRESENT_CONVERSATION) < 0);
    assert(errno == EBUSY);
    assert(snag_render_public_abort(&render) == 0);
    assert(snag_render_rollout_begin(&render, STDERR_FILENO, NULL, SNAG_PRESENT_CONVERSATION) == 0);
    assert(snag_render_rollout_abort(&render) == 0);
    assert(snag_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data,
                  "hidden-prefix live-suffix hidden-tail") == 0);
    snag_buf_free(&delivered);
    snag_render_free(&render);
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
    struct snag_render render;
    struct snag_buf delivered;

    assert(setlocale(LC_ALL, "") != NULL);
    assert(setenv("TZ", "UTC0", 1) == 0);
    tzset();
    test_local_mention_highlight();
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

    snag_buf_init(&delivered, 1024u);
    assert(capture_wrapped("alpha beta gamm", "a delta", 20u, true,
                           "• alpha beta gamm", "• alpha beta gamma\n  delta",
                           output, sizeof(output), &delivered) > 0u);
    assert(strcmp(output, "• alpha beta gamma\n  delta") == 0);
    assert(snag_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data,
                  "alpha beta gamma delta") == 0);
    snag_buf_free(&delivered);
    snag_buf_init(&delivered, 64u);
    assert(capture_wrapped("123456789012345678 ", "next", 20u, true,
                           "• 123456789012345678",
                           "• 123456789012345678\n  next",
                           output, sizeof(output), &delivered) > 0u);
    assert(snag_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data,
                  "123456789012345678 next") == 0);
    snag_buf_free(&delivered);
    test_punctuation_wrapping();

    snag_buf_init(&delivered, sizeof(markdown));
    assert(capture_markdown(markdown, true, true, SNAG_COLOR_NEVER,
                            output, sizeof(output), &delivered) > 0u);
    assert(strcmp(output, rendered) == 0);
    assert(snag_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data, markdown) == 0);
    snag_buf_free(&delivered);
    assert(capture_markdown(markdown, false, false, SNAG_COLOR_NEVER,
                            output, sizeof(output), NULL) > 0u);
    assert(strcmp(output, markdown) == 0);
    assert(capture_markdown(markdown, true, false, SNAG_COLOR_ALWAYS,
                            output, sizeof(output), NULL) > 0u);
    assert(strstr(output, "\033[0;1;36mLive") != NULL);
    assert(strstr(output, "\033[0;33mcode") != NULL);
    assert(strstr(output, "\033[0;4;34mhttps://example.test") != NULL);
    assert(strstr(output, "\033[0;34;2mold") != NULL);
    assert(capture_markdown("**", true, true, SNAG_COLOR_NEVER,
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
    test_retained_prompt();
    test_mention_completion();
    test_completion_choices();
    test_destination_editor();
    test_markdown_streaming();
    test_markdown_tables();
    test_tool_previews();
    test_semantic_history();
    test_live_downgrade();
    for (unsigned int verbosity = 0u; verbosity <= 6u; ++verbosity)
        test_append_only_views(verbosity);

    assert(capture_lifecycle(0u, SNAG_COLOR_NEVER,
                             output, sizeof(output)) > 0u);
    assert(strcmp(output,
        "• Compacted\n"
        "• Goal set\n"
        "• Goal set\n"
        "• Goal cleared\n"
        "• Goal cleared\n") == 0);
    assert(capture_lifecycle(4u, SNAG_COLOR_NEVER,
                             output, sizeof(output)) > 0u);
    assert(strstr(output,
                  "• Compacted\nevent › 1 compaction_completed synced\n"));
    assert(strstr(output,
                  "• Goal cleared\nevent › 5 goal_cancelled synced\n"));
    assert(strstr(output, "event › 6 turn_completed synced\n"));
    assert(capture_lifecycle(0u, SNAG_COLOR_ALWAYS,
                             output, sizeof(output)) > 0u);
    assert(count_text(output, "\033[1;32m• ") == 5u);
    assert(count_text(output, "\n\033[0m") == 5u);
    assert(capture_resume_hint(SNAG_COLOR_NEVER,
                               output, sizeof(output)) > 0u);
    assert(strcmp(output,
        "• You can resume this session with the following command:\n"
        "'snajpagent' --resume '0123'\n") == 0);
    assert(capture_resume_hint(SNAG_COLOR_ALWAYS,
                               output, sizeof(output)) > 0u);
    assert(strcmp(output,
        "\033[1;32m• You can resume this session with the following command:"
        "\033[0m\n'snajpagent' --resume '0123'\n") == 0);

    snag_render_init(&render, 6u);
    errno = 0;
    assert(snag_render_transport(&render, '>', "bad\rline", 8u) < 0);
    assert(errno == EINVAL);

    assert(capture_color(SNAG_COLOR_ALWAYS, false, 6u, 2500, 0u, 0u,
                         output, sizeof(output)) > 0u);
    assert(strstr(output, "\033[1;36m› \033[0mplain\n") != NULL);
    assert(strstr(output, "\033[1;33m" SNAJPAGENT_NAME
                  ": careful\n\033[0m") != NULL);
    assert(strstr(output, "\033[1;31m" SNAJPAGENT_NAME
                  ": broken\n\033[0m") != NULL);
    assert(strstr(output, "\033[34mstatus\n\033[0m") != NULL);
    assert(strstr(output, "\033[1;32m• Compacted\n\033[0m") != NULL);
    assert(strstr(output, "event › 7 compaction_completed synced\n") != NULL);
    assert(strstr(output,
                  "\033[33m→ exec_command\033[0m  {\"command\":\"printf plain\"") != NULL);
    assert(strstr(output, "  timeout: 2500ms\n") != NULL);
    assert(count_text(output, "\033[1;36magent \033[0m› answer") == 4u);
    assert(count_text(output, "\033[1;35m@agent \033[0m› answer") == 4u);
    assert(count_text(output, "\033[1;36m-agent \033[0m- answer") == 4u);
    assert(count_text(output, "\033[1;35m-@agent \033[0m- answer") == 4u);
    assert(capture_color(SNAG_COLOR_ALWAYS, true, 6u, -1, 0u, 0u,
                         output, sizeof(output)) > 0u);
    assert(strstr(output, "\033[1;36m› \033[0mplain\n") != NULL);
    assert(count_text(output, "\033[1;36magent \033[0m› answer") == 4u);
    assert(count_text(output, "\033[1;35m@agent \033[0m› answer") == 4u);
    assert(count_text(output, "\033[1;36m-agent \033[0m- answer") == 4u);
    assert(count_text(output, "\033[1;35m-@agent \033[0m- answer") == 4u);
    assert(capture_color(SNAG_COLOR_NEVER, true, 6u, -1, 0u, 0u,
                         output, sizeof(output)) > 0u);
    assert(strchr(output, '\033') == NULL);
    assert(strstr(output, "→ exec_command") == NULL);
    assert(capture_color(SNAG_COLOR_NEVER, false, 1u, -1, 0u, 0u,
                         output, sizeof(output)) > 0u);
    assert(strstr(output, "→ exec_command  {\"command\":\"printf plain\"") != NULL);
    assert(strstr(output, "  arguments:") == NULL);
    assert(strstr(output, "  output:") == NULL);
    assert(strstr(output, "fixture tool output") == NULL);
    assert(capture_color(SNAG_COLOR_NEVER, true, 1u, -1, 0u, 0u,
                         output, sizeof(output)) > 0u);
    assert(strstr(output, "→ exec_command") == NULL);
    assert(strstr(output, "  arguments:") == NULL);
    assert(strstr(output, "fixture tool output: café") == NULL);
    assert(capture_color(SNAG_COLOR_NEVER, false, 3u, -1, 4000u, 8u,
                         output, sizeof(output)) > 0u);
    assert(strstr(output,
                  "  timeout: 4000ms\n") != NULL);
    assert(strstr(output, "  arguments:") != NULL);
    assert(strstr(output, "  output:\nfixture ") != NULL);
    assert(strstr(output, "[output truncated]") != NULL);
    assert(strstr(output, "fixture tool output: café") == NULL);

    assert(setenv("NO_COLOR", "1", 1) == 0);
    snag_render_init(&render, 0u);
    render.stderr_terminal = true;
    snag_render_set_color(&render, SNAG_COLOR_AUTO);
    assert(!render.color_stderr);
    assert(unsetenv("NO_COLOR") == 0);
    puts("test_render: ok");
    return 0;
}
