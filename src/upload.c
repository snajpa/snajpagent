/* SPDX-License-Identifier: GPL-2.0-only */
#include "upload.h"
#include "json.h"
#include "media.h"
#include "upload_md5.h"
#include "upload_wire.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
int
snag_upload_receive(int tty, int stage_fd, size_t slots, bool directory,
                    int (*checkpoint)(void *), void *opaque,
                    struct snag_upload_result *result, char *error, size_t error_size)
{
    (void)tty;
    (void)stage_fd;
    (void)slots;
    (void)directory;
    (void)checkpoint;
    (void)opaque;
    (void)result;
    return snag_fail(error, error_size, ENOTSUP, "terminal upload is unavailable on this host");
}

void
snag_upload_cleanup(int stage_fd, struct snag_upload_result *result)
{
    (void)stage_fd;
    (void)result;
}
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#define SNAG_UPLOAD_FRAME_TIMEOUT_MS 20000u
#define SNAG_UPLOAD_READ_CHUNK 4096u

struct upload_io {
    int fd;
    int stage_fd;
    int (*checkpoint)(void *);
    void *opaque;
    unsigned char input[SNAG_UPLOAD_READ_CHUNK];
    size_t at;
    size_t len;
};

struct upload_frame {
    char type[5];
    struct snag_buf payload;
};

static int
wait_ready(struct upload_io *io, short events, uint64_t deadline)
{
    for (;;) {
        if (io->checkpoint && io->checkpoint(io->opaque)) return snag_errno(ECANCELED);
        uint64_t now = snag_monotonic_ms();
        if (now >= deadline) return snag_errno(ETIMEDOUT);
        int timeout = (int)(deadline - now > 50u ? 50u : deadline - now);
        struct pollfd pollfd = {.fd = io->fd, .events = events};
        int rc = poll(&pollfd, 1u, timeout);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) return -1;
        if (rc && (pollfd.revents & events)) return 0;
        if (rc && (pollfd.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            return snag_errno(EIO);
        }
    }
}

