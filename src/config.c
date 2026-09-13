/* SPDX-License-Identifier: GPL-2.0-only */
#include "config.h"
#include "fs.h"
#include "base.h"
#include "snajpagent.h"
#include "rules.h"

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
    SECTION_NONE, SECTION_AGENT, SECTION_PROVIDER, SECTION_MODEL_LIMIT,
    SECTION_MODEL_ALIAS, SECTION_UI, SECTION_IRC, SECTION_TOOL, SECTION_RULE, SECTION_COUNT };

struct parse_state {
    struct snag_config *config;
    enum section section;
    unsigned int seen_sections;
    const char *seen_keys[16];
    size_t provider_index;
    size_t model_limit_index;
    size_t model_alias_index;
    struct {
        char provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
        struct snag_provider_model model;
    } models[SNAG_CONFIG_MODEL_ALIAS_MAX];
    size_t model_count;
    json_t *rules;
    size_t rule_index;
};

static int
copy_value(char *dst, size_t size, const char *value)
{
    size_t len = strlen(value);
    if (!len || len >= size) return snag_errno(EINVAL);
    memcpy(dst, value, len + 1u);
    return 0;
}

static int
copy_header_value(char *dst, size_t size, const char *value)
{
    size_t len = strlen(value);
    if (!len || len >= size) goto invalid;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20u || c > 0x7eu) goto invalid;
    }
    memcpy(dst, value, len + 1u);
    return 0;
invalid: return snag_errno(EINVAL);
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
    static const char prompt[] = "{activity_spinner}{goal_spinner} {hour:02}:{minute:02}:{second:02} "
        "{chat:{operator}@{host} :}"
        "{rollout-idle:{provider}/{model}/{effort} {context:3}% {queued:({queue}) }›}"
        "{rollout-active:{provider}/{model}/{effort} {context:3}% {queued:({queue}) }»}";

    memset(config, 0, sizeof(*config));
    memcpy(config->model, "default", 8u);
    memcpy(config->reasoning_effort, "default", 8u);
    snag_config_provider_init(&config->providers[0], "openai");
    if (snag_secret_source_parse(&config->providers[0].api_key, "${OPENAI_API_KEY}", NULL, NULL, 0u) < 0)
        return;
    config->provider_count = 1u;
    config->max_goal_prompt_bytes = 256u * 1024u;
    config->max_turn_retries = 5u;
    config->read_agents_md = true;
#ifdef SNAJPAGENT_UPDATE_URL
    config->auto_update = strchr(SNAJPAGENT_VERSION, '-') == NULL;
    (void)snag_strcpy(config->update_url, sizeof(config->update_url), SNAJPAGENT_UPDATE_URL);
#endif
    config->color = SNAG_COLOR_AUTO;
    config->markdown = true;
    config->resume_history_turns = 1u;
    config->typing_pause_ms = 500u;
    memcpy(config->prompt, prompt, sizeof(prompt));
    memcpy(config->prompt_spinner_goal, " ⚑", sizeof(" ⚑"));
    memcpy(config->prompt_spinner_provider, " ◴◷◶◵", sizeof(" ◴◷◶◵"));
    memcpy(config->prompt_spinner_tool, " ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏", sizeof(" ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"));
    config->prompt_spinner_per_second = 8u;
    config->prompt_tool_spinner_off_delay_ms = 500u;
    memcpy(config->irc.listen, "localhost:6667", 15u);
    config->irc.history_lines = 200u;
    config->shell = snag_default_shell();
    config->default_yield_ms = 10000u;
    config->max_wait_ms = 60000u;
    config->max_parallel_commands = 4u;
    config->default_timeout_ms = 0u;
    config->max_timeout_ms = 86400000u;
    config->max_output_tokens = SNAG_DEFAULT_TOOL_OUTPUT_TOKENS;
    config->max_output_bytes = 0u;
}

void
snag_config_free(struct snag_config *config)
{
    snag_rules_free(config->rules);
    free(config->shell);
    for (size_t i = 0; i < config->provider_count; ++i) {
        snag_secret_source_free(&config->providers[i].api_key);
        free(config->providers[i].models);
    }
    for (size_t i = 0; i < config->secret_count; ++i) snag_secret_source_free(&config->secrets[i]);
    memset(config, 0, sizeof(*config));
}

static char *
trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r') ++s;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) --end;
    *end = '\0';
    return s;
}

static int
parse_u64(const char *text, uint64_t min, uint64_t max, uint64_t *out)
{
    uint64_t value = 0u;

    if (!*text) goto invalid;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        uint64_t digit;

        if (*p < '0' || *p > '9') goto invalid;
        digit = (uint64_t)(*p - '0');
        if (value > (UINT64_MAX - digit) / 10u) goto invalid;
        value = value * 10u + digit;
    }
    if (value < min || value > max) goto invalid;
    *out = value;
    return 0;
invalid: return snag_errno(EINVAL);
}

static int
parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *out)
{
    uint64_t value;
    if (parse_u64(text, min, max, &value) < 0) return -1;
    *out = (uint32_t)value;
    return 0;
}

static int
parse_bool(const char *text, bool *out)
{
    if (snag_string_in(text, "true 1")) {
        *out = true;
        return 0;
    }
    if (snag_string_in(text, "false 0")) {
        *out = false;
        return 0;
    }
    return snag_errno(EINVAL);
}

static int
parse_token_count(const char *text, enum snag_token_count_mode *out)
{
    bool enabled;

    if (strcmp(text, "auto") == 0) *out = SNAG_TOKEN_COUNT_AUTO;
    else if (parse_bool(text, &enabled) < 0) return -1;
    else *out = enabled ? SNAG_TOKEN_COUNT_STRICT : SNAG_TOKEN_COUNT_OFF;
    return 0;
}

static bool
unsafe_prompt_cp(uint32_t cp)
{
    return cp == 0x00ad || cp == 0x061c || cp == 0x200b || cp == 0x200e || cp == 0x200f ||
           (cp >= 0x202a && cp <= 0x202e) || cp == 0x2060 || (cp >= 0x2066 && cp <= 0x206f) || cp == 0xfeff ||
           (cp >= 0xfff9 && cp <= 0xfffb);
}

