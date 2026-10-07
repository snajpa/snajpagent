/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_internal.h"
#include "media.h"
#include "base.h"
#include "fs.h"
#include "irc.h"
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
open_store_dir(struct snag_store *store, const char *name, const char *label, char *error, size_t error_size)
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
        !snag_hex_is_lower(name + SNAG_ID_HEX_LEN + 1u, SNAG_TRASH_SUFFIX_HEX_LEN)) return false;
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
resolve_prefix(struct snag_store *store, const char *prefix,
               struct resolved_session *target, bool include_trash,
               snag_store_emit_fn matches_emit, void *opaque, char *error, size_t error_size)
{
    size_t len = prefix ? strlen(prefix) : 0u;
    unsigned int matches = 0;

    memset(target, 0, sizeof(*target));
    if (!len || len > SNAG_ID_HEX_LEN || !snag_hex_is_lower(prefix, len))
        return snag_fail(error, error_size, EINVAL,
            "session id must be 1..32 lowercase hex characters");
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
                if (strlen(entry) != SNAG_ID_HEX_LEN || !snag_hex_is_lower(entry, SNAG_ID_HEX_LEN)) continue;
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
                    return snag_fail(error, error_size, saved,
                                     "cannot write matching session ids");
                }
            }
        }
        if (finish_directory(dir, error, error_size) < 0) return -1;
    }

    if (matches != 1u) {
        snag_errorf(error, error_size, matches ? "session id prefix is ambiguous" :
                  "session id was not found");
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
    if (session->dir_fd < 0) return snag_errorf(error, error_size, "cannot open session %s: %s", id,
                  strerror(errno));
    return snag_store_verify_private_fd(session->dir_fd, true, "session directory", error, error_size);
}

int
snag_session_locate(struct snag_store *store, struct snag_session *session,
                    const char *prefix, snag_store_emit_fn matches_emit, void *opaque,
                    char *error, size_t error_size)
{
    struct resolved_session target;

    if (session->dir_fd >= 0 || session->log_fd >= 0 || session->lock_fd >= 0 ||
        session->dir_path) {
        return snag_fail(error, error_size, EINVAL, "session location is already open");
    }
    if (resolve_prefix(store, prefix, &target, false, matches_emit, opaque,
                       error, error_size) < 0) return -1;
    /* Attachment only needs the verified directory. Opening a writer or replaying
     * the journal here would turn a lookup into a second session runner. */
    return snag_store_open_session_directory(store, session, target.id, error, error_size);
}

static int
open_full_id(struct snag_store *store, struct snag_session *session,
             const char *id, char *error, size_t error_size)
{
    if (snag_store_open_session_directory(store, session, id, error, error_size) < 0) return -1;
    int format = snag_store_open_session_files(session, false, error, error_size);
    if (format < 0) return -1;
    int rc = format ?
        snag_store_load_binary_session(session, SNAG_TAIL_TRUNCATE, error, error_size) :
        snag_store_scan_log(session, SNAG_TAIL_TRUNCATE, error, error_size);
    if (rc < 0) return -1;
    if (snag_media_work_remove(session->dir_fd,error,error_size)<0)return -1;
    if (snag_store_remove_upload_staging(session->dir_fd, error, error_size) < 0) return -1;
    if (session->delete_requested) {
        if (snag_session_complete_delete(store, session, error, error_size) < 0) return -1;
        snag_errorf(error, error_size, "session deletion was completed");
        return 1;
    }
    return 0;
}

int
snag_session_open(struct snag_store *store, struct snag_session *session,
                 const char *prefix, char *error, size_t error_size)
{
    struct resolved_session target;

    if (resolve_prefix(store, prefix, &target, true, NULL, NULL,
                       error, error_size) < 0) return -1;
    if (target.trash) {
        if (snag_store_complete_trash_delete(store, target.trash_name, error, error_size) < 0) return -1;
        snag_errorf(error, error_size, "session deletion was completed");
        return 1;
    }
    return open_full_id(store, session, target.id, error, error_size);
}

static int
open_snapshot(struct snag_store *store, struct snag_session *session,
              const char *id, char *error, size_t error_size)
{
    if (snag_store_open_session_directory(store, session, id, error, error_size) < 0) return -1;
    bool native = true;
    session->log_fd = snag_open_read_security_at(session->dir_fd, "journal.bin", false);
    if (session->log_fd < 0 && errno == ENOENT) {
        native = false;
        session->log_fd = snag_open_read_security_at(session->dir_fd, "events.jsonl", false);
    }
    if (session->log_fd < 0) return -1;
    if (snag_store_verify_private_fd(session->log_fd, false, "event log", error, error_size) < 0) return -1;
    session->snapshot_read_only = true;
    if (native) return snag_store_load_binary_session(session, SNAG_TAIL_IGNORE, error, error_size);
    session->log_end = snag_seek(session->log_fd, 0, SEEK_END);
    if (session->log_end < 0) return -1;
    int rc = snag_store_scan_log(session, SNAG_TAIL_IGNORE, error, error_size);
    session->snapshot_read_only = true;
    return rc;
}

