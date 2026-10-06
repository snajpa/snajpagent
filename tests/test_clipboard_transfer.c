/* SPDX-License-Identifier: GPL-2.0-only */
#include "clipboard_transfer.h"
#include "fs.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct snag_clipboard *
source(const void *bytes, size_t length)
{
    struct snag_clipboard_backend backend = {0};
    struct snag_clipboard *copy = snag_clipboard_open(-1, bytes, length, &backend);
    assert(copy);
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    for (;;) {
        struct snag_clipboard_result result;
        snag_clipboard_result(copy, &result);
        if (result.state == SNAG_CLIPBOARD_READY) return copy;
        assert(result.state == SNAG_CLIPBOARD_PREPARING && snag_monotonic_ms() < deadline);
        assert(snag_wakeup_wait(snag_clipboard_fd(copy), 100) >= 0);
    }
}

static int
receive(struct snag_clipboard_receive *receiver, const char *title, size_t size,
    uint64_t now, char *reply)
{
    static const char prefix[] = "\033]2;" SNAG_SCREEN_PREFIX;
    assert(size > sizeof(prefix) && !memcmp(title, prefix, sizeof(prefix) - 1u));
    assert(title[size - 1u] == '\a');
    return snag_clipboard_receive_title(receiver, title + sizeof(prefix) - 1u,
        size - sizeof(prefix), now, reply, 128u);
}

static void
begin(struct snag_clipboard_send *sender, bool stream, uint64_t *now,
    char *title, size_t *size)
{
    int n = snag_clipboard_send_output(sender, *now, title, SNAG_SCREEN_TITLE_MAX);
    unsigned int nonce;
    assert(n > 0 && sscanf(title, "\033[?9001;%un", &nonce) == 1);
    assert(snag_clipboard_send_wait(sender, *now) == 1000);
    if (stream) {
        char reply[32];
        int amount = snprintf(reply, sizeof(reply), "\033[>%uS", nonce);
        assert(snag_clipboard_send_input(sender, reply, (size_t)amount, *now));
    } else *now += 1000u;
    n = snag_clipboard_send_output(sender, *now, title, SNAG_SCREEN_TITLE_MAX);
    assert(n > 0 && strstr(title, "CLIP:BEGIN:"));
    *size = (size_t)n;
}

static void
cancel_before_begin(void)
{
    struct snag_clipboard *copy = source("prior canceled", 14u);
    uint64_t now = 100u;
    struct snag_clipboard_send *sender = snag_clipboard_send_open(copy, now);
    struct snag_clipboard_receive *receiver = snag_clipboard_receive_open(SNAG_CLIP_OSC52, NULL);
    assert(sender && receiver);
    char title[SNAG_SCREEN_TITLE_MAX], cancel[SNAG_SCREEN_TITLE_MAX], reply[128], repeated[128];
    size_t size;
    begin(sender, true, &now, title, &size);
    snag_clipboard_send_cancel(sender, now);
    int n = snag_clipboard_send_output(sender, now, cancel, sizeof(cancel));
    assert(n > 0);
    int accepted = receive(receiver, cancel, (size_t)n, now, reply);
    assert(accepted > 0 && snag_clipboard_send_input(sender, reply, (size_t)accepted, now));
    struct snag_copy_result result;
    snag_clipboard_send_result(sender, &result);
    assert(result.state == SNAG_COPY_CANCELED);
    int retry = receive(receiver, title, size, now + 1u, repeated);
    assert(retry == accepted && !memcmp(reply, repeated, (size_t)retry));
    assert(!snag_clipboard_receive_osc(receiver));
    snag_clipboard_receive_close(receiver);
    snag_clipboard_send_close(sender);
    snag_clipboard_close(copy);
}

