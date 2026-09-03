/* SPDX-License-Identifier: GPL-2.0-only */
#include "config.h"
#include "base.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

enum section {
    SECTION_NONE,
    SECTION_AGENT,
    SECTION_PROVIDER,
    SECTION_UI,
    SECTION_TOOL,
    SECTION_COUNT
};

struct parse_state {
    struct snj_config *config;
    enum section section;
    unsigned int seen_sections;
    unsigned int seen_keys[SECTION_COUNT];
    unsigned int seen_provider_keys[SNJ_CONFIG_PROVIDER_MAX];
    size_t provider_index;
    bool providers_started;
};

static void
set_error(char *error, size_t size, const char *fmt, ...)
{
    va_list ap;
    if (!size)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(error, size, fmt, ap);
    va_end(ap);
}

static int
copy_value(char *dst, size_t size, const char *value)
{
    size_t len = strlen(value);
    if (!len || len >= size) {
        errno = EINVAL;
        return -1;
    }
    memcpy(dst, value, len + 1u);
    return 0;
}

static void
provider_init(struct snj_provider_config *provider, const char *name)
{
    memset(provider, 0, sizeof(*provider));
    (void)snprintf(provider->name, sizeof(provider->name), "%s", name);
    provider->connect_timeout_ms = 30000u;
    provider->idle_timeout_ms = 120000u;
    provider->request_timeout_ms = 1800000u;
    provider->auto_compact_input_tokens = 120000u;
    provider->exact_token_count = true;
    provider->native_compaction = true;
    memcpy(provider->base_url, "https://api.openai.com", 23u);
    memcpy(provider->api_key_env, "OPENAI_API_KEY", 15u);
}

void
snj_config_init(struct snj_config *config)
{
    memset(config, 0, sizeof(*config));
    memcpy(config->model, "default", 8u);
    memcpy(config->reasoning_effort, "default", 8u);
    provider_init(&config->providers[0], "default");
    config->provider_count = 1u;
    config->verbosity = 0u;
    config->color = SNJ_COLOR_AUTO;
    config->resume_history_turns = 2u;
    config->shell = snj_strdup_checked("/bin/sh", SNJ_CONFIG_PATH_MAX);
    config->default_yield_ms = 10000u;
    config->default_timeout_ms = 1800000u;
    config->max_timeout_ms = 86400000u;
}

void
snj_config_free(struct snj_config *config)
{
    free(config->shell);
    for (size_t i = 0; i < config->secret_env_count; ++i)
        free(config->secret_env[i]);
    memset(config, 0, sizeof(*config));
}

static char *
trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r')
        ++s;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        --end;
    *end = '\0';
    return s;
}

static int
parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *out)
{
    uint64_t value = 0u;
    if (!*text)
        goto invalid;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9')
            goto invalid;
        if (value > (UINT32_MAX - (uint32_t)(*p - '0')) / 10u)
            goto invalid;
        value = value * 10u + (uint32_t)(*p - '0');
    }
    if (value < min || value > max)
        goto invalid;
    *out = (uint32_t)value;
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