static int
write_bytes(struct upload_io *io, const unsigned char *bytes, size_t length)
{
    uint64_t deadline = snag_monotonic_ms() + SNAG_UPLOAD_FRAME_TIMEOUT_MS;
    while (length) {
        if (wait_ready(io, POLLOUT, deadline) < 0) return -1;
        ssize_t amount = write(io->fd, bytes, length);
        if (amount < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (amount <= 0) return amount < 0 ? -1 : snag_errno(EIO);
        bytes += (size_t)amount;
        length -= (size_t)amount;
    }
    return 0;
}

static int
read_byte(struct upload_io *io, uint64_t deadline, unsigned char *byte)
{
    while (io->at == io->len) {
        if (wait_ready(io, POLLIN, deadline) < 0) return -1;
        ssize_t amount = read(io->fd, io->input, sizeof(io->input));
        if (amount < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (amount <= 0) return amount < 0 ? -1 : snag_errno(EPIPE);
        io->at = 0;
        io->len = (size_t)amount;
    }
    *byte = io->input[io->at++];
    return 0;
}

static int
read_frame(struct upload_io *io, struct upload_frame *frame)
{
    uint64_t deadline = snag_monotonic_ms() + SNAG_UPLOAD_FRAME_TIMEOUT_MS;
    struct snag_buf line;
    snag_buf_init(&line, SNAG_UPLOAD_LINE_MAX + 8u);
    int rc = -1;

    for (;;) {
        unsigned char byte;
        if (read_byte(io, deadline, &byte) < 0) goto done;
        if (byte == '\n') break;
        if (snag_buf_putc(&line, byte) < 0) goto done;
    }
    size_t colon = 0;
    if (line.len < 3u || line.data[0] != '#') {
        errno = EPROTO;
        goto done;
    }
    for (colon = 1u; colon < line.len && colon <= 5u && line.data[colon] != ':';
         ++colon) {}
    if (colon <= 1u || colon > 5u || colon == line.len || line.data[colon] != ':') {
        errno = EPROTO;
        goto done;
    }
    memcpy(frame->type, line.data + 1u, colon - 1u);
    frame->type[colon - 1u] = '\0';
    snag_buf_reset(&frame->payload);
    if (snag_buf_append(&frame->payload, line.data + colon + 1u,
                        line.len - colon - 1u) < 0) goto done;
    rc = 0;
done:
    snag_buf_free(&line);
    return rc;
}

static int
read_expected(struct upload_io *io, struct upload_frame *frame, const char *type)
{
    if (read_frame(io, frame) < 0) return -1;
    if (!strcmp(frame->type, "fail") || !strcmp(frame->type, "FAIL")) {
        return snag_errno(ECANCELED);
    }
    if (strcmp(frame->type, type)) return snag_errno(EPROTO);
    return 0;
}

static int
read_integer(struct upload_io *io, struct upload_frame *frame,
             const char *type, uint64_t *value)
{
    if (read_expected(io, frame, type) < 0) return -1;
    if (!frame->payload.len || frame->payload.len > 20u) return snag_errno(EPROTO);
    uint64_t n = 0;

    for (size_t i = 0; i < frame->payload.len; ++i) {
        unsigned char byte = frame->payload.data[i];
        if (byte < '0' || byte > '9' || n > ((uint64_t)INT64_MAX - (uint64_t)(byte - '0')) / 10u) {
            return snag_errno(EPROTO);
        }
        n = 10u * n + (byte - '0');
    }
    *value = n;
    return 0;
}

static int
send_integer(struct upload_io *io, uint64_t value)
{
    char text[40];
    int length = snprintf(text, sizeof(text), "#SUCC:%" PRIu64 "\n", value);
    if (length < 0 || (size_t)length >= sizeof(text)) return snag_errno(EOVERFLOW);
    return write_bytes(io, (const unsigned char *)text, (size_t)length);
}

static int
send_encoded(struct upload_io *io, const char *type, const void *data, size_t length)
{
    struct snag_buf frame;
    snag_buf_init(&frame, SNAG_UPLOAD_LINE_MAX + 8u);
    int rc = -1;

    if (snag_buf_printf(&frame, "#%s:", type) < 0 ||
        snag_upload_wire_encode(data, length, &frame) < 0 ||
        snag_buf_putc(&frame, '\n') < 0) goto done;
    rc = write_bytes(io, frame.data, frame.len);
done:
    snag_buf_free(&frame);
    return rc;
}

static int
decode_payload(struct upload_frame *frame, unsigned char *out,
               size_t capacity, size_t *written)
{
    return snag_upload_wire_decode((const char *)frame->payload.data,
                                   frame->payload.len, out, capacity, written);
}

static int
receive_action(struct upload_io *io, struct upload_frame *frame, bool *cancelled)
{
    unsigned char decoded[16384];
    size_t length;
    char parse_error[128] = {0};

    if (read_expected(io, frame, "ACT") < 0 ||
        decode_payload(frame, decoded, sizeof(decoded), &length) < 0) return -1;
    json_t *action = snag_json_load_strict(decoded, length, sizeof(decoded),
                                          parse_error, sizeof(parse_error));
    if (!action) return snag_errno(EPROTO);

    json_t *confirm = json_object_get(action, "confirm");
    json_t *protocol = json_object_get(action, "protocol");
    json_t *newline = json_object_get(action, "newline");
    const char *ending = json_string_value(newline);
    bool valid = json_is_object(action) && json_is_boolean(confirm) &&
                 json_is_integer(protocol) && json_integer_value(protocol) >= 1 &&
                 ending && !strcmp(ending, "\n") &&
                 !json_is_true(json_object_get(action, "tunnel")) &&
                 !json_is_true(json_object_get(action, "tmuxcc"));
    *cancelled = json_is_false(confirm);
    json_decref(action);
    return valid ? 0 : snag_errno(EPROTO);
}

static int
receive_name(struct upload_io *io, struct upload_frame *frame,
             struct snag_upload_file *file)
{
    unsigned char bytes[SNAG_NAME_MAX_BYTES + 1u];
    size_t length;
    file->fd = -1;

    if (read_expected(io, frame, "NAME") < 0 ||
        decode_payload(frame, bytes, SNAG_NAME_MAX_BYTES, &length) < 0) return -1;
    if (!length || !snag_utf8_valid(bytes, length, true) ||
        (length == 1u && bytes[0] == '.') ||
        (length == 2u && bytes[0] == '.' && bytes[1] == '.') ||
        (length > 1u && bytes[1] == ':')) return snag_errno(EPROTO);
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] == '/' || bytes[i] == '\\') return snag_errno(EPROTO);
    }
    memcpy(file->name, bytes, length);
    file->name[length] = '\0';

    for (unsigned int attempt = 0; attempt < 4u; ++attempt) {
        char id[SNAG_ID_HEX_LEN + 1u];
        if (snag_random_id(id) < 0) return -1;
        /* The caller supplies a private, operation-owned staging directory. */
        int fd = snag_create_output_at(io->stage_fd, id);
        if (fd >= 0) {
            memcpy(file->leaf, id, sizeof(file->leaf));
            file->fd = fd;
            return send_encoded(io, "SUCC", file->leaf, strlen(file->leaf));
        }
        if (errno != EEXIST) return -1;
    }
    return snag_errno(EEXIST);
}

