/* SPDX-License-Identifier: GPL-2.0-only */
#include "clipboard.h"
#include "fs.h"
#include "process_host.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Bounded I/O quanta keep cancellation and pipe backpressure observable. */
#define CLIPBOARD_CHUNK 65536u
#define CLIPBOARD_HELPER_IDLE_MS 10000u

enum clipboard_request { REQUEST_NONE, REQUEST_PUBLISH, REQUEST_CANCEL };
struct snag_clipboard {
    struct snag_clipboard_backend backend;
    int fd;
    unsigned char *text;
    uint64_t length;
    pthread_t worker;
    bool started;
    snag_wake_fd wake[2], control[2];
    atomic_int state, request;
    atomic_bool stop;
    atomic_uint_fast64_t progress;
    char sha256[SNAG_SHA256_HEX_LEN + 1u], error[256];
};

void
snag_clipboard_backend_free(struct snag_clipboard_backend *backend)
{
    free(backend->program);
    free(backend->pasteboard);
    memset(backend, 0, sizeof(*backend));
}

int
snag_clipboard_backend(struct snag_clipboard_backend *backend)
{
    *backend = (struct snag_clipboard_backend){0};
#ifdef __APPLE__
    backend->kind = SNAG_CLIPBOARD_MAC;
#elif !defined(_WIN32)
    const char *wayland = getenv("WAYLAND_DISPLAY"), *display = getenv("DISPLAY");
    const char *names[] = {"wl-copy", "xclip"};
    for (size_t i = 0u; i < 2u; ++i) {
        const char *session = i ? display : wayland;
        if (!session || !*session) continue;
        char *program = snag_program_path(names[i]);
        if (program && snag_path_root_len(program) && snag_file_executable(program) == 0) {
            backend->program = program;
            backend->kind = i ? SNAG_CLIPBOARD_X11 : SNAG_CLIPBOARD_WAYLAND;
            break;
        }
        free(program);
    }
#endif
    return 0;
}

int
snag_clipboard_read(const struct snag_clipboard *copy, uint64_t offset, void *bytes, size_t size)
{
    if (!copy || offset > copy->length || size > copy->length - offset || (!bytes && size))
        return snag_errno(EINVAL);
    if (!size) return 0;
    if (copy->fd < 0) {
        memcpy(bytes, copy->text + (size_t)offset, size);
        return 0;
    }
    size_t done = 0u;
    while (done < size) {
        ssize_t amount = snag_pread(copy->fd, (unsigned char *)bytes + done,
            size - done, (int64_t)(offset + done));
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) return snag_errno(amount < 0 ? errno : EIO);
        done += (size_t)amount;
    }
    return 0;
}

static void
state(struct snag_clipboard *copy, enum snag_clipboard_state next)
{
    atomic_store(&copy->state, next);
    snag_wakeup_send(copy->wake[1]);
}

static bool
canceled(const struct snag_clipboard *copy)
{
    return atomic_load(&copy->stop) || atomic_load(&copy->request) == REQUEST_CANCEL;
}

static int
prepare(struct snag_clipboard *copy)
{
    struct snag_sha256 hash;
    snag_sha256_init(&hash);
    unsigned char bytes[CLIPBOARD_CHUNK], utf8[4];
    size_t used = 0u, expected = 0u;
    for (uint64_t offset = 0u; offset < copy->length;) {
        if (canceled(copy)) return snag_errno(ECANCELED);
        size_t size = copy->length - offset < sizeof(bytes) ?
            (size_t)(copy->length - offset) : sizeof(bytes);
        if (snag_clipboard_read(copy, offset, bytes, size) < 0) return -1;
        for (size_t i = 0u; i < size; ++i) {
            if (!used) {
                expected = snag_utf8_size(bytes[i]);
                if (!expected || !bytes[i]) return snag_errno(EILSEQ);
            }
            utf8[used++] = bytes[i];
            if (used == expected) {
                if (!snag_utf8_valid(utf8, used, true)) return snag_errno(EILSEQ);
                used = 0u;
            }
        }
        snag_sha256_update(&hash, bytes, size);
        offset += size;
        atomic_store(&copy->progress, offset);
    }
    if (used) return snag_errno(EILSEQ);
    snag_sha256_final_hex(&hash, copy->sha256);
    return canceled(copy) ? snag_errno(ECANCELED) : 0;
}

