<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Interactive Output And Queue Editing

This note defines how streamed model output and the active input composer share
the terminal, and how users inspect and modify queued turns.

The [pager-retention and Vim workspace design](vim-mode.md) specifies
external-terminal buffering and a proposed, explicitly selected full-screen
interface. Its history redraw and window controls apply to that optional mode;
the streaming presentation contract here continues to apply to ordinary startup.

## Runtime Ownership And Scheduling

### Native session attachment

The operator must be able to leave a live session without a terminal multiplexer
and reconnect without restarting its engine. `--attach` / `-A` attach to a live
owner; `--resume` attaches to a live session or continues a stopped one. Existing file `/attach` and
`/detach` remain unchanged. Bare `/session` prints the full current ID followed
by running sessions; `/session list` also includes stored sessions;
`/session detach` returns to the shell; `/session attach ID` switches terminals
to another live owner. Unique ID prefixes are accepted and ambiguous matches
are listed rather than guessed. Omitting an attach ID offers a live-session
selection, not an implicit resume or a new session.
Attachment bypasses provider onboarding and uses the live owner's configuration,
including when its state directory has no default configuration file.

Session tables end with LAST PROMPT and IRC, splitting remaining terminal columns
equally. IRC previews show `endpoint: message`; endpoint lists place the hosted
`s/nick@endpoint` before comma-separated `c/nick@endpoint` clients. Each nick is
the recorded agent identity with later verified rename events applied; unknown
nicks leave the endpoint alone. Names, models and text
are flattened and clipped by display width; redirected lists remain tab-separated.
The resume/attach picker uses the same table.

Keep the existing engine/presentation ownership split. One process remains the
session and journal owner throughout requests, tools, IRC, goals and terminal
disconnects. A small same-binary terminal client is the replaceable attachment;
no service manager, terminal emulator, screen buffer, pane/session multiplexer,
second journal writer or per-session helper executable is required. The client
owns its actual terminal modes and forwards input/geometry. The owner retains
editor/render state and presents output through a narrow local connection.
Start the surviving owner before creating worker threads, not by forking a
multithreaded process at `/session detach`. Noninteractive execution, listing,
help and version do not silently create surviving background work.

One terminal may control a session at a time. Authenticate the local endpoint
using the existing private-store ownership boundary and native peer identity;
never treat a stale socket/PID file as proof of ownership. The session lock is
the durable writer authority. List/status probes must neither acquire an
attachment nor release the current process's session lock. Attach-target lookup
opens only the verified session directory: it neither replays/truncates the
journal nor opens the lock, removes staging files or finishes trash deletion.
Ambiguous prefixes report full matching IDs. Live-owner authentication and the
attachment reservation happen separately against the private endpoint. A failed switch
keeps the source attached: resolve/authenticate/reserve the destination and wait
for its commit acknowledgement before releasing the source. The source's switch
request alone never closes its client. Do not silently steal a terminal
or terminate either owner's work. Stopped/unreachable targets report the actual
condition and the appropriate explicit attach/resume action.

Detachment and terminal transport failure are not engine EOF, cancellation or
shutdown. Keep active requests/process handles/goals intact and keep admitting
their durable results. Preserve the unsent draft, route, selected view and
model/effort in the owning session; drafts are not submitted by switching.
Detached presentation must not backpressure the engine indefinitely. Do not
spool unlimited raw terminal bytes or replay cursor-control output into a new
terminal. At attachment, establish fresh geometry and an explicit bounded
catch-up from existing display history, with a visible omission/range notice
when needed, then show the retained composer. Catch-up is display only and must
not create model inputs, repeat provider work, resend IRC, or change journal
lineage. Normal streaming/resizing still never repaints sealed conversation.
Forward each resize notification to presentation, including unchanged dimensions:
the private PTY's size ioctl only generates SIGWINCH when its dimensions change.
Close and unlink the owned attachment endpoint before deleting a session, while
the original writer lock is held. The connected client remains until the final
exit acknowledgement.

Before enabling this path, account for every actual-terminal ownership path:
Ctrl-C/quit versus detach, Ctrl-Z/job control, signals and abrupt SSH/terminal
loss, external editor/pager, audio capture and native upload/download. Preserve
the existing exclusive-terminal contracts rather than accidentally letting a
background owner read the shell or signal the user's process group. Cover these
through the existing PTY/tmux/fake-provider suites, including detached live-tool
completion, attach/list races, stale endpoints, failed switching, lock retention,
catch-up bounds and no redraw/repeated model response. A native transport must
fit `term_host`/the current UI boundary, not duplicate the app controller.

The initial Linux host uses one private PTY as the existing terminal I/O endpoint. This
is needed by the current external editor/pager and native file-transfer paths,
which take exclusive ownership of terminal descriptors. A transport owner drains
the PTY while detached; it retains no screen cells or raw-output history. The
replaceable client controls the real terminal. Session startup forks before any
application worker exists; the surviving process keeps the PTY and all application
owners. Terminal loss closes only its client connection. Ctrl-Z suspends the
client, not the session engine. Reattachment invalidates only the previous
composer coordinates and uses bounded semantic history on the new terminal.

The pre-thread startup primitive requires all standard descriptors on the same
terminal and preserves its modes and geometry. The frontend keeps only its
socket and the child identity. The surviving owner creates a separate process
session and controlling terminal, retains the private PTY master and slave,
and directs its standard streams there. Closing the frontend socket or original
terminal leaves this private terminal alive. Startup does not open a journal,
acquire a session lock or publish an attachment endpoint; those remain with
the application after successful initialization.

POSIX input-mode changes apply immediately and discard input only when requested.
They preserve queued output for the relay. A UI command may run while the session
owner waits for its completion, so a terminal-mode syscall must not wait for that
owner to drain the private PTY. This applies to shutdown, external-program handoff,
raw-input entry and hidden-input prompts.