static void
roundtrip(bool stream, bool losses, bool corrupt_digest, bool cancel, bool native,
    const char *program, const char *path)
{
    size_t length = 70003u;
    unsigned char *bytes = malloc(length);
    assert(bytes);
    memset(bytes, 'x', length);
    memcpy(bytes + 95u, "\xe7\x95\x8c", 3u);
    memcpy(bytes + 65535u, "\xf0\x9f\x91\xa9", 4u);
    memcpy(bytes, "{\\rtf1 literal}\n", 16u);
    struct snag_clipboard *copy = source(bytes, length);
    uint64_t now = snag_monotonic_ms();
    uint64_t origin = now, deadline = now + 15000u;
    struct snag_clipboard_send *sender = snag_clipboard_send_open(copy, now);
    struct snag_clipboard_backend backend = {.kind = SNAG_CLIPBOARD_WAYLAND,
        .program = (char *)program};
    struct snag_clipboard_receive *receiver = snag_clipboard_receive_open(
        native ? SNAG_CLIP_NATIVE : SNAG_CLIP_OSC52, native ? &backend : NULL);
    assert(sender && receiver);
    char title[SNAG_SCREEN_TITLE_MAX], reply[128], saved_begin[SNAG_SCREEN_TITLE_MAX];
    size_t size;
    begin(sender, stream, &now, title, &size);
    memcpy(saved_begin, title, size + 1u);
    size_t begin_size = size;
    uint64_t advance = now - origin;
    unsigned int dropped = 0u, publications = 0u;
    bool canceled = false;
    struct snag_copy_result result = {0};
    for (;;) {
        if (size) {
            assert(stream || size < 256u);
            if (corrupt_digest && strstr(title, "CLIP:COMMIT:")) {
                char *digest = strstr(title, "CLIP:COMMIT:") +
                    sizeof("CLIP:COMMIT:") - 1u + 9u + 33u;
                assert(*digest && digest[64] == ':');
                *digest = *digest == '0' ? '1' : '0';
            }
            int n = receive(receiver, title, size, now, reply);
            assert(n >= 0);
            bool drop = losses && ((dropped == 0u && strstr(title, "CLIP:BEGIN:")) ||
                (dropped == 1u && strstr(title, "CLIP:DATA:")) ||
                (dropped == 2u && strstr(title, "CLIP:COMMIT:")));
            if (drop) ++dropped;
            else if (n) assert(snag_clipboard_send_input(sender, reply, (size_t)n, now));
        }
        snag_clipboard_receive_poll(receiver, now);
        struct snag_clipboard *ready = snag_clipboard_receive_osc(receiver);
        if (ready) {
            assert(!native && !corrupt_digest && !cancel && publications++ == 0u);
            unsigned char chunk[4096];
            for (size_t at = 0u; at < length;) {
                size_t count = length - at < sizeof(chunk) ? length - at : sizeof(chunk);
                assert(snag_clipboard_read(ready, at, chunk, count) == 0);
                assert(!memcmp(chunk, bytes + at, count));
                at += count;
            }
            snag_clipboard_receive_osc_done(receiver, true);
        }
        snag_clipboard_send_result(sender, &result);
        if (result.state >= SNAG_COPY_WRITTEN) break;
        if (cancel && !canceled && result.bytes) {
            snag_clipboard_send_cancel(sender, now);
            canceled = true;
        }
        int n = snag_clipboard_send_output(sender, now, title, sizeof(title));
        assert(n >= 0);
        size = (size_t)n;
        if (!size) (void)snag_wakeup_wait(snag_clipboard_receive_fd(receiver), 1);
        now = snag_monotonic_ms() + advance;
        assert(now < deadline + advance);
    }
    assert(result.remote);
    assert(result.state == (cancel ? SNAG_COPY_CANCELED : corrupt_digest ? SNAG_COPY_FAILED :
        native ? SNAG_COPY_WRITTEN : SNAG_COPY_SEQUENCE_SENT));
    if (!cancel) assert(result.bytes == length);
    if (losses) assert(dropped == 3u);
    /* A completed operation returns its receipt, even if an old BEGIN is
     * displayed again after a lost success receipt. It cannot publish twice. */
    int n = receive(receiver, saved_begin, begin_size, now, reply);
    assert(n > 0 && !snag_clipboard_send_input(sender, reply, (size_t)n, now));
    assert(!snag_clipboard_receive_osc(receiver));
    if (native) {
        FILE *file = fopen(path, "rb");
        assert(file);
        for (size_t i = 0u; i < length; ++i) assert(fgetc(file) == bytes[i]);
        assert(fgetc(file) == EOF && fclose(file) == 0);
    }
    snag_clipboard_receive_close(receiver);
    snag_clipboard_send_close(sender);
    snag_clipboard_close(copy);
    free(bytes);
}

