/* SPDX-License-Identifier: GPL-2.0-only */
#include "config.h"
#include "irc.h"
#include "json.h"
#include "media.h"
#include "store.h"
#include "store_binary_legacy.h"
#include "turn.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID "0123456789abcdef0011223344556677"
#define NEW_ID "fedcba98765432107766554433221100"
#define HASH0 "0000000000000000000000000000000000000000000000000000000000000000"
#define HASHF "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
#define GOAL "\"goal_id\":\"" ID "\""
#define TIMER "\"timer_id\":\"" ID "\""
#define ASSET "{\"id\":\"" ID "\",\"sha256\":\"" HASHF "\",\"bytes\":1," \
    "\"mime_type\":\"image/png\"}"
#define VOICE_QUEUE_ID "11adbaa8fafddbccd72abe5fff648e9e"
#define RESPONSE_IDS "\"turn_id\":\"" ID "\",\"response_id\":\"" NEW_ID "\",\"cycle\":1"
#define PUBLIC_ITEM "{\"kind\":\"assistant\",\"phase\":\"commentary\",\"local_item_id\":\"" ID \
    "\",\"provider_item_id\":\"p\",\"text\":\"t\"}"
#define CALL_ITEM "{\"kind\":\"tool_call\",\"call_id\":\"" ID "\",\"provider_item_id\":\"p\"," \
    "\"provider_call_id\":\"c\",\"name\":\"get_cwd\",\"arguments\":{}}"
#define EMPTY_USAGE "{\"input_tokens\":null,\"output_tokens\":null," \
    "\"reasoning_tokens\":null,\"total_tokens\":null}"
#define EMPTY_EXCERPT "{\"encoding\":\"utf8\",\"retained\":\"\",\"retained_bytes\":0," \
    "\"original_bytes\":0,\"discarded_bytes\":0}"
#define TOOL_RESULT "{\"status\":\"succeeded\",\"reason\":null,\"handle\":null," \
    "\"duration_ms\":0,\"exit_code\":0,\"signal\":null,\"model_text\":\"\"," \
    "\"stdout\":" EMPTY_EXCERPT ",\"stderr\":" EMPTY_EXCERPT "}"
#define OUTPUT_REF "{\"handle\":\"" ID "\",\"stdout_start\":5,\"stdout_end\":5," \
    "\"stderr_start\":8,\"stderr_end\":8,\"stdin_accepted\":9,\"stdin_written\":3," \
    "\"stdin_pending\":5,\"stdin_open\":true,\"log_start\":123,\"log_end\":456}"
#define IRC_EVENT "\"kind\":\"message\",\"endpoint\":\"e\",\"room\":\"\",\"nick\":\"\"," \
    "\"text\":\"\",\"historical\":false,\"local\":false,\"op\":false,\"timestamp_ms\":1"
#define TRANSFER "\"transfer_id\":\"" ID "\",\"target_session_id\":\"" NEW_ID "\"," \
    "\"source_session_id\":\"" ID "\",\"source_as_of_seq\":1,\"count\":1"
#define RESPONSE_START "\"turn_id\":\"" ID "\",\"response_id\":\"" NEW_ID "\",\"cycle\":1," \
    "\"count_method\":\"exact\",\"input_tokens_bound\":0,\"baseline_sha256\":null," \
    "\"compact_id\":null,\"model\":\"m\",\"capability_version\":\"c\",\"profile_id\":\"p\"," \
    "\"request_sha256\":\"" HASH0 "\",\"count_request_sha256\":\"" HASHF "\"," \
    "\"model_input_sha256\":\"" HASH0 "\",\"steering_ids\":[]"

static const struct legacy_sample {
    const char *type, *data;
    unsigned int kind;
} samples[] = {
    {"session_created", "{\"format\":2,\"protocol\":\"responses\",\"workspace\":\"/\","
        "\"default_provider\":\"p\",\"default_model\":\"m\",\"default_effort\":\"e\"}", 1},
    {"session_created", "{\"format\":3,\"protocol\":\"responses\",\"cwd\":\"/\","
        "\"default_provider\":\"p\",\"default_model\":\"m\",\"default_effort\":\"e\"}", 1},
    {"session_created", "{\"format\":4,\"protocol\":\"responses\",\"cwd\":\"C:\\\\x\","
        "\"default_provider\":\"p\",\"default_model\":\"m\",\"default_effort\":\"e\"}", 1},
    {"timer_scheduled", "{" TIMER ",\"due_ms\":257,\"text\":\"t\"}", 32},
    {"timer_fired", "{" TIMER "}", 33},
    {"timer_cancelled", "{" TIMER "}", 34},
    {"goal_started", "{" GOAL ",\"prompt\":\"start\\né\"}", 64},
    {"goal_replaced", "{" GOAL ",\"new_goal_id\":\"" NEW_ID "\","
        "\"actor\":\"user\",\"prompt\":\"replacement\"}", 65},
    {"goal_reworded", "{" GOAL ",\"actor\":\"model\",\"prompt\":\"changed\"}", 66},
    {"goal_lock_changed", "{" GOAL ",\"locked\":true}", 67},
    {"goal_paused", "{" GOAL ",\"reason\":\"input_closed\"}", 68},
    {"goal_blocked", "{" GOAL ",\"actor\":\"model\",\"reason\":\"dependency\"}", 69},
    {"goal_completed", "{" GOAL ",\"actor\":\"model\"}", 70},
    {"goal_resumed", "{" GOAL "}", 71},
    {"goal_cancelled", "{" GOAL "}", 72},
    {"cwd_changed", "{\"old_cwd\":\"/a\",\"new_cwd\":\"C:\\\\b\"}", 2},
    {"session_archived", "{\"origin\":\"user\"}", 3},
    {"session_unarchived", "{\"origin\":\"user\"}", 4},
    {"session_delete_requested", "{\"confirmed_id_prefix\":\"01234567\","
        "\"trash_name\":\"" ID "." NEW_ID "\"}", 5},
    {"banner_updated", "{\"text\":\"\"}", 6},
    {"steering_updated", "{\"mode\":\"\"}", 7},
    {"model_selection_changed", "{\"old_provider\":\"p\",\"old_model\":\"m\",\"old_effort\":\"e\","
        "\"new_provider\":\"q\",\"new_model\":\"n\",\"new_effort\":\"f\"}", 8},
    {"turn_model_changed", "{\"turn_id\":\"" ID "\",\"old_provider\":\"p\",\"old_model\":\"m\","
        "\"old_effort\":\"e\",\"new_effort\":\"f\"}", 9},
    {"effort_changed", "{\"old_effort\":\"default\",\"new_effort\":\"high\"}", 10},
    {"context_selection_changed", "{\"old_mode\":\"default\",\"old_tokens\":0,"
        "\"new_mode\":\"tokens\",\"new_tokens\":65537}", 11},
    {"command_shell_changed", "{\"shell\":\"C:\\\\cmd.exe\"}", 12},
    {"rule_log", "{\"chain\":[true,-9223372036854775808],"
        "\"message\":{\"a\":\"é\"},\"rule\":null}", 248},
    {"rule_transform", "{\"call_id\":\"" ID "\",\"original_sha256\":\"" HASH0 "\","
        "\"effective_sha256\":\"" HASHF "\",\"rule\":\"\"}", 249},
    {"audio_usage", "{\"operation\":\"dictation\",\"provider\":\"p\","
        "\"model\":\"m\",\"report\":\"r\"}", 256},
    {"voice_event", "{\"connection_id\":\"" ID "\",\"provider\":\"p\",\"model\":\"m\","
        "\"event\":{\"type\":\"voice_transcript\",\"text\":null,"
        "\"new_field\":[1,false],\"\":{\"a\":\"é\"}}}", 257},
    {"control_requested", "{\"control\":1}", 16},
    {"control_started", "{\"control\":2}", 17},
    {"control_finished", "{\"control\":4}", 18},
    {"control_requested", "{\"control\":4,\"origin\":\"image_boundary\",\"source_seq\":257}", 16},
    {"download_queued", "{\"id\":\"" ID "\",\"path\":\"/a\",\"name\":\"a\",\"bytes\":0,"
        "\"mtime\":1,\"sha256\":\"" HASH0 "\",\"queued_ms\":257}", 240},
    {"download_removed", "{\"id\":\"" ID "\",\"reason\":\"\"}", 241},
    {"downloads_cleared", "{\"reason\":\"cleared\"}", 242},
    {"context_rebased", "{\"turn_id\":\"" ID "\",\"reason\":\"goal_recovery\"}", 227},
    {"response_capacity_rejected", "{\"turn_id\":\"" ID "\",\"response_id\":\"" NEW_ID "\","
        "\"cycle\":1,\"code\":\"context_length_exceeded\",\"message\":\"\","
        "\"provider_source_sha256\":\"" HASH0 "\",\"request_sha256\":\"" HASHF "\","
        "\"context_limit_tokens\":null,\"requested_input_tokens\":4000000000,"
        "\"observed_hard_input_tokens\":1}", 166},
    {"compaction_started", "{\"compact_id\":\"" ID "\",\"predecessor_compact_id\":null,"
        "\"reason\":\"manual\",\"count_method\":\"unknown\",\"source_seq\":1,"
        "\"input_tokens_bound\":0,\"source_sha256\":\"" HASH0 "\","
        "\"request_sha256\":\"" HASHF "\",\"count_request_sha256\":\"" HASH0 "\","
        "\"model\":\"m\",\"capability_version\":\"c\",\"profile_id\":\"p\"}", 224},
    {"compaction_interrupted", "{\"compact_id\":\"" ID "\",\"reason\":\"user\"}", 225},
    {"compaction_completed", "{\"compact_id\":\"" ID "\",\"count_method\":\"unknown\","
        "\"input_tokens_bound\":0,\"output_count_method\":\"exact\",\"output_tokens_bound\":1,"
        "\"output_count_request_sha256\":\"" HASH0 "\",\"source_sha256\":\"" HASH0 "\","
        "\"output_sha256\":\"3d2e3e2d5f8ce97d6daa3938544911b9ca0764c564f5dfd205a1b825860984b6\","
        "\"output\":[{\"type\":\"compaction\",\"encrypted_content\":\"x\"}]}", 226},
    {"turn_yield_requested", "{\"turn_id\":\"" ID "\"}", 129},
    {"turn_cancel_requested", "{\"turn_id\":\"" ID "\"}", 130},
    {"turn_recovery", "{\"turn_id\":\"" ID "\",\"class\":\"\",\"message\":\"\"}", 131},
    {"turn_completed", "{\"turn_id\":\"" ID "\",\"final_response_id\":\"" NEW_ID "\","
        "\"final_item_id\":\"ffffffffffffffffffffffffffffffff\"}", 132},
    {"turn_completed_silent", "{\"turn_id\":\"" ID "\",\"response_id\":\"" NEW_ID "\","
        "\"reason\":\"room_update_quiet\"}", 133},
    {"turn_interrupted", "{\"turn_id\":\"" ID "\",\"origin\":\"user\","
        "\"reason\":\"cancelled\"}", 134},
    {"turn_failed", "{\"turn_id\":\"" ID "\",\"class\":\"internal\",\"message\":\"\"}", 135},
    {"turn_recovery", "{\"turn_id\":\"" ID "\",\"class\":\"\",\"message\":\"\","
        "\"retry_attempts\":4294967296}", 131},
    {"input_cancelled", "{}", 97},
    {"steering_deferred", "{\"turn_id\":\"" ID "\"}", 100},
    {"input_admitted", "{\"turn_id\":\"" ID "\",\"time_ms\":1,"
        "\"steering_ids\":[\"" NEW_ID "\",\"" ID "\"]}", 101},
    {"future_queue_state", "{\"armed\":true}", 102},
    {"future_turn_cancelled", "{\"reason\":\"user\","
        "\"queue_ids\":[\"" NEW_ID "\",\"" ID "\"]}", 103},
    {"input_received", "{\"provider\":\"p\",\"model\":\"m\",\"effort\":\"e\","
        "\"text\":\"t\",\"instructions\":[],\"read_only\":false,\"received_at_ms\":0}", 96},
    {"steering_added", "{\"steering_id\":\"" ID "\",\"turn_id\":\"" NEW_ID "\","
        "\"text\":\"t\"}", 98},
    {"irc_reply_reminder", "{\"steering_id\":\"" ID "\",\"turn_id\":\"" NEW_ID "\","
        "\"text\":\"" SNAG_IRC_REPLY_REMINDER_TEXT "\"}", 99},
    {"future_turn_queued", "{\"queue_id\":\"" ID "\",\"read_only\":false,\"text\":\"t\","
        "\"while_turn_id\":\"\"}", 104},
    {"future_turn_edited", "{\"queue_id\":\"" ID "\",\"read_only\":false,\"text\":\"t\"}", 105},
    {"future_turn_queued", "{\"queue_id\":\"" VOICE_QUEUE_ID "\",\"read_only\":false,"
        "\"text\":\"t\",\"while_turn_id\":null,\"voice\":{\"connection_id\":\"" ID "\","
        "\"input_id\":\"i\",\"response_id\":\"r\",\"call_id\":\"c\",\"provider\":\"p\","
        "\"model\":\"m\",\"transcript\":\"a\",\"request\":\"q\"}}", 104},
    {"turn_started", "{\"turn_id\":\"" ID "\",\"turn_number\":1,\"input_kind\":\"direct\","
        "\"queue_id\":null,\"queue_seq\":null,\"workspace\":\"/w\",\"text\":\"t\","
        "\"instructions\":[],\"config\":{\"provider\":\"p\",\"model\":\"m\","
        "\"effort\":\"e\"}}", 128},
    {"turn_started", "{\"turn_id\":\"" ID "\",\"turn_number\":2,\"input_kind\":\"direct\","
        "\"queue_id\":null,\"queue_seq\":null,\"cwd\":\"/w\",\"text\":\"t\","
        "\"read_only\":false,\"received_at_ms\":0,\"instructions\":[],\"config\":{"
        "\"provider\":\"p\",\"model\":\"m\",\"effort\":\"e\",\"max_parallel_commands\":4,"
        "\"parallel_tool_calls\":false}}", 128},
    {"turn_started", "{\"turn_id\":\"" ID "\",\"turn_number\":1,\"input_kind\":\"queued\","
        "\"queue_id\":\"" NEW_ID "\",\"queue_seq\":1,\"cwd\":\"C:\\\\w\",\"text\":\"t\","
        "\"read_only\":true,\"instructions\":[{\"bytes\":0,\"path\":\"C:\\\\a\","
        "\"sha256\":\"" HASHF "\"},\"/b\"],\"config\":{"
        "\"provider\":\"p\",\"model\":\"m\",\"effort\":\"e\"}}", 128},
    {"response_started", "{" RESPONSE_START "}", 160},
    {"response_started", "{" RESPONSE_START ",\"provider\":\"p\",\"effort\":\"e\","
        "\"capacity_source\":\"unknown\",\"source_bound\":false,\"hard_input_tokens\":null,"
        "\"requested_output_tokens\":null,\"model_input_bytes\":1,\"request_input_bytes\":1,"
        "\"request_input_count\":0,\"provider_source_sha256\":\"" HASHF "\","
        "\"request_input_sha256\":\"" HASH0 "\"}", 160},
    {"response_output", "{" RESPONSE_IDS
        ",\"index\":0,\"offset\":0,\"item\":" PUBLIC_ITEM "}", 161},
    {"response_interrupted", "{" RESPONSE_IDS ",\"origin\":\"user\",\"reason\":\"cancelled\","
        "\"partial_public\":[]}", 162},
    {"response_failed", "{" RESPONSE_IDS ",\"class\":\"internal\",\"retry_count\":0,"
        "\"message\":\"\",\"partial_public\":[]}", 163},
    {"response_output_correction", "{" RESPONSE_IDS ",\"correction_id\":\"" ID "\","
        "\"text\":\"" SNAG_EMPTY_OUTPUT_CORRECTION "\",\"partial_public\":[]}", 164},
    {"response_completed", "{" RESPONSE_IDS ",\"provider_response_id\":\"p\","
        "\"status\":\"completed\",\"items\":[],\"usage\":" EMPTY_USAGE "}", 165},
    {"tool_started", "{\"turn_id\":\"" ID "\",\"call_id\":\"" NEW_ID "\","
        "\"action_sha256\":\"" HASHF "\",\"resolved_workdir\":\"/\"}", 176},
    {"process_output", "{\"turn_id\":\"" ID "\",\"handle\":\"" NEW_ID "\",\"stream\":0,"
        "\"offset\":0,\"encoding\":\"utf8\",\"data\":\"t\"}", 192},
    {"process_output", "{\"turn_id\":\"" ID "\",\"handle\":\"" NEW_ID "\",\"stream\":1,"
        "\"offset\":7,\"encoding\":\"base64\",\"data\":\"AP8=\"}", 192},
    {"tool_finished", "{\"turn_id\":\"" ID "\",\"call_id\":\"" NEW_ID "\","
        "\"result\":" TOOL_RESULT "}", 177},
    {"process_closed", "{\"turn_id\":\"" ID "\",\"handle\":\"" NEW_ID "\","
        "\"cause\":\"user_interrupt\",\"result\":" TOOL_RESULT "}", 193},
    {"irc_event", "{" IRC_EVENT "}", 208},
    {"irc_event", "{" IRC_EVENT ",\"stream\":\"" ID "\",\"sequence\":1,\"input\":true,"
        "\"urgent\":true,\"reply\":true}", 208},
    {"irc_snapshot", "{\"reason\":\"join\",\"text\":\"t\",\"timestamp_ms\":1}", 209},
    {"irc_admitted", "{\"sequences\":[1,128,9223372036854775807]}", 210},
    {"voice_transfer_sealed", "{" TRANSFER "}", 265},
    {"voice_transfer_adopted", "{" TRANSFER ",\"begin_offset\":1,\"begin_seq\":2,"
        "\"begin_sha256\":\"" HASHF "\"}", 266},
    {"voice_transfer_record", "{\"transfer_id\":\"" ID "\",\"target_session_id\":\"" NEW_ID "\","
        "\"source_session_id\":\"" ID "\",\"source_seq\":1,\"source_type\":\"session_checkpoint\","
        "\"data\":{\"covers_through_seq\":257,\"provider_view\":true,\"snapshot_v\":2}}", 264},
    {"session_named", "{\"name\":\"work é\"}", 13},
    {"session_options", "{\"args\":[\"--config\",\"./config\",\"--markdown\",\"-v\","
        "\"--listen\",\"[::1]:1234\",\"--no-listen\"]}", 14},
    {"hosted_search_started", "{\"turn_id\":\"" ID "\",\"item_id\":\"ws_search\"}", 167},
    {"hosted_search_finished", "{\"turn_id\":\"" ID "\",\"item_id\":\"ws_search\","
        "\"status\":\"completed\"}", 168},
    {"fallback_changed", "{\"value\":\"p/m/high:100000\"}", 19},
    {"turn_fallback_started", "{\"turn_id\":\"" ID "\",\"provider\":\"p\","
        "\"model\":\"m\",\"effort\":\"high\",\"context_mode\":\"tokens\","
        "\"context_tokens\":100000}", 20},
};

static void
reject_json(const char *type, const json_t *data)
{
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_buf_append(&bytes, "keep", 4u));
    enum snag_binary_kind kind = SNAG_BINARY_VOICE_EVENT;
    assert(snag_binary_legacy_encode(&bytes, type, data, &kind) < 0);
    assert(kind == SNAG_BINARY_VOICE_EVENT);
    assert(bytes.len == 4u && !memcmp(bytes.data, "keep", 4u));
    snag_buf_free(&bytes);
}

static void
reject_record(const struct snag_binary_record *record)
{
    const char *type = "keep";
    json_t *data = json_true();
    assert(snag_binary_legacy_decode(record, &type, &data) < 0);
    assert(!strcmp(type, "keep") && data == json_true());
}

static void
roundtrip_checked(const char *type, const json_t *data, unsigned int expected_kind, bool prefixes)
{
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_buf_append(&bytes, "pre", 3u));
    enum snag_binary_kind kind = 0;
    assert(!snag_binary_legacy_encode(&bytes, type, data, &kind));
    assert((unsigned int)kind == expected_kind && !memcmp(bytes.data, "pre", 3u));
    uint16_t version = (expected_kind >= 96u && expected_kind <= 105u) ||
        expected_kind == 177u || expected_kind == 193u || expected_kind == 266u ? 2u : 1u;
    assert(snag_binary_event_version(kind) == version);
    struct snag_binary_record record = {
        .kind = (uint16_t)kind, .version = version,
        .payload = bytes.data + 3u, .size = bytes.len - 3u
    };
    const char *decoded_type = NULL;
    json_t *decoded = NULL;
    assert(!snag_binary_legacy_decode(&record, &decoded_type, &decoded));
    assert(!strcmp(type, decoded_type) && json_equal(data, decoded));
    struct snag_buf before = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf after = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_json_canonical(data, &before));
    assert(!snag_json_canonical(decoded, &after));
    assert(before.len == after.len && !memcmp(before.data, after.data, before.len));
    snag_buf_free(&before);
    snag_buf_free(&after);
    json_decref(decoded);
    if (expected_kind >= 96u && expected_kind <= 105u) {
        /* Version 2 adds references; these literal layouts are unchanged. */
        record.version = 1u;
        decoded = NULL;
        assert(!snag_binary_legacy_decode(&record, &decoded_type, &decoded));
        assert(!strcmp(type, decoded_type) && json_equal(data, decoded));
        json_decref(decoded);
        record.version = version;
    }
    if (prefixes) {
        for (size_t n = 0u; n < record.size; ++n) {
            struct snag_binary_record short_record = record;
            short_record.size = n;
            reject_record(&short_record);
        }
    }
    record.version = version + 1u;
    reject_record(&record);
    record.version = 0u;
    reject_record(&record);
    record.version = version;
    record.flags = 1u;
    reject_record(&record);
    snag_buf_free(&bytes);
}

static void
roundtrip(const char *type, const json_t *data, unsigned int kind)
{
    roundtrip_checked(type, data, kind, true);
}

static void
samples_and_shapes(void)
{
    for (size_t i = 0u; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        json_t *data = json_loads(samples[i].data, JSON_REJECT_DUPLICATES, NULL);
        assert(data);
        roundtrip(samples[i].type, data, samples[i].kind);
        assert(!json_object_set_new(data, "unknown", json_true()));
        reject_json(samples[i].type, data);
        assert(!json_object_del(data, "unknown"));
        const char *key;
        json_t *value;
        json_object_foreach(data, key, value) {
            json_t *copy = json_copy(data);
            assert(copy && !json_object_del(copy, key));
            if ((samples[i].kind == 131u && !strcmp(key, "retry_attempts")) ||
                (samples[i].kind == 128u &&
                 (!strcmp(key, "read_only") || !strcmp(key, "received_at_ms")))) {
                roundtrip(samples[i].type, copy, samples[i].kind);
            } else {
                reject_json(samples[i].type, copy);
            }
            assert(!json_object_set_new(copy, key, json_null()));
            bool nullable = samples[i].kind == 248u ||
                (samples[i].kind == 160u && (!strcmp(key, "baseline_sha256") ||
                 !strcmp(key, "compact_id") || !strcmp(key, "hard_input_tokens") ||
                 !strcmp(key, "requested_output_tokens"))) ||
                (samples[i].kind == 128u && json_is_null(json_object_get(data, key)) &&
                 (!strcmp(key, "queue_id") || !strcmp(key, "queue_seq"))) ||
                (samples[i].kind == 104u && json_object_get(data, "voice") &&
                 !strcmp(key, "while_turn_id")) ||
                (samples[i].kind == 224u && !strcmp(key, "predecessor_compact_id")) ||
                (samples[i].kind == 166u &&
                (!strcmp(key, "context_limit_tokens") || !strcmp(key, "requested_input_tokens") ||
                 !strcmp(key, "observed_hard_input_tokens")));
            if (nullable) {
                roundtrip(samples[i].type, copy, samples[i].kind);
            } else {
                reject_json(samples[i].type, copy);
            }
            json_decref(copy);
        }
        json_decref(data);
    }
}

