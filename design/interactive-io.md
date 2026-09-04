<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Interactive Output And Queue Editing

This note defines how streamed model output and the active input composer share
the terminal, and how users inspect and modify queued turns.

## Streamed Output And Typing

- Public model text is soft-wrapped at word boundaries to the current terminal
  width. Explicit model newlines remain explicit, and a single word wider than
  the terminal may hard-wrap. Trailing punctuation stays with the preceding
  text; hyphens, dashes, periods, commas, and similar closing punctuation make
  the next character a wrap opportunity rather than beginning a wrapped line.
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
  `MODEL/EFFORT » ` composer on a new terminal line immediately. `»` is U+00BB
  RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK; the composer does not spell out
  `steer`.
- Visible model output pauses while the user is editing. Each edit restarts the
  pause. After the pause expires, the current composer line remains as a
  readable snapshot and model output resumes on the following line.
- If editing resumes after more model text, that text is ended on its current
  line and the updated draft is shown on a new `MODEL/EFFORT » ` line. This
  cycle can repeat without losing or changing the draft.
- `[ui] typing_pause_ms` controls the inactivity pause. It defaults to `500`,
  accepts `0` through `5000`, and applies only to interactive terminal display.
  A value of `0` retains the line separation but disables the delay.

The pause provides display focus, not a provider-generation guarantee. Input,
interrupts, and local active-turn commands remain responsive while output is
paused.

The `working…` activity line is shown only after an open public item has been
closed. It follows all text already decoded for that item on a separate line;
response completion must not release a withheld final word and paint the
activity line as one delayed update.

## Prompt Identity And Tab

Ordinary input uses U+203A RIGHT-POINTING SINGLE ANGLE QUOTATION MARK (`›`)
while no turn is running and U+00BB RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK
(`»`) while input will be added to a running turn. The glyph is the complete
state marker; the ordinary composer never includes a `steer` word or a
different active-turn arrow.

The non-networked prompt is `MODEL/EFFORT › ` while idle and
`MODEL/EFFORT » ` during a turn. The idle identity is the effective next-turn
selection. The active identity is the model and effort frozen for that turn,
even if a command stages different next-turn settings. Submitted input keeps
the same identity and glyph in terminal scrollback. Terminal-unsafe code points
in a trusted model or effort selector are visibly escaped in the composer only;
the selected value supplied to the provider remains byte-for-byte unchanged.

The networked prompt identity and its chat/rollout views are specified in
`irc-chat.md`.

Tab uses the following order in every ordinary composer:

1. an empty draft cycles the available presentation views;
2. a nonempty slash-command prefix is completed when possible; and
3. other nonempty text retains the existing contextual action: indentation
   while idle and future-turn queueing while active.

Non-networked mode has only the rollout view, so empty Tab is a no-op. A
nonempty draft never changes views. Queue-edit composers retain their explicit
save behavior.

## Queue Commands

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
the view-cycle meaning defined above.

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
  byte-exact persisted text.
- PTY coverage demonstrates `/q` and `/queue` listing, delete, clear, and edit
  forms during active and idle operation.
- Store replay coverage rejects invalid edit targets and no-op edits, and
  reconstructs a valid edited queue. PTY coverage verifies the durable delete
  and clear events.
- Configuration coverage checks the default, bounds, duplicate-key handling,
  and explicit `typing_pause_ms` values.

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
  must be visible during a subsequent provider pause before `working…` appears
  alone on the following line; and
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
