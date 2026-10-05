/* SPDX-License-Identifier: GPL-2.0-only */
#include "fixture_store_binary.h"
#include "context.h"
#include "fs.h"
#include "json.h"
#include "snajpagent.h"
#include "store_binary_context.h"
#include "store_binary_import.h"
#include "store_internal.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned int compared, without_journal, prefix_compared, prefix_rejected;
static bool checked_failures, checked_source_failures;
static unsigned int warm_compared, checkpoint_compared, checkpoint_rejected;
static unsigned int checkpoint_voice_rejected;
static unsigned int direct_compared, direct_rejected, direct_uninspected;
static unsigned int bounded_suffix_compared, bounded_suffix_rejected;
static unsigned int checkpoint_late_rejected, suffix_compared;
static unsigned int suffix_late_rejected, suffix_bad_rejected, suffix_tails;
static unsigned int checkpoint_rewrite_rejected;
static unsigned int suffix_open, suffix_process, suffix_queue, suffix_download, suffix_voice;

typedef int (*checkpoint_reader)(struct snag_session *, struct snag_session *,
    const struct snag_binary_anchor *, const void *, size_t, struct snag_binary_recovery *,
    struct snag_binary_checkpoint_sources *, const struct snag_context_control *, char *, size_t);

static void checkpoint_late_failures(struct snag_session *, struct snag_session *,
    struct snag_binary_checkpoint_sources *, const struct snag_binary_anchor *,
    const void *, size_t, checkpoint_reader, uint64_t);
static void suffix_failure_paths(struct snag_session *, struct snag_session *,
    struct snag_binary_checkpoint_sources *, const struct snag_binary_anchor *,
    const struct snag_binary_anchor *, const void *, size_t);

static void
same_document(const struct snag_json_document *a, const struct snag_json_document *b)
{
    const json_t *items_a = json_object_get(a->value, "items");
    const json_t *items_b = json_object_get(b->value, "items");
    if (json_is_array(items_a) && json_is_array(items_b) && !json_equal(items_a, items_b)) {
        size_t count = json_array_size(items_a);
        if (count < json_array_size(items_b)) count = json_array_size(items_b);
        for (size_t i = 0u; i < count; ++i) {
            json_t *left = json_array_get(items_a, i);
            json_t *right = json_array_get(items_b, i);
            if (left == right || (left && right && json_equal(left, right))) continue;
            fprintf(stderr, "projection[%u] differs at item %zu (%zu versus %zu items)\n",
                compared, i, json_array_size(items_a), json_array_size(items_b));
            break;
        }
    }
    assert(a->value == b->value || (a->value && b->value && json_equal(a->value, b->value)));
    assert(a->bytes == b->bytes && !strcmp(a->sha256, b->sha256));
}

static void
same_projection(const struct snag_context_projection *a, const struct snag_context_projection *b)
{
    same_document(&a->model_input, &b->model_input);
    same_document(&a->create_request, &b->create_request);
    same_document(&a->count_request, &b->count_request);
    assert(a->host_context == b->host_context || json_equal(a->host_context, b->host_context));
    assert(!strcmp(a->continuation_scope, b->continuation_scope));
    assert(!strcmp(a->request_input_sha256, b->request_input_sha256));
    assert(a->request_input_bytes == b->request_input_bytes);
    assert(a->request_input_count == b->request_input_count);
    assert(a->request_controller_count == b->request_controller_count);
    assert(a->input_tokens_bound == b->input_tokens_bound);
    assert(a->irc_seq == b->irc_seq && a->source_seq == b->source_seq);
}

static json_t *
logical_cache(json_t *cache)
{
    /* Cross-format cursors have independently validated physical coordinates.
     * Compare their logical fields without mutating shared capture payloads. */
    json_t *copy = json_deep_copy(cache);
    assert(copy);
    json_decref(cache);
    json_t *recent = json_object_get(copy, "recent");
    for (size_t i = 0u; i < json_array_size(recent); ++i) {
        json_t *event = json_array_get(recent, i);
        const char *type = snag_json_string(event, "type");
        if (!type || strcmp(type, "voice_transfer_adopted")) continue;
        json_t *data = json_object_get(event, "data");
        assert(json_is_integer(json_object_get(data, "begin_offset")));
        assert(snag_hex_is_lower(snag_json_string(data, "begin_sha256"), SNAG_SHA256_HEX_LEN));
        assert(!json_object_del(data, "begin_offset"));
        assert(!json_object_del(data, "begin_sha256"));
    }
    return copy;
}

static void
same_cache(struct snag_session *a, struct snag_session *b, bool relocated)
{
    assert(a->on_checkpoint && b->on_checkpoint);
    json_t *left = a->on_checkpoint(a->on_commit_opaque, a);
    json_t *right = b->on_checkpoint(b->on_commit_opaque, b);
    assert(left && right);
    if (relocated) {
        left = logical_cache(left);
        right = logical_cache(right);
    }
    if (json_is_true(json_object_get(left, "rebuild_images")) &&
        json_is_true(json_object_get(right, "rebuild_images"))) {
        /* A rebuild consumes recent, not the incremental pending cursor. The
         * loaded recipe initially marks its whole seam pending. Everything else
         * must match; warm caches retain exact cursor comparison below. */
        assert(json_object_del(left, "pending_first_seq") == 0);
        assert(json_object_del(right, "pending_first_seq") == 0);
    }
    assert(json_equal(left, right));
    json_decref(left);
    json_decref(right);
}

static void
provider_number(unsigned char *out, uint64_t number)
{
    for (size_t i = 0u; i < 8u; ++i) out[i] = (unsigned char)(number >> (i * 8u));
}

struct provider_cancel {
    size_t calls, fail_at;
};

static bool
cancel_provider(void *opaque)
{
    struct provider_cancel *cancel = opaque;
    return ++cancel->calls == cancel->fail_at;
}

static void
provider_source_checks(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_sources *origins, const struct snag_session *state,
    const json_t *recent, const json_t *history, const struct snag_buf *provider)
{
    struct snag_buf access_bytes = {.max = SIZE_MAX};
    struct snag_binary_checkpoint_index access;
    binary_fixture_access(fd, through, &access_bytes, &access);
    struct snag_buf selected = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_access_capture(fd, through, &access, &access.tree,
        origins, state, provider->data, provider->len, NULL, NULL, &selected));
    unsigned char root[32];
    assert(!snag_binary_index_tree_root(&access.tree, root));
    struct snag_binary_checkpoint_index captured;
    assert(!snag_binary_checkpoint_index_decode(selected.data, selected.len,
        &access.identity, through, root, &captured));
    int64_t position = snag_seek(fd, 0, SEEK_CUR);
    for (unsigned int mode = 0u; mode < 3u; ++mode) {
        json_t *events = NULL;
        json_t *sources = NULL;
        const struct snag_binary_checkpoint_index *chosen = mode == 2u ? &captured :
            mode ? &access : NULL;
        int rc = snag_binary_checkpoint_provider_read(fd, through, chosen,
            provider->data, provider->len, NULL, NULL, &events, &sources);
        if (rc < 0) fprintf(stderr, "provider source mode %u: %d\n", mode, errno);
        assert(!rc && json_equal(events, recent) && json_equal(sources, history));
        assert(events != recent && sources != history);
        json_decref(events);
        json_decref(sources);
    }
    struct snag_binary_checkpoint_provider view;
    assert(!snag_binary_checkpoint_provider_decode(provider->data, provider->len, &view));
    json_t *events = (json_t *)&view;
    json_t *sources = (json_t *)&access;
    json_t *old_events = events;
    json_t *old_sources = sources;
    uint64_t required[4];
    assert(!snag_json_integer_u64(json_array_get(recent, 0u), "seq", &required[0]));
    assert(!snag_json_integer_u64(json_array_get(recent, json_array_size(recent) / 2u),
        "seq", &required[1]));
    assert(!snag_json_integer_u64(json_array_get(recent, json_array_size(recent) - 1u),
        "seq", &required[2]));
    required[3] = 0u;
    if (json_array_size(history))
        assert(!snag_json_integer_u64(json_array_get(history, 0u), "seq", &required[3]));
    for (size_t i = 0u; i < sizeof(required) / sizeof(required[0]); ++i) {
        if (!required[i]) continue;
        struct snag_buf omitted = {.max = SIZE_MAX};
        struct snag_binary_checkpoint_index missing;
        binary_fixture_access_omit(&access, required[i], &omitted, &missing);
        errno = 0;
        assert(snag_binary_checkpoint_provider_read(fd, through, &missing,
            provider->data, provider->len, NULL, NULL, &events, &sources) < 0 && errno == ENOENT);
        assert(events == old_events && sources == old_sources);
        snag_buf_free(&omitted);
    }
    static bool cancellation_checked[2];
    unsigned int class_mode = view.history_count ? 1u : 0u;
    if (!cancellation_checked[class_mode]) {
        cancellation_checked[class_mode] = true;
        for (size_t i = 1u; i <= view.recent_count + view.history_count + 1u; ++i) {
            struct provider_cancel cancel = {.fail_at = i};
            errno = 0;
            assert(snag_binary_checkpoint_provider_read(fd, through, &access,
                provider->data, provider->len, cancel_provider, &cancel, &events, &sources) < 0 &&
                errno == ECANCELED && cancel.calls == i);
            assert(events == old_events && sources == old_sources);
        }
        if (view.history_count) {
            struct snag_buf changed = {.max = SIZE_MAX};
            assert(!snag_buf_append(&changed, provider->data, provider->len));
            size_t offset = (size_t)(view.history - (const unsigned char *)provider->data);
            provider_number((unsigned char *)changed.data + offset, 1u);
            assert(snag_binary_checkpoint_provider_read(fd, through, &access,
                changed.data, changed.len, NULL, NULL, &events, &sources) < 0 && errno == EINVAL);
            assert(events == old_events && sources == old_sources);
            snag_buf_free(&changed);
        }
    }
    struct snag_binary_anchor wrong = *through;
    ++wrong.next_seq;
    assert(snag_binary_checkpoint_provider_read(fd, &wrong, &access,
        provider->data, provider->len, NULL, NULL, &events, &sources) < 0 && errno == EINVAL);
    assert(snag_binary_checkpoint_provider_read(-1, through, &access,
        provider->data, provider->len, NULL, NULL, &events, &sources) < 0 && errno == EINVAL);
    assert(snag_binary_checkpoint_provider_read(fd, through, &access,
        provider->data, provider->len, NULL, NULL, &events, &events) < 0 && errno == EINVAL);
    assert(events == old_events && sources == old_sources);
    assert(snag_seek(fd, 0, SEEK_CUR) == position);
    snag_buf_free(&selected);
    snag_buf_free(&access_bytes);
}

