/* SPDX-License-Identifier: GPL-2.0-only */
#include "rules.h"

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct snag_rule_predicate {
    char *pointer;
    regex_t regex;
};

struct snag_rule_threshold {
    char *pointer;
    json_int_t value;
};

struct snag_rule {
    char name[SNAG_RULE_NAME_MAX + 1u];
    char chain[SNAG_RULE_NAME_MAX + 1u];
    size_t chain_id;
    enum snag_rule_verb verb;
    char *text;
    char *target;
    char *log;
    json_t *value;
    char *to;
    char *command;
    unsigned int timeout_ms;
    struct snag_rule_predicate *predicates;
    size_t predicate_count;
    struct snag_rule_threshold *thresholds;
    size_t threshold_count;
};

struct snag_rules {
    char (*chains)[SNAG_RULE_NAME_MAX + 1u];
    size_t chain_count;
    struct snag_rule *rules;
    size_t rule_count;
    char digest[SNAG_SHA256_HEX_LEN + 1u];
};

static int
invalid(char *error, size_t size, const char *message)
{
    return snag_errorf(error, size, "%s", message);
}

static bool
name_valid(const char *name)
{
    if (!name || !*name || strlen(name) > SNAG_RULE_NAME_MAX) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_' || *p == '.')) return false;
    return true;
}

bool
snag_rules_boundary(const char *boundary)
{
    return boundary && (!strcmp(boundary, "in") || !strcmp(boundary, "out") || !strcmp(boundary, "event"));
}

static size_t
chain_index(const struct snag_rules *rules, const char *name)
{
    if (!name) return rules->chain_count;
    for (size_t i = 0u; i < rules->chain_count; ++i)
        if (!strcmp(rules->chains[i], name)) return i;
    return rules->chain_count;
}

static int
chain_add(struct snag_rules *rules, const char *name)
{
    if (!name_valid(name) || chain_index(rules, name) != rules->chain_count) return -1;
    if (rules->chain_count >= SNAG_CHAINS_MAX) return -1;
    (void)snprintf(rules->chains[rules->chain_count], SNAG_RULE_NAME_MAX + 1u, "%s", name);
    ++rules->chain_count;
    return 0;
}

/* JSON pointer subset: nonempty segments, ~0 for ~ and ~1 for /. */
static bool
pointer_valid(const char *p)
{
    if (!p || *p != '/') return false;
    for (const char *c = p + 1; *c; ++c) {
        if (*c == '~') {
            if (c[1] != '0' && c[1] != '1') return false;
            ++c;
        }
    }
    return true;
}

static const json_t *
pointer_get(const json_t *value, const char *p)
{
    const char *segment = p;
    while (value && *segment) {
        const char *start = segment + 1;
        const char *end = strchr(start, '/');
        size_t raw = end ? (size_t)(end - start) : strlen(start);
        char name[256];
        size_t n = 0u;
        for (size_t i = 0u; i < raw; ++i) {
            char c = start[i];
            if (c == '~' && i + 1u < raw) {
                ++i;
                c = start[i] == '0' ? '~' : '/';
            }
            if (n + 1u >= sizeof(name)) return NULL;
            name[n++] = c;
        }
        name[n] = '\0';
        if (json_is_array(value)) {
            char *end = NULL;
            unsigned long index = strtoul(name, &end, 10);
            if (!*name || (end && *end)) return NULL;
            value = json_array_get(value, (size_t)index);
        } else if (json_is_object(value)) {
            value = json_object_get(value, name);
        } else {
            return NULL;
        }
        if (!end) break;
        segment = end;
    }
    return value;
}

static int
append_text(struct snag_buf *out, const json_t *value)
{
    if (json_is_string(value)) return snag_buf_append(out, json_string_value(value),
                               strlen(json_string_value(value)));
    return snag_json_canonical(value, out);
}

