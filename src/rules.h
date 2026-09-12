/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_RULES_H
#define SNAJPAGENT_RULES_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>

/* Bounded, stateless I/O filter. One envelope crosses one real boundary; an
 * ordered chain of rules matches it and yields a verdict. The engine never
 * owns durable state: logging, insertion, confirmation and replacement are
 * applied by the host through one callback. Matching is filtering, never
 * containment; containment belongs to bindings and native dispatch. */

#define SNAG_RULES_MAX 256u
#define SNAG_CHAINS_MAX 64u
#define SNAG_RULE_NAME_MAX 63u
#define SNAG_RULE_VISITS_MAX 4096u
#define SNAG_RULE_TEXT_MAX (64u * 1024u)
#define SNAG_RULE_ENVELOPE_MAX (4u * 1024u * 1024u)

enum snag_rule_verb {
    SNAG_RULE_PASS, SNAG_RULE_ACCEPT, SNAG_RULE_REJECT, SNAG_RULE_JUMP, SNAG_RULE_RETURN,
    SNAG_RULE_VERB_COUNT };

struct snag_rules;
struct snag_rule;

/* Caller owns the normalized envelope and must keep it a JSON object with a
 * valid `boundary`. The host recomputes any derived facts before acting. */
struct snag_rule_frame {
    json_t *envelope;
};

struct snag_rule_verdict {
    bool rejected;
    size_t visits, matches;
};

/* Apply one matched rule's host-owned part: log flag, confirm flag, insert and
 * replace effects. Called in chain order for every matched rule. Return 0 to
 * continue, 1 to veto the operation (sticky reject), -1 for a host failure.
 * A veto does not stop traversal, so later observational rules still run. */
typedef int (*snag_rule_effect_fn)(void *opaque, const struct snag_rule *rule, struct snag_rule_frame *frame,
                                   char *error, size_t size);

struct snag_rules *snag_rules_compile(const json_t *definition, char *error, size_t size);
void snag_rules_free(struct snag_rules *rules);
bool snag_rules_empty(const struct snag_rules *rules);
bool snag_rules_boundary(const char *boundary);
const char *snag_rules_digest(const struct snag_rules *rules);

const char *snag_rule_name(const struct snag_rule *rule);
const char *snag_rule_chain(const struct snag_rule *rule);
enum snag_rule_verb snag_rule_verb(const struct snag_rule *rule);
const char *snag_rule_text(const struct snag_rule *rule);
const char *snag_rule_target(const struct snag_rule *rule);
const char *snag_rule_log(const struct snag_rule *rule);
const json_t *snag_rule_value(const struct snag_rule *rule);
bool snag_rule_confirm(const struct snag_rule *rule);

int snag_rules_eval(const struct snag_rules *rules, struct snag_rule_frame *frame,
                    snag_rule_effect_fn effect, void *opaque,
                    struct snag_rule_verdict *verdict, char *error, size_t size);

/* Render %{/json/pointer} against the envelope; %% is a literal percent. */
int snag_rule_render(const char *format, const json_t *envelope, struct snag_buf *out);

#endif