static void
provider_codec_checks(struct snag_session *state, const json_t *recent,
    const json_t *history, const struct snag_buf *provider)
{
    struct snag_binary_checkpoint_provider view;
    assert(snag_binary_checkpoint_provider_decode(provider->data, provider->len, &view) == 0);
    assert(view.next_seq == state->next_seq && view.compact_seq == state->compact_seq &&
        view.rebase_seq == state->context_rebase_seq);
    assert(view.recent_count == json_array_size(recent) &&
        view.history_count == json_array_size(history));
    assert(provider->len == 48u + view.recent_count * 36u + view.history_count * 8u);
    json_t *restored_events = NULL, *restored_history = NULL;
    assert(snag_binary_checkpoint_provider_materialize(provider->data, provider->len,
        recent, history, &restored_events, &restored_history) == 0);
    assert(json_equal(restored_events, recent) && json_equal(restored_history, history));
    assert(restored_events != recent && restored_history != history);
    for (size_t i = 0u; i < json_array_size(recent); ++i) {
        json_t *entry = json_array_get(restored_events, i);
        const json_t *original = json_array_get(recent, i);
        assert(entry != original);
        assert(json_object_get(entry, "data") == json_object_get(original, "data"));
    }
    json_t *empty = json_array();
    assert(empty);
    json_t *old_events = restored_events, *old_history = restored_history;
    assert(snag_binary_checkpoint_provider_materialize(provider->data, provider->len,
        empty, history, &restored_events, &restored_history) < 0 && errno == EINVAL);
    assert(restored_events == old_events && restored_history == old_history);
    if (json_array_size(history)) {
        assert(snag_binary_checkpoint_provider_materialize(provider->data, provider->len,
            recent, empty, &restored_events, &restored_history) < 0 && errno == EINVAL);
        assert(restored_events == old_events && restored_history == old_history);
    }
    json_decref(empty);
    json_decref(restored_events);
    json_decref(restored_history);
    static bool classes_checked[2];
    unsigned int class_mode = json_array_size(history) ? 1u : 0u;
    if (!classes_checked[class_mode]) {
        classes_checked[class_mode] = true;
        for (unsigned int variant = 0u; variant < 4u; ++variant) {
            json_t *pool = json_deep_copy(class_mode ? history : recent);
            assert(pool);
            json_t *entry = json_array_get(pool, 0u);
            switch (variant) {
            case 0u: assert(json_object_set_new(entry, "type", json_string("unknown")) == 0); break;
            case 1u:
                assert(json_object_set_new(entry, "data", json_array()) == 0); break;
            case 2u:
                assert(json_object_set_new(entry, "seq", json_integer(0)) == 0); break;
            case 3u:
                assert(json_object_set_new(entry, "type", class_mode ?
                    json_string("session_checkpoint") :
                    json_stringn("session_created\0tail", 20u)) == 0);
                if (class_mode) assert(json_object_set_new(entry, "data",
                    json_pack("{s:b}", "not_marker_metadata", true)) == 0);
                break;
            }
            restored_events = restored_history = NULL;
            assert(snag_binary_checkpoint_provider_materialize(provider->data, provider->len,
                class_mode ? recent : pool, class_mode ? pool : history,
                &restored_events, &restored_history) < 0 && errno == EINVAL);
            assert(!restored_events && !restored_history);
            json_decref(pool);
        }
    }
    static bool checked;
    if (checked) return;
    checked = true;
    unsigned char saved[sizeof(view)];
    memcpy(saved, &view, sizeof(view));
    for (size_t size = 0u; size < provider->len; ++size) {
        assert(snag_binary_checkpoint_provider_decode(provider->data, size, &view) < 0);
        assert(!memcmp(saved, &view, sizeof(view)));
    }
    struct snag_buf copy = {.max = SIZE_MAX};
    assert(snag_buf_append(&copy, provider->data, provider->len) == 0);
    assert(snag_buf_append(&copy, "x", 1u) == 0);
    assert(snag_binary_checkpoint_provider_decode(copy.data, copy.len, &view) < 0);
    assert(!memcmp(saved, &view, sizeof(view)));
    copy.len -= 1u;
    memset(copy.data + 8u, 255, 8u);
    memset(copy.data + 48u + 8u, 255, 8u);
    assert(snag_binary_checkpoint_provider_decode(copy.data, copy.len, &view) == 0);
    assert(view.next_seq == UINT64_MAX);
    restored_events = restored_history = NULL;
    assert(snag_binary_checkpoint_provider_materialize(copy.data, copy.len,
        recent, history, &restored_events, &restored_history) < 0 && errno == EOVERFLOW);
    assert(!restored_events && !restored_history);
    snag_buf_free(&copy);
    struct snag_buf limited = {.max = provider->len};
    assert(snag_buf_append(&limited, "x", 1u) == 0);
    assert(snag_binary_checkpoint_provider_encode(&limited, state, recent, history) < 0);
    assert(limited.len == 1u && limited.data[0] == 'x');
    snag_buf_free(&limited);
    json_t *invalid = json_deep_copy(recent);
    assert(invalid &&
        json_object_set_new(json_array_get(invalid, 0u), "time", json_integer(-1)) == 0);
    copy.max = SIZE_MAX;
    assert(snag_buf_append(&copy, "x", 1u) == 0);
    assert(snag_binary_checkpoint_provider_encode(&copy, state, invalid, history) < 0);
    assert(copy.len == 1u && copy.data[0] == 'x');
    assert(json_object_set(json_array_get(invalid, 0u), "time",
        json_object_get(json_array_get(recent, 0u), "time")) == 0);
    assert(json_object_set_new(json_array_get(invalid, 0u), "turn",
        json_stringn("00000000000000000000000000000000\0x", 34u)) == 0);
    assert(snag_binary_checkpoint_provider_encode(&copy, state, invalid, history) < 0);
    assert(copy.len == 1u && copy.data[0] == 'x');
    json_decref(invalid);
    snag_buf_free(&copy);
}

static void
seal_checkpoint(struct snag_binary_checkpoint_frame *frame,
    struct snag_binary_checkpoint_receipt *receipt, const unsigned char root[32],
    struct snag_buf *bytes)
{
    assert(!snag_binary_checkpoint_frame_encode(bytes, frame));
    *receipt = (struct snag_binary_checkpoint_receipt){.generation = frame->generation,
        .image_size = bytes->len, .boundary = frame->boundary};
    memcpy(receipt->index_root, root, 32u);
    memcpy(receipt->image_digest, bytes->data + bytes->len - 32u, 32u);
    struct snag_binary_identity identity = frame->identity;
    assert(!snag_binary_checkpoint_frame_from_receipt(bytes->data, bytes->len,
        &identity, receipt, frame));
}

static void
reject_materialization(struct snag_session *source,
    const struct snag_binary_checkpoint_frame *frame,
    const struct snag_binary_checkpoint_receipt *receipt,
    const struct snag_context_control *control, int expected_errno)
{
    struct snag_session target;
    snag_session_init(&target);
    target.strings = json_pack("{s:s}", "keep", "old owner");
    assert(target.strings);
    struct snag_session saved;
    memcpy(&saved, &target, sizeof(saved));
    struct snag_binary_checkpoint_sources origins = {.response_start = 1234u};
    struct snag_binary_checkpoint_sources old_origins;
    memcpy(&old_origins, &origins, sizeof(old_origins));
    int64_t position = snag_seek(source->log_fd, 0, SEEK_CUR);
    char error[128] = {0};
    errno = 0;
    assert(snag_store_materialize_binary_context_checkpoint(source, &target, frame, receipt,
        &origins, control, error, sizeof(error)) < 0);
    assert(!expected_errno || errno == expected_errno);
    assert(!strcmp(snag_json_string(target.strings, "keep"), "old owner"));
    assert(!memcmp(&target, &saved, sizeof(target)) &&
        !memcmp(&origins, &old_origins, sizeof(origins)));
    assert(snag_seek(source->log_fd, 0, SEEK_CUR) == position);
    snag_session_close(&target);
    ++direct_rejected;
}

static void
write_source_byte(int fd, int64_t offset, unsigned char byte)
{
    int64_t position = snag_seek(fd, 0, SEEK_CUR);
    assert(snag_seek(fd, offset, SEEK_SET) == offset);
    assert(!snag_write_full(fd, &byte, 1u));
    assert(snag_seek(fd, position, SEEK_SET) == position);
}

struct direct_mutation {
    struct provider_cancel cancel;
    int fd;
    unsigned int kind;
    int64_t size;
    unsigned char first, last;
    bool changed;
};

