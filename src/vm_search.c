/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_search.h"
#include "unicode.h"
#include "vm_source.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct origin {
    uint64_t seq, byte;
    bool first;
};

struct snag_vm_search {
    uint32_t *pattern;
    size_t *prefix, count, matched, slot;
    struct origin *origins;
    struct snag_vm_anchor start, best, fallback;
    bool ignorecase, reverse, found, wrapped, heading;
    char key[160], handle[SNAG_ID_HEX_LEN + 1u];
    unsigned int stream;
    uint64_t end, order;
};

void
snag_vm_search_close(struct snag_vm_search *search)
{
    if (!search) return;
    free(search->pattern);
    free(search->prefix);
    free(search->origins);
    free(search);
}

struct snag_vm_search *
snag_vm_search_open(const char *query, bool ignorecase, bool reverse,
    const struct snag_vm_anchor *start)
{
    if (!query || !*query || strlen(query) > SNAG_MAX_DIRECT_PROMPT || !start) {
        errno = EINVAL;
        return NULL;
    }
    struct snag_vm_search *search = calloc(1u, sizeof(*search));
    if (!search) return NULL;
    search->ignorecase = ignorecase;
    search->reverse = reverse;
    search->start = *start;
    struct snag_buf pattern = {.max = SNAG_MAX_DIRECT_PROMPT * 3u * sizeof(uint32_t)};
    size_t length = strlen(query);
    for (size_t at = 0u; at < length;) {
        uint32_t cp, folded[3];
        size_t bytes = snag_utf8_decode((const unsigned char *)query + at, length - at, &cp);
        if (!bytes) { errno = EILSEQ; goto failed; }
        folded[0] = cp;
        unsigned int count = ignorecase ? snag_unicode_casefold(cp, folded) : 1u;
        if (snag_buf_append(&pattern, folded, count * sizeof(*folded)) < 0) goto failed;
        at += bytes;
    }
    search->count = pattern.len / sizeof(uint32_t);
    search->pattern = (uint32_t *)pattern.data;
    pattern.data = NULL;
    search->prefix = calloc(search->count, sizeof(*search->prefix));
    search->origins = calloc(search->count, sizeof(*search->origins));
    if (!search->prefix || !search->origins) goto failed;
    for (size_t i = 1u, match = 0u; i < search->count; ++i) {
        while (match && search->pattern[i] != search->pattern[match])
            match = search->prefix[match - 1u];
        if (search->pattern[i] == search->pattern[match]) ++match;
        search->prefix[i] = match;
    }
    return search;
failed:
    snag_buf_free(&pattern);
    snag_vm_search_close(search);
    return NULL;
}

static int
compare(const struct snag_vm_anchor *a, const struct snag_vm_anchor *b)
{
    if (strcmp(a->key, b->key)) {
        if (a->seq != b->seq) return a->seq < b->seq ? -1 : 1;
        if (a->order != b->order) return a->order < b->order ? -1 : 1;
    }
    if (a->heading != b->heading) return a->heading ? -1 : 1;
    return a->byte < b->byte ? -1 : a->byte > b->byte;
}

static void
match(struct snag_vm_search *search, const struct origin *origin)
{
    struct snag_vm_anchor at = {.seq = origin->seq, .byte = origin->byte,
        .heading = search->heading, .order = search->order};
    if (*search->handle) {
        (void)snprintf(at.key, sizeof(at.key), "event/%llu/output",
            (unsigned long long)at.seq);
    } else (void)snprintf(at.key, sizeof(at.key), "%s", search->key);
    int relation = compare(&at, &search->start);
    if (search->reverse ? relation < 0 : relation > 0) {
        if (!search->found || (search->reverse ? compare(&at, &search->best) > 0 :
            compare(&at, &search->best) < 0)) search->best = at;
        search->found = true;
    }
    if (!search->wrapped || (search->reverse ? compare(&at, &search->fallback) > 0 :
        compare(&at, &search->fallback) < 0)) search->fallback = at;
    search->wrapped = true;
}

