/* SPDX-License-Identifier: GPL-2.0-only */
#include "render.h"
#include "fs.h"
#include "presentation.h"
#include "snajpagent.h"
#include "store.h"
#include "store_internal.h"

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

struct output_capture {
    int fd;
    int saved[2];
};

static void
test_null_output(void)
{
    int fd = open("/dev/null", O_WRONLY);
    assert(fd >= 0);
    assert(snag_term_write(fd, "discarded", 9u) == 0);
    assert(close(fd) == 0);
    assert(snag_term_write(fd, "closed", 6u) < 0);
    fd = open("/dev/null", O_RDONLY);
    assert(fd >= 0);
    assert(snag_term_write(fd, "readonly", 8u) < 0);
    assert(close(fd) == 0);
}

static struct output_capture
capture_open(bool stdout_enabled, bool stderr_enabled)
{
    struct output_capture capture;
    const bool enabled[] = {stdout_enabled, stderr_enabled};
    int fds[2];

    assert(pipe(fds) == 0);
    capture.fd = fds[0];
    for (int i = 0; i < 2; ++i) {
        capture.saved[i] = enabled[i] ? dup(i + 1) : -1;
        if (enabled[i]) {
            assert(capture.saved[i] >= 0);
            assert(dup2(fds[1], i + 1) >= 0);
        }
    }
    close(fds[1]);
    return capture;
}

static struct output_capture
capture_terminal(struct snag_render *render, struct snag_term *term, unsigned int columns,
    bool stdout_enabled, bool stderr_enabled)
{
    struct output_capture capture = capture_open(stdout_enabled, stderr_enabled);
    snag_term_init(term);
    term->columns = columns;
    snag_render_init(render, 0u);
    render->stdout_terminal |= stdout_enabled;
    render->stderr_terminal |= stderr_enabled;
    snag_render_attach_term(render, term);
    return capture;
}

static void
capture_restore(struct output_capture *capture)
{
    for (int i = 0; i < 2; ++i) {
        if (capture->saved[i] < 0) continue;
        assert(dup2(capture->saved[i], i + 1) >= 0);
        close(capture->saved[i]);
    }
}

static size_t
capture_close(struct output_capture *capture, char *out, size_t size, size_t used)
{
    ssize_t n;

    capture_restore(capture);
    while ((n = read(capture->fd, out + used, size - used - 1u)) > 0) used += (size_t)n;
    assert(n == 0);
    close(capture->fd);
    out[used] = '\0';
    return used;
}

static size_t
count_text(const char *haystack, const char *needle)
{
    size_t count = 0u;
    size_t n = strlen(needle);
    for (const char *p = haystack; (p = strstr(p, needle)); p += n) ++count;
    return count;
}

struct styled_output {
    struct snag_buf text, styles;
};

static int
collect_styled(void *opaque, const char *text, size_t length, unsigned int style, uint64_t source,
    size_t source_length)
{
    struct styled_output *out = opaque;
    (void)source;
    (void)source_length;
    if (snag_buf_append(&out->text, text, length) < 0) return -1;
    for (size_t i = 0u; i < length; ++i)
        if (snag_buf_append(&out->styles, &style, sizeof(style)) < 0) return -1;
    return 0;
}

static void
styled_terminal(struct styled_output *out, const char *text)
{
    unsigned int style = 0u;
    while (*text) {
        if (text[0] == '\033' && text[1] == '[') {
            text += 2u;
            do {
                char *end;
                unsigned long value = strtoul(text, &end, 10);
                assert(end != text);
                text = end;
                if (value == 0u)
                    style = 0u;
                else if (value == 1u)
                    style |= 1u;
                else if (value == 2u)
                    style |= 2u;
                else if (value == 3u)
                    style |= 256u;
                else if (value == 4u)
                    style |= 4u;
                else if (value == 7u)
                    style |= 8u;
                else if (value >= 30u && value <= 37u)
                    style = (style & ~240u) | ((unsigned int)(value - 29u) << 4u);
                else if (value >= 40u && value <= 47u)
                    style = (style & ~7680u) | ((unsigned int)(value - 39u) << 9u);
                else
                    assert(false);
            } while (*text == ';' && *++text);
            assert(*text++ == 'm');
        } else {
            assert(collect_styled(out, text++, 1u, style, UINT64_MAX, 0u) == 0);
        }
    }
}

static void
render_sink_fixture(struct snag_render *render)
{
    const char *text = "## Heading\n\nA **bold** and *italic* word with `code`.\n\n"
                       "| Key | Value |\n| --- | --- |\n| alpha | some long wrapped value |\n\n"
                       "```c\nprintf(\"hello\");\n```\n\nSafe \033[31m text.\n";
    assert(snag_render_input_submitted(render, "READY> ", "tab\tinput") == 0);
    assert(snag_render_public_begin(render, STDOUT_FILENO, NULL) == 0);
    assert(snag_render_public(render, text, strlen(text), NULL) == 0);
    assert(snag_render_public_end(render) == 0);
    struct snag_response_item call = {.name = "read_file",
        .call_id = "fixture-call",
        .arguments = json_pack("{s:s}", "path", "file.txt")};
    struct snag_render_block tool = {0};
    assert(snag_render_prepare_tool_start(&tool, &call, "/fixture", 0u, render->verbosity,
               render->sink.columns ? render->sink.columns : render->term->columns) == 0);
    assert(snag_render_tool_block(render, &tool) == 0);
    snag_render_block_free(&tool);
    json_decref(call.arguments);
    struct snag_irc_event irc = {.kind = SNAG_IRC_MESSAGE,
        .timestamp_ms = 1000u,
        .endpoint = "local:16667",
        .room = "#lab",
        .nick = "peer",
        .text = "Chat **bold** with a long line which can wrap across the pane."};
    render->view = SNAG_RENDER_CHAT;
    strcpy(render->chat_endpoint, irc.endpoint);
    strcpy(render->chat_room, irc.room);
    assert(snag_render_irc_event(render, &irc) == 0);
    const char *resume = "'snajpagent' --resume '0123'";
    assert(!snag_render_resume_hint(render, resume, strlen(resume)));
}

static int
read_presentation_file(const char *path, const char *id)
{
    int fd = snag_open_read(path, false);
    if (fd < 0) return 2;
    json_t *rows = NULL;
    struct snag_binary_anchor begin, tail;
    bool incomplete;
    int rc = snag_presentation_read(
        fd, id, NULL, NULL, true, SIZE_MAX, &rows, &begin, &tail, &incomplete, NULL, NULL);
    if (!rc) rc = json_dumpf(rows, stdout, JSON_COMPACT);
    json_decref(rows);
    (void)close(fd);
    return rc < 0 ? 3 : 0;
}

static bool
cancel_presentation_read(void *opaque)
{
    unsigned int *remaining = opaque;
    if (!*remaining) return true;
    --*remaining;
    return false;
}

static void
test_retained_response_reopen(void)
{
    char path[] = "build/presentation-response-XXXXXX";
    assert(mkdtemp(path));
    int dir = snag_open_read(path, true);
    assert(dir >= 0);
    const char *id = "0123456789abcdef0123456789abcdef";
    struct snag_presentation_writer *writer = snag_presentation_writer_open(dir, id);
    assert(writer && !snag_presentation_start(writer, 31u));
    struct snag_ui_command response = {.kind = SNAG_UI_DURABLE,
        .text = "response_completed",
        .data.durable.source = {.offset = 123, .len = 45u}};
    assert(!snag_presentation_append(writer, &response));
    snag_presentation_writer_close(writer);
    writer = snag_presentation_writer_open(dir, id);
    assert(writer && !snag_presentation_start(writer, 32u));
    struct snag_ui_command tool = {.kind = SNAG_UI_DURABLE,
        .text = "tool_started",
        .data.durable.source = {.offset = 168, .len = 24u}};
    assert(!snag_presentation_append(writer, &tool));
    snag_presentation_writer_close(writer);
    int fd = snag_open_read_at(dir, SNAG_PRESENTATION_FILE, false);
    assert(fd >= 0);
    json_t *rows = NULL;
    struct snag_binary_anchor begin, tail;
    bool incomplete;
    assert(!snag_presentation_read(
        fd, id, NULL, NULL, true, SIZE_MAX, &rows, &begin, &tail, &incomplete, NULL, NULL));
    const json_t *data = json_object_get(json_array_get(rows, 0u), "data");
    const json_t *saved = json_object_get(data, "response");
    assert(json_integer_value(json_object_get(saved, "offset")) == 123);
    assert(json_integer_value(json_object_get(saved, "length")) == 45);
    json_decref(rows);
    assert(!close(fd));
    assert(!snag_unlink_at(dir, SNAG_PRESENTATION_FILE, false));
    assert(!close(dir));
    assert(!rmdir(path));
}

static void
test_retained_presentation(void)
{
    char path[] = "build/presentation-XXXXXX";
    assert(mkdtemp(path));
    int dir = snag_open_read(path, true);
    assert(dir >= 0);
    const char *id = "0123456789abcdef0123456789abcdef";
    struct snag_presentation_writer *writer = snag_presentation_writer_open(dir, id);
    assert(writer);
    assert(!snag_presentation_start(writer, 31u));
    const char *text = "A **bold** answer.\n";
    struct snag_ui_command commands[] = {
        {.kind = SNAG_UI_ORIENTATION, .text = "/fixture", .label = id},
        {.kind = SNAG_UI_SUBMITTED, .text = "/fast off", .label = "READY> ", .data.value = 1u},
        {.kind = SNAG_UI_HOST, .text = "Fast mode off"},
        {.kind = SNAG_UI_PUBLIC_BEGIN, .data.public = {STDOUT_FILENO, SNAG_PRESENT_CONVERSATION}},
        {.kind = SNAG_UI_PUBLIC, .text = text, .len = strlen(text)}, {.kind = SNAG_UI_ROLLOUT_END},
        {.kind = SNAG_UI_BEFORE_PROMPT}, {.kind = SNAG_UI_WARNING, .text = "Retained warning"}};
    struct snag_render direct, replay;
    struct styled_output expected = {.text = {.max = 65536u}, .styles = {.max = 262144u}};
    struct styled_output actual = {.text = {.max = 65536u}, .styles = {.max = 262144u}};
    snag_render_init(&direct, 0u);
    snag_render_init(&replay, 0u);
    direct.stdout_terminal = direct.stderr_terminal = true;
    replay.stdout_terminal = replay.stderr_terminal = true;
    direct.color_stdout = direct.color_stderr = replay.color_stdout = replay.color_stderr = true;
    direct.sink =
        (struct snag_render_sink){.text = collect_styled, .opaque = &expected, .columns = 40u};
    replay.sink =
        (struct snag_render_sink){.text = collect_styled, .opaque = &actual, .columns = 40u};
    for (size_t i = 0u; i < sizeof(commands) / sizeof(*commands); ++i) {
        assert(!snag_presentation_append(writer, &commands[i]));
        assert(!snag_presentation_apply(&direct, &commands[i], NULL));
    }
    snag_presentation_writer_close(writer);
    writer = snag_presentation_writer_open(dir, id);
    assert(writer);
    assert(!snag_presentation_start(writer, 100u));
    json_t *position = snag_presentation_snapshot(writer);
    assert(position && json_integer_value(json_object_get(position, "origin")) == 31);
    assert(json_array_size(json_object_get(position, "tail")) == 5u);
    json_decref(position);
    snag_presentation_writer_close(writer);
    int fd = snag_open_private_append_at(dir, SNAG_PRESENTATION_FILE, false);
    assert(fd >= 0);
    struct snag_binary_anchor begin, tail;
    json_t *rows = NULL;
    bool incomplete;
    assert(!snag_presentation_read(
        fd, id, NULL, NULL, true, SIZE_MAX, &rows, &begin, &tail, &incomplete, NULL, NULL));
    assert(!incomplete && begin.next_seq == 1u && json_array_size(rows) == 8u);
    uint64_t origin;
    struct snag_binary_anchor first;
    assert(!snag_presentation_origin(fd, &tail, &origin, &first));
    assert(origin == 31u && first.next_seq == 2u);
    for (size_t i = json_array_size(rows); i; --i)
        assert(!snag_presentation_replay(
            &replay, json_object_get(json_array_get(rows, i - 1u), "data"), -1));
    assert(expected.text.len == actual.text.len &&
           !memcmp(expected.text.data, actual.text.data, actual.text.len));
    assert(expected.styles.len == actual.styles.len &&
           !memcmp(expected.styles.data, actual.styles.data, actual.styles.len));
    json_decref(rows);
    rows = NULL;
    struct snag_binary_anchor preserved_begin = begin, preserved_tail = tail;
    unsigned int remaining = 2u;
    assert(snag_presentation_read(fd, id, NULL, NULL, true, SIZE_MAX, &rows, &begin, &tail,
               &incomplete, cancel_presentation_read, &remaining) < 0 &&
           errno == ECANCELED);
    assert(!rows && !memcmp(&begin, &preserved_begin, sizeof(begin)) &&
           !memcmp(&tail, &preserved_tail, sizeof(tail)));
    assert(
        !snag_presentation_writer_open(dir, "1123456789abcdef0123456789abcdef") && errno == EINVAL);
    assert(!snag_write_full(fd, "unfinished", 10u));
    assert(!close(fd));
    writer = snag_presentation_writer_open(dir, id);
    assert(writer);
    snag_presentation_writer_close(writer);
    fd = snag_open_read_at(dir, SNAG_PRESENTATION_FILE, false);
    snag_file_info info;
    assert(fd >= 0 && !snag_fstat(fd, &info) && (uint64_t)info.st_size == tail.end);
    assert(!close(fd));
    fd = snag_open_private_append_at(dir, SNAG_PRESENTATION_FILE, false);
    assert(fd >= 0 && !snag_write_full(fd, "closed-corruption", 18u));
    assert(!snag_presentation_writer_open(dir, id));
    assert(!snag_fstat(fd, &info) && (uint64_t)info.st_size == tail.end + 18u);
    assert(!close(fd));
    snag_render_free(&direct);
    snag_render_free(&replay);
    snag_buf_free(&expected.text);
    snag_buf_free(&expected.styles);
    snag_buf_free(&actual.text);
    snag_buf_free(&actual.styles);
    assert(!snag_unlink_at(dir, SNAG_PRESENTATION_FILE, false));
    assert(!close(dir) && !rmdir(path));
}

static void
test_styled_sink_matches_terminal(void)
{
    const unsigned int widths[] = {20u, 41u, 120u};
    for (size_t w = 0u; w < sizeof(widths) / sizeof(widths[0]); ++w) {
        for (unsigned int level = 0u; level <= 3u; ++level) {
            struct snag_render terminal, pane;
            struct snag_term term;
            struct output_capture capture =
                capture_terminal(&terminal, &term, widths[w], true, true);
            terminal.verbosity = level;
            terminal.color_stdout = terminal.color_stderr = true;
            render_sink_fixture(&terminal);
            char wire[16384];
            (void)capture_close(&capture, wire, sizeof(wire), 0u);
            struct styled_output expected = {.text = {.max = 65536u}, .styles = {.max = 262144u}};
            struct styled_output actual = {.text = {.max = 65536u}, .styles = {.max = 262144u}};
            styled_terminal(&expected, wire);
            snag_render_init(&pane, level);
            pane.stdout_terminal = pane.stderr_terminal = true;
            pane.color_stdout = pane.color_stderr = true;
            pane.sink = (struct snag_render_sink){
                .text = collect_styled, .opaque = &actual, .columns = widths[w]};
            render_sink_fixture(&pane);
            assert(expected.text.len == actual.text.len);
            assert(!memcmp(expected.text.data, actual.text.data, actual.text.len));
            assert(expected.styles.len == actual.styles.len);
            assert(!memcmp(expected.styles.data, actual.styles.data, actual.styles.len));
            snag_buf_free(&expected.text);
            snag_buf_free(&expected.styles);
            snag_buf_free(&actual.text);
            snag_buf_free(&actual.styles);
            snag_render_free(&terminal);
            snag_render_free(&pane);
            snag_term_close(&term);
        }
    }
}

static void
test_native_rebind(void)
{
    struct snag_render render;
    struct snag_term term;
    struct output_capture capture = capture_terminal(&render, &term, 57u, false, true);
    struct snag_buf delivered = {.max = 1024u};
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    snag_render_set_markdown(&render, false);
    assert(snag_buf_append(&term.draft, "retained draft", 14u) == 0);
    term.cursor = 4u;
    term.input[0] = 'x';
    term.input_len = 1u;
    assert(snag_render_rollout_begin(
               &render, STDERR_FILENO, "agent: ", SNAG_PRESENT_CONVERSATION) == 0);
    assert(snag_render_rollout(&render, "before", 6u, &delivered) == 0);
    struct snag_render_record *open = render.rollout_open;
    assert(open);
    term.prompt_visible = true;
    term.rendered_rows = 4u;
    term.rendered_cursor_row = 3u;
    assert(snag_buf_append(&term.painted_prompt, "obsolete coordinates", 20u) == 0);
    snag_term_rebind(&term);
    assert(!term.prompt_visible && !term.rendered_rows && !term.rendered_cursor_row);
    assert(!term.painted_prompt.len && !term.output_seen);
    assert(term.draft.len == 14u && !memcmp(term.draft.data, "retained draft", 14u));
    assert(term.cursor == 4u && term.input_len == 1u && term.input[0] == 'x');
    assert(snag_render_rebind(&render) == 0 && render.rollout_open == open);
    assert(snag_render_rollout(&render, "after", 5u, &delivered) == 0);
    assert(snag_render_rollout_end(&render) == 0);
    snag_buf_free(&delivered);
    snag_render_free(&render);
    snag_term_close(&term);
    char output[4096];
    (void)capture_close(&capture, output, sizeof(output), 0u);
    assert(count_text(output, "before") == 1u && count_text(output, "after") == 1u);
    assert(!strstr(output, "\033[") && !strstr(output, "obsolete coordinates"));
}

static size_t
history_records(const struct snag_history *history, const char *text, bool local_only)
{
    struct snag_history_snapshot snapshot = history->snapshot;
    struct snag_history_reader reader = {.fd = {-1, -1}};
    if (local_only) snapshot.global_path = NULL;
    snag_history_reader_open(&reader, &snapshot);
    struct snag_history_cursor start = snag_history_end(&snapshot), end;
    const char *entry;
    size_t count = 0u;
    int rc;
    while ((rc = snag_history_read(&reader, &snapshot, false, start, &start, &end, &entry)) > 0)
        if (!text || strcmp(entry, text) == 0) ++count;
    assert(rc == 0);
    snag_history_reader_close(&reader);
    return count;
}

static bool
term_history_has(const struct snag_history *history, const char *text)
{
    return history_records(history, text, false) != 0u;
}

static void
test_history_reader_boundaries(void)
{
    char build[4096], temp[4096], path[4096];
    assert(mkdir("build", 0700) == 0 || errno == EEXIST);
    assert(realpath("build", build));
    assert(snprintf(temp, sizeof(temp), "%s/history-reader-XXXXXX", build) > 0 && mkdtemp(temp));
    assert(snprintf(path, sizeof(path), "%s/prompt_history", temp) > 0);
    FILE *file = fopen(path, "w");
    assert(file);
    fputs("oldest\n", file);
    for (size_t i = 0u; i < 150000u; ++i) fputc('x', file);
    fputs("\nnewest\n", file);
    assert(fclose(file) == 0 && chmod(path, 0600) == 0);
    struct snag_history history = {0};
    assert(snag_history_open(&history, temp) == 0 && history.snapshot.count == 0u);
    struct snag_history_reader reader = {.fd = {-1, -1}};
    snag_history_reader_open(&reader, &history.snapshot);
    struct snag_history_cursor start = snag_history_end(&history.snapshot), end;
    const char *text;
    const size_t lengths[] = {6u, 150000u, 6u};
    for (size_t i = 0u; i < 3u; ++i) {
        int rc;
        do {
            reader.budget = 127u;
            rc = snag_history_read(&reader, &history.snapshot, false, start, &start, &end, &text);
        } while (rc == 2);
        assert(rc == 1 && strlen(text) == lengths[i]);
        if (i != 1u) assert(strcmp(text, i == 0u ? "newest" : "oldest") == 0);
    }
    assert(snag_history_read(&reader, &history.snapshot, false, start, &start, &end, &text) == 0);
    /* Reader boundaries stay fixed through a peer append. */
    struct snag_history peer = {0};
    assert(snag_history_open(&peer, temp) == 0 && snag_history_add(&peer, "peer") == 0);
    assert(snag_history_merge(&peer) == 0);
    snag_history_free(&peer);
    for (size_t i = 1u; i < 3u; ++i) {
        int rc;
        do {
            reader.budget = 127u;
            rc = snag_history_read(&reader, &history.snapshot, true, end, &start, &end, &text);
        } while (rc == 2);
        assert(rc == 1 && strlen(text) == lengths[2u - i]);
    }
    assert(snag_history_read(&reader, &history.snapshot, true, end, &start, &end, &text) == 0);
    snag_history_reader_close(&reader);
    assert(term_history_has(&history, "peer"));
    snag_history_free(&history);
    assert(unlink(path) == 0 && rmdir(temp) == 0);
}

