/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_internal.h"
#include "media.h"
#include "base.h"
#include "context.h"
#include "fs.h"
#include "irc.h"
#include "metrics.h"
#include "session_host.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct resolved_session {
    char id[SNAG_ID_HEX_LEN + 1u];
    char trash_name[SNAG_TRASH_NAME_LEN + 1u];
    bool trash;
};

static struct snag_directory *
open_store_dir(
    struct snag_store *store, const char *name, const char *label, char *error, size_t error_size)
{
    int fd = snag_open_read_security_at(store->root_fd, name, true);
    struct snag_directory *dir;

    if (fd < 0) return NULL;
    if (snag_store_verify_private_fd(fd, true, label, error, error_size) < 0) {
        (void)close(fd);
        return NULL;
    }
    dir = snag_directory_open(fd);
    if (!dir) {
        (void)close(fd);
        return NULL;
    }
    return dir;
}

static struct snag_directory *
open_sessions_dir(struct snag_store *store, char *error, size_t error_size)
{
    return open_store_dir(store, "sessions", "sessions directory", error, error_size);
}

static int
finish_directory(struct snag_directory *dir, char *error, size_t error_size)
{
    int saved = errno;

    if (snag_directory_close(dir) < 0 && !saved) saved = errno;
    if (!saved) return 0;
    snag_errorf(error, error_size, "cannot read session directory: %s", strerror(saved));
    errno = saved;
    return -1;
}

bool
snag_store_trash_id(const char *name, char id[SNAG_ID_HEX_LEN + 1u])
{
    if (!name || strlen(name) != SNAG_TRASH_NAME_LEN || name[SNAG_ID_HEX_LEN] != '.' ||
        !snag_hex_is_lower(name + SNAG_ID_HEX_LEN + 1u, SNAG_TRASH_SUFFIX_HEX_LEN))
        return false;
    memcpy(id, name, SNAG_ID_HEX_LEN);
    id[SNAG_ID_HEX_LEN] = '\0';
    return snag_hex_is_lower(id, SNAG_ID_HEX_LEN);
}

static int
emit_match(snag_store_emit_fn emit, void *opaque, const char *id)
{
    char line[SNAG_ID_HEX_LEN + 1u];

    memcpy(line, id, SNAG_ID_HEX_LEN);
    line[SNAG_ID_HEX_LEN] = '\n';
    return emit(opaque, line, sizeof(line));
}

static int
resolve_prefix(struct snag_store *store, const char *prefix, struct resolved_session *target,
    bool include_trash, snag_store_emit_fn matches_emit, void *opaque, char *error,
    size_t error_size)
{
    size_t len = prefix ? strlen(prefix) : 0u;
    unsigned int matches = 0;

    memset(target, 0, sizeof(*target));
    if (!len || len > SNAG_ID_HEX_LEN || !snag_hex_is_lower(prefix, len))
        return snag_fail(
            error, error_size, EINVAL, "session id must be 1..32 lowercase hex characters");
    for (unsigned int trash = 0u; trash < (include_trash ? 2u : 1u); ++trash) {
        struct snag_directory *dir = open_store_dir(store, trash ? "trash" : "sessions",
            trash ? "trash directory" : "sessions directory", error, error_size);
        const char *entry;
        if (!dir) return -1;
        while ((entry = snag_directory_next(dir)) != NULL) {
            char id[SNAG_ID_HEX_LEN + 1u];
            if (trash) {
                if (!snag_store_trash_id(entry, id)) continue;
            } else {
                if (strlen(entry) != SNAG_ID_HEX_LEN || !snag_hex_is_lower(entry, SNAG_ID_HEX_LEN))
                    continue;
                memcpy(id, entry, sizeof(id));
            }
            if (strncmp(id, prefix, len) != 0) continue;
            if (matches++ == 0u) {
                memcpy(target->id, id, sizeof(target->id));
                target->trash = trash != 0u;
                if (trash) memcpy(target->trash_name, entry, sizeof(target->trash_name));
            } else if (matches_emit) {
                /* Emit the first candidate only once ambiguity is established. */
                if ((matches == 2u && emit_match(matches_emit, opaque, target->id) < 0) ||
                    emit_match(matches_emit, opaque, id) < 0) {
                    int saved = errno ? errno : EIO;
                    (void)snag_directory_close(dir);
                    return snag_fail(error, error_size, saved, "cannot write matching session ids");
                }
            }
        }
        if (finish_directory(dir, error, error_size) < 0) return -1;
    }

    if (matches != 1u) {
        snag_errorf(error, error_size,
            matches ? "session id prefix is ambiguous" : "session id was not found");
        errno = matches ? EEXIST : ENOENT;
        return -1;
    }
    return 0;
}

int
snag_store_open_session_directory(struct snag_store *store, struct snag_session *session,
    const char *id, char *error, size_t error_size)
{
    char *sessions = NULL;

    memcpy(session->id, id, SNAG_ID_HEX_LEN + 1u);
    sessions = snag_path_join(store->root_path, "sessions");
    if (sessions) {
        session->dir_path = snag_path_join(sessions, id);
        free(sessions);
    }
    if (!session->dir_path) return -1;
    session->dir_fd = snag_open_read_security_at(store->sessions_fd, id, true);
    if (session->dir_fd < 0)
        return snag_errorf(error, error_size, "cannot open session %s: %s", id, strerror(errno));
    return snag_store_verify_private_fd(
        session->dir_fd, true, "session directory", error, error_size);
}

int
snag_session_locate(struct snag_store *store, struct snag_session *session, const char *prefix,
    snag_store_emit_fn matches_emit, void *opaque, char *error, size_t error_size)
{
    struct resolved_session target;

    if (session->dir_fd >= 0 || session->log_fd >= 0 || session->lock_fd >= 0 ||
        session->dir_path) {
        return snag_fail(error, error_size, EINVAL, "session location is already open");
    }
    if (resolve_prefix(store, prefix, &target, false, matches_emit, opaque, error, error_size) < 0)
        return -1;
    /* Attachment only needs the verified directory. Opening a writer or replaying
     * the journal here would turn a lookup into a second session runner. */
    return snag_store_open_session_directory(store, session, target.id, error, error_size);
}

static int
open_full_id(struct snag_store *store, struct snag_session *session, const char *id, char *error,
    size_t error_size)
{
    if (snag_store_open_session_directory(store, session, id, error, error_size) < 0) return -1;
    int format = snag_store_open_session_files(session, false, error, error_size);
    if (format < 0) return -1;
    int rc = format ? snag_store_load_binary_session(session, SNAG_TAIL_TRUNCATE, error, error_size)
                    : snag_store_scan_log(session, SNAG_TAIL_TRUNCATE, error, error_size);
    if (rc < 0) return -1;
    if (snag_media_work_remove(session->dir_fd, error, error_size) < 0) return -1;
    if (snag_store_remove_upload_staging(session->dir_fd, error, error_size) < 0) return -1;
    if (session->delete_requested) {
        if (snag_session_complete_delete(store, session, error, error_size) < 0) return -1;
        snag_errorf(error, error_size, "session deletion was completed");
        return 1;
    }
    return 0;
}