/* JXA exposes the native pasteboard API without loading AppKit into the
 * terminal process. Source bytes enter only through stdin; script and argv
 * are fixed. This also keeps GUI allocations out of forked converters. */
static const char mac_script[] =
    "ObjC.import('AppKit');"
    "function run(a) {"
    "let d=$.NSFileHandle.fileHandleWithStandardInput.readDataToEndOfFile;"
    "let b=a.length ? $.NSPasteboard.pasteboardWithName($(a[0])) : "
    "$.NSPasteboard.generalPasteboard;"
    "b.clearContents;"
    "if (!b.setDataForType(d,$('public.utf8-plain-text'))) throw Error('clipboard write failed');"
    "}";

static enum snag_clipboard_state
helper_publish(struct snag_clipboard *copy)
{
    const char *wayland[] = {copy->backend.program, "--type", "text/plain;charset=utf-8", NULL};
    const char *x11[] = {copy->backend.program, "-selection", "clipboard", "-in",
        "-target", "UTF8_STRING", NULL};
    const char *mac[] = {"/usr/bin/osascript", "-l", "JavaScript", "-e", mac_script,
        "--", copy->backend.pasteboard, NULL};
    const char *const *argv = copy->backend.kind == SNAG_CLIPBOARD_MAC ? mac :
        copy->backend.kind == SNAG_CLIPBOARD_WAYLAND ? wayland : x11;
    struct snag_child child;
    snag_child_init(&child);
    if (snag_child_spawn_host(&child, argv) < 0) {
        (void)snprintf(copy->error, sizeof(copy->error), "Cannot start clipboard helper: %s",
            strerror(errno));
        return SNAG_CLIPBOARD_FAILED;
    }
    unsigned char bytes[CLIPBOARD_CHUNK], discard[4096];
    uint64_t offset = 0u, active = snag_monotonic_ms();
    size_t at = 0u, size = 0u;
    bool closed = false, reading[2] = {true, true};
    enum snag_clipboard_state result = SNAG_CLIPBOARD_UNCERTAIN;
    for (;;) {
        if (atomic_load(&copy->stop)) {
            (void)snprintf(copy->error, sizeof(copy->error),
                "Native publication interrupted; outcome is uncertain");
            break;
        }
        int exited = snag_child_exited(&child);
        if (exited) {
            if (exited > 0 && snag_child_reap(&child) == 0 && child.exit_code == 0 && closed) {
                result = SNAG_CLIPBOARD_WRITTEN;
            } else (void)snprintf(copy->error, sizeof(copy->error),
                "Clipboard helper failed (exit %lld, signal %d); outcome is uncertain",
                (long long)child.exit_code, child.signal_number);
            break;
        }
        if (snag_monotonic_ms() - active >= CLIPBOARD_HELPER_IDLE_MS) {
            (void)snprintf(copy->error, sizeof(copy->error),
                "Clipboard helper stopped responding; outcome is uncertain");
            break;
        }
        if (!closed && at == size) {
            if (offset == copy->length) {
                snag_child_close_stream(&child, 2u);
                closed = true;
                active = snag_monotonic_ms();
            } else {
                size = copy->length - offset < sizeof(bytes) ?
                    (size_t)(copy->length - offset) : sizeof(bytes);
                if (snag_clipboard_read(copy, offset, bytes, size) < 0) {
                    (void)snprintf(copy->error, sizeof(copy->error),
                        "Clipboard source read failed");
                    break;
                }
                at = 0u;
            }
        }
        if (!closed) {
            ssize_t n = snag_child_write(&child, bytes + at, size - at);
            if (n > 0) {
                at += (size_t)n;
                offset += (size_t)n;
                active = snag_monotonic_ms();
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                (void)snprintf(copy->error, sizeof(copy->error), "Clipboard helper input failed");
                break;
            }
        }
        /* Helpers may print selected data on failure. Drain it without copying
         * it into status text, logs, model context or another terminal stream. */
        for (unsigned int stream = 0u; stream < 2u; ++stream) {
            if (!reading[stream]) continue;
            ssize_t got = snag_child_read(&child, stream, discard, sizeof(discard));
            if (!got || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                reading[stream] = false;
                snag_child_close_stream(&child, stream);
            }
        }
        struct snag_child_event events[] = {
            {.child = &child, .stream = 0u, .events = reading[0] ? SNAG_CHILD_READ : 0u},
            {.child = &child, .stream = 1u, .events = reading[1] ? SNAG_CHILD_READ : 0u},
            {.child = &child, .stream = 2u, .events = closed ? 0u : SNAG_CHILD_WRITE}};
        if (snag_child_wait(events, 3u, copy->control[0], 25) < 0 && errno != EINTR) {
            (void)snprintf(copy->error, sizeof(copy->error), "Clipboard helper wait failed");
            break;
        }
    }
    snag_child_free(&child);
    return result;
}