static int
parse_spinner(char dst[SNAG_CONFIG_SPINNER_MAX], const char *value)
{
    size_t len = strlen(value), pos, frames = 0u, previous = SIZE_MAX;

    if (len < 3u || value[0] != '"' || value[len - 1u] != '"' || len - 2u >= SNAG_CONFIG_SPINNER_MAX ||
        memchr(value + 1u, '"', len - 2u)) goto invalid;
    --len;
    pos = value[1] == '\\' && value[2] == '0' ? 3u : 1u;
    if (pos == 1u) {
        uint32_t cp;
        size_t n = snag_utf8_decode((const unsigned char *)value + 1u, len - 1u, &cp);

        if (!n || snag_char_width(cp) != 1 || unsafe_prompt_cp(cp)) goto invalid;
        pos += n;
    }
    while (pos < len) {
        uint32_t cp;
        size_t n = snag_utf8_decode((const unsigned char *)value + pos, len - pos, &cp);

        if (!n || ++frames > SNAG_CONFIG_SPINNER_FRAMES_MAX || snag_char_width(cp) != 1 ||
            unsafe_prompt_cp(cp) || (previous != SIZE_MAX && n == pos - previous &&
            memcmp(value + previous, value + pos, n) == 0)) goto invalid;
        previous = pos;
        pos += n;
    }
    memcpy(dst, value + 1u, len - 1u);
    dst[len - 1u] = '\0';
    return 0;
invalid: return snag_errno(EINVAL);
}

static size_t
prompt_end(const char *text, size_t len, size_t start)
{
    size_t depth = 1u;

    for (size_t i = start; i < len; ++i) {
        if (text[i] == '\\') ++i;
        else if (text[i] == '{') ++depth;
        else if (text[i] == '}' && !--depth) return i + 1u;
    }
    return 0u;
}

static int
prompt_body(const char *text, size_t len, const char *const values[SNAG_PROMPT_FIELD_COUNT],
            unsigned char marker, unsigned int spinners[3], unsigned int modes,
            unsigned int selected, unsigned int *seen, struct snag_buf *out)
{
    static const char *const fields[] = {"provider", "model", "effort",
        "operator", "host", "context", "mode", "queue", "hour", "minute", "second",
        "goal_spinner", "activity_spinner"};
    static const char *const names[] = {"chat:", "rollout-idle:", "rollout-active:"};

    for (size_t i = 0u; i < len; ++i) {
        unsigned char c = (unsigned char)text[i];
        size_t field = 0u;

        if (c < 0x20u || c == 0x7fu) goto invalid;
        if (c == '\\') {
            if (++i >= len || (text[i] != '\\' && text[i] != '{' && text[i] != '}')) goto invalid;
            if (out && snag_buf_putc(out, (unsigned char)text[i]) < 0) return -1;
        } else if (c == '{') {
            size_t name = 3u;
            if (seen)
                for (name = 0u; name < 3u; ++name)
                    if (strncmp(text + i + 1u, names[name], strlen(names[name])) == 0) break;
            bool queued = len - i >= 8u && memcmp(text + i, "{queued:", 8u) == 0;
            if (name < 3u || queued) {
                size_t start = i + 1u + (queued ? 7u : strlen(names[name]));
                size_t end = prompt_end(text, len, start);
                bool display = out && (queued ? strcmp(values[SNAG_PROMPT_QUEUE], "0") != 0 :
                                      name == selected);
                if (!end || end == start + 1u || (name < 3u && (*seen & (1u << name))) ||
                    prompt_body(text + start, end - start - 1u, values, marker, spinners,
                                queued ? modes : 1u << name, selected, NULL, display ? out : NULL) < 0)
                    goto invalid;
                if (name < 3u) *seen |= 1u << name;
                i = end - 1u;
                continue;
            }
            const char *end = memchr(text + i + 1u, '}', len - i - 1u);
            size_t field_len = end ? (size_t)(end - text - i - 1u) : 0u;
            const char *format = memchr(text + i + 1u, ':', field_len);
            size_t width = 0u;
            unsigned char fill = ' ';

            if (format) field_len = (size_t)(format - text - i - 1u);

            while (field < sizeof(fields) / sizeof(fields[0]) && (strlen(fields[field]) != field_len ||
                    memcmp(fields[field], text + i + 1u, field_len) != 0)) ++field;
            if (!end || field == sizeof(fields) / sizeof(fields[0])) goto invalid;
            if (format) {
                bool clock = field >= SNAG_PROMPT_HOUR && field <= SNAG_PROMPT_SECOND;

                if (field != SNAG_PROMPT_CONTEXT && field != SNAG_PROMPT_QUEUE && !clock) goto invalid;
                ++format;
                if (format < end && *format == '0') {
                    if (!clock) goto invalid;
                    fill = '0';
                    ++format;
                }
                if (format == end || *format < '1' || *format > '9') goto invalid;
                for (; format < end; ++format) {
                    if (*format < '0' || *format > '9' || width > (SNAG_TERM_LABEL_BYTES - 2u -
                                 (unsigned int)(*format - '0')) / 10u) goto invalid;
                    width = width * 10u + (unsigned int)(*format - '0');
                }
            }
            if (field >= SNAG_PROMPT_FIELD_COUNT) {
                unsigned int bit = 1u << (field - SNAG_PROMPT_FIELD_COUNT);
                for (unsigned int mode = 0u; mode < 3u; ++mode) {
                    if (!(modes & (1u << mode))) continue;
                    if (spinners[mode] & bit) goto invalid;
                    spinners[mode] |= bit;
                }
                if (out && snag_buf_putc(out, marker + field - SNAG_PROMPT_FIELD_COUNT) < 0) goto invalid;
            } else if (out) {
                size_t value_len = strlen(values[field]);

                if (values[field][0] == '-') fill = ' ';
                for (size_t pad = value_len; pad < width; ++pad)
                    if (snag_buf_putc(out, fill) < 0) goto invalid;
                if (snag_buf_append(out, values[field], value_len) < 0) goto invalid;
            }
            i = (size_t)(end - text);
        } else if (c == '}') {
            goto invalid;
        } else if (out && snag_buf_putc(out, c) < 0) {
            return -1;
        }
    }
    if (!seen || *seen == 7u) return 0;
invalid: return snag_errno(EINVAL);
}

static int
parse_prompt(const char *text, unsigned int selected, const char *const values[SNAG_PROMPT_FIELD_COUNT],
             unsigned char marker, struct snag_buf *out)
{
    unsigned int seen = 0u, spinners[3] = {0u};
    return prompt_body(text, strlen(text), values, marker, spinners, 7u, selected, &seen, out);
}

int
snag_config_prompt_expand(const char *text, unsigned int mode,
                         const char *const values[SNAG_PROMPT_FIELD_COUNT], unsigned char marker,
                         char *label, size_t label_size)
{
    int rc = -1;

    if (!text || mode >= 3u || !values || !label || label_size < 2u || marker > 0xfeu)
        return snag_errno(EINVAL);
    struct snag_buf out = {.max = label_size};
    if (parse_prompt(text, mode, values, marker, &out) < 0 || !out.len ||
        snag_buf_putc(&out, ' ') < 0 || snag_buf_terminate(&out) < 0) goto out;
    memcpy(label, out.data, out.len + 1u);
    rc = 0;
out: snag_buf_free(&out);
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
    else goto invalid;
    while (len > scheme_len && value[len - 1u] == '/') --len;
    if (len >= size) goto invalid;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x21u || c > 0x7eu || c == '?' || c == '#') goto invalid;
    }
    host = value + scheme_len;
    if (host >= value + len || *host == '/') goto invalid;
    path = memchr(host, '/', (size_t)(value + len - host));
    if (path == host) goto invalid;
    memcpy(dst, value, len);
    dst[len] = '\0';
    return 0;
