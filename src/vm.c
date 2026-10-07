/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm.h"
#include "app.h"
#include "clipboard_transfer.h"
#include "config.h"
#include "irc.h"
#include "irc_address.h"
#include "json.h"
#include "render.h"
#include "secret.h"
#include "session_client.h"
#include "snajpagent.h"
#include "term_host.h"
#include "tmux.h"
#include "unicode.h"
#include "vm_connection.h"
#include "vm_editor.h"
#include "vm_grid.h"
#include "vm_input.h"
#include "vm_layout.h"
#include "vm_reader.h"
#include "vm_report.h"
#include "vm_source.h"
#include "vm_text.h"
#include "vm_workspace.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef _WIN32
#include <poll.h>
#endif

#if SNAJPAGENT_VM
enum view_kind { VIEW_SESSIONS, VIEW_WORKSPACES, VIEW_HELP, VIEW_TRANSCRIPT,
    VIEW_REPORT, VIEW_REPORTS, VIEW_BUFFERS };
enum history_load { LOAD_NONE, LOAD_FIRST, LOAD_LAST, LOAD_PREVIOUS, LOAD_NEXT,
    LOAD_KEEP, LOAD_ANCHOR, LOAD_REFRESH, LOAD_POLL, LOAD_KEEP_NEXT, LOAD_KEEP_PREVIOUS };
static const char *const view_names[] = {"sessions", "workspaces", "help", "transcript",
    "report", "reports", "buffers"};
static const char *const help_rows[] = {
    "j/k or arrows: select    gg/G: first/last    Ctrl-D/U: half page",
    "/: filter picker or search text    ?: backward search    n/N: repeat/reverse",
    ":set ignorecase/noignorecase    Ctrl-C: cancel search    R: refresh",
    ":sessions    :workspaces    :help    o: open selected history",
    ":history SESSION_ID    :verbosity 0..6    R: refresh current history",
    "History: h/l w/b/e 0/^/$, j/k logical lines, gj/gk wrapped rows; counted gg/G: line",
    "v/V/Ctrl-V: character/line/block selection    y: yank    p/P: paste into composer",
    "yy or y + motion: copy    Esc: cancel selection    Ctrl-C: cancel copy",
    "Mouse: click cursor, drag text, wheel hovered window; drag separators to resize",
    ":set mouse/nomouse: enable/disable terminal mouse reporting",
    ":split [BUFFER] or :sp    :vsplit [BUFFER] or :vsp    :close    :q    :qa",
    ":buffers: conversations    :buffer ADDRESS/ID    :bn/:bp: cycle",
    "/query NICK in a composer opens its chat here; agent chats are read-only",
    "Ctrl-W s/v: split    Ctrl-W w/h/j/k/l: focus    Ctrl-W =: equalize",
    "Ctrl-W +/-: height    Ctrl-W >/<: width    Ctrl-W q: close",
    ":workspace    :workspace name NAME    :workspace save",
    "Workspace names accept quoted text. :q in these pickers closes the window.",
    "Enter/:session [ID]: resume owner    :new [NAME]: create owner",
    ":session detach (or :session d): save and detach the workspace; owners continue",
    "Command line: Tab/Shift-Tab complete commands and options; Enter executes",
    ":attach [ID]: control a running owner    o/:history SESSION: read-only",
    ":classic [SESSION]: use its full terminal; /s d returns to this workspace",
    "i/a/A: edit prompt    Esc: NORMAL    Enter: submit    Ctrl-J: newline",
    "INSERT Ctrl-N/Ctrl-P: complete @nicknames from the current conversation",
    "Composer: h/j/k/l w/b/e 0/^/$ gg/G; counts multiply (2d3w deletes six words).",
    "gj/gk: wrapped rows    H/M/L: visible rows    zz: center    Ctrl-U/D/B/F: pages",
    "i/a/I/A/o/O: INSERT    d/c/y + motion, dd/cc/yy, x, p/P    u/Ctrl-R: undo/redo",
    ":detach: preserve owner    :q/:qa: quit controlled owners    :recover: recover submission",
    "Draft conflict: :draft local keeps this copy; :draft owner uses the owner's copy.",
    ":reports: command output list    :report [ID]: reopen output    :history: return to session",
    "Reports: gg/G, j/k, pages and splits. :q closes a report window and preserves the owner.",
    "Workspace selection restores its layout. Bracketed paste never runs commands."
};

struct vm_window {
    uint64_t id, launch;
    enum view_kind kind;
    size_t selected, top, composer_top, history_rows, composer_row, composer_rows;
    bool center_composer;
    char prompt[SNAG_TERM_LABEL_BYTES];
    struct snag_prompt_clock prompt_clock;
    struct snag_vm_selection visual;
    struct snag_journal_cursor visual_tail;
    struct snag_vm_editor navigation;
    size_t visual_column, visual_width;
    bool yank_motion, yank_after_load;
    char selected_id[SNAG_ID_HEX_LEN + 1u];
    char *filter;
    struct snag_vm_rectangle rectangle;
    struct snag_vm_document *document;
    json_t *report, *route;
    bool report_catalog, report_open_pending;
    char report_selector[SNAG_ID_HEX_LEN + 1u];
    char session_id[SNAG_ID_HEX_LEN + 1u], anchor_key[160];
    uint64_t anchor_seq;
    size_t anchor_byte;
    bool anchor_heading, anchor_source, incomplete, best_effort, follow, source_failed;
    uint64_t follow_at;
    unsigned int verbosity;
    enum history_load load;
    struct snag_journal_cursor begin, end, tail;
};

struct vm_launch {
    struct vm_launch *next;
    struct snag_session_packet packet;
    int fd;
    uint64_t child, token;
    char session[SNAG_ID_HEX_LEN + 1u];
    struct snag_app_direct *direct;
    struct snag_view_channel channel;
    bool ready;
};

struct vm_pending_input {
    struct vm_pending_input *next;
    struct snag_vm_input_event event;
    size_t charge;
    unsigned char text[];
};

struct vm_motion_origin {
    struct snag_vm_document *document;
    struct snag_vm_anchor anchor;
    uint64_t window;
    size_t selected, top;
    struct snag_journal_cursor begin, end, tail;
    bool follow, incomplete, best_effort;
};

struct vm_clipboard {
    struct snag_clipboard *source;
    struct snag_clipboard_send *send;
    struct snag_clipboard_osc osc;
    struct snag_terminal_profile profile;
    char client[SNAG_TERMINAL_NAME_BYTES];
    int output, pending_fd;
    struct snag_buf pending_text;
    uint64_t pending_length, shown_bytes;
    unsigned char packet[SNAG_SCREEN_TITLE_MAX + 4u * ((SNAG_SCREEN_TITLE_MAX + 127u) / 128u)];
    size_t packet_at, packet_length;
    bool pending, local, canceling, settling;
};

struct vm_read {
    struct snag_vm_reader *reader;
    uint64_t generation, window;
    enum history_load load;
};

struct vm {
    struct snag_store store;
    struct snag_config config;
    struct snag_secret_set secrets;
    struct snag_vm_workspace *workspace, *switch_workspace;
    struct vm_read page, scan;
    struct snag_vm_connection *connections;
    struct vm_launch *launches;
    char *program;
    const char *start_session;
    uint64_t next_launch;
    struct snag_vm_layout *layout;
    struct vm_window *windows;
    size_t count, focus;
    uint64_t next_window, stored_limit, save_at, prompt_due;
    json_t *sessions, *workspaces, *buffers;
    struct snag_term_host terminal;
    struct snag_vm_grid grid;
    struct snag_vm_input input;
    struct snag_buf command, paste;
    struct snag_vm_register reg;
    struct vm_clipboard clipboard;
    struct snag_session_typeahead classic;
    size_t command_cursor;
    char *completion_prefix;
    size_t completion_index;
    char mode, prefix;
    char *search_query;
    const char *search_finish;
    bool searching, copying, navigating, motion_loading, input_drained;
    bool search_reverse, search_command, ignorecase;
    struct vm_motion_origin motion_origin;
    struct vm_pending_input *pending_input, *pending_input_tail;
    size_t pending_input_bytes;
    char message[512];
    int output;
    bool dirty, save_dirty, meaningful, quit, suspend, entering, paste_failed;
    bool composer, insert, quit_all, detach_exit, detach_suspend;
    bool classic_pending, classic_ready, classic_uncertain;
    bool mouse, mouse_reported, mouse_down, mouse_select, unfocused;
    uint64_t mouse_window;
    struct snag_vm_separator mouse_separator;
    uint64_t quit_window;
    size_t cursor_row, cursor_column;
};

static volatile sig_atomic_t stopped, resized;

static json_t *selected_row(struct vm *, const struct vm_window *);
static int buffer_catalog(struct vm *);
static bool matches(const struct vm_window *, const json_t *);
static int clipboard_step(struct vm *);
static void clipboard_settle(struct vm *);

static void
stop_signal(int number)
{
    stopped = number;
}

static void
resize_signal(int number)
{
    (void)number;
    resized = 1;
}

static void
notice(struct vm *vm, const char *text)
{
    (void)snprintf(vm->message, sizeof(vm->message), "%s", text);
    vm->dirty = true;
}

static void
changed(struct vm *vm)
{
    vm->dirty = vm->save_dirty = vm->meaningful = true;
    if (!vm->save_at) vm->save_at = snag_monotonic_ms() + 1000u;
}

static struct snag_vm_connection *
connection_for(struct vm *vm, const char *session, bool create)
{
    if (!*session) return NULL;
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next)
        if (!strcmp(c->session, session)) return c;
    if (!create) return NULL;
    struct snag_vm_connection *c = snag_vm_connection_new(session);
    if (c) {
        c->next = vm->connections;
        vm->connections = c;
    }
    return c;
}

static struct snag_vm_connection *
focused_connection(struct vm *vm)
{
    return connection_for(vm, vm->windows[vm->focus].session_id, false);
}

static struct snag_vm_buffer *
window_buffer(struct vm *vm, const struct vm_window *window)
{
    return snag_vm_buffer_get(connection_for(vm, window->session_id, false), window->route, false);
}

static struct snag_vm_buffer *
focused_buffer(struct vm *vm)
{
    return window_buffer(vm, &vm->windows[vm->focus]);
}

static struct snag_vm_buffer *
submission_source(struct vm *vm, struct snag_vm_buffer *buffer)
{
    if (!buffer->origin) return buffer;
    struct snag_vm_connection *c = connection_for(vm,
        snag_json_string(buffer->origin, "session"), false);
    return snag_vm_buffer_get(c, json_object_get(buffer->origin, "route"), false);
}

static struct snag_vm_buffer *
submission_from(struct vm *vm, struct snag_vm_buffer *source)
{
    if (!source) return NULL;
    if (source->pending) return source;
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next)
        for (struct snag_vm_buffer *b = c->buffers; b; b = b->next)
            if (b->pending && b->origin && submission_source(vm, b) == source) return b;
    return NULL;
}

static bool
connection_visible(const struct vm *vm, const struct snag_vm_connection *c)
{
    for (size_t i = 0u; i < vm->count; ++i)
        if (vm->windows[i].kind == VIEW_BUFFERS ||
            !strcmp(vm->windows[i].session_id, c->session)) return true;
    return false;
}

static void
detach_unused(struct vm *vm)
{
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next) {
        if (c->direct || connection_visible(vm, c)) continue;
        c->control = false;
        if (!c->detaching) snag_vm_connection_detach(c);
    }
}

static struct vm_launch *
direct_session(struct vm *vm)
{
    for (struct vm_launch *job = vm->launches; job; job = job->next)
        if (job->direct) return job;
    return NULL;
}

static void
windows_free(struct vm_window *windows, size_t count)
{
    for (size_t i = 0u; i < count; ++i) {
        free(windows[i].filter);
        json_decref(windows[i].report);
        json_decref(windows[i].route);
        snag_vm_document_free(windows[i].document);
    }
    free(windows);
}

static bool
document_view(const struct vm_window *window)
{
    return window->kind == VIEW_TRANSCRIPT || window->kind == VIEW_REPORT;
}

static void
remember_anchor(struct vm_window *window)
{
    struct snag_vm_document_row row;
    if (snag_vm_document_row(window->document, window->selected, &row) < 0) return;
    const json_t *block = snag_vm_document_block(window->document, row.block);
    (void)snprintf(window->anchor_key, sizeof(window->anchor_key), "%s",
        snag_json_string(block, "key"));
    window->anchor_seq = (uint64_t)json_integer_value(json_object_get(block, "seq"));
    window->anchor_byte = snag_vm_document_source(window->document, &row, row.begin);
    window->anchor_heading = row.heading;
    window->anchor_source = true;
}

static size_t
locate_anchor(struct vm_window *window)
{
    if (!window->anchor_source && !window->anchor_heading) {
        for (size_t i = 0u;; ++i) {
            const json_t *block = snag_vm_document_block(window->document, i);
            if (!block) break;
            if (strcmp(snag_json_string(block, "key"), window->anchor_key)) continue;
            window->anchor_byte = snag_vm_source_unformatted(block, window->anchor_byte);
            window->anchor_source = true;
            break;
        }
    }
    return (window->anchor_source ? snag_vm_document_locate_source :
        snag_vm_document_locate)(window->document, window->anchor_key,
            window->anchor_seq, window->anchor_byte, window->anchor_heading);
}

static bool
keeps_anchor(enum history_load load)
{
    return load == LOAD_KEEP || load == LOAD_ANCHOR || load == LOAD_REFRESH ||
        load == LOAD_KEEP_NEXT || load == LOAD_KEEP_PREVIOUS;
}

static void
clear_pending_input(struct vm *vm)
{
    while (vm->pending_input) {
        struct vm_pending_input *next = vm->pending_input->next;
        free(vm->pending_input);
        vm->pending_input = next;
    }
    vm->pending_input_tail = NULL;
    vm->pending_input_bytes = 0u;
}

static void
restore_motion(struct vm *vm, bool restore)
{
    struct vm_motion_origin *origin = &vm->motion_origin;
    for (size_t i = 0u; restore && origin->document && i < vm->count; ++i) {
        struct vm_window *window = &vm->windows[i];
        if (window->id != origin->window) continue;
        snag_vm_document_free(window->document);
        window->document = origin->document;
        origin->document = NULL;
        memcpy(window->anchor_key, origin->anchor.key, sizeof(window->anchor_key));
        window->anchor_seq = origin->anchor.seq;
        window->anchor_byte = origin->anchor.byte;
        window->anchor_heading = origin->anchor.heading;
        window->anchor_source = true;
        window->selected = origin->selected;
        window->top = origin->top;
        window->begin = origin->begin;
        window->end = origin->end;
        window->tail = origin->tail;
        window->follow = origin->follow;
        window->incomplete = origin->incomplete;
        window->best_effort = origin->best_effort;
        window->load = LOAD_NONE;
        window->yank_motion = window->yank_after_load = false;
    }
    snag_vm_document_free(origin->document);
    memset(origin, 0, sizeof(*origin));
    vm->motion_loading = false;
    vm->search_finish = NULL;
}

static void
cancel_read(struct vm_read *read)
{
    snag_vm_reader_cancel(read->reader);
    read->generation = read->window = 0u;
    read->load = LOAD_NONE;
}

static void
cancel_search(struct vm *vm)
{
    if (!vm->searching && !vm->copying && !vm->navigating && !vm->motion_loading) return;
    cancel_read(&vm->scan);
    if (vm->motion_loading && vm->page.window == vm->motion_origin.window)
        cancel_read(&vm->page);
    notice(vm, vm->copying ? "Copy canceled; register preserved" :
        vm->navigating || vm->motion_loading ? "Navigation canceled" : "Search canceled");
    restore_motion(vm, true);
    clear_pending_input(vm);
    vm->searching = vm->copying = vm->navigating = false;
}

static void
queue_history(struct vm *vm, struct vm_window *window, enum history_load load)
{
    if (load == LOAD_KEEP && (window->load || window->source_failed)) return;
    if (load != LOAD_POLL && load != LOAD_KEEP &&
        !(vm->motion_loading && vm->motion_origin.window == window->id))
        cancel_search(vm);
    if (load == LOAD_FIRST || load == LOAD_LAST || load == LOAD_REFRESH) {
        if (window->source_failed) notice(vm, "Loading retained history");
        window->source_failed = false;
    }
    if ((load == LOAD_KEEP || load == LOAD_REFRESH) && !window->anchor_key[0]) {
        remember_anchor(window);
    }
    window->load = load;
    vm->dirty = true;
}

static json_t *
state_snapshot(const struct vm *vm)
{
    json_t *windows = json_array();
    if (!windows) return NULL;
    for (size_t i = 0u; i < vm->count; ++i) {
        const struct vm_window *window = &vm->windows[i];
        json_t *row = json_pack("{s:I,s:s,s:s,s:I,s:I,s:s}", "id", (json_int_t)window->id,
            "kind", view_names[window->kind], "selected", window->selected_id,
            "top", (json_int_t)window->top, "row", (json_int_t)window->selected,
            "filter", window->filter ? window->filter : "");
        if (row && window->kind == VIEW_TRANSCRIPT) {
            json_t *history = json_pack("{s:s,s:s,s:I,s:I,s:b,s:i,s:b,s:b,s:O}",
                "session", window->session_id, "key", window->anchor_key,
                "seq", (json_int_t)window->anchor_seq, "byte", (json_int_t)window->anchor_byte,
                "heading", window->anchor_heading, "verbosity", (int)window->verbosity,
                "follow", window->follow, "source", window->anchor_source,
                "route", window->route ? window->route : json_null());
            if (!history || json_object_set_new(row, "history", history) < 0) {
                json_decref(row);
                row = NULL;
            }
        }
        if (row && (window->kind == VIEW_REPORT || window->kind == VIEW_REPORTS)) {
            if (json_object_set_new(row, "session", json_string(window->session_id)) < 0 ||
                (window->kind == VIEW_REPORT &&
                 (json_object_set(row, "report", window->report) < 0 ||
                  json_object_set_new(row, "byte", json_integer(
                    (json_int_t)window->anchor_byte)) < 0 ||
                  json_object_set_new(row, "source", json_boolean(window->anchor_source)) < 0))) {
                json_decref(row);
                row = NULL;
            }
        }
        if (!row || json_array_append_new(windows, row) < 0) {
            json_decref(windows);
            return NULL;
        }
    }
    json_t *layout = snag_vm_layout_json(vm->layout);
    if (!layout) {
        json_decref(windows);
        return NULL;
    }
    json_t *buffers = snag_vm_connections_json(vm->connections);
    if (!buffers) {
        json_decref(layout);
        json_decref(windows);
        return NULL;
    }
    json_t *result = json_pack("{s:i,s:I,s:o,s:o,s:o}", "v", 12,
        "focus", (json_int_t)vm->windows[vm->focus].id, "layout", layout,
        "windows", windows, "buffers", buffers);
    json_t *classic = json_null();
    if (result && vm->classic.bytes.len) {
        struct snag_buf encoded = {.max = SNAG_MAX_DIRECT_PROMPT * 2u};
        classic = snag_base64_append(&encoded, vm->classic.bytes.data, vm->classic.bytes.len) < 0 ?
            NULL : json_pack("{s:s,s:o}", "session", vm->classic.session,
                "input", json_stringn((const char *)encoded.data, encoded.len));
        snag_buf_free(&encoded);
    }
    if (!classic || json_object_set_new(result, "classic", classic) < 0) {
        json_decref(result);
        return NULL;
    }
    return result;
}

static int
state_restore(struct vm *vm, const json_t *state, char *error, size_t size)
{
    struct snag_vm_layout *layout = NULL;
    struct vm_window *windows = NULL;
    struct snag_vm_connection *connections = NULL;
    struct snag_session_typeahead classic = {.bytes.max = SNAG_MAX_DIRECT_PROMPT};
    uint64_t focus_id = 0u, next = 0u;
    json_int_t version = json_integer_value(json_object_get(state, "v"));
    size_t count = json_array_size(json_object_get(state, "windows")), focus = SIZE_MAX;
    if (!snag_json_exact_keys(state, version >= 5 ? "v focus layout windows buffers classic" :
        version >= 3 ? "v focus layout windows buffers" :
        "v focus layout windows") || (version < 1 || version > 12) || !count ||
        (version >= 3 &&
         snag_vm_connections_load(json_object_get(state, "buffers"), &connections) < 0) ||
        snag_json_integer_u64(state, "focus", &focus_id) < 0 ||
        !(layout = snag_vm_layout_load(json_object_get(state, "layout"), error, size)) ||
        snag_vm_layout_count(layout) != count || count > SIZE_MAX / sizeof(*windows) ||
        !(windows = calloc(count, sizeof(*windows)))) goto invalid;
    const json_t *saved_classic = json_object_get(state, "classic");
    if (version >= 5 && !json_is_null(saved_classic)) {
        const char *session = snag_json_bounded_string(json_object_get(saved_classic, "session"),
            SNAG_ID_HEX_LEN);
        const char *input = snag_json_bounded_string(json_object_get(saved_classic, "input"),
            SNAG_MAX_DIRECT_PROMPT * 2u);
        if (!snag_json_exact_keys(saved_classic, "session input") ||
            !session || !snag_hex_is_lower(session, SNAG_ID_HEX_LEN) || !input ||
            snag_base64_decode(&classic.bytes, input) < 0 || !classic.bytes.len) goto invalid;
        memcpy(classic.session, session, sizeof(classic.session));
    }
    for (size_t i = 0u; i < count; ++i) {
        json_t *row = json_array_get(json_object_get(state, "windows"), i);
        const char *kind = snag_json_string(row, "kind");
        const char *selected = snag_json_string(row, "selected");
        const char *filter = snag_json_string(row, "filter");
        uint64_t top, selected_row;
        struct vm_window *window = &windows[i];
        bool history = kind && !strcmp(kind, "transcript") && version >= 2;
        bool report = kind && !strcmp(kind, "report") && version >= 6;
        bool reports = kind && !strcmp(kind, "reports") && version >= 6;
        if (!snag_json_exact_keys(row, report && version >= 7 ?
            "id kind selected top row filter session report byte source" : report ?
            "id kind selected top row filter session report byte" :
            reports ? "id kind selected top row filter session" :
            history ? "id kind selected top row filter history" :
            "id kind selected top row filter") ||
            snag_json_integer_u64(row, "id", &window->id) < 0 ||
            !snag_vm_layout_contains(layout, window->id) || !kind || !selected || !filter ||
            strlen(filter) != json_string_length(json_object_get(row, "filter")) ||
            strlen(selected) != json_string_length(json_object_get(row, "selected")) ||
            (*selected && (strlen(selected) != SNAG_ID_HEX_LEN ||
                          !snag_hex_is_lower(selected, SNAG_ID_HEX_LEN))) ||
            snag_json_integer_u64(row, "top", &top) < 0 || top > SIZE_MAX ||
            snag_json_integer_u64(row, "row", &selected_row) < 0 || selected_row > SIZE_MAX)
            goto invalid;
        for (size_t j = 0u; j < i; ++j) if (windows[j].id == window->id) goto invalid;
        size_t k = 0u;
        while (k < sizeof(view_names) / sizeof(view_names[0]) && strcmp(kind, view_names[k])) ++k;
        if (k == sizeof(view_names) / sizeof(view_names[0]) ||
            (k == VIEW_TRANSCRIPT && !history) || (k == VIEW_REPORT && !report) ||
            (k == VIEW_REPORTS && !reports)) goto invalid;
        window->kind = (enum view_kind)k;
        window->top = (size_t)top;
        window->selected = (size_t)selected_row;
        memcpy(window->selected_id, selected, strlen(selected) + 1u);
        window->filter = snag_strdup_checked(filter, SNAG_MAX_DIRECT_PROMPT);
        if (!window->filter) goto invalid;
        if (history) {
            const json_t *saved = json_object_get(row, "history");
            const char *sid = snag_json_string(saved, "session");
            const char *key = snag_json_string(saved, "key");
            uint64_t byte, level;
            bool following = json_object_get(saved, "follow") != NULL;
            if (!snag_json_exact_keys(saved, version >= 8 ?
                "session key seq byte heading verbosity follow source route" : version >= 7 ?
                "session key seq byte heading verbosity follow source" : following ?
                "session key seq byte heading verbosity follow" :
                "session key seq byte heading verbosity") ||
                (following && !json_is_boolean(json_object_get(saved, "follow"))) ||
                (version >= 7 && !json_is_boolean(json_object_get(saved, "source"))) ||
                !sid || strlen(sid) != SNAG_ID_HEX_LEN ||
                json_string_length(json_object_get(saved, "session")) != SNAG_ID_HEX_LEN ||
                !snag_hex_is_lower(sid, SNAG_ID_HEX_LEN) ||
                !key || strlen(key) >= sizeof(window->anchor_key) ||
                strlen(key) != json_string_length(json_object_get(saved, "key")) ||
                snag_json_integer_u64(saved, "seq", &window->anchor_seq) < 0 ||
                window->anchor_seq >= INT64_MAX ||
                snag_json_integer_u64(saved, "byte", &byte) < 0 || byte > SIZE_MAX ||
                snag_json_integer_u64(saved, "verbosity", &level) < 0 ||
                level > SNAG_VERBOSITY_MAX ||
                !json_is_boolean(json_object_get(saved, "heading"))) goto invalid;
            memcpy(window->session_id, sid, sizeof(window->session_id));
            memcpy(window->anchor_key, key, strlen(key) + 1u);
            window->anchor_byte = (size_t)byte;
            window->anchor_heading = json_is_true(json_object_get(saved, "heading"));
            window->anchor_source = json_is_true(json_object_get(saved, "source"));
            window->verbosity = (unsigned int)level;
            window->follow = json_is_true(json_object_get(saved, "follow"));
            json_t *route = json_object_get(saved, "route");
            if (version >= 8 && !json_is_null(route)) {
                struct snag_irc_conversation_target target;
                if (snag_view_conversation_read(route, &target) < 0) goto invalid;
                window->route = json_incref(route);
            }
            window->load = !window->follow && window->anchor_seq ? LOAD_ANCHOR : LOAD_LAST;
        }
        if (report || reports) {
            const char *sid = snag_json_bounded_string(json_object_get(row, "session"),
                SNAG_ID_HEX_LEN);
            if (!sid || strlen(sid) != SNAG_ID_HEX_LEN ||
                !snag_hex_is_lower(sid, SNAG_ID_HEX_LEN)) goto invalid;
            memcpy(window->session_id, sid, sizeof(window->session_id));
            window->report_catalog = reports;
            if (report) {
                uint64_t byte;
                json_t *value = json_object_get(row, "report");
                if (!snag_vm_report_valid(value) ||
                    snag_json_integer_u64(row, "byte", &byte) < 0 || byte > SIZE_MAX ||
                    (version >= 7 && !json_is_boolean(json_object_get(row, "source")))) {
                    goto invalid;
                }
                window->report = json_incref(value);
                window->anchor_byte = (size_t)byte;
                window->anchor_source = json_is_true(json_object_get(row, "source"));
                memcpy(window->anchor_key, snag_json_string(value, "id"), SNAG_ID_HEX_LEN + 1u);
                window->load = LOAD_KEEP;
            }
        }
        if (window->id == focus_id) focus = i;
        if (window->id > next) next = window->id;
    }
    if (focus == SIZE_MAX || next == INT64_MAX) goto invalid;
    for (size_t i = 0u; i < count; ++i) {
        if (!windows[i].route) continue;
        struct snag_vm_connection *c;
        for (c = connections; c; c = c->next)
            if (!strcmp(c->session, windows[i].session_id)) break;
        if (!c || !snag_vm_buffer_get(c, windows[i].route, true)) goto invalid;
    }
    snag_vm_connections_free(vm->connections);
    vm->connections = connections;
    snag_buf_free(&vm->classic.bytes);
    vm->classic = classic;
    vm->classic_uncertain = classic.bytes.len != 0u;
    vm->classic_pending = vm->classic_ready = false;
    windows_free(vm->windows, vm->count);
    snag_vm_layout_free(vm->layout);
    vm->windows = windows;
    vm->layout = layout;
    vm->count = count;
    vm->focus = focus;
    vm->next_window = next + 1u;
    (void)buffer_catalog(vm);
    vm->composer = vm->insert = false;
    vm->quit_all = false;
    vm->quit_window = 0u;
    vm->dirty = true;
    return 0;
invalid:
    snag_buf_free(&classic.bytes);
    snag_vm_connections_free(connections);
    windows_free(windows, windows ? count : 0u);
    snag_vm_layout_free(layout);
    return snag_fail(error, size, EINVAL, "unsupported or invalid workspace views");
}

