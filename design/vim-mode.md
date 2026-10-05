<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Pager retention and the Vim workspace

Status: proposed implementation design, October 6, 2026. Source inspection is
against `7a70d81d336ef6f14d91ef944b1226c8efa65636`. This document describes the
intended behavior; the `vm` command and semantic attachment described below are
not implemented at that revision.

## 1. Outcome and decisions

Keep receiving and retaining session output while a pager, editor or transfer
owns the terminal. Add an optional full-screen workspace with Vim-style input,
history navigation, search, selection, splits and mouse controls. History comes
from retained session events and remains navigable across context compaction.
Verbosity changes apply to the historical text currently being viewed.

The main decisions are:

- `snajpagent vm` explicitly enters the workspace. Plain `snajpagent` keeps its
  current interface. Output height never activates full-screen mode.
- One workspace process owns the terminal and every window. Existing session
  owner processes continue owning their engines, tools, IRC and journals.
- The workspace renders semantic history. It does not reconstruct history by
  interpreting the current terminal byte stream.
- A new private semantic attachment shares controller ownership with the
  existing terminal attachment. Existing owners and terminal clients remain
  usable throughout the transition.
- All retained journal history is addressable. RAM contains working pages and
  visible layouts; cache eviction never deletes history.
- Explicit yanks update an internal register and attempt workstation clipboard
  delivery. Remote delivery uses the existing negotiated wrapper transport.
- `:q` explicitly quits the focused session; `:detach` preserves its running
  owner. `:close` closes a window. These distinctions are visible in help.
- `WITH_VM=1` includes the module in ordinary and release builds. `WITH_VM=0`
  removes its implementation. Runtime activation remains explicit.

This is a terminal workspace, with a supported Vim subset defined here. It does
not embed Vim, add a scripting language or turn transcript text into an editable
file. The first implementation has one session-host connection domain per
workspace. A remote workspace runs on its session host through the existing
`remote` wrapper.

## 2. Current implementation and the pager failure mechanism

The current source already provides most of the durable foundation:

| Area | Verified behavior at the source revision |
| --- | --- |
| Session execution | `session_host.c`, `session_relay.c` and `session_client.c` separate a surviving POSIX owner with a private PTY from its terminal frontend. Windows currently lacks this native-owner backend. |
| Local attachment | `terminal.sock` uses version-5 framing, one controlling terminal, reservation generations and physical-write acknowledgements. STATUS probes do not attach. |
| Detached display | The relay drains and discards raw PTY output. Reattachment reconstructs a bounded semantic catch-up; raw bytes are not a history store. |
| Public model output | `snag_app_flush_public()` commits complete UTF-8 `response_output` fragments before presentation. |
| Tool and IRC output | `snag_app_tool_output()` commits `process_output`; `snag_app_irc_event()` commits `irc_event` before presenting it. |
| Existing renderer | Render records contain typed content and source references, but `snag_render_flush_view()` consumes records after printing. They are pending queues, not a random-access transcript. |
| Old-history reads | `snag_session_history_open()` opens a verified committed prefix without a writer lock or recovery mutation. Forward/reverse iterators provide bounded work and cursors. |
| Existing `/history` | A report reads a bounded page, currently 4 MiB, and reports earlier history. This is not an all-history viewport. |
| Remote terminal | `remote.c` owns a local PTY wrapper. `screen_wire.c` supplies checked, acknowledged title-state framing for Mosh and a negotiated byte-stream path. Clipboard delivery is absent. |

Source entry points: [app.c](../src/app.c), [app_stream.c](../src/app_stream.c),
[app_events.c](../src/app_events.c), [ui.c](../src/ui.c),
[render.c](../src/render.c), [store.h](../src/store.h),
[session_host.h](../src/session_host.h), [session_relay.c](../src/session_relay.c).

The pager problem has a concrete source-level path. `SNAG_UI_EXTERNAL` stops
terminal input and sets the suspended flag. While the pager waits,
`service_external()` continues servicing tools and IRC. `SNAG_UI_IRC` can still
call the renderer immediately, and the presentation loop's pending-render flush
checks the native attachment barrier without checking external suspension.
Consequently, a record can be printed and consumed while the pager owns the
screen. The pager can overwrite those bytes, and closing it does not replay the
consumed record.

This establishes a defect in display ownership. It does not identify the exact
bytes lost in the reported incident: that incident has not been reproduced or
captured during this design work. The existing active-turn pager test closes the
pager promptly; it does not hold it through sustained background output.

There is also a scheduling constraint: the current pager call blocks the app
controller and runs a limited service callback. That callback does not run the
ordinary provider-turn loop. The fix must cover both display retention and
continued engine progress, without recursively invoking the app controller.

## 3. Fix pager retention first

### 3.1 Separate acceptance from painting

Introduce an explicit terminal lease in the presentation owner. Its states are
`PRESENTING`, `EXTERNAL`, `CATCHING_UP` and `DETACHED`. Only `PRESENTING` and
`CATCHING_UP` may issue ordinary renderer writes. Pager, editor and transfer
protocol output use the current external lease instead.

Retaining an event and painting an event have separate cursors. Acceptance
records a durable source reference, or retains a presentation-only item. Painting
advances only after the output sink accepts the complete logical range. A
partially written range retains its write offset. Code must not mark a render
record displayed merely because an external child is running.

On external entry:

1. Finish the current renderer write boundary, save the presented cursor and
   stop ordinary painting, including IRC, warnings, feedback and delayed flushes.
2. Preserve the composer and stop its input consumer. Transfer terminal modes
   and input ownership to the external child or transfer implementation.
3. Continue journal admission and engine service. Retain newly available ranges
   by source cursor instead of accumulating a text copy for every token.

