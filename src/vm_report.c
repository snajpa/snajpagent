/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_report.h"
#include "fs.h"
#include "secret_source.h"
#include "store_internal.h"
#include "unicode.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

int
snag_vm_report_text(const unsigned char *data, size_t length,
    const struct snag_wire_secrets *secrets, bool (*cancel)(void *), void *opaque,
    struct snag_buf *out)
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
            rc = snag_buf_append(out, "[redacted]", 10u);
        } else if (!unit || !cp) {
            unit = 1u;
            rc = snag_buf_printf(out, "\\x%02x", data[at]);
        } else rc = snag_buf_append(out, data + at, unit);
        if (rc < 0) return -1;
        at += unit;
    }
    return 0;
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
    json_t *blocks = NULL;
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
    if (snag_vm_report_text(raw.data, raw.len, secrets, cancel, opaque, &text) < 0) goto out;
    if (snag_fstat(fd, &info) < 0) goto out;
    if (info.st_size < 0 || (uint64_t)info.st_size != bytes) {
        (void)snag_fail(error, size, ESTALE, "command report size changed");
        goto out;
    }
    blocks = json_pack("[{s:s,s:i,s:s,s:o}]", "key", snag_json_string(report, "id"),
        "seq", 0, "label", "", "text",
        json_stringn(text.data ? (const char *)text.data : "", text.len));
    if (blocks) document = snag_vm_document_open(blocks, columns, cancel, opaque);
out: {
        int saved = errno;
        if (fd >= 0) (void)close(fd);
        snag_session_close(&location);
        json_decref(blocks);
        snag_secret_clear(raw.data, raw.len);
        snag_buf_free(&raw);
        snag_buf_free(&text);
        if (!document && error && size && !*error)
            (void)snag_errorf(error, size, "cannot read command report: %s", strerror(saved));
        errno = saved;
        return document;
    }
}