static int
save(struct vm *vm, const char *name)
{
    if (!vm->meaningful && !name) return 0;
    json_t *state = state_snapshot(vm);
    char error[256] = "cannot save workspace";
    int rc = -1;
    if (state) {
        if (vm->workspace->dir_fd < 0) {
            rc = snag_vm_workspace_create(vm->workspace, &vm->store, name ? name : "",
                state, snag_time_ms(), error, sizeof(error));
        } else {
            if (!name) name = snag_json_string(vm->workspace->snapshot, "name");
            rc = snag_vm_workspace_save(vm->workspace, name, state,
                snag_time_ms(), error, sizeof(error));
        }
    }
    json_decref(state);
    if (rc < 0) notice(vm, error);
    else vm->save_dirty = false;
    /* Failed storage stays visible; retry on another edit or explicit save. */
    vm->save_at = 0u;
    return rc;
}

static uint64_t
reader_request(struct vm *vm, struct vm_read *read, struct snag_vm_read_request *request)
{
    if (read == &vm->scan) {
        if (vm->page.window == vm->windows[vm->focus].id) cancel_read(&vm->page);
        cancel_read(read);
        vm->searching = vm->copying = vm->navigating = false;
    } else if (read->generation && read->window) {
        /* Replacing the shared page worker must not abandon another pane's
         * initial load. A pane without a document cannot poll itself yet. */
        for (size_t i = 0u; i < vm->count; ++i) {
            struct vm_window *window = &vm->windows[i];
            if (window->id != read->window) continue;
            if (read->load == LOAD_NONE) window->report_catalog = true;
            else if (!window->load) window->load = read->load;
            break;
        }
    }
    json_t *sources = json_array();
    if (!sources) return 0u;
    for (size_t i = 0u; i < vm->count; ++i) {
        if (vm->windows[i].session_id[0] &&
            json_array_append_new(sources, json_string(vm->windows[i].session_id)) < 0) {
            json_decref(sources);
            return 0u;
        }
    }
    request->plain = !vm->config.markdown;
    request->no_color = !vm->grid.color;
    request->retained_sessions = sources;
    uint64_t generation = snag_vm_reader_request(read->reader, request);
    json_decref(sources);
    request->retained_sessions = NULL;
    return generation;
}

static void
search_notice(struct vm *vm, const char *status)
{
    const char *query = vm->search_query ? vm->search_query : "";
    size_t shown = strcspn(query, "\r\n");
    if (shown > 160u) shown = 160u;
    while (shown && ((unsigned char)query[shown] & 0xc0u) == 0x80u) --shown;
    (void)snprintf(vm->message, sizeof(vm->message), "%s%.*s%s  %s",
        vm->search_command ? "command › /search " : "Search: ",
        (int)shown, query, query[shown] ? "…" : "", status);
    vm->dirty = true;
}

static void
search(struct vm *vm, const char *query, bool reverse, bool command_input)
{
    struct vm_window *window = &vm->windows[vm->focus];
    if (!document_view(window)) { notice(vm, "Search needs a transcript or report"); return; }
    if (query && *query) {
        char *copy = snag_strdup_checked(query, SNAG_MAX_DIRECT_PROMPT);
        if (!copy) { notice(vm, "Cannot retain search text"); return; }
        free(vm->search_query);
        vm->search_query = copy;
    }
    if (query) vm->search_reverse = reverse;
    if (!vm->search_query) { notice(vm, "No previous search"); return; }
    struct snag_vm_read_request request = {
        .kind = window->kind == VIEW_REPORT ? SNAG_VM_READ_REPORT : SNAG_VM_READ_HISTORY,
        .report = window->report, .route = window->route, .query = vm->search_query,
        .search_reverse = reverse,
        .ignorecase = vm->ignorecase, .verbosity = window->verbosity,
        .columns = window->rectangle.columns, .refresh = true};
    if (!window->anchor_key[0]) remember_anchor(window);
    memcpy(request.session_id, window->session_id, sizeof(request.session_id));
    request.trusted_tail = snag_vm_connection_tail(focused_connection(vm), &request.tail);
    if (window->visual.kind && window->visual_tail.next_seq) {
        request.tail = window->visual_tail;
        request.trusted_tail = request.pin_tail = true;
        request.refresh = false;
    }
    (void)snprintf(request.search_start.key, sizeof(request.search_start.key), "%s",
        window->anchor_key);
    request.search_start.seq = window->anchor_seq;
    request.search_start.byte = window->anchor_byte;
    request.search_start.heading = window->anchor_heading;
    struct snag_vm_document_row row;
    if (snag_vm_document_row(window->document, window->selected, &row) == 0)
        request.search_start.order = snag_vm_search_order(
            snag_vm_document_block(window->document, row.block));
    vm->scan.generation = reader_request(vm, &vm->scan, &request);
    if (!vm->scan.generation) { notice(vm, "Cannot start search"); return; }
    vm->searching = true;
    vm->search_command = command_input;
    vm->scan.window = window->id;
    vm->scan.load = LOAD_NONE;
    window->load = LOAD_NONE;
    window->follow = false;
    vm->composer = vm->insert = false;
    search_notice(vm, "Searching… Ctrl-C cancels");
    changed(vm);
}

static void
refresh(struct vm *vm)
{
    char error[256] = "cannot read workspace list";
    json_t *rows = snag_vm_workspace_list(&vm->store, vm->stored_limit, error, sizeof(error));
    if (rows) {
        json_decref(vm->workspaces);
        vm->workspaces = rows;
    } else notice(vm, error);
    bool sessions = false;
    for (size_t i = 0u; i < vm->count; ++i)
        if (vm->windows[i].kind == VIEW_SESSIONS) sessions = true;
    if (!sessions) { vm->dirty = true; return; }
    struct snag_vm_read_request request = {.kind = SNAG_VM_READ_SESSIONS,
        .stored_limit = vm->stored_limit};
    vm->page.generation = reader_request(vm, &vm->page, &request);
    vm->page.window = 0u;
    if (!vm->page.generation) notice(vm, "Cannot start session list loading");
    vm->dirty = true;
}

static int
install_workspace(struct vm *vm, struct snag_vm_workspace *next)
{
    cancel_search(vm);
    char error[256];
    int rc = save(vm, NULL);
    if (!rc) {
        cancel_read(&vm->page);
        rc = state_restore(vm, json_object_get(next->snapshot, "state"), error, sizeof(error));
        if (rc < 0) notice(vm, error);
    }
    if (rc < 0) {
        snag_vm_workspace_close(next);
        free(next);
        return -1;
    }
    snag_vm_workspace_close(vm->workspace);
    free(vm->workspace);
    vm->workspace = next;
    vm->meaningful = true;
    vm->save_dirty = false;
    notice(vm, vm->classic_uncertain ?
        "Saved classic input has an uncertain outcome; inspect history, then :recover" :
        "Workspace restored");
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next)
        if (connection_visible(vm, c)) (void)snag_vm_connection_open(c, &vm->store, c->control);
    refresh(vm);
    return 0;
}

static int
restore_workspace(struct vm *vm, const char *selector)
{
    if (direct_session(vm)) {
        notice(vm, "Quit the live session before switching workspaces");
        return -1;
    }
    struct snag_vm_workspace *next = calloc(1u, sizeof(*next));
    if (!next) return -1;
    snag_vm_workspace_init(next);
    char error[256];
    struct vm checked = {0};
    int rc = snag_vm_workspace_open(next, &vm->store, selector, error, sizeof(error));
    if (!rc) rc = state_restore(&checked, json_object_get(next->snapshot, "state"),
        error, sizeof(error));
    snag_vm_connections_free(checked.connections);
    snag_buf_free(&checked.classic.bytes);
    windows_free(checked.windows, checked.count);
    snag_vm_layout_free(checked.layout);
    if (rc < 0) {
        snag_vm_workspace_close(next);
        free(next);
        notice(vm, error);
        return -1;
    }
    bool connected = false;
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next) {
        if (!snag_view_channel_opened(&c->channel)) continue;
        connected = true;
        snag_vm_connection_detach(c);
    }
    if (!connected) return install_workspace(vm, next);
    vm->switch_workspace = next;
    notice(vm, "Saving owner drafts before workspace switch");
    return 0;
}

static void
split(struct vm *vm, enum snag_vm_split axis)
{
    cancel_search(vm);
    if (vm->count == SIZE_MAX / sizeof(*vm->windows) || vm->next_window > INT64_MAX) return;
    struct vm_window copy = vm->windows[vm->focus];
    copy.filter = snag_strdup_checked(copy.filter ? copy.filter : "", SNAG_MAX_DIRECT_PROMPT);
    if (!copy.filter) return;
    struct vm_window *grown = realloc(vm->windows, (vm->count + 1u) * sizeof(*grown));
    if (!grown) {
        free(copy.filter);
        return;
    }
    vm->windows = grown;
    if (snag_vm_layout_split(vm->layout, copy.id, vm->next_window, axis) < 0) {
        free(copy.filter);
        notice(vm, "Cannot split this window further");
        return;
    }
    copy.document = snag_vm_document_ref(copy.document);
    copy.report = json_incref(copy.report);
    copy.route = json_incref(copy.route);
    if (copy.report_open_pending || copy.kind == VIEW_REPORTS) copy.report_catalog = true;
    if (document_view(&copy) && !copy.document)
        copy.load = copy.kind == VIEW_REPORT ? LOAD_KEEP :
            copy.anchor_seq ? LOAD_ANCHOR : LOAD_LAST;
    copy.id = vm->next_window++;
    copy.launch = 0u;
    vm->windows[vm->count] = copy;
    vm->focus = vm->count++;
    changed(vm);
}

static void view(struct vm *vm, enum view_kind kind);

static void
detach_workspace(struct vm *vm)
{
    if (direct_session(vm)) {
        notice(vm, "This host runs the session inside the workspace; keep it open to preserve it");
        return;
    }
    cancel_search(vm);
    if (save(vm, NULL) < 0) return;
    vm->detach_exit = true;
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next)
        snag_vm_connection_detach(c);
    notice(vm, "Saving drafts and detaching workspace; owners continue");
}

static void
close_window(struct vm *vm)
{
    cancel_search(vm);
    if (vm->count == 1u) {
        if (direct_session(vm)) {
            view(vm, VIEW_SESSIONS);
            notice(vm, "Live session hidden; :buffers reopens it, :qa quits it and the workspace");
            return;
        }
        detach_workspace(vm);
        return;
    }
    if (snag_vm_layout_close(vm->layout, vm->windows[vm->focus].id) < 0) return;
    free(vm->windows[vm->focus].filter);
    snag_vm_document_free(vm->windows[vm->focus].document);
    json_decref(vm->windows[vm->focus].report);
    json_decref(vm->windows[vm->focus].route);
    memmove(vm->windows + vm->focus, vm->windows + vm->focus + 1u,
        (vm->count - vm->focus - 1u) * sizeof(*vm->windows));
    if (vm->focus >= --vm->count) vm->focus = vm->count - 1u;
    vm->composer = vm->insert = false;
    detach_unused(vm);
    changed(vm);
}

static void
view(struct vm *vm, enum view_kind kind)
{
    cancel_search(vm);
    struct vm_window *window = &vm->windows[vm->focus];
    if (vm->page.window == window->id) cancel_read(&vm->page);
    snag_vm_document_free(window->document);
    window->document = NULL;
    json_decref(window->report);
    window->report = NULL;
    json_decref(window->route);
    window->route = NULL;
    window->report_catalog = window->report_open_pending = false;
    window->report_selector[0] = '\0';
    window->load = LOAD_NONE;
    window->visual.kind = SNAG_VM_SELECT_NONE;
    window->yank_motion = window->yank_after_load = false;
    window->follow = window->source_failed = false;
    window->follow_at = 0u;
    window->session_id[0] = window->anchor_key[0] = 0;
    window->launch = 0u;
    window->anchor_seq = window->anchor_byte = 0u;
    memset(&window->begin, 0, sizeof(window->begin));
    memset(&window->end, 0, sizeof(window->end));
    memset(&window->tail, 0, sizeof(window->tail));
    window->anchor_heading = window->anchor_source = false;
    window->kind = kind;
    window->selected = window->top = 0u;
    window->selected_id[0] = 0;
    free(window->filter);
    window->filter = NULL;
    vm->composer = vm->insert = false;
    changed(vm);
}

static bool
open_history(struct vm *vm, const char *selector)
{
    char error[256];
    struct snag_session location;
    snag_session_init(&location);
    bool opened = snag_session_locate(&vm->store, &location, selector, NULL, NULL,
        error, sizeof(error)) == 0;
    if (!opened) notice(vm, error);
    else {
        view(vm, VIEW_TRANSCRIPT);
        struct vm_window *window = &vm->windows[vm->focus];
        memcpy(window->session_id, location.id, sizeof(window->session_id));
        window->verbosity = 0u;
        window->follow = true;
        window->best_effort = true;
        struct snag_vm_connection *c = connection_for(vm, location.id, true);
        if (c && !snag_view_channel_opened(&c->channel) && snag_session_host_supported())
            (void)snag_vm_connection_open(c, &vm->store, false);
        queue_history(vm, window, LOAD_LAST);
        notice(vm, c && c->bound ? c->message :
            "Read-only retained history  gg/G: oldest/newest  :verbosity 0..6");
    }
    snag_session_close(&location);
    detach_unused(vm);
    return opened;
}

static void
open_conversation(struct vm *vm, struct snag_vm_buffer *buffer)
{
    json_t *route = json_incref(buffer->route);
    view(vm, VIEW_TRANSCRIPT);
    struct vm_window *window = &vm->windows[vm->focus];
    memcpy(window->session_id, buffer->connection->session, sizeof(window->session_id));
    window->route = route;
    window->verbosity = 0u;
    window->follow = true;
    window->best_effort = true;
    queue_history(vm, window, LOAD_LAST);
    notice(vm, snag_vm_buffer_writable(buffer) ?
        "Conversation  i: compose  :history: rollout" :
        "Agent conversation (read-only)  :history: rollout");
}

static char *
buffer_address(const struct snag_vm_buffer *buffer)
{
    struct snag_irc_address address = {.kind = json_is_object(buffer->route) ?
        SNAG_IRC_CONVERSATION : SNAG_IRC_TRANSCRIPT};
    if (address.kind == SNAG_IRC_CONVERSATION &&
        !json_object_get(buffer->route, "room") && !json_object_get(buffer->route, "peer"))
        address.kind = SNAG_IRC_CONNECTION;
    (void)snag_strcpy(address.session, sizeof(address.session), buffer->connection->session);
    if (address.kind != SNAG_IRC_TRANSCRIPT) {
        (void)snag_strcpy(address.endpoint, sizeof(address.endpoint), buffer->endpoint[0] ?
            buffer->endpoint : snag_json_string(buffer->route, "connection"));
        if (address.kind == SNAG_IRC_CONVERSATION)
            (void)snag_strcpy(address.target, sizeof(address.target),
                snag_view_conversation_name(buffer->route));
    }
    return snag_irc_address_format(&address);
}

static int
buffer_rank(const json_t *route)
{
    return !json_is_object(route) ? -1 : json_object_get(route, "room") ? 1 :
        json_object_get(route, "peer") ? 2 : 0;
}

static int
compare_buffers(const void *left, const void *right)
{
    const json_t *a = *(json_t *const *)left;
    const json_t *b = *(json_t *const *)right;
    const char *fields[] = {"session_name", "session", "endpoint"};
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        int order = strcmp(snag_json_string(a, fields[i]), snag_json_string(b, fields[i]));
        if (order) return order;
    }
    const json_t *ar = json_object_get(a, "route");
    const json_t *br = json_object_get(b, "route");
    const char *ac = snag_json_string(ar, "connection");
    const char *bc = snag_json_string(br, "connection");
    int order = strcmp(ac ? ac : "", bc ? bc : "");
    if (order) return order;
    order = buffer_rank(ar) - buffer_rank(br);
    if (order) return order;
    order = strcmp(snag_json_string(a, "label"), snag_json_string(b, "label"));
    if (order) return order;
    const char *ai = snag_json_string(ar, "identity");
    const char *bi = snag_json_string(br, "identity");
    order = (ai && !strcmp(ai, "agent")) - (bi && !strcmp(bi, "agent"));
    return order ? order : strcmp(snag_json_string(a, "id"), snag_json_string(b, "id"));
}

static bool
buffer_stale(const struct snag_vm_buffer *buffer, const json_t *state)
{
    if (!state) return false;
    const json_t *route = json_object_get(state, "route");
    const char *fields[] = {"generation", "peer", "room", "membership"};
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        const json_t *a = json_object_get(buffer->route, fields[i]);
        const json_t *b = json_object_get(route, fields[i]);
        if ((a || b) && !json_equal(a, b)) return true;
    }
    return false;
}

static bool
buffer_visible(struct vm *vm, const struct snag_vm_buffer *buffer)
{
    for (size_t i = 0u; i < vm->count; ++i) {
        const struct vm_window *window = &vm->windows[i];
        if (window_buffer(vm, window) == buffer) return true;
        const json_t *row = window->kind == VIEW_BUFFERS ? selected_row(vm, window) : NULL;
        const char *session = snag_json_string(row, "session");
        if (session && !strcmp(session, buffer->connection->session) &&
            json_equal(json_object_get(row, "route"), buffer->route)) return true;
    }
    return false;
}

static int
buffer_catalog(struct vm *vm)
{
    json_t *rows = json_array();
    json_t **ordered = NULL;
    if (!rows) return -1;
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next) {
        for (struct snag_vm_buffer *b = c->buffers; b; b = b->next) {
            const json_t *state = snag_vm_buffer_state(b);
            bool stale = buffer_stale(b, state);
            if (stale && !b->draft.len && !b->pending && !b->draft_conflict &&
                !buffer_visible(vm, b)) continue;
            char *address = buffer_address(b);
            char *route = json_dumps(b->route, JSON_COMPACT | JSON_SORT_KEYS | JSON_ENCODE_ANY);
            char digest[SNAG_SHA256_HEX_LEN + 1u];
            if (!address || !route) { free(address); free(route); goto failed; }
            snag_sha256_hex(route, strlen(route), digest);
            free(route);
            digest[SNAG_ID_HEX_LEN] = 0;
            if (json_is_string(b->route)) memcpy(digest, c->session, sizeof(c->session));
            const char *name = snag_json_string(c->state, "name");
            const char *label = buffer_rank(b->route) < 0 ? name && *name ? name : "session" :
                snag_view_conversation_name(b->route);
            struct snag_vm_activity activity;
            snag_vm_buffer_activity(b, &activity);
            json_t *row = json_pack("{s:s,s:s,s:s,s:O,s:b,s:b,s:s,s:s,s:s,s:b,s:O,s:O,"
                "s:b,s:b,s:I,s:I,s:s}",
                "id", digest, "name", address, "session", c->session, "route", b->route,
                "draft", b->draft.len != 0u, "pending", b->pending != NULL,
                "session_name", name ? name : "", "endpoint", b->endpoint, "label", label,
                "stale", stale, "connected", state && json_object_get(state, "connected") ?
                    json_object_get(state, "connected") : json_null(),
                "joined", state && json_object_get(state, "joined") ?
                    json_object_get(state, "joined") : json_null(),
                "activity_known", activity.known, "activity_exact", activity.exact,
                "unread", (json_int_t)activity.unread, "time", (json_int_t)activity.time,
                "owner", c->bound ? "attached" : c->hello ? "observing" : "stored");
            free(address);
            if (!row || json_array_append_new(rows, row) < 0) goto failed;
        }
    }
    size_t count = json_array_size(rows);
    ordered = calloc(count ? count : 1u, sizeof(*ordered));
    if (!ordered) goto failed;
    for (size_t i = 0u; i < count; ++i) ordered[i] = json_array_get(rows, i);
    if (count > 1u) qsort(ordered, count, sizeof(*ordered), compare_buffers);
    json_t *sorted = json_array();
    if (!sorted) goto failed;
    for (size_t i = 0u; i < count; ++i) {
        if (json_array_append(sorted, ordered[i]) < 0) {
            json_decref(sorted);
            goto failed;
        }
    }
    free(ordered);
    ordered = NULL;
    json_decref(rows);
    rows = sorted;
    if (json_equal(vm->buffers, rows)) { json_decref(rows); return 0; }
    for (size_t i = 0u; i < vm->count; ++i) {
        struct vm_window *window = &vm->windows[i];
        if (window->kind != VIEW_BUFFERS) continue;
        const char *id = snag_json_string(selected_row(vm, window), "id");
        if (id) (void)snag_strcpy(window->selected_id, sizeof(window->selected_id), id);
        size_t visible = 0u;
        for (size_t j = 0u; j < count; ++j) {
            const json_t *row = json_array_get(rows, j);
            if (!matches(window, row)) continue;
            if (!strcmp(window->selected_id, snag_json_string(row, "id")))
                window->selected = visible;
            ++visible;
        }
    }
    json_decref(vm->buffers);
    vm->buffers = rows;
    return 0;
failed:
    free(ordered);
    json_decref(rows);
    return -1;
}

static void
open_buffer_row(struct vm *vm, const json_t *row)
{
    if (!row) return;
    struct snag_vm_connection *c = connection_for(vm, snag_json_string(row, "session"), false);
    struct snag_vm_buffer *b = snag_vm_buffer_get(c, json_object_get(row, "route"), false);
    if (!b) { notice(vm, "Buffer is no longer available"); return; }
    if (json_is_object(b->route)) {
        open_conversation(vm, b);
        if (!snag_view_channel_opened(&c->channel) && snag_session_host_supported())
            (void)snag_vm_connection_open(c, &vm->store, c->control);
    } else (void)open_history(vm, c->session);
}

static int
locate_buffer_session(struct vm *vm, struct snag_session *location, const char *selector,
    char *error, size_t size)
{
    int rc = snag_session_locate(&vm->store, location, selector, NULL, NULL, error, size);
    if (rc == 0 || (errno != EINVAL && errno != ENOENT)) return rc;
    char id[SNAG_ID_HEX_LEN + 1u];
    if (snag_store_find_name(&vm->store, selector, id, NULL, NULL, error, size) < 0) return -1;
    return snag_session_locate(&vm->store, location, id, NULL, NULL, error, size);
}

static void
buffer_select(struct vm *vm, const char *text)
{
    char *selector = NULL, error[256];
    const char *rest;
    if (snag_irc_address_operand(text, &selector, &rest, error, sizeof(error)) < 0) {
        notice(vm, error);
        return;
    }
    if (*rest) notice(vm, "Use :buffer ADDRESS or :buffer ID from :buffers");
    else if (buffer_catalog(vm) == 0) {
        const json_t *match = NULL;
        size_t matches = 0u;
        for (size_t i = 0u; i < json_array_size(vm->buffers); ++i) {
            const json_t *row = json_array_get(vm->buffers, i);
            const char *id = snag_json_string(row, "id");
            if (!strcmp(selector, snag_json_string(row, "name")) ||
                (*selector && strlen(selector) <= SNAG_ID_HEX_LEN &&
                 !strncmp(selector, id, strlen(selector)))) {
                match = row;
                ++matches;
            }
        }
        if (!matches) {
            struct snag_irc_address address;
            struct snag_session location;
            snag_session_init(&location);
            if (snag_irc_address_parse(&address, selector, SNAG_IRC_BUFFER_ADDRESS,
                error, sizeof(error)) == 0 &&
                locate_buffer_session(vm, &location, address.session, error, sizeof(error)) == 0) {
                for (size_t i = 0u; i < json_array_size(vm->buffers); ++i) {
                    const json_t *row = json_array_get(vm->buffers, i);
                    const json_t *route = json_object_get(row, "route");
                    struct snag_vm_connection *c = connection_for(vm, location.id, false);
                    struct snag_vm_buffer *b = snag_vm_buffer_get(c, route, false);
                    if (!b || strcmp(location.id, snag_json_string(row, "session"))) continue;
                    bool same = address.kind == SNAG_IRC_TRANSCRIPT && json_is_string(route);
                    if (address.kind == SNAG_IRC_CONNECTION && json_is_object(route))
                        same = !json_object_get(route, "peer") && !json_object_get(route, "room") &&
                            snag_vm_buffer_writable(b) && !strcmp(address.endpoint, b->endpoint);
                    if (address.kind == SNAG_IRC_CONVERSATION && json_is_object(route))
                        same = snag_vm_buffer_writable(b) &&
                            !strcmp(address.endpoint, b->endpoint) &&
                            (json_object_get(route, "room") ? snag_irc_name_equal(
                                (enum snag_irc_casemapping)json_integer_value(
                                    json_object_get(route, "casemapping")),
                                address.target, snag_view_conversation_name(route)) :
                                !strcmp(address.target, snag_view_conversation_name(route)));
                    if (same) { match = row; ++matches; }
                }
            }
            snag_session_close(&location);
        }
        if (matches == 1u) open_buffer_row(vm, match);
        else notice(vm, matches ? "Ambiguous buffer; choose its exact ID in :buffers" :
            "Unknown buffer; use :buffers, /query NICK or /chat #CHANNEL");
    }
    free(selector);
}

