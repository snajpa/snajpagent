<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Persistent goals

A session can retain several independent objectives. The model receives every
goal and its state and chooses useful work among active goals. The session owner
runs one model turn at a time. A final answer is a checkpoint while any goal is
active. These multi-goal controls describe current source.

## Command grammar

```text
/goal
/goal status
/goal help
/goal TEXT
/goal "TEXT"
/goal add TEXT
/goal set [ID] TEXT
/goal pause [ID]
/goal resume [ID]
/goal lock [ID]
/goal unlock [ID]
/goal complete [ID]
/goal cancel [ID]
/goal clear [ID]
```

`/state goal` accepts the same arguments. Bare `/goal` and `/goal status` list
all goals, including terminal and replaced goals, with their full IDs, wording,
state, lock, revision, turn count, blocker, wait destination and lineage.

`/goal TEXT` and `/goal add TEXT` create an additional active goal. Use `add` or
quote the entire wording when its first word is a command. One pair of outer
double quotes is removed; backslashes have no escape meaning. Empty or
whitespace-only wording is rejected.

`/goal set ID TEXT` replaces one unfinished goal. The replacement gets a fresh
ID and retains its parent's state, lock and blocker. The parent remains visible
as replaced. With one unfinished goal, `set TEXT` selects it; with none, it
starts a goal. An unknown or terminal explicit ID fails. With several unfinished
goals, `set`, `lock`, `unlock`, `complete`, `clear` and `cancel` require a full ID.

`/goal pause` pauses every unfinished goal, including blocked goals. Add an ID
to pause only that goal. The current turn can finish; Ctrl-C on an empty composer
interrupts it. `/goal resume` activates all unfinished goals and submits explicit
operator direction to resume work. The next model request includes the complete
goal/state list. `/goal resume ID` activates only the named goal and leaves
other stops intact.

`lock` prevents model rewording and blocking; the operator can still replace
wording. Model completion and resume remain available. `complete` records success.
`cancel` and `clear` record a terminal stop without a success claim. These
commands leave the current turn and queued ordinary work intact.

## Wording limit

```ini
[agent]
max_goal_prompt_bytes = 4096
```

The default per-goal limit is 4096 UTF-8 bytes. Values from 1 through 1,048,576
are accepted. The limit applies to new wording from the operator or model.
Lowering it preserves previously saved wording; a replacement must fit the
current limit.

## Model controls

`create_goal` accepts `objective` and creates an independent goal. Only an
explicit user or system/developer request authorizes creation. Ordinary tasks,
Markdown plans and recorded proposals do not activate continuation.
An existing blocked goal does not prevent creation of an authorized continuation
goal. Its blocked state and dependency remain unchanged, including across resume.

`update_goal` accepts `id`, `action`, `text` and `wait_for`. Omit `id` only when
exactly one goal is unfinished. Example calls, with `ID` replaced by a listed ID:

```json
{"id":"ID","action":"rewrite","text":"new wording"}
{"id":"ID","action":"complete"}
{"id":"ID","action":"block","text":"waiting for a reply","wait_for":"irc: team/secretary"}
{"id":"ID","action":"resume","text":null}
```

Rewrite accepts active, paused or blocked goals and preserves their state. A
locked goal rejects model rewrite and block. Complete and block require an active
goal. Resume accepts a paused or blocked goal. Complete and resume require omitted
or null `text`; non-block actions require omitted or null `wait_for`.

Block requires a concrete reason and a single-line wait destination: `operator`,
`timer`, `irc: endpoint/nick`, `process: handle`, or `external: concrete dependency`.
Each block text field permits 64 KiB. Use `operator` only when operator input is
required. This label describes the dependency; it does not schedule a wakeup.
Old block records without a destination retain an unspecified wait. Completion,
resume and cancellation clear the wait. Blocking one goal leaves other active
goals running. A failed attempt is not completion or a reason to block.

Lifecycle tools follow read-only, queued-work, managed-process and policy fences.
Managed command handles must be collected before model completion. Assistant
prose never substitutes for a successful lifecycle call. A final response with
live commands parks an interactive turn while input and output collection stay
available; it does not complete the goal or discard handles.

## Continuation and recovery

Every request projects all goals and states, including after compaction, minimal
recovery, restart and fork. Read-only and queued work receive the saved list with
a suspended-controller notice. That list authorizes no extra work in those turns.
Queued operator prompts run before another automatic goal turn. An unarmed queue
still requires `/next` or explicit goal start/resume; an open editor prevents drain.

Automatic continuation proceeds while any goal is active. When none is active,
paused goals retain their explicit stop and blocked goals retain their dependency.
The idle notice identifies each paused or blocked goal. Policy stops and explicit
turn interruption pause active goals. Ordinary errors retain active goals and use
the existing retry path. No per-goal turn or step budget is added.

Goal state is an immutable ID-keyed directory. Legacy scalar fields remain a
last-event compatibility projection, not a selected goal cursor. Events target
one ID; replacements retain parent/replacement links. Goal turns count against
all active goals represented in that turn. Native checkpoints retain every goal
and its literal origins. Old single-goal records restore as a one-entry directory;
saved wording is never checked against a later lower configured limit.

A skill activated with one unfinished goal binds to that ID. With several goals,
it retains the IDs unfinished at activation and stays active while any of those
IDs is unfinished. This lifetime survives restart and fork and cannot attach to
an unrelated goal created later. Existing visit and read-only lifetimes still
apply.