static int
parse_bool(const char *text, bool *out)
{
    if (strcmp(text, "true") == 0 || strcmp(text, "1") == 0) {
        *out = true;
        return 0;
    }
    if (strcmp(text, "false") == 0 || strcmp(text, "0") == 0) {
        *out = false;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

static bool
env_name_valid(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_'))
        return false;
    for (++p; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_'))
            return false;
    return true;
}

static int
copy_base_url(char *dst, size_t size, const char *value)
{
    const char *host;
    const char *path;
    size_t len = strlen(value);
    size_t scheme_len;

    if (strncmp(value, "https://", 8u) == 0)
        scheme_len = 8u;
    else if (strncmp(value, "http://", 7u) == 0)
        scheme_len = 7u;
    else
        goto invalid;
    while (len > scheme_len && value[len - 1u] == '/')
        --len;
    if (len >= size)
        goto invalid;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x21u || c > 0x7eu || c == '?' || c == '#')
            goto invalid;
    }
    host = value + scheme_len;
    if (host >= value + len || *host == '/')
        goto invalid;
    path = memchr(host, '/', (size_t)(value + len - host));
    if (path == host)
        goto invalid;
    memcpy(dst, value, len);
    dst[len] = '\0';
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

static int
parse_secret_env(struct snj_config *config, const char *value)
{
    char *copy;
    char *cursor;

    for (size_t i = 0; i < config->secret_env_count; ++i) {
        free(config->secret_env[i]);
        config->secret_env[i] = NULL;
    }
    config->secret_env_count = 0u;
    if (!*value)
        return 0;
    copy = snj_strdup_checked(value, SNJ_CONFIG_FILE_MAX);
    if (!copy)
        return -1;
    cursor = copy;
    for (;;) {
        char *comma = strchr(cursor, ',');
        char *name;
        if (comma)
            *comma = '\0';
        name = trim(cursor);
        if (!*name || strlen(name) > SNJ_CONFIG_ENV_NAME_MAX ||
            !env_name_valid(name) ||
            config->secret_env_count >= SNJ_CONFIG_SECRET_ENV_MAX) {
            free(copy);
            errno = EINVAL;
            return -1;
        }
        for (size_t i = 0; i < config->secret_env_count; ++i)
            if (strcmp(config->secret_env[i], name) == 0) {
                free(copy);
                errno = EINVAL;
                return -1;
            }
        config->secret_env[config->secret_env_count] =
            snj_strdup_checked(name, SNJ_CONFIG_ENV_NAME_MAX);
        if (!config->secret_env[config->secret_env_count]) {
            free(copy);
            return -1;
        }
        ++config->secret_env_count;
        if (!comma)
            break;
        cursor = comma + 1u;
    }
    free(copy);
    return 0;
}

static int
set_provider_section(struct parse_state *state, const char *name)
{
    struct snj_config *config = state->config;
    const unsigned char *p = (const unsigned char *)name;

    if (!*p || strlen(name) > SNJ_CONFIG_PROVIDER_NAME_MAX)
        goto invalid;
    for (; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') ||
              (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '_' || *p == '-'))
            goto invalid;
    if (!state->providers_started) {
        memset(config->providers, 0, sizeof(config->providers));
        config->provider_count = 0u;
        state->providers_started = true;
    }
    for (size_t i = 0; i < config->provider_count; ++i)
        if (strcmp(config->providers[i].name, name) == 0)
            goto invalid;
    if (config->provider_count >= SNJ_CONFIG_PROVIDER_MAX)
        goto invalid;
    state->provider_index = config->provider_count++;
    provider_init(&config->providers[state->provider_index], name);
    state->section = SECTION_PROVIDER;
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

static int
set_section(struct parse_state *state, char *name)
{
    enum section section;
    if (strcmp(name, "agent") == 0)
        section = SECTION_AGENT;
    else if (strcmp(name, "provider") == 0)
        return set_provider_section(state, "default");
    else if (strncmp(name, "provider ", 9u) == 0)
        return set_provider_section(state, trim(name + 9u));
    else if (strcmp(name, "ui") == 0)
        section = SECTION_UI;
    else if (strcmp(name, "tool") == 0)
        section = SECTION_TOOL;
    else {
        errno = EINVAL;
        return -1;
    }
    if (state->seen_sections & (1u << section)) {
        errno = EINVAL;
        return -1;
    }
    state->seen_sections |= 1u << section;
    state->section = section;
    return 0;
}

static int
claim_key(struct parse_state *state, unsigned int bit)
{
    unsigned int *seen = state->section == SECTION_PROVIDER ?
        &state->seen_provider_keys[state->provider_index] :
        &state->seen_keys[state->section];
    if (*seen & (1u << bit)) {
        errno = EINVAL;
        return -1;
    }
    *seen |= 1u << bit;
    return 0;
}

static int
parse_agent(struct parse_state *state, const char *key, const char *value)
{
    struct snj_config *config = state->config;
    if (strcmp(key, "model") == 0) {
        return claim_key(state, 0u) < 0 ? -1 :
               copy_value(config->model, sizeof(config->model), value);
    }
    if (strcmp(key, "reasoning_effort") == 0) {
        if (claim_key(state, 1u) < 0)
            goto invalid;
        return copy_value(config->reasoning_effort,
                          sizeof(config->reasoning_effort), value);
    }
invalid:
    errno = EINVAL;
    return -1;
}

static int
parse_provider(struct parse_state *state, const char *key, const char *value)
{
    struct snj_provider_config *provider =
        &state->config->providers[state->provider_index];
    if (strcmp(key, "connect_timeout_ms") == 0)
        return claim_key(state, 0u) < 0 ? -1 :
               parse_u32(value, 1000u, 120000u, &provider->connect_timeout_ms);
    if (strcmp(key, "idle_timeout_ms") == 0)
        return claim_key(state, 1u) < 0 ? -1 :
               parse_u32(value, 1000u, 600000u, &provider->idle_timeout_ms);
    if (strcmp(key, "request_timeout_ms") == 0)
        return claim_key(state, 2u) < 0 ? -1 :
               parse_u32(value, 1000u, 3600000u, &provider->request_timeout_ms);
    if (strcmp(key, "auto_compact_input_tokens") == 0)
        return claim_key(state, 3u) < 0 ? -1 :
               parse_u32(value, 0u, 4000000u,
                         &provider->auto_compact_input_tokens);
    if (strcmp(key, "base_url") == 0)
        return claim_key(state, 4u) < 0 ? -1 :
               copy_base_url(provider->base_url,
                             sizeof(provider->base_url), value);
    if (strcmp(key, "api_key_env") == 0) {
        if (claim_key(state, 5u) < 0 ||
            strlen(value) > SNJ_CONFIG_ENV_NAME_MAX ||
            !env_name_valid(value))
            goto invalid;
        return copy_value(provider->api_key_env,
                          sizeof(provider->api_key_env), value);
    }
    if (strcmp(key, "exact_token_count") == 0)
        return claim_key(state, 6u) < 0 ? -1 :
               parse_bool(value, &provider->exact_token_count);
    if (strcmp(key, "native_compaction") == 0)
        return claim_key(state, 7u) < 0 ? -1 :
               parse_bool(value, &provider->native_compaction);
invalid:
    errno = EINVAL;
    return -1;
}

static int
parse_ui(struct parse_state *state, const char *key, const char *value)
{
    struct snj_config *config = state->config;
    uint32_t parsed;
    if (strcmp(key, "verbosity") == 0) {
        if (claim_key(state, 0u) < 0 ||
            parse_u32(value, 0u, 6u, &parsed) < 0)
            return -1;
        config->verbosity = (unsigned int)parsed;
        return 0;
    }
    if (strcmp(key, "color") == 0) {
        if (claim_key(state, 1u) < 0)
            return -1;
        if (strcmp(value, "auto") == 0)
            config->color = SNJ_COLOR_AUTO;
        else if (strcmp(value, "never") == 0)
            config->color = SNJ_COLOR_NEVER;
        else {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "resume_history_turns") == 0) {
        if (claim_key(state, 2u) < 0 ||
            parse_u32(value, 0u, 100u, &parsed) < 0)
            return -1;
        config->resume_history_turns = (unsigned int)parsed;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

static int
parse_tool(struct parse_state *state, const char *key, const char *value)
{
    struct snj_config *config = state->config;
    char *copy;
    if (strcmp(key, "shell") == 0) {
        if (claim_key(state, 0u) < 0 || value[0] != '/')
            goto invalid;
        copy = snj_strdup_checked(value, SNJ_CONFIG_PATH_MAX);
        if (!copy)
            return -1;
        free(config->shell);
        config->shell = copy;
        return 0;
    }
    if (strcmp(key, "default_yield_ms") == 0)
        return claim_key(state, 1u) < 0 ? -1 :
               parse_u32(value, 0u, 600000u, &config->default_yield_ms);
    if (strcmp(key, "default_timeout_ms") == 0)
        return claim_key(state, 2u) < 0 ? -1 :
               parse_u32(value, 1000u, 86400000u,
                         &config->default_timeout_ms);
    if (strcmp(key, "max_timeout_ms") == 0)
        return claim_key(state, 3u) < 0 ? -1 :
               parse_u32(value, 1000u, 86400000u, &config->max_timeout_ms);
    if (strcmp(key, "secret_env") == 0)
        return claim_key(state, 4u) < 0 ? -1 :
               parse_secret_env(config, value);
invalid:
    errno = EINVAL;
    return -1;
}

static int
parse_assignment(struct parse_state *state, char *line)
{
    char *equal;
    char *key;
    char *value;
    if (state->section == SECTION_NONE || !(equal = strchr(line, '='))) {
        errno = EINVAL;
        return -1;
    }
    *equal = '\0';
    key = trim(line);
    value = trim(equal + 1u);
    if (!*key)
        goto invalid;
    switch (state->section) {
    case SECTION_AGENT: return parse_agent(state, key, value);
    case SECTION_PROVIDER: return parse_provider(state, key, value);
    case SECTION_UI: return parse_ui(state, key, value);
    case SECTION_TOOL: return parse_tool(state, key, value);
    case SECTION_NONE: case SECTION_COUNT: break;
    }
invalid:
    errno = EINVAL;
    return -1;
}

static int
parse_file(struct snj_config *config, char *text, char *error, size_t error_size)
{
    struct parse_state state;
    char *line = text;
    unsigned int number = 1u;

    memset(&state, 0, sizeof(state));
    state.config = config;
    for (;;) {
        char *next = strchr(line, '\n');
        char *clean;
        if (next)
            *next = '\0';
        clean = trim(line);
        if (*clean && *clean != '#' && *clean != ';') {
            size_t len = strlen(clean);
            int rc;
            if (clean[0] == '[') {
                if (len < 3u || clean[len - 1u] != ']')
                    rc = -1;
                else {
                    clean[len - 1u] = '\0';
                    rc = set_section(&state, clean + 1u);
                }
            } else {
                rc = parse_assignment(&state, clean);
            }
            if (rc < 0) {
                set_error(error, error_size,
                          "invalid configuration at line %u", number);
                return -1;
            }
        }
        if (!next)
            break;
        line = next + 1u;
        ++number;
    }
    return 0;
}

static char *
default_path(const char *dotdir, char *error, size_t error_size)
{
    struct snj_buf path;
    char *result = NULL;

    snj_buf_init(&path, SNJ_CONFIG_PATH_MAX);
    if (!dotdir || dotdir[0] != '/')
        goto invalid;
    if (snj_buf_printf(&path, "%s/config.ini", dotdir) < 0)
        goto unavailable;
    if (snj_buf_terminate(&path) < 0)
        goto unavailable;
    result = (char *)path.data;
    path.data = NULL;
    snj_buf_free(&path);
    return result;
invalid:
    set_error(error, error_size,
              "configuration requires an absolute dotdir");
    errno = EINVAL;
    snj_buf_free(&path);
    return NULL;
unavailable:
    set_error(error, error_size, "configuration path exceeds the supported limit");
    snj_buf_free(&path);
    return NULL;
}

static int
read_config(const char *path, bool explicit_path, struct snj_buf *text,
            char *error, size_t error_size)
{
    struct stat st;
    int fd;
    int rc = -1;

    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (!explicit_path && errno == ENOENT)
            return 1;
        set_error(error, error_size, "cannot open configuration %s: %s",
                  path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uintmax_t)st.st_size > SNJ_CONFIG_FILE_MAX) {
        set_error(error, error_size,
                  "configuration must be a regular file no larger than 64 KiB");
        errno = EINVAL;
        goto out;
    }
    for (;;) {
        unsigned char chunk[4096];
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            set_error(error, error_size, "cannot read configuration: %s",
                      strerror(errno));
            goto out;
        }
        if (got == 0)
            break;
        if (snj_buf_append(text, chunk, (size_t)got) < 0) {
            set_error(error, error_size, "configuration exceeds 64 KiB");
            goto out;
        }
    }
    if (!snj_utf8_valid(text->data, text->len, true)) {
        set_error(error, error_size,
                  "configuration must be valid UTF-8 without NUL bytes");
        errno = EILSEQ;
        goto out;
    }
    if (snj_buf_terminate(text) < 0) {
        set_error(error, error_size, "cannot terminate configuration buffer");
        goto out;
    }
    rc = 0;
out:
    {
        int saved = errno;
        (void)close(fd);
        errno = saved;
    }
    return rc;
}

static int
validate_shell(struct snj_config *config, char *error, size_t error_size)
{
    char *resolved;
    struct stat st;
    if (!config->shell || config->shell[0] != '/')
        goto invalid;
    resolved = realpath(config->shell, NULL);
    if (!resolved)
        goto invalid;
    if (strlen(resolved) > SNJ_CONFIG_PATH_MAX || stat(resolved, &st) < 0 ||
        !S_ISREG(st.st_mode) || access(resolved, X_OK) < 0) {
        free(resolved);
        goto invalid;
    }
    free(config->shell);
    config->shell = resolved;
    return 0;
invalid:
    set_error(error, error_size,
              "configured shell must resolve to an executable regular file");
    errno = EINVAL;
    return -1;
}

int
snj_config_load(struct snj_config *config, const char *explicit_path,
                const char *dotdir,
                char *error, size_t error_size)
{
    struct snj_buf text;
    char *owned_path = NULL;
    const char *path = explicit_path;
    int read_rc;
    int rc = -1;

    if (!config->shell) {
        set_error(error, error_size, "cannot initialize configuration defaults");
        errno = ENOMEM;
        return -1;
    }
    if (explicit_path && (explicit_path[0] != '/' ||
                          strlen(explicit_path) > SNJ_CONFIG_PATH_MAX)) {
        set_error(error, error_size,
                  "-c requires an absolute path within the supported limit");
        errno = EINVAL;
        return -1;
    }
    if (!path) {
        owned_path = default_path(dotdir, error, error_size);
        if (!owned_path)
            return -1;
        path = owned_path;
    }
    snj_buf_init(&text, SNJ_CONFIG_FILE_MAX + 1u);
    read_rc = read_config(path, explicit_path != NULL, &text, error, error_size);
    if (read_rc < 0)
        goto out;
    if (read_rc == 0 && parse_file(config, (char *)text.data,
                                   error, error_size) < 0)
        goto out;
    if (config->default_timeout_ms > config->max_timeout_ms) {
        set_error(error, error_size,
                  "tool default_timeout_ms cannot exceed max_timeout_ms");
        errno = EINVAL;
        goto out;
    }
    if (validate_shell(config, error, error_size) < 0)
        goto out;
    rc = 0;
out:
    free(owned_path);
    snj_buf_free(&text);
    return rc;
}

const struct snj_provider_config *
snj_config_provider(const struct snj_config *config, const char *name)
{
    if (!config || config->provider_count == 0u)
        return NULL;
    if (!name)
        return &config->providers[0];
    for (size_t i = 0; i < config->provider_count; ++i)
        if (strcmp(config->providers[i].name, name) == 0)
            return &config->providers[i];
    return NULL;
}