The local stream protocol has bounded frames and independently handles partial
reads/writes. Socket access requires the private session directory and matching
native peer credentials. Only a held session writer lock authorizes publishing
or replacing an endpoint; endpoint cleanup checks its recorded inode identity.
A socket file alone is not proof of a live session. The handshake reserves one
client before committing attachment, and output during reservation is discarded,
not accumulated for raw replay. These are transport boundaries, not a second
application controller. Other hosts retain their existing direct
terminal behavior until their attachment backend is implemented and tested.

The relay has one descriptor owner and one frame per input/output direction;
other threads must serialize requests to that owner. Each frame carries at most
16 KiB. The client acknowledges the cumulative physical-write offset of each
output frame before the relay sends the next one. Partial-write acknowledgements
extend the progress deadline. This keeps bulk bytes out of the socket's control
capacity when the physical terminal stalls. A silent handshake
expires after 15 seconds; five seconds without progress in a queued input or
output frame disconnects only that client. Detached output is drained rather
than saved. A competing client receives a busy response, including while the
first client is reserved but has not committed. Socket hangup is processed even
when pending PTY input has temporarily disabled socket reads.
For job-control suspension the existing client retains its reservation without
an idle timeout while output is drained. On continue it submits new geometry
through the same commit/repaint boundary; the session engine is never stopped.

Commit supplies the new geometry and requests a presentation barrier. While
presentation writes are quiescent, forget the old composer coordinates and flush
the private PTY's old output before acknowledging attachment. On Linux the slave
output flush precedes the master input flush: pending flip-buffer output and
already-delivered line-discipline bytes are different queues. A read-until-EAGAIN
loop alone does not establish this boundary. This sequence preserves the
engine's pending input. Only the current reservation generation may activate;
a delayed acknowledgement cannot activate a replacement client. The foreground
terminal/SIGTTOU rules also apply during external pager/editor ownership. After
activation, present bounded semantic catch-up rather than old cursor bytes.

The terminal client keeps one frame per direction plus one incoming control
frame. Physical input reaches the source during the destination handshake,
including input already readable in the poll that accepts the destination.
Acceptance stops new source reads while queued source output/input finishes.
The client then shuts down its source write side and waits for relay EOF, so
queued input and display acknowledgements are consumed before full closure.
Late source display and controls are consumed without presentation or retargeting
during this final drain. Subsequent input enters the destination after switching.
Initial attachment has no source and holds input until acceptance. Refusal,
handshake EOF and timeout before acceptance retain the source. A failure after
write shutdown ends the terminal connection with a diagnostic; rollback is then
unavailable. A failed-switch diagnostic returns to its presentation owner as a control
message; it neither becomes engine input nor disconnects that source. Suspension
continues the same reservation through a fresh geometry/repaint handshake.
That reserved client may finish keyboard and control frames already in flight
before its continuation handshake. A newly reserved client still cannot send
input or resize before activation.

Five consecutive Ctrl-C presses retain the owner's existing hard-escape policy.
Its sole relay writer announces accepted hard-exit intent; the input watchdog
then exits independently of blocked display output. The client can read that
notice while holding a partially written display frame, and treats the following
peer EOF as a successful hard exit. Unexpected peer loss remains an error.
After peer hangup, pending display bytes may be discarded to reach terminal
controls and restore physical modes. Live output is never discarded for this
purpose, and the frontend never interprets raw transfer bytes as hard escapes.

Exclusive file-transfer input belongs to the attachment that began the exchange.
Loss or replacement of that attachment cancels an unfinished exchange and clears
its unread protocol input before the composer reopens. Verified, completed
transfer results remain valid; only the old attachment's keyboard tail is
discarded. A replacement terminal performs its own workstation capability probe.

Attachment metadata must describe the new physical terminal, not the surviving
owner's startup environment. The native wire draft 5 carries TERM, STY, TMUX and TMUX_PANE
with the geometry commit. Earlier attachment drafts are rejected; the read-only
status query separately supports drafts 2 through 5 for listings across upgrades. Each name is bounded to 255 bytes plus its
terminator, matching a terminal-name
or screen socket-name component. Validate the complete metadata before changing
attachment state. The native frontend requires an ANSI-capable terminal; plain
or missing TERM keeps ordinary startup on its direct-terminal path, and an
unsupported replacement is refused without changing the live owner.

Initial widths below the renderer's existing 20-column threshold also keep
ordinary startup direct, preserving cooked input and terminal-driver echo.
An existing native owner retains raw input when its terminal becomes narrower,
including on return from an editor or pager. Rendering capability controls
presentation; it must not let the private PTY consume Ctrl-Z before the UI can
delegate suspension to the frontend.

Apply the accepted profile at the existing rebind barrier. Screen passthrough
for input modes, workstation probes and transfers uses that profile. Owner
preferences, including color policy, stay unchanged. Newly launched editors and
pagers receive TERM/STY/TMUX/TMUX_PANE overrides in their own child environment, without
interpolating metadata into shell source. Never change the multithreaded owner's
environment.
An already-running external program keeps its launch environment and receives
the existing private-PTY resize/redraw notification on reattachment.

Local dictation, playback and voice belong to the terminal that explicitly
started them. Native detach, terminal loss, switching away or job suspension
ends those operations; replacement does not authorize microphone restart.
Check the activated attachment generation in the voice worker's connection and
media checkpoints as well as the engine's audio service. Stop before accepting
an unadmitted voice handoff; preserve accepted coding work and final transcripts
through the existing voice-close drain. Cancel unfinished dictation without
submitting captured audio or inserting a delayed transcript into a new terminal.
The UI must refuse capture activation while detached, even though the owner's
private PTY remains raw and open. Explicit /voice on or /dictate after attachment
starts a new operation.