static bool
mutate_materialization(void *opaque)
{
    struct direct_mutation *mutation = opaque;
    if (++mutation->cancel.calls != mutation->cancel.fail_at) return false;
    mutation->changed = true;
    if (!mutation->kind) {
        write_source_byte(mutation->fd, 0, mutation->first ^ 1u);
    } else {
        assert(!snag_truncate(mutation->fd, mutation->size + (mutation->kind == 1u ? 1 : -1)));
    }
    return false;
}

static void
uninspected_materialization(struct snag_session *source, struct snag_session *expected,
    const struct snag_binary_checkpoint_frame *frame,
    const struct snag_binary_checkpoint_receipt *receipt,
    const struct snag_binary_checkpoint_index *available,
    const struct snag_binary_checkpoint_index *selected)
{
    if (direct_uninspected) return;
    uint64_t *protected = calloc(selected->entry_count * 2u, sizeof(*protected));
    assert(protected);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    for (size_t i = 0u; i < selected->entry_count; ++i) {
        struct snag_binary_index_entry entry;
        uint64_t sequence = 0u;
        for (size_t j = 0u; j < 8u; ++j) {
            unsigned char byte = selected->entries[i * SNAG_BINARY_INDEX_ENTRY_SIZE + j];
            sequence |= (uint64_t)byte << (j * 8u);
        }
        assert(!snag_binary_checkpoint_index_find(selected, sequence, &entry));
        struct snag_binary_batch batch;
        struct snag_binary_anchor before;
        assert(!snag_binary_checkpoint_batch_find(source->log_fd, &frame->boundary, selected,
            sequence, &scratch, &batch, &before));
        protected[i * 2u] = entry.batch_offset;
        protected[i * 2u + 1u] = before.previous;
    }
    uint64_t unused = 0u;
    for (size_t i = 0u; i < available->entry_count; ++i) {
        struct snag_binary_index_entry entry;
        assert(!snag_binary_checkpoint_index_find(available, (uint64_t)i + 1u, &entry));
        bool used = false;
        for (size_t j = 0u; j < selected->entry_count * 2u; ++j)
            used |= protected[j] == entry.batch_offset;
        if (!used) { unused = entry.batch_offset; break; }
    }
    free(protected);
    snag_buf_free(&scratch);
    if (!unused) return;
    unsigned char byte;
    assert(snag_pread(source->log_fd, &byte, 1u, (int64_t)unused + 1) == 1);
    write_source_byte(source->log_fd, (int64_t)unused + 1, byte ^ 1u);
    struct snag_session restored;
    snag_session_init(&restored);
    struct snag_binary_checkpoint_sources origins = {0};
    char error[128] = {0};
    assert(!snag_store_materialize_binary_context_checkpoint(source, &restored, frame, receipt,
        &origins, NULL, error, sizeof(error)));
    same_cache(expected, &restored, false);
    snag_session_close(&restored);
    snag_binary_checkpoint_sources_free(&origins);
    struct snag_binary_recovery recovery;
    snag_session_init(&restored);
    assert(snag_store_reconcile_binary_prefix(source, &restored, &frame->boundary, NULL, NULL,
        &recovery, &origins, error, sizeof(error)) < 0);
    snag_session_close(&restored);
    snag_binary_checkpoint_sources_free(&origins);
    write_source_byte(source->log_fd, (int64_t)unused + 1, byte);
    ++direct_uninspected;
}

static void
joint_materialization_checks(struct snag_session *source, struct snag_session *expected,
    const struct snag_binary_checkpoint_sources *origins,
    const struct snag_binary_checkpoint_frame *base)
{
    struct snag_buf available_bytes = {.max = SIZE_MAX};
    struct snag_binary_checkpoint_index available;
    binary_fixture_access(source->log_fd, &base->boundary, &available_bytes, &available);
    struct snag_buf selected = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_access_capture(source->log_fd, &base->boundary,
        &available, &available.tree, origins, expected, base->provider.data, base->provider.size,
        NULL, NULL, &selected));
    unsigned char root[32];
    assert(!snag_binary_index_tree_root(&available.tree, root));
    struct snag_binary_checkpoint_index access;
    assert(!snag_binary_checkpoint_index_decode(selected.data, selected.len,
        &base->identity, &base->boundary, root, &access));
    struct snag_binary_checkpoint_frame frame = *base;
    frame.access = (struct snag_binary_checkpoint_section){.version = 1u,
        .data = (const unsigned char *)selected.data, .size = selected.len};
    struct snag_binary_checkpoint_receipt receipt;
    struct snag_buf bytes = {.max = SIZE_MAX};
    seal_checkpoint(&frame, &receipt, root, &bytes);
    struct snag_session restored;
    snag_session_init(&restored);
    struct snag_binary_checkpoint_sources adopted = {0};
    struct provider_cancel cancel = {0};
    struct snag_context_control control = {.cancelled = cancel_provider, .opaque = &cancel};
    char error[128] = {0};
    int64_t position = snag_seek(source->log_fd, 0, SEEK_CUR);
    int rc = snag_store_materialize_binary_context_checkpoint(source, &restored, &frame, &receipt,
        &adopted, &control, error, sizeof(error));
    if (rc < 0) fprintf(stderr, "joint materialization: %s (%d)\n", error, errno);
    assert(!rc && snag_seek(source->log_fd, 0, SEEK_CUR) == position);
    assert(restored.dir_fd < 0 && restored.log_fd < 0 && restored.lock_fd < 0 &&
        !restored.pending_log);
    /* Neither the returned core nor its provider capture borrows image bytes. */
    snag_buf_free(&bytes);
    same_cache(expected, &restored, false);
    json_t *left = snag_checkpoint_state_encode(expected);
    json_t *right = snag_checkpoint_state_encode(&restored);
    assert(left && right && json_equal(left, right));
    json_decref(left);
    json_decref(right);
    snag_session_close(&restored);
    snag_binary_checkpoint_sources_free(&adopted);
    ++direct_compared;
    snag_buf_init(&bytes, SIZE_MAX);
    frame = *base;
    frame.access = (struct snag_binary_checkpoint_section){.version = 1u,
        .data = (const unsigned char *)selected.data, .size = selected.len};
    seal_checkpoint(&frame, &receipt, root, &bytes);
    uninspected_materialization(source, expected, &frame, &receipt, &available, &access);
    static bool negatives[2];
    struct snag_binary_checkpoint_provider provider;
    assert(!snag_binary_checkpoint_provider_decode(frame.provider.data, frame.provider.size,
        &provider));
    unsigned int class_mode = provider.history_count ? 1u : 0u;
    if (!negatives[class_mode]) {
        negatives[class_mode] = true;
        for (size_t i = 1u; i <= cancel.calls; ++i) {
            struct provider_cancel stopped = {.fail_at = i};
            control.opaque = &stopped;
            reject_materialization(source, &frame, &receipt, &control, ECANCELED);
            assert(stopped.calls == i);
        }
        for (unsigned int kind = 0u; kind < 3u; ++kind) {
            snag_file_info before;
            assert(!snag_fstat(source->log_fd, &before));
            struct direct_mutation mutation = {.cancel = {.fail_at = cancel.calls},
                .fd = source->log_fd, .kind = kind, .size = before.st_size};
            assert(snag_pread(source->log_fd, &mutation.first, 1u, 0) == 1);
            assert(snag_pread(source->log_fd, &mutation.last, 1u, mutation.size - 1) == 1);
            struct snag_context_control changed = {.cancelled = mutate_materialization,
                .opaque = &mutation};
            reject_materialization(source, &frame, &receipt, &changed, EAGAIN);
            assert(mutation.changed);
            assert(!snag_truncate(source->log_fd, mutation.size));
            write_source_byte(source->log_fd, 0, mutation.first);
            write_source_byte(source->log_fd, mutation.size - 1, mutation.last);
        }
        struct snag_buf entries = {.max = SIZE_MAX};
        uint64_t required = origins->texts.slots[SNAG_BINARY_TEXT_CWD].declaration;
        if (!required) {
            for (size_t j = 0u; j < 8u; ++j)
                required |= (uint64_t)provider.recent[j] << (j * 8u);
        }
        bool removed = false;
        for (uint64_t sequence = 1u; sequence < access.boundary.next_seq; ++sequence) {
            struct snag_binary_index_entry entry;
            int found = snag_binary_checkpoint_index_find(&access, sequence, &entry);
            if (found == 1) continue;
            assert(!found);
            if (sequence == required) { removed = true; continue; }
            assert(!snag_buf_append(&entries, &entry, sizeof(entry)));
        }
        assert(removed);
        struct snag_buf missing_access = {.max = SIZE_MAX};
        assert(!snag_binary_checkpoint_index_encode(&missing_access, &access.identity,
            &access.boundary, &access.tree, (const struct snag_binary_index_entry *)entries.data,
            entries.len / sizeof(struct snag_binary_index_entry)));
        struct snag_binary_checkpoint_frame missing = frame;
        missing.access.data = (const unsigned char *)missing_access.data;
        missing.access.size = missing_access.len;
        struct snag_binary_checkpoint_receipt missing_receipt;
        struct snag_buf missing_bytes = {.max = SIZE_MAX};
        seal_checkpoint(&missing, &missing_receipt, root, &missing_bytes);
        reject_materialization(source, &missing, &missing_receipt, NULL, ENOENT);
        snag_buf_free(&missing_bytes);
        snag_buf_free(&missing_access);
        snag_buf_free(&entries);
        struct snag_binary_checkpoint_receipt wrong = receipt;
        ++wrong.generation;
        reject_materialization(source, &frame, &wrong, NULL, EINVAL);
        wrong = receipt;
        wrong.index_root[0] ^= 1u;
        reject_materialization(source, &frame, &wrong, NULL, EINVAL);
        wrong = receipt;
        wrong.image_digest[0] ^= 1u;
        reject_materialization(source, &frame, &wrong, NULL, EINVAL);
        wrong = receipt;
        ++wrong.image_size;
        reject_materialization(source, &frame, &wrong, NULL, EINVAL);
        struct snag_binary_checkpoint_frame absent = *base;
        struct snag_buf unsupported = {.max = SIZE_MAX};
        seal_checkpoint(&absent, &wrong, root, &unsupported);
        reject_materialization(source, &absent, &wrong, NULL, ENOTSUP);
        snag_buf_free(&unsupported);
        struct snag_buf provider_wrong = {.max = SIZE_MAX};
        assert(!snag_buf_append(&provider_wrong, base->provider.data, base->provider.size));
        provider_number((unsigned char *)provider_wrong.data + 16u,
            expected->compact_seq ? 0u : 1u);
        struct snag_binary_checkpoint_frame mismatch = frame;
        mismatch.provider.data = (const unsigned char *)provider_wrong.data;
        struct snag_buf mismatched = {.max = SIZE_MAX};
        seal_checkpoint(&mismatch, &wrong, root, &mismatched);
        reject_materialization(source, &mismatch, &wrong, NULL, EINVAL);
        snag_buf_free(&mismatched);
        snag_buf_free(&provider_wrong);
    }
    snag_buf_free(&bytes);
    snag_buf_free(&selected);
    snag_buf_free(&available_bytes);
}

