/* SPDX-License-Identifier: GPL-2.0-only */
#include "upload.h"
#include "json.h"
#include "media.h"
#include "upload_md5.h"
#include "upload_wire.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
int
snag_download_send(int tty, int file_fd, const char *name, const char *expected_sha,
                    int (*checkpoint)(void *), void *opaque,
                    struct snag_upload_result *result, char *error, size_t error_size)
{
    (void)tty;
    (void)file_fd;
    (void)expected_sha;
    (void)name;
    (void)checkpoint;
    (void)opaque;
    (void)result;
    return snag_fail(error, error_size, ENOTSUP, "terminal download is unavailable on this host");
}

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
    bool display_started;
    bool screen;
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
        ssize_t amount;
        if (io->screen) {
            /* GNU screen's DCS passthrough forwards these bytes without storing
             * protocol text in its virtual display. Stay below its string buffer. */
            unsigned char packet[132];
            size_t chunk = length > 128u ? 128u : length;
            packet[0] = 0x1bu;
            packet[1] = 'P';
            memcpy(packet + 2u, bytes, chunk);
            packet[chunk + 2u] = 0x1bu;
            packet[chunk + 3u] = '\\';
            size_t sent = 0;
            while (sent < chunk + 4u) {
                if (wait_ready(io, POLLOUT, deadline) < 0) return -1;
                ssize_t n = write(io->fd, packet + sent, chunk + 4u - sent);
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                if (n <= 0) return n < 0 ? -1 : snag_errno(EIO);
                sent += (size_t)n;
            }
            amount = (ssize_t)chunk;
        } else {
            amount = write(io->fd, bytes, length);
        }
        if (amount < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (amount <= 0) return amount < 0 ? -1 : snag_errno(EIO);
        bytes += (size_t)amount;
        length -= (size_t)amount;
    }
    return 0;
}

static int
transfer_begin(struct upload_io *io, char direction)
{
    /* TERM describes capabilities and can be forwarded over SSH. Only STY
     * identifies a GNU screen backend that will unwrap DCS packets here. */
    const char *sty = getenv("STY");
    io->screen = sty && *sty;
    /* The final two digits are platform flags: 10 means Windows, not a nonce.
     * Match the POSIX client's 13-digit marker with a reserved 00 suffix. */
    uint64_t identifier = (snag_time_ms() % 100000000000ull) * 100ull;
    char marker[128];
    int n = snprintf(marker, sizeof(marker),
        "%s\033[s::TRZSZ:TRANSFER:%c:1.0.0:%013llu:0\r\n",
        io->screen ? "" : "\033[?1049h", direction, (unsigned long long)identifier);
    if (n < 0 || (size_t)n >= sizeof(marker)) return snag_errno(EOVERFLOW);
    io->display_started = true;
    return write_bytes(io, (const unsigned char *)marker, (size_t)n);
}

