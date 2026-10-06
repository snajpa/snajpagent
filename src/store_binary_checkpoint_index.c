/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_index.h"
#include "store_binary_event.h"
#include "store_binary_wire.h"

#include <errno.h>
#include <string.h>

static void
put_le(unsigned char *out, uint64_t value, size_t width)
{
    for (size_t i = 0u; i < width; ++i) {
        out[i] = (unsigned char)value;
        value >>= 8u;
    }
}

static uint64_t
get_le(const unsigned char *bytes, size_t width)
{
    uint64_t value = 0u;
    for (size_t i = 0u; i < width; ++i) value |= (uint64_t)bytes[i] << (8u * i);
    return value;
}

static bool
boundary_valid(const struct snag_binary_anchor *boundary)
{
    if (!boundary || !boundary->next_seq || boundary->turns >= boundary->next_seq ||
        boundary->end < SNAG_BINARY_HEADER_SIZE || boundary->end > INT64_MAX) {
        return false;
    }
    if (boundary->end == SNAG_BINARY_HEADER_SIZE) {
        return boundary->next_seq == 1u && !boundary->turns && !boundary->previous;
    }
    return boundary->next_seq > 1u && boundary->previous >= SNAG_BINARY_HEADER_SIZE &&
        boundary->previous < boundary->end;
}

static bool
tree_valid(const struct snag_binary_index_tree *tree, const struct snag_binary_anchor *boundary)
{
    if (!tree || tree->count != boundary->next_seq - 1u) return false;
    const unsigned char zero[SNAG_BINARY_INDEX_HASH_SIZE] = {0};
    for (size_t i = 0u; i < 64u; ++i) {
        if (!(tree->count & (UINT64_C(1) << i)) && memcmp(tree->peaks[i], zero, sizeof(zero))) {
            return false;
        }
    }
    return true;
}

/* Entry encoding/decoding additionally checks its own complete shape/checksum.
 * These bounds do not establish membership or working-set completeness. */
static bool
entry_within(const struct snag_binary_index_entry *entry,
    const struct snag_binary_anchor *boundary)
{
    if (entry->sequence >= boundary->next_seq || entry->turn > boundary->turns ||
        entry->batch_offset > boundary->previous || entry->batch_offset >= boundary->end) {
        return false;
    }
    size_t minimum;
    return snag_binary_wire_size((size_t)entry->record_offset + SNAG_BINARY_RECORD_HEADER_SIZE +
        SNAG_BINARY_BATCH_FOOTER_SIZE, &minimum) == 0 &&
        minimum <= boundary->end - entry->batch_offset;
}

int
snag_binary_checkpoint_index_encode(struct snag_buf *out,
    const struct snag_binary_identity *identity, const struct snag_binary_anchor *boundary,
    const struct snag_binary_index_tree *tree, const struct snag_binary_index_entry *entries,
    size_t count)
{
    if (!out || !identity || !boundary_valid(boundary) || !tree_valid(tree, boundary) ||
        (!entries && count)) {
        return snag_errno(EINVAL);
    }
    const size_t header_size = SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE;
    if (count > (SIZE_MAX - header_size) / SNAG_BINARY_INDEX_ENTRY_SIZE) {
        return snag_errno(EOVERFLOW);
    }
    size_t size = header_size + count * SNAG_BINARY_INDEX_ENTRY_SIZE;
    struct snag_buf encoded = {.max = size};
    unsigned char header[SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE] = {0};
    put_le(header, 1u, 2u);
    put_le(header + 4u, 2u, 2u);
    put_le(header + 8u, tree->count, 8u);
    put_le(header + 16u, count, 8u);
    memcpy(header + 24u, tree->peaks, sizeof(tree->peaks));
    int rc = snag_buf_append(&encoded, header, sizeof(header));
    uint64_t previous = 0u;
    for (size_t i = 0u; rc == 0 && i < count; ++i) {
        unsigned char bytes[SNAG_BINARY_INDEX_ENTRY_SIZE];
        if (entries[i].sequence <= previous ||
            snag_binary_index_entry_encode(bytes, identity, &entries[i]) < 0 ||
            !entry_within(&entries[i], boundary)) {
            rc = snag_errno(EINVAL);
            break;
        }
        rc = snag_buf_append(&encoded, bytes, sizeof(bytes));
        previous = entries[i].sequence;
    }
    if (rc == 0) rc = snag_buf_append(out, encoded.data, encoded.len);
    snag_buf_free(&encoded);
    return rc;
}