static void
test_session_prompt_history(void)
{
    char build[4096], temp[4096], first_dir[4096], second_dir[4096], path[4096];
    struct snag_history seed = {0}, first = {0}, second = {0}, next = {0};
    struct stat st;
    assert(mkdir("build", 0700) == 0 || errno == EEXIST);
    assert(realpath("build", build));
    assert(snprintf(temp, sizeof(temp), "%s/session-history-XXXXXX", build) > 0);
    assert(mkdtemp(temp));
    assert(snprintf(first_dir, sizeof(first_dir), "%s/first", temp) > 0);
    assert(snprintf(second_dir, sizeof(second_dir), "%s/second", temp) > 0);
    assert(mkdir(first_dir, 0700) == 0 && mkdir(second_dir, 0700) == 0);
    assert(snag_history_open(&seed, temp) == 0);
    assert(snag_history_add(&seed, "seed") == 0);
    assert(snag_history_merge(&seed) == 0);
    snag_history_free(&seed);
    assert(snag_history_open(&first, temp) == 0);
    assert(snag_history_open(&second, temp) == 0);
    assert(first.snapshot.count == 0u && second.snapshot.count == 0u);
    assert(term_history_has(&first, "seed"));
    assert(snag_history_add(&first, "first-before-bind") == 0);
    assert(snag_history_bind(&first, first_dir) == 0);
    assert(snag_history_bind(&second, second_dir) == 0);
    assert(snag_history_add(&first, "same") == 0);
    assert(snag_history_add(&first, "same") == 0);
    assert(snag_history_add(&second, "second") == 0);
    assert(snag_history_merge(&first) == 0);
    assert(snag_history_merge(&first) == 0); /* Retry without new entries is inert. */
    assert(history_records(&second, "first-before-bind", true) == 0u);
    assert(snag_history_merge(&second) == 0);
    assert(history_records(&first, "second", true) == 0u);
    assert(snag_history_open(&next, temp) == 0);
    assert(next.snapshot.count == 0u); /* No global copy. Intentional repeats remain. */
    assert(history_records(&next, NULL, false) == 5u);
    assert(history_records(&next, "same", false) == 2u);
    assert(term_history_has(&next, "first-before-bind"));
    assert(term_history_has(&next, "second"));
    assert(snag_history_merge(&next) == 0);
    snag_history_free(&next);
    /* A failed merge keeps its pending suffix for retry; no seed re-import. */
    assert(snag_history_add(&first, "merge-retry") == 0);
    assert(snprintf(path, sizeof(path), "%s/prompt_history", temp) > 0);
    char backup[4096];
    assert(snprintf(backup, sizeof(backup), "%s/saved-global", temp) > 0);
    assert(rename(path, backup) == 0 && mkdir(path, 0700) == 0);
    assert(snag_history_merge(&first) < 0 && first.merged < first.snapshot.local_end);
    assert(snag_history_take_warning(&first));
    assert(rmdir(path) == 0 && rename(backup, path) == 0);
    assert(snag_history_merge(&first) == 0 && first.merged == first.snapshot.local_end);
    assert(snag_history_merge(&first) == 0);
    assert(snag_history_add(&first, "saved-before-crash") == 0);
    snag_history_free(&first); /* No orderly global merge. */
    assert(snag_history_open(&first, temp) == 0);
    assert(!term_history_has(&first, "saved-before-crash"));
    assert(snag_history_bind(&first, first_dir) == 0);
    assert(term_history_has(&first, "saved-before-crash"));
    assert(history_records(&first, "second", true) == 0u);
    assert(first.merged == first.snapshot.local_end);
    assert(snag_history_merge(&first) == 0);
    assert(snprintf(path, sizeof(path), "%s/prompt_history", first_dir) > 0);
    assert(stat(path, &st) == 0 && (st.st_mode & 0777u) == 0600u);
    int writer = dup(first.local_fd), readonly = open(path, O_RDONLY);
    assert(writer >= 0 && readonly >= 0 && dup2(readonly, first.local_fd) >= 0);
    close(readonly);
    assert(snag_history_add(&first, "write-failed") < 0);
    assert(term_history_has(&first, "write-failed"));
    assert(snag_history_take_warning(&first));
    assert(dup2(writer, first.local_fd) >= 0);
    close(writer);
    assert(snag_history_add(&first, "write-recovered") == 0);
    snag_history_free(&first);
    assert(snag_history_open(&first, temp) == 0);
    assert(snag_history_bind(&first, first_dir) == 0);
    assert(term_history_has(&first, "saved-before-crash"));
    assert(term_history_has(&first, "write-failed"));
    assert(term_history_has(&first, "write-recovered"));
    snag_history_free(&first);
    snag_history_free(&second);
    assert(unlink(path) == 0 && rmdir(first_dir) == 0);
    assert(snprintf(path, sizeof(path), "%s/prompt_history", second_dir) > 0);
    assert(unlink(path) == 0 && rmdir(second_dir) == 0);
    assert(snprintf(path, sizeof(path), "%s/prompt_history", temp) > 0);
    assert(unlink(path) == 0 && rmdir(temp) == 0);
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
    assert(snag_history_merge(&term) == 0);
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
    struct snag_history_reader reader = {.fd = {-1, -1}};
    snag_history_reader_open(&reader, &term.snapshot);
    struct snag_history_cursor cursor = snag_history_end(&term.snapshot), end;
    const char *entry_text;
    assert(
        snag_history_read(&reader, &term.snapshot, false, cursor, &cursor, &end, &entry_text) == 1);
    assert(reader.warning && strcmp(entry_text, "duplicate") == 0);
    snag_history_reader_close(&reader);
    assert(term.snapshot.count == 0u && history_records(&term, NULL, false) == 3u);
    assert(term_history_has(&term, sample));
    assert(history_records(&term, "duplicate", false) == 2u);
    assert(stat(path, &st) == 0 && (st.st_mode & 0777u) == 0600u);
    assert(snag_history_merge(&term) == 0);
    snag_history_free(&term);

    assert(unlink(path) == 0);
    for (unsigned int process = 0u; process < 2u; ++process) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            struct snag_history writer;
            int rc = 0;
            memset(&writer, 0, sizeof(writer));
            if (snag_history_open(&writer, temp) < 0) rc = 1;
            for (unsigned int i = 0u; !rc && i < 10u; ++i) {
                (void)snprintf(entry, sizeof(entry), "child-%u-%u", process, i);
                if (snag_history_add(&writer, entry) < 0) rc = 1;
            }
            if (!rc && snag_history_merge(&writer) < 0) rc = 1;
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
    assert(term.snapshot.count == 0u && history_records(&term, NULL, false) == 20u);
    for (unsigned int process = 0u; process < 2u; ++process)
        for (unsigned int i = 0u; i < 10u; ++i) {
            (void)snprintf(entry, sizeof(entry), "child-%u-%u", process, i);
            assert(term_history_has(&term, entry));
        }
    assert(snprintf(subdir, sizeof(subdir), "%s/long-session", temp) > 0);
    assert(mkdir(subdir, 0700) == 0 && snag_history_bind(&term, subdir) == 0);
    for (unsigned int i = 0u; i < 205u; ++i) {
        (void)snprintf(entry, sizeof(entry), "unbounded-%03u", i);
        assert(snag_history_add(&term, entry) == 0);
    }
    assert(term.snapshot.count == 0u);
    assert(history_records(&term, NULL, true) == 205u);
    assert(term_history_has(&term, "unbounded-000"));
    assert(snag_history_merge(&term) == 0);
    snag_history_free(&term);
    assert(snag_history_open(&term, temp) == 0);
    assert(history_records(&term, NULL, false) == 225u);
    snag_history_free(&term);
    char local[4096];
    assert(snprintf(local, sizeof(local), "%s/prompt_history", subdir) > 0);
    assert(unlink(local) == 0 && rmdir(subdir) == 0);
    assert(unlink(path) == 0);

    assert(snprintf(subdir, sizeof(subdir), "%s/symlink", temp) > 0);
    assert(mkdir(subdir, 0700) == 0);
    assert(snprintf(path, sizeof(path), "%s/prompt_history", subdir) > 0);
    assert(symlink("../target", path) == 0);
    memset(&term, 0, sizeof(term));
    assert(snag_history_open(&term, subdir) < 0);
    assert(snag_history_take_warning(&term));
    assert(snag_history_merge(&term) == 0);
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
        assert(snag_history_merge(&term) == 0);
        snag_history_free(&term);
        assert(unlink(path) == 0 && rmdir(subdir) == 0);
    }

    assert(snprintf(subdir, sizeof(subdir), "%s/nonregular", temp) > 0);
    assert(mkdir(subdir, 0700) == 0);
    assert(snprintf(path, sizeof(path), "%s/prompt_history", subdir) > 0);
    assert(mkdir(path, 0700) == 0);
    memset(&term, 0, sizeof(term));
    assert(snag_history_open(&term, subdir) < 0);
    assert(snag_history_merge(&term) == 0);
    snag_history_free(&term);
    assert(rmdir(path) == 0 && rmdir(subdir) == 0);

    assert(snprintf(subdir, sizeof(subdir), "%s/oversized", temp) > 0);
    assert(mkdir(subdir, 0700) == 0);
    assert(snprintf(path, sizeof(path), "%s/prompt_history", subdir) > 0);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)(SNAG_HISTORY_BYTES * 5u + 1u)) == 0);
    assert(close(fd) == 0);
    memset(&term, 0, sizeof(term));
    assert(snag_history_open(&term, subdir) == 0);
    assert(term.snapshot.count == 0u);
    assert(snag_history_merge(&term) == 0);
    snag_history_free(&term);
    assert(unlink(path) == 0 && rmdir(subdir) == 0);
    assert(rmdir(temp) == 0);
}

static void
test_history_hold_count_uncapped(void)
{
    char temp[] = "/tmp/snajpagent-history-hold-XXXXXX";
    char path[256], entry[64];
    struct snag_history history = {0};

    assert(mkdtemp(temp));
    assert(snprintf(path, sizeof(path), "%s/prompt_history", temp) > 0);
    assert(snag_history_open(&history, temp) == 0);
    /* Unbound entries stay in memory; past the former 100-entry ceiling. */
    for (unsigned int i = 0u; i < 150u; ++i) {
        (void)snprintf(entry, sizeof(entry), "held-%03u", i);
        assert(snag_history_add(&history, entry) == 0);
    }
    assert(history.snapshot.count == 150u);
    assert(!snag_history_take_warning(&history));
    snag_history_free(&history);
    assert(unlink(path) == 0);
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
test_submit_holds_composer_until_activity(void)
{
    struct snag_term term;
    struct output_capture capture;
    enum snag_term_action action;
    char *text = NULL;
    char output[8192];
    char prompt[] = {
        'x', (char)(SNAG_TERM_SPINNER_MARKER_BASE + SNAG_TERM_SPINNER_PROVIDER), '>', ' ', '\0'};
    const char *spinners[SNAG_TERM_SPINNER_COUNT] = {
        " \u2691", " \u25f4\u25f7\u25f6\u25f5", " \u280b\u2819"};

    snag_term_init(&term);
    term.opened = term.capable = term.raw = true;
    term.columns = 80u;
    assert(snag_term_set_prompt_template(&term, false, prompt, spinners, 8u, 0u) == 0);

    /* A submission holds the composer: while the turn has reported no activity,
     * no composed label is written - otherwise the inactive glyph paints a
     * ready-looking prompt under the line just submitted. */
    assert(snag_term_restore_draft(&term, "held") == 0);
    capture = capture_open(false, true);
    term.input[0] = '\r';
    term.input_len = 1u;
    assert(snag_term_poll(&term, 0, -1, &action, &text) == 1);
    assert(action == SNAG_TERM_SUBMIT && text && strcmp(text, "held") == 0);
    free(text);
    text = NULL;
    term.input_pos = term.input_len = 0u;
    (void)capture_close(&capture, output, sizeof(output), 0u);
    assert(strstr(output, "x >") == NULL);
    assert(strstr(output, "\u25f4") == NULL);

    /* Spinner activity alone is not readiness. Only the engine's explicit
     * prompt acknowledgement releases the hold. */
    capture = capture_open(false, true);
    assert(snag_term_set_spinner_states(&term, 1u << SNAG_TERM_SPINNER_PROVIDER) == 0);
    assert(snag_term_set_prompt_template(
               &term, true, prompt, spinners, 8u, 1u << SNAG_TERM_SPINNER_PROVIDER) == 0);
    (void)capture_close(&capture, output, sizeof(output), 0u);
    assert(strstr(output, "\u25f4") == NULL);
    term.submit_awaiting_activity = false;
    capture = capture_open(false, true);
    assert(snag_term_set_prompt_template(
               &term, true, prompt, spinners, 8u, 1u << SNAG_TERM_SPINNER_PROVIDER) == 0);
    (void)capture_close(&capture, output, sizeof(output), 0u);
    assert(strstr(output, "\u25f4") != NULL);

    /* Time and a later prompt configuration cannot release a held action. */
    assert(snag_term_set_spinner_states(&term, 0u) == 0);
    assert(snag_term_restore_draft(&term, "again") == 0);
    capture = capture_open(false, true);
    term.input[0] = '\r';
    term.input_len = 1u;
    assert(snag_term_poll(&term, 0, -1, &action, &text) == 1);
    free(text);
    text = NULL;
    term.input_pos = term.input_len = 0u;
    (void)capture_close(&capture, output, sizeof(output), 0u);
    assert(strstr(output, "x >") == NULL);
    struct timespec past = {.tv_sec = 0, .tv_nsec = 300000000L};
    assert(nanosleep(&past, NULL) == 0);
    capture = capture_open(false, true);
    assert(snag_term_set_prompt_template(&term, false, prompt, spinners, 8u, 0u) == 0);
    (void)capture_close(&capture, output, sizeof(output), 0u);
    assert(strstr(output, "x >") == NULL);
    term.submit_awaiting_activity = false;
    capture = capture_open(false, true);
    assert(snag_term_set_prompt_template(&term, false, prompt, spinners, 8u, 0u) == 0);
    (void)capture_close(&capture, output, sizeof(output), 0u);
    assert(strstr(output, "x >") != NULL);

    snag_term_close(&term);
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
    assert(snag_term_set_prompt_template(&term, false, prompt, spinners, 8u, 0u) == 0);
    assert(strcmp(term.label, "x >") == 0);
    assert(term.animation.frames[SNAG_TERM_SPINNER_GOAL].inactive_len == 0u);
    assert(term.animation.frames[SNAG_TERM_SPINNER_PROVIDER].inactive_len == 1u);
    assert(term.animation.frames[SNAG_TERM_SPINNER_TOOL].inactive_len == 0u);
    assert(snag_term_set_spinner_states(&term, 1u << SNAG_TERM_SPINNER_GOAL) == 0);
    assert(strcmp(term.label, "x◆ >") == 0);
    assert(snag_term_set_spinner_states(&term, 1u << SNAG_TERM_SPINNER_PROVIDER) == 0);
    assert(strcmp(term.label, "x|>") == 0);
    memcpy(saved, term.label, sizeof(saved));
    assert(snag_term_set_prompt_template(&term, false, prompt, bad, 8u, 0u) < 0);
    assert(memcmp(saved, term.label, sizeof(saved)) == 0);
    assert(term.animation.states == (1u << SNAG_TERM_SPINNER_PROVIDER));
    assert(snag_term_set_prompt_template(
               &term, false, prompt, spinners, 8u, 1u << SNAG_TERM_SPINNER_COUNT) < 0);
    memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized) - 3u] = (char)SNAG_TERM_SPINNER_MARKER_BASE;
    oversized[sizeof(oversized) - 2u] = '\0';
    assert(snag_term_set_prompt_template(&term, false, oversized, wide, 8u, 0u) < 0);
    assert(memcmp(saved, term.label, sizeof(saved)) == 0);
    {
        /* Frame capacity follows the spinner string; the former 16-frame cap
         * is gone, so a longer sequence is accepted and fully retained. */
        const char *many[SNAG_TERM_SPINNER_COUNT] = {
            "\\0abcdefghijklmnopqrstuvwxyz0123", " ", "\\0"};

        assert(snag_term_set_prompt_template(&term, false, prompt, many, 8u, 0u) == 0);
        assert(term.animation.frames[SNAG_TERM_SPINNER_GOAL].frame_count == 30u);
        assert(snag_term_set_spinner_states(&term, 1u << SNAG_TERM_SPINNER_GOAL) == 0);
        assert(strcmp(term.label, "xa >") == 0);
    }
    {
        const char padded[] = "\xfd\xfe  9%> ";
        const char *stable[] = {" ⚑", " P", " ⠋"};
        const char *compact[] = {"\\0◆", "\\0P", "\\0T"};
        const char *blank[] = {" ", "\\0 P", "\\0"};

        for (unsigned int state = 0u; state < 8u; ++state) {
            assert(snag_term_set_prompt_template(&term, true, padded, stable, 8u, state) == 0);
            char expected[32];

            assert(snprintf(expected, sizeof(expected), "%s%s  9%%> ", state & 1u ? "⚑" : " ",
                       state & 4u   ? "⠋"
                       : state & 2u ? "P"
                                    : " ") > 0);
            assert(strcmp(term.label, expected) == 0);
            assert(snag_term_text_width(term.label, strlen(term.label)) == 8u);
            assert(snag_term_set_spinner_states(&term, 2u) == 0);
            assert(strcmp(term.label, " P  9%> ") == 0);
            assert(snag_term_set_spinner_states(&term, 6u) == 0);
            assert(strcmp(term.label, " ⠋  9%> ") == 0);
            assert(snag_term_set_spinner_states(&term, 0u) == 0);
            assert(strcmp(term.label, "    9%> ") == 0);
        }
        assert(snag_term_set_prompt_template(&term, true, padded, compact, 8u, 0u) == 0);
        assert(strcmp(term.label, "  9%> ") == 0);
        assert(snag_term_set_spinner_states(&term, 2u) == 0);
        assert(strcmp(term.label, "P  9%> ") == 0);
        assert(snag_term_set_spinner_states(&term, 4u) == 0);
        assert(strcmp(term.label, "T  9%> ") == 0);
        assert(snag_term_set_prompt_template(&term, true, padded, blank, 8u, 2u) == 0);
        assert(strcmp(term.label, "    9%> ") == 0);
        assert(snag_term_set_spinner_states(&term, 0u) == 0);
        assert(strcmp(term.label, "   9%> ") == 0);
        assert(snag_term_set_prompt_template(&term, true, "  9%> ", compact, 8u, 7u) == 0);
        assert(strcmp(term.label, "  9%> ") == 0);
    }
    {
        const char *frames[] = {" ⚑", " P", " ⠋"};
        char output[512];

        /* The longest activity variant must fit even while the slot is idle. */
        memset(oversized, 'x', sizeof(oversized));
        oversized[sizeof(oversized) - 3u] = (char)0xfe;
        oversized[sizeof(oversized) - 2u] = '\0';
        assert(snag_term_set_prompt_template(&term, false, oversized, frames, 8u, 0u) < 0);
        assert(snag_term_set_prompt_template(&term, true, "\xfd\xfe  9%> ", frames, 8u, 2u) == 0);
        assert(snag_buf_append(&term.draft, "draft", 5u) == 0);
        term.cursor = term.draft.len;
        term.columns = 80u;
        term.opened = term.capable = true;
        struct output_capture capture = capture_open(false, true);
        assert(snag_term_set_prompt_template(&term, true, "\xfd\xfe  9%> ", frames, 8u, 2u) == 0);
        assert(read(capture.fd, output, sizeof(output)) > 0);
        assert(snag_term_set_spinner_states(&term, 6u) == 0);
        assert(snag_term_set_spinner_states(&term, 2u) == 0);
        assert(term.cursor == 5u && term.rendered_cursor_col == 13u);
        assert(capture_close(&capture, output, sizeof(output), 0u) > 0u);
        assert(strstr(output, "⠋") && strstr(output, "P"));
        assert(!strstr(output, "9%") && !strstr(output, "draft"));
        assert(!strstr(output, "\033[2K") && !strchr(output, '\n'));
        term.prompt_visible = false;
    }
    term.opened = false;
    term.animation.tool_delay_ms = 500u;
    const char *held[] = {" G", " P", " T"};
    assert(snag_term_set_prompt_template(&term, true, "\xfd\xfe> ", held, 8u, 6u) == 0);
    assert(snag_term_set_spinner_states(&term, 3u) == 0);
    assert(strcmp(term.label, "GT> ") == 0 && term.animation.states == 3u);
    uint64_t deadline = term.animation.tool_until;
    assert(snag_term_set_prompt_template(&term, false, "\xfd\xfe idle> ", held, 8u, 1u) == 0);
    assert(term.animation.tool_until == deadline && strstr(term.label, "GT"));
    assert(snag_term_set_spinner_states(&term, 5u) == 0 && !term.animation.tool_until);
    assert(snag_term_set_spinner_states(&term, 3u) == 0 && term.animation.tool_until >= deadline);
    term.animation.tool_until = 1u;
    assert(snag_term_set_prompt_template(&term, true, "\xfd\xfe> ", held, 8u, 3u) == 0);
    assert(strcmp(term.label, "GP> ") == 0);
    term.animation.tool_delay_ms = 0u;
    assert(snag_term_set_spinner_states(&term, 6u) == 0);
    assert(snag_term_set_spinner_states(&term, 2u) == 0);
    assert(strcmp(term.label, " P> ") == 0 && !term.animation.tool_until);
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
editor_input(struct snag_term *term, const char *bytes)
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

static int
unexpected_native_suspend(void *opaque)
{
    (void)opaque;
    assert(false);
    return -1;
}

static void
test_editor_grapheme_deletion(void)
{
    struct snag_term term;
    snag_term_init(&term);
    term.input_only = term.capable = true;
    assert(snag_term_restore_draft(&term, "é👩‍💻X") == 0);
    editor_input(&term, "\033[D\177");
    assert(term.draft.len == strlen("éX"));
    assert(memcmp(term.draft.data, "éX", term.draft.len) == 0);
    editor_input(&term, "\001\033[3~");
    assert(term.draft.len == 1u && term.draft.data[0] == 'X');
    snag_term_close(&term);
}

static void
test_editor_literal_paste(void)
{
    struct snag_term term;
    snag_term_init(&term);
    term.input_only = term.capable = true;
    assert(snag_term_restore_draft(&term, "headtail") == 0);
    term.cursor = 4u;
    editor_input(&term, "\033[200~first\rsecond");
    assert(term.draft.len == 8u);
    editor_input(&term, "\033[201~");
    assert(term.draft.len == strlen("headfirst\nsecondtail"));
    assert(!memcmp(term.draft.data, "headfirst\nsecondtail", term.draft.len));
    editor_input(&term, "\033[200~unfinished");
    enum snag_term_action action = SNAG_TERM_NONE;
    char *text = NULL;
    term.input[0] = 0x03u;
    term.input_pos = 0u;
    term.input_len = 1u;
    assert(snag_term_poll(&term, 0, -1, &action, &text) == 1);
    assert(action == SNAG_TERM_CANCEL && !text && !term.paste && !term.draft.len);
    assert(!term.paste_text.len);
    assert(snag_buf_reserve(&term.draft, SNAG_MAX_DIRECT_PROMPT) == 0);
    memset(term.draft.data, 'x', SNAG_MAX_DIRECT_PROMPT - 1u);
    term.cursor = term.draft.len = SNAG_MAX_DIRECT_PROMPT - 1u;
    editor_input(&term, "\033[200~é\033[201~");
    assert(term.draft.len == SNAG_MAX_DIRECT_PROMPT - 1u && term.draft_clamped);
    assert(snag_utf8_valid(term.draft.data, term.draft.len, true));
    snag_term_close(&term);
}

static void
test_history_refresh_cursor(void)
{
    struct snag_term term;
    struct snag_history_snapshot snapshot = {0};
    struct output_capture capture = capture_open(false, true);
    char output[8192];

    snag_term_init(&term);
    term.opened = term.raw = term.capable = true;
    term.columns = 24u;
    memcpy(term.label, "> ", 3u);
    snapshot.items = calloc(1u, sizeof(*snapshot.items));
    assert(snapshot.items);
    snapshot.items[0] = strdup("first\nsecond");
    assert(snapshot.items[0]);
    snapshot.count = snapshot.capacity = 1u;
    snapshot.bytes = 12u;
    assert(snag_term_history_set(&term, &snapshot, false) == 0);
    assert(snag_term_restore_draft(&term, "unsent") == 0);
    editor_input(&term, "\020\033OA");
    assert(term.cursor == 5u);
    size_t preferred = term.preferred_column;
    assert(snag_history_snapshot_copy(&snapshot, &term.history) == 0);
    /* The engine's refresh can arrive between two cursor keys. */
    assert(snag_term_history_set(&term, &snapshot, true) == 0);
    assert(term.cursor == 5u && term.preferred_column == preferred);
    editor_input(&term, "\033OBY");
    assert(term.draft.len == 13u && !memcmp(term.draft.data, "first\nsecondY", 13u));

    editor_input(&term, "\025\020\033OA");
    assert(snag_history_snapshot_copy(&snapshot, &term.history) == 0);
    free(snapshot.items[0]);
    snapshot.items[0] = strdup("new");
    assert(snapshot.items[0]);
    snapshot.bytes = 3u;
    assert(snag_term_history_set(&term, &snapshot, true) == 0);
    assert(term.cursor == 3u && term.draft.len == 3u);
    assert(!memcmp(term.draft.data, "new", 3u));
    snag_term_close(&term);
    (void)capture_close(&capture, output, sizeof(output), 0u);
}

