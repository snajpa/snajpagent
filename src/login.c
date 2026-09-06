/* SPDX-License-Identifier: GPL-2.0-only */
#include "login.h"
#include "fs.h"
#include "app.h"
#include "auth.h"
#include "base.h"
#include "config.h"
#include "json.h"
#include "provider.h"
#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t cancelled;

static void
cancel_login(int signo)
{
    (void)signo;
    cancelled = 1;
}

static int
login_pump(void *opaque, uint32_t wait_ms)
{
    (void)opaque;
    if (!cancelled && wait_ms)
        (void)poll(NULL, 0, (int)wait_ms);
    return cancelled ? 2 : 0;
}

static int
read_line(const char *prompt, char *out, size_t size, bool secret,
           bool from_stdin, char *error, size_t error_size)
{
    struct termios before, hidden;
    bool changed = false;
    size_t used = 0u;
    int rc = -1;

    memset(out, 0, size);
    if (!from_stdin && (!isatty(STDIN_FILENO) || !isatty(STDERR_FILENO))) {
        snag_errorf(error, error_size, "login needs a terminal; use a named provider and --with-api-key for stdin credentials");
        return -1;
    }
    if (secret && isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &before) < 0)
            goto out;
        hidden = before;
        hidden.c_lflag &= (tcflag_t)~ECHO;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) < 0)
            goto out;
        changed = true;
    }
    if (!from_stdin)
        (void)fprintf(stderr, "%s", prompt);
    for (;;) {
        char c;
        ssize_t n;
        if (cancelled)
            goto out;
        n = read(STDIN_FILENO, &c, 1u);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 || (!n && !used))
            goto out;
        if (!n || c == '\n') {
            if (used && out[used - 1u] == '\r')
                --used;
            out[used] = '\0';
            rc = 0;
            break;
        }
        if (!c || used + 1u >= size) {
            snag_errorf(error, error_size, "login input is invalid or too long");
            goto out;
        }
        out[used++] = c;
    }
out:
    if (changed) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &before);
        (void)fprintf(stderr, "\n");
    }
    if (rc < 0) {
        volatile char *p = out;
        for (size_t i = 0; i < size; ++i)
            p[i] = 0;
        if (!error[0])
            snag_errorf(error, error_size, "login cancelled or input closed");
    }
    return rc;
}

static bool
plain_value(const char *value)
{
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 0x20u || *p == 0x7fu)
            return false;
    return value[0] && snag_utf8_valid((const unsigned char *)value, strlen(value), true);
}