int
snag_session_open(struct snag_store *store, struct snag_session *session, const char *prefix,
    char *error, size_t error_size)
{
    struct resolved_session target;

    if (resolve_prefix(store, prefix, &target, true, NULL, NULL, error, error_size) < 0) return -1;
    if (target.trash) {
        if (snag_store_complete_trash_delete(store, target.trash_name, error, error_size) < 0)
            return -1;
        snag_errorf(error, error_size, "session deletion was completed");
        return 1;
    }
    return open_full_id(store, session, target.id, error, error_size);
}

static int
read_snapshot(struct snag_session *session, bool native)
{
    session->snapshot_read_only = true;
    if (native)
        return snag_store_load_binary_session(session, SNAG_TAIL_IGNORE, NULL, 0u);
    session->log_end = snag_seek(session->log_fd, 0, SEEK_END);
    if (session->log_end < 0) return -1;
    int rc = snag_store_scan_log(session, SNAG_TAIL_IGNORE, NULL, 0u);
    session->snapshot_read_only = true;
    return rc;
}

enum catalog_content { CATALOG_IDENTITY, CATALOG_CELLS, CATALOG_MONITOR, CATALOG_PREVIEW };

struct catalog_row {
    json_t *owner;
    json_t *cells; /* Borrowed from owner. Name may also borrow the open snapshot. */
    const char *name;
    uint64_t activity_ms;
    bool published;
};

static bool load_catalog_row(struct snag_store *, struct snag_session *, const char *,
    unsigned int, enum catalog_content, bool (*)(void *), void *,
    struct snag_sha256 *, struct catalog_row *);

bool
snag_session_name_valid(const char *name)
{
    if (!snag_text_valid(name, 1u, SNAG_PATH_MAX_BYTES) || snag_text_blank(name)) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (*p < 0x20u || *p == 0x7fu) return false;
    }
    return true;
}

int
snag_store_find_name(struct snag_store *store, const char *name, char id[SNAG_ID_HEX_LEN + 1u],
    snag_store_emit_fn matches_emit, void *opaque, char *error, size_t error_size)
{
    const char *entry;
    unsigned int matches = 0u;

    if (!snag_session_name_valid(name)) {
        return snag_fail(error, error_size, EINVAL, "invalid session name");
    }
    struct snag_directory *dir = open_sessions_dir(store, error, error_size);
    if (!dir) return -1;
    while ((entry = snag_directory_next(dir)) != NULL) {
        struct snag_session snapshot;
        struct catalog_row row;
        if (!load_catalog_row(store, &snapshot, entry, 0u, CATALOG_IDENTITY,
                NULL, NULL, NULL, &row)) continue;
        bool match = row.name && !strcmp(row.name, name);
        json_decref(row.owner);
        snag_session_close(&snapshot);
        if (!match) continue;
        if (!matches++) {
            memcpy(id, entry, SNAG_ID_HEX_LEN + 1u);
        } else if (matches_emit && ((matches == 2u && emit_match(matches_emit, opaque, id) < 0) ||
                                       emit_match(matches_emit, opaque, entry) < 0)) {
            (void)snag_directory_close(dir);
            return snag_errorf(error, error_size, "cannot write matching session ids");
        }
    }
    if (finish_directory(dir, error, error_size) < 0) return -1;
    if (matches != 1u) {
        return snag_fail(error, error_size, matches ? EEXIST : ENOENT,
            matches ? "session name is ambiguous; select one of the matching ids"
                    : "session name was not found");
    }
    return 0;
}

int
snag_store_find_last(
    struct snag_store *store, char id[SNAG_ID_HEX_LEN + 1u], char *error, size_t error_size)
{
    struct snag_directory *dir;
    const char *entry;
    char best[SNAG_ID_HEX_LEN + 1u] = {0};
    uint64_t best_time = 0;

    dir = open_sessions_dir(store, error, error_size);
    if (!dir) return -1;
    while ((entry = snag_directory_next(dir)) != NULL) {
        struct snag_session snapshot;
        struct catalog_row row;
        if (!load_catalog_row(store, &snapshot, entry, 0u, CATALOG_IDENTITY,
                NULL, NULL, NULL, &row)) continue;
        uint64_t last = row.activity_ms;
        json_decref(row.owner);
        if (!best[0] || last > best_time || (last == best_time && strcmp(entry, best) > 0)) {
            memcpy(best, entry, sizeof(best));
            best_time = last;
        }
        snag_session_close(&snapshot);
    }
    if (finish_directory(dir, error, error_size) < 0) return -1;
    if (!best[0]) return snag_fail(error, error_size, ENOENT, "no saved session");
    memcpy(id, best, sizeof(best));
    return 0;
}

int
snag_session_open_last(
    struct snag_store *store, struct snag_session *session, char *error, size_t error_size)
{
    char id[SNAG_ID_HEX_LEN + 1u];
    if (snag_store_find_last(store, id, error, error_size) < 0) return -1;
    return open_full_id(store, session, id, error, error_size);
}

bool
snag_session_is_live(const struct snag_session *session)
{
    int fd;
    bool live;
    int saved_errno;

    /* Never reopen a lock already owned by this process: closing that descriptor
     * would also drop its original POSIX record lock. */
    if (session->lock_fd >= 0) return true;
    if (session->dir_fd < 0) return false;
    fd = snag_create_private_at(session->dir_fd, "lock", false);
    if (fd < 0) return false;
    live = snag_lock_file(fd, false) < 0 && (errno == EAGAIN || errno == EACCES);
    saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
    return live;
}

static int
append_list_preview(struct snag_buf *row, const char *text, size_t width, bool pad)
{
    size_t length = text ? strlen(text) : 0u;
    size_t offset = 0u;
    size_t shown = 0u;
    size_t ellipsis_end = row->len;
    size_t ellipsis_width = 0u;
    bool space = false;

    if (!width) return 0;
    while (offset < length) {
        uint32_t cp;
        size_t bytes = snag_utf8_decode((const unsigned char *)text + offset, length - offset, &cp);
        if (!bytes) return snag_errno(EILSEQ);
        if (cp <= 0x20u || (cp >= 0x7fu && cp <= 0x9fu) || cp == 0xadu || cp == 0x61cu ||
            cp == 0x200bu || cp == 0x200eu || cp == 0x200fu || (cp >= 0x2028u && cp <= 0x202eu) ||
            cp == 0x2060u || (cp >= 0x2066u && cp <= 0x206fu) || cp == 0xfeffu ||
            (cp >= 0xfff9u && cp <= 0xfffbu)) {
            space = shown != 0u;
            offset += bytes;
            continue;
        }
        int cells = snag_char_width(cp);
        if (cells < 0) cells = 1;
        if (shown + (space ? 1u : 0u) + (size_t)cells > width) {
            row->len = ellipsis_end;
            if (snag_buf_append(row, "…", 3u) < 0) return -1;
            shown = ellipsis_width + 1u;
            break;
        }
        if (space && snag_buf_putc(row, ' ') < 0) return -1;
        if (snag_buf_append(row, text + offset, bytes) < 0) return -1;
        shown += (space ? 1u : 0u) + (size_t)cells;
        if (shown < width) {
            ellipsis_end = row->len;
            ellipsis_width = shown;
        }
        space = false;
        offset += bytes;
    }
    while (pad && shown++ < width) {
        if (snag_buf_putc(row, ' ') < 0) return -1;
    }
    return 0;
}