static void
buffer_cycle(struct vm *vm, bool previous)
{
    struct snag_vm_buffer *b = focused_buffer(vm);
    if (!b) return;
    struct snag_vm_connection *c = b->connection;
    struct snag_vm_buffer *next = b->next ? b->next : c->buffers;
    if (previous) {
        for (struct snag_vm_buffer *candidate = c->buffers; candidate; candidate = candidate->next)
            if (candidate->next == b || (!candidate->next && b == c->buffers)) next = candidate;
    }
    if (next == b) return;
    if (json_is_object(next->route)) open_conversation(vm, next);
    else (void)open_history(vm, c->session);
}

static void
open_report(struct vm *vm, struct snag_vm_connection *connection, json_t *report)
{
    if (!connection || !snag_vm_report_valid(report)) {
        notice(vm, "No retained command report; use :reports");
        return;
    }
    report = json_incref(report);
    view(vm, VIEW_REPORT);
    struct vm_window *window = &vm->windows[vm->focus];
    memcpy(window->session_id, connection->session, sizeof(window->session_id));
    window->report = report;
    memcpy(window->anchor_key, snag_json_string(report, "id"), SNAG_ID_HEX_LEN + 1u);
    window->anchor_byte = 0u;
    window->load = LOAD_FIRST;
    notice(vm, "Command report  gg/G: first/last  :reports: earlier output  :history: session");
}

static void
attach(struct vm *vm, const char *selector)
{
    if (*selector && !open_history(vm, selector)) return;
    struct vm_window *window = &vm->windows[vm->focus];
    if (window->kind != VIEW_TRANSCRIPT) {
        notice(vm, "Use :attach SESSION or Enter in the session picker");
        return;
    }
    struct snag_vm_connection *c = connection_for(vm, window->session_id, true);
    if (!c) return;
    if (!snag_session_host_supported() && !c->direct) {
        notice(vm, "Use :session to start a stored session; "
            "attaching another process is unavailable");
        return;
    }
    if (!c->bound && !c->direct) (void)snag_vm_connection_open(c, &vm->store, true);
    notice(vm, c->message);
    changed(vm);
}

static void
launch_owner(struct vm *vm, const char *id, const char *name)
{
    if (direct_session(vm)) {
        notice(vm, "One live session per workspace on this host; quit it before starting another");
        return;
    }
    for (struct vm_launch *job = vm->launches; id && job; job = job->next) {
        if (job->fd >= 0 && !strcmp(job->session, id)) {
            vm->windows[vm->focus].launch = job->token;
            notice(vm, "This owner is already starting");
            return;
        }
    }
    struct vm_launch *job = calloc(1u, sizeof(*job));
    if (!job) return;
    job->fd = -1;
    snag_view_channel_init(&job->channel, -1);
    if (snag_session_host_supported())
        job->fd = snag_session_launch(vm->program, &vm->terminal, vm->store.root_path,
            id, name, &job->child);
    else job->direct = snag_app_direct_start(vm->program, vm->store.root_path,
        id, name, &job->channel);
    if (job->fd < 0 && !job->direct) {
        char error[256];
        (void)snprintf(error, sizeof(error), "Cannot launch owner: %s", strerror(errno));
        notice(vm, error);
        free(job);
        return;
    }
    if (id) memcpy(job->session, id, sizeof(job->session));
    else view(vm, VIEW_SESSIONS);
    job->token = ++vm->next_launch;
    vm->windows[vm->focus].launch = job->token;
    job->next = vm->launches;
    vm->launches = job;
    detach_unused(vm);
    notice(vm, job->direct ? "Starting live session; this workspace owns its lifetime" :
        "Starting owner; workspace remains available. Closing it leaves the owner running.");
}

static void
session_request(struct vm *vm, const char *selector)
{
    const struct vm_window *window = &vm->windows[vm->focus];
    if (!*selector) selector = window->session_id[0] ? window->session_id :
        window->kind == VIEW_SESSIONS ? snag_json_string(selected_row(vm, window), "id") : NULL;
    if (!selector || !*selector) {
        notice(vm, "Use :session SESSION_ID or select a session");
        return;
    }
    struct snag_session location;
    snag_session_init(&location);
    char error[256], id[SNAG_ID_HEX_LEN + 1u];
    bool live = false;
    int rc = snag_session_locate(&vm->store, &location, selector, NULL, NULL, error, sizeof(error));
    if (!rc) {
        memcpy(id, location.id, sizeof(id));
        live = snag_session_is_live(&location);
    }
    snag_session_close(&location);
    if (rc < 0) notice(vm, error);
    else if (live) attach(vm, id);
    else if (open_history(vm, id)) launch_owner(vm, id, NULL);
}

static void
launch_result(struct vm *vm, struct vm_launch *job, const char *id, const char *message)
{
    refresh(vm);
    for (size_t i = 0u; i < vm->count; ++i) {
        if (vm->windows[i].launch != job->token) continue;
        vm->windows[i].launch = 0u;
        if (!id) continue;
        size_t focus = vm->focus;
        bool composer = vm->composer, insert = vm->insert;
        vm->focus = i;
        if (job->direct) (void)open_history(vm, id);
        else attach(vm, id);
        vm->focus = focus;
        if (focus != i) {
            vm->composer = composer;
            vm->insert = insert;
        }
    }
    notice(vm, message);
}

static bool
direct_step(struct vm *vm, struct vm_launch *job)
{
    char id[SNAG_ID_HEX_LEN + 1u], error[256];
    int status;
    enum snag_app_direct_state state = snag_app_direct_state(job->direct,
        id, error, sizeof(error), &status);
    if (!job->ready && state == SNAG_APP_DIRECT_READY) {
        memcpy(job->session, id, sizeof(job->session));
        struct snag_vm_connection *c = connection_for(vm, id, true);
        if (!c || snag_vm_connection_direct(c, &job->channel) < 0) {
            snag_app_direct_stop(job->direct);
            notice(vm, "Cannot connect the live session; waiting for shutdown");
        } else launch_result(vm, job, id, "Live session ready; :close hides it, :q quits it");
        job->ready = true;
    }
    if (state != SNAG_APP_DIRECT_FINISHED) return false;
    struct snag_vm_connection *c = connection_for(vm, job->session, false);
    /* Keep the endpoint until the client consumes the final queued EXIT. */
    if (c && snag_view_channel_opened(&c->channel)) return false;
    if (!job->ready) launch_result(vm, job, NULL, *error ? error : "Session startup stopped");
    else if (c && !c->exited) {
        c->exited = true;
        c->control = false;
        ++c->revision;
        notice(vm, *error ? error : status ? "Live session failed" : "Live session stopped");
    }
    snag_view_channel_close(&job->channel);
    snag_app_direct_free(job->direct);
    return true;
}

static void
launches_step(struct vm *vm)
{
    struct vm_launch **link = &vm->launches;
    while (*link) {
        struct vm_launch *job = *link;
        if (job->direct) {
            if (direct_step(vm, job)) {
                *link = job->next;
                free(job);
            } else link = &job->next;
            continue;
        }
        snag_session_launch_reap(&job->child);
        int rc = job->fd < 0 ? 0 : snag_session_packet_read(job->fd, &job->packet);
        if (rc) {
            (void)close(job->fd);
            job->fd = -1;
            const char *data = (const char *)job->packet.bytes + SNAG_SESSION_HEADER;
            size_t length = rc > 0 ? snag_session_packet_length(&job->packet) : 0u;
            bool ready = rc > 0 && snag_session_packet_type(&job->packet) == SNAG_SESSION_READY &&
                length == SNAG_ID_HEX_LEN;
            char id[SNAG_ID_HEX_LEN + 1u] = "";
            if (ready) {
                memcpy(id, data, length);
                ready = snag_hex_is_lower(id, SNAG_ID_HEX_LEN);
            }
            char message[512];
            if (ready) (void)snprintf(message, sizeof(message), "Session ready: %s", id);
            else if (rc > 0 && snag_session_packet_type(&job->packet) == SNAG_SESSION_ERROR)
                (void)snprintf(message, sizeof(message), "%.*s", (int)length, data);
            else (void)snprintf(message, sizeof(message),
                "Owner startup report lost; inspect :sessions before trying again");
            launch_result(vm, job, ready ? id : NULL, message);
        }
        if (job->fd < 0 && !job->child) {
            *link = job->next;
            free(job);
        } else link = &job->next;
    }
}

static int
classic_connect(void *opaque, const char *selector, char *selected, char *error, size_t size)
{
    struct vm *vm = opaque;
    struct snag_session location;
    snag_session_init(&location);
    int peer = -1;
    if (snag_session_locate(&vm->store, &location, selector, NULL, NULL, error, size) == 0) {
        peer = snag_session_endpoint_connect(location.dir_fd, location.dir_path);
        if (peer >= 0 && selected) memcpy(selected, location.id, sizeof(location.id));
        if (peer < 0) (void)snag_errorf(error, size,
            "No reachable native owner for %.8s: %s", location.id, strerror(errno));
    }
    snag_session_close(&location);
    return peer;
}

static void
classic_request(struct vm *vm, const char *selector)
{
    char id[SNAG_ID_HEX_LEN + 1u], error[256];
    if (!snag_session_host_supported()) {
        notice(vm, "Classic attachment is unavailable on this host");
        return;
    }
    if (vm->classic_uncertain && vm->classic.bytes.len) {
        notice(vm, "Saved classic input has an uncertain outcome; inspect history, then :recover");
        return;
    }
    const struct vm_window *window = &vm->windows[vm->focus];
    if (!*selector) selector = vm->classic.bytes.len ? vm->classic.session :
        window->session_id[0] ? window->session_id :
        window->kind == VIEW_SESSIONS ? snag_json_string(selected_row(vm, window), "id") : "";
    if (!*selector) {
        notice(vm, "Use :classic SESSION_ID or select a session");
        return;
    }
    struct snag_session location;
    snag_session_init(&location);
    int found = snag_session_locate(&vm->store, &location, selector, NULL, NULL,
        error, sizeof(error));
    if (!found) memcpy(id, location.id, sizeof(id));
    snag_session_close(&location);
    if (found < 0) {
        notice(vm, error);
        return;
    }
    if (vm->classic.bytes.len && strcmp(id, vm->classic.session)) {
        notice(vm, "Queued classic input belongs to another session; :recover it first");
        return;
    }
    memset(vm->classic.command, 0, sizeof(vm->classic.command));
    struct snag_vm_connection *source = connection_for(vm, id, false);
    if (source && source->terminal_commands && source->rollout->terminal_result &&
        source->rollout->pending) {
        memcpy(vm->classic.command, source->instance, SNAG_ID_HEX_LEN);
        memcpy(vm->classic.command + SNAG_ID_HEX_LEN,
            snag_json_string(source->rollout->pending, "id"), SNAG_ID_HEX_LEN);
        source->rollout->terminal_auto = false;
    }
    memcpy(vm->classic.session, id, sizeof(id));
    vm->classic_pending = true;
    vm->classic_ready = false;
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next)
        snag_vm_connection_detach(c);
    notice(vm, "Saving owner drafts before classic attachment; /s d returns here");
    changed(vm);
}

static bool
classic_recover(struct vm *vm)
{
    if (!vm->classic.bytes.len) return false;
    struct snag_buf text = {.max = SNAG_MAX_DIRECT_PROMPT};
    for (size_t i = 0u; i < vm->classic.bytes.len;) {
        const unsigned char *bytes = vm->classic.bytes.data + i;
        uint32_t cp;
        size_t count = snag_utf8_decode(bytes, vm->classic.bytes.len - i, &cp);
        int rc;
        if (*bytes == '\r') {
            count = 1u;
            rc = snag_buf_putc(&text, '\n');
        } else if (!count || (*bytes < 32u && *bytes != '\n' && *bytes != '\t') || *bytes == 127u) {
            count = 1u;
            rc = snag_buf_printf(&text, "\\x%02x", *bytes);
        } else rc = snag_buf_append(&text, bytes, count);
        if (rc < 0) {
            notice(vm, "Recovered input exceeds the draft limit; classic input remains saved");
            snag_buf_free(&text);
            return true;
        }
        i += count;
    }
    if (open_history(vm, vm->classic.session)) {
        struct snag_vm_buffer *c = focused_buffer(vm);
        if (c && snag_vm_editor_end(c) == 0 && snag_vm_editor_replace(c,
            c->draft.len, c->draft.len, text.data, text.len) == 0) {
            (void)snag_vm_editor_end(c);
            snag_buf_reset(&vm->classic.bytes);
            vm->classic.session[0] = 0;
            vm->classic_uncertain = false;
            vm->composer = vm->insert = true;
            changed(vm);
            notice(vm, "Classic input recovered as unsent draft; review it before submitting");
            (void)save(vm, NULL);
        } else notice(vm, "Cannot recover into this draft; classic input remains saved");
    }
    snag_buf_free(&text);
    return true;
}

static void
quit_sessions(struct vm *vm, bool all, bool force)
{
    struct snag_vm_connection *focused = focused_connection(vm);
    struct vm_launch *direct = direct_session(vm);
    bool any = false;
    if (vm->classic.bytes.len && (all || vm->count == 1u || (focused &&
        !strcmp(focused->session, vm->classic.session)))) {
        if (!force) {
            notice(vm, "Unsent classic input; :recover it, :close preserves it, "
                "or :q! discards it");
            return;
        }
        snag_buf_reset(&vm->classic.bytes);
        vm->classic_uncertain = false;
    }
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next) {
        if ((!c->bound && !(all && direct && !strcmp(c->session, direct->session))) ||
            (!all && c != focused)) continue;
        if (!force && snag_vm_connection_unsaved(c)) {
            notice(vm, "Unsent draft or unresolved submission; :close preserves it, :q! discards");
            return;
        }
        bool synchronizing = c->draft_wait != NULL;
        for (struct snag_vm_buffer *b = c->buffers; b; b = b->next) {
            if (!snag_vm_buffer_writable(b) ||
                !snag_vm_buffer_supported(b)) continue;
            synchronizing |= b->draft_get || !b->draft_ready || b->draft_dirty;
        }
        if (!force && c->drafts && synchronizing) {
            notice(vm, "Waiting for the owner draft; retry quit after synchronization");
            return;
        }
        if (c->channel.output) {
            notice(vm, "Owner request is still sending; retry quit");
            return;
        }
    }
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next) {
        if (!c->bound || (!all && c != focused)) continue;
        if (force) snag_vm_connection_discard(c);
        if (snag_vm_connection_control(c, "quit") < 0) {
            notice(vm, "Cannot request session shutdown");
            return;
        }
        any = true;
    }
    if (all && direct && !any) {
        struct snag_vm_connection *c = connection_for(vm, direct->session, false);
        if (force && c) snag_vm_connection_discard(c);
        snag_app_direct_stop(direct->direct);
        any = true;
    }
    if (any) {
        vm->quit_all = all;
        vm->quit_window = all ? 0u : vm->windows[vm->focus].id;
        notice(vm, direct ? "Waiting for live session shutdown" :
            "Waiting for session shutdown; :detach stops waiting");
        changed(vm);
    } else if (all) {
        if (save(vm, NULL) == 0) vm->quit = true;
    } else close_window(vm);
}

static void
load_history(struct vm *vm)
{
    for (size_t i = 0u; i < vm->count; ++i) {
        struct vm_window *window = &vm->windows[(vm->focus + i) % vm->count];
        if (vm->scan.window == window->id) continue;
        if (window->report_catalog && window->rectangle.visible) {
            if (vm->page.generation && (i || !vm->page.window)) return;
            struct snag_vm_connection *c = connection_for(vm, window->session_id, false);
            struct snag_vm_read_request request = {.kind = SNAG_VM_READ_REPORTS,
                .known_reports = c ? c->reports : NULL};
            memcpy(request.session_id, window->session_id, sizeof(request.session_id));
            vm->page.generation = reader_request(vm, &vm->page, &request);
            if (!vm->page.generation) notice(vm, "Cannot start report catalogue read");
            vm->page.window = vm->page.generation ? window->id : 0u;
            vm->page.load = LOAD_NONE;
            window->report_catalog = false;
            window->load = LOAD_NONE;
            return;
        }
        if (!document_view(window) || !window->load || !window->rectangle.visible)
            continue;
        if (vm->page.generation && (i || !vm->page.window || window->load == LOAD_POLL)) return;
        struct snag_vm_read_request request = {
            .kind = window->kind == VIEW_REPORT ? SNAG_VM_READ_REPORT : SNAG_VM_READ_HISTORY,
            .report = window->report, .route = window->route, .project = true,
            .verbosity = window->verbosity, .columns = window->rectangle.columns};
        memcpy(request.session_id, window->session_id, sizeof(request.session_id));
        request.previous = window->tail;
        request.trusted_tail = snag_vm_connection_tail(
            connection_for(vm, window->session_id, false), &request.tail);
        if (window->visual.kind && window->visual_tail.next_seq) {
            request.tail = window->visual_tail;
            request.trusted_tail = request.pin_tail = true;
        }
        if (vm->motion_loading && vm->motion_origin.window == window->id &&
            vm->motion_origin.tail.next_seq) {
            request.tail = vm->motion_origin.tail;
            request.trusted_tail = request.pin_tail = true;
        }
        enum history_load load = window->load;
        if (load == LOAD_FIRST || load == LOAD_LAST || load == LOAD_REFRESH || load == LOAD_POLL)
            request.refresh = true;
        if (load == LOAD_POLL && window->visual.kind) {
            window->load = LOAD_NONE;
            continue;
        }
        if (load == LOAD_POLL) {
            request.if_changed = true;
            request.tail_only = !window->follow;
        }
        /* A lower verbosity may have skipped the anchor's entire page. Reload
         * from that source event when detail returns or the view is reflowed. */
        if ((load == LOAD_KEEP || load == LOAD_REFRESH) && window->anchor_seq &&
            (window->anchor_seq < window->begin.next_seq ||
             (window->end.next_seq && window->anchor_seq >= window->end.next_seq))) {
            load = LOAD_ANCHOR;
        }
        if (load == LOAD_LAST || load == LOAD_PREVIOUS || load == LOAD_ANCHOR ||
            load == LOAD_POLL || load == LOAD_KEEP_PREVIOUS) {
            request.reverse = true;
            if (load == LOAD_PREVIOUS || load == LOAD_KEEP_PREVIOUS)
                request.before_seq = window->begin.next_seq;
            if (load == LOAD_ANCHOR) request.before_seq = window->anchor_seq + 1u;
        } else if (load == LOAD_NEXT || load == LOAD_KEEP_NEXT) request.cursor = window->end;
        else if (load == LOAD_KEEP || load == LOAD_REFRESH) request.cursor = window->begin;
        request.rows = window->document ? window->rectangle.rows : 0u;
        vm->page.generation = reader_request(vm, &vm->page, &request);
        if (!vm->page.generation) {
            if (vm->motion_loading && vm->motion_origin.window == window->id) cancel_search(vm);
            notice(vm, "Cannot start history read");
        }
        vm->page.window = vm->page.generation ? window->id : 0u;
        vm->page.load = load;
        window->load = LOAD_NONE;
        window->follow_at = snag_monotonic_ms() + 1000u;
        return;
    }
}

static void
command(struct vm *vm, const char *text)
{
    char *word = NULL, error[256];
    const char *rest;
    if (snag_irc_address_operand(text, &word, &rest, error, sizeof(error)) < 0) {
        notice(vm, error);
        return;
    }
    if (!strcmp(word, "workspace")) {
        if (!*rest) {
            (void)snprintf(vm->message, sizeof(vm->message), "%s  %s%s",
                vm->workspace->id[0] ? vm->workspace->id : "Unsaved workspace",
                vm->workspace->snapshot ? snag_json_string(vm->workspace->snapshot, "name") : "",
                vm->save_dirty ? "  [modified]" : "");
        } else if (!strcmp(rest, "save")) {
            vm->meaningful = true;
            if (save(vm, NULL) == 0) notice(vm, "Workspace saved");
        } else if (!strncmp(rest, "name ", 5u)) {
            char *name = NULL;
            const char *tail;
            if (snag_irc_address_operand(rest + 5u, &name, &tail, error, sizeof(error)) < 0)
                notice(vm, error);
            else if (*tail) notice(vm, "Quote a workspace name containing spaces");
            else if (save(vm, name) == 0) {
                vm->meaningful = true;
                notice(vm, "Workspace named and saved");
            }
            free(name);
        } else notice(vm, "Use :workspace, :workspace save, or :workspace name NAME");
    } else if (!strcmp(word, "draft")) {
        struct snag_vm_buffer *c = focused_buffer(vm);
        if (strcmp(rest, "local") && strcmp(rest, "owner"))
            notice(vm, "Use :draft local or :draft owner to resolve a draft conflict");
        else if (!c || snag_vm_draft_choose(c, !strcmp(rest, "local")) < 0)
            notice(vm, "No ready draft conflict; recover any retained submission first");
        else {
            notice(vm, c->message);
            changed(vm);
        }
    } else if (!strcmp(word, "reports")) {
        struct snag_vm_connection *c = focused_connection(vm);
        if (*rest || !c) notice(vm, "Use :reports in a session window");
        else {
            view(vm, VIEW_REPORTS);
            memcpy(vm->windows[vm->focus].session_id, c->session, sizeof(c->session));
            vm->windows[vm->focus].report_catalog = true;
            notice(vm, "Command reports  Enter: open  :history: return to session");
        }
    } else if (!strcmp(word, "report")) {
        struct snag_vm_connection *c = focused_connection(vm);
        if (!c || strlen(rest) > SNAG_ID_HEX_LEN) {
            notice(vm, "No retained command report; use :reports");
        } else {
            struct vm_window *window = &vm->windows[vm->focus];
            memcpy(window->report_selector, rest, strlen(rest) + 1u);
            window->report_catalog = window->report_open_pending = true;
            notice(vm, "Reading retained report catalogue");
        }
    } else if (!strcmp(word, "history")) {
        char id[SNAG_ID_HEX_LEN + 1u];
        memcpy(id, vm->windows[vm->focus].session_id, sizeof(id));
        if (*rest || *id) open_history(vm, *rest ? rest : id);
        else notice(vm, "Use :history SESSION_ID");
    }
    else if (!strcmp(word, "attach")) attach(vm, rest);
    else if (!strcmp(word, "session")) {
        if (!strcmp(rest, "d") || !strcmp(rest, "detach")) detach_workspace(vm);
        else session_request(vm, rest);
    }
    else if (!strcmp(word, "new")) {
        char *name = NULL;
        const char *tail = "";
        if (*rest && snag_irc_address_operand(rest, &name, &tail, error, sizeof(error)) < 0)
            notice(vm, error);
        else if (*tail || (name && !snag_session_name_valid(name)))
            notice(vm, "Use :new [NAME]; quote a name containing spaces");
        else launch_owner(vm, NULL, name);
        free(name);
    } else if (!strcmp(word, "verbosity")) {
        struct vm_window *window = &vm->windows[vm->focus];
        uint64_t level;
        if (window->kind != VIEW_TRANSCRIPT || snag_parse_count(rest, &level) < 0 ||
            level > SNAG_VERBOSITY_MAX) notice(vm, "Use :verbosity 0..6 in a transcript");
        else {
            cancel_search(vm);
            if (window->visual.kind) notice(vm, "Visual selection ended; register preserved");
            window->visual.kind = SNAG_VM_SELECT_NONE;
            window->yank_motion = window->yank_after_load = false;
            window->verbosity = (unsigned int)level;
            queue_history(vm, window, LOAD_KEEP);
            changed(vm);
        }
    } else if (!strcmp(word, "set")) {
        if (!strcmp(rest, "ignorecase") || !strcmp(rest, "noignorecase")) {
            cancel_search(vm);
            vm->ignorecase = !strcmp(rest, "ignorecase");
            notice(vm, vm->ignorecase ? "ignorecase: Unicode case folding" : "noignorecase");
        } else if (!strcmp(rest, "mouse") || !strcmp(rest, "nomouse")) {
            vm->mouse = !strcmp(rest, "mouse");
            vm->mouse_down = false;
            notice(vm, vm->mouse ? "mouse: click, drag, scroll and resize" :
                "nomouse: terminal selection enabled");
        } else notice(vm, "Use :set ignorecase/noignorecase or :set mouse/nomouse");
    } else if (!strcmp(word, "classic")) classic_request(vm, rest);
    else if (!strcmp(word, "buffer") || !strcmp(word, "b")) buffer_select(vm, rest);
    else if (!strcmp(word, "split") || !strcmp(word, "sp") ||
        !strcmp(word, "vsplit") || !strcmp(word, "vsp")) {
        size_t count = vm->count;
        split(vm, word[0] == 'v' ? SNAG_VM_VERTICAL : SNAG_VM_HORIZONTAL);
        if (vm->count > count && *rest) buffer_select(vm, rest);
    }
    else if (*rest) notice(vm, "Unexpected command argument");
    else if (!strcmp(word, "close")) close_window(vm);
    else if (!strcmp(word, "q") || !strcmp(word, "q!")) {
        enum view_kind kind = vm->windows[vm->focus].kind;
        if (kind == VIEW_REPORT || kind == VIEW_REPORTS || vm->windows[vm->focus].route)
            close_window(vm);
        else quit_sessions(vm, false, word[1] == '!');
    }
    else if (!strcmp(word, "qa") || !strcmp(word, "qa!"))
        quit_sessions(vm, true, word[2] == '!');
    else if (!strcmp(word, "detach")) {
        struct snag_vm_connection *c = focused_connection(vm);
        if (c && c->direct) {
            notice(vm, "This session runs inside the workspace; :close hides it, :q quits it");
        } else if (c) {
            c->control = false;
            snag_vm_connection_detach(c);
            vm->composer = vm->insert = false;
            vm->quit_window = 0u;
            notice(vm, c->quitting ? "Detached from pending shutdown" :
                c->detaching ? c->message : "Detached; owner continues running");
            changed(vm);
        }
    } else if (!strcmp(word, "recover") && classic_recover(vm)) {
        /* The original destination owns recovered input. */
    } else if (!strcmp(word, "recover")) {
        struct snag_vm_buffer *c = focused_buffer(vm);
        struct snag_vm_buffer *pending = submission_from(vm, c);
        if (!pending || snag_vm_buffer_recover(pending, c) < 0)
            notice(vm, "Recovery needs an empty draft and a resolved or disconnected submission");
        else {
            vm->composer = true;
            notice(vm, c->message);
            changed(vm);
        }
    } else if (!strcmp(word, "buffers")) {
        if (buffer_catalog(vm) == 0) view(vm, VIEW_BUFFERS);
    } else if (!strcmp(word, "bnext") || !strcmp(word, "bn")) buffer_cycle(vm, false);
    else if (!strcmp(word, "bprevious") || !strcmp(word, "bp")) buffer_cycle(vm, true);
    else if (!strcmp(word, "sessions")) {
        view(vm, VIEW_SESSIONS);
        refresh(vm);
    }
    else if (!strcmp(word, "workspaces")) {
        refresh(vm);
        view(vm, VIEW_WORKSPACES);
    } else if (!strcmp(word, "help")) view(vm, VIEW_HELP);
    else notice(vm, "Unknown workspace command; use :help");
    detach_unused(vm);
    free(word);
    vm->dirty = true;
}