static int
receive_file(struct upload_io *io, struct upload_frame *frame,
             struct snag_upload_file *file)
{
    if (receive_name(io, frame, file) < 0) return -1;

    uint64_t size;
    if (read_integer(io, frame, "SIZE", &size) < 0) return -1;
    if (!size || size > SNAG_MEDIA_FILE_MAX) return snag_errno(EFBIG);
    file->bytes = size;
    if (send_integer(io, size) < 0) return -1;

    struct snag_upload_md5 hash;
    snag_upload_md5_init(&hash);
    uint64_t received = 0;
    unsigned char data[SNAG_UPLOAD_BLOCK_MAX];

    while (received < size) {
        size_t length;
        if (read_expected(io, frame, "DATA") < 0 ||
            decode_payload(frame, data, sizeof(data), &length) < 0) return -1;
        if (!length || length > size - received) return snag_errno(EPROTO);

        size_t offset = 0;
        while (offset < length) {
            ssize_t amount = write(file->fd, data + offset, length - offset);
            if (amount < 0 && errno == EINTR) continue;
            if (amount <= 0) return amount < 0 ? -1 : snag_errno(EIO);
            offset += (size_t)amount;
        }
        snag_upload_md5_update(&hash, data, length);
        received += length;
        if (send_integer(io, length) < 0) return -1;
    }

    unsigned char digest[16];
    unsigned char announced[16];
    size_t length;
    snag_upload_md5_finish(&hash, digest);
    if (read_expected(io, frame, "MD5") < 0 ||
        decode_payload(frame, announced, sizeof(announced), &length) < 0) return -1;
    if (length != sizeof(digest) || memcmp(digest, announced, sizeof(digest))) {
        return snag_errno(EPROTO);
    }
    if (fsync(file->fd) < 0) return -1;
    int rc = close(file->fd);
    file->fd = -1;
    if (rc < 0) return -1;
    return send_encoded(io, "SUCC", digest, sizeof(digest));
}

void
snag_upload_cleanup(int stage_fd, struct snag_upload_result *result)
{
    if (!result) return;
    for (size_t i = 0; i < result->count; ++i) {
        struct snag_upload_file *file = &result->files[i];
        if (file->fd >= 0 && file->leaf[0]) (void)close(file->fd);
        file->fd = -1;
        if (stage_fd >= 0 && file->leaf[0]) {
            (void)snag_unlink_at(stage_fd, file->leaf, false);
        }
        file->leaf[0] = '\0';
    }
    result->count = 0u;
}

