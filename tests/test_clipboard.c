/* SPDX-License-Identifier: GPL-2.0-only */
#include "clipboard.h"
#include "fs.h"
#include "process_host.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
#include <sys/resource.h>
#endif

#ifdef __APPLE__
#include <ApplicationServices/ApplicationServices.h>
#endif

static struct snag_clipboard_result
wait_state(struct snag_clipboard *copy, enum snag_clipboard_state wanted)
{
    uint64_t start = snag_monotonic_ms();
    for (;;) {
        struct snag_clipboard_result result;
        snag_clipboard_result(copy, &result);
        if (result.state == wanted) return result;
        if (result.state >= SNAG_CLIPBOARD_WRITTEN) {
            (void)fprintf(stderr, "clipboard state %d, wanted %d: %s\n",
                result.state, wanted, result.error);
            abort();
        }
        assert(snag_monotonic_ms() - start < 5000u);
        assert(snag_wakeup_wait(snag_clipboard_fd(copy), 100) >= 0);
    }
}

#ifndef _WIN32
static void
read_exact(const char *path, const void *expected, size_t size)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    unsigned char bytes[4096];
    size_t offset = 0u;
    while (offset < size) {
        size_t amount = size - offset < sizeof(bytes) ? size - offset : sizeof(bytes);
        assert(fread(bytes, 1u, amount, file) == amount);
        assert(!memcmp(bytes, (const unsigned char *)expected + offset, amount));
        offset += amount;
    }
    assert(fgetc(file) == EOF && !ferror(file));
    assert(fclose(file) == 0);
}

static int
helper(int argc, char **argv)
{
    assert((argc == 3 && !strcmp(argv[1], "--type") &&
        !strcmp(argv[2], "text/plain;charset=utf-8")) ||
        (argc == 6 && !strcmp(argv[1], "-selection") && !strcmp(argv[2], "clipboard") &&
        !strcmp(argv[3], "-in") && !strcmp(argv[4], "-target") && !strcmp(argv[5], "UTF8_STRING")));
    char *directory = snag_realpath(".");
    const char *expected = getenv("SNAJPAGENT_TEST_CLIPBOARD_CWD");
    assert(directory && expected && !strcmp(directory, expected));
    free(directory);
#ifndef _WIN32
    struct rlimit cpu, files;
    assert(getrlimit(RLIMIT_CPU, &cpu) == 0 && getrlimit(RLIMIT_FSIZE, &files) == 0);
    char limits[128];
    (void)snprintf(limits, sizeof(limits), "%llu:%llu:%llu:%llu",
        (unsigned long long)cpu.rlim_cur, (unsigned long long)cpu.rlim_max,
        (unsigned long long)files.rlim_cur, (unsigned long long)files.rlim_max);
    assert(!strcmp(limits, getenv("SNAJPAGENT_TEST_CLIPBOARD_LIMITS")));
#endif
    const char *mode = getenv("SNAJPAGENT_TEST_CLIPBOARD_MODE");
    if (mode && !strcmp(mode, "stall")) {
        for (;;) (void)snag_wakeup_wait(SNAG_WAKE_INVALID, 100);
    }
    if (mode && !strcmp(mode, "fail")) {
        (void)fputs("must-never-appear-in-ui\033]52;c;bad\a", stderr);
        return 7;
    }
    struct snag_buf bytes = {.max = SIZE_MAX};
    assert(snag_buf_read(&bytes, STDIN_FILENO) == 0);
    const char *path = getenv("SNAJPAGENT_TEST_CLIPBOARD_OUTPUT");
    assert(path);
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(bytes.data, 1u, bytes.len, file) == bytes.len);
    assert(fclose(file) == 0);
    snag_buf_free(&bytes);
    return 0;
}

#endif /* !_WIN32 */

static void
prepared(struct snag_clipboard *copy, const void *bytes, size_t size)
{
    assert(copy);
    struct snag_clipboard_result result = wait_state(copy, SNAG_CLIPBOARD_READY);
    assert(result.bytes == size && result.length == size && !*result.error);
    char hash[SNAG_SHA256_HEX_LEN + 1u];
    snag_sha256_hex(bytes, size, hash);
    assert(!strcmp(hash, result.sha256));
    unsigned char chunk[1024];
    for (size_t at = 0u; at < size;) {
        size_t count = size - at < sizeof(chunk) ? size - at : sizeof(chunk);
        assert(snag_clipboard_read(copy, at, chunk, count) == 0);
        assert(!memcmp(chunk, (const unsigned char *)bytes + at, count));
        at += count;
    }
    assert(snag_clipboard_read(copy, size, chunk, 1u) < 0 && errno == EINVAL);
}