struct list_irc {
    const char *prompt;
    struct snag_buf preview;
    struct snag_buf endpoints;
    json_t *renames;
    uint64_t checkpoint_seq;
    bool prompt_found;
    bool topology_found;
    bool wants_message;
};

static bool
irc_prompt_header(const char *text)
{
    return !strncmp(text, "[IRC update id=", 15u) || !strncmp(text, "[IRC endpoint=", 14u);
}

static int
list_irc_prompt(struct list_irc *irc)
{
    const char *selected = NULL;
    const char *text = irc->prompt;

    irc->prompt_found = true;
    if (!text || !irc_prompt_header(text)) return 0;
    for (const char *line = text; line && *line;) {
        if (irc_prompt_header(line)) {
            const char *end = strchr(line, '\n');
            const char *message = strstr(line, " event=message ");
            const char *notice = strstr(line, " event=notice ");
            bool chat = (message && (!end || message < end)) || (notice && (!end || notice < end));
            if (chat || !irc->wants_message) selected = line;
            if (chat) irc->wants_message = true;
        }
        line = strchr(line, '\n');
        if (line) ++line;
    }
    if (!selected) return 0;
    const char *end = strchr(selected, ']');
    const char *endpoint = strstr(selected, "endpoint=");
    if (!end || !endpoint || endpoint > end) return 0;
    endpoint += 9u;
    size_t endpoint_len = strcspn(endpoint, " ]\n");
    if (!endpoint_len || endpoint_len > SNAG_CONFIG_IRC_ENDPOINT_MAX) return 0;
    if (snag_buf_printf(&irc->preview, "%.*s: ", (int)endpoint_len, endpoint) < 0) return -1;
    if (!strncmp(selected, "[IRC update id=", 15u)) {
        irc->prompt_found = false;
        const char *kind = strstr(selected, "event=");
        if (kind && kind < end) {
            kind += 6u;
            return snag_buf_append(&irc->preview, kind, strcspn(kind, " ]\n"));
        }
        return 0;
    }
    const char *body = end + 1u;
    if (*body == '\n') ++body;
    const char *next = strstr(body, "\n[IRC ");
    return snag_buf_append(&irc->preview, body, next ? (size_t)(next - body) : strlen(body));
}

static bool
list_nick_equal(const char *nick, size_t length, const char *other)
{
    if (strlen(other) != length) return false;
    for (size_t i = 0u; i < length; ++i) {
        if (snag_irc_fold((unsigned char)nick[i]) != snag_irc_fold((unsigned char)other[i])) {
            return false;
        }
    }
    return true;
}

static int
list_irc_endpoint(struct list_irc *irc, const char *text, const char *end, char role,
    const char *endpoint, size_t length, const char *nick, size_t nick_len)
{
    char key[SNAG_CONFIG_IRC_ENDPOINT_MAX + 32u];

    if (!length || length > SNAG_CONFIG_IRC_ENDPOINT_MAX) return 0;
    (void)snprintf(key, sizeof(key), "\naliases[%.*s]: model ", (int)length, endpoint);
    const char *alias = strstr(text, key);
    if (alias && alias < end) {
        nick = alias + strlen(key);
        nick_len = strcspn(nick, " \t\r\n");
    }
    if (nick_len > SNAG_CONFIG_IRC_NICK_MAX) nick_len = 0u;

    /* Reverse traversal collected newest first; apply the rename chain forward. */
    for (size_t i = json_array_size(irc->renames); nick_len && i;) {
        const json_t *event = json_array_get(irc->renames, --i);
        const char *address = snag_json_string(event, "endpoint");
        if (strlen(address) == length && !memcmp(address, endpoint, length) &&
            list_nick_equal(nick, nick_len, snag_json_string(event, "nick"))) {
            nick = snag_json_string(event, "text");
            nick_len = strlen(nick);
        }
    }
    return snag_buf_printf(&irc->endpoints, "%s%c/%.*s%s%.*s", irc->endpoints.len ? "," : "", role,
        (int)nick_len, nick ? nick : "", nick_len ? "@" : "", (int)length, endpoint);
}

static int
list_irc_topology(struct list_irc *irc, const char *text)
{
    const char *end = strstr(text, "\nhistory:\n");
    const char *host = strstr(text, "\nhosted: ");
    const char *nick = strstr(text, "\nmodel nick: ");
    size_t nick_len = 0u;
    size_t host_len = 0u;

    if (!end) end = text + strlen(text);
    if (nick && nick < end) {
        nick += 13u;
        nick_len = strcspn(nick, " \t\r\n");
    }
    if (host && host < end) {
        host += 9u;
        host_len = strcspn(host, "\r\n");
        if (host_len == 2u && !strncmp(host, "no", 2u)) host_len = 0u;
        if (list_irc_endpoint(irc, text, end, 's', host, host_len, nick, nick_len) < 0) return -1;
    }
    const char *destination = strstr(text, "\ndestination[");
    bool destinations = destination && destination < end;
    for (const char *line = text; line && line < end;) {
        const char *endpoint = NULL;
        size_t length = 0u;
        if (destinations && !strncmp(line, "destination[", 12u)) {
            const char *colon = strstr(line, "]: ");
            const char *line_end = strchr(line, '\n');
            if (colon && (!line_end || colon < line_end)) {
                endpoint = colon + 3u;
                length = strcspn(endpoint, "\r\n");
            }
        } else if (!destinations && !strncmp(line, "endpoint[", 9u)) {
            endpoint = line + 9u;
            const char *close = strstr(endpoint, "]: ");
            const char *line_end = strchr(line, '\n');
            if (close && (!line_end || close < line_end)) length = (size_t)(close - endpoint);
        }
        if (length && (length != host_len || memcmp(endpoint, host, length))) {
            if (list_irc_endpoint(irc, text, end, 'c', endpoint, length, nick, nick_len) < 0)
                return -1;
        }
        line = strchr(line, '\n');
        if (line) ++line;
    }
    irc->topology_found = true;
    return 0;
}