On external return, restore terminal modes and geometry, enter `CATCHING_UP`,
render through a captured committed tail, then join live output. Events arriving
during catch-up extend the pending range; they cannot overtake it. Work is sliced
so resize, cancellation and attachment controls remain responsive. A long
backlog remains available even if the user interrupts catch-up and opens history.

Classic catch-up can still exceed a terminal's visible height, and Mosh may skip
intermediate screen states. Retained source history is the recovery mechanism;
VM supplies the navigable viewport over it. Successful physical writes alone do
not prove that every historical line reached a client's terminal scrollback.

Generated command reports already have a private temporary-file path for the
pager. Retain that snapshot through its display lifecycle. Presentation-only
notices that lack a journal source use a private spill file during suspension;
the in-memory queue holds offsets. This spool is transient presentation data,
not a second session journal. Close it after successful catch-up. Allocation or
disk errors produce an explicit retention failure through the available status
path; the implementation must never silently advance over dropped records.

### 3.2 Make external jobs asynchronous to the engine

Split the platform external-program primitive into start, poll/service and
finish operations. Preserve its existing process-group, foreground-terminal,
suspend/resume, temporary-file and error-restoration behavior. The app controller
stores the job handle and continues its usual provider/tool/IRC scheduling.
Only terminal interaction is suspended. A worker running the old callback loop
would introduce competing access to the app state and is not an acceptable fix.

Pager spawn failure, child failure, Ctrl-C, Ctrl-Z, terminal loss and an owner
shutdown all converge on one lease cleanup path. A normal external-job cancel
affects that job; the existing explicit session-interrupt action remains
separate. Detach or connection loss releases the actual terminal and preserves
the session owner. Raw external-program screen contents need not be replayed.
Their generated report snapshot and the session transcript remain available.

Keep current pager selection: when available, unset `PAGER` and bare `less` use
`less -X`; explicit pager commands retain their arguments. The retention fix
applies with the VM module compiled out.

## 4. CLI, startup and session selection

Proposed examples:

```sh
snajpagent vm
snajpagent vm --resume SESSION_ID
snajpagent vm --attach SESSION_ID
snajpagent vm -N session-name
snajpagent remote ssh -t host snajpagent vm
snajpagent remote mosh host snajpagent vm --resume SESSION_ID
```

Dispatch `vm` beside `remote` in `main.c`, before normal session startup. Reuse
existing session selectors and compatible model/configuration options. A bare
launch opens the picker and creates no session, provider request or IRC owner.
`--resume` attaches to a live owner or starts a stopped session; `--attach`
requires a live owner. `-N` keeps the existing exact-name behavior. Ambiguous
names and ID prefixes remain errors with visible candidates. An explicit initial
prompt or a new-session action creates a session through the ordinary startup
path. Noninteractive execution flags are rejected by `vm` with a specific
diagnostic; listing, help and version work without entering the alternate screen.

The picker uses the same data and comparator as `snajpagent -l`: attached,
detached, running with unknown attachment state, then stored; newest journal
activity first within each group, with the existing ID tie-break. Initial stored
rows use the CLI's default count, currently ten. Scrolling past them or selecting
“load older sessions” retrieves more. Viewing metadata never acquires control.

Keep ID/name, status, last activity, last prompt and IRC preview, progressively
hiding preview columns on narrow terminals. The selected row stays anchored to
the session ID as live activity reorders other rows. Enter opens a selected live
session or resumes a stored one. A read-only action opens its history without
starting it. A new-session action opens the normal creation flow.

Picker keys are `j`/`k` or arrows to move, Enter to attach/resume, `o` to inspect
read-only, `n` to create, `/` to filter metadata and `R` to refresh. Workspace
commands `:sessions`, `:new [NAME]`, `:buffer SESSION` and `:help` open the picker,
create a session, change the focused buffer or show the supported controls.
Replacing the last window of a controlled buffer detaches it through the same
draft-preserving path as `:close`; a failed destination open retains the source.

An already controlled session opens read-only with its attachment state shown.
An explicit attach attempt may report busy; it never steals another frontend.
The picker reports an unreachable owner as unknown/running rather than guessing
detached. Status freshness is visible when a probe fails.

Runtime opt-in uses the explicit command in this design. A shell alias can make
it convenient. Whether plain startup should be locally redirected on particular
machines is an outstanding preference; it does not block implementation and no
local default changes follow from this note.

## 5. Process, thread and ownership model

```mermaid
flowchart LR
    K["Terminal: keys, mouse, resize"] <--> W["One workspace process\ninput, windows, grid, clipboard"]
    W <--> S1["Session A owner\nengine, tools, IRC, sole journal writer"]
    W <--> S2["Session B owner\nengine, tools, IRC, sole journal writer"]
    S1 --> J1["A: committed journal"]
    S2 --> J2["B: committed journal"]
    J1 --> H["Workspace history worker"]
    J2 --> H
    H --> W
```

The workspace has one event loop owning terminal reads/writes, window layout,
mode transitions, selection and control connections. A background history worker
performs cancellable journal reads, projection and search. Begin with one worker
and prioritize visible-page requests ahead of speculative prefetch and search.
Add concurrency only if measurements show a concrete need. Worker results carry
buffer and layout generations so obsolete results cannot move a newer viewport.

Each session buffer has one backend connection, one shared draft per input route
and one submission state. Windows reference buffers and independently own their
viewport, cursor, verbosity and follow state. Splitting one session does not open
a second engine, writer or controller. Two windows cannot submit one draft twice.

