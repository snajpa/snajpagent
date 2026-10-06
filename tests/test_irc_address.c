/* SPDX-License-Identifier: GPL-2.0-only */
#include "irc_address.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct snag_irc_address
parse(const char *value, enum snag_irc_address_form form)
{
    struct snag_irc_address address;
    char error[256] = "";
    assert(snag_irc_address_parse(&address, value, form, error, sizeof(error)) == 0);
    char *formatted = snag_irc_address_format(&address);
    assert(formatted);
    struct snag_irc_address restored;
    assert(snag_irc_address_parse(&restored, formatted, form, error, sizeof(error)) == 0);
    assert(restored.kind == address.kind && !strcmp(restored.session, address.session) &&
        !strcmp(restored.endpoint, address.endpoint) && !strcmp(restored.target, address.target));
    free(formatted);
    return address;
}

int
main(void)
{
    struct snag_irc_address address = parse("lead", SNAG_IRC_BUFFER_ADDRESS);
    assert(address.kind == SNAG_IRC_TRANSCRIPT && !strcmp(address.session, "lead"));
    address = parse("lead/[::1]:6667/", SNAG_IRC_BUFFER_ADDRESS);
    assert(address.kind == SNAG_IRC_CONNECTION && !strcmp(address.endpoint, "[::1]:6667"));
    address = parse("lead/localhost:6667/secretary", SNAG_IRC_BUFFER_ADDRESS);
    assert(address.kind == SNAG_IRC_CONVERSATION && !strcmp(address.target, "secretary"));
    address = parse("team%2flead/lab%252Fone/#work", SNAG_IRC_BUFFER_ADDRESS);
    assert(!strcmp(address.session, "team/lead") && !strcmp(address.endpoint, "lab%2Fone") &&
        !strcmp(address.target, "#work"));
    address = parse("peer", SNAG_IRC_MESSAGE_ADDRESS);
    assert(!*address.session && !*address.endpoint && !strcmp(address.target, "peer"));
    address = parse("local/peer", SNAG_IRC_MESSAGE_ADDRESS);
    assert(!*address.session && !strcmp(address.endpoint, "local"));
    (void)parse("lead/local/peer", SNAG_IRC_MESSAGE_ADDRESS);
    address = parse("lead/local/all", SNAG_IRC_MESSAGE_ADDRESS);
    assert(!strcmp(address.target, "all"));
    (void)parse("pracovní relace/local/#kanál", SNAG_IRC_BUFFER_ADDRESS);
    const char *bad[] = {"", "lead/local", "lead//peer", "lead/local/peer/more", "lead/%00/peer",
        "lead/local/pe%er", "lead/local/\033"};
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        struct snag_irc_address old = address;
        char error[256] = "";
        assert(snag_irc_address_parse(&address, bad[i], SNAG_IRC_BUFFER_ADDRESS,
            error, sizeof(error)) < 0 && *error);
        assert(!memcmp(&old, &address, sizeof(address)));
    }
    char error[256] = "";
    assert(snag_irc_address_parse(&address, "lead/local/", SNAG_IRC_MESSAGE_ADDRESS,
        error, sizeof(error)) < 0);
    char *operand = NULL;
    const char *rest = NULL;
    assert(snag_irc_address_operand("  'work space'/local/#chan  hello # world", &operand,
        &rest, error, sizeof(error)) == 0);
    assert(!strcmp(operand, "work space/local/#chan") && !strcmp(rest, "hello # world"));
    free(operand);
    assert(snag_irc_address_operand("\"$(literal)\"/local/peer text", &operand,
        &rest, error, sizeof(error)) == 0);
    assert(!strcmp(operand, "$(literal)/local/peer") && !strcmp(rest, "text"));
    free(operand);
    assert(snag_irc_address_operand("'unfinished", &operand, &rest, error, sizeof(error)) < 0);
    assert(!operand);
    assert(snag_irc_address_operand("nick\\", &operand, &rest, error, sizeof(error)) < 0);
    assert(!operand);
    puts("test_irc_address: ok");
    return 0;
}
