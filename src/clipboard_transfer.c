/* SPDX-License-Identifier: GPL-2.0-only */
#include "clipboard_transfer.h"
#include "fs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Match file-transfer liveness. The deadline restarts after actual progress;
 * a selection's total length and duration have no separate quota. */
#define COPY_IDLE_MS 20000u
#define COPY_RETRY_MS 750u
#define COPY_HEADER 24u /* 128-bit operation ID and 64-bit source offset. */
#define COPY_REPLY_MAX 128u

enum copy_command { COPY_BEGIN, COPY_DATA, COPY_COMMIT, COPY_CANCEL, COPY_STATUS };
enum copy_reply {
    REPLY_NATIVE = 1, REPLY_OSC, REPLY_ACK, REPLY_PENDING, REPLY_BUSY,
    REPLY_WRITTEN, REPLY_SENT, REPLY_CANCELED, REPLY_UNAVAILABLE, REPLY_FAILED,
    REPLY_UNCERTAIN
};
struct copy_message {
    enum copy_command command;
    char nonce[9], operation[33], digest[65];
    uint64_t length, offset;
    uint32_t sequence, checksum;
    size_t chunk;
    unsigned int attempt;
};
struct copy_response {
    enum copy_reply code;
    char nonce[9], operation[33];
    uint32_t sequence, checksum;
};
struct snag_clipboard_send {
    struct snag_clipboard *source;
    struct copy_message message;
    struct snag_copy_result result;
    uint64_t next, deadline;
    size_t sent;
    uint32_t probe;
    bool probing, probed, committed;
};
struct copy_receipt {
    struct copy_receipt *next;
    char nonce[9], operation[33];
    enum copy_reply code;
};
struct snag_clipboard_receive {
    enum snag_clipboard_policy policy;
    struct snag_clipboard_backend backend;
    struct copy_receipt *receipts, *active;
    struct copy_message message;
    struct snag_clipboard *copy;
    int fd;
    char *path;
    uint64_t bytes, touched, last_offset;
    size_t last_size;
    uint32_t sequence, last_checksum;
    bool committed, osc_ready, osc_started, canceling;
};

