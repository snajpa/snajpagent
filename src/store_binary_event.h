/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_EVENT_H
#define SNAJPAGENT_STORE_BINARY_EVENT_H

#include "irc.h"
#include "store_binary.h"
#include "turn.h"

/* Stable draft record IDs. Use 1..31 for session metadata and 0x8000..ffff
 * for explicitly optional metadata. Required semantic kinds stay below 0x8000.
 * Extend these concrete event families; do not reuse an assigned number. */
enum snag_binary_kind {
    SNAG_BINARY_SESSION_CREATED = 1,
    SNAG_BINARY_CWD_CHANGED = 2,
    /* Retired metadata; preserved for reading older records. */
    SNAG_BINARY_SESSION_ARCHIVED = 3,
    SNAG_BINARY_SESSION_UNARCHIVED = 4,
    SNAG_BINARY_SESSION_DELETE_REQUESTED = 5,
    SNAG_BINARY_BANNER_UPDATED = 6,
    SNAG_BINARY_STEERING_UPDATED = 7,
    SNAG_BINARY_MODEL_SELECTED = 8,
    SNAG_BINARY_TURN_MODEL_CHANGED = 9,
    SNAG_BINARY_EFFORT_CHANGED = 10,
    SNAG_BINARY_CONTEXT_SELECTION_CHANGED = 11,
    SNAG_BINARY_COMMAND_SHELL_CHANGED = 12,
    SNAG_BINARY_SESSION_NAMED = 13,
    SNAG_BINARY_SESSION_OPTIONS = 14,
    SNAG_BINARY_SERVICE_TIER_CHANGED = 15,
    SNAG_BINARY_CONTROL_REQUESTED = 16,
    SNAG_BINARY_CONTROL_STARTED = 17,
    SNAG_BINARY_CONTROL_FINISHED = 18,
    SNAG_BINARY_TIMER_SCHEDULED = 32,
    SNAG_BINARY_TIMER_FIRED = 33,
    SNAG_BINARY_TIMER_CANCELLED = 34,
    SNAG_BINARY_GOAL_STARTED = 64,
    SNAG_BINARY_GOAL_REPLACED = 65,
    SNAG_BINARY_GOAL_REWORDED = 66,
    SNAG_BINARY_GOAL_LOCK_CHANGED = 67,
    SNAG_BINARY_GOAL_PAUSED = 68,
    SNAG_BINARY_GOAL_BLOCKED = 69,
    SNAG_BINARY_GOAL_COMPLETED = 70,
    SNAG_BINARY_GOAL_RESUMED = 71,
    SNAG_BINARY_GOAL_CANCELLED = 72,
    SNAG_BINARY_GOAL_BLOCKED_WAIT_FOR = 73,
    SNAG_BINARY_INPUT_RECEIVED = 96,
    SNAG_BINARY_INPUT_CANCELLED = 97,
    SNAG_BINARY_STEERING_ADDED = 98,
    SNAG_BINARY_IRC_REPLY_REMINDER = 99,
    SNAG_BINARY_STEERING_DEFERRED = 100,
    SNAG_BINARY_INPUT_ADMITTED = 101,
    SNAG_BINARY_FUTURE_QUEUE_STATE = 102,
    SNAG_BINARY_FUTURE_TURN_CANCELLED = 103,
    SNAG_BINARY_FUTURE_TURN_QUEUED = 104,
    SNAG_BINARY_FUTURE_TURN_EDITED = 105,
    SNAG_BINARY_TURN_STARTED = 128,
    SNAG_BINARY_TURN_YIELD_REQUESTED = 129,
    SNAG_BINARY_TURN_CANCEL_REQUESTED = 130,
    SNAG_BINARY_TURN_RECOVERY = 131,
    SNAG_BINARY_TURN_COMPLETED = 132,
    SNAG_BINARY_TURN_COMPLETED_SILENT = 133,
    SNAG_BINARY_TURN_INTERRUPTED = 134,
    SNAG_BINARY_TURN_FAILED = 135,
    SNAG_BINARY_RESPONSE_STARTED = 160,
    SNAG_BINARY_RESPONSE_OUTPUT = 161,
    SNAG_BINARY_RESPONSE_INTERRUPTED = 162,
    SNAG_BINARY_RESPONSE_FAILED = 163,
    SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION = 164,
    SNAG_BINARY_RESPONSE_COMPLETED = 165,
    SNAG_BINARY_RESPONSE_CAPACITY_REJECTED = 166,
    SNAG_BINARY_HOSTED_SEARCH_STARTED = 167,
    SNAG_BINARY_HOSTED_SEARCH_FINISHED = 168,
    SNAG_BINARY_TOOL_STARTED = 176,
    SNAG_BINARY_TOOL_FINISHED = 177,
    SNAG_BINARY_PROCESS_OUTPUT = 192,
    SNAG_BINARY_PROCESS_CLOSED = 193,
    SNAG_BINARY_IRC_EVENT = 208,
    SNAG_BINARY_IRC_SNAPSHOT = 209,
    SNAG_BINARY_IRC_ADMITTED = 210,
    SNAG_BINARY_IRC_SLEEP_SET = 211,
    SNAG_BINARY_IRC_SLEEP_WOKE = 212,
    SNAG_BINARY_IRC_COMPACT_CONFIGURED = 213,
    SNAG_BINARY_IRC_COMPACTED = 214,
    SNAG_BINARY_IRC_EVENT_V2 = 215,
    SNAG_BINARY_COMPACTION_STARTED = 224,
    SNAG_BINARY_COMPACTION_INTERRUPTED = 225,
    SNAG_BINARY_COMPACTION_COMPLETED = 226,
    SNAG_BINARY_CONTEXT_REBASED = 227,
    SNAG_BINARY_DOWNLOAD_QUEUED = 240,
    SNAG_BINARY_DOWNLOAD_REMOVED = 241,
    SNAG_BINARY_DOWNLOADS_CLEARED = 242,
    SNAG_BINARY_RULE_LOG = 248,
    SNAG_BINARY_RULE_TRANSFORM = 249,
    SNAG_BINARY_AUDIO_USAGE = 256,
    SNAG_BINARY_VOICE_EVENT = 257,
    SNAG_BINARY_VOICE_TRANSFER_RECORD = 264,
    SNAG_BINARY_VOICE_TRANSFER_SEALED = 265,
    SNAG_BINARY_VOICE_TRANSFER_ADOPTED = 266
};

enum snag_binary_actor { SNAG_BINARY_USER = 1, SNAG_BINARY_MODEL = 2 };
enum snag_binary_protocol { SNAG_BINARY_RESPONSES = 1 };
enum snag_binary_steering {
    SNAG_BINARY_STEERING_DEFAULT = 0,
    SNAG_BINARY_STEERING_MENTIONS = 1,
    SNAG_BINARY_STEERING_ALL = 2
};
enum snag_binary_context {
    SNAG_BINARY_CONTEXT_DEFAULT = 0,
    SNAG_BINARY_CONTEXT_MAX = 1,
    SNAG_BINARY_CONTEXT_TOKENS = 2
};
enum snag_binary_pause {
    SNAG_BINARY_PAUSE_INPUT_CLOSED = 1,
    SNAG_BINARY_PAUSE_PROVIDER_POLICY = 2,
    SNAG_BINARY_PAUSE_REFUSAL = 3,
    SNAG_BINARY_PAUSE_SESSION_RESUMED = 4,
    SNAG_BINARY_PAUSE_TURN_STOPPED = 5,
    SNAG_BINARY_PAUSE_USER = 6
};

