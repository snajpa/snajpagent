/* SPDX-License-Identifier: GPL-2.0-only */
#include "config.h"
#include "fs.h"
#include "base.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum section {
    SECTION_NONE,
    SECTION_AGENT,
    SECTION_PROVIDER,
    SECTION_MODEL_LIMIT,
    SECTION_MODEL_ALIAS,
    SECTION_UI,
    SECTION_IRC,
    SECTION_TOOL,
    SECTION_COUNT
};

struct parse_state {
    struct snag_config *config;
    enum section section;
    unsigned int seen_sections;
    const char *seen_keys[SECTION_COUNT][16];
    const char *seen_provider_keys[SNAG_CONFIG_PROVIDER_MAX][16];
    const char *seen_model_limit_keys[SNAG_CONFIG_MODEL_LIMIT_MAX][16];
    const char *seen_alias_keys[SNAG_CONFIG_MODEL_ALIAS_MAX][16];
    size_t provider_index;
    size_t model_limit_index;
    size_t model_alias_index;
    struct {
        char provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
        struct snag_provider_model model;
    } models[SNAG_CONFIG_MODEL_ALIAS_MAX];
    size_t model_count;
    bool providers_started;
};

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

static int
copy_header_value(char *dst, size_t size, const char *value)
{
    size_t len = strlen(value);
    if (!len || len >= size)
        goto invalid;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20u || c > 0x7eu)
            goto invalid;
    }
    memcpy(dst, value, len + 1u);
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

void
snag_config_provider_init(struct snag_provider_config *provider, const char *name)
{
    memset(provider, 0, sizeof(*provider));
    (void)snprintf(provider->name, sizeof(provider->name), "%s", name);
    provider->connect_timeout_ms = 30000u;
    provider->idle_timeout_ms = 120000u;
    provider->request_timeout_ms = 1800000u;
    provider->auto_compact_input_tokens = SNAG_CONFIG_COMPACT_AUTO;
    provider->exact_token_count = SNAG_TOKEN_COUNT_AUTO;
    provider->native_compaction = true;
    provider->parallel_tool_calls = true;
    memcpy(provider->base_url, "https://api.openai.com", 23u);
}

void
snag_config_init(struct snag_config *config)
{
    static const char prompt[] =
        "{chat:{goal_spinner}{activity_spinner} {hour:02}:{minute:02}:{second:02} "
        "{operator}@{host} :}"
        "{rollout-idle:{goal_spinner}{activity_spinner}{context:4} "
        "{provider}/{model}/{effort} ›}"
        "{rollout-active:{goal_spinner}{activity_spinner}{context:4} "
        "{provider}/{model}/{effort} »}";

    memset(config, 0, sizeof(*config));
    memcpy(config->model, "default", 8u);
    memcpy(config->reasoning_effort, "default", 8u);
    snag_config_provider_init(&config->providers[0], "openai");
    if (snag_secret_source_parse(&config->providers[0].api_key,
                                 "${OPENAI_API_KEY}", NULL, NULL, 0u) < 0)
        return;
    config->provider_count = 1u;
    config->max_goal_prompt_bytes = 256u * 1024u;
    config->read_agents_md = true;
    config->color = SNAG_COLOR_AUTO;
    config->markdown = true;
    config->resume_history_turns = 2u;
    config->typing_pause_ms = 500u;
    memcpy(config->prompt, prompt, sizeof(prompt));
    memcpy(config->prompt_spinner_goal, " ⚑", sizeof(" ⚑"));
    memcpy(config->prompt_spinner_provider, " ◴◷◶◵", sizeof(" ◴◷◶◵"));
    memcpy(config->prompt_spinner_tool, " ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏",
           sizeof(" ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"));
    config->prompt_spinner_per_second = 8u;
    memcpy(config->irc.listen, "localhost:6667", 15u);
    config->irc.history_lines = 200u;
    config->shell = snag_strdup_checked("/bin/sh", SNAG_CONFIG_PATH_MAX);
    config->default_yield_ms = 10000u;
    config->max_parallel_commands = 4u;
    config->default_timeout_ms = 0u;
    config->max_timeout_ms = 86400000u;
    config->max_output_tokens = SNAG_DEFAULT_TOOL_OUTPUT_TOKENS;
    config->max_output_bytes = 0u;
}

