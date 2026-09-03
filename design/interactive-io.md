<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Interactive Output And Queue Editing

This note defines how streamed model output and the active input composer share
the terminal, and how users inspect and modify queued turns.

## Streamed Output And Typing

- Public model text is soft-wrapped at word boundaries to the current terminal
  width. Explicit model newlines remain explicit, and a single word wider than
  the terminal may hard-wrap.
- Wrapping is a terminal presentation detail. Stored response text, partial
  response events, redirected output, and provider protocol data remain byte
  exact and do not gain presentation newlines.
- While a turn is active, the first edit after visible model output starts the
  `steer › ` composer on a new terminal line immediately.
- Visible model output pauses while the user is editing. Each edit restarts the
  pause. After the pause expires, the current composer line remains as a
  readable snapshot and model output resumes on the following line.
- If editing resumes after more model text, that text is ended on its current
  line and the updated draft is shown on a new `steer › ` line. This cycle can
  repeat without losing or changing the draft.
- `[ui] typing_pause_ms` controls the inactivity pause. It defaults to `500`,
  accepts `0` through `5000`, and applies only to interactive terminal display.
  A value of `0` retains the line separation but disables the delay.

The pause provides display focus, not a provider-generation guarantee. Input,
interrupts, and local active-turn commands remain responsive while output is
paused.

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
typing it directly and pressing Tab.

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
  text. PTY coverage demonstrates newline-separated steer snapshots, pause
  reset on continued typing, output resumption after the configured delay, and
  byte-exact persisted text.
- PTY coverage demonstrates `/q` and `/queue` listing, delete, clear, and edit
  forms during active and idle operation.
- Store replay coverage rejects invalid edit targets and no-op edits, and
  reconstructs a valid edited queue. PTY coverage verifies the durable delete
  and clear events.
- Configuration coverage checks the default, bounds, duplicate-key handling,
  and explicit `typing_pause_ms` values.