static int
transfer_end(struct upload_io *io, bool stopped)
{
    const char *restore = io->screen ? "\033[u\033[0J\r\n" :
                                      "\033[u\033[0J\033[?1049l\r\n";
    if (!io->display_started) return 0;
    io->checkpoint = NULL;
    if (stopped) {
        /* The client drains terminal output on error. Match trzsz's 500 ms
         * server-exit quiet interval so it does not discard the restored UI. */
        uint64_t deadline = snag_monotonic_ms() + 500u;
        for (;;) {
            uint64_t now = snag_monotonic_ms();
            if (now >= deadline) break;
            if (poll(NULL, 0, (int)(deadline - now)) < 0 && errno != EINTR) return -1;
        }
    }
    if (io->screen) {
        /* End a DCS even if cancellation interrupted a partial packet. ST is
         * harmless outside a string and never becomes screen history text. */
        io->screen = false;
        int rc = write_bytes(io, (const unsigned char *)"\033\\", 2u);
        io->screen = true;
        if (rc < 0) return -1;
    }
    return write_bytes(io, (const unsigned char *)restore, strlen(restore));
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
        if (byte == 0x03u) { errno = ECANCELED; goto done; }
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
    int length = snprintf(text, sizeof(text), "#SUCC:%llu\n", (unsigned long long)value);
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
send_config(struct upload_io *io)
{
    char config[256];
    /* screen's input queue can lose a large pasted protocol-1 DATA line.
     * Keep client DATA frames within a small raw-tty input burst there. */
    unsigned int block = io->screen ? 1024u : SNAG_UPLOAD_BLOCK_MAX;
    int n = snprintf(config, sizeof(config),
        "{\"lang\":\"c-snajpagent\",\"protocol\":1,\"binary\":false,"
        "\"directory\":false,\"overwrite\":false,\"bufsize\":%u,"
        "\"timeout\":20,\"quiet\":false}", block);
    if (n < 0 || (size_t)n >= sizeof(config)) return snag_errno(EOVERFLOW);
    return send_encoded(io, "CFG", config, (size_t)n);
}

static int
decode_payload(struct upload_frame *frame, unsigned char *out,
               size_t capacity, size_t *written)
{
    return snag_upload_wire_decode((const char *)frame->payload.data,
                                   frame->payload.len, out, capacity, written);
}

static int
receive_action(struct upload_io *io, struct upload_frame *frame, bool *cancelled, bool *native)
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
    json_t *client = json_object_get(action, "native");
    const char *ending = json_string_value(newline);
    bool valid = json_is_object(action) && (!client || json_is_boolean(client)) &&
                 json_is_boolean(confirm) &&
                 json_is_integer(protocol) && json_integer_value(protocol) >= 1 &&
                 ending && !strcmp(ending, "\n") &&
                 !json_is_true(json_object_get(action, "tunnel")) &&
                 !json_is_true(json_object_get(action, "tmuxcc"));
    *cancelled = json_is_false(confirm);
    *native = json_is_true(client);
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
    if (transfer_begin(&io, directory ? 'D' : 'R') < 0 ||
        receive_action(&io, &frame, &peer_cancelled, &result->native_client) < 0) goto done;
    if (peer_cancelled) {
        rc = 1;
        goto done;
    }
    if (directory) {
        errno = ENOTSUP;
        goto done;
    }
    if (send_config(&io) < 0) goto done;

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
    if (transfer_end(&io, rc != 0) < 0) {
        (void)snag_errorf(error, error_size, "Cannot restore transfer display: %s",
                          strerror(errno));
        snag_upload_cleanup(stage_fd, result);
        rc = -1;
    }
    snag_buf_free(&frame.payload);
    return rc;
}

static int
download_integer(struct upload_io *io, struct upload_frame *frame,
                 const char *type, uint64_t value)
{
    char text[48];
    uint64_t accepted;
    int length = snprintf(text, sizeof(text), "#%s:%llu\n", type, (unsigned long long)value);
    if (length < 0 || (size_t)length >= sizeof(text)) return snag_errno(EOVERFLOW);
    if (write_bytes(io, (const unsigned char *)text, (size_t)length) < 0 ||
        read_integer(io, frame, "SUCC", &accepted) < 0) return -1;
    return accepted == value ? 0 : snag_errno(EPROTO);
}

int
snag_download_send(int tty, int file_fd, const char *name, const char *expected_sha,
                    int (*checkpoint)(void *), void *opaque,
                    struct snag_upload_result *result, char *error, size_t error_size)
{
    struct upload_io io = {.fd = tty, .checkpoint = checkpoint, .opaque = opaque};
    struct upload_frame frame;
    snag_file_info before, after;
    const char *phase = "download handshake";
    bool cancelled = false;
    bool started = false;
    int rc = -1;
    unsigned char block[SNAG_UPLOAD_BLOCK_MAX];
    unsigned char decoded[SNAG_NAME_MAX_BYTES + 1u];
    size_t length;