The real-terminal frontend opens an independent descriptor and restores the
original terminal modes on detach/failure. It emits an ANSI reset only if it
actually forwarded escape bytes. A narrow plain-output fallback stays plain.
The frontend stops only itself for job control. On continue it waits for
foreground ownership, reads fresh geometry and recommits the same reservation.
The frontend never signals a session owner.
Isolated PTY tests cover actual stop/continue, mode restoration, resize, input,
output and termination of the replaceable client.

The presentation thread drives the relay at idle, input and output checkpoints.
Native editor polling yields between buffered input bytes so output credits and
resize controls are serviced before the next repaint. The composer paints once
at the end of that buffer; per-byte old-width paints could otherwise remain in
the relay after a physical resize and erase previously displayed response rows.
Input-only output checkpoints retain burst admission without recursively painting
the composer.
It binds nonblocking output aliases before opening input so startup output can
progress through a full private PTY buffer. A commit wakes the engine with its
attachment generation. The UI rebind clears physical coordinates while keeping
the draft, buffered input and live semantic response. Input remains buffered
through the engine's catch-up work; a matching ready operation releases that
barrier and restores the composer. Raw composer suspension queues a client
control frame without stopping the owner or flushing its input.

Application startup now forks the native owner before worker creation. Linux
terminal sessions persist before the first composer and publish the endpoint
under that original writer lock. Idle, active-work and external-service
checkpoints handle attachment; a pending commit wakes an idle engine even
without a submitted action. Same-client job continuation restores only the
composer, while replacement clients receive bounded semantic history. Endpoint
closure precedes writer-lock release and the exit acknowledgement.

The app connects --attach/-A, the live picker and /session attach|a/detach|d
with /s aliases. Isolated full-app coverage verifies frontend loss, draft and
lock retention, competing clients, failed switching, selection and no duplicate
model turns. Pager/editor startup establishes a foreground process group before
exec, waits for stopped children and delegates suspension to the frontend.
The owner can service private-PTY control operations while that child owns the
foreground; exec children restore ordinary job-control signal dispositions.
Reattachment requests a redraw from the private terminal's foreground job.
The isolated pager regression covers actual frontend stop/continue, real
terminal mode restoration and the foreground job's redraw. Transfer/audio
interactions and broader lifecycle coverage remain under implementation/
validation; installed rescue binaries still do not provide native attachment.

Every application mode uses the same two core threads: one presentation owner
and one engine owner. The editor and renderer stay together on the presentation
thread. Only actual input/output capabilities differ for interactive, execute,
listing, and redirected output; there is no synchronous alternate runtime.
CLI preflight, help/version, and initial piped execute input precede startup.

The engine owns application state, Jansson graphs, providers, tools,
configuration, context, session durability, and history/cache disk work. None
of those operations runs on the presentation thread. Two bounded SPSC queues
carry owned typed values, never pointers into the other owner's mutable state.
Release/acquire publication orders queue items; nonblocking pipes wake owners.
Output backpressures the engine. Full input admission retains the draft and
reports the backlog without blocking editing; urgent interrupt/exit/failure
flags bypass ordinary backlog. Actions retain their originating prompt state.

Empty and whitespace-only Enter in the ordinary composer are local submitted
lines, in either view and during active work. The presentation owner retains the
old label in the transcript, resets the draft clock and shows a fresh prompt.
This reuses the local-feedback slot and submitted-line renderer; blank lines
never enter the engine action queue, input history, provider, IRC or goal state.
The formatted composer marks this policy on the terminal; simple confirmation
and queue-edit prompts continue delivering blank answers to their existing
owners. Ctrl-J remains newline insertion, and nonempty Enter keeps its normal
submission/steering/chat behavior. Continuing work requires explicit text.

The presentation owner echoes entered commands with their frozen prompt before
dispatch, including during engine stalls. Local controls and later engine
acknowledgements share the same echo marker to keep one submitted line. View
switches retain that line in the originating view before switching. A displayed
command confirms terminal receipt; acceptance and completion remain separate.

`/fast` toggles the session's requested Responses service tier between priority
and standard, with explicit ON/OFF feedback. It preserves model and reasoning
effort and leaves admitted requests running. The journal owns the selection;
configuration reload and resume preserve it. An unset selection omits the wire
field to retain existing provider defaults. Request preparation checks for a
changed tier after token counting before admitting a request.

The command/completion table lists each accepted syntax form with a short
explanation. `/help` groups that table by topic with a required/optional legend;
`/goal help` selects its goal rows from the same owner. Completion deduplicates
repeated command tokens and retains aliases. Help shares the model catalogue's
configured pager and external-terminal handoff; pager-off, missing pager and
failed pager fall back to direct output. Generated pager text is plain text.
Direct help uses ordinary foreground text and bold headings where enabled,
batched into one output transaction. `/s` aliases `/session`, and `l` aliases
its `list` subcommand. Help uses the existing terminal
word layout through a presentation operation, preserving the active draft,
streaming Markdown state and redirected bytes. Other host/status output keeps
its existing formatting. Interactive model syntax documents its own parser:
two components mean model/effort, three name a provider; omitted effort uses
highest cached/default/current effort, unlike the CLI's first-effort choice.

Optional networking adds one IRC server owner and one client owner per outgoing
endpoint (its agent/operator sockets stay together). Each runs the same existing
protocol code on private state. Bounded owned events, traces and room/identity
snapshots reach the engine in order; commands return acknowledged results.
No IRC thread invokes application, storage or presentation callbacks. The engine
commits incoming events before displaying/admitting them. Backpressure or DNS
in one endpoint cannot hold the server or another endpoint's protocol state.
All threads are joined; there is no per-peer thread, pool or detached worker.
Each IRC owner has 64 pending records; a short mailbox mutex orders publication,
not protocol work. State, event and trace records share that order. Commands
complete only after their preceding records are admitted by the engine. Owners
sleep on nonblocking sockets/wake pipes and real reconnect deadlines. Historical
restore precedes thread startup, and only the engine and hosted server retain
history. Outgoing owners do not duplicate history or allocate unused peer slots.

