/* SPDX-License-Identifier: GPL-2.0-only */
#include "rules.h"

#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct snag_rule_predicate {
    char *pointer;
    regex_t regex;
};

struct snag_rule {
    char name[SNAG_RULE_NAME_MAX + 1u];
    enum snag_rule_verb verb;
    char *message;
    struct snag_rule_predicate *predicates;
    size_t predicate_count;
};

struct snag_rules {
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

static void
rule_free(struct snag_rule *rule)
{
    for (size_t i = 0u; i < rule->predicate_count; ++i) {
        regfree(&rule->predicates[i].regex);
        free(rule->predicates[i].pointer);
    }
    free(rule->predicates);
    free(rule->message);
    memset(rule, 0, sizeof(*rule));
}

static int
compile_rule(struct snag_rules *rules, json_t *definition, size_t index, char *error, size_t size)
{
    static const char *const removed =
        " (rule chains, jumps, thresholds, templates and helper verbs are unsupported by this build;"
        " see design/io-rules.md for the migration)";
    struct snag_rule *rule = &rules->rules[index];
    const char *name = snag_json_string(definition, "name");
    const char *action = snag_json_string(definition, "action");
    const char *message = snag_json_string(definition, "message");
    const json_t *match = json_object_get(definition, "match");

    if (!json_is_object(definition) || !name_valid(name) || !action || !*action)
        return invalid(error, size, "invalid rule identity");
    {
        for (void *it = json_object_iter(definition); it;
             it = json_object_iter_next(definition, it)) {
            const char *key = json_object_iter_key(it);
            if (strcmp(key, "name") && strcmp(key, "match") && strcmp(key, "action") &&
                strcmp(key, "message")) {
                char hint[192];
                (void)snprintf(hint, sizeof(hint), "unknown rule key '%s'%s", key, removed);
                return invalid(error, size, hint);
            }
        }
    }
    for (size_t i = 0u; i < index; ++i)
        if (strcmp(rules->rules[i].name, name) == 0) return invalid(error, size, "duplicate rule name");
    if (!strcmp(action, "allow")) {
        rule->verb = SNAG_RULE_ALLOW;
    } else if (!strcmp(action, "deny")) {
        rule->verb = SNAG_RULE_DENY;
    } else {
        return invalid(error, size, "rule action must be allow or deny");
    }
    if (message) {
        const json_t *node = json_object_get(definition, "message");
        if (!json_is_string(node) || strlen(message) > SNAG_RULE_TEXT_MAX)
            return invalid(error, size, "message must be a bounded string");
        if (rule->verb == SNAG_RULE_ALLOW) return invalid(error, size, "message applies only to deny");
        rule->message = snag_strdup_checked(message, SNAG_RULE_TEXT_MAX + 1u);
        if (!rule->message) return -1;
    }
    if (match && !json_is_object(match)) return invalid(error, size, "match must be an object");
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
    (void)snprintf(rule->name, sizeof(rule->name), "%s", name);
    return 0;
}

struct snag_rules *
snag_rules_compile(const json_t *definition, char *error, size_t size)
{
    struct snag_rules *rules;
    const json_t *list;

    if (!definition || json_is_null(definition)) {
        json_t *empty;

        rules = calloc(1u, sizeof(*rules));
        if (!rules) return NULL;
        empty = json_object();
        if (!empty) {
            free(rules);
            return NULL;
        }
        if (snag_json_digest(empty, rules->digest) < 0) {
            json_decref(empty);
            free(rules);
            return NULL;
        }
        json_decref(empty);
        return rules;
    }
    if (!json_is_object(definition)) {
        invalid(error, size, "rules must be an object with a rules array");
        return NULL;
    }
    {
        for (void *it = json_object_iter((json_t *)definition); it;
             it = json_object_iter_next((json_t *)definition, it)) {
            const char *key = json_object_iter_key(it);
            if (strcmp(key, "rules")) {
                invalid(error, size, "rules must be an object with a rules array");
                return NULL;
            }
        }
    }
    list = json_object_get(definition, "rules");
    if (list && !json_is_array(list)) {
        invalid(error, size, "rules must be an array");
        return NULL;
    }
    rules = calloc(1u, sizeof(*rules));
    if (!rules) return NULL;
    if (snag_json_digest(definition, rules->digest) < 0) goto fail;
    size_t count = list ? json_array_size(list) : 0u;
    rules->rules = calloc(count ? count : 1u, sizeof(*rules->rules));
    if (!rules->rules) goto fail;
    rules->rule_count = count;
    for (size_t i = 0u; i < rules->rule_count; ++i)
        if (compile_rule(rules, json_array_get(list, i), i, error, size) < 0) goto fail;
    return rules;
fail:
    if (error && !error[0]) (void)snag_errorf(error, size, "invalid rule definition");
    snag_rules_free(rules);
    return NULL;
}

void
snag_rules_free(struct snag_rules *rules)
{
    if (!rules) return;
    for (size_t i = 0u; i < rules->rule_count; ++i) rule_free(&rules->rules[i]);
    free(rules->rules);
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
enum snag_rule_verb snag_rule_verb(const struct snag_rule *rule) { return rule->verb; }
const char *snag_rule_message(const struct snag_rule *rule) { return rule->message; }

static int
rule_matches(const struct snag_rule *rule, const json_t *envelope)
{
    struct snag_buf bytes;
    int result = 0;

    snag_buf_init(&bytes, SNAG_RULE_ENVELOPE_MAX);
    for (size_t i = 0u; i < rule->predicate_count; ++i) {
        const json_t *value = pointer_get(envelope, rule->predicates[i].pointer);
        const char *text;
        int rc;
        if (!value) goto done;
        text = json_is_string(value) ? json_string_value(value) : NULL;
        if (!text) {
            snag_buf_reset(&bytes);
            if (snag_json_canonical(value, &bytes) < 0 || snag_buf_terminate(&bytes) < 0) {
                result = -1;
                goto done;
            }
            text = (const char *)bytes.data;
        }
        rc = regexec(&rule->predicates[i].regex, text, 0u, NULL, 0);
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

struct snag_rule_position {
    size_t chain, next;
};

int
snag_rules_eval(const struct snag_rules *rules, struct snag_rule_frame *frame,
                snag_rule_effect_fn effect, void *opaque,
                struct snag_rule_verdict *verdict, char *error, size_t size)
{
    const char *boundary;
    bool decided = false;

    if (!rules || !frame || !verdict || !json_is_object(frame->envelope))
        return invalid(error, size, "invalid evaluation envelope");
    boundary = snag_json_string(frame->envelope, "boundary");
    if (!snag_rules_boundary(boundary)) return invalid(error, size, "invalid evaluation boundary");
    memset(verdict, 0, sizeof(*verdict));
    for (size_t i = 0u; i < rules->rule_count; ++i) {
        const struct snag_rule *rule = &rules->rules[i];
        int matched, rc;
        ++verdict->visits;
        matched = rule_matches(rule, frame->envelope);
        if (matched < 0) return invalid(error, size, "rule matching failed");
        if (!matched) continue;
        ++verdict->matches;
        if (effect) {
            rc = effect(opaque, rule, frame, error, size);
            if (rc < 0) return -1;
        }
        if (!decided) {
            decided = true;
            verdict->rejected = rule->verb == SNAG_RULE_DENY;
        }
    }
    return 0;
}
