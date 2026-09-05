/* SPDX-License-Identifier: GPL-2.0-only */
#include "config.h"
#include "base.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
write_bytes(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, data, len) == (ssize_t)len);
    assert(close(fd) == 0);
}

static void
expect_invalid(const char *path)
{
    struct snag_config config;
    char error[256];

    snag_config_init(&config);
    error[0] = '\0';
    assert(snag_config_load(&config, path, NULL, error, sizeof(error)) < 0);
    assert(error[0] != '\0');
    snag_config_free(&config);
}

static void
expect_ui(const char *path, const char *key, const char *value, bool valid)
{
    struct snag_config config;
    char data[512], error[256] = {0};
    int n = snprintf(data, sizeof(data), "[ui]\n%s=%s\n", key, value);

    assert(n > 0 && (size_t)n < sizeof(data));
    write_bytes(path, data, (size_t)n);
    snag_config_init(&config);
    assert((snag_config_load(&config, path, NULL, error, sizeof(error)) == 0) ==
           valid);
    if (!valid)
        assert(error[0]);
    snag_config_free(&config);
}

static void
test_compact_setting(const char *path)
{
    static const struct {
        const char *text;
        uint32_t expected;
        bool valid;
    } cases[] = {
        {"auto", SNAG_CONFIG_COMPACT_AUTO, true},
        {"0", 0u, true},
        {"1", 1u, true},
        {"120000", 120000u, true},
        {"4000000", 4000000u, true},
        {"4000001", 0u, false},
        {"4294967295", 0u, false},
        {"-1", 0u, false},
        {"90%", 0u, false},
        {"automatic", 0u, false},
        {"auto\nauto_compact_input_tokens=1", 0u, false}
    };

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct snag_config config;
        char data[256], error[256] = {0};
        int n = snprintf(data, sizeof(data),
                         "[provider first]\nauto_compact_input_tokens=%s\n"
                         "[provider second]\n", cases[i].text);

        assert(n > 0 && (size_t)n < sizeof(data));
        write_bytes(path, data, (size_t)n);
        snag_config_init(&config);
        assert((snag_config_load(&config, path, NULL,
                                error, sizeof(error)) == 0) == cases[i].valid);
        if (cases[i].valid) {
            assert(config.providers[0].auto_compact_input_tokens ==
                   cases[i].expected);
            assert(config.providers[1].auto_compact_input_tokens ==
                   SNAG_CONFIG_COMPACT_AUTO);
        } else {
            assert(error[0]);
        }
        snag_config_free(&config);
    }
}

static void
test_batch_settings(const char *path)
{
    static const char *const values[] = {"0", "1", "4", "32", "33", "-1", "no"};
    for (size_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
        struct snag_config config;
        char text[128], error[256] = {0};
        int n = snprintf(text, sizeof(text), "[tool]\nmax_parallel_commands=%s\n"
                         "[provider]\nparallel_tool_calls=false\n", values[i]);
        write_bytes(path, text, (size_t)n);
        snag_config_init(&config);
        bool valid = i >= 1u && i <= 3u;
        assert((snag_config_load(&config, path, NULL, error, sizeof(error)) == 0) == valid);
        if (valid) {
            assert(config.max_parallel_commands == (uint32_t)strtoul(values[i], NULL, 10));
            assert(!config.providers[0].parallel_tool_calls);
        }
        snag_config_free(&config);
    }
    static const char duplicate[] = "[tool]\nmax_parallel_commands=4\nmax_parallel_commands=2\n";
    write_bytes(path, duplicate, sizeof(duplicate) - 1u);
    expect_invalid(path);
}

static void
test_auth_settings(const char *path)
{
    struct snag_config config;
    struct snag_provider_config provider;
    char error[256] = {0};
    static const char initial[] = "# retain this\n[ui]\ntyping_pause_ms=2\n";
    static const char invalid[] = "[provider]\nauth=chatgpt\nbase_url=https://other.test\n";
    static const char duplicate[] = "[provider]\nauth=env\nauth=api_key\n";

    write_bytes(path, invalid, sizeof(invalid) - 1u);
    expect_invalid(path);
    write_bytes(path, duplicate, sizeof(duplicate) - 1u);
    expect_invalid(path);
    write_bytes(path, initial, sizeof(initial) - 1u);
    snag_config_init(&config);
    provider = config.providers[0];
    strcpy(provider.name, "openrouter");
    strcpy(provider.base_url, "https://openrouter.ai/api/v1");
    strcpy(provider.api_key_env, "OPENROUTER_API_KEY");
    provider.auth = SNAG_AUTH_API_KEY;
    provider.native_compaction = false;
    assert(snag_config_save_provider(path, false, &provider, NULL, NULL,
                                     error, sizeof(error)) == 0);
    assert(snag_config_load(&config, path, NULL, error, sizeof(error)) == 0);
    assert(config.provider_count == 2u);
    assert(strcmp(config.providers[0].name, "default") == 0);
    assert(config.providers[0].auth == SNAG_AUTH_ENV);
    assert(config.providers[1].auth == SNAG_AUTH_API_KEY);
    assert(!config.providers[1].native_compaction);
    assert(config.typing_pause_ms == 2u);
    snag_config_free(&config);
    provider.auth = SNAG_AUTH_CHATGPT;
    assert(snag_config_validate_provider(&provider, error, sizeof(error)) < 0);
    strcpy(provider.base_url, SNAG_CHATGPT_BASE);
    assert(snag_config_validate_provider(&provider, error, sizeof(error)) == 0);
    assert(snag_config_save_provider(path, false, &provider, "chosen/model", "high",
                                     error, sizeof(error)) == 0);
    snag_config_init(&config);
    assert(snag_config_load(&config, path, NULL, error, sizeof(error)) == 0);
    assert(config.provider_count == 2u);
    assert(config.providers[1].auth == SNAG_AUTH_CHATGPT);
    assert(strcmp(config.provider, "openrouter") == 0);
    assert(strcmp(config.model, "chosen/model") == 0);
    snag_config_free(&config);
}

