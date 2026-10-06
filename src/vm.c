/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm.h"
#include "app.h"
#include "config.h"
#include "irc_address.h"
#include "json.h"
#include "secret.h"
#include "snajpagent.h"
#include "term_host.h"
#include "vm_connection.h"
#include "vm_editor.h"
#include "vm_grid.h"
#include "vm_input.h"
#include "vm_layout.h"
#include "vm_reader.h"
#include "vm_text.h"
#include "vm_workspace.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
#include <poll.h>
#endif

#if SNAJPAGENT_VM
enum view_kind { VIEW_SESSIONS, VIEW_WORKSPACES, VIEW_HELP, VIEW_TRANSCRIPT };
enum history_load { LOAD_NONE, LOAD_FIRST, LOAD_LAST, LOAD_PREVIOUS, LOAD_NEXT,
    LOAD_KEEP, LOAD_ANCHOR, LOAD_REFRESH, LOAD_POLL };
static const char *const view_names[] = {"sessions", "workspaces", "help", "transcript"};
static const char *const help_rows[] = {
    "j/k or arrows: select    gg/G: first/last    Ctrl-D/U: half page",
    "/: filter picker    R: refresh    Ctrl-L: redraw    Ctrl-Z: suspend",
    ":sessions    :workspaces    :help    o: open selected history",
    ":history SESSION_ID    :verbosity 0..6    R: refresh current history",
    "History: gg/G oldest/newest, j/k and Ctrl-D/U cross retained source pages",
    ":split or :sp    :vsplit or :vsp    :close    :q    :qa",
    "Ctrl-W s/v: split    Ctrl-W w/h/j/k/l: focus    Ctrl-W =: equalize",
    "Ctrl-W +/-: height    Ctrl-W >/<: width    Ctrl-W q: close",
    ":workspace    :workspace name NAME    :workspace save",
    "Workspace names accept quoted text. :q in these pickers closes the window.",
    "Enter/:attach SESSION: control owner    o/:history SESSION: read-only",
    "i/a/A: edit prompt    Esc: NORMAL    Enter: submit    Ctrl-J: newline",
    "Composer: h/j/k/l w/b/e 0/^/$ gg/G; counts multiply (2d3w deletes six words).",
    "gj/gk: wrapped rows    H/M/L: visible rows    zz: center    Ctrl-U/D/B/F: pages",
    "i/a/I/A/o/O: INSERT    d/c/y + motion, dd/cc/yy, x, p/P    u/Ctrl-R: undo/redo",
    ":detach: preserve owner    :q/:qa: quit controlled owners    :recover: recover submission",
    "Draft conflict: :draft local keeps this copy; :draft owner uses the owner's copy.",
    "Workspace selection restores its layout. Bracketed paste never runs commands."
};

struct vm_window {
    uint64_t id;
    enum view_kind kind;
    size_t selected, top, composer_top;
    bool center_composer;
    char selected_id[SNAG_ID_HEX_LEN + 1u];
    char *filter;
    struct snag_vm_rectangle rectangle;
    struct snag_vm_document *document;
    char session_id[SNAG_ID_HEX_LEN + 1u], anchor_key[160];
    uint64_t anchor_seq;
    size_t anchor_byte;
    bool anchor_heading, incomplete, follow, source_failed;
    uint64_t follow_at;
    unsigned int verbosity;
    enum history_load load;
    struct snag_journal_cursor begin, end, tail;
};

struct vm {
    struct snag_store store;
    struct snag_config config;
    struct snag_secret_set secrets;
    struct snag_vm_workspace *workspace, *switch_workspace;
    struct snag_vm_reader *reader;
    struct snag_vm_connection *connections;
    struct snag_vm_layout *layout;
    struct vm_window *windows;
    size_t count, focus;
    uint64_t next_window, generation, stored_limit, save_at, reading_window;
    enum history_load reading_load;
    json_t *sessions, *workspaces;
    struct snag_term_host terminal;
    struct snag_vm_grid grid;
    struct snag_vm_input input;
    struct snag_buf command, paste;
    struct snag_vm_register reg;
    size_t command_cursor;
    char mode, prefix;
    char message[512];
    int output;
    bool dirty, save_dirty, meaningful, quit, suspend, entering, paste_failed;
    bool composer, insert, quit_all, detach_exit, detach_suspend;
    uint64_t quit_window;
    size_t cursor_row, cursor_column;
};

static volatile sig_atomic_t stopped, resized;

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

static bool
connection_visible(const struct vm *vm, const struct snag_vm_connection *c)
{
    for (size_t i = 0u; i < vm->count; ++i)
        if (!strcmp(vm->windows[i].session_id, c->session)) return true;
    return false;
}

static void
detach_unused(struct vm *vm)
{
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next) {
        if (connection_visible(vm, c)) continue;
        c->control = false;
        if (!c->detaching) snag_vm_connection_detach(c);
    }
}

static void
windows_free(struct vm_window *windows, size_t count)
{
    for (size_t i = 0u; i < count; ++i) {
        free(windows[i].filter);
        snag_vm_document_free(windows[i].document);
    }
    free(windows);
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
    window->anchor_byte = row.begin;
    window->anchor_heading = row.heading;
}

