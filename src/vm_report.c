/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_report.h"
#include "fs.h"
#include "secret_source.h"
#include "store_internal.h"
#include "vm_source.h"
#include "unicode.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* JSON escaping can expand each byte of the existing command field sixfold;
 * the remaining fixed fields and signed-64-bit length fit within 256 bytes. */
#define REPORT_ROW_MAX (6u * SNAG_MAX_DIRECT_PROMPT + 256u)
#define REPORT_CATALOG ".view-reports.jsonl"

bool
snag_vm_report_valid(const json_t *report)
{
    const char *id = snag_json_bounded_string(json_object_get(report, "id"), SNAG_ID_HEX_LEN);
    const char *hash = snag_json_bounded_string(json_object_get(report, "sha256"),
        SNAG_SHA256_HEX_LEN);
    const char *command = snag_json_bounded_string(json_object_get(report, "command"),
        SNAG_MAX_DIRECT_PROMPT);
    uint64_t bytes;
    return snag_json_exact_keys(report, "id bytes sha256 command") &&
        id && strlen(id) == SNAG_ID_HEX_LEN && snag_hex_is_lower(id, SNAG_ID_HEX_LEN) &&
        hash && strlen(hash) == SNAG_SHA256_HEX_LEN &&
        snag_hex_is_lower(hash, SNAG_SHA256_HEX_LEN) && command &&
        snag_json_integer_u64(report, "bytes", &bytes) == 0;
}

/* A crash may leave only the final catalogue append incomplete. Locate its
 * preceding newline before publishing another entry; complete rows stay intact. */
static int64_t
catalog_boundary(int fd)
{
    snag_file_info info;
    if (snag_fstat(fd, &info) < 0 || info.st_size < 0) return -1;
    int64_t end = info.st_size;
    int64_t at = end;
    while (at > 0) {
        unsigned char buffer[4096];
        size_t length = at < (int64_t)sizeof(buffer) ? (size_t)at : sizeof(buffer);
        if ((uint64_t)(end - at) > REPORT_ROW_MAX) return snag_errno(EILSEQ);
        ssize_t got = snag_pread(fd, buffer, length, at - (int64_t)length);
        if (got < 0 && errno == EINTR) continue;
        if (got != (ssize_t)length) return snag_errno(got < 0 ? errno : ESTALE);
        for (size_t i = length; i; --i)
            if (buffer[i - 1u] == '\n') return at - (int64_t)length + (int64_t)i;
        at -= (int64_t)length;
    }
    return 0;
}

static int
catalog_append(int dir_fd, const json_t *report)
{
    char *encoded = json_dumps(report, JSON_COMPACT | JSON_ENSURE_ASCII);
    if (!encoded) return -1;
    size_t length = strlen(encoded);
    encoded[length] = '\n';
    int fd = snag_open_private_append_at(dir_fd, REPORT_CATALOG, false);
    if (fd < 0 && errno == ENOENT)
        fd = snag_open_private_append_at(dir_fd, REPORT_CATALOG, true);
    int64_t start = fd < 0 ? -1 : catalog_boundary(fd);
    int rc = start < 0 ? -1 : snag_truncate(fd, start);
    if (!rc) rc = snag_write_full(fd, encoded, length + 1u);
    if (!rc) rc = snag_sync_file(fd);
    int saved = errno;
    if (rc < 0 && start >= 0 && snag_truncate(fd, start) == 0) (void)snag_sync_file(fd);
    if (fd >= 0 && close(fd) < 0 && !rc) { rc = -1; saved = errno; }
    if (!rc && snag_sync_dir(dir_fd) < 0) { rc = -1; saved = errno; }
    snag_secret_clear(encoded, length + 1u);
    free(encoded);
    errno = saved;
    return rc;
}

struct report_writer {
    int dir_fd, fd;
    char id[SNAG_ID_HEX_LEN + 1u], name[64];
    struct snag_sha256 digest;
    uint64_t bytes;
};

