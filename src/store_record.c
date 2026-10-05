/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_record.h"
#include "json.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

struct record_input {
    snag_record_read_fn read;
    void *opaque;
    int *read_error;
    int64_t position;
    int64_t end;
    size_t used;
    size_t length;
    size_t hashed;
    struct snag_sha256 *hash;
    unsigned char bytes[8192];
};

struct checkpoint_metadata {
    json_int_t format;
    json_int_t snapshot;
    bool provider_view;
};

struct record_value {
    json_type type;
    json_int_t integer;
};

static void
flush_digest(struct record_input *input)
{
    if (input->hash && input->used > input->hashed) {
        snag_sha256_update(input->hash, input->bytes + input->hashed,
            input->used - input->hashed);
    }
    input->hashed = input->used;
}

static int
peek(struct record_input *input)
{
    if (*input->read_error) return snag_errno(*input->read_error);
    if (input->position >= input->end) return -1;
    if (input->used == input->length) {
        flush_digest(input);
        int64_t left = input->end - input->position;
        size_t want = left < (int64_t)sizeof(input->bytes) ? (size_t)left :
            sizeof(input->bytes);
        ssize_t got;
        do {
            got = input->read(input->opaque, input->bytes, want, input->position);
        } while (got < 0 && errno == EINTR);
        if (got <= 0 || (size_t)got > want) {
            *input->read_error = got < 0 ? errno : EIO;
            return snag_errno(*input->read_error);
        }
        input->used = input->hashed = 0u;
        input->length = (size_t)got;
    }
    return input->bytes[input->used];
}

static int
take(struct record_input *input)
{
    int byte = peek(input);
    if (byte >= 0) {
        ++input->used;
        ++input->position;
    }
    return byte;
}

static int
literal(struct record_input *input, const char *text)
{
    while (*text) {
        if (take(input) != (unsigned char)*text++) return snag_errno(EINVAL);
    }
    return 0;
}

static int
hex_digit(int byte)
{
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    return -1;
}

/* Canonical strings use literal UTF-8, escaped quotes/backslashes and lower
 * case \u00xx for non-NUL controls. Return zero at the closing quote. */
static int
string_character(struct record_input *input, uint32_t *value)
{
    int first = take(input);
    if (first == '"') return 0;
    if (first < 0x20) return snag_errno(EINVAL);
    if (first == '\\') {
        first = take(input);
        if (first != '"' && first != '\\') {
            if (first != 'u' || literal(input, "00") < 0) return snag_errno(EINVAL);
            int high = hex_digit(take(input));
            int low = hex_digit(take(input));
            if (high < 0 || low < 0) return snag_errno(EINVAL);
            first = high * 16 + low;
            if (first <= 0 || first >= 0x20) return snag_errno(EINVAL);
        }
    } else if (first >= 0x80) {
        unsigned char bytes[4] = {(unsigned char)first};
        size_t length = snag_utf8_size(bytes[0]);
        if (!length || length > sizeof(bytes)) return snag_errno(EILSEQ);
        for (size_t i = 1u; i < length; ++i) {
            int next = take(input);
            if (next < 0) return snag_errno(EILSEQ);
            bytes[i] = (unsigned char)next;
        }
        if (snag_utf8_decode(bytes, length, value) != length || !*value) {
            return snag_errno(EILSEQ);
        }
        return 1;
    }
    *value = (uint32_t)first;
    return 1;
}

static int
string(struct record_input *input)
{
    if (take(input) != '"') return snag_errno(EINVAL);
    uint32_t value;
    int rc;
    while ((rc = string_character(input, &value)) > 0) {}
    return rc;
}

static struct record_input
key_reader(const struct record_input *input, int64_t start, int64_t end)
{
    return (struct record_input){.read = input->read, .opaque = input->opaque,
        .read_error = input->read_error, .position = start + 1, .end = end};
}

static bool
key_is(const struct record_input *input, int64_t start, int64_t end, const char *key)
{
    struct record_input copy = key_reader(input, start, end);
    uint32_t value;
    for (;;) {
        int rc = string_character(&copy, &value);
        if (rc <= 0) return rc == 0 && !*key;
        if (!*key || value != (unsigned char)*key++) return false;
    }
}