/* On success the caller owns the snapshot; no selected-field copies. */
static int
matching_snapshot(struct snag_store *store, struct snag_session *snapshot,
    const char *id, bool (*cancel)(void *), void *cancel_opaque)
{
    char error[128];

    snag_session_init(snapshot);
    snapshot->history_cancel = cancel;
    snapshot->history_cancel_opaque = cancel_opaque;
    if (strlen(id) == SNAG_ID_HEX_LEN && snag_hex_is_lower(id, SNAG_ID_HEX_LEN) &&
        open_snapshot(store, snapshot, id, error, sizeof(error)) == 0 &&
        !snapshot->delete_requested) return 0;
    snag_session_close(snapshot);
    return -1;
}

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
        if (matching_snapshot(store, &snapshot, entry, NULL, NULL) < 0) continue;
        bool match = snapshot.name && !strcmp(snapshot.name, name);
        snag_session_close(&snapshot);
        if (!match) continue;
        if (!matches++) {
            memcpy(id, entry, SNAG_ID_HEX_LEN + 1u);
        } else if (matches_emit &&
                   ((matches == 2u && emit_match(matches_emit, opaque, id) < 0) ||
                    emit_match(matches_emit, opaque, entry) < 0)) {
            (void)snag_directory_close(dir);
            return snag_errorf(error, error_size, "cannot write matching session ids");
        }
    }
    if (finish_directory(dir, error, error_size) < 0) return -1;
    if (matches != 1u) {
        return snag_fail(error, error_size, matches ? EEXIST : ENOENT, matches ?
            "session name is ambiguous; select one of the matching ids" :
            "session name was not found");
    }
    return 0;
}