Presentation checks input/resize and due spinners before bounded output work,
at most every 16 ms while editing/animating and 25 ms during other activity.
Long text and view catch-up are sliced; Markdown and literal output share the
same renderer. Inactive operation waits for real input, wakeups, or deadlines.
Inherited descriptors are never made nonblocking. The UI privately reopens each
terminal output descriptor with `O_NONBLOCK`, verifies its device identity and
writes bounded slices to it. Redirected stdout stays redirected. Output waits
also service input-only checkpoints: editing and priority intent are consumed
without recursively painting. Deferred feedback and draft painting resume when
the terminal drains; a refusing consumer cannot display text before that.
Prompt label/visibility and verbosity never decide whether input is polled.

Provider and managed-process waits include the action wake descriptor. A single
libcurl-multi driver serves create/count/compact/catalog requests. Indivisible
engine syscalls may delay semantic acknowledgement, but cannot stop editing or
spinners. Controls run first when such calls return. History search/navigation
uses owned memory snapshots while engine refresh/persistence is pending.

Durable append-and-sync-before-adopt/ack remains unchanged. Display messages
retain engine order; public deltas acknowledge their exact delivered prefix
before the callback returns, including cancellation and failure paths. Editor
and pager handoff restores cooked mode before the external program starts and
reclaims the terminal after it exits. Pagers run asynchronously: the engine polls
their process state during ordinary provider and idle service. The renderer
closes its physical stream before handover, retains unpainted records and reads
durable IRC payloads from journal offsets. Provider deltas remain accepted while
their physical display cursor stays parked. Return drains the selected view and
restores the latest prompt. Model downloads remain queued during pager ownership.
The configuration editor remains a synchronous boundary operation; its existing
service callback maintains managed processes, IRC and native attachment.
`/cat` resolves cwd-relative paths
and passes the file to the configured pager without copying its contents into
the conversation. Shutdown closes admission, restores output
and terminal state, and joins the presentation thread; no thread is detached.

## Streamed Output And Typing

- All terminals hard-wrap displayed model text at word boundaries to the
  current width. Explicit model newlines remain explicit; generated prose
  continuations begin with two spaces below the text after `• `. Words keep
  their trailing punctuation and hard-wrap only when overlong; a leading
  separator space at a generated wrap is omitted.
- The renderer retains an unfinished fitting word across
  provider/style chunks until whitespace or item completion establishes its
  boundary. Complete words then wrap together. Overlong words and combining
  sequences flush at bounded width/byte limits. Receipt, durable public text
  and terminal painting are distinct; the presentation buffer never changes
  stored text.
- Wrapping is a terminal presentation detail. Stored response text, partial
  response events, redirected output, and provider protocol data remain byte
  exact and do not gain presentation newlines. Markdown-enabled and literal
  terminal output share the same explicit wrapping policy.
- The live composer is displayed immediately, including during model output;
  on cursor-capable terminals there is no quiet-output delay. The plain-text
  fallback cannot erase a live composer and never splits output to repaint it. `»` (U+00BB RIGHT-POINTING DOUBLE ANGLE
  QUOTATION MARK) marks active rollout; the composer does not spell out `steer`.
- Ordinary character insertion, deletion, and cursor movement update the
  visible composer in place. They do not erase and repaint its unchanged rows;
  status, spinner, search, and history updates use the same retained-frame
  painter. Unchanged rows emit nothing. Changed rows preserve common leading
  characters and, when their widths match, common trailing characters too.
  Styles, wide characters and combining marks participate in that comparison.
  Cursor controls, changed spans and obsolete suffix erasures are batched;
  ordinary updates never clear the whole composer before repainting it.
  Changes to wrapping or row count overwrite in natural wrap order, clearing
  obsolete suffixes afterward. Interposed output and resize invalidate the
  retained frame; conversation scrollback is never part of the frame.
  Configured spinner strings remain owned; their paint state comes from the
  shared label and frame, without separate offsets or copied frame metadata.
  Timer eligibility comes from the actual template and active slots.
- Visible model output pauses while the user is editing. Each edit restarts the
  configured typing pause, never a delay in prompt display. After that pause,
  erase the composer before appending output at the retained text endpoint,
  then immediately paint the current draft below the new output. No WIP prompt
  or draft snapshot is committed to conversation scrollback; actual submitted
  input is still recorded. Editing never changes the streamed text endpoint.
- The counted cursor detour preserves pending right-margin wrap, wide characters
  and combining marks. One bounded public slice brackets internal parser writes
  so the composer is repainted once per slice, not per character/checkpoint.
  Tool rows, argument/output blocks and streamed-output bursts each hold one
  output span, so a logical burst parks and repaints the composer once rather
  than once per internal slice.
- `[ui] typing_pause_ms` controls the inactivity pause. It defaults to `500`,
  accepts `0` through `5000`, and applies only to interactive terminal display.
  A value of `0` disables the typing pause without changing composer layout.

The pause provides display focus, not a provider-generation guarantee. Input,
interrupts, and local active-turn commands remain responsive while output is
paused.

Enter, Tab, and Ctrl-C have distinct active-turn meanings. In rollout, the
active composer appears when the turn starts, even before the provider's
`response.created` event, and stays visible across response handoffs. Enter
durably submits the draft as steering; input submitted before provider
acceptance is resolved at a safe request boundary. A foreground slash command
is the only reason to hide the composer until that command finishes. In chat,
Enter sends a room message;
only a mention of the local agent steers, while ordinary operator messages
remain background context. Outside completion, Tab durably appends the draft to the
future-turn FIFO and does not interrupt the response, yield a managed command,
or expose the text to the current model cycle. Ctrl-C is composer-first in
both idle and active states: it leaves the displayed draft in scrollback,
appends literal `^C` and a newline, discards the draft/search state, and opens a
clean prompt. A nonempty active draft does not interrupt the turn; an empty
active composer requests safe turn interruption. The outer tracked-turn owner
settles goal pause and retained-turn cleanup before showing idle. Every committed
goal-status change refreshes the existing spinner state before its notification
can repaint the prompt; no second goal state is maintained. Blank Enter after
interruption remains local and cannot resume the paused goal. Five consecutive Ctrl-C
presses within two seconds request exit through normal durable cleanup. Other
input or expiry resets the sequence. Empty Ctrl-D and terminal EOF use the same
priority exit control, interrupting active work and preserving the session;
`/exit` is available while idle. After an accepted Enter steer, the submitted
line remains visible while the old response is interrupted. The next active
composer appears when the replacement provider request acknowledges
`response.created`; typeahead waits until that boundary.