static int
report_start(struct report_writer *writer, int dir_fd)
{
    *writer = (struct report_writer){.dir_fd = dir_fd, .fd = -1};
    if (snag_random_id(writer->id) < 0) return -1;
    (void)snprintf(writer->name, sizeof(writer->name), ".view-report-%s", writer->id);
    writer->fd = snag_create_private_at(dir_fd, writer->name, true);
    snag_sha256_init(&writer->digest);
    return writer->fd < 0 ? -1 : 0;
}

static int
report_write(struct report_writer *writer, const void *data, size_t length)
{
    if (length > (uint64_t)INT64_MAX - writer->bytes) return snag_errno(EOVERFLOW);
    if (snag_write_full(writer->fd, data, length) < 0) return -1;
    snag_sha256_update(&writer->digest, data, length);
    writer->bytes += length;
    return 0;
}

static void
report_abort(struct report_writer *writer)
{
    int saved = errno;
    (void)close(writer->fd);
    (void)snag_unlink_at(writer->dir_fd, writer->name, false);
    errno = saved;
}

static json_t *
report_finish(struct report_writer *writer, const char *command)
{
    char hash[SNAG_SHA256_HEX_LEN + 1u];
    snag_sha256_final_hex(&writer->digest, hash);
    json_t *report = json_pack("{s:s,s:I,s:s,s:s}", "id", writer->id,
        "bytes", (json_int_t)writer->bytes, "sha256", hash, "command", command);
    if (!report || !snag_vm_report_valid(report)) {
        json_decref(report);
        errno = EINVAL;
        report_abort(writer);
        return NULL;
    }
    int rc = snag_sync_file(writer->fd);
    int saved = errno;
    if (close(writer->fd) < 0 && !rc) { rc = -1; saved = errno; }
    if (!rc && snag_sync_dir(writer->dir_fd) < 0) { rc = -1; saved = errno; }
    if (rc < 0) (void)snag_unlink_at(writer->dir_fd, writer->name, false);
    else if (catalog_append(writer->dir_fd, report) < 0) { rc = -1; saved = errno; }
    /* Keep complete bytes if catalogue publication fails: a failed rollback
     * may already have left a complete reference to them on disk. */
    if (rc < 0) { json_decref(report); report = NULL; }
    errno = saved;
    return report;
}

json_t *
snag_vm_report_store(int dir_fd, const char *command, const void *data, size_t length)
{
    struct report_writer writer;
    if (report_start(&writer, dir_fd) < 0) return NULL;
    if (report_write(&writer, data, length) < 0) { report_abort(&writer); return NULL; }
    return report_finish(&writer, command);
}

json_t *
snag_vm_report_file(int dir_fd, const char *command, const void *prefix, size_t prefix_length,
    int source_fd, bool (*cancel)(void *), void *opaque)
{
    snag_file_info before, after;
    if (snag_fstat(source_fd, &before) < 0) return NULL;
    if (before.st_size < 0 || !S_ISREG(before.st_mode)) { errno = EINVAL; return NULL; }
    struct report_writer writer;
    if (report_start(&writer, dir_fd) < 0) return NULL;
    if (report_write(&writer, prefix, prefix_length) < 0) goto failed;
    for (int64_t at = 0; at < before.st_size;) {
        if (cancel && cancel(opaque)) { errno = ECANCELED; goto failed; }
        unsigned char buffer[65536];
        size_t length = before.st_size - at < (int64_t)sizeof(buffer) ?
            (size_t)(before.st_size - at) : sizeof(buffer);
        ssize_t got = snag_pread(source_fd, buffer, length, at);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { if (!got) errno = ESTALE; goto failed; }
        if (report_write(&writer, buffer, (size_t)got) < 0) goto failed;
        at += got;
    }
    if (cancel && cancel(opaque)) { errno = ECANCELED; goto failed; }
    if (snag_fstat(source_fd, &after) < 0) goto failed;
    if (before.st_size != after.st_size || before.st_mtime != after.st_mtime) {
        errno = ESTALE;
        goto failed;
    }
    return report_finish(&writer, command);
failed:
    report_abort(&writer);
    return NULL;
}

