<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Interactive Output And Queue Editing

This note defines how streamed model output and the active input composer share
the terminal, and how users inspect and modify queued turns.

## Runtime Ownership And Scheduling

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

Networked operation adds one IRC server owner and one client owner per outgoing
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
Inherited terminal descriptors are never made nonblocking. A stalled terminal,
kernel, or unscheduled process is outside this userspace guarantee.

Provider and managed-process waits include the action wake descriptor. A single
libcurl-multi driver serves create/count/compact/catalog requests. Indivisible
engine syscalls may delay semantic acknowledgement, but cannot stop editing or
spinners. Controls run first when such calls return. History search/navigation
uses owned memory snapshots while engine refresh/persistence is pending.

Durable append-and-sync-before-adopt/ack remains unchanged. Display messages
retain engine order; public deltas acknowledge their exact delivered prefix
before the callback returns, including cancellation and failure paths. Editor
handoff restores cooked mode before the engine starts the editor and reclaims
the terminal only after it exits. Shutdown closes admission, restores output
and terminal state, and joins the presentation thread; no thread is detached.

## Streamed Output And Typing

- Public model text is soft-wrapped at word boundaries to the current terminal
  width. Explicit model newlines remain explicit, and a single word wider than
  the terminal may hard-wrap. Trailing punctuation stays with the preceding
  text; hyphens, dashes, periods, commas, and similar closing punctuation make
  the next character a wrap opportunity rather than beginning a wrapped line.
  Generated Markdown prose soft-wrap continuation rows begin with two spaces,
  aligned below the paragraph text after `• `. A separator space that would
  otherwise be the first character printed after that wrap is omitted.
- Every complete UTF-8 provider delta becomes visible before its delivery
  callback returns. Word-wrap lookahead must not retain the newest word until
  another delta or response completion. If a provider divides one word across
  deltas, its already-visible prefix cannot be moved; the continuation remains
  contiguous and may use the terminal's hard wrap if it reaches the margin.
- Wrapping is a terminal presentation detail. Stored response text, partial
  response events, redirected output, and provider protocol data remain byte
  exact and do not gain presentation newlines. Markdown-enabled and literal
  terminal output use this same wrapping implementation.
- While a turn is active, the first edit after visible model output starts the
  configured rollout-active composer on a new terminal line immediately. `»`
  is U+00BB
  RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK; the composer does not spell out
  `steer`.
- Ordinary character insertion, deletion, and cursor movement update the
  visible composer in place. They do not erase and repaint its unchanged rows;
  complete composer reconstruction is reserved for structural transitions such
  as interposed output, status changes, and terminal resize.
- Visible model output pauses while the user is editing. Each edit restarts the
  pause. After the pause expires, the current composer line remains as a
  readable snapshot and model output resumes on the following line.
- If editing resumes after more model text, that text is ended on its current
  line and the updated draft is shown on a new rollout-active prompt line. This
  cycle can repeat without losing or changing the draft.
- An automatically revealed spinner prompt is temporary: erasing it restores
  the streamed output cursor, including the terminal's right-margin wrap state.
  It does not insert paragraph boundaries or split provider-fragment words.
  Actual editing commits the separate prompt line as the snapshot described
  above. Unicode cell widths and terminal reflow determine cursor restoration.
- `[ui] typing_pause_ms` controls the inactivity pause. It defaults to `500`,
  accepts `0` through `5000`, and applies only to interactive terminal display.
  A value of `0` retains the line separation but disables the delay.

The pause provides display focus, not a provider-generation guarantee. Input,
interrupts, and local active-turn commands remain responsive while output is
paused.

