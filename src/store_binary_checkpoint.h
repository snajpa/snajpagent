/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_CHECKPOINT_H
#define SNAJPAGENT_STORE_BINARY_CHECKPOINT_H

#include "store.h"
#include "store_binary_event.h"
#include "store_binary_index.h"

/* Provider version 1 is an explicit rebuild recipe. Each recent row names one
 * canonical sequence and saves post-reduction time/turn/flags. Historical rows
 * name the IRC lookup closure. Payloads, requests and provider configuration stay
 * outside this section. These borrowed views carry structural validity only;
 * source membership, roles and completeness require a common-boundary check. */
struct snag_binary_checkpoint_provider {
    uint64_t next_seq, compact_seq, rebase_seq;
    const unsigned char *recent, *history;
    size_t recent_count, history_count;
};
int snag_binary_checkpoint_provider_encode(struct snag_buf *, const struct snag_session *,
    const json_t *recent, const json_t *history);
int snag_binary_checkpoint_provider_decode(const void *, size_t,
    struct snag_binary_checkpoint_provider *);
/* Materialize fresh metadata entries with shared immutable canonical payloads.
 * The two source pools must come from semantic verification of the same immutable
 * prefix; this lookup does not establish source authority or lifecycle itself.
 * Both owned output arrays are installed only on complete success; callers
 * release their old arrays separately. */
int snag_binary_checkpoint_provider_materialize(const void *, size_t,
    const json_t *source_recent, const json_t *source_history, json_t **recent, json_t **history);

/* Hydrate one canonical source event without replaying or adopting its state.
 * Requires independently trusted immutable source membership and complete
 * dependency closure. Typed roles, original scope/field tuples and causality are
 * checked; producer lifecycle/receipt authority is not re-established here.
 * NULL access retains independent backward lookup. Outputs change on success. */
int snag_binary_checkpoint_projection_read(int fd, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_index *, uint64_t sequence,
    const char **type, json_t **out);

/* Restore this recipe directly from canonical sources and complete pinned
 * working-set closure. Recent/history membership, metadata and lifecycle require
 * joint checkpoint admission; this materializer grants no resume authority.
 * Cancellation before each source row; pread only, both outputs atomic. */
int snag_binary_checkpoint_provider_read(int fd, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_index *, const void *data, size_t size,
    bool (*cancelled)(void *), void *opaque, json_t **recent, json_t **history);

/* Field-shaped accounting block, version 1. The observations have independent
 * lifetimes; invalid observations retain their metadata and counter values.
 * This block alone is neither a complete core snapshot nor resume authority. */
struct snag_binary_checkpoint_accounting {
    struct snag_input_observation active_accounting, usage_anchor;
    struct snag_input_observation context_meter, capacity_rejection;
    struct snag_usage_totals usage_totals;
};

/* Append/decode one complete block. Failure preserves the destination; decode
 * requires exact input consumption and allocates no memory. All counters use
 * unsigned 64-bit wire values, independently of the legacy JSON projection. */
int snag_binary_checkpoint_accounting_encode(struct snag_buf *,
    const struct snag_binary_checkpoint_accounting *);
int snag_binary_checkpoint_accounting_decode(const void *, size_t,
    struct snag_binary_checkpoint_accounting *);

/* Version-1 control metadata and six control sequences. Decode updates only
 * its listed scalar fields, after full validation. Common frame identity/counts,
 * journal/checkpoint coordinates, payloads, accounting, voice history, arrays,
 * resources and callbacks remain untouched. Use a provisional state; this is
 * not a full checkpoint loader and establishes no lifecycle/reference authority. */
int snag_binary_checkpoint_controls_encode(struct snag_buf *, const struct snag_session *);
int snag_binary_checkpoint_controls_decode(const void *, size_t, struct snag_session *);

enum snag_binary_checkpoint_text_slot {
    SNAG_BINARY_TEXT_CWD, SNAG_BINARY_TEXT_FIRST_USER, SNAG_BINARY_TEXT_LAST_USER,
    SNAG_BINARY_TEXT_ACTIVE_PROMPT, SNAG_BINARY_TEXT_GOAL_PROMPT, SNAG_BINARY_TEXT_GOAL_BLOCKER,
    SNAG_BINARY_TEXT_TIMER, SNAG_BINARY_TEXT_BANNER, SNAG_BINARY_TEXT_STEERING,
    SNAG_BINARY_TEXT_IRC_SNAPSHOT, SNAG_BINARY_TEXT_NAME,
    SNAG_BINARY_CHECKPOINT_TEXT_COUNT
};

