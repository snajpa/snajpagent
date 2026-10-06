/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_IRC_ADDRESS_H
#define SNAJPAGENT_IRC_ADDRESS_H

#include "base.h"
#include "config.h"

enum snag_irc_address_form { SNAG_IRC_BUFFER_ADDRESS, SNAG_IRC_MESSAGE_ADDRESS };
enum snag_irc_address_kind { SNAG_IRC_TRANSCRIPT, SNAG_IRC_CONNECTION, SNAG_IRC_CONVERSATION };

struct snag_irc_address {
    enum snag_irc_address_kind kind;
    char session[SNAG_PATH_MAX_BYTES + 1u];
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char target[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
};

/* Buffer: session, session/endpoint/, session/endpoint/target.
 * Message: target, endpoint/target, session/endpoint/target.
 * Split before decoding %2F and %25. This parses selectors; the owner must
 * resolve exact connection identity and validate its target/channel grammar.
 * Failure preserves the previous address. No aggregate/wildcard expansion. */
int snag_irc_address_parse(struct snag_irc_address *, const char *, enum snag_irc_address_form,
    char *, size_t);
char *snag_irc_address_format(const struct snag_irc_address *);
/* One quoted operand plus untouched message suffix. No shell evaluation or
 * comments; # remains channel text. Caller owns the returned operand. */
int snag_irc_address_operand(const char *, char **operand, const char **rest, char *, size_t);

#endif