static json_t *
view_rows(struct vm *vm, const struct vm_window *window)
{
    struct snag_vm_connection *c = connection_for(vm, window->session_id, false);
    if (window->kind == VIEW_REPORTS) return c ? c->reports : NULL;
    if (window->kind == VIEW_BUFFERS) return vm->buffers;
    return window->kind == VIEW_SESSIONS ? vm->sessions : vm->workspaces;
}

static bool
matches(const struct vm_window *window, const json_t *row)
{
    const char *filter = window->filter;
    if (!filter || !*filter) return true;
    const char *id = snag_json_string(row, "id"), *name = snag_json_string(row, "name");
    const char *command = snag_json_string(row, "command");
    const char *session = snag_json_string(row, "session_name");
    const char *identity = snag_json_string(json_object_get(row, "route"), "identity");
    if ((id && strstr(id, filter)) || (name && strstr(name, filter)) ||
        (command && strstr(command, filter)) || (identity && strstr(identity, filter)) ||
        (session && strstr(session, filter))) return true;
    json_t *cells = json_object_get(row, "cells");
    for (size_t i = 0u; i < json_array_size(cells); ++i) {
        const char *cell = json_string_value(json_array_get(cells, i));
        if (cell && strstr(cell, filter)) return true;
    }
    return false;
}

static size_t
row_count(struct vm *vm, const struct vm_window *window)
{
    if (document_view(window)) return snag_vm_document_rows(window->document);
    if (window->kind == VIEW_HELP) return sizeof(help_rows) / sizeof(help_rows[0]);
    size_t count = 0u;
    json_t *rows = view_rows(vm, window);
    for (size_t i = 0u; i < json_array_size(rows); ++i)
        if (matches(window, json_array_get(rows, i))) ++count;
    return count;
}

static json_t *
selected_row(struct vm *vm, const struct vm_window *window)
{
    json_t *rows = view_rows(vm, window);
    size_t visible = 0u;
    for (size_t i = 0u; i < json_array_size(rows); ++i) {
        json_t *row = json_array_get(rows, i);
        if (matches(window, row) && visible++ == window->selected) return row;
    }
    return NULL;
}

static void
selection(struct vm *vm, size_t at)
{
    cancel_search(vm);
    struct vm_window *window = &vm->windows[vm->focus];
    size_t count = row_count(vm, window);
    window->selected = !count ? 0u : at < count ? at : count - 1u;
    if (document_view(window)) {
        if (window->selected + 1u < count || window->end.offset < window->tail.offset)
            window->follow = false;
        remember_anchor(window);
        changed(vm);
        return;
    }
    const char *id = window->kind == VIEW_HELP ? NULL :
        snag_json_string(selected_row(vm, window), "id");
    (void)snprintf(window->selected_id, sizeof(window->selected_id), "%s", id ? id : "");
    vm->dirty = true;
    if (count) changed(vm);
}

static void
move(struct vm *vm, bool down, size_t amount)
{
    struct vm_window *window = &vm->windows[vm->focus];
    size_t at = window->selected, count = row_count(vm, window);
    if (window->kind == VIEW_TRANSCRIPT) {
        if (!down && !at && window->begin.offset) {
            window->follow = false;
            queue_history(vm, window, LOAD_PREVIOUS);
            return;
        }
        if (down && (!count || at + 1u >= count) && window->end.offset < window->tail.offset) {
            queue_history(vm, window, LOAD_NEXT);
            return;
        }
    }
    selection(vm, down ? amount > SIZE_MAX - at ? SIZE_MAX : at + amount :
        at > amount ? at - amount : 0u);
}

static void
focus_direction(struct vm *vm, unsigned int key)
{
    const struct snag_vm_rectangle *from = &vm->windows[vm->focus].rectangle;
    size_t best = vm->focus;
    uint64_t distance = UINT64_MAX;
    for (size_t i = 0u; i < vm->count; ++i) {
        const struct snag_vm_rectangle *to = &vm->windows[i].rectangle;
        bool horizontal = key == 'h' || key == 'l';
        unsigned int from_start = horizontal ? from->column : from->row;
        unsigned int from_size = horizontal ? from->columns : from->rows;
        unsigned int to_start = horizontal ? to->column : to->row;
        unsigned int to_size = horizontal ? to->columns : to->rows;
        unsigned int from_cross = horizontal ? from->row : from->column;
        unsigned int from_extent = horizontal ? from->rows : from->columns;
        unsigned int to_cross = horizontal ? to->row : to->column;
        unsigned int to_extent = horizontal ? to->rows : to->columns;
        bool backward = key == 'h' || key == 'k';
        if (!to->visible || (backward ? to_start + to_size > from_start :
            to_start < from_start + from_size) ||
            to_cross >= from_cross + from_extent || from_cross >= to_cross + to_extent)
            continue;
        uint64_t gap = backward ? from_start - to_start - to_size :
            to_start - from_start - from_size;
        size_t cursor = horizontal ? vm->cursor_row : vm->cursor_column;
        if (cursor < from_cross || cursor >= from_cross + from_extent)
            cursor = from_cross + from_extent / 2u;
        uint64_t cross = cursor < to_cross ? to_cross - cursor :
            cursor >= to_cross + to_extent ? cursor - to_cross - to_extent + 1u : 0u;
        uint64_t score = gap * (vm->grid.rows + vm->grid.columns + 1u) + cross;
        if (score < distance) {
            distance = score;
            best = i;
        }
    }
    if (best != vm->focus) {
        cancel_search(vm);
        vm->focus = best;
        changed(vm);
    }
}

static int
insert_command(struct vm *vm, const void *text, size_t size)
{
    free(vm->completion_prefix);
    vm->completion_prefix = NULL;
    if (snag_buf_reserve(&vm->command, size) < 0) {
        notice(vm, "Command is too long");
        return 0;
    }
    memmove(vm->command.data + vm->command_cursor + size,
        vm->command.data + vm->command_cursor, vm->command.len - vm->command_cursor);
    memcpy(vm->command.data + vm->command_cursor, text, size);
    vm->command.len += size;
    vm->command_cursor += size;
    vm->dirty = true;
    return 0;
}

static int
complete_command(struct vm *vm, bool previous)
{
    static const char *const choices[] = {
        "attach", "b", "bn", "bnext", "bp", "bprevious", "buffer", "buffers",
        "classic", "close", "detach", "draft", "help", "history", "new",
        "q", "q!", "qa", "qa!", "recover", "report", "reports", "session",
        "sessions", "set", "sp", "split", "verbosity", "vsp", "vsplit",
        "workspace", "workspaces", "draft local", "draft owner", "session detach",
        "set ignorecase", "set mouse", "set noignorecase", "set nomouse",
        "verbosity 0", "verbosity 1", "verbosity 2", "verbosity 3", "verbosity 4",
        "verbosity 5", "verbosity 6", "workspace name", "workspace save"
    };
    size_t count = sizeof(choices) / sizeof(*choices);
    bool first = !vm->completion_prefix;
    if (first) {
        vm->completion_prefix = malloc(vm->command_cursor + 1u);
        if (!vm->completion_prefix) return -1;
        if (vm->command_cursor)
            memcpy(vm->completion_prefix, vm->command.data, vm->command_cursor);
        vm->completion_prefix[vm->command_cursor] = '\0';
    }
    size_t length = strlen(vm->completion_prefix);
    bool argument = strchr(vm->completion_prefix, ' ') != NULL;
    size_t index = first ? (previous ? 0u : count - 1u) : vm->completion_index;
    for (size_t i = 0u; i < count; ++i) {
        index = previous ? (index ? index - 1u : count - 1u) : (index + 1u) % count;
        const char *choice = choices[index];
        if (argument != (strchr(choice, ' ') != NULL) ||
            strncmp(choice, vm->completion_prefix, length)) continue;
        size_t size = strlen(choice), at = vm->command_cursor;
        if (size > at && snag_buf_reserve(&vm->command, size - at) < 0) return -1;
        memmove(vm->command.data + size, vm->command.data + at, vm->command.len - at);
        memcpy(vm->command.data, choice, size);
        vm->command.len = vm->command.len - at + size;
        vm->command_cursor = size;
        vm->completion_index = index;
        vm->dirty = true;
        return 0;
    }
    notice(vm, "No command completion matches");
    return 0;
}

static int
edit_draft(struct vm *vm, size_t begin, size_t end, const void *text, size_t size)
{
    struct snag_vm_buffer *c = focused_buffer(vm);
    if (!snag_vm_buffer_writable(c)) return 0;
    if (snag_vm_editor_replace(c, begin, end, text, size) < 0)
        notice(vm, "Cannot edit draft: invalid text or input limit reached");
    else changed(vm);
    return 0;
}

static struct snag_vm_buffer *
command_buffer(struct vm *vm, struct snag_vm_buffer *source, const char *text)
{
    size_t verb = strcspn(text, " \t\r\n");
    const char *commands[] = {"/query", "/msg", "/notice", "/chat", "/join", "/part",
        "/connections"};
    bool addressed = false;
    for (size_t i = 0u; i < sizeof(commands) / sizeof(*commands); ++i)
        if (strlen(commands[i]) == verb && !strncmp(text, commands[i], verb)) addressed = true;
    if (!addressed) return source;
    char *operand = NULL;
    const char *rest;
    char error[256];
    if (snag_irc_address_operand(text + verb, &operand, &rest, error, sizeof(error)) < 0)
        return source;
    bool connection = verb == 12u;
    struct snag_irc_address address;
    int parsed = snag_irc_address_parse(&address, operand,
        connection ? SNAG_IRC_BUFFER_ADDRESS : SNAG_IRC_MESSAGE_ADDRESS, error, sizeof(error));
    bool numbered = connection && !strchr(operand, '/') &&
        strspn(operand, "0123456789") == strlen(operand);
    free(operand);
    if (parsed < 0 || !address.session[0] || numbered) return source;
    struct snag_session location;
    snag_session_init(&location);
    struct snag_vm_buffer *result = NULL;
    if (locate_buffer_session(vm, &location, address.session, error, sizeof(error)) < 0) {
        if (connection && address.kind == SNAG_IRC_TRANSCRIPT) result = source;
        else notice(vm, error);
    } else if (!strcmp(location.id, source->connection->session)) {
        result = source;
    } else {
        struct snag_vm_connection *target = connection_for(vm, location.id, false);
        if (!target || !target->bound || target->detaching || target->quitting) {
            (void)snprintf(error, sizeof(error),
                "Addressed session is read-only; :attach %s in another split before sending",
                location.id);
            notice(vm, error);
        } else if (!target->commands) {
            notice(vm, "Addressed owner needs :classic for slash commands");
        } else result = target->rollout;
    }
    snag_session_close(&location);
    return result;
}

static void
submit_draft(struct vm *vm)
{
    struct snag_vm_buffer *c = focused_buffer(vm);
    if (!c) return;
    if (submission_from(vm, c)) {
        notice(vm, "Previous submission still retained; inspect its receipt");
        return;
    }
    const char *text = c->draft.len ? (const char *)c->draft.data : "";
    if (*text && json_is_object(c->route) && !json_object_get(c->route, "peer") &&
        !json_object_get(c->route, "room") && !snag_prompt_command(text)) {
        notice(vm, "Connection text needs an explicit target; use /query, /chat or /msg");
        return;
    }
    if (!strncmp(text, "/search", 7u) && (!text[7] || isspace((unsigned char)text[7]))) {
        if (c->pending || c->draft_conflict) {
            notice(vm, "Resolve the retained submission or draft conflict first");
            return;
        }
        const char *query = text + 7u;
        while (isspace((unsigned char)*query)) ++query;
        bool entry = !*query;
        if (!entry) search(vm, query, false, true);
        if (!entry && !vm->searching) return;
        if (snag_vm_editor_replace(c, 0u, c->draft.len, NULL, 0u) < 0) {
            notice(vm, "Cannot clear search command draft");
            return;
        }
        vm->composer = vm->insert = false;
        if (entry) {
            vm->mode = '/';
            vm->search_command = true;
            snag_buf_reset(&vm->command);
            vm->command_cursor = 0u;
            notice(vm, "command › /search");
        }
        changed(vm);
        return;
    }
    struct snag_vm_buffer *target = command_buffer(vm, c, text);
    if (!target) return;
    if (target != c && submission_from(vm, target)) {
        notice(vm, "Addressed composer has a retained submission; inspect its receipt");
        return;
    }
    if (snag_vm_buffer_prepare(target, c, vm->windows[vm->focus].id) < 0) {
        notice(vm, errno == ENOTSUP ? !snag_vm_buffer_supported(target) ?
            "This owner does not support this conversation input" :
            "This owner needs :classic for slash commands" :
            target->pending ? "Addressed composer has a retained submission; inspect its receipt" :
            c->draft_conflict ? "Resolve the draft conflict with :draft local or :draft owner" :
            !target->connection->bound ? "Read-only session; :attach to submit" : "Draft is empty");
        return;
    }
    changed(vm);
    if (save(vm, NULL) < 0) return;
    if (snag_vm_buffer_send(target) < 0) notice(vm, "Submission was not sent; :recover its text");
    else notice(vm, target->message);
}

static void
clipboard_pending_clear(struct vm_clipboard *copy)
{
    if (copy->pending_fd >= 0) (void)close(copy->pending_fd);
    copy->pending_fd = -1;
    snag_buf_free(&copy->pending_text);
    copy->pending = false;
}

static bool
clipboard_cancel(struct vm *vm)
{
    struct vm_clipboard *copy = &vm->clipboard;
    bool active = copy->source || copy->pending;
    clipboard_pending_clear(copy);
    if (!copy->source) return active;
    if (copy->send) {
        if (!copy->canceling) snag_clipboard_send_cancel(copy->send, snag_monotonic_ms());
    } else if (!copy->osc.source) (void)snag_clipboard_cancel(copy->source);
    copy->canceling = true;
    return active;
}

static void
clipboard_yank(struct vm *vm)
{
    struct vm_clipboard *copy = &vm->clipboard;
    if (!vm->reg.length || vm->config.terminal_clipboard == SNAG_CLIP_OFF) return;
    (void)clipboard_cancel(vm);
    if (vm->reg.file) {
        if (fflush(vm->reg.file) != 0) goto failed;
        copy->pending_fd = dup(fileno(vm->reg.file));
        if (copy->pending_fd < 0 || snag_fd_cloexec(copy->pending_fd) < 0) goto failed;
    } else {
        copy->pending_text.max = SIZE_MAX;
        if (snag_buf_append(&copy->pending_text, vm->reg.text.data, vm->reg.text.len) < 0)
            goto failed;
    }
    copy->pending_length = vm->reg.length;
    copy->pending = true;
    return;
failed:
    clipboard_pending_clear(copy);
    notice(vm, "Yank retained; cannot prepare clipboard copy");
}

static bool
mention_byte(unsigned char c)
{
    return c >= 0x80u || snag_irc_nick_char(c);
}

static bool
complete_mention(struct vm *vm, struct snag_vm_buffer *buffer, bool previous)
{
    struct snag_irc_conversation_target target;
    if (!json_is_object(buffer->route) ||
        snag_view_conversation_read(buffer->route, &target) < 0) return false;
    struct snag_vm_editor *editor = &buffer->editor;
    const unsigned char *text = buffer->draft.data;
    size_t begin = buffer->cursor, end = buffer->cursor;
    while (begin && mention_byte(text[begin - 1u])) --begin;
    if (!begin || text[begin - 1u] != '@' ||
        (begin > 1u && mention_byte(text[begin - 2u]))) return false;
    while (end < buffer->draft.len && mention_byte(text[end])) ++end;
    bool cycling = editor->completing && editor->completion_begin == begin &&
        editor->completion_end == end && buffer->cursor == end;
    size_t prefix = cycling ? editor->completion_prefix : buffer->cursor - begin;
    enum snag_irc_casemapping mapping;
    json_t *names = snag_irc_completion_names(
        json_object_get(buffer->connection->state, "irc_names"), &target, &mapping);
    json_t *matches = json_array();
    if (!names || !matches) goto failed;
    for (size_t i = 0u; i < json_array_size(names); ++i) {
        const char *name = snag_json_bounded_string(json_array_get(names, i),
            SNAG_CONFIG_IRC_NICK_MAX);
        if (!name) continue;
        size_t j = 0u;
        while (j < prefix && name[j] &&
            snag_irc_name_fold(mapping, (unsigned char)name[j]) ==
            snag_irc_name_fold(mapping, text[begin + j])) ++j;
        if (j == prefix && json_array_append(matches, json_array_get(names, i)) < 0) goto failed;
    }
    size_t count = json_array_size(matches);
    if (!count) {
        editor->completing = false;
        notice(vm, "No nickname matches in this conversation");
        goto done;
    }
    size_t selected = previous ? count - 1u : 0u;
    for (size_t i = 0u; cycling && i < count; ++i) {
        const char *name = json_string_value(json_array_get(matches, i));
        if (strlen(name) == end - begin && !memcmp(text + begin, name, end - begin)) {
            selected = previous ? (i ? i - 1u : count - 1u) : (i + 1u) % count;
            break;
        }
    }
    const char *name = json_string_value(json_array_get(matches, selected));
    if (snag_vm_editor_replace(buffer, begin, end, name, strlen(name)) < 0) goto failed;
    editor->completing = true;
    editor->completion_begin = begin;
    editor->completion_prefix = prefix;
    editor->completion_end = buffer->cursor;
    notice(vm, "Nickname completed; Ctrl-N/Ctrl-P cycles matches");
    goto done;
failed:
    notice(vm, "Cannot complete nickname; draft retained");
done:
    json_decref(names);
    json_decref(matches);
    changed(vm);
    return true;
}

static bool
composer_key(struct vm *vm, const struct snag_vm_input_event *event)
{
    struct snag_vm_buffer *c = focused_buffer(vm);
    if (!vm->composer || !snag_vm_buffer_writable(c)) return false;
    if (vm->insert && event->kind == SNAG_VM_KEY && (event->modifiers & SNAG_VM_CTRL) &&
        (event->key == 'n' || event->key == 'p') && complete_mention(vm, c, event->key == 'p'))
        return true;
    const struct snag_vm_rectangle *r = &vm->windows[vm->focus].rectangle;
    enum snag_vm_edit_result result = snag_vm_editor_key(c, &vm->reg, event,
        vm->insert, r->columns, (r->rows > 1u ? r->rows - 1u : 0u) / 3u + 1u,
        vm->windows[vm->focus].composer_top, vm->windows[vm->focus].prompt);
    if (result == SNAG_VM_EDIT_UNUSED) return false;
    if (result == SNAG_VM_EDIT_YANK) clipboard_yank(vm);
    else if (result == SNAG_VM_EDIT_ERROR) {
        notice(vm, errno == ENOTSUP ? "Unsupported composer command" :
            "Cannot edit draft: invalid text, count or input limit reached");
    } else if (result == SNAG_VM_EDIT_INSERT) {
        vm->insert = true;
        notice(vm, "INSERT  Enter: submit  Ctrl-J: newline  Esc: NORMAL");
    } else if (result == SNAG_VM_EDIT_NORMAL) {
        vm->insert = false;
        notice(vm, "NORMAL composer  Tab: transcript  i: insert");
    } else if (result == SNAG_VM_EDIT_SUBMIT) submit_draft(vm);
    else if (result == SNAG_VM_EDIT_CENTER) vm->windows[vm->focus].center_composer = true;
    changed(vm);
    return true;
}

static struct snag_vm_anchor
history_anchor(const struct vm_window *window)
{
    struct snag_vm_anchor anchor = {.seq = window->anchor_seq, .byte = window->anchor_byte,
        .heading = window->anchor_heading};
    memcpy(anchor.key, window->anchor_key, sizeof(anchor.key));
    struct snag_vm_document_row row;
    if (snag_vm_document_row(window->document, window->selected, &row) == 0)
        anchor.order = snag_vm_search_order(snag_vm_document_block(window->document, row.block));
    return anchor;
}

static const char *
history_field(const struct vm_window *window, struct snag_vm_document_row *row,
    size_t *length, size_t *at)
{
    if (snag_vm_document_row(window->document, window->selected, row) < 0) return NULL;
    const json_t *block = snag_vm_document_block(window->document, row->block);
    const char *text = snag_vm_document_text(window->document, row);
    *length = strlen(text);
    if (row->heading && *length && text[*length - 1u] == '\n') --*length;
    uint64_t byte = row->heading || !window->anchor_source ? window->anchor_byte :
        snag_vm_source_position(block, window->anchor_byte, false);
    *at = byte <= *length ? (size_t)byte : row->begin;
    return text;
}

static void
history_cursor(struct vm *vm, const struct snag_vm_document_row *row, size_t at)
{
    struct vm_window *window = &vm->windows[vm->focus];
    const json_t *block = snag_vm_document_block(window->document, row->block);
    (void)snprintf(window->anchor_key, sizeof(window->anchor_key), "%s",
        snag_json_string(block, "key"));
    window->anchor_seq = (uint64_t)json_integer_value(json_object_get(block, "seq"));
    window->anchor_heading = row->heading;
    window->anchor_source = true;
    window->anchor_byte = snag_vm_document_source(window->document, row, at);
    window->selected = locate_anchor(window);
    window->follow = false;
    changed(vm);
}

static size_t
history_column(const struct vm_window *window, size_t *width)
{
    struct snag_vm_document_row row;
    size_t length, at;
    const char *text = history_field(window, &row, &length, &at);
    if (!text) { *width = 1u; return 0u; }
    size_t column = row.column;
    for (size_t byte = row.begin; byte < at;) {
        struct snag_vm_glyph unit = snag_vm_glyph(text + byte, length - byte, column, false);
        if (unit.bytes > at - byte) break;
        column += unit.columns;
        byte += unit.bytes;
    }
    struct snag_vm_glyph glyph = snag_vm_glyph(text + at, length - at, column, false);
    *width = glyph.columns ? glyph.columns : 1u;
    if (window->visual.kind == SNAG_VM_SELECT_BLOCK && window->navigation.column_valid &&
        !window->navigation.column_display && window->navigation.column != SIZE_MAX &&
        window->navigation.column > column) {
        column = window->navigation.column;
        *width = 1u;
    }
    return column;
}

static void
visual_begin(struct vm *vm, enum snag_vm_selection_kind kind)
{
    cancel_search(vm);
    struct vm_window *window = &vm->windows[vm->focus];
    if (vm->page.window == window->id) cancel_read(&vm->page);
    window->load = LOAD_NONE;
    window->follow = false;
    window->visual_tail = window->tail;
    window->visual.exclusive = false;
    window->visual.first = history_anchor(window);
    window->visual.kind = kind;
    size_t width;
    window->visual_column = history_column(window, &width);
    window->visual_width = width;
    notice(vm, kind == SNAG_VM_SELECT_LINE ? "VISUAL LINE" :
        kind == SNAG_VM_SELECT_BLOCK ? "VISUAL BLOCK" : "VISUAL");
    changed(vm);
}

static void
motion_origin(struct vm *vm, struct vm_window *window)
{
    restore_motion(vm, false);
    vm->motion_origin = (struct vm_motion_origin){
        .document = snag_vm_document_ref(window->document), .anchor = history_anchor(window),
        .window = window->id, .selected = window->selected, .top = window->top,
        .begin = window->begin, .end = window->end, .tail = window->tail,
        .follow = window->follow, .incomplete = window->incomplete,
        .best_effort = window->best_effort};
    window->follow = false;
}

static void
history_navigate(struct vm *vm, enum snag_vm_navigation_kind kind, size_t count)
{
    cancel_search(vm);
    struct vm_window *window = &vm->windows[vm->focus];
    motion_origin(vm, window);
    struct snag_vm_read_request request = {
        .kind = window->kind == VIEW_REPORT ? SNAG_VM_READ_REPORT : SNAG_VM_READ_HISTORY,
        .report = window->report, .route = window->route, .verbosity = window->verbosity,
        .columns = window->rectangle.columns,
        .tail = window->visual.kind ? window->visual_tail : window->tail,
        .navigation = {.kind = kind, .start = history_anchor(window), .count = count,
            .column = window->navigation.column, .operate = window->yank_motion}};
    request.trusted_tail = request.pin_tail = request.tail.next_seq != 0u;
    if (kind != SNAG_VM_NAV_LINE && kind != SNAG_VM_NAV_LAST)
        request.cursor = window->begin;
    memcpy(request.session_id, window->session_id, sizeof(request.session_id));
    vm->scan.generation = reader_request(vm, &vm->scan, &request);
    if (!vm->scan.generation) {
        restore_motion(vm, true);
        notice(vm, "Cannot start navigation; cursor preserved");
        return;
    }
    vm->navigating = true;
    vm->scan.window = window->id;
    vm->scan.load = LOAD_NONE;
    window->load = LOAD_NONE;
    window->yank_after_load = window->yank_motion;
    notice(vm, "Moving through retained history; Ctrl-C cancels");
}

static enum snag_vm_navigation_kind
navigation_kind(unsigned int key)
{
    switch (key) {
    case 'h': case SNAG_VM_KEY_LEFT: return SNAG_VM_NAV_LEFT;
    case 'l': case SNAG_VM_KEY_RIGHT: return SNAG_VM_NAV_RIGHT;
    case 'j': case SNAG_VM_KEY_DOWN: return SNAG_VM_NAV_DOWN;
    case 'k': case SNAG_VM_KEY_UP: return SNAG_VM_NAV_UP;
    case 'w': return SNAG_VM_NAV_WORD_NEXT;
    case 'b': return SNAG_VM_NAV_WORD_PREVIOUS;
    case 'e': return SNAG_VM_NAV_WORD_END;
    case '0': case SNAG_VM_KEY_HOME: return SNAG_VM_NAV_LINE_START;
    case '^': return SNAG_VM_NAV_LINE_FIRST;
    case '$': case SNAG_VM_KEY_END: return SNAG_VM_NAV_LINE_END;
    default: return SNAG_VM_NAV_NONE;
    }
}

