/* SPDX-License-Identifier: GPL-2.0-only */
#include "history_view.h"
#include "json.h"
#include "store.h"

#include <stdlib.h>
#include <string.h>

char *
snag_history_event_data(uint64_t seq, const char *type, const json_t *data,
    const struct snag_wire_secrets *secrets, char *error, size_t size)
{
    json_t *view = NULL, *items = NULL;
    char *encoded = NULL;
    char *filtered = NULL;
    struct snag_buf clean = {.max = SNAG_MAX_EVENT_LINE + 1u};
    bool omitted = false;
    if (!strcmp(type, "session_checkpoint")) {
        view = json_pack("{s:I,s:b,s:I}", "covers_through_seq", (json_int_t)(seq - 1u),
            "provider_view", json_is_object(json_object_get(data, "context")) ||
                json_is_true(json_object_get(data, "provider_view")),
            "snapshot_v", json_integer_value(json_object_get(data, "snapshot_v")));
    } else if (!strcmp(type, "voice_transfer_record") ||
        !strcmp(type, "voice_transfer_sealed")) {
        view = json_pack("{s:b,s:s}", "prepared_voice_history", true,
            "transfer_id", snag_json_string(data, "transfer_id"));
    } else if (!strcmp(type, "voice_transfer_adopted")) {
        view = json_pack("{s:b,s:s,s:s,s:I}", "session_boundary", true,
            "source_session_id", snag_json_string(data, "source_session_id"),
            "target_session_id", snag_json_string(data, "target_session_id"),
            "source_as_of_seq", json_integer_value(json_object_get(data, "source_as_of_seq")));
    } else {
        view = json_copy((json_t *)data);
        if (!view) goto out;
        if (!strcmp(type, "response_completed")) {
            json_t *continuation = json_object_get(view, "continuation");
            omitted = continuation && !json_is_null(continuation);
            (void)json_object_del(view, "continuation");
        } else if (!strcmp(type, "compaction_completed")) {
            json_t *output = json_object_get(data, "output");
            items = json_array();
            if (!items) goto out;
            for (size_t i = 0u; i < json_array_size(output); ++i) {
                json_t *item = json_copy(json_array_get(output, i));
                if (!item) goto out;
                if (json_object_get(item, "encrypted_content")) {
                    omitted = true;
                    (void)json_object_del(item, "encrypted_content");
                }
                if (json_array_append_new(items, item) < 0) goto out;
            }
            if (json_object_set(view, "output", items) < 0) goto out;
        }
        if (omitted && json_object_set_new(view, "provider_payload_omitted", json_true()) < 0)
            goto out;
    }
    if (view) encoded = json_dumps(view, JSON_COMPACT | JSON_SORT_KEYS | JSON_ENCODE_ANY);
    if (encoded) {
        size_t length = strlen(encoded);
        if (snag_wire_json_redact_bounded((const unsigned char *)encoded, length,
                SNAG_MAX_EVENT_LINE, secrets, &clean, error, size) == 0 &&
            snag_buf_terminate(&clean) == 0) {
            filtered = (char *)clean.data;
            clean.data = NULL;
        }
        snag_secret_clear(encoded, length);
        free(encoded);
    }
out:
    snag_buf_free(&clean);
    json_decref(items);
    json_decref(view);
    return filtered;
}