static void
test_native_input_yield(void)
{
    struct snag_term term;
    enum snag_term_action action;
    char *text = NULL;

    char output[4096];
    struct output_capture capture = capture_open(false, true);
    snag_term_init(&term);
    term.opened = term.capable = true;
    term.columns = 32u;
    assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
    const char *frames[SNAG_TERM_SPINNER_COUNT] = {"*", "|/-", "."};
    assert(snag_term_set_prompt_template(&term, true, "  9%> ", frames, 8u, 0u) == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
    term.suspend = unexpected_native_suspend;
    memcpy(term.input, "abc", 3u);
    term.input_len = 3u;
    for (size_t i = 1u; i <= 3u; ++i) {
        assert(snag_term_poll(&term, 0, -1, &action, &text) == 0);
        assert(action == SNAG_TERM_NONE && !text);
        assert(term.input_pos == i && term.draft.len == i);
        /* Geometry is serviced per byte; only the complete edit is painted. */
        size_t painted = prompt_output(capture.fd, output, sizeof(output));
        assert(i == 3u ? painted > 0u : painted == 0u);
    }
    /* Output checkpoints retain burst admission without recursive painting. */
    term.input_only = true;
    editor_input(&term, "def");
    assert(term.draft.len == 6u && !memcmp(term.draft.data, "abcdef", 6u));
    term.input_only = false;
    assert(prompt_output(capture.fd, output, sizeof(output)) == 0u);
    snag_term_close(&term);
    (void)capture_close(&capture, output, sizeof(output), 0u);
}

struct resize_input {
    struct snag_term *term;
    int output;
    unsigned int reads;
    unsigned int checkpoints;
    char *submitted;
};

static int
resize_input_status(void *opaque)
{
    struct resize_input *input = opaque;
    return input->reads < 2u ? SNAG_TERM_WAIT_INPUT : 0;
}

static ssize_t
resize_input_read(void *opaque, void *buffer, size_t size)
{
    struct resize_input *input = opaque;
    const char *bytes = input->reads ? "nections 1\r" : "\025/con";
    size_t length = strlen(bytes);
    assert(input->reads < 2u && size >= length);
    memcpy(buffer, bytes, length);
    if (!input->reads++) snag_term_notify_resize();
    return (ssize_t)length;
}

static int
resize_input_checkpoint(void *opaque)
{
    struct resize_input *input = opaque;
    char bytes[4096];
    while (read(input->output, bytes, sizeof(bytes)) > 0) {
    }
    assert(errno == EAGAIN);
    ++input->checkpoints;
    enum snag_term_action action;
    char *text = NULL;
    int rc = snag_term_poll(input->term, 0, -1, &action, &text);
    assert(rc >= 0);
    if (rc > 0) {
        assert(action == SNAG_TERM_SUBMIT && !input->submitted);
        input->submitted = text;
    } else
        assert(!text);
    return 0;
}

static void
test_resize_checkpoint_preserves_newly_read_input(void)
{
    struct output_capture capture = capture_open(false, true);
    struct snag_term term;
    snag_term_init(&term);
    term.opened = term.capable = term.prompt_visible = term.prompt_wanted = true;
    term.columns = 80u;
    term.rendered_rows = 1u;
    struct resize_input input = {.term = &term, .output = capture.fd};
    snag_term_input_redirect(&term.host, resize_input_status, resize_input_read, &input);
    term.input_checkpoint = resize_input_checkpoint;
    term.input_opaque = &input;
    snag_term_output_bind(&term);
    assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
    assert(fcntl(STDERR_FILENO, F_SETFL, O_NONBLOCK) == 0);
    char output[4096] = {0};
    while (write(STDERR_FILENO, output, sizeof(output)) > 0) {
    }
    assert(errno == EAGAIN);
    for (unsigned int attempt = 0u; !input.submitted && attempt < 3u; ++attempt) {
        enum snag_term_action action;
        char *text = NULL;
        int rc = snag_term_poll(&term, 0, -1, &action, &text);
        assert(rc >= 0);
        if (rc > 0) {
            assert(action == SNAG_TERM_SUBMIT && !input.submitted);
            input.submitted = text;
        } else
            assert(!text);
    }
    term.input_checkpoint = NULL;
    term.opened = false;
    snag_term_close(&term);
    (void)capture_close(&capture, output, sizeof(output), 0u);
    assert(input.checkpoints && input.submitted);
    assert(!strcmp(input.submitted, "/connections 1"));
    free(input.submitted);
}

static void
test_retained_prompt(void)
{
    struct snag_term term;
    const char *frames[SNAG_TERM_SPINNER_COUNT] = {" ⚑", " |/-", " ⠋⠙"};
    char output[4096];
    struct output_capture capture = capture_open(false, true);

    snag_term_init(&term);
    term.opened = term.capable = true;
    term.columns = 24u;
    assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
    assert(snag_term_set_prompt_template(&term, true, "  9%> ", frames, 8u, 0u) == 0);
    assert(snag_term_restore_draft(&term, "first row stays\nsecond row stays") == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
    assert(snag_term_set_prompt_template(&term, true, "  9%> ", frames, 8u, 0u) == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) == 0u);
    size_t cursor_row = term.rendered_cursor_row, cursor_col = term.rendered_cursor_col;
    assert(snag_term_set_prompt_template(&term, true, " 10%> ", frames, 8u, 0u) == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
    assert(strstr(output, "10") && !strstr(output, "stays") && !strchr(output, '\n'));
    assert(!strstr(output, "\033[K") && !strstr(output, "\033[2K"));
    assert(term.rendered_cursor_row == cursor_row && term.rendered_cursor_col == cursor_col);
    assert(snag_term_restore_draft(&term, "first row stays\nsecond row short") == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
    assert(!strstr(output, "first") && !strstr(output, "10%"));
    assert(snag_term_restore_draft(&term, "small") == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
    assert(strstr(output, "\033[K") && !strstr(output, "\033[2K"));
    assert(term.rendered_rows == 1u && term.rendered_cursor_col == 11u);
    assert(snag_term_restore_draft(&term, "invalid:\xff") == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
    assert(strstr(output, "invalid:\\xFF"));
    assert(snag_term_set_prompt_template(&term, true, "two\nlines> ", frames, 8u, 0u) == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
    assert(strstr(output, "two\\nlines> "));
    assert(snag_term_set_prompt_template(&term, true, " 10%> ", frames, 8u, 0u) == 0);
    (void)prompt_output(capture.fd, output, sizeof(output));
    char multiline[141];
    memset(multiline, '\n', sizeof(multiline) - 1u);
    multiline[sizeof(multiline) - 1u] = '\0';
    assert(snag_term_restore_draft(&term, multiline) == 0);
    (void)prompt_output(capture.fd, output, sizeof(output));
    assert(snag_term_restore_draft(&term, "") == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
    assert(term.rendered_rows == 1u);

    assert(snag_term_restore_draft(&term, "café界 tail") == 0);
    (void)prompt_output(capture.fd, output, sizeof(output));
    assert(snag_term_restore_draft(&term, "cafè界 tail") == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
    assert(strstr(output, "è") && !strstr(output, "caf") && !strstr(output, "tail"));
    assert(snag_term_restore_draft(&term, "cafè語 tail") == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
    assert(strstr(output, "語") && !strstr(output, "tail"));
    assert(snag_term_restore_draft(&term, "aaaaaaaaaaaaaaaaa界") == 0);
    assert(term.rendered_rows == 2u && term.rendered_cursor_col == 19u);
    assert(snag_term_restore_draft(&term, "aaaaaaaaaaaaaaaaaa\nnext") == 0);
    assert(term.rendered_rows == 2u && term.rendered_cursor_col == 10u);
    assert(snag_term_restore_draft(&term, "cafè語 tail") == 0);
    (void)prompt_output(capture.fd, output, sizeof(output));
    snag_term_set_color(&term, true);
    assert(snag_term_set_prompt_template(&term, true, " 10%> ", frames, 8u, 0u) == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
    assert(strstr(output, "\033[1;36m") && !strstr(output, "tail"));
    snag_term_set_color(&term, false);
    assert(snag_term_set_prompt_template(&term, true, " 10%> ", frames, 8u, 0u) == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
    assert(!strstr(output, "tail"));

    assert(snag_term_restore_draft(&term, "") == 0);
    assert(snag_term_set_prompt_template(&term, true, "\xfd\xfe> ", frames, 8u, 2u) == 0);
    (void)prompt_output(capture.fd, output, sizeof(output));
    term.animation.epoch -= 125u;
    uint64_t epoch = term.animation.epoch;
    assert(snag_term_set_prompt_template(&term, true, "\xfd\xfe> ", frames, 8u, 2u) == 0);
    assert(term.animation.epoch == epoch);
    assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
    assert(strchr(output, '/') && !strchr(output, '>'));
    assert(snag_term_set_prompt_template(&term, true, "\xfd\xfe> ", frames, 8u, 2u) == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) == 0u);

    assert(snag_term_output_begin(&term) == 0);
    assert(snag_term_note_output(&term, "public", 6u, "") == 0);
    assert(snag_term_output_end(&term) == 0);
    (void)prompt_output(capture.fd, output, sizeof(output));
    assert(term.prompt_visible && term.painted_prompt.len != 0u);
    enum snag_term_action action;
    char *text;
    int input[2], stdin_fd = dup(STDIN_FILENO);
    assert(stdin_fd >= 0 && pipe(input) == 0 && dup2(input[0], STDIN_FILENO) >= 0);
    assert(snag_term_poll(&term, 20, -1, &action, &text) == 0);
    assert(term.prompt_visible && prompt_output(capture.fd, output, sizeof(output)) == 0u);
    editor_input(&term, "x");
    assert(term.prompt_visible && prompt_output(capture.fd, output, sizeof(output)) > 0u);
    assert(snag_term_output_begin(&term) == 0);
    assert(snag_term_note_output(&term, "more", 4u, "") == 0);
    assert(snag_term_output_end(&term) == 0);
    (void)prompt_output(capture.fd, output, sizeof(output));
    assert(term.prompt_visible);
    assert(snag_term_poll(&term, 0, -1, &action, &text) == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) == 0u);
    assert(snag_term_set_prompt_template(&term, false, "cursor> ", frames, 8u, 0u) == 0);
    assert(snag_term_restore_draft(&term, "unchanged") == 0);
    (void)prompt_output(capture.fd, output, sizeof(output));
    const char *moves[] = {"\033[H", "\033[F", "\033[D"};
    const size_t columns[] = {8u, 17u, 16u};
    for (size_t i = 0u; i < 3u; ++i) {
        editor_input(&term, moves[i]);
        assert(term.rendered_cursor_col == columns[i] && term.rendered_cursor_row == 0u);
        assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
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
    assert(snag_term_set_prompt_template(&term, true, "failure> ", frames, 8u, 0u) < 0);
    assert(term.painted_prompt.len == 0u);
    capture_restore(&capture);
    close(capture.fd);
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
        {"agent\n", "@missing", NULL, 8u, 8u},
        {"", "@", NULL, 1u, 1u},
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
            struct snag_irc_destinations destinations = {.count = 1u};

            snag_term_init(&term);
            term.chat = term.conversation_tabs = term.blank_local = true;
            term.active = active != 0u;
            destinations.items[0].target = (struct snag_irc_target){1u, 1u};
            destinations.items[0].joined = true;
            strcpy(destinations.items[0].nicks, cases[i].nicks);
            assert(snag_term_set_destinations(&term, &destinations) == 0);
            assert(snag_buf_append(&term.draft, cases[i].draft, strlen(cases[i].draft)) == 0);
            term.cursor = cases[i].cursor;
            enum snag_term_action action;
            char *text = NULL;
            term.input[0] = '\t';
            term.input_len = 1u;
            bool view = !cases[i].expected;
            assert(snag_term_poll(&term, 0, -1, &action, &text) == (view ? 1 : 0));
            assert(action == (view ? SNAG_TERM_VIEW : SNAG_TERM_NONE) && !text);
            const char *expected = view ? cases[i].draft : cases[i].expected;
            assert(term.draft.len == strlen(expected));
            assert(memcmp(term.draft.data, expected, term.draft.len) == 0);
            assert(term.cursor == cases[i].result_cursor);
            snag_term_close(&term);
        }
    }
}

static void
test_completion_choices(void)
{
    static const struct snag_term_command commands[] = {{"/connect ENDPOINT", NULL},
        {"/config FILE", NULL}, {"/compact", NULL}, {"/help", NULL}, {"/help", NULL}};
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
    struct output_capture capture = capture_open(false, true);
    assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
    for (unsigned int flags = 0u; flags < 4u; ++flags) {
        for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            struct snag_term term;
            char output[4096];
            struct snag_irc_destinations destinations = {.count = 2u,
                .items = {
                    {.target = {.id = 12u},
                        .joined = true,
                        .nicks = "agent1\nagent2\nčenda\nčerven\n"},
                    {.target = {.id = 17u}, .joined = true, .nicks = "Agent1\nagent2\n"},
                }};
            snag_term_init(&term);
            term.chat = true;
            term.active = (flags & 1u) != 0u;
            term.columns = flags & 2u ? 10u : 80u;
            snag_term_set_commands(&term, commands, sizeof(commands) / sizeof(commands[0]));
            assert(snag_term_set_destinations(&term, &destinations) == 0);
            assert(snag_term_restore_draft(&term, cases[i].draft) == 0);
            editor_input(&term, "\t");
            assert(term.draft.len == strlen(cases[i].common));
            assert(memcmp(term.draft.data, cases[i].common, term.draft.len) == 0);
            assert(prompt_output(capture.fd, output, sizeof(output)) == 0u);
            size_t cursor = term.cursor;
            editor_input(&term, "\t");
            assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
            assert(strstr(output, cases[i].first) && strstr(output, cases[i].second));
            assert(count_text(output, cases[i].first) == 1u);
            assert(!strstr(output, "ENDPOINT") && !strstr(output, "FILE"));
            assert(term.cursor == cursor && term.draft.len == strlen(cases[i].common));
            assert(memcmp(term.draft.data, cases[i].common, term.draft.len) == 0);
            /* Cursor movement breaks the consecutive-Tab sequence. */
            editor_input(&term, "\033[D\033[C\t");
            assert(prompt_output(capture.fd, output, sizeof(output)) == 0u);
            /* Capture while output is stalled; listing is deferred, not lost. */
            term.input_only = true;
            editor_input(&term, "\t");
            assert(term.completion_output.len &&
                   prompt_output(capture.fd, output, sizeof(output)) == 0u);
            term.input_only = false;
            editor_input(&term, "x");
            assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
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
        editor_input(&term, "\t");
        const char *expected = i == 2u ? "/help tail" : "/help ";
        assert(term.draft.len == strlen(expected));
        assert(memcmp(term.draft.data, expected, term.draft.len) == 0);
        assert(term.cursor == 6u);
        assert(!term.completion_armed);
    }
    const char *submissions[] = {
        "/help ", "/17 ", "/model value ", "ordinary ", "//help ", "/help\n "};
    const char *submitted[] = {"/help", "/17", "/model value ", "ordinary ", "//help ", "/help\n "};
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
    capture_restore(&capture);
    close(capture.fd);
}

static void
test_dictation_editor(void)
{
    struct snag_term term;
    enum snag_term_action action;
    char *text = NULL;
    snag_term_init(&term);
    assert(snag_term_restore_draft(&term, "keep typed") == 0);
    assert(snag_term_audio(&term, "[mic] ", true) < 0); /* no capture on a pipe */
    int input[2], output[2], saved_in = dup(STDIN_FILENO), saved_out = dup(STDERR_FILENO);
    assert(saved_in >= 0 && saved_out >= 0 && pipe(input) == 0 && pipe(output) == 0);
    assert(dup2(input[0], STDIN_FILENO) >= 0 && dup2(output[1], STDERR_FILENO) >= 0);
    close(input[0]);
    close(output[1]);
    term.raw = term.opened = term.capable = term.active = true;
    term.columns = 160u;
    assert(snag_term_audio(&term, "[mic] ", true) == 0);
    assert(term.prompt_wanted);
    const char *keys[] = {"\r", "\003", "\t"};
    const enum snag_term_action expected[] = {
        SNAG_TERM_DICTATE_DONE, SNAG_TERM_DICTATE_CANCEL, SNAG_TERM_NONE};
    for (size_t i = 0; i < 3u; ++i) {
        term.input_pos = 0;
        term.input_len = 1;
        term.input[0] = (unsigned char)*keys[i];
        (void)snag_term_poll(&term, 0, -1, &action, &text);
        assert(action == expected[i] && !text && term.dictating);
        assert(term.draft.len == 10u && !memcmp(term.draft.data, "keep typed", 10u));
    }
    term.cursor = 5u;
    assert(snag_term_insert_draft(&term, "spoken ") == 0);
    assert(term.draft.len == 17u && !memcmp(term.draft.data, "keep spoken typed", 17u));
    assert(snag_term_insert_draft(&term, "\xff") < 0 && term.draft.len == 17u);
    term.input_pos = 0;
    term.input_len = 1;
    term.input[0] = 0x1bu;
    assert(snag_term_poll(&term, 0, -1, &action, &text) == 0);
    assert(snag_term_poll(&term, 0, -1, &action, &text) == 1);
    assert(action == SNAG_TERM_DICTATE_CANCEL && term.draft.len == 17u);
    assert(snag_term_audio(&term, "", false) == 0 && !term.dictating);
    assert(snag_term_audio(&term, "[voice mic] ", false) == 0);
    assert(snag_term_caption(&term, 0u, "spoken partial") == 0);
    assert(snag_term_caption(&term, 1u, "reply \033[2J\nnext") == 0);
    assert(term.cursor == 12u && term.draft.len == 17u && !term.dictating);
    assert(snag_buf_terminate(&term.painted_prompt) == 0);
    assert(strstr((char *)term.painted_prompt.data, "You [voice, partial]: spoken partial"));
    assert(strstr((char *)term.painted_prompt.data, "Voice model [generated]: reply"));
    assert(!strstr((char *)term.painted_prompt.data, "\033[2J"));
    assert(snag_term_caption(&term, 0u, "\xff") < 0);
    assert(!strcmp(term.caption[0], "spoken partial"));
    assert(snag_term_restore_draft(&term, "first\nsecond") == 0);
    assert(term.rendered_rows >= 4u);
    term.input_pos = 0;
    term.input_len = 1;
    term.input[0] = '\r';
    assert(snag_term_poll(&term, 0, -1, &action, &text) == 1);
    assert(action == SNAG_TERM_SUBMIT && text && !strcmp(text, "first\nsecond"));
    free(text);
    text = NULL;
    assert(snag_term_audio(&term, "", false) == 0);
    assert(!term.caption[0][0] && !term.caption[1][0]);
    term.raw = false;
    snag_term_close(&term);
    assert(dup2(saved_in, STDIN_FILENO) >= 0 && dup2(saved_out, STDERR_FILENO) >= 0);
    close(saved_in);
    close(saved_out);
    close(input[1]);
    close(output[0]);
}

static void
test_query_tab_keeps_modal_queue_action(void)
{
    for (unsigned int modal = 0u; modal < 2u; ++modal) {
        struct snag_term term;
        snag_term_init(&term);
        term.active = term.chat = term.conversation_tabs = true;
        term.blank_local = !modal;
        assert(snag_term_restore_draft(&term, "saved draft") == 0);
        term.input[0] = '\t';
        term.input_len = 1u;
        enum snag_term_action action;
        char *text = NULL;
        assert(snag_term_poll(&term, 0, -1, &action, &text) == 1);
        if (modal) {
            assert(action == SNAG_TERM_QUEUE && text && !strcmp(text, "saved draft"));
            assert(!term.draft.len);
        } else {
            assert(action == SNAG_TERM_VIEW && !text && term.draft.len == 11u);
        }
        free(text);
        snag_term_close(&term);
    }
}

static void
test_destination_editor(void)
{
    static const struct {
        const char *draft, *expected;
        bool chat;
    } cases[] = {{"@ag", "@agent2 ", true}, {"/17 @ag", "/17 @agent17 ", true},
        {"/17 @ag", "/17 @agent17 ", false}, {"/all @ag", "/all @agent", true},
        {"/9 @ag", NULL, true}, {"/2oops @ag", NULL, true}, {"/1", "/17 ", true}};
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
        snag_term_init(&term);
        term.chat = cases[i].chat;
        term.conversation_tabs = term.blank_local = true;
        term.active = true;
        assert(snag_term_set_destinations(&term, &destinations) == 0);
        assert(term.destination.id == 2u);
        assert(snag_term_restore_draft(&term, cases[i].draft) == 0);
        enum snag_term_action action;
        char *text = NULL;
        term.input[0] = '\t';
        term.input_len = 1u;
        bool view = !cases[i].expected;
        assert(snag_term_poll(&term, 0, -1, &action, &text) == (view ? 1 : 0));
        assert(action == (view ? SNAG_TERM_VIEW : SNAG_TERM_NONE) && !text);
        const char *expected = view ? cases[i].draft : cases[i].expected;
        assert(term.draft.len == strlen(expected));
        assert(memcmp(term.draft.data, expected, term.draft.len) == 0);
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
    strcpy(destinations.items[0].endpoint, "first:6667");
    assert(snag_term_set_destinations(&term, &destinations) == 0);
    term.conversation = (struct snag_irc_conversation_target){.kind = SNAG_IRC_QUERY,
        .identity = SNAG_IRC_OPERATOR,
        .endpoint = "first:6667",
        .peer = "peer",
        .conversation = "11111111111111111111111111111111"};
    snag_term_destination_prefix(&term, label, sizeof(label));
    assert(!strcmp(label, "[peer] "));
    term.conversation.identity = SNAG_IRC_AGENT;
    snag_term_destination_prefix(&term, label, sizeof(label));
    assert(!strcmp(label, "[peer; viewing model's chat] "));
    destinations.count = 2u;
    strcpy(destinations.items[1].endpoint, "second:6667");
    assert(snag_term_set_destinations(&term, &destinations) == 0);
    snag_term_destination_prefix(&term, label, sizeof(label));
    assert(!strcmp(label, "[first:6667/peer; viewing model's chat] "));
    memset(&term.conversation, 0, sizeof(term.conversation));
    destinations.count = 1u;
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
    editor_input(&term, "\r");
    assert(term.draft.len == 3u);
    snag_term_close(&term);
}

static size_t
capture(unsigned int verbosity, char *out, size_t out_size)
{
    struct snag_render render;

    struct output_capture capture = capture_open(false, true);
    snag_render_init(&render, verbosity);
    assert(snag_render_protocol(&render, "request JSON", "{\"x\":1}", 7u) == 0);
    assert(snag_render_protocol(&render, "response JSON", "{}", 2u) == 0);
    assert(snag_render_transport(&render, '>', "POST https://example.test", 25u) == 0);
    return capture_close(&capture, out, out_size, 0u);
}

static size_t
capture_orientation(bool resumed, char *out, size_t out_size)
{
    struct snag_render render;
    struct output_capture capture = capture_open(false, true);

    snag_render_init(&render, 0u);
    assert(snag_render_orientation(&render, "/work/tree", "0123456789abcdef0123456789abcdef", 3u,
               2u, resumed, false) == 0);
    return capture_close(&capture, out, out_size, 0u);
}

static size_t
drain_available(int fd, char *out, size_t out_size, size_t used)
{
    ssize_t n;

    while ((n = read(fd, out + used, out_size - used - 1u)) > 0) used += (size_t)n;
    assert(n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)));
    out[used] = '\0';
    return used;
}

static size_t
capture_wrapped(const char *first, const char *second, unsigned int columns, bool markdown,
    const char *first_output, const char *second_output, char *out, size_t out_size,
    struct snag_buf *delivered)
{
    struct snag_render render;
    struct snag_term term;
    size_t used = 0u;

    struct output_capture capture = capture_terminal(&render, &term, columns, true, false);
    assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
    snag_render_set_markdown(&render, markdown);
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snag_render_public(&render, first, strlen(first), delivered) == 0);
    used = drain_available(capture.fd, out, out_size, used);
    assert(!markdown || out[0] == '\n');
    assert(strcmp(out + (markdown ? 1u : 0u), first_output) == 0);
    assert(snag_render_public(&render, second, strlen(second), delivered) == 0);
    used = drain_available(capture.fd, out, out_size, used);
    assert(strcmp(out + (markdown ? 1u : 0u), second_output) == 0);
    assert(snag_render_public_end(&render) == 0);
    used = capture_close(&capture, out, out_size, used);
    snag_term_close(&term);
    return used;
}

static void
test_punctuation_wrapping(void)
{
    static const char *const punctuation[] = {"-", ",", ".", ";", ":", "!", "?", "/", ")", "]", "}",
        "..", "\")", "\xe2\x80\x90", "\xe2\x80\x92", "\xe2\x80\x93", "\xe2\x80\x94",
        "\xe2\x80\xa6"};
    static const char first[] = "1234567890 word";
    char second[32];
    char first_output[64];
    char second_output[128];
    char delivered_output[128];
    char output[256];

    for (size_t enabled = 0u; enabled < 2u; ++enabled) {
        const char *prefix = enabled ? "• " : "";

        assert(snprintf(first_output, sizeof(first_output), "%s%s", prefix, "1234567890") > 0);
        for (size_t i = 0u; i < sizeof(punctuation) / sizeof(punctuation[0]); ++i) {
            assert(snprintf(second, sizeof(second), "%smores", punctuation[i]) > 0);
            assert(snprintf(second_output, sizeof(second_output), "%s1234567890", prefix) > 0);
            assert(snprintf(delivered_output, sizeof(delivered_output), "%s%smores", first,
                       punctuation[i]) > 0);
            struct snag_buf delivered = {.max = sizeof(delivered_output)};
            assert(capture_wrapped(first, second, 20u, enabled != 0u, first_output, second_output,
                       output, sizeof(output), &delivered) > 0u);
            assert(snprintf(second_output, sizeof(second_output), "%s1234567890\n%sword%smores",
                       prefix, enabled ? "  " : "", punctuation[i]) > 0);
            assert(strstr(output, second_output));
            assert(snag_buf_terminate(&delivered) == 0);
            assert(strcmp((const char *)delivered.data, delivered_output) == 0);
            snag_buf_free(&delivered);
        }
        {
            assert(snprintf(first_output, sizeof(first_output), "%s1234567890", prefix) > 0);
            assert(snprintf(second_output, sizeof(second_output), "%s1234567890", prefix) > 0);
            struct snag_buf delivered = {.max = 32u};
            assert(capture_wrapped("1234567890 ", "-something", 20u, enabled != 0u, first_output,
                       second_output, output, sizeof(output), &delivered) > 0u);
            assert(snag_buf_terminate(&delivered) == 0);
            assert(strcmp((const char *)delivered.data, "1234567890 -something") == 0);
            snag_buf_free(&delivered);
        }
    }
}

static int
editable_checkpoint(void *opaque)
{
    const struct snag_term *term = opaque;
    assert(term->output_depth == 0u);
    return 0;
}

static size_t
capture_markdown(const char *text, bool enabled, bool split, enum snag_color_mode color,
    unsigned int columns, char *out, size_t out_size, struct snag_buf *delivered)
{
    struct snag_render render;
    struct snag_term term;
    size_t len = strlen(text);
    size_t used = 0u;

    struct output_capture capture = capture_terminal(&render, &term, columns, true, false);
    term.opened = term.defer_redraw = true;
    snag_render_set_color(&render, color);
    snag_render_set_markdown(&render, enabled);
    render.checkpoint = editable_checkpoint;
    render.checkpoint_opaque = &term;
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    if (split) {
        for (size_t i = 0u; i < len; ++i)
            assert(snag_render_public(&render, text + i, 1u, delivered) == 0);
    } else {
        assert(snag_render_public(&render, text, len, delivered) == 0);
    }
    assert(snag_render_public_end(&render) == 0);
    used = capture_close(&capture, out, out_size, used);
    snag_term_close(&term);
    return used;
}

static void
test_capable_terminal_hard_wrap(void)
{
    static const char source[] = "alpha beta gamma delta";
    struct snag_render render;
    struct snag_term term;
    struct snag_buf delivered = {.max = 1024u};
    char output[256];
    struct output_capture capture = capture_terminal(&render, &term, 20u, true, false);

    term.opened = term.capable = term.defer_redraw = true;
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    for (size_t i = 0u; i < sizeof(source) - 1u; ++i)
        assert(snag_render_public(&render, source + i, 1u, &delivered) == 0);
    assert(snag_render_public_end(&render) == 0);
    assert(capture_close(&capture, output, sizeof(output), 0u) > 0u);
    assert(strcmp(output, "\n• alpha beta gamma\n  delta\n\n") == 0);
    assert(snag_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data, source) == 0);
    snag_buf_free(&delivered);
    snag_term_close(&term);
}

static void
test_punctuation_word_boundaries(void)
{
    static const char *const sources[] = {
        "1234567890 WAL-like: next",
        "1234567890 **WAL-like**: next",
        "1234567890 `WAL-like`: next",
        "1234567890 record, next",
        "1234567890 **record**, next",
        "1234567890 café́界: next",
        "1234567890 *word*?! next",
        "1234567890 `word`). next",
    };
    static const char *const words[] = {
        "WAL-like:", "WAL-like:", "WAL-like:", "record,", "record,", "café́界:", "word?!", "word)."};
    char output[4096], plain[4096];

    for (size_t i = 0u; i < sizeof(sources) / sizeof(sources[0]); ++i) {
        for (unsigned int columns = 20u; columns <= 26u; ++columns) {
            for (size_t split = 0u; split < 2u; ++split) {
                for (size_t color = 0u; color < 2u; ++color) {
                    struct snag_buf delivered = {.max = 1024u};
                    assert(capture_markdown(sources[i], true, split != 0u,
                               color ? SNAG_COLOR_ALWAYS : SNAG_COLOR_NEVER, columns, output,
                               sizeof(output), &delivered) > 0u);
                    size_t used = 0u;
                    for (size_t at = 0u; output[at]; ++at) {
                        if (output[at] == '\033') {
                            while (output[at] && output[at] != 'm') ++at;
                            assert(output[at] == 'm');
                        } else {
                            plain[used++] = output[at];
                        }
                    }
                    plain[used] = '\0';
                    assert(strstr(plain, words[i]));
                    assert(!strstr(plain, "\n  :"));
                    assert(!strstr(plain, "\n  ,"));
                    if (color && i == 4u) assert(strstr(output, "\033[0;1mrecord\033[0m,"));
                    assert(snag_buf_terminate(&delivered) == 0);
                    assert(strcmp((const char *)delivered.data, sources[i]) == 0);
                    snag_buf_free(&delivered);
                }
            }
        }
    }
}

static void
test_bounded_wrap_word(void)
{
    for (unsigned int markdown = 0u; markdown < 2u; ++markdown) {
        struct snag_render render;
        struct snag_term term;
        char output[8192];
        struct output_capture capture = capture_terminal(&render, &term, 20u, true, false);
        assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
        snag_render_set_markdown(&render, markdown != 0u);
        assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
        /* Both display-width and zero-width byte bounds survive arbitrary
         * provider splits. A later word still starts at a real wrap boundary. */
        for (size_t i = 0u; i < 5000u; ++i) {
            assert(snag_render_public(&render, "a", 1u, NULL) == 0);
            assert(render.wrap_pending.len <= 21u);
            assert(render.wrap_pending.len == render.wrap_styles.len);
            (void)drain_available(capture.fd, output, sizeof(output), 0u);
        }
        for (size_t i = 0u; i < 3000u; ++i) {
            assert(snag_render_public(&render, "́", 2u, NULL) == 0);
            assert(render.wrap_pending.len < 4096u);
            assert(render.wrap_pending.len == render.wrap_styles.len);
            (void)drain_available(capture.fd, output, sizeof(output), 0u);
        }
        assert(snag_render_public(&render, " record, next", 13u, NULL) == 0);
        assert(snag_render_public_abort(&render) == 0);
        (void)capture_close(&capture, output, sizeof(output), 0u);
        assert(strstr(output, "record,"));
        assert(strstr(output, "next"));
        snag_term_close(&term);
    }
}

static void
test_banner_word_layout(void)
{
    struct snag_buf out = {.max = 4096u};
    const char *text = "first line\n1234567890123456789 completed";
    assert(snag_term_append_wrapped(&out, text, strlen(text), 28u) == 0);
    assert(snag_buf_terminate(&out) == 0);
    assert(!strcmp((char *)out.data, "first line\n1234567890123456789 \ncompleted"));
    snag_buf_reset(&out);
    text = "界界界界界界界界界界 completed";
    assert(snag_term_append_wrapped(&out, text, strlen(text), 28u) == 0);
    assert(snag_buf_terminate(&out) == 0);
    assert(strstr((char *)out.data, " \ncompleted"));
    snag_buf_reset(&out);
    text = "1234567890123456789 \033safe";
    assert(snag_term_append_wrapped(&out, text, strlen(text), 28u) == 0);
    assert(snag_buf_terminate(&out) == 0);
    assert(strstr((char *)out.data, "\x1b") == NULL);
    assert(strstr((char *)out.data, "\\x1Bsafe"));
    snag_buf_free(&out);
}

static void
test_help_emphasis(void)
{
    const char *help = "\nSession history\n/session — current session\n";
    struct snag_render render;
    struct snag_term term;
    char output[1024];
    struct output_capture capture = capture_terminal(&render, &term, 80u, false, true);
    snag_render_set_color(&render, SNAG_COLOR_ALWAYS);
    assert(snag_render_help(&render, help) == 0);
    assert(capture_close(&capture, output, sizeof(output), 0u) > 0u);
    assert(strstr(output, "\033[34m") == NULL);
    assert(strstr(output, "\033[1mSession history\n\033[0m"));
    assert(strstr(output, "/session — current session\n"));
    snag_render_free(&render);
    snag_term_close(&term);
}

static void
test_help_layout(void)
{
    const char *help = "/goal pause|resume — automatic continuation\n";
    const char *sources[] = {"streamed", "```c\nint", "**bold", "# heading"};
    const unsigned int widths[] = {20u, 28u, 40u, 80u, 120u};
    for (size_t w = 0u; w < sizeof(widths) / sizeof(widths[0]); ++w)
        for (size_t i = 0u; i < sizeof(sources) / sizeof(sources[0]); ++i) {
            struct snag_render render;
            struct snag_term term;
            struct snag_buf delivered = {.max = 1024u};
            char output[4096];
            struct output_capture capture = capture_open(false, true);
            snag_term_init(&term);
            term.columns = widths[w];
            assert(snag_term_restore_draft(&term, "keep draft") == 0);
            snag_render_init(&render, 0u);
            render.stderr_terminal = true;
            snag_render_set_color(&render, SNAG_COLOR_NEVER);
            snag_render_attach_term(&render, &term);
            assert(snag_render_public_begin(&render, STDERR_FILENO, NULL) == 0);
            assert(snag_render_public(&render, sources[i], strlen(sources[i]), &delivered) == 0);
            assert(snag_render_help(&render, help) == 0);
            assert(term.cursor == strlen("keep draft") && term.draft.len == term.cursor);
            assert(!memcmp(term.draft.data, "keep draft", term.cursor));
            assert(snag_render_public(&render, " suffix", 7u, &delivered) == 0);
            assert(snag_render_public_end(&render) == 0);
            assert(snag_buf_terminate(&delivered) == 0);
            assert(delivered.len == strlen(sources[i]) + 7u);
            assert(!memcmp(delivered.data, sources[i], strlen(sources[i])));
            assert(!strcmp((char *)delivered.data + strlen(sources[i]), " suffix"));
            assert(capture_close(&capture, output, sizeof(output), 0u) > 0u);
            assert(count_text(output, "continuation") == 1u);
            assert(count_text(output, "/goal") == 1u);
            snag_buf_free(&delivered);
            snag_render_free(&render);
            snag_term_close(&term);
        }
    struct snag_render render;
    char output[256];
    struct output_capture capture = capture_open(false, true);
    snag_render_init(&render, 0u);
    render.stderr_terminal = false;
    assert(snag_render_help(&render, help) == 0);
    assert(capture_close(&capture, output, sizeof(output), 0u) == strlen(help));
    assert(!strcmp(output, help));
    snag_render_free(&render);
}

static void
test_update_banner(void)
{
    const char *const sources[] = {"streamed", "```c\nint", "**bold", "# heading"};
    const char *banner = "=== snajpagent updated ===\nInstalled test. Restart when "
                         "convenient.\nChangelog: https://example.test/#changelog\n";
    const unsigned int widths[] = {20u, 28u, 40u, 80u, 120u};
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); ++w)
        for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
            struct snag_render render;
            struct snag_term term;
            struct snag_buf delivered;
            char output[4096];
            struct output_capture capture = capture_open(false, true);
            snag_term_init(&term);
            term.columns = widths[w];
            assert(snag_term_restore_draft(&term, "keep my draft") == 0);
            size_t cursor = term.cursor;
            snag_render_init(&render, 0u);
            render.stderr_terminal = true;
            snag_render_set_color(&render, SNAG_COLOR_NEVER);
            snag_render_attach_term(&render, &term);
            snag_buf_init(&delivered, 1024u);
            assert(snag_render_public_begin(&render, STDERR_FILENO, NULL) == 0);
            assert(snag_render_public(&render, sources[i], strlen(sources[i]), &delivered) == 0);
            assert(snag_render_update(&render, banner) == 0);
            assert(term.cursor == cursor && term.draft.len == strlen("keep my draft"));
            assert(memcmp(term.draft.data, "keep my draft", term.draft.len) == 0);
            assert(snag_render_public(&render, " tail", 5u, &delivered) == 0);
            assert(snag_render_public_end(&render) == 0);
            assert(delivered.len == strlen(sources[i]) + 5u);
            assert(memcmp(delivered.data, sources[i], strlen(sources[i])) == 0);
            snag_buf_free(&delivered);
            snag_render_free(&render);
            snag_term_close(&term);
            assert(capture_close(&capture, output, sizeof(output), 0u) > 0u);
            assert(strstr(output, "\n\n=== snajpagent"));
            assert(strstr(output, "updated"));
            assert(strstr(output, "convenient."));
            assert(strstr(output, "#changelog\n\n"));
            if (widths[w] >= 40u) assert(strstr(output, "=== snajpagent updated ===\n"));
        }
    /* Narrow terminal geometry must not change redirected notice bytes. */
    struct snag_render render;
    struct snag_term term;
    char output[4096];
    struct output_capture capture = capture_open(false, true);
    snag_term_init(&term);
    term.columns = 20u;
    snag_render_init(&render, 0u);
    render.stderr_terminal = false;
    snag_render_attach_term(&render, &term);
    assert(snag_render_update(&render, banner) == 0);
    assert(capture_close(&capture, output, sizeof(output), 0u) == strlen(banner));
    assert(!strcmp(output, banner));
    snag_render_free(&render);
    snag_term_close(&term);
}

static void
test_markdown_fences(void)
{
    static const struct {
        const char *source, *expected;
    } cases[] = {
        {"```text\nplain\n```\n", "┌─\n│ plain\n└─\n"},
        {"~~~ txt  \nplain\n~~~\n", "┌─\n│ plain\n└─\n"},
        {"```plaintext\nplain\n```", "┌─\n│ plain\n└─"},
        {"```c\nint x;\n```\n", "┌─ c\n│ int x;\n└─\n"},
        {"```text example\nplain\n```\n", "┌─ text example\n│ plain\n└─\n"},
        {"```\n    **literal**\n| A |\n| --- |\n| B |\n```\n",
            "┌─\n│     **literal**\n│ | A |\n│ | --- |\n│ | B |\n└─\n"},
    };
    char output[2048];

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        for (unsigned int split = 0; split < 2u; ++split) {
            struct snag_buf delivered;
            snag_buf_init(&delivered, 1024u);
            assert(capture_markdown(cases[i].source, true, split != 0u, SNAG_COLOR_NEVER, 120u,
                       output, sizeof(output), &delivered) > 0u);
            assert(strcmp(output, cases[i].expected) == 0);
            assert(snag_buf_terminate(&delivered) == 0);
            assert(strcmp((const char *)delivered.data, cases[i].source) == 0);
            snag_buf_free(&delivered);
        }
        assert(capture_markdown(cases[i].source, false, true, SNAG_COLOR_NEVER, 120u, output,
                   sizeof(output), NULL) > 0u);
        assert(strcmp(output, cases[i].source) == 0);
    }
}