struct snag_binary_checkpoint_text_source {
    uint64_t declaration;
    /* field=0 selects the declaration's control text; input fields name the
     * original literal, never another reference. Steering and the empty IRC
     * snapshot established by session creation have no byte slice. */
    struct snag_binary_input_reference original;
};

struct snag_binary_checkpoint_texts {
    uint64_t through; /* Last observed semantic event; optional metadata may follow. */
    struct snag_binary_checkpoint_text_source slots[SNAG_BINARY_CHECKPOINT_TEXT_COUNT];
};

/* Advance after successful strict reduction, starting with creation at seq1.
 * State and record must describe the same provisional event. Publication and
 * source verification remain the caller's responsibility. No payload copies. */
int snag_binary_checkpoint_texts_step(struct snag_binary_checkpoint_texts *,
    const struct snag_binary_record *, uint64_t sequence, const struct snag_session *);
int snag_binary_checkpoint_texts_encode(struct snag_buf *,
    const struct snag_binary_checkpoint_texts *);
int snag_binary_checkpoint_texts_decode(const void *, size_t,
    struct snag_binary_checkpoint_texts *);

/* Materialize the ten fixed dictionary slots from an immutable same-journal
 * prefix with an independently trusted anchor. Check declaration role and its
 * exact original-field tuple; do not confer current state/lifecycle authority
 * from a decoded table. The complete consumer must establish that this table
 * belongs to its snapshot. No state adoption; only success replaces *out with
 * a new owned JSON object (the caller retains ownership of its old *out).
 * Pinned access uses direct old-record locations and a caller-bounded newer
 * suffix; NULL retains backward lookup for the independent full-prefix oracle. */
int snag_binary_checkpoint_texts_read(int fd, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_index *, const struct snag_binary_checkpoint_texts *,
    json_t **out);

struct snag_binary_checkpoint_call_source {
    uint64_t graph;
    struct snag_binary_checkpoint_text_source cwd;
};

struct snag_binary_checkpoint_process_source {
    char handle[SNAG_ID_HEX_LEN + 1u]; /* Producer correlation, derived on read. */
    uint64_t started;
    struct snag_binary_checkpoint_call_source call;
};

struct snag_binary_checkpoint_queue_source {
    uint64_t creation, text;
};

struct snag_binary_checkpoint_download_source {
    char id[SNAG_ID_HEX_LEN + 1u]; /* Producer correlation; not stored twice. */
    uint64_t receipt;
};

/* Current payload origins, published together only after complete replay. */
struct snag_binary_checkpoint_sources {
    struct snag_binary_checkpoint_texts texts;
    struct snag_binary_checkpoint_call_source calls;
    struct snag_binary_checkpoint_process_source *processes;
    size_t process_count;
    uint64_t input;
    struct snag_binary_checkpoint_queue_source *queue;
    size_t queue_count;
    uint64_t compact_start, compact_end, response_start, response_end;
    uint64_t active_compact; /* Current attempt, separate from retained completed output. */
    uint64_t resume_options;
    struct snag_binary_checkpoint_download_source *downloads;
    size_t download_count;
};

/* Release an initialized/returned owning source set, then zero it. Successful
 * replay returns new ownership; callers release old sources before reusing them. */
void snag_binary_checkpoint_sources_free(struct snag_binary_checkpoint_sources *);

struct snag_binary_checkpoint_calls {
    struct snag_binary_checkpoint_call_source source;
    const unsigned char *flags; /* Borrowed: bit0 started, bit1 finished. */
    size_t count;
};

/* Select complete core/provider materializer closure from an independently
 * verified snapshot and available old working-set sources plus bounded suffix.
 * Available must contain every old field/span/transform dependency; omissions
 * are not absence proofs. The caller supplies the full frontier at through and
 * establishes common identity/ancestry, immutable bytes and snapshot semantics.
 * Current roots and their required locations are sorted/deduplicated; no lifetime
 * map/index file/replay is read. Atomic encoded append; pread and cancellation. */