struct snag_binary_text {
    const unsigned char *data;
    size_t size;
};

struct snag_binary_asset {
    unsigned char id[16], sha256[32];
    uint64_t bytes;
    struct snag_binary_text mime;
};

enum snag_binary_part_kind {
    SNAG_BINARY_PART_TEXT = 1, SNAG_BINARY_PART_FILE = 2, SNAG_BINARY_PART_IMAGE = 3
};

struct snag_binary_part {
    enum snag_binary_part_kind kind;
    struct snag_binary_text text; /* Text part or image source note. */
    struct snag_binary_asset asset, source;
    bool has_source;
};

struct snag_binary_instruction {
    /* Bounded NUL-terminated UTF-8 on wire. */
    struct snag_binary_text path;
    bool has_snapshot; /* Preserve workspace-era metadata, not inferred file contents. */
    uint64_t bytes;
    unsigned char sha256[32];
};

/* Validated, borrowed typed-list encodings, not opaque provider JSON. Iteration
 * starts with offset zero and uses only returned offsets. Bytes stay immutable.
 * Encoders append atomically; decode/next preserve output on failure or EOF.
 * next returns 0 for an item, 1 for EOF, -1 on error. Domain checks (path/MIME
 * policy, duplicate paths, image totals, actual assets) remain reducer work. */
struct snag_binary_content { const unsigned char *data; size_t size; };
struct snag_binary_instructions { const unsigned char *data; size_t size; };
/* Ordered stored arguments: u32 count and bounded NUL-terminated UTF-8 strings.
 * The same borrowed-view/atomic-output rules apply as for the lists below. */
struct snag_binary_options { const unsigned char *data; size_t size; };
int snag_binary_options_encode(struct snag_buf *out, const json_t *args);
int snag_binary_options_decode(const void *data, size_t size, struct snag_binary_options *out);
int snag_binary_options_next(const struct snag_binary_options *options, size_t *offset,
    struct snag_binary_text *out);
int snag_binary_content_encode(struct snag_buf *out, const struct snag_binary_part *parts,
    size_t count);
int snag_binary_content_decode(const void *data, size_t size, struct snag_binary_content *out);
int snag_binary_content_next(const struct snag_binary_content *content, size_t *offset,
    struct snag_binary_part *out);
int snag_binary_instructions_encode(struct snag_buf *out,
    const struct snag_binary_instruction *instructions, size_t count);
int snag_binary_instructions_decode(const void *data, size_t size,
    struct snag_binary_instructions *out);
int snag_binary_instructions_next(const struct snag_binary_instructions *instructions,
    size_t *offset, struct snag_binary_instruction *out);

enum snag_binary_input_origin { SNAG_BINARY_INPUT_DEFAULT = 0, SNAG_BINARY_INPUT_TIMER = 1 };
struct snag_binary_ids { const unsigned char (*values)[16]; size_t count; };

enum snag_binary_input_leaf {
    SNAG_BINARY_INPUT_TEXT = 1,
    SNAG_BINARY_INPUT_CONTENT = 2,
    SNAG_BINARY_INPUT_INSTRUCTIONS = 3,
    SNAG_BINARY_INPUT_VOICE_TRANSCRIPT = 4,
    SNAG_BINARY_INPUT_VOICE_REQUEST = 5
};

/* All-zero means the adjacent literal is used. A reference names a canonical
 * source field, with the adjacent literal empty. Decode preserves the reference;
 * callers resolve it before semantic adoption, retaining its original identity. */
struct snag_binary_input_reference {
    enum snag_binary_input_leaf field;
    struct snag_binary_ref target;
};

enum snag_binary_while_turn {
    SNAG_BINARY_WHILE_EMPTY = 0, SNAG_BINARY_WHILE_ID = 1, SNAG_BINARY_WHILE_NULL = 2
};

/* Voice provenance is not speaker authentication or additional authority. */
struct snag_binary_voice_source {
    unsigned char connection_id[16];
    struct snag_binary_text input_id, response_id, call_id;
    struct snag_binary_text provider, model, transcript, request;
    struct snag_binary_input_reference transcript_ref, request_ref;
};

struct snag_binary_selection {
    struct snag_binary_text provider, model, effort;
};

struct snag_binary_context_choice {
    enum snag_binary_context mode;
    uint64_t tokens;
};

struct snag_binary_control {
    unsigned int control; /* One existing SNAG_CONTROL_* bit. */
    bool image_boundary; /* Only requested COMPACT may carry this origin. */
    uint64_t source_seq; /* Present exactly with image_boundary. */
};

enum snag_binary_rebase_reason {
    SNAG_BINARY_REBASE_GOAL_RECOVERY = 1, SNAG_BINARY_REBASE_TURN_RECOVERY = 2
};

struct snag_binary_context_rebase {
    unsigned char turn[16];
    enum snag_binary_rebase_reason reason;
};

struct snag_binary_download {
    unsigned char id[16], sha256[32];
    uint64_t bytes, mtime, queued_ms;
    /* Preserve source-platform spelling; admission/filesystem rules are external. */
    struct snag_binary_text path, name;
};

struct snag_binary_capacity_rejection {
    unsigned char turn[16], response[16];
    uint32_t cycle;
    unsigned char provider_source_sha256[32], request_sha256[32];
    uint64_t context_limit, requested_input, observed_input; /* Zero represents null. */
    struct snag_binary_text message;
};

enum snag_binary_turn_origin {
    SNAG_BINARY_TURN_DIRECT = 0,
    SNAG_BINARY_TURN_QUEUED = 1,
    SNAG_BINARY_TURN_GOAL = 2,
    SNAG_BINARY_TURN_TIMER = 3
};

enum snag_binary_quiet_reason {
    SNAG_BINARY_QUIET_ROOM_UPDATE = 1, SNAG_BINARY_QUIET_REPLY_EXHAUSTED = 2
};

enum snag_binary_interrupt_origin {
    SNAG_BINARY_INTERRUPT_USER = 1,
    SNAG_BINARY_INTERRUPT_RECOVERY = 2,
    SNAG_BINARY_INTERRUPT_OUTPUT = 3,
    SNAG_BINARY_INTERRUPT_STEERING = 4
};

enum snag_binary_interrupt_reason {
    SNAG_BINARY_INTERRUPT_CANCELLED = 1,
    SNAG_BINARY_INTERRUPT_PROCESS_LOST = 2,
    SNAG_BINARY_INTERRUPT_OUTPUT_LOST = 3,
    SNAG_BINARY_INTERRUPT_SESSION_RECOVERED = 4,
    SNAG_BINARY_INTERRUPT_STEERED = 5,
    SNAG_BINARY_INTERRUPT_CONTROL = 6
};

