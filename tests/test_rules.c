/* SPDX-License-Identifier: GPL-2.0-only */
#include "rules.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct host_log {
    size_t calls;
    size_t veto_at;
    char last[128];
};

static int
host_effect(void *opaque, const struct snag_rule *rule,
            struct snag_rule_frame *frame, char *error, size_t error_size)
{
    struct host_log *log = opaque;
    (void)error;
    (void)error_size;
    ++log->calls;
    if (snag_rule_log(rule)) {
        struct snag_buf out;
        snag_buf_init(&out, 256u);
        assert(snag_rule_render(snag_rule_log(rule), frame->envelope, &out) == 0);
        assert(snag_buf_terminate(&out) == 0);
        (void)snprintf(log->last, sizeof(log->last), "%s", (const char *)out.data);
        snag_buf_free(&out);
    }
    return log->veto_at == log->calls ? 1 : 0;
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
test_compile_and_simple_verdicts(void)
{
    json_t *def = definition( "[{\"name\":\"deny-exec\",\"chain\":\"out\","
        "\"match\":{\"/tool\":\"^exec_command$\"},\"action\":\"reject\","
        "\"text\":\"Running commands is disabled here.\"}]");
    struct snag_rules *rules = snag_rules_compile(def, NULL, 0u);
    struct host_log log = {0};
    struct snag_rule_verdict verdict;
    json_t *envelope;

    assert(rules && !snag_rules_empty(rules));
    assert(snag_rules_digest(rules)[0]);
    envelope = json_pack("{s:s,s:s,s:s}", "boundary", "out", "kind", "tool_call", "tool", "exec_command");
    assert(evaluate(rules, envelope, &log, &verdict) == 0);
    assert(verdict.rejected && verdict.matches == 1u);
    json_decref(envelope);

    envelope = json_pack("{s:s,s:s,s:s}", "boundary", "out", "kind", "tool_call", "tool", "read_file");
    assert(evaluate(rules, envelope, &log, &verdict) == 0);
    assert(!verdict.rejected && verdict.matches == 0u);
    json_decref(envelope);
    snag_rules_free(rules);
    json_decref(def);
}

static void
test_log_and_template(void)
{
    json_t *def = definition( "[{\"name\":\"audit\",\"chain\":\"out\",\"match\":{\"/kind\":\"^tool_call$\"},"
        "\"action\":\"pass\",\"log\":\"tool=%{/tool} surface=%{/surface}\"}]");
    struct snag_rules *rules = snag_rules_compile(def, NULL, 0u);
    struct host_log log = {0};
    struct snag_rule_verdict verdict;
    json_t *envelope = json_pack("{s:s,s:s,s:s,s:s}", "boundary", "out",
                                 "kind", "tool_call", "surface", "model", "tool", "apply_patch");

    assert(evaluate(rules, envelope, &log, &verdict) == 0);
    assert(log.calls == 1u);
    assert(!strcmp(log.last, "tool=apply_patch surface=model"));
    json_decref(envelope);
    snag_rules_free(rules);
    json_decref(def);
}

static void
test_threshold_matching(void)
{
    json_t *def = definition( "[{\"name\":\"over\",\"chain\":\"event\",\"at_least\":{\"/percent\":80},"
        "\"action\":\"reject\",\"text\":\"over\"}]");
    struct snag_rules *rules = snag_rules_compile(def, NULL, 0u);
    struct host_log log = {0};
    struct snag_rule_verdict verdict;
    json_t *envelope = json_pack("{s:s,s:i}", "boundary", "event", "percent", 85);

    assert(evaluate(rules, envelope, &log, &verdict) == 0 && verdict.rejected);
    json_object_set_new(envelope, "percent", json_integer(79));
    assert(evaluate(rules, envelope, &log, &verdict) == 0 && !verdict.rejected);
    json_object_del(envelope, "percent");
    assert(evaluate(rules, envelope, &log, &verdict) == 0 && !verdict.rejected);
    json_decref(envelope);
    snag_rules_free(rules);
    json_decref(def);
}

static void
test_jump_and_return(void)
{
    json_t *def = definition( "[{\"name\":\"enter\",\"chain\":\"out\",\"match\":{\"/kind\":\"^tool_call$\"},"
        "\"action\":\"jump\",\"target\":\"policy\"},"
        "{\"name\":\"deny\",\"chain\":\"policy\",\"match\":{\"/tool\":\"^exec_\"},"
        "\"action\":\"reject\",\"text\":\"denied\"}]");
    struct snag_rules *rules = snag_rules_compile(def, NULL, 0u);
    struct host_log log = {0};
    struct snag_rule_verdict verdict;
    json_t *envelope = json_pack("{s:s,s:s,s:s}", "boundary", "out",
                                 "kind", "tool_call", "tool", "exec_command");

    assert(rules);
    assert(evaluate(rules, envelope, &log, &verdict) == 0);
    assert(verdict.rejected && verdict.matches == 2u);
    json_decref(envelope);
    snag_rules_free(rules);
    json_decref(def);
}

static void
test_veto_is_sticky(void)
{
    json_t *def = definition( "[{\"name\":\"veto\",\"chain\":\"out\",\"match\":{\"/kind\":\"^tool_call$\"},"
        "\"action\":\"pass\"}," "{\"name\":\"after\",\"chain\":\"out\",\"match\":{\"/kind\":\"^tool_call$\"},"
        "\"action\":\"pass\",\"log\":\"seen\"}]");
    struct snag_rules *rules = snag_rules_compile(def, NULL, 0u);
    struct host_log log = {.veto_at = 1u};
    struct snag_rule_verdict verdict;
    json_t *envelope = json_pack("{s:s,s:s}", "boundary", "out", "kind", "tool_call");

    assert(evaluate(rules, envelope, &log, &verdict) == 0);
    assert(verdict.rejected && verdict.matches == 2u && log.calls == 2u);
    json_decref(envelope);
    snag_rules_free(rules);
    json_decref(def);
}

static void
test_invalid_definitions_rejected(void)
{
    static const char *const bad[] = {
        "[{\"name\":\"x\",\"chain\":\"out\",\"action\":\"explode\"}]",
        "[{\"name\":\"x\",\"chain\":\"out\",\"action\":\"pass\",\"match\":{\"/a\":\"(\"}}]",
        "[{\"name\":\"x\",\"chain\":\"out\",\"action\":\"jump\",\"target\":\"missing\"}]",
        "[{\"name\":\"x\",\"chain\":\"out\",\"action\":\"pass\"},"
        "{\"name\":\"x\",\"chain\":\"out\",\"action\":\"pass\"}]",
        "[{\"name\":\"x\",\"chain\":\"out\",\"action\":\"pass\",\"bogus\":1}]",
        "[{\"name\":\"x\",\"chain\":\"out\",\"action\":\"pass\",\"match\":{\"bad\":\"a\"}}]",
        "[{\"name\":\"x\",\"chain\":\"out\",\"action\":\"jump\"}]" };

    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        json_t *def = definition(bad[i]);
        char error[192] = {0};
        struct snag_rules *rules = snag_rules_compile(def, error, sizeof(error));
        assert(!rules && error[0]);
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
    assert(!verdict.rejected);
    json_object_set_new(envelope, "boundary", json_string("sideways"));
    assert(snag_rules_eval(rules, &frame, host_effect, &log, &verdict, NULL, 0u) < 0);
    json_decref(envelope);
    snag_rules_free(rules);
}

int
main(void)
{
    test_compile_and_simple_verdicts();
    test_log_and_template();
    test_threshold_matching();
    test_jump_and_return();
    test_veto_is_sticky();
    test_invalid_definitions_rejected();
    test_empty_and_boundary_rules();
    puts("test_rules: ok");
    return 0;
}