static int
choose_provider(const struct snag_cli *cli, struct snag_config *config,
                 struct snag_provider_config *provider, bool *existing,
                 char *error, size_t error_size)
{
    char name[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    const struct snag_provider_config *found;
    const char *selection = cli->auth_provider;

    if (!selection) {
        (void)fprintf(stderr,
            "Choose a provider:\n  codex      ChatGPT device login\n"
            "  openrouter OpenRouter API key\n  openai     OpenAI API key\n"
            "  custom     Responses-compatible endpoint\n");
        for (size_t i = 0; i < config->provider_count; ++i)
            (void)fprintf(stderr, "  %s (configured)\n", config->providers[i].name);
        if (read_line("Provider: ", name, sizeof(name), false, false,
                       error, error_size) < 0 || !*name)
            return -1;
        selection = name;
    }
    found = snag_config_provider(config, selection);
    *existing = found != NULL;
    if (found) {
        *provider = *found;
        memset(&provider->api_key, 0, sizeof(provider->api_key));
        if (found->api_key.kind != SNAG_SECRET_NONE &&
            snag_secret_source_parse(&provider->api_key, found->api_key.expression,
                                      config->source_path, error, error_size) < 0)
            return -1;
    } else {
        snag_config_provider_init(provider, selection);
        if (!snag_strcpy(provider->name, sizeof(provider->name), selection))
            return -1;
        provider->auth = SNAG_AUTH_API_KEY;
        provider->native_compaction = false;
        if (strcmp(selection, "codex") == 0) {
            provider->auth = SNAG_AUTH_CHATGPT;
            provider->native_compaction = true;
            (void)snag_strcpy(provider->base_url, sizeof(provider->base_url), SNAG_CHATGPT_BASE);
        } else if (strcmp(selection, "openrouter") == 0) {
            (void)snag_strcpy(provider->base_url, sizeof(provider->base_url), "https://openrouter.ai/api/v1");
        } else if (strcmp(selection, "openai") != 0) {
            if (strcmp(selection, "custom") == 0 &&
                (read_line("Provider name: ", provider->name, sizeof(provider->name), false,
                            false, error, error_size) < 0 || !*provider->name))
                return -1;
            if (read_line("Responses API base URL: ", provider->base_url,
                           sizeof(provider->base_url), false, false, error, error_size) < 0)
                return -1;
        }
    }
    if (cli->device_auth) {
        if (strcmp(provider->base_url, SNAG_CHATGPT_BASE) != 0) {
            snag_errorf(error, error_size, "--device-auth requires the direct Codex provider");
            return -1;
        }
        provider->auth = SNAG_AUTH_CHATGPT;
        snag_secret_source_free(&provider->api_key);
    }
    if (!*existing && !cli->auth_provider && strcmp(selection, "custom") != 0) {
        char local_name[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
        char prompt[128];
        (void)snprintf(prompt, sizeof(prompt), "Local provider name [%s]: ", provider->name);
        if (read_line(prompt, local_name, sizeof(local_name), false, false, error, error_size) < 0)
            return -1;
        if (local_name[0])
            (void)snag_strcpy(provider->name, sizeof(provider->name), local_name);
        if (snag_config_provider(config, provider->name)) {
            snag_errorf(error, error_size, "provider name is already configured");
            return -1;
        }
    }
    if (cli->with_api_key) {
        provider->auth = SNAG_AUTH_API_KEY;
        snag_secret_source_free(&provider->api_key);
    }
    for (const unsigned char *p = (const unsigned char *)provider->name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) {
            snag_errorf(error, error_size, "invalid provider name");
            return -1;
        }
    if (!plain_value(provider->name) || !plain_value(provider->base_url) ||
        (strncmp(provider->base_url, "https://", 8u) && strncmp(provider->base_url, "http://", 7u)) ||
        strchr(provider->base_url, '@') || strchr(provider->base_url, '?') || strchr(provider->base_url, '#')) {
        snag_errorf(error, error_size, "invalid provider endpoint");
        return -1;
    }
    size_t len = strlen(provider->base_url);
    while (len && provider->base_url[len - 1u] == '/')
        provider->base_url[--len] = '\0';
    return snag_config_validate_provider(provider, error, error_size);
}

static int
acquire_login(const struct snag_cli *cli, struct snag_provider_config *provider,
               const char *config_path,
               int root_fd, struct snag_auth_tokens *tokens,
               char *error, size_t error_size)
{
    char key[SNAG_CREDENTIAL_MAX + 1u];
    int rc;
    if (provider->api_key.kind == SNAG_SECRET_NONE && root_fd >= 0 &&
        !cli->device_auth && !cli->with_api_key) {
        rc = snag_auth_load(root_fd, provider, tokens, error, error_size);
        if (rc < 0)
            return -1;
        if (rc == 0 && (provider->auth == SNAG_AUTH_API_KEY ||
                       tokens->expires_at_ms > snag_time_ms() + 60000u)) {
            if (!isatty(STDIN_FILENO) || !isatty(STDERR_FILENO))
                return 0;
            if (read_line("Use the existing stored login? [Y/n]: ", key, sizeof(key),
                           false, false, error, error_size) < 0)
                return -1;
            if (!*key || strcmp(key, "Y") == 0 || strcmp(key, "y") == 0)
                return 0;
        }
        snag_auth_clear(tokens);
        error[0] = '\0';
    }
    if (provider->auth == SNAG_AUTH_CHATGPT)
        return snag_auth_device(tokens, login_pump, NULL, error, error_size);
    if (provider->api_key.kind != SNAG_SECRET_NONE)
        return snag_credential_resolve(&tokens->credential, &provider->api_key,
                                       error, error_size);
    rc = read_line("API key (hidden; blank selects a secret source): ",
                    key, sizeof(key), true, cli->with_api_key, error, error_size);
    if (rc == 0 && !key[0] && !cli->with_api_key) {
        if (read_line("Secret source (${ENV}, quoted literal, or path; hidden): ", key,
                       sizeof(key), true, false, error, error_size) < 0 ||
            snag_secret_source_parse(&provider->api_key, key, config_path, error, error_size) < 0) {
            rc = -1;
        } else {
            rc = snag_credential_resolve(&tokens->credential, &provider->api_key,
                                         error, error_size);
        }
    } else if (rc == 0) {
        rc = snag_auth_key(tokens, key, error, error_size);
    }
    volatile char *p = key;
    for (size_t i = 0; i < sizeof(key); ++i)
        p[i] = 0;
    return rc;
}

static int
choose_model(const struct snag_cli *cli, const struct snag_config *config,
              const struct snag_provider_config *provider,
              struct snag_auth_tokens *tokens, char model[SNAG_CONFIG_MODEL_MAX],
              char *error, size_t error_size)
{
    json_t *models = NULL;
    char answer[SNAG_CONFIG_MODEL_MAX];
    int rc = -1;
    if (cli->model) {
        if (!plain_value(cli->model) || !snag_strcpy(model, SNAG_CONFIG_MODEL_MAX, cli->model))
            return -1;
        return 0;
    }
    if (read_line("Fetch this provider's model list now? [Y/n]: ", answer,
                   sizeof(answer), false, false, error, error_size) < 0)
        goto out;
    if (!*answer || strcmp(answer, "y") == 0 || strcmp(answer, "Y") == 0) {
        tokens->credential.root_fd = -1; /* Uncommitted credentials. */
        if (snag_provider_models_list(config, provider, &tokens->credential,
                                      NULL, login_pump, NULL, &models,
                                      error, error_size) < 0) {
            if (cancelled)
                goto out;
            (void)fprintf(stderr, "Model discovery failed: %s\nYou can enter a model ID manually.\n", error);
            error[0] = '\0';
        }
        for (size_t i = 0; i < json_array_size(models); ++i) {
            if (i == 100u) {
                (void)fprintf(stderr, "More models available; enter an exact ID for any model.\n");
                break;
            }
            const char *id = snag_json_string(json_array_get(models, i), "id");
            if (plain_value(id))
                (void)fprintf(stderr, "%zu. %s\n", i + 1u, id);
        }
    }
    if (read_line("Model number or exact model ID: ", answer, sizeof(answer),
                   false, false, error, error_size) < 0 || !plain_value(answer))
        goto out;
    {
        char *end;
        unsigned long n = strtoul(answer, &end, 10);
        const char *id = answer;
        if (!*end && n && n <= json_array_size(models))
            id = snag_json_string(json_array_get(models, n - 1u), "id");
        if (!snag_strcpy(model, SNAG_CONFIG_MODEL_MAX, id))
            goto out;
    }
    rc = 0;
out:
    json_decref(models);
    return rc;
}

static int
login_status(const struct snag_config *config, int root_fd,
              const char *selected, char *error, size_t error_size)
{
    bool found = false;
    for (size_t i = 0; i < config->provider_count; ++i) {
        const struct snag_provider_config *provider = &config->providers[i];
        struct snag_auth_tokens tokens;
        int rc;
        if (selected && strcmp(selected, provider->name))
            continue;
        found = true;
        snag_auth_clear(&tokens);
        if (provider->api_key.kind != SNAG_SECRET_NONE) {
            rc = snag_credential_resolve(&tokens.credential, &provider->api_key,
                                         error, error_size);
            (void)printf("%s: %s (%s)\n", provider->name, snag_secret_source_kind(&provider->api_key),
                          rc == 0 ? "available" : "missing or invalid");
        } else {
            rc = root_fd < 0 ? 1 : snag_auth_load(root_fd, provider, &tokens, error, error_size);
            if (rc < 0) {
                snag_auth_clear(&tokens);
                return -1;
            }
            (void)printf("%s: %s (%s)\n", provider->name, snag_auth_kind_name(provider->auth),
                rc == 1 ? "not logged in" : provider->auth == SNAG_AUTH_CHATGPT &&
                tokens.expires_at_ms <= snag_time_ms() ? "expired; refresh on use" : "stored");
        }
        snag_auth_clear(&tokens);
    }
    if (!found)
        snag_errorf(error, error_size, "provider is not configured");
    return found ? 0 : -1;
}

int
snag_login_dispatch(const struct snag_cli *cli, bool *handled)
{
    struct snag_config config;
    struct snag_store store;
    struct snag_provider_config provider = {0};
    struct snag_auth_tokens tokens, previous;
    struct sigaction action, old_int, old_term;
    snag_file_info st;
    char error[256] = {0}, rollback_error[256] = {0};
    char model[SNAG_CONFIG_MODEL_MAX] = {0};
    char *dotdir = NULL, *path = NULL;
    bool first, existing = false, signals = false, credentials_written = false;
    bool setup = cli->auth_command == SNAG_CLI_AUTH_NONE;
    int root_fd = -1, rc = 2;

    *handled = false;
#ifdef SNAJPAGENT_TEST_FIXTURE
    /* Ordinary agent fixtures bypass onboarding; login PTYs opt into it. */
    if (setup && !getenv("SNAJPAGENT_TEST_LOGIN"))
        return 0;
#endif
    if (setup && (cli->execute || cli->resume || cli->list || cli->config_path || cli->provider ||
                   !isatty(STDIN_FILENO) || !isatty(STDERR_FILENO)))
        return 0;
    snag_config_init(&config);
    snag_store_init(&store);
    snag_auth_clear(&tokens);
    snag_auth_clear(&previous);
    dotdir = snag_app_dotdir(cli->dotdir, error, sizeof(error));
    if (!dotdir)
        goto out;
    path = snag_config_path(cli->config_path, dotdir, error, sizeof(error));
    if (!path)
        goto out;
    first = snag_lstat(path, &st) < 0 && errno == ENOENT;
    if (setup && (!first || (getenv("OPENAI_API_KEY") && *getenv("OPENAI_API_KEY")))) {
        rc = 0;
        goto out;
    }
    *handled = true;
    if (snag_config_load(&config, cli->config_path, dotdir, error, sizeof(error)) < 0)
        goto out;
    if (first) {
        for (size_t i = 0; i < config.provider_count; ++i)
            snag_secret_source_free(&config.providers[i].api_key);
        config.provider_count = 0u;
    }
    if (cli->auth_command == SNAG_CLI_LOGIN_STATUS || cli->auth_command == SNAG_CLI_LOGOUT) {
        root_fd = open(dotdir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (root_fd < 0 && errno != ENOENT)
            goto out;
        if (cli->auth_command == SNAG_CLI_LOGIN_STATUS) {
            rc = login_status(&config, root_fd, cli->auth_provider, error, sizeof(error)) < 0 ? 2 : 0;
        } else {
            const struct snag_provider_config *p = snag_config_provider(&config,
                cli->auth_provider ? cli->auth_provider : config.provider[0] ? config.provider : NULL);
            if (!p) {
                snag_errorf(error, sizeof(error), "provider is not configured");
                goto out;
            }
            if (p->api_key.kind != SNAG_SECRET_NONE) {
                (void)printf("%s uses config-owned credentials; no stored login removed\n", p->name);
                rc = 0;
            } else {
                rc = root_fd < 0 ? 0 : snag_auth_logout(root_fd, p, NULL, NULL, error, sizeof(error));
                if (rc == 0)
                    (void)printf("%s: stored login removed\n", p->name);
            }
        }
        goto out;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = cancel_login;
    sigemptyset(&action.sa_mask);
    cancelled = 0;
    if (sigaction(SIGINT, &action, &old_int) < 0)
        goto out;
    if (sigaction(SIGTERM, &action, &old_term) < 0) {
        (void)sigaction(SIGINT, &old_int, NULL);
        goto out;
    }
    signals = true;
    if (choose_provider(cli, &config, &provider, &existing, error, sizeof(error)) < 0)
        goto out;
    if (first && !cli->model && !isatty(STDIN_FILENO)) {
        snag_errorf(error, sizeof(error), "first noninteractive login needs -m MODEL before login");
        goto out;
    }
    root_fd = open(dotdir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd < 0 && errno != ENOENT)
        goto out;
    if (acquire_login(cli, &provider, path, root_fd, &tokens, error, sizeof(error)) < 0)
        goto out;
    if (first && choose_model(cli, &config, &provider, &tokens, model, error, sizeof(error)) < 0)
        goto out;
    if (cancelled)
        goto out;
    if (snag_store_open(&store, dotdir, error, sizeof(error)) < 0)
        goto out;
    if (provider.api_key.kind == SNAG_SECRET_NONE) {
        if (snag_auth_save(store.root_fd, &provider, &tokens, &previous,
                          login_pump, NULL, error, sizeof(error)) < 0)
            goto out;
        credentials_written = true;
    }
    if (snag_config_save_provider(path, cli->config_path == NULL, &provider,
                                  first ? model : NULL, cli->effort,
                                  error, sizeof(error)) < 0) {
        if (credentials_written && snag_auth_restore(store.root_fd, &provider, &tokens,
                &previous, rollback_error, sizeof(rollback_error)) < 0)
            (void)fprintf(stderr, "snajpagent: %s\n", rollback_error);
        goto out;
    }
    (void)fprintf(stderr, "%s: %s configured\n",
                   provider.name, snag_auth_kind_name(provider.auth));
    if (first)
        (void)fprintf(stderr, "Default model: %s / %s\n", provider.name, model);
    else if (!existing)
        (void)fprintf(stderr, "Provider added; current default model selection is unchanged\n");
    rc = 0;
    if (setup)
        *handled = false;
out:
    if (signals) {
        (void)sigaction(SIGINT, &old_int, NULL);
        (void)sigaction(SIGTERM, &old_term, NULL);
    }
    if (root_fd >= 0)
        (void)close(root_fd);
    snag_store_close(&store);
    snag_config_free(&config);
    snag_secret_source_free(&provider.api_key);
    snag_auth_clear(&tokens);
    snag_auth_clear(&previous);
    free(path);
    free(dotdir);
    if (rc != 0) {
        *handled = true;
        (void)fprintf(stderr, "snajpagent: %s\n", error[0] ? error : "login cancelled or setup failed");
    }
    return rc == 0 ? 0 : 2;
}