enum snag_binary_failure_class {
    SNAG_BINARY_FAILURE_CONTEXT = 1,
    SNAG_BINARY_FAILURE_PROVIDER = 2,
    SNAG_BINARY_FAILURE_PROTOCOL = 3,
    SNAG_BINARY_FAILURE_TOOL = 4,
    SNAG_BINARY_FAILURE_PERSISTENCE = 5,
    SNAG_BINARY_FAILURE_RESOURCE = 6,
    SNAG_BINARY_FAILURE_OUTPUT = 7,
    SNAG_BINARY_FAILURE_INTERNAL = 8
};

/* Native values preserve historically unconstrained individual fields, never
 * whole host records. Result/config fields use depth45 through data/object/field;
 * rule fields and extension objects use depth46 through data/field. Decode
 * borrows immutable bytes without allocating; json reconstructs an owned value.
 * Encoders append atomically and all failures preserve caller outputs. */
enum snag_binary_result_value_kind {
    SNAG_BINARY_RESULT_NULL = 0, SNAG_BINARY_RESULT_FALSE = 1,
    SNAG_BINARY_RESULT_TRUE = 2, SNAG_BINARY_RESULT_INTEGER = 3,
    SNAG_BINARY_RESULT_STRING = 4, SNAG_BINARY_RESULT_ARRAY = 5,
    SNAG_BINARY_RESULT_OBJECT = 6
};

struct snag_binary_result_value {
    const unsigned char *data;
    size_t size;
    size_t canonical_size;
    enum snag_binary_result_value_kind kind;
    int64_t integer;
};

/* Display-only provider observations. Detail is an optional action object on
 * start, or an optional string array of sources on finish. */
struct snag_binary_hosted_search {
    unsigned char turn[16];
    struct snag_binary_text item_id, status;
    struct snag_binary_result_value detail;
    bool has_detail;
};

int snag_binary_result_value_encode(struct snag_buf *out, const json_t *value);
int snag_binary_result_value_decode(const void *data, size_t size,
    struct snag_binary_result_value *out);
int snag_binary_result_value_json(const struct snag_binary_result_value *value, json_t **out);
int snag_binary_rule_value_encode(struct snag_buf *out, const json_t *value);
int snag_binary_rule_value_decode(const void *data, size_t size,
    struct snag_binary_result_value *out);
int snag_binary_rule_value_json(const struct snag_binary_result_value *value, json_t **out);

/* Fixed schema positions. Numeric presence uses bits0..7; the six historically
 * unconstrained named values use bits8..13. Preserve absence for legacy defaults. */
enum snag_binary_turn_config_number {
    SNAG_BINARY_TURN_MAX_PARALLEL = 0,
    SNAG_BINARY_TURN_DEFAULT_YIELD = 1,
    SNAG_BINARY_TURN_MAX_WAIT = 2,
    SNAG_BINARY_TURN_DEFAULT_TIMEOUT = 3,
    SNAG_BINARY_TURN_MAX_TIMEOUT = 4,
    SNAG_BINARY_TURN_TOOL_OUTPUT = 5,
    SNAG_BINARY_TURN_OUTPUT_CACHE = 6,
    SNAG_BINARY_TURN_MAX_RETRIES = 7,
    SNAG_BINARY_TURN_NUMBER_COUNT = 8
};

enum snag_binary_turn_config_value {
    SNAG_BINARY_TURN_PROMPT_SCHEMA = 0,
    SNAG_BINARY_TURN_REPLAY_SCHEMA = 1,
    SNAG_BINARY_TURN_TOOL_SCHEMA = 2,
    SNAG_BINARY_TURN_CAPABILITY = 3,
    SNAG_BINARY_TURN_PROFILE = 4,
    SNAG_BINARY_TURN_MAX_OUTPUT = 5,
    SNAG_BINARY_TURN_VALUE_COUNT = 6
};

enum snag_binary_turn_config_flag {
    SNAG_BINARY_TURN_PARALLEL_CALLS = 1u << 14
};

struct snag_binary_turn_config {
    struct snag_binary_selection selection;
    uint16_t present;
    uint64_t numbers[SNAG_BINARY_TURN_NUMBER_COUNT];
    struct snag_binary_result_value values[SNAG_BINARY_TURN_VALUE_COUNT];
    /* Rule-depth object; no named setting may appear among these keys. A zero
     * view writes an empty object, identical to an explicit empty object. */
    struct snag_binary_result_value extensions;
    bool parallel_calls;
};

struct snag_binary_turn_start {
    unsigned char id[16];
    unsigned char queue_id[16];
    uint64_t number;
    uint64_t queue_seq;
    uint64_t received_ms;
    enum snag_binary_turn_origin origin;
    bool has_read_only;
    bool read_only;
    bool has_received_ms;
    bool workspace;
    struct snag_binary_text cwd;
    struct snag_binary_text text;
    struct snag_binary_turn_config config;
    struct snag_binary_instructions instructions;
    struct snag_binary_content content;
    struct snag_binary_input_reference text_ref;
    struct snag_binary_input_reference content_ref;
    struct snag_binary_input_reference instructions_ref;
};

enum snag_binary_count_method {
    SNAG_BINARY_COUNT_EXACT = 1,
    SNAG_BINARY_COUNT_MEDIA_UPPER_BOUND = 2,
    SNAG_BINARY_COUNT_UNKNOWN = 3,
    SNAG_BINARY_COUNT_ANCHORED_UPPER_BOUND = 4,
    SNAG_BINARY_COUNT_STATISTICAL_UPPER_ESTIMATE = 5,
    SNAG_BINARY_COUNT_QUALIFIED_UPPER_BOUND = 6
};

enum snag_binary_compact_reason {
    SNAG_BINARY_COMPACT_MANUAL = 1, SNAG_BINARY_COMPACT_PROACTIVE = 2,
    SNAG_BINARY_COMPACT_HARD_BUDGET = 3, SNAG_BINARY_COMPACT_PROVIDER_REJECTION = 4,
    SNAG_BINARY_COMPACT_MODEL_SWITCH = 5, SNAG_BINARY_COMPACT_IMAGE_BOUNDARY = 6,
    SNAG_BINARY_COMPACT_REDUCE = 7
};

enum snag_binary_compact_stop_reason {
    SNAG_BINARY_COMPACT_STOP_STEERING = 1, SNAG_BINARY_COMPACT_STOP_USER = 2,
    SNAG_BINARY_COMPACT_STOP_ENDPOINT_UNAVAILABLE = 3,
    SNAG_BINARY_COMPACT_STOP_CONTEXT_REJECTED = 4, SNAG_BINARY_COMPACT_STOP_ERROR = 5
};