static void
osc_chunks(void)
{
    const char *texts[] = {"", "a", "ab", "abc", "abcdefghij"};
    const char *encoded[] = {"", "YQ==", "YWI=", "YWJj", "YWJjZGVmZ2hpag=="};
    for (size_t i = 0u; i < sizeof(texts) / sizeof(texts[0]); ++i) {
        struct snag_clipboard *copy = source(texts[i], strlen(texts[i]));
        for (size_t capacity = 16u; capacity < 40u; ++capacity) {
            struct snag_clipboard_osc osc = {.source = copy};
            char bytes[128], expected[128];
            size_t used = 0u;
            int n;
            while ((n = snag_clipboard_osc_next(&osc, bytes + used, capacity)) > 0) {
                used += (size_t)n;
                assert(used < sizeof(bytes) - capacity);
            }
            assert(n == 0 && osc.done);
            int length = snprintf(expected, sizeof(expected), "\033]52;c;%s\a", encoded[i]);
            assert(length > 0 && used == (size_t)length && !memcmp(bytes, expected, used));
        }
        snag_clipboard_close(copy);
    }
}

static void
reject_damaged_frames(void)
{
    struct snag_clipboard *copy = source("literal source", 14u);
    uint64_t now = 1u;
    struct snag_clipboard_send *sender = snag_clipboard_send_open(copy, now);
    struct snag_clipboard_receive *receiver = snag_clipboard_receive_open(SNAG_CLIP_OSC52, NULL);
    assert(sender && receiver);
    char title[SNAG_SCREEN_TITLE_MAX], reply[128], damaged[SNAG_SCREEN_TITLE_MAX];
    size_t size;
    begin(sender, true, &now, title, &size);
    int n = receive(receiver, title, size, now, reply);
    assert(n > 0);
    memcpy(damaged, reply, (size_t)n);
    /* The first decimal nonce byte remains well-formed but belongs to no copy. */
    damaged[8] = damaged[8] == '1' ? '2' : '1';
    assert(!snag_clipboard_send_input(sender, damaged, (size_t)n, now));
    assert(!snag_clipboard_send_input(sender, reply, (size_t)n - 1u, now));
    assert(snag_clipboard_send_input(sender, reply, (size_t)n, now));
    n = snag_clipboard_send_output(sender, now, title, sizeof(title));
    assert(n > 0 && strstr(title, "CLIP:DATA:"));
    size = (size_t)n;
    memcpy(damaged, title, size + 1u);
    damaged[size - 5u] = damaged[size - 5u] == 'A' ? 'B' : 'A';
    assert(receive(receiver, damaged, size, now, reply) == 0);
    static const char prefix[] = "\033]2;" SNAG_SCREEN_PREFIX;
    assert(snag_clipboard_receive_title(receiver, title + sizeof(prefix) - 1u,
        size - sizeof(prefix) - 3u, now, reply, sizeof(reply)) == 0);
    n = receive(receiver, title, size, now, reply);
    assert(n > 0 && snag_clipboard_send_input(sender, reply, (size_t)n, now));
    n = snag_clipboard_send_output(sender, now, title, sizeof(title));
    assert(n > 0 && strstr(title, "CLIP:COMMIT:"));
    size = (size_t)n;
    n = receive(receiver, title, size, now, reply);
    assert(n > 0 && snag_clipboard_send_input(sender, reply, (size_t)n, now));
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    struct snag_clipboard *verified = NULL;
    while (!(verified = snag_clipboard_receive_osc(receiver))) {
        snag_clipboard_receive_poll(receiver, now);
        assert(snag_monotonic_ms() < deadline);
        (void)snag_wakeup_wait(snag_clipboard_receive_fd(receiver), 1);
    }
    assert(!snag_clipboard_receive_osc(receiver));
    /* Publication has begun. Cancellation must not claim the previous
     * clipboard was preserved, and a lost publication result is uncertain. */
    snag_clipboard_send_cancel(sender, now);
    n = snag_clipboard_send_output(sender, now, title, sizeof(title));
    assert(n > 0);
    size = (size_t)n;
    n = receive(receiver, title, size, now, reply);
    assert(n > 0 && snag_clipboard_send_input(sender, reply, (size_t)n, now));
    struct snag_copy_result result;
    snag_clipboard_send_result(sender, &result);
    assert(result.state == SNAG_COPY_CANCELING);
    snag_clipboard_receive_osc_done(receiver, false);
    n = receive(receiver, title, size, now, reply);
    assert(n > 0 && snag_clipboard_send_input(sender, reply, (size_t)n, now));
    snag_clipboard_send_result(sender, &result);
    assert(result.state == SNAG_COPY_UNCERTAIN);
    snag_clipboard_receive_close(receiver);
    snag_clipboard_send_close(sender);
    snag_clipboard_close(copy);
}