static void
checkpoint_matches(struct snag_session *source, struct snag_session *expected,
    const struct snag_binary_anchor *anchor, struct snag_binary_checkpoint_sources *origins)
{
    const json_t *recent, *history;
    assert(snag_context_capture_seam(expected, &recent, &history) == 0);
    struct snag_buf core = {.max = SIZE_MAX}, provider = {.max = SIZE_MAX};
    assert(snag_binary_checkpoint_core_encode(&core, origins, expected) == 0);
    assert(snag_binary_checkpoint_provider_encode(&provider, expected, recent, history) == 0);
    provider_codec_checks(expected, recent, history, &provider);
    provider_source_checks(source->log_fd, anchor, origins, expected, recent, history, &provider);
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    assert(pread(source->log_fd, header, sizeof(header), 0) == (ssize_t)sizeof(header));
    struct snag_binary_checkpoint_frame frame = {.generation = 1u, .boundary = *anchor,
        .core = {.version = SNAG_BINARY_CORE_VERSION,
            .data = (unsigned char *)core.data, .size = core.len},
        .provider = {.version = 1u, .data = (unsigned char *)provider.data, .size = provider.len}};
    struct snag_binary_anchor root;
    assert(snag_binary_header_decode(header, sizeof(header), &frame.identity, &root) == 0);
    struct snag_buf bytes = {.max = SIZE_MAX};
    assert(snag_binary_checkpoint_frame_encode(&bytes, &frame) == 0);
    joint_materialization_checks(source, expected, origins, &frame);
    struct snag_session checked;
    snag_session_init(&checked);
    struct snag_binary_recovery recovery = {0};
    struct snag_binary_checkpoint_sources adopted = {0};
    char error[256] = {0};
    int rc = snag_store_verify_binary_context_checkpoint(source, &checked, anchor,
        bytes.data, bytes.len, &recovery, &adopted, NULL, error, sizeof(error));
    if (rc < 0) fprintf(stderr, "checkpoint oracle: %s (%d)\n", error, errno);
    assert(rc == 0 && !recovery.incomplete_tail_bytes);
    same_cache(expected, &checked, false);
    json_t *left = snag_checkpoint_state_encode(expected);
    json_t *right = snag_checkpoint_state_encode(&checked);
    if (left && right && !json_equal(left, right)) {
        const char *key;
        json_t *value;
        json_object_foreach(left, key, value) {
            if (!json_equal(value, json_object_get(right, key)))
                fprintf(stderr, "native checkpoint state differs: %s\n", key);
        }
    }
    assert(left && right && json_equal(left, right));
    json_decref(left);
    json_decref(right);
    ++checkpoint_compared;
    if (expected->voice_history.adopted_seq) {
        uint64_t previous = 0u;
        for (size_t i = 0u; i < json_array_size(recent); ++i) {
            const json_t *event = json_array_get(recent, i);
            const char *type = snag_json_string(event, "type");
            if (type && !strcmp(type, "voice_transfer_adopted")) {
                assert(!snag_json_integer_u64(event, "seq", &previous));
                break;
            }
        }
        assert(previous && previous < expected->voice_history.adopted_seq);
        struct snag_session saved = checked;
        struct snag_binary_checkpoint_sources saved_sources = adopted;
        for (unsigned int stale = 0u; stale < 2u; ++stale) {
            struct snag_buf changed = {.max = SIZE_MAX}, bad = {.max = SIZE_MAX};
            assert(!snag_buf_append(&changed, core.data, core.len));
            provider_number(changed.data + 20u, stale ? previous : 0u);
            struct snag_binary_checkpoint_frame wrong = frame;
            wrong.core.data = changed.data;
            struct snag_session candidate;
            struct snag_binary_checkpoint_sources candidate_sources = {0};
            snag_session_init(&candidate);
            assert(!snag_binary_checkpoint_core_read(source->log_fd, &wrong, NULL,
                &candidate, &candidate_sources));
            assert(candidate.voice_history.adopted_seq == (stale ? previous : 0u));
            snag_session_close(&candidate);
            snag_binary_checkpoint_sources_free(&candidate_sources);
            assert(!snag_binary_checkpoint_frame_encode(&bad, &wrong));
            assert(snag_store_verify_binary_context_checkpoint(source, &checked, anchor,
                bad.data, bad.len, &recovery, &adopted, NULL, error, sizeof(error)) < 0);
            assert(errno == EINVAL && !memcmp(&saved, &checked, sizeof(checked)));
            assert(!memcmp(&saved_sources, &adopted, sizeof(adopted)));
            same_cache(expected, &checked, false);
            snag_buf_free(&changed);
            snag_buf_free(&bad);
            ++checkpoint_voice_rejected;
        }
    }

    static bool plain, with_history;
    bool *done = json_array_size(history) ? &with_history : &plain;
    if (!*done && json_array_size(recent) > 1u) {
        *done = true;
        unsigned char old_state[sizeof(checked)], old_origins[sizeof(adopted)];
        memcpy(old_state, &checked, sizeof(checked));
        memcpy(old_origins, &adopted, sizeof(adopted));
        for (unsigned int variant = 0u; variant < 16u; ++variant) {
            struct snag_buf changed = {.max = SIZE_MAX}, bad = {.max = SIZE_MAX};
            struct snag_buf other_core = {.max = SIZE_MAX};
            assert(snag_buf_append(&changed, provider.data, provider.len) == 0);
            struct snag_binary_checkpoint_frame wrong = frame;
            wrong.provider.data = (unsigned char *)changed.data;
            switch (variant) {
            case 0u: changed.data[2u] = 0u; break;
            case 1u: changed.data[48u + 8u] ^= 1u; break;
            case 2u: changed.data[48u + 16u] ^= 2u; break;
            case 3u: changed.data[48u + 17u] = 1u; break;
            case 4u: memset(changed.data + 32u, 255, 8u); break;
            case 5u: memset(changed.data + 48u, 0, 8u); break;
            case 6u: memcpy(changed.data + 48u + 36u, changed.data + 48u, 8u); break;
            case 7u: wrong.identity.id[0u] ^= 1u; break;
            case 8u: wrong.boundary.digest[0u] ^= 1u; break;
            case 9u: wrong.core.version = 3u; break;
            case 10u: wrong.provider.version = 2u; break;
            case 11u: wrong.core.size -= 1u; break;
            case 12u:
                memmove(changed.data + 48u, changed.data + 84u, changed.len - 84u);
                changed.len -= 36u;
                provider_number(changed.data + 32u, json_array_size(recent) - 1u);
                wrong.provider.size = changed.len;
                break;
            case 13u: {
                struct snag_session altered = *expected;
                altered.goal_locked = !altered.goal_locked;
                assert(snag_binary_checkpoint_core_encode(&other_core, origins, &altered) == 0);
                wrong.core.data = (unsigned char *)other_core.data;
                wrong.core.size = other_core.len;
                break;
            }
            case 14u:
                if (json_array_size(history)) {
                    wrong.provider.size -= json_array_size(history) * 8u;
                    provider_number(changed.data + 40u, 0u);
                } else {
                    provider_number(changed.data + 16u, expected->compact_seq ? 0u : 1u);
                }
                break;
            case 15u:
                if (json_array_size(history)) {
                    provider_number(changed.data + 48u + json_array_size(recent) * 36u, 1u);
                } else {
                    changed.data[48u + 16u] |= 8u;
                    changed.data[48u + 20u] ^= 42u;
                }
                break;
            }
            if (variant >= 12u) {
                struct snag_binary_checkpoint_provider structurally_valid;
                assert(snag_binary_checkpoint_provider_decode(wrong.provider.data,
                    wrong.provider.size, &structurally_valid) == 0);
            }
            assert(snag_binary_checkpoint_frame_encode(&bad, &wrong) == 0);
            assert(snag_store_verify_binary_context_checkpoint(source, &checked, anchor,
                bad.data, bad.len, &recovery, &adopted, NULL, error, sizeof(error)) < 0);
            assert(errno == EINVAL && !recovery.incomplete_tail_bytes);
            assert(!memcmp(old_state, &checked, sizeof(checked)));
            assert(!memcmp(old_origins, &adopted, sizeof(adopted)));
            same_cache(expected, &checked, false);
            snag_buf_free(&changed);
            snag_buf_free(&bad);
            snag_buf_free(&other_core);
            ++checkpoint_rejected;
        }
        checkpoint_late_failures(source, &checked, &adopted, anchor, bytes.data, bytes.len,
            snag_store_verify_binary_context_checkpoint, anchor->end);
    }
    /* Empty suffix and successful replacement of an already owning destination. */
    assert(snag_store_resume_binary_context_checkpoint(source, &checked, anchor,
        bytes.data, bytes.len, &recovery, &adopted, NULL, error, sizeof(error)) == 0);
    assert(recovery.verified.end == anchor->end && !recovery.incomplete_tail_bytes);
    same_cache(expected, &checked, false);
    struct snag_session core_only;
    snag_session_init(&core_only);
    assert(snag_store_reconcile_binary(source, &core_only, NULL, NULL, &recovery,
        &adopted, error, sizeof(error)) == 0);
    left = snag_checkpoint_state_encode(&core_only);
    right = snag_checkpoint_state_encode(&checked);
    assert(left && right && json_equal(left, right));
    json_decref(left);
    json_decref(right);
    snag_session_close(&core_only);
    /* All subsequent projection checks now use the jointly verified candidate. */
    snag_session_close(expected);
    *expected = checked;
    snag_binary_checkpoint_sources_free(origins);
    *origins = adopted;
    snag_buf_free(&bytes);
    snag_buf_free(&core);
    snag_buf_free(&provider);
}

