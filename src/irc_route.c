/* SPDX-License-Identifier: GPL-2.0-only */
#include "irc.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Growable route and destination value lists. These stay free of engine
 * dependencies so the terminal and renderer can link them without the IRC
 * core. */

int
snag_irc_route_add(struct snag_irc_route *route, struct snag_irc_target target)
{
    struct snag_irc_target *grown;

    for (size_t i = 0u; i < route->count; ++i)
        if (route->targets[i].id == target.id && route->targets[i].revision == target.revision) return 0;
    if (route->count == route->capacity) {
        size_t capacity = route->capacity ? route->capacity * 2u : 8u;

        if (capacity < route->capacity) return snag_errno(EOVERFLOW);
        grown = realloc(route->targets, capacity * sizeof(*grown));
        if (!grown) return snag_errno(ENOMEM);
        route->targets = grown;
        route->capacity = capacity;
    }
    route->targets[route->count++] = target;
    return 0;
}

void
snag_irc_route_clear(struct snag_irc_route *route)
{
    free(route->targets);
    route->targets = NULL;
    route->count = route->capacity = 0u;
}

int
snag_irc_route_copy(struct snag_irc_route *dst, const struct snag_irc_route *src)
{
    snag_irc_route_clear(dst);
    for (size_t i = 0u; i < src->count; ++i)
        if (snag_irc_route_add(dst, src->targets[i]) < 0) {
            snag_irc_route_clear(dst);
            return -1;
        }
    return 0;
}

void
snag_irc_destinations_free(struct snag_irc_destinations *destinations)
{
    free(destinations->items);
    destinations->items = NULL;
    destinations->count = destinations->capacity = 0u;
}

bool
snag_irc_destinations_equal(const struct snag_irc_destinations *left,
                           const struct snag_irc_destinations *right)
{
    return left->count == right->count &&
        (!left->count || memcmp(left->items, right->items, left->count * sizeof(*left->items)) == 0);
}

int
snag_irc_destinations_assign(struct snag_irc_destinations *dst, const struct snag_irc_destinations *src)
{
    struct snag_irc_destination *items = NULL;

    if (src->count) {
        items = malloc(src->count * sizeof(*items));
        if (!items) return snag_errno(ENOMEM);
        memcpy(items, src->items, src->count * sizeof(*items));
    }
    snag_irc_destinations_free(dst);
    dst->items = items;
    dst->count = dst->capacity = src->count;
    return 0;
}