static void
unavailable(void)
{
    struct snag_clipboard *copy = source("text", 4u);
    for (unsigned int off = 0u; off < 2u; ++off) {
        uint64_t now = 1u;
        struct snag_clipboard_send *sender = snag_clipboard_send_open(copy, now);
        assert(sender);
        char title[SNAG_SCREEN_TITLE_MAX], reply[128];
        size_t size;
        begin(sender, false, &now, title, &size);
        if (off) {
            struct snag_clipboard_receive *receiver =
                snag_clipboard_receive_open(SNAG_CLIP_OFF, NULL);
            assert(receiver);
            int n = receive(receiver, title, size, now, reply);
            assert(n > 0 && snag_clipboard_send_input(sender, reply, (size_t)n, now));
            snag_clipboard_receive_close(receiver);
        } else assert(snag_clipboard_send_output(sender, now + 1500u, title, sizeof(title)) == 0);
        struct snag_copy_result result;
        snag_clipboard_send_result(sender, &result);
        assert(result.state == SNAG_COPY_UNAVAILABLE && result.remote == (off != 0u));
        snag_clipboard_send_close(sender);
    }
    snag_clipboard_close(copy);
}

int
main(int argc, char **argv)
{
    snag_ignore_sigpipe();
#ifndef _WIN32
    if (argc > 1) {
        assert(argc == 3 && !strcmp(argv[1], "--type") &&
            !strcmp(argv[2], "text/plain;charset=utf-8"));
        const char *path = getenv("SNAJPAGENT_TEST_COPY_PATH");
        assert(path);
        FILE *file = fopen(path, "wb");
        assert(file);
        int c;
        while ((c = getchar()) != EOF) assert(fputc(c, file) == c);
        assert(!ferror(stdin) && fclose(file) == 0);
        return 0;
    }
#endif
    osc_chunks();
    reject_damaged_frames();
    cancel_before_begin();
    unavailable();
    roundtrip(true, false, false, false, false, NULL, NULL);
    roundtrip(false, true, false, false, false, NULL, NULL);
    roundtrip(true, false, true, false, false, NULL, NULL);
    roundtrip(false, false, false, true, false, NULL, NULL);
#ifndef _WIN32
    char path[] = "/tmp/snag-copy-transfer-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && close(fd) == 0);
    assert(setenv("SNAJPAGENT_TEST_COPY_PATH", path, 1) == 0);
    char *program = snag_program_path(argv[0]);
    assert(program);
    roundtrip(true, true, false, false, true, program, path);
    assert(unlink(path) == 0 && unsetenv("SNAJPAGENT_TEST_COPY_PATH") == 0);
    free(program);
#else
    (void)argc;
    (void)argv;
#endif
    (void)puts("test_clipboard_transfer: ok");
    return 0;
}