/* Make checkpoint boundaries independent of the importer's batching policy.
 * Sequence-based canonical references keep their exact source record/payload;
 * only framing offsets and the commit-chain hashes change. */
static FILE *
separate_batches(FILE *original)
{
    int fd = fileno(original);
    off_t size = lseek(fd, 0, SEEK_END);
    assert(size >= (off_t)SNAG_BINARY_HEADER_SIZE);
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    assert(pread(fd, header, sizeof(header), 0) == (ssize_t)sizeof(header));
    struct snag_binary_identity identity;
    struct snag_binary_anchor input;
    assert(snag_binary_header_decode(header, sizeof(header), &identity, &input) == 0);
    struct snag_binary_anchor output = input;
    FILE *file = tmpfile();
    assert(file && fwrite(header, 1u, sizeof(header), file) == sizeof(header));
    struct snag_buf read = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf write = {.max = SNAG_BINARY_WIRE_BATCH_MAX};
    while (input.end < (uint64_t)size) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor next;
        assert(snag_binary_batch_read(fd, (uint64_t)size, &input, &read, &batch, &next) == 0);
        size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
        for (uint32_t i = 0u; i < batch.count; ++i) {
            struct snag_binary_record record;
            uint64_t seq;
            assert(snag_binary_record_next(&batch, &cursor, &record, &seq) == 0);
            assert(seq == output.next_seq);
            write.len = 0u;
            uint64_t turns = output.turns + (record.kind == SNAG_BINARY_TURN_STARTED);
            assert(binary_fixture_append(&write, &output, &record, 1u, turns, &output) == 0);
            assert(fwrite(write.data, 1u, write.len, file) == write.len);
        }
        input = next;
    }
    assert(input.next_seq == output.next_seq && input.turns == output.turns);
    assert(fflush(file) == 0);
    snag_buf_free(&read);
    snag_buf_free(&write);
    return file;
}

static void
reject_bounded_suffix(struct snag_session *source, const struct snag_binary_checkpoint_frame *frame,
    const struct snag_binary_checkpoint_index *original,
    const struct snag_binary_checkpoint_index *access, const struct snag_binary_anchor *stop,
    bool (*cancelled)(void *opaque), void *opaque, int expected_errno)
{
    struct snag_session state;
    snag_session_init(&state);
    struct snag_binary_checkpoint_sources origins = {0};
    assert(!snag_binary_checkpoint_core_read(source->log_fd, frame, original, &state, &origins));
    struct snag_binary_recovery recovery = {0};
    char error[128] = {0};
    int64_t position = snag_seek(source->log_fd, 0, SEEK_CUR);
    errno = 0;
    assert(snag_store_reduce_binary_suffix_prefix(source, &state, &frame->boundary, stop, access,
        NULL, NULL, cancelled, opaque, &recovery, &origins, error, sizeof(error)) < 0);
    assert(errno == expected_errno && !recovery.incomplete_tail_bytes);
    assert(snag_seek(source->log_fd, 0, SEEK_CUR) == position);
    snag_session_close(&state);
    snag_binary_checkpoint_sources_free(&origins);
    ++bounded_suffix_rejected;
}

static void
bounded_suffix_checks(struct snag_session *source, const struct snag_session *expected,
    const struct snag_binary_checkpoint_sources *expected_origins,
    const struct snag_binary_checkpoint_frame *frame, const struct snag_binary_anchor *stop)
{
    struct snag_buf locations = {.max = SIZE_MAX};
    struct snag_binary_checkpoint_index access;
    binary_fixture_access(source->log_fd, &frame->boundary, &locations, &access);
    struct snag_session state;
    snag_session_init(&state);
    struct snag_binary_checkpoint_sources origins = {0};
    assert(!snag_binary_checkpoint_core_read(source->log_fd, frame, &access, &state, &origins));
    struct snag_binary_recovery recovery = {.verified = *stop};
    struct provider_cancel cancel = {0};
    char error[128] = {0};
    int64_t position = snag_seek(source->log_fd, 0, SEEK_CUR);
    int rc = snag_store_reduce_binary_suffix_prefix(source, &state, &frame->boundary,
        &recovery.verified, &access, NULL, NULL, cancel_provider, &cancel, &recovery, &origins,
        error, sizeof(error));
    if (rc < 0) fprintf(stderr, "bounded suffix: %s (%d)\n", error, errno);
    assert(!rc && !recovery.incomplete_tail_bytes && !recovery.problem_seq &&
        recovery.verified.end == stop->end && recovery.verified.next_seq == stop->next_seq);
    assert(recovery.batches == stop->next_seq - frame->boundary.next_seq &&
        snag_seek(source->log_fd, 0, SEEK_CUR) == position);
    json_t *left = snag_checkpoint_state_encode(expected);
    json_t *right = snag_checkpoint_state_encode(&state);
    assert(left && right && json_equal(left, right));
    json_decref(left);
    json_decref(right);
    struct snag_buf wanted = {.max = SIZE_MAX};
    struct snag_buf got = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_core_encode(&wanted, expected_origins, expected));
    assert(!snag_binary_checkpoint_core_encode(&got, &origins, &state));
    assert(wanted.len == got.len && !memcmp(wanted.data, got.data, got.len));
    snag_buf_free(&wanted);
    snag_buf_free(&got);
    snag_session_close(&state);
    snag_binary_checkpoint_sources_free(&origins);
    ++bounded_suffix_compared;
    static bool checked[2];
    struct snag_binary_checkpoint_provider provider;
    assert(!snag_binary_checkpoint_provider_decode(frame->provider.data, frame->provider.size,
        &provider));
    unsigned int class_mode = provider.history_count ? 1u : 0u;
    if (!checked[class_mode] && stop->next_seq > frame->boundary.next_seq) {
        checked[class_mode] = true;
        for (size_t i = 1u; i <= cancel.calls; ++i) {
            struct provider_cancel stopped = {.fail_at = i};
            reject_bounded_suffix(source, frame, &access, &access, stop, cancel_provider,
                &stopped, ECANCELED);
            assert(stopped.calls == i);
        }
        for (unsigned int field = 0u; field < 6u; ++field) {
            struct snag_binary_anchor bad = *stop;
            if (field == 0u) --bad.end;
            if (field == 1u) ++bad.end;
            if (field == 2u) ++bad.next_seq;
            if (field == 3u) ++bad.turns;
            if (field == 4u) ++bad.previous;
            if (field == 5u) bad.digest[0] ^= 1u;
            reject_bounded_suffix(source, frame, &access, &access, &bad, NULL, NULL, EINVAL);
        }
        reject_bounded_suffix(source, frame, &access, NULL, stop, NULL, NULL, ENOTSUP);
        struct snag_binary_checkpoint_index bad_access = access;
        ++bad_access.boundary.next_seq;
        reject_bounded_suffix(source, frame, &access, &bad_access, stop, NULL, NULL, EINVAL);
        bad_access = access;
        ++bad_access.identity.created_ms;
        reject_bounded_suffix(source, frame, &access, &bad_access, stop, NULL, NULL, EINVAL);
        snag_file_info before;
        assert(!snag_fstat(source->log_fd, &before));
        struct direct_mutation mutation = {.cancel = {.fail_at = cancel.calls},
            .fd = source->log_fd, .size = before.st_size};
        assert(snag_pread(source->log_fd, &mutation.first, 1u, 0) == 1);
        reject_bounded_suffix(source, frame, &access, &access, stop, mutate_materialization,
            &mutation, EAGAIN);
        assert(mutation.changed);
        write_source_byte(source->log_fd, 0, mutation.first);
    }
    /* A stop at capture inspects no suffix or later corrupt wire bytes. */
    snag_session_init(&state);
    assert(!snag_binary_checkpoint_core_read(source->log_fd, frame, &access, &state, &origins));
    unsigned char next_byte = 0u;
    bool has_tail = frame->boundary.end < stop->end;
    if (has_tail) {
        assert(snag_pread(source->log_fd, &next_byte, 1u, (int64_t)frame->boundary.end) == 1);
        write_source_byte(source->log_fd, (int64_t)frame->boundary.end, next_byte ^ 1u);
    }
    recovery.verified = frame->boundary;
    assert(!snag_store_reduce_binary_suffix_prefix(source, &state, &recovery.verified,
        &recovery.verified, &access, NULL, NULL, NULL, NULL, &recovery, &origins,
        error, sizeof(error)));
    assert(!recovery.batches && !recovery.incomplete_tail_bytes &&
        recovery.verified.end == frame->boundary.end);
    if (has_tail) write_source_byte(source->log_fd, (int64_t)frame->boundary.end, next_byte);
    snag_session_close(&state);
    snag_binary_checkpoint_sources_free(&origins);
    snag_buf_free(&locations);
}