The existing engine remains the sole journal writer. UI navigation does not
replay tools, trigger model requests or enqueue IRC messages. Existing synchronous
`snag_ui_send()` uses borrowed data and an engine/UI rendezvous; its pointer must
not be retained in a new asynchronous queue. New notifications own their small
payloads or refer to immutable committed ranges.

When used remotely, the workspace and history worker run on the session host.
The tunnel carries terminal state and input; multi-gigabyte history remains on
the host and is read there. The workstation wrapper handles workstation effects
such as clipboard publication. Multiple host domains can use separate workspaces
or an existing terminal multiplexer; a new network session daemon is unnecessary.

## 6. Semantic attachment and compatibility

### 6.1 Endpoint and authority

Add `view.sock` under the same private session directory on POSIX. It uses the
existing ownership and peer-credential checks. A session writer publishes and
cleans it up under the same endpoint-identity rules as `terminal.sock`.

A separate endpoint is justified by the different contract: terminal clients
receive bytes to display; workspace clients receive stable history boundaries,
status and command results. Keep the version-5 terminal framing intact so a new
binary can still attach to an old running owner. Do not send JSON controls into
the old PTY input path or turn ANSI parsing into a history API.

Both endpoints share one controller reservation and generation. A legacy terminal
and a workspace cannot control a session simultaneously. Read-only subscriptions
do not reserve input or change `-l` attachment status. A committed workspace
controller counts as attached even when its window is unfocused. Socket loss
releases its lease and leaves the owner running.

Committing a semantic controller switches the owner's presentation sink from
classic PTY painting to typed notifications and report retention. It keeps
canonical draft/route state but does not render a second, discarded transcript
for every live fragment. Read-only observers leave the existing sink alone.
Switching back to a classic controller restores its normal catch-up boundary.
An external terminal transaction temporarily bridges the owner's existing PTY
to the workspace's whole-terminal lease, using the current transfer framing;
ordinary semantic updates continue to accumulate by committed cursor.

### 6.2 Protocol contract

Use an independently versioned, length-delimited control envelope with checked
partial reads/writes. Reuse the existing 16 KiB transport slice where practical;
chunk long input and reports with offsets and a declared total. This is a frame
size, never a prompt, transcript or session quota. Unsupported protocol/schema
versions fail explicitly before acquiring control.

| Exchange | Required information and effect |
| --- | --- |
| `HELLO` / `CAPABILITIES` | Version, session identity, owner instance, supported history schema, observer/control and external-I/O capabilities. |
| `OPEN` / `SNAPSHOT` | Observer or controller intent; committed sequence, byte end and digest; current status, model, effort, priority, route and draft revision. Snapshot and subsequent changes share one ordering boundary. |
| `RESERVE` / `COMMIT` / `BOUND` | Reuse the existing reservation-generation semantics and exclusive controller arbitration. Input starts only after BOUND. |
| `TAIL` / `STATE` | Coalescible committed-tail watermark and revisioned ephemeral status. They never contain an uncommitted journal suffix. |
| `DRAFT` | Controller generation, route, expected revision and replacement draft or edit. Accepted revisions are retained by the owner for later attachments. |
| `SUBMIT` / `RESULT` | Connection request ID, controller generation, draft revision and submission result, including the durable input identity once admitted. |
| `COMMAND` / `RESULT` | Existing command dispatch with explicit scope; report, error, state change or exclusive-terminal request as a typed result. |
| `CANCEL`, `QUIT`, `DETACH` | Distinct operations against the controlled session. Quit acknowledges intent and then completion through normal owner shutdown. |
| `REPORT` / external lease | Immutable report handle and metadata, or an attachment-bound terminal transaction. Workspace receives no arbitrary owner-supplied workstation command. |

The owner exposes a committed tail after its journal commit. Register the
subscription and capture its initial snapshot atomically with respect to tail
publication. On a slow subscriber, replace pending watermarks with the newest;
the complete event range still lives in the journal. Bound only the transport's
working buffers. Disconnect an unresponsive observer without blocking the engine.

The workspace opens the exact prefix through the existing read-only history API.
Add a refresh operation that verifies journal identity and the new committed
boundary before extending the reader. Replaced files, truncation, invalid hashes
or changed lineage invalidate cached pages and stop the read with a visible
error. The workspace never repairs, truncates or takes the session writer lock.
Opening stopped history obtains a verified final complete boundary read-only.

Commands use a serialized owner mailbox, with heap-owned input, separate from
the UI's existing synchronous request slot. A submission is acknowledged only
after existing admission commits succeed. Retain a receipt for each request
through the live owner connection/reconnect window. On lost acknowledgement,
query that receipt; never automatically resubmit an uncertain turn. If the owner
restarted and cannot establish acceptance from the durable input identity, show
the uncertainty and retained draft for explicit recovery.

Draft text stays local until an owner acknowledgement. An older draft reply
cannot overwrite newer typing. Detach flushes acknowledged draft state before
releasing control; sudden connection loss can lose only the unacknowledged edit
suffix, which the still-running workspace retains. Clean workspace exit warns
about an unsaved draft through normal command feedback, without discarding it.

### 6.3 Old owners, external I/O and operator context

Owners without `view.sock` offer read-only history and a clearly
labelled classic attachment action. Classic attachment temporarily hands the
workspace terminal to the existing frontend; its normal detach returns to the
workspace. Never restart an owner to obtain a capability. An old live owner
cannot advertise the new committed-tail boundary. Its observer therefore shows
a labelled best-effort snapshot of complete, hash-verified records, refreshing
on an idle timer rather than every frame. It must detect a subsequently rolled
back suffix and invalidate that range. Only a new owner snapshot or a stopped
session's verified final prefix receives the committed-boundary guarantee.