On POSIX, the native input worker distinguishes a real EOF from a zero-byte
read in open noncanonical VMIN=0/VTIME=0 mode. A flush can remove bytes after
poll reported readiness; if the PTY has no hangup, the worker retries and
keeps accepting later controls. Canonical VEOF, a disconnected PTY and pipe
EOF still end input. The stalled-output PTY case waits for a full master
backlog before timing the five-Ctrl-C escape; no output is drained to release
the blocked presentation writer.

When Enter interrupts visible model output, `response_interrupted` retains its
byte-exact public prefix. The next request places that prefix in assistant role,
then an explicit developer steering-boundary notice, then the exact steer in
user role. Multiple steers are projected exactly once in durable arrival order.
Provider output indexes for public items need only increase: hidden reasoning,
search, or tool items can create gaps. Duplicate or decreasing indexes and
identity, kind, or phase changes remain protocol errors. Output and protocol
failures retain a bounded specific diagnostic instead of being collapsed into
a generic delivery error.

Enter during `exec_command` or `write_stdin` returns the live managed-process
handle without signaling it, with `reason=steering_handoff`, and starts the
next model cycle with the command result and steering boundary. The handle-bound
`write_stdin` tool can then wait or interact as before, or set `terminate=true`
with empty data and no EOF request to use the existing TERM-then-bounded-KILL
closure and return the terminal result. A rejected termination combination does
not modify the process.

There is no textual activity row: interactive operation emits neither
`working…` nor `interrupting…` and reserves no extra status row. Idle,
goal, provider, and synchronous tool state appear only in the prompt spinner
fields described below.

Interactive submitted input and the first visible model block have exactly one
empty row between them. The final visible model block and the next input prompt
have the same separation. Boundary handling counts the model
block's existing trailing newlines and emits only the missing amount, so a
paragraph break, prompt redraw, or repeated boundary call cannot accumulate
extra empty rows. The rule is independent of Markdown presentation type and
does not alter submitted text, model text, events, or provider traffic.

Every prompt after non-prompt output has one empty row above it, irrespective
of view or Markdown mode. Consecutive submissions can be adjacent. Compaction
and goal changes share a bullet class: one row around a group, none inside it.
Pause reasons come from the durable event, not a duplicate host warning.

Startup/resume orientation, history headings/counts and the installed-update
notice share terminal-safe word layout with the composer. The banner adapter
uses display-cell widths, preserves complete words when they fit, and leaves
oversized paths/URLs to hard-wrap. It does not reuse or reset the active model's
Markdown state. Existing role boundaries preserve the draft, spacing and update
heading color. Redirected banner text remains byte-for-byte unchanged.

The composer word-wraps fitting whitespace-delimited words while retaining every
draft byte. Overlong words hard-wrap; explicit newlines keep their existing
indentation. Drafts taller than the screen show a cursor-following slice of the
same wrapped text, keeping edits out of terminal scrollback. Up/Down move among
draft rows with a preferred cell column, including recalled prompts. The first
or last row clamps to the actual text start/end; another arrow there selects
one history entry. Ctrl-P/Ctrl-N always select history directly.
Ctrl-A/Home and Ctrl-E/End jump to the start and end of the whole draft,
including across explicit newlines; both pairs use the same editor actions.
Ctrl-arrow, Alt-arrow and Meta-b/f implement the same whitespace-delimited word
movement as Ctrl-W. Display-to-source mapping uses the same sanitizing/wrapping
pass as painting, using current draft geometry even during input-only output
checkpoints; resize first accounts for the actual old painted bytes.

Rendered prose also has one empty row above and below throughout streaming,
independent of its neighboring block type. The terminal's existing output detour
stores a row count: it parks below live prose and resumes at the retained logical
endpoint on the next delta. A prompt uses that same gap rather than adding one.
Completion commits the gap; resize and exact-margin restoration reuse the same
cursor handling. This adds constant-size state, not a paragraph buffer or a
full-screen repaint. Explicit Ctrl-L also repaints only that retained composer;
it never clears the whole screen or re-emits completed conversation. A terminal
may save a whole-screen erase into scrollback, so replacing the display with
previous text would duplicate it even without another model response.

## Prompt Identity And Tab

`[ui] prompt` is one data-only template with exactly one `{chat:TEXT}`, one
`{rollout-idle:TEXT}`, and one `{rollout-active:TEXT}` case. It supports
separate `{provider}`, `{model}`, `{effort}`, `{operator}`, `{model_nick}`,
`{session_name}`, `{host}`,
`{context}`, `{queue}`, `{mode}`, `{hour}`, `{minute}`, and `{second}` fields plus optional `{goal_spinner}`,
and `{activity_spinner}` fields and escaped literal
braces/backslash; it performs no shell or environment expansion. The default
rollout prompt is `   HH:MM:SS PROVIDER/MODEL/EFFORT   0% › ` while idle and uses
`»` while active. All modes share one prefix outside the cases: activity slot,
goal slot, a space, the clock, and a space. Shared fields use the same formatting
as case fields; each spinner may occur only once in any expanded mode. Inactive
slots and unused digits in the four-column percentage remain spaces. The default idle chat prompt is
`   HH:MM:SS OPERATOR@HOST : `. Snajpagent appends one
space after the expanded template.

