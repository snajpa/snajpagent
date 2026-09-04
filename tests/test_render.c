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
    assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
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
    used = drain_available(fds[0], out, out_size, used);
    assert(strcmp(out, "alpha beta gamm") == 0);
    assert(snj_render_public(&render, second, sizeof(second) - 1u, delivered) == 0);
    used = drain_available(fds[0], out, out_size, used);
    assert(strcmp(out, "alpha beta gamma\ndelta") == 0);
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
    assert(snj_render_public_abort(&render) == 0);
    assert(snj_render_public_begin(&render, STDOUT_FILENO, NULL) == 0);
    assert(snj_render_public(&render, "literal", 7u, NULL) == 0);
    assert(snj_render_public_end(&render) == 0);
    assert(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    while (read(fds[0], output, sizeof(output)) > 0)
        ;
    close(fds[0]);
    snj_term_close(&term);
}

static size_t
capture_color(enum snj_color_mode mode, bool networked,
              char *out, size_t out_size)
{
    struct snj_render render;
    struct snj_irc_event event;
    struct snj_response_item call;
    json_t *arguments;
    int fds[2];
    int saved;
    ssize_t n;
    size_t used = 0u;

    assert(pipe(fds) == 0);
    saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fds[1], STDERR_FILENO) >= 0);
    close(fds[1]);
    snj_render_init(&render, 6u);
    snj_render_set_networked(&render, networked, "agent");
    snj_render_set_color(&render, mode);
    assert(snj_render_submitted(&render, "› ", "plain") == 0);
    assert(snj_render_warning_ctx(&render, "careful") == 0);
    assert(snj_render_error_ctx(&render, "broken") == 0);
    assert(snj_render_host(&render, "status") == 0);
    memset(&call, 0, sizeof(call));
    arguments = json_object();
    assert(arguments != NULL);
    assert(json_object_set_new(arguments, "command",
                               json_string("printf plain")) == 0);
    call.name = "exec_command";
    call.arguments = arguments;
    assert(snj_render_tool_start(&render, &call, "/tmp") == 0);
    json_decref(arguments);
    memset(&event, 0, sizeof(event));
    event.kind = SNJ_IRC_MESSAGE;
    event.timestamp_ms = 1000u;
    memcpy(event.nick, "agent", 6u);
    memcpy(event.text, "answer", 7u);
    event.local = true;
    assert(snj_render_irc_event(&render, &event) == 0);
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
    static const char markdown[] =
        "# **Live** _Markdown_\n"
        "- item with `code` and [docs](https://example.test)\n"
        "> ~~old~~ new\n"
        "````c\nint main(void) { return 0; }\n````\n"
        "~~~text\ntilde fence\n~~~\n";
    static const char rendered[] =
        "Live Markdown\n"
        "• item with code and [docs] <https://example.test>\n"
        "│ old new\n"
        "┌─ c\n│ int main(void) { return 0; }\n└─\n"
        "┌─ text\n│ tilde fence\n└─\n";
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
    assert(capture_wrapped(output, sizeof(output), &delivered) > 0u);
    assert(strcmp(output, "alpha beta gamma\ndelta") == 0);
    assert(snj_buf_terminate(&delivered) == 0);
    assert(strcmp((const char *)delivered.data,
                  "alpha beta gamma delta") == 0);
    snj_buf_free(&delivered);

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
                            output, sizeof(output), NULL) == 2u);
    assert(strcmp(output, "**") == 0);

    assert(capture_static_markdown(output, sizeof(output)) > 0u);
    assert(strstr(output, "agent › answer and code\n") != NULL);
    assert(strstr(output, "@operator › **literal operator**\n") != NULL);
    assert(strstr(output, "remote › ┌─ c\n") != NULL);
    assert(strstr(output, "remote › │ int value = 1;\n") != NULL);
    assert(strstr(output, "remote › └─\n") != NULL);
    assert(strstr(output, "-remote - **literal notice**\n") != NULL);
    assert(strstr(output, "remote › plain after quit\n") != NULL);
    assert(strstr(output, "remote › │ plain after quit\n") == NULL);
    assert(strstr(output, "user: **literal user**\n") != NULL);
    assert(strstr(output, "assistant: Saved answer\n") != NULL);
    assert(strstr(output, "remote › **literal agent**\n") != NULL);
    assert(strstr(output, "assistant: ## Literal assistant\n") != NULL);
    test_history_failure();
    test_markdown_streaming();

    snj_render_init(&render, 6u);
    errno = 0;
    assert(snj_render_transport(&render, '>', "bad\rline", 8u) < 0);
    assert(errno == EINVAL);

    assert(capture_color(SNJ_COLOR_ALWAYS, false,
                         output, sizeof(output)) > 0u);
    assert(strstr(output, "\033[1;36m› \033[0mplain\n") != NULL);
    assert(strstr(output, "\033[1;33msnajpagent: careful\n\033[0m") != NULL);
    assert(strstr(output, "\033[1;31msnajpagent: broken\n\033[0m") != NULL);
    assert(strstr(output, "\033[34mstatus\n\033[0m") != NULL);
    assert(strstr(output,
                  "\033[33m→ exec\033[0m  'printf plain'\n") != NULL);
    assert(strstr(output, "\033[1;36magent \033[0m› answer") != NULL);
    assert(capture_color(SNJ_COLOR_NEVER, true,
                         output, sizeof(output)) > 0u);
    assert(strchr(output, '\033') == NULL);

    assert(setenv("NO_COLOR", "1", 1) == 0);
    snj_render_init(&render, 0u);
    render.stderr_terminal = true;
    snj_render_set_color(&render, SNJ_COLOR_AUTO);
    assert(!render.color_stderr);
    assert(unsetenv("NO_COLOR") == 0);
    puts("test_render: ok");
    return 0;
}