static void
wire_bytes(void)
{
    static const unsigned char created[] = {
        2, 0, 1, 1, 0, 0, 0, 'p', 1, 0, 0, 0, 'm', 1, 0, 0, 0, 'e', 1, 0, 0, 0, '/'
    };
    static const unsigned char timer[] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 't'
    };
    static const unsigned char completed[] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 2
    };
    static const unsigned char deletion[] = {
        0x01, 0x23, 0x45, 0x67,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
        0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00
    };
    static const unsigned char selection[] = {
        1, 0, 0, 0, 'p', 1, 0, 0, 0, 'm', 1, 0, 0, 0, 'e',
        1, 0, 0, 0, 'q', 1, 0, 0, 0, 'n', 1, 0, 0, 0, 'f'
    };
    static const unsigned char context[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 0, 1, 0, 0, 0, 0, 0
    };
    static const unsigned char billing[] = {
        1, 0, 0, 0, 'p', 1, 0, 0, 0, 'm', 1, 0, 0, 0, 'r'
    };
    unsigned char transform[84] = {0};
    memcpy(transform, completed, 16u);
    memset(transform + 48u, 0xff, 32u);
    static const unsigned char control[] = {1, 0};
    static const unsigned char started[] = {2};
    static const unsigned char finished[] = {4};
    static const unsigned char boundary[] = {4, 1, 1, 1, 0, 0, 0, 0, 0, 0};
    unsigned char download[83] = {0};
    memcpy(download, completed, 16u);
    download[56] = 1u;
    download[64] = 1u;
    download[65] = 1u;
    download[72] = 2u;
    download[76] = '/';
    download[77] = 'a';
    download[78] = 1u;
    download[82] = 'a';
    unsigned char rebase[17] = {0};
    memcpy(rebase, completed, 16u);
    rebase[16] = 1u;
    unsigned char rejection[128] = {0};
    memcpy(rejection, completed, 16u);
    memcpy(rejection + 16u, deletion + 20u, 16u);
    rejection[32] = 1u;
    memset(rejection + 68u, 0xff, 32u);
    rejection[109] = 0x28u;
    rejection[110] = 0x6bu;
    rejection[111] = 0xeeu;
    rejection[116] = 1u;
    unsigned char compact_start[146] = {0};
    memcpy(compact_start, completed, 16u);
    compact_start[17] = 1u;
    compact_start[18] = 3u;
    compact_start[19] = 1u;
    memset(compact_start + 67u, 0xff, 32u);
    compact_start[131] = compact_start[136] = compact_start[141] = 1u;
    compact_start[135] = 'm';
    compact_start[140] = 'c';
    compact_start[145] = 'p';
    static const unsigned char output[] = "[{\"encrypted_content\":\"x\",\"type\":\"compaction\"}]";
    static const unsigned char output_digest[32] = {
        0x3d, 0x2e, 0x3e, 0x2d, 0x5f, 0x8c, 0xe9, 0x7d,
        0x6d, 0xaa, 0x39, 0x38, 0x54, 0x49, 0x11, 0xb9,
        0xca, 0x07, 0x64, 0xc5, 0x64, 0xf5, 0xdf, 0xd2,
        0x05, 0xa1, 0xb8, 0x25, 0x86, 0x09, 0x84, 0xb6
    };
    unsigned char compact_done[135u + sizeof(output) - 1u] = {0};
    memcpy(compact_done, completed, 16u);
    compact_done[17] = 3u;
    compact_done[18] = 1u;
    compact_done[27] = 1u;
    memcpy(compact_done + 99u, output_digest, sizeof(output_digest));
    compact_done[131] = sizeof(output) - 1u;
    memcpy(compact_done + 135u, output, sizeof(output) - 1u);
    unsigned char recovery[25] = {0};
    memcpy(recovery, completed, 16u);
    unsigned char retry[33] = {0};
    memcpy(retry, completed, 16u);
    retry[16] = retry[21] = 1u;
    unsigned char turn_complete[48] = {0};
    memcpy(turn_complete, completed, 16u);
    memcpy(turn_complete + 16u, deletion + 20u, 16u);
    memset(turn_complete + 32u, 0xff, 16u);
    unsigned char silent[33] = {0};
    memcpy(silent, turn_complete, 32u);
    silent[32] = 1u;
    unsigned char interrupted[18] = {0};
    memcpy(interrupted, completed, 16u);
    interrupted[16] = interrupted[17] = 1u;
    unsigned char failed[21] = {0};
    memcpy(failed, completed, 16u);
    failed[16] = 8u;
    unsigned char admission[60] = {0};
    memcpy(admission, completed, 16u);
    admission[16] = 1u;
    admission[24] = 2u;
    memcpy(admission + 28u, deletion + 20u, 16u);
    memcpy(admission + 44u, completed, 16u);
    unsigned char queue_cancel[37] = {1u, 2u};
    memcpy(queue_cancel + 5u, deletion + 20u, 16u);
    memcpy(queue_cancel + 21u, completed, 16u);
    static const unsigned char receipt[] = "\0\0\0\0\0\0\0\0\0\0"
        "\1\0\0\0p\1\0\0\0m\1\0\0\0e\0\0\0\0\1\0\0\0t";
    unsigned char steering[38] = {0};
    memcpy(steering, completed, 16u);
    memcpy(steering + 16u, deletion + 20u, 16u);
    steering[33] = 1u;
    steering[37] = 't';
    unsigned char reminder[37u + sizeof(SNAG_IRC_REPLY_REMINDER_TEXT) - 1u] = {0};
    assert(sizeof(SNAG_IRC_REPLY_REMINDER_TEXT) - 1u < 256u);
    memcpy(reminder, steering, 32u);
    reminder[33] = (unsigned char)(sizeof(SNAG_IRC_REPLY_REMINDER_TEXT) - 1u);
    memcpy(reminder + 37u, SNAG_IRC_REPLY_REMINDER_TEXT, sizeof(SNAG_IRC_REPLY_REMINDER_TEXT) - 1u);
    unsigned char queued[23] = {0};
    memcpy(queued, completed, 16u);
    queued[18] = 1u;
    queued[22] = 't';
    unsigned char edited[22] = {0};
    memcpy(edited, completed, 16u);
    edited[17] = 1u;
    edited[21] = 't';
    unsigned char voiced[74] = {
        0x11u, 0xadu, 0xbau, 0xa8u, 0xfau, 0xfdu, 0xdbu, 0xccu,
        0xd7u, 0x2au, 0xbeu, 0x5fu, 0xffu, 0x64u, 0x8eu, 0x9eu, 32u, 2u, 1u
    };
    voiced[22] = 't';
    memcpy(voiced + 23u, completed, 16u);
    for (size_t i = 0u; i < 7u; ++i) {
        voiced[39u + 5u * i] = 1u;
        voiced[43u + 5u * i] = (unsigned char)"ircpmaq"[i];
    }
    unsigned char turn_start[] = "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\1\0\0\0\0\0\0\0\0\20\2\0\0\0/w\1\0\0\0p\1\0\0\0m\1\0\0\0e"
        "\0\0\6\7\0\0\0\0\1\0\0\0t";
    memcpy(turn_start, completed, 16u);
    unsigned char response_start[162] = {0};
    memcpy(response_start + 2u, completed, 16u);
    memcpy(response_start + 18u, deletion + 20u, 16u);
    response_start[34] = response_start[38] = 1u;
    memset(response_start + 79u, 0xff, 32u);
    memcpy(response_start + 143u, "\1\0\0\0m\1\0\0\0c\1\0\0\0p", 15u);
    unsigned char response_full[261] = {0};
    memcpy(response_full, response_start, sizeof(response_start));
    response_full[0] = 1u;
    memcpy(response_full + 162u, "\1\0\0\0p\1\0\0\0e\1", 11u);
    memset(response_full + 173u, 0xff, 32u);
    response_full[237] = response_full[245] = 1u;
    unsigned char response_output[80] = {0};
    memcpy(response_output, response_start + 2u, 36u);
    response_output[52] = response_output[53] = 1u;
    memcpy(response_output + 54u, completed, 16u);
    memcpy(response_output + 70u, "\1\0\0\0p\1\0\0\0t", 10u);
    unsigned char response_interrupted[42] = {0}, response_failed[47] = {0};
    memcpy(response_interrupted, response_start + 2u, 36u);
    response_interrupted[36] = response_interrupted[37] = 1u;
    memcpy(response_failed, response_start + 2u, 36u);
    response_failed[36] = 8u;
    unsigned char correction[60u + sizeof(SNAG_EMPTY_OUTPUT_CORRECTION) - 1u] = {0};
    memcpy(correction, response_start + 2u, 36u);
    memcpy(correction + 36u, completed, 16u);
    correction[52] = sizeof(SNAG_EMPTY_OUTPUT_CORRECTION) - 1u;
    memcpy(correction + 56u, SNAG_EMPTY_OUTPUT_CORRECTION,
        sizeof(SNAG_EMPTY_OUTPUT_CORRECTION) - 1u);
    unsigned char response_complete[47] = {0};
    memcpy(response_complete, response_start + 2u, 36u);
    memcpy(response_complete + 36u, "\1\0\0\0p", 5u);
    unsigned char tool_start[69] = {0};
    memcpy(tool_start, response_start + 2u, 32u);
    memset(tool_start + 32u, 0xff, 32u);
    tool_start[64] = 1u;
    tool_start[68] = '/';
    unsigned char process_text[47] = {0}, process_binary[48] = {0};
    memcpy(process_text, response_start + 2u, 32u);
    process_text[41] = process_text[42] = 1u;
    process_text[46] = 't';
    memcpy(process_binary, response_start + 2u, 32u);
    process_binary[32] = 7u;
    process_binary[40] = 1u;
    process_binary[41] = process_binary[42] = 2u;
    process_binary[47] = 0xff;
    unsigned char tool_result[107] = {0}, process_closed[108] = {0};
    memcpy(tool_result, tool_start, 32u);
    tool_result[32] = 6u;
    tool_result[43] = 128u;
    tool_result[49] = tool_result[78] = 1u;
    memcpy(process_closed, tool_start, 32u);
    process_closed[32] = 1u;
    memcpy(process_closed + 33u, tool_result + 32u, sizeof(tool_result) - 32u);
    unsigned char irc_legacy[28] = {7u, 0u, 0u, 1u};
    irc_legacy[11] = 1u;
    irc_legacy[15] = 'e';
    unsigned char irc_modern[52] = {7u, 248u, 1u, 1u};
    memcpy(irc_modern + 11u, completed, 16u);
    irc_modern[27] = 1u;
    memcpy(irc_modern + 35u, irc_legacy + 11u, 17u);
    unsigned char irc_snapshot[14] = {1u, 1u};
    irc_snapshot[9] = 1u;
    irc_snapshot[13] = 't';
    unsigned char irc_admitted[18] = {3u, 0u, 0u, 0u, 1u, 128u, 1u};
    memset(irc_admitted + 7u, 255, 8u);
    irc_admitted[15] = 127u;
    unsigned char voice_seal[64] = {0}, voice_adopted[113] = {0};
    memcpy(voice_seal, tool_start, 32u);
    memcpy(voice_seal + 32u, completed, 16u);
    voice_seal[48] = voice_seal[56] = 1u;
    memcpy(voice_adopted + 1u, voice_seal, sizeof(voice_seal));
    voice_adopted[65] = 1u;
    voice_adopted[73] = 2u;
    memset(voice_adopted + 81u, 255, 32u);
    const struct { size_t sample; const unsigned char *bytes; size_t size; } cases[] = {
        {61u, turn_start, sizeof(turn_start) - 1u},
        {64u, response_start, sizeof(response_start)}, {65u, response_full, sizeof(response_full)},
        {66u, response_output, sizeof(response_output)},
        {67u, response_interrupted, sizeof(response_interrupted)},
        {68u, response_failed, sizeof(response_failed)}, {69u, correction, sizeof(correction)},
        {70u, response_complete, sizeof(response_complete)},
        {71u, tool_start, sizeof(tool_start)}, {72u, process_text, sizeof(process_text)},
        {73u, process_binary, sizeof(process_binary)},
        {74u, tool_result, sizeof(tool_result)}, {75u, process_closed, sizeof(process_closed)},
        {76u, irc_legacy, sizeof(irc_legacy)}, {77u, irc_modern, sizeof(irc_modern)},
        {78u, irc_snapshot, sizeof(irc_snapshot)}, {79u, irc_admitted, sizeof(irc_admitted)},
        {80u, voice_seal, sizeof(voice_seal)}, {81u, voice_adopted, sizeof(voice_adopted)},
        {0u, created, sizeof(created)}, {3u, timer, sizeof(timer)},
        {12u, completed, sizeof(completed)}, {18u, deletion, sizeof(deletion)},
        {21u, selection, sizeof(selection)}, {24u, context, sizeof(context)},
        {27u, transform, sizeof(transform)}, {28u, billing, sizeof(billing)},
        {30u, control, sizeof(control)}, {31u, started, sizeof(started)},
        {32u, finished, sizeof(finished)}, {33u, boundary, sizeof(boundary)},
        {34u, download, sizeof(download)}, {37u, rebase, sizeof(rebase)},
        {38u, rejection, sizeof(rejection)}, {39u, compact_start, sizeof(compact_start)},
        {40u, completed, sizeof(completed)}, {41u, compact_done, sizeof(compact_done)},
        {42u, completed, 16u}, {43u, completed, 16u}, {44u, recovery, sizeof(recovery)},
        {45u, turn_complete, sizeof(turn_complete)}, {46u, silent, sizeof(silent)},
        {47u, interrupted, sizeof(interrupted)}, {48u, failed, sizeof(failed)},
        {49u, retry, sizeof(retry)}, {50u, completed, 0u}, {51u, completed, 16u},
        {52u, admission, sizeof(admission)}, {53u, control, 1u},
        {54u, queue_cancel, sizeof(queue_cancel)}, {55u, receipt, sizeof(receipt) - 1u},
        {56u, steering, sizeof(steering)}, {57u, reminder, sizeof(reminder)},
        {58u, queued, sizeof(queued)}, {59u, edited, sizeof(edited)}, {60u, voiced, sizeof(voiced)}
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const size_t n = cases[i].sample;
        json_t *data = json_loads(samples[n].data, JSON_REJECT_DUPLICATES, NULL);
        struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
        enum snag_binary_kind kind;
        assert(data && !snag_binary_legacy_encode(&bytes, samples[n].type, data, &kind));
        assert(bytes.len == cases[i].size &&
            (!bytes.len || !memcmp(bytes.data, cases[i].bytes, bytes.len)));
        snag_buf_free(&bytes);
        json_decref(data);
    }
}