static size_t
capture_prompt_boundary(const char *text, bool markdown, char *out, size_t out_size)
{
    struct snag_render render;
    struct snag_term term;

    struct output_capture capture = capture_terminal(&render, &term, 120u, true, true);
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    snag_render_set_markdown(&render, markdown);
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snag_render_public(&render, text, strlen(text), NULL) == 0);
    assert(snag_render_public_end(&render) == 0);
    assert(snag_render_before_prompt(&render) == 0);
    assert(snag_render_before_prompt(&render) == 0);
    snag_render_free(&render);
    snag_term_close(&term);
    return capture_close(&capture, out, out_size, 0u);
}

static void
test_model_prompt_boundaries(void)
{
    static const struct {
        const char *source;
        const char *rendered;
        bool markdown;
    } cases[] = {
        {"plain prose", "\n• plain prose", true},
        {"# heading", "heading", true},
        {"- unordered", "• unordered", true},
        {"7. ordered", "7. ordered", true},
        {"> quotation", "│ quotation", true},
        {"```c\nint x;\n```", "┌─ c\n│ int x;\n└─", true},
        {"| A |\n| --- |\n| B |", "┌───┐\n│ A │\n├───┤\n│ B │\n└───┘", true},
        {"**inline**", "\n• inline", true},
        {"- literal", "- literal", false},
    };
    static const char *const suffixes[] = {"", "\n", "\n\n"};
    char source[256];
    char expected[256];
    char output[512];

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        for (size_t j = 0u; j < sizeof(suffixes) / sizeof(suffixes[0]); ++j) {
            assert(snprintf(source, sizeof(source), "%s%s", cases[i].source, suffixes[j]) > 0);
            assert(snprintf(expected, sizeof(expected), "%s\n\n", cases[i].rendered) > 0);
            assert(capture_prompt_boundary(source, cases[i].markdown, output, sizeof(output)) > 0u);
            assert(strcmp(output, expected) == 0);
        }
    }
}

static void
test_paragraph_spacing(void)
{
    static const struct {
        const char *source, *expected;
    } cases[] = {
        {"one\n\n\n\nsecond", "\n• one\n\n• second\n\n"},
        {"one\ncontinued", "\n• one\n  continued\n\n"},
        {"# heading\none\n# next", "heading\n\n• one\n\nnext"},
        {"- first\n- second\none\n- next", "• first\n• second\n\n• one\n\n• next"},
        {"1. first\none\n2. next", "1. first\n\n• one\n\n2. next"},
        {"> first\n> second\none\n> next", "│ first\n│ second\n\n• one\n\n│ next"},
        {"one\n```c\na\n\n\nb\n```\nsecond",
            "\n• one\n\n┌─ c\n│ a\n│ \n│ \n│ b\n└─\n\n• second\n\n"},
        {"one\n| A |\n| --- |\n| B |\nsecond",
            "\n• one\n\n┌───┐\n│ A │\n├───┤\n│ B │\n└───┘\n\n• second\n\n"},
        {"one\n \n  \nsecond", "\n• one\n\n• second\n\n"},
    };
    char output[4096];
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i)
        for (unsigned int split = 0u; split < 2u; ++split) {
            struct snag_buf delivered;
            snag_buf_init(&delivered, 4096u);
            assert(capture_markdown(cases[i].source, true, split != 0u, SNAG_COLOR_NEVER, 120u,
                       output, sizeof(output), &delivered) > 0u);
            assert(strcmp(output, cases[i].expected) == 0);
            assert(delivered.len == strlen(cases[i].source));
            assert(memcmp(delivered.data, cases[i].source, delivered.len) == 0);
            snag_buf_free(&delivered);
        }
}

static void
test_interposed_paragraph_gap(void)
{
    char output[2048];
    struct snag_render render;
    struct output_capture capture = capture_open(true, true);
    snag_render_init(&render, 0u);
    render.stdout_terminal = render.stderr_terminal = true;
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snag_render_public(&render, "first", 5u, NULL) == 0);
    assert(snag_render_host(&render, "intervening") == 0);
    assert(snag_render_public(&render, "second", 6u, NULL) == 0);
    assert(snag_render_input_submitted(&render, "user › ", "steer") == 0);
    assert(snag_render_public(&render, "third", 5u, NULL) == 0);
    assert(snag_render_public_end(&render) == 0);
    snag_render_free(&render);
    (void)capture_close(&capture, output, sizeof(output), 0u);
    assert(strstr(output, "\n• first\n\n") == output);
    assert(strstr(output, "intervening\n\nsecond\n\nuser › steer\n\nthird\n\n"));
    assert(!strstr(output, "\n\n\n"));
}