static int
ordered_keys(const struct record_input *input, int64_t previous_start,
    int64_t previous_end, int64_t start, int64_t end)
{
    /* Compare decoded keys by rereading their spans. Even exceptionally long
     * member names do not accumulate allocations at each nesting level. */
    struct record_input previous = key_reader(input, previous_start, previous_end);
    struct record_input current = key_reader(input, start, end);
    for (;;) {
        uint32_t a = 0u;
        uint32_t b = 0u;
        int ar = string_character(&previous, &a);
        int br = string_character(&current, &b);
        if (ar < 0 || br < 0) return -1;
        if (!ar || !br) return !ar && br ? 0 : snag_errno(EINVAL);
        if (a != b) return a < b ? 0 : snag_errno(EINVAL);
    }
}

static int
integer(struct record_input *input, json_int_t *value)
{
    bool negative = peek(input) == '-';
    if (negative) (void)take(input);
    int byte = peek(input);
    if (byte < '0' || byte > '9') return snag_errno(EINVAL);
    uint64_t magnitude = 0u;
    uint64_t limit = (uint64_t)INT64_MAX + (negative ? 1u : 0u);
    bool zero = byte == '0';
    size_t digits = 0u;
    while ((byte = peek(input)) >= '0' && byte <= '9') {
        unsigned int digit = (unsigned int)(byte - '0');
        if ((zero && digits) || magnitude > (limit - digit) / 10u) {
            return snag_errno(EINVAL);
        }
        magnitude = magnitude * 10u + digit;
        ++digits;
        (void)take(input);
    }
    if (negative && !magnitude) return snag_errno(EINVAL);
    *value = negative ? (magnitude == (uint64_t)INT64_MAX + 1u ? INT64_MIN :
        -(json_int_t)magnitude) : (json_int_t)magnitude;
    return 0;
}

static int value(struct record_input *, unsigned int, struct record_value *,
    struct checkpoint_metadata *);

static int
object(struct record_input *input, unsigned int depth, struct checkpoint_metadata *metadata)
{
    (void)take(input);
    if (peek(input) == '}') return take(input) < 0 ? -1 : 0;
    int64_t previous_start = -1;
    int64_t previous_end = -1;
    for (;;) {
        int64_t start = input->position;
        if (string(input) < 0) return -1;
        int64_t end = input->position;
        if (previous_start >= 0 &&
            ordered_keys(input, previous_start, previous_end, start, end) < 0) return -1;
        if (take(input) != ':') return snag_errno(EINVAL);
        struct record_value member = {0};
        if (value(input, depth + 1u, &member, metadata) < 0) return -1;
        if (depth == 1u) {
            if (key_is(input, start, end, "context")) {
                metadata->provider_view = member.type == JSON_OBJECT;
            } else if (member.type == JSON_INTEGER) {
                if (key_is(input, start, end, "format")) metadata->format = member.integer;
                if (key_is(input, start, end, "snapshot_v")) metadata->snapshot = member.integer;
            }
        }
        previous_start = start;
        previous_end = end;
        int separator = take(input);
        if (separator == '}') return 0;
        if (separator != ',') return snag_errno(EINVAL);
    }
}

static int
value(struct record_input *input, unsigned int depth, struct record_value *result,
    struct checkpoint_metadata *metadata)
{
    if (depth > 48u) return snag_errno(EOVERFLOW);
    int byte = peek(input);
    if (byte == '{') {
        result->type = JSON_OBJECT;
        return object(input, depth, metadata);
    }
    if (byte == '[') {
        result->type = JSON_ARRAY;
        (void)take(input);
        if (peek(input) == ']') return take(input) < 0 ? -1 : 0;
        for (;;) {
            struct record_value member = {0};
            if (value(input, depth + 1u, &member, metadata) < 0) return -1;
            int separator = take(input);
            if (separator == ']') return 0;
            if (separator != ',') return snag_errno(EINVAL);
        }
    }
    if (byte == '"') {
        result->type = JSON_STRING;
        return string(input);
    }
    if (byte == 't' || byte == 'f' || byte == 'n') {
        result->type = byte == 't' ? JSON_TRUE : byte == 'f' ? JSON_FALSE : JSON_NULL;
        return literal(input, byte == 't' ? "true" : byte == 'f' ? "false" : "null");
    }
    result->type = JSON_INTEGER;
    return integer(input, &result->integer);
}