static void
test_prompt_numbers(const char *path)
{
    const char *values[SNAG_PROMPT_FIELD_COUNT] = {
        "p", "m", "e", "op", "host", "0%", "chat", "3", "7", "9"};
    static const char *const contexts[] = {"0%", "9%", "10%", "99%",
                                           "100%", "?%"};
    static const char *const padded[] = {"  0% ", "  9% ", " 10% ", " 99% ",
                                         "100% ", "  ?% "};
    static const char *const invalid[] = {
        "time", "context:", "context:0", "context:04", "hour:0", "hour:00",
        "hour:002", "hour:-2", "hour:+2", "minute: 2", "second:2 ",
        "hour:2:2", "context:511", "hour:0511", "context:99999999999999999999",
        "model:2", "provider:2", "mode:2", "goal_spinner:1",
        "activity_spinner:1", "provider_spinner", "tool_spinner", "host:2", "operator:2",
        "effort:2"};
    char label[SNAG_TERM_LABEL_BYTES], template[256];

    for (size_t i = 0u; i < sizeof(contexts) / sizeof(contexts[0]); ++i) {
        values[SNAG_PROMPT_CONTEXT] = contexts[i];
        assert(snag_config_prompt_expand(
            "{chat:{context:4}}{rollout-idle:x}{rollout-active:y}", 0u,
            values, 0xfdu, label, sizeof(label)) == 0);
        assert(strcmp(label, padded[i]) == 0);
    }
    assert(snag_config_prompt_expand(
        "{chat:{hour}:{minute}:{second}/{hour:2}:{minute:02}:{second:02}}"
        "{rollout-idle:x}{rollout-active:y}", 0u, values, 0xfdu,
        label, sizeof(label)) == 0);
    assert(strcmp(label, "3:7:9/ 3:07:09 ") == 0);
    for (unsigned int i = 0u; i <= 60u; ++i) {
        char number[4], expected[16];

        assert(snprintf(number, sizeof(number), "%u", i) > 0);
        values[SNAG_PROMPT_SECOND] = number;
        assert(snprintf(expected, sizeof(expected), "%u/%2u/%02u ", i, i, i) > 0);
        assert(snag_config_prompt_expand(
            "{chat:{second}/{second:2}/{second:02}}{rollout-idle:x}"
            "{rollout-active:y}", 0u, values, 0xfdu,
            label, sizeof(label)) == 0);
        assert(strcmp(label, expected) == 0);
    }
    values[SNAG_PROMPT_CONTEXT] = "100%";
    values[SNAG_PROMPT_HOUR] = "23";
    values[SNAG_PROMPT_MINUTE] = "59";
    values[SNAG_PROMPT_SECOND] = "60";
    assert(snag_config_prompt_expand(
        "{chat:{context:2}/{hour:1}:{minute:1}:{second:01}}"
        "{rollout-idle:x}{rollout-active:y}", 0u, values, 0xfdu,
        label, sizeof(label)) == 0);
    assert(strcmp(label, "100%/23:59:60 ") == 0);
    values[SNAG_PROMPT_HOUR] = values[SNAG_PROMPT_MINUTE] =
        values[SNAG_PROMPT_SECOND] = "--";
    assert(snag_config_prompt_expand(
        "{chat:{hour:02}:{minute:03}:{second:3}}{rollout-idle:x}"
        "{rollout-active:y}", 0u, values, 0xfdu, label, sizeof(label)) == 0);
    assert(strcmp(label, "--: --: -- ") == 0);
    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(snprintf(template, sizeof(template),
            "{chat:x}{rollout-idle:y}{rollout-active:{%s}}", invalid[i]) > 0);
        expect_ui(path, "prompt", template, false);
        /* Even an inactive mode must reject malformed numeric formats. */
        assert(snag_config_prompt_expand(template, 0u, values, 0xfdu,
                                         label, sizeof(label)) < 0);
    }
    expect_ui(path, "prompt",
        "{chat:{hour:0510}}{rollout-idle:{context:510}}{rollout-active:y}", true);
    assert(snag_config_prompt_expand(
        "{chat:{context:510}}{rollout-idle:x}{rollout-active:y}", 0u,
        values, 0xfdu, label, sizeof(label)) == 0);
    assert(strlen(label) == sizeof(label) - 1u);
    assert(strcmp(label + 506u, "100% ") == 0);
    assert(snag_config_prompt_expand(
        "{chat:{context:510}x}{rollout-idle:x}{rollout-active:y}", 0u,
        values, 0xfdu, label, sizeof(label)) < 0);
    assert(snag_config_prompt_expand(
        "{chat:\\{{hour:02}\\}\\\\}{rollout-idle:x}{rollout-active:y}", 0u,
        values, 0xfdu, label, sizeof(label)) == 0);
    assert(strcmp(label, "{--}\\ ") == 0);
}