struct snag_binary_compact_start {
    unsigned char id[16], predecessor[16], scope[32];
    bool has_predecessor, has_scope, has_compaction_model;
    enum snag_binary_compact_reason reason;
    enum snag_binary_count_method count_method;
    uint64_t source_seq, input_tokens;
    unsigned char source_sha256[32], request_sha256[32], count_request_sha256[32];
    struct snag_binary_text model, capability, profile, compaction_model;
};

struct snag_binary_compact_interrupt {
    unsigned char id[16];
    enum snag_binary_compact_stop_reason reason;
};

struct snag_binary_compact_complete {
    unsigned char id[16], scope[32];
    bool has_scope;
    enum snag_binary_count_method count_method, output_count_method;
    uint64_t input_tokens, output_tokens;
    unsigned char output_count_request_sha256[32], source_sha256[32], output_sha256[32];
    /* Canonical provider-boundary array; unknown provider extensions remain intact. */
    struct snag_binary_text output;
};

enum snag_binary_capacity_source {
    SNAG_BINARY_CAPACITY_UNKNOWN = 1,
    SNAG_BINARY_CAPACITY_ADVERTISED = 2,
    SNAG_BINARY_CAPACITY_CONFIGURED = 3,
    SNAG_BINARY_CAPACITY_OBSERVED = 4,
    SNAG_BINARY_CAPACITY_STALE_CATALOG_IGNORED = 5
};

struct snag_binary_response_start {
    unsigned char turn[16], response[16], compact[16];
    unsigned char request_sha256[32], count_request_sha256[32], model_input_sha256[32];
    unsigned char baseline_sha256[32], provider_source_sha256[32], request_input_sha256[32];
    uint32_t cycle;
    enum snag_binary_count_method count_method;
    struct snag_binary_selection selection;
    struct snag_binary_text capability, profile;
    struct snag_binary_ids steering;
    uint64_t input_tokens_bound, irc_seq;
    bool full_accounting, has_irc_seq, has_baseline, has_compact;
    enum snag_binary_capacity_source capacity_source;
    bool source_bound, has_hard_input, has_requested_output;
    uint64_t model_input_bytes, request_input_bytes, request_input_count;
    uint64_t hard_input_tokens, requested_output_tokens;
    /* Absent when empty; otherwise at least two nonempty text-only user-role
     * data messages. Boundary text and admission remain reducer checks. */
    struct snag_binary_content host_context;
};

enum snag_binary_item_kind {
    SNAG_BINARY_ITEM_ASSISTANT = 1, SNAG_BINARY_ITEM_REFUSAL = 2, SNAG_BINARY_ITEM_TOOL_CALL = 3
};

enum snag_binary_item_phase {
    SNAG_BINARY_PHASE_COMMENTARY = 1, SNAG_BINARY_PHASE_FINAL_ANSWER = 2
};

struct snag_binary_public_item {
    enum snag_binary_item_kind kind; /* Only assistant/refusal in a public item. */
    enum snag_binary_item_phase phase;
    unsigned char id[16];
    struct snag_binary_text provider_id, text;
};

struct snag_binary_response_output {
    unsigned char turn[16], response[16];
    uint32_t cycle;
    uint64_t index, offset; /* Partial-public index and byte offset, not graph item index. */
    struct snag_binary_public_item item;
};

/* A complete item's original fragments, including intervening non-output
 * records. Fixed-size even when a provider streams one byte per event. */
#define SNAG_BINARY_OUTPUT_SPAN_SIZE 28u
struct snag_binary_output_span {
    struct snag_binary_ref first;
    uint64_t last_sequence;
    uint32_t bytes;
};

struct snag_binary_public_value {
    struct snag_binary_public_item item;
    struct snag_binary_output_span source; /* Mutually exclusive with item.text. */
};

struct snag_binary_public_items {
    const unsigned char *data;
    size_t size;
};

struct snag_binary_response_interruption {
    unsigned char turn[16], response[16];
    uint32_t cycle;
    enum snag_binary_interrupt_origin origin;
    enum snag_binary_interrupt_reason reason;
    struct snag_binary_public_items partial;
};

enum snag_binary_response_failure_field {
    SNAG_BINARY_FAILURE_POLICY_STOPPED = 1u,
    SNAG_BINARY_FAILURE_TURN_RETRIES = 2u,
    SNAG_BINARY_FAILURE_NEW_INPUT = 4u,
    SNAG_BINARY_FAILURE_POLICY = 8u
};

struct snag_binary_response_failure {
    unsigned char turn[16], response[16];
    uint32_t cycle;
    enum snag_binary_failure_class class_name;
    uint8_t retry_count;
    /* Historical field prefixes: 0, 1, 3, 7, 15. */
    uint8_t present;
    bool policy_stopped;
    bool new_input;
    uint64_t turn_retry_attempts;
    struct snag_binary_text message;
    struct snag_binary_text policy_code, policy_type, clarification_skipped;
    struct snag_binary_public_items partial;
};

struct snag_binary_response_correction {
    unsigned char turn[16], response[16], correction[16];
    uint32_t cycle;
    struct snag_binary_text text;
    struct snag_binary_public_items partial;
};

struct snag_binary_call {
    unsigned char id[16];
    struct snag_binary_text provider_id, provider_call_id, name;
    /* Canonical provider-supplied JSON object, not a host-record envelope. */
    struct snag_binary_text arguments;
};

struct snag_binary_graph_item {
    enum snag_binary_item_kind kind;
    union {
        struct snag_binary_public_value output;
        struct snag_binary_call call;
    } data;
};

struct snag_binary_graph_items {
    const unsigned char *data;
    size_t size;
    uint32_t count;
};

struct snag_binary_continuation_item {
    uint64_t before;
    /* Canonical provider reasoning object, retaining provider extensions. */
    struct snag_binary_text item;
};

struct snag_binary_continuation {
    const unsigned char *data;
    size_t size;
};

enum snag_binary_continuation_form {
    SNAG_BINARY_CONTINUATION_ABSENT = 0,
    SNAG_BINARY_CONTINUATION_NULL = 1,
    SNAG_BINARY_CONTINUATION_ITEMS = 2
};

struct snag_binary_response_complete {
    unsigned char turn[16], response[16], continuation_scope[32];
    uint32_t cycle;
    struct snag_binary_text provider_id;
    struct snag_binary_graph_items items;
    struct snag_response_usage usage;
    bool cached_present; /* Preserve an explicitly null cache field. */
    enum snag_binary_continuation_form continuation_form;
    struct snag_binary_continuation continuation;
};

struct snag_binary_graph_source {
    unsigned char turn[16];
    unsigned char response[16];
    uint32_t cycle;
    uint32_t index;
    struct snag_binary_graph_item item;
};

struct snag_binary_continuation_source {
    unsigned char turn[16];
    unsigned char response[16];
    unsigned char scope[32];
    uint32_t cycle;
    struct snag_binary_continuation items;
};

struct snag_binary_rule_log {
    struct snag_binary_result_value chain, message, rule;
};

struct snag_binary_rule_transform {
    unsigned char call[16], original_sha256[32], effective_sha256[32];
    struct snag_binary_text rule;
};