static bool
local_motion(const struct vm_window *window, const struct snag_vm_document_row *row,
    const char *text, size_t length, size_t at, size_t target,
    enum snag_vm_navigation_kind kind, size_t count)
{
    const json_t *block = snag_vm_document_block(window->document, row->block);
    bool prefix = !row->heading && json_integer_value(json_object_get(block, "source_begin"));
    /* Logical motions use the unwrapped projection on the scan worker. */
    if (!row->heading && json_object_get(block, "format_map") &&
        (kind == SNAG_VM_NAV_DOWN || kind == SNAG_VM_NAV_UP ||
         kind == SNAG_VM_NAV_LINE_END || kind == SNAG_VM_NAV_LINE_START ||
         kind == SNAG_VM_NAV_LINE_FIRST)) return false;
    if (!row->heading && json_array_size(json_object_get(block, "source_map")) &&
        kind >= SNAG_VM_NAV_LEFT && kind <= SNAG_VM_NAV_WORD_END) return false;
    if (kind == SNAG_VM_NAV_DOWN || kind == SNAG_VM_NAV_UP || kind == SNAG_VM_NAV_LINE_END) {
        if (kind == SNAG_VM_NAV_LINE_END) --count;
        size_t line = snag_vm_text_line_start(text, length, at);
        while (count) {
            if (kind == SNAG_VM_NAV_UP) {
                if (!line) return false;
                line = snag_vm_text_line_start(text, length, line - 1u);
            } else {
                line = snag_vm_text_line_end(text, length, line);
                if (line >= length || line + 1u >= length) return false;
                ++line;
            }
            --count;
        }
        return (!prefix || line) && snag_vm_text_line_end(text, length, line) < length;
    }
    if (kind == SNAG_VM_NAV_LINE_START || kind == SNAG_VM_NAV_LINE_FIRST ||
        kind == SNAG_VM_NAV_LEFT) return target || !prefix;
    if (kind == SNAG_VM_NAV_RIGHT)
        return snag_vm_text_line_end(text, length, target) < length ||
            snag_vm_text_next(text, length, target) < length;
    if (kind == SNAG_VM_NAV_WORD_PREVIOUS) return target > 0u;
    return target < length && snag_vm_text_next(text, length, target) < length;
}

static void
yank(struct vm *vm)
{
    struct vm_window *window = &vm->windows[vm->focus];
    window->visual.last = history_anchor(window);
    size_t width, column = history_column(window, &width);
    window->visual.left = column < window->visual_column ? column : window->visual_column;
    window->visual.right = column > window->visual_column ? column + width :
        window->visual_column + window->visual_width;
    struct snag_vm_read_request request = {
        .kind = window->kind == VIEW_REPORT ? SNAG_VM_READ_REPORT : SNAG_VM_READ_HISTORY,
        .report = window->report, .route = window->route, .selection = window->visual,
        .columns = window->rectangle.columns, .verbosity = window->verbosity,
        .tail = window->visual_tail,
        .trusted_tail = window->visual_tail.next_seq != 0u,
        .pin_tail = window->visual_tail.next_seq != 0u};
    memcpy(request.session_id, window->session_id, sizeof(request.session_id));
    vm->scan.generation = reader_request(vm, &vm->scan, &request);
    if (!vm->scan.generation) { notice(vm, "Cannot start copy; register preserved"); return; }
    vm->copying = true;
    vm->scan.window = window->id;
    vm->scan.load = LOAD_NONE;
    window->load = LOAD_NONE;
    window->yank_motion = window->yank_after_load = false;
    window->navigation.count = window->navigation.operator_count = 0u;
    notice(vm, "Copying selection; Ctrl-C cancels");
}

static bool
history_rows(struct vm *vm, bool down, size_t count)
{
    struct vm_window *window = &vm->windows[vm->focus];
    struct snag_vm_document_row row;
    if (snag_vm_document_row(window->document, window->selected, &row) < 0) return false;
    if (!window->navigation.column_valid || !window->navigation.column_display) {
        size_t width;
        window->navigation.column = history_column(window, &width) - row.column;
    }
    window->navigation.column_valid = window->navigation.column_display = true;
    size_t rows = snag_vm_document_rows(window->document);
    if (count > (down ? rows - window->selected - 1u : window->selected)) {
        history_navigate(vm, down ? SNAG_VM_NAV_ROW_DOWN : SNAG_VM_NAV_ROW_UP, count);
        return true;
    }
    size_t target = down ? window->selected + count : window->selected - count;
    if (snag_vm_document_row(window->document, target, &row) < 0) return false;
    const char *text = snag_vm_document_text(window->document, &row);
    size_t column = window->navigation.column > SIZE_MAX - row.column ? SIZE_MAX :
        row.column + window->navigation.column;
    size_t byte = snag_vm_text_at_column(text, strlen(text), row.begin, column, false);
    if (byte >= row.end) byte = row.end > row.begin ?
        snag_vm_text_previous(text, strlen(text), row.end) : row.begin;
    history_cursor(vm, &row, byte);
    return true;
}

static bool
history_key(struct vm *vm, const struct snag_vm_input_event *event,
    unsigned int key, bool control)
{
    struct vm_window *window = &vm->windows[vm->focus];
    if (!document_view(window) || !window->document || vm->composer || vm->prefix == 'w')
        return false;
    if (key == SNAG_VM_KEY_ESCAPE || (control && key == '[') ||
        (control && key == 'c' && window->visual.kind)) {
        cancel_search(vm);
        window->visual.kind = SNAG_VM_SELECT_NONE;
        window->yank_motion = window->yank_after_load = false;
        window->navigation.count = 0u;
        vm->prefix = 0;
        notice(vm, "NORMAL");
        return true;
    }
    if ((!control && (key == 'v' || key == 'V')) || (control && key == 'v')) {
        enum snag_vm_selection_kind kind = control ? SNAG_VM_SELECT_BLOCK :
            key == 'V' ? SNAG_VM_SELECT_LINE : SNAG_VM_SELECT_CHAR;
        if (window->visual.kind == kind) {
            window->visual.kind = SNAG_VM_SELECT_NONE;
            notice(vm, "NORMAL");
        } else if (window->visual.kind) {
            window->visual.kind = kind;
            notice(vm, kind == SNAG_VM_SELECT_LINE ? "VISUAL LINE" :
                kind == SNAG_VM_SELECT_BLOCK ? "VISUAL BLOCK" : "VISUAL");
        } else visual_begin(vm, kind);
        return true;
    }
    if (!control && (key == 'p' || key == 'P')) {
        if (!vm->reg.length) { notice(vm, "Register is empty"); return true; }
        struct snag_vm_connection *connection = connection_for(vm, window->session_id, true);
        struct snag_vm_buffer *c = snag_vm_buffer_get(connection, window->route, true);
        if (!snag_vm_buffer_writable(c)) {
            notice(vm, "This conversation is read-only");
            return true;
        }
        c->editor.count = window->navigation.count;
        window->navigation.count = 0u;
        if (window->kind == VIEW_REPORT) {
            char id[SNAG_ID_HEX_LEN + 1u];
            memcpy(id, window->session_id, sizeof(id));
            (void)open_history(vm, id);
        }
        vm->composer = true;
        window->visual.kind = SNAG_VM_SELECT_NONE;
        (void)composer_key(vm, event);
        return true;
    }
    if (!control && key >= '0' && key <= '9' && (key != '0' || window->navigation.count)) {
        size_t digit = key - '0';
        if (window->navigation.count > (SIZE_MAX - digit) / 10u)
            notice(vm, "Motion count is too large");
        else window->navigation.count = window->navigation.count * 10u + digit;
        return true;
    }
    size_t count = window->navigation.count ? window->navigation.count : 1u;
    bool counted = window->navigation.count != 0u;
    if (window->yank_motion && window->navigation.operator_count) {
        if (count > SIZE_MAX / window->navigation.operator_count) {
            notice(vm, "Motion count is too large");
            return true;
        }
        count *= window->navigation.operator_count;
    }
    if (!control && key == 'y') {
        if (window->visual.kind) {
            if (window->yank_motion) {
                window->visual.kind = SNAG_VM_SELECT_LINE;
                if (count > 1u) {
                    window->navigation.count = window->navigation.operator_count = 0u;
                    history_navigate(vm, SNAG_VM_NAV_DOWN, count - 1u);
                    return true;
                }
            }
            yank(vm);
        } else {
            visual_begin(vm, SNAG_VM_SELECT_CHAR);
            window->yank_motion = true;
            window->navigation.operator_count = count;
            window->navigation.count = 0u;
            notice(vm, "y: choose a motion or y for the current line");
        }
        return true;
    }
    if (key == 'g' && vm->prefix != 'g') return false;
    if (!control && key == 'z') {
        if (vm->prefix == 'z') {
            size_t half = window->rectangle.rows / 2u;
            window->top = window->selected > half ? window->selected - half : 0u;
            vm->prefix = 0;
            changed(vm);
        } else vm->prefix = 'z';
        return true;
    }
    if ((control && (key == 'u' || key == 'd' || key == 'b' || key == 'f')) ||
        key == SNAG_VM_KEY_PAGE_UP || key == SNAG_VM_KEY_PAGE_DOWN) {
        size_t rows = key == 'u' || key == 'd' ? window->rectangle.rows / 2u :
            window->rectangle.rows;
        (void)history_rows(vm, key == 'd' || key == 'f' || key == SNAG_VM_KEY_PAGE_DOWN,
            count <= SIZE_MAX / (rows ? rows : 1u) ?
            count * rows : SIZE_MAX);
        window->navigation.count = 0u;
        if (window->yank_motion && !vm->navigating) yank(vm);
        return true;
    }
    if (!control && (key == 'G' || (key == 'g' && vm->prefix == 'g'))) {
        window->navigation.count = 0u;
        window->navigation.column_valid = false;
        vm->prefix = 0;
        if (window->yank_motion) window->visual.kind = SNAG_VM_SELECT_LINE;
        if (counted || key == 'g') history_navigate(vm, SNAG_VM_NAV_LINE, count);
        else {
            cancel_search(vm);
            motion_origin(vm, window);
            vm->motion_loading = true;
            window->yank_after_load = window->yank_motion;
            window->follow = window->kind == VIEW_TRANSCRIPT && !window->visual.kind;
            queue_history(vm, window, LOAD_LAST);
        }
        return true;
    }
    if (control) return false;
    if (key == 'H' || key == 'M' || key == 'L') {
        size_t rows = window->rectangle.rows > 1u ? window->rectangle.rows - 1u : 1u;
        size_t offset = count > rows ? rows - 1u : count - 1u;
        selection(vm, window->top + (key == 'H' ? offset :
            key == 'M' ? rows / 2u : rows - offset - 1u));
        window->navigation.count = 0u;
        if (window->yank_motion) {
            window->visual.kind = SNAG_VM_SELECT_LINE;
            yank(vm);
        }
        return true;
    }
    struct snag_vm_document_row row;
    size_t length, at;
    const char *text = history_field(window, &row, &length, &at);
    if (!text) return false;
    bool wrapped = vm->prefix == 'g';
    if (wrapped && (key == 'j' || key == 'k')) {
        (void)history_rows(vm, key == 'j', count);
        vm->prefix = 0;
        window->navigation.count = 0u;
        if (window->yank_motion && !vm->navigating) yank(vm);
        return true;
    }
    enum snag_vm_navigation_kind kind = navigation_kind(key);
    if (kind == SNAG_VM_NAV_NONE) {
        if (key == 'd' || key == 'c' || key == 'x' || key == SNAG_VM_KEY_DELETE) {
            notice(vm, "Transcript is read-only; use the composer to edit a prompt");
            window->navigation.count = 0u;
            return true;
        }
        return false;
    }
    window->navigation.operator = window->yank_motion ? 'y' : 0u;
    struct snag_vm_motion motion = snag_vm_editor_motion(&window->navigation, text,
        length, at, key, count, counted, false, false, window->rectangle.columns,
        window->rectangle.rows, window->top, NULL);
    window->navigation.operator = 0u;
    cancel_search(vm);
    if (window->yank_motion) {
        if (motion.lines) window->visual.kind = SNAG_VM_SELECT_LINE;
        else window->visual.exclusive = !motion.inclusive;
    }
    window->navigation.count = 0u;
    vm->prefix = 0;
    if (!local_motion(window, &row, text, length, at, motion.at, kind, count)) {
        history_navigate(vm, kind, count);
        return true;
    }
    if (!window->yank_motion && (kind == SNAG_VM_NAV_DOWN || kind == SNAG_VM_NAV_UP) &&
        motion.at && (motion.at == length || text[motion.at] == '\n') &&
        text[motion.at - 1u] != '\n')
        motion.at = snag_vm_text_previous(text, length, motion.at);
    history_cursor(vm, &row, motion.at);
    if (window->yank_motion) yank(vm);
    return true;
}

static int
queue_input(struct vm *vm, const struct snag_vm_input_event *event)
{
    size_t charge = event->length ? event->length : 1u;
    if (charge > SNAG_MAX_DIRECT_PROMPT - vm->pending_input_bytes) {
        cancel_search(vm);
        notice(vm, "Pending input exceeds the input limit; operation canceled");
        return 0;
    }
    struct vm_pending_input *pending = malloc(sizeof(*pending) + event->length);
    if (!pending) return -1;
    *pending = (struct vm_pending_input){.event = *event, .charge = charge};
    if (event->length) {
        memcpy(pending->text, event->text, event->length);
        pending->event.text = pending->text;
    }
    if (vm->pending_input_tail) vm->pending_input_tail->next = pending;
    else vm->pending_input = pending;
    vm->pending_input_tail = pending;
    vm->pending_input_bytes += charge;
    return 0;
}

static size_t
mouse_byte(const char *text, size_t begin, size_t end, size_t logical, size_t column,
    bool insert)
{
    size_t at = begin, cells = 0u, previous = begin;
    while (at < end) {
        struct snag_vm_glyph glyph = snag_vm_glyph(text + at, end - at, logical + cells, false);
        if (glyph.newline || cells >= column || glyph.columns > column - cells) break;
        previous = at;
        at += glyph.bytes;
        cells += glyph.columns;
    }
    return at == end && !insert ? previous : at;
}

static void
mouse_composer(struct vm *vm, struct vm_window *window, unsigned int row, unsigned int column)
{
    struct snag_vm_buffer *buffer = focused_buffer(vm);
    if (!buffer) return;
    const char *text = buffer->draft.len ? (const char *)buffer->draft.data : "";
    struct snag_buf frame = {0};
    struct snag_term_composer_layout layout;
    unsigned int columns = window->rectangle.columns;
    if (snag_term_composer_frame(window->prompt, text, buffer->draft.len,
        buffer->cursor, columns, &frame, &layout) < 0) goto done;
    size_t target = window->composer_top + row - window->composer_row;
    size_t at = 0u;
    struct snag_term_prompt_row line = {0};
    for (size_t i = 0u; i <= target && at <= frame.len; ++i) {
        line = snag_term_prompt_row(&frame, at, columns);
        if (i < target) at = line.next;
    }
    size_t hit = mouse_byte((const char *)frame.data, line.start, line.end, 0u,
        column - window->rectangle.column, true);
    size_t source;
    if (snag_term_composer_hit(window->prompt, text, buffer->draft.len, columns,
        hit, &source) < 0) goto done;
    source = snag_vm_text_floor(text, buffer->draft.len, source);
    if (!vm->insert && source == buffer->draft.len && source)
        source = snag_vm_text_previous(text, buffer->draft.len, source);
    snag_vm_draft_cursor(buffer, source);
    buffer->editor.column_valid = false;
    if (!vm->insert) snag_vm_editor_normal(buffer, false);
    notice(vm, vm->insert ? "INSERT  Enter: submit  Ctrl-J: newline  Esc: NORMAL" :
        "NORMAL composer  Tab: transcript  i: insert");
done:
    snag_buf_free(&frame);
}

static void
mouse_cancel_operator(struct vm_window *window)
{
    if (window->yank_motion) window->visual.kind = SNAG_VM_SELECT_NONE;
    window->yank_motion = window->yank_after_load = false;
    window->navigation.count = window->navigation.operator_count = 0u;
    window->navigation.operator = window->navigation.prefix = 0u;
    window->navigation.column_valid = false;
}

static int
mouse_input(struct vm *vm, const struct snag_vm_input_event *event)
{
    unsigned int button = event->button & ~28u;
    bool motion = button == 32u;
    bool wheel = button == 64u || button == 65u;
    bool dragging = vm->mouse_down && (motion || event->release);
    if (!vm->mouse || (!dragging &&
        (event->release || (button != 0u && !wheel)))) {
        if (event->release) vm->mouse_down = false;
        return 0;
    }
    cancel_search(vm);
    if (dragging && vm->mouse_separator.first) {
        int rc = snag_vm_layout_drag(vm->layout, vm->windows[vm->focus].id,
            vm->grid.rows > 1u ? (unsigned int)vm->grid.rows - 1u : 1u,
            (unsigned int)vm->grid.columns, &vm->mouse_separator, event->row, event->column);
        if (event->release || rc < 0) vm->mouse_down = false;
        if (rc > 0) changed(vm);
        return 0;
    }
    if (!dragging) {
        vm->mouse_down = vm->mouse_select = false;
        if (!wheel) mouse_cancel_operator(&vm->windows[vm->focus]);
        if (!wheel && snag_vm_layout_separator(vm->layout, vm->windows[vm->focus].id,
            vm->grid.rows > 1u ? (unsigned int)vm->grid.rows - 1u : 1u,
            (unsigned int)vm->grid.columns, event->row, event->column,
            &vm->mouse_separator) == 1) {
            vm->mouse_down = true;
            return 0;
        }
    }
    for (size_t i = 0u; i < vm->count; ++i) {
        struct vm_window *window = &vm->windows[i];
        const struct snag_vm_rectangle *r = &window->rectangle;
        if (!r->visible) continue;
        if (dragging) {
            if (window->id != vm->mouse_window) continue;
        } else if (event->row < r->row || event->row - r->row >= r->rows ||
            event->column < r->column || event->column - r->column >= r->columns) continue;
        if (wheel) {
            size_t focus = vm->focus;
            vm->focus = i;
            size_t count = row_count(vm, window), height = window->history_rows;
            size_t top = window->top;
            if (button == 65u) {
                size_t last = count > height ? count - height : 0u;
                top = top >= last || last - top < 3u ? last : top + 3u;
            } else top = top > 3u ? top - 3u : 0u;
            if (height && top != window->top) {
                size_t at = window->selected < top ? top : window->selected;
                if (at - top >= height) at = top + height - 1u;
                selection(vm, at);
                window->top = top;
            } else if (height) move(vm, button == 65u, 3u);
            window->follow = false;
            vm->focus = focus;
            changed(vm);
            return 0;
        }
        if (dragging && !vm->mouse_select && event->release) {
            vm->mouse_down = false;
            return 0;
        }
        if (!dragging) {
            struct snag_vm_buffer *old = focused_buffer(vm);
            if (old) {
                snag_vm_editor_normal(old, vm->insert);
                if (snag_vm_editor_end(old) < 0) return -1;
            }
            bool insert = vm->focus == i && vm->composer && vm->insert;
            vm->focus = i;
            vm->composer = vm->insert = false;
            vm->mode = 0;
            vm->prefix = 0;
            changed(vm);
            window->visual.kind = SNAG_VM_SELECT_NONE;
            mouse_cancel_operator(window);
            if (window->composer_rows && event->row >= window->composer_row &&
                event->row - window->composer_row < window->composer_rows) {
                vm->composer = true;
                vm->insert = insert;
                mouse_composer(vm, window, event->row, event->column);
                changed(vm);
                return 0;
            }
            if (event->row - r->row >= window->history_rows) {
                changed(vm);
                return 0;
            }
            vm->mouse_window = window->id;
            vm->mouse_down = true;
        }
        if (!window->history_rows) return 0;
        size_t line = event->row > r->row ? event->row - r->row : 0u;
        if (line >= window->history_rows) line = window->history_rows - 1u;
        size_t index = window->top + line;
        if (document_view(window)) {
            struct snag_vm_document_row row;
            size_t count = row_count(vm, window);
            if (index >= count) index = count ? count - 1u : 0u;
            if (snag_vm_document_row(window->document, index, &row) < 0) return 0;
            if (dragging && !vm->mouse_select) {
                visual_begin(vm, SNAG_VM_SELECT_CHAR);
                vm->mouse_select = true;
            }
            const char *text = snag_vm_document_text(window->document, &row);
            size_t column = event->column > r->column ? event->column - r->column : 0u;
            if (column >= r->columns) column = r->columns - 1u;
            size_t at = mouse_byte(text, row.begin, row.end, row.column, column, false);
            history_cursor(vm, &row, at);
        } else selection(vm, index);
        if (event->release) vm->mouse_down = false;
        changed(vm);
        return 0;
    }
    if (event->release) vm->mouse_down = false;
    return 0;
}