`{operator}` and `{model_nick}` use accepted live IRC identities, following the
selected endpoint in chat and the first endpoint in rollout. Destination updates
refresh both fields while preserving the draft and clock. Without endpoints,
local nick values remain available. `{model}` remains the provider model ID.
`{session_name}` inserts the saved name literally, or empty text when unnamed;
renames and resumed sessions use the current stored name. Interactive renames
validate all prompt modes before saving. A name omitted from the template keeps
the session-name storage limit and consumes no label space.

Clock components are natural decimal local-time values from one capture per
composer, not fragments of a preformatted string. `{hour:02}:{minute:02}:{second:02}`
produces the default clock. Submission or Ctrl-C cancellation ends the capture;
editing, search, resize, view/nick changes, asynchronous output, and status
transitions preserve it. The clock has no timer. Failed capture produces `--`
for all three components. Submitted/cancelled labels keep their displayed time.

`{context}` supplies plain digits or `?` when unknown, without `%`. The template
`{context:3}%` produces `  0%`, `  9%`, ` 10%`, `100%`, or `  ?%`.
`{queue}` supplies plain digits including `0`; `{queue:3}` accepts a width too.
The optional `{queued:TEXT}` section expands only when the count is nonzero,
inside a case or shared text, and is validated even when hidden. The default
`{queued:({queue}) }` owns the parentheses and spacing. Explicit pre-1.0 templates
must add their own `%` and queue decoration; neither field injects punctuation.
Clock components also support space
widths (`{hour:2}`), and only clocks support zero-fill (`{hour:02}`). Bare fields
remain unpadded; widths never truncate. Unknown clock components always use
space padding. Widths are positive decimal integers up to 510; reject empty,
signed, extra-leading-zero, whitespace, overflowing, or nonnumeric formats.
The whole label, input-separator space, and largest active frames must fit the
512-byte label buffer. Widths on other fields, including spinners, are invalid.
All modes are validated, and a failed `/config` reload keeps the prior config.
The pre-1.0 combined `{time}` field is removed; explicit templates must use the
component fields instead. No compatibility alias or automatic rewrite exists.

The context meter follows the rollout model identity and precedes the optional
queue count. `N` is the rounded-up percentage of the
latest durable token-domain input bound against the resolved hard input budget
for the same provider source, model, effort, and compaction lineage. A fresh
session displays `0%`. Accounting from a different provider source, selection,
or lineage renders `?%`, as does accounting with an unknown hard budget.
Serialized byte counts are never substituted as measured tokens. The idle
identity is the effective next-turn selection. The active identity is the
model and effort frozen for that turn, even if a command stages different
next-turn settings. Submitted input and queue-edit prompts keep the same meter
and glyph in terminal scrollback. Terminal-unsafe code points in a trusted
model or effort selector are visibly escaped in the composer only; the
selected value supplied to the provider remains byte-for-byte unchanged.

`prompt_spinner_goal`, `prompt_spinner_provider`, and `prompt_spinner_tool`
are quoted inactive-state plus active-frame strings. The first item is either a
safe one-column inactive code point or the leading `\0` zero-width sentinel;
the remaining safe one-column code points are active frames, bounded only by
the value's byte limit.
The defaults are `" ⚑"`, `" ◴◷◶◵"`, and `" ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"`, reserving
two columns: an independent goal flag and one shared activity slot. Tool
activity takes priority over provider activity in that same cell, even when
both activity bits are set. Otherwise the provider setting supplies its active
frames during model work and its inactive item when idle. The tool setting
supplies the frames (or its inactive item if it has no frames) during tool work.
`"\0"` disables the selected sequence, `" "` reserves a blank cell, and
`"\0⚑"` makes the goal flag appear only while active. The old separate
`{provider_spinner}` and `{tool_spinner}` placeholders are removed pre-1.0;
explicit templates must replace them with one `{activity_spinner}`. One
active frame is static and schedules no periodic work or cell rewrite.
Multiple active frames use the one shared `prompt_spinner_per_second` rate
(1--60, default 8), monotonic phase, and no catch-up bursts. A tick overwrites
only changed spinner cells; a width-changing `\0` transition performs one
structural redraw. A search prompt, hidden prompt, suspended process, or
non-addressable terminal does not animate. Tool status spans the synchronous
adapter call and returns to provider after a managed `running` result.
Interruption retains the provider or tool field until the turn closes; there
is no fourth interruption glyph, spinner, timer, or configuration key.

Literal spaces never disappear implicitly, and numeric padding never adds a
column to an absent spinner. An active space frame still occupies one column.
Omitting a spinner placeholder removes it for that mode. Compact provider/tool
handoffs account for changed slot ownership even when total width is unchanged.
Queue editing uses the same leading slots and four-column context before
its special `edit NUMBER › ` label.

The networked prompt identity and its chat/rollout views are specified in
`irc-chat.md`.

## Persistent Prompt History And Reverse Search

Chat and rollout composers within a session share its local prompt history.
Startup seeds an in-memory snapshot from `DOTDIR/prompt_history`; resumed
sessions restore their own `sessions/ID/prompt_history` instead. A new session
writes that snapshot when its ordinary durable state is first persisted.
Subsequent accepted lines update the local file and the bounded pending suffix.
History navigation and Ctrl-R receive only engine-owned local snapshots; they
perform no disk imports. The existing stable navigation/search handoff remains.

Orderly process exit reads the current global archive under its advisory lock,
appends only the pending suffix in order, then applies the same history bounds.
Imported and previously resumed entries are never merged again; deliberate
identical submissions remain separate. Concurrent exits serialize their merges
without overwriting one another's retained entries. There is no timer or new
configuration. Crash recovery uses the saved local history; a crash can skip the
global merge. Empty sessions keep history in memory and merge on exit without
creating a session directory. Session deletion removes its local history file.