static void
test_spacing_classes(void)
{
    const char *frames[SNAG_TERM_SPINNER_COUNT] = {" ", " ", " "};
    const char *events[] = {"goal_started", "goal_reworded", "goal_replaced", "goal_resumed",
        "goal_completed", "goal_cancelled", "compaction_completed"};
    char output[8192];
    struct snag_render render;
    struct snag_term term;
    struct output_capture capture = capture_terminal(&render, &term, 120u, true, true);
    assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
    term.opened = term.capable = true;
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    assert(snag_term_set_prompt_template(&term, false, "input › ", frames, 8u, 0u) == 0);
    (void)drain_available(capture.fd, output, sizeof(output), 0u);
    assert(snag_render_input_submitted(&render, "input › ", "one") == 0);
    assert(snag_render_input_submitted(&render, "input › ", "two") == 0);
    assert(term.prompt_visible && term.output_detour == 0u);
    assert(term.output_newlines == 1u && term.output_gap == 1u);
    (void)drain_available(capture.fd, output, sizeof(output), 0u);
    assert(!strstr(output, "\n\n"));
    assert(snag_render_host(&render, "diagnostic") == 0);
    assert(term.prompt_visible && term.output_detour == 1u);
    assert(term.output_newlines == 1u && term.output_gap == 2u);
    (void)drain_available(capture.fd, output, sizeof(output), 0u);
    assert(snag_term_set_prompt_template(&term, false, "input › ", frames, 8u, 0u) == 0);
    assert(drain_available(capture.fd, output, sizeof(output), 0u) == 0u);
    for (size_t i = 0u; i < sizeof(events) / sizeof(events[0]); ++i) {
        assert(snag_render_event(&render, i + 1u, events[i]) == 0);
        assert(term.prompt_visible && term.output_detour == 1u);
        (void)drain_available(capture.fd, output, sizeof(output), 0u);
        assert(!strstr(output, "\n\n•") || i == 0u);
    }
    snag_render_free(&render);
    snag_term_close(&term);
    (void)capture_close(&capture, output, sizeof(output), 0u);

    /* Exact permanent transcript, including retained bullet-class replay. */
    capture = capture_open(true, true);
    snag_render_init(&render, 0u);
    render.stdout_terminal = render.stderr_terminal = true;
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    assert(snag_render_input_submitted(&render, "input › ", "one\n\n") == 0);
    assert(snag_render_input_submitted(&render, "input › ", "two") == 0);
    for (size_t i = 0u; i < sizeof(events) / sizeof(events[0]); ++i)
        assert(snag_render_event(&render, i + 1u, events[i]) == 0);
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snag_render_public(&render, "# heading", 9u, NULL) == 0);
    assert(snag_render_public_end(&render) == 0);
    assert(snag_render_input_submitted(&render, "input › ", "three") == 0);
    snag_render_free(&render);
    (void)capture_close(&capture, output, sizeof(output), 0u);
    assert(strcmp(output, "input › one\ninput › two\n\n"
                          "• Goal set\n• Goal updated\n"
                          "• Goal updated\n• Goal resumed\n• Goal cleared\n• Goal cleared\n"
                          "• Compacted\n\nheading\n\ninput › three\n") == 0);

    /* A provider wait notice must not consume the literal model paragraph's gap. */
    capture = capture_open(true, true);
    snag_render_init(&render, 0u);
    render.stdout_terminal = render.stderr_terminal = true;
    render.markdown = false;
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    assert(snag_render_warning_ctx(&render, "waiting") == 0);
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snag_render_public(&render, "model text", 10u, NULL) == 0);
    assert(snag_render_public_end(&render) == 0);
    snag_render_free(&render);
    (void)capture_close(&capture, output, sizeof(output), 0u);
    assert(strstr(output, "snajpagent: waiting\n\nmodel text"));
}

static void
test_live_paragraph_gap(void)
{
    const char *frames[SNAG_TERM_SPINNER_COUNT] = {" ", " ", " "};
    const char *parts[] = {"123456789012345678 ", "🌙", "́", " fragment", "\n", "\n", "\n", "next"};
    const unsigned int rows[] = {2u, 2u, 2u, 2u, 1u, 0u, 0u, 2u};
    char output[8192];
    struct snag_render render;
    struct snag_term term;
    struct snag_buf delivered;
    struct output_capture capture = capture_terminal(&render, &term, 20u, true, true);
    assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
    term.opened = term.capable = term.active = true;
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    assert(snag_term_set_prompt_template(&term, true, "input › ", frames, 8u, 0u) == 0);
    snag_buf_init(&delivered, 1024u);
    assert(snag_render_input_submitted(&render, "user › ", "question") == 0);
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    (void)drain_available(capture.fd, output, sizeof(output), 0u);
    for (size_t i = 0u; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        assert(snag_render_public(&render, parts[i], strlen(parts[i]), &delivered) == 0);
        assert(term.output_detour == rows[i]);
        assert(term.output_newlines + term.output_detour >= 2u);
        assert(render.public_item_open && term.prompt_visible);
        (void)drain_available(capture.fd, output, sizeof(output), 0u);
        if (i == 0u) {
            assert(snag_term_set_prompt_template(&term, true, "input › ", frames, 8u, 0u) == 0);
            assert(term.output_detour == 2u && term.prompt_visible);
            (void)drain_available(capture.fd, output, sizeof(output), 0u);
            assert(snag_term_set_prompt_template(&term, true, "input › ", frames, 8u, 0u) == 0);
            assert(drain_available(capture.fd, output, sizeof(output), 0u) == 0u);
        }
    }
    assert(snag_render_public_abort(&render) == 0);
    assert(term.output_detour == 0u && term.output_newlines == 2u);
    (void)drain_available(capture.fd, output, sizeof(output), 0u);
    assert(snag_render_before_prompt(&render) == 0);
    assert(snag_render_before_prompt(&render) == 0);
    assert(drain_available(capture.fd, output, sizeof(output), 0u) == 0u);
    assert(snag_buf_terminate(&delivered) == 0);
    assert(strcmp((char *)delivered.data, "123456789012345678 🌙́ fragment\n\n\nnext") == 0);
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snag_render_public(&render, "before edit", 11u, NULL) == 0);
    assert(snag_term_set_prompt_template(&term, true, "input › ", frames, 8u, 0u) == 0);
    term.typing_active = true;
    assert(term.output_detour == 2u);
    assert(snag_term_restore_draft(&term, "unsent draft") == 0);
    (void)drain_available(capture.fd, output, sizeof(output), 0u);
    assert(snag_render_public(&render, "after edit", 10u, NULL) == 0);
    (void)drain_available(capture.fd, output, sizeof(output), 0u);
    assert(strstr(output, "\033[2K"));
    assert(!strstr(output, "\n\nafter edit"));
    assert(term.prompt_visible && term.draft.len == strlen("unsent draft"));
    assert(term.output_detour == 2u);
    assert(snag_render_public_end(&render) == 0);
    snag_buf_free(&delivered);
    snag_render_free(&render);
    snag_term_close(&term);
    (void)capture_close(&capture, output, sizeof(output), 0u);
}

static void
test_input_model_boundaries(void)
{
    static const char *const suffixes[] = {"", "\n", "\n\n"};
    char output[256];
    char question[32];

    for (size_t enabled = 0u; enabled < 2u; ++enabled) {
        for (size_t suffix = 0u; suffix < sizeof(suffixes) / sizeof(suffixes[0]); ++suffix) {
            struct snag_render render;
            struct snag_term term;
            struct output_capture capture = capture_terminal(&render, &term, 120u, true, true);
            snag_render_set_color(&render, SNAG_COLOR_NEVER);
            snag_render_set_markdown(&render, enabled != 0u);
            assert(snprintf(question, sizeof(question), "question%s", suffixes[suffix]) > 0);
            assert(snag_render_input_submitted(&render, "model/low › ", question) == 0);
            assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
            assert(snag_render_public(&render, "answer", 6u, NULL) == 0);
            assert(snag_render_public_end(&render) == 0);
            assert(snag_render_before_prompt(&render) == 0);
            assert(snag_render_before_prompt(&render) == 0);
            snag_render_free(&render);
            snag_term_close(&term);
            (void)capture_close(&capture, output, sizeof(output), 0u);
            assert(strcmp(output, enabled ? "model/low › question\n\n• answer\n\n"
                                          : "model/low › question\n\nanswer\n\n") == 0);
        }
    }
    {
        struct snag_render render;
        struct snag_term term;
        struct output_capture capture = capture_terminal(&render, &term, 80u, false, true);
        memcpy(term.label, "model/low › ", strlen("model/low › ") + 1u);
        term.line_submission_echoed = true;
        assert(snag_render_input_submitted(&render, "model/low › ", "question") == 0);
        snag_render_free(&render);
        snag_term_close(&term);
        (void)capture_close(&capture, output, sizeof(output), 0u);
        assert(strcmp(output, "") == 0);
    }
}

static size_t
capture_static_markdown(unsigned int verbosity, char *out, size_t out_size)
{
    static const struct {
        enum snag_irc_event_kind kind;
        const char *nick, *text;
        bool op;
    } events[] = {{SNAG_IRC_MESSAGE, "agent", "**answer** and `code`", false},
        {SNAG_IRC_MESSAGE, "agent", "- actual list item", false},
        {SNAG_IRC_MESSAGE, "operator", "**literal operator**", true},
        {SNAG_IRC_MESSAGE, "remote", "```c", false},
        {SNAG_IRC_MESSAGE, "remote", "int value = 1;", false},
        {SNAG_IRC_MESSAGE, "remote", "```", false},
        {SNAG_IRC_NOTICE, "remote", "**literal notice**", false},
        {SNAG_IRC_MESSAGE, "remote", "```c", false}, {SNAG_IRC_QUIT, "remote", "gone", false},
        {SNAG_IRC_MESSAGE, "remote", "plain after quit", false}};
    struct snag_render render;
    struct snag_irc_event event = {.timestamp_ms = 1000u, .endpoint = "local", .local = true};

    struct output_capture capture = capture_open(false, true);
    snag_render_init(&render, verbosity);
    render.stderr_terminal = true;
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
    for (size_t i = 0u; i < sizeof(events) / sizeof(events[0]); ++i) {
        event.kind = events[i].kind;
        event.op = events[i].op;
        strcpy(event.nick, events[i].nick);
        strcpy(event.text, events[i].text);
        assert(snag_render_irc_event(&render, &event) == 0);
    }
    assert(snag_render_history(&render,
               &(struct snag_history_turn){
                   .user = "**literal user**", .assistant = "## Saved *answer*"},
               1u, 2u, 3u) == 0);
    assert(snag_render_history(&render, NULL, 1u, 2u, 3u) == 0);
    snag_render_set_markdown(&render, false);
    event.kind = SNAG_IRC_MESSAGE;
    memcpy(event.text, "**literal agent**", 18u);
    assert(snag_render_irc_event(&render, &event) == 0);
    assert(snag_render_history(&render,
               &(struct snag_history_turn){.assistant = "## Literal assistant"}, 1u, 2u, 3u) == 0);
    assert(snag_render_history(&render, NULL, 1u, 2u, 3u) == 0);
    return capture_close(&capture, out, out_size, 0u);
}

static void
test_query_markdown_isolation(void)
{
    struct snag_render render;
    struct snag_irc_event event = {.routed = true,
        .kind = SNAG_IRC_MESSAGE,
        .timestamp_ms = 1000u,
        .endpoint = "server:6667",
        .nick = "peer",
        .text = "```",
        .route = {.identity = SNAG_IRC_OPERATOR,
            .kind = SNAG_IRC_QUERY,
            .connection = "11111111111111111111111111111111",
            .conversation = "22222222222222222222222222222222",
            .peer = "peer"}};
    char output[8192];
    struct output_capture capture = capture_open(false, true);
    snag_render_init(&render, 1u);
    render.stderr_terminal = true;
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    struct snag_irc_conversation_target target = {.kind = SNAG_IRC_QUERY,
        .identity = SNAG_IRC_OPERATOR,
        .peer = "peer",
        .conversation = "22222222222222222222222222222222"};
    assert(snag_render_set_chat_conversation(&render, event.endpoint, &target, false) == 0);
    assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
    assert(snag_render_irc_event(&render, &event) == 0);
    event.route.identity = SNAG_IRC_AGENT;
    strcpy(event.route.conversation, "33333333333333333333333333333333");
    strcpy(event.text, "**other conversation**");
    assert(snag_render_irc_event(&render, &event) == 0);
    event.route.identity = SNAG_IRC_OPERATOR;
    strcpy(event.route.conversation, "22222222222222222222222222222222");
    strcpy(event.text, "**code**");
    assert(snag_render_irc_event(&render, &event) == 0);
    target.identity = SNAG_IRC_AGENT;
    strcpy(target.conversation, "33333333333333333333333333333333");
    assert(snag_render_set_chat_conversation(&render, event.endpoint, &target, true) == 0);
    snag_render_free(&render);
    (void)capture_close(&capture, output, sizeof(output), 0u);
    assert(!strstr(output, "[server:6667/peer"));
    assert(strstr(output, "query server:6667/peer; viewing model's chat"));
    if (!strstr(output, "**code**") || strstr(output, "**other conversation**"))
        fprintf(stderr, "query Markdown output: %s\n", output);
    assert(strstr(output, "other conversation") && !strstr(output, "**other conversation**") &&
           strstr(output, "**code**"));
}

static void
test_query_send_receipts(void)
{
    struct snag_render render;
    struct snag_irc_event event = {.routed = true,
        .kind = SNAG_IRC_MESSAGE,
        .timestamp_ms = 1000u,
        .endpoint = "server:6667",
        .nick = "operator",
        .text = "one-private-body",
        .route = {.identity = SNAG_IRC_OPERATOR,
            .kind = SNAG_IRC_QUERY,
            .direction = SNAG_IRC_OUTGOING,
            .connection = "11111111111111111111111111111111",
            .conversation = "22222222222222222222222222222222",
            .peer = "peer",
            .send = "33333333333333333333333333333333"}};
    struct snag_irc_conversation_target target = {.kind = SNAG_IRC_QUERY,
        .identity = SNAG_IRC_OPERATOR,
        .peer = "peer",
        .conversation = "22222222222222222222222222222222"};
    char output[8192];
    struct output_capture capture = capture_open(false, true);
    snag_render_init(&render, 1u);
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    assert(snag_render_set_chat_conversation(&render, event.endpoint, &target, false) == 0);
    assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
    for (unsigned int state = SNAG_IRC_PENDING; state <= SNAG_IRC_ACKNOWLEDGED; ++state) {
        event.route.delivery = (enum snag_irc_delivery)state;
        assert(snag_render_irc_event(&render, &event) == 0);
    }
    event.route.revised = true;
    strcpy(event.text, "server-revised-body");
    assert(snag_render_irc_event(&render, &event) == 0);
    snag_render_free(&render);
    (void)capture_close(&capture, output, sizeof(output), 0u);
    assert(count_text(output, "one-private-body") == 1u);
    assert(strstr(output, "send 33333333 pending"));
    assert(strstr(output, "send 33333333 written"));
    assert(strstr(output, "send 33333333 acknowledged"));
    assert(count_text(output, "server-revised-body") == 1u);
    assert(strstr(output, "server text: server-revised-body"));
}

static void
test_history_failure(void)
{
    struct snag_render render;
    struct output_capture capture = capture_open(false, true);
    snag_render_init(&render, 0u);
    render.stderr_terminal = true;
    errno = 0;
    assert(snag_render_history(
               &render, &(struct snag_history_turn){.assistant = "\xff"}, 1u, 1u, 1u) < 0);
    assert(errno == EILSEQ && !render.public_item_open);
    assert(snag_render_public_begin(&render, STDERR_FILENO, NULL) == 0);
    assert(snag_render_public_end(&render) == 0);
    capture_restore(&capture);
    close(capture.fd);
}

static void
test_history_turns(void)
{
    const struct snag_history_turn turns[] = {{.user = "first input", .assistant = "first answer"},
        {.user = "second input", .assistant = "second answer"}};
    struct snag_render render;
    char output[4096];
    struct output_capture capture = capture_open(false, true);
    snag_render_init(&render, 0u);
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    assert(snag_render_history(&render, NULL, 0u, 0u, 0u) == 0);
    assert(snag_render_rollout_begin(&render, STDERR_FILENO, NULL, SNAG_PRESENT_CONVERSATION) == 0);
    assert(snag_render_rollout(&render, "active prefix", 13u, NULL) == 0);
    for (size_t i = 0u; i < 2u; ++i)
        assert(snag_render_history(&render, &turns[i], i + 1u, 2u, 3u) == 0);
    assert(snag_render_history(&render, NULL, 2u, 2u, 3u) == 0);
    assert(snag_render_rollout(&render, "active suffix", 13u, NULL) == 0);
    assert(snag_render_rollout_end(&render) == 0);
    assert(capture_close(&capture, output, sizeof(output), 0u) > 0u);
    assert(count_text(output, "── history: 3 total turns ──") == 1u);
    assert(count_text(output, "history: 0 shown · 0 completed among shown · 0 total") == 1u);
    const char *position = output;
    const char *const fragments[] = {"active prefix", "history: 3 total turns", "user: first input",
        "first answer", "user: second input", "second answer",
        "history: 2 shown · 2 completed among shown · 3 total", "active suffix"};
    for (size_t i = 0u; i < sizeof(fragments) / sizeof(fragments[0]); ++i) {
        position = strstr(position, fragments[i]);
        assert(position);
        position += strlen(fragments[i]);
    }
    assert(count_text(output, "assistant:") == 2u);
    snag_render_free(&render);
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
    char output[4096] = {0};
    size_t used = 0u;
    struct output_capture capture = capture_terminal(&render, &term, 80u, true, false);

    assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    struct snag_buf delivered = {.max = 1024u};
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snag_render_public(&render, first, sizeof(first) - 1u, &delivered) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(strcmp(output, "") == 0);
    for (size_t i = 0u; i < sizeof(second) - 1u; ++i)
        assert(snag_render_public(&render, second + i, 1u, &delivered) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(strcmp(output, "Live café [docs]") == 0);
    assert(snag_render_public(&render, third, sizeof(third) - 1u, &delivered) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(strcmp(output, "Live café [docs] <https://example.test> and") == 0);
    assert(snag_render_public(&render, fourth, sizeof(fourth) - 1u, &delivered) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(strcmp(output, "Live café [docs] <https://example.test> and code\n") == 0);
    assert(snag_render_public_end(&render) == 0);
    assert(snag_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data,
               "# **Live** café [docs](https://example.test) and `code`\n") == 0);
    snag_buf_free(&delivered);

    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snag_render_public(&render, "**aborted", 9u, NULL) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(strcmp(output, "Live café [docs] <https://example.test> and code\n\n"
                          "•") == 0);
    assert(snag_render_public_abort(&render) == 0);
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snag_render_public(&render, "literal", 7u, NULL) == 0);
    assert(snag_render_public_end(&render) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(strcmp(output, "Live café [docs] <https://example.test> and code\n\n"
                          "• aborted\n\n• literal\n\n") == 0);
    (void)capture_close(&capture, output, sizeof(output), used);
    snag_term_close(&term);
}

static const char *
load_timeline_table(void)
{
    static char text[8192];
    FILE *file = fopen("tests/fixtures/markdown-timeline.md", "r");
    assert(file);
    size_t len = fread(text, 1u, sizeof(text) - 1u, file);
    assert(!ferror(file) && feof(file) && fclose(file) == 0);
    text[len] = '\0';
    const char *table = strstr(text, "| Time | Evidence |");
    assert(table);
    return table;
}

/* Reassemble wrapped cells and compare every displayed value with the fixture. */
static void
check_timeline_grid(const char *output, const char *source, unsigned int columns)
{
    char values[2][4096] = {{0}};
    size_t used[2] = {0};
    unsigned int rows = 0u;
    size_t table_width = snag_term_text_width(output, (size_t)(strchr(output, '\n') - output));
    for (const char *line = output; *line;) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        assert(snag_term_text_width(line, (size_t)(end - line)) == table_width &&
               table_width < columns);
        if (!strncmp(line, "│", strlen("│"))) {
            const char *part = line + strlen("│");
            for (size_t i = 0u; i < 2u; ++i) {
                const char *next = strstr(part, "│");
                assert(next && next < end);
                const char *finish = next;
                while (part < finish && *part == ' ') ++part;
                while (finish > part && finish[-1] == ' ') --finish;
                size_t len = (size_t)(finish - part);
                assert(used[i] + len + 2u < sizeof(values[i]));
                if (len && used[i]) values[i][used[i]++] = ' ';
                memcpy(values[i] + used[i], part, len);
                used[i] += len;
                values[i][used[i]] = '\0';
                part = next + strlen("│");
            }
            assert(part == end);
        } else if ((!strncmp(line, "├", strlen("├")) || !strncmp(line, "└", strlen("└"))) &&
                   used[0]) {
            const char *part = source + 1u;
            for (size_t i = 0u; i < 2u; ++i) {
                const char *next = strchr(part, '|'), *finish = next;
                assert(next);
                while (part < finish && *part == ' ') ++part;
                while (finish > part && finish[-1] == ' ') --finish;
                assert((size_t)(finish - part) == used[i]);
                assert(!memcmp(values[i], part, used[i]));
                values[i][0] = '\0';
                used[i] = 0u;
                part = next + 1u;
            }
            source = strchr(source, '\n') + 1u;
            if (!rows++) source = strchr(source, '\n') + 1u; /* Delimiter row. */
        }
        line = *end ? end + 1u : end;
    }
    assert(rows == 11u && !*source);
}

static void
test_wrapped_markdown_tables(void)
{
    char output[32768], whole[32768];
    const char *timeline = load_timeline_table();
    const unsigned int widths[] = {40u, 80u, 120u, 160u};
    for (size_t i = 0u; i < sizeof(widths) / sizeof(widths[0]); ++i) {
        struct snag_buf delivered = {.max = SNAG_MAX_PUBLIC_ITEM};
        assert(capture_markdown(timeline, true, true, SNAG_COLOR_NEVER, widths[i], output,
                   sizeof(output), &delivered) > 0u);
        if (strstr(output, "┌─ table"))
            fprintf(stderr, "long two-column cells incorrectly force vertical table fallback\n");
        assert(!strstr(output, "┌─ table"));
        assert(strstr(output, "┬") && strstr(output, "┼") && strstr(output, "┴"));
        assert(
            delivered.len == strlen(timeline) && !memcmp(delivered.data, timeline, delivered.len));
        snag_buf_free(&delivered);
        check_timeline_grid(output, timeline, widths[i]);
        assert(capture_markdown(timeline, true, false, SNAG_COLOR_NEVER, widths[i], whole,
                   sizeof(whole), NULL) > 0u);
        assert(!strcmp(whole, output));
    }
    assert(capture_markdown(
               timeline, true, true, SNAG_COLOR_NEVER, 28u, output, sizeof(output), NULL) > 0u);
    assert(strstr(output, "┌─ table"));
    for (const char *line = output; *line;) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        assert(snag_term_text_width(line, (size_t)(end - line)) < 28u);
        assert(!strncmp(line, "│ ", strlen("│ ")) || !strncmp(line, "┌", strlen("┌")) ||
               !strncmp(line, "├", strlen("├")) || !strncmp(line, "└", strlen("└")));
        line = *end ? end + 1u : end;
    }
    static const char aligned[] = "| A | B | C |\n| :---: | :---: | ---: |\n"
                                  "| abc def ghi jkl mno pqr stu vwx | M | 7 |\n"
                                  "| after | middle | 8 | ignored |\n";
    assert(capture_markdown(
               aligned, true, true, SNAG_COLOR_NEVER, 35u, output, sizeof(output), NULL) > 0u);
    assert(!strstr(output, "┌─ table") && !strstr(output, "ignored"));
    assert(strstr(output, "│  abc def ghi jkl  │   M    │ 7 │"));
    assert(strstr(output, "│  mno pqr stu vwx  │        │   │"));
    static const char styled[] =
        "| Key | Result |\n| :--- | ---: |\n"
        "| **café 界 é** | `one two three four five six seven eight nine ten eleven twelve` |\n"
        "| tab\tkey | ~~struck~~ and [link](https://example.test) plus escaped \\| and `a|b` |\n";
    assert(capture_markdown(
               styled, true, true, SNAG_COLOR_NEVER, 48u, output, sizeof(output), NULL) > 0u);
    assert(!strstr(output, "┌─ table"));
    assert(strstr(output, "café 界 é") && strstr(output, "a|b"));
    size_t styled_width = snag_term_text_width(output, (size_t)(strchr(output, '\n') - output));
    for (const char *line = output; *line;) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        assert(
            snag_term_text_width(line, (size_t)(end - line)) == styled_width && styled_width < 48u);
        line = *end ? end + 1u : end;
    }
    assert(capture_markdown(
               styled, true, true, SNAG_COLOR_ALWAYS, 48u, output, sizeof(output), NULL) > 0u);
    assert(count_text(output, "\033[0;33m") >= 2u);
    assert(strstr(output, "\033[0;1mcafé") && strstr(output, "\033[0;4;34mhttps://example.test"));
    assert(snag_utf8_valid((const unsigned char *)output, strlen(output), true));
    assert(capture_markdown("**before\n| Key | Value |\n| --- | --- |\n| item | body |\nafter\n",
               true, true, SNAG_COLOR_ALWAYS, 80u, output, sizeof(output), NULL) > 0u);
    assert(!strstr(output, "\033[0;1m┌") && !strstr(output, "\033[0;1mafter"));
}