invalid: return snag_errno(EINVAL);
}

static int
set_provider_section(struct parse_state *state, const char *name)
{
    struct snag_config *config = state->config;

    if (!snag_config_name_valid(name)) goto invalid;
    for (size_t i = 0; i < config->provider_count; ++i)
        if (strcmp(config->providers[i].name, name) == 0) goto invalid;
    if (config->provider_count >= SNAG_CONFIG_PROVIDER_MAX) goto invalid;
    state->provider_index = config->provider_count++;
    snag_config_provider_init(&config->providers[state->provider_index], name);
    state->section = SECTION_PROVIDER;
    return 0;
invalid: return snag_errno(EINVAL);
}

bool
snag_config_name_valid(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;

    if (!name || !*p || strlen(name) > SNAG_CONFIG_PROVIDER_NAME_MAX) return false;
    for (; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '_' || *p == '-')) return false;
    return true;
}

static int
set_model_limit_section(struct parse_state *state, char *name)
{
    struct snag_config *config = state->config;
    struct snag_model_limit_config *limit;
    char *slash = strchr(name, '/');
    const char *model = slash ? slash + 1u : "";

    if ((slash && (slash == name || !slash[1])) || !snag_text_valid(model, 0u, SNAG_CONFIG_MODEL_MAX - 1u) ||
        config->model_limit_count >= SNAG_CONFIG_MODEL_LIMIT_MAX) goto invalid;
    if (slash) *slash = '\0';
    if (!snag_config_name_valid(name)) goto invalid;
    for (size_t i = 0; i < config->model_limit_count; ++i)
        if (strcmp(config->model_limits[i].provider, name) == 0 &&
            strcmp(config->model_limits[i].model, model) == 0) goto invalid;
    state->model_limit_index = config->model_limit_count++;
    limit = &config->model_limits[state->model_limit_index];
    memset(limit, 0, sizeof(*limit));
    (void)snprintf(limit->provider, sizeof(limit->provider), "%s", name);
    (void)snprintf(limit->model, sizeof(limit->model), "%s", model);
    state->section = SECTION_MODEL_LIMIT;
    return 0;
invalid: return snag_errno(EINVAL);
}

static int
set_model_alias_section(struct parse_state *state, char *name)
{
    char *slash = strchr(name, '/');
    size_t index;

    if (!slash || state->model_count >= SNAG_CONFIG_MODEL_ALIAS_MAX) goto invalid;
    *slash = '\0';
    if (!snag_config_name_valid(name) || !snag_config_name_valid(slash + 1u)) goto invalid;
    for (size_t i = 0; i < state->model_count; ++i)
        if (strcmp(state->models[i].provider, name) == 0 &&
            strcmp(state->models[i].model.name, slash + 1u) == 0) goto invalid;
    index = state->model_alias_index = state->model_count++;
    (void)snag_strcpy(state->models[index].provider, sizeof(state->models[index].provider), name);
    (void)snag_strcpy(state->models[index].model.name, sizeof(state->models[index].model.name), slash + 1u);
    state->section = SECTION_MODEL_ALIAS;
    return 0;
invalid: return snag_errno(EINVAL);
}

static int
set_rule_section(struct parse_state *state, const char *name)
{
    json_t *rule;

    if (!snag_config_name_valid(name)) goto invalid;
    for (size_t i = 0u; state->rules && i < json_array_size(state->rules); ++i) {
        const char *other = snag_json_string(json_array_get(state->rules, i), "name");
        if (other && strcmp(other, name) == 0) goto invalid;
    }
    if (!state->rules) {
        state->rules = json_array();
        if (!state->rules) return -1;
    }
    if (json_array_size(state->rules) >= SNAG_RULES_MAX) goto invalid;
    rule = json_object();
    if (!rule) return -1;
    if (json_object_set_new(rule, "name", json_string(name)) < 0 ||
        json_array_append_new(state->rules, rule) < 0) return -1;
    state->rule_index = json_array_size(state->rules) - 1u;
    state->section = SECTION_RULE;
    return 0;
invalid: return snag_errno(EINVAL);
}

static int
set_section(struct parse_state *state, char *name)
{
    enum section section;
    if (strcmp(name, "agent") == 0) section = SECTION_AGENT;
    else if (strncmp(name, "provider ", 9u) == 0) return set_provider_section(state, trim(name + 9u));
    else if (strncmp(name, "model-limit ", 12u) == 0) return set_model_limit_section(state, trim(name + 12u));
    else if (strncmp(name, "model-alias ", 12u) == 0) return set_model_alias_section(state, trim(name + 12u));
    else if (strncmp(name, "rule ", 5u) == 0) return set_rule_section(state, trim(name + 5u));
    else if (strcmp(name, "ui") == 0) section = SECTION_UI;
    else if (strcmp(name, "irc") == 0) section = SECTION_IRC;
    else if (strcmp(name, "tool") == 0) section = SECTION_TOOL;
    else return snag_errno(EINVAL);
    if (state->seen_sections & (1u << section)) return snag_errno(EINVAL);
    state->seen_sections |= 1u << section;
    state->section = section;
    return 0;
}

static int
claim_key(struct parse_state *state, const char *key)
{
    /* Keys borrow the parsed file until parse_file returns. IRC clients repeat. */
    if ((state->section == SECTION_IRC && strcmp(key, "client") == 0) ||
        (state->section == SECTION_TOOL && strcmp(key, "secret") == 0)) return 0;
    for (size_t i = 0; i < sizeof(state->seen_keys) / sizeof(state->seen_keys[0]); ++i) {
        if (!state->seen_keys[i]) {
            state->seen_keys[i] = key;
            return 0;
        }
        if (strcmp(state->seen_keys[i], key) == 0) break;
    }
    return snag_errno(EINVAL);
}

enum setting_kind {
    SET_TEXT, SET_HEADER, SET_HTTPS, SET_U32, SET_U64, SET_BOOL, SET_SPINNER };