static int
list_irc_event(void *opaque, const struct snag_session *state, uint64_t seq, const char *type,
    const json_t *data, char *error, size_t error_size)
{
    struct list_irc *irc = opaque;
    (void)state;
    (void)error;
    (void)error_size;

    if (seq && seq == irc->checkpoint_seq) return SNAG_JOURNAL_STOP_AFTER;
    if (!irc->topology_found && !strcmp(type, "irc_snapshot")) {
        const char *text = snag_json_string(data, "text");
        if (text && list_irc_topology(irc, text) < 0) return -1;
    }
    if ((!irc->prompt_found || !irc->topology_found) &&
        snag_string_in(type, "irc_event irc_event_v2")) {
        struct snag_irc_event event;
        if (snag_irc_event_record_read(type, data, &event) < 0) return -1;
        if (!irc->topology_found && event.kind == SNAG_IRC_NICK && !event.historical &&
            (!event.routed || (event.route.kind == SNAG_IRC_CONNECTION_EVENTS && event.local)) &&
            event.text[0] && strlen(event.text) <= SNAG_CONFIG_IRC_NICK_MAX) {
            if (!irc->renames) irc->renames = json_array();
            if (!irc->renames || json_array_append(irc->renames, (json_t *)data) < 0) return -1;
        }
        char reference[96];
        (void)snprintf(reference, sizeof(reference), "[IRC update id=%s:%llu ", event.stream,
            (unsigned long long)event.sequence);
        if (!irc->prompt_found && snag_irc_event_model_visible(&event) && event.stream[0] &&
            strstr(irc->prompt, reference) &&
            (!irc->wants_message || event.kind == SNAG_IRC_MESSAGE ||
                event.kind == SNAG_IRC_NOTICE)) {
            irc->preview.len = 0u;
            if (snag_buf_printf(&irc->preview, "%s: %s", event.endpoint,
                    event.text[0] ? event.text : snag_irc_kind_name(event.kind)) < 0)
                return -1;
            irc->prompt_found = true;
        }
    }
    return irc->topology_found && irc->prompt_found ? SNAG_JOURNAL_STOP_AFTER : 0;
}

static json_t *
list_cells(
    struct snag_session *session, const char *state, unsigned int columns)
{
    struct list_irc irc = {.prompt = session->last_user,
        .preview = {.max = SNAG_MAX_DIRECT_PROMPT},
        .endpoints = {.max = SNAG_MAX_IRC_SNAPSHOT}};
    struct snag_buf cell = {.max = SNAG_MAX_DIRECT_PROMPT};
    json_t *cells = NULL;
    uint64_t before = 0u;
    char turns[32];

    const char *topology = snag_json_string(session->strings, "irc_snapshot");
    if (list_irc_prompt(&irc) < 0) goto out;
    /* The saved disconnected snapshot needs no nick history. Old checkpoints
     * may retain this state after its original IRC event leaves recent context. */
    irc.topology_found = topology && (!*topology ||
        (strstr(topology, "\nhosted: no\nno active endpoints\n") &&
         !strstr(topology, "\ndestination[") && !strstr(topology, "\nendpoint[") &&
         !strstr(topology, "\nhistory:\n")));
    const json_t *recent = json_object_get(session->checkpoint_context, "recent");
    const json_t *history = json_object_get(session->checkpoint_context, "history_sources");
    bool captured = snag_context_capture_seam(session, &recent, &history) == 0;
    if (captured && !irc.topology_found) {
        bool topology_covered = false;
        for (size_t i = 0u; i < json_array_size(recent); ++i) {
            const char *type = snag_json_string(json_array_get(recent, i), "type");
            if (type && !strcmp(type, "irc_snapshot")) topology_covered = true;
        }
        if (!topology_covered) {
            captured = false;
            recent = json_object_get(session->checkpoint_context, "recent");
        }
    }
    if (captured) {
        /* Native admission already materialized the current lookup closure.
         * Rewalking the journal here duplicates recovery and can read gigabytes. */
        for (size_t i = json_array_size(recent);
             i && (!irc.topology_found || !irc.prompt_found);) {
            const json_t *entry = json_array_get(recent, --i);
            const char *type = snag_json_string(entry, "type");
            if (type && list_irc_event(&irc, NULL, 0u, type,
                    json_object_get(entry, "data"), NULL, 0u) < 0) goto out;
        }
        for (size_t i = json_array_size(history); !irc.prompt_found && i; ) {
            const json_t *entry = json_array_get(history, --i);
            const char *type = snag_json_string(entry, "type");
            if (type && list_irc_event(&irc, NULL, 0u, type,
                    json_object_get(entry, "data"), NULL, 0u) < 0) goto out;
        }
    }
    if (!captured) {
        for (size_t i = 0u; i < json_array_size(recent); ++i) {
            const json_t *entry = json_array_get(recent, i);
            const char *type = snag_json_string(entry, "type");
            if (type && !strcmp(type, "irc_snapshot")) {
                irc.checkpoint_seq = session->checkpoint_seq;
                break;
            }
        }
        if (irc.topology_found) irc.checkpoint_seq = session->checkpoint_seq;
        size_t scan_bytes = SIZE_MAX;
        if (irc.checkpoint_seq && !session->binary &&
            session->history_cursor.next_seq == irc.checkpoint_seq + 1u &&
            session->history_cursor.offset >= 0 &&
            session->history_cursor.offset <= session->log_end) {
            uint64_t suffix_bytes = (uint64_t)(session->log_end - session->history_cursor.offset);
            if (suffix_bytes <= SIZE_MAX) {
                /* The admitted checkpoint already supplies the next lookup.
                 * Spend the existing reverse-scan budget only on its suffix. */
                scan_bytes = (size_t)suffix_bytes;
                before = irc.checkpoint_seq;
            }
        }
        if (scan_bytes && (!irc.topology_found || !irc.prompt_found)) {
            (void)snag_session_each_event_reverse(
                session, 0u, scan_bytes, list_irc_event, &irc, &before, NULL, 0u);
        }
        if (irc.checkpoint_seq) {
            /* Apply fresh suffix events before the verified recent checkpoint data.
             * The snapshot boundary keeps old or unrelated renames out of the list. */
            irc.checkpoint_seq = 0u;
            if (before == session->checkpoint_seq + 1u) before = session->checkpoint_seq;
            for (size_t i = json_array_size(recent);
                 i && (!irc.topology_found || !irc.prompt_found);) {
                const json_t *entry = json_array_get(recent, --i);
                const char *type = snag_json_string(entry, "type");
                if (type && list_irc_event(&irc, NULL, 0u, type,
                        json_object_get(entry, "data"), NULL, 0u) < 0)
                    goto out;
            }
            if (before && (!irc.topology_found || !irc.prompt_found)) {
                (void)snag_session_each_event_reverse(
                    session, before, SIZE_MAX, list_irc_event, &irc, &before, NULL, 0u);
            }
        }
    } else if (!irc.topology_found || !irc.prompt_found) {
        (void)snag_session_each_event_reverse(
            session, 0u, SIZE_MAX, list_irc_event, &irc, &before, NULL, 0u);
    }
    if (!irc.topology_found && topology) {
        json_array_clear(irc.renames);
        if (list_irc_topology(&irc, topology) < 0) goto out;
    }
    if (snag_buf_terminate(&irc.preview) < 0 || snag_buf_terminate(&irc.endpoints) < 0) goto out;
    (void)snprintf(turns, sizeof(turns), "%llu", (unsigned long long)session->turn_count);
    const char *values[] = {session->id, session->name ? session->name : "-",
        session->default_model, turns,
        state, irc.preview.len ? (const char *)irc.preview.data : session->last_user,
        irc.endpoints.len ? (const char *)irc.endpoints.data : "-"};
    cells = json_array();
    for (size_t i = 0u; cells && i < 7u; ++i) {
        cell.len = 0u;
        if (append_list_preview(&cell, values[i], i >= 5u && columns ? columns : 80u, false) < 0 ||
            json_array_append_new(cells, json_stringn((const char *)cell.data, cell.len)) < 0) {
            json_decref(cells);
            cells = NULL;
        }
    }
out:
    json_decref(irc.renames);
    snag_buf_free(&cell);
    snag_buf_free(&irc.preview);
    snag_buf_free(&irc.endpoints);
    return cells;
}

