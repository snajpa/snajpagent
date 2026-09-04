/* SPDX-License-Identifier: GPL-2.0-only */
#include "config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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
    struct snj_config config;
    char error[256];

    snj_config_init(&config);
    error[0] = '\0';
    assert(snj_config_load(&config, path, NULL, error, sizeof(error)) < 0);
    assert(error[0] != '\0');
    snj_config_free(&config);
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
        "\n[model-limit default/gpt-5.5]\n"
        "context_window_tokens = 1050000\n"
        "max_input_tokens = 922000\n"
        "max_output_tokens = 128000\n"
        "\n[model-limit backup/org/model/with/slashes]\n"
        "max_input_tokens = 4000000000\n"
        "\n[ui]\n"
        "verbosity = 4\n"
        "color = never\n"
        "markdown = false\n"
        "resume_history_turns = 0\n"
        "typing_pause_ms = 750\n"
        "\n[irc]\n"
        "daemon = true\n"
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
        "max_output_bytes = 123456\n"
        "secret_env = TOKEN_ONE, TOKEN_TWO\n";
    char temp[] = "/tmp/snajpagent-config-XXXXXX";
    char dotdir[4096];
    char path[4096];
    char link_path[4096];
    char error[256];
    struct snj_config config;
    char *shell;

    assert(mkdtemp(temp));
    assert(snprintf(dotdir, sizeof(dotdir), "%s/dotdir", temp) > 0);
    assert(mkdir(dotdir, 0700) == 0);

    snj_config_init(&config);
    assert(snj_config_load(&config, NULL, dotdir,
                           error, sizeof(error)) == 0);
    assert(strcmp(config.model, "default") == 0);
    assert(config.provider[0] == '\0');
    assert(strcmp(config.reasoning_effort, "default") == 0);
    assert(config.max_goal_prompt_bytes == 256u * 1024u);
    assert(config.read_agents_md);
    assert(config.verbosity == 0u);
    assert(config.markdown);
    assert(config.resume_history_turns == 2u);
    assert(config.typing_pause_ms == 500u);
    assert(!config.irc_daemon);
    assert(!config.irc_listen_explicit);
    assert(strcmp(config.irc_listen, "localhost:6667") == 0);
    assert(config.irc_client_count == 0u);
    assert(config.irc_history_lines == 200u);
    assert(config.default_timeout_ms == 0u);
    assert(config.max_timeout_ms == 86400000u);
    assert(config.max_output_bytes == 0u);
    assert(config.provider_count == 1u);
    assert(strcmp(config.providers[0].name, "default") == 0);
    assert(config.providers[0].auto_compact_input_tokens == 120000u);
    assert(config.providers[0].exact_token_count);
    assert(config.providers[0].native_compaction);
    assert(strcmp(config.providers[0].base_url, "https://api.openai.com") == 0);
    assert(strcmp(config.providers[0].api_key_env, "OPENAI_API_KEY") == 0);
    assert(config.providers[0].openrouter_referer[0] == '\0');
    assert(config.providers[0].openrouter_title[0] == '\0');
    assert(config.secret_env_count == 0u);
    shell = realpath("/bin/sh", NULL);
    assert(shell);
    assert(strcmp(config.shell, shell) == 0);
    free(shell);
    snj_config_free(&config);

    assert(snprintf(path, sizeof(path), "%s/valid.ini", temp) > 0);
    write_bytes(path, valid, sizeof(valid) - 1u);
    snj_config_init(&config);
    assert(snj_config_load(&config, path, dotdir,
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
    assert(!config.providers[0].exact_token_count);
    assert(!config.providers[0].native_compaction);
    assert(strcmp(config.providers[1].name, "backup") == 0);
    assert(strcmp(config.providers[1].base_url,
                  "https://backup.example.test/v1") == 0);
    assert(strcmp(config.providers[1].api_key_env, "BACKUP_API_KEY") == 0);
    assert(config.model_limit_count == 2u);
    {
        const struct snj_model_limit_config *limit =
            snj_config_model_limit(&config, "default", "gpt-5.5");
        assert(limit);
        assert(limit->context_window_known);
        assert(limit->context_window_tokens == UINT64_C(1050000));
        assert(limit->max_input_known);
        assert(limit->max_input_tokens == UINT64_C(922000));
        assert(limit->max_output_known);
        assert(limit->max_output_tokens == UINT64_C(128000));
    }
    {
        const struct snj_model_limit_config *limit =
            snj_config_model_limit(&config, "backup",
                                   "org/model/with/slashes");
        assert(limit);
        assert(!limit->context_window_known);
        assert(limit->max_input_known);
        assert(limit->max_input_tokens == SNJ_CONFIG_TOKEN_LIMIT_MAX);
        assert(!limit->max_output_known);
    }
    assert(snj_config_model_limit(&config, "default", "missing") == NULL);
    assert(snj_config_provider(&config, NULL) == &config.providers[0]);
    assert(snj_config_provider(&config, "backup") == &config.providers[1]);
    assert(snj_config_provider(&config, "missing") == NULL);
    assert(config.verbosity == 4u);
    assert(config.color == SNJ_COLOR_NEVER);
    assert(!config.markdown);
    assert(config.resume_history_turns == 0u);
    assert(config.typing_pause_ms == 750u);
    assert(config.irc_daemon);
    assert(config.irc_listen_explicit);
    assert(strcmp(config.irc_listen, "127.0.0.1:7667") == 0);
    assert(config.irc_client_count == 2u);
    assert(strcmp(config.irc_clients[0], "irc-a.example") == 0);
    assert(strcmp(config.irc_clients[1], "[2001:db8::20]:7667") == 0);
    assert(strcmp(config.irc_model_nick, "builder") == 0);
    assert(strcmp(config.irc_operator_nick, "alice") == 0);
    assert(strcmp(config.irc_room_name, "build-host") == 0);
    assert(config.irc_history_lines == 321u);
    assert(config.default_yield_ms == 0u);
    assert(config.default_timeout_ms == 4000u);
    assert(config.max_timeout_ms == 5000u);
    assert(config.max_output_bytes == 123456u);
    assert(config.secret_env_count == 2u);
    assert(strcmp(config.secret_env[0], "TOKEN_ONE") == 0);
    assert(strcmp(config.secret_env[1], "TOKEN_TWO") == 0);
    snj_config_free(&config);

    assert(snprintf(path, sizeof(path), "%s/config.ini", dotdir) > 0);
    write_bytes(path, valid, sizeof(valid) - 1u);
    snj_config_init(&config);
    assert(snj_config_load(&config, NULL, dotdir,
                           error, sizeof(error)) == 0);
    assert(config.provider_count == 2u);
    assert(strcmp(config.providers[0].name, "default") == 0);
    assert(strcmp(config.providers[1].name, "backup") == 0);
    snj_config_free(&config);

    assert(snprintf(path, sizeof(path), "%s/valid.ini", temp) > 0);

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
    snj_config_init(&config);
    assert(snj_config_load(&config, path, dotdir,
                           error, sizeof(error)) == 0);
    assert(config.max_timeout_ms == UINT32_MAX);
    snj_config_free(&config);
    write_bytes(path, "[tool]\nmax_timeout_ms=4294967296\n",
                sizeof("[tool]\nmax_timeout_ms=4294967296\n") - 1u);
    expect_invalid(path);
    write_bytes(path, "[tool]\nmax_output_bytes=4294967295\n",
                sizeof("[tool]\nmax_output_bytes=4294967295\n") - 1u);
    snj_config_init(&config);
    assert(snj_config_load(&config, path, dotdir,
                           error, sizeof(error)) == 0);
    assert(config.max_output_bytes == UINT32_MAX);
    snj_config_free(&config);
    write_bytes(path, "[tool]\nmax_output_bytes=4294967296\n",
                sizeof("[tool]\nmax_output_bytes=4294967296\n") - 1u);
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
        char *large = malloc(SNJ_CONFIG_FILE_MAX + 1u);
        assert(large);
        memset(large, '#', SNJ_CONFIG_FILE_MAX + 1u);
        write_bytes(path, large, SNJ_CONFIG_FILE_MAX + 1u);
        free(large);
        expect_invalid(path);
    }
    snj_config_init(&config);
    assert(snj_config_load(&config, "relative.ini", dotdir,
                           error, sizeof(error)) < 0);
    snj_config_free(&config);

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
        assert(snj_config_save_model(path, false, "second", "new-model",
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
        snj_config_init(&config);
        assert(snj_config_load(&config, path, dotdir,
                               error, sizeof(error)) == 0);
        assert(strcmp(config.provider, "second") == 0);
        assert(strcmp(config.model, "new-model") == 0);
        assert(strcmp(config.reasoning_effort, "ultra") == 0);
        snj_config_free(&config);

        assert(snprintf(created, sizeof(created), "%s/new.ini", dotdir) > 0);
        assert(snj_config_save_model(created, true, "default", "created-model",
                                     "medium", error, sizeof(error)) == 0);
        assert(stat(created, &st) == 0);
        assert((st.st_mode & 0777u) == 0600u);
        snj_config_init(&config);
        assert(snj_config_load(&config, created, dotdir,
                               error, sizeof(error)) == 0);
        assert(strcmp(config.provider, "default") == 0);
        assert(strcmp(config.model, "created-model") == 0);
        snj_config_free(&config);

        assert(snprintf(created, sizeof(created), "%s/absent.ini", dotdir) > 0);
        assert(snj_config_save_model(created, false, "default", "nope",
                                     "medium", error, sizeof(error)) < 0);
        assert(access(created, F_OK) < 0 && errno == ENOENT);

        fd = open(path, O_RDONLY);
        assert(fd >= 0);
        got = read(fd, bytes, sizeof(bytes));
        assert(got > 0);
        assert(close(fd) == 0);
        assert(snj_config_save_model(path, false, "missing", "nope",
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