static void
queue_history(struct vm *vm, struct vm_window *window, enum history_load load)
{
    if (load == LOAD_KEEP && (window->load || window->source_failed)) return;
    if (load == LOAD_FIRST || load == LOAD_LAST || load == LOAD_REFRESH) {
        if (window->source_failed) notice(vm, "Loading retained history");
        window->source_failed = false;
    }
    if (load == LOAD_KEEP || load == LOAD_REFRESH) remember_anchor(window);
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
            json_t *history = json_pack("{s:s,s:s,s:I,s:I,s:b,s:i,s:b}",
                "session", window->session_id, "key", window->anchor_key,
                "seq", (json_int_t)window->anchor_seq, "byte", (json_int_t)window->anchor_byte,
                "heading", window->anchor_heading, "verbosity", (int)window->verbosity,
                "follow", window->follow);
            if (!history || json_object_set_new(row, "history", history) < 0) {
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
    return json_pack("{s:i,s:I,s:o,s:o,s:o}", "v", 4,
        "focus", (json_int_t)vm->windows[vm->focus].id, "layout", layout,
        "windows", windows, "buffers", buffers);
}

static int
state_restore(struct vm *vm, const json_t *state, char *error, size_t size)
{
    struct snag_vm_layout *layout = NULL;
    struct vm_window *windows = NULL;
    struct snag_vm_connection *connections = NULL;
    uint64_t focus_id = 0u, next = 0u;
    json_int_t version = json_integer_value(json_object_get(state, "v"));
    size_t count = json_array_size(json_object_get(state, "windows")), focus = SIZE_MAX;
    if (!snag_json_exact_keys(state, version >= 3 ? "v focus layout windows buffers" :
        "v focus layout windows") || (version < 1 || version > 4) || !count ||
        (version >= 3 &&
         snag_vm_connections_load(json_object_get(state, "buffers"), &connections) < 0) ||
        snag_json_integer_u64(state, "focus", &focus_id) < 0 ||
        !(layout = snag_vm_layout_load(json_object_get(state, "layout"), error, size)) ||
        snag_vm_layout_count(layout) != count || count > SIZE_MAX / sizeof(*windows) ||
        !(windows = calloc(count, sizeof(*windows)))) goto invalid;
    for (size_t i = 0u; i < count; ++i) {
        json_t *row = json_array_get(json_object_get(state, "windows"), i);
        const char *kind = snag_json_string(row, "kind");
        const char *selected = snag_json_string(row, "selected");
        const char *filter = snag_json_string(row, "filter");
        uint64_t top, selected_row;
        struct vm_window *window = &windows[i];
        bool history = kind && !strcmp(kind, "transcript") && version >= 2;
        if (!snag_json_exact_keys(row, history ? "id kind selected top row filter history" :
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
        while (k < 4u && strcmp(kind, view_names[k])) ++k;
        if (k == 4u || (k == VIEW_TRANSCRIPT && !history)) goto invalid;
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
            if (!snag_json_exact_keys(saved, following ?
                "session key seq byte heading verbosity follow" :
                "session key seq byte heading verbosity") ||
                (following && !json_is_boolean(json_object_get(saved, "follow"))) ||
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
            window->verbosity = (unsigned int)level;
            window->follow = json_is_true(json_object_get(saved, "follow"));
            window->load = !window->follow && window->anchor_seq ? LOAD_ANCHOR : LOAD_LAST;
        }
        if (window->id == focus_id) focus = i;
        if (window->id > next) next = window->id;
    }
    if (focus == SIZE_MAX || next == INT64_MAX) goto invalid;
    snag_vm_connections_free(vm->connections);
    vm->connections = connections;
    windows_free(vm->windows, vm->count);
    snag_vm_layout_free(vm->layout);
    vm->windows = windows;
    vm->layout = layout;
    vm->count = count;
    vm->focus = focus;
    vm->next_window = next + 1u;
    vm->composer = vm->insert = false;
    vm->quit_all = false;
    vm->quit_window = 0u;
    vm->dirty = true;
    return 0;
invalid:
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
reader_request(struct vm *vm, struct snag_vm_read_request *request)
{
    json_t *sources = json_array();
    if (!sources) return 0u;
    for (size_t i = 0u; i < vm->count; ++i) {
        if (vm->windows[i].kind == VIEW_TRANSCRIPT &&
            json_array_append_new(sources, json_string(vm->windows[i].session_id)) < 0) {
            json_decref(sources);
            return 0u;
        }
    }
    request->retained_sessions = sources;
    uint64_t generation = snag_vm_reader_request(vm->reader, request);
    json_decref(sources);
    request->retained_sessions = NULL;
    return generation;
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
    struct snag_vm_read_request request = {.kind = SNAG_VM_READ_SESSIONS,
        .stored_limit = vm->stored_limit};
    vm->generation = reader_request(vm, &request);
    vm->reading_window = 0u;
    if (!vm->generation) notice(vm, "Cannot start session list loading");
    vm->dirty = true;
}

static int
install_workspace(struct vm *vm, struct snag_vm_workspace *next)
{
    char error[256];
    int rc = save(vm, NULL);
    if (!rc) {
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
    notice(vm, "Workspace restored");
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next)
        if (connection_visible(vm, c)) (void)snag_vm_connection_open(c, &vm->store, c->control);
    refresh(vm);
    return 0;
}

static int
restore_workspace(struct vm *vm, const char *selector)
{
    struct snag_vm_workspace *next = calloc(1u, sizeof(*next));
    if (!next) return -1;
    snag_vm_workspace_init(next);
    char error[256];
    struct vm checked = {0};
    int rc = snag_vm_workspace_open(next, &vm->store, selector, error, sizeof(error));
    if (!rc) rc = state_restore(&checked, json_object_get(next->snapshot, "state"),
        error, sizeof(error));
    snag_vm_connections_free(checked.connections);
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
        if (c->channel.fd < 0) continue;
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
    if (copy.kind == VIEW_TRANSCRIPT && !copy.document)
        copy.load = copy.anchor_seq ? LOAD_ANCHOR : LOAD_LAST;
    copy.id = vm->next_window++;
    vm->windows[vm->count] = copy;
    vm->focus = vm->count++;
    changed(vm);
}

static void
close_window(struct vm *vm)
{
    if (vm->count == 1u) {
        if (save(vm, NULL) == 0) {
            vm->detach_exit = true;
            for (struct snag_vm_connection *c = vm->connections; c; c = c->next)
                snag_vm_connection_detach(c);
        }
        return;
    }
    if (snag_vm_layout_close(vm->layout, vm->windows[vm->focus].id) < 0) return;
    free(vm->windows[vm->focus].filter);
    snag_vm_document_free(vm->windows[vm->focus].document);
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
    struct vm_window *window = &vm->windows[vm->focus];
    if (vm->reading_window == window->id) {
        snag_vm_reader_cancel(vm->reader);
        vm->generation = vm->reading_window = 0u;
    }
    snag_vm_document_free(window->document);
    window->document = NULL;
    window->load = LOAD_NONE;
    window->follow = window->source_failed = false;
    window->follow_at = 0u;
    window->session_id[0] = window->anchor_key[0] = 0;
    window->anchor_seq = 0u;
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
        window->verbosity = 1u;
        window->follow = true;
        queue_history(vm, window, LOAD_LAST);
        notice(vm, "Read-only retained history  gg/G: oldest/newest  :verbosity 0..6");
    }
    snag_session_close(&location);
    detach_unused(vm);
    return opened;
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
    if (!c->bound) (void)snag_vm_connection_open(c, &vm->store, true);
    notice(vm, c->message);
    changed(vm);
}

static void
quit_sessions(struct vm *vm, bool all, bool force)
{
    struct snag_vm_connection *focused = focused_connection(vm);
    bool any = false;
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next) {
        if (!c->bound || (!all && c != focused)) continue;
        if (!force && (c->draft.len || c->pending || c->draft_conflict)) {
            notice(vm, "Unsent draft or unresolved submission; :close detaches, :q! discards");
            return;
        }
        if (!force && c->drafts && !c->draft_ready) {
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
        if (force) {
            snag_vm_editor_reset(&c->editor);
            snag_buf_reset(&c->draft);
            c->cursor = 0u;
            json_decref(c->pending);
            c->pending = NULL;
            json_decref(c->conflict_draft);
            c->conflict_draft = NULL;
            c->draft_conflict = false;
            c->send_pending = c->reconcile_pending = c->draft_dirty = false;
        }
        if (snag_vm_connection_control(c, "quit") < 0) {
            notice(vm, "Cannot request session shutdown");
            return;
        }
        any = true;
    }
    if (any) {
        vm->quit_all = all;
        vm->quit_window = all ? 0u : vm->windows[vm->focus].id;
        notice(vm, "Waiting for session shutdown; :detach stops waiting");
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
        if (window->kind != VIEW_TRANSCRIPT || !window->load || !window->rectangle.visible)
            continue;
        if (vm->generation && (i || !vm->reading_window || window->load == LOAD_POLL)) return;
        struct snag_vm_read_request request = {.kind = SNAG_VM_READ_HISTORY, .project = true,
            .verbosity = window->verbosity, .columns = window->rectangle.columns};
        memcpy(request.session_id, window->session_id, sizeof(request.session_id));
        enum history_load load = window->load;
        if (load == LOAD_FIRST || load == LOAD_LAST || load == LOAD_REFRESH || load == LOAD_POLL)
            request.refresh = true;
        if (load == LOAD_POLL) {
            request.if_changed = true;
            request.tail_only = !window->follow;
            request.tail = window->tail;
        }
        if (load == LOAD_LAST || load == LOAD_PREVIOUS || load == LOAD_ANCHOR ||
            load == LOAD_POLL) {
            request.reverse = true;
            if (load == LOAD_PREVIOUS) request.before_seq = window->begin.next_seq;
            if (load == LOAD_ANCHOR) request.before_seq = window->anchor_seq + 1u;
        } else if (load == LOAD_NEXT) request.cursor = window->end;
        else if (load == LOAD_KEEP || load == LOAD_REFRESH) request.cursor = window->begin;
        vm->generation = reader_request(vm, &request);
        if (!vm->generation) notice(vm, "Cannot start history read");
        vm->reading_window = window->id;
        vm->reading_load = load;
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
        struct snag_vm_connection *c = focused_connection(vm);
        if (strcmp(rest, "local") && strcmp(rest, "owner"))
            notice(vm, "Use :draft local or :draft owner to resolve a draft conflict");
        else if (!c || snag_vm_draft_choose(c, !strcmp(rest, "local")) < 0)
            notice(vm, "No ready draft conflict; recover any retained submission first");
        else {
            notice(vm, c->message);
            changed(vm);
        }
    } else if (!strcmp(word, "history") && *rest) open_history(vm, rest);
    else if (!strcmp(word, "attach")) attach(vm, rest);
    else if (!strcmp(word, "verbosity")) {
        struct vm_window *window = &vm->windows[vm->focus];
        uint64_t level;
        if (window->kind != VIEW_TRANSCRIPT || snag_parse_count(rest, &level) < 0 ||
            level > SNAG_VERBOSITY_MAX) notice(vm, "Use :verbosity 0..6 in a transcript");
        else {
            window->verbosity = (unsigned int)level;
            queue_history(vm, window, LOAD_KEEP);
            changed(vm);
        }
    } else if (*rest) notice(vm, "Unexpected command argument");
    else if (!strcmp(word, "close")) close_window(vm);
    else if (!strcmp(word, "q") || !strcmp(word, "q!")) quit_sessions(vm, false, word[1] == '!');
    else if (!strcmp(word, "qa") || !strcmp(word, "qa!"))
        quit_sessions(vm, true, word[2] == '!');
    else if (!strcmp(word, "detach")) {
        struct snag_vm_connection *c = focused_connection(vm);
        if (c) {
            c->control = false;
            snag_vm_connection_detach(c);
            vm->composer = vm->insert = false;
            vm->quit_window = 0u;
            notice(vm, c->quitting ? "Detached from pending shutdown" :
                "Detached; owner continues running");
            changed(vm);
        }
    } else if (!strcmp(word, "recover")) {
        struct snag_vm_connection *c = focused_connection(vm);
        if (!c || snag_vm_connection_recover(c) < 0)
            notice(vm, "Recovery needs an empty draft and a resolved or disconnected submission");
        else {
            vm->composer = true;
            notice(vm, c->message);
            changed(vm);
        }
    } else if (!strcmp(word, "split") || !strcmp(word, "sp")) split(vm, SNAG_VM_HORIZONTAL);
    else if (!strcmp(word, "vsplit") || !strcmp(word, "vsp")) split(vm, SNAG_VM_VERTICAL);
    else if (!strcmp(word, "sessions")) view(vm, VIEW_SESSIONS);
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
    return window->kind == VIEW_SESSIONS ? vm->sessions : vm->workspaces;
}

static bool
matches(const struct vm_window *window, const json_t *row)
{
    const char *filter = window->filter;
    if (!filter || !*filter) return true;
    const char *id = snag_json_string(row, "id"), *name = snag_json_string(row, "name");
    if ((id && strstr(id, filter)) || (name && strstr(name, filter))) return true;
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
    if (window->kind == VIEW_TRANSCRIPT) return snag_vm_document_rows(window->document);
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
    struct vm_window *window = &vm->windows[vm->focus];
    size_t count = row_count(vm, window);
    window->selected = !count ? 0u : at < count ? at : count - 1u;
    if (window->kind == VIEW_TRANSCRIPT) {
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
        int64_t y = (int64_t)to->row * 2 + to->rows - ((int64_t)from->row * 2 + from->rows);
        int64_t x = (int64_t)to->column * 2 + to->columns -
            ((int64_t)from->column * 2 + from->columns);
        if (!to->visible || (key == 'h' && x >= 0) || (key == 'l' && x <= 0) ||
            (key == 'k' && y >= 0) || (key == 'j' && y <= 0)) continue;
        uint64_t score = (uint64_t)llabs(x) + (uint64_t)llabs(y);
        if (score < distance) {
            distance = score;
            best = i;
        }
    }
    if (best != vm->focus) {
        vm->focus = best;
        changed(vm);
    }
}

static int
insert_command(struct vm *vm, const void *text, size_t size)
{
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
edit_draft(struct vm *vm, size_t begin, size_t end, const void *text, size_t size)
{
    struct snag_vm_connection *c = focused_connection(vm);
    if (!c) return 0;
    if (snag_vm_editor_replace(c, begin, end, text, size) < 0)
        notice(vm, "Cannot edit draft: invalid text or input limit reached");
    else changed(vm);
    return 0;
}

static void
submit_draft(struct vm *vm)
{
    struct snag_vm_connection *c = focused_connection(vm);
    if (!c) return;
    if (snag_vm_connection_prepare(c) < 0) {
        notice(vm, c->pending ? "Previous submission still retained; inspect its receipt" :
            c->draft_conflict ? "Resolve the draft conflict with :draft local or :draft owner" :
            !c->bound ? "Read-only session; :attach to submit" : "Draft is empty");
        return;
    }
    changed(vm);
    if (save(vm, NULL) < 0) return;
    if (snag_vm_connection_send(c) < 0) notice(vm, "Submission was not sent; :recover its text");
    else notice(vm, c->message);
}

static bool
composer_key(struct vm *vm, const struct snag_vm_input_event *event)
{
    struct snag_vm_connection *c = focused_connection(vm);
    if (!vm->composer || !c) return false;
    const struct snag_vm_rectangle *r = &vm->windows[vm->focus].rectangle;
    enum snag_vm_edit_result result = snag_vm_editor_key(c, &vm->reg, event,
        vm->insert, r->columns, (r->rows > 1u ? r->rows - 1u : 0u) / 3u + 1u,
        vm->windows[vm->focus].composer_top);
    if (result == SNAG_VM_EDIT_UNUSED) return false;
    if (result == SNAG_VM_EDIT_ERROR) {
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

static int
input_event(void *opaque, const struct snag_vm_input_event *event)
{
    struct vm *vm = opaque;
    if (vm->detach_exit || vm->detach_suspend || vm->switch_workspace) return 0;
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
        struct snag_vm_connection *c = focused_connection(vm);
        if (vm->composer && vm->insert && c) {
            if (snag_vm_editor_end(c) < 0) return -1;
            (void)edit_draft(vm, c->cursor, c->cursor, vm->paste.data, vm->paste.len);
            return snag_vm_editor_end(c);
        }
        notice(vm, "Enter INSERT, command or filter input before pasting text");
        return 0;
    }
    if (event->kind == SNAG_VM_MOUSE) {
        if (event->release) return 0;
        struct snag_vm_connection *c = focused_connection(vm);
        if (vm->insert && c) {
            snag_vm_editor_normal(c, true);
            if (snag_vm_editor_end(c) < 0) return -1;
        } else if (c) snag_vm_editor_normal(c, false);
        for (size_t i = 0u; i < vm->count; ++i) {
            const struct snag_vm_rectangle *r = &vm->windows[i].rectangle;
            if (!r->visible || event->row < r->row || event->row - r->row >= r->rows ||
                event->column < r->column || event->column - r->column >= r->columns) continue;
            vm->focus = i;
            vm->composer = vm->insert = false;
            if (event->button == 64u || event->button == 65u) move(vm, event->button == 65u, 3u);
            else if (event->button == 0u && event->row - r->row + 1u < r->rows)
                selection(vm, vm->windows[i].top + event->row - r->row);
            vm->dirty = true;
            break;
        }
        return 0;
    }
    unsigned int key = event->kind == SNAG_VM_TEXT && event->length == 1u ?
        event->text[0] : event->kind == SNAG_VM_KEY ? event->key : 0u;
    bool control = (event->modifiers & SNAG_VM_CTRL) != 0u;
    /* Terminals encode Alt-text as Escape followed by text. Vim's supported
     * editing subset uses that sequence to leave INSERT/command input first,
     * so a fast Escape-colon cannot become literal prompt text. */
    if (event->kind == SNAG_VM_TEXT && (event->modifiers & SNAG_VM_ALT)) {
        struct snag_vm_connection *c = focused_connection(vm);
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
            else if (window->kind == VIEW_TRANSCRIPT) {
                notice(vm, "Transcript search is unavailable in this development build");
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
    if (vm->prefix != 'w' && composer_key(vm, event)) return 0;
    if (key == SNAG_VM_KEY_ESCAPE) vm->prefix = 0;
    else if (control && key == 'z') {
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
        struct snag_vm_connection *c = focused_connection(vm);
        if (c && snag_vm_connection_control(c, "cancel") == 0)
            notice(vm, "Interrupt requested");
    }
    else if (vm->prefix == 'w') {
        vm->prefix = 0;
        if (key == 's' || key == 'v') split(vm, key == 'v' ? SNAG_VM_VERTICAL : SNAG_VM_HORIZONTAL);
        else if (key == 'q' || key == 'c') close_window(vm);
        else if (key == 'w') {
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
    } else if (key == ':' || key == '/') {
        vm->mode = (char)key;
        vm->prefix = 0;
        snag_buf_reset(&vm->command);
        vm->command_cursor = 0u;
        vm->dirty = true;
    } else if (key == 'g') {
        if (vm->prefix == 'g') {
            if (window->kind == VIEW_TRANSCRIPT) {
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
        } else if (key == 'G') {
            if (window->kind == VIEW_TRANSCRIPT) {
                window->follow = true;
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
            if (window->kind == VIEW_TRANSCRIPT) queue_history(vm, window, LOAD_REFRESH);
            else refresh(vm);
        }
        else if (key == 'o' && window->kind == VIEW_SESSIONS) {
            const char *id = snag_json_string(selected_row(vm, window), "id");
            if (id) open_history(vm, id);
        }
        else if (key == SNAG_VM_KEY_ENTER) {
            json_t *row = selected_row(vm, window);
            const char *id = snag_json_string(row, "id");
            if (id && window->kind == VIEW_WORKSPACES) (void)restore_workspace(vm, id);
            else if (id && window->kind == VIEW_SESSIONS)
                attach(vm, id);
        }
    }
    return 0;
}

static int
input_ready(struct vm *vm, int timeout)
{
#ifdef _WIN32
    int ready = snag_term_input_wait(&vm->terminal, snag_vm_reader_fd(vm->reader), timeout);
#else
    size_t count = 2u;
    uint64_t now = snag_monotonic_ms();
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next) {
        if (c->channel.fd >= 0) ++count;
        timeout = snag_vm_connection_wait(c, now, timeout);
    }
    struct pollfd *fds = calloc(count, sizeof(*fds));
    if (!fds) return -1;
    fds[0] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};
    fds[1] = (struct pollfd){.fd = snag_vm_reader_fd(vm->reader), .events = POLLIN};
    size_t at = 2u;
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next) {
        if (c->channel.fd < 0) continue;
        fds[at++] = (struct pollfd){.fd = c->channel.fd,
            .events = POLLIN | (c->channel.output ? POLLOUT : 0)};
    }
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
        if (count > 0 && snag_vm_input_feed(&vm->input, bytes, (size_t)count,
            snag_monotonic_ms(), input_event, vm) < 0) return -1;
    }
    return snag_vm_input_expire(&vm->input, snag_monotonic_ms(), input_event, vm);
}

static void
connections_step(struct vm *vm)
{
    bool waiting = false, connected = false;
    for (struct snag_vm_connection *c = vm->connections; c; c = c->next) {
        uint64_t revision = c->revision;
        char previous[sizeof(c->message)];
        memcpy(previous, c->message, sizeof(previous));
        snag_vm_connection_step(c);
        if (revision != c->revision) {
            if (focused_connection(vm) == c && strcmp(previous, c->message))
                notice(vm, c->message);
            changed(vm);
        }
        if (c->quitting && !c->exited && c->control) waiting = true;
        if (c->channel.fd >= 0) connected = true;
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
    if (waiting) return;
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

struct draft_rows {
    struct vm *vm;
    struct vm_window *window;
    struct snag_vm_connection *connection;
    size_t count, cursor_row, top, height, first_row;
    bool paint;
};

static int
draft_row(void *opaque, const struct snag_vm_text_row *row)
{
    struct draft_rows *rows = opaque;
    struct snag_vm_connection *c = rows->connection;
    size_t index = rows->count++;
    if (c->cursor >= row->begin && c->cursor <= row->end) rows->cursor_row = index;
    if (!rows->paint || index < rows->top || index - rows->top >= rows->height) return 0;
    const struct snag_vm_rectangle *r = &rows->window->rectangle;
    if (snag_vm_grid_text_column(&rows->vm->grid, rows->first_row + index - rows->top,
        r->column, r->columns, c->draft.len ? (const char *)c->draft.data + row->begin : "",
        row->end - row->begin, 0u, row->logical_column) < 0) return -1;
    if (c->cursor >= row->begin && c->cursor <= row->end &&
        rows->window == &rows->vm->windows[rows->vm->focus]) {
        size_t column = snag_vm_text_column((const char *)c->draft.data,
            c->draft.len, c->cursor, false) - row->logical_column;
        rows->vm->cursor_row = rows->first_row + index - rows->top;
        rows->vm->cursor_column = r->column + (column < r->columns ? column : r->columns - 1u);
    }
    return 0;
}

static int
draw_window(void *opaque, const struct snag_vm_rectangle *rectangle)
{
    struct vm *vm = opaque;
    size_t index = 0u;
    while (index < vm->count && vm->windows[index].id != rectangle->window) ++index;
    if (index == vm->count) return -1;
    struct vm_window *window = &vm->windows[index];
    window->rectangle = *rectangle;
    if (!rectangle->visible) return 0;
    size_t height = rectangle->rows > 1u ? rectangle->rows - 1u : 0u;
    struct snag_vm_connection *c = connection_for(vm, window->session_id, false);
    struct draft_rows draft = {.vm = vm, .window = window, .connection = c};
    bool editing = index == vm->focus && vm->composer && c && !vm->mode;
    if (c && (c->control || c->draft.len || editing) && height > 1u) {
        const char *text = c->draft.len ? (const char *)c->draft.data : "";
        if (snag_vm_text_wrap(text, c->draft.len, rectangle->columns, false,
            draft_row, &draft) < 0) return -1;
        draft.height = height / 3u + 1u;
        if (draft.height > draft.count) draft.height = draft.count;
        if (draft.height >= height) draft.height = height - 1u;
        draft.top = window->composer_top;
        if (window->center_composer) {
            draft.top = draft.cursor_row > draft.height / 2u ?
                draft.cursor_row - draft.height / 2u : 0u;
            window->center_composer = false;
        }
        if (draft.top > draft.count - draft.height) draft.top = draft.count - draft.height;
        if (draft.top > draft.cursor_row) draft.top = draft.cursor_row;
        if (draft.cursor_row - draft.top >= draft.height)
            draft.top = draft.cursor_row - draft.height + 1u;
        window->composer_top = draft.top;
        height -= draft.height;
        draft.first_row = rectangle->row + height;
        draft.count = 0u;
        draft.paint = true;
        if (snag_vm_text_wrap(text, c->draft.len, rectangle->columns, false,
            draft_row, &draft) < 0) return -1;
    }
    size_t count = row_count(vm, window);
    if (window->selected >= count) window->selected = count ? count - 1u : 0u;
    if (window->top > window->selected) window->top = window->selected;
    if (height && window->selected - window->top >= height)
        window->top = window->selected - height + 1u;
    if (window->kind == VIEW_TRANSCRIPT) {
        if (window->document && snag_vm_document_columns(window->document) != rectangle->columns &&
            !window->load && vm->reading_window != window->id) queue_history(vm, window, LOAD_KEEP);
        for (size_t i = window->top; i < count && i - window->top < height; ++i) {
            struct snag_vm_document_row row;
            if (snag_vm_document_row(window->document, i, &row) < 0) return -1;
            const char *text = snag_vm_document_text(window->document, &row);
            unsigned int style = row.heading ? SNAG_VM_BOLD : 0u;
            if (i == window->selected) style |= SNAG_VM_REVERSE;
            if (snag_vm_grid_text_column(&vm->grid, rectangle->row + i - window->top,
                rectangle->column, rectangle->columns, text + row.begin, row.end - row.begin,
                style, row.column) < 0) return -1;
        }
    }
    json_t *rows = view_rows(vm, window);
    size_t visible = 0u;
    size_t available = window->kind == VIEW_HELP ? count : json_array_size(rows);
    for (size_t i = 0u; window->kind != VIEW_TRANSCRIPT && i < available; ++i) {
        json_t *row = json_array_get(rows, i);
        if (window->kind != VIEW_HELP && !matches(window, row)) continue;
        size_t position = visible++;
        if (position < window->top || position - window->top >= height) continue;
        struct snag_buf text = {.max = SNAG_MAX_DIRECT_PROMPT};
        if (window->kind == VIEW_HELP) {
            (void)snag_buf_printf(&text, "%s", help_rows[i]);
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
        if (c && c->bound && c->state) {
            const char *provider = snag_json_string(c->state, "provider");
            const char *model = snag_json_string(c->state, "model");
            const char *effort = snag_json_string(c->state, "effort");
            (void)snprintf(owner, sizeof(owner), " %s/%s/%s %s", provider ? provider : "?",
                model ? model : "?", effort ? effort : "?",
                json_is_true(json_object_get(c->state, "active")) ? "working" : "idle");
        }
        (void)snprintf(status, sizeof(status), "%s%s%s%.8s%s history v%u %s  seq %llu%s%s%s%s%s",
            editing ? vm->insert ? "INSERT " : "NORMAL draft " : "",
            c && c->bound ? "ATTACHED " : "read-only ",
            c && c->draft_conflict ? "[draft conflict] " : "", window->session_id, owner,
            window->verbosity, window->follow ? "FOLLOW" : "HOLD",
            (unsigned long long)window->anchor_seq,
            window->begin.offset ? "  ↑ older" : "  [start]",
            window->end.offset < window->tail.offset ? "  ↓ newer" : "  [tail]",
            window->source_failed ? "  source error; R" :
                window->incomplete ? "  partial tail" : "",
            (window->load && window->load != LOAD_POLL) ||
            (vm->reading_window == window->id && vm->reading_load != LOAD_POLL) ? "  loading" : "",
            c && c->pending ? "  [submission retained]" : "");
    }
    return snag_vm_grid_text(&vm->grid, rectangle->row + rectangle->rows - 1u,
        rectangle->column, rectangle->columns, status, strlen(status),
        index == vm->focus ? SNAG_VM_BOLD | SNAG_VM_REVERSE : SNAG_VM_REVERSE);
}

static int
draw(struct vm *vm)
{
    size_t rows = snag_term_host_rows(), columns = snag_term_host_columns();
    if (!rows) rows = 1u;
    if (!columns) columns = 1u;
    if (snag_vm_grid_resize(&vm->grid, rows, columns) < 0) return -1;
    vm->dirty = false;
    vm->cursor_row = vm->cursor_column = SIZE_MAX;
    snag_vm_grid_begin(&vm->grid);
    if (snag_vm_layout_place(vm->layout, vm->windows[vm->focus].id,
        rows > 1u ? (unsigned int)rows - 1u : 1u, (unsigned int)columns, draw_window, vm) < 0)
        return -1;
    size_t cursor = 0u;
    if (vm->mode) {
        size_t start = 0u;
        size_t column = snag_vm_text_column((const char *)vm->command.data,
            vm->command.len, vm->command_cursor, false);
        if (column + 2u > columns) start = snag_vm_text_at_column((const char *)vm->command.data,
            vm->command.len, 0u, column + 2u - columns, false);
        size_t start_column = snag_vm_text_column((const char *)vm->command.data,
            vm->command.len, start, false);
        if (snag_vm_grid_text(&vm->grid, rows - 1u, 0u, 1u, &vm->mode, 1u, SNAG_VM_BOLD) < 0 ||
            snag_vm_grid_text_column(&vm->grid, rows - 1u, 1u, columns - 1u,
                vm->command.data ? (const char *)vm->command.data + start : "",
                vm->command.len - start, 0u, start_column) < 0) return -1;
        cursor = column - start_column + 1u;
        if (cursor >= columns) cursor = columns - 1u;
    } else if (snag_vm_grid_text(&vm->grid, rows - 1u, 0u, columns,
        vm->message, strlen(vm->message), 0u) < 0) return -1;
    bool editing = vm->composer && focused_connection(vm);
    if (editing && !vm->mode && vm->cursor_row < rows && vm->cursor_column < columns)
        return snag_vm_grid_flush(&vm->grid, vm->cursor_row, vm->cursor_column, true, emit, vm);
    return snag_vm_grid_flush(&vm->grid, rows - 1u, cursor, vm->mode != 0, emit, vm);
}

static void
collect(struct vm *vm)
{
    struct snag_vm_read_result *result = snag_vm_reader_take(vm->reader);
    if (!result) return;
    uint64_t target = vm->reading_window;
    enum history_load load = vm->reading_load;
    vm->generation = vm->reading_window = 0u;
    if (result->error_number) {
        notice(vm, result->error);
        for (size_t i = 0u; i < vm->count; ++i) {
            if (vm->windows[i].id != target) continue;
            vm->windows[i].source_failed = true;
            vm->windows[i].follow_at = UINT64_MAX;
            vm->windows[i].load = LOAD_NONE;
        }
    } else if (target) {
        for (size_t i = 0u; i < vm->count; ++i) {
            struct vm_window *window = &vm->windows[i];
            if (window->id != target || window->kind != VIEW_TRANSCRIPT ||
                strcmp(window->session_id, result->request.session_id)) continue;
            window->follow_at = snag_monotonic_ms() + 1000u;
            if (load == LOAD_POLL &&
                (result->unchanged || result->request.tail_only || !window->follow)) {
                if (!result->unchanged || window->incomplete != result->incomplete)
                    vm->dirty = true;
                window->tail = result->tail;
                window->incomplete = result->incomplete;
                continue;
            }
            struct snag_journal_cursor end = result->request.before_seq ?
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
            snag_vm_document_free(window->document);
            window->document = result->document;
            result->document = NULL;
            size_t count = snag_vm_document_rows(window->document);
            if (load == LOAD_KEEP || load == LOAD_ANCHOR || load == LOAD_REFRESH)
                window->selected = snag_vm_document_locate(window->document, window->anchor_key,
                    window->anchor_seq, window->anchor_byte, window->anchor_heading);
            else if (load == LOAD_LAST || load == LOAD_PREVIOUS || load == LOAD_POLL)
                window->selected = count ? count - 1u : 0u;
            else window->selected = 0u;
            size_t height = window->rectangle.rows > 1u ? window->rectangle.rows - 1u : 1u;
            window->top = window->selected >= height ? window->selected - height + 1u : 0u;
            if (count) remember_anchor(window);
            else if (result->request.reverse && window->begin.offset)
                queue_history(vm, window, LOAD_PREVIOUS);
            else if (!result->request.reverse && window->end.offset < window->tail.offset)
                queue_history(vm, window, LOAD_NEXT);
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
}

static int
enter_screen(struct vm *vm)
{
    static const char modes[] = "\033[?1049h\033[?2004h\033[?1000h\033[?1006h\033[?1004h\033[?25l";
    vm->entering = true;
    int rc = snag_term_input_raw(&vm->terminal);
    if (!rc) rc = snag_term_output_mode(&vm->terminal, true);
    if (!rc) rc = emit(vm, modes, sizeof(modes) - 1u);
    vm->entering = false;
    vm->grid.valid = false;
    vm->dirty = true;
    return rc;
}

static int
restore_checkpoint(void *opaque)
{
    uint64_t *deadline = opaque;
    return snag_monotonic_ms() >= *deadline ? snag_errno(ETIMEDOUT) : 0;
}

static void
leave_screen(struct vm *vm)
{
    static const char modes[] = "\033[0m\033[?25h\033[?1004l\033[?1006l"
        "\033[?1000l\033[?2004l\033[?1049l";
    uint64_t deadline = snag_monotonic_ms() + 250u;
    (void)snag_term_output_write(&vm->terminal, vm->output, modes, sizeof(modes) - 1u,
        false, restore_checkpoint, &deadline);
    (void)snag_term_output_mode(&vm->terminal, false);
    (void)snag_term_input_restore(&vm->terminal, false);
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
    if (enter_screen(vm) < 0) goto out;
    refresh(vm);
    while (!vm->quit && !stopped) {
        connections_step(vm);
        if (vm->quit) break;
        collect(vm);
        if (resized || snag_term_input_resized(&vm->terminal)) {
            resized = 0;
            vm->grid.valid = false;
            vm->dirty = true;
        }
        if (vm->suspend) {
            vm->suspend = false;
            if (snag_term_can_suspend() && save(vm, NULL) == 0) {
                for (struct snag_vm_connection *c = vm->connections; c; c = c->next)
                    snag_vm_connection_close(c);
                leave_screen(vm);
                (void)snag_term_suspend();
                if (enter_screen(vm) < 0) goto out;
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
                !window->load && vm->reading_window != window->id && window->follow_at <= now)
                window->load = LOAD_POLL;
        }
        if (vm->dirty && draw(vm) < 0) {
            if (vm->quit || stopped) break;
            goto out;
        }
        load_history(vm);
        int timeout = snag_vm_input_wait_ms(&vm->input, snag_monotonic_ms());
        if (vm->save_at) {
            uint64_t now = snag_monotonic_ms();
            int remaining = vm->save_at > now ? (int)(vm->save_at - now) : 0;
            if (timeout < 0 || timeout > remaining) timeout = remaining;
        }
        if (!vm->generation) {
            uint64_t now = snag_monotonic_ms();
            for (size_t i = 0u; i < vm->count; ++i) {
                struct vm_window *window = &vm->windows[i];
                if (window->kind != VIEW_TRANSCRIPT || !window->document ||
                    !window->rectangle.visible || window->source_failed) continue;
                int remaining = window->follow_at > now ? (int)(window->follow_at - now) : 0;
                if (timeout < 0 || timeout > remaining) timeout = remaining;
            }
        }
        if (vm->dirty || vm->suspend) timeout = 0;
        if (input_ready(vm, timeout) < 0) goto out;
    }
    rc = save(vm, NULL) < 0 ? 1 : 0;
out:
    snag_shutdown_detach(&shutdown);
    if (entered) leave_screen(vm);
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
        "       snajpagent vm [--dotdir DIR] -l [STORED_COUNT]\n"
        "       snajpagent vm --help | --version\n\n"
        "Open a Vim workspace with the session picker; :help lists available controls.\n"
        "-l lists saved workspaces, with all open rows and ten stored rows by default.");
}

int
snag_vm_main(int argc, char **argv)
{
    const char *dotdir_option = NULL, *name = NULL, *resume = NULL;
    bool list = false, last = false;
    struct vm vm = {.output = -1, .stored_limit = 10u, .next_window = 2u};
    char error[256] = "invalid workspace arguments";
    int rc = 2;
    snag_store_init(&vm.store);
    snag_config_init(&vm.config);
    vm.workspace = calloc(1u, sizeof(*vm.workspace));
    if (!vm.workspace) return 1;
    snag_vm_workspace_init(vm.workspace);
    snag_buf_init(&vm.command, SNAG_MAX_DIRECT_PROMPT + 1u);
    snag_buf_init(&vm.paste, SNAG_MAX_DIRECT_PROMPT);
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
        else if (!strcmp(arg, "--last") && !last) last = true;
        else if ((!strcmp(arg, "-l") || !strcmp(arg, "--list")) && !list) {
            list = true;
            if (i + 1 < argc && argv[i + 1][0] != '-' &&
                snag_parse_count(argv[++i], &vm.stored_limit) < 0) goto invalid;
        } else goto invalid;
    }
    if ((unsigned int)(name != NULL) + (resume != NULL) + last + list > 1u) goto invalid;
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
    vm.windows = calloc(1u, sizeof(*vm.windows));
    vm.layout = snag_vm_layout_new(1u);
    if (!vm.windows || !vm.layout) goto failed;
    vm.count = 1u;
    vm.windows[0].id = 1u;
    if (snag_config_load(&vm.config, NULL, vm.store.root_path, error, sizeof(error)) < 0 ||
        snag_secret_set_build(&vm.secrets, &vm.config, NULL, error, sizeof(error)) < 0) goto failed;
    vm.reader = snag_vm_reader_open(&vm.store, &vm.secrets.wire, error, sizeof(error));
    if (!vm.reader) goto failed;
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
    goto out;
invalid:
    usage();
failed:
    (void)fprintf(stderr, "snajpagent: %s\n", error);
    rc = 2;
out:
    snag_vm_reader_close(vm.reader);
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
    snag_buf_free(&vm.reg.text);
    json_decref(vm.sessions);
    json_decref(vm.workspaces);
    snag_secret_set_free(&vm.secrets);
    snag_config_free(&vm.config);
    snag_store_close(&vm.store);
    return rc;
}
#else
int
snag_vm_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    (void)fprintf(stderr, "snajpagent: Vim mode is unavailable in this build (WITH_VM=0)\n");
    return 2;
}
#endif /* SNAJPAGENT_VM */