int snag_binary_checkpoint_access_capture(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *available,
    const struct snag_binary_index_tree *frontier,
    const struct snag_binary_checkpoint_sources *, const struct snag_session *,
    const void *provider, size_t provider_size, bool (*cancelled)(void *), void *opaque,
    struct snag_buf *out);

/* Calls all originate in one completed graph. Save its directory before a
 * later cwd change; status bits come from the same provisional core snapshot.
 * Decode borrows the block. Encode appends atomically and supports aliasing. */
int snag_binary_checkpoint_calls_encode(struct snag_buf *,
    const struct snag_binary_checkpoint_call_source *, const struct snag_session *);
int snag_binary_checkpoint_calls_decode(const void *, size_t,
    struct snag_binary_checkpoint_calls *);

/* Read only: match the snapshot's turn/response/cycle, exact graph call order
 * and canonical graph-time cwd. Return a new owned array on success, leaving
 * the caller's previous *out owned by the caller. Empty calls yield NULL.
 * The anchor must already authenticate the immutable same-journal prefix.
 * Access pins the complete graph/transform/literal closure; NULL retains the
 * independent contiguous oracle. Missing old point sources fail ENOENT.
 * Source checks do not prove current snapshot membership or status authority;
 * complete checkpoint validation/adoption remains the enclosing consumer's job. */
int snag_binary_checkpoint_calls_read(int fd, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_index *,
    const struct snag_binary_checkpoint_calls *, const struct snag_session *,
    struct snag_pending_call **out);

/* Derive a process's immutable handle/labels from its accepting tool start and
 * original graph. Status, counters and journal scan caches remain zero. */
int snag_binary_checkpoint_process_source_read(int fd, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_index *,
    const struct snag_binary_checkpoint_process_source *, const struct snag_session *,
    struct snag_process_state *out);

struct snag_binary_checkpoint_processes {
    const unsigned char *data; /* Borrowed entries, each 97 bytes in version1. */
    size_t count;
};

/* Exact field-shaped metadata plus original-source references. No argument or
 * label copies and no JSONL log_offset/log_seq/log_hash caches on wire. Decode
 * borrows input. Read returns a new owned array without adopting state or
 * recreating a live process. The complete consumer owns snapshot authority,
 * counter/lifecycle coherence and verified native scan cursor construction. */
int snag_binary_checkpoint_processes_encode(struct snag_buf *,
    const struct snag_binary_checkpoint_sources *, const struct snag_session *);
int snag_binary_checkpoint_processes_decode(const void *, size_t,
    struct snag_binary_checkpoint_processes *);
/* Read one origin from a decoded view; its correlation handle is derived by
 * processes_read, not duplicated on wire. Failure preserves the destination. */
int snag_binary_checkpoint_processes_origin(const struct snag_binary_checkpoint_processes *,
    size_t index, struct snag_binary_checkpoint_process_source *);
int snag_binary_checkpoint_processes_read(int fd, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_index *,
    const struct snag_binary_checkpoint_processes *, const struct snag_session *,
    struct snag_process_state **out);

struct snag_binary_checkpoint_inputs {
    uint64_t input;
    const unsigned char *queue, *steering; /* Borrowed 24/16-byte entries. */
    size_t queue_count, steering_count;
};

struct snag_binary_checkpoint_inputs_state {
    json_t *input, *strings; /* Strings own the arrays' text pointers. */
    struct snag_queued_turn *queue;
    struct snag_pending_steering *steering;
    size_t queue_count, steering_count, queue_bytes, steering_bytes;
};

/* Read one accepted receipt, including embedded IRC input/steering. Steering
 * also accepts reply reminders and output-correction prompts. References use
 * the replay resolver; output is new ownership only on success. No reduction.
 * Pinned access covers declarations and original literal fields; missing old
 * entries fail. NULL access retains the independent full-prefix oracle path. */
int snag_binary_checkpoint_receipt_read(int fd, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_index *, uint64_t sequence, enum snag_binary_kind,
    json_t **out, uint64_t *timestamp);

/* Version1: direct receipt, ordered queue creation/latest-text receipts and
 * steering receipts, plus first-context timestamps. Payloads stay in the
 * journal. Structural/source validation does not establish pending membership
 * or lifecycle authority. Read checks steering turn against provisional state;
 * full snapshot adoption and string-dictionary merging belong to its consumer.
 * Success replaces *out with new ownership; failure leaves it unchanged. */