static int
input_event(void *opaque, const struct snag_vm_input_event *event)
{
    struct vm *vm = opaque;
    if (event->kind == SNAG_VM_TERMINAL_REPLY) {
        (void)snag_clipboard_send_input(vm->clipboard.send, event->text, event->length,
            snag_monotonic_ms());
        return 0;
    }
    if (vm->clipboard.settling || vm->quit || vm->detach_exit || vm->detach_suspend ||
        vm->switch_workspace || vm->classic_pending) return 0;
    if (event->kind == SNAG_VM_KEY && event->key == 'c' &&
        (event->modifiers & SNAG_VM_CTRL) && clipboard_cancel(vm)) {
        notice(vm, "Canceling clipboard copy; register retained");
        return 0;
    }
    if (event->kind == SNAG_VM_FOCUS) {
        vm->unfocused = !event->focused;
        if (vm->unfocused) vm->mouse_down = false;
        vm->dirty = true;
        return 0;
    }
    if (event->kind == SNAG_VM_MOUSE) return mouse_input(vm, event);
    vm->mouse_down = false;
    if (vm->copying || vm->navigating || vm->motion_loading) {
        bool control = (event->modifiers & SNAG_VM_CTRL) != 0u;
        bool cancel = event->kind == SNAG_VM_KEY &&
            (event->key == SNAG_VM_KEY_ESCAPE ||
             (control && (event->key == 'c' || event->key == '[' || event->key == 'z' ||
                event->key == 'w')));
        bool compose = vm->motion_loading && !vm->pending_input &&
            event->kind == SNAG_VM_TEXT && event->length == 1u &&
            (event->text[0] == 'i' || event->text[0] == 'a' || event->text[0] == 'A') &&
            snag_vm_buffer_writable(focused_buffer(vm));
        if (cancel || compose) cancel_search(vm);
        else if (event->kind == SNAG_VM_KEY && control && event->key == 'l') {
            resized = 1;
            vm->dirty = true;
            return 0;
        } else return queue_input(vm, event);
        if (control && event->key == 'c' && !vm->windows[vm->focus].visual.kind) return 0;
    }
    struct vm_window *window = &vm->windows[vm->focus];
    if (event->kind == SNAG_VM_PASTE_BEGIN) {
        snag_buf_reset(&vm->paste);
        vm->paste_failed = false;
        return 0;
    }
    if (event->kind == SNAG_VM_PASTE_TEXT) {
        if (!vm->paste_failed && snag_buf_append(&vm->paste, event->text, event->length) < 0)
            vm->paste_failed = true;
        return 0;
    }
    if (event->kind == SNAG_VM_PASTE_END) {
        if (vm->paste_failed) {
            notice(vm, "Paste exceeds the input limit");
            return 0;
        }
        if (vm->mode && snag_utf8_valid(vm->paste.data, vm->paste.len, true))
            return insert_command(vm, vm->paste.data, vm->paste.len);
        struct snag_vm_buffer *c = focused_buffer(vm);
        if (vm->composer && vm->insert && snag_vm_buffer_writable(c)) {
            if (snag_vm_editor_end(c) < 0) return -1;
            (void)edit_draft(vm, c->cursor, c->cursor, vm->paste.data, vm->paste.len);
            return snag_vm_editor_end(c);
        }
        notice(vm, "Enter INSERT, command or filter input before pasting text");
        return 0;
    }
    unsigned int key = event->kind == SNAG_VM_TEXT && event->length == 1u ?
        event->text[0] : event->kind == SNAG_VM_KEY ? event->key : 0u;
    bool control = (event->modifiers & SNAG_VM_CTRL) != 0u;
    /* Terminals encode Alt-text as Escape followed by text. Vim's supported
     * editing subset uses that sequence to leave INSERT/command input first,
     * so a fast Escape-colon cannot become literal prompt text. */
    if (event->kind == SNAG_VM_TEXT && (event->modifiers & SNAG_VM_ALT)) {
        struct snag_vm_buffer *c = focused_buffer(vm);
        if (vm->insert && c) {
            snag_vm_editor_normal(c, true);
            if (snag_vm_editor_end(c) < 0) return -1;
        }
        vm->mode = 0;
        vm->insert = false;
        vm->prefix = 0;
        vm->dirty = true;
    }
    if (vm->mode) {
        if (control && key == 'l') {
            resized = 1;
            vm->dirty = true;
            return 0;
        }
        if (vm->mode == ':' && key == SNAG_VM_KEY_TAB)
            return complete_command(vm, (event->modifiers & SNAG_VM_SHIFT) != 0u);
        free(vm->completion_prefix);
        vm->completion_prefix = NULL;
        const char *bytes = (const char *)vm->command.data;
        size_t at = vm->command_cursor, end = at;
        if (event->kind == SNAG_VM_TEXT) return insert_command(vm, event->text, event->length);
        if (key == SNAG_VM_KEY_ESCAPE || (control && key == 'c')) vm->mode = 0;
        else if (key == SNAG_VM_KEY_ENTER) {
            if (snag_buf_putc(&vm->command, 0u) < 0) return -1;
            --vm->command.len;
            char mode = vm->mode;
            vm->mode = 0;
            if (mode == ':') command(vm, (const char *)vm->command.data);
            else if (document_view(window)) {
                search(vm, (const char *)vm->command.data, mode == '?', vm->search_command);
            } else {
                char *filter = snag_strdup_checked((const char *)vm->command.data,
                    SNAG_MAX_DIRECT_PROMPT);
                if (!filter) return -1;
                free(window->filter);
                window->filter = filter;
                window->selected = window->top = 0u;
                window->selected_id[0] = 0;
                changed(vm);
            }
        } else if (key == SNAG_VM_KEY_LEFT) at = snag_vm_text_previous(bytes, vm->command.len, at);
        else if (key == SNAG_VM_KEY_RIGHT) at = snag_vm_text_next(bytes, vm->command.len, at);
        else if (key == SNAG_VM_KEY_HOME) at = 0u;
        else if (control && key == 'u') {
            memmove(vm->command.data, vm->command.data + at, vm->command.len - at);
            vm->command.len -= at;
            at = 0u;
        }
        else if (key == SNAG_VM_KEY_END) at = vm->command.len;
        else if (key == SNAG_VM_KEY_BACKSPACE) {
            at = snag_vm_text_previous(bytes, vm->command.len, at);
            memmove(vm->command.data + at, vm->command.data + end, vm->command.len - end);
            vm->command.len -= end - at;
        } else if (key == SNAG_VM_KEY_DELETE) {
            end = snag_vm_text_next(bytes, vm->command.len, at);
            memmove(vm->command.data + at, vm->command.data + end, vm->command.len - end);
            vm->command.len -= end - at;
        }
        if (key != SNAG_VM_KEY_ENTER) vm->command_cursor = at;
        vm->dirty = true;
        return 0;
    }
    if (control && key == 'c' && (vm->searching || vm->copying)) {
        cancel_search(vm);
        return 0;
    }
    if (!vm->insert && control && key == 'w' && vm->prefix != 'w') {
        cancel_search(vm);
        vm->prefix = 'w';
        return 0;
    }
    if (vm->prefix != 'w' && composer_key(vm, event)) return 0;
    if (history_key(vm, event, key, control)) return 0;
    if (key == SNAG_VM_KEY_ESCAPE) vm->prefix = 0;
    else if (vm->prefix == 'w') {
        if (key == SNAG_VM_KEY_LEFT || key == SNAG_VM_KEY_BACKSPACE) key = 'h';
        else if (key == SNAG_VM_KEY_DOWN) key = 'j';
        else if (key == SNAG_VM_KEY_UP) key = 'k';
        else if (key == SNAG_VM_KEY_RIGHT) key = 'l';
        vm->prefix = 0;
        if (key == 's' || key == 'v') split(vm, key == 'v' ? SNAG_VM_VERTICAL : SNAG_VM_HORIZONTAL);
        else if (key == 'q' || key == 'c') close_window(vm);
        else if (key == 'w') {
            cancel_search(vm);
            vm->focus = (vm->focus + 1u) % vm->count;
            vm->composer = vm->insert = false;
            changed(vm);
        }
        else if (key == 'h' || key == 'j' || key == 'k' || key == 'l') focus_direction(vm, key);
        else if (key == '=') {
            snag_vm_layout_equalize(vm->layout);
            changed(vm);
        }
        else if (key == '+' || key == '-' || key == '>' || key == '<') {
            (void)snag_vm_layout_resize(vm->layout, window->id,
                key == '>' || key == '<' ? SNAG_VM_VERTICAL : SNAG_VM_HORIZONTAL,
                key == '+' || key == '>' ? 5 : -5);
            changed(vm);
        }
    }
    else if (control && key == 'z') {
        if (!snag_term_can_suspend()) {
            notice(vm, "Suspension is unavailable on this host");
            return 0;
        }
        cancel_search(vm);
        vm->detach_suspend = true;
        for (struct snag_vm_connection *c = vm->connections; c; c = c->next)
            snag_vm_connection_detach(c);
        vm->dirty = true;
    }
    else if (control && key == 'l') {
        resized = 1;
        vm->dirty = true;
    }
    else if (control && key == 'w') vm->prefix = 'w';
    else if (control && key == 'c') {
        if (vm->searching) cancel_search(vm);
        else {
            struct snag_vm_connection *c = focused_connection(vm);
            if (c && snag_vm_connection_control(c, "cancel") == 0)
                notice(vm, "Interrupt requested");
        }
    }
    else if (key == ':' || key == '/' || (key == '?' && document_view(window))) {
        cancel_search(vm);
        vm->mode = (char)key;
        free(vm->completion_prefix);
        vm->completion_prefix = NULL;
        vm->search_command = false;
        vm->prefix = 0;
        snag_buf_reset(&vm->command);
        vm->command_cursor = 0u;
        vm->dirty = true;
    } else if (key == 'g') {
        if (vm->prefix == 'g') {
            if (window->kind == VIEW_TRANSCRIPT ||
                (window->kind == VIEW_REPORT && !window->document)) {
                window->follow = false;
                queue_history(vm, window, LOAD_FIRST);
            } else selection(vm, 0u);
            vm->prefix = 0;
        }
        else vm->prefix = 'g';
    } else {
        vm->prefix = 0;
        if (key == SNAG_VM_KEY_TAB && window->kind == VIEW_TRANSCRIPT) {
            vm->composer = !vm->composer;
            (void)connection_for(vm, window->session_id, true);
            vm->insert = false;
            changed(vm);
        } else if ((key == 'i' || key == 'a' || key == 'A') && window->kind == VIEW_TRANSCRIPT) {
            struct snag_vm_connection *c = connection_for(vm, window->session_id, true);
            if (c) {
                vm->composer = true;
                (void)composer_key(vm, event);
            }
        } else if ((key == 'n' || key == 'N') && document_view(window)) {
            search(vm, NULL, key == 'n' ? vm->search_reverse : !vm->search_reverse, false);
        } else if (key == 'G') {
            if (window->kind == VIEW_TRANSCRIPT ||
                (window->kind == VIEW_REPORT && !window->document)) {
                window->follow = window->kind == VIEW_TRANSCRIPT && !window->visual.kind;
                queue_history(vm, window, LOAD_LAST);
            } else selection(vm, SIZE_MAX);
        }
        else if (key == 'j' || key == SNAG_VM_KEY_DOWN) move(vm, true, 1u);
        else if (key == 'k' || key == SNAG_VM_KEY_UP) move(vm, false, 1u);
        else if (key == SNAG_VM_KEY_PAGE_DOWN || (control && key == 'd'))
            move(vm, true, window->rectangle.rows / 2u + 1u);
        else if (key == SNAG_VM_KEY_PAGE_UP || (control && key == 'u'))
            move(vm, false, window->rectangle.rows / 2u + 1u);
        else if (key == 'R') {
            if (document_view(window)) queue_history(vm, window, LOAD_REFRESH);
            else if (window->kind == VIEW_REPORTS) window->report_catalog = true;
            else refresh(vm);
        }
        else if (key == 'o' && window->kind == VIEW_SESSIONS) {
            const char *id = snag_json_string(selected_row(vm, window), "id");
            if (id) open_history(vm, id);
        }
        else if (key == SNAG_VM_KEY_ENTER) {
            json_t *row = selected_row(vm, window);
            const char *id = snag_json_string(row, "id");
            if (window->kind == VIEW_BUFFERS) open_buffer_row(vm, row);
            else if (window->kind == VIEW_REPORTS)
                open_report(vm, focused_connection(vm), row);
            else if (id && window->kind == VIEW_WORKSPACES) (void)restore_workspace(vm, id);
            else if (id && window->kind == VIEW_SESSIONS)
                session_request(vm, id);
        }
    }
    return 0;
}

static void
drain_input(struct vm *vm)
{
    while (vm->pending_input && !vm->copying && !vm->navigating && !vm->motion_loading) {
        vm->input_drained = true;
        struct vm_pending_input *pending = vm->pending_input;
        vm->pending_input = pending->next;
        if (!vm->pending_input) vm->pending_input_tail = NULL;
        vm->pending_input_bytes -= pending->charge;
        int rc = input_event(vm, &pending->event);
        free(pending);
        if (rc < 0) {
            notice(vm, "Cannot process pending input");
            vm->quit = true;
            clear_pending_input(vm);
        }
    }
}

static int
input_ready(struct vm *vm, int timeout)
{
    uint64_t now = snag_monotonic_ms();
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next)
        timeout = snag_vm_connection_wait(c, now, timeout);
    if (direct_session(vm) && (timeout < 0 || timeout > 16)) timeout = 16;
    bool buffered = vm->clipboard.packet_at < vm->clipboard.packet_length;
    int copy_wait = buffered ? -1 :
        snag_clipboard_send_wait(vm->clipboard.send, snag_monotonic_ms());
    if (copy_wait >= 0 && (timeout < 0 || timeout > copy_wait)) timeout = copy_wait;
    if (!buffered && ((vm->clipboard.pending && !vm->clipboard.source) ||
        vm->clipboard.osc.source)) timeout = 0;
#ifdef _WIN32
    /* The console wait accepts one worker fd; poll only while the other worker
     * has a scan in flight. Idle workspaces keep their normal blocking wait. */
    if ((vm->clipboard.source || vm->scan.generation) && (timeout < 0 || timeout > 20))
        timeout = 20;
    int ready = snag_term_input_wait(&vm->terminal, snag_vm_reader_fd(vm->page.reader), timeout);
#else
    size_t count = 5u;
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next) {
        if (c->channel.fd >= 0) ++count;
    }
    for (struct vm_launch *job = vm->launches; job; job = job->next) {
        if (job->fd >= 0) ++count;
        else if (job->child && (timeout < 0 || timeout > 100)) timeout = 100;
    }
    struct pollfd *fds = calloc(count, sizeof(*fds));
    if (!fds) return -1;
    fds[0] = (struct pollfd){.fd = STDIN_FILENO,
        .events = vm->classic_pending ? 0 : POLLIN};
    fds[1] = (struct pollfd){.fd = snag_vm_reader_fd(vm->page.reader), .events = POLLIN};
    fds[2] = (struct pollfd){.fd = snag_clipboard_fd(vm->clipboard.source), .events = POLLIN};
    fds[3] = (struct pollfd){.fd = buffered ? vm->clipboard.output : -1, .events = POLLOUT};
    fds[4] = (struct pollfd){.fd = snag_vm_reader_fd(vm->scan.reader), .events = POLLIN};
    size_t at = 5u;
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next) {
        if (c->channel.fd < 0) continue;
        fds[at++] = (struct pollfd){.fd = c->channel.fd,
            .events = POLLIN | (c->channel.output ? POLLOUT : 0)};
    }
    for (struct vm_launch *job = vm->launches; job; job = job->next)
        if (job->fd >= 0) fds[at++] = (struct pollfd){.fd = job->fd, .events = POLLIN};
    int ready = poll(fds, (nfds_t)count, timeout);
    if (ready >= 0) ready = (fds[0].revents & POLLIN ? SNAG_TERM_WAIT_INPUT : 0) |
        (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL) ? SNAG_TERM_WAIT_END : 0);
    free(fds);
#endif /* _WIN32 */
    if (ready < 0) return errno == EINTR ? 0 : -1;
    if (ready & SNAG_TERM_WAIT_END) {
        vm->quit = true;
        return 0;
    }
    if (ready > 0 && (ready & SNAG_TERM_WAIT_INPUT)) {
        unsigned char bytes[8192];
        ssize_t count = snag_term_input_read(&vm->terminal, bytes, sizeof(bytes));
        if (count < 0 && errno != EAGAIN && errno != EINTR) return -1;
        uint64_t now = snag_monotonic_ms();
        for (ssize_t i = 0; i < count; ++i) {
            if (snag_vm_input_feed(&vm->input, bytes + i, 1u, now, input_event, vm) < 0)
                return -1;
            if (vm->classic_pending) {
                /* The decoder has consumed the command's Enter. The remainder
                 * belongs to the terminal owner, including escape/paste bytes. */
                if (snag_buf_append(&vm->classic.bytes, bytes + i + 1,
                    (size_t)(count - i - 1)) < 0) return -1;
                changed(vm);
                break;
            }
        }
    }
    if (vm->classic_pending) return 0;
    return snag_vm_input_expire(&vm->input, snag_monotonic_ms(), input_event, vm);
}

static void
connections_step(struct vm *vm)
{
    bool waiting = false, connected = false;
    bool catalog = false;
    for (size_t i = 0u; i < vm->count; ++i)
        if (vm->windows[i].kind == VIEW_BUFFERS) catalog = true;
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next) {
        struct snag_vm_buffer *focused = focused_buffer(vm);
        struct snag_vm_buffer *feedback = submission_from(vm, focused);
        if (!feedback) feedback = focused;
        char buffer_previous[256] = "";
        if (feedback && feedback->connection == c)
            memcpy(buffer_previous, feedback->message, sizeof(buffer_previous));
        uint64_t revision = c->revision;
        char previous[sizeof(c->message)];
        memcpy(previous, c->message, sizeof(previous));
        snag_vm_connection_step(c);
        if (revision != c->revision) {
            if (focused_connection(vm) == c && strcmp(previous, c->message))
                notice(vm, c->message);
            if (feedback && feedback->connection == c &&
                strcmp(buffer_previous, feedback->message)) notice(vm, feedback->message);
            changed(vm);
        }
        for (struct snag_vm_buffer *b = c->buffers; b; b = b->next) {
            struct vm_window *window = &vm->windows[vm->focus];
            struct snag_vm_buffer *source = submission_source(vm, b);
            bool origin = source && focused_buffer(vm) == source &&
                (!b->request_window || b->request_window == window->id) &&
                window->kind == VIEW_TRANSCRIPT && !window->visual.kind &&
                !vm->copying && !vm->navigating && !vm->mode &&
                !vm->classic_pending && !vm->detach_exit && !vm->switch_workspace &&
                !vm->detach_suspend && !vm->quit_all && !vm->quit_window;
            if (b->selection) {
                struct snag_vm_buffer *selected = snag_vm_buffer_get(c, b->selection, true);
                if (selected && origin && !source->draft.len) open_conversation(vm, selected);
            } else if (b->report_open && origin && !source->draft.len)
                open_report(vm, c, b->report_open);
            json_decref(b->selection);
            b->selection = NULL;
            json_decref(b->report_open);
            b->report_open = NULL;
            if (!b->pending) {
                json_decref(b->origin);
                b->origin = NULL;
            }
            if (b == c->rollout && b->terminal_result && b->terminal_auto && origin) {
                b->terminal_auto = false;
                classic_request(vm, c->session);
            }
        }
        if (catalog && revision != c->revision && buffer_catalog(vm) < 0)
            notice(vm, "Cannot update buffer list");
        if (c->quitting && !c->exited && c->control) waiting = true;
        if (snag_view_channel_opened(&c->channel)) connected = true;
    }
    if (vm->detach_exit && !connected) {
        vm->detach_exit = false;
        if (save(vm, NULL) == 0) vm->quit = true;
    }
    if (vm->switch_workspace && !connected) {
        struct snag_vm_workspace *next = vm->switch_workspace;
        vm->switch_workspace = NULL;
        (void)install_workspace(vm, next);
    }
    if (vm->detach_suspend && !connected) {
        vm->detach_suspend = false;
        vm->suspend = true;
    }
    if (vm->classic_pending && !connected) vm->classic_ready = true;
    if (waiting || (direct_session(vm) && (vm->quit_all || vm->quit_window))) return;
    if (vm->quit_all) {
        vm->quit_all = false;
        if (save(vm, NULL) == 0) vm->quit = true;
    } else if (vm->quit_window) {
        uint64_t id = vm->quit_window;
        vm->quit_window = 0u;
        for (size_t i = 0u; i < vm->count; ++i) {
            if (vm->windows[i].id != id) continue;
            vm->focus = i;
            close_window(vm);
            break;
        }
    }
}

static int
output_checkpoint(void *opaque)
{
    struct vm *vm = opaque;
    if (stopped || vm->quit) return snag_errno(ECANCELED);
    return vm->entering ? 0 : input_ready(vm, 0);
}

static int
emit(void *opaque, const void *text, size_t length)
{
    struct vm *vm = opaque;
    return snag_term_output_write(&vm->terminal, vm->output, text, length, true,
        output_checkpoint, vm);
}

static int
mouse_reporting(struct vm *vm)
{
    if (vm->mouse_reported == vm->mouse) return 0;
    vm->mouse_reported = vm->mouse;
    const char *modes = vm->mouse ? "\033[?1000l\033[?1002h\033[?1006h" :
        "\033[?1002l\033[?1000l\033[?1006l";
    return emit(vm, modes, strlen(modes));
}

static int
window_prompt(struct vm *vm, struct vm_window *window, struct snag_vm_connection *connection)
{
    const json_t *state = connection ? connection->state : NULL;
    const json_t *prompt = json_object_get(state, "prompt");
    const json_t *fields = json_object_get(prompt, "values");
    const json_t *frames = json_object_get(prompt, "frames");
    const char *source = snag_json_string(prompt, "template");
    bool described = source && json_array_size(fields) == SNAG_PROMPT_FIELD_COUNT &&
        json_array_size(frames) == SNAG_TERM_SPINNER_COUNT;
    bool active = json_is_true(json_object_get(described ? prompt : state, "active"));
    unsigned int mode = window->route ? 0u : active ? 2u : 1u;
    const char *values[SNAG_PROMPT_FIELD_COUNT] = {0};
    const char *spinners[SNAG_TERM_SPINNER_COUNT] = {vm->config.prompt_spinner_goal,
        vm->config.prompt_spinner_provider, vm->config.prompt_spinner_tool};
    unsigned int states = 0u, rate = vm->config.prompt_spinner_per_second;
    char hour[12], minute[12], second[12], hostname[256];
    if (described) {
        for (size_t i = 0u; i < SNAG_PROMPT_FIELD_COUNT; ++i) {
            values[i] = json_string_value(json_array_get(fields, i));
            if (!values[i]) return snag_errno(EPROTO);
        }
        for (size_t i = 0u; i < SNAG_TERM_SPINNER_COUNT; ++i)
            spinners[i] = json_string_value(json_array_get(frames, i));
        json_int_t supplied_rate = json_integer_value(json_object_get(prompt, "rate"));
        json_int_t supplied_states = json_integer_value(json_object_get(prompt, "states"));
        if (supplied_rate < 1 || supplied_rate > 60 || supplied_states < 0 ||
            supplied_states >= (1 << SNAG_TERM_SPINNER_COUNT)) return snag_errno(EPROTO);
        rate = (unsigned int)supplied_rate;
        states = (unsigned int)supplied_states;
    } else {
        source = vm->config.prompt;
        values[SNAG_PROMPT_PROVIDER] = snag_json_string(state, "provider");
        values[SNAG_PROMPT_MODEL] = snag_json_string(state, "model");
        values[SNAG_PROMPT_EFFORT] = snag_json_string(state, "effort");
        if (values[SNAG_PROMPT_EFFORT] && !strcmp(values[SNAG_PROMPT_EFFORT], "default"))
            values[SNAG_PROMPT_EFFORT] = "medium";
        values[SNAG_PROMPT_OPERATOR] = vm->config.irc.operator_nick;
        values[SNAG_PROMPT_MODEL_NICK] = vm->config.irc.model_nick;
        values[SNAG_PROMPT_SESSION_NAME] = snag_json_string(state, "name");
        values[SNAG_PROMPT_CONTEXT] = "?";
        values[SNAG_PROMPT_QUEUE] = "?";
        if (snag_hostname(hostname, sizeof(hostname)) < 0) memcpy(hostname, "localhost", 10u);
        values[SNAG_PROMPT_HOST] = hostname;
        struct snag_term clock = {.prompt_clock = window->prompt_clock};
        snag_term_capture_prompt_clock(&clock, time(NULL));
        window->prompt_clock = clock.prompt_clock;
        (void)snprintf(hour, sizeof(hour), "%d", clock.prompt_clock.hour);
        (void)snprintf(minute, sizeof(minute), "%d", clock.prompt_clock.minute);
        (void)snprintf(second, sizeof(second), "%d", clock.prompt_clock.second);
        values[SNAG_PROMPT_HOUR] = hour;
        values[SNAG_PROMPT_MINUTE] = minute;
        values[SNAG_PROMPT_SECOND] = second;
        for (size_t i = 0u; i < SNAG_PROMPT_FIELD_COUNT; ++i)
            if (!values[i]) values[i] = "?";
    }
    values[SNAG_PROMPT_MODE] = mode == 0u ? "chat" : mode == 1u ?
        "rollout-idle" : "rollout-active";
    char label[SNAG_TERM_LABEL_BYTES];
    uint64_t now = snag_monotonic_ms(), epoch = 0u, tool_until = 0u;
    if (described) {
        (void)snag_json_integer_u64(prompt, "epoch", &epoch);
        (void)snag_json_integer_u64(prompt, "tool_until", &tool_until);
    }
    if (tool_until > now) states |= 1u << SNAG_TERM_SPINNER_TOOL;
    uint64_t elapsed = now > epoch ? now - epoch : 0u;
    bool animated = false;
    if (snag_config_prompt_expand(source, mode, values, SNAG_TERM_SPINNER_MARKER_BASE,
        label, sizeof(label)) < 0 || snag_term_prompt_render(label, spinners, states,
            elapsed / 1000u * rate + elapsed % 1000u * rate / 1000u,
            window->prompt, &animated) < 0) return -1;
    uint64_t due = animated ? now + (1000u + rate - 1u) / rate : 0u;
    if (tool_until > now && (!due || tool_until < due)) due = tool_until;
    if (due && (!vm->prompt_due || due < vm->prompt_due)) vm->prompt_due = due;
    return 0;
}

static int
draw_composer(struct vm *vm, struct vm_window *window, struct snag_vm_buffer *buffer,
    size_t *height, bool cursor_visible)
{
    const struct snag_vm_rectangle *r = &window->rectangle;
    if (window_prompt(vm, window, buffer ? buffer->connection : NULL) < 0) return -1;
    const char *text = buffer && buffer->draft.len ? (const char *)buffer->draft.data : "";
    size_t length = buffer ? buffer->draft.len : 0u;
    size_t cursor = buffer ? buffer->cursor : 0u;
    struct snag_buf frame = {0};
    struct snag_term_composer_layout layout;
    if (snag_term_composer_frame(window->prompt, text, length, cursor, r->columns,
        &frame, &layout) < 0) { snag_buf_free(&frame); return -1; }
    size_t count = layout.end_row + 1u;
    size_t rows = *height / 3u + 1u;
    if (rows > count) rows = count;
    if (rows >= *height) rows = *height > 1u ? *height - 1u : *height;
    size_t top = window->composer_top;
    if (window->center_composer) {
        top = layout.cursor_row > rows / 2u ? layout.cursor_row - rows / 2u : 0u;
        window->center_composer = false;
    }
    if (top > count - rows) top = count - rows;
    if (top > layout.cursor_row) top = layout.cursor_row;
    if (layout.cursor_row - top >= rows) top = layout.cursor_row - rows + 1u;
    window->composer_top = top;
    *height -= rows;
    window->composer_row = r->row + *height;
    window->composer_rows = rows;
    int rc = 0;
    for (size_t i = 0u, at = 0u; i < top + rows && at <= frame.len; ++i) {
        struct snag_term_prompt_row row = snag_term_prompt_row(&frame, at, r->columns);
        if (i >= top) {
            size_t y = window->composer_row + i - top;
            rc = snag_vm_grid_text(&vm->grid, y, r->column, r->columns,
                (const char *)frame.data + at, row.end - at, 0u);
            size_t label_end = row.end < layout.label ? row.end : layout.label;
            size_t colored = at < label_end ?
                snag_term_text_width((const char *)frame.data + at, label_end - at) : 0u;
            for (size_t x = 0u; x < colored && x < r->columns; ++x)
                vm->grid.back.cells[y * vm->grid.columns + r->column + x].style =
                    SNAG_VM_BOLD | SNAG_VM_CYAN;
            if (rc < 0) break;
        }
        at = row.next;
    }
    if (cursor_visible) {
        vm->cursor_row = window->composer_row + layout.cursor_row - top;
        vm->cursor_column = r->column + layout.cursor_column;
    }
    snag_buf_free(&frame);
    return rc;
}

static void
highlight_selection(struct vm *vm, const struct vm_window *window,
    const struct snag_vm_document_row *row, size_t screen_row)
{
    if (!window->visual.kind) return;
    struct snag_vm_anchor first = window->visual.first, last = history_anchor(window);
    if (snag_vm_anchor_compare(&first, &last) > 0) {
        struct snag_vm_anchor swap = first;
        first = last;
        last = swap;
    }
    const json_t *block = snag_vm_document_block(window->document, row->block);
    const char *text = snag_vm_document_text(window->document, row);
    size_t length = strlen(text);
    size_t current_width, current_column = history_column(window, &current_width);
    size_t left = current_column < window->visual_column ? current_column : window->visual_column;
    size_t right = current_column > window->visual_column ? current_column + current_width :
        window->visual_column + window->visual_width;
    if (window->visual.kind != SNAG_VM_SELECT_CHAR) {
        struct snag_vm_anchor *ends[] = {&first, &last};
        for (size_t i = 0u; i < 2u; ++i) {
            struct snag_vm_anchor *anchor = ends[i];
            if (anchor->heading != row->heading ||
                strcmp(anchor->key, snag_json_string(block, "key"))) continue;
            uint64_t byte = row->heading ? anchor->byte :
                snag_vm_source_position(block, anchor->byte, false);
            if (byte > length) continue;
            size_t at = i ? snag_vm_text_line_end(text, length, (size_t)byte) :
                snag_vm_text_line_start(text, length, (size_t)byte);
            anchor->byte = snag_vm_document_source(window->document, row, at);
        }
    }
    size_t column = row->column;
    for (size_t at = row->begin; at < row->end;) {
        struct snag_vm_glyph glyph = snag_vm_glyph(text + at, row->end - at, column, false);
        struct snag_vm_anchor point = {.seq =
            (uint64_t)json_integer_value(json_object_get(block, "seq")),
            .byte = snag_vm_document_source(window->document, row, at),
            .heading = row->heading, .order = snag_vm_search_order(block)};
        (void)snprintf(point.key, sizeof(point.key), "%s", snag_json_string(block, "key"));
        bool selected = snag_vm_anchor_compare(&point, &first) >= 0 &&
            snag_vm_anchor_compare(&point, &last) <= 0;
        if (window->visual.kind == SNAG_VM_SELECT_BLOCK)
            selected = selected && column < right && column + glyph.columns > left;
        if (selected) {
            size_t x = window->rectangle.column + column - row->column;
            size_t end = x + glyph.columns;
            size_t limit = window->rectangle.column + window->rectangle.columns;
            if (end > limit) end = limit;
            for (; x < end; ++x)
                vm->grid.back.cells[screen_row * vm->grid.columns + x].style |= SNAG_VM_REVERSE;
        }
        column += glyph.columns;
        at += glyph.bytes;
    }
}