static void
test_openrouter_provider(void)
{
    static const struct {
        const char *url;
        bool openrouter;
    } cases[] = {
        {"https://openrouter.ai/api/v1", true},
        {"https://openrouter.ai/api/v1/", true},
        {"https://openrouter.ai", true},
        {"http://OPENROUTER.AI:80/api/v1", true},
        {"https://OpenRouter.Ai.:443/api/v1", true},
        {"https://openrouter.ai.", true},
        {"https://openrouter.ai:65535/api/v1", true},
        {"https://api.openai.com", false},
        {"http://127.0.0.1:2455/backend-api/codex", false},
        {"https://example.test/openrouter.ai/api/v1", false},
        {"https://openrouter.ai.example.test/api/v1", false},
        {"https://notopenrouter.ai/api/v1", false},
        {"https://api.openrouter.ai/api/v1", false},
        {"https://openrouter.ai@elsewhere.test/api/v1", false},
        {"https://user@openrouter.ai/api/v1", false},
        {"https://openrouter.ai:443@elsewhere.test/api/v1", false},
        {"https://openrouter.ai:0/api/v1", false},
        {"https://openrouter.ai:65536/api/v1", false},
        {"https://openrouter.ai:999999999999/api/v1", false},
        {"https://openrouter.ai:/api/v1", false},
        {"https://openrouter.ai:abc/api/v1", false},
        {"https://openrouter.ai..", false},
        {"https://openrouter.ai?host=example.test", false},
        {"ftp://openrouter.ai/api/v1", false},
        {"", false}
    };
    struct snag_provider_config provider = {0};

    assert(!snag_config_provider_is_openrouter(NULL));
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        (void)snprintf(provider.base_url, sizeof(provider.base_url), "%s", cases[i].url);
        (void)snprintf(provider.name, sizeof(provider.name), "%s",
                       cases[i].openrouter ? "arbitrary" : "openrouter");
        assert(snag_config_provider_is_openrouter(&provider) == cases[i].openrouter);
    }
}