/* Version1 audio usage has the fixed operation "dictation". */
struct snag_binary_audio_usage {
    struct snag_binary_text provider, model, report;
};

enum snag_binary_voice_kind {
    SNAG_BINARY_VOICE_STARTED = 1, SNAG_BINARY_VOICE_STOPPED = 2,
    SNAG_BINARY_VOICE_TRANSCRIPT = 3, SNAG_BINARY_VOICE_USAGE = 4,
    SNAG_BINARY_VOICE_ASR_FAILED = 5, SNAG_BINARY_VOICE_INTERRUPTED = 6,
    SNAG_BINARY_VOICE_RESPONSE = 7, SNAG_BINARY_VOICE_RESULT = 8,
    SNAG_BINARY_VOICE_MUTED = 9
};

/* Named optional fields preserve the historical unconstrained value domain.
 * A zero view is absent; encoded null is present. Values use result-value APIs
 * at depth3 through data/event/field. Changing the v1 field set needs a new
 * version: extensions cannot shadow any of these names, or the type field. */
struct snag_binary_voice_fields {
    struct snag_binary_result_value arguments, call_id, covered_next_seq;
    struct snag_binary_result_value covered_offset, covered_sha256, delay_ms;
    struct snag_binary_result_value disposition, effort, error, http_status;
    struct snag_binary_result_value input, input_id, interrupted, item_id, item_index, kind;
    struct snag_binary_result_value metrics, model, muted, operation, output, output_id;
    struct snag_binary_result_value played_ms, presentation, provider, queue_id, reason;
    struct snag_binary_result_value reply, report, response_id, result, retry_after_ms;
    struct snag_binary_result_value source, source_as_of_seq, source_call_id, source_through_seq;
    struct snag_binary_result_value speaker, status, stream, submitted_id, summary;
    struct snag_binary_result_value target_session_id, text, tool, tool_call_id, turn_id;
};

struct snag_binary_voice_body {
    enum snag_binary_voice_kind kind;
    struct snag_binary_voice_fields fields;
    /* A native object of unknown fields, rooted at depth2. Zero means empty. */
    struct snag_binary_result_value extensions;
};

/* Legacy adapters for the inner observation only. Decode borrows immutable
 * bytes; json returns an owned object. All operations preserve outputs on
 * failure. The codec encodes named fields, never an opaque event object. */
int snag_binary_voice_body_encode(struct snag_buf *out, const json_t *body);
int snag_binary_voice_body_decode(const void *data, size_t size,
    struct snag_binary_voice_body *out);
int snag_binary_voice_body_json(const struct snag_binary_voice_body *body, json_t **out);

struct snag_binary_voice_event {
    unsigned char connection[16];
    struct snag_binary_text provider, model;
    struct snag_binary_voice_body body;
};

struct snag_binary_voice_transfer {
    unsigned char id[16], target[16], source[16];
    uint64_t source_as_of, count;
};

/* A typed source-data snapshot, not an executable event in the target session.
 * Child bytes borrow the enclosing record. References still need archive-domain
 * relocation/validation; decoding grants no source-journal I/O authority. */
struct snag_binary_voice_archive {
    unsigned char id[16], target[16], source[16];
    uint64_t source_seq;
    uint16_t source_kind, source_version;
    const unsigned char *data;
    size_t size;
};

/* Enclosed archive-only profile. It is never an ordinary event payload version.
 * Kind zero carries an unassigned source name; checkpoint is not a journal kind.
 * Field positions and names are frozen by the public version. The 64 slots are
 * the mask's representation, not a limit on unknown extension fields. */
#define SNAG_BINARY_ARCHIVE_PUBLIC_VERSION 0x8001u
#define SNAG_BINARY_ARCHIVE_CHECKPOINT_KIND 0x7fffu
#define SNAG_BINARY_ARCHIVE_FIELDS_MAX 64u
struct snag_binary_archive_fields {
    uint16_t kind; /* Source-kind context supplied with the immutable bytes. */
    const unsigned char *data;
    size_t size;
    struct snag_binary_text type;
    uint64_t present;
    struct snag_binary_result_value values[SNAG_BINARY_ARCHIVE_FIELDS_MAX];
    struct snag_binary_result_value extensions;
};
const char *snag_binary_archive_field_name(uint16_t kind, size_t index);
int snag_binary_archive_fields_encode(struct snag_buf *out, const char *type,
    const json_t *data, uint16_t *kind);
int snag_binary_archive_fields_decode(uint16_t kind, const void *data, size_t size,
    struct snag_binary_archive_fields *out);
/* Revalidates kind+bytes, ignoring derived type/mask/value views. */
int snag_binary_archive_fields_json(const struct snag_binary_archive_fields *fields, json_t **out);

/* Version2 distinguishes a native sequence reference from literal JSONL
 * coordinates. Native references carry only begin_seq; their physical cursor
 * is derived from the authenticated containing batch. Version1 is literal. */
struct snag_binary_voice_adopted {
    struct snag_binary_voice_transfer transfer;
    uint64_t begin_offset, begin_seq;
    unsigned char begin_sha256[32];
    bool native;
};

enum snag_binary_tool_status {
    SNAG_BINARY_TOOL_NOT_RUN = 1, SNAG_BINARY_TOOL_OUTCOME_UNKNOWN = 2,
    SNAG_BINARY_TOOL_DENIED = 3, SNAG_BINARY_TOOL_CANCELLED = 4,
    SNAG_BINARY_TOOL_RUNNING = 5, SNAG_BINARY_TOOL_SUCCEEDED = 6,
    SNAG_BINARY_TOOL_FAILED = 7, SNAG_BINARY_TOOL_SIGNALED = 8,
    SNAG_BINARY_TOOL_TIMED_OUT = 9, SNAG_BINARY_TOOL_PATCH_REJECTED = 10,
    SNAG_BINARY_TOOL_IO_FAILED = 11
};

enum snag_binary_tool_reason {
    SNAG_BINARY_TOOL_REASON_NONE = 0, SNAG_BINARY_TOOL_PROTOCOL_CONFLICT = 1,
    SNAG_BINARY_TOOL_READ_ONLY = 2, SNAG_BINARY_TOOL_PROCESS_LIMIT = 3,
    SNAG_BINARY_TOOL_BATCH_YIELD = 4, SNAG_BINARY_TOOL_OPERATOR_YIELD = 5,
    SNAG_BINARY_TOOL_PROCESS_BUSY = 6, SNAG_BINARY_TOOL_STDIN_BUSY = 7,
    SNAG_BINARY_TOOL_STDIN_CLOSED = 8, SNAG_BINARY_TOOL_INVALID_ARGUMENTS = 9,
    SNAG_BINARY_TOOL_MANAGED_PROCESS_CONFLICT = 10,
    SNAG_BINARY_TOOL_MANAGED_PROCESS_HANDLE_MISMATCH = 11,
    SNAG_BINARY_TOOL_RECOVERY_UNSTARTED = 12, SNAG_BINARY_TOOL_SUPERSEDED_BY_STEERING = 13,
    SNAG_BINARY_TOOL_TURN_CANCELLED = 14, SNAG_BINARY_TOOL_PROCESS_INTERACTION_REQUIRED = 15,
    SNAG_BINARY_TOOL_RULE_REJECTED = 16, SNAG_BINARY_TOOL_OWNER_LOST = 17,
    SNAG_BINARY_TOOL_UNREAPED_AFTER_SIGKILL = 18, SNAG_BINARY_TOOL_USER_DENIED = 19,
    SNAG_BINARY_TOOL_TIMEOUT_HANDOFF = 20, SNAG_BINARY_TOOL_WAIT_TIMEOUT = 21,
    SNAG_BINARY_TOOL_STEERING_HANDOFF = 22, SNAG_BINARY_TOOL_OUTPUT_DRAIN_TIMEOUT = 23
};

