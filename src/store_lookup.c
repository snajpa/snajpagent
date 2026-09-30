/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_internal.h"
#include "media.h"
#include "base.h"
#include "fs.h"

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
    if (len < 8u || len > SNAG_ID_HEX_LEN || !snag_hex_is_lower(prefix, len))
        return snag_fail(error, error_size, EINVAL, "session id must be 8..32 lowercase hex characters");
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

static int
open_session_dir(struct snag_store *store, struct snag_session *session,
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
    return open_session_dir(store, session, target.id, error, error_size);
}

static int
open_full_id(struct snag_store *store, struct snag_session *session,
             const char *id, char *error, size_t error_size)
{
    if (open_session_dir(store, session, id, error, error_size) < 0) return -1;
    if (snag_store_open_session_files(session, false, error, error_size) < 0 ||
        snag_store_scan_log(session, SNAG_TAIL_TRUNCATE,
                           error, error_size) < 0)
        return -1;
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
    if (open_session_dir(store, session, id, error, error_size) < 0) return -1;
    session->log_fd = snag_open_read_security_at(session->dir_fd, "events.jsonl", false);
    if (session->log_fd < 0) return -1;
    if (snag_store_verify_private_fd(session->log_fd, false, "event log", error, error_size) < 0) return -1;
    session->log_end = snag_seek(session->log_fd, 0, SEEK_END);
    if (session->log_end < 0) return -1;
    return snag_store_scan_log(session, SNAG_TAIL_IGNORE, error, error_size);
}

/* On success the caller owns the snapshot; no selected-field copies. */
static int
matching_snapshot(struct snag_store *store, struct snag_session *snapshot,
                   const char *id)
{
    char error[128];

    snag_session_init(snapshot);
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
        if (matching_snapshot(store, &snapshot, entry) < 0) continue;
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
        if (matching_snapshot(store, &snapshot, entry) < 0) continue;
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
append_list_preview(struct snag_buf *row, const char *text)
{
    size_t length = text ? strlen(text) : 0u;
    size_t offset = 0u;
    size_t shown = 0u;
    bool space = false;

    /* A table cell previews saved text; full prompts can contain an IRC transcript. */
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
        if (shown + (space ? 1u : 0u) >= 80u) break;
        if (space && snag_buf_putc(row, ' ') < 0) return -1;
        if (snag_buf_append(row, text + offset, bytes) < 0) return -1;
        shown += space ? 2u : 1u;
        space = false;
        offset += bytes;
    }
    return offset < length ? snag_buf_append(row, "…", 3u) : 0;
}

int
snag_store_list(struct snag_store *store, const struct snag_session *owned,
                enum snag_session_list filter, snag_store_emit_fn emit, void *opaque, char *error, size_t error_size)
{
    struct snag_directory *dir;
    const char *entry;
    unsigned int shown = 0;

    dir = open_sessions_dir(store, error, error_size);
    if (!dir) return -1;
    while ((entry = snag_directory_next(dir)) != NULL) {
        struct snag_session snapshot;
        if (matching_snapshot(store, &snapshot, entry) < 0) continue;
        struct snag_buf row = {.max = SNAG_PATH_MAX_BYTES + 8192u};
        /* Closing any descriptor of our POSIX lock file drops all locks held
         * by this process on that file, even if another descriptor owns them. */
        bool live = owned && owned->lock_fd >= 0 && !strcmp(owned->id, entry);
        if (!live) live = snag_session_is_live(&snapshot);
        if (filter == SNAG_SESSIONS_RUNNING && !live) {
            snag_session_close(&snapshot);
            continue;
        }
        if ((!shown && snag_buf_printf(&row,
                "SESSION\tNAME\tMODEL\tTURNS\tPROCESS\tFIRST PROMPT\n") < 0) ||
            snag_buf_printf(&row, "%.8s\t", entry) < 0 ||
            append_list_preview(&row, snapshot.name ? snapshot.name : "-") < 0 ||
            snag_buf_putc(&row, '\t') < 0 ||
            append_list_preview(&row, snapshot.default_model) < 0 ||
            snag_buf_printf(&row, "\t%llu\t%s\t",
                            (unsigned long long)snapshot.turn_count,
                            live ? "live" : "stored") < 0 ||
            append_list_preview(&row, snapshot.first_user) < 0 ||
            snag_buf_putc(&row, '\n') < 0 ||
            emit(opaque, (const char *)row.data, row.len) < 0) {
            snag_buf_free(&row);
            snag_session_close(&snapshot);
            (void)snag_directory_close(dir);
            return snag_errorf(error, error_size, "cannot write session list");
        }
        snag_buf_free(&row);
        snag_session_close(&snapshot);
        ++shown;
    }
    if (finish_directory(dir, error, error_size) < 0) return -1;
    if (!shown) snag_errorf(error, error_size, "no matching sessions");
    return 0;
}