int
snag_rule_render(const char *format, const json_t *envelope, struct snag_buf *out)
{
    if (!format || !out) return snag_errno(EINVAL);
    for (const char *c = format; *c; ++c) {
        if (*c != '%') {
            if (snag_buf_putc(out, (unsigned char)*c) < 0) return -1;
            continue;
        }
        if (c[1] == '%') {
            if (snag_buf_putc(out, '%') < 0) return -1;
            ++c;
            continue;
        }
        if (c[1] != '{') {
            if (snag_buf_putc(out, (unsigned char)'%') < 0) return -1;
            continue;
        }
        const char *end = strchr(c + 2, '}');
        if (!end) return snag_errno(EINVAL);
        char path[256];
        size_t len = (size_t)(end - (c + 2));
        if (len == 0u || len >= sizeof(path)) return snag_errno(EINVAL);
        memcpy(path, c + 2, len);
        path[len] = '\0';
        if (!pointer_valid(path)) return snag_errno(EINVAL);
        const json_t *value = pointer_get(envelope, path);
        if (value && append_text(out, value) < 0) return -1;
        c = end;
    }
    return 0;
}

static void
rule_free(struct snag_rule *rule)
{
    for (size_t i = 0u; i < rule->predicate_count; ++i) {
        regfree(&rule->predicates[i].regex);
        free(rule->predicates[i].pointer);
    }
    for (size_t i = 0u; i < rule->threshold_count; ++i) free(rule->thresholds[i].pointer);
    free(rule->predicates);
    free(rule->thresholds);
    free(rule->text);
    free(rule->target);
    free(rule->log);
    json_decref(rule->value);
    free(rule->to);
    free(rule->command);
    memset(rule, 0, sizeof(*rule));
}

static enum snag_rule_verb
verb_parse(const char *name)
{
    static const char *const names[] = {
        "pass", "accept", "reject", "jump", "return", "insert", "command", "confirm"
    };
    for (size_t i = 0u; i < SNAG_RULE_VERB_COUNT; ++i)
        if (!strcmp(name, names[i])) return (enum snag_rule_verb)i;
    return SNAG_RULE_VERB_COUNT;
}