static void
test_markdown_tables(void)
{
    static const char markdown[] = "| Name | State | Count\n"
                                   "| :--- | :---: | ---:\n"
                                   "| **alpha** | `ready` | 7\n"
                                   "| escaped \\| pipe | [docs](https://example.test) | 42 |\n"
                                   "after table\n";
    static const char rendered[] = "┌────────────────┬───────────────────────────────┬───────┐\n"
                                   "│ Name           │             State             │ Count │\n"
                                   "├────────────────┼───────────────────────────────┼───────┤\n"
                                   "│ alpha          │             ready             │     7 │\n"
                                   "│ escaped | pipe │ [docs] <https://example.test> │    42 │\n"
                                   "└────────────────┴───────────────────────────────┴───────┘\n"
                                   "\n• after table\n\n";
    static const char narrow[] = "┌─ table\n"
                                 "├─ row\n"
                                 "│ Name: alpha\n"
                                 "│ State: ready\n"
                                 "│ Count: 7\n"
                                 "├─ row\n"
                                 "│ Name: escaped | pipe\n"
                                 "│ State: [docs]\n│ <https://example.test>\n"
                                 "│ Count: 42\n"
                                 "└─\n"
                                 "\n• after table\n\n";
    static const char malformed[] = "| Name | State |\n"
                                    "| -- | nope |\n"
                                    "after\n";
    static const char code_pipe[] = "| Code | Other\n"
                                    "| --- | ---\n"
                                    "| `a|b` | tail\n";
    static const char code_pipe_rendered[] = "┌──────┬───────┐\n"
                                             "│ Code │ Other │\n"
                                             "├──────┼───────┤\n"
                                             "│ a|b  │ tail  │\n"
                                             "└──────┴───────┘\n";
    char output[8192];

    struct snag_buf delivered = {.max = sizeof(markdown)};
    assert(capture_markdown(markdown, true, true, SNAG_COLOR_NEVER, 120u, output, sizeof(output),
               &delivered) > 0u);
    assert(strcmp(output, rendered) == 0);
    assert(snag_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data, markdown) == 0);
    snag_buf_free(&delivered);
    assert(capture_markdown(
               markdown, true, false, SNAG_COLOR_ALWAYS, 120u, output, sizeof(output), NULL) > 0u);
    assert(strstr(output, "\033[0;1mName") != NULL);
    assert(strstr(output, "\033[0;1malpha") != NULL);
    assert(strstr(output, "\033[0;33mready") != NULL);
    assert(strstr(output, "\033[0;4;34mhttps://example.test") != NULL);

    assert(capture_markdown(
               markdown, true, true, SNAG_COLOR_NEVER, 28u, output, sizeof(output), NULL) > 0u);
    assert(strcmp(output, narrow) == 0);

    assert(capture_markdown("| Name |\n| --- |\n", true, true, SNAG_COLOR_NEVER, 9u, output,
               sizeof(output), NULL) > 0u);
    assert(strcmp(output, "┌─ table\n│ Name\n└─\n") == 0);
    assert(capture_markdown("| Name |\n| --- |", true, true, SNAG_COLOR_NEVER, 9u, output,
               sizeof(output), NULL) > 0u);
    assert(strcmp(output, "┌─ table\n│ Name\n└─") == 0);

    assert(capture_markdown(
               malformed, true, true, SNAG_COLOR_NEVER, 120u, output, sizeof(output), NULL) > 0u);
    assert(strcmp(output, "\n• | Name | State |\n  | -- | nope |\n  after\n\n") == 0);
    assert(capture_markdown(
               code_pipe, true, true, SNAG_COLOR_NEVER, 120u, output, sizeof(output), NULL) > 0u);
    assert(strcmp(output, code_pipe_rendered) == 0);
    assert(capture_markdown(
               markdown, false, false, SNAG_COLOR_NEVER, 120u, output, sizeof(output), NULL) > 0u);
    assert(strcmp(output, markdown) == 0);
}

static size_t
capture_color(enum snag_color_mode mode, bool chat_view, unsigned int verbosity, int timeout_ms,
    uint32_t default_timeout_ms, uint32_t max_output_bytes, char *out, size_t out_size)
{
    struct snag_render render;
    struct snag_irc_event event = {
        .kind = SNAG_IRC_MESSAGE, .timestamp_ms = 1000u, .nick = "agent", .text = "answer"};
    struct snag_response_item call = {.name = "exec_command"};
    json_t *arguments;
    json_t *result;

    struct output_capture capture = capture_open(false, true);
    snag_render_init(&render, verbosity);
    if (chat_view) assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
    snag_render_set_color(&render, mode);
    assert(snag_render_submitted(&render, "› ", "plain") == 0);
    assert(snag_render_warning_ctx(&render, "careful") == 0);
    assert(snag_render_error_ctx(&render, "broken") == 0);
    assert(snag_render_host(&render, "status") == 0);
    assert(snag_render_event(&render, 7u, "compaction_completed") == 0);
    arguments = json_pack("{s:s,s:o}", "command", "printf plain", "timeout_ms",
        timeout_ms < 0 ? json_null() : json_integer(timeout_ms));
    assert(arguments != NULL);
    call.arguments = arguments;
    {
        struct snag_render_block block;
        assert(snag_render_prepare_tool_start(
                   &block, &call, "/tmp", default_timeout_ms, verbosity, 0u) == 0);
        assert(snag_render_tool_block(&render, &block) == 0);
        snag_render_block_free(&block);
    }
    json_decref(arguments);
    result = json_pack("{s:i,s:i,s:s,s:n,s:s}", "duration_ms", 12, "exit_code", 0, "model_text",
        "fixture tool output: café\n", "reason", "status", "succeeded");
    assert(result != NULL);
    {
        struct snag_render_block block;
        assert(snag_render_prepare_tool_finish(
                   &block, call.name, NULL, result, max_output_bytes, verbosity, 0u) == 0);
        assert(snag_render_tool_block(&render, &block) == 0);
        snag_render_block_free(&block);
    }
    json_decref(result);
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
    while (snag_render_view_pending(&render)) assert(snag_render_flush_pending(&render, 8u) == 0);
    if (chat_view) assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
    snag_render_free(&render);
    return capture_close(&capture, out, out_size, 0u);
}

static void
test_local_mention_highlight(void)
{
    static const struct {
        const char *text, *nick, *room, *sender;
        enum snag_irc_event_kind kind;
        bool op, highlight;
    } cases[] = {
        {"@ALICE **bold** `code` tail", "alice", "#room", "peer", SNAG_IRC_MESSAGE, false, true},
        {"alice: plain tail", "alice", "#room", "peer", SNAG_IRC_MESSAGE, true, true},
        {"@alice notice tail", "alice", "#room", "peer", SNAG_IRC_NOTICE, false, true},
        {"malice alice2 alice-other éalice aliceé", "alice", "#room", "peer", SNAG_IRC_MESSAGE,
            false, false},
        {"@bob wrong destination", "alice", "#room", "peer", SNAG_IRC_MESSAGE, false, false},
        {"@alice wrong room", "alice", "#other", "peer", SNAG_IRC_MESSAGE, false, false},
        {"@alice old alias", "alice2", "#room", "peer", SNAG_IRC_MESSAGE, false, false},
        {"@alice2 accepted alias", "alice2", "#room", "peer", SNAG_IRC_MESSAGE, false, true},
        {"@alice topic", "alice", "#room", "peer", SNAG_IRC_TOPIC, false, false},
        {"unregistered nick", "", "#room", "peer", SNAG_IRC_MESSAGE, false, false},
        {"@local-other also addressed", "alice", "#room", "peer", SNAG_IRC_MESSAGE, false, true},
        {"# @alice heading", "alice", "#room", "peer", SNAG_IRC_MESSAGE, false, true},
        {"> @alice quote", "alice", "#room", "peer", SNAG_IRC_MESSAGE, false, true},
        {"@alice *italic* ~~strike~~ [link](https://example.test)", "alice", "#room", "peer",
            SNAG_IRC_MESSAGE, false, true},
        {"@alice self", "alice", "#room", "alice", SNAG_IRC_MESSAGE, false, false},
        {"@alice self operator", "alice", "#room", "ALICE", SNAG_IRC_MESSAGE, true, false},
        {"@alice self notice", "alice", "#room", "Alice", SNAG_IRC_NOTICE, false, false},
        {"@{ALICE} self folded", "[alice]", "#room", "{Alice}", SNAG_IRC_MESSAGE, false, false},
        {"@alice2 self alias", "alice2", "#room", "ALICE2", SNAG_IRC_MESSAGE, false, false},
        {"@alice not self", "alice", "#room", "alice2", SNAG_IRC_MESSAGE, false, true},
        {"@alice and @local-other", "alice", "#room", "alice", SNAG_IRC_MESSAGE, false, true},
        {"@alice and @local-other", "alice", "#room", "local-other", SNAG_IRC_MESSAGE, true, true},
        {"@local-other self", "alice", "#room", "local-other", SNAG_IRC_MESSAGE, false, false},
    };
    for (unsigned int flags = 0u; flags < 16u; ++flags) {
        for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            struct snag_render render;
            struct snag_term term;
            struct snag_irc_destinations destinations = {.count = 2u,
                .items = {
                    {.endpoint = "server",
                        .room = "#room",
                        .operator = "local-other",
                        .model = "local-other",
                        .target = {.id = 1u}},
                    {.endpoint = "other",
                        .room = "#room",
                        .operator = "bob",
                        .model = "bob",
                        .target = {.id = 2u}},
                }};
            struct snag_irc_event event = {.endpoint = "server", .nick = "peer"};
            char output[8192] = {0};
            struct output_capture capture = capture_terminal(&render, &term, 40u, false, true);
            bool color = (flags & 1u) != 0u;
            assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
            strcpy(flags & 4u ? destinations.items[0].model : destinations.items[0].operator,
                cases[i].nick);
            assert(snag_term_set_destinations(&term, &destinations) == 0);
            render.markdown = !(flags & 8u);
            snag_render_set_color(&render, color ? SNAG_COLOR_ALWAYS : SNAG_COLOR_NEVER);
            assert(snag_render_set_chat_room(&render, "server", cases[i].room, false) == 0);
            if (flags & 2u) assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
            strcpy(event.room, cases[i].room);
            strcpy(event.nick, cases[i].sender);
            strcpy(event.text, cases[i].text);
            event.kind = cases[i].kind;
            event.op = cases[i].op;
            event.historical = !(flags & 2u);
            assert(snag_render_irc_event(&render, &event) == 0);
            assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
            size_t used = drain_available(capture.fd, output, sizeof(output), 0u);
            event.kind = SNAG_IRC_MESSAGE;
            event.op = false;
            strcpy(event.text, "ordinary followup");
            assert(snag_render_irc_event(&render, &event) == 0);
            (void)drain_available(capture.fd, output, sizeof(output), used);
            snag_render_free(&render);
            snag_term_close(&term);
            capture_restore(&capture);
            close(capture.fd);
            assert((strstr(output, "[1] \033[1;35m") != NULL) == (color && cases[i].highlight));
            if (color) {
                assert(strstr(output, strcmp(cases[i].room, "#room") == 0
                                          ? "\033[2m[1] "
                                          : "\033[2m[server #other] "));
                if (cases[i].kind == SNAG_IRC_MESSAGE || cases[i].kind == SNAG_IRC_NOTICE) {
                    char nick[128u], boundary[32u];
                    const char *separator = cases[i].kind == SNAG_IRC_NOTICE ? "- " : "› ";
                    snprintf(nick, sizeof(nick), "%s ", cases[i].sender);
                    const char *body = strstr(output, nick);
                    assert(body);
                    body += strlen(nick);
                    snprintf(boundary, sizeof(boundary), "%s%s\033[0m",
                        cases[i].highlight ? "" : "\033[0m", separator);
                    assert(strncmp(body, boundary, strlen(boundary)) == 0);
                    assert(!strstr(body, "35m"));
                    snprintf(nick, sizeof(nick), "\033[1;35m%s ", cases[i].sender);
                    assert(
                        (strstr(output, nick) != NULL) ==
                        (!cases[i].op && cases[i].highlight && cases[i].kind == SNAG_IRC_MESSAGE));
                }
            }
            assert(strstr(output + used, "ordinary") && strstr(output + used, "followup"));
            assert(!strstr(output + used, "35m"));
            if (!color) assert(!strchr(output, '\033'));
            if (color && !(flags & 8u) && i == 0u) {
                assert(strstr(output, "\033[0;1m"));
                assert(strstr(output, "\033[0;33m"));
                assert(!strstr(output, ";1;1;35m"));
                assert(!strstr(output, ";33;1;35m"));
                assert(strstr(output, "code\033[0m"));
                assert(!strstr(output, "\033[0;1;35m"));
                assert(strstr(output, "tail"));
            }
            if (color && !(flags & 8u) && i == 11u) assert(strstr(output, "\033[0;1;36m"));
            if (color && !(flags & 8u) && i == 12u) assert(strstr(output, "\033[0;34m"));
            if (color && !(flags & 8u) && i == 13u) {
                assert(strstr(output, "\033[0;3m"));
                assert(strstr(output, "\033[0;2m"));
                assert(strstr(output, "\033[0;4;34m"));
            }
        }
    }
}

static size_t
capture_lifecycle(unsigned int verbosity, enum snag_color_mode color, char *out, size_t out_size)
{
    static const char *const events[] = {"compaction_completed", "goal_started", "goal_reworded",
        "goal_replaced", "goal_completed", "goal_cancelled", "turn_completed"};
    struct snag_render render;

    struct output_capture capture = capture_open(false, true);
    snag_render_init(&render, verbosity);
    snag_render_set_color(&render, color);
    for (size_t i = 0u; i < sizeof(events) / sizeof(events[0]); ++i)
        assert(snag_render_event(&render, i + 1u, events[i]) == 0);
    snag_render_free(&render);
    return capture_close(&capture, out, out_size, 0u);
}

static size_t
capture_resume_hint(enum snag_color_mode color, char *out, size_t out_size)
{
    static const char command[] = "'snajpagent' --resume '0123'";
    struct snag_render render;

    struct output_capture capture = capture_open(false, true);
    snag_render_init(&render, 0u);
    snag_render_set_color(&render, color);
    assert(snag_render_resume_hint(&render, command, sizeof(command) - 1u) == 0);
    snag_render_free(&render);
    return capture_close(&capture, out, out_size, 0u);
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
            assert(memchr(block.text.data, '\n', block.text.len) ==
                   block.text.data + block.text.len - 1u);
            snag_render_block_free(&block);
        }
        json_decref(call.arguments);
    }
    for (size_t n = 511u; n <= 513u; ++n) {
        json_t *result = json_object();
        assert(result);
        assert(json_object_set_new(result, "model_text", json_stringn(text, n)) == 0);
        for (unsigned int level = 1u; level <= 3u; ++level) {
            assert(snag_render_prepare_tool_finish(
                       &block, call.name, NULL, result, 0u, level, 0u) == 0);
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
    struct snag_render_source source = {.offset = ftello(file), .len = strlen(text)};
    assert(source.offset >= 0);
    assert(fwrite(text, 1u, source.len, file) == source.len);
    assert(fflush(file) == 0);
    return source;
}

static void
test_native_durable_source(void)
{
    char cwd[4096];
    char root[4096];
    char error[256];
    assert(getcwd(cwd, sizeof(cwd)));
    int written = snprintf(root, sizeof(root), "%s/build/native-render-XXXXXX", cwd);
    assert(written > 0 && (size_t)written < sizeof(root) && mkdtemp(root));
    struct snag_store store;
    struct snag_session session;
    snag_store_init(&store);
    snag_session_init(&session);
    assert(!snag_store_open(&store, root, error, sizeof(error)));
    assert(!snag_session_create(
               &store, &session, cwd, "default", "fixture", "high", error, sizeof(error)) &&
           session.binary);
    json_t *data = json_pack("{s:s,s:s,s:s,s:{s:s,s:s,s:s}}", "connection_id",
        "0123456789abcdef0123456789abcdef", "provider", "default", "model", "fixture", "event",
        "type", "voice_transcript", "speaker", "user", "text", "native-pinned-caption");
    assert(data);
    json_t *transcript = json_incref(json_object_get(data, "event"));
    uint64_t sequence;
    assert(data &&
           !snag_session_commit(&session, "voice_event", data, &sequence, error, sizeof(error)));
    struct snag_render_source source = {.offset = session.committed_start,
        .len = (size_t)(session.committed_end - session.committed_start),
        .native_sequence = sequence};
    assert(!snag_session_binary_checkpoint_capture(
        &session, &source.native_boundary, NULL, NULL, error, sizeof(error)));
    struct output_capture capture = capture_open(false, true);
    struct snag_render render;
    snag_render_init(&render, 6u);
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    assert(!snag_render_set_view(&render, SNAG_RENDER_ROLLOUT));
    /* Live ASR dispatch is immediate; its durable observation does not repeat it. */
    assert(!snag_render_voice_event(&render, transcript, 0u, 0u));
    json_decref(transcript);
    assert(!snag_render_durable(&render, session.log_fd, source, "voice_event", 0u, 0u));
    assert(!snag_session_commit(&session, "retry_auto_changed", json_pack("{s:s}", "value", "on"),
        NULL, error, sizeof(error)));
    int64_t position = snag_seek(session.log_fd, 13, SEEK_SET);
    assert(position == 13);
    while (snag_render_view_pending(&render)) assert(!snag_render_flush_pending(&render, 8u));
    assert(snag_seek(session.log_fd, 0, SEEK_CUR) == position);
    struct snag_render_source invalid = source;
    invalid.native_sequence = invalid.native_boundary.next_seq;
    assert(snag_render_durable(&render, session.log_fd, invalid, "goal_lock_changed", 0u, 0u) < 0 &&
           errno == EINVAL);
    snag_render_free(&render);
    char output[4096];
    assert(capture_close(&capture, output, sizeof(output), 0u));
    assert(count_text(output, "native-pinned-caption") == 1u);
    snag_session_close(&session);
    snag_store_close(&store);
}

static void
test_voice_tool_history(void)
{
    for (unsigned int live = 0u; live <= 1u; ++live)
        for (unsigned int level = 0u; level <= 3u; ++level) {
            char path[] = "build/voice-tool-log-XXXXXX";
            int log_fd = mkstemp(path);
            assert(log_fd >= 0 && unlink(path) == 0);
            FILE *file = fdopen(log_fd, "w+");
            assert(file);
            struct output_capture capture = capture_open(false, true);
            assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
            struct snag_render render;
            snag_render_init(&render, live ? level : 6u);
            snag_render_set_color(&render, SNAG_COLOR_NEVER);
            if (!live) assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);

            struct snag_render_source source = append_event(file,
                "{\"data\":{\"event\":{\"type\":\"voice_response\","
                "\"operation\":\"interface_tool_started\",\"tool\":\"submit_input\","
                "\"tool_call_id\":\"voice-call\",\"arguments\":{\"target\":\"queue\","
                "\"text\":\"requested task\"}}}}\n");
            assert(snag_render_durable(&render, fileno(file), source, "voice_event", 0u, 0u) == 0);
            source = append_event(file,
                "{\"data\":{\"event\":{\"type\":\"voice_response\","
                "\"operation\":\"interface_tool\",\"tool\":\"submit_input\","
                "\"tool_call_id\":\"voice-call\",\"result\":{\"status\":\"succeeded\","
                "\"model_text\":\"Queued request fixture-queue.\"}}}}\n");
            assert(snag_render_durable(&render, fileno(file), source, "voice_event", 0u, 0u) == 0);
            bool streaming = live && !level;
            if (streaming) {
                while (snag_render_view_pending(&render)) {
                    assert(snag_render_flush_pending(&render, 8u) == 0);
                }
                assert(snag_render_rollout_begin(
                           &render, STDERR_FILENO, "assistant: ", SNAG_PRESENT_CONVERSATION) == 0);
                assert(snag_render_rollout(&render, "working before", 14u, NULL) == 0);
            }
            json_t *transcript = json_pack("{s:s,s:s,s:s}", "type", "voice_transcript", "speaker",
                "user", "text", "original spoken words");
            assert(snag_render_voice_event(&render, transcript, 0u, 0u) == 0);
            json_decref(transcript);
            transcript = json_pack("{s:s,s:s,s:s}", "type", "voice_transcript", "speaker",
                "assistant", "text", "spoken reply");
            assert(snag_render_voice_event(&render, transcript, 0u, 0u) == 0);
            json_decref(transcript);
            if (streaming) {
                assert(snag_render_rollout(&render, "working after", 13u, NULL) == 0);
                assert(snag_render_rollout_end(&render) == 0);
            }
            source = append_event(
                file, "{\"data\":{\"event\":{\"type\":\"voice_started\",\"state\":\"ready\"}}}\n");
            assert(snag_render_durable(&render, fileno(file), source, "voice_event", 0u, 0u) == 0);
            render.verbosity = level;
            assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
            while (snag_render_view_pending(&render))
                assert(snag_render_flush_pending(&render, 8u) == 0);

            char output[4096] = {0};
            size_t used = drain_available(capture.fd, output, sizeof(output), 0u);
            assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
            assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
            (void)drain_available(capture.fd, output, sizeof(output), used);
            snag_render_free(&render);
            fclose(file);
            capture_restore(&capture);
            close(capture.fd);
            assert((strstr(output, "→ voice: submit_input") != NULL) == (level >= 1u));
            assert((strstr(output, "← voice: submit_input") != NULL) == (level >= 1u));
            assert((strstr(output, "Queued request fixture-queue.") != NULL) == (level >= 2u));
            assert(!strstr(output, "ready"));
            assert(count_text(output, "submit_input") == (level ? 2u : 0u));
            assert(count_text(output, "You [voice, ASR]: original spoken words") == 1u);
            assert(count_text(output, "Voice model [generated]: spoken reply") == 1u);
            if (streaming) {
                assert(count_text(output, "working before") == 1u);
                assert(count_text(output, "working after") == 1u);
                assert(strstr(output, "working before") < strstr(output, "You [voice, ASR]"));
                assert(strstr(output, "Voice model [generated]") < strstr(output, "working after"));
            }
        }
}