static void *
worker(void *opaque)
{
    struct snag_clipboard *copy = opaque;
    enum snag_clipboard_state result = SNAG_CLIPBOARD_FAILED;
    if (prepare(copy) < 0) {
        result = errno == ECANCELED ? SNAG_CLIPBOARD_CANCELED : SNAG_CLIPBOARD_FAILED;
        if (result == SNAG_CLIPBOARD_FAILED)
            (void)snprintf(copy->error, sizeof(copy->error), "Cannot prepare clipboard text: %s",
                strerror(errno));
        goto done;
    }
    state(copy, SNAG_CLIPBOARD_READY);
    while (atomic_load(&copy->request) == REQUEST_NONE && !atomic_load(&copy->stop)) {
        (void)snag_wakeup_wait(copy->control[0], -1);
        snag_wakeup_drain(copy->control[0]);
    }
    snag_wakeup_drain(copy->control[0]);
    if (canceled(copy)) { result = SNAG_CLIPBOARD_CANCELED; goto done; }
    state(copy, SNAG_CLIPBOARD_PUBLISHING);
    if (copy->backend.kind != SNAG_CLIPBOARD_NONE) result = helper_publish(copy);
    else {
        result = SNAG_CLIPBOARD_UNAVAILABLE;
        (void)snprintf(copy->error, sizeof(copy->error),
            "No native desktop clipboard is available");
    }
done:
    state(copy, result);
    return NULL;
}

struct snag_clipboard *
snag_clipboard_open(int fd, const void *bytes, uint64_t length,
    const struct snag_clipboard_backend *backend)
{
    if (length > INT64_MAX || (fd < 0 && (length > SIZE_MAX || (!bytes && length))) ||
        (backend && (backend->kind < SNAG_CLIPBOARD_NONE || backend->kind > SNAG_CLIPBOARD_X11))) {
        errno = EINVAL;
        return NULL;
    }
    struct snag_clipboard *copy = calloc(1u, sizeof(*copy));
    if (!copy) return NULL;
    copy->fd = -1;
    copy->length = length;
    copy->wake[0] = copy->wake[1] = copy->control[0] = copy->control[1] = SNAG_WAKE_INVALID;
    atomic_init(&copy->state, SNAG_CLIPBOARD_PREPARING);
    atomic_init(&copy->request, REQUEST_NONE);
    atomic_init(&copy->stop, false);
    atomic_init(&copy->progress, 0u);
    if (backend) {
        copy->backend.kind = backend->kind;
        if (backend->program) {
            copy->backend.program = snag_strdup_checked(backend->program, SNAG_PATH_MAX_BYTES);
            if (!copy->backend.program) goto failed;
        }
        if (backend->pasteboard) {
            copy->backend.pasteboard = snag_strdup_checked(backend->pasteboard,
                SNAG_PATH_MAX_BYTES);
            if (!copy->backend.pasteboard) goto failed;
        }
    } else if (snag_clipboard_backend(&copy->backend) < 0) goto failed;
    if ((copy->backend.kind == SNAG_CLIPBOARD_WAYLAND ||
        copy->backend.kind == SNAG_CLIPBOARD_X11) &&
        (!copy->backend.program || !snag_path_root_len(copy->backend.program))) {
        errno = EINVAL;
        goto failed;
    }
    if (fd >= 0) {
        snag_file_info info;
        if (snag_fstat(fd, &info) < 0) goto failed;
        if (!S_ISREG(info.st_mode) || info.st_size < 0 || (uint64_t)info.st_size < length) {
            errno = EINVAL;
            goto failed;
        }
        copy->fd = dup(fd);
        if (copy->fd < 0 || snag_fd_cloexec(copy->fd) < 0) goto failed;
    } else {
        copy->text = malloc(length ? (size_t)length : 1u);
        if (!copy->text) goto failed;
        if (length) memcpy(copy->text, bytes, (size_t)length);
    }
    if (snag_wakeup_create(copy->wake) < 0 || snag_wakeup_create(copy->control) < 0) goto failed;
    int rc = pthread_create(&copy->worker, NULL, worker, copy);
    if (rc) { errno = rc; goto failed; }
    copy->started = true;
    return copy;
failed:
    {
        int saved = errno;
        snag_clipboard_close(copy);
        errno = saved;
    }
    return NULL;
}