/* A catalog is disposable display metadata, never a recovery checkpoint. */
/* The terminal size protocol has 16-bit columns. Keep enough preview for any
 * supported terminal, without copying an entire multi-megabyte direct prompt. */
#define CATALOG_COLUMNS UINT16_MAX
#define CATALOG_MAX ((7u * SNAG_MAX_DIRECT_PROMPT + SNAG_PATH_MAX_BYTES) * 6u + 2048u)

static void
catalog_source(const snag_file_info *info, json_int_t values[8])
{
    long modified = 0;
    long changed = 0;
    int64_t change_seconds = 0;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    modified = info->st_mtimespec.tv_nsec;
    changed = info->st_ctimespec.tv_nsec;
    change_seconds = info->st_ctime;
#elif defined(_WIN32)
    modified = info->mtime_nsec;
#else
    modified = info->st_mtim.tv_nsec;
    changed = info->st_ctim.tv_nsec;
    change_seconds = info->st_ctime;
#endif
    values[0] = (json_int_t)info->st_dev;
    values[1] = (json_int_t)info->st_ino;
    values[2] = (json_int_t)info->st_size;
    values[3] = (json_int_t)info->st_mode;
    values[4] = (json_int_t)info->st_mtime;
    values[5] = (json_int_t)modified;
    values[6] = (json_int_t)change_seconds;
    values[7] = (json_int_t)changed;
}

static int
catalog_source_header(const snag_file_info *source, char *text, size_t size)
{
    json_int_t values[8];
    catalog_source(source, values);
    return snprintf(text, size, "{\"source\":[%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld],",
        (long long)values[0], (long long)values[1], (long long)values[2],
        (long long)values[3], (long long)values[4], (long long)values[5],
        (long long)values[6], (long long)values[7]);
}

static json_t *
catalog_read(int directory, const char *id, const snag_file_info *source, unsigned int columns)
{
    int fd = snag_open_read_security_at(directory, "catalog.json", false);
    snag_file_info info;
    json_t *row = NULL;
    if (fd < 0) return NULL;
    if (snag_store_verify_private_fd(fd, false, "session catalog", NULL, 0u) == 0 &&
        snag_fstat(fd, &info) == 0 && info.st_size > 0 && info.st_size <= CATALOG_MAX) {
        unsigned char *bytes = malloc((size_t)info.st_size);
        if (bytes && snag_pread(fd, bytes, (size_t)info.st_size, 0) == info.st_size) {
            char header[256];
            int length = source ? catalog_source_header(source, header, sizeof(header)) : 0;
            /* Reject stale generations before the JSON parser copies their text.
             * A cache predating this layout is disposable and rebuilt once. */
            if (length >= 0 && (size_t)length < sizeof(header) &&
                (size_t)length <= (size_t)info.st_size &&
                (!length || !memcmp(bytes, header, (size_t)length)))
                row = snag_json_load_strict(bytes, (size_t)info.st_size, CATALOG_MAX, NULL, 0u);
        }
        free(bytes);
    }
    (void)close(fd);
    uint64_t version;
    uint64_t activity;
    uint64_t width;
    const char *saved_id = json_string_value(
        json_array_get(json_object_get(row, "cells"), 0u));
    const char *name = snag_json_string(row, "name");
    const json_t *cells = json_object_get(row, "cells");
    bool valid = snag_json_integer_u64(row, "version", &version) == 0 && version == 2u &&
        snag_json_integer_u64(row, "columns", &width) == 0 &&
        width >= columns && width <= CATALOG_COLUMNS &&
        saved_id && !strcmp(saved_id, id) && name && strlen(name) <= SNAG_PATH_MAX_BYTES &&
        json_string_length(json_object_get(row, "name")) == strlen(name) &&
        snag_json_integer_u64(row, "activity_ms", &activity) == 0 &&
        json_is_array(cells) && (json_array_size(cells) == 7u ||
            (!width && json_array_size(cells) == 1u));
    for (size_t i = 0u; valid && i < json_array_size(cells); ++i) {
        const char *text = json_string_value(json_array_get(cells, i));
        valid = text && json_string_length(json_array_get(cells, i)) == strlen(text) &&
            strlen(text) <= SNAG_MAX_DIRECT_PROMPT;
    }
    const json_t *saved_source = json_object_get(row, "source");
    valid = valid && json_is_array(saved_source) && json_array_size(saved_source) == 8u;
    for (size_t i = 0u; valid && i < 8u; ++i)
        valid = json_is_integer(json_array_get(saved_source, i));
    if (valid && source) {
        json_int_t values[8];
        catalog_source(source, values);
        for (size_t i = 0u; valid && i < 8u; ++i)
            valid = values[i] == json_integer_value(json_array_get(saved_source, i));
    }
    if (valid) return row;
    json_decref(row);
    return NULL;
}

static int
catalog_emit(const char *bytes, size_t length, void *opaque)
{
    return snag_write_full(*(int *)opaque, bytes, length);
}

