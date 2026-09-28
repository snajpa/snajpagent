/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_EVENT_H
#define SNAJPAGENT_STORE_BINARY_EVENT_H

#include "store_binary.h"

/* Stable draft record IDs. Reserve 1..31 for session metadata and 0x8000..ffff
 * for explicitly optional metadata. Required semantic kinds stay below 0x8000.
 * Extend these concrete event families; do not reuse an assigned number. */
enum snag_binary_kind {
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
    SNAG_BINARY_GOAL_CANCELLED = 72
};

enum snag_binary_actor { SNAG_BINARY_USER = 1, SNAG_BINARY_MODEL = 2 };
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

struct snag_binary_event {
    enum snag_binary_kind kind;
    union {
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

/* Typed payload codec, version 1, flags 0. The encoder appends atomically to
 * out. The decoder borrows immutable record bytes and leaves out unchanged on
 * failure. It returns 1 only for an unknown, explicitly optional metadata kind,
 * -1 for malformed/unsupported required semantics, and 0 for a decoded event.
 * Reducer checks still govern state transitions, authority and current IDs. */
int snag_binary_event_encode(struct snag_buf *out, const struct snag_binary_event *event);
int snag_binary_event_decode(const struct snag_binary_record *record,
    struct snag_binary_event *out);

#endif