static bool
hex_text(const char *text, size_t length)
{
    for (size_t i = 0u; i < length; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return false;
    return text[length] == 0;
}

static uint64_t
hex_number(const char *text, size_t length)
{
    uint64_t value = 0u;
    for (size_t i = 0u; i < length; ++i)
        value = (value << 4u) | (uint64_t)(text[i] <= '9' ? text[i] - '0' : text[i] - 'a' + 10);
    return value;
}

static void
chunk_header(unsigned char *out, const struct copy_message *message)
{
    for (size_t i = 0u; i < 16u; ++i)
        out[i] = (unsigned char)hex_number(message->operation + i * 2u, 2u);
    for (size_t i = 0u; i < 8u; ++i)
        out[16u + i] = (unsigned char)(message->offset >> ((7u - i) * 8u));
}

static int
encode_title(const struct copy_message *message, const void *data, size_t length,
    char *out, size_t capacity)
{
    if (!hex_text(message->nonce, 8u) || !hex_text(message->operation, 32u) ||
        message->attempt > 15u) return snag_errno(EINVAL);
    static const char prefix[] = "\033]2;" SNAG_SCREEN_PREFIX "CLIP:";
    int n = -1;
    if (message->command == COPY_DATA) {
        unsigned char bytes[SNAG_SCREEN_STREAM_CHUNK];
        if (!data || !length || length > sizeof(bytes) - COPY_HEADER || capacity < 6u)
            return snag_errno(EINVAL);
        chunk_header(bytes, message);
        memcpy(bytes + COPY_HEADER, data, length);
        n = snag_screen_encode(out, capacity - 5u, message->nonce, message->sequence,
            message->attempt, bytes, length + COPY_HEADER);
        if (n < 0) return -1;
        size_t at = sizeof("\033]2;" SNAG_SCREEN_PREFIX) - 1u;
        memmove(out + at + 5u, out + at, (size_t)n - at + 1u);
        memcpy(out + at, "CLIP:", 5u);
        return n + 5;
    }
    if (message->command == COPY_BEGIN) {
        if (message->chunk != SNAG_SCREEN_CHUNK && message->chunk != SNAG_SCREEN_STREAM_CHUNK)
            return snag_errno(EINVAL);
        n = snprintf(out, capacity, "%sBEGIN:%s:%s:%016llx:UTF8:%zu:%x\a", prefix,
            message->nonce, message->operation, (unsigned long long)message->length,
            message->chunk, message->attempt);
    } else if (message->command == COPY_COMMIT) {
        if (!hex_text(message->digest, 64u)) return snag_errno(EINVAL);
        n = snprintf(out, capacity, "%sCOMMIT:%s:%s:%s:%x\a", prefix,
            message->nonce, message->operation, message->digest, message->attempt);
    } else if (message->command == COPY_CANCEL || message->command == COPY_STATUS) {
        n = snprintf(out, capacity, "%s%s:%s:%s:%x\a", prefix,
            message->command == COPY_CANCEL ? "CANCEL" : "STATUS",
            message->nonce, message->operation, message->attempt);
    }
    if (n < 0 || (size_t)n >= capacity) return snag_errno(EOVERFLOW);
    return n;
}

static int
parse_title(const void *data, size_t length, struct copy_message *message,
    unsigned char *payload, size_t *payload_size)
{
    const unsigned char *body = data;
    *message = (struct copy_message){0};
    *payload_size = 0u;
    if (length < 5u || memcmp(body, "CLIP:", 5u)) return -1;
    body += 5u;
    length -= 5u;
    if (length >= 34u && !memcmp(body, "DATA:", 5u)) {
        memcpy(message->nonce, body + 5u, 8u);
        unsigned char bytes[SNAG_SCREEN_STREAM_CHUNK];
        size_t count;
        if (!hex_text(message->nonce, 8u) ||
            snag_screen_decode(body, length, message->nonce, &message->sequence,
                &message->checksum, bytes, sizeof(bytes), &count) < 0 || count <= COPY_HEADER)
            return -1;
        for (size_t i = 0u; i < 16u; ++i)
            (void)snprintf(message->operation + i * 2u, 3u, "%02x", bytes[i]);
        for (size_t i = 0u; i < 8u; ++i)
            message->offset = (message->offset << 8u) | bytes[16u + i];
        *payload_size = count - COPY_HEADER;
        memcpy(payload, bytes + COPY_HEADER, *payload_size);
        message->command = COPY_DATA;
        return 0;
    }
    /* Control fields have fixed representation widths; selection length is u64. */
    char control[160], amount[17] = {0}, attempt[2] = {0};
    if (length >= sizeof(control) || memchr(body, 0, length)) return -1;
    memcpy(control, body, length);
    control[length] = 0;
    unsigned int chunk = 0u;
    int used = 0;
    int fields = sscanf(control, "BEGIN:%8[0-9a-f]:%32[0-9a-f]:%16[0-9a-f]:UTF8:%4u:%1[0-9a-f]%n",
        message->nonce, message->operation, amount, &chunk, attempt, &used);
    if (fields == 5 && used == (int)length && hex_text(amount, 16u) &&
        (chunk == SNAG_SCREEN_CHUNK || chunk == SNAG_SCREEN_STREAM_CHUNK)) {
        message->command = COPY_BEGIN;
        message->length = hex_number(amount, 16u);
        message->chunk = chunk;
    } else {
        used = 0;
        fields = sscanf(control, "COMMIT:%8[0-9a-f]:%32[0-9a-f]:%64[0-9a-f]:%1[0-9a-f]%n",
            message->nonce, message->operation, message->digest, attempt, &used);
        if (fields == 4 && used == (int)length && hex_text(message->digest, 64u))
            message->command = COPY_COMMIT;
        else {
            const char *name = !strncmp(control, "CANCEL:", 7u) ? "CANCEL:" :
                !strncmp(control, "STATUS:", 7u) ? "STATUS:" : NULL;
            if (!name) return -1;
            used = 0;
            fields = sscanf(control + 7u, "%8[0-9a-f]:%32[0-9a-f]:%1[0-9a-f]%n",
                message->nonce, message->operation, attempt, &used);
            if (fields != 3 || used + 7 != (int)length) return -1;
            message->command = name[0] == 'C' ? COPY_CANCEL : COPY_STATUS;
        }
    }
    if (!hex_text(message->nonce, 8u) || !hex_text(message->operation, 32u) ||
        !hex_text(attempt, 1u)) return -1;
    message->attempt = (unsigned int)hex_number(attempt, 1u);
    return 0;
}

static int
encode_reply(const struct copy_response *reply, char *out, size_t capacity)
{
    int n = snprintf(out, capacity, "\033[>9003;%u;%u;%u;%u;%u;%u;%u;%uc",
        (unsigned int)hex_number(reply->nonce, 8u),
        (unsigned int)hex_number(reply->operation, 8u),
        (unsigned int)hex_number(reply->operation + 8u, 8u),
        (unsigned int)hex_number(reply->operation + 16u, 8u),
        (unsigned int)hex_number(reply->operation + 24u, 8u),
        (unsigned int)reply->code, reply->sequence, reply->checksum);
    return n < 0 || (size_t)n >= capacity ? snag_errno(EOVERFLOW) : n;
}

static bool
parse_reply(const void *data, size_t length, struct copy_response *reply)
{
    const unsigned char *bytes = data;
    static const char prefix[] = "\033[>9003;";
    if (length < sizeof(prefix) || length >= COPY_REPLY_MAX ||
        memcmp(bytes, prefix, sizeof(prefix) - 1u) || bytes[length - 1u] != 'c') return false;
    uint32_t values[8] = {0};
    size_t at = sizeof(prefix) - 1u;
    for (size_t i = 0u; i < 8u; ++i) {
        size_t begin = at;
        while (at < length && bytes[at] >= '0' && bytes[at] <= '9') {
            unsigned int digit = bytes[at++] - '0';
            if (values[i] > (UINT32_MAX - digit) / 10u) return false;
            values[i] = values[i] * 10u + digit;
        }
        if (begin == at || at >= length || bytes[at++] != (i == 7u ? 'c' : ';')) return false;
    }
    if (at != length || values[5] < REPLY_NATIVE || values[5] > REPLY_UNCERTAIN) return false;
    (void)snprintf(reply->nonce, sizeof(reply->nonce), "%08x", values[0]);
    for (size_t i = 0u; i < 4u; ++i)
        (void)snprintf(reply->operation + 8u * i, 9u, "%08x", values[1u + i]);
    reply->code = (enum copy_reply)values[5];
    reply->sequence = values[6];
    reply->checksum = values[7];
    return true;
}

struct snag_clipboard_send *
snag_clipboard_send_open(struct snag_clipboard *source, uint64_t now)
{
    if (!source) { errno = EINVAL; return NULL; }
    struct snag_clipboard_result result;
    snag_clipboard_result(source, &result);
    if (result.state != SNAG_CLIPBOARD_READY) { errno = EAGAIN; return NULL; }
    struct snag_clipboard_send *send = calloc(1u, sizeof(*send));
    if (!send) return NULL;
    if (snag_random_id(send->message.operation) < 0) { free(send); return NULL; }
    memcpy(send->message.nonce, send->message.operation, 8u);
    memcpy(send->message.digest, result.sha256, sizeof(send->message.digest));
    send->source = source;
    send->message.length = result.length;
    send->message.chunk = SNAG_SCREEN_CHUNK;
    send->result.length = result.length;
    send->result.state = SNAG_COPY_CONNECTING;
    send->probe = (uint32_t)(hex_number(send->message.nonce, 8u) % 1000000000u);
    send->probing = true;
    send->deadline = now + 1000u;
    return send;
}

int
snag_clipboard_send_output(struct snag_clipboard_send *send, uint64_t now,
    char *out, size_t capacity)
{
    if (send->result.state >= SNAG_COPY_WRITTEN) return 0;
    if (send->probing) {
        if (!send->probed) {
            int n = snprintf(out, capacity, "\033[?9001;%un", send->probe);
            if (n < 0 || (size_t)n >= capacity) return snag_errno(EOVERFLOW);
            send->probed = true;
            return n;
        }
        if (now < send->deadline) return 0;
        send->probing = false;
        send->deadline = now + 1500u;
    }
    if (now >= send->deadline) {
        send->result.state = send->committed ? SNAG_COPY_UNCERTAIN :
            send->message.command == COPY_CANCEL ? SNAG_COPY_CANCELED :
            send->result.remote ? SNAG_COPY_FAILED : SNAG_COPY_UNAVAILABLE;
        return 0;
    }
    if (now < send->next) return 0;
    unsigned char bytes[SNAG_SCREEN_STREAM_CHUNK];
    size_t length = 0u;
    if (send->message.command == COPY_DATA) {
        uint64_t remaining = send->message.length - send->message.offset;
        length = remaining < send->message.chunk - COPY_HEADER ? (size_t)remaining :
            send->message.chunk - COPY_HEADER;
        if (snag_clipboard_read(send->source, send->message.offset, bytes, length) < 0) {
            send->result.state = SNAG_COPY_FAILED;
            return 0;
        }
        unsigned char header[SNAG_SCREEN_STREAM_CHUNK];
        chunk_header(header, &send->message);
        memcpy(header + COPY_HEADER, bytes, length);
        send->message.checksum = snag_screen_checksum(header, length + COPY_HEADER);
        send->sent = length;
    }
    int n = encode_title(&send->message, bytes, length, out, capacity);
    if (n < 0) return -1;
    send->message.attempt = (send->message.attempt + 1u) % 16u;
    send->next = now + COPY_RETRY_MS;
    if (send->message.command == COPY_COMMIT) send->committed = true;
    return n;
}

bool
snag_clipboard_send_input(struct snag_clipboard_send *send, const void *bytes,
    size_t length, uint64_t now)
{
    if (!send || send->result.state >= SNAG_COPY_WRITTEN) return false;
    if (send->probing) {
        char expected[24];
        int n = snprintf(expected, sizeof(expected), "\033[>%uS", send->probe);
        if (n > 0 && (size_t)n == length && !memcmp(bytes, expected, length)) {
            send->probing = false;
            send->message.chunk = SNAG_SCREEN_STREAM_CHUNK;
            send->deadline = now + 1500u;
            return true;
        }
    }
    struct copy_response reply = {0};
    if (!parse_reply(bytes, length, &reply) || strcmp(reply.nonce, send->message.nonce) ||
        strcmp(reply.operation, send->message.operation)) return false;
    send->result.remote = true;
    if (reply.code == REPLY_NATIVE || reply.code == REPLY_OSC) {
        if (send->message.command != COPY_BEGIN || reply.sequence != send->message.chunk)
            return true;
        send->message.command = send->message.length ? COPY_DATA : COPY_COMMIT;
        send->result.state = send->message.length ? SNAG_COPY_SENDING : SNAG_COPY_VERIFYING;
    } else if (reply.code == REPLY_ACK) {
        if (send->message.command != COPY_DATA || !send->sent ||
            reply.sequence != send->message.sequence || reply.checksum != send->message.checksum)
            return true;
        send->message.offset += send->sent;
        send->result.bytes = send->message.offset;
        send->sent = 0u;
        ++send->message.sequence;
        if (send->message.offset == send->message.length) {
            send->message.command = COPY_COMMIT;
            send->result.state = SNAG_COPY_VERIFYING;
        }
    } else if (reply.code == REPLY_PENDING || reply.code == REPLY_BUSY) {
        if (reply.code == REPLY_PENDING && send->message.command != COPY_COMMIT &&
            send->message.command != COPY_STATUS && send->message.command != COPY_CANCEL) {
            return true;
        }
        if (reply.code == REPLY_BUSY && send->message.command != COPY_BEGIN) return true;
        if (reply.code == REPLY_PENDING && send->message.command == COPY_COMMIT)
            send->message.command = COPY_STATUS;
        send->deadline = now + COPY_IDLE_MS;
        return true;
    } else {
        if ((reply.code == REPLY_WRITTEN || reply.code == REPLY_SENT) && !send->committed) {
            return true;
        }
        static const enum snag_copy_state states[] = {SNAG_COPY_WRITTEN, SNAG_COPY_SEQUENCE_SENT,
            SNAG_COPY_CANCELED, SNAG_COPY_UNAVAILABLE, SNAG_COPY_FAILED, SNAG_COPY_UNCERTAIN};
        send->result.state = states[reply.code - REPLY_WRITTEN];
        return true;
    }
    send->deadline = now + COPY_IDLE_MS;
    send->next = 0u;
    send->message.attempt = 0u;
    return true;
}

void
snag_clipboard_send_cancel(struct snag_clipboard_send *send, uint64_t now)
{
    if (!send || send->result.state >= SNAG_COPY_WRITTEN) return;
    if (send->probing) { send->result.state = SNAG_COPY_CANCELED; return; }
    send->message.command = COPY_CANCEL;
    send->result.state = SNAG_COPY_CANCELING;
    send->message.attempt = 0u;
    send->next = 0u;
    send->deadline = now + COPY_IDLE_MS;
}

void
snag_clipboard_send_result(const struct snag_clipboard_send *send, struct snag_copy_result *result)
{
    *result = send->result;
}

int
snag_clipboard_send_wait(const struct snag_clipboard_send *send, uint64_t now)
{
    if (!send || send->result.state >= SNAG_COPY_WRITTEN) return -1;
    uint64_t next = send->probing ? send->probed ? send->deadline : 0u : send->next;
    if (next > send->deadline) next = send->deadline;
    return next <= now ? 0 : next - now > INT_MAX ? INT_MAX : (int)(next - now);
}

void
snag_clipboard_send_close(struct snag_clipboard_send *send)
{
    free(send);
}

static void
receive_source_close(struct snag_clipboard_receive *receive)
{
    snag_clipboard_close(receive->copy);
    receive->copy = NULL;
    if (receive->fd >= 0) (void)close(receive->fd);
    receive->fd = -1;
    if (receive->path) (void)snag_unlink_at(-1, receive->path, false);
    free(receive->path);
    receive->path = NULL;
    receive->osc_ready = receive->osc_started = receive->canceling = false;
}

static void
receive_finish(struct snag_clipboard_receive *receive, enum copy_reply code)
{
    if (receive->active) receive->active->code = code;
    receive_source_close(receive);
    receive->active = NULL;
}

static int
receive_file(struct snag_clipboard_receive *receive)
{
#ifdef _WIN32
    char *temporary = snag_environment("TEMP");
    if (!temporary) temporary = snag_home_directory();
#else
    char *temporary = snag_environment("TMPDIR");
    if (!temporary) temporary = strdup("/tmp");
#endif
    if (!temporary) return -1;
    char id[SNAG_ID_HEX_LEN + 1u], name[64];
    if (snag_random_id(id) < 0) { free(temporary); return -1; }
    (void)snprintf(name, sizeof(name), ".snajpagent-clipboard-%s", id);
    receive->path = snag_path_join(temporary, name);
    free(temporary);
    if (!receive->path) return -1;
    receive->fd = snag_create_private_at(-1, receive->path, true);
    if (receive->fd < 0) {
        /* A failed exclusive create gives no ownership of the named file. */
        free(receive->path);
        receive->path = NULL;
        return -1;
    }
#ifndef _WIN32
    if (snag_unlink_at(-1, receive->path, false) < 0) return -1;
    free(receive->path);
    receive->path = NULL;
#endif
    return 0;
}

struct snag_clipboard_receive *
snag_clipboard_receive_open(enum snag_clipboard_policy policy,
    const struct snag_clipboard_backend *backend)
{
    if (policy < SNAG_CLIP_NATIVE || policy > SNAG_CLIP_OFF) { errno = EINVAL; return NULL; }
    struct snag_clipboard_receive *receive = calloc(1u, sizeof(*receive));
    if (!receive) return NULL;
    receive->fd = -1;
    receive->policy = policy;
    if (policy != SNAG_CLIP_NATIVE) return receive;
    if (backend) {
        receive->backend.kind = backend->kind;
        if (backend->program && !(receive->backend.program = strdup(backend->program))) goto failed;
        if (backend->pasteboard && !(receive->backend.pasteboard = strdup(backend->pasteboard)))
            goto failed;
    } else if (snag_clipboard_backend(&receive->backend) < 0) goto failed;
    return receive;
failed:
    snag_clipboard_receive_close(receive);
    return NULL;
}

void
snag_clipboard_receive_poll(struct snag_clipboard_receive *receive, uint64_t now)
{
    if (!receive || !receive->active) return;
    if (!receive->committed && now - receive->touched >= COPY_IDLE_MS) {
        receive_finish(receive, REPLY_CANCELED);
        return;
    }
    if (!receive->copy || receive->osc_ready) return;
    struct snag_clipboard_result result;
    snag_clipboard_result(receive->copy, &result);
    if (receive->canceling && result.state < SNAG_CLIPBOARD_WRITTEN) return;
    if (result.state == SNAG_CLIPBOARD_READY) {
        if (strcmp(result.sha256, receive->message.digest)) {
            receive_finish(receive, REPLY_FAILED);
        } else if (receive->policy == SNAG_CLIP_OSC52) {
            receive->osc_ready = true;
        } else if (snag_clipboard_publish(receive->copy) < 0) {
            receive_finish(receive, REPLY_FAILED);
        }
    } else if (result.state >= SNAG_CLIPBOARD_WRITTEN) {
        enum copy_reply code = result.state == SNAG_CLIPBOARD_WRITTEN ? REPLY_WRITTEN :
            result.state == SNAG_CLIPBOARD_CANCELED ? REPLY_CANCELED :
            result.state == SNAG_CLIPBOARD_UNAVAILABLE ? REPLY_UNAVAILABLE :
            result.state == SNAG_CLIPBOARD_UNCERTAIN ? REPLY_UNCERTAIN : REPLY_FAILED;
        receive_finish(receive, code);
    }
}

static struct copy_receipt *
receipt_find(struct snag_clipboard_receive *receive, const char *operation)
{
    for (struct copy_receipt *r = receive->receipts; r; r = r->next)
        if (!strcmp(r->operation, operation)) return r;
    return NULL;
}

static struct copy_receipt *
receipt_add(struct snag_clipboard_receive *receive, const struct copy_message *message,
    enum copy_reply code)
{
    struct copy_receipt *receipt = calloc(1u, sizeof(*receipt));
    if (!receipt) return NULL;
    memcpy(receipt->operation, message->operation, sizeof(receipt->operation));
    memcpy(receipt->nonce, message->nonce, sizeof(receipt->nonce));
    receipt->code = code;
    receipt->next = receive->receipts;
    receive->receipts = receipt;
    return receipt;
}

static int
reply_message(const struct copy_message *message, enum copy_reply code,
    uint32_t sequence, uint32_t checksum, char *reply, size_t capacity)
{
    struct copy_response response = {.code = code, .sequence = sequence, .checksum = checksum};
    memcpy(response.nonce, message->nonce, sizeof(response.nonce));
    memcpy(response.operation, message->operation, sizeof(response.operation));
    return encode_reply(&response, reply, capacity);
}

int
snag_clipboard_receive_title(struct snag_clipboard_receive *receive, const void *data,
    size_t length, uint64_t now, char *reply, size_t capacity)
{
    struct copy_message message;
    unsigned char payload[SNAG_SCREEN_STREAM_CHUNK];
    size_t size;
    if (parse_title(data, length, &message, payload, &size) < 0) return 0;
    /* Admit cancellation before a READY worker can cross the publication barrier. */
    if (message.command == COPY_CANCEL && receive->active && receive->copy &&
        !receive->osc_started && !strcmp(receive->active->operation, message.operation) &&
        !strcmp(receive->active->nonce, message.nonce) && snag_clipboard_cancel(receive->copy)) {
        receive->canceling = true;
        receive->osc_ready = false;
    }
    snag_clipboard_receive_poll(receive, now);
    struct copy_receipt *receipt = receipt_find(receive, message.operation);
    if (receipt && strcmp(receipt->nonce, message.nonce))
        return reply_message(&message, REPLY_FAILED, 0u, 0u, reply, capacity);
    if (receipt && receipt != receive->active)
        return reply_message(&message, receipt->code, 0u, 0u, reply, capacity);
    if (!receipt) {
        if (message.command == COPY_CANCEL) {
            if (!receipt_add(receive, &message, REPLY_CANCELED)) return -1;
            return reply_message(&message, REPLY_CANCELED, 0u, 0u, reply, capacity);
        }
        if (message.command != COPY_BEGIN) return 0;
        if (receive->policy == SNAG_CLIP_OFF ||
            (receive->policy == SNAG_CLIP_NATIVE && receive->backend.kind == SNAG_CLIPBOARD_NONE))
            return reply_message(&message, REPLY_UNAVAILABLE, 0u, 0u, reply, capacity);
        if (receive->active)
            return reply_message(&message, REPLY_BUSY, 0u, 0u, reply, capacity);
        receipt = receipt_add(receive, &message, REPLY_PENDING);
        if (!receipt) return -1;
        receive->active = receipt;
        receive->message = message;
        receive->bytes = receive->last_offset = 0u;
        receive->last_size = 0u;
        receive->sequence = receive->last_checksum = 0u;
        receive->committed = false;
        receive->touched = now;
        if (message.length > INT64_MAX || receive_file(receive) < 0) {
            receive_finish(receive, REPLY_FAILED);
            return reply_message(&message, REPLY_FAILED, 0u, 0u, reply, capacity);
        }
    }
    receive->touched = now;
    enum copy_reply code = REPLY_PENDING;
    uint32_t sequence = 0u, checksum = 0u;
    if (message.command == COPY_BEGIN) {
        if (message.length != receive->message.length || message.chunk != receive->message.chunk) {
            if (!receive->committed) receive_finish(receive, REPLY_FAILED);
            code = REPLY_FAILED;
        } else if (!receive->committed) {
            code = receive->policy == SNAG_CLIP_OSC52 ? REPLY_OSC : REPLY_NATIVE;
            sequence = (uint32_t)receive->message.chunk;
        }
    } else if (message.command == COPY_CANCEL) {
        if (!receive->committed) {
            receive_finish(receive, REPLY_CANCELED);
            code = REPLY_CANCELED;
        }
    } else if (message.command == COPY_DATA) {
        if (receive->committed || size > receive->message.chunk - COPY_HEADER) return 0;
        if (message.offset == receive->bytes && message.sequence == receive->sequence) {
            if (size > receive->message.length - receive->bytes) {
                receive_finish(receive, REPLY_FAILED);
                return reply_message(&message, REPLY_FAILED, 0u, 0u, reply, capacity);
            }
            size_t done = 0u;
            while (done < size) {
                ssize_t n = write(receive->fd, payload + done, size - done);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                    receive_finish(receive, REPLY_FAILED);
                    return reply_message(&message, REPLY_FAILED, 0u, 0u, reply, capacity);
                }
                done += (size_t)n;
            }
            receive->last_offset = message.offset;
            receive->last_size = size;
            receive->last_checksum = message.checksum;
            receive->bytes += size;
            ++receive->sequence;
        } else if (!receive->last_size || message.offset != receive->last_offset ||
            size != receive->last_size || message.sequence != receive->sequence - 1u ||
            message.checksum != receive->last_checksum) return 0;
        code = REPLY_ACK;
        sequence = message.sequence;
        checksum = message.checksum;
    } else if (message.command == COPY_COMMIT && !receive->committed) {
        if (receive->bytes != receive->message.length) {
            receive_finish(receive, REPLY_FAILED);
            return reply_message(&message, REPLY_FAILED, 0u, 0u, reply, capacity);
        }
        memcpy(receive->message.digest, message.digest, sizeof(message.digest));
        receive->copy = snag_clipboard_open(receive->fd, NULL, receive->bytes, &receive->backend);
        if (!receive->copy) {
            receive_finish(receive, REPLY_FAILED);
            return reply_message(&message, REPLY_FAILED, 0u, 0u, reply, capacity);
        }
        receive->committed = true;
    }
    return reply_message(&message, code, sequence, checksum, reply, capacity);
}

snag_wake_fd
snag_clipboard_receive_fd(const struct snag_clipboard_receive *receive)
{
    return receive ? snag_clipboard_fd(receive->copy) : SNAG_WAKE_INVALID;
}

struct snag_clipboard *
snag_clipboard_receive_osc(struct snag_clipboard_receive *receive)
{
    if (!receive || !receive->osc_ready || receive->osc_started) return NULL;
    receive->osc_started = true;
    return receive->copy;
}

void
snag_clipboard_receive_osc_done(struct snag_clipboard_receive *receive, bool success)
{
    if (receive && receive->osc_ready) {
        receive_finish(receive, success ? REPLY_SENT : REPLY_UNCERTAIN);
    }
}

void
snag_clipboard_receive_close(struct snag_clipboard_receive *receive)
{
    if (!receive) return;
    receive_source_close(receive);
    snag_clipboard_backend_free(&receive->backend);
    while (receive->receipts) {
        struct copy_receipt *next = receive->receipts->next;
        free(receive->receipts);
        receive->receipts = next;
    }
    free(receive);
}