int
snag_upload_receive(int tty, int stage_fd, size_t slots, bool directory,
                    int (*checkpoint)(void *), void *opaque,
                    struct snag_upload_result *result, char *error, size_t error_size)
{
    static const char config[] =
        "{\"lang\":\"c-snajpagent\",\"protocol\":1,\"binary\":false,"
        "\"directory\":false,\"overwrite\":false,\"bufsize\":65536,"
        "\"timeout\":20,\"quiet\":false}";
    struct upload_io io = {.fd = tty, .stage_fd = stage_fd,
                           .checkpoint = checkpoint, .opaque = opaque};
    struct upload_frame frame;
    const char *phase = "handshake";
    bool peer_cancelled = false;
    int rc = -1;

    if (!result || !error || !error_size || stage_fd < 0 ||
        slots > SNAG_UPLOAD_FILES_MAX || !isatty(tty)) {
        return snag_fail(error, error_size, EINVAL, "upload needs a controlling terminal");
    }
    int flags = fcntl(tty, F_GETFL);
    if (flags < 0 || !(flags & O_NONBLOCK)) {
        return snag_fail(error, error_size, EINVAL, "upload needs a private nonblocking terminal");
    }
    memset(result, 0, sizeof(*result));
    if (!slots) return snag_fail(error, error_size, EFBIG, "attachment slots are full");

    snag_buf_init(&frame.payload, SNAG_UPLOAD_LINE_MAX);
    uint64_t identifier = 1000000000000ull + snag_time_ms() % 899999999999ull;
    char marker[96];
    int marker_len = snprintf(marker, sizeof(marker), "\033[s::TRZSZ:TRANSFER:%c:1.0.0:%013"
                              PRIu64 ":0\r\n", directory ? 'D' : 'R', identifier);
    if (marker_len < 0 || (size_t)marker_len >= sizeof(marker)) {
        errno = EOVERFLOW;
        goto done;
    }
    if (write_bytes(&io, (const unsigned char *)marker, (size_t)marker_len) < 0 ||
        receive_action(&io, &frame, &peer_cancelled) < 0) goto done;
    if (peer_cancelled) {
        rc = 1;
        goto done;
    }
    if (directory) {
        errno = ENOTSUP;
        goto done;
    }
    if (send_encoded(&io, "CFG", config, sizeof(config) - 1u) < 0) goto done;

    phase = "file count";
    uint64_t count;
    if (read_integer(&io, &frame, "NUM", &count) < 0) goto done;
    if (count > slots) {
        errno = EFBIG;
        goto done;
    }
    if (send_integer(&io, count) < 0) goto done;
    for (size_t i = 0; i < (size_t)count; ++i) {
        phase = "file contents or digest";
        struct snag_upload_file *file = &result->files[result->count++];
        file->fd = -1;
        if (receive_file(&io, &frame, file) < 0) goto done;
    }
    phase = "final transfer exit";
    unsigned char summary[16384];
    size_t summary_len;
    if (read_expected(&io, &frame, "EXIT") < 0 ||
        decode_payload(&frame, summary, sizeof(summary), &summary_len) < 0) goto done;
    result->tail_len = io.len - io.at;
    memcpy(result->tail, io.input + io.at, result->tail_len);
    rc = 0;
done:
    if (rc < 0) {
        int cause = errno;
        if (cause != ECANCELED && cause != ETIMEDOUT && cause != EPIPE && cause != EIO) {
            (void)send_encoded(&io, "FAIL", "upload rejected", sizeof("upload rejected") - 1u);
        }
        (void)snag_errorf(error, error_size, "%s: %s", phase, strerror(cause));
        if (cause == ECANCELED) rc = 1;
        snag_upload_cleanup(stage_fd, result);
        errno = cause;
    }
    snag_buf_free(&frame.payload);
    return rc;
}
#endif /* _WIN32 */