enum snag_binary_excerpt_encoding { SNAG_BINARY_EXCERPT_UTF8 = 1, SNAG_BINARY_EXCERPT_BASE64 = 2 };

struct snag_binary_tool_excerpt {
    enum snag_binary_excerpt_encoding encoding;
    uint64_t discarded_bytes, original_bytes, retained_bytes;
    /* Preserve the original string. Legacy base64 admission checks length only. */
    struct snag_binary_text retained;
};

struct snag_binary_tool_output_ref {
    unsigned char handle[16];
    uint64_t from[2], to[2];
    uint64_t stdin_accepted, stdin_written, stdin_pending;
    /* Original presentation coordinates remain exact in provider/history views.
     * Native readers use the separate half-open sequence range, never these. */
    uint64_t log_start, log_end;
    uint64_t first_sequence, end_sequence; /* Both zero: no recorded log hint. */
    bool native;
    bool stdin_open;
};

struct snag_binary_tool_result {
    enum snag_binary_tool_status status;
    enum snag_binary_tool_reason reason;
    uint64_t duration_ms, max_output_tokens; /* Zero limit means absent. */
    struct snag_binary_result_value exit_code, signal;
    struct snag_binary_text model_text;
    struct snag_binary_tool_excerpt streams[2]; /* stdout, stderr */
    struct snag_binary_tool_output_ref output_ref;
    struct snag_binary_content content; /* Empty view means absent. */
    unsigned char handle[16];
    bool has_handle, has_output_ref;
};

struct snag_binary_tool_finish {
    unsigned char turn[16], call[16];
    struct snag_binary_tool_result result;
};

enum snag_binary_process_cause {
    SNAG_BINARY_PROCESS_USER_INTERRUPT = 1, SNAG_BINARY_PROCESS_PROVIDER_FAILURE = 2,
    SNAG_BINARY_PROCESS_PROTOCOL_FAILURE = 3, SNAG_BINARY_PROCESS_TOOL_FAILURE = 4,
    SNAG_BINARY_PROCESS_OUTPUT_FAILURE = 5, SNAG_BINARY_PROCESS_INTERNAL_FAILURE = 6
};

struct snag_binary_process_close {
    unsigned char turn[16], handle[16];
    enum snag_binary_process_cause cause;
    struct snag_binary_tool_result result;
};

struct snag_binary_result_source {
    enum snag_binary_kind kind;
    unsigned char turn[16], owner[16]; /* Call ID or process ID, according to kind. */
    enum snag_binary_process_cause cause; /* Zero for tool_finished. */
    struct snag_binary_tool_result result;
};

struct snag_binary_tool_start {
    unsigned char turn[16];
    unsigned char call[16];
    unsigned char action_sha256[32];
    struct snag_binary_text cwd;
};

/* Source JSON representation; the binary record always stores decoded bytes. */
enum snag_binary_byte_encoding {
    SNAG_BINARY_BYTES_UTF8 = 1, SNAG_BINARY_BYTES_BASE64 = 2
};

/* Existing process-output replay chunk bound. */
#define SNAG_BINARY_PROCESS_CHUNK_MAX (16u * 1024u)

struct snag_binary_process_output {
    unsigned char turn[16];
    unsigned char handle[16];
    uint64_t offset;
    unsigned stream;
    enum snag_binary_byte_encoding encoding;
    const unsigned char *data;
    size_t size;
};

enum snag_binary_irc_kind {
    SNAG_BINARY_IRC_CONNECTED = 1, SNAG_BINARY_IRC_DISCONNECTED = 2,
    SNAG_BINARY_IRC_JOIN = 3, SNAG_BINARY_IRC_PART = 4, SNAG_BINARY_IRC_QUIT = 5,
    SNAG_BINARY_IRC_NICK = 6, SNAG_BINARY_IRC_MESSAGE = 7, SNAG_BINARY_IRC_NOTICE = 8,
    SNAG_BINARY_IRC_TOPIC = 9, SNAG_BINARY_IRC_MODE = 10, SNAG_BINARY_IRC_HISTORY_READY = 11
};

struct snag_binary_irc_route {
    unsigned char connection[16], conversation[16], send[16], membership[16];
    unsigned char reply_conversation[16], reply_membership[16];
    struct snag_binary_text peer, target, source;
    uint64_t generation;
    enum snag_irc_identity identity;
    enum snag_irc_conversation_kind kind;
    enum snag_irc_direction direction;
    enum snag_irc_delivery delivery;
    bool action, revised, has_send, has_membership, joined, rejoin;
    bool reply_captured, has_reply;
};

struct snag_binary_irc_event {
    enum snag_binary_irc_kind kind;
    uint64_t timestamp_ms;
    uint64_t sequence;
    unsigned char stream[16];
    struct snag_binary_text endpoint, room, nick, text;
    bool historical, is_local, op;
    bool has_watermark, has_stream, input;
    bool classified, urgent, reply;
    struct snag_binary_irc_route route; /* Record215 only. */
};

enum snag_binary_irc_snapshot_reason {
    SNAG_BINARY_IRC_SNAPSHOT_JOIN = 1, SNAG_BINARY_IRC_SNAPSHOT_NICK = 2,
    SNAG_BINARY_IRC_SNAPSHOT_TOPOLOGY = 3, SNAG_BINARY_IRC_SNAPSHOT_COMPACTION = 4
};

struct snag_binary_irc_snapshot {
    enum snag_binary_irc_snapshot_reason reason;
    uint64_t timestamp_ms;
    struct snag_binary_text text;
};

/* Nonempty, strictly increasing positive signed-64-bit journal sequences.
 * The borrowed encoding uses count4 and canonical unsigned base-128 integers.
 * Iterate a validated immutable list from offset zero, then returned offsets.
 * next returns 0/item, 1/EOF, -1/error and preserves outputs on failure/EOF. */
struct snag_binary_sequences { const unsigned char *data; size_t size; };
int snag_binary_sequences_encode(struct snag_buf *out, const uint64_t *values, size_t count);
int snag_binary_sequences_decode(const void *data, size_t size, struct snag_binary_sequences *out);
int snag_binary_sequences_next(const struct snag_binary_sequences *sequences,
    size_t *offset, uint64_t *out);

