/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_EVENT_H
#define SNAJPAGENT_STORE_BINARY_EVENT_H

#include "store_binary.h"

/* Stable draft record IDs. Use 1..31 for session metadata and 0x8000..ffff
 * for explicitly optional metadata. Required semantic kinds stay below 0x8000.
 * Extend these concrete event families; do not reuse an assigned number. */
enum snag_binary_kind {
    SNAG_BINARY_SESSION_CREATED = 1,
    SNAG_BINARY_CWD_CHANGED = 2,
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
    SNAG_BINARY_TURN_STARTED = 128
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

enum snag_binary_turn_origin {
    SNAG_BINARY_TURN_DIRECT = 0,
    SNAG_BINARY_TURN_QUEUED = 1,
    SNAG_BINARY_TURN_GOAL = 2,
    SNAG_BINARY_TURN_TIMER = 3
};

/* Fixed schema positions, not arbitrary configuration keys. Each numeric
 * field's presence is bit (1u << field); preserve absence for legacy defaults. */
enum snag_binary_turn_config_number {
    SNAG_BINARY_TURN_PROMPT_SCHEMA = 0,
    SNAG_BINARY_TURN_REPLAY_SCHEMA = 1,
    SNAG_BINARY_TURN_TOOL_SCHEMA = 2,
    SNAG_BINARY_TURN_MAX_PARALLEL = 3,
    SNAG_BINARY_TURN_DEFAULT_YIELD = 4,
    SNAG_BINARY_TURN_MAX_WAIT = 5,
    SNAG_BINARY_TURN_DEFAULT_TIMEOUT = 6,
    SNAG_BINARY_TURN_MAX_TIMEOUT = 7,
    SNAG_BINARY_TURN_TOOL_OUTPUT = 8,
    SNAG_BINARY_TURN_OUTPUT_CACHE = 9,
    SNAG_BINARY_TURN_MAX_RETRIES = 10,
    SNAG_BINARY_TURN_NUMBER_COUNT = 11
};

enum snag_binary_turn_config_flag {
    SNAG_BINARY_TURN_CAPABILITY = 1u << 11,
    SNAG_BINARY_TURN_PROFILE = 1u << 12,
    SNAG_BINARY_TURN_MAX_OUTPUT = 1u << 13,
    SNAG_BINARY_TURN_PARALLEL_CALLS = 1u << 14
};

struct snag_binary_turn_config {
    struct snag_binary_selection selection;
    uint16_t present;
    uint64_t numbers[SNAG_BINARY_TURN_NUMBER_COUNT];
    struct snag_binary_text capability;
    struct snag_binary_text profile;
    uint64_t max_output_tokens;
    bool max_output_null;
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
        struct snag_binary_text banner, shell;
        enum snag_binary_steering steering;
        struct { struct snag_binary_selection before, after; } model;
        struct {
            unsigned char id[16];
            struct snag_binary_selection before;
            struct snag_binary_text effort;
        } turn_model;
        struct { struct snag_binary_context_choice before, after; } context;
        struct snag_binary_turn_start started;
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
        } goal;
    } data;
};

/* Return the current payload version, or zero for an unsupported kind.
 * Input version 2 adds references; turn-start version 1 includes them from its
 * first definition. Other kinds use 1. */
uint16_t snag_binary_event_version(enum snag_binary_kind kind);

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

#endif