static int
hex_string(struct record_input *input, char *text, size_t length)
{
    if (take(input) != '"') return snag_errno(EINVAL);
    for (size_t i = 0u; i < length; ++i) {
        int byte = take(input);
        if (hex_digit(byte) < 0) return snag_errno(EINVAL);
        text[i] = (char)byte;
    }
    text[length] = '\0';
    return take(input) == '"' ? 0 : snag_errno(EINVAL);
}

json_t *
snag_store_checkpoint_metadata(snag_record_read_fn read, void *opaque, int64_t start,
    int64_t end, char digest[SNAG_SHA256_HEX_LEN + 1u])
{
    if (!read || start < 0 || end <= start) {
        errno = EINVAL;
        return NULL;
    }
    unsigned char newline;
    ssize_t got;
    do { got = read(opaque, &newline, 1u, end - 1); } while (got < 0 && errno == EINTR);
    if (got < 0) return NULL;
    if (got != 1 || newline != '\n') {
        errno = EINVAL;
        return NULL;
    }
    struct snag_sha256 hash;
    snag_sha256_init(&hash);
    int read_error = 0;
    struct record_input input = {.read = read, .opaque = opaque, .position = start,
        .end = end - 1, .hash = &hash, .read_error = &read_error};
    struct checkpoint_metadata metadata = {0};
    struct record_value payload = {0};
    json_int_t pointer = 0;
    json_int_t seq = 0;
    json_int_t timestamp = 0;
    json_int_t version = 0;
    char event_hash[SNAG_SHA256_HEX_LEN + 1u];
    char previous_hash[SNAG_SHA256_HEX_LEN + 1u];
    char session[SNAG_ID_HEX_LEN + 1u];
    if (literal(&input, "{\"") < 0) goto invalid;
    bool indexed = peek(&input) == 'c';
    if (indexed && (literal(&input, "checkpoint_offset\":") < 0 ||
        integer(&input, &pointer) < 0 || pointer < 0 || literal(&input, ",\"") < 0)) {
        goto invalid;
    }
    if (literal(&input, "data\":") < 0 || peek(&input) != '{' ||
        value(&input, 1u, &payload, &metadata) < 0) goto invalid;
    flush_digest(&input);
    input.hash = NULL;
    if (literal(&input, ",\"event_sha256\":") < 0 ||
        hex_string(&input, event_hash, SNAG_SHA256_HEX_LEN) < 0) goto invalid;
    input.hashed = input.used;
    input.hash = &hash;
    if (literal(&input, ",\"prev_sha256\":") < 0 ||
        hex_string(&input, previous_hash, SNAG_SHA256_HEX_LEN) < 0 ||
        literal(&input, ",\"seq\":") < 0 || integer(&input, &seq) < 0 || seq < 0 ||
        literal(&input, ",\"session_id\":") < 0 ||
        hex_string(&input, session, SNAG_ID_HEX_LEN) < 0 ||
        literal(&input, ",\"time_ms\":") < 0 || integer(&input, &timestamp) < 0 || timestamp < 0 ||
        literal(&input, ",\"type\":\"session_checkpoint\",\"v\":") < 0 ||
        integer(&input, &version) < 0 || version != (indexed ? 2 : 1) ||
        take(&input) != '}' || input.position != input.end) goto invalid;
    flush_digest(&input);
    snag_sha256_final_hex(&hash, digest);
    json_t *record = json_pack("{s:{s:I,s:b,s:I},s:s,s:s,s:I,s:s,s:I,s:s,s:I}",
        "data", "format", metadata.format, "provider_view", metadata.provider_view,
        "snapshot_v", metadata.snapshot, "event_sha256", event_hash,
        "prev_sha256", previous_hash, "seq", seq, "session_id", session,
        "time_ms", timestamp, "type", "session_checkpoint", "v", version);
    if (record && indexed &&
        snag_json_set_new(record, "checkpoint_offset", json_integer(pointer)) < 0) {
        json_decref(record);
        return NULL;
    }
    return record;
invalid:
    errno = read_error ? read_error : EINVAL;
    return NULL;
}