int snag_binary_checkpoint_inputs_encode(struct snag_buf *,
    const struct snag_binary_checkpoint_sources *, const struct snag_session *);
int snag_binary_checkpoint_inputs_decode(const void *, size_t,
    struct snag_binary_checkpoint_inputs *);
int snag_binary_checkpoint_inputs_read(int fd, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_index *, const struct snag_binary_checkpoint_inputs *,
    const struct snag_session *,
    struct snag_binary_checkpoint_inputs_state *out);
void snag_binary_checkpoint_inputs_free(struct snag_binary_checkpoint_inputs_state *);

struct snag_binary_checkpoint_payloads {
    uint64_t turn, compact_start, compact_end, response_start, response_end;
    uint64_t resume_options;
    const unsigned char *downloads; /* Borrowed LEu64 receipt sequences. */
    size_t download_count;
    bool downloads_present;
};

struct snag_binary_checkpoint_payloads_state {
    json_t *instructions, *compact_output, *response_public, *downloads;
    json_t *resume_options;
    size_t response_public_bytes;
};

/* Remaining dynamic core payloads, selected by their accepting declarations.
 * Decode borrows; encode appends atomically. Read returns new ownership only on
 * success, from a trusted immutable same-journal prefix. It validates source
 * roles/owners and reconstructs stream fragments/downloads with the strict
 * reducer in private state, without dispatch or state adoption. Complete
 * snapshot membership, lifecycle authority and provider adoption remain the
 * enclosing consumer's responsibility. Caller releases old outputs separately. */
int snag_binary_checkpoint_payloads_encode(struct snag_buf *,
    const struct snag_binary_checkpoint_sources *, const struct snag_session *);
int snag_binary_checkpoint_payloads_decode(const void *, size_t,
    struct snag_binary_checkpoint_payloads *);
int snag_binary_checkpoint_payloads_read(int fd, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_index *,
    const struct snag_binary_checkpoint_payloads *, const struct snag_session *,
    struct snag_binary_checkpoint_payloads_state *);
void snag_binary_checkpoint_payloads_free(struct snag_binary_checkpoint_payloads_state *);

/* Check accepting response/active-compaction declarations against provisional
 * control state. An empty retained stream still has a response epoch. */
int snag_binary_checkpoint_epochs_check(int fd, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_index *,
    const struct snag_binary_checkpoint_sources *, const struct snag_session *);

/* Resolve an adoption and its logical begin within a verified native boundary.
 * Returns a fresh physical cursor; never accepts stored JSONL coordinates.
 * Snapshot membership/authority belongs to the enclosing checkpoint consumer. */
int snag_binary_checkpoint_voice_read(int fd, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_index *,
    uint64_t sequence, const char *session_id, struct snag_voice_history_root *out);

/* Version-2 core candidate: seven field-shaped components, an adoption and two accepting
 * epochs. Encode appends atomically, including when inputs borrow its buffer.
 * Read requires a frame already decoded against an independently authenticated
 * identity/boundary in this immutable journal. It returns new state-only and
 * source ownership together; failure leaves both outputs untouched. Pinned
 * access supplies the complete working-set and causal-range closure, with its
 * common identity/ancestry and bounded newer suffix established by the caller.
 * NULL access retains the independent prefix oracle. Callers
 * release old outputs separately, and close the returned session normally.
 *
 * This is provisional assembly, not resume authority. Latest membership and
 * lifecycle validation, provider decoding, native process scan cursors and
 * joint adoption remain with the enclosing consumer. Voice history names its
 * native adoption record; its cursor is reconstructed from the journal. Resources,
 * callbacks, derived caches and unreachable private string owners are omitted. */
#define SNAG_BINARY_CORE_VERSION 2u
int snag_binary_checkpoint_core_encode(struct snag_buf *,
    const struct snag_binary_checkpoint_sources *, const struct snag_session *);
int snag_binary_checkpoint_core_read(int fd, const struct snag_binary_checkpoint_frame *,
    const struct snag_binary_checkpoint_index *,
    struct snag_session *, struct snag_binary_checkpoint_sources *);

#endif