static void
test_semantic_history(void)
{
    for (unsigned int live = 0u; live <= 1u; ++live)
        for (unsigned int rejected = 0u; rejected <= 1u; ++rejected)
            for (unsigned int level = 0u; level <= 3u; ++level) {
                char path[] = "build/verbosity-log-XXXXXX";
                int log_fd = mkstemp(path);
                assert(log_fd >= 0 && unlink(path) == 0);
                FILE *file = fdopen(log_fd, "w+");
                struct snag_render render;
                char args[1401], result[801], output[8192] = {0};
                struct output_capture capture = capture_open(false, true);
                assert(file);
                assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
                memset(args, 'A', sizeof(args) - 1u);
                args[sizeof(args) - 1u] = '\0';
                memset(result, 'R', sizeof(result) - 1u);
                result[sizeof(result) - 1u] = '\0';
                struct snag_buf response = {.max = 4096u};
                struct snag_buf finish = {.max = 4096u};
                assert(snag_buf_printf(&response,
                           "{\"data\":{\"items\":[{\"name\":\"future_tool\",\"call_id\":\"one\","
                           "\"arguments\":\"%s\"}]}}\n",
                           args) == 0);
                assert(snag_buf_terminate(&response) == 0);
                assert(snag_buf_printf(&finish,
                           "{\"data\":{\"call_id\":\"one\",\"result\":{"
                           "\"status\":\"%s\",\"reason\":\"invalid_arguments\",\"model_text\":\"%"
                           "s\"}}}\n",
                           rejected ? "not_run" : "failed", result) == 0);
                assert(snag_buf_terminate(&finish) == 0);
                snag_render_init(&render, live ? level : 6u);
                snag_render_set_color(&render, SNAG_COLOR_NEVER);
                if (!live) assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
                struct snag_render_source source = append_event(file, (char *)response.data);
                assert(snag_render_durable(
                           &render, fileno(file), source, "response_completed", 0u, 0u) == 0);
                source = append_event(
                    file, "{\"data\":{\"call_id\":\"one\",\"resolved_workdir\":\"/work\"}}\n");
                if (!rejected)
                    assert(snag_render_durable(
                               &render, fileno(file), source, "tool_started", 0u, 0u) == 0);
                source = append_event(file, (char *)finish.data);
                assert(snag_render_durable(
                           &render, fileno(file), source, "tool_finished", 0u, 0u) == 0);
                assert(snag_render_runtime(&render, "hidden-debug") == 0);
                assert(snag_render_protocol(&render, "hidden", "hidden-protocol", 15u) == 0);
                render.verbosity = level;
                assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
                while (snag_render_view_pending(&render))
                    assert(snag_render_flush_pending(&render, 8u) == 0);
                size_t used = drain_available(capture.fd, output, sizeof(output), 0u);
                assert((strstr(output, "future_tool") != NULL) == (level >= 1u));
                assert((strstr(output, "invalid_arguments") != NULL) == (rejected && level >= 1u));
                if (level == 1u && rejected)
                    assert(!strstr(output, "AAAA") && !strstr(output, "RRRR"));
                assert((strstr(output, "RRRR") != NULL) == (level >= 2u));
                assert((strstr(output, "[…]") != NULL) == (level == 2u));
                assert(!strstr(output, "hidden-debug") && !strstr(output, "hidden-protocol"));
                render.verbosity = 6u;
                assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
                assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
                (void)drain_available(capture.fd, output, sizeof(output), used);
                assert(count_text(output, "future_tool") == (level ? (rejected ? 1u : 2u) : 0u));
                snag_render_free(&render);
                snag_buf_free(&response);
                snag_buf_free(&finish);
                fclose(file);
                capture_restore(&capture);
                close(capture.fd);
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
    if (++change->calls == 2u) change->render->verbosity = change->level;
    return 0;
}

static void
test_live_downgrade(void)
{
    struct snag_render render;
    struct snag_render_block block;
    struct downgrade change = {&render, 0u, 2u};
    char payload[5000], output[8192] = {0};
    struct output_capture capture = capture_open(false, true);
    assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
    memset(payload, 'Q', sizeof(payload));
    memcpy(payload + sizeof(payload) - 12u, "secret-tail", 12u);
    json_t *result = json_object();
    assert(result && json_object_set_new(result, "model_text", json_string(payload)) == 0);
    assert(snag_render_prepare_tool_finish(&block, "future", NULL, result, 0u, 3u, 0u) == 0);
    snag_render_init(&render, 3u);
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    render.checkpoint = downgrade_checkpoint;
    render.checkpoint_opaque = &change;
    assert(snag_render_tool_block(&render, &block) == 0);
    (void)drain_available(capture.fd, output, sizeof(output), 0u);
    assert(strstr(output, "[…]") && !strstr(output, "secret-tail"));
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
    (void)drain_available(capture.fd, output, sizeof(output), 0u);
    assert(strstr(output, "[…]") && !strstr(output, "secret-tail"));
    snag_render_free(&render);
    capture_restore(&capture);
    close(capture.fd);
}

static void
test_append_only_views(unsigned int verbosity)
{
    struct snag_render render;
    struct snag_irc_event event = {0};
    char output[8192] = {0};
    size_t used = 0u;
    struct output_capture capture = capture_open(false, true);
    assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
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
    struct snag_buf delivered = {.max = 1024u};
    assert(snag_render_rollout_begin(
               &render, STDERR_FILENO, "agent › ", SNAG_PRESENT_CONVERSATION) == 0);
    assert(snag_render_rollout(&render, "hidden-prefix ", 14u, &delivered) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
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
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(strstr(output, "── rollout ──\n• Goal set\n• Compacted\n"
                          "agent › hidden-prefix live-suffix ") != NULL);
    assert(strstr(output, "queued-runtime") != NULL);
    assert(strstr(output, "chat-two") == NULL);

    /* An open live record is still the ordering head, but after its available
     * bytes are repainted it must not keep a view switch or input composer
     * waiting for the provider to close the response. */
    assert(!snag_render_view_pending(&render));
    assert(!snag_render_view_runnable(&render));

    assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
    assert(snag_render_rollout(&render, "hidden-tail", 11u, &delivered) == 0);
    assert(snag_render_rollout_end(&render) == 0);
    assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
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
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(count_text(output, "── rollout ──") == 2u);
    errno = 0;
    assert(snag_render_set_view(&render, (enum snag_render_view) - 1) < 0);
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
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(count_text(output, "agent › public-before-rename") == 1u);
    assert(count_text(output, "agent2 › public-after-rename") == 1u);
    assert(count_text(output, "peer-with-old-nick") == 1u);
    assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
    event.historical = true;
    memcpy(event.nick, "agent2", 7u);
    memcpy(event.text, "retained-own-message", 21u);
    assert(snag_render_irc_event(&render, &event) == 0);
    event.kind = SNAG_IRC_HISTORY_READY;
    strcpy(event.text, "replayed");
    event.historical = false;
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
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(!strstr(output, "retained-own-message"));
    assert(!strstr(output, "── history replayed ──"));
    assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(count_text(output, "agent2 › retained-own-message") == 1u);
    assert(!strstr(output, " history agent2"));
    assert(count_text(output, "-agent2 - own-public-notice") == 1u);
    assert(strstr(output, "· topic · /workspace\n") != NULL);
    assert(count_text(output, "── history replayed ──\n") == 1u);
    assert(!strstr(output, "history synchronized"));
    assert(strstr(output, "retained-own-message") < strstr(output, "── history replayed ──"));
    assert(strstr(output, "── history replayed ──") < strstr(output, "own-public-notice"));
    assert(snag_render_view(&render) == SNAG_RENDER_CHAT);
    assert(snag_render_rollout_begin(&render, STDERR_FILENO, NULL, SNAG_PRESENT_CONVERSATION) == 0);
    assert(snag_render_rollout(&render, "offline-private", 15u, NULL) == 0);
    assert(snag_render_rollout_end(&render) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(!strstr(output, "offline-private"));
    event.kind = SNAG_IRC_MESSAGE;
    memcpy(event.nick, "peer", 5u);
    memcpy(event.text, "offline-retained", 17u);
    assert(snag_render_irc_event(&render, &event) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(count_text(output, "offline-retained") == 1u);
    assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
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
    assert(strcmp((const char *)delivered.data, "hidden-prefix live-suffix hidden-tail") == 0);
    snag_buf_free(&delivered);
    snag_render_free(&render);
    capture_restore(&capture);
    close(capture.fd);
}

static void
test_chat_room_views(void)
{
    struct snag_render render;
    struct snag_irc_event event = {.kind = SNAG_IRC_MESSAGE, .timestamp_ms = 1000u};
    char output[8192] = {0};
    size_t used = 0u;
    struct output_capture capture = capture_open(false, true);

    assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
    snag_render_init(&render, 1u);
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    strcpy(event.endpoint, "irc-a");
    strcpy(event.nick, "peer");
    strcpy(event.room, "#one");
    strcpy(event.text, "one-before");
    assert(snag_render_irc_event(&render, &event) == 0);
    strcpy(event.room, "#two");
    strcpy(event.text, "two-before");
    assert(snag_render_irc_event(&render, &event) == 0);

    assert(snag_render_set_chat_room(&render, "irc-a", "#one", false) == 0);
    assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(strstr(output, "── chat irc-a #one ──\n"));
    assert(strstr(output, "one-before"));
    assert(!strstr(output, "two-before"));

    strcpy(event.room, "#one");
    strcpy(event.text, "one-live");
    assert(snag_render_irc_event(&render, &event) == 0);
    strcpy(event.room, "#two");
    strcpy(event.text, "two-hidden");
    assert(snag_render_irc_event(&render, &event) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(strstr(output, "one-live"));
    assert(!strstr(output, "two-hidden"));

    assert(snag_render_set_chat_room(&render, "irc-a", "#two", true) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(strstr(output, "── chat irc-a #two ──\n"));
    assert(strstr(output, "two-before"));
    assert(strstr(output, "two-hidden"));
    assert(count_text(output, "one-before") == 1u);
    assert(count_text(output, "one-live") == 1u);

    strcpy(event.room, "#one");
    strcpy(event.text, "one-later");
    assert(snag_render_irc_event(&render, &event) == 0);
    assert(snag_render_set_chat_room(&render, "irc-a", "#one", true) == 0);
    used = drain_available(capture.fd, output, sizeof(output), used);
    assert(strstr(output, "one-later"));
    assert(count_text(output, "two-before") == 1u);
    assert(count_text(output, "two-hidden") == 1u);

    snag_render_free(&render);
    capture_restore(&capture);
    close(capture.fd);
}

static void
test_async_render_backfill(void)
{
    struct snag_render render;
    snag_wake_fd wake[2];
    char path[] = "build/render-backfill-XXXXXX";
    char output[32768] = {0};
    struct output_capture capture = capture_open(false, true);
    int fd = mkstemp(path);

    assert(fd >= 0 && unlink(path) == 0);
    FILE *file = fdopen(fd, "w+");
    assert(file && fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
    assert(snag_wakeup_create(wake) == 0);
    snag_render_init(&render, 1u);
    snag_render_set_color(&render, SNAG_COLOR_NEVER);
    assert(snag_render_set_view(&render, SNAG_RENDER_CHAT) == 0);
    assert(snag_render_backfill_start(&render, wake[1]) == 0);

    struct snag_render_source response =
        append_event(file, "{\"data\":{\"items\":[{\"name\":\"exec_command\",\"call_id\":\"one\","
                           "\"arguments\":{\"command\":\"printf async\"}}]}}\n");
    assert(snag_render_durable(&render, fileno(file), response, "response_completed", 0u, 0u) == 0);
    for (unsigned int i = 0u; i < 24u; ++i) {
        struct snag_render_source source =
            append_event(file, "{\"data\":{\"call_id\":\"one\",\"resolved_workdir\":\"/tmp\"}}\n");
        assert(snag_render_durable(&render, fileno(file), source, "tool_started", 0u, 0u) == 0);
    }
    assert(snag_render_set_view(&render, SNAG_RENDER_ROLLOUT) == 0);
    for (unsigned int tries = 0u; snag_render_view_pending(&render) && tries < 200u; ++tries) {
        assert(snag_render_backfill_collect(&render) == 0);
        if (snag_render_view_runnable(&render))
            assert(snag_render_flush_pending(&render, 8u) == 0);
        else {
            assert(snag_wakeup_wait(wake[0], 1000) >= 0);
            snag_wakeup_drain(wake[0]);
        }
    }
    assert(!snag_render_view_pending(&render));
    (void)drain_available(capture.fd, output, sizeof(output), 0u);
    assert(count_text(output, "exec_command") == 24u);

    snag_render_free(&render);
    snag_wakeup_close(wake);
    assert(fclose(file) == 0);
    capture_restore(&capture);
    close(capture.fd);
}

static void
test_safe_text_width(void)
{
    const struct {
        const char *input, *output;
        size_t width;
    } cases[] = {{"\xff", "\\xFF", 4u}, {"\xe2\x82", "\\xE2\\x82", 8u}, {"\xc2\x9b", "\\x9B", 4u},
        {"\xe2\x80\xae", "\\u{202E}", 8u}, {"\x1b[31m", "\\x1B[31m", 8u}, {"a\t", "a   ", 4u},
        {"café界", "café界", 6u}, {"a\nb", "a\nb", 6u}};
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct snag_buf safe = {.max = 64u};
        const char *input = cases[i].input;
        assert(snag_term_append_safe(&safe, input, strlen(input)) == 0);
        assert(snag_buf_terminate(&safe) == 0);
        assert(strcmp((const char *)safe.data, cases[i].output) == 0);
        assert(snag_term_text_width(input, strlen(input)) == cases[i].width);
        snag_buf_free(&safe);
    }
}

/* Feed chunks through the public path with stdout captured, no terminal. */
static size_t
capture_citations(const char **chunks, size_t count, char *out, size_t out_size)
{
    struct snag_render render;
    size_t used = 0u;

    struct output_capture capture = capture_open(true, false);
    assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
    snag_render_init(&render, 0u);
    assert(snag_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    for (size_t i = 0u; i < count; ++i) {
        assert(snag_render_public(&render, chunks[i], strlen(chunks[i]), NULL) == 0);
        used = drain_available(capture.fd, out, out_size, used);
    }
    assert(snag_render_public_end(&render) == 0);
    used = capture_close(&capture, out, out_size, used);
    return used;
}

static void
test_citation_blocks(void)
{
    static const char open[] = "\xee\x88\x80";
    static const char close[] = "\xee\x88\x81";
    static const char sep[] = "\xee\x88\x82";
    char out[2048], text[512];
    const char *chunks[3];

    assert(strlen(open) == 3u && strlen(close) == 3u && strlen(sep) == 3u);

    /* Sorted, deduplicated turns collapse into one reference. */
    assert(snprintf(text, sizeof(text),
               "a%scite%sturn2view0%sturn1view0%s"
               "turn0view2%sbuild",
               open, sep, sep, sep, close) > 0);
    chunks[0] = text;
    assert(capture_citations(chunks, 1u, out, sizeof(out)) > 0u);
    assert(strcmp(out, "a[cite: turn 0-2]build") == 0);

    /* A block split across feeds is held until its terminator arrives. */
    char mid[64], tail[64];
    assert(snprintf(mid, sizeof(mid), "%scite%sturn3view1", open, sep) > 0);
    assert(snprintf(tail, sizeof(tail), "%sturn5view0%sy", sep, close) > 0);
    chunks[0] = "x";
    chunks[1] = mid;
    chunks[2] = tail;
    assert(capture_citations(chunks, 3u, out, sizeof(out)) > 0u);
    assert(strcmp(out, "x[cite: turn 3, 5]y") == 0);

    /* Non-consecutive and repeated turns stay one reference each. */
    assert(snprintf(text, sizeof(text),
               "%scite%sturn4view0%sturn4view1%s"
               "turn0view3%s",
               open, sep, sep, sep, close) > 0);
    chunks[0] = text;
    assert(capture_citations(chunks, 1u, out, sizeof(out)) > 0u);
    assert(strcmp(out, "[cite: turn 0, 4]") == 0);

    /* Unknown verb, malformed reference and unterminated block pass through. */
    assert(snprintf(text, sizeof(text), "%snote%sturn0view0%s", open, sep, close) > 0);
    chunks[0] = text;
    assert(capture_citations(chunks, 1u, out, sizeof(out)) > 0u);
    assert(strcmp(out, chunks[0]) == 0);
    assert(snprintf(text, sizeof(text), "%scite%sturnXview0%s", open, sep, close) > 0);
    chunks[0] = text;
    assert(capture_citations(chunks, 1u, out, sizeof(out)) > 0u);
    assert(strcmp(out, chunks[0]) == 0);
    assert(snprintf(text, sizeof(text), "z%scite%sturn1view0", open, sep) > 0);
    chunks[0] = text;
    assert(capture_citations(chunks, 1u, out, sizeof(out)) > 0u);
    assert(strcmp(out, chunks[0]) == 0);
}

static void
test_citation_fragment_budget(void)
{
    static const char *const inputs[] = {"x\xee\x88\x80"
                                         "cite\xee\x88\x82"
                                         "turn2view3\xee\x88\x81y",
        "x\xee\x88\x80"
        "note\xee\x88\x82"
        "turn2view3\xee\x88\x81y"};
    static const char *const expected[] = {"x[cite: turn 2]y", inputs[1]};
    for (size_t c = 0u; c < 2u; ++c) {
        size_t len = strlen(inputs[c]);
        for (size_t split = 0u; split <= len; ++split) {
            for (unsigned int rollout = 0u; rollout < 2u; ++rollout) {
                struct snag_render render;
                struct snag_buf delivered = {.max = 256u};
                char output[1024];
                struct output_capture capture = capture_open(true, false);
                snag_render_init(&render, 0u);
                assert((rollout ? snag_render_rollout_begin(
                                      &render, STDOUT_FILENO, NULL, SNAG_PRESENT_CONVERSATION)
                                : snag_render_public_begin(&render, STDOUT_FILENO, NULL)) == 0);
                for (unsigned int part = 0u; part < 2u; ++part) {
                    const char *text = inputs[c] + (part ? split : 0u);
                    size_t size = part ? len - split : split;
                    assert((rollout ? snag_render_rollout(&render, text, size, &delivered)
                                    : snag_render_public(&render, text, size, &delivered)) == 0);
                }
                assert((rollout ? snag_render_rollout_end(&render)
                                : snag_render_public_end(&render)) == 0);
                assert(snag_buf_terminate(&delivered) == 0);
                assert(!strcmp((const char *)delivered.data, expected[c]));
                assert(capture_close(&capture, output, sizeof(output), 0u) > 0u);
                assert(count_text(output, expected[c]) == 1u);
                snag_buf_free(&delivered);
                snag_render_free(&render);
            }
        }
    }
}

static void
test_tool_ref_rows(void)
{
    static const char long_id[] = "call_abcdef1234567890-very-long-provider-id";
    struct snag_response_item call = {.name = "exec", .call_id = long_id};
    struct snag_render_block start, finish;
    json_t *arguments = json_pack("{s:s}", "command", "true");
    json_t *result = json_pack("{s:s,s:i}", "status", "succeeded", "exit_code", 0);
    const char *start_ref, *finish_ref;

    assert(arguments != NULL && result != NULL);
    call.arguments = arguments;
    assert(snag_render_prepare_tool_start(&start, &call, "/work", 0u, 1u, 80u) == 0);
    assert(snag_render_prepare_tool_finish(&finish, call.name, long_id, result, 0u, 1u, 80u) == 0);
    assert(snag_buf_terminate(&start.text) == 0);
    assert(snag_buf_terminate(&finish.text) == 0);
    start_ref = strstr((char *)start.text.data, "[call_abc]");
    finish_ref = strstr((char *)finish.text.data, "[call_abc]");
    /* Both rows carry the same 8-character reference at the same column, even
       when the provider id is longer than the stored id limit. */
    assert(start_ref != NULL && finish_ref != NULL);
    assert((size_t)(start_ref - (char *)start.text.data) ==
           (size_t)(finish_ref - (char *)finish.text.data));
    assert(strstr((char *)start.text.data, "[call_abcdef1234567890") == NULL);
    snag_render_block_free(&start);
    snag_render_block_free(&finish);
    json_decref(arguments);
    json_decref(result);
}

static void
test_output_span_prompt_repaint(void)
{
    const char *frames[SNAG_TERM_SPINNER_COUNT] = {" ", " ", " "};
    struct snag_term term;
    struct snag_render render;
    struct snag_render_block block;
    struct output_capture capture = capture_open(false, true);
    char output[8192];
    char *body = malloc(5001u);
    json_t *result;

    assert(body != NULL);
    memset(body, 'x', 5000u);
    body[5000u] = '\0';
    snag_term_init(&term);
    term.opened = term.capable = true;
    term.columns = 80u;
    assert(fcntl(capture.fd, F_SETFL, O_NONBLOCK) == 0);
    snag_render_init(&render, 3u);
    render.stderr_terminal = true;
    snag_render_attach_term(&render, &term);
    assert(snag_term_set_prompt_template(&term, true, "BURST> ", frames, 1u, 0u) == 0);
    assert(prompt_output(capture.fd, output, sizeof(output)) > 0u);
    result = json_pack("{s:i,s:i,s:s,s:s}", "duration_ms", 3, "exit_code", 0, "status", "succeeded",
        "model_text", body);
    assert(result != NULL);
    assert(snag_render_prepare_tool_finish(&block, "exec", NULL, result, 0u, 3u, 80u) == 0);
    assert(snag_render_tool_block(&render, &block) == 0);
    snag_render_block_free(&block);
    json_decref(result);
    free(body);
    (void)prompt_output(capture.fd, output, sizeof(output));
    /* A logical tool burst must park and repaint the composer once, not once per internal output
     * slice. */
    assert(term.prompt_visible);
    assert(count_text(output, "BURST> ") == 1u);
    snag_render_free(&render);
    snag_term_close(&term);
    capture_restore(&capture);
    close(capture.fd);
}

static void
test_hosted_search_rows(void)
{
    static const char prefix[] = "→ web_search [ws_1]";
    struct snag_render_block block;
    json_t *action = json_pack("{s:s,s:s}", "query", "selinux 6.18", "type", "search");
    json_t *sources =
        json_pack("[s,s]", "https://example.test/selinux", "https://example.test/kernel");

    assert(action && sources);
    for (unsigned int level = 1u; level <= 3u; ++level) {
        assert(snag_render_prepare_hosted_start(&block, "ws_1", action, level, 0u) == 0);
        assert(block.role == SNAG_ROLE_ACTIVITY);
        assert(snag_buf_terminate(&block.text) == 0);
        assert(strncmp((char *)block.text.data, prefix, sizeof(prefix) - 1u) == 0);
        assert(strstr((char *)block.text.data, "\"query\":\"selinux 6.18\"") != NULL);
        assert((block.body.len != 0u) == (level >= 2u));
        snag_render_block_free(&block);

        assert(snag_render_prepare_hosted_finish(&block, "ws_1", "completed", sources, level, 0u) ==
               0);
        assert(block.role == SNAG_ROLE_SUCCESS);
        assert(snag_buf_terminate(&block.text) == 0);
        assert(
            strcmp((char *)block.text.data, "← web_search [ws_1]  completed · 2 sources\n") == 0);
        assert((block.body.len != 0u) == (level >= 2u));
        if (level >= 2u) {
            assert(snag_buf_terminate(&block.body) == 0);
            assert(strstr((char *)block.body.data, "https://example.test/selinux\n") != NULL);
        }
        snag_render_block_free(&block);
    }
    /* Without an action the row still identifies the item and invents nothing. */
    assert(snag_render_prepare_hosted_start(&block, "ws_2", NULL, 1u, 0u) == 0);
    assert(snag_buf_terminate(&block.text) == 0);
    assert(strcmp((char *)block.text.data, "→ web_search [ws_2]\n") == 0);
    assert(block.body.len == 0u);
    snag_render_block_free(&block);
    assert(snag_render_prepare_hosted_finish(&block, "ws_2", "in_progress", NULL, 1u, 0u) == 0);
    assert(block.role == SNAG_ROLE_WARNING);
    snag_render_block_free(&block);
    assert(snag_render_prepare_hosted_finish(&block, "ws_2", "failed", NULL, 1u, 0u) == 0);
    assert(block.role == SNAG_ROLE_ERROR);
    snag_render_block_free(&block);
    /* Any number of retained sources is counted and rendered. */
    {
        json_t *many = json_array();
        assert(many);
        for (size_t i = 0u; i < 40u; ++i) {
            char url[64];
            (void)snprintf(url, sizeof(url), "https://example.test/%zu", i);
            assert(json_array_append_new(many, json_string(url)) == 0);
        }
        assert(
            snag_render_prepare_hosted_finish(&block, "ws_many", "completed", many, 2u, 0u) == 0);
        assert(block.role == SNAG_ROLE_SUCCESS);
        assert(snag_buf_terminate(&block.text) == 0);
        assert(strstr((char *)block.text.data, "completed · 40 sources") != NULL);
        assert(snag_buf_terminate(&block.body) == 0);
        assert(strstr((char *)block.body.data, "https://example.test/0") != NULL);
        snag_render_block_free(&block);
        json_decref(many);
    }
    json_decref(action);
    json_decref(sources);
}

int
main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--read-presentation"))
        return read_presentation_file(argv[2], argv[3]);
    test_null_output();
    test_query_markdown_isolation();
    static const char markdown[] = "# **Live** _Markdown_\n"
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
        "┌─\n│ tilde fence\n└─\n"
        "\n• First prose line\n  continued prose\n\n• second paragraph\n\n";
    char output[4096];
    struct snag_render render;

    assert(setlocale(LC_ALL, "") != NULL);
    test_safe_text_width();
    assert(setenv("TZ", "UTC0", 1) == 0);
    tzset();
    test_local_mention_highlight();
    assert(capture_orientation(false, output, sizeof(output)) > 0u);
    assert(strcmp(output, SNAJPAGENT_IDENTITY " · /work/tree · session id 01234567\n") == 0);
    assert(strstr(output, "model-must-not-appear") == NULL);
    assert(capture_orientation(true, output, sizeof(output)) > 0u);
    assert(strcmp(output, SNAJPAGENT_IDENTITY " · resumed · /work/tree · session id 01234567"
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

    test_submit_holds_composer_until_activity();
    test_input_model_boundaries();
    test_model_prompt_boundaries();
    test_paragraph_spacing();
    test_live_paragraph_gap();
    test_interposed_paragraph_gap();
    test_spacing_classes();

    struct snag_buf delivered = {.max = 1024u};
    assert(capture_wrapped("alpha beta gamm", "a delta", 20u, true, "• alpha beta",
               "• alpha beta gamma", output, sizeof(output), &delivered) > 0u);
    assert(strcmp(output, "\n• alpha beta gamma\n  delta\n\n") == 0);
    assert(snag_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data, "alpha beta gamma delta") == 0);
    snag_buf_free(&delivered);
    snag_buf_init(&delivered, 64u);
    assert(capture_wrapped("123456789012345678 ", "next", 20u, true, "• 123456789012345678",
               "• 123456789012345678", output, sizeof(output), &delivered) > 0u);
    assert(snag_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data, "123456789012345678 next") == 0);
    snag_buf_free(&delivered);
    test_punctuation_word_boundaries();
    test_bounded_wrap_word();
    test_punctuation_wrapping();
    test_capable_terminal_hard_wrap();
    test_citation_blocks();
    test_citation_fragment_budget();

    snag_buf_init(&delivered, sizeof(markdown));
    assert(capture_markdown(markdown, true, true, SNAG_COLOR_NEVER, 120u, output, sizeof(output),
               &delivered) > 0u);
    assert(strcmp(output, rendered) == 0);
    assert(snag_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data, markdown) == 0);
    snag_buf_free(&delivered);
    assert(capture_markdown(
               markdown, false, false, SNAG_COLOR_NEVER, 120u, output, sizeof(output), NULL) > 0u);
    assert(strcmp(output, markdown) == 0);
    assert(capture_markdown(
               markdown, true, false, SNAG_COLOR_ALWAYS, 120u, output, sizeof(output), NULL) > 0u);
    assert(strstr(output, "\033[0;1;36mLive") != NULL);
    assert(strstr(output, "\033[0;33mcode") != NULL);
    assert(strstr(output, "\033[0;4;34mhttps://example.test") != NULL);
    assert(strstr(output, "\033[0;34;2mold") != NULL);
    assert(capture_markdown("**", true, true, SNAG_COLOR_NEVER, 120u, output, sizeof(output),
               NULL) == strlen("\n• **\n\n"));
    assert(strcmp(output, "\n• **\n\n") == 0);

    assert(capture_static_markdown(0u, output, sizeof(output)) > 0u);
    assert(count_text(output, "history: 1 shown · 2 completed among shown · 3 total") == 2u);
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
    assert(strstr(output, "00:00:01 -remote - **literal notice**\n") != NULL);
    assert(strstr(output, "remote › plain after quit\n") != NULL);
    assert(strstr(output, "remote › │ plain after quit\n") == NULL);
    assert(strstr(output, "user: **literal user**\n") != NULL);
    assert(strstr(output, "assistant: Saved answer\n") != NULL);
    assert(strstr(output, "remote › **literal agent**\n") != NULL);
    assert(strstr(output, "assistant: ## Literal assistant\n") != NULL);
    test_history_failure();
    test_history_turns();
    test_prompt_history();
    test_session_prompt_history();
    test_history_reader_boundaries();
    test_history_hold_count_uncapped();
    test_prompt_clock();
    test_prompt_spinners();
    test_native_input_yield();
    test_resize_checkpoint_preserves_newly_read_input();
    test_retained_prompt();
    test_styled_sink_matches_terminal();
    test_retained_presentation();
    test_retained_response_reopen();
    test_native_rebind();
    test_history_refresh_cursor();
    test_tool_ref_rows();
    test_output_span_prompt_repaint();
    test_mention_completion();
    test_completion_choices();
    test_dictation_editor();
    test_editor_grapheme_deletion();
    test_editor_literal_paste();
    test_destination_editor();
    test_query_tab_keeps_modal_queue_action();
    test_markdown_streaming();
    test_markdown_fences();
    test_banner_word_layout();
    test_help_layout();
    test_help_emphasis();
    test_update_banner();
    test_wrapped_markdown_tables();
    test_markdown_tables();
    test_tool_previews();
    test_hosted_search_rows();
    test_semantic_history();
    test_native_durable_source();
    test_voice_tool_history();
    test_live_downgrade();
    for (unsigned int verbosity = 0u; verbosity <= 6u; ++verbosity)
        test_append_only_views(verbosity);
    test_chat_room_views();
    test_query_send_receipts();
    test_async_render_backfill();

    assert(capture_lifecycle(0u, SNAG_COLOR_NEVER, output, sizeof(output)) > 0u);
    assert(strcmp(output, "• Compacted\n"
                          "• Goal set\n"
                          "• Goal updated\n"
                          "• Goal updated\n"
                          "• Goal cleared\n"
                          "• Goal cleared\n") == 0);
    assert(capture_lifecycle(4u, SNAG_COLOR_NEVER, output, sizeof(output)) > 0u);
    assert(strstr(output, "• Compacted\nevent › 1 compaction_completed synced\n"));
    assert(strstr(output, "• Goal cleared\nevent › 6 goal_cancelled synced\n"));
    assert(strstr(output, "event › 7 turn_completed synced\n"));
    assert(capture_lifecycle(0u, SNAG_COLOR_ALWAYS, output, sizeof(output)) > 0u);
    assert(count_text(output, "\033[1;32m• ") == 6u);
    assert(count_text(output, "\n\033[0m") == 6u);
    assert(capture_resume_hint(SNAG_COLOR_NEVER, output, sizeof(output)) > 0u);
    assert(strcmp(output, "• You can resume this session with the following command:\n"
                          "'snajpagent' --resume '0123'\n") == 0);
    assert(capture_resume_hint(SNAG_COLOR_ALWAYS, output, sizeof(output)) > 0u);
    assert(strcmp(output, "\033[1;32m• You can resume this session with the following command:"
                          "\033[0m\n'snajpagent' --resume '0123'\n") == 0);

    snag_render_init(&render, 6u);
    errno = 0;
    assert(snag_render_transport(&render, '>', "bad\rline", 8u) < 0);
    assert(errno == EINVAL);

    assert(capture_color(SNAG_COLOR_ALWAYS, false, 6u, 2500, 0u, 0u, output, sizeof(output)) > 0u);
    assert(strstr(output, "\033[1;36m› \033[0mplain\n") != NULL);
    assert(strstr(output, "\033[1;33m" SNAJPAGENT_NAME ": careful\n\033[0m") != NULL);
    assert(strstr(output, "\033[1;31m" SNAJPAGENT_NAME ": broken\n\033[0m") != NULL);
    assert(strstr(output, "\033[34mstatus\n\033[0m") != NULL);
    assert(strstr(output, "\033[1;32m• Compacted\n\033[0m") != NULL);
    assert(strstr(output, "event › 7 compaction_completed synced\n") != NULL);
    assert(strstr(output, "\033[33m→ exec_command\033[0m  {\"command\":\"printf plain\"") != NULL);
    assert(strstr(output, "  timeout: 2500ms\n") != NULL);
    assert(count_text(output, "\033[1;34magent \033[0m› \033[0manswer") == 4u);
    assert(count_text(output, "\033[1;36m@agent \033[0m› \033[0manswer") == 4u);
    assert(count_text(output, "\033[1;34m-agent \033[0m- \033[0manswer") == 4u);
    assert(count_text(output, "\033[1;36m-@agent \033[0m- \033[0manswer") == 4u);
    assert(capture_color(SNAG_COLOR_ALWAYS, true, 6u, -1, 0u, 0u, output, sizeof(output)) > 0u);
    assert(strstr(output, "\033[1;36m› \033[0mplain\n") != NULL);
    assert(count_text(output, "\033[1;34magent \033[0m› \033[0manswer") == 4u);
    assert(count_text(output, "\033[1;36m@agent \033[0m› \033[0manswer") == 4u);
    assert(count_text(output, "\033[1;34m-agent \033[0m- \033[0manswer") == 4u);
    assert(count_text(output, "\033[1;36m-@agent \033[0m- \033[0manswer") == 4u);
    assert(capture_color(SNAG_COLOR_NEVER, true, 6u, -1, 0u, 0u, output, sizeof(output)) > 0u);
    assert(strchr(output, '\033') == NULL);
    assert(strstr(output, "→ exec_command") == NULL);
    assert(capture_color(SNAG_COLOR_NEVER, false, 1u, -1, 0u, 0u, output, sizeof(output)) > 0u);
    assert(strstr(output, "→ exec_command  {\"command\":\"printf plain\"") != NULL);
    assert(strstr(output, "  arguments:") == NULL);
    assert(strstr(output, "  output:") == NULL);
    assert(strstr(output, "fixture tool output") == NULL);
    assert(capture_color(SNAG_COLOR_NEVER, true, 1u, -1, 0u, 0u, output, sizeof(output)) > 0u);
    assert(strstr(output, "→ exec_command") == NULL);
    assert(strstr(output, "  arguments:") == NULL);
    assert(strstr(output, "fixture tool output: café") == NULL);
    assert(capture_color(SNAG_COLOR_NEVER, false, 3u, -1, 4000u, 8u, output, sizeof(output)) > 0u);
    assert(strstr(output, "  timeout: 4000ms\n") != NULL);
    assert(strstr(output, "  arguments:") != NULL);
    assert(strstr(output, "  output:\nfixture ") != NULL);
    assert(strstr(output, "[…]") != NULL);
    assert(strstr(output, "fixture tool output: café") == NULL);

    assert(setenv("NO_COLOR", "1", 1) == 0);
    snag_render_init(&render, 0u);
    render.stderr_terminal = true;
    snag_render_set_color(&render, SNAG_COLOR_AUTO);
    assert(!render.color_stderr);
    assert(unsetenv("NO_COLOR") == 0);
    /* A draft at the direct-prompt cap must clamp, not fail. The old behaviour returned
     * EOVERFLOW from the insert, the runtime latched it as fatal, and the poll then returned it
     * on every pass without reading input - no keystroke, not even Ctrl-C, could be consumed, so
     * a session holding an oversized draft was unreachable except by signal. */
    {
        struct snag_term capper;
        size_t cap = SNAG_MAX_DIRECT_PROMPT;
        char *chunk = malloc(cap + 8u);
        assert(chunk != NULL);
        memset(chunk, 'a', cap + 8u);
        chunk[cap + 8u - 1u] = '\0';
        snag_term_init(&capper);
        assert(snag_term_insert_draft(&capper, chunk) == 0);
        assert(capper.draft.len == cap);
        assert(capper.draft_clamped);
        assert(snag_term_insert_draft(&capper, "more") == 0);
        assert(capper.draft.len == cap);
        snag_term_close(&capper);
        free(chunk);
    }
#define CANARY "canary-content-free-check"
    /* The composer trace is off unless asked, names the branch that suppressed a repaint, and is
     * content-free. */
    {
        char path[] = "/var/tmp/composer-trace-XXXXXX";
        struct snag_term traced;
        char buf[4096];
        ssize_t got;
        int fd = mkstemp(path);
        assert(fd >= 0);
        close(fd);
        assert(unlink(path) == 0);
        snag_term_init(&traced);
        snag_term_trace(&traced, "skip", "output_depth"); /* gate unset: must write nothing */
        assert(access(path, F_OK) != 0);
        assert(setenv("SNAJPAGENT_TERM_TRACE", path, 1) == 0);
        snag_term_trace(&traced, "skip", "output_depth");
        snag_term_trace(&traced, "paint", "compose_frame");
        {
            const char *frames[SNAG_TERM_SPINNER_COUNT];
            for (size_t i = 0u; i < SNAG_TERM_SPINNER_COUNT; ++i) frames[i] = "x";
            (void)snag_term_set_prompt_template(&traced, true, "trace", frames, 1u, 1u);
            /* A caller name containing "draft" must not be mistaken for content, so the
             * content-free rule is asserted against a canary that is provably in the draft. */
            assert(snag_term_insert_draft(&traced, CANARY) == 0);
            assert(traced.draft.len == strlen(CANARY));
            assert(snag_term_restore_draft(&traced, CANARY) == 0);
        }
        snag_term_close(&traced);
        assert(unsetenv("SNAJPAGENT_TERM_TRACE") == 0);
        fd = open(path, O_RDONLY);
        assert(fd >= 0);
        got = read(fd, buf, sizeof(buf) - 1u);
        close(fd);
        assert(got > 0);
        buf[got] = '\0';
        assert(strstr(buf, "ev=skip src=output_depth") != NULL);
        assert(strstr(buf, "ev=paint src=compose_frame") != NULL);
        assert(strstr(buf, "want=") != NULL && strstr(buf, "vis=") != NULL &&
               strstr(buf, "rows=") != NULL);
        assert(strstr(buf, "src=restore_draft") != NULL); /* a "draft" caller is not content */
        assert(strstr(buf, CANARY) == NULL);              /* and no content is recorded at all */
        assert(strstr(buf, "want-true src=set_prompt_template") != NULL);
        unlink(path);
    }
    /* B1: a plain ASCII/LF chunk skips the escaping pass, but every other class must still be
     * escaped or expanded exactly as before - these assertions fail if the fast path is taken for
     * a tab, a CR, an invalid byte, a wide glyph or any non-ASCII byte. */
    {
        struct snag_term t;
        snag_term_init(&t);
        t.opened = true;
        t.capable = true;
        snag_buf_reset(&t.output_line);
        snag_buf_reset(&t.output_cell);
        t.output_columns = 0u;
        t.output_newlines = 0u;
        assert(snag_term_note_output(&t, "abc def", 7u, "s") == 0);
        assert(t.output_line.len == 7u && memcmp(t.output_line.data, "abc def", 7u) == 0);
        assert(t.output_columns == 7u);
        assert(t.output_cell.len == 1u && memcmp(t.output_cell.data, "f", 1u) == 0);
        assert(t.output_cell_width == 1u);
        /* tab: expanded by column, so a fast path here would drop three spaces */
        snag_buf_reset(&t.output_line);
        snag_buf_reset(&t.output_cell);
        t.output_columns = 0u;
        t.output_newlines = 0u;
        assert(snag_term_note_output(&t, "a\tb", 3u, "s") == 0);
        assert(t.output_line.len == 5u && memcmp(t.output_line.data, "a   b", 5u) == 0);
        assert(t.output_columns == 5u);
        /* carriage return: escaped to four characters */
        snag_buf_reset(&t.output_line);
        snag_buf_reset(&t.output_cell);
        t.output_columns = 0u;
        t.output_newlines = 0u;
        assert(snag_term_note_output(&t, "a\rb", 3u, "s") == 0);
        assert(t.output_line.len == 6u && memcmp(t.output_line.data, "a\\x0Db", 6u) == 0);
        assert(t.output_columns == 6u);
        /* invalid UTF-8: escaped, not copied */
        snag_buf_reset(&t.output_line);
        snag_buf_reset(&t.output_cell);
        t.output_columns = 0u;
        t.output_newlines = 0u;
        assert(snag_term_note_output(&t, "\xff", 1u, "s") == 0);
        assert(t.output_line.len == 4u && memcmp(t.output_line.data, "\\xFF", 4u) == 0);
        assert(t.output_columns == 4u);
        /* wide glyph: preserved, width 2 */
        snag_buf_reset(&t.output_line);
        snag_buf_reset(&t.output_cell);
        t.output_columns = 0u;
        t.output_newlines = 0u;
        assert(snag_term_note_output(&t, "\xe4\xb8\xad", 3u, "s") == 0);
        assert(t.output_line.len == 3u && memcmp(t.output_line.data, "\xe4\xb8\xad", 3u) == 0);
        assert(t.output_columns == 2u && t.output_cell_width == 2u);
        /* newline resets the line model and is counted */
        snag_buf_reset(&t.output_line);
        snag_buf_reset(&t.output_cell);
        t.output_columns = 0u;
        t.output_newlines = 0u;
        assert(snag_term_note_output(&t, "ab\n", 3u, "s") == 0);
        assert(t.output_columns == 0u && t.output_line.len == 0u && t.output_newlines == 1u);
        snag_term_close(&t);
    }
    /* B1 follow-up: a plain chunk is accounted run-wise (one bulk append per line and the
     * closed-form wrap update), so long runs, wraps and line resets must land on the exact state
     * the glyph loop produced. */
    {
        struct snag_term t;
        snag_term_init(&t);
        t.opened = true;
        t.capable = true;
        t.columns = 5u;
        snag_buf_reset(&t.output_line);
        snag_buf_reset(&t.output_cell);
        t.output_columns = 0u;
        t.output_newlines = 0u;
        /* a run crossing the wrap boundary: 1..5, reset, 1..3 */
        assert(snag_term_note_output(&t, "abcdefgh", 8u, "s") == 0);
        assert(t.output_line.len == 8u && memcmp(t.output_line.data, "abcdefgh", 8u) == 0);
        assert(t.output_columns == 3u);
        assert(t.output_cell.len == 1u && memcmp(t.output_cell.data, "h", 1u) == 0);
        assert(t.output_cell_width == 1u);
        /* a full line wraps on the next run */
        snag_buf_reset(&t.output_line);
        snag_buf_reset(&t.output_cell);
        t.output_columns = 0u;
        t.output_newlines = 0u;
        assert(snag_term_note_output(&t, "abcde", 5u, "s") == 0);
        assert(t.output_columns == 5u);
        assert(snag_term_note_output(&t, "f", 1u, "s") == 0);
        assert(t.output_columns == 1u);
        assert(t.output_line.len == 6u && memcmp(t.output_line.data, "abcdef", 6u) == 0);
        assert(t.output_cell.len == 1u && memcmp(t.output_cell.data, "f", 1u) == 0);
        /* a newline mid-chunk resets the line model and keeps only the tail */
        snag_buf_reset(&t.output_line);
        snag_buf_reset(&t.output_cell);
        t.output_columns = 0u;
        t.output_newlines = 0u;
        assert(snag_term_note_output(&t, "ab\ncde", 6u, "s") == 0);
        assert(t.output_line.len == 3u && memcmp(t.output_line.data, "cde", 3u) == 0);
        assert(t.output_columns == 3u);
        assert(t.output_cell.len == 1u && memcmp(t.output_cell.data, "e", 1u) == 0);
        /* a trailing newline leaves an empty line model */
        snag_buf_reset(&t.output_line);
        snag_buf_reset(&t.output_cell);
        t.output_columns = 0u;
        t.output_newlines = 0u;
        assert(snag_term_note_output(&t, "xy\n", 3u, "s") == 0);
        assert(t.output_columns == 0u && t.output_line.len == 0u && t.output_cell.len == 0u);
        assert(t.output_newlines == 1u);
        snag_term_close(&t);
    }
    puts("test_render: ok");
    return 0;
}