static void
checkpoint_suffix_matches(struct snag_session *source, struct snag_session *expected,
    const struct snag_binary_anchor *full, struct snag_binary_checkpoint_sources *origins)
{
    if (full->next_seq <= 2u) return;
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    assert(pread(source->log_fd, header, sizeof(header), 0) == (ssize_t)sizeof(header));
    struct snag_binary_checkpoint_frame frame = {.generation = 1u};
    struct snag_binary_anchor anchor;
    assert(snag_binary_header_decode(header, sizeof(header), &frame.identity, &anchor) == 0);
    struct snag_buf batch_bytes = {.max = SNAG_BINARY_BATCH_MAX};
    while (anchor.next_seq < (full->next_seq + 1u) / 2u) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor next;
        assert(snag_binary_batch_read(source->log_fd, full->end, &anchor,
            &batch_bytes, &batch, &next) == 0);
        anchor = next;
    }
    snag_buf_free(&batch_bytes);
    assert(anchor.next_seq > 1u && anchor.next_seq < full->next_seq);
    struct snag_session prefix, resumed;
    snag_session_init(&prefix);
    snag_session_init(&resumed);
    struct snag_binary_recovery recovery = {0};
    struct snag_binary_checkpoint_sources before = {0}, after = {0};
    char error[256] = {0};
    assert(snag_store_reconcile_binary_context_prefix(source, &prefix, &anchor,
        &recovery, &before, NULL, error, sizeof(error)) == 0);
    suffix_open += prefix.response_open;
    suffix_process += prefix.process_count != 0u;
    suffix_voice += prefix.voice_history.adopted_seq != 0u;
    suffix_queue += prefix.pending_queue_count != 0u;
    suffix_download += json_array_size(prefix.download_queue) != 0u;
    const json_t *recent, *history;
    assert(snag_context_capture_seam(&prefix, &recent, &history) == 0);
    struct snag_buf core = {.max = SIZE_MAX}, provider = {.max = SIZE_MAX};
    assert(snag_binary_checkpoint_core_encode(&core, &before, &prefix) == 0);
    assert(snag_binary_checkpoint_provider_encode(&provider, &prefix, recent, history) == 0);
    frame.boundary = anchor;
    frame.core = (struct snag_binary_checkpoint_section){.version = SNAG_BINARY_CORE_VERSION,
        .data = (unsigned char *)core.data, .size = core.len};
    frame.provider = (struct snag_binary_checkpoint_section){.version = 1u,
        .data = (unsigned char *)provider.data, .size = provider.len};
    bounded_suffix_checks(source, expected, origins, &frame, full);
    struct snag_buf bytes = {.max = SIZE_MAX};
    assert(snag_binary_checkpoint_frame_encode(&bytes, &frame) == 0);
    int rc = snag_store_resume_binary_context_checkpoint(source, &resumed, &anchor,
        bytes.data, bytes.len, &recovery, &after, NULL, error, sizeof(error));
    if (rc < 0) fprintf(stderr, "checkpoint suffix: %s (%d)\n", error, errno);
    assert(rc == 0 && !recovery.incomplete_tail_bytes);
    assert(recovery.batches == full->next_seq - 1u && recovery.verified.end == full->end);
    same_cache(expected, &resumed, false);
    json_t *left = snag_checkpoint_state_encode(expected);
    json_t *right = snag_checkpoint_state_encode(&resumed);
    assert(left && right && json_equal(left, right));
    json_decref(left);
    json_decref(right);
    struct snag_buf want = {.max = SIZE_MAX}, got = {.max = SIZE_MAX};
    assert(snag_binary_checkpoint_core_encode(&want, origins, expected) == 0);
    assert(snag_binary_checkpoint_core_encode(&got, &after, &resumed) == 0);
    assert(want.len == got.len && !memcmp(want.data, got.data, want.len));
    snag_buf_free(&want);
    snag_buf_free(&got);
    static bool plain, with_history;
    const json_t *final_recent, *final_history;
    assert(snag_context_capture_seam(&resumed, &final_recent, &final_history) == 0);
    bool *done = json_array_size(final_history) ? &with_history : &plain;
    if (!*done) {
        *done = true;
        suffix_failure_paths(source, &resumed, &after, &anchor, full, bytes.data, bytes.len);
    }
    snag_session_close(expected);
    *expected = resumed;
    snag_binary_checkpoint_sources_free(origins);
    *origins = after;
    snag_session_close(&prefix);
    snag_binary_checkpoint_sources_free(&before);
    snag_buf_free(&bytes);
    snag_buf_free(&core);
    snag_buf_free(&provider);
    ++suffix_compared;
}

static void
prefix_matches(struct snag_session *source, struct snag_session *expected,
    const struct snag_binary_anchor *anchor)
{
    struct snag_session prefix;
    struct snag_binary_recovery recovery = {.verified = *anchor};
    struct snag_binary_checkpoint_sources origins = {0};
    char error[256] = {0};
    snag_session_init(&prefix);
    assert(snag_store_reconcile_binary_context_prefix(source, &prefix, &recovery.verified,
        &recovery, &origins, NULL, error, sizeof(error)) == 0);
    assert(!recovery.incomplete_tail_bytes && !recovery.problem_seq);
    same_cache(expected, &prefix, false);
    json_t *left = snag_checkpoint_state_encode(expected);
    json_t *right = snag_checkpoint_state_encode(&prefix);
    assert(left && right && json_equal(left, right));
    json_decref(left);
    json_decref(right);
    snag_binary_checkpoint_sources_free(&origins);
    snag_session_close(&prefix);
    ++prefix_compared;
}

static void
prefix_rejections(struct snag_session *source, struct snag_session *target,
    struct snag_binary_checkpoint_sources *origins, const struct snag_binary_anchor *anchor,
    uint64_t file_end)
{
    unsigned char old_state[sizeof(*target)], old_origins[sizeof(*origins)];
    memcpy(old_state, target, sizeof(*target));
    memcpy(old_origins, origins, sizeof(*origins));
    json_t *old_cache = target->on_checkpoint(target->on_commit_opaque, target);
    assert(old_cache);
    for (unsigned int variant = 0u; variant < 11u; ++variant) {
        struct snag_binary_anchor bad = *anchor;
        switch (variant) {
        case 0: bad.end = 0u; break;
        case 1: bad.end = SNAG_BINARY_HEADER_SIZE; break;
        case 2: --bad.end; break;
        case 3: ++bad.end; break;
        case 4: bad.end = file_end + 1u; break;
        case 5: ++bad.next_seq; break;
        case 6: ++bad.previous; break;
        case 7: ++bad.turns; break;
        case 8: bad.digest[0] ^= 1u; break;
        case 9: bad.end = file_end; break; /* Includes corrupt committed metadata. */
        default: break; /* NULL boundary. */
        }
        struct snag_binary_recovery recovery = {0};
        char error[256] = {0};
        errno = 0;
        assert(snag_store_reconcile_binary_context_prefix(source, target,
            variant == 10u ? NULL : &bad, &recovery, origins, NULL, error, sizeof(error)) < 0);
        assert(errno == EINVAL && *error && !recovery.incomplete_tail_bytes);
        assert(!memcmp(old_state, target, sizeof(*target)));
        assert(!memcmp(old_origins, origins, sizeof(*origins)));
        json_t *cache = target->on_checkpoint(target->on_commit_opaque, target);
        assert(cache && json_equal(old_cache, cache));
        json_decref(cache);
        ++prefix_rejected;
    }
    json_decref(old_cache);
}

static bool
cancel_capture(void *opaque)
{
    unsigned int *remaining = opaque;
    return --*remaining == 0u;
}

struct capture_hook {
    unsigned int calls, change_at;
    int fd;
    uint64_t end;
    unsigned char byte;
    bool overwrite;
};

static bool
observe_capture(void *opaque)
{
    struct capture_hook *hook = opaque;
    ++hook->calls;
    if (hook->change_at && hook->calls == hook->change_at) {
        snag_file_info before;
        if (hook->overwrite) assert(snag_fstat(hook->fd, &before) == 0);
        const void *data = hook->overwrite ? (const void *)&hook->byte : (const void *)"X";
        assert(pwrite(hook->fd, data, 1u, (off_t)hook->end) == 1);
        if (hook->overwrite) {
            /* Automatic write stamps may share a filesystem clock tick. Force
             * a distinct fractional stamp in the same second to test the guard
             * deterministically; metadata never substitutes for source locking. */
            struct timespec times[2] = {{.tv_nsec = UTIME_OMIT}, {.tv_sec = before.st_mtime}};
#ifdef __APPLE__
            times[1].tv_nsec = (before.st_mtimespec.tv_nsec + 1l) % 1000000000l;
#else
            times[1].tv_nsec = (before.st_mtim.tv_nsec + 1l) % 1000000000l;
#endif
            assert(futimens(hook->fd, times) == 0);
        }
    }
    return false;
}