Generated `/status`, help, model lists and similar reports open immutable report
buffers inside the workspace. `/cat` opens a file snapshot with explicit refresh;
it must not treat a mutable pathname as immutable history. External editor/pager
and upload/download commands acquire the entire terminal, temporarily suspending
workspace painting. Reuse the existing attachment-bound transfer protocol and
terminal profile. Only one exclusive transaction may own the terminal, across
all windows. Cancellation and focus changes cannot redirect its protocol bytes
into a different session's composer. Owners continue working throughout.

The model-facing presentation hint needs a truthful workspace adapter. Send a
small controller snapshot indicating focused/unfocused, view, verbosity and
whether the user is following live output. The next provider request may use
that snapshot under the existing hint policy. Read-only observers, search terms,
selections and clipboard contents do not enter model context. A multi-window
hint must not claim that every tool output is visible or read.

## 7. History, projection and retention

### 7.1 Source and identity

Use the current JSONL history readers. The proposed
[binary storage format](session-storage.md) is a separate migration; VM neither
depends on it nor creates a parallel authoritative event log. Put history access
behind a small reader interface so that future storage can supply the same
committed-range and seek operations.

A display block has a stable identity based on session ID, source event sequence,
item identity and logical UTF-8 offset. It also has type, stream/channel, style,
verbosity policy and source ranges. View anchors use these identities and an
affinity at boundaries. Wrapped screen row numbers are derived and disposable.

Projection must merge streamed `response_output` with completed response items
without rendering the final response twice. Apply output corrections by identity;
retain interruption/failure markers. Reassemble tool stdout/stderr by their
recorded stream offsets, preserving the journal's interleaving order. Show an IRC
source event once; model-admission events are not a second chat message. Model
compaction changes the model's input projection and adds visible compaction
markers, while previous operator history remains available.

Ordinary history applies the existing visibility/redaction policy and renders
control bytes inert. Wire/debug views retain their explicit verbosity rules.
Model or tool text can never become a live terminal escape, clipboard request,
file-transfer control or Vim command. Binary output has a labelled representation;
copying its displayed text does not claim to recover original binary bytes.

### 7.2 Paging and caches

Keep a bounded cache of decoded event pages and a separate cache of laid-out
visible blocks plus nearby pages. Budgets govern regenerable working memory,
not history retention. Select initial budgets from measured fixtures and expose
their usage in developer diagnostics before adding user configuration knobs.

Fetch around source anchors through bounded forward/reverse reads. Reuse the
current 4 MiB read quantum as a starting scheduling unit; individual valid records
can exceed it. Parsing a large checkpoint may still be expensive. Perform it in
the worker, cancel at available parse/read boundaries, and avoid repeating it on
every motion. Never call full-session reducer replay to paint or search a page.

The current reader still materializes a complete JSON record. Consequently,
bounded page caches alone do not bound peak memory for a large checkpoint.
Add a history-only streaming path for checkpoint records: validate canonical
JSON and envelope fields, compute the existing digest with `event_sha256`
omitted, and consume the checkpoint payload without constructing its state tree.
Maintain the same hash-chain checks and reject malformed/noncanonical data.
This is a reader optimization with byte-for-byte equivalence fixtures against
the existing validator, not a change to journal format or recovery semantics.
Ordinary display records retain their existing record-size contract. Measure
peak memory and cancellation latency for both record classes independently.

Maintain sparse offsets for visited pages. They are a disposable acceleration,
not a new required on-disk index. `gg` seeks the earliest displayable event and
`G` seeks the current tail. Neither requires knowing a total rendered line count.
Show source position/sequence and loaded-range information when an exact global
line count is unavailable. Do not invent a scroll percentage from loaded pages.

“Endless” means navigation through every retained committed event, subject to
the actual journal still existing. Cache eviction, model context limits, pager
lifetime and terminal height do not truncate that range. Pre-upgrade output
never recorded in the journal cannot be reconstructed. Command report snapshots
and presentation-only diagnostics remain available for their owning process's
lifetime; they are labelled separately from durable session history. Arbitrary
external programs' private screens are outside the transcript.

Keep report snapshots in private spill files while referenced, including a
command-result entry from which the buffer can be reopened. Do not silently
delete a report while a window or selection references it. This design adds no
new journal event types: current reducers reject unknown types, and a cosmetic
UI upgrade must not unexpectedly make existing journals unreadable by the
previous binary. Persisting presentation-only reports across owner restarts would
be an explicit later storage-format decision.

### 7.3 Verbosity, following and selection stability

Each window has its own verbosity. Changing it invalidates that window's
projection/layout and re-renders both historical and future output. Preserve
the nearest stable source anchor; if its block becomes hidden, anchor to its
containing visible heading or nearest visible neighbor and show that adjustment.
Keep the original source anchor so restoring detail can recover the position.

The existing `/verbose [0..6]` command reads or changes the focused window's
verbosity in VM. `/chat` and `/rollout` select that window's view. These are
frontend presentation settings; other windows of the same session retain theirs.
The controlling window's next presentation hint communicates the applicable
setting to the owner without rewriting durable session history.

`G` places the cursor at the newest visible text and enables FOLLOW. Navigation
away from the tail, search and visual selection enter HOLD. Incoming events add
a “new output” indicator without moving a held viewport. Returning to the tail
with `G` resumes following. Resize/reflow preserves the logical anchor, not its
old row number. A verbosity change preserves FOLLOW/HOLD state.

A selection pins a committed source boundary and the selected text projection.
New output cannot extend it. Resize can reflow its highlight. Verbosity changes
end visual mode with a visible notice and leave the existing register unchanged;
this avoids copying text that no longer matches the highlighted view.

## 8. Interaction contract