static bool
catalog_write(struct snag_session *session, json_t *cells,
    const snag_file_info *before, unsigned int columns)
{
    snag_file_info after;
    char random[SNAG_ID_HEX_LEN + 1u];
    char temporary[sizeof(".catalog-") + SNAG_ID_HEX_LEN];
    if (session->dir_fd < 0 || session->log_fd < 0 ||
        snag_fstat(session->log_fd, &after) < 0 || !snag_file_unchanged(before, &after) ||
        snag_random_id(random) < 0) return false;
    (void)snprintf(temporary, sizeof(temporary), ".catalog-%s", random);
    int fd = snag_create_private_at(session->dir_fd, temporary, true);
    if (fd < 0) return false;
    char metadata[512];
    int length = catalog_source_header(before, metadata, sizeof(metadata));
    bool written = length > 0 && (size_t)length < sizeof(metadata) &&
        snag_write_full(fd, metadata, (size_t)length) == 0;
    length = snprintf(metadata, sizeof(metadata),
        "\"version\":2,\"columns\":%u,\"activity_ms\":%llu,\"name\":",
        columns, (unsigned long long)session->last_time_ms);
    written = written && length > 0 && (size_t)length < sizeof(metadata) &&
        snag_write_full(fd, metadata, (size_t)length) == 0;
    const json_t *name = json_object_get(session->strings, "name");
    size_t flags = JSON_COMPACT | JSON_ENSURE_ASCII | JSON_ENCODE_ANY;
    const char *text = json_string_value(name);
    if (text && snag_session_name_valid(text) && !strpbrk(text, "\"\\")) {
        /* Valid unescaped names can be written directly from the snapshot. */
        written = written && snag_write_full(fd, "\"", 1u) == 0 &&
            snag_write_full(fd, text, json_string_length(name)) == 0 &&
            snag_write_full(fd, "\"", 1u) == 0;
    } else {
        written = written && (name ? json_dump_callback(name, catalog_emit, &fd, flags) == 0 :
            snag_write_full(fd, "\"\"", 2u) == 0);
    }
    written = written && snag_write_full(fd, ",\"cells\":", 9u) == 0;
    if (cells) {
        written = written && json_dump_callback(cells, catalog_emit, &fd, flags) == 0;
    } else {
        written = written && snag_write_full(fd, "[\"", 2u) == 0 &&
            snag_write_full(fd, session->id, SNAG_ID_HEX_LEN) == 0 &&
            snag_write_full(fd, "\"]", 2u) == 0;
    }
    written = written && snag_write_full(fd, "}", 1u) == 0;
    if (close(fd) < 0) written = false;
    /* A power-loss miss rebuilds disposable metadata from the synced journal.
     * Stream borrowed cells; do not allocate another row or serialized copy. */
    bool published = written && snag_fstat(session->log_fd, &after) == 0 &&
        snag_file_unchanged(before, &after) &&
        snag_rename_at(session->dir_fd, temporary, session->dir_fd, "catalog.json") == 0;
    (void)snag_unlink_at(session->dir_fd, temporary, false);
    return published;
}

static json_t *
catalog_metrics(struct snag_store *store, const char *id)
{
    struct snag_metrics_view metrics;
    if (snag_metrics_read(store->root_fd, id, &metrics, NULL, 0u) < 0 || !metrics.valid)
        return NULL;
    char turns[32];
    (void)snprintf(turns, sizeof(turns), "%llu", (unsigned long long)metrics.turns);
    return json_pack("[s,s,s,s,s,s,s]", id, "-", metrics.model, turns, "stored", "-", "-");
}

static bool
load_catalog_row(struct snag_store *store, struct snag_session *snapshot, const char *id,
    unsigned int columns, enum catalog_content content, bool (*cancel)(void *),
    void *cancel_opaque, struct snag_sha256 *pending, struct catalog_row *summary)
{
    if (!columns && content != CATALOG_IDENTITY) columns = 80u;
    if (strlen(id) != SNAG_ID_HEX_LEN || !snag_hex_is_lower(id, SNAG_ID_HEX_LEN)) return false;
    snag_session_init(snapshot);
    /* Discovery and read-only recovery need only private descriptors. Reuse
     * these same descriptors on a miss instead of reopening the source. */
    snapshot->dir_fd = snag_open_read_security_at(store->sessions_fd, id, true);
    if (snapshot->dir_fd < 0 || snag_store_verify_private_fd(snapshot->dir_fd, true,
            "session directory", NULL, 0u) < 0) goto fail;
    bool native = true;
    snapshot->log_fd = snag_open_read_security_at(snapshot->dir_fd, "journal.bin", false);
    if (snapshot->log_fd < 0 && errno == ENOENT) {
        native = false;
        snapshot->log_fd = snag_open_read_security_at(snapshot->dir_fd, "events.jsonl", false);
    }
    snag_file_info before;
    if (snapshot->log_fd < 0 ||
        snag_store_verify_private_fd(snapshot->log_fd, false, "journal", NULL, 0u) < 0 ||
        snag_fstat(snapshot->log_fd, &before) < 0) goto fail;
    if (content == CATALOG_MONITOR) {
        /* Metrics already supply every monitor field. Do not queue journal
         * verification for a live owner on each append, or replay a completed
         * conversation only to discard its preview in the monitor. */
        json_t *cells = catalog_metrics(store, id);
        if (cells) {
            *summary = (struct catalog_row){.owner = cells, .cells = cells};
            return true;
        }
    }
    json_t *row = catalog_read(snapshot->dir_fd, id, &before, columns);
    if (row) goto cached;
    json_decref(row);
    if (pending) {
        json_int_t source[8];
        catalog_source(&before, source);
        snag_sha256_update(pending, id, SNAG_ID_HEX_LEN);
        snag_sha256_update(pending, source, sizeof(source));
        /* A picker may show the last metadata from an append-only owner while
         * its worker verifies the new boundary. Replacements never reuse it. */
        row = catalog_read(snapshot->dir_fd, id, NULL, columns);
        const json_t *stamp = json_object_get(row, "source");
        if (row && json_integer_value(json_array_get(stamp, 0u)) == (json_int_t)before.st_dev &&
            json_integer_value(json_array_get(stamp, 1u)) == (json_int_t)before.st_ino &&
            json_integer_value(json_array_get(stamp, 2u)) <= before.st_size) goto cached;
        json_decref(row);
        if (content == CATALOG_PREVIEW) {
            json_t *cells = catalog_metrics(store, id);
            if (cells) {
                *summary = (struct catalog_row){.owner = cells, .cells = cells};
                return true;
            }
        }
        goto fail;
    }
    memcpy(snapshot->id, id, sizeof(snapshot->id));
    snapshot->history_cancel = cancel;
    snapshot->history_cancel_opaque = cancel_opaque;
    if (read_snapshot(snapshot, native) < 0 || snapshot->delete_requested) goto fail;
    snag_file_info after;
    if (snag_fstat(snapshot->log_fd, &after) < 0 ||
        !snag_file_unchanged(&before, &after)) goto fail;
    *summary = (struct catalog_row){.name = snapshot->name,
        .activity_ms = snapshot->last_time_ms};
    /* Identity-only discovery retains the source witness and full name, with
     * no preview/history formatting. A later display fills the other cells. */
    if (content == CATALOG_IDENTITY) {
        summary->published = catalog_write(snapshot, NULL, &before, 0u);
        return true;
    }
    json_t *cells = list_cells(snapshot, "stored", columns);
    if (!cells) goto fail;
    summary->owner = cells;
    summary->cells = cells;
    summary->published = catalog_write(snapshot, cells, &before, columns);
    return true;
cached:
    *summary = (struct catalog_row){.owner = row,
        .cells = json_object_get(row, "cells"), .name = snag_json_string(row, "name"),
        .activity_ms = (uint64_t)json_integer_value(json_object_get(row, "activity_ms")),
        .published = true};
    return true;
fail:
    snag_session_close(snapshot);
    return false;
}

static size_t
list_text_width(const char *text)
{
    size_t width = 0u;
    size_t length = strlen(text);
    for (size_t offset = 0u; offset < length;) {
        uint32_t cp;
        size_t bytes = snag_utf8_decode((const unsigned char *)text + offset, length - offset, &cp);
        if (!bytes) break;
        int cells = snag_char_width(cp);
        if (cells > 0) width += (size_t)cells;
        offset += bytes;
    }
    return width;
}