struct snag_binary_irc_admission {
    struct snag_binary_sequences sequences;
    /* kind=0 means absent; otherwise a complete typed input_received/steering_added payload. */
    struct snag_binary_record input;
};

struct snag_binary_event {
    enum snag_binary_kind kind;
    union {
        struct {
            uint16_t source_format; /* Legacy semantic revision 2/3/4, separate from framing. */
            enum snag_binary_protocol protocol;
            struct snag_binary_selection selection;
            struct snag_binary_text cwd;
        } created;
        struct { struct snag_binary_text before, after; } cwd, effort;
        struct { enum snag_binary_actor origin; } archive;
        struct {
            unsigned char confirmed_prefix[4], session[16], nonce[16];
        } deletion;
        struct snag_binary_text banner, shell, name, service_tier;
        struct snag_binary_options options;
        enum snag_binary_steering steering;
        struct { struct snag_binary_selection before, after; } model;
        struct {
            unsigned char id[16];
            struct snag_binary_selection before;
            struct snag_binary_text effort;
        } turn_model;
        struct { struct snag_binary_context_choice before, after; } context;
        struct snag_binary_turn_start started;
        struct snag_binary_response_start response_started;
        struct snag_binary_response_output response_output;
        struct snag_binary_response_interruption response_interrupted;
        struct snag_binary_response_failure response_failed;
        struct snag_binary_response_correction response_correction;
        struct snag_binary_response_complete response_completed;
        struct snag_binary_hosted_search hosted_search;
        struct snag_binary_control control;
        struct snag_binary_rule_log rule_log;
        struct snag_binary_rule_transform rule_transform;
        struct snag_binary_audio_usage audio_usage;
        struct snag_binary_voice_event voice_event;
        struct snag_binary_voice_archive voice_transfer_record;
        struct snag_binary_voice_transfer voice_transfer_sealed;
        struct snag_binary_voice_adopted voice_transfer_adopted;
        struct snag_binary_download download_queued;
        struct {
            unsigned char id[16];
            struct snag_binary_text reason;
        } download_removed;
        struct snag_binary_text downloads_cleared;
        struct snag_binary_compact_start compaction_started;
        struct snag_binary_compact_interrupt compaction_interrupted;
        struct snag_binary_compact_complete compaction_completed;
        struct snag_binary_context_rebase context_rebased;
        struct snag_binary_capacity_rejection capacity_rejected;
        struct snag_binary_tool_start tool_started;
        struct snag_binary_tool_finish tool_finished;
        struct snag_binary_process_output process_output;
        struct snag_binary_process_close process_closed;
        struct snag_binary_irc_event irc_event;
        struct snag_binary_irc_snapshot irc_snapshot;
        struct snag_binary_irc_admission irc_admitted;
        struct { uint64_t until_ms; uint32_t messages; } irc_sleep_set;
        enum { SNAG_BINARY_IRC_WAKE_TIMEOUT = 1, SNAG_BINARY_IRC_WAKE_MENTION = 2,
            SNAG_BINARY_IRC_WAKE_MESSAGES = 3 } irc_sleep_woke;
        struct {
            uint32_t after_updates;
            struct snag_binary_text instruction;
        } irc_compact_configured;
        struct {
            uint64_t through_seq, count;
            struct snag_binary_text summary;
        } irc_compacted;
        struct { unsigned char turn[16], response[16], item[16]; } completed;
        struct {
            unsigned char turn[16], response[16];
            enum snag_binary_quiet_reason reason;
        } silent;
        struct {
            unsigned char turn[16];
            enum snag_binary_interrupt_origin origin;
            enum snag_binary_interrupt_reason reason;
        } interrupted;
        struct {
            unsigned char turn[16];
            enum snag_binary_failure_class class_id;
            struct snag_binary_text message;
        } failed;
        struct {
            unsigned char turn[16];
            struct snag_binary_text class_name, message;
            bool has_retry_attempts;
            uint64_t retry_attempts;
        } recovery;
        struct {
            struct snag_binary_selection selection;
            struct snag_binary_text text;
            struct snag_binary_instructions instructions;
            struct snag_binary_content content; /* Absent when content_ref is also zero. */
            struct snag_binary_input_reference text_ref, content_ref, instructions_ref;
            uint64_t received_ms;
            enum snag_binary_input_origin origin;
            bool read_only;
        } input;
        struct {
            unsigned char id[16], turn[16];
            struct snag_binary_text text;
            struct snag_binary_content content;
            struct snag_binary_input_reference text_ref, content_ref;
            uint64_t received_ms;
            bool has_received_ms;
        } steering_input;
        struct {
            unsigned char turn[16];
            uint64_t time_ms;
            struct snag_binary_ids ids;
        } admission;
        struct { enum snag_binary_actor actor; struct snag_binary_ids ids; } queue_cancel;
        unsigned char turn[16];
        bool queue_armed;
        struct {
            unsigned char id[16], while_id[16];
            enum snag_binary_while_turn while_kind;
            bool read_only, has_armed, armed, has_received_ms, has_voice;
            uint64_t received_ms;
            struct snag_binary_text text;
            struct snag_binary_content content;
            struct snag_binary_voice_source voice;
            struct snag_binary_input_reference text_ref, content_ref;
        } queued;
        struct {
            unsigned char id[16];
            uint64_t due_ms;
            struct snag_binary_text text;
        } timer;
        struct {
            unsigned char id[16], replacement[16];
            enum snag_binary_actor actor;
            enum snag_binary_pause pause;
            bool locked;
            struct snag_binary_text text; /* Prompt or blocker, selected by kind. */
            struct snag_binary_text wait_for; /* Required only by the wait-bearing variant. */
        } goal;
    } data;
};

/* Return the current payload version, or zero for an unsupported kind.
 * Input version 2 adds references; turn-start version 1 includes them from its
 * first definition. Other kinds use 1. */
uint16_t snag_binary_event_version(enum snag_binary_kind kind);
/* Exact legacy type names for known semantic kinds. Unknown kinds return NULL;
 * a failed name lookup leaves out unchanged. This does not validate a payload. */
const char *snag_binary_event_name(enum snag_binary_kind kind);
int snag_binary_event_kind(const char *name, enum snag_binary_kind *out);

/* Encode the current payload version, flags zero, appending atomically to out.
 * Decode also accepts input version 1's literal-only fields. It borrows immutable
 * record bytes and leaves out unchanged on
 * failure. It returns 1 only for an unknown, explicitly optional metadata kind,
 * -1 for malformed/unsupported required semantics, and 0 for a decoded event.
 * References require resolution before adoption; reducer checks still govern
 * state transitions, authority and current IDs. */
int snag_binary_event_encode(struct snag_buf *out, const struct snag_binary_event *event);
int snag_binary_event_decode(const struct snag_binary_record *record,
    struct snag_binary_event *out);

/* Create/resolve whole input-field references in a verified immutable batch
 * from the same journal. Text excludes its length prefix; content/instructions
 * include their complete typed list. Resolve validates the source structure and
 * exact literal-field identity, including empty versus absent fields. Reference
 * chains are rejected: reuse the original reference directly. Views borrow the
 * batch. Outputs stay unchanged on failure. The caller resolves every required
 * reference and checks causal ordering, receipt/provenance identity, target-field
 * constraints and reducer authority before adoption. */