The transcript is a read-only buffer. The composer is an editable draft. The
status line identifies `NORMAL`, `INSERT`, `VISUAL`, `VISUAL LINE`, `VISUAL BLOCK`
or command/search entry, together with session identity, live/stored/read-only
state, model/effort/priority, verbosity and FOLLOW/HOLD. A tiny terminal prioritizes
session identity, mode and connection state. Engine “working” status comes from
owner state, not a frontend animation left running after an error.

### 8.1 Movement and composition

The following subset follows [Vim motion conventions](https://vimhelp.org/motion.txt.html)
and [scrolling conventions](https://vimhelp.org/scroll.txt.html). Unsupported
commands report briefly and leave the buffer unchanged.

| Keys | Effect |
| --- | --- |
| `h j k l`, arrow keys | Character and logical-line movement; `gj`/`gk` move wrapped display rows. |
| Counts, `w b e`, `0 ^ $` | Counted motions, word movement and logical-line boundaries. |
| `gg`, `G`, `[count]G` | Beginning, live tail, or one-based logical line. A distant numbered line may require cancellable scanning. |
| `Ctrl-U`, `Ctrl-D`, `Ctrl-B`, `Ctrl-F` | Half-page and full-page movement. |
| `H M L`, `zz` | Top/middle/bottom visible position; center the cursor. |
| `i`, `a`, `A` from transcript | Focus its composer at the remembered position or end and enter INSERT. |
| `Esc`, `Ctrl-[` | Leave INSERT/visual/command entry for NORMAL; preserve draft text. |
| `Tab` in NORMAL | Switch focus between transcript and composer. |
| `Ctrl-L` | Force a complete redraw from the current semantic state. |

In composer NORMAL, support the same applicable motions plus `i a I A o O`,
`x`, `d`/`c` with supported motions, `dd`, `cc`, `p`/`P`, `u` and `Ctrl-R`.
Undo is draft-local. Transcript delete/change commands report “read-only”.
Macros, mappings, arbitrary Ex commands and Vim scripting are outside this subset.

INSERT retains the existing composer behavior: Enter submits through normal
command/prompt dispatch and Ctrl-J inserts a newline. These are the existing
bindings; additional modified-Enter sequences require terminal support.
Multi-line bracketed paste
is inserted as one edit and cannot submit, invoke `:q` or execute normal-mode
keys embedded in the text. Completion, history, queued-input editing and file
attachments retain their current commands through the composer adapter.

Normal-mode `Ctrl-C` first cancels a local search/copy/external operation if one
is active; otherwise it performs the current session interrupt. The status bar
names the cancelled operation. Existing hard-escape behavior remains a separate
explicit sequence and must be tested through the new input decoder.

### 8.2 Search

`/pattern` searches forward and `?pattern` backward in NORMAL; `n` repeats and
`N` reverses. `/search TEXT` entered as a slash command invokes the same viewer
search, is visibly echoed as a command and never becomes a model prompt.
`/search` without text opens search entry. Slash-prefixed text in INSERT continues
to use the existing command dispatcher, so search cannot steal an ordinary prompt.

The initial search syntax is literal UTF-8 with case-sensitive matching and an
explicit `:set ignorecase` / `:set noignorecase` toggle. Do not advertise Vim
regular-expression compatibility. Search covers the active buffer's current
verbosity/view, across all retained history; a report buffer searches that report.
Use a background scan with overlap across chunk boundaries, progress by source
bytes/events, cancellation and a pinned committed tail. Report wraparound.
Reaching the pinned end offers subsequent live output on the next repeat.
Cold whole-history search is linear in the inspected history; a UI label must
not imply completion while only loaded pages have been searched.

Search results contain source anchors and excerpts. Opening a result does not
move other windows. Changing view/verbosity cancels an obsolete scan and keeps
the query for a new search. A corrupted historical range produces a visible
incomplete-search result, not “no matches”.

### 8.3 Visual selection, registers and mouse

Use `v`, `V` and `Ctrl-V` for character, line and rectangular selection, following
[Vim's visual modes](https://vimhelp.org/visual.txt.html). `y` yanks the selection;
`yy` and `y` plus a supported motion work in NORMAL. The unnamed register holds
the result and a linewise/blockwise flag. Yanking also attempts clipboard output.
`p`/`P` paste that register into the composer; transcript text remains immutable.

Copy logical text without terminal escapes, status decorations or soft-wrap
newlines. Linewise copy includes line separators. Blockwise copy uses the visual
column rectangle at selection time, pads short rows and never splits a wide or
combining character. The status reports copied bytes/lines and clipboard result.
Very large selections stream through a private file/register backing store.

Mouse mode is enabled inside VM and can be toggled with `:set mouse` /
`:set nomouse`. Use SGR mouse reports: click focuses and places the cursor,
wheel scrolls the hovered window, drag selects, and a separator drag resizes
splits. Use one click/drag implementation before adding multi-click gestures.
Terminal-native selection remains available through the terminal's own bypass
modifier or by disabling mouse reporting. Leaving VM restores mouse modes.

### 8.4 Windows and quit semantics

Use [Vim's split and focus conventions](https://vimhelp.org/windows.txt.html):
`:split [SESSION]`, `:vsplit [SESSION]`, `Ctrl-W s/v`, `Ctrl-W h/j/k/l/w`,
`Ctrl-W =`, and size adjustments `Ctrl-W +`, `-`, `>` and `<`. No argument splits
the same buffer; an argument resolves a session through the picker rules.

Keep a binary layout tree with orientation and proportions. Terminal shrink
temporarily hides windows that cannot fit their content/status minima, prioritizing
the focused window. Retain their buffers, leases and layout for expansion; show
the hidden-window count. Closing a window does not delete session storage.

Session quit deliberately follows the session lifecycle requested for this UI:

| Command/context | Effect |
| --- | --- |
| `:q` in a controlled session | Dispatch normal `/exit`: interrupt/settle active work, preserve the session and stop its owner. All windows of that session show its stopped state. Other owners keep running. |
| `:q` in a read-only session or report | Close that view; no control operation is authorized by observation. |
| `:q` in the picker with no active view | Exit the workspace, releasing any remaining leases through detach. |
| `:close` | Close this window. If it was the last view of a controlled session, detach and preserve the owner and its draft. |
| `:detach` | Release this buffer's controller, leave the owner running and show read-only history or the picker. |
| `:qa` | Explicitly quit the sessions controlled by this workspace, then exit after their normal shutdown acknowledgements. Unrelated and observed owners remain running. |

An unsent draft blocks `:q`/`:qa` with a clear message. `:q!` discards that
session's draft and requests the same normal shutdown; it is never SIGKILL.
`:qa!` applies that rule to the workspace's controlled sessions. A slow quit
shows “waiting for session shutdown” and permits inspection or explicit detach.
It does not silently turn a timeout into a kill. Terminal close, broken SSH,
workspace crash and job-control suspension preserve session owners. A report's
close returns to its originating window without changing the session lifecycle.

## 9. Screen rendering and terminal correctness

Extract reusable typed text/style formatting from the current fd-writing
renderer. Keep the existing streaming sink and add a grid-layout sink. The shared
formatting policy controls labels, redaction and verbosity; each sink controls
placement. Avoid a VT emulator between the old renderer and the workspace.

The workspace builds a complete back grid for the current screen. Each cell
contains a grapheme reference, width/continuation flag and style. Diff it against
the last fully written front grid and emit changed runs with explicit cursor and
style transitions. Swap front/back only after all bytes in that frame complete.
If a newer frame arrives while writing, coalesce future work; finish the current
escape sequence/frame before changing the assumed terminal state.

Invalidation follows events: changed content, input, layout, status, terminal
size or a timed status update. An idle workspace blocks on readiness/deadlines.
It does not repeatedly lay out the transcript or spin to animate a working icon.
Keep pending output bounded to the in-flight frame and newest desired screen.
A stalled terminal blocks presentation, not journal admission or engine work.

Use the alternate screen only on explicit VM entry. Save and restore raw/cooked,
paste, mouse, cursor and screen modes on clean exit, suspend/resume, external
lease transitions and handled signals. `Ctrl-L`, resize, attachment recovery and
external return invalidate the front grid and force a full redraw. Synchronized
terminal updates can be used when positively supported; correctness cannot rely
on them. Redirected output or an incapable terminal gets a diagnostic before
any screen-control sequence, with the classic interface command available.

UTF-8 input decoding, grapheme boundaries, display width and byte-to-cell maps
must be shared by navigation, selection, wrapping and mouse hit testing. Current
UTF-8 helpers alone are not evidence of complete grapheme support. Use portable
tables/helpers for combining sequences and wide characters; test invalid UTF-8,
tabs, wide glyphs at the right edge and ambiguous-width terminal differences.
Preserve logical bytes even when the terminal renders an unusual cluster poorly.

Decode escape/key sequences incrementally. Use the existing terminal parser where
possible, extending it for mouse and VM commands. Lone Escape disambiguation must
not require a following key to flush a paste or a partial UTF-8 sequence. Keep
bracketed paste, capability replies, clipboard ACKs and file-transfer input in
their own active transactions before dispatching ordinary key bindings.

## 10. Clipboard and transport behavior

### 10.1 Local copy

Always commit a yank to the internal register first. Prefer a workstation-native
clipboard backend: platform API or a discovered fixed-argv helper such as
`pbcopy`, `wl-copy` or `xclip`, with data on stdin. Never interpolate selected
text into shell code. Detect an available graphical session and report helper
exit/failure; a helper name alone does not establish clipboard availability.

Where native publication is unavailable, OSC 52 is an optional terminal fallback.
Its ability to set a selection depends on terminal policy, as described in the
[xterm control-sequence reference](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html).
Do not probe by reading the clipboard. Ordinary OSC 52 emission has no reliable
end-to-end write receipt; report “clipboard sequence sent”, distinct from a
confirmed native write. Respect the backend's size support and preserve the
internal register when external copy fails.

### 10.2 Remote copy

Negotiate a `clipboard-write` capability with `snajpagent remote`. Introduce an
explicit operation in the existing checked transfer envelope, separate from
file upload/download and the durable file outbox. Only the outer workstation
wrapper publishes the clipboard; inner wrappers relay the operation.

A transfer carries an attachment-bound nonce, operation ID, UTF-8 encoding,
declared byte length, sequenced chunks and final digest. The receiver stages the
selection in a private file and publishes only after verification. Duplicate
chunks are acknowledged without duplication. A completed operation ID returns
its receipt on retry instead of repeating clipboard publication. Receipts
distinguish native success, OSC emission, cancellation and failure.

Cancellation, expired attachment, replaced controller, truncated input or digest
failure leaves the old workstation clipboard untouched. A successful clipboard
write followed by a lost receipt is reported as uncertain until its receipt can
be recovered. New yanks serialize behind or explicitly cancel the previous
operation; late ACKs cannot finish a different selection. After capability
negotiation, stop forwarding protocol titles and keyboard replies as user text.

Reuse the negotiated byte-stream path over SSH and acknowledged title-state path
over stock Mosh. Mosh synchronizes visible terminal state and can omit intermediate
output, which is also why terminal scrollback is incomplete
([Mosh FAQ](https://mosh.org/)). A one-shot escape sequence is therefore not the
transport contract. The existing Mosh path retains each small frame until its
ACK. Large yanks may be slow on that path: show byte progress, permit cancellation
and keep the workspace responsive. Do not upgrade a missing capability to an
assumed success or a new lifetime quota.

Clipboard writes require an explicit input action such as a yank. Provider/tool
output can contain copied text but cannot trigger a clipboard operation merely
by being rendered. The wrapper remains a trusted-remote-session boundary: its
nonce binds the transport, not proof that a hostile host had a human gesture.
Provide a local wrapper setting to disable remote clipboard writes; no clipboard
read capability is introduced.

### 10.3 tmux, screen and transport matrix

Reuse the current attachment profile and mux routing from
[remote-terminal.md](remote-terminal.md). tmux routing requires a unique eligible
attached client tty; ambiguous or absent clients fail without sending controls
to an arbitrary terminal. GNU screen uses the existing DCS relay boundary.
Never change global tmux/screen settings to make a copy appear successful.
tmux's own OSC clipboard handling depends on configuration and terminal support
([upstream clipboard guidance](https://github.com/tmux/tmux/wiki/Clipboard)).

Qualification must exercise these actual layouts:

| Layout | Expected clipboard destination and mechanism |
| --- | --- |
| Local VM | Local native backend, with labelled OSC fallback. |
| Local tmux or screen containing VM | Current attached workstation; tested mux routing or native desktop access. |
| `remote ssh … vm` | Outer wrapper's workstation backend over negotiated byte stream. |
| `remote mosh … vm` | Outer wrapper's workstation backend over title-state transport. |
| `remote ssh/mosh … tmux … vm` | Correct currently attached tmux client path, then outer wrapper. |
| `remote ssh/mosh … screen … vm` | Qualified screen passthrough/relay, then outer wrapper. |
| Wrapper inside a local mux; remote mux; nested SSH | Test relay boundaries and ensure only the outer endpoint publishes. |
| Plain SSH/Mosh without wrapper | Internal register always; available terminal fallback is explicitly unconfirmed. |

Arbitrary nested multiplexers are not already qualified by the existing transfer
implementation. Each supported nesting needs a fixture and documented wrapper
placement. Missing support reports the failed capability while retaining the
selection. File upload/download must continue working in all qualified VM paths,
including while other panes receive output.

## 11. Module and platform boundaries

Proposed source responsibilities, combined further where implementation stays
clear:

| Component | Responsibility |
| --- | --- |
| Existing `ui`, `render`, `platform` | Pager lease fix, asynchronous external jobs, shared formatting and classic presentation. Always compiled. |
| `session_view` | Semantic endpoint, controller arbitration and typed dispatch. Compiled with VM; existing terminal attachment remains independent. |
| `vm` | CLI entry, buffers/windows, input modes, layout and grid output. |
| `vm_history` | Read-only source paging, stable anchors, projection, search and cache ownership. |
| `clipboard` | Register backing and workstation publication; wrapper protocol integrates in existing remote/screen code. |
| `vm_stub` | Helpful unsupported-feature result for a `WITH_VM=0` binary. |

Use `WITH_VM ?= 1` in the existing build configuration and include it in build
input fingerprints. A disabled build links no VM renderer/history workspace or
clipboard implementation, retains the shared pager correction and keeps normal
remote file transfer working. `vm --help` clearly reports the omitted feature.
Feature discovery prevents one side of a mixed installation from sending an
unsupported clipboard or semantic command. All release target recipes include
the module; custom lean builds can explicitly exclude it.

POSIX native owners are the first full live-workspace backend on Linux, macOS
and the supported BSD targets. Reuse their peer authentication and PTY code;
do not describe that existing implementation as Linux-only.

Windows ships the portable VM module too. The initial platform adapter provides
stored-history/report viewing and a single direct live session using the current
engine/UI boundary; splits can show that session and other stored histories.
It must clearly disclose that persistent multi-owner attachment and the `remote`
wrapper are unavailable on that platform today. Direct-engine termination follows
its current platform lifecycle, and cannot claim POSIX detach persistence. Disable
unsupported detach/multi-live controls before executing them. Closing the last
window of a direct live session keeps its buffer/engine hidden in the workspace;
exiting the workspace requires explicitly quitting that session first. A window
close must not silently become an engine exit. Full Windows
persistent ownership requires a separate same-binary owner launch and authenticated
named-pipe backend; it is a platform follow-up, not an implicit requirement to
add a second engine thread inside this workspace. A compiled feature flag alone
never counts as runtime qualification.

The new interface must use existing command and settings authority. `/configure`
explicitly reloads configuration and model cache; opening windows and repainting
do not reread them. Model selection, `/fast`, nick/route changes and command echo
retain their current semantics. All entered slash and colon commands appear in
the appropriate command history/result area, including failures. Viewer commands
remain presentation-only and never create a model turn.

## 12. Verification and acceptance

Use owned session fixtures and the local fake provider. Never test quit, detach,
failure injection or clipboard mutation against an unrelated live owner. Tests
must assert observable behavior and source-range completeness, not implementation
data-structure shape.

| Test family | Required observable result |
| --- | --- |
| Pager regression before/after | Hold a pager open across numbered IRC events, tool output and streamed model output. Demonstrate the old failure where reproducible; after the fix, verify every accepted item is retained and rendered once after return. Assert provider progress while the pager remains open. |
| Pager failure paths | Spawn failure, early/nonzero exit, resize, suspend/resume, interrupt, detach and terminal loss restore ownership and preserve retained ranges. Exercise native and direct frontends, and `WITH_VM=0`. |
| Durable history | `gg`, `G`, page movement and search traverse a multi-gigabyte synthetic journal, older formats, large checkpoints, interrupted responses, compaction and mixed tool streams without a full reducer replay. Corruption yields an explicit incomplete range. |
| Cache and responsiveness | A history scan and live output run together. Record input-to-paint latency, read volume, RSS and idle CPU. Repeated local motion reads cached/adjacent ranges; repeated redraw does not reread the journal prefix. Cancelling search lets a visible-page request progress. |
| View projection | Retrospective verbosity toggles preserve source position; hidden anchors restore predictably. Completion records do not duplicate streamed text. HOLD does not move on incoming output. |
| Input | Slow/fragmented Escape, UTF-8 and bracketed paste; a paste ending exactly at a read boundary appears without an extra key. Pasted Vim commands never execute. Composer edits, undo and queue handling preserve text. |
| Grid | PTY screen assertions cover partial writes, blocked output, resize, wide/combining text, external return and force-redraw. No protocol frame or raw model escape reaches the visible terminal. |
| Windows and lifecycle | Same-session splits share one draft/controller. Different sessions progress independently. Verify `:q`, `:close`, `:detach`, unsent drafts, failed attach, stale generation and lost acknowledgement without a duplicate prompt or owner kill. |
| Session list | Observers leave status unchanged; VM controllers show attached; disconnect shows detached only after verified release. Newest activity ordering and selected-row identity survive refresh. |
| Clipboard | Exact UTF-8 bytes and line/block selection; large streamed copy, unavailable helper, denied clipboard, digest mismatch, duplicated chunks, cancelled/lost receipt and nested wrappers. Internal register survives each external failure. |
| Remote and files | Real SSH, stock Mosh, tmux and GNU screen fixtures for the matrix above; clipboard and existing upload/download both work with resize, disconnect and background output. Test refusal for ambiguous tmux clients. |
| Compatibility/build | Old owner/new VM, old wrapper/new VM, VM omitted, all production target compile profiles, Windows capability restrictions and ordinary CLI preservation. |
| Context isolation | Navigation, search, copy and report viewing cause no provider requests or added context. A focused presentation hint has the right verbosity/follow state; explicit commands keep their existing journal/context effects. |

Use a desktop clipboard integration check on the actual supported workstation in
addition to a fake clipboard sink that verifies exact bytes. A fake backend
proves encoding and routing, not desktop clipboard access. Record which actual
terminal/mux versions and layouts passed; do not infer the entire matrix from a
single PTY test.

Performance acceptance is structural first: memory stabilizes with the working
set rather than journal age; redraw cost follows visible cells; history work
does not occupy the owner engine thread; idle CPU has no redraw loop. Benchmark
tail open, `gg`, cached page motion, cold search, heavy live output and tiny-screen
resizing on the existing hosts. Record measured timing targets after a baseline;
do not hide a slow linear scan behind an unsupported constant-time promise.

## 13. Implementation and delivery sequence

1. **Pager correction.** Reproduce and add permanent regression coverage; gate
   every ordinary display path under the terminal lease; add retained catch-up
   and asynchronous external jobs. Ship the fix through normal development
   delivery independently of the full-screen workspace.
2. **Read-only workspace.** Add the compile switch/stub, explicit CLI, grid,
   history reader, modes, movement, retrospective verbosity, search and visual
   registers. Exercise large fixture history and tiny terminals before connecting
   live control. Keep the command clearly experimental while incomplete.
3. **Live semantic attachment.** Implement common controller arbitration, tail
   snapshots, drafts, typed commands, external-I/O transactions and disconnect
   recovery. Cover old owners and old frontends. Add picker and command reports.
4. **Windows and clipboard.** Complete split ownership, mouse, quit/detach,
   native clipboard and remote capability/receipt integration. Qualify file
   transfer and copy through SSH/Mosh/tmux/screen with all panes active.
5. **Platform and workflow completion.** Finish the Windows direct adapter and
   BSD qualification, compile-out checks and documentation. Publish the exact
   supported Vim subset and platform differences. Full feature completion means
   these acceptance cases pass, not merely that a full-screen demo launches.

Update `snajpagent.1`, affected tutorial/troubleshooting and README examples,
design/status pages and Unreleased notes with implementation changes. Render the
manual and check example syntax. Preserve the classic interface's append-only
rendering contract outside VM. The VM exception is explicit in the architecture.

Development delivery follows the current staging/master policy and recorded
shipment authority, using `Pavel Snajdr <snajpa@snajpa.net>`. Implementation
deliveries refresh the Mac and snajpadev binaries and manuals, preserve rollback
copies and keep running production owners intact. A running old owner gains new
features only on its normal exit/resume; installation never authorizes restarting
it. Source/development delivery does not select a new release version, tag or
website publication. No release is produced on E2B hardware.

## 14. Remaining decisions and risks

The engineering defaults above are sufficient to begin implementation. Local
automatic opt-in remains a user preference; the command works without resolving
it. The first development step is the held-pager regression, followed by its
retention and scheduling fix.

The largest implementation risks are the shared controller transition between
two frontend types, separating pure formatting from fd writes, and preserving
all external-terminal paths while the engine continues running. They deserve
focused early tests. The main performance risk is historical JSONL parsing,
especially large individual checkpoints; worker scheduling and visited-page
caches are required even before a future binary journal exists.

Clipboard qualification depends on actual terminal/mux placement and desktop
access. The checked wrapper protocol makes transport completion explicit, while
the internal register keeps copy useful when the external endpoint cannot accept
it. Session history completeness is bounded by retained source events; this
design cannot recover bytes that older versions never recorded.