int
snag_binary_checkpoint_index_decode(const void *data, size_t size,
    const struct snag_binary_identity *identity, const struct snag_binary_anchor *boundary,
    const unsigned char root[32], struct snag_binary_checkpoint_index *out)
{
    if (!data || !identity || !boundary_valid(boundary) || !root || !out ||
        size < SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE) {
        return snag_errno(EINVAL);
    }
    const unsigned char *bytes = data;
    size_t entry_size = size - SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE;
    if (get_le(bytes, 2u) != 1u || get_le(bytes + 2u, 2u) ||
        get_le(bytes + 4u, 2u) != 2u || get_le(bytes + 6u, 2u) ||
        entry_size % SNAG_BINARY_INDEX_ENTRY_SIZE ||
        get_le(bytes + 16u, 8u) != entry_size / SNAG_BINARY_INDEX_ENTRY_SIZE) {
        return snag_errno(EINVAL);
    }
    struct snag_binary_checkpoint_index decoded = {
        .identity = *identity, .boundary = *boundary,
        .tree = {.count = get_le(bytes + 8u, 8u)},
        .entries = bytes + SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE,
        .entry_count = entry_size / SNAG_BINARY_INDEX_ENTRY_SIZE
    };
    memcpy(decoded.tree.peaks, bytes + 24u, sizeof(decoded.tree.peaks));
    unsigned char computed[32];
    if (!tree_valid(&decoded.tree, boundary) ||
        snag_binary_index_tree_root(&decoded.tree, computed) < 0 || memcmp(computed, root, 32u)) {
        return snag_errno(EINVAL);
    }
    uint64_t previous = 0u;
    for (size_t i = 0u; i < decoded.entry_count; ++i) {
        const unsigned char *entry_bytes = decoded.entries + i * SNAG_BINARY_INDEX_ENTRY_SIZE;
        uint64_t sequence = get_le(entry_bytes, 8u);
        struct snag_binary_index_entry entry;
        if (sequence <= previous || snag_binary_index_entry_decode(entry_bytes,
                SNAG_BINARY_INDEX_ENTRY_SIZE, identity, sequence, &entry) < 0 ||
            !entry_within(&entry, boundary)) {
            return snag_errno(EINVAL);
        }
        previous = sequence;
    }
    *out = decoded;
    return 0;
}

int
snag_binary_checkpoint_index_copy(struct snag_buf *out,
    const struct snag_binary_checkpoint_index *source)
{
    if (!out || !source || (!source->entries && source->entry_count)) return snag_errno(EINVAL);
    if (source->entry_count >
        (SIZE_MAX - SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE) / SNAG_BINARY_INDEX_ENTRY_SIZE) {
        return snag_errno(EOVERFLOW);
    }
    struct snag_buf staged = {.max = SIZE_MAX};
    int rc = snag_binary_checkpoint_index_encode(&staged, &source->identity,
        &source->boundary, &source->tree, NULL, 0u);
    if (!rc) {
        put_le(staged.data + 16u, source->entry_count, 8u);
        rc = snag_buf_append(&staged, source->entries,
            source->entry_count * SNAG_BINARY_INDEX_ENTRY_SIZE);
    }
    unsigned char root[32];
    struct snag_binary_checkpoint_index verified;
    if (!rc) rc = snag_binary_index_tree_root(&source->tree, root);
    if (!rc) rc = snag_binary_checkpoint_index_decode(staged.data, staged.len,
        &source->identity, &source->boundary, root, &verified);
    if (!rc) rc = snag_buf_append(out, staged.data, staged.len);
    int saved = errno;
    snag_buf_free(&staged);
    errno = saved;
    return rc;
}

static bool
access_within(const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access)
{
    if (access && (!boundary_valid(&access->boundary) ||
        access->tree.count != access->boundary.next_seq - 1u)) {
        return false;
    }
    if (access) {
        const struct snag_binary_anchor *captured = &access->boundary;
        if (through->end == captured->end && (through->next_seq != captured->next_seq ||
            through->turns != captured->turns || through->previous != captured->previous ||
            memcmp(through->digest, captured->digest, 32u))) {
            return false;
        }
        if (through->end < captured->end && (through->next_seq >= captured->next_seq ||
            through->turns > captured->turns)) {
            return false;
        }
        if (through->end > captured->end && (through->next_seq <= captured->next_seq ||
            through->turns < captured->turns)) {
            return false;
        }
    }
    return true;
}