static void
checkpoint_late_failures(struct snag_session *source, struct snag_session *target,
    struct snag_binary_checkpoint_sources *origins, const struct snag_binary_anchor *anchor,
    const void *bytes, size_t size, checkpoint_reader read, uint64_t source_end)
{
    struct capture_hook hook = {.fd = source->log_fd, .end = source_end};
    struct snag_context_control control = {.opaque = &hook, .cancelled = observe_capture};
    struct snag_session probe;
    snag_session_init(&probe);
    struct snag_binary_recovery recovery = {0};
    char error[256] = {0};
    assert(read(source, &probe, anchor,
        bytes, size, &recovery, NULL, &control, error, sizeof(error)) == 0);
    same_cache(target, &probe, false);
    snag_session_close(&probe);
    unsigned int calls = hook.calls;
    assert(calls > 3u);
    /* Last three hooks are seed, loaded capture bind, and final cancellation
     * before source recheck. None may replace the already adopted destination. */
    unsigned char last;
    assert(pread(source->log_fd, &last, 1u, (off_t)source_end - 1) == 1);
    for (unsigned int offset = 0u; offset < 3u; ++offset) {
        for (unsigned int change = 0u; change < (offset ? 2u : 3u); ++change) {
            unsigned char old_state[sizeof(*target)], old_origins[sizeof(*origins)];
            memcpy(old_state, target, sizeof(*target));
            memcpy(old_origins, origins, sizeof(*origins));
            json_t *old_cache = target->on_checkpoint(target->on_commit_opaque, target);
            assert(old_cache);
            unsigned int remaining = calls - offset;
            hook.calls = 0u;
            hook.change_at = remaining;
            hook.overwrite = change == 2u;
            hook.end = hook.overwrite ? source_end - 1u : source_end;
            hook.byte = last ^ 1u;
            control.cancelled = change ? observe_capture : cancel_capture;
            control.opaque = change ? (void *)&hook : (void *)&remaining;
            assert(read(source, target, anchor,
                bytes, size, &recovery, origins, &control, error, sizeof(error)) < 0);
            assert(errno == (change ? EAGAIN : ECANCELED));
            assert(!recovery.incomplete_tail_bytes);
            assert(!memcmp(old_state, target, sizeof(*target)));
            assert(!memcmp(old_origins, origins, sizeof(*origins)));
            json_t *new_cache = target->on_checkpoint(target->on_commit_opaque, target);
            assert(new_cache && json_equal(old_cache, new_cache));
            json_decref(old_cache);
            json_decref(new_cache);
            if (change) assert(ftruncate(source->log_fd, (off_t)source_end) == 0);
            if (hook.overwrite) {
                assert(pwrite(source->log_fd, &last, 1u, (off_t)source_end - 1) == 1);
                ++checkpoint_rewrite_rejected;
            } else if (read == snag_store_resume_binary_context_checkpoint) {
                ++suffix_late_rejected;
            } else {
                ++checkpoint_late_rejected;
            }
        }
    }
}

static void
suffix_failure_paths(struct snag_session *source, struct snag_session *target,
    struct snag_binary_checkpoint_sources *origins, const struct snag_binary_anchor *anchor,
    const struct snag_binary_anchor *full, const void *bytes, size_t size)
{
    checkpoint_late_failures(source, target, origins, anchor, bytes, size,
        snag_store_resume_binary_context_checkpoint, full->end);
    unsigned char old_state[sizeof(*target)], old_origins[sizeof(*origins)];
    memcpy(old_state, target, sizeof(*target));
    memcpy(old_origins, origins, sizeof(*origins));
    json_t *old_cache = target->on_checkpoint(target->on_commit_opaque, target);
    assert(old_cache);
    unsigned char last;
    assert(pread(source->log_fd, &last, 1u, (off_t)full->end - 1) == 1);
    struct snag_binary_recovery recovery = {0};
    char error[256] = {0};
    for (unsigned int variant = 0u; variant < 2u; ++variant) {
        struct snag_buf bad = {.max = SNAG_BINARY_BATCH_MAX};
        if (!variant) {
            unsigned char payload = 0u;
            struct snag_binary_record record = {.kind = SNAG_BINARY_LEGACY_CHECKPOINT,
                .version = 1u, .flags = SNAG_BINARY_RECORD_OPTIONAL,
                .timestamp_ms = target->last_time_ms, .payload = &payload, .size = 1u};
            assert(binary_fixture_append(&bad, full, &record, 1u, full->turns, NULL) == 0);
            assert(pwrite(source->log_fd, bad.data, bad.len, (off_t)full->end) ==
                (ssize_t)bad.len);
        } else {
            unsigned char changed = last ^ 1u;
            assert(pwrite(source->log_fd, &changed, 1u, (off_t)full->end - 1) == 1);
        }
        assert(snag_store_resume_binary_context_checkpoint(source, target, anchor,
            bytes, size, &recovery, origins, NULL, error, sizeof(error)) < 0);
        assert(errno == EINVAL && !recovery.incomplete_tail_bytes);
        assert(!memcmp(old_state, target, sizeof(*target)));
        assert(!memcmp(old_origins, origins, sizeof(*origins)));
        json_t *cache = target->on_checkpoint(target->on_commit_opaque, target);
        assert(cache && json_equal(old_cache, cache));
        json_decref(cache);
        snag_file_info info;
        assert(snag_fstat(source->log_fd, &info) == 0);
        assert((uint64_t)info.st_size == full->end + bad.len);
        assert(ftruncate(source->log_fd, (off_t)full->end) == 0);
        assert(pwrite(source->log_fd, &last, 1u, (off_t)full->end - 1) == 1);
        snag_buf_free(&bad);
        ++suffix_bad_rejected;
    }
    json_decref(old_cache);

    /* Incomplete data is reported only after complete successful adoption;
     * the reader leaves both an appended byte and a partial batch untouched. */
    for (unsigned int partial = 0u; partial < 2u; ++partial) {
        uint64_t end = partial ? full->end - 1u : full->end + 1u;
        if (partial) assert(ftruncate(source->log_fd, (off_t)end) == 0);
        else assert(pwrite(source->log_fd, "x", 1u, (off_t)full->end) == 1);
        struct snag_session expected, resumed;
        snag_session_init(&expected);
        snag_session_init(&resumed);
        struct snag_binary_recovery baseline = {0};
        struct snag_binary_checkpoint_sources wanted = {0}, got = {0};
        assert(snag_store_reconcile_binary_context(source, &expected, &baseline,
            &wanted, NULL, error, sizeof(error)) == 0);
        assert(snag_store_resume_binary_context_checkpoint(source, &resumed, anchor,
            bytes, size, &recovery, &got, NULL, error, sizeof(error)) == 0);
        assert(recovery.incomplete_tail_bytes == baseline.incomplete_tail_bytes);
        assert(recovery.incomplete_tail_bytes && recovery.batches == baseline.batches);
        assert(recovery.verified.end == baseline.verified.end);
        same_cache(&expected, &resumed, false);
        json_t *left = snag_checkpoint_state_encode(&expected);
        json_t *right = snag_checkpoint_state_encode(&resumed);
        assert(left && right && json_equal(left, right));
        json_decref(left);
        json_decref(right);
        snag_file_info info;
        assert(snag_fstat(source->log_fd, &info) == 0 && (uint64_t)info.st_size == end);
        snag_session_close(&expected);
        snag_session_close(&resumed);
        snag_binary_checkpoint_sources_free(&wanted);
        snag_binary_checkpoint_sources_free(&got);
        assert(ftruncate(source->log_fd, (off_t)full->end) == 0);
        assert(pwrite(source->log_fd, &last, 1u, (off_t)full->end - 1) == 1);
        ++suffix_tails;
    }
}

static void
failed_replay(struct snag_session *source, struct snag_session *target,
    struct snag_binary_checkpoint_sources *origins, const struct snag_context_control *control,
    int expected_errno)
{
    unsigned char old_state[sizeof(*target)], old_origins[sizeof(*origins)];
    memcpy(old_state, target, sizeof(*target));
    memcpy(old_origins, origins, sizeof(*origins));
    json_t *old_cache = target->on_checkpoint(target->on_commit_opaque, target);
    assert(old_cache);
    struct snag_binary_recovery recovery = {0};
    char error[256] = {0};
    errno = 0;
    assert(snag_store_reconcile_binary_context(source, target, &recovery, origins,
        control, error, sizeof(error)) < 0 && errno == expected_errno);
    assert(!memcmp(old_state, target, sizeof(*target)));
    assert(!memcmp(old_origins, origins, sizeof(*origins)));
    json_t *new_cache = target->on_checkpoint(target->on_commit_opaque, target);
    assert(new_cache && json_equal(old_cache, new_cache));
    json_decref(old_cache);
    json_decref(new_cache);
}

static void
late_failure_paths(struct snag_session *source, struct snag_session *target,
    struct snag_binary_checkpoint_sources *origins, const struct snag_binary_anchor *anchor)
{
    struct capture_hook hook = {.fd = source->log_fd, .end = anchor->end};
    struct snag_context_control control = {.opaque = &hook, .cancelled = observe_capture};
    struct snag_session probe;
    struct snag_binary_recovery recovery = {0};
    char error[256] = {0};
    snag_session_init(&probe);
    assert(snag_store_reconcile_binary_context(source, &probe, &recovery, NULL,
        &control, error, sizeof(error)) == 0);
    same_cache(target, &probe, false);
    snag_session_close(&probe);
    assert(hook.calls > 2u);

    /* The final hook is bind; the preceding hook is the last source lookup
     * callback when an IRC source table exists (otherwise the last event). */
    for (unsigned int offset = 0u; offset < 2u; ++offset) {
        unsigned int remaining = hook.calls - offset;
        struct snag_context_control cancelled = {.opaque = &remaining, .cancelled = cancel_capture};
        failed_replay(source, target, origins, &cancelled, ECANCELED);
        assert(!remaining);
    }
    hook.change_at = hook.calls;
    hook.calls = 0u;
    failed_replay(source, target, origins, &control, EAGAIN);
    assert(hook.calls == hook.change_at);
    assert(ftruncate(source->log_fd, (off_t)anchor->end) == 0);
}