static int
parse_setting(struct parse_state *state, const char *key, const char *value)
{
    struct snag_config *config = state->config;
    struct snag_provider_config *provider = &config->providers[state->provider_index];
    struct snag_model_limit_config *limit = &config->model_limits[state->model_limit_index];
    struct snag_irc_config *irc = &config->irc;
    const struct {
        enum section section;
        const char *key;
        enum setting_kind kind;
        void *target;
        uint64_t min, max;
    } settings[] = {
        {SECTION_AGENT, "auto_update", SET_BOOL, &config->auto_update, 0, 0},
        {SECTION_AGENT, "update_url", SET_HTTPS, config->update_url, 0, sizeof(config->update_url)},
        {SECTION_AGENT, "provider", SET_TEXT, config->provider, 0, sizeof(config->provider)},
        {SECTION_AGENT, "model", SET_TEXT, config->model, 0, sizeof(config->model)},
        {SECTION_AGENT, "reasoning_effort", SET_TEXT, config->reasoning_effort, 0, sizeof(config->reasoning_effort)},
        {SECTION_AGENT, "max_goal_prompt_bytes", SET_U32, &config->max_goal_prompt_bytes, 1, 1024u * 1024u},
        {SECTION_AGENT, "read_agents_md", SET_BOOL, &config->read_agents_md, 0, 0},
        {SECTION_AGENT, "max_turn_retries", SET_U32, &config->max_turn_retries, 0, UINT32_MAX},
        {SECTION_PROVIDER, "parallel_tool_calls", SET_BOOL, &provider->parallel_tool_calls, 0, 0},
        {SECTION_PROVIDER, "connect_timeout_ms", SET_U32, &provider->connect_timeout_ms, 1000, 120000},
        {SECTION_PROVIDER, "idle_timeout_ms", SET_U32, &provider->idle_timeout_ms, 1000, 600000},
        {SECTION_PROVIDER, "request_timeout_ms", SET_U32, &provider->request_timeout_ms, 1000, 3600000},
        {SECTION_PROVIDER, "native_compaction", SET_BOOL, &provider->native_compaction, 0, 0},
        {SECTION_PROVIDER, "openrouter_referer", SET_HEADER, provider->openrouter_referer, 0, sizeof(provider->openrouter_referer)},
        {SECTION_PROVIDER, "openrouter_title", SET_HEADER, provider->openrouter_title, 0, sizeof(provider->openrouter_title)},
        {SECTION_MODEL_LIMIT, "context_window_tokens", SET_U64, &limit->context_window_tokens, 1, SNAG_CONFIG_TOKEN_LIMIT_MAX},
        {SECTION_MODEL_LIMIT, "max_input_tokens", SET_U64, &limit->max_input_tokens, 1, SNAG_CONFIG_TOKEN_LIMIT_MAX},
        {SECTION_MODEL_LIMIT, "max_output_tokens", SET_U64, &limit->max_output_tokens, 1, SNAG_CONFIG_TOKEN_LIMIT_MAX},
        {SECTION_MODEL_ALIAS, "model", SET_HEADER, state->models[state->model_alias_index].model.upstream, 0, SNAG_CONFIG_MODEL_MAX},
        {SECTION_UI, "typing_pause_ms", SET_U32, &config->typing_pause_ms, 0, 5000},
        {SECTION_UI, "markdown", SET_BOOL, &config->markdown, 0, 0},
        {SECTION_UI, "prompt_spinner_goal", SET_SPINNER, config->prompt_spinner_goal, 0, 0},
        {SECTION_UI, "prompt_spinner_provider", SET_SPINNER, config->prompt_spinner_provider, 0, 0},
        {SECTION_UI, "prompt_spinner_tool", SET_SPINNER, config->prompt_spinner_tool, 0, 0},
        {SECTION_UI, "prompt_tool_spinner_off_delay_ms", SET_U32, &config->prompt_tool_spinner_off_delay_ms, 0, 60000},
        {SECTION_UI, "prompt_spinner_per_second", SET_U32, &config->prompt_spinner_per_second, 1, 60},
        {SECTION_IRC, "room_name", SET_TEXT, irc->room_name, 0, sizeof(irc->room_name)},
        {SECTION_IRC, "history_lines", SET_U32, &irc->history_lines, 1, 1000},
        {SECTION_TOOL, "default_yield_ms", SET_U32, &config->default_yield_ms, 0, 600000},
        {SECTION_TOOL, "max_wait_ms", SET_U32, &config->max_wait_ms, 1, UINT32_MAX},
        {SECTION_TOOL, "max_parallel_commands", SET_U32, &config->max_parallel_commands, 1, 32},
        {SECTION_TOOL, "default_timeout_ms", SET_U32, &config->default_timeout_ms, 0, UINT32_MAX},
        {SECTION_TOOL, "max_timeout_ms", SET_U32, &config->max_timeout_ms, 1, UINT32_MAX},
        {SECTION_TOOL, "max_output_bytes", SET_U32, &config->max_output_bytes, 0, UINT32_MAX},
        {SECTION_TOOL, "max_output_tokens", SET_U32, &config->max_output_tokens, 1, SNAG_CONFIG_TOKEN_LIMIT_MAX}
    };

    if (state->section == SECTION_NONE || !*key || claim_key(state, key) < 0) goto invalid;
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        if (settings[i].section != state->section || strcmp(key, settings[i].key)) continue;
        void *target = settings[i].target;
        uint64_t min = settings[i].min, max = settings[i].max;
        switch (settings[i].kind) {
        case SET_HTTPS:
            if (strncmp(value, "https://", 8u) != 0 || !value[8]) goto invalid;
            return copy_value(target, (size_t)max, value);
        case SET_TEXT: return copy_value(target, (size_t)max, value);
        case SET_HEADER: return copy_header_value(target, (size_t)max, value);
        case SET_U32: return parse_u32(value, (uint32_t)min, (uint32_t)max, target);
        case SET_U64: return parse_u64(value, min, max, target);
        case SET_BOOL: return parse_bool(value, target);
        case SET_SPINNER: return parse_spinner(target, value);
        }
    }
    switch (state->section) {
    case SECTION_PROVIDER:
        if (!strcmp(key, "auth")) {
            if (!strcmp(value, "api_key")) provider->auth = SNAG_AUTH_API_KEY;
            else if (!strcmp(value, "chatgpt")) provider->auth = SNAG_AUTH_CHATGPT;
            else goto invalid;
            return 0;
        }
        if (!strcmp(key, "auto_compact_input_tokens")) {
            if (!strcmp(value, "auto")) {
                provider->auto_compact_input_tokens = SNAG_CONFIG_COMPACT_AUTO;
                return 0;
            }
            return parse_u32(value, 0, 4000000u, &provider->auto_compact_input_tokens);
        }
        if (!strcmp(key, "base_url"))
            return copy_base_url(provider->base_url, sizeof(provider->base_url), value);
        if (!strcmp(key, "api_key"))
            return snag_secret_source_parse(&provider->api_key, value, config->source_path, NULL, 0);
        if (!strcmp(key, "exact_token_count")) return parse_token_count(value, &provider->exact_token_count);
        break;
    case SECTION_UI:
        if (!strcmp(key, "color")) {
            const char *const names[] = {"auto", "always", "never"};
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
                if (!strcmp(value, names[i])) {
                    config->color = (enum snag_color_mode)i;
                    return 0;
                }
        }
        if (!strcmp(key, "resume_history_turns"))
            return snag_parse_count(value, &config->resume_history_turns);
        if (!strcmp(key, "prompt")) return copy_value(config->prompt, sizeof(config->prompt), value) < 0 ?
                -1 : parse_prompt(config->prompt, 3u, NULL, 0u, NULL);
        break;
    case SECTION_IRC:
        if (!strcmp(key, "client")) {
            if (irc->client_count >= SNAG_CONFIG_IRC_CLIENT_MAX) goto invalid;
            for (size_t i = 0; i < irc->client_count; ++i)
                if (!strcmp(irc->clients[i], value)) goto invalid;
            if (copy_value(irc->clients[irc->client_count], sizeof(irc->clients[0]), value) < 0) return -1;
            ++irc->client_count;
            return 0;
        }
        if (!strcmp(key, "listen")) {
            if (copy_value(irc->listen, sizeof(irc->listen), value) < 0) return -1;
            irc->listen_explicit = true;
            return 0;
        }
        if (!strcmp(key, "model_nick") || !strcmp(key, "operator_nick")) {
            bool model = !strcmp(key, "model_nick");
            if (copy_value(model ? irc->model_nick : irc->operator_nick, sizeof(irc->model_nick), value) < 0)
                return -1;
            if (model) irc->model_nick_implicit = false;
            else irc->operator_nick_implicit = false;
            return 0;
        }
        break;
    case SECTION_TOOL:
        if (!strcmp(key, "shell")) {
            if (!snag_path_root_len(value)) goto invalid;
            char *copy = snag_strdup_checked(value, SNAG_CONFIG_PATH_MAX);
            if (!copy) return -1;
            free(config->shell);
            config->shell = copy;
            return 0;
        }
        if (!strcmp(key, "secret")) {
            if (config->secret_count >= SNAG_CONFIG_SECRET_MAX ||
                snag_secret_source_parse(&config->secrets[config->secret_count], value,
                                           config->source_path, NULL, 0) < 0) goto invalid;
            ++config->secret_count;
            return 0;
        }
        break;
    case SECTION_RULE: {
        json_t *rule = state->rules ? json_array_get(state->rules, state->rule_index) : NULL;
        json_t *text;
        if (!rule) goto invalid;
        if (!strcmp(key, "chain") || !strcmp(key, "action") || !strcmp(key, "text") ||
            !strcmp(key, "target") || !strcmp(key, "log") || !strcmp(key, "to") || !strcmp(key, "command")) {
            /* Only the model tool-call boundary is evaluated in this build;
             * in/event hosts are not wired, so refuse them instead of accepting
             * rules that could never fire. */
            if (!strcmp(key, "chain") && (!strcmp(value, "in") || !strcmp(value, "event"))) goto invalid;
            /* Only insert-to-model is wired; program and IRC hosts do not exist
             * at this boundary yet. */
            if (!strcmp(key, "to") && (!strcmp(value, "program") || !strcmp(value, "irc"))) goto invalid;
            /* Rule messages may be JSON-quoted strings so escapes and newlines
             * survive; chain/action/target stay plain identifiers. */
            if ((!strcmp(key, "text") || !strcmp(key, "log")) && value[0] == '"') {
                char message_error[128];
                text = snag_json_load_strict((const unsigned char *)value, strlen(value),
                                             SNAG_MAX_EVENT_LINE, message_error, sizeof(message_error));
                if (!text || !json_is_string(text)) {
                    json_decref(text);
                    goto invalid;
                }
            } else {
                text = json_string(value);
            }
            if (!text || json_object_set_new(rule, key, text) < 0) return -1;
            return 0;
        }
        if (!strcmp(key, "timeout_ms")) {
            uint32_t ms;
            if (parse_u32(value, 1u, 60000u, &ms) < 0) goto invalid;
            return json_object_set_new(rule, key, json_integer((json_int_t)ms)) < 0 ? -1 : 0;
        }
        if (!strcmp(key, "match") || !strcmp(key, "at_least") || !strcmp(key, "value")) {
            char json_error[128];
            json_t *parsed = snag_json_load_strict((const unsigned char *)value,
                strlen(value), SNAG_MAX_EVENT_LINE, json_error, sizeof(json_error));
            bool value_key = !strcmp(key, "value");
            if (!parsed || !(json_is_object(parsed) || (value_key && json_is_string(parsed))) ||
                (json_is_object(parsed) && json_object_size(parsed) == 0u)) {
                json_decref(parsed);
                goto invalid;
            }
            return json_object_set_new(rule, key, parsed) < 0 ? -1 : 0;
        }
        break;
    }
    default: break;
    }