static void
variants_and_errors(void)
{
    static const char *const reasons[] = {
        "input_closed", "provider_policy", "refusal", "session_resumed", "turn_stopped", "user"
    };
    json_t *data = json_loads(samples[10].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    for (size_t i = 0u; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        assert(!json_object_set_new(data, "reason", json_string(reasons[i])));
        roundtrip("goal_paused", data, 68u);
    }
    assert(!json_object_set_new(data, "reason", json_string("model")));
    reject_json("goal_paused", data);
    json_decref(data);
    data = json_loads(samples[9].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_set_new(data, "locked", json_false()));
    roundtrip("goal_lock_changed", data, 67u);
    assert(!json_object_set_new(data, "locked", json_integer(1)));
    reject_json("goal_lock_changed", data);
    json_decref(data);
    data = json_loads(samples[12].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_set_new(data, "actor", json_string("user")));
    roundtrip("goal_completed", data, 70u);
    reject_json("goal_blocked", data);
    assert(!json_object_set_new(data, "actor", json_string("Model")));
    reject_json("goal_completed", data);
    assert(!json_object_set_new(data, "actor", json_stringn("user\0model", 10u)));
    reject_json("goal_completed", data);
    json_decref(data);
    data = json_loads(samples[3].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    const json_int_t due[] = {-1, 0, 1, INT64_MAX};
    for (size_t i = 0u; i < sizeof(due) / sizeof(due[0]); ++i) {
        assert(!json_object_set_new(data, "due_ms", json_integer(due[i])));
        if (due[i] <= 0) reject_json("timer_scheduled", data);
        else roundtrip("timer_scheduled", data, 32u);
    }
    assert(!json_object_set_new(data, "due_ms", json_real(1.0)));
    reject_json("timer_scheduled", data);
    assert(!json_object_set_new(data, "due_ms", json_integer(1)));
    assert(!json_object_set_new(data, "timer_id", json_string("0123456789ABCDEF0011223344556677")));
    reject_json("timer_scheduled", data);
    assert(!json_object_set_new(data, "timer_id", json_string(ID)));
    assert(!json_object_set_new(data, "text", json_stringn("x\0y", 3u)));
    reject_json("timer_scheduled", data);
    assert(!json_object_set_new(data, "text", json_stringn_nocheck("\xff", 1u)));
    reject_json("timer_scheduled", data);
    json_decref(data);
}

static void
bounds_and_atomicity(void)
{
    json_t *data = json_loads(samples[6].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    char *text = malloc(SNAG_MAX_GOAL_PROMPT + 1u);
    assert(text);
    memset(text, 'x', SNAG_MAX_GOAL_PROMPT + 1u);
    assert(!json_object_set_new(data, "prompt", json_stringn(text, SNAG_MAX_GOAL_PROMPT)));
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    enum snag_binary_kind kind = SNAG_BINARY_GOAL_STARTED;
    assert(!snag_binary_legacy_encode(&bytes, "goal_started", data, &kind));
    struct snag_binary_record record = {
        .kind = 64u, .version = 1u, .payload = bytes.data, .size = bytes.len
    };
    const char *type;
    json_t *decoded;
    assert(!snag_binary_legacy_decode(&record, &type, &decoded));
    assert(json_equal(data, decoded));
    json_decref(decoded);
    snag_buf_free(&bytes);
    assert(!json_object_set_new(data, "prompt", json_stringn(text, SNAG_MAX_GOAL_PROMPT + 1u)));
    reject_json("goal_started", data);
    free(text);
    assert(!json_object_set_new(data, "prompt", json_string("small")));
    struct snag_buf tiny = {.max = 4u};
    assert(!snag_buf_append(&tiny, "keep", 4u));
    kind = SNAG_BINARY_VOICE_EVENT;
    assert(snag_binary_legacy_encode(&tiny, "goal_started", data, &kind) < 0);
    assert(kind == SNAG_BINARY_VOICE_EVENT && tiny.len == 4u && !memcmp(tiny.data, "keep", 4u));
    snag_buf_free(&tiny);
    json_decref(data);
    unsigned char timer[29] = {0};
    memset(timer + 16u, 0xff, 8u);
    timer[24] = 1u;
    timer[28] = 'x';
    record = (struct snag_binary_record){.kind = 32u, .version = 1u,
        .payload = timer, .size = sizeof(timer)};
    struct snag_binary_event event;
    assert(!snag_binary_event_decode(&record, &event));
    reject_record(&record);
}

static void
creation_and_key_errors(void)
{
    json_t *data = json_loads(samples[0].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    const json_int_t formats[] = {0, 1, 5, 65538, INT64_MAX};
    for (size_t i = 0u; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        assert(!json_object_set_new(data, "format", json_integer(formats[i])));
        reject_json("session_created", data);
    }
    assert(!json_object_set_new(data, "format", json_integer(2)));
    assert(!json_object_set_new(data, "cwd", json_string("/")));
    reject_json("session_created", data);
    assert(!json_object_del(data, "workspace"));
    reject_json("session_created", data);
    assert(!json_object_set_new(data, "format", json_integer(4)));
    roundtrip("session_created", data, 1u);
    assert(!json_object_set_new(data, "protocol", json_string("other")));
    reject_json("session_created", data);
    assert(!json_object_set_new(data, "protocol", json_stringn("responses\0", 10u)));
    reject_json("session_created", data);
    json_decref(data);
    data = json_loads(samples[11].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_set_new(data, "actor", json_string("user")));
    reject_json("goal_blocked", data);
    json_decref(data);
    data = json_loads(samples[14].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_setn_new(data, "goal_id\0", 8u, json_string(ID)));
    reject_json("goal_cancelled", data);
    assert(!json_object_del(data, "goal_id"));
    reject_json("goal_cancelled", data);
    json_decref(data);
}

static void
metadata_variants(void)
{
    static const char *const steering[] = {"", "mentions", "all"};
    json_t *data = json_object();
    assert(data);
    for (size_t i = 0u; i < sizeof(steering) / sizeof(steering[0]); ++i) {
        assert(!json_object_set_new(data, "mode", json_string(steering[i])));
        roundtrip("steering_updated", data, 7u);
    }
    assert(!json_object_set_new(data, "mode", json_string("default")));
    reject_json("steering_updated", data);
    json_decref(data);
    data = json_loads(samples[24].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    static const char *const modes[] = {"default", "max", "tokens"};
    for (size_t i = 0u; i < 3u; ++i) {
        for (size_t j = 0u; j < 3u; ++j) {
            assert(!json_object_set_new(data, "old_mode", json_string(modes[i])));
            assert(!json_object_set_new(data, "new_mode", json_string(modes[j])));
            assert(!json_object_set_new(data, "old_tokens", json_integer(i == 2u ? 1 : 0)));
            json_int_t tokens = j == 2u ? 4000000000LL : 0;
            assert(!json_object_set_new(data, "new_tokens", json_integer(tokens)));
            roundtrip("context_selection_changed", data, 11u);
        }
    }
    const json_int_t bad_tokens[] = {-1, 0, 4000000001LL};
    for (size_t i = 0u; i < sizeof(bad_tokens) / sizeof(bad_tokens[0]); ++i) {
        assert(!json_object_set_new(data, "new_tokens", json_integer(bad_tokens[i])));
        reject_json("context_selection_changed", data);
    }
    assert(!json_object_set_new(data, "new_tokens", json_integer(1)));
    assert(!json_object_set_new(data, "new_mode", json_string("max")));
    reject_json("context_selection_changed", data);
    assert(!json_object_set_new(data, "new_mode", json_string("other")));
    reject_json("context_selection_changed", data);
    json_decref(data);
    data = json_loads(samples[18].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    const char *const bad_trash[] = {
        ID ":" NEW_ID, ID "." NEW_ID "0", ID ".", "g" ID "." NEW_ID,
        "0123456789ABCDEF0011223344556677." NEW_ID,
        ID ".fedcba9876543210776655443322110z"
    };
    for (size_t i = 0u; i < sizeof(bad_trash) / sizeof(bad_trash[0]); ++i) {
        assert(!json_object_set_new(data, "trash_name", json_string(bad_trash[i])));
        reject_json("session_delete_requested", data);
    }
    assert(!json_object_set_new(data, "trash_name", json_string(ID "." NEW_ID)));
    const char *const bad_prefix[] = {"", "0123456", "012345678", "abcdef0G", "zbcdef01"};
    for (size_t i = 0u; i < sizeof(bad_prefix) / sizeof(bad_prefix[0]); ++i) {
        assert(!json_object_set_new(data, "confirmed_id_prefix", json_string(bad_prefix[i])));
        reject_json("session_delete_requested", data);
    }
    json_decref(data);
    data = json_loads(samples[16].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_set_new(data, "origin", json_string("model")));
    reject_json("session_archived", data);
    reject_json("session_unarchived", data);
    json_decref(data);
}

static void
session_metadata_variants(void)
{
    const char *const names[] = {"work é", " work ", "a/b", "", " ", "a\tb", "a\nb", "a\177"};
    for (size_t i = 0u; i < sizeof(names) / sizeof(*names); ++i) {
        json_t *data = json_pack("{s:s}", "name", names[i]);
        assert(data);
        if (i < 3u) roundtrip("session_named", data, 13u);
        else reject_json("session_named", data);
        json_decref(data);
    }
    const char *const options[] = {
        "[]", "[\"-v\",\"-v\"]", "[\"--config\",\"--no-client\"]",
        "[\"--config\",\"é\",\"-d\",\"./state\",\"--color\",\"never\","
        "\"--listen\",\"[::1]:1234\",\"--client\",\"host:1\","
        "\"--model-nick\",\"agent\",\"--operator-nick\",\"operator\","
        "\"--room-name\",\"#room\",\"--no-listen\",\"--no-client\","
        "\"--markdown\",\"--no-markdown\",\"-v\"]",
        "null", "{}", "[true]", "[1]", "[null]", "[\"--config\"]",
        "[\"--unknown\"]", "[\"--color\",\"\"]", "[\"-v\",\"extra\"]"
    };
    for (size_t i = 0u; i < sizeof(options) / sizeof(*options); ++i) {
        json_t *args = json_loads(options[i], JSON_DECODE_ANY, NULL);
        json_t *data = json_object();
        assert(args && data && !json_object_set_new(data, "args", args));
        if (i < 4u) roundtrip("session_options", data, 14u);
        else reject_json("session_options", data);
        json_decref(data);
    }
    char *long_text = malloc(SNAG_PATH_MAX_BYTES + 2u);
    assert(long_text);
    memset(long_text, 'x', SNAG_PATH_MAX_BYTES + 1u);
    long_text[SNAG_PATH_MAX_BYTES + 1u] = '\0';
    for (size_t n = SNAG_PATH_MAX_BYTES; n <= SNAG_PATH_MAX_BYTES + 1u; ++n) {
        json_t *data = json_object();
        assert(data && !json_object_set_new(data, "name", json_stringn(long_text, n)));
        if (n == SNAG_PATH_MAX_BYTES) roundtrip_checked("session_named", data, 13u, false);
        else reject_json("session_named", data);
        json_decref(data);
        json_t *args = json_array();
        data = json_object();
        assert(args && data && !json_array_append_new(args, json_string("--config")));
        assert(!json_array_append_new(args, json_stringn(long_text, n)));
        assert(!json_object_set_new(data, "args", args));
        if (n == SNAG_PATH_MAX_BYTES) roundtrip_checked("session_options", data, 14u, false);
        else reject_json("session_options", data);
        json_decref(data);
    }
    free(long_text);
}

static void
session_metadata_wire(void)
{
    const unsigned char name[] = {7, 0, 0, 0, 'w', 'o', 'r', 'k', ' ', 0xc3, 0xa9};
    unsigned char options[] = {1, 0, 0, 0, '-', 'v', 0};
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    json_t *data = json_pack("{s:s}", "name", "work é");
    enum snag_binary_kind kind = 0;
    assert(data && !snag_binary_legacy_encode(&bytes, "session_named", data, &kind));
    assert(kind == 13u && bytes.len == sizeof(name) && !memcmp(bytes.data, name, sizeof(name)));
    json_decref(data);
    snag_buf_free(&bytes);
    bytes.max = SNAG_MAX_EVENT_LINE;
    data = json_pack("{s:[s]}", "args", "-v");
    assert(data && !snag_binary_legacy_encode(&bytes, "session_options", data, &kind));
    assert(kind == 14u && bytes.len == sizeof(options) &&
        !memcmp(bytes.data, options, sizeof(options)));
    struct snag_binary_options view = {.data = name, .size = sizeof(name)};
    for (size_t n = 0u; n < sizeof(options); ++n) {
        assert(snag_binary_options_decode(options, n, &view) < 0);
        assert(view.data == name && view.size == sizeof(name));
    }
    assert(!snag_binary_options_decode(options, sizeof(options), &view));
    size_t offset = 0u;
    struct snag_binary_text text = {0};
    assert(!snag_binary_options_next(&view, &offset, &text));
    assert(text.size == 2u && !memcmp(text.data, "-v", 2u) && offset == sizeof(options));
    assert(snag_binary_options_next(&view, &offset, &text) == 1 && offset == sizeof(options));
    assert(text.data == options + 4u && text.size == 2u);
    offset = 1u;
    assert(snag_binary_options_next(&view, &offset, &text) < 0 && offset == 1u);
    struct snag_binary_record record = {.kind = 14u, .version = 1u,
        .payload = options, .size = sizeof(options)};
    options[0] = 0u;
    reject_record(&record);
    options[0] = 0xffu;
    reject_record(&record);
    options[0] = 1u;
    options[5] = 'x';
    reject_record(&record);
    options[5] = 'v';
    options[4] = 0u;
    reject_record(&record);
    struct snag_buf small = {.max = 4u};
    assert(!snag_buf_append(&small, "keep", 4u));
    assert(snag_binary_options_encode(&small, json_object_get(data, "args")) < 0);
    assert(small.len == 4u && !memcmp(small.data, "keep", 4u));
    snag_buf_free(&small);
    json_decref(data);
    snag_buf_free(&bytes);
}

static void
hosted_search_variants(void)
{
    const char *const details[] = {
        "{}", "{\"type\":\"search\",\"query\":\"é\",\"extra\":[true,null,17,{\"\":false}]}",
        "[]", "[\"https://example.test/a\",\"https://example.test/a\",\"unparsed source\"]",
        "null", "1", "true", "\"text\"", "[null]", "[\"\"]", "[{}]"
    };
    for (unsigned int finished = 0u; finished < 2u; ++finished) {
        const char *type = finished ? "hosted_search_finished" : "hosted_search_started";
        unsigned int kind = finished ? 168u : 167u;
        json_t *data = json_pack("{s:s,s:s}", "turn_id", ID, "item_id", "ws_é");
        assert(data);
        if (finished) assert(!json_object_set_new(data, "status", json_string("provider status")));
        roundtrip(type, data, kind);
        for (size_t i = 0u; i < sizeof(details) / sizeof(*details); ++i) {
            json_t *detail = json_loads(details[i], JSON_DECODE_ANY, NULL);
            assert(detail && !json_object_set_new(data, finished ? "sources" : "action", detail));
            if ((finished && (i == 2u || i == 3u)) || (!finished && i < 2u)) {
                roundtrip(type, data, kind);
            } else {
                reject_json(type, data);
            }
        }
        assert(!json_object_del(data, finished ? "sources" : "action"));
        assert(!json_object_set_new(data, finished ? "action" : "sources", json_object()));
        reject_json(type, data);
        assert(!json_object_del(data, finished ? "action" : "sources"));
        if (finished) {
            char status[66];
            memset(status, 's', sizeof(status));
            for (size_t n = 64u; n <= 65u; ++n) {
                assert(!json_object_set_new(data, "status", json_stringn(status, n)));
                if (n == 64u) roundtrip(type, data, kind);
                else reject_json(type, data);
            }
            assert(!json_object_set_new(data, "status", json_string("")));
            reject_json(type, data);
            assert(!json_object_set_new(data, "status", json_string(" ")));
            roundtrip(type, data, kind);
        }
        const char *const bad_ids[] = {"", "x\n", "x\177", "x\302\200"};
        for (size_t i = 0u; i < sizeof(bad_ids) / sizeof(*bad_ids); ++i) {
            assert(!json_object_set_new(data, "item_id", json_string(bad_ids[i])));
            reject_json(type, data);
        }
        json_decref(data);
    }
    char *text = malloc(SNAG_MAX_HOSTED_ACTION + 1u);
    assert(text);
    memset(text, 'x', SNAG_MAX_HOSTED_ACTION);
    text[SNAG_MAX_HOSTED_ACTION] = '\0';
    for (unsigned int excess = 0u; excess < 2u; ++excess) {
        json_t *action = json_object();
        json_t *data = json_pack("{s:s,s:s}", "turn_id", ID, "item_id", "ws");
        assert(action && data && !json_object_set_new(action, "q",
            json_stringn(text, SNAG_MAX_HOSTED_ACTION - 8u + excess)));
        assert(!json_object_set_new(data, "action", action));
        if (!excess) roundtrip_checked("hosted_search_started", data, 167u, false);
        else reject_json("hosted_search_started", data);
        json_decref(data);
        json_t *sources = json_array();
        data = json_pack("{s:s,s:s,s:s}", "turn_id", ID, "item_id", "ws", "status", "done");
        assert(sources && data && !json_array_append_new(sources,
            json_stringn(text, SNAG_MAX_HOSTED_SOURCE_URL + excess)));
        assert(!json_object_set_new(data, "sources", sources));
        if (!excess) roundtrip_checked("hosted_search_finished", data, 168u, false);
        else reject_json("hosted_search_finished", data);
        json_decref(data);
    }
    free(text);
}

static void
hosted_search_wire(void)
{
    const unsigned char id[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    unsigned char started[25] = {0}, finished[31] = {0};
    memcpy(started, id, sizeof(id));
    started[16] = 2u;
    started[20] = 'w';
    started[21] = 's';
    started[22] = 1u;
    started[23] = 6u;
    started[24] = 7u;
    memcpy(finished, started, 22u);
    finished[22] = 2u;
    finished[26] = 'o';
    finished[27] = 'k';
    finished[28] = 1u;
    finished[29] = 5u;
    finished[30] = 7u;
    for (unsigned int end = 0u; end < 2u; ++end) {
        json_t *data = json_pack("{s:s,s:s}", "turn_id", ID, "item_id", "ws");
        assert(data);
        if (end) assert(!json_object_set_new(data, "status", json_string("ok")));
        assert(!json_object_set_new(data, end ? "sources" : "action",
            end ? json_array() : json_object()));
        struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
        enum snag_binary_kind kind = 0;
        assert(!snag_binary_legacy_encode(&bytes,
            end ? "hosted_search_finished" : "hosted_search_started", data, &kind));
        unsigned char *golden = end ? finished : started;
        size_t size = end ? sizeof(finished) : sizeof(started);
        assert((unsigned int)kind == (end ? 168u : 167u));
        assert(bytes.len == size && !memcmp(bytes.data, golden, size));
        struct snag_binary_record record = {.kind = (uint16_t)kind, .version = 1u,
            .payload = golden, .size = size};
        size_t present = end ? 28u : 22u;
        golden[present] = 2u;
        reject_record(&record);
        golden[present] = 0u;
        reject_record(&record);
        golden[present] = 1u;
        golden[present + 1u] = end ? 6u : 5u;
        reject_record(&record);
        snag_buf_free(&bytes);
        json_decref(data);
    }
}

static void
rule_voice_variants(void)
{
    json_t *data = json_loads(samples[26].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    for (size_t depth = 46u; depth <= 47u; ++depth) {
        json_t *value = json_null();
        for (size_t i = 0u; i < depth; ++i) {
            json_t *array = json_array();
            assert(array && !json_array_append_new(array, value));
            value = array;
        }
        assert(!json_object_set_new(data, "chain", value));
        if (depth == 46u) roundtrip("rule_log", data, 248u);
        else reject_json("rule_log", data);
    }
    assert(!json_object_set_new(data, "chain", json_real(1.25)));
    reject_json("rule_log", data);
    json_decref(data);
    data = json_loads(samples[29].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    json_t *body = json_object_get(data, "event");
    static const char *const types[] = {
        "voice_started", "voice_stopped", "voice_transcript", "voice_usage", "voice_asr_failed",
        "voice_interrupted", "voice_response", "voice_result", "voice_muted"
    };
    assert(!json_object_set_new(body, "turn_id", json_array()));
    assert(!json_object_set_new(body, "text", json_true()));
    for (size_t i = 0u; i < sizeof(types) / sizeof(types[0]); ++i) {
        assert(!json_object_set_new(body, "type", json_string(types[i])));
        roundtrip("voice_event", data, 257u);
    }
    assert(!json_object_set_new(body, "type", json_string("voice_caption")));
    reject_json("voice_event", data);
    assert(!json_object_set_new(body, "type", json_string("voice_transcript")));
    for (size_t depth = 45u; depth <= 46u; ++depth) {
        json_t *value = json_null();
        for (size_t i = 0u; i < depth; ++i) {
            json_t *array = json_array();
            assert(array && !json_array_append_new(array, value));
            value = array;
        }
        assert(!json_object_set_new(body, "text", value));
        if (depth == 45u) roundtrip("voice_event", data, 257u);
        else reject_json("voice_event", data);
    }
    json_decref(data);
    data = json_loads(samples[27].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_set_new(data, "original_sha256", json_string(HASH0 "0")));
    reject_json("rule_transform", data);
    assert(!json_object_set_new(data, "original_sha256", json_string(HASH0)));
    assert(!json_object_set_new(data, "effective_sha256",
        json_string("Ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff")));
    reject_json("rule_transform", data);
    json_decref(data);
    data = json_loads(samples[28].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_set_new(data, "operation", json_string("voice")));
    reject_json("audio_usage", data);
    json_decref(data);
}

static void
rule_storage_and_aggregate(void)
{
    static const char *const names[] = {"chain", "message", "rule"};
    json_t *data = json_object();
    char *text = malloc(8193u);
    assert(data && text);
    for (size_t i = 0u; i < 3u; ++i) {
        memset(text, 'a' + (int)i, 8193u);
        assert(!json_object_set_new(data, names[i], json_stringn(text, 8193u)));
    }
    free(text);
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    enum snag_binary_kind kind;
    assert(!snag_binary_legacy_encode(&bytes, "rule_log", data, &kind));
    struct snag_binary_record record = {
        .kind = 248u, .version = 1u, .payload = bytes.data, .size = bytes.len
    };
    const char *type = NULL;
    json_t *decoded = NULL;
    assert(!snag_binary_legacy_decode(&record, &type, &decoded));
    assert(kind == 248u && !strcmp(type, "rule_log") && json_equal(data, decoded));
    json_decref(decoded);
    snag_buf_free(&bytes);
    bytes = (struct snag_buf){.max = SNAG_MAX_EVENT_LINE};
    /* Canonical control-character escapes occupy six bytes each. */
    const size_t length = 1024u * 1024u;
    text = malloc(length);
    assert(text);
    memset(text, '\n', length);
    json_t *value = json_stringn(text, length);
    free(text);
    assert(value);
    for (size_t i = 0u; i < 3u; ++i) {
        assert(!json_object_set(data, names[i], value));
        assert(!snag_binary_rule_value_encode(&bytes, value));
    }
    json_decref(value);
    /* Each field and the native record fit; their combined canonical JSON does not. */
    reject_json("rule_log", data);
    record.payload = bytes.data;
    record.size = bytes.len;
    struct snag_binary_event event;
    assert(!snag_binary_event_decode(&record, &event));
    reject_record(&record);
    snag_buf_free(&bytes);
    json_decref(data);
}

static void
control_variants(void)
{
    static const char *const types[] = {"control_requested", "control_started", "control_finished"};
    json_t *data = json_object();
    assert(data);
    for (size_t i = 0u; i < 3u; ++i) {
        for (unsigned int bit = 1u; bit <= 64u; bit <<= 1u) {
            assert(!json_object_set_new(data, "control", json_integer(bit)));
            roundtrip(types[i], data, 16u + (unsigned int)i);
        }
        const json_int_t invalid[] = {-1, 0, 3, 128, 256, 4294967297LL, INT64_MAX};
        for (size_t j = 0u; j < sizeof(invalid) / sizeof(invalid[0]); ++j) {
            assert(!json_object_set_new(data, "control", json_integer(invalid[j])));
            reject_json(types[i], data);
        }
    }
    json_decref(data);
    data = json_loads(samples[33].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    reject_json("control_started", data);
    reject_json("control_finished", data);
    assert(!json_object_set_new(data, "control", json_integer(1)));
    reject_json("control_requested", data);
    assert(!json_object_set_new(data, "control", json_integer(4)));
    assert(!json_object_set_new(data, "source_seq", json_integer(0)));
    reject_json("control_requested", data);
    assert(!json_object_set_new(data, "source_seq", json_integer(INT64_MAX)));
    roundtrip("control_requested", data, 16u);
    assert(!json_object_set_new(data, "origin", json_string("manual")));
    reject_json("control_requested", data);
    json_decref(data);
}

static void
download_variants(void)
{
    json_t *data = json_loads(samples[34].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    static const char *const numbers[] = {"bytes", "mtime", "queued_ms"};
    for (size_t i = 0u; i < 3u; ++i) {
        assert(!json_object_set_new(data, numbers[i], json_integer(INT64_MAX)));
        roundtrip("download_queued", data, 240u);
        assert(!json_object_set_new(data, numbers[i], json_integer(-1)));
        reject_json("download_queued", data);
        assert(!json_object_set_new(data, numbers[i], json_integer(0)));
        roundtrip("download_queued", data, 240u);
    }
    /* Preserve source-platform name widths without consulting the current host. */
    char name[1020];
    memset(name, 'x', sizeof(name));
    assert(!json_object_set_new(data, "name", json_stringn(name, sizeof(name))));
    assert(!json_object_set_new(data, "path", json_string("C:\\dir\\file")));
    roundtrip("download_queued", data, 240u);
    json_decref(data);
    char reason[1025];
    memset(reason, 'x', sizeof(reason));
    for (size_t i = 35u; i <= 36u; ++i) {
        data = json_loads(samples[i].data, JSON_REJECT_DUPLICATES, NULL);
        assert(data && !json_object_set_new(data, "reason", json_stringn(reason, 1024u)));
        roundtrip(samples[i].type, data, samples[i].kind);
        assert(!json_object_set_new(data, "reason", json_stringn(reason, sizeof(reason))));
        reject_json(samples[i].type, data);
        json_decref(data);
    }
}

static void
context_variants(void)
{
    json_t *data = json_loads(samples[37].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_set_new(data, "reason", json_string("turn_recovery")));
    roundtrip("context_rebased", data, 227u);
    assert(!json_object_set_new(data, "reason", json_string("recovery")));
    reject_json("context_rebased", data);
    json_decref(data);
    data = json_loads(samples[38].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    static const char *const names[] = {
        "context_limit_tokens", "requested_input_tokens", "observed_hard_input_tokens"
    };
    for (unsigned int mask = 0u; mask < 8u; ++mask) {
        for (size_t i = 0u; i < 3u; ++i) {
            json_t *value = mask & (1u << i) ? json_integer(4000000000LL) : json_null();
            assert(!json_object_set_new(data, names[i], value));
        }
        roundtrip("response_capacity_rejected", data, 166u);
    }
    const json_int_t invalid[] = {-1, 0, 4000000001LL};
    for (size_t i = 0u; i < 3u; ++i) {
        for (size_t j = 0u; j < sizeof(invalid) / sizeof(invalid[0]); ++j) {
            assert(!json_object_set_new(data, names[i], json_integer(invalid[j])));
            reject_json("response_capacity_rejected", data);
        }
        assert(!json_object_set_new(data, names[i], json_integer(1)));
    }
    assert(!json_object_set_new(data, "cycle", json_integer(UINT32_MAX)));
    roundtrip("response_capacity_rejected", data, 166u);
    assert(!json_object_set_new(data, "cycle", json_integer(4294967297LL)));
    reject_json("response_capacity_rejected", data);
    assert(!json_object_set_new(data, "cycle", json_integer(0)));
    reject_json("response_capacity_rejected", data);
    assert(!json_object_set_new(data, "cycle", json_integer(1)));
    assert(!json_object_set_new(data, "code", json_string("capacity")));
    reject_json("response_capacity_rejected", data);
    assert(!json_object_set_new(data, "code", json_string("context_length_exceeded")));
    char message[256];
    memset(message, 'x', sizeof(message));
    assert(!json_object_set_new(data, "message", json_stringn(message, 255u)));
    roundtrip("response_capacity_rejected", data, 166u);
    assert(!json_object_set_new(data, "message", json_stringn(message, sizeof(message))));
    reject_json("response_capacity_rejected", data);
    json_decref(data);
}

static void
compact_variants(void)
{
    static const char *const methods[] = {
        "exact", "media_upper_bound", "unknown", "anchored_upper_bound",
        "statistical_upper_estimate", "qualified_upper_bound"
    };
    static const char *const reasons[] = {
        "manual", "proactive", "hard_budget", "provider_rejection", "model_switch",
        "image_boundary", "reduce"
    };
    static const char *const stops[] = {
        "steering", "user", "endpoint_unavailable", "context_rejected", "error"
    };
    json_t *data = json_loads(samples[39].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    for (size_t i = 0u; i < 7u; ++i) {
        assert(!json_object_set_new(data, "reason", json_string(reasons[i])));
        for (size_t j = 0u; j < 6u; ++j) {
            assert(!json_object_set_new(data, "count_method", json_string(methods[j])));
            assert(!json_object_set_new(data, "input_tokens_bound",
                json_integer(j == 2u ? 0 : INT64_MAX)));
            roundtrip("compaction_started", data, 224u);
            assert(!json_object_set_new(data, "input_tokens_bound", json_integer(j == 2u ? 1 : 0)));
            reject_json("compaction_started", data);
        }
    }
    assert(!json_object_set_new(data, "count_method", json_string("unknown")));
    assert(!json_object_set_new(data, "input_tokens_bound", json_integer(0)));
    for (unsigned int mask = 0u; mask < 8u; ++mask) {
        assert(!json_object_set_new(data, "predecessor_compact_id",
            mask & 1u ? json_string(NEW_ID) : json_null()));
        if (mask & 2u) {
            assert(!json_object_set_new(data, "continuation_scope", json_string(HASHF)));
        } else {
            (void)json_object_del(data, "continuation_scope");
        }
        if (mask & 4u) {
            assert(!json_object_set_new(data, "compaction_model", json_string(" ")));
        } else {
            (void)json_object_del(data, "compaction_model");
        }
        roundtrip("compaction_started", data, 224u);
    }
    assert(!json_object_set_new(data, "source_seq", json_integer(INT64_MAX)));
    roundtrip("compaction_started", data, 224u);
    assert(!json_object_set_new(data, "source_seq", json_integer(0)));
    reject_json("compaction_started", data);
    assert(!json_object_set_new(data, "source_seq", json_integer(1)));
    assert(!json_object_set_new(data, "continuation_scope", json_null()));
    reject_json("compaction_started", data);
    assert(!json_object_set_new(data, "continuation_scope", json_string(HASH0)));
    assert(!json_object_set_new(data, "compaction_model", json_string("")));
    reject_json("compaction_started", data);
    assert(!json_object_set_new(data, "compaction_model", json_null()));
    reject_json("compaction_started", data);
    json_decref(data);
    data = json_loads(samples[40].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    for (size_t i = 0u; i < 5u; ++i) {
        assert(!json_object_set_new(data, "reason", json_string(stops[i])));
        roundtrip("compaction_interrupted", data, 225u);
    }
    assert(!json_object_set_new(data, "reason", json_string("unknown")));
    reject_json("compaction_interrupted", data);
    json_decref(data);
    data = json_loads(samples[41].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_set_new(data, "continuation_scope", json_string(HASHF)));
    for (size_t i = 0u; i < 6u; ++i) {
        for (size_t j = 0u; j < 6u; ++j) {
            assert(!json_object_set_new(data, "count_method", json_string(methods[i])));
            assert(!json_object_set_new(data, "output_count_method", json_string(methods[j])));
            assert(!json_object_set_new(data, "input_tokens_bound",
                json_integer(i == 2u ? 0 : INT64_MAX)));
            assert(!json_object_set_new(data, "output_tokens_bound",
                json_integer(j == 2u ? 0 : INT64_MAX)));
            if (i == 3u || j == 3u) {
                reject_json("compaction_completed", data);
            } else {
                roundtrip("compaction_completed", data, 226u);
                assert(!json_object_set_new(data, "input_tokens_bound",
                    json_integer(i == 2u ? 1 : 0)));
                reject_json("compaction_completed", data);
                assert(!json_object_set_new(data, "input_tokens_bound",
                    json_integer(i == 2u ? 0 : INT64_MAX)));
                assert(!json_object_set_new(data, "output_tokens_bound",
                    json_integer(j == 2u ? 1 : 0)));
                reject_json("compaction_completed", data);
                assert(!json_object_set_new(data, "output_tokens_bound",
                    json_integer(j == 2u ? 0 : INT64_MAX)));
            }
        }
    }
    assert(!json_object_set_new(data, "continuation_scope", json_null()));
    reject_json("compaction_completed", data);
    assert(!json_object_del(data, "continuation_scope"));
    assert(!json_object_set_new(data, "output_sha256", json_string(HASH0)));
    reject_json("compaction_completed", data);
    json_decref(data);
}

static void
compact_digest(json_t *data)
{
    char digest[65];
    assert(!snag_json_digest(json_object_get(data, "output"), digest));
    assert(!json_object_set_new(data, "output_sha256", json_string(digest)));
}

static void
compact_output_variants(void)
{
    json_t *data = json_loads(samples[41].data, JSON_REJECT_DUPLICATES, NULL);
    json_t *output = json_array();
    json_t *item = json_loads("{\"type\":\"\",\"vendor\":{\"\":[null,true,-9223372036854775808]}}",
        JSON_REJECT_DUPLICATES, NULL);
    assert(data && output && item && !json_object_set(data, "output", output));
    compact_digest(data);
    reject_json("compaction_completed", data);
    assert(!json_array_append(output, item));
    compact_digest(data);
    roundtrip("compaction_completed", data, 226u);
    for (size_t i = 1u; i < 128u; ++i) assert(!json_array_append(output, item));
    compact_digest(data);
    roundtrip("compaction_completed", data, 226u);
    assert(!json_array_append(output, item));
    compact_digest(data);
    roundtrip("compaction_completed", data, 226u);
    assert(!json_array_clear(output) && !json_array_append(output, item));
    assert(!json_object_set_new(item, "type", json_integer(1)));
    compact_digest(data);
    reject_json("compaction_completed", data);
    assert(!json_object_del(item, "type"));
    compact_digest(data);
    reject_json("compaction_completed", data);
    json_decref(item);
    json_decref(output);
    json_decref(data);

    data = json_loads(samples[41].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    item = json_array_get(json_object_get(data, "output"), 0u);
    const size_t maximum = 12u * 1024u * 1024u;
    char *text = malloc(maximum);
    assert(text);
    memset(text, 'x', maximum);
    /* The independent 47-byte provider fixture has one byte of encrypted content. */
    assert(!json_object_set_new(item, "encrypted_content", json_stringn(text, maximum - 46u)));
    compact_digest(data);
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    enum snag_binary_kind kind = 0;
    assert(!snag_binary_legacy_encode(&bytes, "compaction_completed", data, &kind));
    assert(kind == 226u && bytes.len == 135u + maximum);
    struct snag_binary_record record = {
        .kind = (uint16_t)kind, .version = 1u, .payload = bytes.data, .size = bytes.len
    };
    const char *type = NULL;
    json_t *decoded = NULL;
    assert(!snag_binary_legacy_decode(&record, &type, &decoded));
    assert(!strcmp(type, "compaction_completed") && json_equal(data, decoded));
    json_decref(decoded);
    snag_buf_free(&bytes);
    assert(!json_object_set_new(item, "encrypted_content", json_stringn(text, maximum - 45u)));
    compact_digest(data);
    reject_json("compaction_completed", data);
    json_decref(data);
    free(text);
}

static void
turn_outcome_variants(void)
{
    json_t *data = json_loads(samples[46].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_set_new(data, "reason", json_string("reply_reminder_exhausted")));
    roundtrip("turn_completed_silent", data, 133u);
    assert(!json_object_set_new(data, "reason", json_string("quiet")));
    reject_json("turn_completed_silent", data);
    json_decref(data);
    data = json_loads(samples[47].data, JSON_REJECT_DUPLICATES, NULL);
    static const char *const origins[] = {"user", "recovery", "output", "steering"};
    static const char *const reasons[] = {
        "cancelled", "process_lost", "output_lost", "session_recovered", "steered", "control"
    };
    assert(data);
    for (size_t i = 0u; i < 4u; ++i) {
        for (size_t j = 0u; j < 6u; ++j) {
            assert(!json_object_set_new(data, "origin", json_string(origins[i])));
            assert(!json_object_set_new(data, "reason", json_string(reasons[j])));
            if (i == 3u || j >= 4u) reject_json("turn_interrupted", data);
            else roundtrip("turn_interrupted", data, 134u);
        }
    }
    json_decref(data);
    data = json_loads(samples[48].data, JSON_REJECT_DUPLICATES, NULL);
    static const char *const classes[] = {
        "context", "provider", "protocol", "tool", "persistence", "resource", "output", "internal"
    };
    assert(data);
    for (size_t i = 0u; i < 8u; ++i) {
        assert(!json_object_set_new(data, "class", json_string(classes[i])));
        roundtrip("turn_failed", data, 135u);
    }
    assert(!json_object_set_new(data, "class", json_string("")));
    reject_json("turn_failed", data);
    assert(!json_object_set_new(data, "class", json_string("future class")));
    reject_json("turn_failed", data);
    roundtrip("turn_recovery", data, 131u);
    json_decref(data);
    data = json_loads(samples[44].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    const json_int_t attempts[] = {-1, 0, UINT32_MAX, 4294967296LL, 4294967297LL};
    for (size_t i = 0u; i < sizeof(attempts) / sizeof(attempts[0]); ++i) {
        assert(!json_object_set_new(data, "retry_attempts", json_integer(attempts[i])));
        if (i == 0u || i == 4u) reject_json("turn_recovery", data);
        else roundtrip("turn_recovery", data, 131u);
    }
    assert(!json_object_del(data, "retry_attempts"));
    char message[8193];
    memset(message, 'r', sizeof(message));
    assert(!json_object_set_new(data, "class", json_stringn(message, sizeof(message))));
    roundtrip("turn_recovery", data, 131u);
    assert(!json_object_set_new(data, "class", json_string("provider")));
    for (size_t size = 8192u; size <= 8193u; ++size) {
        assert(!json_object_set_new(data, "message", json_stringn(message, size)));
        if (size == 8192u) {
            roundtrip("turn_recovery", data, 131u);
            roundtrip("turn_failed", data, 135u);
        } else {
            reject_json("turn_recovery", data);
            reject_json("turn_failed", data);
        }
    }
    json_decref(data);
}

static void
input_control_variants(void)
{
    json_t *data = json_loads(samples[53].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_set_new(data, "armed", json_false()));
    roundtrip("future_queue_state", data, 102u);
    assert(!json_object_set_new(data, "armed", json_integer(0)));
    reject_json("future_queue_state", data);
    json_decref(data);
    data = json_loads(samples[52].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    const json_int_t times[] = {-1, 0, 1, INT64_MAX};
    for (size_t i = 0u; i < sizeof(times) / sizeof(times[0]); ++i) {
        assert(!json_object_set_new(data, "time_ms", json_integer(times[i])));
        if (i < 2u) reject_json("input_admitted", data);
        else roundtrip("input_admitted", data, 101u);
    }
    json_decref(data);
    for (size_t i = 0u; i < 2u; ++i) {
        const struct legacy_sample *sample = &samples[i ? 54u : 52u];
        const char *key = i ? "queue_ids" : "steering_ids";
        data = json_loads(sample->data, JSON_REJECT_DUPLICATES, NULL);
        json_t *ids = json_object_get(data, key);
        assert(data && ids && !json_array_clear(ids));
        if (i) reject_json(sample->type, data);
        else roundtrip(sample->type, data, sample->kind);
        /* Membership/uniqueness is a reducer check; keep literal order and repeats. */
        for (size_t n = 0u; n < 513u; ++n) {
            assert(!json_array_append_new(ids, json_string(n & 1u ? ID : NEW_ID)));
        }
        roundtrip(sample->type, data, sample->kind);
        assert(!json_array_set_new(ids, 512u, json_null()));
        reject_json(sample->type, data);
        assert(!json_array_set_new(ids, 512u, json_integer(1)));
        reject_json(sample->type, data);
        assert(!json_array_set_new(ids, 512u, json_string("FEDCBA98765432107766554433221100")));
        reject_json(sample->type, data);
        assert(!json_array_set_new(ids, 512u, json_string("short")));
        reject_json(sample->type, data);
        assert(!json_object_set_new(data, key, json_object()));
        reject_json(sample->type, data);
        json_decref(data);
    }
    data = json_loads(samples[54].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_set_new(data, "reason", json_string("model")));
    reject_json("future_turn_cancelled", data);
    json_decref(data);
    struct snag_binary_event event = {.kind = SNAG_BINARY_INPUT_ADMITTED};
    event.data.admission.time_ms = UINT64_MAX;
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&bytes, &event));
    struct snag_binary_record record = {
        .kind = 101u, .version = 2u, .payload = bytes.data, .size = bytes.len
    };
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    reject_record(&record);
    snag_buf_free(&bytes);
}

static void
input_receipt_variants(void)
{
    json_t *content = json_loads("[{\"type\":\"input_text\",\"text\":\"\"},"
        "{\"type\":\"file\",\"asset\":" ASSET "},"
        "{\"type\":\"input_image\",\"asset\":" ASSET ",\"source\":" ASSET ",\"note\":\"\"}]",
        JSON_REJECT_DUPLICATES, NULL);
    assert(content);
    for (size_t i = 55u; i < 58u; ++i) {
        const struct legacy_sample *sample = &samples[i];
        json_t *data = json_loads(sample->data, JSON_REJECT_DUPLICATES, NULL);
        assert(data);
        for (unsigned int mask = 0u; mask < (i == 55u ? 8u : 4u); ++mask) {
            if (mask & 1u) assert(!json_object_set(data, "content", content));
            else (void)json_object_del(data, "content");
            if (i == 55u) {
                assert(!json_object_set_new(data, "read_only", json_boolean(mask & 2u)));
                if (mask & 4u) {
                    assert(!json_object_set_new(data, "origin", json_string("timer")));
                } else {
                    (void)json_object_del(data, "origin");
                }
            } else if (mask & 2u) {
                assert(!json_object_set_new(data, "received_at_ms", json_integer(0)));
            } else {
                (void)json_object_del(data, "received_at_ms");
            }
            roundtrip(sample->type, data, sample->kind);
        }
        assert(!json_object_set_new(data, "received_at_ms", json_integer(INT64_MAX)));
        roundtrip(sample->type, data, sample->kind);
        assert(!json_object_set_new(data, "received_at_ms", json_integer(-1)));
        reject_json(sample->type, data);
        assert(!json_object_set_new(data, "received_at_ms", json_null()));
        reject_json(sample->type, data);
        json_decref(data);
    }
    json_t *data = json_loads(samples[55].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    static const char *const bad_content[] = {
        "null", "[]", "{}", "[null]", "[{}]", "[{\"type\":\"other\"}]",
        "[{\"type\":\"input_text\",\"text\":null}]",
        "[{\"type\":\"input_text\",\"text\":\"x\",\"extra\":1}]",
        "[{\"type\":\"input_image\",\"asset\":" ASSET ",\"note\":\"x\"}]",
        "[{\"type\":\"input_image\",\"asset\":" ASSET ",\"source\":null,\"note\":\"x\"}]"
    };
    for (size_t i = 0u; i < sizeof(bad_content) / sizeof(bad_content[0]); ++i) {
        assert(!json_object_set_new(data, "content",
            json_loads(bad_content[i], JSON_DECODE_ANY, NULL)));
        reject_json("input_received", data);
    }
    assert(!json_object_set(data, "content", content));
    json_t *file = json_object_get(json_array_get(content, 1u), "asset");
    json_t *image = json_object_get(json_array_get(content, 2u), "asset");
    json_t *source = json_object_get(json_array_get(content, 2u), "source");
    assert(!json_object_set_new(source, "mime_type", json_string("application/pdf")));
    assert(!json_object_set_new(source, "id", json_string(NEW_ID)));
    assert(!json_object_set_new(source, "sha256", json_string(HASH0)));
    assert(!json_object_set_new(source, "bytes", json_integer(SNAG_MEDIA_FILE_MAX)));
    assert(!json_object_set_new(file, "bytes", json_integer(SNAG_MEDIA_FILE_MAX)));
    assert(!json_object_set_new(image, "bytes", json_integer(SNAG_MEDIA_REQUEST_MAX)));
    roundtrip("input_received", data, 96u);
    static const char *const asset_keys[] = {"id", "sha256", "bytes", "mime_type"};
    for (size_t i = 0u; i < 4u; ++i) {
        json_t *copy = json_deep_copy(data);
        json_t *parts = json_object_get(copy, "content");
        json_t *asset = json_object_get(json_array_get(parts, 1u), "asset");
        assert(copy && asset && !json_object_del(asset, asset_keys[i]));
        reject_json("input_received", copy);
        assert(!json_object_set_new(asset, asset_keys[i], json_null()));
        reject_json("input_received", copy);
        json_decref(copy);
    }
    char note[1025];
    memset(note, 'n', sizeof(note));
    json_t *image_part = json_array_get(content, 2u);
    assert(!json_object_set_new(image_part, "note", json_stringn(note, 1024u)));
    roundtrip("input_received", data, 96u);
    assert(!json_object_set_new(image_part, "note", json_stringn(note, 1025u)));
    reject_json("input_received", data);
    assert(!json_object_set_new(image_part, "note", json_string("")));
    assert(!json_object_set_new(image, "bytes", json_integer(SNAG_MEDIA_REQUEST_MAX / 2u)));
    assert(!json_object_set_new(image, "mime_type", json_string("image/jpeg")));
    assert(!json_array_append_new(content, json_deep_copy(image_part)));
    roundtrip("input_received", data, 96u);
    json_t *other = json_object_get(json_array_get(content, 3u), "asset");
    assert(!json_object_set_new(other, "bytes", json_integer(SNAG_MEDIA_REQUEST_MAX / 2u + 1u)));
    reject_json("input_received", data);
    assert(!json_array_remove(content, 3u));
    assert(!json_object_set_new(image, "bytes", json_integer(SNAG_MEDIA_REQUEST_MAX + 1u)));
    reject_json("input_received", data);
    assert(!json_object_set_new(image, "bytes", json_integer(1)));
    assert(!json_object_set_new(file, "bytes", json_integer(SNAG_MEDIA_FILE_MAX + 1u)));
    reject_json("input_received", data);
    assert(!json_object_set_new(file, "bytes", json_integer(0)));
    reject_json("input_received", data);
    assert(!json_object_set_new(file, "bytes", json_integer(1)));
    assert(!json_object_set_new(file, "mime_type", json_string("BAD")));
    reject_json("input_received", data);
    assert(!json_object_set_new(file, "mime_type", json_string("image/png")));
    assert(!json_object_set_new(image, "mime_type", json_string("image/webp")));
    reject_json("input_received", data);
    assert(!json_object_del(data, "content"));
    static const char *const bad_paths[] = {
        "null", "{}", "[null]", "[1]", "[{}]", "[\"\"]", "[\"/a\",\"/a\"]",
        "[{\"path\":\"/a\",\"bytes\":0,\"sha256\":\"" HASH0 "\"}]"
    };
    for (size_t i = 0u; i < sizeof(bad_paths) / sizeof(bad_paths[0]); ++i) {
        assert(!json_object_set_new(data, "instructions",
            json_loads(bad_paths[i], JSON_DECODE_ANY, NULL)));
        reject_json("input_received", data);
    }
    assert(!json_object_set_new(data, "instructions", json_pack("[s,s]", "C:\\foreign\\a", "/b")));
    roundtrip("input_received", data, 96u);
    assert(!json_object_set_new(data, "origin", json_null()));
    reject_json("input_received", data);
    assert(!json_object_set_new(data, "origin", json_string("user")));
    reject_json("input_received", data);
    assert(!json_object_del(data, "origin"));
    assert(!json_object_set_new(data, "read_only", json_integer(0)));
    reject_json("input_received", data);
    json_decref(data);
    data = json_loads(samples[57].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_set_new(data, "text", json_string("Use")));
    reject_json("irc_reply_reminder", data);
    json_decref(data);
    json_decref(content);
}

static void
input_receipt_storage(void)
{
    json_t *data = json_loads(samples[55].data, JSON_REJECT_DUPLICATES, NULL);
    json_t *paths = json_array();
    size_t path_size = 16379u;
    char *path = malloc(SNAG_PATH_MAX_BYTES + 1u);
    assert(data && paths && path);
    memset(path, 'x', SNAG_PATH_MAX_BYTES + 1u);
    path[0] = '/';
    for (unsigned int i = 0u; i < 1024u; ++i) {
        assert(snprintf(path + path_size - 8u, 9u, "%08u", i) == 8);
        assert(!json_array_append_new(paths, json_stringn(path, path_size)));
    }
    assert(!json_object_set(data, "instructions", paths));
    /* All-prefix scans of large path lists would be quadratic. Small fixtures
     * exercise each truncation; this checks the entire near-limit conversion. */
    roundtrip_checked("input_received", data, 96u, false);
    assert(!json_array_clear(paths));
    memset(path, 'x', SNAG_PATH_MAX_BYTES + 1u);
    path[0] = '/';
    assert(!json_array_append_new(paths, json_stringn(path, SNAG_PATH_MAX_BYTES)));
    json_t *content = json_pack("[{s:s,s:s}]", "type", "input_text", "text", "");
    char *text = malloc(SNAG_MAX_DIRECT_PROMPT + 1u);
    assert(content && text);
    memset(text, 'x', SNAG_MAX_DIRECT_PROMPT + 1u);
    assert(!json_object_set_new(json_array_get(content, 0u), "text", json_stringn(text, 32769u)));
    assert(!json_object_set(data, "content", content));
    roundtrip_checked("input_received", data, 96u, false);
    assert(!json_array_set_new(paths, 0u, json_stringn(path, SNAG_PATH_MAX_BYTES + 1u)));
    reject_json("input_received", data);
    assert(!json_array_clear(paths));
    assert(!json_object_set_new(json_array_get(content, 0u), "text",
        json_stringn(text, SNAG_MAX_DIRECT_PROMPT + 1u)));
    roundtrip_checked("input_received", data, 96u, false);
    assert(!json_object_del(data, "content"));
    assert(!json_object_set_new(data, "text", json_stringn(text, SNAG_MAX_DIRECT_PROMPT)));
    roundtrip_checked("input_received", data, 96u, false);
    assert(!json_object_set_new(data, "text", json_stringn(text, SNAG_MAX_DIRECT_PROMPT + 1u)));
    reject_json("input_received", data);
    json_decref(data);
    data = json_loads(samples[56].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_set_new(data, "text", json_stringn(text, SNAG_MAX_STEERING_TEXT)));
    roundtrip_checked("steering_added", data, 98u, false);
    assert(!json_object_set_new(data, "text", json_stringn(text, SNAG_MAX_STEERING_TEXT + 1u)));
    reject_json("steering_added", data);
    json_decref(data);
    json_decref(paths);
    json_decref(content);
    free(path);
    free(text);
}

static void
reject_native_projection(const struct snag_binary_event *event, int error)
{
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&bytes, event));
    struct snag_binary_record record = {
        .kind = (uint16_t)event->kind, .version = snag_binary_event_version(event->kind),
        .payload = bytes.data, .size = bytes.len
    };
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    errno = 0;
    reject_record(&record);
    if (error) assert(errno == error);
    snag_buf_free(&bytes);
}

static void
input_receipt_projection(void)
{
    for (size_t i = 55u; i < 58u; ++i) {
        json_t *data = json_loads(samples[i].data, JSON_REJECT_DUPLICATES, NULL);
        struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
        enum snag_binary_kind kind;
        assert(data && !snag_binary_legacy_encode(&bytes, samples[i].type, data, &kind));
        struct snag_binary_record record = {
            .kind = (uint16_t)kind, .version = 2u, .payload = bytes.data, .size = bytes.len
        };
        struct snag_binary_event base;
        assert(!snag_binary_event_decode(&record, &base));
        for (unsigned int field = 1u; field <= (i == 55u ? 3u : 2u); ++field) {
            struct snag_binary_event event = base;
            struct snag_binary_input_reference ref = {
                .field = (enum snag_binary_input_leaf)field, .target = {1u, 0u, 32u}
            };
            if (i == 55u && field == 1u) {
                event.data.input.text = (struct snag_binary_text){0};
                event.data.input.text_ref = ref;
            } else if (i == 55u && field == 2u) {
                event.data.input.content_ref = ref;
            } else if (i == 55u) {
                event.data.input.instructions = (struct snag_binary_instructions){0};
                event.data.input.instructions_ref = ref;
            } else if (field == 1u) {
                event.data.steering_input.text = (struct snag_binary_text){0};
                event.data.steering_input.text_ref = ref;
            } else {
                event.data.steering_input.content_ref = ref;
            }
            reject_native_projection(&event, ENOTSUP);
        }
        struct snag_binary_event event = base;
        if (i == 55u) event.data.input.received_ms = UINT64_MAX;
        else {
            event.data.steering_input.has_received_ms = true;
            event.data.steering_input.received_ms = UINT64_MAX;
        }
        reject_native_projection(&event, 0);
        if (i == 55u) {
            struct snag_binary_instruction paths[2] = {
                {.path = {(const unsigned char *)"/a", 2u}},
                {.path = {(const unsigned char *)"/a", 2u}}
            };
            struct snag_buf list = {.max = SNAG_MAX_EVENT_LINE};
            assert(!snag_binary_instructions_encode(&list, paths, 2u));
            event = base;
            event.data.input.instructions = (struct snag_binary_instructions){list.data, list.len};
            reject_native_projection(&event, 0);
            snag_buf_reset(&list);
            struct snag_binary_part part = {
                .kind = SNAG_BINARY_PART_FILE,
                .asset = {.bytes = 1u, .mime = {(const unsigned char *)"BAD", 3u}}
            };
            assert(!snag_binary_content_encode(&list, &part, 1u));
            event = base;
            event.data.input.content = (struct snag_binary_content){list.data, list.len};
            reject_native_projection(&event, 0);
            snag_buf_reset(&list);
            part.kind = SNAG_BINARY_PART_IMAGE;
            part.asset.mime = (struct snag_binary_text){(const unsigned char *)"image/png", 9u};
            part.asset.bytes = SNAG_MEDIA_REQUEST_MAX + 1u;
            assert(!snag_binary_content_encode(&list, &part, 1u));
            event.data.input.content = (struct snag_binary_content){list.data, list.len};
            reject_native_projection(&event, 0);
            snag_buf_free(&list);
        }
        if (i == 57u) {
            event = base;
            event.data.steering_input.text = (struct snag_binary_text){
                (const unsigned char *)"x", 1u
            };
            reject_native_projection(&event, 0);
        }
        snag_buf_free(&bytes);
        json_decref(data);
    }
}

static void
queued_input_variants(void)
{
    for (size_t i = 58u; i < 61u; ++i) {
        const struct legacy_sample *sample = &samples[i];
        json_t *data = json_loads(sample->data, JSON_REJECT_DUPLICATES, NULL);
        assert(data);
        for (unsigned int arm = 0u; arm < 3u; ++arm) {
            for (unsigned int mask = 0u; mask < 8u; ++mask) {
                assert(!json_object_set_new(data, "read_only", json_boolean(mask & 1u)));
                if (arm) {
                    assert(!json_object_set_new(data, "armed", json_boolean(arm == 2u)));
                } else {
                    (void)json_object_del(data, "armed");
                }
                if (mask & 2u) {
                    assert(!json_object_set_new(data, "received_at_ms", json_integer(0)));
                } else {
                    (void)json_object_del(data, "received_at_ms");
                }
                if (mask & 4u) {
                    assert(!json_object_set_new(data, "content",
                        json_pack("[{s:s,s:s}]", "type", "input_text", "text", "media")));
                } else {
                    (void)json_object_del(data, "content");
                }
                if (i == 60u && (mask & 5u)) reject_json(sample->type, data);
                else roundtrip(sample->type, data, sample->kind);
            }
        }
        assert(!json_object_set_new(data, "read_only", json_false()));
        assert(!json_object_del(data, "content"));
        assert(!json_object_set_new(data, "received_at_ms", json_integer(INT64_MAX)));
        roundtrip(sample->type, data, sample->kind);
        assert(!json_object_set_new(data, "received_at_ms", json_integer(-1)));
        reject_json(sample->type, data);
        assert(!json_object_set_new(data, "received_at_ms", json_null()));
        reject_json(sample->type, data);
        assert(!json_object_del(data, "received_at_ms"));
        assert(!json_object_set_new(data, "armed", json_integer(0)));
        reject_json(sample->type, data);
        assert(!json_object_set_new(data, "armed", json_null()));
        reject_json(sample->type, data);
        assert(!json_object_del(data, "armed"));
        if (i != 59u) {
            assert(!json_object_set_new(data, "while_turn_id", json_string(NEW_ID)));
            roundtrip(sample->type, data, sample->kind);
            assert(!json_object_set_new(data, "while_turn_id",
                i == 60u ? json_string("") : json_null()));
            reject_json(sample->type, data);
        } else {
            assert(!json_object_set_new(data, "while_turn_id", json_string(NEW_ID)));
            reject_json(sample->type, data);
            assert(!json_object_del(data, "while_turn_id"));
            assert(!json_object_set_new(data, "voice", json_object()));
            reject_json(sample->type, data);
        }
        json_decref(data);
    }
    char *text = malloc(SNAG_MAX_QUEUED_TEXT + 1u);
    assert(text);
    memset(text, 'q', SNAG_MAX_QUEUED_TEXT + 1u);
    for (size_t i = 58u; i < 61u; ++i) {
        json_t *data = json_loads(samples[i].data, JSON_REJECT_DUPLICATES, NULL);
        assert(data);
        assert(!json_object_set_new(data, "text", json_stringn(text, SNAG_MAX_QUEUED_TEXT)));
        roundtrip_checked(samples[i].type, data, samples[i].kind, false);
        assert(!json_object_set_new(data, "text", json_stringn(text, SNAG_MAX_QUEUED_TEXT + 1u)));
        reject_json(samples[i].type, data);
        assert(!json_object_set_new(data, "text", json_string("")));
        reject_json(samples[i].type, data);
        json_decref(data);
    }
    free(text);
}

static void
set_voice_queue_id(json_t *data)
{
    json_t *voice = json_object_get(data, "voice");
    json_t *key = json_pack("{s:O,s:O}", "connection_id", json_object_get(voice, "connection_id"),
        "input_id", json_object_get(voice, "input_id"));
    char digest[65];
    assert(key && !snag_json_digest(key, digest));
    json_decref(key);
    assert(!json_object_set_new(data, "queue_id", json_stringn(digest, 32u)));
}

static void
queued_voice_variants(void)
{
    static const struct { const char *key; size_t limit; } fields[] = {
        {"input_id", SNAG_MAX_PROVIDER_ID}, {"response_id", SNAG_MAX_PROVIDER_ID},
        {"call_id", SNAG_MAX_PROVIDER_ID}, {"provider", SNAG_CONFIG_PROVIDER_NAME_MAX},
        {"model", SNAG_MODEL_MAX_BYTES - 1u}, {"transcript", SNAG_MAX_QUEUED_TEXT - 1u},
        {"request", SNAG_MAX_QUEUED_TEXT - 1u}
    };
    char *text = malloc(SNAG_MAX_QUEUED_TEXT + 1u);
    assert(text);
    memset(text, 'x', SNAG_MAX_QUEUED_TEXT + 1u);
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        json_t *data = json_loads(samples[60].data, JSON_REJECT_DUPLICATES, NULL);
        json_t *voice = json_object_get(data, "voice");
        assert(data && voice);
        assert(!json_object_set_new(voice, fields[i].key, json_stringn(text, fields[i].limit)));
        set_voice_queue_id(data);
        roundtrip_checked("future_turn_queued", data, 104u, false);
        assert(!json_object_set_new(voice, fields[i].key,
            json_stringn(text, fields[i].limit + 1u)));
        set_voice_queue_id(data);
        reject_json("future_turn_queued", data);
        assert(!json_object_set_new(voice, fields[i].key, json_string("")));
        set_voice_queue_id(data);
        reject_json("future_turn_queued", data);
        assert(!json_object_set_new(voice, fields[i].key, json_null()));
        reject_json("future_turn_queued", data);
        assert(!json_object_del(voice, fields[i].key));
        reject_json("future_turn_queued", data);
        json_decref(data);
    }
    json_t *data = json_loads(samples[60].data, JSON_REJECT_DUPLICATES, NULL);
    json_t *voice = json_object_get(data, "voice");
    assert(data && voice);
    assert(!json_object_set_new(voice, "transcript",
        json_stringn(text, SNAG_MAX_QUEUED_TEXT - 1u)));
    assert(!json_object_set_new(voice, "request", json_stringn(text, SNAG_MAX_QUEUED_TEXT - 1u)));
    roundtrip_checked("future_turn_queued", data, 104u, false);
    assert(!json_object_set_new(voice, "input_id", json_string("i\n")));
    reject_json("future_turn_queued", data);
    set_voice_queue_id(data);
    roundtrip_checked("future_turn_queued", data, 104u, false);
    assert(!json_object_set_new(voice, "connection_id", json_string(NEW_ID)));
    reject_json("future_turn_queued", data);
    set_voice_queue_id(data);
    roundtrip_checked("future_turn_queued", data, 104u, false);
    assert(!json_object_set_new(voice, "extra", json_true()));
    reject_json("future_turn_queued", data);
    assert(!json_object_del(voice, "extra"));
    assert(!json_object_set_new(voice, "connection_id", json_null()));
    reject_json("future_turn_queued", data);
    assert(!json_object_del(voice, "connection_id"));
    reject_json("future_turn_queued", data);
    json_decref(data);
    free(text);
}

static void
queued_input_projection(void)
{
    for (size_t i = 58u; i < 61u; ++i) {
        json_t *data = json_loads(samples[i].data, JSON_REJECT_DUPLICATES, NULL);
        struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
        enum snag_binary_kind kind;
        assert(data && !snag_binary_legacy_encode(&bytes, samples[i].type, data, &kind));
        struct snag_binary_record record = {
            .kind = (uint16_t)kind, .version = 2u, .payload = bytes.data, .size = bytes.len
        };
        struct snag_binary_event base;
        assert(!snag_binary_event_decode(&record, &base));
        for (unsigned int n = 0u; n < (i == 60u ? 3u : 2u); ++n) {
            struct snag_binary_event event = base;
            unsigned int field = !n ? 1u : i == 60u ? n + 3u : 2u;
            struct snag_binary_input_reference ref = {
                .field = (enum snag_binary_input_leaf)field, .target = {1u, 0u, 32u}
            };
            if (field == 1u) {
                event.data.queued.text = (struct snag_binary_text){0};
                event.data.queued.text_ref = ref;
            } else if (field == 2u) {
                event.data.queued.content_ref = ref;
            } else if (field == 4u) {
                event.data.queued.voice.transcript = (struct snag_binary_text){0};
                event.data.queued.voice.transcript_ref = ref;
            } else {
                event.data.queued.voice.request = (struct snag_binary_text){0};
                event.data.queued.voice.request_ref = ref;
            }
            reject_native_projection(&event, ENOTSUP);
        }
        struct snag_binary_event event = base;
        event.data.queued.has_received_ms = true;
        event.data.queued.received_ms = UINT64_MAX;
        reject_native_projection(&event, 0);
        if (i == 60u) {
            event = base;
            event.data.queued.id[0] ^= 1u;
            reject_native_projection(&event, 0);
        }
        snag_buf_free(&bytes);
        json_decref(data);
    }
}

static void
turn_config_variants(void)
{
    const char *numbers[] = {"max_parallel_commands", "default_yield_ms", "max_wait_ms",
        "default_timeout_ms", "max_timeout_ms", "tool_output_bytes", "output_cache_bytes",
        "max_turn_retries"};
    const json_int_t literals[] = {INT64_MAX, 10000, 60000, 0, 86400000,
        SNAG_DEFAULT_TOOL_OUTPUT_TOKENS, SNAG_CONFIG_OUTPUT_CACHE_MAX, UINT32_MAX};
    for (unsigned int mask = 0u; mask < 256u; ++mask) {
        json_t *data = json_loads(samples[61].data, JSON_REJECT_DUPLICATES, NULL);
        assert(data);
        json_t *config = json_object_get(data, "config");
        for (size_t i = 0u; i < 8u; ++i) {
            if (mask & (1u << i)) {
                assert(!json_object_set_new(config, numbers[i], json_integer(literals[i])));
            }
        }
        roundtrip("turn_started", data, 128u);
        json_decref(data);
    }
    for (size_t i = 0u; i < 8u; ++i) {
        json_t *data = json_loads(samples[61].data, JSON_REJECT_DUPLICATES, NULL);
        assert(data);
        json_t *config = json_object_get(data, "config");
        json_int_t maximum = i == 0u ? INT64_MAX :
            i == 6u ? SNAG_CONFIG_OUTPUT_CACHE_MAX : UINT32_MAX;
        if (i == 1u || i == 3u) {
            assert(!json_object_set_new(config, numbers[i + 1u], json_integer(UINT32_MAX)));
        }
        assert(!json_object_set_new(config, numbers[i], json_integer(maximum)));
        roundtrip("turn_started", data, 128u);
        if (i) {
            assert(!json_object_set_new(config, numbers[i], json_integer(maximum + 1)));
            reject_json("turn_started", data);
        }
        assert(!json_object_set_new(config, numbers[i], json_integer(0)));
        if (i == 1u || i == 3u || i == 6u || i == 7u) {
            roundtrip("turn_started", data, 128u);
        } else {
            reject_json("turn_started", data);
        }
        assert(!json_object_set_new(config, numbers[i], json_integer(-1)));
        reject_json("turn_started", data);
        assert(!json_object_set_new(config, numbers[i], json_null()));
        reject_json("turn_started", data);
        assert(!json_object_set_new(config, numbers[i], json_true()));
        reject_json("turn_started", data);
        json_decref(data);
    }
    json_t *data = json_loads(samples[61].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    json_t *config = json_object_get(data, "config");
    assert(!json_object_set_new(config, "max_wait_ms", json_integer(1)));
    reject_json("turn_started", data);
    assert(!json_object_set_new(config, "default_yield_ms", json_integer(0)));
    roundtrip("turn_started", data, 128u);
    assert(!json_object_set_new(config, "default_timeout_ms", json_integer(86400001)));
    reject_json("turn_started", data);
    json_object_del(config, "default_timeout_ms");
    assert(!json_object_set_new(config, "parallel_tool_calls", json_null()));
    reject_json("turn_started", data);
    assert(!json_object_set_new(config, "parallel_tool_calls", json_integer(0)));
    reject_json("turn_started", data);
    json_object_del(config, "parallel_tool_calls");
    const char *fields[] = {"prompt_schema", "replay_schema", "tool_schema", "capability_version",
        "profile_id", "max_output_tokens", "", "extension"};
    json_t *values = json_loads(
        "[null,false,true,-9223372036854775808,\"text\",[1,null],{\"nested\":false}]", 0u, NULL);
    assert(values);
    for (size_t i = 0u; i < json_array_size(values); ++i) {
        for (size_t j = 0u; j < sizeof(fields) / sizeof(fields[0]); ++j) {
            assert(!json_object_set(config, fields[j], json_array_get(values, i)));
        }
        roundtrip("turn_started", data, 128u);
    }
    json_decref(values);
    const char *selection[] = {"provider", "model", "effort"};
    for (size_t i = 0u; i < 3u; ++i) {
        json_t *saved = json_incref(json_object_get(config, selection[i]));
        json_object_del(config, selection[i]);
        reject_json("turn_started", data);
        assert(!json_object_set_new(config, selection[i], json_null()));
        reject_json("turn_started", data);
        assert(!json_object_set_new(config, selection[i], json_string("")));
        reject_json("turn_started", data);
        assert(!json_object_set_new(config, selection[i], saved));
    }
    json_decref(data);
}

static void
turn_start_variants(void)
{
    const char *origins[] = {"direct", "queued", "goal", "timer"};
    for (size_t origin = 0u; origin < 4u; ++origin) {
        for (unsigned int ro = 0u; ro < 3u; ++ro) {
            for (unsigned int mask = 0u; mask < 8u; ++mask) {
                json_t *data = json_loads(samples[61].data, JSON_REJECT_DUPLICATES, NULL);
                assert(data && !json_object_set_new(data, "input_kind",
                    json_string(origins[origin])));
                if (!(mask & 1u)) {
                    assert(!json_object_set(data, "cwd", json_object_get(data, "workspace")));
                    json_object_del(data, "workspace");
                }
                if (ro) {
                    assert(!json_object_set_new(data, "read_only", json_boolean(ro == 2u)));
                }
                if (mask & 2u) {
                    assert(!json_object_set_new(data, "received_at_ms", json_integer(INT64_MAX)));
                }
                if (mask & 4u) {
                    assert(!json_object_set_new(data, "content",
                        json_loads("[{\"type\":\"input_image\",\"asset\":" ASSET "}]", 0u, NULL)));
                }
                if (origin == 1u) {
                    assert(!json_object_set_new(data, "queue_id", json_string(NEW_ID)));
                    assert(!json_object_set_new(data, "queue_seq", json_integer(INT64_MAX)));
                }
                if (origin == 2u) {
                    assert(!json_object_set_new(data, "text",
                        json_string(SNAG_GOAL_CONTINUATION_TEXT)));
                }
                if (origin == 2u && (ro == 2u || (mask & 4u))) {
                    reject_json("turn_started", data);
                } else {
                    roundtrip("turn_started", data, 128u);
                }
                json_decref(data);
            }
        }
    }
    json_t *data = json_loads(samples[61].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    assert(!json_object_set_new(data, "cwd", json_string("/w")));
    reject_json("turn_started", data);
    json_object_del(data, "cwd");
    assert(!json_object_set_new(data, "queue_seq", json_integer(1)));
    reject_json("turn_started", data);
    assert(!json_object_set_new(data, "queue_seq", json_null()));
    assert(!json_object_set_new(data, "queue_id", json_string(ID)));
    reject_json("turn_started", data);
    assert(!json_object_set_new(data, "queue_id", json_null()));
    assert(!json_object_set_new(data, "input_kind", json_string("goal")));
    reject_json("turn_started", data);
    assert(!json_object_set_new(data, "input_kind", json_string("direct")));
    assert(!json_object_set_new(data, "turn_number", json_integer(INT64_MAX)));
    roundtrip("turn_started", data, 128u);
    assert(!json_object_set_new(data, "turn_number", json_integer(0)));
    reject_json("turn_started", data);
    assert(!json_object_set_new(data, "turn_number", json_integer(-1)));
    reject_json("turn_started", data);
    json_decref(data);

    data = json_loads(samples[63].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    json_t *instructions = json_object_get(data, "instructions");
    json_t *snapshot = json_array_get(instructions, 0u);
    assert(!json_object_set_new(snapshot, "bytes", json_integer(INT64_MAX)));
    roundtrip("turn_started", data, 128u);
    assert(!json_array_append(instructions, snapshot));
    reject_json("turn_started", data);
    assert(!json_array_remove(instructions, 2u));
    assert(!json_array_set(instructions, 1u, json_object_get(snapshot, "path")));
    reject_json("turn_started", data);
    assert(!json_array_set_new(instructions, 1u, json_string("/other")));
    assert(!json_array_insert_new(instructions, 0u, json_string("/first")));
    reject_json("turn_started", data);
    assert(!json_array_remove(instructions, 0u));
    assert(!json_object_set_new(snapshot, "extra", json_null()));
    reject_json("turn_started", data);
    json_object_del(snapshot, "extra");
    assert(!json_object_set_new(snapshot, "bytes", json_integer(-1)));
    reject_json("turn_started", data);
    assert(!json_object_set_new(snapshot, "bytes", json_null()));
    reject_json("turn_started", data);
    assert(!json_object_set_new(snapshot, "bytes", json_integer(0)));
    assert(!json_object_set_new(snapshot, "sha256", json_string("bad")));
    reject_json("turn_started", data);
    assert(!json_object_set_new(snapshot, "sha256", json_string(HASHF)));
    assert(!json_object_set_new(data, "queue_seq", json_integer(0)));
    reject_json("turn_started", data);
    json_decref(data);
}

static void
turn_start_storage(void)
{
    json_t *data = json_loads(samples[63].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data);
    size_t size = SNAG_MAX_DIRECT_PROMPT + 1u;
    char *text = malloc(size);
    assert(text && size > 524289u);
    memset(text, 'x', size);
    text[0] = '/';
    json_t *snapshot = json_array_get(json_object_get(data, "instructions"), 0u);
    assert(!json_object_set_new(snapshot, "path", json_stringn(text, SNAG_PATH_MAX_BYTES)));
    json_t *part = json_pack("{s:s,s:s#}", "type", "input_text", "text", text, (int)131073);
    json_t *content = json_array();
    assert(part && content && !json_array_append_new(content, part));
    assert(!json_object_set_new(data, "content", content));
    json_t *config = json_object_get(data, "config");
    const char *fields[] = {"prompt_schema", "replay_schema", "tool_schema", "capability_version",
        "profile_id", "max_output_tokens"};
    for (size_t i = 0u; i < 6u; ++i) {
        assert(!json_object_set_new(config, fields[i], json_stringn(text, 61111u + i)));
    }
    /* Late extension growth must not invalidate instruction/content/named views. */
    assert(!json_object_set_new(config, "extension", json_stringn(text, 524289u)));
    assert(!json_object_set_new(data, "text", json_stringn(text, SNAG_MAX_QUEUED_TEXT)));
    roundtrip_checked("turn_started", data, 128u, false);
    assert(!json_object_set_new(data, "text", json_stringn(text, SNAG_MAX_QUEUED_TEXT + 1u)));
    reject_json("turn_started", data);
    assert(!json_object_set_new(data, "queue_id", json_null()));
    assert(!json_object_set_new(data, "queue_seq", json_null()));
    for (unsigned int timer = 0u; timer < 2u; ++timer) {
        assert(!json_object_set_new(data, "input_kind", json_string(timer ? "timer" : "direct")));
        assert(!json_object_set_new(data, "text", json_stringn(text, SNAG_MAX_DIRECT_PROMPT)));
        roundtrip_checked("turn_started", data, 128u, false);
        assert(!json_object_set_new(data, "text", json_stringn(text, SNAG_MAX_DIRECT_PROMPT + 1u)));
        reject_json("turn_started", data);
    }
    assert(!json_object_set_new(data, "text", json_string("t")));
    assert(!json_object_set_new(snapshot, "path", json_stringn(text, SNAG_PATH_MAX_BYTES + 1u)));
    reject_json("turn_started", data);
    assert(!json_object_set_new(snapshot, "path", json_string("/a")));
    assert(!json_object_set_new(data, "received_at_ms", json_integer(-1)));
    reject_json("turn_started", data);
    free(text);
    json_decref(data);
}

static void
turn_start_projection(void)
{
    json_t *data = json_loads(samples[62].data, JSON_REJECT_DUPLICATES, NULL);
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    enum snag_binary_kind kind;
    assert(data && !snag_binary_legacy_encode(&bytes, "turn_started", data, &kind));
    struct snag_binary_record record = {
        .kind = (uint16_t)kind, .version = 1u, .payload = bytes.data, .size = bytes.len
    };
    struct snag_binary_event base;
    assert(!snag_binary_event_decode(&record, &base));
    for (unsigned int i = 1u; i <= 3u; ++i) {
        struct snag_binary_event event = base;
        struct snag_binary_input_reference ref = {
            .field = (enum snag_binary_input_leaf)i, .target = {1u, 0u, 32u}
        };
        if (i == 1u) {
            event.data.started.text = (struct snag_binary_text){0};
            event.data.started.text_ref = ref;
        } else if (i == 2u) {
            event.data.started.content_ref = ref;
        } else {
            event.data.started.instructions = (struct snag_binary_instructions){0};
            event.data.started.instructions_ref = ref;
        }
        reject_native_projection(&event, ENOTSUP);
    }
    for (size_t i = 0u; i < 11u; ++i) {
        struct snag_binary_event event = base;
        if (i == 0u) event.data.started.number = UINT64_MAX;
        else if (i == 1u) event.data.started.received_ms = UINT64_MAX;
        else if (i == 2u) {
            event.data.started.origin = SNAG_BINARY_TURN_QUEUED;
            event.data.started.queue_seq = (uint64_t)INT64_MAX + 1u;
        } else {
            event.data.started.config.present |= (uint16_t)(1u << (i - 3u));
            event.data.started.config.numbers[i - 3u] = UINT64_MAX;
        }
        reject_native_projection(&event, 0);
    }
    struct snag_binary_event event = base;
    event.data.started.origin = SNAG_BINARY_TURN_GOAL;
    reject_native_projection(&event, 0);
    struct snag_binary_instruction paths[2] = {
        {.path = {(const unsigned char *)"/a", 2u}},
        {.path = {(const unsigned char *)"/b", 2u}, .has_snapshot = true}
    };
    struct snag_buf list = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_instructions_encode(&list, paths, 2u));
    event = base;
    event.data.started.instructions = (struct snag_binary_instructions){list.data, list.len};
    reject_native_projection(&event, 0);
    paths[0].has_snapshot = true;
    paths[0].bytes = UINT64_MAX;
    snag_buf_reset(&list);
    assert(!snag_binary_instructions_encode(&list, paths, 2u));
    event.data.started.instructions = (struct snag_binary_instructions){list.data, list.len};
    reject_native_projection(&event, 0);
    snag_buf_free(&list);
    snag_buf_free(&bytes);
    json_decref(data);
}

static void
response_start_variants(void)
{
    const char *methods[] = {"exact", "media_upper_bound", "unknown", "anchored_upper_bound",
        "statistical_upper_estimate", "qualified_upper_bound"};
    for (size_t sample = 64u; sample < 66u; ++sample) {
        for (size_t method = 0u; method < 6u; ++method) {
            for (unsigned int mask = 0u; mask < 4u; ++mask) {
                json_t *data = json_loads(samples[sample].data, JSON_REJECT_DUPLICATES, NULL);
                assert(data && !json_object_set_new(data, "count_method",
                    json_string(methods[method])));
                assert(!json_object_set_new(data, "input_tokens_bound",
                    json_integer(method == 2u ? 0 : INT64_MAX)));
                assert(!json_object_set_new(data, "baseline_sha256",
                    method == 3u ? json_string(HASHF) : json_null()));
                if (mask & 1u) {
                    assert(!json_object_set_new(data, "compact_id", json_string(ID)));
                }
                if (mask & 2u) {
                    assert(!json_object_set_new(data, "irc_seq", json_integer(INT64_MAX)));
                }
                roundtrip("response_started", data, 160u);
                assert(!json_object_set_new(data, "input_tokens_bound", json_integer(0)));
                roundtrip("response_started", data, 160u);
                if (method == 2u) {
                    assert(!json_object_set_new(data, "input_tokens_bound", json_integer(1)));
                    reject_json("response_started", data);
                }
                assert(!json_object_set_new(data, "baseline_sha256",
                    method == 3u ? json_null() : json_string(HASHF)));
                reject_json("response_started", data);
                json_decref(data);
            }
        }
    }
    const char *sources[] = {
        "unknown", "advertised", "configured", "observed", "stale-catalog-ignored"
    };
    for (size_t source = 0u; source < 5u; ++source) {
        for (unsigned int variant = 0u; variant < 12u; ++variant) {
            json_t *data = json_loads(samples[65].data, JSON_REJECT_DUPLICATES, NULL);
            assert(data && !json_object_set_new(data, "capacity_source",
                json_string(sources[source])));
            assert(!json_object_set_new(data, "source_bound", json_boolean(variant & 1u)));
            assert(!json_object_set_new(data, "requested_output_tokens", (variant & 2u) ?
                json_integer(SNAG_CONFIG_TOKEN_LIMIT_MAX) : json_null()));
            assert(!json_object_set_new(data, "hard_input_tokens",
                variant / 4u == 0u ? json_null() :
                json_integer(variant / 4u == 1u ? 0 : INT64_MAX)));
            roundtrip("response_started", data, 160u);
            json_decref(data);
        }
    }
    json_t *data = json_loads(samples[65].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_set_new(data, "cycle", json_integer(UINT32_MAX)));
    roundtrip("response_started", data, 160u);
    const json_int_t bad_cycles[] = {-1, 0, (json_int_t)UINT32_MAX + 1, (json_int_t)UINT32_MAX + 2};
    for (size_t i = 0u; i < 4u; ++i) {
        assert(!json_object_set_new(data, "cycle", json_integer(bad_cycles[i])));
        reject_json("response_started", data);
    }
    assert(!json_object_set_new(data, "cycle", json_integer(1)));
    const char *counts[] = {"model_input_bytes", "request_input_bytes", "request_input_count",
        "input_tokens_bound", "hard_input_tokens", "irc_seq"};
    for (size_t i = 0u; i < 6u; ++i) {
        assert(!json_object_set_new(data, counts[i], json_integer(INT64_MAX)));
        roundtrip("response_started", data, 160u);
        assert(!json_object_set_new(data, counts[i], json_integer(-1)));
        reject_json("response_started", data);
        assert(!json_object_set_new(data, counts[i], json_integer(0)));
        if (i < 2u) reject_json("response_started", data);
        else roundtrip("response_started", data, 160u);
        assert(!json_object_set_new(data, counts[i], json_integer(1)));
    }
    const json_int_t bad_limits[] = {-1, 0, (json_int_t)SNAG_CONFIG_TOKEN_LIMIT_MAX + 1};
    for (size_t i = 0u; i < 3u; ++i) {
        assert(!json_object_set_new(data, "requested_output_tokens", json_integer(bad_limits[i])));
        reject_json("response_started", data);
    }
    json_decref(data);
}

static void
response_start_host(void)
{
    json_t *data = json_loads(samples[65].data, JSON_REJECT_DUPLICATES, NULL);
    json_t *host = json_pack("[{s:s,s:s},{s:s,s:s},{s:s,s:s}]",
        "role", "user", "content", SNAG_HOST_CONTEXT_BEGIN,
        "role", "user", "content", "snapshot", "role", "user", "content", SNAG_HOST_CONTEXT_END);
    assert(data && host && !json_object_set(data, "host_context", host));
    roundtrip("response_started", data, 160u);
    json_t *middle = json_array_get(host, 1u);
    assert(!json_object_set_new(middle, "role", json_string("assistant")));
    reject_json("response_started", data);
    assert(!json_object_set_new(middle, "role", json_string("user")));
    assert(!json_object_set_new(middle, "extra", json_null()));
    reject_json("response_started", data);
    json_object_del(middle, "extra");
    assert(!json_object_set_new(middle, "content", json_string("")));
    reject_json("response_started", data);
    char *text = malloc(131073u);
    assert(text);
    memset(text, 'x', 131073u);
    assert(!json_object_set_new(middle, "content", json_stringn(text, 131073u)));
    free(text);
    json_t *ids = json_object_get(data, "steering_ids");
    for (size_t i = 0u; i < 513u; ++i) {
        assert(!json_array_append_new(ids, json_string(i & 1u ? ID : NEW_ID)));
    }
    roundtrip_checked("response_started", data, 160u, false);
    assert(!json_object_set_new(json_array_get(host, 0u), "content", json_string("wrong")));
    reject_json("response_started", data);
    assert(!json_object_set_new(json_array_get(host, 0u), "content",
        json_string(SNAG_HOST_CONTEXT_BEGIN)));
    assert(!json_object_set_new(json_array_get(host, 2u), "content", json_string("wrong")));
    reject_json("response_started", data);
    assert(!json_object_set_new(json_array_get(host, 2u), "content",
        json_string(SNAG_HOST_CONTEXT_END)));
    assert(!json_array_remove(host, 1u));
    roundtrip("response_started", data, 160u);
    json_t *legacy = json_loads(samples[64].data, JSON_REJECT_DUPLICATES, NULL);
    assert(legacy && !json_object_set(legacy, "host_context", host));
    reject_json("response_started", legacy);
    json_decref(legacy);
    assert(!json_array_remove(host, 0u));
    reject_json("response_started", data);
    assert(!json_array_remove(host, 0u));
    reject_json("response_started", data);
    assert(!json_object_set_new(data, "host_context", json_null()));
    reject_json("response_started", data);
    json_decref(host);
    json_decref(data);
}

static void
response_start_projection(void)
{
    json_t *data = json_loads(samples[65].data, JSON_REJECT_DUPLICATES, NULL);
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    enum snag_binary_kind kind;
    assert(data && !snag_binary_legacy_encode(&bytes, "response_started", data, &kind));
    struct snag_binary_record record = {
        .kind = (uint16_t)kind, .version = 1u, .payload = bytes.data, .size = bytes.len
    };
    struct snag_binary_event base;
    assert(!snag_binary_event_decode(&record, &base));
    for (size_t i = 0u; i < 6u; ++i) {
        struct snag_binary_event event = base;
        struct snag_binary_response_start *start = &event.data.response_started;
        uint64_t *counts[] = {&start->input_tokens_bound, &start->model_input_bytes,
            &start->request_input_bytes, &start->request_input_count, &start->hard_input_tokens,
            &start->irc_seq};
        start->has_hard_input = start->has_irc_seq = true;
        *counts[i] = UINT64_MAX;
        reject_native_projection(&event, 0);
    }
    const struct snag_binary_part parts[] = {
        {.kind = SNAG_BINARY_PART_TEXT, .text = {(const unsigned char *)"wrong", 5u}},
        {.kind = SNAG_BINARY_PART_TEXT, .text = {(const unsigned char *)"wrong", 5u}}
    };
    struct snag_buf content = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_content_encode(&content, parts, 2u));
    base.data.response_started.host_context =
        (struct snag_binary_content){content.data, content.len};
    reject_native_projection(&base, 0);
    snag_buf_free(&content);
    snag_buf_free(&bytes);
    json_decref(data);
}

static void
response_public_variants(void)
{
    for (size_t sample = 66u; sample < 70u; ++sample) {
        json_t *data = json_loads(samples[sample].data, JSON_REJECT_DUPLICATES, NULL);
        json_t *item = json_loads(PUBLIC_ITEM, JSON_REJECT_DUPLICATES, NULL);
        assert(data && item);
        if (sample == 66u) assert(!json_object_set(data, "item", item));
        else assert(!json_array_append(json_object_get(data, "partial_public"), item));
        for (unsigned int variant = 0u; variant < 4u; ++variant) {
            assert(!json_object_set_new(item, "kind",
                json_string(variant & 1u ? "refusal" : "assistant")));
            assert(!json_object_set_new(item, "phase",
                json_string(variant & 2u ? "final_answer" : "commentary")));
            if (variant == 1u) reject_json(samples[sample].type, data);
            else roundtrip(samples[sample].type, data, samples[sample].kind);
        }
        assert(!json_object_set_new(item, "kind", json_string("assistant")));
        assert(!json_object_set_new(data, "cycle", json_integer(UINT32_MAX)));
        roundtrip(samples[sample].type, data, samples[sample].kind);
        const json_int_t cycles[] = {-1, 0, (json_int_t)UINT32_MAX + 1};
        for (size_t i = 0u; i < 3u; ++i) {
            assert(!json_object_set_new(data, "cycle", json_integer(cycles[i])));
            reject_json(samples[sample].type, data);
        }
        assert(!json_object_set_new(data, "cycle", json_integer(1)));
        const char *keys[] = {"kind", "phase", "local_item_id", "provider_item_id", "text"};
        for (size_t i = 0u; i < 5u; ++i) {
            json_t *old = json_incref(json_object_get(item, keys[i]));
            assert(old);
            json_object_del(item, keys[i]);
            reject_json(samples[sample].type, data);
            assert(!json_object_set_new(item, keys[i], json_null()));
            reject_json(samples[sample].type, data);
            assert(!json_object_set_new(item, keys[i], json_string("")));
            reject_json(samples[sample].type, data);
            assert(!json_object_set_new(item, keys[i], old));
        }
        assert(!json_object_set_new(item, "extra", json_integer(1)));
        reject_json(samples[sample].type, data);
        json_object_del(item, "extra");
        assert(!json_object_set_new(item, "provider_item_id", json_string("a\n")));
        reject_json(samples[sample].type, data);
        assert(!json_object_set_new(item, "provider_item_id", json_string("a\xc2\x80")));
        reject_json(samples[sample].type, data);
        assert(!json_object_set_new(item, "provider_item_id", json_string("p")));
        if (sample != 66u) {
            json_t *partial = json_object_get(data, "partial_public");
            json_t *second = json_deep_copy(item);
            assert(second && !json_array_append(partial, second));
            reject_json(samples[sample].type, data);
            assert(!json_object_set_new(second, "local_item_id", json_string(NEW_ID)));
            roundtrip(samples[sample].type, data, samples[sample].kind);
            json_decref(second);
        }
        json_decref(item);
        json_decref(data);
    }
    const char *origins[] = {"user", "steering", "recovery", "output"};
    const char *reasons[] = {"cancelled", "steered", "control", "process_lost", "output_lost",
        "session_recovered"};
    json_t *data = json_loads(samples[67].data, JSON_REJECT_DUPLICATES, NULL);
    for (size_t origin = 0u; origin < 4u; ++origin) {
        for (size_t reason = 0u; reason < 6u; ++reason) {
            assert(!json_object_set_new(data, "origin", json_string(origins[origin])));
            assert(!json_object_set_new(data, "reason", json_string(reasons[reason])));
            if (reason == 5u || ((origin == 1u) != (reason == 1u))) {
                reject_json("response_interrupted", data);
            } else roundtrip("response_interrupted", data, 162u);
        }
    }
    json_decref(data);
    data = json_loads(samples[69].data, JSON_REJECT_DUPLICATES, NULL);
    const char *corrections[] = {SNAG_EMPTY_OUTPUT_CORRECTION, SNAG_OVERSIZED_OUTPUT_CORRECTION,
        SNAG_CYBER_CLARIFICATION};
    json_t *item = json_loads(PUBLIC_ITEM, JSON_REJECT_DUPLICATES, NULL);
    assert(data && item && !json_array_append(json_object_get(data, "partial_public"), item));
    for (size_t i = 0u; i < 3u; ++i) {
        assert(!json_object_set_new(data, "text", json_string(corrections[i])));
        assert(!json_object_set_new(item, "kind", json_string("assistant")));
        roundtrip("response_output_correction", data, 164u);
        assert(!json_object_set_new(item, "kind", json_string("refusal")));
        assert(!json_object_set_new(item, "phase", json_string("final_answer")));
        if (i == 2u) reject_json("response_output_correction", data);
        else roundtrip("response_output_correction", data, 164u);
    }
    assert(!json_object_set_new(data, "text", json_string("not a correction")));
    reject_json("response_output_correction", data);
    json_decref(item);
    json_decref(data);
}

static void
response_failure_variants(void)
{
    for (unsigned int mask = 0u; mask < 16u; ++mask) {
        for (unsigned int values = 0u; values < 4u; ++values) {
            json_t *data = json_loads(samples[68].data, JSON_REJECT_DUPLICATES, NULL);
            assert(data);
            if (mask & 1u) {
                assert(!json_object_set_new(data, "policy_stopped", json_boolean(values & 1u)));
            }
            if (mask & 2u) {
                assert(!json_object_set_new(data, "turn_retry_attempts",
                    json_integer((json_int_t)UINT32_MAX + 1)));
            }
            if (mask & 4u) {
                assert(!json_object_set_new(data, "new_input", json_boolean(values & 2u)));
            }
            if (mask & 8u) {
                assert(!json_object_set_new(data, "policy", json_pack("{s:s,s:s,s:s}",
                    "code", "", "type", "", "clarification_skipped", "s")));
            }
            if (mask & (mask + 1u)) reject_json("response_failed", data);
            else roundtrip("response_failed", data, 163u);
            json_decref(data);
        }
    }
    const char *classes[] = {"context", "provider", "protocol", "resource", "output", "internal",
        "tool", "persistence"};
    json_t *data = json_loads(samples[68].data, JSON_REJECT_DUPLICATES, NULL);
    for (size_t class_name = 0u; class_name < 8u; ++class_name) {
        assert(!json_object_set_new(data, "class", json_string(classes[class_name])));
        for (unsigned int retry = 0u; retry < 4u; ++retry) {
            assert(!json_object_set_new(data, "retry_count", json_integer(retry)));
            if (class_name < 6u && retry < 3u) {
                roundtrip("response_failed", data, 163u);
            } else {
                reject_json("response_failed", data);
            }
        }
    }
    assert(!json_object_set_new(data, "class", json_string("provider")));
    assert(!json_object_set_new(data, "retry_count", json_integer(-1)));
    reject_json("response_failed", data);
    assert(!json_object_set_new(data, "retry_count", json_integer(0)));
    assert(!json_object_set_new(data, "policy_stopped", json_false()));
    assert(!json_object_set_new(data, "turn_retry_attempts",
        json_integer((json_int_t)UINT32_MAX + 2)));
    reject_json("response_failed", data);
    assert(!json_object_set_new(data, "turn_retry_attempts", json_integer(-1)));
    reject_json("response_failed", data);
    assert(!json_object_set_new(data, "turn_retry_attempts", json_integer(0)));
    assert(!json_object_set_new(data, "new_input", json_false()));
    json_t *policy = json_pack("{s:s,s:s,s:s}",
        "code", "", "type", "", "clarification_skipped", "s");
    assert(policy && !json_object_set(data, "policy", policy));
    const char *flags[] = {"policy_stopped", "new_input"};
    for (size_t i = 0u; i < 2u; ++i) {
        assert(!json_object_set_new(data, flags[i], json_integer(0)));
        reject_json("response_failed", data);
        assert(!json_object_set_new(data, flags[i], json_null()));
        reject_json("response_failed", data);
        assert(!json_object_set_new(data, flags[i], json_false()));
    }
    char text[8193];
    memset(text, 'x', sizeof(text));
    const char *keys[] = {"code", "type", "clarification_skipped", "message"};
    const size_t limits[] = {63u, 63u, 127u, 8192u};
    for (size_t i = 0u; i < 4u; ++i) {
        json_t *object = i == 3u ? data : policy;
        json_t *old = json_incref(json_object_get(object, keys[i]));
        assert(!json_object_set_new(object, keys[i], json_stringn(text, limits[i])));
        roundtrip_checked("response_failed", data, 163u, false);
        assert(!json_object_set_new(object, keys[i], json_stringn(text, limits[i] + 1u)));
        reject_json("response_failed", data);
        json_object_del(object, keys[i]);
        reject_json("response_failed", data);
        assert(!json_object_set_new(object, keys[i], json_null()));
        reject_json("response_failed", data);
        assert(!json_object_set_new(object, keys[i], old));
    }
    assert(!json_object_set_new(policy, "clarification_skipped", json_string("")));
    reject_json("response_failed", data);
    assert(!json_object_set_new(policy, "clarification_skipped", json_string("s")));
    assert(!json_object_set_new(policy, "extra", json_null()));
    reject_json("response_failed", data);
    json_decref(policy);
    json_decref(data);
}

static void
response_public_storage(void)
{
    json_t *data = json_loads(samples[66].data, JSON_REJECT_DUPLICATES, NULL);
    json_t *item = json_object_get(data, "item");
    char *text = malloc(SNAG_MAX_PUBLIC_ITEM + 1u);
    assert(data && item && text);
    memset(text, 'x', SNAG_MAX_PUBLIC_ITEM + 1u);
    assert(!json_object_set_new(item, "provider_item_id",
        json_stringn(text, SNAG_MAX_PROVIDER_ID)));
    roundtrip("response_output", data, 161u);
    assert(!json_object_set_new(item, "provider_item_id",
        json_stringn(text, SNAG_MAX_PROVIDER_ID + 1u)));
    reject_json("response_output", data);
    assert(!json_object_set_new(item, "provider_item_id", json_string("p")));
    assert(!json_object_set_new(data, "index", json_integer(INT64_MAX)));
    assert(!json_object_set_new(data, "offset", json_integer(SNAG_MAX_PUBLIC_ITEM - 1u)));
    roundtrip("response_output", data, 161u);
    assert(!json_object_set_new(data, "offset", json_integer(SNAG_MAX_PUBLIC_ITEM)));
    reject_json("response_output", data);
    assert(!json_object_set_new(data, "offset", json_integer(-1)));
    reject_json("response_output", data);
    assert(!json_object_set_new(data, "offset", json_integer(0)));
    assert(!json_object_set_new(data, "index", json_integer(-1)));
    reject_json("response_output", data);
    assert(!json_object_set_new(data, "index", json_integer(0)));
    assert(!json_object_set_new(item, "text", json_stringn(text, SNAG_MAX_PUBLIC_ITEM)));
    roundtrip_checked("response_output", data, 161u, false);
    assert(!json_object_set_new(item, "text", json_stringn(text, SNAG_MAX_PUBLIC_ITEM + 1u)));
    reject_json("response_output", data);
    memset(text, '\1', SNAG_MAX_PUBLIC_ITEM);
    assert(!json_object_set_new(item, "text", json_stringn(text, SNAG_MAX_PUBLIC_ITEM)));
    reject_json("response_output", data);
    json_decref(data);

    memset(text, 'x', SNAG_MAX_PUBLIC_ITEM + 1u);
    data = json_loads(samples[67].data, JSON_REJECT_DUPLICATES, NULL);
    json_t *partial = json_object_get(data, "partial_public");
    const char *ids[] = {ID, NEW_ID, "ffffffffffffffffffffffffffffffff",
        "00000000000000000000000000000000"};
    for (size_t i = 0u; i < 4u; ++i) {
        item = json_loads(PUBLIC_ITEM, JSON_REJECT_DUPLICATES, NULL);
        assert(item && !json_object_set_new(item, "local_item_id", json_string(ids[i])));
        assert(!json_object_set_new(item, "text", json_stringn(text, SNAG_MAX_PUBLIC_ITEM)));
        assert(!json_array_append_new(partial, item));
    }
    size_t encoded;
    assert(!snag_json_digest_bounded(partial, SNAG_MAX_EVENT_LINE, NULL, &encoded));
    assert(encoded > SNAG_MAX_RESPONSE_GRAPH && encoded - SNAG_MAX_RESPONSE_GRAPH < 1024u);
    reject_json("response_interrupted", data);
    size_t last_size = SNAG_MAX_PUBLIC_ITEM - (encoded - SNAG_MAX_RESPONSE_GRAPH);
    assert(!json_object_set_new(item, "text", json_stringn(text, last_size)));
    assert(!snag_json_digest_bounded(partial, SNAG_MAX_EVENT_LINE, NULL, &encoded));
    assert(encoded == SNAG_MAX_RESPONSE_GRAPH);
    roundtrip_checked("response_interrupted", data, 162u, false);
    assert(!json_object_set_new(item, "text", json_stringn(text, last_size + 1u)));
    reject_json("response_interrupted", data);
    json_decref(data);
    free(text);
}

static void
response_public_projection(void)
{
    json_t *data = json_loads(samples[66].data, JSON_REJECT_DUPLICATES, NULL);
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    enum snag_binary_kind kind;
    assert(data && !snag_binary_legacy_encode(&bytes, "response_output", data, &kind));
    struct snag_binary_record record = {
        .kind = (uint16_t)kind, .version = 1u, .payload = bytes.data, .size = bytes.len
    };
    struct snag_binary_event output;
    assert(!snag_binary_event_decode(&record, &output));
    output.data.response_output.index = UINT64_MAX;
    reject_native_projection(&output, 0);
    output.data.response_output.index = 0u;
    struct snag_binary_public_value values[2] = {
        {.item = output.data.response_output.item}, {.item = output.data.response_output.item}
    };
    char *text = malloc(SNAG_MAX_PUBLIC_ITEM);
    assert(text);
    memset(text, '\1', SNAG_MAX_PUBLIC_ITEM);
    output.data.response_output.item.text =
        (struct snag_binary_text){(const unsigned char *)text, SNAG_MAX_PUBLIC_ITEM};
    reject_native_projection(&output, 0);
    free(text);
    struct snag_buf duplicate = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_public_items_encode(&duplicate, values, 2u));
    values[1].item.text = (struct snag_binary_text){0};
    values[1].source = (struct snag_binary_output_span){
        .first = {1u, 79u, 1u}, .last_sequence = 1u, .bytes = 1u
    };
    struct snag_buf referenced = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_public_items_encode(&referenced, values, 2u));
    for (size_t sample = 67u; sample < 70u; ++sample) {
        json_t *source = json_loads(samples[sample].data, JSON_REJECT_DUPLICATES, NULL);
        struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
        assert(source && !snag_binary_legacy_encode(&encoded, samples[sample].type, source, &kind));
        record = (struct snag_binary_record){
            .kind = (uint16_t)kind, .version = 1u, .payload = encoded.data, .size = encoded.len
        };
        struct snag_binary_event event;
        assert(!snag_binary_event_decode(&record, &event));
        struct snag_binary_public_items *partial = sample == 67u ?
            &event.data.response_interrupted.partial : sample == 68u ?
            &event.data.response_failed.partial : &event.data.response_correction.partial;
        *partial = (struct snag_binary_public_items){duplicate.data, duplicate.len};
        reject_native_projection(&event, 0);
        *partial = (struct snag_binary_public_items){referenced.data, referenced.len};
        reject_native_projection(&event, ENOTSUP);
        snag_buf_free(&encoded);
        json_decref(source);
    }
    snag_buf_free(&referenced);
    snag_buf_free(&duplicate);
    snag_buf_free(&bytes);
    json_decref(data);
}

static void
response_complete_usage(void)
{
    const char *keys[] = {"input_tokens", "output_tokens", "reasoning_tokens", "total_tokens"};
    const json_int_t tokens[] = {10, 4, 2, 14};
    for (unsigned int mask = 0u; mask < 16u; ++mask) {
        for (unsigned int cached = 0u; cached < 3u; ++cached) {
            json_t *data = json_loads(samples[70].data, JSON_REJECT_DUPLICATES, NULL);
            json_t *usage = json_object_get(data, "usage");
            assert(data && usage);
            for (size_t i = 0u; i < 4u; ++i) {
                assert(!json_object_set_new(usage, keys[i], mask & (1u << i) ?
                    json_integer(tokens[i]) : json_null()));
            }
            if (cached) {
                assert(!json_object_set_new(usage, "cached_tokens", cached == 1u ?
                    json_null() : json_integer(INT64_MAX)));
            }
            roundtrip("response_completed", data, 165u);
            struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
            enum snag_binary_kind kind;
            assert(!snag_binary_legacy_encode(&bytes, "response_completed", data, &kind));
            assert(bytes.len >= 47u && bytes.data[45] ==
                (mask | (cached ? 32u : 0u) | (cached == 2u ? 16u : 0u)));
            snag_buf_free(&bytes);
            json_decref(data);
        }
    }
    json_t *data = json_loads(samples[70].data, JSON_REJECT_DUPLICATES, NULL);
    json_t *usage = json_object_get(data, "usage");
    for (size_t i = 0u; i < 4u; ++i) {
        json_object_del(usage, keys[i]);
        reject_json("response_completed", data);
        assert(!json_object_set_new(usage, keys[i], json_integer(-1)));
        reject_json("response_completed", data);
        assert(!json_object_set_new(usage, keys[i], json_true()));
        reject_json("response_completed", data);
        assert(!json_object_set_new(usage, keys[i], json_integer(INT64_MAX)));
        roundtrip("response_completed", data, 165u);
        assert(!json_object_set_new(usage, keys[i], json_null()));
    }
    assert(!json_object_set_new(usage, "input_tokens", json_integer(INT64_MAX)));
    assert(!json_object_set_new(usage, "output_tokens", json_integer(0)));
    assert(!json_object_set_new(usage, "reasoning_tokens", json_integer(0)));
    assert(!json_object_set_new(usage, "total_tokens", json_integer(INT64_MAX)));
    roundtrip("response_completed", data, 165u);
    assert(!json_object_set_new(usage, "output_tokens", json_integer(1)));
    reject_json("response_completed", data);
    assert(!json_object_set_new(usage, "total_tokens", json_null()));
    roundtrip("response_completed", data, 165u);
    assert(!json_object_set_new(usage, "reasoning_tokens", json_integer(2)));
    reject_json("response_completed", data);
    assert(!json_object_set_new(usage, "reasoning_tokens", json_integer(1)));
    assert(!json_object_set_new(usage, "cached_tokens", json_integer(-1)));
    reject_json("response_completed", data);
    assert(!json_object_set_new(usage, "cached_tokens", json_true()));
    reject_json("response_completed", data);
    assert(!json_object_set_new(usage, "cached_tokens", json_null()));
    assert(!json_object_set_new(usage, "extra", json_null()));
    reject_json("response_completed", data);
    json_decref(data);
}

static void
response_complete_graph(void)
{
    json_t *data = json_loads(samples[70].data, JSON_REJECT_DUPLICATES, NULL);
    json_t *items = json_object_get(data, "items");
    json_t *public = json_loads(PUBLIC_ITEM, JSON_REJECT_DUPLICATES, NULL);
    json_t *call = json_loads(CALL_ITEM, JSON_REJECT_DUPLICATES, NULL);
    assert(data && public && call && !json_array_append(items, public));
    roundtrip("response_completed", data, 165u);
    assert(!json_array_append(items, call));
    roundtrip("response_completed", data, 165u);
    assert(!json_object_set_new(public, "phase", json_string("final_answer")));
    roundtrip("response_completed", data, 165u);
    assert(!json_object_set_new(public, "kind", json_string("refusal")));
    roundtrip("response_completed", data, 165u);
    json_t *second = json_deep_copy(public);
    assert(second && !json_array_append(items, second));
    reject_json("response_completed", data);
    assert(!json_object_set_new(second, "local_item_id", json_string(NEW_ID)));
    roundtrip("response_completed", data, 165u);
    json_decref(second);
    second = json_deep_copy(call);
    assert(second && !json_array_append(items, second));
    reject_json("response_completed", data);
    assert(!json_object_set_new(second, "call_id", json_string(NEW_ID)));
    roundtrip("response_completed", data, 165u);
    json_decref(second);
    const char *keys[] = {
        "kind", "call_id", "provider_item_id", "provider_call_id", "name", "arguments"
    };
    for (size_t i = 0u; i < 6u; ++i) {
        json_t *old = json_incref(json_object_get(call, keys[i]));
        json_object_del(call, keys[i]);
        reject_json("response_completed", data);
        assert(!json_object_set_new(call, keys[i], json_null()));
        reject_json("response_completed", data);
        assert(!json_object_set_new(call, keys[i], old));
    }
    assert(!json_object_set_new(call, "extra", json_null()));
    reject_json("response_completed", data);
    json_object_del(call, "extra");
    assert(!json_object_set_new(call, "name", json_string("not_a_tool")));
    reject_json("response_completed", data);
    assert(!json_object_set_new(call, "name", json_string("get_cwd")));
    assert(!json_object_set_new(call, "provider_call_id", json_string("\n")));
    reject_json("response_completed", data);
    assert(!json_object_set_new(call, "provider_call_id", json_string("c")));
    json_t *arguments = json_loads(
        "{\"\":[null,false,true,-7,\"s\",[],{}],\"x\":{\"a\":1}}", 0u, NULL);
    assert(arguments && !json_object_set_new(call, "arguments", arguments));
    roundtrip("response_completed", data, 165u);
    assert(!json_object_set_new(data, "cycle", json_integer(UINT32_MAX)));
    roundtrip("response_completed", data, 165u);
    const json_int_t bad_cycles[] = {-1, 0, (json_int_t)UINT32_MAX + 1};
    for (size_t i = 0u; i < 3u; ++i) {
        assert(!json_object_set_new(data, "cycle", json_integer(bad_cycles[i])));
        reject_json("response_completed", data);
    }
    assert(!json_object_set_new(data, "cycle", json_integer(1)));
    assert(!json_object_set_new(data, "status", json_string("failed")));
    reject_json("response_completed", data);
    assert(!json_object_set_new(data, "status", json_string("completed")));
    assert(!json_object_set_new(data, "provider_response_id", json_string("\n")));
    reject_json("response_completed", data);
    json_decref(call);
    json_decref(public);
    json_decref(data);
}

static void
response_complete_continuation(void)
{
    for (unsigned int mask = 0u; mask < 4u; ++mask) {
        for (unsigned int form = 0u; form < 3u; ++form) {
            json_t *data = json_loads(samples[70].data, JSON_REJECT_DUPLICATES, NULL);
            assert(data);
            if (mask & 1u) {
                json_t *continuation = form == 0u ? json_null() : form == 1u ? json_array() :
                    json_pack("[{s:i,s:{s:s}}]", "before", 0, "item", "type", "reasoning");
                assert(continuation && !json_object_set_new(data, "continuation", continuation));
            }
            if (mask & 2u) {
                assert(!json_object_set_new(data, "continuation_scope", json_string(HASHF)));
            }
            if (mask == 0u || mask == 3u) {
                roundtrip("response_completed", data, 165u);
            } else {
                reject_json("response_completed", data);
            }
            json_decref(data);
        }
    }
    json_t *data = json_loads(samples[70].data, JSON_REJECT_DUPLICATES, NULL);
    assert(data && !json_object_set_new(data, "continuation_scope", json_string(HASHF)));
    assert(!json_array_append_new(json_object_get(data, "items"),
        json_loads(PUBLIC_ITEM, 0u, NULL)));
    json_t *continuation = json_loads("[{\"before\":0,\"item\":{\"type\":\"reasoning\","
        "\"id\":\"r\",\"status\":\"completed\",\"encrypted_content\":null,"
        "\"x\":{\"\":[true,null,7]},"
        "\"content\":[{\"type\":\"reasoning_text\",\"text\":\"\",\"x\":0}],"
        "\"summary\":[{\"type\":\"summary_text\",\"text\":\"s\"}]}},"
        "{\"before\":1,\"item\":{\"type\":\"reasoning\",\"status\":null}}]", 0u, NULL);
    assert(continuation && !json_object_set(data, "continuation", continuation));
    roundtrip("response_completed", data, 165u);
    json_t *last = json_array_get(continuation, 1u);
    const json_int_t before[] = {-1, 2};
    for (size_t i = 0u; i < 2u; ++i) {
        assert(!json_object_set_new(last, "before", json_integer(before[i])));
        reject_json("response_completed", data);
    }
    assert(!json_object_set_new(last, "before", json_integer(0)));
    roundtrip("response_completed", data, 165u);
    assert(!json_object_set_new(json_array_get(continuation, 0u), "before", json_integer(1)));
    reject_json("response_completed", data);
    assert(!json_object_set_new(last, "before", json_integer(1)));
    roundtrip("response_completed", data, 165u);
    assert(!json_object_set_new(last, "extra", json_null()));
    reject_json("response_completed", data);
    json_object_del(last, "extra");
    json_t *item = json_object_get(last, "item");
    assert(!json_object_set_new(item, "type", json_string("tool_call")));
    reject_json("response_completed", data);
    assert(!json_object_set_new(item, "type", json_string("reasoning")));
    assert(!json_object_set_new(item, "status", json_string("failed")));
    reject_json("response_completed", data);
    assert(!json_object_set_new(item, "status", json_null()));
    assert(!json_object_set_new(item, "id", json_string("")));
    reject_json("response_completed", data);
    json_object_del(item, "id");
    assert(!json_object_set_new(item, "summary", json_pack("[{s:s,s:s}]",
        "type", "reasoning_text", "text", "s")));
    reject_json("response_completed", data);
    json_object_del(item, "summary");
    assert(!json_object_set_new(data, "continuation_scope", json_null()));
    reject_json("response_completed", data);
    assert(!json_object_set_new(data, "continuation_scope", json_string("wrong")));
    reject_json("response_completed", data);
    json_decref(continuation);
    json_decref(data);
}

static void
response_complete_storage(void)
{
    json_t *data = json_loads(samples[70].data, JSON_REJECT_DUPLICATES, NULL);
    json_t *items = json_object_get(data, "items");
    json_t *continuation = json_array();
    char *text = malloc(SNAG_MAX_RESPONSE_GRAPH + 1u);
    assert(data && continuation && text);
    memset(text, 'x', SNAG_MAX_RESPONSE_GRAPH + 1u);
    assert(!json_object_set(data, "continuation", continuation));
    assert(!json_object_set_new(data, "continuation_scope", json_string(HASHF)));
    for (size_t i = 0u; i < 2u; ++i) {
        json_t *call = json_loads(CALL_ITEM, 0u, NULL);
        assert(call && !json_object_set_new(call, "call_id", json_string(i ? NEW_ID : ID)));
        json_t *arguments = json_object_get(call, "arguments");
        assert(!json_object_set_new(arguments, "i", json_integer((json_int_t)i)));
        assert(!json_object_set_new(arguments, "x", json_stringn(text, 65537u + i * 65541u)));
        assert(!json_array_append_new(items, call));
        json_t *record = json_pack("{s:i,s:{s:s,s:i}}", "before", i ? 2 : 0,
            "item", "type", "reasoning", "i", (int)i);
        assert(record && !json_object_set_new(json_object_get(record, "item"), "x",
            json_stringn(text, 524289u + i * 524291u)));
        assert(!json_array_append_new(continuation, record));
    }
    roundtrip_checked("response_completed", data, 165u, false);
    json_t *call = json_array_get(items, 0u);
    json_t *arguments = json_object_get(call, "arguments");
    json_object_del(arguments, "i");
    assert(!json_object_set_new(arguments, "x", json_stringn(text, SNAG_MAX_TOOL_ARGUMENTS - 8u)));
    roundtrip_checked("response_completed", data, 165u, false);
    assert(!json_object_set_new(arguments, "x", json_stringn(text, SNAG_MAX_TOOL_ARGUMENTS - 7u)));
    reject_json("response_completed", data);
    assert(!json_object_set_new(arguments, "x", json_string("small")));
    assert(!json_array_clear(continuation));
    json_t *record = json_pack("{s:i,s:{s:s,s:s}}", "before", 0,
        "item", "type", "reasoning", "x", "");
    assert(record && !json_array_append_new(continuation, record));
    size_t overhead;
    assert(!snag_json_digest_bounded(continuation, SNAG_MAX_RESPONSE_GRAPH, NULL, &overhead));
    json_t *item = json_object_get(record, "item");
    assert(overhead < 128u);
    assert(!json_object_set_new(item, "x", json_stringn(text, SNAG_MAX_RESPONSE_GRAPH - overhead)));
    roundtrip_checked("response_completed", data, 165u, false);
    assert(!json_object_set_new(item, "x",
        json_stringn(text, SNAG_MAX_RESPONSE_GRAPH - overhead + 1u)));
    reject_json("response_completed", data);
    json_decref(continuation);
    json_decref(data);
    free(text);
}

static void
response_complete_projection(void)
{
    json_t *data = json_loads(samples[70].data, JSON_REJECT_DUPLICATES, NULL);
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    enum snag_binary_kind kind;
    assert(data && !snag_binary_legacy_encode(&bytes, "response_completed", data, &kind));
    struct snag_binary_record record = {
        .kind = (uint16_t)kind, .version = 1u, .payload = bytes.data, .size = bytes.len
    };
    struct snag_binary_event event;
    assert(!snag_binary_event_decode(&record, &event));
    struct snag_binary_graph_item items[2] = {{.kind = SNAG_BINARY_ITEM_ASSISTANT}};
    items[0].data.output.item = (struct snag_binary_public_item){
        .kind = SNAG_BINARY_ITEM_ASSISTANT, .phase = SNAG_BINARY_PHASE_COMMENTARY,
        .provider_id = {(const unsigned char *)"p", 1u}, .text = {(const unsigned char *)"t", 1u}
    };
    items[1] = items[0];
    struct snag_buf graph = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_graph_items_encode(&graph, items, 2u));
    event.data.response_completed.items =
        (struct snag_binary_graph_items){graph.data, graph.len, 2u};
    reject_native_projection(&event, 0);
    items[1].data.output.item.text = (struct snag_binary_text){0};
    items[1].data.output.source = (struct snag_binary_output_span){
        .first = {1u, 79u, 1u}, .last_sequence = 1u, .bytes = 1u
    };
    snag_buf_reset(&graph);
    assert(!snag_binary_graph_items_encode(&graph, items, 2u));
    event.data.response_completed.items =
        (struct snag_binary_graph_items){graph.data, graph.len, 2u};
    reject_native_projection(&event, ENOTSUP);
    char *text = malloc(SNAG_MAX_PUBLIC_ITEM);
    assert(text);
    memset(text, '\1', SNAG_MAX_PUBLIC_ITEM);
    items[0].data.output.item.text =
        (struct snag_binary_text){(const unsigned char *)text, SNAG_MAX_PUBLIC_ITEM};
    snag_buf_reset(&graph);
    assert(!snag_binary_graph_items_encode(&graph, items, 1u));
    event.data.response_completed.items =
        (struct snag_binary_graph_items){graph.data, graph.len, 1u};
    reject_native_projection(&event, 0);
    free(text);
    snag_buf_free(&graph);
    snag_buf_free(&bytes);
    json_decref(data);
}

static void
tool_process_variants(void)
{
    json_t *data = json_loads(samples[71].data, JSON_REJECT_DUPLICATES, NULL);
    const char *paths[] = {"C:\\work\\source", "\\\\server\\share\\dir", "/work/source"};
    assert(data);
    for (size_t i = 0u; i < 3u; ++i) {
        assert(!json_object_set_new(data, "resolved_workdir", json_string(paths[i])));
        roundtrip("tool_started", data, 176u);
    }
    char *path = malloc(SNAG_PATH_MAX_BYTES + 1u);
    assert(path);
    memset(path, 'p', SNAG_PATH_MAX_BYTES + 1u);
    path[0] = '/';
    assert(!json_object_set_new(data, "resolved_workdir", json_stringn(path, SNAG_PATH_MAX_BYTES)));
    roundtrip_checked("tool_started", data, 176u, false);
    assert(!json_object_set_new(data, "resolved_workdir",
        json_stringn(path, SNAG_PATH_MAX_BYTES + 1u)));
    reject_json("tool_started", data);
    assert(!json_object_set_new(data, "resolved_workdir", json_string("")));
    reject_json("tool_started", data);
    free(path);
    json_decref(data);
    for (size_t sample = 72u; sample < 74u; ++sample) {
        data = json_loads(samples[sample].data, JSON_REJECT_DUPLICATES, NULL);
        const json_int_t bytes = sample == 72u ? 1 : 2;
        assert(data && !json_object_set_new(data, "offset", json_integer(INT64_MAX - bytes)));
        roundtrip("process_output", data, 192u);
        assert(!json_object_set_new(data, "offset", json_integer(INT64_MAX - bytes + 1)));
        reject_json("process_output", data);
        assert(!json_object_set_new(data, "offset", json_integer(-1)));
        reject_json("process_output", data);
        assert(!json_object_set_new(data, "offset", json_integer(0)));
        const json_int_t streams[] = {0, 1, 2, -1};
        for (size_t i = 0u; i < 4u; ++i) {
            assert(!json_object_set_new(data, "stream", json_integer(streams[i])));
            if (i < 2u) roundtrip("process_output", data, 192u);
            else reject_json("process_output", data);
        }
        assert(!json_object_set_new(data, "stream", json_false()));
        reject_json("process_output", data);
        assert(!json_object_set_new(data, "stream", json_integer(0)));
        assert(!json_object_set_new(data, "encoding", json_string("bytes")));
        reject_json("process_output", data);
        json_decref(data);
    }
    data = json_loads(samples[73].data, JSON_REJECT_DUPLICATES, NULL);
    const char *invalid[] = {
        "", "!!!!", "AB==", "AAB=", "AA=A", "====", "AAA", "AA==AAAA", "AA==\n"
    };
    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!json_object_set_new(data, "data", json_string(invalid[i])));
        reject_json("process_output", data);
    }
    const char *valid[] = {"AA==", "AAA=", "AAAA", "/w==", "YWJj", "/+7d"};
    for (size_t i = 0u; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        assert(!json_object_set_new(data, "data", json_string(valid[i])));
        roundtrip("process_output", data, 192u);
    }
    json_decref(data);
}

static void
tool_process_storage(void)
{
    json_t *data = json_loads(samples[72].data, JSON_REJECT_DUPLICATES, NULL);
    unsigned char *bytes = malloc(SNAG_BINARY_PROCESS_CHUNK_MAX + 1u);
    assert(data && bytes);
    memset(bytes, 'x', SNAG_BINARY_PROCESS_CHUNK_MAX + 1u);
    assert(!json_object_set_new(data, "data", json_string("")));
    reject_json("process_output", data);
    assert(!json_object_set_new(data, "data", json_string("\xe2\x82\xac\n")));
    roundtrip("process_output", data, 192u);
    for (size_t count = SNAG_BINARY_PROCESS_CHUNK_MAX - 1u;
        count <= SNAG_BINARY_PROCESS_CHUNK_MAX + 1u; ++count) {
        assert(!json_object_set_new(data, "data", json_stringn((const char *)bytes, count)));
        if (count <= SNAG_BINARY_PROCESS_CHUNK_MAX) {
            roundtrip_checked("process_output", data, 192u, false);
        } else reject_json("process_output", data);
    }
    assert(!json_object_set_new(data, "encoding", json_string("base64")));
    for (size_t i = 0u; i <= SNAG_BINARY_PROCESS_CHUNK_MAX; ++i) {
        bytes[i] = (unsigned char)i;
    }
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    const size_t counts[] = {
        256u, SNAG_BINARY_PROCESS_CHUNK_MAX, SNAG_BINARY_PROCESS_CHUNK_MAX + 1u
    };
    for (size_t i = 0u; i < 3u; ++i) {
        snag_buf_reset(&encoded);
        assert(!snag_base64_append(&encoded, bytes, counts[i]));
        assert(!json_object_set_new(data, "data",
            json_stringn((const char *)encoded.data, encoded.len)));
        if (i < 2u) roundtrip_checked("process_output", data, 192u, false);
        else reject_json("process_output", data);
    }
    snag_buf_free(&encoded);
    free(bytes);
    json_decref(data);
}

static void
tool_result_domain(void)
{
    const char *statuses[] = {
        "not_run", "outcome_unknown", "denied", "cancelled", "running", "succeeded", "failed",
        "signaled", "timed_out", "patch_rejected", "io_failed", "unknown"
    };
    const char *reasons[] = {
        NULL, "protocol_conflict", "read_only", "process_limit", "batch_yield", "operator_yield",
        "process_busy", "stdin_busy", "stdin_closed", "invalid_arguments",
        "managed_process_conflict",
        "managed_process_handle_mismatch", "recovery_unstarted", "superseded_by_steering",
        "turn_cancelled", "process_interaction_required", "rule_rejected", "owner_lost",
        "unreaped_after_sigkill", "user_denied", "timeout_handoff", "wait_timeout",
        "steering_handoff",
        "output_drain_timeout", "unknown"
    };
    json_t *data = json_loads(samples[74].data, 0, NULL);
    json_t *result = json_object_get(data, "result");
    for (size_t s = 0u; s < sizeof(statuses) / sizeof(statuses[0]); ++s) {
        assert(!json_object_set_new(result, "status", json_string(statuses[s])));
        for (size_t r = 0u; r < sizeof(reasons) / sizeof(reasons[0]); ++r) {
            assert(!json_object_set_new(result, "reason",
                reasons[r] ? json_string(reasons[r]) : json_null()));
            for (unsigned int h = 0u; h < 3u; ++h) {
                assert(!json_object_set_new(result, "handle",
                    h ? json_string(h == 1u ? ID : "bad") : json_null()));
                for (unsigned int v = 0u; v < 3u; ++v) {
                    assert(!json_object_set_new(result, "exit_code", v == 0u ? json_integer(0) :
                        v == 1u ? json_null() : json_pack("{s:i}", "ignored", 1)));
                    assert(!json_object_set_new(result, "signal", v == 0u ? json_null() :
                        v == 1u ? json_integer(9) : json_pack("[i]", 2)));
                    if (!snag_tool_result_valid(result)) {
                        roundtrip_checked("tool_finished", data, 177u, false);
                    } else {
                        reject_json("tool_finished", data);
                    }
                }
            }
        }
    }
    json_decref(data);

    const char *causes[] = {"user_interrupt", "provider_failure", "protocol_failure",
        "tool_failure", "output_failure", "internal_failure", "unknown"};
    const char *valid_reasons[] = {"read_only", "owner_lost", "user_denied", "turn_cancelled"};
    const char *values[] = {"null", "false", "true", "-9223372036854775808", "\"v\"",
        "[0,null]", "{\"\":false,\"z\":{\"n\":3}}"};
    for (size_t s = 0u; s < 11u; ++s) {
        data = json_loads(samples[75].data, 0, NULL);
        result = json_object_get(data, "result");
        assert(!json_object_set_new(result, "status", json_string(statuses[s])));
        assert(!json_object_set_new(result, "reason",
            s < 4u ? json_string(valid_reasons[s]) : json_null()));
        if (s == 4u) {
            assert(!json_object_set_new(result, "handle", json_string(ID)));
        }
        if (s == 7u) {
            assert(!json_object_set_new(result, "exit_code", json_null()));
            assert(!json_object_set_new(result, "signal", json_integer(9)));
        }
        bool terminal = snag_string_in(statuses[s],
            "succeeded failed signaled timed_out cancelled outcome_unknown io_failed");
        for (size_t c = 0u; c < 7u; ++c) {
            assert(!json_object_set_new(data, "cause", json_string(causes[c])));
            if (terminal && c < 6u) roundtrip("process_closed", data, 193u);
            else reject_json("process_closed", data);
        }
        assert(!json_object_set_new(data, "cause", json_string(causes[0])));
        if (s < 5u || s >= 8u) {
            for (size_t a = 0u; a < 7u; ++a) {
                for (size_t b = 0u; b < 7u; ++b) {
                    assert(!json_object_set_new(result, "exit_code",
                        json_loads(values[a], JSON_DECODE_ANY, NULL)));
                    assert(!json_object_set_new(result, "signal",
                        json_loads(values[b], JSON_DECODE_ANY, NULL)));
                    assert(!snag_tool_result_valid(result));
                    if (terminal) {
                        roundtrip_checked("process_closed", data, 193u, false);
                    }
                    json_t *finish = json_pack("{s:s,s:s,s:O}",
                        "turn_id", ID, "call_id", NEW_ID, "result", result);
                    assert(finish);
                    roundtrip_checked("tool_finished", finish, 177u, false);
                    json_decref(finish);
                }
            }
        }
        json_decref(data);
    }
}

static void
tool_result_options(void)
{
    for (size_t n = 74u; n <= 75u; ++n) {
        for (unsigned int mask = 0u; mask < 8u; ++mask) {
            json_t *data = json_loads(samples[n].data, 0, NULL);
            json_t *result = json_object_get(data, "result");
            if (mask & 1u) {
                assert(!json_object_set_new(result, "max_output_tokens", json_integer(4000000000)));
            }
            if (mask & 2u) {
                assert(!json_object_set_new(result, "output_ref", json_loads(OUTPUT_REF, 0, NULL)));
            }
            if (mask & 4u) {
                assert(!json_object_set_new(result, "content",
                    json_pack("[{s:s,s:s}]", "type", "input_text", "text", "t")));
            }
            if ((mask & 2u) && !(mask & 1u)) reject_json(samples[n].type, data);
            else roundtrip(samples[n].type, data, samples[n].kind);
            json_decref(data);
        }
        json_t *data = json_loads(samples[n].data, 0, NULL);
        json_t *result = json_object_get(data, "result");
        const char *keys[] = {"duration_ms", "max_output_tokens"};
        const json_int_t numbers[] = {-1, 0, 1, 4000000000, 4000000001, INT64_MAX};
        for (size_t k = 0u; k < 2u; ++k) {
            for (size_t i = 0u; i < sizeof(numbers) / sizeof(numbers[0]); ++i) {
                assert(!json_object_set_new(result, keys[k], json_integer(numbers[i])));
                if (!snag_tool_result_valid(result)) {
                    roundtrip(samples[n].type, data, samples[n].kind);
                } else {
                    reject_json(samples[n].type, data);
                }
            }
        }
        assert(!json_object_set_new(result, "max_output_tokens", json_integer(1)));
        assert(!json_object_set_new(result, "output_ref", json_loads(OUTPUT_REF, 0, NULL)));
        json_t *ref = json_object_get(result, "output_ref");
        const char *key;
        json_t *value;
        json_object_foreach(ref, key, value) {
            json_t *copy = json_deep_copy(data);
            json_t *changed = json_object_get(json_object_get(copy, "result"), "output_ref");
            assert(!json_object_del(changed, key));
            reject_json(samples[n].type, copy);
            assert(!json_object_set_new(changed, key, json_null()));
            reject_json(samples[n].type, copy);
            if (json_is_integer(value)) {
                for (size_t i = 0u; i < sizeof(numbers) / sizeof(numbers[0]); ++i) {
                    assert(!json_object_set_new(changed, key, json_integer(numbers[i])));
                    if (!snag_tool_result_valid(json_object_get(copy, "result"))) {
                        roundtrip(samples[n].type, copy, samples[n].kind);
                    } else {
                        reject_json(samples[n].type, copy);
                    }
                }
            }
            json_decref(copy);
        }
        assert(!json_object_set_new(ref, "stdin_open", json_false()));
        roundtrip(samples[n].type, data, samples[n].kind);
        assert(!json_object_set_new(ref, "stdin_open", json_integer(0)));
        reject_json(samples[n].type, data);
        assert(!json_object_set_new(ref, "stdin_open", json_true()));
        assert(!json_object_set_new(ref, "unknown", json_integer(0)));
        reject_json(samples[n].type, data);
        assert(!json_object_del(ref, "unknown"));
        json_object_foreach(result, key, value) {
            json_t *copy = json_deep_copy(data);
            json_t *changed = json_object_get(copy, "result");
            assert(!json_object_del(changed, key));
            if (snag_string_in(key, "max_output_tokens output_ref")) {
                if (!strcmp(key, "output_ref")) {
                    roundtrip(samples[n].type, copy, samples[n].kind);
                } else {
                    reject_json(samples[n].type, copy);
                }
            } else {
                reject_json(samples[n].type, copy);
            }
            assert(!json_object_set_new(changed, key, json_null()));
            if (!snag_tool_result_valid(changed)) {
                roundtrip(samples[n].type, copy, samples[n].kind);
            } else {
                reject_json(samples[n].type, copy);
            }
            json_decref(copy);
        }
        assert(!json_object_set_new(result, "unknown", json_true()));
        reject_json(samples[n].type, data);
        json_decref(data);
    }
}

static void
tool_result_excerpts(void)
{
    const char *streams[] = {"stdout", "stderr"};
    const char *encodings[] = {"utf8", "base64", "unknown"};
    const char *texts[] = {"", "t", "!!!!", "AB==", "AAB=", "a\nbc"};
    const json_int_t counts[] = {-1, 0, 1, 4, INT64_MAX};
    json_t *data = json_loads(samples[74].data, 0, NULL);
    json_t *result = json_object_get(data, "result");
    for (size_t s = 0u; s < 2u; ++s) {
        json_t *excerpt = json_object_get(result, streams[s]);
        for (size_t e = 0u; e < 3u; ++e) {
            assert(!json_object_set_new(excerpt, "encoding", json_string(encodings[e])));
            for (size_t t = 0u; t < sizeof(texts) / sizeof(texts[0]); ++t) {
                assert(!json_object_set_new(excerpt, "retained", json_string(texts[t])));
                for (size_t i = 0u; i < sizeof(counts) / sizeof(counts[0]); ++i) {
                    assert(!json_object_set_new(excerpt, "retained_bytes",
                        json_integer(counts[i])));
                    if (!snag_tool_result_valid(result)) {
                        roundtrip_checked("tool_finished", data, 177u, false);
                    } else {
                        reject_json("tool_finished", data);
                    }
                }
            }
        }
        assert(!json_object_set_new(excerpt, "encoding", json_string("utf8")));
        assert(!json_object_set_new(excerpt, "retained", json_string("t")));
        assert(!json_object_set_new(excerpt, "retained_bytes", json_integer(1)));
        for (size_t a = 0u; a < 5u; ++a) {
            for (size_t b = 0u; b < 5u; ++b) {
                assert(!json_object_set_new(excerpt, "original_bytes", json_integer(counts[a])));
                assert(!json_object_set_new(excerpt, "discarded_bytes", json_integer(counts[b])));
                if (!snag_tool_result_valid(result)) {
                    roundtrip("tool_finished", data, 177u);
                } else {
                    reject_json("tool_finished", data);
                }
            }
        }
        const char *key;
        json_t *value;
        json_object_foreach(excerpt, key, value) {
            json_t *copy = json_deep_copy(data);
            json_t *changed = json_object_get(json_object_get(copy, "result"), streams[s]);
            assert(!json_object_del(changed, key));
            reject_json("tool_finished", copy);
            assert(!json_object_set_new(changed, key, json_null()));
            reject_json("tool_finished", copy);
            json_decref(copy);
        }
        assert(!json_object_set_new(excerpt, "unknown", json_true()));
        reject_json("tool_finished", data);
        assert(!json_object_del(excerpt, "unknown"));
    }
    json_decref(data);
}

static void
tool_result_storage(void)
{
    json_t *data = json_loads(samples[74].data, 0, NULL);
    json_t *result = json_object_get(data, "result");
    assert(!json_object_set_new(result, "status", json_string("io_failed")));
    size_t size = 2u * 1024u * 1024u + 1u;
    char *text = malloc(size);
    assert(text);
    memset(text, 't', size);
    assert(!json_object_set_new(result, "exit_code", json_stringn(text, 65537u)));
    assert(!json_object_set_new(result, "signal", json_stringn(text, 131073u)));
    json_t *content = json_pack("[{s:s,s:s}]", "type", "input_text", "text", "t");
    assert(content && !json_object_set_new(json_array_get(content, 0u), "text",
        json_stringn(text, 262145u)));
    assert(!json_object_set_new(result, "content", content));
    assert(!json_object_set_new(result, "model_text", json_stringn(text, size)));
    roundtrip_checked("tool_finished", data, 177u, false);
    free(text);
    assert(!json_object_set_new(result, "exit_code", json_real(1.5)));
    reject_json("tool_finished", data);
    assert(!json_object_set_new(result, "exit_code", json_stringn("a\0b", 3u)));
    reject_json("tool_finished", data);
    assert(!json_object_set_new(result, "exit_code", json_null()));
    assert(!json_object_set_new(result, "signal", json_null()));
    content = json_loads("[{\"type\":\"input_image\",\"asset\":" ASSET "},"
        "{\"type\":\"input_image\",\"asset\":" ASSET "}]", 0, NULL);
    assert(content);
    for (size_t i = 0u; i < 2u; ++i) {
        assert(!json_object_set_new(json_object_get(json_array_get(content, i), "asset"),
            "bytes", json_integer(6u * 1024u * 1024u)));
    }
    assert(!json_object_set_new(result, "content", content));
    roundtrip_checked("tool_finished", data, 177u, false);
    assert(!json_object_set_new(json_object_get(json_array_get(content, 1u), "asset"),
        "bytes", json_integer(6u * 1024u * 1024u + 1u)));
    reject_json("tool_finished", data);
    assert(!json_object_del(result, "content"));
    assert(!json_object_set_new(result, "model_text", json_string("")));
    json_t *wrapped = json_pack("{s:O}", "data", data);
    struct snag_buf canonical = {.max = SNAG_MAX_EVENT_LINE};
    assert(wrapped && !snag_json_canonical(wrapped, &canonical));
    size = SNAG_MAX_EVENT_LINE - canonical.len;
    text = malloc(size + 1u);
    assert(text);
    memset(text, 't', size + 1u);
    assert(!json_object_set_new(result, "model_text", json_stringn(text, size)));
    roundtrip_checked("tool_finished", data, 177u, false);
    assert(!json_object_set_new(result, "model_text", json_stringn(text, size + 1u)));
    reject_json("tool_finished", data);
    free(text);
    snag_buf_free(&canonical);
    json_decref(wrapped);
    json_decref(data);
}

static void
tool_result_projection(void)
{
    json_t *data = json_loads(samples[74].data, 0, NULL);
    json_t *result = json_object_get(data, "result");
    assert(!json_object_set_new(result, "status", json_string("io_failed")));
    json_t *value = json_null();
    for (size_t depth = 0u; depth <= 46u; ++depth) {
        assert(!json_object_set(result, "exit_code", value));
        if (depth <= 45u) roundtrip_checked("tool_finished", data, 177u, false);
        else reject_json("tool_finished", data);
        json_t *array = json_array();
        assert(array && !json_array_append_new(array, value));
        value = array;
    }
    json_decref(value);
    assert(!json_object_set_new(result, "exit_code", json_null()));
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    enum snag_binary_kind kind;
    assert(!snag_binary_legacy_encode(&bytes, "tool_finished", data, &kind));
    struct snag_binary_record record = {
        .kind = (uint16_t)kind, .version = 1u, .payload = bytes.data, .size = bytes.len
    };
    struct snag_binary_event event;
    assert(!snag_binary_event_decode(&record, &event));
    size_t count = SNAG_MAX_EVENT_LINE / 6u + 1u;
    unsigned char *text = malloc(count);
    assert(text);
    memset(text, 1, count);
    event.data.tool_finished.result.model_text = (struct snag_binary_text){text, count};
    struct snag_buf native = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&native, &event));
    record.payload = native.data;
    record.size = native.len;
    /* Legal native text can expand beyond the complete legacy canonical budget. */
    reject_record(&record);
    free(text);
    snag_buf_free(&native);
    snag_buf_free(&bytes);
    json_decref(data);
}

static void
irc_event_variants(void)
{
    for (unsigned int kind = 0u; kind <= SNAG_IRC_HISTORY_READY; ++kind) {
        for (unsigned int watermark = 0u; watermark < 5u; ++watermark) {
            for (unsigned int classified = 0u; classified < 5u; ++classified) {
                for (unsigned int flags = 0u; flags < 8u; ++flags) {
                    json_t *data = json_loads(samples[76].data, 0, NULL);
                    assert(!json_object_set_new(data, "kind",
                        json_string(snag_irc_kind_name((enum snag_irc_event_kind)kind))));
                    assert(!json_object_set_new(data, "historical", json_boolean(flags & 1u)));
                    assert(!json_object_set_new(data, "local", json_boolean(flags & 2u)));
                    assert(!json_object_set_new(data, "op", json_boolean(flags & 4u)));
                    if (watermark) {
                        assert(!json_object_set_new(data, "stream",
                            json_string(watermark > 2u ? ID : "")));
                        assert(!json_object_set_new(data, "sequence",
                            json_integer(watermark > 2u ? INT64_MAX : 0)));
                        assert(!json_object_set_new(data, "input",
                            json_boolean(!(watermark & 1u))));
                    }
                    if (classified) {
                        assert(!json_object_set_new(data, "urgent",
                            json_boolean((classified - 1u) & 1u)));
                        assert(!json_object_set_new(data, "reply",
                            json_boolean((classified - 1u) & 2u)));
                    }
                    struct snag_irc_event source;
                    assert(!snag_irc_event_read(data, &source));
                    roundtrip_checked("irc_event", data, 208u, false);
                    json_decref(data);
                }
            }
        }
    }
    json_t *data = json_loads(samples[77].data, 0, NULL);
    const char *flags[] = {"historical", "local", "op", "input", "urgent", "reply"};
    for (size_t i = 0u; i < 6u; ++i) {
        json_t *copy = json_copy(data);
        assert(!json_object_set_new(copy, flags[i], json_integer(1)));
        reject_json("irc_event", copy);
        json_decref(copy);
    }
    for (unsigned int mask = 0u; mask < 8u; ++mask) {
        json_t *copy = json_copy(data);
        const char *fields[] = {"stream", "sequence", "input"};
        for (unsigned int i = 0u; i < 3u; ++i) {
            if (mask & (1u << i)) assert(!json_object_del(copy, fields[i]));
        }
        if (mask == 0u || mask == 7u) roundtrip("irc_event", copy, 208u);
        else reject_json("irc_event", copy);
        json_decref(copy);
    }
    const char *streams[] = {"", "bad", "0123456789abcdef001122334455667G",
        "00000000000000000000000000000000"};
    for (size_t i = 0u; i < 4u; ++i) {
        assert(!json_object_set_new(data, "stream", json_string(streams[i])));
        if (i == 3u) roundtrip("irc_event", data, 208u);
        else reject_json("irc_event", data);
    }
    assert(!json_object_set_new(data, "stream", json_string(ID)));
    const json_int_t times[] = {-1, 0, 1, INT64_MAX};
    for (size_t i = 0u; i < 4u; ++i) {
        assert(!json_object_set_new(data, "timestamp_ms", json_integer(times[i])));
        assert(!json_object_set_new(data, "sequence", json_integer(times[i])));
        if (times[i] > 0) roundtrip("irc_event", data, 208u);
        else reject_json("irc_event", data);
    }
    assert(!json_object_set_new(data, "kind", json_string("unknown")));
    reject_json("irc_event", data);
    json_decref(data);
}

static void
irc_text_bounds(void)
{
    const char *keys[] = {"endpoint", "room", "nick", "text"};
    const size_t limits[] = {SNAG_CONFIG_IRC_ENDPOINT_MAX, SNAG_CONFIG_IRC_ROOM_MAX + 1u,
        SNAG_CONFIG_IRC_NICK_MAX, SNAG_IRC_TEXT_MAX};
    json_t *data = json_loads(samples[76].data, 0, NULL);
    for (size_t k = 0u; k < 4u; ++k) {
        char *text = malloc(limits[k] + 1u);
        assert(text);
        memset(text, 't', limits[k] + 1u);
        for (size_t n = limits[k]; n <= limits[k] + 1u; ++n) {
            assert(!json_object_set_new(data, keys[k], json_stringn(text, n)));
            if (n == limits[k]) {
                roundtrip_checked("irc_event", data, 208u, false);
            } else {
                reject_json("irc_event", data);
            }
        }
        free(text);
        assert(!json_object_set_new(data, keys[k], json_string("")));
        if (k) roundtrip("irc_event", data, 208u);
        else reject_json("irc_event", data);
        for (unsigned int c = 1u; c <= 127u; ++c) {
            char byte = (char)c;
            assert(!json_object_set_new(data, keys[k], json_stringn(&byte, 1u)));
            if (c < 32u || c == 127u) reject_json("irc_event", data);
            else roundtrip_checked("irc_event", data, 208u, false);
        }
        assert(!json_object_set_new(data, keys[k], json_string("\xc2\x85")));
        roundtrip("irc_event", data, 208u);
        assert(!json_object_set_new(data, keys[k], json_string("t")));
    }
    json_decref(data);
    data = json_loads(samples[78].data, 0, NULL);
    const char *reasons[] = {"join", "nick", "topology", "compaction", "unknown"};
    for (size_t i = 0u; i < 5u; ++i) {
        assert(!json_object_set_new(data, "reason", json_string(reasons[i])));
        if (i < 4u) roundtrip("irc_snapshot", data, 209u);
        else reject_json("irc_snapshot", data);
    }
    assert(!json_object_set_new(data, "reason", json_string("join")));
    char *text = malloc(SNAG_MAX_IRC_SNAPSHOT + 1u);
    assert(text);
    memset(text, 't', SNAG_MAX_IRC_SNAPSHOT + 1u);
    for (size_t n = SNAG_MAX_IRC_SNAPSHOT; n <= SNAG_MAX_IRC_SNAPSHOT + 1u; ++n) {
        assert(!json_object_set_new(data, "text", json_stringn(text, n)));
        if (n == SNAG_MAX_IRC_SNAPSHOT) {
            roundtrip_checked("irc_snapshot", data, 209u, false);
        } else {
            reject_json("irc_snapshot", data);
        }
    }
    free(text);
    assert(!json_object_set_new(data, "text", json_string("")));
    reject_json("irc_snapshot", data);
    assert(!json_object_set_new(data, "text", json_string("\n")));
    const json_int_t times[] = {-1, 0, 1, INT64_MAX};
    for (size_t i = 0u; i < 4u; ++i) {
        assert(!json_object_set_new(data, "timestamp_ms", json_integer(times[i])));
        if (times[i] > 0) roundtrip("irc_snapshot", data, 209u);
        else reject_json("irc_snapshot", data);
    }
    json_decref(data);
}

static void
irc_admission_variants(void)
{
    const char *bad[] = {"null", "[]", "[0]", "[-1]", "[1,1]", "[2,1]", "[true]",
        "[null]", "[\"1\"]", "{}"};
    json_t *data = json_loads(samples[79].data, 0, NULL);
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(!json_object_set_new(data, "sequences", json_loads(bad[i], JSON_DECODE_ANY, NULL)));
        reject_json("irc_admitted", data);
    }
    json_decref(data);
    for (size_t i = 55u; i <= 56u; ++i) {
        data = json_loads(samples[79].data, 0, NULL);
        json_t *child = json_loads(samples[i].data, 0, NULL);
        const char *key = i == 55u ? "input" : "steering";
        assert(!json_object_set_new(data, key, child));
        roundtrip("irc_admitted", data, 210u);
        struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
        enum snag_binary_kind kind;
        assert(!snag_binary_legacy_encode(&bytes, "irc_admitted", data, &kind));
        struct snag_binary_record record = {
            .kind = (uint16_t)kind, .version = 1u, .payload = bytes.data, .size = bytes.len
        };
        struct snag_binary_event event;
        assert(!snag_binary_event_decode(&record, &event));
        assert(event.data.irc_admitted.input.kind == samples[i].kind);
        assert(event.data.irc_admitted.input.version == 2u);
        for (uint16_t version = 1u; version <= 2u; ++version) {
            event.data.irc_admitted.input.version = version;
            struct snag_buf native = {.max = SNAG_MAX_EVENT_LINE};
            assert(!snag_binary_event_encode(&native, &event));
            record.payload = native.data;
            record.size = native.len;
            const char *type = NULL;
            json_t *projected = NULL;
            assert(!snag_binary_legacy_decode(&record, &type, &projected));
            assert(!strcmp(type, "irc_admitted") && json_equal(data, projected));
            json_decref(projected);
            snag_buf_free(&native);
        }
        struct snag_binary_event base;
        assert(!snag_binary_event_decode(&event.data.irc_admitted.input, &base));
        for (unsigned int field = 1u; field <= (i == 55u ? 3u : 2u); ++field) {
            struct snag_binary_event changed = base;
            struct snag_binary_input_reference ref = {
                .field = (enum snag_binary_input_leaf)field, .target = {1u, 0u, 32u}
            };
            if (i == 55u && field == 1u) {
                changed.data.input.text = (struct snag_binary_text){0};
                changed.data.input.text_ref = ref;
            } else if (i == 55u && field == 2u) {
                changed.data.input.content_ref = ref;
            } else if (i == 55u) {
                changed.data.input.instructions = (struct snag_binary_instructions){0};
                changed.data.input.instructions_ref = ref;
            } else if (field == 1u) {
                changed.data.steering_input.text = (struct snag_binary_text){0};
                changed.data.steering_input.text_ref = ref;
            } else {
                changed.data.steering_input.content_ref = ref;
            }
            struct snag_buf native = {.max = SNAG_MAX_EVENT_LINE};
            assert(!snag_binary_event_encode(&native, &changed));
            struct snag_binary_event wrapped = event;
            wrapped.data.irc_admitted.input.payload = native.data;
            wrapped.data.irc_admitted.input.size = native.len;
            reject_native_projection(&wrapped, ENOTSUP);
            snag_buf_free(&native);
        }
        snag_buf_free(&bytes);
        json_t *sequences = json_array();
        assert(sequences);
        for (size_t n = 1u; n <= 513u; ++n) {
            assert(!json_array_append_new(sequences, json_integer((json_int_t)n)));
        }
        assert(!json_object_set_new(data, "sequences", sequences));
        char *text = malloc(131073u);
        assert(text);
        memset(text, 't', 131073u);
        assert(!json_object_set_new(child, "text", json_stringn(text, 131073u)));
        free(text);
        roundtrip_checked("irc_admitted", data, 210u, false);
        json_t *copy = json_deep_copy(data);
        assert(!json_object_set_new(json_object_get(copy, key), "unknown", json_true()));
        reject_json("irc_admitted", copy);
        json_decref(copy);
        assert(!json_object_set_new(data, key, json_null()));
        reject_json("irc_admitted", data);
        json_decref(data);
    }
    data = json_loads(samples[79].data, 0, NULL);
    assert(!json_object_set_new(data, "input", json_loads(samples[55].data, 0, NULL)));
    assert(!json_object_set_new(data, "steering", json_loads(samples[56].data, 0, NULL)));
    reject_json("irc_admitted", data);
    json_decref(data);
}

static void
transfer_metadata_variants(void)
{
    const char *counters[] = {"source_as_of_seq", "count", "begin_offset", "begin_seq"};
    const json_int_t numbers[] = {-1, 0, 1, 2, INT64_MAX};
    const char *wrong_types[] = {"true", "false", "null", "\"1\"", "1.5", "[]", "{}"};
    const char *ids[] = {"transfer_id", "source_session_id", "target_session_id"};
    for (size_t n = 80u; n <= 81u; ++n) {
        json_t *data = json_loads(samples[n].data, 0, NULL);
        for (size_t k = 0u; k < (n == 80u ? 2u : 4u); ++k) {
            for (size_t v = 0u; v < 5u; ++v) {
                assert(!json_object_set_new(data, counters[k], json_integer(numbers[v])));
                if (numbers[v] >= (k == 3u ? 2 : 1)) {
                    roundtrip(samples[n].type, data, samples[n].kind);
                } else {
                    reject_json(samples[n].type, data);
                }
            }
            for (size_t v = 0u; v < 7u; ++v) {
                assert(!json_object_set_new(data, counters[k],
                    json_loads(wrong_types[v], JSON_DECODE_ANY, NULL)));
                reject_json(samples[n].type, data);
            }
            assert(!json_object_set_new(data, counters[k], json_integer(k == 3u ? 2 : 1)));
        }
        const char *bad_ids[] = {"", "bad", "0123456789abcdef001122334455667G"};
        for (size_t k = 0u; k < 3u; ++k) {
            for (size_t v = 0u; v < 3u; ++v) {
                assert(!json_object_set_new(data, ids[k], json_string(bad_ids[v])));
                reject_json(samples[n].type, data);
            }
            assert(!json_object_set_new(data, ids[k], json_string(ID)));
            roundtrip(samples[n].type, data, samples[n].kind);
        }
        if (n == 81u) {
            const char *hashes[] = {"", "bad",
                "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", HASH0};
            for (size_t h = 0u; h < 4u; ++h) {
                assert(!json_object_set_new(data, "begin_sha256", json_string(hashes[h])));
                if (h == 3u) roundtrip(samples[n].type, data, samples[n].kind);
                else reject_json(samples[n].type, data);
            }
        }
        json_decref(data);
    }
}

static json_t *
archive_wrapper(const char *type, json_t *source)
{
    json_t *data = json_pack("{s:s,s:s,s:s,s:i,s:s,s:O}", "transfer_id", ID,
        "target_session_id", NEW_ID, "source_session_id", ID, "source_seq", 1,
        "source_type", type, "data", source);
    assert(data);
    return data;
}

static void
archive_variants(void)
{
    json_t *values = json_loads("[null,false,true,-9223372036854775808,\"redacted\","
        "[1,null],{\"k\":false}]", 0u, NULL);
    assert(values);
    for (size_t n = 0u; n < sizeof(samples) / sizeof(samples[0]); ++n) {
        if (samples[n].kind == 264u || samples[n].kind == 265u) continue;
        json_t *source = json_loads(samples[n].data, 0u, NULL);
        json_t *data = archive_wrapper(samples[n].type, source);
        roundtrip("voice_transfer_record", data, 264u);
        /* Every named field in the executable sample has a public slot. */
        for (void *iter = json_object_iter(source); iter;
            iter = json_object_iter_next(source, iter)) {
            const char *key = json_object_iter_key(iter);
            size_t i = 0u;
            const char *name;
            while ((name = snag_binary_archive_field_name((uint16_t)samples[n].kind, i))) {
                if (!strcmp(key, name)) break;
                ++i;
            }
            assert(name);
        }
        json_object_clear(source);
        roundtrip("voice_transfer_record", data, 264u);
        for (size_t v = 0u; v < json_array_size(values); ++v) {
            json_t *value = json_array_get(values, v);
            for (size_t i = 0u; ; ++i) {
                const char *name = snag_binary_archive_field_name((uint16_t)samples[n].kind, i);
                if (!name) break;
                assert(i < 64u && !json_object_set(source, name, value));
                roundtrip_checked("voice_transfer_record", data, 264u, false);
            }
            assert(!json_object_set(source, "", value));
            assert(!json_object_set(source, "unassigned_extension", value));
            roundtrip_checked("voice_transfer_record", data, 264u, false);
            struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
            enum snag_binary_kind kind;
            assert(!snag_binary_legacy_encode(&bytes, "voice_transfer_record", data, &kind));
            struct snag_binary_record record = {.kind = 264u, .version = 1u,
                .payload = bytes.data, .size = bytes.len};
            struct snag_binary_event decoded;
            assert(!snag_binary_event_decode(&record, &decoded));
            assert(decoded.data.voice_transfer_record.source_kind == samples[n].kind);
            assert(decoded.data.voice_transfer_record.source_version == 0x8001u);
            snag_buf_free(&bytes);
            json_object_clear(source);
        }
        json_decref(data);
        json_decref(source);
    }
    json_t *source = json_object();
    json_t *data = archive_wrapper("unknown\001", source);
    assert(source);
    const char *allowed[] = {"unknown\001", "voice_transfer_record ", "voice_transfer_sealed!",
        "session_checkpoint", "response_completed", "compaction_completed"};
    for (size_t i = 0u; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        assert(!json_object_set_new(data, "source_type", json_string(allowed[i])));
        assert(!json_object_set(source, "provider_payload_omitted", json_true()));
        roundtrip("voice_transfer_record", data, 264u);
    }
    const char *forbidden[] = {"", "voice_transfer_record", "voice_transfer_sealed"};
    for (size_t i = 0u; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        assert(!json_object_set_new(data, "source_type", json_string(forbidden[i])));
        reject_json("voice_transfer_record", data);
    }
    assert(!json_object_set_new(data, "source_type", json_string("response_completed")));
    const char *outer_ids[] = {"transfer_id", "target_session_id", "source_session_id"};
    for (size_t i = 0u; i < 3u; ++i) {
        assert(!json_object_set_new(data, outer_ids[i], json_string("redacted")));
        reject_json("voice_transfer_record", data);
        assert(!json_object_set_new(data, outer_ids[i], json_string(ID)));
    }
    const json_int_t seqs[] = {0, -1, 1, INT64_MAX};
    for (size_t i = 0u; i < 4u; ++i) {
        assert(!json_object_set_new(data, "source_seq", json_integer(seqs[i])));
        if (seqs[i] > 0) roundtrip("voice_transfer_record", data, 264u);
        else reject_json("voice_transfer_record", data);
    }
    for (size_t v = 0u; v < json_array_size(values); ++v) {
        assert(!json_object_set(data, "source_type", json_array_get(values, v)));
        if (json_is_string(json_array_get(values, v))) {
            roundtrip("voice_transfer_record", data, 264u);
        } else {
            reject_json("voice_transfer_record", data);
        }
        assert(!json_object_set_new(data, "source_type", json_string("response_completed")));
        assert(!json_object_set(data, "data", json_array_get(values, v)));
        if (json_is_object(json_array_get(values, v))) {
            roundtrip("voice_transfer_record", data, 264u);
        } else {
            reject_json("voice_transfer_record", data);
        }
        assert(!json_object_set(data, "data", source));
    }
    json_t *nested = json_null();
    for (size_t i = 0u; i < 45u; ++i) {
        json_t *array = json_array();
        assert(array && !json_array_append_new(array, nested));
        nested = array;
    }
    const char *keys[] = {"items", "extension"};
    for (size_t i = 0u; i < 2u; ++i) {
        json_object_clear(source);
        assert(!json_object_set(source, keys[i], nested));
        roundtrip("voice_transfer_record", data, 264u);
        json_t *deeper = json_array();
        assert(deeper && !json_array_append(deeper, nested));
        assert(!json_object_set_new(source, keys[i], deeper));
        reject_json("voice_transfer_record", data);
    }
    json_decref(nested);
    json_decref(data);
    json_decref(source);
    json_decref(values);
}

static void
archive_bounds(void)
{
    char *text = malloc(SNAG_MAX_EVENT_LINE + 1u);
    assert(text);
    memset(text, 'x', SNAG_MAX_EVENT_LINE + 1u);
    for (size_t i = 0u; i < 3u; ++i) {
        json_t *source = json_object();
        json_t *data = archive_wrapper(i ? "response_completed" : "", source);
        const char *key = i == 1u ? "items" : "extension";
        json_t *target = i ? source : data;
        if (!i) key = "source_type";
        assert(!json_object_set_new(target, key, json_string("")));
        json_t *wrapper = json_pack("{s:O}", "data", data);
        struct snag_buf canonical = {.max = SNAG_MAX_EVENT_LINE};
        assert(wrapper && !snag_json_canonical(wrapper, &canonical));
        size_t room = SNAG_MAX_EVENT_LINE - canonical.len;
        assert(!json_object_set_new(target, key, json_stringn(text, room)));
        assert(!snag_json_canonical(wrapper, &canonical));
        assert(canonical.len == SNAG_MAX_EVENT_LINE);
        roundtrip_checked("voice_transfer_record", data, 264u, false);
        assert(!json_object_set_new(target, key, json_stringn(text, room + 1u)));
        reject_json("voice_transfer_record", data);
        json_decref(wrapper);
        json_decref(data);
        json_decref(source);
        snag_buf_free(&canonical);
    }
    /* Two individually representable native fields can exceed the complete
     * legacy envelope's escaped-text budget. Projection must fail atomically. */
    size_t length = 1536u * 1024u;
    memset(text, 1, length);
    json_t *literal = json_stringn(text, length);
    json_t *source = json_pack("{s:O,s:O}", "items", literal, "usage", literal);
    assert(literal && source);
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    uint16_t kind;
    assert(!snag_binary_archive_fields_encode(&bytes, "response_completed", source, &kind));
    struct snag_binary_event event = {.kind = SNAG_BINARY_VOICE_TRANSFER_RECORD};
    event.data.voice_transfer_record = (struct snag_binary_voice_archive){.source_seq = 1u,
        .source_kind = kind, .source_version = SNAG_BINARY_ARCHIVE_PUBLIC_VERSION,
        .data = bytes.data, .size = bytes.len};
    reject_native_projection(&event, 0);
    snag_buf_free(&bytes);
    json_decref(literal);
    json_decref(source);
    free(text);
}

static void
unsupported_and_null(void)
{
    json_t *data = json_object();
    assert(data);
    reject_json("voice_transfer_record", data);
    assert(errno == EINVAL);
    reject_json("not_an_event", data);
    reject_json(NULL, data);
    reject_json("goal_started", NULL);
    reject_json("goal_started", json_null());
    reject_record(NULL);
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    enum snag_binary_kind kind = SNAG_BINARY_GOAL_STARTED;
    assert(snag_binary_legacy_encode(NULL, "goal_started", data, &kind) < 0);
    assert(snag_binary_legacy_encode(&bytes, "goal_started", data, NULL) < 0);
    unsigned char archive[64] = {0};
    archive[48] = 1u;
    archive[56] = 97u;
    archive[58] = 2u;
    struct snag_binary_record record = {
        .kind = 264u, .version = 1u, .payload = archive, .size = sizeof(archive)
    };
    struct snag_binary_event event;
    assert(!snag_binary_event_decode(&record, &event));
    const char *archive_type = NULL;
    json_t *projected = NULL;
    assert(!snag_binary_legacy_decode(&record, &archive_type, &projected));
    assert(!strcmp(archive_type, "voice_transfer_record"));
    assert(!strcmp(snag_json_string(projected, "source_type"), "input_cancelled"));
    assert(json_is_object(json_object_get(projected, "data")) &&
        !json_object_size(json_object_get(projected, "data")));
    json_decref(projected);
    record = (struct snag_binary_record){.kind = 0x8000u, .version = 1u, .flags = 1u};
    reject_record(&record);
    assert(errno == ENOTSUP);
    const char *type = "keep";
    assert(snag_binary_legacy_decode(&record, NULL, &data) < 0);
    assert(snag_binary_legacy_decode(&record, &type, NULL) < 0);
    assert(!strcmp(type, "keep") && json_is_object(data));
    json_decref(data);
    snag_buf_free(&bytes);
}

void
test_store_binary_legacy(void)
{
    samples_and_shapes();
    archive_variants();
    archive_bounds();
    wire_bytes();
    variants_and_errors();
    bounds_and_atomicity();
    creation_and_key_errors();
    metadata_variants();
    session_metadata_variants();
    session_metadata_wire();
    hosted_search_variants();
    hosted_search_wire();
    rule_voice_variants();
    rule_storage_and_aggregate();
    control_variants();
    download_variants();
    context_variants();
    compact_variants();
    compact_output_variants();
    turn_outcome_variants();
    input_control_variants();
    input_receipt_variants();
    input_receipt_storage();
    input_receipt_projection();
    queued_input_variants();
    queued_voice_variants();
    queued_input_projection();
    turn_config_variants();
    turn_start_variants();
    turn_start_storage();
    turn_start_projection();
    response_start_variants();
    response_start_host();
    response_start_projection();
    response_public_variants();
    response_failure_variants();
    response_public_storage();
    response_public_projection();
    response_complete_usage();
    response_complete_graph();
    response_complete_continuation();
    response_complete_storage();
    response_complete_projection();
    tool_process_variants();
    tool_process_storage();
    tool_result_domain();
    tool_result_options();
    tool_result_excerpts();
    tool_result_storage();
    tool_result_projection();
    irc_event_variants();
    irc_text_bounds();
    irc_admission_variants();
    transfer_metadata_variants();
    unsupported_and_null();
}