Both `0600` no-follow regular files use UTF-8 physical lines with reversible
escapes for backslash, newline, carriage return, tab and other controls. Each
snapshot, pending suffix and file retains the newest 100 decoded entries within
4 MiB. Torn tails and malformed records are repaired on load; failed history
writes warn once without rejecting accepted input. Initial noninteractive input,
confirmation input, aborted drafts, peer/model/tool text and resumed `last_user`
are excluded. Prompt history is independent of provider context and `/history`.

Ctrl-R performs case-sensitive newest-to-oldest substring search and displays
`(reverse-i-search)`QUERY': MATCH`; a miss uses
`(failed reverse-i-search)`QUERY': `. Repeated Ctrl-R selects older matches,
Backspace broadens again, Ctrl-G restores the exact original draft/cursor,
Escape accepts without submitting, movement/editing accepts then applies, and
Enter submits the displayed match. Search never wraps or animates its prompt.

Tab uses the following order in every ordinary composer:

1. an empty draft cycles the available presentation views;
2. a nonempty slash-command prefix (including numeric IRC destinations) is
   completed when possible;
3. in chat, an `@nick` token at the cursor completes from current joined-room
   members across endpoints, using IRC case folding. One match expands with a
   trailing space; multiple matches expand their common prefix. A second
   consecutive Tab lists ambiguous choices in terminal-width columns and
   restores the draft/cursor. This same rule applies to commands and destinations.
   Unique completion reuses an existing space or inserts one before any retained
   suffix, leaving the cursor after the separator. No match leaves the draft
   unchanged. Completion never queues or
   sends text, preserves surrounding text and UTF-8 boundaries, and follows
   joins, departures, reconnects and nick changes through existing owner queues;
4. other nonempty text retains the existing contextual action: indentation
   while idle and future-turn queueing while active.

Both views exist even with no network connections. Empty Tab toggles them;
a nonempty draft never changes views. Queue-edit composers retain their
explicit save behavior. Startup selects chat when roles are enabled, otherwise
rollout. Runtime role changes preserve view, draft, history and verbosity.

The selected view owns both presentation and ordinary input
routing. Chat submissions go through IRC and are admitted through the local
room event. Rollout submissions remain local: an idle submission starts a turn
and an active submission steers it. Every accepted rollout submission is
rendered exactly once with its frozen submitted prompt label and never emits an
IRC message.

Offline chat is readable but cannot send: Enter reports no active destinations
and restores the draft. It never falls back to private model input. `/server
start`, `/server stop`, `/connect` and `/disconnect` use the common idle/active
command path, including during provider silence, retry, counting and managed
tool waits. They neither steer nor cancel work. Adding/removing one stable IRC
owner preserves the others; removed owners stop producing and drain accepted
records before being freed. Session-pending input survives final disconnection.

## Command admission and transcript

The engine's shared command dispatcher accepts commands in both views during
provider, count, catalog, compaction and managed-tool waits. Presentation and
inspection run when the foreground owner permits it. `/model` changes the next
request in the current turn and interrupts a streaming response at a safe
boundary; effort changes apply on a subsequent request. Both preferences remain
until changed, including across resume. CLI `-m` and `--effort` use the same
durable session preferences. Accepted inputs and completed tool results remain
bound to their durable identities.
Ctrl-C, Ctrl-D and `/yield` during a managed-tool wait remain priority controls
while the ordinary composer is held.
Configuration, catalog refresh, compaction, retry and lifecycle controls retain
accepted intent and run at their safe owner boundary. Commands captured under
an idle prompt are not blocked behind an active turn: ordinary rollout text from
that prompt enters the durable future queue while following commands proceed.

The existing control owner drains newly admitted kinds after the current
operation returns. Repeated requests for the same pending kind coalesce with an
explicit acknowledgement. A no-safe-prefix compaction stays pending without a
busy loop. Parked policy recovery still applies safe controls without releasing
the policy stop. Shutdown leaves unapplied controls pending. Delete requires its
explicit prefix even when other commands are entered at confirmation; opening a
queue editor cancels deletion. External `$EDITOR` has exclusive terminal input.

Every accepted slash-command line remains visible above its output in chat and
rollout, idle and active. The shared submission boundary owns that echo, not
individual handlers; UI-local commands keep their exact-once path. An editable
draft and the durable prompt-entry history are separate from immutable scrollback.
A queued prompt renders as an ordinary submission once at dispatch, with a fresh
local clock and its frozen effective model/effort. Queue receipt provenance and
any live draft's clock remain unchanged, including across same-turn retry.

`/history` and automatic resume use the same event iterator and renderer. The
default is one retained turn, including unfinished/failed/interrupted work.
Explicit counts are uncapped within available history; overflowing decimal
counts saturate safely. Nonempty replay opens with total session turns and
completed turns; every replay ends with shown/completed/total counts, even for
empty sessions and `/history 0`. Replay does not change model context or the log.
This conversation history is distinct from the session's prompt-entry history.

## Queue Commands

`/ro QUERY` is a per-prompt read-only query, not a persistent setting. It works
at idle and through `-e`; in chat view it explicitly opens local rollout view.
Active Enter or Tab on `/ro` (including multiline input) accepts a durable
future read-only turn. It never changes the active turn's tool permissions;
`/queue /ro QUERY` uses the same future-turn representation. Ordinary steering inside an existing read-only turn
keeps that turn read-only. `//ro ...` is literal ordinary input. Empty `/ro`
queries are rejected before turn creation. Incoming IRC text is not parsed as
a local slash command.

Queue add/edit and turn-start events carry required `read_only` booleans.
Queue listing identifies `/ro` entries; editing restores the command prefix
(or the literal-slash escape) and saves both normalized text and mode. Edits
retain FIFO identity, cancellation removes mode with the item, and replay
restores the exact mode without interpreting prompt text again.