static size_t
lower_bound(const struct snag_binary_checkpoint_index *index, uint64_t sequence)
{
    size_t low = 0u, high = index->entry_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        const unsigned char *bytes = index->entries + middle * SNAG_BINARY_INDEX_ENTRY_SIZE;
        if (get_le(bytes, 8u) < sequence) low = middle + 1u;
        else high = middle;
    }
    return low;
}

int
snag_binary_checkpoint_batch_find(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access, uint64_t sequence,
    struct snag_buf *scratch, struct snag_binary_batch *batch, struct snag_binary_anchor *before)
{
    if (fd < 0 || !boundary_valid(through) || !sequence || sequence >= through->next_seq ||
        !scratch || !batch || !before) {
        return snag_errno(EINVAL);
    }
    if (!access_within(through, access)) return snag_errno(EINVAL);
    if (!access || sequence >= access->boundary.next_seq) {
        return snag_binary_batch_find(fd, through, sequence, scratch, batch, before);
    }
    struct snag_binary_index_entry entry;
    int rc = snag_binary_checkpoint_index_find(access, sequence, &entry);
    if (rc != 0) return rc < 0 ? rc : snag_errno(ENOENT);
    struct snag_binary_batch found;
    struct snag_binary_anchor previous, after;
    struct snag_binary_record record;
    if (snag_binary_batch_at(fd, through->end, entry.batch_offset, entry.batch_digest,
            scratch, &found, &previous, &after) < 0 ||
        snag_binary_index_resolve(&entry, &previous, &after, found.data, found.size, &record) < 0) {
        return -1;
    }
    *batch = found;
    *before = previous;
    return 0;
}

static bool
cancelled_read(bool (*cancelled)(void *), void *opaque)
{
    if (!cancelled || !cancelled(opaque)) return false;
    errno = ECANCELED;
    return true;
}

static int
visit_record(int (*visit)(void *, const struct snag_binary_record *, uint64_t),
    void *opaque, const struct snag_binary_record *record, uint64_t sequence)
{
    int rc = visit(opaque, record, sequence);
    return rc > 0 ? snag_errno(EINVAL) : rc;
}

static bool
batch_within(const struct snag_binary_anchor *after, const struct snag_binary_anchor *through)
{
    if (after->end > through->end || after->next_seq > through->next_seq ||
        after->turns > through->turns) return false;
    if (after->end == through->end || after->next_seq == through->next_seq) {
        return after->end == through->end && after->next_seq == through->next_seq &&
            after->turns == through->turns && after->previous == through->previous &&
            !memcmp(after->digest, through->digest, 32u);
    }
    return true;
}

static int
advance_turn(const struct snag_binary_record *record, uint64_t *turn)
{
    if (record->kind != SNAG_BINARY_TURN_STARTED) return 0;
    struct snag_binary_event event;
    if (*turn == UINT64_MAX || snag_binary_event_decode(record, &event) != 0 ||
        event.data.started.number != *turn + 1u) return snag_errno(EINVAL);
    ++*turn;
    return 0;
}

int
snag_binary_cursor_capture(const struct snag_binary_anchor *before,
    const struct snag_binary_anchor *after, const struct snag_binary_batch *batch,
    uint64_t next_seq, struct snag_binary_cursor *out)
{
    if (!boundary_valid(before) || !boundary_valid(after) || !batch || !batch->data || !out ||
        next_seq < before->next_seq || next_seq > after->next_seq) return snag_errno(EINVAL);
    struct snag_binary_batch checked;
    struct snag_binary_anchor found;
    int rc = snag_binary_batch_decode(batch->data, batch->size, before, &checked, &found);
    if (rc != 0) return rc < 0 ? rc : snag_errno(EINVAL);
    if (checked.size != batch->size || checked.count != batch->count ||
        checked.first_seq != batch->first_seq || !batch_within(&found, after) ||
        found.end != after->end) return snag_errno(EINVAL);
    size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
    uint64_t sequence, turn = before->turns;
    struct snag_binary_record record;
    struct snag_binary_cursor captured = {.before = *after, .next_seq = after->next_seq,
        .record_offset = SNAG_BINARY_BATCH_HEADER_SIZE};
    for (;;) {
        size_t start = offset;
        rc = snag_binary_record_next(&checked, &offset, &record, &sequence);
        if (rc != 0) break;
        if (advance_turn(&record, &turn) < 0) return -1;
        if (sequence == next_seq) captured = (struct snag_binary_cursor){.before = *before,
            .next_seq = next_seq, .record_offset = (uint32_t)start};
    }
    if (rc < 0) return -1;
    if (turn != after->turns) return snag_errno(EINVAL);
    *out = captured;
    return 0;
}