static void
native(void)
{
#ifdef __APPLE__
    char id[SNAG_ID_HEX_LEN + 1u], name[96];
    assert(snag_random_id(id) == 0);
    (void)snprintf(name, sizeof(name), "net.snajpa.snajpagent.clipboard-test.%s", id);
    CFStringRef board_name = CFStringCreateWithCString(kCFAllocatorDefault, name,
        kCFStringEncodingUTF8);
    PasteboardRef board = NULL;
    assert(board_name && PasteboardCreate(board_name, &board) == noErr);
    struct snag_clipboard_backend backend = {.kind = SNAG_CLIPBOARD_MAC, .pasteboard = name};
    const char *texts[] = {"{\\rtf1 literal, not rich text} e\xcc\x81 \xe7\x95\x8c\n",
        "%!PS-Adobe-3.0\n$(touch must-not-run) `literal`\n", ""};
    for (size_t i = 0u; i < sizeof(texts) / sizeof(texts[0]); ++i) {
        struct snag_clipboard *copy = snag_clipboard_open(-1, texts[i], strlen(texts[i]), &backend);
        prepared(copy, texts[i], strlen(texts[i]));
        assert(snag_clipboard_publish(copy) == 0);
        (void)wait_state(copy, SNAG_CLIPBOARD_WRITTEN);
        assert(!snag_clipboard_cancel(copy));
        snag_clipboard_close(copy);
        (void)PasteboardSynchronize(board);
        PasteboardItemID item;
        CFDataRef data = NULL;
        assert(PasteboardGetItemIdentifier(board, 1, &item) == noErr);
        assert(PasteboardCopyItemFlavorData(board, item, CFSTR("public.utf8-plain-text"),
            &data) == noErr);
        assert((size_t)CFDataGetLength(data) == strlen(texts[i]));
        assert(!memcmp(CFDataGetBytePtr(data), texts[i], strlen(texts[i])));
        CFRelease(data);
        CFArrayRef types = NULL;
        assert(PasteboardCopyItemFlavors(board, item, &types) == noErr);
        for (CFIndex n = 0; n < CFArrayGetCount(types); ++n) {
            const void *type = CFArrayGetValueAtIndex(types, n);
            assert(!CFEqual(type, CFSTR("public.rtf")));
            assert(!CFEqual(type, CFSTR("com.adobe.encapsulated-postscript")));
        }
        CFRelease(types);
    }
    CFRelease(board);
    CFRelease(board_name);
#endif
}