struct session_list_row {
    json_t *cells;
    json_t *id; /* Retain the original cell while replacing its visible prefix. */
    uint64_t time_ms;
    unsigned int status;
    unsigned char prefix;
};

static int
compare_list_rows(const void *left, const void *right)
{
    const struct session_list_row *a = left, *b = right;

    if (a->status != b->status) return a->status < b->status ? -1 : 1;
    if (a->time_ms != b->time_ms) return a->time_ms > b->time_ms ? -1 : 1;
    return strcmp(json_string_value(json_array_get(b->cells, 0u)),
        json_string_value(json_array_get(a->cells, 0u)));
}

static int
compare_ids(const void *left, const void *right)
{
    const struct session_list_row *a = left;
    const struct session_list_row *b = right;
    return strcmp(json_string_value(a->id), json_string_value(b->id));
}

static void
extend_prefix(struct session_list_row *row, const char *id)
{
    const char *row_id = json_string_value(row->id);
    size_t same = 0u;
    while (same < SNAG_ID_HEX_LEN && id[same] == row_id[same]) ++same;
    if (same < SNAG_ID_HEX_LEN && same >= row->prefix)
        row->prefix = (unsigned char)(same + 1u);
}

static int
shorten_catalog_ids(struct snag_store *store, struct session_list_row *rows, size_t count)
{
    /* Only the adjacent listed IDs can share the longest prefix with an entry.
     * Stream all selectors, including trash and unreadable sessions, without a
     * second ID array or one directory traversal per displayed row. */
    qsort(rows, count, sizeof(*rows), compare_ids);
    for (size_t i = 0u; i < count; ++i) rows[i].prefix = 8u;
    for (unsigned int trash = 0u; trash < 2u; ++trash) {
        struct snag_directory *dir = open_store_dir(store, trash ? "trash" : "sessions",
            "session selectors", NULL, 0u);
        if (!dir) return -1;
        const char *entry;
        while ((entry = snag_directory_next(dir))) {
            char trash_id[SNAG_ID_HEX_LEN + 1u];
            const char *id = entry;
            if (trash) {
                if (!snag_store_trash_id(entry, trash_id)) continue;
                id = trash_id;
            } else if (strlen(entry) != SNAG_ID_HEX_LEN ||
                !snag_hex_is_lower(entry, SNAG_ID_HEX_LEN)) continue;
            size_t low = 0u;
            size_t high = count;
            while (low < high) {
                size_t middle = low + (high - low) / 2u;
                if (strcmp(json_string_value(rows[middle].id), id) < 0) low = middle + 1u;
                else high = middle;
            }
            if (low) extend_prefix(&rows[low - 1u], id);
            if (low < count && !strcmp(json_string_value(rows[low].id), id)) ++low;
            if (low < count) extend_prefix(&rows[low], id);
        }
        if (finish_directory(dir, NULL, 0u) < 0) return -1;
    }
    for (size_t i = 0u; i < count; ++i) {
        if (json_array_set_new(rows[i].cells, 0u,
                json_stringn(json_string_value(rows[i].id), rows[i].prefix)) < 0) return -1;
    }
    return 0;
}

static int
emit_list(const struct session_list_row *rows, size_t count, unsigned int columns,
    snag_store_emit_fn emit, void *opaque)
{
    static const char *const headers[] = {
        "SESSION", "NAME", "MODEL", "TURNS", "STATUS", "LAST PROMPT", "IRC"};
    size_t widths[] = {8u, 4u, 5u, 5u, 6u, 11u, 3u};
    struct snag_buf line = {.max = SNAG_MAX_DIRECT_PROMPT};
    int rc = -1;

    for (size_t row = 0u; row < count; ++row) {
        for (size_t col = 0u; col < 5u; ++col) {
            size_t width = list_text_width(json_string_value(json_array_get(rows[row].cells, col)));
            if (width > widths[col]) widths[col] = width;
        }
    }
    if (columns) {
        size_t fixed = 12u;
        for (size_t col = 0u; col < 5u; ++col) fixed += widths[col];
        /* Keep both final headings readable, then divide every remaining cell equally. */
        while (fixed + 2u * strlen(headers[5]) > columns) {
            size_t widest = widths[1] > widths[2] ? 1u : 2u;
            if (widths[widest] <= 1u) {
                widest = 3u;
                for (size_t col = 3u; col < 5u; ++col) {
                    if (widths[col] > widths[widest]) widest = col;
                }
            }
            if (widths[widest] <= 1u) break;
            --widths[widest];
            --fixed;
        }
        size_t remaining = columns > fixed ? columns - fixed : 0u;
        widths[5] = remaining / 2u;
        widths[6] = remaining - widths[5];
    }
    for (size_t row = 0u; row <= count; ++row) {
        line.len = 0u;
        for (size_t col = 0u; col < 7u; ++col) {
            const char *text =
                row ? json_string_value(json_array_get(rows[row - 1u].cells, col)) : headers[col];
            if ((col && snag_buf_append(&line, columns ? "  " : "\t", columns ? 2u : 1u) < 0) ||
                append_list_preview(
                    &line, text, columns ? widths[col] : 80u, columns && col != 6u) < 0)
                goto out;
        }
        if (snag_buf_putc(&line, '\n') < 0 || emit(opaque, (const char *)line.data, line.len) < 0)
            goto out;
    }
    rc = 0;
out:
    snag_buf_free(&line);
    return rc;
}

static int
list_sessions(struct snag_store *store, const char *owned_id, uint64_t stored_limit,
    unsigned int columns, snag_store_emit_fn emit, void *opaque, json_t *catalog,
    bool (*cancel)(void *), void *cancel_opaque, struct snag_sha256 *pending,
    enum catalog_content content, char *error, size_t error_size)
{
    struct snag_directory *dir = open_sessions_dir(store, error, error_size);
    const char *entry;
    struct session_list_row *rows = NULL;
    struct snag_session *snapshot = NULL;
    size_t count = 0u, capacity = 0u;
    int rc = -1;

