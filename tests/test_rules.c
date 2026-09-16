/* SPDX-License-Identifier: GPL-2.0-only */
#include "rules.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct host_log {
    size_t calls;
    char last[256];
};

static int
host_effect(void *opaque, const struct snag_rule *rule,
            struct snag_rule_frame *frame, char *error, size_t error_size)
{
    struct host_log *log = opaque;
    (void)frame;
    (void)error;
    (void)error_size;
    ++log->calls;
    (void)snprintf(log->last, sizeof(log->last), "%s:%d", snag_rule_name(rule),
                   snag_rule_verb(rule) == SNAG_RULE_DENY);
    return 0;
}

static json_t *
definition(const char *rules)
{
    static const unsigned char prefix[] = "{\"rules\":";
    static const unsigned char suffix[] = "}";
    size_t len = (sizeof(prefix) - 1u) + strlen(rules) + (sizeof(suffix) - 1u);
    unsigned char *text = malloc(len + 1u);
    char error[128] = {0};
    json_t *parsed;

    assert(text);
    memcpy(text, prefix, sizeof(prefix) - 1u);
    memcpy(text + sizeof(prefix) - 1u, rules, strlen(rules));
    memcpy(text + sizeof(prefix) - 1u + strlen(rules), suffix, sizeof(suffix) - 1u);
    text[len] = '\0';
    parsed = snag_json_load_strict(text, len, 1u << 20, error, sizeof(error));
    free(text);
    assert(parsed);
    return parsed;
}

static int
evaluate(struct snag_rules *rules, json_t *envelope, struct host_log *log, struct snag_rule_verdict *verdict)
{
    struct snag_rule_frame frame = {envelope};
    char error[192] = {0};
    int rc = snag_rules_eval(rules, &frame, host_effect, log, verdict, error, sizeof(error));
    if (rc < 0) fprintf(stderr, "eval error: %s\n", error);
    return rc;
}

static void
test_deny_and_default_allow(void)
{
    json_t *def = definition( "[{\"name\":\"deny-exec\","
        "\"match\":{\"/tool\":\"^exec_command$\"},\"action\":\"deny\","
        "\"message\":\"Running commands is disabled here.\"}]");
    struct snag_rules *rules = snag_rules_compile(def, NULL, 0u);
    struct host_log log = {0};
    struct snag_rule_verdict verdict;
    json_t *envelope;

    assert(rules && !snag_rules_empty(rules));
    assert(snag_rules_digest(rules)[0]);
    envelope = json_pack("{s:s,s:s,s:s}", "boundary", "out", "kind", "tool_call", "tool", "exec_command");
    assert(evaluate(rules, envelope, &log, &verdict) == 0);
    assert(verdict.rejected && verdict.matches == 1u && log.calls == 1u);
    json_decref(envelope);

    envelope = json_pack("{s:s,s:s,s:s}", "boundary", "out", "kind", "tool_call", "tool", "read_file");
    assert(evaluate(rules, envelope, &log, &verdict) == 0);
    assert(!verdict.rejected && verdict.matches == 0u);
    json_decref(envelope);
    snag_rules_free(rules);
    json_decref(def);
}

static void
test_first_match_wins(void)
{
    json_t *def = definition( "[{\"name\":\"allow-exec\",\"match\":{\"/tool\":\"^exec_command$\"},"
        "\"action\":\"allow\"},{\"name\":\"deny-all\",\"action\":\"deny\",\"message\":\"no\"}]");
    struct snag_rules *rules = snag_rules_compile(def, NULL, 0u);
    struct host_log log = {0};
    struct snag_rule_verdict verdict;
    json_t *envelope;

    assert(rules);
    envelope = json_pack("{s:s,s:s}", "boundary", "out", "tool", "exec_command");
    assert(evaluate(rules, envelope, &log, &verdict) == 0);
    assert(!verdict.rejected && verdict.matches == 2u && log.calls == 2u);
    assert(!strcmp(log.last, "deny-all:1"));
    json_decref(envelope);

    envelope = json_pack("{s:s,s:s}", "boundary", "out", "tool", "apply_patch");
    assert(evaluate(rules, envelope, &log, &verdict) == 0);
    assert(verdict.rejected && verdict.matches == 1u);
    json_decref(envelope);
    snag_rules_free(rules);
    json_decref(def);
}

