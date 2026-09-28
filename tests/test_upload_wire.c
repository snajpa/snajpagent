/* SPDX-License-Identifier: GPL-2.0-only */
#include "base64.h"
#include "upload.h"
#include "upload_md5.h"
#include "upload_wire.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int
receive_adapter(const char *stage, const char *receipt)
{
    int fd = open("/dev/tty", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    int stage_fd = snag_open_read(stage, true);
    struct termios saved;
    if (fd < 0 || stage_fd < 0 || tcgetattr(fd, &saved) < 0) return 2;

    struct termios raw = saved;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON | IXOFF);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &raw) < 0) return 2;
    (void)write(fd, "ADAPTER_READY\r\n", sizeof("ADAPTER_READY\r\n") - 1u);

    char line[8] = {0};
    size_t used = 0;
    bool ctrl_c = false;
    uint64_t deadline = snag_monotonic_ms() + 3000u;
    while (snag_monotonic_ms() < deadline) {
        struct pollfd event = {.fd = fd, .events = POLLIN};
        int ready = poll(&event, 1u, 100);
        if (ready <= 0) continue;
        unsigned char byte;
        if (read(fd, &byte, 1u) != 1) continue;
        if (byte == 0x03u) {
            ctrl_c = true;
            used = 0;
        } else if (ctrl_c && byte == '\r') {
            break;
        } else if (ctrl_c && used + 1u < sizeof(line)) {
            line[used++] = (char)byte;
        }
    }
    bool directory = !strcmp(line, "trz -d");
    int rc = 2;
    char error[256] = {0};
    struct snag_upload_result result = {0};
    if (!strcmp(line, "trz") || directory) {
        rc = snag_upload_receive(fd, stage_fd, SNAG_UPLOAD_FILES_MAX, directory,
                                  NULL,
                                 NULL, NULL, &result, error, sizeof(error));
    } else {
        (void)snprintf(error, sizeof(error), "unexpected launch: %s", line);
    }
    if (rc == 0) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 500000000};
        (void)nanosleep(&delay, NULL);
    }
    if (tcsetattr(fd, TCSANOW, &saved) < 0) rc = 3;
    int out = open(receipt, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out >= 0) {
        (void)dprintf(out, "status=%d count=%zu tail=%zu error=%s\n", rc,
                      result.count, result.tail_len, error);
        (void)close(out);
    }
    (void)close(stage_fd);
    (void)close(fd);
    return rc == 0 ? 0 : 1;
}
#endif /* !_WIN32 */

static int
append_encoded(void *opaque, const unsigned char *bytes, size_t length)
{
    return snag_buf_append(opaque, bytes, length);
}

static void
raw_encoded(struct snag_buf *out, const void *bytes, size_t length)
{
    struct snag_base64_stream encoder = {0};
    assert(snag_base64_write(&encoder, bytes, length, append_encoded, out) == 0);
    assert(snag_base64_finish(&encoder, append_encoded, out) == 0);
}

static void
roundtrip(size_t length)
{
    unsigned char source[SNAG_UPLOAD_BLOCK_MAX];
    unsigned char decoded[SNAG_UPLOAD_BLOCK_MAX];
    struct snag_buf wire;
    size_t written = 0;

    for (size_t i = 0; i < length; ++i) source[i] = (unsigned char)(i * 127u + i / 17u);
    snag_buf_init(&wire, SNAG_UPLOAD_LINE_MAX);
    assert(snag_upload_wire_encode(source, length, &wire) == 0);
    assert(snag_upload_wire_decode((const char *)wire.data, wire.len, decoded,
                                   sizeof(decoded), &written) == 0);
    assert(written == length);
    assert(memcmp(source, decoded, length) == 0);
    snag_buf_free(&wire);
}

static void
check_digest(const char *text, const char *expected)
{
    struct snag_upload_md5 hash;
    unsigned char digest[16];
    char actual[33];

    snag_upload_md5_init(&hash);
    for (size_t i = 0; text[i]; ++i) snag_upload_md5_update(&hash, text + i, 1u);
    snag_upload_md5_finish(&hash, digest);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        static const char digits[] = "0123456789abcdef";
        actual[2u * i] = digits[digest[i] >> 4];
        actual[2u * i + 1u] = digits[digest[i] & 15u];
    }
    actual[32] = '\0';
    assert(strcmp(actual, expected) == 0);
}

int
main(int argc, char **argv)
{
#ifndef _WIN32
    if (argc == 4 && !strcmp(argv[1], "--receive")) return receive_adapter(argv[2], argv[3]);
#endif
    assert(argc == 1);
    (void)argv;
    unsigned char decoded[SNAG_UPLOAD_BLOCK_MAX];
    size_t written = 0;
    struct snag_buf wire;

    roundtrip(0u);
    roundtrip(1u);
    roundtrip(2u);
    roundtrip(257u);
    roundtrip(SNAG_UPLOAD_BLOCK_MAX);
    check_digest("", "d41d8cd98f00b204e9800998ecf8427e");
    check_digest("a", "0cc175b9c0f1b6a831c399e269772661");
    check_digest("abc", "900150983cd24fb0d6963f7d28e17f72");
    check_digest("message digest", "f96b697d7cb7938d525a2f31aaf161d0");
    check_digest("abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b");
    check_digest("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
                 "d174ab98d277d9f5a5611c2c9f419d9f");
    check_digest("12345678901234567890123456789012345678901234567890123456789012345678901234567890",
                 "57edf4a22be3c955ac49da2e2107b67a");

    snag_buf_init(&wire, SNAG_UPLOAD_LINE_MAX);
    assert(snag_upload_wire_encode("abcdefghijk", 11u, &wire) == 0);
    assert(snag_upload_wire_decode((const char *)wire.data, wire.len, decoded,
                                   10u, &written) < 0 && errno == EOVERFLOW);
    assert(snag_upload_wire_decode("!!!!", 4u, decoded, sizeof(decoded), &written) < 0);
    assert(snag_upload_wire_decode("YW==", 4u, decoded, sizeof(decoded), &written) < 0);
    assert(snag_upload_wire_decode("YQ=A", 4u, decoded, sizeof(decoded), &written) < 0);
    assert(snag_upload_wire_decode("YQ==\n", 5u, decoded, sizeof(decoded), &written) < 0);
    assert(snag_upload_wire_decode((const char *)wire.data, wire.len - 4u, decoded,
                                   sizeof(decoded), &written) < 0);

    uLongf size = compressBound(4u);
    unsigned char compressed[64];
    assert(compress2(compressed, &size, (const Bytef *)"data", 4u, Z_DEFAULT_COMPRESSION) == Z_OK);
    snag_buf_reset(&wire);
    raw_encoded(&wire, compressed, (size_t)size - 1u);
    assert(snag_upload_wire_decode((const char *)wire.data, wire.len, decoded,
                                   sizeof(decoded), &written) < 0);
    snag_buf_reset(&wire);
    compressed[size++] = 0;
    raw_encoded(&wire, compressed, (size_t)size);
    assert(snag_upload_wire_decode((const char *)wire.data, wire.len, decoded,
                                   sizeof(decoded), &written) < 0);
    snag_buf_free(&wire);
    return 0;
}
