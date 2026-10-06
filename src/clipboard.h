/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CLIPBOARD_H
#define SNAJPAGENT_CLIPBOARD_H

#include "base.h"
#include "wake.h"

enum snag_clipboard_policy { SNAG_CLIP_NATIVE, SNAG_CLIP_OSC52, SNAG_CLIP_OFF };

enum snag_clipboard_backend_kind {
    SNAG_CLIPBOARD_NONE, SNAG_CLIPBOARD_MAC, SNAG_CLIPBOARD_WAYLAND, SNAG_CLIPBOARD_X11
};
struct snag_clipboard_backend {
    enum snag_clipboard_backend_kind kind;
    char *program;
    /* NULL selects the general Mac pasteboard; a name permits isolated tests. */
    char *pasteboard;
};
int snag_clipboard_backend(struct snag_clipboard_backend *);
void snag_clipboard_backend_free(struct snag_clipboard_backend *);

enum snag_clipboard_state {
    SNAG_CLIPBOARD_PREPARING, SNAG_CLIPBOARD_READY, SNAG_CLIPBOARD_PUBLISHING,
    SNAG_CLIPBOARD_WRITTEN, SNAG_CLIPBOARD_CANCELED, SNAG_CLIPBOARD_UNAVAILABLE,
    SNAG_CLIPBOARD_FAILED, SNAG_CLIPBOARD_UNCERTAIN
};
struct snag_clipboard_result {
    enum snag_clipboard_state state;
    uint64_t bytes, length;
    char sha256[SNAG_SHA256_HEX_LEN + 1u], error[256];
};
struct snag_clipboard;

/* Clone immutable bytes or duplicate fd, then validate/hash in the background.
 * fd >= 0 selects a regular file, otherwise bytes supplies length bytes.
 * The backing file must remain immutable; the caller may close its descriptor.
 * NULL backend discovers native desktop support. No clipboard is read.
 * READY is a commit barrier: preparation never changes the native clipboard. */
struct snag_clipboard *snag_clipboard_open(int fd, const void *bytes, uint64_t length,
    const struct snag_clipboard_backend *);
snag_wake_fd snag_clipboard_fd(const struct snag_clipboard *);
void snag_clipboard_result(struct snag_clipboard *, struct snag_clipboard_result *);
/* Exact immutable source reads also serve a checked remote transfer. */
int snag_clipboard_read(const struct snag_clipboard *, uint64_t offset, void *, size_t);
int snag_clipboard_publish(struct snag_clipboard *);
/* True means cancellation was accepted before publication. Once committed,
 * report the actual result; cancellation cannot promise to restore a clipboard. */
bool snag_clipboard_cancel(struct snag_clipboard *);
void snag_clipboard_close(struct snag_clipboard *);

/* The terminal owner serializes these chunks with its display output. Source
 * must remain READY and immutable. Emission has no terminal write receipt. */
struct snag_clipboard_osc {
    struct snag_clipboard *source;
    uint64_t offset;
    bool begun, done;
};
int snag_clipboard_osc_next(struct snag_clipboard_osc *, char *out, size_t capacity);

#endif