static int
search_text(struct snag_vm_search *search, const json_t *block, const char *text,
    bool (*cancel)(void *), void *opaque)
{
    uint64_t seq = (uint64_t)json_integer_value(json_object_get(block, "seq"));
    size_t length = text ? strlen(text) : 0u;
    if (search->heading && length && text[length - 1u] == '\n') --length;
    const char *kind = snag_json_string(block, "kind");
    bool prose = !search->heading && kind && snag_string_in(kind, "assistant refusal");
    for (size_t at = 0u; at < length;) {
        if (cancel && cancel(opaque)) return snag_errno(ECANCELED);
        uint32_t cp, folded[3];
        size_t bytes = snag_utf8_decode((const unsigned char *)text + at, length - at, &cp);
        if (!bytes) return snag_errno(EILSEQ);
        if (prose && !snag_vm_source_mapped(block, at)) { at += bytes; continue; }
        folded[0] = cp;
        unsigned int count = search->ignorecase ? snag_unicode_casefold(cp, folded) : 1u;
        uint64_t source = search->heading ? at : snag_vm_source_position(block, at, true);
        for (unsigned int i = 0u; i < count; ++i) {
            search->origins[search->slot] = (struct origin){seq, source, i == 0u};
            search->slot = (search->slot + 1u) % search->count;
            while (search->matched && folded[i] != search->pattern[search->matched])
                search->matched = search->prefix[search->matched - 1u];
            if (folded[i] == search->pattern[search->matched]) ++search->matched;
            if (search->matched == search->count) {
                const struct origin *origin = search->origins + search->slot;
                if (origin->first && i + 1u == count) match(search, origin);
                search->matched = search->prefix[search->matched - 1u];
            }
        }
        at += bytes;
    }
    return 0;
}

int
snag_vm_search_block(struct snag_vm_search *search, const json_t *block,
    bool (*cancel)(void *), void *opaque)
{
    const char *text = snag_vm_block_text(block, false);
    const char *key = snag_json_string(block, "key");
    const char *handle = snag_json_string(block, "handle");
    uint64_t seq, begin, end;
    if (!search || !text || !key || strlen(key) >= sizeof(search->key) ||
        snag_json_integer_u64(block, "seq", &seq) < 0 ||
        snag_json_integer_u64(block, "source_begin", &begin) < 0 ||
        snag_json_integer_u64(block, "source_end", &end) < 0) return snag_errno(EINVAL);
    unsigned int stream = (unsigned int)json_integer_value(json_object_get(block, "stream"));
    bool contiguous = begin == search->end &&
        (handle ? !strcmp(handle, search->handle) && stream == search->stream :
         !strcmp(key, search->key));
    (void)snprintf(search->key, sizeof(search->key), "%s", key);
    (void)snprintf(search->handle, sizeof(search->handle), "%s", handle ? handle : "");
    search->stream = stream;
    search->end = end;
    search->order = snag_vm_search_order(block);
    if (!contiguous) {
        search->matched = 0u;
        search->heading = true;
        if (search_text(search, block, snag_vm_block_text(block, true), cancel, opaque) < 0)
            return -1;
        search->matched = 0u;
    }
    search->heading = false;
    return search_text(search, block, text, cancel, opaque);
}

uint64_t
snag_vm_search_order(const json_t *block)
{
    const char *kind = snag_json_string(block, "kind");
    return kind && !strcmp(kind, "event") ? UINT64_MAX :
        (uint64_t)json_integer_value(json_object_get(block, "ordinal"));
}

bool
snag_vm_search_result(const struct snag_vm_search *search, struct snag_vm_anchor *at, bool *wrapped)
{
    if (!search || (!search->found && !search->wrapped)) return false;
    *at = search->found ? search->best : search->fallback;
    *wrapped = !search->found;
    return true;
}