static int
cursor_member(const struct snag_binary_checkpoint_index *access, int index_fd,
    const unsigned char *root, uint64_t count, uint64_t sequence,
    struct snag_binary_index_entry *out)
{
    int found = snag_binary_checkpoint_index_find(access, sequence, out);
    if (found <= 0) return found;
    if (index_fd < 0) return snag_errno(ENOENT);
    found = snag_binary_index_read_verified(index_fd, &access->identity,
        count, root, sequence, out);
    return found > 0 ? snag_errno(ENOENT) : found;
}

static int
cursor_read(int fd, int index_fd, const unsigned char *root,
    const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access, uint64_t end,
    struct snag_binary_cursor *cursor,
    int (*visit)(void *, const struct snag_binary_record *, uint64_t,
        const struct snag_binary_cursor *), bool (*cancelled)(void *), void *opaque)
{
    if (fd < 0 || !boundary_valid(through) || !cursor || !boundary_valid(&cursor->before) ||
        !batch_within(&cursor->before, through) || !visit || !access_within(through, access) ||
        (access && ((!access->entries && access->entry_count) ||
            access->entry_count > SIZE_MAX / SNAG_BINARY_INDEX_ENTRY_SIZE)) ||
        cursor->next_seq < cursor->before.next_seq || cursor->next_seq > end ||
        end > through->next_seq || cursor->record_offset < SNAG_BINARY_BATCH_HEADER_SIZE ||
        cursor->record_offset > SNAG_BINARY_BATCH_MAX - SNAG_BINARY_BATCH_FOOTER_SIZE ||
        (cursor->next_seq == cursor->before.next_seq &&
            cursor->record_offset != SNAG_BINARY_BATCH_HEADER_SIZE)) return snag_errno(EINVAL);
    if (cancelled_read(cancelled, opaque)) return -1;
    struct snag_binary_cursor staged = *cursor;
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor after;
    int rc = -1;
    while (staged.next_seq < end || staged.next_seq != staged.before.next_seq) {
        if (cancelled_read(cancelled, opaque)) goto done;
        if (access && staged.next_seq < access->boundary.next_seq) {
            struct snag_binary_index_entry entry;
            if (cursor_member(access, index_fd, root, through->next_seq - 1u,
                staged.next_seq, &entry) < 0) {
                goto done;
            }
            if (entry.batch_offset != staged.before.end) { errno = EINVAL; goto done; }
        }
        int read_rc = snag_binary_batch_read(fd, through->end, &staged.before,
            &scratch, &batch, &after);
        if (read_rc != 0) {
            if (read_rc > 0) errno = EIO;
            goto done;
        }
        if (!batch_within(&after, through) || staged.next_seq >= after.next_seq) {
            errno = EINVAL;
            goto done;
        }
        struct snag_binary_anchor before = staged.before;
        uint64_t first = staged.next_seq, turn = before.turns;
        uint32_t wanted_offset = staged.record_offset;
        bool matched = false;
        size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
        struct snag_binary_record record;
        uint64_t sequence;
        int next;
        for (;;) {
            size_t start = offset;
            next = snag_binary_record_next(&batch, &offset, &record, &sequence);
            if (next != 0) break;
            if (sequence == first) {
                if (start != wanted_offset) { errno = EINVAL; goto done; }
                matched = true;
            }
            if (cancelled_read(cancelled, opaque) || advance_turn(&record, &turn) < 0) goto done;
            if (sequence < first || sequence >= end) continue;
            if (access && sequence < access->boundary.next_seq) {
                struct snag_binary_index_entry entry;
                if (cursor_member(access, index_fd, root, through->next_seq - 1u,
                    sequence, &entry) < 0) {
                    goto done;
                }
                if (entry.batch_offset != before.end || entry.record_offset != start ||
                    entry.kind != record.kind || entry.turn != turn ||
                    memcmp(entry.batch_digest, after.digest, sizeof(after.digest))) {
                    errno = EINVAL;
                    goto done;
                }
            }
            struct snag_binary_cursor cut = {.before = before, .next_seq = sequence + 1u,
                .record_offset = (uint32_t)offset};
            if (cut.next_seq == after.next_seq) cut = (struct snag_binary_cursor){.before = after,
                .next_seq = after.next_seq, .record_offset = SNAG_BINARY_BATCH_HEADER_SIZE};
            int result = visit(opaque, &record, sequence, &cut);
            if (result < 0) goto done;
            if (result > 1) { errno = EINVAL; goto done; }
            staged = cut;
            if (result == 1) { rc = 1; goto adopt; }
        }
        if (next < 0) goto done;
        if (!matched || turn != after.turns) { errno = EINVAL; goto done; }
        if (staged.next_seq == end) break;
        if (staged.before.end != after.end) { errno = EINVAL; goto done; }
    }
    rc = 0;
adopt:
    if (cancelled_read(cancelled, opaque)) { rc = -1; goto done; }
    *cursor = staged;
done:
    snag_buf_free(&scratch);
    return rc;
}