Enter, Tab, and Ctrl-C have distinct active-turn meanings. In rollout, Enter durably
submits the draft as immediate steering and interrupts the current provider
response at the next input-pump boundary. In chat, Enter sends a room message;
only a mention of the local agent steers, while ordinary operator messages
remain background context. Outside completion, Tab durably appends the draft to the
future-turn FIFO and does not interrupt the response, yield a managed command,
or expose the text to the current model cycle. Ctrl-C is composer-first in
both idle and active states: it leaves the displayed draft in scrollback,
appends literal `^C` and a newline, discards the draft/search state, and opens a
clean prompt. A nonempty active draft does not interrupt the turn; an empty
active composer requests safe turn interruption. Five consecutive Ctrl-C
presses within two seconds request exit through normal durable cleanup. Other
input or expiry resets the sequence. Empty Ctrl-D and terminal EOF use the same
priority exit control, interrupting active work and preserving the session;
`/exit` is available while idle. After every accepted Enter steer, an empty
active composer is armed immediately, before provider cancellation or the next
response cycle completes, so another steer can be entered at once.

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

## Prompt Identity And Tab

`[ui] prompt` is one data-only template with exactly one `{chat:TEXT}`, one
`{rollout-idle:TEXT}`, and one `{rollout-active:TEXT}` case. It supports
separate `{provider}`, `{model}`, `{effort}`, `{operator}`, `{host}`,
`{context}`, `{mode}`, `{hour}`, `{minute}`, and `{second}` fields plus optional `{goal_spinner}`,
and `{activity_spinner}` fields and escaped literal
braces/backslash; it performs no shell or environment expansion. The default
rollout prompt is `    0% PROVIDER/MODEL/EFFORT › ` while idle and uses `»` while
active. Two leading indicator slots precede the four-column percentage;
inactive slots and unused percentage digits remain spaces. The
default idle chat prompt is `   HH:MM:SS OPERATOR@HOST : `: two indicator slots
and one literal space before the clock. Snajpagent appends one
space after the expanded template.

Clock components are natural decimal local-time values from one capture per
composer, not fragments of a preformatted string. `{hour:02}:{minute:02}:{second:02}`
produces the default clock. Submission or Ctrl-C cancellation ends the capture;
editing, search, resize, view/nick changes, asynchronous output, and status
transitions preserve it. The clock has no timer. Failed capture produces `--`
for all three components. Submitted/cancelled labels keep their displayed time.

`{context:4}` right-aligns the complete percentage including `%` with spaces:
`  0%`, `  9%`, ` 10%`, `100%`, or `  ?%`. Clock components also support space
widths (`{hour:2}`), and only clocks support zero-fill (`{hour:02}`). Bare fields
remain unpadded; widths never truncate. Unknown clock components always use
space padding. Widths are positive decimal integers up to 510; reject empty,
signed, extra-leading-zero, whitespace, overflowing, or nonnumeric formats.
The whole label, input-separator space, and largest active frames must fit the
512-byte label buffer. Widths on other fields, including spinners, are invalid.
All modes are validated, and a failed `/config` reload keeps the prior config.
The pre-1.0 combined `{time}` field is removed; explicit templates must use the
component fields instead. No compatibility alias or automatic rewrite exists.

The context meter follows the leading indicator slots and precedes the rollout
model identity. `N` is the rounded-up percentage of the
latest durable token-domain input bound against the resolved hard input budget
for the same provider source, model, effort, and compaction lineage. A fresh
session or accounting from a different provider source, selection, or lineage
renders `0%`; compatible accounting with an unknown hard budget renders `?%`.
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
the remaining zero through 16 safe one-column code points are active frames.
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

All ordinary chat and rollout composers in one dotdir share the plaintext
`DOTDIR/prompt_history`. Each submitted line is appended under an advisory lock
as one UTF-8 physical line; backslash, newline, carriage return, tab, and other
controls use reversible text escapes. The `0600` no-follow regular file retains
the newest 100 decoded entries within 4 MiB. Torn tails and malformed records
are skipped or repaired without changing accepted-input semantics. Fresh
Up/Down navigation and Ctrl-R refresh from disk; an active search keeps a stable
snapshot. Initial noninteractive input, confirmation input, aborted drafts,
peer/model/tool text, and resumed `last_user` are excluded.