static void
test_order_decides(void)
{
    json_t *def = definition( "[{\"name\":\"deny-all\",\"action\":\"deny\",\"message\":\"no\"},"
        "{\"name\":\"allow-exec\",\"match\":{\"/tool\":\"^exec_command$\"},\"action\":\"allow\"}]");
    struct snag_rules *rules = snag_rules_compile(def, NULL, 0u);
    struct host_log log = {0};
    struct snag_rule_verdict verdict;
    json_t *envelope = json_pack("{s:s,s:s}", "boundary", "out", "tool", "exec_command");

    assert(rules);
    assert(evaluate(rules, envelope, &log, &verdict) == 0);
    assert(verdict.rejected && verdict.matches == 2u && log.calls == 2u);
    assert(!strcmp(log.last, "allow-exec:0"));
    json_decref(envelope);
    snag_rules_free(rules);
    json_decref(def);
}

static void
test_invalid_definitions_rejected(void)
{
    static const char *const bad[] = {
        "[{\"name\":\"x\",\"action\":\"explode\"}]",
        "[{\"name\":\"x\",\"action\":\"allow\",\"match\":{\"/a\":\"(\"}}]",
        "[{\"name\":\"x\",\"action\":\"allow\"},{\"name\":\"x\",\"action\":\"deny\",\"message\":\"m\"}]",
        "[{\"name\":\"x\",\"action\":\"allow\",\"bogus\":1}]",
        "[{\"name\":\"x\",\"action\":\"allow\",\"match\":{\"bad\":\"a\"}}]",
        "[{\"name\":\"x\",\"action\":\"allow\",\"message\":\"m\"}]",
        "[{\"name\":\"x\",\"chain\":\"out\",\"action\":\"allow\"}]",
        "[{\"name\":\"x\",\"action\":\"jump\",\"target\":\"policy\"}]",
        "[{\"name\":\"x\",\"action\":\"deny\",\"message\":\"m\",\"text\":\"t\"}]",
        "[{\"name\":\"x\",\"action\":\"deny\",\"message\":\"m\",\"at_least\":{\"/n\":1}}]",
        "[{\"name\":\"x\",\"action\":\"deny\",\"message\":\"m\",\"log\":\"l\"}]",
        "[{\"name\":\"x\",\"action\":\"deny\",\"message\":\"m\",\"value\":{\"a\":1}}]",
        "[{\"name\":\"x\",\"action\":\"deny\",\"message\":\"m\",\"command\":\"/bin/true\"}]",
        "[{\"name\":\"x\",\"action\":\"deny\",\"message\":\"m\",\"to\":\"model\",\"text\":\"t\"}]",
        "[{\"name\":\"Bad Name!\",\"action\":\"allow\"}]",
        "[{\"name\":\"x\",\"action\":\"deny\"}]" };

    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]) - 1u; ++i) {
        json_t *def = definition(bad[i]);
        char error[256] = {0};
        struct snag_rules *rules = snag_rules_compile(def, error, sizeof(error));
        assert(!rules && error[0]);
        json_decref(def);
    }
    {
        json_t *def = definition(bad[sizeof(bad) / sizeof(bad[0]) - 1u]);
        struct snag_rules *rules = snag_rules_compile(def, NULL, 0u);
        assert(rules && !snag_rules_empty(rules));
        snag_rules_free(rules);
        json_decref(def);
    }
}

static void
test_empty_and_boundary_rules(void)
{
    struct snag_rules *rules = snag_rules_compile(NULL, NULL, 0u);
    struct snag_rule_frame frame;
    struct snag_rule_verdict verdict;
    struct host_log log = {0};
    json_t *envelope = json_pack("{s:s}", "boundary", "out");

    assert(rules && snag_rules_empty(rules));
    frame.envelope = envelope;
    assert(snag_rules_eval(rules, &frame, host_effect, &log, &verdict, NULL, 0u) == 0);
    assert(!verdict.rejected && verdict.matches == 0u);
    json_object_set_new(envelope, "boundary", json_string("sideways"));
    assert(snag_rules_eval(rules, &frame, host_effect, &log, &verdict, NULL, 0u) < 0);
    json_decref(envelope);
    snag_rules_free(rules);
}

int
main(void)
{
    test_deny_and_default_allow();
    test_first_match_wins();
    test_order_decides();
    test_invalid_definitions_rejected();
    test_empty_and_boundary_rules();
    puts("test_rules: ok");
    return 0;
}