invalid: return snag_errno(EINVAL);
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
    memset(config->providers, 0, sizeof(config->providers));
    config->provider_count = 0u;
    for (;;) {
        char *next = strchr(line, '\n');
        char *clean;
        if (next) *next = '\0';
        clean = trim(line);
        if (*clean && *clean != '#' && *clean != ';') {
            size_t len = strlen(clean);
            int rc;
            if (clean[0] == '[') {
                if (len < 3u || clean[len - 1u] != ']') rc = -1;
                else {
                    clean[len - 1u] = '\0';
                    rc = set_section(&state, clean + 1u);
                    /* Every section is unique; only its current keys can repeat. */
                    memset(state.seen_keys, 0, sizeof(state.seen_keys));
                }
            } else {
                char *equal = strchr(clean, '=');
                rc = -1;
                if (equal) {
                    *equal = '\0';
                    rc = parse_setting(&state, trim(clean), trim(equal + 1u));
                } else errno = EINVAL;
            }
            if (rc < 0) {
                snag_errorf(error, error_size, "invalid configuration at line %u; use named [provider NAME], "
                          "api_key with ${ENV}, quoted literal or path, and repeatable tool secret", number);
                json_decref(state.rules);
                return -1;
            }
        }
        if (!next) break;
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
            json_decref(state.rules);
            return snag_errorf(error, error_size, "model-alias %s/%s needs a configured provider and one real upstream target",
                       state.models[i].provider, state.models[i].model.name);
        }
        models = realloc(provider->models, (provider->model_count + 1u) * sizeof(*models));
        if (!models) {
            json_decref(state.rules);
            return -1;
        }
        provider->models = models;
        provider->models[provider->model_count++] = state.models[i].model;
    }
    {
        json_t *definition = json_object();
        if (!definition) {
            json_decref(state.rules);
            return -1;
        }
        if (state.rules) {
            if (json_object_set_new(definition, "rules", state.rules) < 0) {
                json_decref(definition);
                return -1;
            }
            state.rules = NULL;
        }
        snag_rules_free(config->rules);
        config->rules = snag_rules_compile(definition, error, error_size);
        json_decref(definition);
        if (!config->rules) return -1;
    }
    return 0;
}