snag_wake_fd
snag_clipboard_fd(const struct snag_clipboard *copy)
{
    return copy ? copy->wake[0] : SNAG_WAKE_INVALID;
}

void
snag_clipboard_result(struct snag_clipboard *copy, struct snag_clipboard_result *result)
{
    snag_wakeup_drain(copy->wake[0]);
    *result = (struct snag_clipboard_result){.state = atomic_load(&copy->state),
        .bytes = atomic_load(&copy->progress), .length = copy->length};
    if (result->state == SNAG_CLIPBOARD_READY || result->state == SNAG_CLIPBOARD_PUBLISHING ||
        result->state == SNAG_CLIPBOARD_WRITTEN || result->state == SNAG_CLIPBOARD_UNAVAILABLE ||
        result->state == SNAG_CLIPBOARD_UNCERTAIN)
        memcpy(result->sha256, copy->sha256, sizeof(result->sha256));
    if (result->state >= SNAG_CLIPBOARD_WRITTEN)
        memcpy(result->error, copy->error, sizeof(result->error));
}

int
snag_clipboard_publish(struct snag_clipboard *copy)
{
    if (atomic_load(&copy->state) != SNAG_CLIPBOARD_READY) return snag_errno(EAGAIN);
    int expected = REQUEST_NONE;
    if (!atomic_compare_exchange_strong(&copy->request, &expected, REQUEST_PUBLISH))
        return snag_errno(expected == REQUEST_CANCEL ? ECANCELED : EALREADY);
    snag_wakeup_send(copy->control[1]);
    return 0;
}

bool
snag_clipboard_cancel(struct snag_clipboard *copy)
{
    if (!copy || atomic_load(&copy->state) >= SNAG_CLIPBOARD_WRITTEN) return false;
    int expected = REQUEST_NONE;
    bool accepted = atomic_compare_exchange_strong(&copy->request, &expected, REQUEST_CANCEL);
    if (accepted) snag_wakeup_send(copy->control[1]);
    return accepted && atomic_load(&copy->state) != SNAG_CLIPBOARD_FAILED;
}

void
snag_clipboard_close(struct snag_clipboard *copy)
{
    if (!copy) return;
    atomic_store(&copy->stop, true);
    if (copy->started) {
        snag_wakeup_send(copy->control[1]);
        (void)pthread_join(copy->worker, NULL);
    }
    snag_wakeup_close(copy->wake);
    snag_wakeup_close(copy->control);
    snag_clipboard_backend_free(&copy->backend);
    if (copy->fd >= 0) (void)close(copy->fd);
    free(copy->text);
    free(copy);
}