int
main(void)
{
    static const char valid[] =
        "[agent]\n"
        "provider = backup\n"
        "model = gpt-5.5\n"
        "reasoning_effort = future-effort\n"
        "max_goal_prompt_bytes = 123456\n"
        "read_agents_md = false\n"
        "\n[provider]\n"
        "connect_timeout_ms = 1000\n"
        "idle_timeout_ms = 2000\n"
        "request_timeout_ms = 3000\n"
        "auto_compact_input_tokens = 12345\n"
        "base_url = http://127.0.0.1:2455/backend-api/codex/\n"
        "api_key_env = CODEX_LB_API_KEY\n"
        "openrouter_referer = https://github.com/snajpa/snajpagent\n"
        "openrouter_title = snajpagent\n"
        "exact_token_count = false\n"
        "native_compaction = 0\n"
        "\n[provider backup]\n"
        "base_url = https://backup.example.test/v1\n"
        "api_key_env = BACKUP_API_KEY\n"
        "exact_token_count = true\n"
        "\n[model-limit default/gpt-5.5]\n"
        "context_window_tokens = 1050000\n"
        "max_input_tokens = 922000\n"
        "max_output_tokens = 128000\n"
        "\n[model-limit backup/org/model/with/slashes]\n"
        "max_input_tokens = 4000000000\n"
        "\n[ui]\n"
        "color = never\n"
        "markdown = false\n"
        "resume_history_turns = 0\n"
        "typing_pause_ms = 750\n"
        "prompt = pre{chat:{operator}{goal_spinner}:}{rollout-idle:{provider}/{model}/{effort} {context}{activity_spinner}›}{rollout-active:{mode}{activity_spinner}»}\n"
        "prompt_spinner_goal = \"\\0◆\"\n"
        "prompt_spinner_provider = \" \\|/-\"\n"
        "prompt_spinner_tool = \" \"\n"
        "prompt_spinner_per_second = 60\n"
        "\n[irc]\n"
        "listen = 127.0.0.1:7667\n"
        "client = irc-a.example\n"
        "client = [2001:db8::20]:7667\n"
        "model_nick = builder\n"
        "operator_nick = alice\n"
        "room_name = build-host\n"
        "history_lines = 321\n"
        "\n[tool]\n"
        "shell = /bin/sh\n"
        "default_yield_ms = 0\n"
        "default_timeout_ms = 4000\n"
        "max_timeout_ms = 5000\n"
        "max_output_tokens = 7654\n"
        "max_output_bytes = 123456\n"
        "secret_env = TOKEN_ONE, TOKEN_TWO\n";
    char temp[] = "/tmp/snajpagent-config-XXXXXX";
    char dotdir[4096];
    char path[4096];
    char link_path[4096];
    char error[256];
    struct snag_config config;
    char *shell;

    assert(setlocale(LC_CTYPE, "") != NULL);
    assert(mkdtemp(temp));
    assert(snprintf(dotdir, sizeof(dotdir), "%s/dotdir", temp) > 0);
    assert(mkdir(dotdir, 0700) == 0);

    snag_config_init(&config);
    assert(snag_config_load(&config, NULL, dotdir,
                           error, sizeof(error)) == 0);
    assert(strcmp(config.model, "default") == 0);
    assert(config.provider[0] == '\0');
    assert(strcmp(config.reasoning_effort, "default") == 0);
    assert(config.max_goal_prompt_bytes == 256u * 1024u);
    assert(config.read_agents_md);
    assert(config.markdown);
    assert(config.resume_history_turns == 2u);
    assert(config.typing_pause_ms == 500u);
    assert(strstr(config.prompt, "{chat:{goal_spinner}{activity_spinner} "));
    assert(strstr(config.prompt, "{context:4}"));
    assert(strstr(config.prompt, "{goal_spinner}") != NULL);
    assert(strcmp(config.prompt_spinner_goal, " ⚑") == 0);
    assert(strcmp(config.prompt_spinner_provider, " ◴◷◶◵") == 0);
    assert(strcmp(config.prompt_spinner_tool, " ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏") == 0);
    assert(config.prompt_spinner_per_second == 8u);
    {
        static const char *const contexts[] = {"0%", "9%", "10%", "99%", "100%", "?%"};
        const char *values[SNAG_PROMPT_FIELD_COUNT] = {
            "p", "m", "e", "op", "host", "0%", "chat", "3", "7", "9"};
        char expanded[128], expected[128];

        assert(snag_config_prompt_expand(config.prompt, 0u, values, 0xfdu,
                                        expanded, sizeof(expanded)) == 0);
        assert(strcmp(expanded, "\xfd\xfe 03:07:09 op@host : ") == 0);
        for (size_t i = 0u; i < sizeof(contexts) / sizeof(contexts[0]); ++i) {
            values[SNAG_PROMPT_CONTEXT] = contexts[i];
            for (unsigned int mode = 1u; mode <= 2u; ++mode) {
                assert(snag_config_prompt_expand(config.prompt, mode, values, 0xfdu,
                                                expanded, sizeof(expanded)) == 0);
                assert(snprintf(expected, sizeof(expected), "\xfd\xfe%4s p/m/e %s ",
                                contexts[i], mode == 1u ? "›" : "»") > 0);
                assert(strcmp(expanded, expected) == 0);
            }
        }
    }
    assert(!config.irc_listen_explicit);
    assert(strcmp(config.irc_listen, "localhost:6667") == 0);
    assert(config.irc_client_count == 0u);
    assert(config.irc_history_lines == 200u);
    assert(config.default_timeout_ms == 0u);
    assert(config.max_timeout_ms == 86400000u);
    assert(config.max_output_tokens == 6000u);
    assert(config.max_parallel_commands == 4u);
    assert(config.providers[0].parallel_tool_calls);
    assert(config.max_output_bytes == 0u);
    assert(config.provider_count == 1u);
    assert(strcmp(config.providers[0].name, "default") == 0);
    assert(config.providers[0].auto_compact_input_tokens ==
           SNAG_CONFIG_COMPACT_AUTO);
    assert(config.providers[0].exact_token_count == SNAG_TOKEN_COUNT_AUTO);
    assert(config.providers[0].native_compaction);
    test_openrouter_provider();
    assert(strcmp(config.providers[0].base_url, "https://api.openai.com") == 0);
    assert(strcmp(config.providers[0].api_key_env, "OPENAI_API_KEY") == 0);
    assert(config.providers[0].openrouter_referer[0] == '\0');
    assert(config.providers[0].openrouter_title[0] == '\0');
    assert(config.secret_env_count == 0u);
    shell = realpath("/bin/sh", NULL);
    assert(shell);
    assert(strcmp(config.shell, shell) == 0);
    free(shell);
    snag_config_free(&config);

    assert(snprintf(path, sizeof(path), "%s/valid.ini", temp) > 0);
    write_bytes(path, valid, sizeof(valid) - 1u);
    snag_config_init(&config);
    config.irc_model_nick_implicit = true;
    config.irc_operator_nick_implicit = true;
    assert(snag_config_load(&config, path, dotdir,
                           error, sizeof(error)) == 0);
    assert(strcmp(config.model, "gpt-5.5") == 0);
    assert(strcmp(config.provider, "backup") == 0);
    assert(strcmp(config.reasoning_effort, "future-effort") == 0);
    assert(config.max_goal_prompt_bytes == 123456u);
    assert(!config.read_agents_md);
    assert(config.provider_count == 2u);
    assert(strcmp(config.providers[0].name, "default") == 0);
    assert(config.providers[0].connect_timeout_ms == 1000u);
    assert(config.providers[0].idle_timeout_ms == 2000u);
    assert(config.providers[0].request_timeout_ms == 3000u);
    assert(config.providers[0].auto_compact_input_tokens == 12345u);
    assert(strcmp(config.providers[0].base_url,
                  "http://127.0.0.1:2455/backend-api/codex") == 0);
    assert(strcmp(config.providers[0].api_key_env, "CODEX_LB_API_KEY") == 0);
    assert(strcmp(config.providers[0].openrouter_referer,
                  "https://github.com/snajpa/snajpagent") == 0);
    assert(strcmp(config.providers[0].openrouter_title, "snajpagent") == 0);
    assert(config.providers[0].exact_token_count == SNAG_TOKEN_COUNT_OFF);
    assert(!config.providers[0].native_compaction);
    assert(strcmp(config.providers[1].name, "backup") == 0);
    assert(strcmp(config.providers[1].base_url,
                  "https://backup.example.test/v1") == 0);
    assert(strcmp(config.providers[1].api_key_env, "BACKUP_API_KEY") == 0);
    assert(config.providers[1].exact_token_count == SNAG_TOKEN_COUNT_STRICT);
    assert(config.model_limit_count == 2u);
    {
        const struct snag_model_limit_config *limit =
            snag_config_model_limit(&config, "default", "gpt-5.5");
        assert(limit);
        assert(limit->context_window_known);
        assert(limit->context_window_tokens == UINT64_C(1050000));
        assert(limit->max_input_known);
        assert(limit->max_input_tokens == UINT64_C(922000));
        assert(limit->max_output_known);
        assert(limit->max_output_tokens == UINT64_C(128000));
    }
    {
        const struct snag_model_limit_config *limit =
            snag_config_model_limit(&config, "backup",
                                   "org/model/with/slashes");
        assert(limit);
        assert(!limit->context_window_known);
        assert(limit->max_input_known);
        assert(limit->max_input_tokens == SNAG_CONFIG_TOKEN_LIMIT_MAX);
        assert(!limit->max_output_known);
    }
    assert(snag_config_model_limit(&config, "default", "missing") == NULL);
    assert(snag_config_provider(&config, NULL) == &config.providers[0]);
    assert(snag_config_provider(&config, "backup") == &config.providers[1]);
    assert(snag_config_provider(&config, "missing") == NULL);
    assert(config.color == SNAG_COLOR_NEVER);
    assert(!config.markdown);
    assert(config.resume_history_turns == 0u);
    assert(config.typing_pause_ms == 750u);
    assert(strncmp(config.prompt, "pre{chat:", 9u) == 0);
    assert(strcmp(config.prompt_spinner_goal, "\\0◆") == 0);
    assert(strcmp(config.prompt_spinner_provider, " \\|/-") == 0);
    assert(strcmp(config.prompt_spinner_tool, " ") == 0);
    assert(config.prompt_spinner_per_second == 60u);
    assert(config.irc_listen_explicit);
    assert(strcmp(config.irc_listen, "127.0.0.1:7667") == 0);
    assert(config.irc_client_count == 2u);
    assert(strcmp(config.irc_clients[0], "irc-a.example") == 0);
    assert(strcmp(config.irc_clients[1], "[2001:db8::20]:7667") == 0);
    assert(strcmp(config.irc_model_nick, "builder") == 0);
    assert(strcmp(config.irc_operator_nick, "alice") == 0);
    assert(!config.irc_model_nick_implicit);
    assert(!config.irc_operator_nick_implicit);
    assert(strcmp(config.irc_room_name, "build-host") == 0);
    assert(config.irc_history_lines == 321u);
    assert(config.default_yield_ms == 0u);
    assert(config.default_timeout_ms == 4000u);
    assert(config.max_timeout_ms == 5000u);
    assert(config.max_output_tokens == 7654u);
    assert(config.max_output_bytes == 123456u);
    assert(config.secret_env_count == 2u);
    assert(strcmp(config.secret_env[0], "TOKEN_ONE") == 0);
    assert(strcmp(config.secret_env[1], "TOKEN_TWO") == 0);
    snag_config_free(&config);

    assert(snprintf(path, sizeof(path), "%s/config.ini", dotdir) > 0);
    write_bytes(path, valid, sizeof(valid) - 1u);
    snag_config_init(&config);
    assert(snag_config_load(&config, NULL, dotdir,
                           error, sizeof(error)) == 0);
    assert(config.provider_count == 2u);
    assert(strcmp(config.providers[0].name, "default") == 0);
    assert(strcmp(config.providers[1].name, "backup") == 0);
    snag_config_free(&config);

    assert(snprintf(path, sizeof(path), "%s/valid.ini", temp) > 0);

    expect_ui(path, "prompt_spinner_goal", "\"\\0\"", true);
    expect_ui(path, "prompt_spinner_goal", "\" \"", true);
    expect_ui(path, "prompt_spinner_goal", "\"\\0◆\"", true);
    expect_ui(path, "prompt_spinner_goal",
              "\"\\0abcdefghijklmnop\"", true);
    expect_ui(path, "prompt_spinner_goal",
              "\"\\0abcdefghijklmnopq\"", false);
    expect_ui(path, "prompt_spinner_goal", "\"\"", false);
    expect_ui(path, "prompt_spinner_goal", "unquoted", false);
    expect_ui(path, "prompt_spinner_goal", "\" aab\"", false);
    expect_ui(path, "prompt_spinner_goal",
              "\"\\0" "\xcc\x81" "\"", false);
    expect_ui(path, "prompt_spinner_goal",
              "\"\\0" "\xe2\x80\x8b" "\"", false);
    expect_ui(path, "prompt_spinner_goal",
              "\"\\0" "\xe2\x80\xae" "\"", false);
    expect_ui(path, "prompt_spinner_goal",
              "\"\\0" "\xe7\x95\x8c" "\"", false);
    expect_ui(path, "prompt_spinner_interrupt", "\" x\"", false);
    expect_ui(path, "prompt_spinner_per_second", "0", false);
    expect_ui(path, "prompt_spinner_per_second", "61", false);

    expect_ui(path, "prompt",
        "{chat:x}{rollout-idle:y}{rollout-active:z}", true);
    expect_ui(path, "prompt",
        "{chat:x}{chat:y}{rollout-idle:y}{rollout-active:z}", false);
    expect_ui(path, "prompt", "{chat:x}{rollout-idle:y}", false);
    expect_ui(path, "prompt",
        "{chat:{rollout-idle:x}}{rollout-idle:y}{rollout-active:z}", false);
    expect_ui(path, "prompt",
        "{chat:{unknown}}{rollout-idle:y}{rollout-active:z}", false);
    expect_ui(path, "prompt",
        "{chat:{goal_spinner}{goal_spinner}}{rollout-idle:y}"
        "{rollout-active:z}", false);
    expect_ui(path, "prompt",
        "{chat:{activity_spinner}{activity_spinner}}{rollout-idle:y}"
        "{rollout-active:z}", false);
    expect_ui(path, "prompt", "{chat:}{rollout-idle:y}{rollout-active:z}",
              false);
    {
        const char *values[SNAG_PROMPT_FIELD_COUNT] = {
            "prov", "model", "high", "", "host", "0%", "rollout-idle",
            "12", "34", "56"};
        const char template[] =
            "pre{chat:{hour:02}:{minute:02}:{second:02} {operator}:}"
            "{rollout-idle:{provider}/{model}/{effort} "
            "{context}{goal_spinner}›}{rollout-active:A}";
        const unsigned char expected[] = {
            'p','r','e','p','r','o','v','/','m','o','d','e','l','/','h','i','g','h',
            ' ','0','%',0xfd,0xe2,0x80,0xba,' ','\0'
        };
        char expanded[128];

        assert(snag_config_prompt_expand(template, 1u, values, 0xfdu,
                                         expanded, sizeof(expanded)) == 0);
        assert(memcmp(expanded, expected, sizeof(expected)) == 0);
        assert(snag_config_prompt_expand(template, 0u, values, 0xfdu,
                                         expanded, sizeof(expanded)) == 0);
        assert(strcmp(expanded, "pre12:34:56 : ") == 0);
        assert(snag_config_prompt_expand(
            "{chat:{operator}}{rollout-idle:x}{rollout-active:y}", 0u,
            values, 0xfdu, expanded, sizeof(expanded)) < 0);
    }

    test_compact_setting(path);
    test_batch_settings(path);
    test_auth_settings(path);
    test_prompt_numbers(path);
    assert(snprintf(link_path, sizeof(link_path), "%s/link.ini", temp) > 0);
    assert(symlink(path, link_path) == 0);
    expect_invalid(link_path);

    write_bytes(path, "[ui]\nverbosity=1\nverbosity=2\n",
                sizeof("[ui]\nverbosity=1\nverbosity=2\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[unknown]\nvalue=1\n",
                sizeof("[unknown]\nvalue=1\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[ui]\nverbosity=7\n",
                sizeof("[ui]\nverbosity=7\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[ui]\ntyping_pause_ms=5001\n",
                sizeof("[ui]\ntyping_pause_ms=5001\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[ui]\ncolor=sometimes\n",
                sizeof("[ui]\ncolor=sometimes\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[ui]\nmarkdown=maybe\n",
                sizeof("[ui]\nmarkdown=maybe\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[irc]\nhistory_lines=0\n",
                sizeof("[irc]\nhistory_lines=0\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[irc]\nname=legacy\n",
                sizeof("[irc]\nname=legacy\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[irc]\noperator_name=legacy\n",
                sizeof("[irc]\noperator_name=legacy\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[irc]\nclient=localhost\nclient=localhost\n",
                sizeof("[irc]\nclient=localhost\nclient=localhost\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[ui]\ntyping_pause_ms=1\ntyping_pause_ms=2\n",
                sizeof("[ui]\ntyping_pause_ms=1\ntyping_pause_ms=2\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[ui]\nmarkdown=true\nmarkdown=false\n",
                sizeof("[ui]\nmarkdown=true\nmarkdown=false\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
        "[tool]\ndefault_timeout_ms=5000\nmax_timeout_ms=4000\n",
        sizeof("[tool]\ndefault_timeout_ms=5000\nmax_timeout_ms=4000\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[tool]\nmax_timeout_ms=4294967295\n",
                sizeof("[tool]\nmax_timeout_ms=4294967295\n") - 1u);
    snag_config_init(&config);
    assert(snag_config_load(&config, path, dotdir,
                           error, sizeof(error)) == 0);
    assert(config.max_timeout_ms == UINT32_MAX);
    snag_config_free(&config);
    write_bytes(path, "[tool]\nmax_timeout_ms=4294967296\n",
                sizeof("[tool]\nmax_timeout_ms=4294967296\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[tool]\nmax_output_bytes=4294967295\n",
                sizeof("[tool]\nmax_output_bytes=4294967295\n") - 1u);
    snag_config_init(&config);
    assert(snag_config_load(&config, path, dotdir,
                           error, sizeof(error)) == 0);
    assert(config.max_output_bytes == UINT32_MAX);
    snag_config_free(&config);
    write_bytes(path, "[tool]\nmax_output_bytes=4294967296\n",
                sizeof("[tool]\nmax_output_bytes=4294967296\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[tool]\ndefault_max_output_tokens=6000\n",
                sizeof("[tool]\ndefault_max_output_tokens=6000\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[tool]\nmax_output_tokens=0\n",
                sizeof("[tool]\nmax_output_tokens=0\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[tool]\nmax_output_tokens=4000000001\n",
                sizeof("[tool]\nmax_output_tokens=4000000001\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
        "[tool]\nmax_output_tokens=1\nmax_output_tokens=2\n",
        sizeof("[tool]\nmax_output_tokens=1\n"
               "max_output_tokens=2\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[tool]\nsecret_env=A,A\n",
                sizeof("[tool]\nsecret_env=A,A\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[provider]\nbase_url=ftp://example.test\n",
                sizeof("[provider]\nbase_url=ftp://example.test\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[provider]\nbase_url=https://example.test/a?b\n",
                sizeof("[provider]\nbase_url=https://example.test/a?b\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[provider]\napi_key_env=BAD-NAME\n",
                sizeof("[provider]\napi_key_env=BAD-NAME\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[provider]\nexact_token_count=maybe\n",
                sizeof("[provider]\nexact_token_count=maybe\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[provider]\nnative_compaction=yes\n",
                sizeof("[provider]\nnative_compaction=yes\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[agent]\nmax_goal_prompt_bytes=0\n",
                sizeof("[agent]\nmax_goal_prompt_bytes=0\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[agent]\nmax_goal_prompt_bytes=1048577\n",
                sizeof("[agent]\nmax_goal_prompt_bytes=1048577\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[agent]\nread_agents_md=maybe\n",
                sizeof("[agent]\nread_agents_md=maybe\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[agent]\nread_agents_md=true\nread_agents_md=false\n",
                sizeof("[agent]\nread_agents_md=true\nread_agents_md=false\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[agent]\nprovider=missing\n[provider present]\n",
                sizeof("[agent]\nprovider=missing\n[provider present]\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[provider]\nopenrouter_title=\n",
                sizeof("[provider]\nopenrouter_title=\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[provider duplicate]\n[provider duplicate]\n",
                sizeof("[provider duplicate]\n[provider duplicate]\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[provider paid]\n[model-limit paid/model]\n",
                sizeof("[provider paid]\n[model-limit paid/model]\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[model-limit missing/model]\nmax_input_tokens=1\n",
                sizeof("[model-limit missing/model]\nmax_input_tokens=1\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[model-limit /model]\nmax_input_tokens=1\n",
                sizeof("[model-limit /model]\nmax_input_tokens=1\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[model-limit default/]\nmax_input_tokens=1\n",
                sizeof("[model-limit default/]\nmax_input_tokens=1\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[model-limit default/model]\nmax_input_tokens=0\n",
                sizeof("[model-limit default/model]\nmax_input_tokens=0\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[model-limit default/model]\nmax_input_tokens=4000000001\n",
                sizeof("[model-limit default/model]\nmax_input_tokens=4000000001\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[model-limit default/model]\nmax_output_tokens=18446744073709551616\n",
                sizeof("[model-limit default/model]\nmax_output_tokens=18446744073709551616\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[model-limit default/model]\nmax_input_tokens=1\nmax_input_tokens=2\n",
                sizeof("[model-limit default/model]\nmax_input_tokens=1\nmax_input_tokens=2\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[model-limit default/model]\nmax_input_tokens=1\n"
                "[model-limit default/model]\nmax_output_tokens=1\n",
                sizeof("[model-limit default/model]\nmax_input_tokens=1\n"
                       "[model-limit default/model]\nmax_output_tokens=1\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[model-limit default/model]\ncontext_window_tokens=100\nmax_input_tokens=101\n",
                sizeof("[model-limit default/model]\ncontext_window_tokens=100\nmax_input_tokens=101\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[model-limit default/model]\ncontext_window_tokens=100\nmax_output_tokens=101\n",
                sizeof("[model-limit default/model]\ncontext_window_tokens=100\nmax_output_tokens=101\n") - 1u);
    expect_invalid(path);
    write_bytes(path,
                "[model-limit default/model]\ncontext_window_tokens=100\nmax_input_tokens=70\nmax_output_tokens=31\n",
                sizeof("[model-limit default/model]\ncontext_window_tokens=100\nmax_input_tokens=70\nmax_output_tokens=31\n") - 1u);
    expect_invalid(path);
    {
        static const unsigned char bad_header[] = {
            '[', 'p', 'r', 'o', 'v', 'i', 'd', 'e', 'r', ']', '\n',
            'o', 'p', 'e', 'n', 'r', 'o', 'u', 't', 'e', 'r', '_',
            't', 'i', 't', 'l', 'e', '=', 0x7f, '\n'
        };
        write_bytes(path, bad_header, sizeof(bad_header));
        expect_invalid(path);
    }
    {
        static const unsigned char with_nul[] = {
            '[', 'u', 'i', ']', '\n', 'v', 'e', 'r', 'b', 'o', 's', 'i', 't', 'y',
            '=', '1', '\0', '\n'
        };
        write_bytes(path, with_nul, sizeof(with_nul));
        expect_invalid(path);
    }
    {
        char *large = malloc(SNAG_CONFIG_FILE_MAX + 1u);
        assert(large);
        memset(large, '#', SNAG_CONFIG_FILE_MAX + 1u);
        write_bytes(path, large, SNAG_CONFIG_FILE_MAX + 1u);
        free(large);
        expect_invalid(path);
    }
    snag_config_init(&config);
    assert(snag_config_load(&config, "relative.ini", dotdir,
                           error, sizeof(error)) < 0);
    snag_config_free(&config);

    {
        static const char preserved[] =
            "# keep this comment\n"
            "[agent]\n"
            "model = old\r\n"
            "max_goal_prompt_bytes = 123456\n"
            "reasoning_effort=low\n"
            "\n"
            "[provider first]\n"
            "base_url = https://first.example.test\n"
            "[provider second]\n"
            "base_url = https://second.example.test\n";
        static const char expected[] =
            "# keep this comment\n"
            "[agent]\n"
            "model = new-model\r\n"
            "max_goal_prompt_bytes = 123456\n"
            "reasoning_effort = ultra\n"
            "\n"
            "provider = second\n"
            "[provider first]\n"
            "base_url = https://first.example.test\n"
            "[provider second]\n"
            "base_url = https://second.example.test\n";
        char created[4096];
        char bytes[4096];
        struct stat st;
        ssize_t got;
        int fd;

        assert(snprintf(path, sizeof(path), "%s/save.ini", temp) > 0);
        write_bytes(path, preserved, sizeof(preserved) - 1u);
        assert(chmod(path, 0640) == 0);
        assert(snag_config_save_model(path, false, "second", "new-model",
                                     "ultra", error, sizeof(error)) == 0);
        assert(stat(path, &st) == 0);
        assert((st.st_mode & 0777u) == 0640u);
        fd = open(path, O_RDONLY);
        assert(fd >= 0);
        got = read(fd, bytes, sizeof(bytes) - 1u);
        assert(got == (ssize_t)(sizeof(expected) - 1u));
        bytes[got] = '\0';
        assert(close(fd) == 0);
        assert(memcmp(bytes, expected, sizeof(expected)) == 0);
        assert(strstr(bytes, "# keep this comment\n") != NULL);
        assert(strstr(bytes, "max_goal_prompt_bytes = 123456\n") != NULL);
        assert(strstr(bytes, "base_url = https://first.example.test\n") != NULL);
        assert(strstr(bytes, "provider = second\n") != NULL);
        assert(strstr(bytes, "model = new-model\r\n") != NULL);
        assert(strstr(bytes, "reasoning_effort = ultra\n") != NULL);
        snag_config_init(&config);
        assert(snag_config_load(&config, path, dotdir,
                               error, sizeof(error)) == 0);
        assert(strcmp(config.provider, "second") == 0);
        assert(strcmp(config.model, "new-model") == 0);
        assert(strcmp(config.reasoning_effort, "ultra") == 0);
        snag_config_free(&config);

        assert(snprintf(created, sizeof(created), "%s/new.ini", dotdir) > 0);
        assert(snag_config_save_model(created, true, "default", "created-model",
                                     "medium", error, sizeof(error)) == 0);
        assert(stat(created, &st) == 0);
        assert((st.st_mode & 0777u) == 0600u);
        snag_config_init(&config);
        assert(snag_config_load(&config, created, dotdir,
                               error, sizeof(error)) == 0);
        assert(strcmp(config.provider, "default") == 0);
        assert(strcmp(config.model, "created-model") == 0);
        snag_config_free(&config);

        assert(snprintf(created, sizeof(created), "%s/absent.ini", dotdir) > 0);
        assert(snag_config_save_model(created, false, "default", "nope",
                                     "medium", error, sizeof(error)) < 0);
        assert(access(created, F_OK) < 0 && errno == ENOENT);

        fd = open(path, O_RDONLY);
        assert(fd >= 0);
        got = read(fd, bytes, sizeof(bytes));
        assert(got > 0);
        assert(close(fd) == 0);
        assert(snag_config_save_model(path, false, "missing", "nope",
                                     "medium", error, sizeof(error)) < 0);
        {
            char after[4096];
            ssize_t after_got;
            fd = open(path, O_RDONLY);
            assert(fd >= 0);
            after_got = read(fd, after, sizeof(after));
            assert(after_got == got);
            assert(memcmp(bytes, after, (size_t)got) == 0);
            assert(close(fd) == 0);
        }
    }

    puts("test_config: ok");
    return 0;
}