    if (!result || !error || !error_size || !name || !*name || !isatty(tty) ||
        snag_fstat(file_fd, &before) < 0 || !S_ISREG(before.st_mode) || before.st_size < 0) {
        return snag_fail(error, error_size, EINVAL, "download needs a terminal and a regular file");
    }
    int flags = fcntl(tty, F_GETFL);
    if (flags < 0 || !(flags & O_NONBLOCK))
        return snag_fail(error, error_size, EINVAL,
                         "download needs a private nonblocking terminal");
    length = strlen(name);
    if (length > SNAG_NAME_MAX_BYTES ||
        !snag_utf8_valid((const unsigned char *)name, length, true) ||
        strchr(name, '/') || strchr(name, '\\') || (length > 1u && name[1] == ':') ||
        !strcmp(name, ".") || !strcmp(name, "..")) {
        return snag_fail(error, error_size, EINVAL, "download needs a safe UTF-8 file name");
    }
    for (size_t i = 0; i < length; ++i) {
        if ((unsigned char)name[i] < 0x20u || name[i] == 0x7f)
            return snag_fail(error, error_size, EINVAL, "download needs a printable file name");
    }
    memset(result, 0, sizeof(*result));
    snag_buf_init(&frame.payload, SNAG_UPLOAD_LINE_MAX);
    if (transfer_begin(&io, 'S') < 0) goto done;
    started = true;
    if (receive_action(&io, &frame, &cancelled, &result->native_client) < 0) goto done;
    if (cancelled) { rc = 1; goto done; }
    if (send_config(&io) < 0 ||
        download_integer(&io, &frame, "NUM", 1u) < 0) goto done;
    phase = "download file name";
    if (send_encoded(&io, "NAME", name, strlen(name)) < 0 ||
        read_expected(&io, &frame, "SUCC") < 0 ||
        decode_payload(&frame, decoded, sizeof(decoded), &length) < 0 || !length) goto done;
    phase = "download contents";
    if (download_integer(&io, &frame, "SIZE", (uint64_t)before.st_size) < 0) goto done;
    struct snag_upload_md5 hash;
    struct snag_sha256 queued_hash;
    snag_upload_md5_init(&hash);
    snag_sha256_init(&queued_hash);
    uint64_t sent = 0;
    while (sent < (uint64_t)before.st_size) {
        size_t limit = io.screen ? 1024u : sizeof(block);
        size_t want = (uint64_t)before.st_size - sent > limit ? limit :
                      (size_t)((uint64_t)before.st_size - sent);
        ssize_t amount = read(file_fd, block, want);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) { if (!amount) errno = ESTALE; goto done; }
        uint64_t accepted;
        if (send_encoded(&io, "DATA", block, (size_t)amount) < 0 ||
            read_integer(&io, &frame, "SUCC", &accepted) < 0) goto done;
        if (accepted != (uint64_t)amount) { errno = EPROTO; goto done; }
        snag_upload_md5_update(&hash, block, (size_t)amount);
        if (expected_sha) snag_sha256_update(&queued_hash, block, (size_t)amount);
        sent += (uint64_t)amount;
    }
    /* A regular-mode virtual file can report zero size but contain data. */
    unsigned char extra;
    ssize_t remaining;
    do remaining = read(file_fd, &extra, 1u); while (remaining < 0 && errno == EINTR);
    if (remaining < 0) goto done;
    if (remaining) { errno = ESTALE; goto done; }
    if (snag_fstat(file_fd, &after) < 0) goto done;
    if (before.st_size != after.st_size || before.st_mtime != after.st_mtime ||
        before.st_ctime != after.st_ctime) { errno = ESTALE; goto done; }
    if (expected_sha) {
        char actual[SNAG_SHA256_HEX_LEN + 1u];
        snag_sha256_final_hex(&queued_hash, actual);
        if (strcmp(actual, expected_sha)) { errno = ESTALE; goto done; }
    }
    phase = "download digest";
    unsigned char digest[16];
    snag_upload_md5_finish(&hash, digest);
    if (send_encoded(&io, "MD5", digest, sizeof(digest)) < 0 ||
        read_expected(&io, &frame, "SUCC") < 0 ||
        decode_payload(&frame, decoded, sizeof(decoded), &length) < 0) goto done;
    if (length != sizeof(digest) || memcmp(decoded, digest, sizeof(digest))) {
        errno = EPROTO;
        goto done;
    }
    phase = "download final exit";
    if (read_expected(&io, &frame, "EXIT") < 0 ||
        decode_payload(&frame, block, sizeof(block), &length) < 0) goto done;
    if (result->native_client) {
        json_t *receipt = snag_json_load_strict(block, length, sizeof(block), NULL, 0u);
        const char *path = json_string_value(json_object_get(receipt, "path"));
        bool valid = json_is_object(receipt) && json_is_true(json_object_get(receipt, "native")) &&
                     path && snag_path_root_len(path) &&
                     snag_strcpy(result->receipt, sizeof(result->receipt), path);
        if (valid) {
            for (const unsigned char *at = (const unsigned char *)path; *at; ++at)
                if (*at < 0x20u || *at == 0x7fu) { valid = false; break; }
        }
        json_decref(receipt);
        if (!valid) { errno = EPROTO; goto done; }
    }
    result->tail_len = io.len - io.at;
    memcpy(result->tail, io.input + io.at, result->tail_len);
    rc = 0;
