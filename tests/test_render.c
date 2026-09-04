/* SPDX-License-Identifier: GPL-2.0-only */
#include "render.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
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
                            "%s%s%s\nmores", prefix, first,
                            punctuation[i]) > 0);
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
                            "%s1234567890 \n-something", prefix) > 0);
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

static size_t
capture_markdown(const char *text, bool enabled, bool split,
                 enum snj_color_mode color, char *out, size_t out_size,
                 struct snj_buf *delivered)
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
    term.columns = 120u;
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
capture_static_markdown(char *out, size_t out_size)
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
    snj_render_init(&render, 1u);
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
    assert(snj_render_history(&render, &session) == 0);
    snj_render_set_markdown(&render, false);
    event.kind = SNJ_IRC_MESSAGE;
    memcpy(event.text, "**literal agent**", 18u);
    assert(snj_render_irc_event(&render, &event) == 0);
    session.last_user = NULL;
    session.last_assistant = "## Literal assistant";
    assert(snj_render_history(&render, &session) == 0);
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
    assert(snj_render_history(&render, &session) < 0);
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
    assert(snj_render_tool_start(&render, &call, "/tmp",
                                 default_timeout_ms) == 0);
    json_decref(arguments);
    result = json_object();
    assert(result != NULL);
    assert(json_object_set_new(result, "duration_ms", json_integer(12)) == 0);
    assert(json_object_set_new(result, "exit_code", json_integer(0)) == 0);
    assert(json_object_set_new(result, "model_text",
                               json_string("fixture tool output: café\n")) == 0);
    assert(json_object_set_new(result, "reason", json_null()) == 0);
    assert(json_object_set_new(result, "status", json_string("succeeded")) == 0);
    assert(snj_render_tool_finish(&render, call.name, result,
                                  max_output_bytes) == 0);
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

static void
test_append_only_views(void)
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
    snj_render_init(&render, 1u);
    snj_render_set_color(&render, SNJ_COLOR_NEVER);
    snj_render_set_networked(&render, true, "agent");
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
    assert(capture_wrapped("alpha beta gamm", "a delta", 20u, true,
                           "• alpha beta gamm", "• alpha beta gamma\ndelta",
                           output, sizeof(output), &delivered) > 0u);
    assert(strcmp(output, "• alpha beta gamma\ndelta") == 0);
    assert(snj_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data,
                  "alpha beta gamma delta") == 0);
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

    assert(capture_static_markdown(output, sizeof(output)) > 0u);
    assert(strstr(output, "agent › • answer and code\n") != NULL);
    assert(strstr(output, "@operator › **literal operator**\n") != NULL);
    assert(strstr(output, "remote › ┌─ c\n") != NULL);
    assert(strstr(output, "remote › │ int value = 1;\n") != NULL);
    assert(strstr(output, "remote › └─\n") != NULL);
    assert(strstr(output, "-remote - **literal notice**\n") != NULL);
    assert(strstr(output, "remote › • plain after quit\n") != NULL);
    assert(strstr(output, "remote › │ plain after quit\n") == NULL);
    assert(strstr(output, "user: **literal user**\n") != NULL);
    assert(strstr(output, "assistant: Saved answer\n") != NULL);
    assert(strstr(output, "remote › **literal agent**\n") != NULL);
    assert(strstr(output, "assistant: ## Literal assistant\n") != NULL);
    test_history_failure();
    test_markdown_streaming();
    test_append_only_views();

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

    snj_render_init(&render, 6u);
    errno = 0;
    assert(snj_render_transport(&render, '>', "bad\rline", 8u) < 0);
    assert(errno == EINVAL);

    assert(capture_color(SNJ_COLOR_ALWAYS, false, 6u, 2500, 0u, 0u,
                         output, sizeof(output)) > 0u);
    assert(strstr(output, "\033[1;36m› \033[0mplain\n") != NULL);
    assert(strstr(output, "\033[1;33msnajpagent: careful\n\033[0m") != NULL);
    assert(strstr(output, "\033[1;31msnajpagent: broken\n\033[0m") != NULL);
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