int
snag_binary_cursor_read(int fd, const struct snag_binary_anchor *through, uint64_t end,
    struct snag_binary_cursor *cursor,
    int (*visit)(void *, const struct snag_binary_record *, uint64_t,
        const struct snag_binary_cursor *), bool (*cancelled)(void *), void *opaque)
{
    return cursor_read(fd, -1, NULL, through, NULL, end, cursor, visit, cancelled, opaque);
}

int
snag_binary_checkpoint_cursor_read(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access, uint64_t end,
    struct snag_binary_cursor *cursor,
    int (*visit)(void *, const struct snag_binary_record *, uint64_t,
        const struct snag_binary_cursor *), bool (*cancelled)(void *), void *opaque)
{
    if (!access) return snag_errno(EINVAL);
    return cursor_read(fd, -1, NULL, through, access, end, cursor, visit, cancelled, opaque);
}

int
snag_binary_checkpoint_query_cursor_read(int fd, int index_fd,
    const struct snag_binary_anchor *through, const struct snag_binary_checkpoint_index *access,
    const struct snag_binary_index_tree *frontier, uint64_t end, struct snag_binary_cursor *cursor,
    int (*visit)(void *, const struct snag_binary_record *, uint64_t,
        const struct snag_binary_cursor *), bool (*cancelled)(void *), void *opaque)
{
    if (index_fd < -1 || !boundary_valid(through) || !access || !frontier ||
        frontier->count != through->next_seq - 1u) {
        return snag_errno(EINVAL);
    }
    unsigned char root[32];
    if (snag_binary_index_tree_root(frontier, root) < 0) return -1;
    return cursor_read(fd, index_fd, root, through, access, end, cursor,
        visit, cancelled, opaque);
}

struct contiguous_visit {
    int (*visit)(void *, const struct snag_binary_record *, uint64_t);
    bool (*cancelled)(void *);
    void *opaque;
    uint64_t first;
};

static int
contiguous_visit(void *opaque, const struct snag_binary_record *record, uint64_t sequence,
    const struct snag_binary_cursor *after)
{
    (void)after;
    struct contiguous_visit *state = opaque;
    if (sequence < state->first) return 0;
    return visit_record(state->visit, state->opaque, record, sequence);
}

static bool
contiguous_cancelled(void *opaque)
{
    struct contiguous_visit *state = opaque;
    return state->cancelled && state->cancelled(state->opaque);
}

static int
contiguous_records(int fd, const struct snag_binary_anchor *through,
    uint64_t first, uint64_t end,
    int (*visit)(void *, const struct snag_binary_record *, uint64_t),
    bool (*cancelled)(void *), void *opaque)
{
    if (first == end) return 0;
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor before;
    struct snag_binary_cursor cursor;
    int rc = -1;
    if (cancelled_read(cancelled, opaque) ||
        snag_binary_batch_find(fd, through, first, &scratch, &batch, &before) < 0) goto done;
    cursor = (struct snag_binary_cursor){.before = before, .next_seq = before.next_seq,
        .record_offset = SNAG_BINARY_BATCH_HEADER_SIZE};
    struct contiguous_visit state = {.visit = visit, .cancelled = cancelled, .opaque = opaque,
        .first = first};
    rc = snag_binary_cursor_read(fd, through, end, &cursor,
        contiguous_visit, contiguous_cancelled, &state);
done:
    snag_buf_free(&scratch);
    return rc;
}

/* Walk each selected physical batch once. Check every table entry against its
 * exact canonical position/kind/turn without repeatedly hashing that batch. */