static int
compile_rule(struct snag_rules *rules, json_t *definition, size_t index, char *error, size_t size)
{
    struct snag_rule *rule = &rules->rules[index];
    const char *name = snag_json_string(definition, "name");
    const char *chain = snag_json_string(definition, "chain");
    const char *action = snag_json_string(definition, "action");
    const char *text = snag_json_string(definition, "text");
    const char *target = snag_json_string(definition, "target");
    const char *log = snag_json_string(definition, "log");
    const char *to = snag_json_string(definition, "to");
    const char *command = snag_json_string(definition, "command");
    const json_t *timeout = json_object_get(definition, "timeout_ms");
    const json_t *match = json_object_get(definition, "match");
    const json_t *at_least = json_object_get(definition, "at_least");
    const json_t *value = json_object_get(definition, "value");

    if (!json_is_object(definition) || !name_valid(name) || !name_valid(chain) || !action || !*action)
        return invalid(error, size, "invalid rule identity");
    {
        for (void *it = json_object_iter(definition); it;
             it = json_object_iter_next(definition, it)) {
            const char *key = json_object_iter_key(it);
            static const char *const allowed[] = {
                "name", "chain", "match", "at_least", "action",
                "text", "target", "log", "value", "to",
                "command", "timeout_ms"
            };
            bool known = false;
            for (size_t i = 0u; i < sizeof(allowed) / sizeof(allowed[0]); ++i)
                if (!strcmp(key, allowed[i])) known = true;
            if (!known) return invalid(error, size, "unknown rule key");
        }
    }
    for (size_t i = 0u; i < index; ++i)
        if (strcmp(rules->rules[i].name, name) == 0) return invalid(error, size, "duplicate rule name");
    if (!json_is_object(match) && !json_is_null(match) && match)
        return invalid(error, size, "match must be an object");
    if (at_least && !json_is_object(at_least)) return invalid(error, size, "at_least must be an object");
    if (text && (!json_is_string(json_object_get(definition, "text")) || strlen(text) > SNAG_RULE_TEXT_MAX))
        return invalid(error, size, "text must be a bounded string");

    (void)snprintf(rule->name, sizeof(rule->name), "%s", name);
    if (chain_index(rules, chain) == rules->chain_count && chain_add(rules, chain) < 0)
        return invalid(error, size, "too many chains");
    rule->chain_id = chain_index(rules, chain);
    (void)snprintf(rule->chain, sizeof(rule->chain), "%s", chain);
    rule->verb = verb_parse(action);
    if (rule->verb == SNAG_RULE_VERB_COUNT &&
        snag_string_in(action, "compact transform"))
        return invalid(error, size, "rule action is defined by the design but not implemented in this build");
    if (rule->verb == SNAG_RULE_VERB_COUNT) return invalid(error, size, "unknown rule action");
    if (rule->verb == SNAG_RULE_JUMP && !name_valid(target))
        return invalid(error, size, "jump needs a target chain");
    if (rule->verb != SNAG_RULE_JUMP && target)
        return invalid(error, size, "target applies only to jump");
    if (rule->verb == SNAG_RULE_INSERT) {
        if (!text || !to || (strcmp(to, "program") && strcmp(to, "model") && strcmp(to, "irc")))
            return invalid(error, size, "insert needs to=program|model|irc and text");
    } else if (to) {
        return invalid(error, size, "to applies only to insert");
    }
    if (rule->verb == SNAG_RULE_COMMAND) {
        if (!command || !*command || strlen(command) > 4096u)
            return invalid(error, size, "command needs a bounded helper path");
        if (timeout && (!json_is_integer(timeout) || json_integer_value(timeout) < 1 ||
                        json_integer_value(timeout) > 60000))
            return invalid(error, size, "timeout_ms must be 1..60000");
    } else if (command || timeout) {
        return invalid(error, size, "command and timeout_ms apply only to command");
    }
    /* pass with value is the single transform/override operation. */
    if (value && rule->verb != SNAG_RULE_PASS)
        return invalid(error, size, "value applies only to pass");
    if (value) rule->value = json_incref((json_t *)value);
    if (text) {
        rule->text = snag_strdup_checked(text, SNAG_RULE_TEXT_MAX + 1u);
        if (!rule->text) return -1;
    }
    if (log) {
        if (strlen(log) > SNAG_RULE_TEXT_MAX) return invalid(error, size, "log format is too long");
        rule->log = snag_strdup_checked(log, SNAG_RULE_TEXT_MAX + 1u);
        if (!rule->log) return -1;
    }
    if (rule->verb == SNAG_RULE_JUMP) {
        rule->target = snag_strdup_checked(target, SNAG_RULE_NAME_MAX + 1u);
        if (!rule->target) return -1;
    }
    if (rule->verb == SNAG_RULE_INSERT) {
        rule->to = snag_strdup_checked(to, SNAG_RULE_NAME_MAX + 1u);
        if (!rule->to)
            return -1;
    }
    if (rule->verb == SNAG_RULE_COMMAND) {
        rule->command = snag_strdup_checked(command, 4097u);
        rule->timeout_ms = timeout ? (unsigned int)json_integer_value(timeout) : 10000u;
        if (!rule->command)
            return -1;
    }

    if (json_is_object(match) && json_object_size(match) > 0u) {
        rule->predicates = calloc(json_object_size(match), sizeof(*rule->predicates));
        if (!rule->predicates) return -1;
        for (void *it = json_object_iter((json_t *)match); it;
             it = json_object_iter_next((json_t *)match, it)) {
            const char *key = json_object_iter_key(it);
            const json_t *value = json_object_iter_value(it);
            if (!pointer_valid(key) || !json_is_string(value))
                return invalid(error, size, "match needs /pointer string pairs");
            struct snag_rule_predicate *pred = &rule->predicates[rule->predicate_count];
            pred->pointer = snag_strdup_checked(key, 256u);
            if (!pred->pointer) return -1;
            if (regcomp(&pred->regex, json_string_value(value), REG_EXTENDED | REG_NOSUB) != 0) {
                free(pred->pointer);
                pred->pointer = NULL;
                return invalid(error, size, "invalid match regex");
            }
            ++rule->predicate_count;
        }
    }
    if (json_is_object(at_least) && json_object_size(at_least) > 0u) {
        rule->thresholds = calloc(json_object_size(at_least), sizeof(*rule->thresholds));
        if (!rule->thresholds) return -1;
        for (void *it = json_object_iter((json_t *)at_least); it;
             it = json_object_iter_next((json_t *)at_least, it)) {
            const char *key = json_object_iter_key(it);
            const json_t *value = json_object_iter_value(it);
            if (!pointer_valid(key) || !json_is_integer(value) || json_integer_value(value) < 0)
                return invalid(error, size, "at_least needs /pointer integer pairs");
            rule->thresholds[rule->threshold_count].pointer = snag_strdup_checked(key, 256u);
            if (!rule->thresholds[rule->threshold_count].pointer) return -1;
            rule->thresholds[rule->threshold_count].value = json_integer_value(value);
            ++rule->threshold_count;
        }
    }
    return 0;
}