json_t *
snag_vm_report_catalog(struct snag_store *store, const char *session,
    bool (*cancel)(void *), void *opaque, bool *incomplete, char *error, size_t size)
{
    struct snag_session location;
    snag_session_init(&location);
    struct snag_buf line = {.max = REPORT_ROW_MAX};
    json_t *rows = json_array();
    json_t *ids = json_object();
    int fd = -1;
    int rc = -1;
    *incomplete = false;
    if (!rows || !ids ||
        snag_session_locate(store, &location, session, NULL, NULL, error, size) < 0) goto out;
    fd = snag_open_read_security_at(location.dir_fd, REPORT_CATALOG, false);
    if (fd < 0 && errno == ENOENT) { rc = 0; goto out; }
    if (fd < 0 || snag_store_verify_private_fd(fd, false, "report catalogue", error, size) < 0)
        goto out;
    snag_file_info info;
    if (snag_fstat(fd, &info) < 0 || info.st_size < 0) goto out;
    int64_t end = info.st_size;
    for (int64_t at = 0; at < end;) {
        if (cancel && cancel(opaque)) { errno = ECANCELED; goto out; }
        unsigned char buffer[65536];
        size_t length = end - at < (int64_t)sizeof(buffer) ? (size_t)(end - at) : sizeof(buffer);
        ssize_t got = snag_pread(fd, buffer, length, at);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { if (!got) errno = ESTALE; goto out; }
        for (size_t from = 0u; from < (size_t)got;) {
            const unsigned char *newline = memchr(buffer + from, '\n', (size_t)got - from);
            size_t count = newline ? (size_t)(newline - buffer) - from : (size_t)got - from;
            if (snag_buf_append(&line, buffer + from, count) < 0) goto out;
            from += count;
            if (!newline) break;
            ++from;
            json_t *report = snag_json_load_strict(line.data, line.len, REPORT_ROW_MAX, NULL, 0u);
            if (!report || !snag_vm_report_valid(report)) {
                json_decref(report);
                errno = EILSEQ;
                goto out;
            }
            const char *id = snag_json_string(report, "id");
            const json_t *previous = json_object_get(ids, id);
            bool valid = previous ? json_equal(previous, report) :
                json_object_set(ids, id, report) == 0 && json_array_append(rows, report) == 0;
            json_decref(report);
            if (!valid) { errno = EILSEQ; goto out; }
            snag_buf_reset(&line);
        }
        at += got;
    }
    *incomplete = line.len != 0u;
    rc = 0;
out: {
        int saved = errno;
        if (fd >= 0) (void)close(fd);
        snag_session_close(&location);
        snag_secret_clear(line.data, line.len);
        snag_buf_free(&line);
        json_decref(ids);
        if (rc < 0) {
            json_decref(rows);
            rows = NULL;
            if (error && size && !*error)
                (void)snag_errorf(error, size, "cannot read report catalogue: %s", strerror(saved));
        }
        errno = saved;
        return rows;
    }
}

static int
report_text(const unsigned char *data, size_t length,
    const struct snag_wire_secrets *secrets, bool (*cancel)(void *), void *opaque,
    struct snag_buf *out, json_t *map)
{
    /* Match across read boundaries before replacing invalid bytes. Grid
     * rendering makes all remaining control characters inert. */
    for (size_t at = 0u, checked = 0u; at < length;) {
        if (at >= checked) {
            if (cancel && cancel(opaque)) { errno = ECANCELED; return -1; }
            checked = at > SIZE_MAX - 4096u ? SIZE_MAX : at + 4096u;
        }
        size_t secret = snag_wire_secret_match(data + at, length - at, secrets);
        uint32_t cp;
        size_t unit = snag_utf8_decode(data + at, length - at, &cp);
        int rc;
        if (secret) {
            unit = secret;
            if (map && snag_vm_source_replace(map, out->len, at, 10u, unit) < 0) return -1;
            rc = snag_buf_append(out, "[redacted]", 10u);
        } else if (!unit || !cp) {
            unit = 1u;
            if (map && snag_vm_source_replace(map, out->len, at, 4u, 1u) < 0) return -1;
            rc = snag_buf_printf(out, "\\x%02x", data[at]);
        } else rc = snag_buf_append(out, data + at, unit);
        if (rc < 0) return -1;
        at += unit;
    }
    return 0;
}

