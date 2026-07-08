/* SPDX-License-Identifier: GPL-2.0-only */
#include "config.h"

#include <assert.h>
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
    assert(snj_config_load(&config, path, error, sizeof(error)) < 0);
    assert(error[0] != '\0');
    snj_config_free(&config);
}

int
main(void)
{
    static const char valid[] =
        "[agent]\n"
        "model = gpt-5.5\n"
        "reasoning_effort = high\n"
        "\n[provider]\n"
        "connect_timeout_ms = 1000\n"
        "idle_timeout_ms = 2000\n"
        "request_timeout_ms = 3000\n"
        "auto_compact_input_tokens = 12345\n"
        "base_url = http://127.0.0.1:2455/backend-api/codex/\n"
        "api_key_env = CODEX_LB_API_KEY\n"
        "exact_token_count = false\n"
        "native_compaction = 0\n"
        "\n[ui]\n"
        "verbosity = 4\n"
        "color = never\n"
        "resume_history_turns = 0\n"
        "\n[tool]\n"
        "shell = /bin/sh\n"
        "default_yield_ms = 0\n"
        "default_timeout_ms = 4000\n"
        "max_timeout_ms = 5000\n"
        "secret_env = TOKEN_ONE, TOKEN_TWO\n";
    char temp[] = "/tmp/snajpagent-config-XXXXXX";
    char config_root[4096];
    char path[4096];
    char link_path[4096];
    char error[256];
    struct snj_config config;
    char *shell;

    assert(mkdtemp(temp));
    assert(snprintf(config_root, sizeof(config_root), "%s/config", temp) > 0);
    assert(mkdir(config_root, 0700) == 0);
    assert(setenv("XDG_CONFIG_HOME", config_root, 1) == 0);

    snj_config_init(&config);
    assert(snj_config_load(&config, NULL, error, sizeof(error)) == 0);
    assert(strcmp(config.model, "default") == 0);
    assert(strcmp(config.reasoning_effort, "default") == 0);
    assert(config.verbosity == 0u);
    assert(config.resume_history_turns == 2u);
    assert(config.auto_compact_input_tokens == 120000u);
    assert(config.provider_exact_token_count);
    assert(config.provider_native_compaction);
    assert(strcmp(config.provider_base_url, "https://api.openai.com") == 0);
    assert(strcmp(config.provider_api_key_env, "OPENAI_API_KEY") == 0);
    assert(config.secret_env_count == 0u);
    shell = realpath("/bin/sh", NULL);
    assert(shell);
    assert(strcmp(config.shell, shell) == 0);
    free(shell);
    snj_config_free(&config);

    assert(snprintf(path, sizeof(path), "%s/valid.ini", temp) > 0);
    write_bytes(path, valid, sizeof(valid) - 1u);
    snj_config_init(&config);
    assert(snj_config_load(&config, path, error, sizeof(error)) == 0);
    assert(strcmp(config.model, "gpt-5.5") == 0);
    assert(strcmp(config.reasoning_effort, "high") == 0);
    assert(config.connect_timeout_ms == 1000u);
    assert(config.idle_timeout_ms == 2000u);
    assert(config.request_timeout_ms == 3000u);
    assert(config.auto_compact_input_tokens == 12345u);
    assert(strcmp(config.provider_base_url,
                  "http://127.0.0.1:2455/backend-api/codex") == 0);
    assert(strcmp(config.provider_api_key_env, "CODEX_LB_API_KEY") == 0);
    assert(!config.provider_exact_token_count);
    assert(!config.provider_native_compaction);
    assert(config.verbosity == 4u);
    assert(config.color == SNJ_COLOR_NEVER);
    assert(config.resume_history_turns == 0u);
    assert(config.default_yield_ms == 0u);
    assert(config.default_timeout_ms == 4000u);
    assert(config.max_timeout_ms == 5000u);
    assert(config.secret_env_count == 2u);
    assert(strcmp(config.secret_env[0], "TOKEN_ONE") == 0);
    assert(strcmp(config.secret_env[1], "TOKEN_TWO") == 0);
    snj_config_free(&config);

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
    write_bytes(path,
        "[tool]\ndefault_timeout_ms=5000\nmax_timeout_ms=4000\n",
        sizeof("[tool]\ndefault_timeout_ms=5000\nmax_timeout_ms=4000\n") - 1u);
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
    assert(snj_config_load(&config, "relative.ini", error, sizeof(error)) < 0);
    snj_config_free(&config);

    puts("test_config: ok");
    return 0;
}