static int
buffer_row(struct snag_buf *text, const json_t *row)
{
    const json_t *route = json_object_get(row, "route");
    int rank = buffer_rank(route);
    const char *identity = snag_json_string(route, "identity");
    const char *label = snag_json_string(row, "label");
    if (snag_buf_printf(text, "%.8s %s%s%s [%s]", snag_json_string(row, "id"),
        rank < 0 ? "" : rank ? "    /" : "  ", label, rank == 0 ? "/" : "",
        identity ? identity : "rollout") < 0) return -1;
    if (rank >= 0) {
        if (!json_is_true(json_object_get(row, "activity_exact"))) {
            if (snag_buf_printf(text, " [? unread]") < 0) return -1;
        } else if (snag_buf_printf(text, " [%llu unread]", (unsigned long long)
            json_integer_value(json_object_get(row, "unread"))) < 0) return -1;
    } else if (snag_buf_printf(text, " [%s]", snag_json_string(row, "owner")) < 0) return -1;
    const char *flags[] = {"draft", "pending", "stale"};
    for (size_t i = 0u; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        if (json_is_true(json_object_get(row, flags[i])) &&
            snag_buf_printf(text, " [%s]", i == 2u ? "old route" : flags[i]) < 0) return -1;
    }
    const json_t *connected = json_object_get(row, "connected");
    if (json_is_object(connected)) connected = json_object_get(connected, identity);
    if (rank >= 0 && snag_buf_printf(text, " [%s]", json_is_true(connected) ?
        json_is_false(json_object_get(row, "joined")) ? "left" : "connected" :
        json_is_false(connected) ? "disconnected" : "unknown") < 0) return -1;
    if (rank == 0) {
        const json_t *agent = json_object_get(json_object_get(row, "connected"), "agent");
        if (snag_buf_printf(text, " [agent %s]", json_is_true(agent) ? "connected" :
            json_is_false(agent) ? "disconnected" : "unknown") < 0) return -1;
    }
    json_int_t timestamp = json_integer_value(json_object_get(row, "time"));
    if (timestamp > 0) {
        time_t seconds = (time_t)(timestamp / 1000);
        struct tm local;
        char time[32];
        if (snag_localtime(&seconds, &local) &&
            strftime(time, sizeof(time), "%m-%d %H:%M:%S", &local) &&
            snag_buf_printf(text, "  %s", time) < 0) return -1;
    }
    return 0;
}

static unsigned int
document_style(const json_t *block, bool heading)
{
    const char *kind = snag_json_string(block, "kind");
    if (!kind) return heading ? SNAG_VM_BOLD : 0u;
    if (!strcmp(kind, "reasoning")) return SNAG_VM_DIM;
    if (!heading) return 0u;
    unsigned int color = 0u;
    if (!strcmp(kind, "assistant")) color = SNAG_VM_CYAN;
    else if (snag_string_in(kind,
        "input_received steering_added future_turn_queued future_turn_edited"))
        color = SNAG_VM_MAGENTA;
    else if (!strcmp(kind, "irc")) color = SNAG_VM_BLUE;
    else if (!strcmp(kind, "goal")) color = SNAG_VM_YELLOW;
    else if (snag_string_in(kind, "response_failed turn_failed")) color = SNAG_VM_RED;
    else if (snag_string_in(kind, "response_interrupted turn_interrupted")) color = SNAG_VM_YELLOW;
    else if (snag_string_in(kind, "tool_started tool_finished")) {
        static const unsigned int colors[] = {
            SNAG_VM_YELLOW, SNAG_VM_GREEN, SNAG_VM_YELLOW, SNAG_VM_RED};
        json_int_t role = json_integer_value(json_object_get(block, "role"));
        if (role >= SNAG_ROLE_ACTIVITY && role <= SNAG_ROLE_ERROR) color = colors[role];
    }
    return SNAG_VM_BOLD | color;
}

static void
row_styles(struct vm *vm, const struct snag_vm_rectangle *rectangle, size_t screen_row,
    const json_t *block, const struct snag_vm_document_row *row, const char *text)
{
    const json_t *styles = json_object_get(block, "styles");
    if (!styles || row->heading) return;
    size_t at = row->begin, column = 0u, span = 0u;
    while (span < json_array_size(styles) &&
        (size_t)json_integer_value(json_array_get(json_array_get(styles, span), 1u)) <= at) {
        ++span;
    }
    while (at < row->end && column < rectangle->columns) {
        while (span < json_array_size(styles) &&
            (size_t)json_integer_value(json_array_get(json_array_get(styles, span), 1u)) <= at) {
            ++span;
        }
        const json_t *run = json_array_get(styles, span);
        unsigned int style = (unsigned int)json_integer_value(json_array_get(run, 2u));
        struct snag_vm_glyph glyph = snag_vm_glyph(text + at, row->end - at,
            row->column + column, false);
        for (size_t n = 0u; n < glyph.columns && column < rectangle->columns; ++n, ++column) {
            vm->grid.back.cells[screen_row * vm->grid.columns +
                rectangle->column + column].style = style;
        }
        at += glyph.bytes;
    }
}

static int
draw_window(void *opaque, const struct snag_vm_rectangle *rectangle)
{
    struct vm *vm = opaque;
    size_t index = 0u;
    while (index < vm->count && vm->windows[index].id != rectangle->window) ++index;
    if (index == vm->count) return -1;
    struct vm_window *window = &vm->windows[index];
    bool taller = rectangle->rows > window->rectangle.rows;
    window->rectangle = *rectangle;
    window->history_rows = window->composer_rows = 0u;
    if (!rectangle->visible) return 0;
    if (rectangle->column) {
        for (size_t row = rectangle->row; row < rectangle->row + rectangle->rows; ++row)
            if (snag_vm_grid_text(&vm->grid, row, rectangle->column - 1u, 1u,
                "│", strlen("│"), SNAG_VM_DIM | SNAG_VM_CYAN) < 0) return -1;
    }
    if (rectangle->row) {
        for (size_t column = rectangle->column;
            column < rectangle->column + rectangle->columns; ++column)
            if (snag_vm_grid_text(&vm->grid, rectangle->row - 1u, column, 1u,
                "─", strlen("─"), SNAG_VM_DIM | SNAG_VM_CYAN) < 0) return -1;
    }
    size_t height = rectangle->rows > 1u ? rectangle->rows - 1u : 0u;
    struct snag_vm_connection *c = connection_for(vm, window->session_id, false);
    struct snag_vm_buffer *b = window_buffer(vm, window);
    bool editing = index == vm->focus && vm->composer && snag_vm_buffer_writable(b) && !vm->mode;
    size_t count = row_count(vm, window);
    bool prompt_cursor = index == vm->focus && !vm->mode &&
        window->kind == VIEW_TRANSCRIPT && (editing || window->follow || !count);
    if (window->kind == VIEW_TRANSCRIPT && height &&
        draw_composer(vm, window, b, &height, prompt_cursor) < 0) return -1;
    window->history_rows = height;
    if (document_view(window) && window->follow)
        window->top = count > height ? count - height : 0u;
    if (window->selected >= count) window->selected = count ? count - 1u : 0u;
    if (window->top > window->selected) window->top = window->selected;
    if (height && window->selected - window->top >= height)
        window->top = window->selected - height + 1u;
    if (document_view(window)) {
        if (window->document && (taller ||
            snag_vm_document_columns(window->document) != rectangle->columns) &&
            !window->load && vm->page.window != window->id && vm->scan.window != window->id)
            queue_history(vm, window, window->follow ? LOAD_LAST : LOAD_KEEP);
        for (size_t i = window->top; i < count && i - window->top < height; ++i) {
            struct snag_vm_document_row row;
            if (snag_vm_document_row(window->document, i, &row) < 0) return -1;
            const char *text = snag_vm_document_text(window->document, &row);
            unsigned int style = document_style(
                snag_vm_document_block(window->document, row.block), row.heading);
            if (snag_vm_grid_text_column(&vm->grid, rectangle->row + i - window->top,
                rectangle->column, rectangle->columns, text + row.begin, row.end - row.begin,
                style, row.column) < 0) return -1;
            row_styles(vm, rectangle, rectangle->row + i - window->top,
                snag_vm_document_block(window->document, row.block), &row, text);
            highlight_selection(vm, window, &row, rectangle->row + i - window->top);
            if (i == window->selected && index == vm->focus && !prompt_cursor) {
                const json_t *block = snag_vm_document_block(window->document, row.block);
                size_t byte = row.begin;
                if (window->anchor_heading == row.heading &&
                    !strcmp(window->anchor_key, snag_json_string(block, "key"))) {
                    uint64_t at = row.heading || !window->anchor_source ? window->anchor_byte :
                        snag_vm_source_position(block, window->anchor_byte, false);
                    if (at >= row.begin && at <= row.end) byte = (size_t)at;
                }
                size_t column = 0u;
                for (size_t at = row.begin; at < byte;) {
                    struct snag_vm_glyph glyph = snag_vm_glyph(text + at, row.end - at,
                        row.column + column, false);
                    if (glyph.bytes > byte - at) break;
                    column += glyph.columns;
                    at += glyph.bytes;
                }
                vm->cursor_row = rectangle->row + i - window->top;
                vm->cursor_column = rectangle->column +
                    (column < rectangle->columns ? column : rectangle->columns - 1u);
            }
        }
    }
    json_t *rows = view_rows(vm, window);
    size_t visible = 0u;
    size_t available = window->kind == VIEW_HELP ? count : json_array_size(rows);
    for (size_t i = 0u; !document_view(window) && i < available; ++i) {
        json_t *row = json_array_get(rows, i);
        if (window->kind != VIEW_HELP && !matches(window, row)) continue;
        size_t position = visible++;
        if (position < window->top || position - window->top >= height) continue;
        struct snag_buf text = {.max = SNAG_MAX_DIRECT_PROMPT};
        if (window->kind == VIEW_HELP) {
            (void)snag_buf_printf(&text, "%s", help_rows[i]);
        } else if (window->kind == VIEW_REPORTS) {
            const char *command = snag_json_string(row, "command");
            if (snag_buf_printf(&text, "%.8s  ", snag_json_string(row, "id")) < 0 ||
                snag_vm_report_text((const unsigned char *)command, strlen(command),
                    &vm->secrets.wire, NULL, NULL, &text) < 0) {
                snag_buf_free(&text);
                return -1;
            }
        } else if (window->kind == VIEW_BUFFERS) {
            if (buffer_row(&text, row) < 0) { snag_buf_free(&text); return -1; }
        } else if (window->kind == VIEW_WORKSPACES) {
            const char *error = snag_json_string(row, "error");
            const char *name = snag_json_string(row, "name");
            (void)snag_buf_printf(&text, "%.8s %-6s %s%s", snag_json_string(row, "id"),
                error ? "error" : json_is_true(json_object_get(row, "open")) ? "open" : "stored",
                name ? name : "", error ? error : "");
        } else {
            json_t *cells = json_object_get(row, "cells");
            static const size_t order[] = {0u, 4u, 1u, 2u, 3u, 5u, 6u};
            for (size_t c = 0u; c < sizeof(order) / sizeof(order[0]); ++c) {
                const char *cell = json_string_value(json_array_get(cells, order[c]));
                if (cell) (void)snag_buf_printf(&text, "%s%s", c ? "  " : "", cell);
            }
        }
        int rc = snag_vm_grid_text(&vm->grid, rectangle->row + position - window->top,
            rectangle->column, rectangle->columns, (const char *)text.data, text.len,
            position == window->selected ? SNAG_VM_REVERSE : 0u);
        snag_buf_free(&text);
        if (rc < 0) return -1;
    }
    char status[512];
    (void)snprintf(status, sizeof(status), "%s  %zu/%zu%s%s", view_names[window->kind],
        count ? window->selected + 1u : 0u, count,
        window->filter && *window->filter ? " /" : "", window->filter ? window->filter : "");
    if (window->kind == VIEW_TRANSCRIPT) {
        char owner[192] = "";
        if (c && c->state) {
            const char *provider = snag_json_string(c->state, "provider");
            const char *model = snag_json_string(c->state, "model");
            const char *effort = snag_json_string(c->state, "effort");
            const char *tier = snag_json_string(c->state, "service_tier");
            (void)snprintf(owner, sizeof(owner), " %s/%s/%s %s%s", provider ? provider : "?",
                model ? model : "?", effort ? effort : "?",
                json_is_true(json_object_get(c->state, "active")) ? "working" : "idle",
                tier && !strcmp(tier, "priority") ? " FAST" : "");
        }
        if (window->route) {
            const char *identity = snag_json_string(window->route, "identity");
            (void)snprintf(owner, sizeof(owner), " %s/%s [%s%s]",
                json_object_get(window->route, "room") ? "channel" :
                    json_object_get(window->route, "peer") ? "query" : "connection",
                snag_view_conversation_name(window->route), identity,
                snag_vm_buffer_writable(b) ? "" : " read-only");
        }
        const char *name = c ? snag_json_string(c->state, "name") : NULL;
        const char *attachment = c && c->bound ? "ATTACHED" : "read-only";
        char identity[256];
        if (name && *name) (void)snprintf(identity, sizeof(identity), "%s %s", name, attachment);
        else (void)snprintf(identity, sizeof(identity), "%s %.8s", attachment, window->session_id);
        (void)snprintf(status, sizeof(status), "%s%s%s%s history v%u %s %s  seq %llu%s%s%s%s%s",
            editing ? vm->insert ? "INSERT " : "NORMAL draft " :
                window->visual.kind == SNAG_VM_SELECT_LINE ? "VISUAL LINE " :
                window->visual.kind == SNAG_VM_SELECT_BLOCK ? "VISUAL BLOCK " :
                window->visual.kind ? "VISUAL " : "",
            b && b->draft_conflict ? "[draft conflict] " : "", identity, owner,
            window->verbosity, window->follow ? "FOLLOW" : "HOLD",
            window->document ? window->best_effort ? "snapshot" : "committed" : "",
            (unsigned long long)window->anchor_seq,
            window->begin.offset ? "  ↑ older" : "  [start]",
            window->end.offset < window->tail.offset ? "  ↓ newer" : "  [tail]",
            window->source_failed ? "  source error; R" :
                window->incomplete ? "  partial tail" : "",
            (window->load && window->load != LOAD_POLL) ||
            ((vm->page.window == window->id && vm->page.load != LOAD_POLL) ||
                vm->scan.window == window->id) ? "  loading" : "",
            b && b->pending ? "  [submission retained]" : "");
    }
    if (window->kind == VIEW_REPORT) {
        (void)snprintf(status, sizeof(status), "REPORT %.8s %.8s  %zu/%zu%s%s",
            window->session_id, snag_json_string(window->report, "id"),
            count ? window->selected + 1u : 0u, count,
            window->load || vm->page.window == window->id || vm->scan.window == window->id ?
                "  loading" : "",
            window->source_failed ? "  source error; R" : "");
    }
    size_t status_row = rectangle->row + rectangle->rows - 1u;
    unsigned int style = index == vm->focus ?
        SNAG_VM_REVERSE : SNAG_VM_BOLD | SNAG_VM_REVERSE | SNAG_VM_CYAN;
    for (size_t column = rectangle->column;
        column < rectangle->column + rectangle->columns; ++column)
        vm->grid.back.cells[status_row * vm->grid.columns + column].style = style;
    return snag_vm_grid_text(&vm->grid, status_row, rectangle->column,
        rectangle->columns, status, strlen(status), style);
}

static int
painted_read(struct vm *vm)
{
    if (vm->dirty || resized || vm->unfocused) return 0;
    struct vm_window *window = &vm->windows[vm->focus];
    if (window->kind != VIEW_TRANSCRIPT || !window->rectangle.visible || !window->follow ||
        !window->document || window->incomplete || window->source_failed ||
        window->load || vm->page.window == window->id || vm->scan.window == window->id ||
        window->end.offset != window->tail.offset || !window->end.next_seq) return 0;
    size_t count = row_count(vm, window);
    if (!count || count - 1u < window->top ||
        count - 1u - window->top >= window->history_rows) return 0;
    struct snag_vm_buffer *buffer = window_buffer(vm, window);
    if (!buffer || !json_is_object(buffer->route) ||
        !snag_vm_buffer_read(buffer, window->end.next_seq - 1u)) return 0;
    changed(vm);
    for (size_t i = 0u; i < vm->count; ++i)
        if (vm->windows[i].kind == VIEW_BUFFERS) return buffer_catalog(vm);
    return 0;
}

static int
draw(struct vm *vm)
{
    size_t rows = snag_term_host_rows(), columns = snag_term_host_columns();
    if (!rows) rows = 1u;
    if (!columns) columns = 1u;
    if (snag_vm_grid_resize(&vm->grid, rows, columns) < 0) return -1;
    vm->dirty = false;
    vm->prompt_due = 0u;
    if (mouse_reporting(vm) < 0) return -1;
    vm->cursor_row = vm->cursor_column = SIZE_MAX;
    snag_vm_grid_begin(&vm->grid);
    if (snag_vm_layout_place(vm->layout, vm->windows[vm->focus].id,
        rows > 1u ? (unsigned int)rows - 1u : 1u, (unsigned int)columns, draw_window, vm) < 0)
        return -1;
    size_t cursor = 0u;
    if (vm->mode) {
        const char *prefix = vm->search_command && vm->mode == '/' ? "command › /search " : NULL;
        size_t prefix_bytes = prefix ? strlen(prefix) : 1u;
        size_t prefix_cells = prefix ?
            snag_vm_text_column(prefix, prefix_bytes, prefix_bytes, false) : 1u;
        if (prefix_cells >= columns) prefix_cells = columns > 1u ? columns - 1u : 1u;
        size_t start = 0u;
        size_t column = snag_vm_text_column((const char *)vm->command.data,
            vm->command.len, vm->command_cursor, false);
        if (column + prefix_cells + 1u > columns)
            start = snag_vm_text_at_column((const char *)vm->command.data,
                vm->command.len, 0u, column + prefix_cells + 1u - columns, false);
        size_t start_column = snag_vm_text_column((const char *)vm->command.data,
            vm->command.len, start, false);
        if (snag_vm_grid_text(&vm->grid, rows - 1u, 0u, prefix_cells,
            prefix ? prefix : &vm->mode, prefix_bytes, SNAG_VM_BOLD) < 0 ||
            snag_vm_grid_text_column(&vm->grid, rows - 1u, prefix_cells, columns - prefix_cells,
                vm->command.data ? (const char *)vm->command.data + start : "",
                vm->command.len - start, 0u, start_column) < 0) return -1;
        cursor = column - start_column + prefix_cells;
        if (cursor >= columns) cursor = columns - 1u;
    } else if (snag_vm_grid_text(&vm->grid, rows - 1u, 0u, columns,
        vm->message, strlen(vm->message), 0u) < 0) return -1;
    int rc = !vm->mode && vm->cursor_row < rows && vm->cursor_column < columns ?
        snag_vm_grid_flush(&vm->grid, vm->cursor_row, vm->cursor_column, true, emit, vm) :
        snag_vm_grid_flush(&vm->grid, rows - 1u, cursor, vm->mode != 0, emit, vm);
    return rc < 0 ? rc : painted_read(vm);
}

static void
collect(struct vm *vm, struct vm_read *read)
{
    struct snag_vm_read_result *result = snag_vm_reader_take(read->reader);
    if (!result) {
        uint64_t bytes, events, total;
        if (read == &vm->scan && (vm->searching || vm->copying || vm->navigating) &&
            snag_vm_reader_progress(read->reader, read->generation, &bytes, &events, &total)) {
            char progress[160];
            (void)snprintf(progress, sizeof(progress),
                "%s %llu/%llu bytes, %llu events; Ctrl-C cancels",
                vm->copying ? "Copying" : vm->navigating ? "Moving" : "Searching",
                (unsigned long long)bytes,
                (unsigned long long)total, (unsigned long long)events);
            if (vm->copying || vm->navigating) notice(vm, progress);
            else search_notice(vm, progress);
        }
        return;
    }
    uint64_t target = read->window;
    enum history_load load = read->load;
    read->generation = read->window = 0u;
    if (read == &vm->scan) vm->searching = vm->copying = vm->navigating = false;
    if (result->request.selection.kind) {
        if (result->error_number) notice(vm, result->error);
        else if (!result->copied.length) notice(vm, "Empty motion; register preserved");
        else {
            snag_vm_register_free(&vm->reg);
            vm->reg = result->copied;
            memset(&result->copied, 0, sizeof(result->copied));
            (void)snprintf(vm->message, sizeof(vm->message), "Yanked %llu bytes%s",
                (unsigned long long)vm->reg.length, vm->reg.file ? " (file-backed register)" : "");
            vm->dirty = true;
            clipboard_yank(vm);
        }
        for (size_t i = 0u; !result->error_number && i < vm->count; ++i) {
            if (vm->windows[i].id == target)
                vm->windows[i].visual.kind = SNAG_VM_SELECT_NONE;
        }
        snag_vm_read_result_free(result);
        drain_input(vm);
        return;
    }
    if (result->request.navigation.kind) {
        if (result->error_number) {
            restore_motion(vm, true);
            notice(vm, result->error);
        } else {
            vm->motion_loading = true;
            for (size_t i = 0u; i < vm->count; ++i) {
                struct vm_window *window = &vm->windows[i];
                if (window->id != target || !document_view(window)) continue;
                memcpy(window->anchor_key, result->match.key, sizeof(window->anchor_key));
                window->anchor_seq = result->match.seq;
                window->anchor_byte = result->match.byte;
                window->anchor_heading = result->match.heading;
                window->anchor_source = true;
                queue_history(vm, window, window->kind == VIEW_REPORT ? LOAD_KEEP : LOAD_ANCHOR);
            }
            notice(vm, result->incomplete ? "Position in incomplete snapshot" : "Position found");
        }
        snag_vm_read_result_free(result);
        drain_input(vm);
        return;
    }
    if (result->request.query) {
        if (result->error_number) search_notice(vm, result->error);
        else {
            const char *status = result->found ? result->incomplete ?
                "Match in incomplete snapshot" : result->wrapped ? "Match (wrapped)" : "Match" :
                result->incomplete ? "Search incomplete: unfinished journal suffix" : "No matches";
            search_notice(vm, result->found ? "Loading search result… Ctrl-C cancels" : status);
            for (size_t i = 0u; result->found && i < vm->count; ++i) {
                struct vm_window *window = &vm->windows[i];
                if (window->id != target || !document_view(window)) continue;
                /* Selection and motions must see the matched viewport even
                 * when input arrives before its asynchronous page read. */
                motion_origin(vm, window);
                vm->motion_origin.tail = result->tail;
                vm->motion_origin.best_effort = result->best_effort;
                vm->motion_origin.incomplete = result->incomplete;
                vm->motion_loading = true;
                vm->search_finish = status;
                memcpy(window->anchor_key, result->match.key, sizeof(window->anchor_key));
                window->anchor_seq = result->match.seq;
                window->anchor_byte = result->match.byte;
                window->anchor_heading = result->match.heading;
                window->anchor_source = true;
                queue_history(vm, window, window->kind == VIEW_REPORT ? LOAD_KEEP : LOAD_ANCHOR);
            }
        }
        snag_vm_read_result_free(result);
        return;
    }
    if (result->error_number) {
        if (vm->motion_loading && vm->motion_origin.window == target) restore_motion(vm, true);
        notice(vm, result->error);
        for (size_t i = 0u; i < vm->count; ++i) {
            if (vm->windows[i].id != target) continue;
            vm->windows[i].source_failed = true;
            vm->windows[i].report_open_pending = false;
            vm->windows[i].follow_at = UINT64_MAX;
            vm->windows[i].load = LOAD_NONE;
        }
    } else if (target) {
        for (size_t i = 0u; i < vm->count; ++i) {
            struct vm_window *window = &vm->windows[i];
            if (window->id == target && result->request.kind == SNAG_VM_READ_REPORTS &&
                !strcmp(window->session_id, result->request.session_id)) {
                struct snag_vm_connection *c = connection_for(vm, window->session_id, false);
                bool open = window->report_open_pending;
                window->report_open_pending = false;
                if (!c || snag_vm_reports_merge(c, result->catalog,
                    result->request.known_reports) < 0) {
                    notice(vm, "Report catalogue conflicts with retained references");
                    continue;
                }
                if (result->incomplete)
                    notice(vm, "Showing reports before incomplete final catalogue entry");
                if (open) {
                    json_t *selected = NULL;
                    size_t matches = 0u;
                    const char *prefix = window->report_selector;
                    for (size_t n = 0u; n < json_array_size(c->reports); ++n) {
                        json_t *report = json_array_get(c->reports, n);
                        if (!*prefix ||
                            !strncmp(snag_json_string(report, "id"), prefix, strlen(prefix))) {
                            selected = report;
                            ++matches;
                        }
                    }
                    if (*prefix && matches > 1u) notice(vm, "Ambiguous report ID");
                    else {
                        size_t focus = vm->focus;
                        bool composer = vm->composer, insert = vm->insert;
                        vm->focus = i;
                        open_report(vm, c, selected);
                        vm->focus = focus;
                        if (focus != i) { vm->composer = composer; vm->insert = insert; }
                    }
                }
                changed(vm);
                continue;
            }
            if (window->id == target && window->kind == VIEW_REPORT &&
                !strcmp(window->session_id, result->request.session_id) &&
                json_equal(window->report, result->request.report)) {
                snag_vm_document_free(window->document);
                window->document = result->document;
                result->document = NULL;
                size_t count = snag_vm_document_rows(window->document);
                window->selected = load == LOAD_LAST ? (count ? count - 1u : 0u) :
                    load == LOAD_FIRST ? 0u : locate_anchor(window);
                window->top = window->selected;
                if (load != LOAD_KEEP || !window->anchor_source) remember_anchor(window);
                changed(vm);
                continue;
            }
            if (window->id != target || window->kind != VIEW_TRANSCRIPT ||
                strcmp(window->session_id, result->request.session_id)) continue;
            window->follow_at = snag_monotonic_ms() + 1000u;
            if (load == LOAD_POLL &&
                (result->unchanged || result->request.tail_only || !window->follow)) {
                if (!result->unchanged || window->incomplete != result->incomplete ||
                    window->best_effort != result->best_effort)
                    vm->dirty = true;
                if (!window->visual.kind) window->tail = result->tail;
                window->incomplete = result->incomplete;
                window->best_effort = result->best_effort;
                continue;
            }
            struct snag_journal_cursor end = result->request.before_seq && load != LOAD_ANCHOR ?
                window->begin : result->tail;
            if (load == LOAD_ANCHOR) {
                /* The next forward read may start at the page's first boundary;
                 * recover its exact end before continuing past this anchor. */
                window->begin = result->cursor;
                window->load = LOAD_KEEP;
            }
            if (result->request.reverse) {
                window->begin = result->cursor;
                window->end = end;
            } else {
                window->begin = result->request.cursor;
                window->end = result->cursor;
            }
            window->tail = result->tail;
            window->incomplete = result->incomplete;
            window->best_effort = result->best_effort;
            if (vm->motion_loading && vm->motion_origin.window == window->id) {
                window->incomplete |= vm->motion_origin.incomplete;
                window->best_effort |= vm->motion_origin.best_effort;
            }
            snag_vm_document_free(window->document);
            window->document = result->document;
            result->document = NULL;
            size_t count = snag_vm_document_rows(window->document);
            if (keeps_anchor(load)) window->selected = locate_anchor(window);
            else if (load == LOAD_LAST || load == LOAD_PREVIOUS || load == LOAD_POLL)
                window->selected = count ? count - 1u : 0u;
            else window->selected = 0u;
            size_t height = window->rectangle.rows > 1u ? window->rectangle.rows - 1u : 1u;
            window->top = window->selected >= height ? window->selected - height + 1u : 0u;
            if (count) {
                if (!result->request.rows && window->follow && window->begin.offset &&
                    count < height) queue_history(vm, window, LOAD_LAST);
                if (!keeps_anchor(load) || !window->anchor_source) remember_anchor(window);
                /* Forward and reverse page boundaries differ when a large
                 * record crosses the byte budget. Finish reaching the anchor
                 * before accepting the replacement viewport. */
                if (keeps_anchor(load) && load != LOAD_ANCHOR &&
                    window->anchor_seq >= window->end.next_seq &&
                    window->end.offset < window->tail.offset)
                    queue_history(vm, window, LOAD_KEEP_NEXT);
            } else if (load != LOAD_ANCHOR) {
                if (result->request.reverse && window->begin.offset) {
                    queue_history(vm, window,
                        keeps_anchor(load) ? LOAD_KEEP_PREVIOUS : LOAD_PREVIOUS);
                } else if (!result->request.reverse && window->end.offset < window->tail.offset) {
                    queue_history(vm, window, keeps_anchor(load) ? LOAD_KEEP_NEXT : LOAD_NEXT);
                }
            }
            changed(vm);
        }
    } else {
        json_decref(vm->sessions);
        vm->sessions = result->catalog;
        result->catalog = NULL;
        for (size_t w = 0u; w < vm->count; ++w) {
            struct vm_window *window = &vm->windows[w];
            if (window->kind != VIEW_SESSIONS || !window->selected_id[0]) continue;
            size_t visible = 0u;
            for (size_t i = 0u; i < json_array_size(vm->sessions); ++i) {
                json_t *row = json_array_get(vm->sessions, i);
                if (!matches(window, row)) continue;
                if (!strcmp(snag_json_string(row, "id"), window->selected_id))
                    window->selected = visible;
                ++visible;
            }
        }
        vm->dirty = true;
    }
    snag_vm_read_result_free(result);
    struct vm_window *window = &vm->windows[vm->focus];
    if (vm->motion_loading && vm->motion_origin.window == target &&
        window->id == target && !window->load) {
        const char *status = vm->search_finish;
        restore_motion(vm, false);
        if (status) search_notice(vm, status);
    }
    if (window->yank_after_load && window->id == target && !window->load) {
        window->yank_after_load = false;
        yank(vm);
    }
    drain_input(vm);
}