Ctrl-R performs case-sensitive newest-to-oldest substring search and displays
`(reverse-i-search)`QUERY': MATCH`; a miss uses
`(failed reverse-i-search)`QUERY': `. Repeated Ctrl-R selects older matches,
Backspace broadens again, Ctrl-G restores the exact original draft/cursor,
Escape accepts without submitting, movement/editing accepts then applies, and
Enter submits the displayed match. Search never wraps or animates its prompt.

Tab uses the following order in every ordinary composer:

1. an empty draft cycles the available presentation views;
2. a nonempty slash-command prefix is completed when possible;
3. in chat, an `@nick` token at the cursor completes from current joined-room
   members across endpoints, using IRC case folding. One match expands with a
   trailing space at draft end; multiple matches expand their common prefix.
   No match leaves the draft unchanged. Mention completion never queues or
   sends text, preserves surrounding text and UTF-8 boundaries, and follows
   joins, departures, reconnects and nick changes through existing owner queues;
4. other nonempty text retains the existing contextual action: indentation
   while idle and future-turn queueing while active.

Non-networked mode has only the rollout view, so empty Tab is a no-op. A
nonempty draft never changes views. Queue-edit composers retain their explicit
save behavior.

In networked mode the selected view owns both presentation and ordinary input
routing. Chat submissions go through IRC and are admitted through the local
room event. Rollout submissions remain local: an idle submission starts a turn
and an active submission steers it. Every accepted rollout submission is
rendered exactly once with its frozen submitted prompt label and never emits an
IRC message.

## Queue Commands

`/ro QUERY` is a per-prompt read-only query, not a persistent setting. It works
at idle and through `-e`; in chat view it explicitly opens local rollout view.
Active Enter on `/ro` (including multiline input) preserves the draft and
directs the user to Tab or `/queue /ro QUERY`. It never steers, interrupts, or
changes the active tools. Ordinary steering inside an existing read-only turn
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
label. Enter saves it in place; during an active turn, Tab also saves it. The
entry keeps its durable ID, sequence, and FIFO position. Beginning an edit
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

- Rendering coverage demonstrates word wrapping without changing delivered
  text. PTY coverage demonstrates newline-separated active-turn snapshots, pause
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
- PTY coverage asserts persistent cross-mode/cross-process history, Ctrl-R
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
  stable draft snapshot, and another edit/resume cycle after more output;
- numbered `/q` and `/queue` listing plus edit, delete, clear, and newest-item
  pop, including the `edit N › ` composer and preservation of queue order;
- prompt, status, model output, and composer redraws without leaked escape
  sequences, overwritten text, duplicate fragments, or missing fragments; and
- API-like paced decoding through small fragments delivered roughly every
  40--100 ms, including a word divided across deltas and a final fragment with
  no trailing whitespace; each complete fragment must be visible at callback
  cadence, the divided word must remain contiguous, and the final fragment
  must remain visible during a subsequent provider pause without any textual
  activity row appearing; and
- enabled and disabled `AGENTS.md` discovery as recorded in the durable
  `turn_started` event.

The tmux socket, session, workspace, configuration, and state directory are
unique to the test. The test never addresses or stops an unrelated tmux server
or snajpagent process. Deterministic tmux coverage is a normal local test target;
live provider coverage remains explicit because it consumes credentials and
provider capacity.

The live real-work qualification uses the configured default `codex-lb`
profile and the exact prompt:

```text
great... now please gather the complete state of livepatch status for vpsadminos kernel 6.12.95
```

Only one live qualification instance may run at a time. It runs from `/root`
in a narrow tmux, admits the applicable `/root/AGENTS.md`, waits through all
local tool work and response cycles, and exits normally. Acceptance compares
the normalized logical text sequence in the final rendered pane/history with
the byte-exact `response_completed` data, checks the
`turn_started.data.instructions` path/size/SHA-256 metadata, and rejects
dropped, duplicated, reordered, overwritten, or visibly escaped text.