char *
snag_config_path(const char *explicit_path, const char *dotdir, char *error, size_t error_size)
{
    struct snag_buf path;
    char *result = NULL;

    if (explicit_path) {
        if (!snag_path_root_len(explicit_path) || strlen(explicit_path) > SNAG_CONFIG_PATH_MAX) {
            (void)snag_fail(error, error_size, EINVAL,
                "--config requires an absolute path within the supported limit");
            return NULL;
        }
        result = snag_strdup_checked(explicit_path, SNAG_CONFIG_PATH_MAX);
        if (result) snag_path_slashes(result);
        if (!result) snag_errorf(error, error_size, "configuration path is unavailable");
        return result;
    }
    snag_buf_init(&path, SNAG_CONFIG_PATH_MAX);
    if (!snag_path_root_len(dotdir)) goto invalid;
    if (snag_buf_printf(&path, "%s/config.ini", dotdir) < 0) goto unavailable;
    if (snag_buf_terminate(&path) < 0) goto unavailable;
    result = (char *)path.data;
    path.data = NULL;
    snag_buf_free(&path);
    return result;
invalid: (void)snag_fail(error, error_size, EINVAL, "configuration requires an absolute dotdir");
    snag_buf_free(&path);
    return NULL;
unavailable: snag_errorf(error, error_size, "configuration path exceeds the supported limit");
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
    if (fd < 0 && !permissions && errno == EACCES) fd = snag_open_read(path, false);
    if (fd < 0) {
        if (!require_file && errno == ENOENT) return 1;
        return snag_errorf(error, error_size, "cannot open configuration %s: %s", path, strerror(errno));
    }
    if (snag_fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uintmax_t)st.st_size > SNAG_CONFIG_FILE_MAX) {
        (void)snag_fail(error, error_size, EINVAL,
            "configuration must be a regular file no larger than 64 KiB");
        goto out;
    }
    if (file_stat) *file_stat = st;
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
    int read_rc = snag_buf_read(text, fd);
    if (read_rc < 0) {
        snag_errorf(error, error_size, read_rc == -2 ? "configuration exceeds 64 KiB" :
                    "cannot read configuration: %s", strerror(errno));
        goto out;
    }
    if (!snag_utf8_valid(text->data, text->len, true)) {
        (void)snag_fail(error, error_size, EILSEQ, "configuration must be valid UTF-8 without NUL bytes");
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
    if (!snag_path_root_len(config->shell)) goto invalid;
    resolved = snag_realpath(config->shell);
    if (!resolved) goto invalid;
    if (strlen(resolved) > SNAG_CONFIG_PATH_MAX || snag_stat(resolved, &st) < 0 ||
        !S_ISREG(st.st_mode) || snag_file_executable(resolved) < 0) {
        free(resolved);
        goto invalid;
    }
    /* Preserve symlink-selected shell personalities (for example BusyBox sh). */
    free(resolved);
    return 0;
invalid: return snag_fail(error, error_size, EINVAL,
              "configured shell must resolve to an executable regular file");
}

static bool
config_has_literals(const struct snag_config *config)
{
    for (size_t i = 0; i < config->provider_count; ++i)
        if (config->providers[i].api_key.kind == SNAG_SECRET_LITERAL) return true;
    for (size_t i = 0; i < config->secret_count; ++i)
        if (config->secrets[i].kind == SNAG_SECRET_LITERAL) return true;
    return false;
}

static int
validate_config(struct snag_config *config, bool private_file, char *error, size_t error_size)
{
    if (config_has_literals(config) && !private_file)
        return snag_fail(error, error_size, EACCES, "literal secrets require an owner-private configuration file (0600)");
    for (size_t i = 0; i < config->provider_count; ++i) {
        if (config->providers[i].auth == SNAG_AUTH_CHATGPT &&
            (strcmp(config->providers[i].base_url, SNAG_CHATGPT_BASE) != 0 ||
             config->providers[i].api_key.kind != SNAG_SECRET_NONE)) {
            return snag_fail(error, error_size, EINVAL,
                       "chatgpt authentication requires " SNAG_CHATGPT_BASE " and no api_key");
        }
    }
    for (size_t i = 0; i < config->model_limit_count; ++i) {
        const struct snag_model_limit_config *limit = &config->model_limits[i];
        if (!snag_config_provider(config, limit->provider) ||
            (!limit->context_window_tokens && !limit->max_input_tokens && !limit->max_output_tokens) ||
            (limit->context_window_tokens && limit->max_output_tokens &&
             limit->max_output_tokens >= limit->context_window_tokens)) {
            return snag_fail(error, error_size, EINVAL, "invalid model-limit section for %s/%s",
                      limit->provider, limit->model);
        }
    }
    if (config->default_timeout_ms > config->max_timeout_ms)
        return snag_fail(error, error_size, EINVAL, "tool default_timeout_ms cannot exceed max_timeout_ms");
    if (validate_shell(config, error, error_size) < 0) return -1;
    if (config->provider[0] && !snag_config_provider(config, config->provider))
        return snag_fail(error, error_size, EINVAL, "configured agent provider is not defined");
    return 0;
}

int
snag_config_load(struct snag_config *config, const char *explicit_path, const char *dotdir,
                char *error, size_t error_size)
{
    char *owned_path = NULL;
    const char *path = explicit_path;
    snag_file_info file_stat;
    int read_rc;
    int rc = -1;

    if (!config->shell)
        return snag_fail(error, error_size, ENOMEM, "cannot initialize configuration defaults");
    owned_path = snag_config_path(explicit_path, dotdir, error, error_size);
    if (!owned_path) return -1;
    path = owned_path;
    (void)snag_strcpy(config->source_path, sizeof(config->source_path), path);
    struct snag_buf text = {.max = SNAG_CONFIG_FILE_MAX + 1u};
    bool private_file = false;
    read_rc = read_config(path, explicit_path != NULL, &text, &file_stat, &private_file, NULL,
                          error, error_size);
    if (read_rc < 0) goto out;
    if (read_rc == 0 && parse_file(config, (char *)text.data, error, error_size) < 0) goto out;
    rc = validate_config(config, read_rc != 0 || private_file, error, error_size);
out: free(owned_path);
    snag_secret_clear(text.data, text.len);
    snag_buf_free(&text);
    return rc;
}

static bool
same_file(const snag_file_info *left, const snag_file_info *right)
{
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
           left->st_size == right->st_size && left->st_mtime == right->st_mtime &&
           left->st_mode == right->st_mode;
}

static int
validate_config_text(const struct snag_buf *text, const char *path, bool private_file,
                     char *error, size_t error_size)
{
    struct snag_config candidate;
    char *copy;
    int rc = -1;

    copy = malloc(text->len + 1u);
    if (!copy) return -1;
    memcpy(copy, text->data, text->len);
    copy[text->len] = '\0';
    snag_config_init(&candidate);
    (void)snag_strcpy(candidate.source_path, sizeof(candidate.source_path), path);
    if (!candidate.shell) {
        snag_errorf(error, error_size, "cannot initialize configuration defaults");
        goto out;
    }
    if (parse_file(&candidate, copy, error, error_size) < 0) goto out;
    rc = validate_config(&candidate, private_file, error, error_size);
out: snag_config_free(&candidate);
    snag_secret_clear(copy, text->len);
    free(copy);
    return rc;
}

