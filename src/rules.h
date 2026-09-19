/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_RULES_H
#define SNAJPAGENT_RULES_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>

/* Bounded, stateless I/O filter. One envelope crosses one real boundary; the
 * ordered rule list is scanned once and the first matching rule decides.
 * Later matching rules are still reported, so a trailing match-all rule gives
 * a complete audit trail without changing the verdict. The engine never owns
 * durable state: logging is applied by the host through one callback.
 * Matching is filtering, never containment; containment belongs to bindings
 * and native dispatch. */

#define SNAG_RULES_MAX 256u
#define SNAG_RULE_NAME_MAX 63u
#define SNAG_RULE_TEXT_MAX (64u * 1024u)
#define SNAG_RULE_ENVELOPE_MAX (4u * 1024u * 1024u)

enum snag_rule_verb {
    SNAG_RULE_ALLOW, SNAG_RULE_DENY, SNAG_RULE_VERB_COUNT };

struct snag_rules;
struct snag_rule;

/* Caller owns the normalized envelope and must keep it a JSON object with a
 * valid `boundary`. */
struct snag_rule_frame {
    json_t *envelope;
};

struct snag_rule_verdict {
    bool rejected;
    size_t visits, matches;
};

/* Report one matched rule to the host (fixed-format logging). Return 0 to
 * continue scanning, -1 for a host failure. The verdict belongs to the first
 * matching rule; the callback never changes it. */
typedef int (*snag_rule_effect_fn)(void *opaque, const struct snag_rule *rule, struct snag_rule_frame *frame,
                                   char *error, size_t size);

struct snag_rules *snag_rules_compile(const json_t *definition, char *error, size_t size);
void snag_rules_free(struct snag_rules *rules);
bool snag_rules_empty(const struct snag_rules *rules);
bool snag_rules_boundary(const char *boundary);
const char *snag_rules_digest(const struct snag_rules *rules);

const char *snag_rule_name(const struct snag_rule *rule);
enum snag_rule_verb snag_rule_verb(const struct snag_rule *rule);
const char *snag_rule_message(const struct snag_rule *rule);

int snag_rules_eval(const struct snag_rules *rules, struct snag_rule_frame *frame,
                    snag_rule_effect_fn effect, void *opaque,
                    struct snag_rule_verdict *verdict, char *error, size_t size);

#endif