    if (!dir) goto out;
    snapshot = malloc(sizeof(*snapshot));
    if (!snapshot) goto out;
    while ((entry = snag_directory_next(dir)) != NULL) {
        if (cancel && cancel(cancel_opaque)) goto canceled;
        struct catalog_row summary;
        if (!load_catalog_row(
                store, snapshot, entry, columns, content,
                cancel, cancel_opaque, pending, &summary)) {
            if (cancel && cancel(cancel_opaque)) goto canceled;
            continue;
        }
        /* Closing a second descriptor of our lock drops the owner's POSIX lock. */
        bool live = owned_id && !strcmp(owned_id, entry);
        if (!live) live = snag_session_is_live(snapshot);
        if (!stored_limit && !live) {
            json_decref(summary.owner);
            snag_session_close(snapshot);
            continue;
        }
        struct snag_metrics_view metrics;
        unsigned int status = live ? 2u : 3u;
        if (live && content != CATALOG_MONITOR && content != CATALOG_PREVIEW) {
            int attachment = snag_session_endpoint_status(snapshot->dir_fd, snapshot->dir_path);
            if (attachment >= 0) status = attachment ? 0u : 1u;
        } else if (live &&
            snag_metrics_read(store->root_fd, entry, &metrics, NULL, 0u) == 0) {
            if (!strcmp(metrics.state, "attached")) status = 0u;
            else if (!strcmp(metrics.state, "detached")) status = 1u;
        }
        static const char *const states[] = {"attached", "detached", "running", "stored"};
        uint64_t time_ms = summary.activity_ms;
        json_t *cells = json_incref(summary.cells);
        const char *state = json_string_value(json_array_get(cells, 4u));
        if (cells && (!state || strcmp(state, states[status])))
            (void)json_array_set_new(cells, 4u, json_string(states[status]));
        json_decref(summary.owner);
        snag_session_close(snapshot);
        if (cancel && cancel(cancel_opaque)) {
            json_decref(cells);
            goto canceled;
        }
        if (!cells) goto out;
        if (count == capacity) {
            size_t next = capacity ? capacity * 2u : 16u;
            struct session_list_row *grown = NULL;
            if (next > capacity && next <= SIZE_MAX / sizeof(*rows))
                grown = realloc(rows, next * sizeof(*rows));
            if (!grown) {
                json_decref(cells);
                goto out;
            }
            rows = grown;
            capacity = next;
        }
        rows[count] =
            (struct session_list_row){.cells = cells, .time_ms = time_ms, .status = status};
        rows[count].id = json_incref(json_array_get(cells, 0u));
        ++count;
    }
    if (cancel && cancel(cancel_opaque)) goto canceled;
    rc = finish_directory(dir, error, error_size);
    dir = NULL;
    if (rc < 0) goto out;
    if (!count) {
        if (!catalog) (void)snag_errorf(error, error_size, "no matching sessions");
    } else {
        if (shorten_catalog_ids(store, rows, count) < 0) { rc = -1; goto out; }
        qsort(rows, count, sizeof(*rows), compare_list_rows);
        size_t visible = 0u;
        uint64_t stored = 0u;
        while (visible < count) {
            if (rows[visible].status == 3u && stored++ >= stored_limit) break;
            ++visible;
        }
        if (catalog) {
            for (size_t i = 0u; i < visible; ++i) {
                json_t *row = json_pack("{s:s,s:I,s:i,s:O}", "id",
                    json_string_value(rows[i].id), "activity_ms", (json_int_t)rows[i].time_ms,
                    "status_rank", (int)rows[i].status, "cells",
                    rows[i].cells);
                if (!row || json_array_append_new(catalog, row) < 0) {
                    rc = -1;
                    break;
                }
            }
        } else {
            rc = emit_list(rows, visible, columns, emit, opaque);
        }
    }
    goto out;
canceled:
    rc = snag_fail(error, error_size, ECANCELED, "session list loading canceled");
out:
    if (dir) (void)snag_directory_close(dir);
    for (size_t row = 0u; row < count; ++row) {
        json_decref(rows[row].cells);
        json_decref(rows[row].id);
    }
    free(rows);
    free(snapshot);
    if (rc < 0 && error_size && !error[0])
        (void)snag_errorf(error, error_size, "cannot write session list");
    return rc;
}

int
snag_store_list(struct snag_store *store, const struct snag_session *owned, uint64_t stored_limit,
    unsigned int columns, snag_store_emit_fn emit, void *opaque, char *error,
    size_t error_size)
{
    return list_sessions(store, owned && owned->lock_fd >= 0 ? owned->id : NULL,
        stored_limit, columns, emit, opaque, NULL, NULL,
        NULL, NULL, CATALOG_CELLS, error, error_size);
}

json_t *
snag_store_catalog(struct snag_store *store, const char *owned_id,
    uint64_t stored_limit, bool (*cancel)(void *), void *cancel_opaque, char *error,
    size_t error_size)
{
    json_t *rows = json_array();
    if (rows && list_sessions(store, owned_id, stored_limit, 0u, NULL, NULL, rows, cancel,
                    cancel_opaque, NULL, CATALOG_CELLS, error, error_size) < 0) {
        json_decref(rows);
        rows = NULL;
    }
    return rows;
}

json_t *
snag_store_catalog_preview(struct snag_store *store, const char *owned_id,
    uint64_t stored_limit, bool (*cancel)(void *), void *opaque, bool *incomplete,
    char *error, size_t error_size)
{
    struct snag_sha256 pending;
    snag_sha256_init(&pending);
    *incomplete = false;
    json_t *rows = json_array();
    if (rows && list_sessions(store, owned_id, stored_limit, 0u, NULL, NULL, rows, cancel,
            opaque, &pending, CATALOG_PREVIEW, error, error_size) < 0) {
        json_decref(rows);
        return NULL;
    }
    *incomplete = pending.bit_count != 0u;
    return rows;
}

json_t *
snag_store_catalog_monitor(
    struct snag_store *store, char pending[SNAG_SHA256_HEX_LEN + 1u],
    char *error, size_t error_size)
{
    struct snag_sha256 source;
    snag_sha256_init(&source);
    if (pending) pending[0] = '\0';
    json_t *rows = json_array();
    if (rows && list_sessions(store, NULL, UINT64_MAX, 0u, NULL, NULL, rows, NULL,
            NULL, pending ? &source : NULL, CATALOG_MONITOR, error, error_size) < 0) {
        json_decref(rows);
        return NULL;
    }
    if (pending && source.bit_count) snag_sha256_final_hex(&source, pending);
    return rows;
}

int
snag_store_catalog_refresh(struct snag_store *store, bool (*cancel)(void *), void *opaque,
    char *error, size_t error_size)
{
    struct snag_directory *directory = open_sessions_dir(store, error, error_size);
    if (!directory) return -1;
    struct snag_session *snapshot = malloc(sizeof(*snapshot));
    if (!snapshot) { (void)snag_directory_close(directory); return -1; }
    const char *id;
    int rc = 0;
    while ((id = snag_directory_next(directory))) {
        if (cancel && cancel(opaque)) {
            rc = snag_fail(error, error_size, ECANCELED, "catalog refresh canceled");
            break;
        }
        struct catalog_row row;
        if (strlen(id) != SNAG_ID_HEX_LEN || !snag_hex_is_lower(id, SNAG_ID_HEX_LEN)) continue;
        struct snag_metrics_view metrics;
        if (snag_metrics_read(store->root_fd, id, &metrics, NULL, 0u) == 0 && metrics.valid)
            continue;
        if (load_catalog_row(store, snapshot, id, 0u, CATALOG_CELLS,
                cancel, opaque, NULL, &row)) {
            if (!row.published)
                rc = snag_fail(error, error_size, EIO, "cannot publish session metadata");
            json_decref(row.owner);
            snag_session_close(snapshot);
        } else {
            rc = snag_fail(error, error_size, EIO, "cannot read session metadata");
        }
    }
    free(snapshot);
    int closed = finish_directory(directory, error, error_size);
    return rc < 0 ? rc : closed;
}