done:
    if (rc < 0) {
        int cause = errno;
        if (started && cause != EPIPE && cause != EIO && cause != ETIMEDOUT && cause != ECANCELED)
            (void)send_encoded(&io, "FAIL", "download failed", sizeof("download failed") - 1u);
        (void)snag_errorf(error, error_size, "%s: %s", phase, strerror(cause));
        if (cause == ECANCELED) rc = 1;
        errno = cause;
    }
    /* Complete the wrapper's terminal restoration before the app repaints. */
    if (transfer_end(&io, rc != 0) < 0) {
        (void)snag_errorf(error, error_size, "Cannot restore transfer display: %s",
                          strerror(errno));
        rc = -1;
    }
    snag_buf_free(&frame.payload);
    return rc;
}

/* Client counterparts use the same bounded frames/codec as the server. The
 * terminal proxy owns presentation and local selection; this layer owns wire
 * acknowledgement and only its operation's temporary output file. */
static int
client_config(struct upload_io *io, struct upload_frame *frame, size_t *block)
{
    static const char action[] = "{\"confirm\":true,\"protocol\":1,\"newline\":\"\\n\","
        "\"directory\":false,\"binary\":false,\"native\":true}";
    unsigned char decoded[16384];
    size_t length;
    if (send_encoded(io, "ACT", action, sizeof(action) - 1u) < 0 ||
        read_expected(io, frame, "CFG") < 0 ||
        decode_payload(frame, decoded, sizeof(decoded), &length) < 0) return -1;
    json_t *config = snag_json_load_strict(decoded, length, sizeof(decoded), NULL, 0u);
    json_t *version = json_object_get(config, "protocol");
    json_t *size = json_object_get(config, "bufsize");
    bool valid = json_is_object(config) && json_is_integer(version) &&
        json_integer_value(version) == 1 && json_is_false(json_object_get(config, "binary")) &&
        json_is_false(json_object_get(config, "directory")) && json_is_integer(size) &&
        json_integer_value(size) > 0 && json_integer_value(size) <= SNAG_UPLOAD_BLOCK_MAX;
    if (valid) *block = (size_t)json_integer_value(size);
    json_decref(config);
    return valid ? 0 : snag_errno(ENOTSUP);
}

static int
client_publish(int dir, struct snag_upload_file *file, const char *path,
               struct snag_client_result *result)
{
    char saved[SNAG_NAME_MAX_BYTES + 1u];
    if (!snag_strcpy(saved, sizeof(saved), file->name)) return snag_errno(ENAMETOOLONG);
    for (unsigned int attempt = 0u; attempt < 2u; ++attempt) {
        int length = snprintf(result->path, sizeof(result->path), "%s%s%s", path,
                              path[strlen(path) - 1u] == '/' ? "" : "/", saved);
        if (length < 0 || (size_t)length >= sizeof(result->path))
            return snag_errno(ENAMETOOLONG);
        if (snag_link_at(dir, file->leaf, dir, saved) == 0) {
            result->landed = true;
            if (snag_unlink_at(dir, file->leaf, false) < 0) return -1;
            file->leaf[0] = '\0';
            return fsync(dir);
        }
        if (errno != EEXIST || attempt) return -1;
        /* A collision gets the operation's unique suffix, never an overwrite.
         * The filesystem's actual component bound keeps the UTF-8 prefix safe. */
        long maximum = fpathconf(dir, _PC_NAME_MAX);
        size_t bound = maximum > 0 && (uintmax_t)maximum < sizeof(saved) ?
                       (size_t)maximum : sizeof(saved) - 1u;
        size_t suffix = strlen(file->leaf) + 1u;
        if (bound <= suffix) return snag_errno(ENAMETOOLONG);
        size_t prefix = strlen(file->name);
        if (prefix > bound - suffix) prefix = bound - suffix;
        while (prefix && !snag_utf8_valid((const unsigned char *)file->name, prefix, true))
            --prefix;
        length = snprintf(saved, sizeof(saved), "%.*s.%s", (int)prefix, file->name, file->leaf);
        if (length < 0 || (size_t)length >= sizeof(saved)) return snag_errno(ENAMETOOLONG);
    }
    return snag_errno(EEXIST);
}