int
snag_store_find_last(struct snag_store *store, char id[SNAG_ID_HEX_LEN + 1u],
                     char *error, size_t error_size)
{
    struct snag_directory *dir;
    const char *entry;
    char best[SNAG_ID_HEX_LEN + 1u] = {0};
    uint64_t best_time = 0;

    dir = open_sessions_dir(store, error, error_size);
    if (!dir) return -1;
    while ((entry = snag_directory_next(dir)) != NULL) {
        struct snag_session snapshot;
        if (matching_snapshot(store, &snapshot, entry, NULL, NULL) < 0) continue;
        uint64_t last = snapshot.last_time_ms;
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
snag_session_open_last(struct snag_store *store, struct snag_session *session,
                       char *error, size_t error_size)
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
        size_t bytes = snag_utf8_decode((const unsigned char *)text + offset,
                                       length - offset, &cp);
        if (!bytes) return snag_errno(EILSEQ);
        if (cp <= 0x20u || (cp >= 0x7fu && cp <= 0x9fu) || cp == 0xadu ||
            cp == 0x61cu || cp == 0x200bu || cp == 0x200eu || cp == 0x200fu ||
            (cp >= 0x2028u && cp <= 0x202eu) || cp == 0x2060u ||
            (cp >= 0x2066u && cp <= 0x206fu) || cp == 0xfeffu ||
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
    return !strncmp(text, "[IRC update id=", 15u) ||
        !strncmp(text, "[IRC endpoint=", 14u);
}

static int
list_irc_prompt(struct list_irc *irc)
{
    const char *selected = NULL;
    const char *text = irc->prompt;

    irc->prompt_found = true;
    if (!text || !irc_prompt_header(text)) return 0;
    for (const char *line = text; line && *line; ) {
        if (irc_prompt_header(line)) {
            const char *end = strchr(line, '\n');
            const char *message = strstr(line, " event=message ");
            const char *notice = strstr(line, " event=notice ");
            bool chat = (message && (!end || message < end)) ||
                (notice && (!end || notice < end));
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
list_irc_endpoint(struct list_irc *irc, const char *text, const char *end,
    char role, const char *endpoint, size_t length, const char *nick, size_t nick_len)
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
    for (size_t i = json_array_size(irc->renames); nick_len && i; ) {
        const json_t *event = json_array_get(irc->renames, --i);
        const char *address = snag_json_string(event, "endpoint");
        if (strlen(address) == length && !memcmp(address, endpoint, length) &&
            list_nick_equal(nick, nick_len, snag_json_string(event, "nick"))) {
            nick = snag_json_string(event, "text");
            nick_len = strlen(nick);
        }
    }
    return snag_buf_printf(&irc->endpoints, "%s%c/%.*s%s%.*s",
        irc->endpoints.len ? "," : "", role, (int)nick_len, nick ? nick : "",
        nick_len ? "@" : "", (int)length, endpoint);
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
        if (list_irc_endpoint(irc, text, end, 's', host, host_len, nick, nick_len) < 0)
            return -1;
    }
    const char *destination = strstr(text, "\ndestination[");
    bool destinations = destination && destination < end;
    for (const char *line = text; line && line < end; ) {
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
list_irc_event(void *opaque, const struct snag_session *state, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t error_size)
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
            event.text[0] && strlen(event.text) <= SNAG_CONFIG_IRC_NICK_MAX &&
            json_array_append(irc->renames, (json_t *)data) < 0) return -1;
        char reference[96];
        (void)snprintf(reference, sizeof(reference), "[IRC update id=%s:%llu ",
            event.stream, (unsigned long long)event.sequence);
        if (!irc->prompt_found && snag_irc_event_model_visible(&event) && event.stream[0] &&
            strstr(irc->prompt, reference) &&
            (!irc->wants_message || event.kind == SNAG_IRC_MESSAGE ||
             event.kind == SNAG_IRC_NOTICE)) {
            irc->preview.len = 0u;
            if (snag_buf_printf(&irc->preview, "%s: %s", event.endpoint,
                    event.text[0] ? event.text : snag_irc_kind_name(event.kind)) < 0) return -1;
            irc->prompt_found = true;
        }
    }
    return irc->topology_found && irc->prompt_found ? SNAG_JOURNAL_STOP_AFTER : 0;
}

static json_t *
list_cells(struct snag_store *store, struct snag_session *session, const char *state,
             unsigned int columns)
{
    struct list_irc irc = {.prompt = session->last_user,
        .preview = {.max = SNAG_MAX_DIRECT_PROMPT},
        .endpoints = {.max = SNAG_MAX_IRC_SNAPSHOT}, .renames = json_array()};
    struct snag_buf cell = {.max = SNAG_MAX_DIRECT_PROMPT};
    json_t *cells = NULL;
    uint64_t before = 0u;
    char id[SNAG_ID_HEX_LEN + 1u];
    char turns[32];

    const char *topology = snag_json_string(session->strings, "irc_snapshot");
    if (!irc.renames || list_irc_prompt(&irc) < 0) goto out;
    irc.topology_found = topology && !*topology;
    const json_t *recent = json_object_get(session->checkpoint_context, "recent");
    for (size_t i = 0u; i < json_array_size(recent); ++i) {
        const json_t *entry = json_array_get(recent, i);
        const char *type = snag_json_string(entry, "type");
        if (type && !strcmp(type, "irc_snapshot")) {
            irc.checkpoint_seq = session->checkpoint_seq;
            break;
        }
    }
    if (irc.topology_found) irc.checkpoint_seq = session->checkpoint_seq;
    if (!irc.topology_found || !irc.prompt_found) {
        (void)snag_session_each_event_reverse(session, 0u, SIZE_MAX, list_irc_event,
            &irc, &before, NULL, 0u);
    }
    if (irc.checkpoint_seq) {
        /* Apply fresh suffix events before the verified recent checkpoint data.
         * The snapshot boundary keeps old or unrelated renames out of the list. */
        irc.checkpoint_seq = 0u;
        for (size_t i = json_array_size(recent);
            i && (!irc.topology_found || !irc.prompt_found); ) {
            const json_t *entry = json_array_get(recent, --i);
            const char *type = snag_json_string(entry, "type");
            if (type && list_irc_event(&irc, NULL, 0u, type,
                    json_object_get(entry, "data"), NULL, 0u) < 0) goto out;
        }
        if (before && (!irc.topology_found || !irc.prompt_found)) {
            (void)snag_session_each_event_reverse(session, before, SIZE_MAX, list_irc_event,
                &irc, &before, NULL, 0u);
        }
    }
    if (!irc.topology_found && topology) {
        /* A damaged historical prefix still permits the last cached topology. */
        json_array_clear(irc.renames);
        if (list_irc_topology(&irc, topology) < 0) goto out;
    }
    if (snag_buf_terminate(&irc.preview) < 0 ||
        snag_buf_terminate(&irc.endpoints) < 0) goto out;
    /* Expand collisions, including stored/trash sessions outside this filter,
     * so the displayed selector resolves through every session entry point. */
    for (size_t length = 8u; length <= SNAG_ID_HEX_LEN; ++length) {
        struct resolved_session target;
        memcpy(id, session->id, length);
        id[length] = '\0';
        if (resolve_prefix(store, id, &target, true, NULL, NULL, NULL, 0u) == 0 ||
            errno != EEXIST) break;
    }
    (void)snprintf(turns, sizeof(turns), "%llu", (unsigned long long)session->turn_count);
    const char *values[] = {id, session->name ? session->name : "-", session->default_model,
        turns, state,
        irc.preview.len ? (const char *)irc.preview.data : session->last_user,
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

static size_t
list_text_width(const char *text)
{
    size_t width = 0u;
    size_t length = strlen(text);
    for (size_t offset = 0u; offset < length; ) {
        uint32_t cp;
        size_t bytes = snag_utf8_decode((const unsigned char *)text + offset,
            length - offset, &cp);
        if (!bytes) break;
        int cells = snag_char_width(cp);
        if (cells > 0) width += (size_t)cells;
        offset += bytes;
    }
    return width;
}

struct session_list_row {
    json_t *cells;
    uint64_t time_ms;
    unsigned int status;
    char id[SNAG_ID_HEX_LEN + 1u];
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
            size_t width = list_text_width(json_string_value(
                json_array_get(rows[row].cells, col)));
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
            const char *text = row ? json_string_value(
                json_array_get(rows[row - 1u].cells, col)) : headers[col];
            if ((col && snag_buf_append(&line, columns ? "  " : "\t", columns ? 2u : 1u) < 0) ||
                append_list_preview(&line, text, columns ? widths[col] : 80u,
                    columns && col != 6u) < 0) goto out;
        }
        if (snag_buf_putc(&line, '\n') < 0 ||
            emit(opaque, (const char *)line.data, line.len) < 0) goto out;
    }
    rc = 0;
out:
    snag_buf_free(&line);
    return rc;
}

static int
list_sessions(struct snag_store *store, const struct snag_session *owned,
    uint64_t stored_limit, unsigned int columns, snag_store_emit_fn emit,
    void *opaque, json_t *catalog, bool (*cancel)(void *), void *cancel_opaque,
    char *error, size_t error_size)
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
        if (matching_snapshot(store, snapshot, entry, cancel, cancel_opaque) < 0) {
            if (cancel && cancel(cancel_opaque)) goto canceled;
            continue;
        }
        /* Closing a second descriptor of our lock drops the owner's POSIX lock. */
        bool live = owned && owned->lock_fd >= 0 && !strcmp(owned->id, entry);
        if (!live) live = snag_session_is_live(snapshot);
        if (!stored_limit && !live) {
            snag_session_close(snapshot);
            continue;
        }
        int attachment = live ?
            snag_session_endpoint_status(snapshot->dir_fd, snapshot->dir_path) : -1;
        unsigned int status = !live ? 3u : attachment > 0 ? 0u : attachment == 0 ? 1u : 2u;
        static const char *const states[] = {"attached", "detached", "running", "stored"};
        uint64_t time_ms = snapshot->last_time_ms;
        json_t *cells = list_cells(store, snapshot, states[status], columns);
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
        rows[count] = (struct session_list_row){.cells = cells, .time_ms = time_ms,
            .status = status};
        memcpy(rows[count].id, entry, sizeof(rows[count].id));
        ++count;
    }
    if (cancel && cancel(cancel_opaque)) goto canceled;
    rc = finish_directory(dir, error, error_size);
    dir = NULL;
    if (rc < 0) goto out;
    if (!count) {
        if (!catalog) (void)snag_errorf(error, error_size, "no matching sessions");
    } else {
        qsort(rows, count, sizeof(*rows), compare_list_rows);
        size_t visible = 0u;
        uint64_t stored = 0u;
        while (visible < count) {
            if (rows[visible].status == 3u && stored++ >= stored_limit) break;
            ++visible;
        }
        if (catalog) {
            for (size_t i = 0u; i < visible; ++i) {
                json_t *row = json_pack("{s:s,s:I,s:i,s:O}", "id", rows[i].id,
                    "activity_ms", (json_int_t)rows[i].time_ms,
                    "status_rank", (int)rows[i].status, "cells", rows[i].cells);
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
    for (size_t row = 0u; row < count; ++row) json_decref(rows[row].cells);
    free(rows);
    free(snapshot);
    if (rc < 0 && error_size && !error[0])
        (void)snag_errorf(error, error_size, "cannot write session list");
    return rc;
}

int
snag_store_list(struct snag_store *store, const struct snag_session *owned,
    uint64_t stored_limit, unsigned int columns, snag_store_emit_fn emit,
    void *opaque, char *error, size_t error_size)
{
    return list_sessions(store, owned, stored_limit, columns, emit, opaque, NULL, NULL, NULL,
        error, error_size);
}

json_t *
snag_store_catalog(struct snag_store *store, const struct snag_session *owned,
    uint64_t stored_limit, bool (*cancel)(void *), void *cancel_opaque, char *error, size_t error_size)
{
    json_t *rows = json_array();
    if (rows && list_sessions(store, owned, stored_limit, 0u, NULL, NULL, rows,
        cancel, cancel_opaque, error, error_size) < 0) {
        json_decref(rows);
        rows = NULL;
    }
    return rows;
}