static void
failure_paths(struct snag_session *source, struct snag_session *target,
    struct snag_binary_checkpoint_sources *origins, const struct snag_binary_anchor *anchor)
{
    /* Cancellation after a successful first callback must discard the capture,
     * not the previously installed core/provider candidate or origin owner. */
    unsigned int remaining = 2u;
    struct snag_context_control control = {.opaque = &remaining, .cancelled = cancel_capture};
    failed_replay(source, target, origins, &control, ECANCELED);
    assert(!remaining);
    late_failure_paths(source, target, origins, anchor);

    /* Fully framed, malformed metadata after an entire valid prefix. All earlier
     * provider callbacks have run; the suffix is corruption, never tail repair. */
    unsigned char bad = 0u;
    struct snag_binary_record record = {.kind = SNAG_BINARY_LEGACY_CHECKPOINT, .version = 1u,
        .flags = SNAG_BINARY_RECORD_OPTIONAL, .timestamp_ms = target->last_time_ms,
        .payload = &bad, .size = 1u};
    struct snag_buf suffix = {.max = SNAG_BINARY_BATCH_MAX};
    assert(binary_fixture_append(&suffix, anchor, &record, 1u, anchor->turns, NULL) == 0);
    assert(pwrite(source->log_fd, suffix.data, suffix.len, (off_t)anchor->end) ==
        (ssize_t)suffix.len);
    failed_replay(source, target, origins, NULL, EINVAL);
    prefix_matches(source, target, anchor);
    prefix_rejections(source, target, origins, anchor, anchor->end + suffix.len);
    assert(ftruncate(source->log_fd, (off_t)anchor->end) == 0);
    snag_buf_free(&suffix);

    int fd = source->log_fd;
    source->log_fd = -1;
    failed_replay(source, target, origins, NULL, EINVAL);
    source->log_fd = fd;
}

static int
legacy_sources(void *source, snag_session_event_fn fn, void *opaque,
    char *error, size_t error_size)
{
    struct snag_session discarded;
    struct snag_legacy_recovery recovery = {0};
    snag_session_init(&discarded);
    int rc = snag_store_reconcile_legacy(source, &discarded, fn, opaque, &recovery,
        error, error_size);
    snag_session_close(&discarded);
    return rc;
}

void
test_context_binary_projection(struct snag_session *source, unsigned int cycle,
    const json_t *steering, const struct snag_instruction_set *instructions,
    const struct snag_context_projection *expected)
{
    /* Memory-only and deliberately detached cache fixtures have no stopped
     * journal. Their original context assertions remain in the calling test. */
    if (source->pending_log || source->log_fd < 0 || source->lock_fd < 0) {
        ++without_journal;
        return;
    }
    char error[512] = {0};
    off_t position = lseek(source->log_fd, 0, SEEK_CUR);
    assert(position >= 0);
    struct snag_context_capture *capture = snag_context_capture_new(NULL);
    assert(capture);
    struct snag_session legacy, staged, native, native_source;
    snag_session_init(&legacy);
    snag_session_init(&staged);
    snag_session_init(&native);
    snag_session_init(&native_source);
    struct snag_legacy_recovery legacy_recovery = {0};
    int rc = snag_store_reconcile_legacy(source, &legacy, snag_context_capture_event, capture,
        &legacy_recovery, error, sizeof(error));
    if (rc < 0) fprintf(stderr, "legacy provider capture: %s\n", error);
    assert(rc == 0);
    assert(snag_context_capture_sources(capture, &legacy, legacy_sources, source,
        error, sizeof(error)) == 0);
    assert(snag_context_capture_bind(&capture, &legacy, error, sizeof(error)) == 0 && !capture);

    FILE *file = tmpfile();
    assert(file);
    struct snag_binary_import_result imported = {0};
    rc = snag_store_import_binary_journal(source, fileno(file), &staged, &imported,
        error, sizeof(error));
    if (rc < 0) fprintf(stderr, "native provider import: %s\n", error);
    assert(rc == 0);
    FILE *separate = separate_batches(file);
    assert(fclose(file) == 0);
    file = separate;
    native_source.log_fd = fileno(file);
    native_source.lock_fd = source->lock_fd;
    memcpy(native_source.id, source->id, sizeof(source->id));
    off_t native_position = lseek(native_source.log_fd, 0, SEEK_CUR);
    struct snag_binary_recovery recovery = {0};
    struct snag_binary_checkpoint_sources origins = {0};
    rc = snag_store_reconcile_binary_context(&native_source, &native, &recovery, &origins,
        NULL, error, sizeof(error));
    if (rc < 0) fprintf(stderr, "native provider capture: %s\n", error);
    assert(rc == 0);
    assert(native.dir_fd == -1 && native.log_fd == -1 && native.lock_fd == -1);
    assert(!native.pending_log && !native.checkpoint_context && !native.checkpoint_state);
    assert(native.on_commit && native.on_commit_free && native.on_commit_opaque);
    same_cache(&legacy, &native, true);
    checkpoint_matches(&native_source, &native, &recovery.verified, &origins);
    checkpoint_suffix_matches(&native_source, &native, &recovery.verified, &origins);
    prefix_matches(&native_source, &native, &recovery.verified);

    if (!checked_failures && native.next_seq > 2u) {
        failure_paths(&native_source, &native, &origins, &recovery.verified);
        checked_failures = true;
    }
    json_t *cache = native.on_checkpoint(native.on_commit_opaque, &native);
    assert(cache);
    if (!checked_source_failures && json_array_size(json_object_get(cache, "history_sources"))) {
        failure_paths(&native_source, &native, &origins, &recovery.verified);
        checked_source_failures = true;
    }
    json_decref(cache);
    assert(lseek(native_source.log_fd, 0, SEEK_CUR) == native_position);
    assert(lseek(source->log_fd, 0, SEEK_CUR) == position);
    /* Close the native bytes before projection: the captured seam is owned.
     * Only retained media lookup receives a duplicate of the fixture directory;
     * neither reconstructed state has a journal descriptor or a writer lock. */
    assert(fclose(file) == 0);
    native_source.log_fd = native_source.lock_fd = -1;
    legacy.dir_fd = dup(source->dir_fd);
    native.dir_fd = dup(source->dir_fd);
    assert(legacy.dir_fd >= 0 && native.dir_fd >= 0);
    /* The existing rollout-location hint is a host binding, not journal state. */
    legacy.dir_path = strdup(source->dir_path);
    native.dir_path = strdup(source->dir_path);
    assert(legacy.dir_path && native.dir_path);
    for (unsigned int pass = 0u; pass < 2u; ++pass) {
        struct snag_context_projection left = {0}, right = {0};
        rc = snag_context_build(&legacy, SNAJPAGENT_MODEL, "medium", cycle, steering,
            0u, false, NULL, NULL, instructions, NULL, &left, error, sizeof(error), NULL);
        if (rc < 0) {
            fprintf(stderr, "legacy provider projection: %s (seq=%llu turn=%s)\n",
                error, (unsigned long long)legacy.next_seq, legacy.active_turn_id);
        }
        assert(rc == 0);
        rc = snag_context_build(&native, SNAJPAGENT_MODEL, "medium", cycle, steering,
            0u, false, NULL, NULL, instructions, NULL, &right, error, sizeof(error), NULL);
        if (rc < 0) fprintf(stderr, "native provider projection: %s\n", error);
        assert(rc == 0);
        same_projection(&left, &right);
        if (expected) {
            same_projection(expected, &right);
            ++warm_compared;
        }
        same_cache(&legacy, &native, true);
        snag_context_projection_free(&left);
        snag_context_projection_free(&right);
    }
    snag_binary_checkpoint_sources_free(&imported.sources);
    snag_binary_checkpoint_sources_free(&origins);
    snag_session_close(&legacy);
    snag_session_close(&staged);
    snag_session_close(&native);
    ++compared;
}

void
test_context_binary_report(void)
{
    assert(compared && checked_failures && checked_source_failures);
    printf("native context: %u journal projections compared twice; %u non-journal fixtures\n",
        compared, without_journal);
    printf("native context: %u warm-source projection comparisons\n", warm_compared);
    assert(checkpoint_compared == compared && checkpoint_rejected == 32u);
    printf("native checkpoint oracle: %u paired sections; %u rejected candidates\n",
        checkpoint_compared, checkpoint_rejected);
    assert(warm_compared && prefix_compared && prefix_rejected == 22u);
    assert(checkpoint_late_rejected == 12u);
    printf("native checkpoint materialization: %u late cancellations/source changes rejected\n",
        checkpoint_late_rejected);
    printf("native context prefix: %u matches; %u rejected boundaries\n",
        prefix_compared, prefix_rejected);
    assert(suffix_compared == compared && suffix_late_rejected == 12u);
    assert(suffix_bad_rejected == 4u && suffix_tails == 4u);
    assert(checkpoint_rewrite_rejected == 4u && checkpoint_voice_rejected == 2u);
    assert(suffix_open && suffix_process && suffix_queue && suffix_download && suffix_voice);
    printf("native checkpoint suffix: %u matches; %u late failures; %u corruptions; %u tails\n",
        suffix_compared, suffix_late_rejected, suffix_bad_rejected, suffix_tails);
    printf("native suffix starting state: %u open responses; %u processes; "
        "%u queues; %u downloads; %u voice roots\n",
        suffix_open, suffix_process, suffix_queue, suffix_download, suffix_voice);
    printf("native checkpoint source recheck: %u same-size rewrites rejected\n",
        checkpoint_rewrite_rejected);
    printf("native checkpoint voice roots: %u omitted/stale candidates rejected\n",
        checkpoint_voice_rejected);
    printf("native direct checkpoint: %u joint comparisons; %u rejected stages; "
        "%u unrelated old corruption skipped\n", direct_compared, direct_rejected,
        direct_uninspected);
    printf("native bounded suffix: %u comparisons; %u rejected boundaries/stages\n",
        bounded_suffix_compared, bounded_suffix_rejected);
    fflush(stdout);
}