`/queue` and `/q` are equivalent. With no argument, they print queued turns in
FIFO order with one-based numbers and short durable IDs:

```text
1 a1b2c3d4 › first queued turn
2 e5f6a7b8 › second queued turn
```

The following mutation forms are accepted in both active and idle composers:

```text
/q 2d
/q 2 delete
/queue 2d
/queue 2 delete
/q 3e
/q 3 edit
/queue 3e
/queue 3 edit
/q c
/q clear
/queue c
/queue clear
/q p
/q pop
/queue p
/queue pop
```

`delete` removes one numbered item. `clear` removes every queued item. `pop`
removes the most recently queued item, making it the quick undo for an
accidental Tab queue.

`edit` opens the selected text in the normal composer with an `edit N › `
label. Enter on replacement text saves it in place; during an active turn,
Tab also saves it. Slash commands still run, preserving the original item;
`//` enters a literal slash and `/ro` replaces the item's read-only text/mode.
The entry keeps its durable ID, sequence, and FIFO position. Beginning an edit
temporarily pauses automatic queue draining so the unedited entry cannot start;
saving restores the prior armed state when the current turn is still active.

During an active turn, `/queue TEXT` continues to append a future turn. Text
that is identical to a reserved manipulation expression can still be queued by
typing it directly and pressing Tab. The draft must be nonempty; empty Tab has
the view-cycle meaning defined above. Direct Tab queueing has the same strict
non-steering behavior as `/queue TEXT`.

## Durability And Failure Behavior

- Queue deletion and clearing use the existing `future_turn_cancelled` event.
- A saved edit appends `future_turn_edited` with the queue ID and replacement
  text. Replay validates that the ID is pending, replaces only its text, and
  updates the aggregate pending-byte accounting.
- Merely opening an editor does not change durable state. Invalid or oversized
  replacement text remains in the editor for correction.
- Queue numbers are views of current FIFO order; durable events continue to use
  queue IDs so replay does not depend on presentation numbering.

## Acceptance

- Rendering coverage demonstrates explicit word wrapping on capable and dumb
  terminals without changing delivered text. PTY coverage
  demonstrates transient active-turn composers, pause
  reset on continued typing, output resumption after the configured delay, and
  byte-exact persisted text. It also rejects whole-line erase and prompt replay
  during ordinary insertion, deletion, and cursor movement.
- PTY coverage demonstrates `/q` and `/queue` listing, delete, clear, and edit
  forms during active and idle operation.
- Store replay coverage rejects invalid edit targets and no-op edits, and
  reconstructs a valid edited queue. PTY coverage verifies the durable delete
  and clear events.
- Configuration coverage checks prompt cases/fields/escapes, spinner frame and
  shared-rate bounds, duplicate-key handling, and explicit `typing_pause_ms`
  values.
- Response/context coverage demonstrates nonconsecutive public output indexes,
  interrupted-prefix replay, an explicit steering boundary, and exact ordered
  steering content. PTY coverage demonstrates that the empty active composer is
  available after each rapid Enter steer.
- Managed-process coverage demonstrates a steering handoff without a signal,
  continued waiting or interaction, explicit termination, and rejection of
  conflicting termination arguments without touching the process.
- PTY and real-terminal queue coverage assert that Tab creates only
  `future_turn_queued`, creates no steering or response interruption, leaves the
  active response or command running, and drains queued turns later in FIFO
  order.
- PTY coverage asserts persistent cross-mode history, concurrent-session isolation,
  exit-only global merging, resume-local restoration, new-session seeding, Ctrl-R
  search controls, exact append-only `^C` cancellation, five-press Ctrl-C
  exit, prompt expansions, exact spinner-cell updates, and no periodic refresh
  for a selected one-frame state. Separate coverage retains explicit turn
  interruption for Ctrl-C on an empty active composer.

### Real-terminal regression

The permanent terminal test must exercise the complete contract through a real
terminal multiplexer, not only by inspecting the raw byte stream of a PTY.
It runs the fixture binary in an isolated, deliberately narrow tmux session and
asserts tmux's rendered pane contents after each interaction. The deterministic
scenario covers:

- word-boundary wrapping, explicit newlines, long-word hard wrapping, UTF-8,
  and resize to an exact-right-margin composer without changing or erasing the
  stored assistant text;
- the first active-turn edit, continued editing before the pause expires, provider
  text withheld for the configured interval, model output resumption below a
  one transient draft with no stale snapshots, and another edit/resume cycle;
- numbered `/q` and `/queue` listing plus edit, delete, clear, and newest-item
  pop, including the `edit N › ` composer and preservation of queue order;
- prompt, status, model output, and composer redraws without leaked escape
  sequences, overwritten text, duplicate fragments, or missing fragments; and
- API-like paced decoding through small fragments delivered roughly every
  40--100 ms, including a word divided across deltas and a final fragment with
  no trailing whitespace; complete words become visible at their boundaries,
  the divided word remains together when it fits, and item completion flushes
  the final word without a textual activity row; and
- enabled and disabled `AGENTS.md` discovery as recorded in the durable
  `turn_started` event.

The tmux socket, session, test HOME, configuration, and state directory are
unique to the test. The test never addresses or stops an unrelated tmux server
or snajpagent process. Deterministic tmux coverage is a normal local test target;
live provider coverage remains explicit because it consumes credentials and
provider capacity.

Live-provider qualification is an explicit, separately authorized operation.
Use an isolated home directory and non-destructive representative task, with the
chosen provider/model recorded in its evidence. Compare normalized rendered
text with durable public response data, verify advertised instruction paths,
and close only test-owned processes. Existing host-specific live-run records
are historical evidence, not reusable task authority or a mandatory prompt.

A completed bracketed paste requests the final composer redraw. Native input
coalesces intermediate edits until the last buffered byte; the closing paste
marker inserts no text but still completes the edit. This preserves batching
while making the complete draft visible without another key or engine output.