int
snag_client_download(int tty, int directory, const char *path,
                     int (*checkpoint)(void *), void *opaque,
                     struct snag_client_result *result, char *error, size_t error_size)
{
    struct upload_io io = {.fd = tty, .stage_fd = directory,
                           .checkpoint = checkpoint, .opaque = opaque};
    struct upload_frame frame = {0};
    struct snag_upload_file file = {.fd = -1};
    snag_buf_init(&frame.payload, SNAG_UPLOAD_LINE_MAX);
    memset(result, 0, sizeof(*result));
    int rc = -1;
    size_t block;
    uint64_t count, size;
    if (client_config(&io, &frame, &block) < 0 ||
        read_integer(&io, &frame, "NUM", &count) < 0) goto done;
    /* The native server's download contract exports one regular file. */
    if (count != 1u) { errno = ENOTSUP; goto done; }
    if (send_integer(&io, count) < 0 || receive_name(&io, &frame, &file) < 0 ||
        read_integer(&io, &frame, "SIZE", &size) < 0 || send_integer(&io, size) < 0) goto done;
    for (const unsigned char *at = (const unsigned char *)file.name; *at; ++at)
        if (*at < 0x20u || *at == 0x7fu) { errno = EPROTO; goto done; }
    struct snag_upload_md5 hash;
    snag_upload_md5_init(&hash);
    uint64_t received = 0u;
    unsigned char bytes[SNAG_UPLOAD_BLOCK_MAX];
    while (received < size) {
        size_t amount;
        if (read_expected(&io, &frame, "DATA") < 0 ||
            decode_payload(&frame, bytes, sizeof(bytes), &amount) < 0) goto done;
        if (!amount || amount > block || amount > size - received) { errno = EPROTO; goto done; }
        size_t at = 0u;
        while (at < amount) {
            ssize_t written = write(file.fd, bytes + at, amount - at);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) goto done;
            at += (size_t)written;
        }
        snag_upload_md5_update(&hash, bytes, amount);
        received += amount;
        if (send_integer(&io, amount) < 0) goto done;
    }
    unsigned char digest[16], announced[16];
    size_t length;
    snag_upload_md5_finish(&hash, digest);
    if (read_expected(&io, &frame, "MD5") < 0 ||
        decode_payload(&frame, announced, sizeof(announced), &length) < 0) goto done;
    if (length != sizeof(digest) || memcmp(digest, announced, sizeof(digest))) {
        errno = EPROTO;
        goto done;
    }
    if (fsync(file.fd) < 0) goto done;
    int closed = close(file.fd);
    file.fd = -1;
    if (closed < 0) goto done;
    if (client_publish(directory, &file, path, result) < 0) goto done;
    result->bytes = size;
    json_t *receipt = json_pack("{s:b,s:s}", "native", 1, "path", result->path);
    char *text = receipt ? json_dumps(receipt, JSON_COMPACT | JSON_ENSURE_ASCII) : NULL;
    json_decref(receipt);
    if (!text) goto done;
    if (send_encoded(&io, "SUCC", digest, sizeof(digest)) == 0 &&
        send_encoded(&io, "EXIT", text, strlen(text)) == 0) rc = 0;
    free(text);