int
main(int argc, char **argv)
{
    snag_ignore_sigpipe();
#ifdef _WIN32
    (void)argc;
    (void)argv;
    const char text[] = "literal e\xcc\x81 \xe7\x95\x8c";
    struct snag_clipboard_backend absent = {0};
    for (unsigned int cancel = 0u; cancel < 2u; ++cancel) {
        struct snag_clipboard *copy = snag_clipboard_open(-1, text, sizeof(text) - 1u, &absent);
        prepared(copy, text, sizeof(text) - 1u);
        if (cancel) assert(snag_clipboard_cancel(copy));
        else assert(snag_clipboard_publish(copy) == 0);
        (void)wait_state(copy, cancel ? SNAG_CLIPBOARD_CANCELED : SNAG_CLIPBOARD_UNAVAILABLE);
        snag_clipboard_close(copy);
    }
    native();
    (void)puts("test_clipboard: portable worker ok; native backend unavailable");
    return 0;
#else
    if (argc == 2 && !strcmp(argv[1], "--converter-limits")) {
        struct rlimit cpu, files;
        assert(getrlimit(RLIMIT_CPU, &cpu) == 0 && getrlimit(RLIMIT_FSIZE, &files) == 0);
        assert(cpu.rlim_cur == 60u && cpu.rlim_max == 60u);
        assert(files.rlim_cur == 32u << 20 && files.rlim_max == 32u << 20);
        return 0;
    }
    if (argc > 1) return helper(argc, argv);
    char *directory = snag_realpath(".");
    assert(directory && setenv("SNAJPAGENT_TEST_CLIPBOARD_CWD", directory, 1) == 0);
#ifndef _WIN32
    struct rlimit cpu, files;
    assert(getrlimit(RLIMIT_CPU, &cpu) == 0 && getrlimit(RLIMIT_FSIZE, &files) == 0);
    char limits[128];
    (void)snprintf(limits, sizeof(limits), "%llu:%llu:%llu:%llu",
        (unsigned long long)cpu.rlim_cur, (unsigned long long)cpu.rlim_max,
        (unsigned long long)files.rlim_cur, (unsigned long long)files.rlim_max);
    assert(setenv("SNAJPAGENT_TEST_CLIPBOARD_LIMITS", limits, 1) == 0);
#endif
    char path[] = "/tmp/snag-clipboard-XXXXXX";
    int output = mkstemp(path);
    assert(output >= 0 && write(output, "prior", 5u) == 5 && close(output) == 0);
    assert(setenv("SNAJPAGENT_TEST_CLIPBOARD_OUTPUT", path, 1) == 0);
    char *program = snag_program_path(argv[0]);
    assert(program);
#ifndef _WIN32
    char **environment = snag_environment_entries();
    const char *converter[] = {program, "--converter-limits", NULL};
    struct snag_child child;
    snag_child_init(&child);
    assert(environment && snag_child_spawn_argv(&child, converter, directory, environment) == 0);
    uint64_t start = snag_monotonic_ms();
    while (!snag_child_exited(&child)) {
        assert(snag_monotonic_ms() - start < 5000u);
        (void)snag_child_wait(NULL, 0u, SNAG_WAKE_INVALID, 10);
    }
    assert(snag_child_reap(&child) == 0);
    if (child.exit_code != 0) {
        char problem[1024];
        ssize_t length = snag_child_read(&child, 1u, problem, sizeof(problem));
        (void)fprintf(stderr, "converter exit %lld signal %d: %.*s\n",
            (long long)child.exit_code, child.signal_number,
            (int)(length > 0 ? length : 0), problem);
    }
    assert(child.exit_code == 0);
    snag_child_free(&child);
    snag_environment_entries_free(environment);
#endif
    struct snag_clipboard_backend backend = {.kind = SNAG_CLIPBOARD_WAYLAND, .program = program};
    char text[] = "{\\rtf1 literal}\n$(touch nope) `literal` e\xcc\x81 \xe7\x95\x8c\n";
    struct snag_clipboard *copy = snag_clipboard_open(-1, text, strlen(text), &backend);
    prepared(copy, text, strlen(text));
    read_exact(path, "prior", 5u);
    assert(snag_clipboard_cancel(copy));
    assert(snag_clipboard_publish(copy) < 0);
    (void)wait_state(copy, SNAG_CLIPBOARD_CANCELED);
    snag_clipboard_close(copy);
    read_exact(path, "prior", 5u);
    copy = snag_clipboard_open(-1, text, strlen(text), &backend);
    prepared(copy, text, strlen(text));
    assert(snag_clipboard_publish(copy) == 0);
    (void)wait_state(copy, SNAG_CLIPBOARD_WRITTEN);
    assert(!snag_clipboard_cancel(copy));
    snag_clipboard_close(copy);
    read_exact(path, text, strlen(text));
    const unsigned char invalid[][5] = {
        {'a', 0, 'b'}, {0xf0, 0x90}, {0xc0, 0x80}, {0xed, 0xa0, 0x80}};
    const size_t lengths[] = {3u, 2u, 2u, 3u};
    for (size_t i = 0u; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        copy = snag_clipboard_open(-1, invalid[i], lengths[i], &backend);
        assert(copy);
        (void)wait_state(copy, SNAG_CLIPBOARD_FAILED);
        assert(!snag_clipboard_cancel(copy));
        snag_clipboard_close(copy);
        read_exact(path, text, strlen(text));
    }
    size_t size = 196608u;
    unsigned char *large = malloc(size);
    assert(large);
    memset(large, 'x', size);
    memcpy(large + 65535u, "\xf0\x9f\x91\xa9", 4u);
    memcpy(large + 131071u, "\xe7\x95\x8c", 3u);
    FILE *source = tmpfile();
    assert(source && fwrite(large, 1u, size, source) == size && fflush(source) == 0);
    backend.kind = SNAG_CLIPBOARD_X11;
    copy = snag_clipboard_open(fileno(source), NULL, size, &backend);
    assert(fclose(source) == 0);
    prepared(copy, large, size);
    assert(snag_clipboard_publish(copy) == 0);
    (void)wait_state(copy, SNAG_CLIPBOARD_WRITTEN);
    snag_clipboard_close(copy);
    read_exact(path, large, size);
    free(large);
    assert(setenv("SNAJPAGENT_TEST_CLIPBOARD_MODE", "fail", 1) == 0);
    copy = snag_clipboard_open(-1, text, strlen(text), &backend);
    prepared(copy, text, strlen(text));
    assert(snag_clipboard_publish(copy) == 0);
    struct snag_clipboard_result failed = wait_state(copy, SNAG_CLIPBOARD_UNCERTAIN);
    assert(!strstr(failed.error, "must-never-appear"));
    snag_clipboard_close(copy);
    assert(setenv("SNAJPAGENT_TEST_CLIPBOARD_MODE", "stall", 1) == 0);
    copy = snag_clipboard_open(-1, text, strlen(text), &backend);
    prepared(copy, text, strlen(text));
    assert(snag_clipboard_publish(copy) == 0);
    (void)wait_state(copy, SNAG_CLIPBOARD_PUBLISHING);
    assert(!snag_clipboard_cancel(copy));
    uint64_t stopping = snag_monotonic_ms();
    snag_clipboard_close(copy);
    assert(snag_monotonic_ms() - stopping < 1000u);
    assert(unsetenv("SNAJPAGENT_TEST_CLIPBOARD_MODE") == 0);
    struct snag_clipboard_backend absent = {0};
    copy = snag_clipboard_open(-1, text, strlen(text), &absent);
    prepared(copy, text, strlen(text));
    assert(snag_clipboard_publish(copy) == 0);
    (void)wait_state(copy, SNAG_CLIPBOARD_UNAVAILABLE);
    snag_clipboard_close(copy);
    native();
    free(directory);
    assert(unsetenv("SNAJPAGENT_TEST_CLIPBOARD_CWD") == 0);
    assert(unlink(path) == 0);
    assert(unsetenv("SNAJPAGENT_TEST_CLIPBOARD_OUTPUT") == 0);
    free(program);
    (void)puts("test_clipboard: ok");
    return 0;
#endif /* _WIN32 */
}