int snag_binary_input_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    enum snag_binary_input_leaf field, struct snag_binary_ref *out);
int snag_binary_input_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, enum snag_binary_input_leaf field,
    const unsigned char **view);

struct snag_binary_control_text_source {
    struct snag_binary_event event;
    struct snag_binary_text text;
};

/* Whole canonical state text: created/current cwd (never the previous cwd),
 * session name, banner, scheduled timer text, goal prompt or blocker. Pin the exact source
 * kind, validate its entire record and return the decoded event with the leaf.
 * Empty banner text is a present clearing field. Other control events supply no
 * text. Caller owns same-journal, causal, current owner/provenance and lifecycle
 * checks before checkpoint adoption. Borrow immutable verified batch storage;
 * failures preserve outputs. No authority follows from equal text or this view. */
int snag_binary_control_text_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    enum snag_binary_kind kind, struct snag_binary_ref *out);
int snag_binary_control_text_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, enum snag_binary_kind kind,
    struct snag_binary_control_text_source *out);

/* Exact, whole canonical response-output text, never a metadata or partial
 * slice. Resolve returns the original response/item/index/offset as well as
 * borrowed text. The caller checks the same journal, causal sequence and item
 * identity/order before composing a final response; no reference chains.
 * Inputs are verified immutable batches. Outputs stay unchanged on failure. */
int snag_binary_output_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    struct snag_binary_ref *out);
int snag_binary_output_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, struct snag_binary_response_output *out);

/* Whole original graph items and continuation lists from completed responses.
 * Graph references include typed metadata and require the expected item kind.
 * A public item backed by a span cannot supply a new original; reuse its span.
 * Continuation references include count/placement wrappers and permit an empty
 * list, but not an absent/null continuation. Resolve validates the entire source
 * record, returns its owner metadata and borrows immutable batch storage.
 * Caller checks same-journal/causal/state authority. Failures preserve outputs. */
int snag_binary_graph_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    uint32_t index, enum snag_binary_item_kind kind, struct snag_binary_ref *out);
int snag_binary_graph_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, enum snag_binary_item_kind kind,
    struct snag_binary_graph_source *out);
int snag_binary_continuation_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    struct snag_binary_ref *out);
int snag_binary_continuation_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, struct snag_binary_continuation_source *out);

/* Exact original process-output bytes. Resolve validates the whole record and
 * returns turn/process/stream/offset/encoding metadata with the borrowed bytes.
 * Same-journal, causal and process-range checks remain with the caller. Inputs
 * are verified immutable batches; failures leave outputs unchanged. */
int snag_binary_process_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    struct snag_binary_ref *out);
int snag_binary_process_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, struct snag_binary_process_output *out);

/* Select the complete original provider-output array in a compaction completion.
 * Validate its whole source record and hash before returning borrowed fields.
 * The caller owns journal/causal/compaction-state checks on the immutable batch. */
int snag_binary_compact_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    struct snag_binary_ref *out);
int snag_binary_compact_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, struct snag_binary_compact_complete *out);

/* Complete original result fields only, from the required tool-finished or
 * process-closed source kind. Validate the entire selected record and return
 * borrowed fields plus owner metadata. Caller checks journal/causal/owner state
 * and maps historical log coordinates. Inputs are verified immutable batches;
 * failures preserve outputs. These references do not select nested result leaves. */
int snag_binary_result_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    enum snag_binary_kind kind, struct snag_binary_ref *out);
int snag_binary_result_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, enum snag_binary_kind kind,
    struct snag_binary_result_source *out);

int snag_binary_output_span_encode(unsigned char out[SNAG_BINARY_OUTPUT_SPAN_SIZE],
    const struct snag_binary_output_span *span);
int snag_binary_output_span_decode(const void *data, size_t size,
    struct snag_binary_output_span *out);

/* Borrowed list of public values, including an empty list. Decode checks the
 * binary shape/profile. Resolve spans before validating aggregate graph size,
 * identity uniqueness and state. Iterate an immutable decoded list from offset 0;
 * next returns 0/1/-1 for item/EOF/error. Failed operations preserve outputs. */
int snag_binary_public_items_encode(struct snag_buf *out,
    const struct snag_binary_public_value *items, size_t count);
int snag_binary_public_items_decode(const void *data, size_t size,
    struct snag_binary_public_items *out);
int snag_binary_public_items_next(const struct snag_binary_public_items *items,
    size_t *offset, struct snag_binary_public_value *out);

/* Borrowed typed lists. As with public snapshots, resolve sources before graph
 * identity/order/aggregate admission. Iteration requires an immutable decoded
 * list and uses offset 0 initially; returns 0/1/-1 for item/EOF/error. */
int snag_binary_graph_items_encode(struct snag_buf *out,
    const struct snag_binary_graph_item *items, size_t count);
int snag_binary_graph_items_decode(const void *data, size_t size,
    struct snag_binary_graph_items *out);
int snag_binary_graph_items_next(const struct snag_binary_graph_items *items,
    size_t *offset, struct snag_binary_graph_item *out);
int snag_binary_continuation_encode(struct snag_buf *out,
    const struct snag_binary_continuation_item *items, size_t count, uint32_t semantic_count);
int snag_binary_continuation_decode(const void *data, size_t size, uint32_t semantic_count,
    struct snag_binary_continuation *out);
int snag_binary_continuation_next(const struct snag_binary_continuation *items,
    size_t *offset, struct snag_binary_continuation_item *out);

/* Append only after resolving the whole span beneath a caller-verified immutable
 * journal boundary. Anchor must precede or contain its first record. Item supplies
 * expected public metadata; its literal text must be absent. Compare the source's
 * own partial-public index across fragments, never a final graph index. The caller
 * owns journal identity, causal ordering and semantic validation of other records.
 * Uses bounded batch scratch and at most one public item's assembled text. Leaves
 * out and the fd position unchanged on failure; never changes the journal. */
int snag_binary_output_span_resolve(int fd, uint64_t boundary,
    const struct snag_binary_anchor *anchor, const struct snag_binary_output_span *span,
    const unsigned char turn[16], const unsigned char response[16], uint32_t cycle,
    const struct snag_binary_public_item *item, struct snag_buf *out);

struct snag_binary_checkpoint_index;
/* Source-only span assembly under independently trusted immutable membership.
 * Access names every fragment in this span; NULL walks the contiguous oracle.
 * Preserve original scope, item/index, offsets, exact first tuple and total.
 * Missing fragments fail, including zero-byte ends. Atomic append and pread;
 * this does not establish snapshot membership or lifecycle authority. */
int snag_binary_output_span_read(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access, const struct snag_binary_output_span *span,
    const unsigned char turn[16], const unsigned char response[16], uint32_t cycle,
    const struct snag_binary_public_item *item, struct snag_buf *out);

#endif