struct snag_rules *
snag_rules_compile(const json_t *definition, char *error, size_t size)
{
    struct snag_rules *rules;
    const json_t *declared;
    const json_t *list;

    if (!definition || json_is_null(definition)) {
        rules = calloc(1u, sizeof(*rules));
        if (!rules) return NULL;
        if (snag_json_digest(json_object(), rules->digest) < 0) {
            free(rules);
            return NULL;
        }
        return rules;
    }
    if (!json_is_object(definition)) {
        invalid(error, size, "rules must be an object with chains and rules");
        return NULL;
    }
    {
        for (void *it = json_object_iter((json_t *)definition); it;
             it = json_object_iter_next((json_t *)definition, it)) {
            const char *key = json_object_iter_key(it);
            if (strcmp(key, "chains") && strcmp(key, "rules")) {
                invalid(error, size, "rules must be an object with chains and rules");
                return NULL;
            }
        }
    }
    list = json_object_get(definition, "rules");
    if (list && (!json_is_array(list) || json_array_size(list) > SNAG_RULES_MAX)) {
        invalid(error, size, "rules must be a bounded array");
        return NULL;
    }
    rules = calloc(1u, sizeof(*rules));
    if (!rules) return NULL;
    if (snag_json_digest(definition, rules->digest) < 0) goto fail;
    rules->chains = calloc(SNAG_CHAINS_MAX, sizeof(*rules->chains));
    rules->rules = calloc(SNAG_RULES_MAX, sizeof(*rules->rules));
    if (!rules->chains || !rules->rules) goto fail;
    if (chain_add(rules, "in") < 0 || chain_add(rules, "out") < 0 || chain_add(rules, "event") < 0) goto fail;
    declared = json_object_get(definition, "chains");
    if (declared && !json_is_null(declared)) {
        if (!json_is_array(declared)) goto fail;
        for (size_t i = 0u; i < json_array_size(declared); ++i) {
            const json_t *value = json_array_get(declared, i);
            if (!json_is_string(value) || chain_add(rules, json_string_value(value)) < 0) goto fail;
        }
    }
    rules->rule_count = list ? json_array_size(list) : 0u;
    for (size_t i = 0u; i < rules->rule_count; ++i)
        if (compile_rule(rules, json_array_get(list, i), i, error, size) < 0) goto fail;
    for (size_t i = 0u; i < rules->rule_count; ++i)
        if (rules->rules[i].verb == SNAG_RULE_JUMP &&
            chain_index(rules, rules->rules[i].target) == rules->chain_count) goto fail;
    return rules;
fail:
    if (!error[0]) (void)snag_errorf(error, size, "invalid rule definition");
    snag_rules_free(rules);
    return NULL;
}

void
snag_rules_free(struct snag_rules *rules)
{
    if (!rules) return;
    for (size_t i = 0u; i < rules->rule_count; ++i) rule_free(&rules->rules[i]);
    free(rules->rules);
    free(rules->chains);
    free(rules);
}

bool
snag_rules_empty(const struct snag_rules *rules)
{
    return !rules || rules->rule_count == 0u;
}

const char *
snag_rules_digest(const struct snag_rules *rules)
{
    return rules ? rules->digest : "";
}

const char *snag_rule_name(const struct snag_rule *rule) { return rule->name; }
const char *snag_rule_chain(const struct snag_rule *rule) { return rule->chain; }
enum snag_rule_verb snag_rule_verb(const struct snag_rule *rule) { return rule->verb; }
const char *snag_rule_text(const struct snag_rule *rule) { return rule->text; }
const char *snag_rule_target(const struct snag_rule *rule) { return rule->target; }
const char *snag_rule_log(const struct snag_rule *rule) { return rule->log; }
const json_t *snag_rule_value(const struct snag_rule *rule) { return rule->value; }
const char *snag_rule_to(const struct snag_rule *rule) { return rule->to; }
const char *snag_rule_command(const struct snag_rule *rule) { return rule->command; }
unsigned int snag_rule_timeout_ms(const struct snag_rule *rule) { return rule->timeout_ms; }