int
snag_vm_report_text(const unsigned char *data, size_t length,
    const struct snag_wire_secrets *secrets, bool (*cancel)(void *), void *opaque,
    struct snag_buf *out)
{
    return report_text(data, length, secrets, cancel, opaque, out, NULL);
}

struct snag_vm_document *
snag_vm_report_read(struct snag_store *store, const char *session, const json_t *report,
    unsigned int columns, const struct snag_wire_secrets *secrets,
    bool (*cancel)(void *), void *opaque, char *error, size_t size)
{
    struct snag_session location;
    snag_session_init(&location);
    struct snag_buf raw = {.max = SIZE_MAX}, text = {.max = SIZE_MAX};
    struct snag_vm_document *document = NULL;
    json_t *blocks = NULL, *map = NULL;
    int fd = -1;
    if (!snag_vm_report_valid(report) || !columns) {
        (void)snag_fail(error, size, EINVAL, "invalid report reference");
        goto out;
    }
    uint64_t bytes;
    (void)snag_json_integer_u64(report, "bytes", &bytes);
    if (bytes > SIZE_MAX) { errno = EOVERFLOW; goto out; }
    if (snag_session_locate(store, &location, session, NULL, NULL, error, size) < 0) goto out;
    char name[64];
    (void)snprintf(name, sizeof(name), ".view-report-%s", snag_json_string(report, "id"));
    fd = snag_open_read_security_at(location.dir_fd, name, false);
    if (fd < 0 || snag_store_verify_private_fd(fd, false, "command report", error, size) < 0)
        goto out;
    snag_file_info info;
    if (snag_fstat(fd, &info) < 0) goto out;
    if (info.st_size < 0 || (uint64_t)info.st_size != bytes) {
        (void)snag_fail(error, size, ESTALE, "command report size changed");
        goto out;
    }
    struct snag_sha256 digest;
    snag_sha256_init(&digest);
    while (raw.len < bytes) {
        if (cancel && cancel(opaque)) { errno = ECANCELED; goto out; }
        unsigned char buffer[65536];
        size_t length = bytes - raw.len < sizeof(buffer) ?
            (size_t)(bytes - raw.len) : sizeof(buffer);
        ssize_t got = snag_pread(fd, buffer, length, (int64_t)raw.len);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { if (!got) errno = ESTALE; goto out; }
        snag_sha256_update(&digest, buffer, (size_t)got);
        if (snag_buf_append(&raw, buffer, (size_t)got) < 0) goto out;
    }
    char hash[SNAG_SHA256_HEX_LEN + 1u];
    snag_sha256_final_hex(&digest, hash);
    if (strcmp(hash, snag_json_string(report, "sha256"))) {
        (void)snag_fail(error, size, ESTALE, "command report digest changed");
        goto out;
    }
    map = json_array();
    if (!map || report_text(raw.data, raw.len, secrets, cancel, opaque, &text, map) < 0) goto out;
    if (snag_fstat(fd, &info) < 0) goto out;
    if (info.st_size < 0 || (uint64_t)info.st_size != bytes) {
        (void)snag_fail(error, size, ESTALE, "command report size changed");
        goto out;
    }
    blocks = json_pack("[{s:s,s:i,s:s,s:o,s:i,s:I,s:O}]", "key", snag_json_string(report, "id"),
        "seq", 0, "label", "", "text",
        json_stringn(text.data ? (const char *)text.data : "", text.len),
        "source_begin", 0, "source_end", (json_int_t)raw.len, "source_map", map);
    if (blocks) document = snag_vm_document_open(blocks, columns, cancel, opaque);
out: {
        int saved = errno;
        if (fd >= 0) (void)close(fd);
        snag_session_close(&location);
        json_decref(blocks);
        json_decref(map);
        snag_secret_clear(raw.data, raw.len);
        snag_buf_free(&raw);
        snag_buf_free(&text);
        if (!document && error && size && !*error)
            (void)snag_errorf(error, size, "cannot read command report: %s", strerror(saved));
        errno = saved;
        return document;
    }
}