void
snag_config_free(struct snag_config *config)
{
    free(config->shell);
    for (size_t i = 0; i < config->provider_count; ++i) {
        snag_secret_source_free(&config->providers[i].api_key);
        free(config->providers[i].models);
    }
    for (size_t i = 0; i < config->secret_count; ++i)
        snag_secret_source_free(&config->secrets[i]);
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
parse_u64(const char *text, uint64_t min, uint64_t max, uint64_t *out)
{
    uint64_t value = 0u;

    if (!*text)
        goto invalid;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        uint64_t digit;

        if (*p < '0' || *p > '9')
            goto invalid;
        digit = (uint64_t)(*p - '0');
        if (value > (UINT64_MAX - digit) / 10u)
            goto invalid;
        value = value * 10u + digit;
    }
    if (value < min || value > max)
        goto invalid;
    *out = value;
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

static int
parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *out)
{
    uint64_t value;
    if (parse_u64(text, min, max, &value) < 0)
        return -1;
    *out = (uint32_t)value;
    return 0;
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

static int
parse_token_count(const char *text, enum snag_token_count_mode *out)
{
    bool enabled;

    if (strcmp(text, "auto") == 0)
        *out = SNAG_TOKEN_COUNT_AUTO;
    else if (parse_bool(text, &enabled) < 0)
        return -1;
    else
        *out = enabled ? SNAG_TOKEN_COUNT_STRICT : SNAG_TOKEN_COUNT_OFF;
    return 0;
}

static bool
unsafe_prompt_cp(uint32_t cp)
{
    return cp == 0x00ad || cp == 0x061c || cp == 0x200b ||
           cp == 0x200e || cp == 0x200f ||
           (cp >= 0x202a && cp <= 0x202e) || cp == 0x2060 ||
           (cp >= 0x2066 && cp <= 0x206f) || cp == 0xfeff ||
           (cp >= 0xfff9 && cp <= 0xfffb);
}

static int
parse_spinner(char dst[SNAG_CONFIG_SPINNER_MAX], const char *value)
{
    size_t len = strlen(value), pos, frames = 0u, previous = SIZE_MAX;

    if (len < 3u || value[0] != '"' || value[len - 1u] != '"' ||
        len - 2u >= SNAG_CONFIG_SPINNER_MAX ||
        memchr(value + 1u, '"', len - 2u))
        goto invalid;
    --len;
    pos = value[1] == '\\' && value[2] == '0' ? 3u : 1u;
    if (pos == 1u) {
        uint32_t cp;
        size_t n = snag_utf8_decode((const unsigned char *)value + 1u, len - 1u, &cp);

        if (!n || snag_char_width(cp) != 1 || unsafe_prompt_cp(cp))
            goto invalid;
        pos += n;
    }
    while (pos < len) {
        uint32_t cp;
        size_t n = snag_utf8_decode((const unsigned char *)value + pos, len - pos, &cp);

        if (!n || ++frames > SNAG_CONFIG_SPINNER_FRAMES_MAX ||
            snag_char_width(cp) != 1 ||
            unsafe_prompt_cp(cp) || (previous != SIZE_MAX &&
            n == pos - previous &&
            memcmp(value + previous, value + pos, n) == 0))
            goto invalid;
        previous = pos;
        pos += n;
    }
    memcpy(dst, value + 1u, len - 1u);
    dst[len - 1u] = '\0';
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

static int
prompt_body(const char *text, size_t len,
            const char *const values[SNAG_PROMPT_FIELD_COUNT],
            unsigned char marker, struct snag_buf *out)
{
    static const char *const fields[] = {"provider", "model", "effort",
        "operator", "host", "context", "mode", "hour", "minute", "second",
        "goal_spinner", "activity_spinner"};
    unsigned int spinners = 0u;

    for (size_t i = 0u; i < len; ++i) {
        unsigned char c = (unsigned char)text[i];
        size_t field = 0u;

        if (c < 0x20u || c == 0x7fu)
            goto invalid;
        if (c == '\\') {
            if (++i >= len || (text[i] != '\\' && text[i] != '{' &&
                              text[i] != '}'))
                goto invalid;
            if (out && snag_buf_putc(out, (unsigned char)text[i]) < 0)
                return -1;
        } else if (c == '{') {
            const char *end = memchr(text + i + 1u, '}', len - i - 1u);
            size_t field_len = end ? (size_t)(end - text - i - 1u) : 0u;
            const char *format = memchr(text + i + 1u, ':', field_len);
            size_t width = 0u;
            unsigned char fill = ' ';

            if (format)
                field_len = (size_t)(format - text - i - 1u);

            while (field < sizeof(fields) / sizeof(fields[0]) &&
                   (strlen(fields[field]) != field_len ||
                    memcmp(fields[field], text + i + 1u, field_len) != 0))
                ++field;
            if (!end || field == sizeof(fields) / sizeof(fields[0]) ||
                (field >= SNAG_PROMPT_FIELD_COUNT &&
                 (spinners & (1u << (field - SNAG_PROMPT_FIELD_COUNT)))))
                goto invalid;
            if (format) {
                bool clock = field >= SNAG_PROMPT_HOUR &&
                             field <= SNAG_PROMPT_SECOND;

                if (field != SNAG_PROMPT_CONTEXT && !clock)
                    goto invalid;
                ++format;
                if (format < end && *format == '0') {
                    if (!clock)
                        goto invalid;
                    fill = '0';
                    ++format;
                }
                if (format == end || *format < '1' || *format > '9')
                    goto invalid;
                for (; format < end; ++format) {
                    if (*format < '0' || *format > '9' ||
                        width > (SNAG_TERM_LABEL_BYTES - 2u -
                                 (unsigned int)(*format - '0')) / 10u)
                        goto invalid;
                    width = width * 10u + (unsigned int)(*format - '0');
                }
            }
            if (field >= SNAG_PROMPT_FIELD_COUNT) {
                spinners |= 1u << (field - SNAG_PROMPT_FIELD_COUNT);
                if (out && snag_buf_putc(out,
                        marker + field - SNAG_PROMPT_FIELD_COUNT) < 0)
                    return -1;
            } else if (out) {
                size_t value_len = strlen(values[field]);

                if (values[field][0] == '-')
                    fill = ' ';
                for (size_t pad = value_len; pad < width; ++pad)
                    if (snag_buf_putc(out, fill) < 0)
                        return -1;
                if (snag_buf_append(out, values[field], value_len) < 0)
                    return -1;
            }
            i = (size_t)(end - text);
        } else if (c == '}') {
            goto invalid;
        } else if (out && snag_buf_putc(out, c) < 0) {
            return -1;
        }
    }
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

static int
parse_prompt(const char *text, unsigned int selected,
             const char *const values[SNAG_PROMPT_FIELD_COUNT],
             unsigned char marker,
             struct snag_buf *out)
{
    static const char *const names[] = {"chat:", "rollout-idle:",
                                        "rollout-active:"};
    unsigned int seen = 0u;
    size_t len = strlen(text);

    for (size_t i = 0u; i < len; ++i) {
        unsigned char c = (unsigned char)text[i];
        const char *body = NULL;
        size_t name = 0u, depth = 1u, end;

        if (c < 0x20u || c == 0x7fu)
            goto invalid;
        if (c == '\\') {
            if (++i >= len || (text[i] != '\\' && text[i] != '{' &&
                              text[i] != '}'))
                goto invalid;
            if (out && snag_buf_putc(out, (unsigned char)text[i]) < 0)
                return -1;
            continue;
        }
        if (c == '}')
            goto invalid;
        if (c != '{') {
            if (out && snag_buf_putc(out, c) < 0)
                return -1;
            continue;
        }
        for (; name < 3u; ++name)
            if (strncmp(text + i + 1u, names[name], strlen(names[name])) == 0) {
                body = text + i + 1u + strlen(names[name]);
                break;
            }
        if (!body || (seen & (1u << name)))
            goto invalid;
        for (end = (size_t)(body - text); end < len && depth; ++end) {
            if (text[end] == '\\') ++end;
            else if (text[end] == '{') ++depth;
            else if (text[end] == '}') --depth;
        }
        if (depth || end - 1u == (size_t)(body - text) ||
            prompt_body(body, end - 1u - (size_t)(body - text), values, marker,
                        out && name == selected ? out : NULL) < 0)
            goto invalid;
        seen |= 1u << name;
        i = end - 1u;
    }
    if (seen == 7u)
        return 0;
invalid:
    errno = EINVAL;
    return -1;
}

static int
validate_prompt(const char *text)
{
    return parse_prompt(text, 3u, NULL, 0u, NULL);
}

int
snag_config_prompt_expand(const char *text, unsigned int mode,
                         const char *const values[SNAG_PROMPT_FIELD_COUNT],
                         unsigned char marker,
                         char *label, size_t label_size)
{
    struct snag_buf out;
    int rc = -1;

    if (!text || mode >= 3u || !values || !label || label_size < 2u ||
        marker > 0xfeu) {
        errno = EINVAL;
        return -1;
    }
    snag_buf_init(&out, label_size);
    if (parse_prompt(text, mode, values, marker, &out) < 0 || !out.len ||
        snag_buf_putc(&out, ' ') < 0 || snag_buf_terminate(&out) < 0)
        goto out;
    memcpy(label, out.data, out.len + 1u);
    rc = 0;
out:
    snag_buf_free(&out);
    return rc;
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
set_provider_section(struct parse_state *state, const char *name)
{
    struct snag_config *config = state->config;
    const unsigned char *p = (const unsigned char *)name;

    if (!*p || strlen(name) > SNAG_CONFIG_PROVIDER_NAME_MAX)
        goto invalid;
    for (; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') ||
              (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '_' || *p == '-'))
            goto invalid;
    if (!state->providers_started) {
        for (size_t i = 0; i < config->provider_count; ++i)
            snag_secret_source_free(&config->providers[i].api_key);
        memset(config->providers, 0, sizeof(config->providers));
        config->provider_count = 0u;
        state->providers_started = true;
    }
    for (size_t i = 0; i < config->provider_count; ++i)
        if (strcmp(config->providers[i].name, name) == 0)
            goto invalid;
    if (config->provider_count >= SNAG_CONFIG_PROVIDER_MAX)
        goto invalid;
    state->provider_index = config->provider_count++;
    snag_config_provider_init(&config->providers[state->provider_index], name);
    state->section = SECTION_PROVIDER;
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

static bool
provider_name_valid(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;

    if (!*p || strlen(name) > SNAG_CONFIG_PROVIDER_NAME_MAX)
        return false;
    for (; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') ||
              (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '_' || *p == '-'))
            return false;
    return true;
}

static int
set_model_limit_section(struct parse_state *state, char *name)
{
    struct snag_config *config = state->config;
    struct snag_model_limit_config *limit;
    char *slash = strchr(name, '/');
    const char *model = slash ? slash + 1u : "";

    if ((slash && (slash == name || !slash[1])) ||
        strlen(model) >= SNAG_CONFIG_MODEL_MAX ||
        !snag_utf8_valid((const unsigned char *)model, strlen(model), true) ||
        config->model_limit_count >= SNAG_CONFIG_MODEL_LIMIT_MAX)
        goto invalid;
    if (slash)
        *slash = '\0';
    if (!provider_name_valid(name))
        goto invalid;
    for (size_t i = 0; i < config->model_limit_count; ++i)
        if (strcmp(config->model_limits[i].provider, name) == 0 &&
            strcmp(config->model_limits[i].model, model) == 0)
            goto invalid;
    state->model_limit_index = config->model_limit_count++;
    limit = &config->model_limits[state->model_limit_index];
    memset(limit, 0, sizeof(*limit));
    (void)snprintf(limit->provider, sizeof(limit->provider), "%s", name);
    (void)snprintf(limit->model, sizeof(limit->model), "%s", model);
    state->section = SECTION_MODEL_LIMIT;
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

static int
set_model_alias_section(struct parse_state *state, char *name)
{
    char *slash = strchr(name, '/');
    size_t index;

    if (!slash || state->model_count >= SNAG_CONFIG_MODEL_ALIAS_MAX)
        goto invalid;
    *slash = '\0';
    if (!provider_name_valid(name) || !provider_name_valid(slash + 1u))
        goto invalid;
    for (size_t i = 0; i < state->model_count; ++i)
        if (strcmp(state->models[i].provider, name) == 0 &&
            strcmp(state->models[i].model.name, slash + 1u) == 0)
            goto invalid;
    index = state->model_alias_index = state->model_count++;
    (void)snag_strcpy(state->models[index].provider, sizeof(state->models[index].provider), name);
    (void)snag_strcpy(state->models[index].model.name, sizeof(state->models[index].model.name), slash + 1u);
    state->section = SECTION_MODEL_ALIAS;
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
    else if (strncmp(name, "provider ", 9u) == 0)
        return set_provider_section(state, trim(name + 9u));
    else if (strncmp(name, "model-limit ", 12u) == 0)
        return set_model_limit_section(state, trim(name + 12u));
    else if (strncmp(name, "model-alias ", 12u) == 0)
        return set_model_alias_section(state, trim(name + 12u));
    else if (strcmp(name, "ui") == 0)
        section = SECTION_UI;
    else if (strcmp(name, "irc") == 0)
        section = SECTION_IRC;
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
claim_key(struct parse_state *state, const char *key)
{
    const char **seen = state->section == SECTION_PROVIDER ?
        state->seen_provider_keys[state->provider_index] :
        state->section == SECTION_MODEL_LIMIT ?
        state->seen_model_limit_keys[state->model_limit_index] :
        state->section == SECTION_MODEL_ALIAS ?
        state->seen_alias_keys[state->model_alias_index] :
        state->seen_keys[state->section];

    /* Keys borrow the parsed file until parse_file returns. IRC clients repeat. */
    if ((state->section == SECTION_IRC && strcmp(key, "client") == 0) ||
        (state->section == SECTION_TOOL && strcmp(key, "secret") == 0))
        return 0;
    for (size_t i = 0; i < sizeof(state->seen_keys[0]) /
                           sizeof(state->seen_keys[0][0]); ++i) {
        if (!seen[i]) {
            seen[i] = key;
            return 0;
        }
        if (strcmp(seen[i], key) == 0)
            break;
    }
    errno = EINVAL;
    return -1;
}

static int
parse_agent(struct parse_state *state, const char *key, const char *value)
{
    struct snag_config *config = state->config;
    if (strcmp(key, "provider") == 0) {
        if (strlen(value) > SNAG_CONFIG_PROVIDER_NAME_MAX)
            goto invalid;
        return copy_value(config->provider, sizeof(config->provider), value);
    }
    if (strcmp(key, "model") == 0) {
        return copy_value(config->model, sizeof(config->model), value);
    }
    if (strcmp(key, "reasoning_effort") == 0) {
        return copy_value(config->reasoning_effort,
                          sizeof(config->reasoning_effort), value);
    }
    if (strcmp(key, "max_goal_prompt_bytes") == 0)
        return parse_u32(value, 1u, 1024u * 1024u,
                         &config->max_goal_prompt_bytes);
    if (strcmp(key, "read_agents_md") == 0)
        return parse_bool(value, &config->read_agents_md);
invalid:
    errno = EINVAL;
    return -1;
}

static int
parse_provider(struct parse_state *state, const char *key, const char *value)
{
    struct snag_provider_config *provider =
        &state->config->providers[state->provider_index];
    if (strcmp(key, "parallel_tool_calls") == 0)
        return parse_bool(value, &provider->parallel_tool_calls);
    if (strcmp(key, "connect_timeout_ms") == 0)
        return parse_u32(value, 1000u, 120000u, &provider->connect_timeout_ms);
    if (strcmp(key, "idle_timeout_ms") == 0)
        return parse_u32(value, 1000u, 600000u, &provider->idle_timeout_ms);
    if (strcmp(key, "request_timeout_ms") == 0)
        return parse_u32(value, 1000u, 3600000u, &provider->request_timeout_ms);
    if (strcmp(key, "auth") == 0) {
        if (strcmp(value, "api_key") == 0)
            provider->auth = SNAG_AUTH_API_KEY;
        else if (strcmp(value, "chatgpt") == 0)
            provider->auth = SNAG_AUTH_CHATGPT;
        else
            goto invalid;
        return 0;
    }
    if (strcmp(key, "auto_compact_input_tokens") == 0) {
        if (strcmp(value, "auto") == 0) {
            provider->auto_compact_input_tokens = SNAG_CONFIG_COMPACT_AUTO;
            return 0;
        }
        return parse_u32(value, 0u, 4000000u,
                         &provider->auto_compact_input_tokens);
    }
    if (strcmp(key, "base_url") == 0)
        return copy_base_url(provider->base_url,
                             sizeof(provider->base_url), value);
    if (strcmp(key, "api_key") == 0)
        return snag_secret_source_parse(&provider->api_key, value,
                                          state->config->source_path, NULL, 0u);
    if (strcmp(key, "exact_token_count") == 0)
        return parse_token_count(value, &provider->exact_token_count);
    if (strcmp(key, "native_compaction") == 0)
        return parse_bool(value, &provider->native_compaction);
    if (strcmp(key, "openrouter_referer") == 0)
        return copy_header_value(provider->openrouter_referer,
                                 sizeof(provider->openrouter_referer),
                                 value);
    if (strcmp(key, "openrouter_title") == 0)
        return copy_header_value(provider->openrouter_title,
                                 sizeof(provider->openrouter_title),
                                 value);
invalid:
    errno = EINVAL;
    return -1;
}

static int
parse_model_limit(struct parse_state *state, const char *key, const char *value)
{
    struct snag_model_limit_config *limit =
        &state->config->model_limits[state->model_limit_index];

    if (strcmp(key, "context_window_tokens") == 0)
        return parse_u64(value, 1u, SNAG_CONFIG_TOKEN_LIMIT_MAX,
                         &limit->context_window_tokens);
    if (strcmp(key, "max_input_tokens") == 0)
        return parse_u64(value, 1u, SNAG_CONFIG_TOKEN_LIMIT_MAX,
                         &limit->max_input_tokens);
    if (strcmp(key, "max_output_tokens") == 0)
        return parse_u64(value, 1u, SNAG_CONFIG_TOKEN_LIMIT_MAX,
                         &limit->max_output_tokens);
    errno = EINVAL;
    return -1;
}

static int
parse_ui(struct parse_state *state, const char *key, const char *value)
{
    struct snag_config *config = state->config;
    uint32_t parsed;
    if (strcmp(key, "color") == 0) {
        if (strcmp(value, "auto") == 0)
            config->color = SNAG_COLOR_AUTO;
        else if (strcmp(value, "always") == 0)
            config->color = SNAG_COLOR_ALWAYS;
        else if (strcmp(value, "never") == 0)
            config->color = SNAG_COLOR_NEVER;
        else {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "resume_history_turns") == 0) {
        if (parse_u32(value, 0u, 100u, &parsed) < 0)
            return -1;
        config->resume_history_turns = (unsigned int)parsed;
        return 0;
    }
    if (strcmp(key, "typing_pause_ms") == 0)
        return parse_u32(value, 0u, 5000u, &config->typing_pause_ms);
    if (strcmp(key, "markdown") == 0)
        return parse_bool(value, &config->markdown);
    if (strcmp(key, "prompt") == 0)
        return copy_value(config->prompt, sizeof(config->prompt), value) < 0 ?
               -1 : validate_prompt(config->prompt);
    if (strcmp(key, "prompt_spinner_goal") == 0)
        return parse_spinner(config->prompt_spinner_goal, value);
    if (strcmp(key, "prompt_spinner_provider") == 0)
        return parse_spinner(config->prompt_spinner_provider, value);
    if (strcmp(key, "prompt_spinner_tool") == 0)
        return parse_spinner(config->prompt_spinner_tool, value);
    if (strcmp(key, "prompt_spinner_per_second") == 0)
        return parse_u32(value, 1u, 60u,
                         &config->prompt_spinner_per_second);
    errno = EINVAL;
    return -1;
}

static int
parse_irc(struct parse_state *state, const char *key, const char *value)
{
    struct snag_config *config = state->config;

    if (strcmp(key, "listen") == 0) {
        if (copy_value(config->irc.listen, sizeof(config->irc.listen),
                       value) < 0)
            return -1;
        config->irc.listen_explicit = true;
        return 0;
    }
    if (strcmp(key, "client") == 0) {
        if (config->irc.client_count >= SNAG_CONFIG_IRC_CLIENT_MAX)
            goto invalid;
        for (size_t i = 0; i < config->irc.client_count; ++i)
            if (strcmp(config->irc.clients[i], value) == 0)
                goto invalid;
        if (copy_value(config->irc.clients[config->irc.client_count],
                       sizeof(config->irc.clients[0]), value) < 0)
            return -1;
        ++config->irc.client_count;
        return 0;
    }
    if (strcmp(key, "model_nick") == 0) {
        if (copy_value(config->irc.model_nick,
                       sizeof(config->irc.model_nick), value) < 0)
            return -1;
        config->irc.model_nick_implicit = false;
        return 0;
    }
    if (strcmp(key, "operator_nick") == 0) {
        if (copy_value(config->irc.operator_nick,
                       sizeof(config->irc.operator_nick), value) < 0)
            return -1;
        config->irc.operator_nick_implicit = false;
        return 0;
    }
    if (strcmp(key, "room_name") == 0)
        return copy_value(config->irc.room_name,
                          sizeof(config->irc.room_name), value);
    if (strcmp(key, "history_lines") == 0)
        return parse_u32(value, 1u, 1000u, &config->irc.history_lines);
invalid:
    errno = EINVAL;
    return -1;
}

static int
parse_tool(struct parse_state *state, const char *key, const char *value)
{
    struct snag_config *config = state->config;
    char *copy;
    if (strcmp(key, "shell") == 0) {
        if (!snag_path_root_len(value))
            goto invalid;
        copy = snag_strdup_checked(value, SNAG_CONFIG_PATH_MAX);
        if (!copy)
            return -1;
        free(config->shell);
        config->shell = copy;
        return 0;
    }
    if (strcmp(key, "default_yield_ms") == 0)
        return parse_u32(value, 0u, 600000u, &config->default_yield_ms);
    if (strcmp(key, "max_parallel_commands") == 0)
        return parse_u32(value, 1u, 32u, &config->max_parallel_commands);
    if (strcmp(key, "default_timeout_ms") == 0)
        return parse_u32(value, 0u, UINT32_MAX,
                         &config->default_timeout_ms);
    if (strcmp(key, "max_timeout_ms") == 0)
        return parse_u32(value, 1u, UINT32_MAX, &config->max_timeout_ms);
    if (strcmp(key, "secret") == 0) {
        if (config->secret_count >= SNAG_CONFIG_SECRET_MAX ||
            snag_secret_source_parse(&config->secrets[config->secret_count], value,
                                       config->source_path, NULL, 0u) < 0)
            goto invalid;
        ++config->secret_count;
        return 0;
    }
    if (strcmp(key, "max_output_bytes") == 0)
        return parse_u32(value, 0u, UINT32_MAX, &config->max_output_bytes);
    if (strcmp(key, "max_output_tokens") == 0)
        return parse_u32(value, 1u, (uint32_t)SNAG_CONFIG_TOKEN_LIMIT_MAX,
                         &config->max_output_tokens);
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
    if (!*key || claim_key(state, key) < 0)
        goto invalid;
    switch (state->section) {
    case SECTION_AGENT: return parse_agent(state, key, value);
    case SECTION_PROVIDER: return parse_provider(state, key, value);
    case SECTION_MODEL_LIMIT: return parse_model_limit(state, key, value);
    case SECTION_MODEL_ALIAS:
        if (strcmp(key, "model") != 0)
            goto invalid;
        return copy_header_value(state->models[state->model_alias_index].model.upstream,
                                  SNAG_CONFIG_MODEL_MAX, value);
    case SECTION_UI: return parse_ui(state, key, value);
    case SECTION_IRC: return parse_irc(state, key, value);
    case SECTION_TOOL: return parse_tool(state, key, value);
    case SECTION_NONE: case SECTION_COUNT: break;
    }
invalid:
    errno = EINVAL;
    return -1;
}

static int
parse_file(struct snag_config *config, char *text, char *error, size_t error_size)
{
    struct parse_state state;
    char *line = text;
    unsigned int number = 1u;

    memset(&state, 0, sizeof(state));
    state.config = config;
    /* A present file has only its declared providers, including zero. */
    for (size_t i = 0; i < config->provider_count; ++i) {
        snag_secret_source_free(&config->providers[i].api_key);
        free(config->providers[i].models);
    }
    config->provider_count = 0u;
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
                snag_errorf(error, error_size,
                          "invalid configuration at line %u; use named [provider NAME], "
                          "api_key with ${ENV}, quoted literal or path, and repeatable tool secret", number);
                return -1;
            }
        }
        if (!next)
            break;
        line = next + 1u;
        ++number;
    }
    for (size_t i = 0; i < state.model_count; ++i) {
        struct snag_provider_config *provider = NULL;
        struct snag_provider_model *models;
        for (size_t j = 0; j < config->provider_count; ++j)
            if (strcmp(config->providers[j].name, state.models[i].provider) == 0)
                provider = &config->providers[j];
        if (!provider || !state.models[i].model.upstream[0]) {
            snag_errorf(error, error_size, "model-alias %s/%s needs a configured provider and one real upstream target",
                       state.models[i].provider, state.models[i].model.name);
            return -1;
        }
        models = realloc(provider->models, (provider->model_count + 1u) * sizeof(*models));
        if (!models)
            return -1;
        provider->models = models;
        provider->models[provider->model_count++] = state.models[i].model;
    }
    return 0;
}

char *
snag_config_path(const char *explicit_path, const char *dotdir,
                char *error, size_t error_size)
{
    struct snag_buf path;
    char *result = NULL;

    if (explicit_path) {
        if (!snag_path_root_len(explicit_path) ||
            strlen(explicit_path) > SNAG_CONFIG_PATH_MAX) {
            snag_errorf(error, error_size,
                      "--config requires an absolute path within the supported limit");
            errno = EINVAL;
            return NULL;
        }
        result = snag_strdup_checked(explicit_path, SNAG_CONFIG_PATH_MAX);
        if (!result)
            snag_errorf(error, error_size, "configuration path is unavailable");
        return result;
    }
    snag_buf_init(&path, SNAG_CONFIG_PATH_MAX);
    if (!snag_path_root_len(dotdir))
        goto invalid;
    if (snag_buf_printf(&path, "%s/config.ini", dotdir) < 0)
        goto unavailable;
    if (snag_buf_terminate(&path) < 0)
        goto unavailable;
    result = (char *)path.data;
    path.data = NULL;
    snag_buf_free(&path);
    return result;
invalid:
    snag_errorf(error, error_size,
              "configuration requires an absolute dotdir");
    errno = EINVAL;
    snag_buf_free(&path);
    return NULL;
unavailable:
    snag_errorf(error, error_size, "configuration path exceeds the supported limit");
    snag_buf_free(&path);
    return NULL;
}

static int
read_config(const char *path, bool require_file, struct snag_buf *text,
            snag_file_info *file_stat, bool *private_file,
            struct snag_permissions *permissions, char *error, size_t error_size)
{
    snag_file_info st;
    int fd;
    int rc = -1;

    fd = snag_open_read_security_at(-1, path, false);
    if (fd < 0 && !permissions && errno == EACCES)
        fd = snag_open_read(path, false);
    if (fd < 0) {
        if (!require_file && errno == ENOENT)
            return 1;
        snag_errorf(error, error_size, "cannot open configuration %s: %s",
                  path, strerror(errno));
        return -1;
    }
    if (snag_fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uintmax_t)st.st_size > SNAG_CONFIG_FILE_MAX) {
        snag_errorf(error, error_size,
                  "configuration must be a regular file no larger than 64 KiB");
        errno = EINVAL;
        goto out;
    }
    if (file_stat)
        *file_stat = st;
    if (permissions && snag_permissions_capture(fd, permissions) < 0) {
        snag_errorf(error, error_size, "cannot retain configuration permissions: %s", strerror(errno));
        goto out;
    }
    if (private_file) {
        struct snag_file_privacy privacy;
        int saved = errno;
        *private_file = snag_fd_privacy(fd, &privacy) == 0 &&
                        privacy.effective_owner && privacy.private_access;
        errno = saved;
    }
    for (;;) {
        unsigned char chunk[4096];
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            snag_errorf(error, error_size, "cannot read configuration: %s",
                      strerror(errno));
            goto out;
        }
        if (got == 0)
            break;
        if (snag_buf_append(text, chunk, (size_t)got) < 0) {
            snag_errorf(error, error_size, "configuration exceeds 64 KiB");
            goto out;
        }
    }
    if (!snag_utf8_valid(text->data, text->len, true)) {
        snag_errorf(error, error_size,
                  "configuration must be valid UTF-8 without NUL bytes");
        errno = EILSEQ;
        goto out;
    }
    if (snag_buf_terminate(text) < 0) {
        snag_errorf(error, error_size, "cannot terminate configuration buffer");
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
validate_shell(struct snag_config *config, char *error, size_t error_size)
{
    char *resolved;
    snag_file_info st;
    if (!snag_path_root_len(config->shell))
        goto invalid;
    resolved = snag_realpath(config->shell);
    if (!resolved)
        goto invalid;
    if (strlen(resolved) > SNAG_CONFIG_PATH_MAX || snag_stat(resolved, &st) < 0 ||
        !S_ISREG(st.st_mode) || access(resolved, X_OK) < 0) {
        free(resolved);
        goto invalid;
    }
    free(config->shell);
    config->shell = resolved;
    return 0;
invalid:
    snag_errorf(error, error_size,
              "configured shell must resolve to an executable regular file");
    errno = EINVAL;
    return -1;
}

static bool
config_has_literals(const struct snag_config *config)
{
    for (size_t i = 0; i < config->provider_count; ++i)
        if (config->providers[i].api_key.kind == SNAG_SECRET_LITERAL)
            return true;
    for (size_t i = 0; i < config->secret_count; ++i)
        if (config->secrets[i].kind == SNAG_SECRET_LITERAL)
            return true;
    return false;
}

static int
validate_config(struct snag_config *config, bool private_file,
                 char *error, size_t error_size)
{
    if (config_has_literals(config) && !private_file) {
        snag_errorf(error, error_size, "literal secrets require an owner-private configuration file (0600)");
        errno = EACCES;
        return -1;
    }
    for (size_t i = 0; i < config->provider_count; ++i) {
        if (config->providers[i].auth == SNAG_AUTH_CHATGPT &&
            (strcmp(config->providers[i].base_url, SNAG_CHATGPT_BASE) != 0 ||
             config->providers[i].api_key.kind != SNAG_SECRET_NONE)) {
            snag_errorf(error, error_size,
                       "chatgpt authentication requires " SNAG_CHATGPT_BASE " and no api_key");
            errno = EINVAL;
            return -1;
        }
    }
    for (size_t i = 0; i < config->model_limit_count; ++i) {
        const struct snag_model_limit_config *limit = &config->model_limits[i];
        if (!snag_config_provider(config, limit->provider) ||
            (!limit->context_window_tokens && !limit->max_input_tokens &&
             !limit->max_output_tokens) ||
            (limit->context_window_tokens && limit->max_output_tokens &&
             limit->max_output_tokens >= limit->context_window_tokens)) {
            snag_errorf(error, error_size,
                      "invalid model-limit section for %s/%s",
                      limit->provider, limit->model);
            errno = EINVAL;
            return -1;
        }
    }
    if (config->default_timeout_ms > config->max_timeout_ms) {
        snag_errorf(error, error_size,
                  "tool default_timeout_ms cannot exceed max_timeout_ms");
        errno = EINVAL;
        return -1;
    }
    if (validate_shell(config, error, error_size) < 0)
        return -1;
    if (config->provider[0] && !snag_config_provider(config, config->provider)) {
        snag_errorf(error, error_size,
                  "configured agent provider is not defined");
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int
snag_config_load(struct snag_config *config, const char *explicit_path,
                const char *dotdir,
                char *error, size_t error_size)
{
    struct snag_buf text;
    char *owned_path = NULL;
    const char *path = explicit_path;
    snag_file_info file_stat;
    int read_rc;
    int rc = -1;

    if (!config->shell) {
        snag_errorf(error, error_size, "cannot initialize configuration defaults");
        errno = ENOMEM;
        return -1;
    }
    owned_path = snag_config_path(explicit_path, dotdir, error, error_size);
    if (!owned_path)
        return -1;
    path = owned_path;
    (void)snag_strcpy(config->source_path, sizeof(config->source_path), path);
    snag_buf_init(&text, SNAG_CONFIG_FILE_MAX + 1u);
    bool private_file = false;
    read_rc = read_config(path, explicit_path != NULL, &text, &file_stat, &private_file, NULL,
                          error, error_size);
    if (read_rc < 0)
        goto out;
    if (read_rc == 0 && parse_file(config, (char *)text.data,
                                   error, error_size) < 0)
        goto out;
    rc = validate_config(config, read_rc != 0 || private_file, error, error_size);
out:
    free(owned_path);
    snag_secret_clear(text.data, text.len);
    snag_buf_free(&text);
    return rc;
}

static void
trim_span(const unsigned char **start, const unsigned char **end)
{
    while (*start < *end && (**start == ' ' || **start == '\t' ||
                             **start == '\r'))
        ++*start;
    while (*end > *start && ((*end)[-1] == ' ' || (*end)[-1] == '\t' ||
                             (*end)[-1] == '\r'))
        --*end;
}

static bool
line_is_section(const unsigned char *line, size_t len, const char *name)
{
    const unsigned char *start = line;
    const unsigned char *end = line + len;
    size_t name_len = strlen(name);

    if (end > start && end[-1] == '\n')
        --end;
    trim_span(&start, &end);
    return (size_t)(end - start) == name_len + 2u && start[0] == '[' &&
           start[name_len + 1u] == ']' &&
           memcmp(start + 1u, name, name_len) == 0;
}

static bool
line_is_other_section(const unsigned char *line, size_t len)
{
    const unsigned char *start = line;
    const unsigned char *end = line + len;

    if (end > start && end[-1] == '\n')
        --end;
    trim_span(&start, &end);
    return end > start + 1u && start[0] == '[' && end[-1] == ']';
}

static bool
line_has_key(const unsigned char *line, size_t len, const char *key)
{
    const unsigned char *start = line;
    const unsigned char *end = line + len;
    const unsigned char *equal;
    const unsigned char *key_end;
    size_t key_len = strlen(key);

    if (end > start && end[-1] == '\n')
        --end;
    trim_span(&start, &end);
    if (start == end || *start == '#' || *start == ';')
        return false;
    equal = memchr(start, '=', (size_t)(end - start));
    if (!equal)
        return false;
    key_end = equal;
    while (key_end > start && (key_end[-1] == ' ' || key_end[-1] == '\t' ||
                               key_end[-1] == '\r'))
        --key_end;
    return (size_t)(key_end - start) == key_len &&
           memcmp(start, key, key_len) == 0;
}

static int
append_assignment(struct snag_buf *out, const char *key, const char *value,
                  const unsigned char *ending, size_t ending_len)
{
    return snag_buf_printf(out, "%s = %s", key, value) < 0 ||
           snag_buf_append(out, ending, ending_len) < 0 ? -1 : 0;
}

static int
append_missing_model_settings(struct snag_buf *out, bool seen[3],
                              const char *provider, const char *model,
                              const char *effort)
{
    static const unsigned char newline = '\n';
    const char *keys[3] = {"provider", "model", "reasoning_effort"};
    const char *values[3] = {provider, model, effort};

    if (out->len && out->data[out->len - 1u] != '\n' &&
        snag_buf_putc(out, '\n') < 0)
        return -1;
    for (size_t i = 0u; i < 3u; ++i)
        if (!seen[i] && append_assignment(out, keys[i], values[i],
                                          &newline, 1u) < 0)
            return -1;
    return 0;
}

static int
replace_model_settings(const struct snag_buf *input, struct snag_buf *output,
                       const char *provider, const char *model,
                       const char *effort)
{
    size_t offset = 0u;
    bool in_agent = false;
    bool saw_agent = false;
    bool added_missing = false;
    bool seen[3] = {false, false, false};
    const char *keys[3] = {"provider", "model", "reasoning_effort"};
    const char *values[3] = {provider, model, effort};

    while (offset < input->len) {
        const unsigned char *line = input->data + offset;
        const unsigned char *newline = memchr(line, '\n', input->len - offset);
        size_t len = newline ? (size_t)(newline - line) + 1u :
                               input->len - offset;

        if (line_is_section(line, len, "agent")) {
            in_agent = true;
            saw_agent = true;
        } else if (in_agent && line_is_other_section(line, len)) {
            if (append_missing_model_settings(output, seen, provider,
                                              model, effort) < 0)
                return -1;
            added_missing = true;
            in_agent = false;
        }
        if (in_agent) {
            size_t ending_len = len && line[len - 1u] == '\n' ? 1u : 0u;
            if (ending_len && len >= 2u && line[len - 2u] == '\r')
                ending_len = 2u;
            for (size_t i = 0u; i < 3u; ++i) {
                if (!line_has_key(line, len, keys[i]))
                    continue;
                if (append_assignment(output, keys[i], values[i],
                                      line + len - ending_len,
                                      ending_len) < 0)
                    return -1;
                seen[i] = true;
                goto next_line;
            }
        }
        if (snag_buf_append(output, line, len) < 0)
            return -1;
next_line:
        offset += len;
    }
    if (saw_agent && !added_missing) {
        if (append_missing_model_settings(output, seen, provider,
                                          model, effort) < 0)
            return -1;
    } else if (!saw_agent) {
        static const char heading[] = "[agent]\n";
        if (output->len && output->data[output->len - 1u] != '\n' &&
            snag_buf_putc(output, '\n') < 0)
            return -1;
        if (snag_buf_append(output, heading, sizeof(heading) - 1u) < 0 ||
            append_missing_model_settings(output, seen, provider,
                                          model, effort) < 0)
            return -1;
    }
    return 0;
}

static bool
same_file(const snag_file_info *left, const snag_file_info *right)
{
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
           left->st_size == right->st_size && left->st_mtime == right->st_mtime &&
           left->st_mode == right->st_mode;
}

static int
validate_config_text(const struct snag_buf *text, const char *path,
                     bool private_file,
                     char *error, size_t error_size)
{
    struct snag_config candidate;
    char *copy;
    int rc = -1;

    copy = malloc(text->len + 1u);
    if (!copy)
        return -1;
    memcpy(copy, text->data, text->len);
    copy[text->len] = '\0';
    snag_config_init(&candidate);
    (void)snag_strcpy(candidate.source_path, sizeof(candidate.source_path), path);
    if (!candidate.shell) {
        snag_errorf(error, error_size, "cannot initialize configuration defaults");
        goto out;
    }
    if (parse_file(&candidate, copy, error, error_size) < 0)
        goto out;
    rc = validate_config(&candidate, private_file, error, error_size);
out:
    snag_config_free(&candidate);
    snag_secret_clear(copy, text->len);
    free(copy);
    return rc;
}

static int
provider_settings(struct snag_buf *output, const struct snag_provider_config *p)
{
    const char *auth = p->auth == SNAG_AUTH_CHATGPT ? "chatgpt" :
                       "api_key";
    if (snag_buf_printf(output, "auth = %s\nbase_url = %s\nnative_compaction = %s\nparallel_tool_calls = %s\n",
                        auth, p->base_url, p->native_compaction ? "true" : "false",
                        p->parallel_tool_calls ? "true" : "false") < 0)
        return -1;
    return p->api_key.kind == SNAG_SECRET_NONE ? 0 :
        snag_buf_printf(output, "api_key = %s\n", p->api_key.expression);
}

static int
replace_provider_settings(const struct snag_buf *input, struct snag_buf *output,
                           const struct snag_provider_config *provider)
{
    size_t at = 0u;
    bool selected = false, found = false;

    while (at < input->len) {
        size_t end = at;
        char line[SNAG_CONFIG_FILE_MAX + 1u];
        char *s, *equal;
        while (end < input->len && input->data[end] != '\n')
            ++end;
        memcpy(line, input->data + at, end - at);
        line[end - at] = '\0';
        s = trim(line);
        if (*s == '[') {
            char *close = strchr(s, ']');
            selected = false;
            if (close) {
                *close = '\0';
                s = trim(s + 1);
                if (strncmp(s, "provider ", 9u) == 0) {
                    const char *name = trim(s + 9);
                    selected = strcmp(name, provider->name) == 0;
                }
            }
            if (snag_buf_append(output, input->data + at, end - at) < 0 ||
                snag_buf_putc(output, '\n') < 0)
                return -1;
            if (selected) {
                found = true;
                if (provider_settings(output, provider) < 0)
                    return -1;
            }
        } else {
            bool replaced = false;
            equal = strchr(s, '=');
            if (selected && equal) {
                *equal = '\0';
                s = trim(s);
                replaced = strcmp(s, "auth") == 0 || strcmp(s, "base_url") == 0 ||
                    strcmp(s, "api_key") == 0 || strcmp(s, "native_compaction") == 0 ||
                    strcmp(s, "parallel_tool_calls") == 0;
            }
            if (!replaced && snag_buf_append(output, input->data + at,
                    end - at + (end < input->len ? 1u : 0u)) < 0)
                return -1;
        }
        at = end + 1u;
    }
    if (!found) {
        if (snag_buf_printf(output, "\n[provider %s]\n", provider->name) < 0 ||
            provider_settings(output, provider) < 0)
            return -1;
    }
    return 0;
}

int
snag_config_validate_provider(const struct snag_provider_config *provider,
                             char *error, size_t error_size)
{
    struct snag_buf text;
    int rc = -1;
    if (!provider || !provider_name_valid(provider->name) ||
        (provider->auth == SNAG_AUTH_CHATGPT &&
         (strcmp(provider->base_url, SNAG_CHATGPT_BASE) || provider->api_key.kind != SNAG_SECRET_NONE))) {
        snag_errorf(error, error_size, "invalid provider or ChatGPT endpoint");
        return -1;
    }
    snag_buf_init(&text, SNAG_CONFIG_FILE_MAX);
    if (snag_buf_printf(&text, "[provider %s]\n", provider->name) == 0 &&
        provider_settings(&text, provider) == 0)
        rc = validate_config_text(&text, "/config.ini", true, error, error_size);
    snag_secret_clear(text.data, text.len);
    snag_buf_free(&text);
    return rc;
}

static int
save_config_settings(const char *path, bool allow_create,
                      const char *provider, const char *model,
                      const char *effort,
                      const struct snag_provider_config *provider_config,
                      char *error, size_t error_size)
{
    struct snag_buf input;
    struct snag_buf output;
    snag_file_info before;
    snag_file_info current;
    struct snag_permissions permissions = {0};
    struct snag_directory_lock directory_lock = {.fd = -1};
    char id[SNAG_ID_HEX_LEN + 1u];
    char temp[64] = {0};
    char leaf[NAME_MAX + 1u];
    char *path_copy = NULL;
    char *slash;
    int parent_fd = -1;
    int fd = -1;
    int read_rc;
    int rc = -1;
    int saved;

    memset(&before, 0, sizeof(before));
    snag_buf_init(&input, SNAG_CONFIG_FILE_MAX + 1u);
    snag_buf_init(&output, SNAG_CONFIG_FILE_MAX);
    if (!snag_path_root_len(path) || strlen(path) > SNAG_CONFIG_PATH_MAX ||
        !provider || !*provider || strlen(provider) > SNAG_CONFIG_PROVIDER_NAME_MAX ||
        !model || (!provider_config && !*model) || strlen(model) >= SNAG_CONFIG_MODEL_MAX ||
        !effort || !*effort || strlen(effort) >= SNAG_CONFIG_EFFORT_MAX ||
        strchr(provider, '\n') || strchr(provider, '\r') ||
        strchr(model, '\n') || strchr(model, '\r') ||
        strchr(effort, '\n') || strchr(effort, '\r')) {
        snag_errorf(error, error_size, "refusing to save invalid model settings");
        errno = EINVAL;
        goto out;
    }
    path_copy = snag_strdup_checked(path, SNAG_CONFIG_PATH_MAX);
    if (!path_copy)
        goto out;
    slash = strrchr(path_copy, '/');
    if (!slash || !slash[1]) {
        snag_errorf(error, error_size, "configuration path has no file name");
        errno = EINVAL;
        goto out;
    }
    if (strlen(slash + 1u) > NAME_MAX) {
        snag_errorf(error, error_size, "configuration file name is too long");
        errno = ENAMETOOLONG;
        goto out;
    }
    memcpy(leaf, slash + 1u, strlen(slash + 1u) + 1u);
    if ((size_t)(slash - path_copy) < snag_path_root_len(path_copy))
        slash[1] = '\0';
    else
        *slash = '\0';
    parent_fd = snag_open_read(path_copy, true);
    if (parent_fd < 0) {
        snag_errorf(error, error_size, "cannot open configuration directory: %s",
                  strerror(errno));
        goto out;
    }
    /* Serialize cooperating writers without a second persistent state file. */
    if (snag_directory_lock_acquire(parent_fd, &directory_lock) < 0) {
        snag_errorf(error, error_size, "configuration directory is being updated; try again");
        goto out;
    }
    bool private_file = false;
    read_rc = read_config(path, !allow_create, &input, &before, &private_file,
                          &permissions, error, error_size);
    if (read_rc < 0)
        goto out;
    if ((provider_config ?
         replace_provider_settings(&input, &output, provider_config) :
         replace_model_settings(&input, &output, provider, model, effort)) < 0) {
        snag_errorf(error, error_size, "configuration update exceeds 64 KiB");
        goto out;
    }
    if (provider_config && *model) {
        struct snag_buf selected;
        snag_buf_init(&selected, SNAG_CONFIG_FILE_MAX);
        if (replace_model_settings(&output, &selected, provider, model, effort) < 0) {
            snag_buf_free(&selected);
            goto out;
        }
        snag_buf_free(&output);
        output = selected;
    }
    if (validate_config_text(&output, path,
                              read_rc != 0 || private_file,
                              error, error_size) < 0)
        goto out;
    if (snag_random_id(id) < 0)
        goto out;
    (void)snprintf(temp, sizeof(temp), ".snajpagent-config-%s.tmp", id);
    fd = snag_create_private_at(parent_fd, temp, true);
    if (fd < 0 || (read_rc == 0 && snag_permissions_apply(fd, &permissions) < 0) ||
        snag_write_full(fd, output.data, output.len) < 0 ||
        snag_sync_file(fd) < 0) {
        saved = errno;
        snag_errorf(error, error_size, "cannot write configuration: %s",
                  strerror(saved));
        errno = saved;
        goto out;
    }
    if (close(fd) < 0) {
        fd = -1;
        snag_errorf(error, error_size, "cannot close configuration: %s",
                  strerror(errno));
        goto out;
    }
    fd = -1;
    if (read_rc == 0) {
        int original = snag_open_read_security_at(parent_fd, leaf, false);
        bool unchanged = original >= 0 && snag_fstat(original, &current) == 0 &&
                         same_file(&before, &current) &&
                         snag_permissions_match(original, &permissions) == 1;
        if (original >= 0)
            (void)close(original);
        if (!unchanged) {
            snag_errorf(error, error_size,
                      "configuration changed while it was being saved");
            errno = EAGAIN;
            goto out;
        }
    } else if (snag_lstat_at(parent_fd, leaf, &current) == 0 ||
               errno != ENOENT) {
        snag_errorf(error, error_size,
                  "configuration appeared while it was being saved");
        errno = EAGAIN;
        goto out;
    }
    if (snag_rename_at(parent_fd, temp, parent_fd, leaf) < 0 ||
        snag_sync_dir(parent_fd) < 0) {
        saved = errno;
        snag_errorf(error, error_size, "cannot install configuration: %s",
                  strerror(saved));
        errno = saved;
        goto out;
    }
    temp[0] = '\0';
    rc = 0;
out:
    saved = errno;
    if (fd >= 0)
        (void)close(fd);
    if (parent_fd >= 0) {
        if (temp[0])
            (void)snag_unlink_at(parent_fd, temp, false);
        if (snag_directory_lock_release(&directory_lock) < 0 && rc == 0) {
            saved = errno;
            snag_errorf(error, error_size, "cannot unlock configuration directory: %s", strerror(saved));
            rc = -1;
        }
        (void)close(parent_fd);
    }
    free(path_copy);
    snag_permissions_free(&permissions);
    snag_secret_clear(output.data, output.len);
    snag_secret_clear(input.data, input.len);
    snag_buf_free(&output);
    snag_buf_free(&input);
    errno = saved;
    return rc;
}

int
snag_config_save_model(const char *path, bool allow_create,
                      const char *provider, const char *model, const char *effort,
                      char *error, size_t error_size)
{
    return save_config_settings(path, allow_create, provider, model, effort,
                                 NULL, error, error_size);
}

int
snag_config_save_provider(const char *path, bool allow_create,
                         const struct snag_provider_config *provider,
                         const char *initial_model, const char *effort,
                         char *error, size_t error_size)
{
    if (!provider || !provider_name_valid(provider->name) ||
        (provider->auth == SNAG_AUTH_CHATGPT && strcmp(provider->base_url, SNAG_CHATGPT_BASE)) ||
        strchr(provider->base_url, '\n') || strchr(provider->base_url, '\r')) {
        snag_errorf(error, error_size, "invalid provider settings");
        return -1;
    }
    return save_config_settings(path, allow_create, provider->name,
        initial_model ? initial_model : "", effort ? effort : "default",
        provider, error, error_size);
}

const struct snag_provider_config *
snag_config_provider(const struct snag_config *config, const char *name)
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

bool
snag_config_provider_is_openrouter(const struct snag_provider_config *provider)
{
    static const char host[] = "openrouter.ai";
    const char *url = provider ? provider->base_url : "";
    const char *suffix;

    if (strncmp(url, "https://", 8u) == 0)
        url += 8u;
    else if (strncmp(url, "http://", 7u) == 0)
        url += 7u;
    else
        return false;
    if (strncasecmp(url, host, sizeof(host) - 1u) != 0)
        return false;
    suffix = url + sizeof(host) - 1u;
    if (*suffix == '.')
        ++suffix;
    if (*suffix == ':') {
        unsigned int port = 0u;

        ++suffix;
        if (*suffix < '0' || *suffix > '9')
            return false;
        while (*suffix >= '0' && *suffix <= '9') {
            port = port * 10u + (unsigned int)(*suffix++ - '0');
            if (port > 65535u)
                return false;
        }
        if (!port)
            return false;
    }
    return *suffix == '\0' || *suffix == '/';
}

const char *
snag_config_model_upstream(const struct snag_provider_config *provider, const char *model)
{
    if (provider && model)
        for (size_t i = 0; i < provider->model_count; ++i)
            if (strcmp(provider->models[i].name, model) == 0)
                return provider->models[i].upstream;
    return model;
}

static bool
model_pattern_matches(const char *pattern, const char *name)
{
    const char *star = NULL, *retry = NULL;

    while (*name) {
        if (*pattern == '*') {
            star = ++pattern;
            retry = name;
        } else if (*pattern == *name) {
            ++pattern;
            ++name;
        } else if (star) {
            pattern = star;
            name = ++retry;
        } else {
            return false;
        }
    }
    while (*pattern == '*')
        ++pattern;
    return !*pattern;
}

bool
snag_config_resolve_limits(const struct snag_config *config, const char *provider,
                          const char *name,
                          struct snag_model_limit_config *out,
                          const struct snag_model_limit_config *sources[3])
{
    memset(out, 0, sizeof(*out));
    if (sources)
        memset(sources, 0, 3u * sizeof(*sources));
    for (unsigned int tier = 0u; tier < 3u; ++tier) {
        for (size_t i = 0; i < config->model_limit_count; ++i) {
            const struct snag_model_limit_config *rule = &config->model_limits[i];
            bool wildcard = strchr(rule->model, '*') != NULL;
            if (strcmp(rule->provider, provider) != 0)
                continue;
            if (tier == 0u ? rule->model[0] != '\0' :
                (!rule->model[0] || wildcard != (tier == 1u) ||
                 !model_pattern_matches(rule->model, name)))
                continue;
            if (rule->context_window_tokens) {
                out->context_window_tokens = rule->context_window_tokens;
                if (sources)
                    sources[0] = rule;
            }
            if (rule->max_input_tokens) {
                out->max_input_tokens = rule->max_input_tokens;
                if (sources)
                    sources[1] = rule;
            }
            if (rule->max_output_tokens) {
                out->max_output_tokens = rule->max_output_tokens;
                if (sources)
                    sources[2] = rule;
            }
        }
    }
    return out->context_window_tokens || out->max_input_tokens || out->max_output_tokens;
}