static int
provider_settings(struct snag_buf *output, const struct snag_provider_config *p)
{
    const char *auth = p->auth == SNAG_AUTH_CHATGPT ? "chatgpt" : "api_key";
    if (snag_buf_printf(output, "auth = %s\nbase_url = %s\nnative_compaction = %s\nparallel_tool_calls = %s\n",
                        auth, p->base_url, p->native_compaction ? "true" : "false",
                        p->parallel_tool_calls ? "true" : "false") < 0) return -1;
    return p->api_key.kind == SNAG_SECRET_NONE ? 0 :
        snag_buf_printf(output, "api_key = %s\n", p->api_key.expression);
}

static const char *const model_setting_keys[] = {"provider", "model", "reasoning_effort"};

static int
append_missing_model_settings(struct snag_buf *out, const bool seen[3], const char *const values[3])
{
    if (out->len && out->data[out->len - 1u] != '\n' && snag_buf_putc(out, '\n') < 0) return -1;
    for (size_t i = 0u; i < 3u; ++i)
        if (!seen[i] && snag_buf_printf(out, "%s = %s\n", model_setting_keys[i], values[i]) < 0) return -1;
    return 0;
}

/* Walk original bytes once per edit. Provider blocks are replaced at their
 * heading; model assignments retain their individual line endings. */
static int
replace_settings(const struct snag_buf *input, struct snag_buf *output,
                 const struct snag_provider_config *provider, const char *const values[3])
{
    size_t at = 0u;
    bool selected = false, found = false, seen[3] = {false, false, false};

    while (at < input->len) {
        const unsigned char *line = input->data + at;
        const unsigned char *newline = memchr(line, '\n', input->len - at);
        size_t content = newline ? (size_t)(newline - line) : input->len - at;
        size_t len = content + (newline != NULL);
        char copy[SNAG_CONFIG_FILE_MAX + 1u];
        memcpy(copy, line, content);
        copy[content] = '\0';
        char *s = trim(copy);
        bool replaced = false;

        if (provider && *s == '[') {
            char *close = strchr(s, ']');
            selected = false;
            if (close) {
                *close = '\0';
                s = trim(s + 1);
                selected = strncmp(s, "provider ", 9u) == 0 && strcmp(trim(s + 9), provider->name) == 0;
            }
            if (snag_buf_append(output, line, content) < 0 || snag_buf_putc(output, '\n') < 0 ||
                (selected && provider_settings(output, provider) < 0)) return -1;
            found |= selected;
            replaced = true;
        } else if (!provider && *s == '[' && strlen(s) > 1u && s[strlen(s) - 1u] == ']') {
            if (strcmp(s, "[agent]") == 0) {
                selected = found = true;
            } else if (selected) {
                if (append_missing_model_settings(output, seen, values) < 0) return -1;
                selected = false;
            }
        } else if (selected) {
            char *equal = strchr(s, '=');
            if (equal) {
                *equal = '\0';
                s = trim(s);
                if (provider) {
                    replaced = snag_string_in(s,
                        "auth base_url api_key native_compaction parallel_tool_calls");
                } else {
                    size_t ending = newline ? 1u + (content && line[content - 1u] == '\r') : 0u;
                    for (size_t i = 0u; i < 3u; ++i) {
                        if (strcmp(s, model_setting_keys[i]) != 0) continue;
                        if (snag_buf_printf(output, "%s = %s", s, values[i]) < 0 ||
                            snag_buf_append(output, line + len - ending, ending) < 0) return -1;
                        seen[i] = replaced = true;
                        break;
                    }
                }
            }
        }
        if (!replaced && snag_buf_append(output, line, len) < 0) return -1;
        at += len;
    }
    if (provider) {
        if (!found && (snag_buf_printf(output, "\n[provider %s]\n", provider->name) < 0 ||
                       provider_settings(output, provider) < 0)) return -1;
    } else {
        if (!found) {
            if ((output->len && output->data[output->len - 1u] != '\n' && snag_buf_putc(output, '\n') < 0) ||
                snag_buf_append(output, "[agent]\n", 8u) < 0) return -1;
        }
        if ((!found || selected) && append_missing_model_settings(output, seen, values) < 0) return -1;
    }
    return 0;
}

int
snag_config_validate_provider(const struct snag_provider_config *provider, char *error, size_t error_size)
{
    int rc = -1;
    if (!provider || !snag_config_name_valid(provider->name) || (provider->auth == SNAG_AUTH_CHATGPT &&
         (strcmp(provider->base_url, SNAG_CHATGPT_BASE) || provider->api_key.kind != SNAG_SECRET_NONE)))
        return snag_errorf(error, error_size, "invalid provider or ChatGPT endpoint");
    struct snag_buf text = {.max = SNAG_CONFIG_FILE_MAX};
    if (snag_buf_printf(&text, "[provider %s]\n", provider->name) == 0 &&
        provider_settings(&text, provider) == 0)
        rc = validate_config_text(&text, "/config.ini", true, error, error_size);
    snag_secret_clear(text.data, text.len);
    snag_buf_free(&text);
    return rc;
}