static int
enter_screen(struct vm *vm, bool flush_input)
{
    static const char modes[] = "\033[?1049h\033[?2004h\033[?1004h\033[?25l";
    vm->entering = true;
    int rc = snag_term_input_raw(&vm->terminal, flush_input);
    if (!rc) rc = snag_term_output_mode(&vm->terminal, true);
    if (!rc) rc = emit(vm, modes, sizeof(modes) - 1u);
    if (!rc) rc = mouse_reporting(vm);
    vm->entering = false;
    vm->grid.valid = false;
    vm->dirty = true;
    return rc;
}

static void
clipboard_finish(struct vm *vm, const char *message)
{
    struct vm_clipboard *copy = &vm->clipboard;
    if (message) notice(vm, message);
    snag_clipboard_send_close(copy->send);
    copy->send = NULL;
    snag_clipboard_close(copy->source);
    copy->source = NULL;
    copy->osc = (struct snag_clipboard_osc){0};
    if (copy->output >= 0) {
        (void)close(copy->output);
        copy->output = -1;
        if (copy->client[0]) snag_tmux_refresh(&copy->profile, copy->client);
    }
    copy->client[0] = 0;
    copy->local = copy->canceling = false;
    copy->packet_at = copy->packet_length = 0u;
    copy->shown_bytes = 0u;
}

#ifdef _WIN32
static int
clipboard_checkpoint(void *opaque)
{
    struct vm *vm = opaque;
    return stopped || vm->quit ? snag_errno(ECANCELED) : input_ready(vm, 0);
}
#endif

static int
clipboard_flush(struct vm *vm)
{
    struct vm_clipboard *copy = &vm->clipboard;
    if (copy->packet_at == copy->packet_length) return 1;
#ifdef _WIN32
    if (snag_term_output_write(&vm->terminal, vm->output,
        copy->packet + copy->packet_at, copy->packet_length - copy->packet_at,
        true, clipboard_checkpoint, vm) < 0) return -1;
    copy->packet_at = copy->packet_length;
#else
    ssize_t size = write(copy->output, copy->packet + copy->packet_at,
        copy->packet_length - copy->packet_at);
    if (size > 0) copy->packet_at += (size_t)size;
    else if (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
#endif
    return copy->packet_at == copy->packet_length ? 1 : 0;
}

static int
clipboard_write(struct vm *vm, const char *bytes, size_t length)
{
    struct vm_clipboard *copy = &vm->clipboard;
    if (length > SNAG_SCREEN_TITLE_MAX) return snag_errno(EOVERFLOW);
    copy->packet_at = copy->packet_length = 0u;
    bool screen = copy->profile.sty[0] && !copy->profile.tmux[0];
    while (length) {
        size_t size = length < 128u ? length : 128u;
        unsigned char *out = copy->packet + copy->packet_length;
        if (screen) { memcpy(out, "\033P", 2u); out += 2u; }
        memcpy(out, bytes, size);
        if (screen) memcpy(out + size, "\033\\", 2u);
        copy->packet_length += size + (screen ? 4u : 0u);
        bytes += size;
        length -= size;
    }
    return clipboard_flush(vm) < 0 ? -1 : 0;
}

static int
clipboard_step(struct vm *vm)
{
    struct vm_clipboard *copy = &vm->clipboard;
    int flushed = clipboard_flush(vm);
    if (flushed <= 0) return flushed;
    if (!copy->source && copy->pending) {
        struct snag_clipboard_backend none = {0};
        bool native = vm->config.terminal_clipboard == SNAG_CLIP_NATIVE &&
            !getenv("SSH_CONNECTION") && !getenv("SSH_TTY") && !getenv("MOSH_IP");
        copy->source = snag_clipboard_open(copy->pending_fd, copy->pending_text.data,
            copy->pending_length, native ? NULL : &none);
        clipboard_pending_clear(copy);
        if (!copy->source) { notice(vm, "Yank retained; clipboard preparation failed"); return 0; }
        char message[128];
        (void)snprintf(message, sizeof(message), "Yanked %llu bytes; preparing clipboard",
            (unsigned long long)copy->pending_length);
        notice(vm, message);
    }
    if (!copy->source) return 0;
    struct snag_clipboard_result prepared;
    snag_clipboard_result(copy->source, &prepared);
    if (prepared.state >= SNAG_CLIPBOARD_WRITTEN) {
        static const char *const messages[] = {
            "Clipboard written", "Clipboard copy canceled; register retained",
            "Clipboard unavailable; register retained", "Clipboard copy failed; register retained",
            "Clipboard result uncertain; register retained"
        };
        clipboard_finish(vm, messages[prepared.state - SNAG_CLIPBOARD_WRITTEN]);
        return 0;
    }
    if (prepared.state != SNAG_CLIPBOARD_READY ||
        (copy->canceling && !copy->send && !copy->osc.source)) return 0;
    if (!copy->send && !copy->local) {
        if (snag_terminal_profile_capture(&copy->profile) < 0) goto unavailable;
#ifndef _WIN32
        copy->output = copy->profile.tmux[0] ?
            snag_tmux_output_open(&copy->profile, copy->client) :
            snag_term_reopen(STDOUT_FILENO, O_WRONLY);
        if (copy->output < 0) goto unavailable;
#endif
        copy->send = snag_clipboard_send_open(copy->source, snag_monotonic_ms());
        if (!copy->send) goto unavailable;
    }
    if (copy->send) {
        char title[SNAG_SCREEN_TITLE_MAX];
        int size = snag_clipboard_send_output(copy->send, snag_monotonic_ms(),
            title, sizeof(title));
        if (size < 0) {
            clipboard_finish(vm, "Clipboard transfer failed; register retained");
            return 0;
        }
        if (size && clipboard_write(vm, title, (size_t)size) < 0) return -1;
        struct snag_copy_result result;
        snag_clipboard_send_result(copy->send, &result);
        if (result.state < SNAG_COPY_WRITTEN) {
            if (result.bytes != copy->shown_bytes && !copy->canceling) {
                char message[128];
                (void)snprintf(message, sizeof(message),
                    "Yanked %llu bytes; workstation copy %llu/%llu",
                    (unsigned long long)result.length, (unsigned long long)result.bytes,
                    (unsigned long long)result.length);
                notice(vm, message);
                copy->shown_bytes = result.bytes;
            }
            return 0;
        }
        if (result.state == SNAG_COPY_UNAVAILABLE && !result.remote && !copy->canceling) {
            snag_clipboard_send_close(copy->send);
            copy->send = NULL;
            copy->local = true;
            if (vm->config.terminal_clipboard == SNAG_CLIP_OSC52)
                copy->osc = (struct snag_clipboard_osc){.source = copy->source};
            else if (snag_clipboard_publish(copy->source) < 0) goto unavailable;
        } else {
            static const char *const messages[] = {
                "Workstation clipboard written",
                "Clipboard sequence sent; terminal acceptance unconfirmed",
                "Clipboard copy canceled; register retained",
                "Workstation clipboard unavailable; register retained",
                "Clipboard transfer failed; register retained",
                "Clipboard result uncertain; register retained"
            };
            clipboard_finish(vm, messages[result.state - SNAG_COPY_WRITTEN]);
            return 0;
        }
    }
    if (copy->osc.source) {
        if (copy->osc.done) {
            clipboard_finish(vm, "Clipboard sequence sent; terminal acceptance unconfirmed");
            return 0;
        }
        char bytes[6144];
        int size = snag_clipboard_osc_next(&copy->osc, bytes, sizeof(bytes));
        if (size < 0 || (size && clipboard_write(vm, bytes, (size_t)size) < 0)) return -1;
    }
    return 0;
unavailable:
    clipboard_finish(vm, "Clipboard unavailable; register retained");
    return 0;
}

static int
restore_checkpoint(void *opaque)
{
    uint64_t *deadline = opaque;
    return snag_monotonic_ms() >= *deadline ? snag_errno(ETIMEDOUT) : 0;
}

static void
clipboard_settle(struct vm *vm)
{
    struct vm_clipboard *copy = &vm->clipboard;
    (void)clipboard_cancel(vm);
    copy->settling = true;
    /* Drain cancellation/result replies before another client owns stdin. */
    uint64_t deadline = snag_monotonic_ms() + 20000u;
    while (copy->source && !stopped && snag_monotonic_ms() < deadline) {
        if (clipboard_step(vm) < 0 || input_ready(vm, 50) < 0) break;
    }
    if ((copy->osc.begun && !copy->osc.done) || copy->packet_at < copy->packet_length) {
        int fd = copy->output >= 0 ? copy->output : vm->output;
        const char *cancel = copy->profile.sty[0] && !copy->profile.tmux[0] ?
            "\030\033P\030\033\\" : "\030";
        /* A full tty queue must not drop cancellation ahead of mode restore. */
        uint64_t abort_deadline = snag_monotonic_ms() + 250u;
        (void)snag_term_output_write(&vm->terminal, fd, cancel, strlen(cancel),
            false, restore_checkpoint, &abort_deadline);
    }
    clipboard_finish(vm, NULL);
    copy->settling = false;
}

static void
leave_screen(struct vm *vm, bool restore_input)
{
    static const char modes[] = "\033[0m\033[?25h\033[?1004l\033[?1006l"
        "\033[?1002l\033[?1000l\033[?2004l\033[?1049l";
    clipboard_settle(vm);
    vm->mouse_reported = vm->mouse_down = false;
    uint64_t deadline = snag_monotonic_ms() + 250u;
    (void)snag_term_output_write(&vm->terminal, vm->output, modes, sizeof(modes) - 1u,
        false, restore_checkpoint, &deadline);
    (void)snag_term_output_mode(&vm->terminal, false);
    if (restore_input) (void)snag_term_input_restore(&vm->terminal, false);
}

static int
classic_run(struct vm *vm)
{
    char error[256] = "";
    int rc = -1;
    if (save(vm, NULL) == 0) {
        cancel_search(vm);
        cancel_read(&vm->page);
        /* Cooked input between clients can translate or discard incoming paste. */
        leave_screen(vm, false);
        vm->classic.cancelled = &stopped;
        vm->classic.suspend_terminal = &vm->terminal;
        int peer = classic_connect(vm, vm->classic.session, NULL, error, sizeof(error));
        if (peer >= 0) rc = snag_session_client_terminal(peer, false, 0u,
            classic_connect, vm, &vm->classic, error, sizeof(error));
        memset(vm->classic.command, 0, sizeof(vm->classic.command));
        if (vm->classic.signal) stopped = vm->classic.signal;
        if (rc > 0 && !stopped) (void)snprintf(error, sizeof(error),
            "Classic owner exited with status %d", rc);
        if (!stopped && enter_screen(vm, false) < 0) return -1;
        memset(&vm->input, 0, sizeof(vm->input));
        vm->prefix = vm->mode = 0;
        vm->composer = vm->insert = false;
        changed(vm);
        if (save(vm, NULL) < 0) return -1;
        if (!stopped) notice(vm, error[0] ? error : "Returned from classic attachment");
    }
    vm->classic_pending = vm->classic_ready = false;
    if (stopped) return 0;
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next)
        if (connection_visible(vm, c)) (void)snag_vm_connection_open(c, &vm->store, c->control);
    for (size_t i = 0u; i < vm->count; ++i) {
        struct vm_window *window = &vm->windows[i];
        if (window->kind == VIEW_TRANSCRIPT)
            queue_history(vm, window, window->follow ? LOAD_LAST : LOAD_KEEP);
    }
    refresh(vm);
    return 0;
}

static int
interactive(struct vm *vm)
{
    if (!snag_isatty(STDIN_FILENO) || !snag_isatty(STDOUT_FILENO) || !snag_term_host_capable()) {
        (void)fprintf(stderr, "snajpagent: vm requires an interactive terminal\n");
        return 2;
    }
    struct snag_shutdown shutdown = {0};
    bool captured = false, controls = false, entered = false;
    int rc = -1;
    stopped = resized = 0;
    vm->output = snag_term_output_open(&vm->terminal, STDOUT_FILENO);
    if (vm->output < 0 || snag_term_input_capture(&vm->terminal) < 0) goto out;
    captured = true;
    if (snag_shutdown_install(&shutdown, stop_signal, true) < 0 ||
        snag_term_controls_install(&vm->terminal, stop_signal, resize_signal) < 0) goto out;
    controls = true;
    entered = true;
    if (enter_screen(vm, true) < 0) goto out;
    refresh(vm);
    if (vm->start_session) session_request(vm, vm->start_session);
    while (!vm->quit && !stopped) {
        launches_step(vm);
        connections_step(vm);
        if (vm->quit || stopped) break;
        if (vm->classic_ready && classic_run(vm) < 0) goto out;
        if (stopped) break;
        collect(vm, &vm->page);
        collect(vm, &vm->scan);
        if (clipboard_step(vm) < 0) goto out;
        /* Queued commands can schedule owner writes, detach or workspace
         * changes. Run those state transitions before waiting for new input. */
        if (vm->input_drained) {
            vm->input_drained = false;
            continue;
        }
        if (resized || snag_term_input_resized(&vm->terminal)) {
            resized = 0;
            vm->mouse_down = false;
            vm->grid.valid = false;
            vm->dirty = true;
        }
        if (vm->suspend) {
            vm->suspend = false;
            if (snag_term_can_suspend() && save(vm, NULL) == 0) {
                for (struct snag_vm_connection *c = vm->connections; c; c = c->next)
                    snag_vm_connection_close(c);
                leave_screen(vm, true);
                (void)snag_term_suspend();
                if (enter_screen(vm, false) < 0) goto out;
                for (struct snag_vm_connection *c = vm->connections; c; c = c->next)
                    if (connection_visible(vm, c))
                        (void)snag_vm_connection_open(c, &vm->store, c->control);
            }
        }
        if (vm->save_dirty && vm->save_at && snag_monotonic_ms() >= vm->save_at)
            (void)save(vm, NULL);
        uint64_t now = snag_monotonic_ms();
        for (size_t i = 0u; i < vm->count; ++i) {
            struct vm_window *window = &vm->windows[i];
            if (window->kind == VIEW_TRANSCRIPT && window->document && window->rectangle.visible &&
                !window->visual.kind && !window->load &&
                vm->page.window != window->id && vm->scan.window != window->id &&
                window->follow_at <= now)
                window->load = LOAD_POLL;
        }
        if (vm->prompt_due && now >= vm->prompt_due) vm->dirty = true;
        bool clipboard_output = vm->clipboard.osc.source ||
            vm->clipboard.packet_at < vm->clipboard.packet_length;
        if (vm->dirty && !clipboard_output && draw(vm) < 0) {
            if (vm->quit || stopped) break;
            goto out;
        }
        load_history(vm);
        int timeout = snag_vm_input_wait_ms(&vm->input, snag_monotonic_ms());
        if (vm->prompt_due) {
            uint64_t now = snag_monotonic_ms();
            int remaining = vm->prompt_due > now ? (int)(vm->prompt_due - now) : 0;
            if (timeout < 0 || timeout > remaining) timeout = remaining;
        }
        if (vm->save_at) {
            uint64_t now = snag_monotonic_ms();
            int remaining = vm->save_at > now ? (int)(vm->save_at - now) : 0;
            if (timeout < 0 || timeout > remaining) timeout = remaining;
        }
        if (!vm->page.generation) {
            uint64_t now = snag_monotonic_ms();
            for (size_t i = 0u; i < vm->count; ++i) {
                struct vm_window *window = &vm->windows[i];
                if (window->kind != VIEW_TRANSCRIPT || !window->document ||
                    !window->rectangle.visible || window->source_failed ||
                    window->visual.kind || vm->scan.window == window->id) continue;
                int remaining = window->follow_at > now ? (int)(window->follow_at - now) : 0;
                if (timeout < 0 || timeout > remaining) timeout = remaining;
            }
        }
        if ((vm->dirty && !clipboard_output) || vm->suspend) timeout = 0;
        if (input_ready(vm, timeout) < 0) goto out;
    }
    rc = save(vm, NULL) < 0 ? 1 : 0;
out:
    snag_shutdown_detach(&shutdown);
    if (entered) leave_screen(vm, true);
    else if (captured) (void)snag_term_input_restore(&vm->terminal, false);
    if (controls) snag_term_controls_restore(&vm->terminal);
    snag_shutdown_finish(&shutdown);
    if (vm->output >= 0) (void)close(vm->output);
    vm->output = -1;
    snag_term_host_close(&vm->terminal);
    if (rc < 0) {
        (void)fprintf(stderr, "snajpagent: workspace terminal failed: %s\n", strerror(errno));
    } else if (rc) {
        (void)fprintf(stderr, "snajpagent: %s\n", vm->message);
    }
    return rc < 0 ? 1 : rc;
}

static int
list_workspaces(struct vm *vm, char *error, size_t size)
{
    json_t *rows = snag_vm_workspace_list(&vm->store, vm->stored_limit, error, size);
    if (!rows) return -1;
    (void)puts("ID\tNAME\tSTATUS\tACTIVITY_MS");
    for (size_t i = 0u; i < json_array_size(rows); ++i) {
        json_t *row = json_array_get(rows, i);
        const char *name = snag_json_string(row, "name"), *problem = snag_json_string(row, "error");
        (void)printf("%s\t%s\t%s\t%lld%s%s\n", snag_json_string(row, "id"), name ? name : "",
            problem ? "error" : json_is_true(json_object_get(row, "open")) ? "open" : "stored",
            (long long)json_integer_value(json_object_get(row, "activity_ms")),
            problem ? "\t" : "", problem ? problem : "");
    }
    json_decref(rows);
    return ferror(stdout) ? -1 : 0;
}

static void
usage(void)
{
    (void)puts("Usage: snajpagent vm [--dotdir DIR] [-N NAME | --resume WORKSPACE | --last]\n"
        "                         [--session SESSION_ID]\n"
        "       snajpagent vm [--dotdir DIR] -l [STORED_COUNT]\n"
        "       snajpagent vm --help | --version\n\n"
        "Open a Vim workspace with the session picker; :help lists available controls.\n"
        "-l lists saved workspaces, with all open rows and ten stored rows by default.");
}

int
snag_vm_main(int argc, char **argv, const char *program)
{
    if (!snag_text_locale_init()) return 1;
    const char *dotdir_option = NULL, *name = NULL, *resume = NULL;
    bool list = false, last = false;
    struct vm vm = {.output = -1, .stored_limit = 10u, .next_window = 2u, .mouse = true,
        .clipboard = {.output = -1, .pending_fd = -1}};
    char error[sizeof(vm.message)] = "invalid workspace arguments";
    int rc = 2;
    snag_store_init(&vm.store);
    snag_config_init(&vm.config);
    vm.workspace = calloc(1u, sizeof(*vm.workspace));
    if (!vm.workspace) return 1;
    snag_vm_workspace_init(vm.workspace);
    snag_buf_init(&vm.command, SNAG_MAX_DIRECT_PROMPT + 1u);
    snag_buf_init(&vm.paste, SNAG_MAX_DIRECT_PROMPT);
    snag_buf_init(&vm.classic.bytes, SNAG_MAX_DIRECT_PROMPT);
    for (int i = 0; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            usage();
            rc = 0;
            goto out;
        }
        if (!strcmp(arg, "--version") || !strcmp(arg, "-V")) {
            (void)puts(SNAJPAGENT_IDENTITY);
            rc = 0;
            goto out;
        }
        if (!strcmp(arg, "--dotdir") && !dotdir_option && i + 1 < argc) dotdir_option = argv[++i];
        else if (!strcmp(arg, "-N") && !name && i + 1 < argc) name = argv[++i];
        else if (!strcmp(arg, "--resume") && !resume && i + 1 < argc) resume = argv[++i];
        else if (!strcmp(arg, "--session") && !vm.start_session && i + 1 < argc)
            vm.start_session = argv[++i];
        else if (!strcmp(arg, "--last") && !last) last = true;
        else if ((!strcmp(arg, "-l") || !strcmp(arg, "--list")) && !list) {
            list = true;
            if (i + 1 < argc && argv[i + 1][0] != '-' &&
                snag_parse_count(argv[++i], &vm.stored_limit) < 0) goto invalid;
        } else goto invalid;
    }
    if ((unsigned int)(name != NULL) + (resume != NULL) + last + list > 1u ||
        (list && vm.start_session)) goto invalid;
    if (!list && (!snag_isatty(STDIN_FILENO) || !snag_isatty(STDOUT_FILENO) ||
        !snag_term_host_capable())) {
        (void)snprintf(error, sizeof(error), "vm requires an interactive terminal");
        goto failed;
    }
    char *dotdir = snag_app_dotdir(dotdir_option, error, sizeof(error));
    if (!dotdir) goto failed;
    int opened = snag_store_open(&vm.store, dotdir, error, sizeof(error));
    free(dotdir);
    if (opened < 0) goto failed;
    if (list) {
        rc = list_workspaces(&vm, error, sizeof(error));
        if (rc < 0) goto failed;
        goto out;
    }
    vm.program = snag_program_path(program);
    if (!vm.program) goto failed;
    vm.windows = calloc(1u, sizeof(*vm.windows));
    vm.layout = snag_vm_layout_new(1u);
    if (!vm.windows || !vm.layout) goto failed;
    vm.count = 1u;
    vm.windows[0].id = 1u;
    if (snag_config_load(&vm.config, NULL, vm.store.root_path, error, sizeof(error)) < 0 ||
        snag_secret_set_build(&vm.secrets, &vm.config, NULL, error, sizeof(error)) < 0) goto failed;
    vm.grid.color = vm.config.color == SNAG_COLOR_ALWAYS ||
        (vm.config.color == SNAG_COLOR_AUTO && !getenv("NO_COLOR"));
    vm.page.reader = snag_vm_reader_open(&vm.store, &vm.secrets.wire, error, sizeof(error));
    if (!vm.page.reader) goto failed;
    vm.scan.reader = snag_vm_reader_open(&vm.store, &vm.secrets.wire, error, sizeof(error));
    if (!vm.scan.reader) goto failed;
    notice(&vm, "NORMAL  :help for controls  / filter  R refresh");
    if (resume || last) {
        if (restore_workspace(&vm, resume) < 0) {
            (void)snprintf(error, sizeof(error), "%s", vm.message);
            goto failed;
        }
    } else if (name && save(&vm, name) < 0) {
        (void)snprintf(error, sizeof(error), "%s", vm.message);
        goto failed;
    }
    if (name) vm.meaningful = true;
    rc = interactive(&vm);
    /* Signal-driven shutdown must finish even when terminal output is stalled. */
    if (rc == 0 && !stopped && vm.workspace->id[0]) {
        char *command = snag_app_resume_command(vm.program, vm.store.root_path,
            vm.workspace->id, true);
        if (command) (void)fprintf(stderr,
            "• You can resume this workspace with the following command%s:\n%s\n",
            snag_command_shell_note(), command);
        free(command);
    }
    goto out;
invalid:
    usage();
failed:
    (void)fprintf(stderr, "snajpagent: %s\n", error);
    rc = 2;
out:
    while (vm.launches) {
        struct vm_launch *job = vm.launches;
        vm.launches = job->next;
        if (job->fd >= 0) (void)close(job->fd);
        snag_session_launch_reap(&job->child);
        snag_view_channel_close(&job->channel);
        snag_app_direct_free(job->direct);
        free(job);
    }
    free(vm.program);
    free(vm.search_query);
    free(vm.completion_prefix);
    snag_vm_reader_close(vm.page.reader);
    snag_vm_reader_close(vm.scan.reader);
    snag_vm_connections_free(vm.connections);
    if (vm.switch_workspace) {
        snag_vm_workspace_close(vm.switch_workspace);
        free(vm.switch_workspace);
    }
    snag_vm_workspace_close(vm.workspace);
    free(vm.workspace);
    snag_vm_layout_free(vm.layout);
    windows_free(vm.windows, vm.count);
    snag_vm_grid_free(&vm.grid);
    snag_buf_free(&vm.command);
    snag_buf_free(&vm.paste);
    clear_pending_input(&vm);
    restore_motion(&vm, false);
    snag_vm_register_free(&vm.reg);
    snag_buf_free(&vm.classic.bytes);
    json_decref(vm.sessions);
    json_decref(vm.workspaces);
    json_decref(vm.buffers);
    snag_secret_set_free(&vm.secrets);
    snag_config_free(&vm.config);
    snag_store_close(&vm.store);
    return rc;
}
#else
int
snag_vm_main(int argc, char **argv, const char *program)
{
    (void)argc;
    (void)argv;
    (void)program;
    (void)fprintf(stderr, "snajpagent: Vim mode is unavailable in this build (WITH_VM=0)\n");
    return 2;
}
#endif /* SNAJPAGENT_VM */