## Independent model timers

The `timer` tool schedules a new ID when `id` is omitted, replaces a named armed
timer when supplied, lists with `action=list`, and cancels with `action=cancel`
or `delay_ms=0`. Cancellation without an ID works only with one armed timer;
no timers is a successful no-op. Scheduling requires positive `delay_ms` through
4294967295 and nonblank `text`. `recurring=true` repeats at that interval; omitted,
null or false schedules a one-shot. The returned ID addresses that timer alone.

Each due timer admits its stored text as host-originated input for one ordinary
turn. It does not resume a paused or blocked goal. Admission and consume/rearm
share one durable receipt. Recovery retains either the armed timer or its accepted
pending input, closing the gap between firing and input admission. Recurring
wakeups keep their ID and advance to the first future interval. Missed periods
coalesce into one wakeup after delay or restart, with no catch-up replay. Policy
and turn fences still govern what the model can do.

## Controls during active work

All local goal commands remain enterable while the model or tools are active.
Goal state changes are durable immediately; pause/clear affect continuation and
allow the current turn to finish. An empty-composer Ctrl-C explicitly interrupts
that turn. Clear/cancel retain history and do not erase queued ordinary work.
Controls retain their ordinary meaning in queue editing and delete confirmation;
command text is never interpreted as deletion consent.

Source revision matters: a rebuilt executable affects new launches. An existing
process keeps its mapped code until normal exit/resume. A published tag describes
its own immutable code and manual, not all later source fixes.

## Recovery and input provenance

`turn_recovery` closes a failed response attempt without closing its turn or
managed processes. Consecutive recovery notices are coalesced in model context;
detailed diagnostics stay in the journal. Explicit interruption remains separate.
An explicit zero-yield managed call returns after the accepted call wave is
admitted; an omitted yield with a configured default of zero waits until a
terminal result or max_wait_ms. A final reply while commands remain unsettled
cannot complete that turn. On actual cancellation/failure, close only the
owned processes and commit each terminal/unknown `process_closed` result before
`turn_failed` or `turn_interrupted`; an owner-lost handle and its journaled
bytes remain discoverable without claiming a successful command result.
An oversized active turn is not an invitation to repeatedly summarize its entire
tool transcript. Compaction is a *covered-prefix* checkpoint: a completed
compaction retains its source event boundary and later compaction starts from
that boundary plus only the bounded seam needed for continuity. Earlier images,
completed calls and tool results remain in the journal, not a new compaction
source. Normal same-binding compaction of a tractable prefix remains useful.
If the provider rejects a turn request or token count despite these checkpoints,
the next attempt presents the current user input, admitted steering, all goals
and host controls without automatic replay of the old transcript or compaction
output. It points to bounded history/goal/output retrieval so the model can
recover the specific facts it needs. A successful request records `context_rebased`
before `response_started`; a rejected or interrupted attempt never claims that
boundary. Completed tool effects are not re-executed. A second rejection of the
already minimal request fails the turn rather than re-compacting the same
checkpoint. Resume and model switching retain the same boundary semantics,
including when there is no active goal. Steering within that turn retains the
existing rebase instead of recording a second checkpoint. A newly completed
model response after that boundary supplies fresh history; if it later exceeds
the limit, the same turn can advance to another checkpoint rather than being
treated as an unchanged minimal request. When that turn began before the
retained overlap, its own rebase still establishes the active-turn boundary
for later compaction; a rebase from another turn does not move that boundary.
An active IRC update prompt contains durable room-event references. Minimal
recovery resolves only the references named by that current prompt and supplies
their message text, not unrelated old room traffic; the journal remains the
authority for original receipt, sender and room metadata.
Failed compaction attempts record `compaction_interrupted` with reason `error`
before retry or exit. Replay also clears an unfinished compaction at a recorded
turn recovery/termination boundary for journals written by older versions that
only cleared it in memory. Successful compact output and tool effects remain
unchanged; event validation and the original journal hash chain are preserved.

New input carries host-generated UTC receipt and first-request-admission times.
`input_admitted` records the latter before request projection, including counting
requests. Replay keeps both fixed; optional receipt metadata preserves UI queue
delays. User text/roles are unchanged, and older unavailable times stay absent.

## Ordinary-turn retry budget

`[agent] retry_auto` defaults to true. A durable `retry_auto_changed` string
override takes precedence until changed by `/retry auto [on|off]`. Missing saved
state inherits the owner's retained configuration; `/configure` refreshes that
default. The foreground provider consults the live owner at each retry boundary;
the IRC summary owner receives atomic policy changes without sharing config.
Disabling retry lets healthy responses finish and stops transport/turn backoff.
A failed active goal keeps its status and requires explicit `/retry` or new work;
view commands and enabling retry alone do not restart its failed turn.

`[agent] max_turn_retries` defaults to five additional attempts; zero disables
ordinary turn recovery while preserving short transport retries. Capture the
limit at logical turn start. A successful actionable response resets consecutive
failure attempts; a new prompt or manual `/retry` starts a fresh budget. Emit
`turn_failed` and its hint after exhaustion or disabling. When retry is enabled,
an active goal bypasses this limit. User interruption and goal-state changes
remain authoritative; a paused/blocked goal is never resumed by retry. Eligible
queued/background input retains its next-turn handoff. Ordinary chat held behind
a paused/blocked goal cannot veto recovery.