static int
rule_matches(const struct snag_rule *rule, const json_t *envelope)
{
    for (size_t i = 0u; i < rule->threshold_count; ++i) {
        const json_t *value = pointer_get(envelope, rule->thresholds[i].pointer);
        if (!json_is_integer(value) || json_integer_value(value) < rule->thresholds[i].value) return 0;
    }
    struct snag_buf bytes;
    snag_buf_init(&bytes, SNAG_RULE_ENVELOPE_MAX);
    int result = 0;
    for (size_t i = 0u; i < rule->predicate_count; ++i) {
        const json_t *value = pointer_get(envelope, rule->predicates[i].pointer);
        if (!value) goto done;
        const char *text = json_is_string(value) ? json_string_value(value) : NULL;
        if (!text) {
            snag_buf_reset(&bytes);
            if (snag_json_canonical(value, &bytes) < 0 || snag_buf_terminate(&bytes) < 0) {
                result = -1;
                goto done;
            }
            text = (const char *)bytes.data;
        }
        int rc = regexec(&rule->predicates[i].regex, text, 0u, NULL, 0);
        if (rc == REG_NOMATCH) goto done;
        if (rc) {
            result = -1;
            goto done;
        }
    }
    result = 1;
done: snag_buf_free(&bytes);
    return result;
}

int
snag_rules_eval(const struct snag_rules *rules, struct snag_rule_frame *frame,
                snag_rule_effect_fn effect, void *opaque,
                struct snag_rule_verdict *verdict, char *error, size_t size)
{
    struct { size_t chain, next; } stack[SNAG_CHAINS_MAX];
    const char *boundary;
    size_t depth = 0u;

    if (!rules || !frame || !verdict || !json_is_object(frame->envelope))
        return invalid(error, size, "invalid evaluation envelope");
    boundary = snag_json_string(frame->envelope, "boundary");
    if (!snag_rules_boundary(boundary)) return invalid(error, size, "invalid evaluation boundary");
    memset(verdict, 0, sizeof(*verdict));
    stack[0].chain = chain_index(rules, boundary);
    stack[0].next = 0u;
    if (stack[0].chain == rules->chain_count) return 0;
    for (;;) {
        const struct snag_rule *rule = NULL;
        while (stack[depth].next < rules->rule_count) {
            size_t i = stack[depth].next++;
            if (rules->rules[i].chain_id == stack[depth].chain) {
                rule = &rules->rules[i];
                break;
            }
        }
        if (!rule) {
            if (!depth) return 0;
            --depth;
            continue;
        }
        if (++verdict->visits > SNAG_RULE_VISITS_MAX) return invalid(error, size, "evaluation visit limit");
        int matched = rule_matches(rule, frame->envelope);
        if (matched < 0) return invalid(error, size, "rule matching failed");
        if (!matched) continue;
        ++verdict->matches;
        bool needs_host = rule->log || rule->value || rule->verb == SNAG_RULE_INSERT ||
            rule->verb == SNAG_RULE_COMMAND || rule->verb == SNAG_RULE_CONFIRM;
        if (!effect && needs_host)
            return invalid(error, size, "rule effect needs a host handler");
        int rc = effect ? effect(opaque, rule, frame, error, size) : 0;
        if (rc < 0) return -1;
        if (rc > 0) verdict->rejected = true;
        if (rule->verb == SNAG_RULE_REJECT) verdict->rejected = true;
        if (rule->verb == SNAG_RULE_ACCEPT) return 0;
        if (rule->verb == SNAG_RULE_JUMP) {
            size_t target = chain_index(rules, rule->target);
            if (target == rules->chain_count || depth + 1u >= SNAG_CHAINS_MAX)
                return invalid(error, size, "invalid jump target");
            ++depth;
            stack[depth].chain = target;
            stack[depth].next = 0u;
        } else if (rule->verb == SNAG_RULE_RETURN) {
            if (!depth) return 0;
            --depth;
        }
    }
}