done:
    {
        int saved_error = errno;
        if (file.fd >= 0) (void)close(file.fd);
        if (file.leaf[0]) (void)snag_unlink_at(directory, file.leaf, false);
        if (rc < 0) {
            const char *message = strerror(saved_error);
            (void)send_encoded(&io, "fail", message, strlen(message));
            (void)snag_fail(error, error_size, saved_error, "download: %s", message);
        }
        result->tail_len = io.len - io.at;
        if (result->tail_len) memcpy(result->tail, io.input + io.at, result->tail_len);
        snag_buf_free(&frame.payload);
        errno = saved_error;
    }
    return rc;
}

int
snag_client_upload(int tty, int fd, const char *name,
                   int (*checkpoint)(void *), void *opaque,
                   struct snag_client_result *result, char *error, size_t error_size)
{
    struct upload_io io = {.fd = tty, .checkpoint = checkpoint, .opaque = opaque};
    struct upload_frame frame = {0};
    snag_buf_init(&frame.payload, SNAG_UPLOAD_LINE_MAX);
    memset(result, 0, sizeof(*result));
    int rc = -1;
    snag_file_info before, after;
    size_t block;
    unsigned char bytes[SNAG_UPLOAD_BLOCK_MAX], announced[16], digest[16];
    size_t length;
    if (snag_fstat(fd, &before) < 0 || !S_ISREG(before.st_mode) || before.st_size < 0) goto done;
    if (client_config(&io, &frame, &block) < 0 ||
        download_integer(&io, &frame, "NUM", 1u) < 0 ||
        send_encoded(&io, "NAME", name, strlen(name)) < 0 ||
        read_expected(&io, &frame, "SUCC") < 0 ||
        decode_payload(&frame, bytes, sizeof(bytes), &length) < 0 || !length ||
        download_integer(&io, &frame, "SIZE", (uint64_t)before.st_size) < 0) goto done;
    struct snag_upload_md5 hash;
    snag_upload_md5_init(&hash);
    uint64_t sent = 0u;
    while (sent < (uint64_t)before.st_size) {
        size_t want = (uint64_t)before.st_size - sent > block ? block :
                      (size_t)((uint64_t)before.st_size - sent);
        ssize_t amount;
        do amount = read(fd, bytes, want); while (amount < 0 && errno == EINTR);
        if (amount <= 0) { if (!amount) errno = ESTALE; goto done; }
        uint64_t accepted;
        if (send_encoded(&io, "DATA", bytes, (size_t)amount) < 0 ||
            read_integer(&io, &frame, "SUCC", &accepted) < 0) goto done;
        if (accepted != (uint64_t)amount) { errno = EPROTO; goto done; }
        snag_upload_md5_update(&hash, bytes, (size_t)amount);
        sent += (uint64_t)amount;
    }
    ssize_t remaining;
    do remaining = read(fd, bytes, 1u); while (remaining < 0 && errno == EINTR);
    if (remaining < 0) goto done;
    if (remaining || snag_fstat(fd, &after) < 0 || after.st_size != before.st_size ||
        after.st_mtime != before.st_mtime || after.st_ctime != before.st_ctime) {
        errno = ESTALE;
        goto done;
    }
    snag_upload_md5_finish(&hash, digest);
    if (send_encoded(&io, "MD5", digest, sizeof(digest)) < 0 ||
        read_expected(&io, &frame, "SUCC") < 0 ||
        decode_payload(&frame, announced, sizeof(announced), &length) < 0) goto done;
    if (length != sizeof(digest) || memcmp(digest, announced, sizeof(digest))) {
        errno = EPROTO;
        goto done;
    }
    if (send_encoded(&io, "EXIT", "Sent", 4u) < 0) goto done;
    result->bytes = sent;
    rc = 0;
done:
    {
        int saved_error = errno;
        if (rc < 0) {
            const char *message = strerror(saved_error);
            (void)send_encoded(&io, "fail", message, strlen(message));
            (void)snag_fail(error, error_size, saved_error, "upload: %s", message);
        }
        result->tail_len = io.len - io.at;
        if (result->tail_len) memcpy(result->tail, io.input + io.at, result->tail_len);
        snag_buf_free(&frame.payload);
        errno = saved_error;
    }
    return rc;
}

#endif /* _WIN32 */