static int
selected_records(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access, uint64_t first, uint64_t end,
    int (*visit)(void *, const struct snag_binary_record *, uint64_t),
    bool (*cancelled)(void *), void *opaque)
{
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    size_t selected = lower_bound(access, first);
    int rc = -1;
    while (selected < access->entry_count) {
        const unsigned char *bytes = access->entries + selected * SNAG_BINARY_INDEX_ENTRY_SIZE;
        uint64_t wanted = get_le(bytes, 8u);
        if (wanted >= end) break;
        if (cancelled_read(cancelled, opaque)) goto done;
        struct snag_binary_index_entry entry;
        if (snag_binary_index_entry_decode(bytes, SNAG_BINARY_INDEX_ENTRY_SIZE,
                &access->identity, wanted, &entry) < 0) goto done;
        struct snag_binary_batch batch;
        struct snag_binary_anchor before, after;
        if (snag_binary_batch_at(fd, through->end, entry.batch_offset, entry.batch_digest,
                &scratch, &batch, &before, &after) < 0) goto done;
        if (!batch_within(&after, through) || !batch_within(&after, &access->boundary)) {
            errno = EINVAL;
            goto done;
        }
        size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE, old_selected = selected;
        uint64_t turn = before.turns, sequence;
        struct snag_binary_record record;
        int next;
        for (;;) {
            size_t record_offset = offset;
            next = snag_binary_record_next(&batch, &offset, &record, &sequence);
            if (next != 0) break;
            if (cancelled_read(cancelled, opaque) || advance_turn(&record, &turn) < 0) goto done;
            if (selected == access->entry_count) continue;
            bytes = access->entries + selected * SNAG_BINARY_INDEX_ENTRY_SIZE;
            wanted = get_le(bytes, 8u);
            if (wanted >= end) continue;
            if (wanted < sequence) {
                errno = EINVAL;
                goto done;
            }
            if (wanted != sequence) continue;
            if (snag_binary_index_entry_decode(bytes, SNAG_BINARY_INDEX_ENTRY_SIZE,
                    &access->identity, wanted, &entry) < 0) goto done;
            if (entry.batch_offset != before.end || entry.record_offset != record_offset ||
                entry.kind != record.kind || entry.turn != turn ||
                memcmp(entry.batch_digest, after.digest, 32u)) {
                errno = EINVAL;
                goto done;
            }
            if (visit_record(visit, opaque, &record, sequence) < 0) goto done;
            ++selected;
        }
        if (next < 0) goto done;
        if (turn != after.turns || old_selected == selected) {
            errno = EINVAL;
            goto done;
        }
    }
    rc = 0;
done:
    snag_buf_free(&scratch);
    return rc;
}

int
snag_binary_checkpoint_records_read(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access, uint64_t first, uint64_t end,
    int (*visit)(void *, const struct snag_binary_record *, uint64_t),
    bool (*cancelled)(void *), void *opaque)
{
    if (fd < 0 || !boundary_valid(through) || !first || first > end ||
        end > through->next_seq || !visit || !access_within(through, access) ||
        (access && ((!access->entries && access->entry_count) ||
            access->entry_count > SIZE_MAX / SNAG_BINARY_INDEX_ENTRY_SIZE))) {
        return snag_errno(EINVAL);
    }
    if (cancelled_read(cancelled, opaque)) return -1;
    if (!access) return contiguous_records(fd, through, first, end, visit, cancelled, opaque);
    uint64_t captured_end = end < access->boundary.next_seq ? end : access->boundary.next_seq;
    if (first < captured_end && selected_records(fd, through, access,
            first, captured_end, visit, cancelled, opaque) < 0) return -1;
    if (first < captured_end) first = captured_end;
    return contiguous_records(fd, through, first, end, visit, cancelled, opaque);
}

int
snag_binary_checkpoint_index_find(const struct snag_binary_checkpoint_index *index,
    uint64_t sequence, struct snag_binary_index_entry *out)
{
    if (!index || !out || !sequence || (!index->entries && index->entry_count)) {
        return snag_errno(EINVAL);
    }
    size_t low = lower_bound(index, sequence);
    if (low == index->entry_count) return 1;
    const unsigned char *bytes = index->entries + low * SNAG_BINARY_INDEX_ENTRY_SIZE;
    if (get_le(bytes, 8u) != sequence) return 1;
    return snag_binary_index_entry_decode(bytes, SNAG_BINARY_INDEX_ENTRY_SIZE,
        &index->identity, sequence, out);
}