static int
save_config_settings(const char *path, bool allow_create, const char *provider, const char *model,
                      const char *effort, const struct snag_provider_config *provider_config,
                      char *error, size_t error_size)
{
    snag_file_info before;
    snag_file_info current;
    struct snag_permissions permissions = {0};
    struct snag_directory_lock directory_lock = {.fd = -1};
    char id[SNAG_ID_HEX_LEN + 1u];
    char temp[64] = {0};
    char leaf[SNAG_NAME_MAX_BYTES + 1u];
    char *path_copy = NULL;
    char *slash;
    int parent_fd = -1;
    int fd = -1;
    int read_rc;
    int rc = -1;
    int saved;

    memset(&before, 0, sizeof(before));
    struct snag_buf input = {.max = SNAG_CONFIG_FILE_MAX + 1u};
    struct snag_buf output = {.max = SNAG_CONFIG_FILE_MAX};
    if (!snag_path_root_len(path) || strlen(path) > SNAG_CONFIG_PATH_MAX ||
        !provider || !*provider || strlen(provider) > SNAG_CONFIG_PROVIDER_NAME_MAX ||
        !model || (!provider_config && !*model) || strlen(model) >= SNAG_CONFIG_MODEL_MAX ||
        !effort || !*effort || strlen(effort) >= SNAG_CONFIG_EFFORT_MAX ||
        strchr(provider, '\n') || strchr(provider, '\r') || strchr(model, '\n') || strchr(model, '\r') ||
        strchr(effort, '\n') || strchr(effort, '\r')) {
        (void)snag_fail(error, error_size, EINVAL, "refusing to save invalid model settings");
        goto out;
    }
    path_copy = snag_strdup_checked(path, SNAG_CONFIG_PATH_MAX);
    if (!path_copy) goto out;
    slash = strrchr(path_copy, '/');
    if (!slash || !slash[1]) {
        (void)snag_fail(error, error_size, EINVAL, "configuration path has no file name");
        goto out;
    }
    if (strlen(slash + 1u) > SNAG_NAME_MAX_BYTES) {
        (void)snag_fail(error, error_size, ENAMETOOLONG, "configuration file name is too long");
        goto out;
    }
    memcpy(leaf, slash + 1u, strlen(slash + 1u) + 1u);
    if ((size_t)(slash - path_copy) < snag_path_root_len(path_copy)) slash[1] = '\0';
    else *slash = '\0';
    parent_fd = snag_open_read(path_copy, true);
    if (parent_fd < 0) {
        snag_errorf(error, error_size, "cannot open configuration directory: %s", strerror(errno));
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
    if (read_rc < 0) goto out;
    const char *values[] = {provider, model, effort};
    if (replace_settings(&input, &output, provider_config, values) < 0) {
        snag_errorf(error, error_size, "configuration update exceeds 64 KiB");
        goto out;
    }
    if (provider_config && *model) {
        struct snag_buf selected = {.max = SNAG_CONFIG_FILE_MAX};
        if (replace_settings(&output, &selected, NULL, values) < 0) {
            snag_buf_free(&selected);
            goto out;
        }
        snag_buf_free(&output);
        output = selected;
    }
    if (validate_config_text(&output, path, read_rc != 0 || private_file, error, error_size) < 0) goto out;
    if (snag_random_id(id) < 0) goto out;
    (void)snprintf(temp, sizeof(temp), ".snajpagent-config-%s.tmp", id);
    fd = snag_create_private_at(parent_fd, temp, true);
    if (fd < 0 || (read_rc == 0 && snag_permissions_apply(fd, &permissions) < 0) ||
        snag_write_full(fd, output.data, output.len) < 0 || snag_sync_file(fd) < 0) {
        saved = errno;
        snag_errorf(error, error_size, "cannot write configuration: %s", strerror(saved));
        errno = saved;
        goto out;
    }
    if (close(fd) < 0) {
        fd = -1;
        snag_errorf(error, error_size, "cannot close configuration: %s", strerror(errno));
        goto out;
    }
    fd = -1;
    if (read_rc == 0) {
        int original = snag_open_read_security_at(parent_fd, leaf, false);
        bool unchanged = original >= 0 && snag_fstat(original, &current) == 0 &&
                         same_file(&before, &current) && snag_permissions_match(original, &permissions) == 1;
        if (original >= 0) (void)close(original);
        if (!unchanged) {
            (void)snag_fail(error, error_size, EAGAIN, "configuration changed while it was being saved");
            goto out;
        }
    } else if (snag_lstat_at(parent_fd, leaf, &current) == 0 || errno != ENOENT) {
        (void)snag_fail(error, error_size, EAGAIN, "configuration appeared while it was being saved");
        goto out;
    }
    if (snag_rename_at(parent_fd, temp, parent_fd, leaf) < 0 || snag_sync_dir(parent_fd) < 0) {
        saved = errno;
        snag_errorf(error, error_size, "cannot install configuration: %s", strerror(saved));
        errno = saved;
        goto out;
    }
    temp[0] = '\0';
    rc = 0;
out: saved = errno;
    if (fd >= 0) (void)close(fd);
    if (parent_fd >= 0) {
        if (temp[0]) (void)snag_unlink_at(parent_fd, temp, false);
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
    return save_config_settings(path, allow_create, provider, model, effort, NULL, error, error_size);
}

int
snag_config_save_provider(const char *path, bool allow_create, const struct snag_provider_config *provider,
                         const char *initial_model, const char *effort, char *error, size_t error_size)
{
    if (!provider || !snag_config_name_valid(provider->name) ||
        (provider->auth == SNAG_AUTH_CHATGPT && strcmp(provider->base_url, SNAG_CHATGPT_BASE)) ||
        strchr(provider->base_url, '\n') || strchr(provider->base_url, '\r'))
        return snag_errorf(error, error_size, "invalid provider settings");
    return save_config_settings(path, allow_create, provider->name,
        initial_model ? initial_model : "", effort ? effort : "default", provider, error, error_size);
}

const struct snag_provider_config *
snag_config_provider(const struct snag_config *config, const char *name)
{
    if (!config || config->provider_count == 0u) return NULL;
    if (!name) return &config->providers[0];
    for (size_t i = 0; i < config->provider_count; ++i)
        if (strcmp(config->providers[i].name, name) == 0) return &config->providers[i];
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
    else return false;
    if (strncasecmp(url, host, sizeof(host) - 1u) != 0) return false;
    suffix = url + sizeof(host) - 1u;
    if (*suffix == '.') ++suffix;
    if (*suffix == ':') {
        unsigned int port = 0u;

        ++suffix;
        if (*suffix < '0' || *suffix > '9') return false;
        while (*suffix >= '0' && *suffix <= '9') {
            port = port * 10u + (unsigned int)(*suffix++ - '0');
            if (port > 65535u) return false;
        }
        if (!port) return false;
    }
    return *suffix == '\0' || *suffix == '/';
}

const char *
snag_config_model_upstream(const struct snag_provider_config *provider, const char *model)
{
    if (provider && model)
        for (size_t i = 0; i < provider->model_count; ++i)
            if (strcmp(provider->models[i].name, model) == 0) return provider->models[i].upstream;
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
    while (*pattern == '*') ++pattern;
    return !*pattern;
}

bool
snag_config_resolve_limits(const struct snag_config *config, const char *provider, const char *name,
                          struct snag_model_limit_config *out,
                          const struct snag_model_limit_config *sources[3])
{
    memset(out, 0, sizeof(*out));
    if (sources) memset(sources, 0, 3u * sizeof(*sources));
    for (unsigned int tier = 0u; tier < 3u; ++tier) {
        for (size_t i = 0; i < config->model_limit_count; ++i) {
            const struct snag_model_limit_config *rule = &config->model_limits[i];
            bool wildcard = strchr(rule->model, '*') != NULL;
            if (strcmp(rule->provider, provider) != 0) continue;
            if (tier == 0u ? rule->model[0] != '\0' : (!rule->model[0] || wildcard != (tier == 1u) ||
                 !model_pattern_matches(rule->model, name))) continue;
            if (rule->context_window_tokens) {
                out->context_window_tokens = rule->context_window_tokens;
                if (sources) sources[0] = rule;
            }
            if (rule->max_input_tokens) {
                out->max_input_tokens = rule->max_input_tokens;
                if (sources) sources[1] = rule;
            }
            if (rule->max_output_tokens) {
                out->max_output_tokens = rule->max_output_tokens;
                if (sources) sources[2] = rule;
            }
        }
    }
    return out->context_window_tokens || out->max_input_tokens || out->max_output_tokens;
}
