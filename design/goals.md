<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Persistent Goals

`/goal` runs one user objective across as many ordinary model turns as the
objective needs. Goals belong to the current durable session and run only in
the foreground snajpagent process, with no background worker.

## Command Grammar

The interactive commands are:

```text
/goal
/goal status
/goal help
/goal TEXT
/goal "TEXT"
/goal set TEXT
/goal set "TEXT"
/goal pause
/goal resume
/goal lock
/goal unlock
/goal complete
/goal cancel
```

Bare `/goal` and `/goal status` show the current goal identifier, state,
wording-lock state, model-turn count, byte limit, wording, and blocker when
present. `/goal help` prints the grammar and the reserved first words.

`/goal TEXT` starts a new goal when no unfinished goal exists. When a goal is
active, paused, or blocked, it changes that goal's wording without changing
its status or lock. The unquoted form is available when the first word is not
one of `status`, `help`, `set`, `pause`, `resume`, `lock`, `unlock`, `complete`,
`cancel`, or `clear`. `/goal set TEXT` is the unambiguous spelling when the wording
starts with a reserved word.

An entire goal may instead be enclosed in one pair of double quotes. The outer
quotes are removed and everything inside them is retained literally; no
backslash escape language is applied. For example,
`/goal "pause after the release is verified"` sets goal text rather than
pausing the current goal. Empty or whitespace-only wording is rejected.

`/goal pause` prevents another automatic goal turn. During a running turn it
takes effect at that turn's terminal boundary; Ctrl-C remains the immediate
turn interruption. `/goal resume` resumes a paused or blocked goal and begins
the next turn immediately. `/goal lock` prevents the model from changing the
wording; `/goal unlock` restores that ability. Locking never prevents the user
from changing wording and never prevents the model from completing or blocking
the goal. `/goal complete` is the user's explicit successful terminal state.
`/goal clear` (also `/goal cancel`) is the user's terminal stop without a
completion claim. It leaves the current turn running and prevents subsequent
automatic goal turns. Controls accept whitespace around the command word; real
extra text is rejected.

A completed or cancelled goal remains visible through `/goal`. Supplying new
wording after either terminal state creates a new goal with a new identifier,
an unlocked wording, and an active state.

## Wording Limit

Goal wording is valid UTF-8 and has a configurable byte limit:

```ini
[agent]
max_goal_prompt_bytes = 262144
```

The default is 262,144 bytes. Values from 1 through 1,048,576 are accepted.
The limit applies when a user or the model supplies new wording. Lowering the
configuration later does not corrupt or invalidate wording already stored in
a durable session, but any subsequent replacement must fit the current limit.

## Model Control

When no unfinished goal exists, the provider receives a strict `create_goal`
function tool with one required string argument:

```json
{"objective":"the persistent objective"}
```

The model may call it only when the user or system/developer instructions
explicitly request starting or setting a persistent goal. It must never infer
a goal from an ordinary task. Writing, updating, or committing a Markdown
plan or goal document does not activate snajpagent continuation; only a
successful `create_goal` call or the user's `/goal TEXT` command does.

A successful call validates the objective under the same UTF-8 and byte-limit
rules as `/goal`, appends the durable `goal_started` event, and arms automatic
continuation in interactive and one-shot foreground operation. The current
direct turn still reaches its normal final answer; that final is the first
checkpoint, after which queued user turns run before the first synthetic goal
turn.

While a goal is active, the provider receives a strict `update_goal` function
tool with required action and action-dependent text:

```json
{"action":"rewrite","text":"new wording"}
{"action":"complete"}
{"action":"block","text":"specific blocking condition"}
```

`rewrite` changes the durable wording when it is unlocked and within the
configured byte limit. A locked rewrite returns a failed factual tool result
and leaves the goal unchanged. `complete` records successful completion.
`block` records why no dependency-ready action remains and stops automatic
continuation. A blocked goal can later be changed, unlocked, or resumed by the
user.

`create_goal` and `update_goal` are mutually exclusive. An active goal exposes
only `update_goal`; a paused or blocked unfinished goal exposes neither; a
completed, cancelled, or never-created goal exposes only `create_goal`.
Neither lifecycle tool is exposed while an unresolved managed process
restricts the coding-tool surface to the exact final `write_stdin`
continuation. In networked mode IRC tools may precede that continuation so
urgent chat can be handled without abandoning the process. snajpagent never
parses lifecycle requests, completion claims, documentation, or magic phrases
from assistant prose.
An attempted final answer while handles remain unsettled is rejected. Its
model-facing recovery note names `write_stdin` and points to the current
process snapshot; the model must collect terminal results before finalizing.
This guidance does not transfer command ownership across turns. A terminal
turn failure still closes its owned commands.

## Turn Boundaries And Continuation

Each goal iteration is an ordinary durable turn with the selected provider,
model, effort, discovered project instructions, tool fences, token counting,
and compaction behavior. The active wording and controller rules are projected
on every goal response, including after conversation compaction.

When a model turn ends:

1. `completed`, `blocked`, `paused`, and `cancelled` goals stop and return to
   the idle prompt after the current turn has closed.
2. A normal final answer while the goal is still active is a checkpoint, not a
   completion signal. snajpagent immediately starts another goal turn.
3. User turns queued during the active turn run in FIFO order before that next
   automatic continuation. No fresh goal-controller reminder is projected
   throughout those turns, including the final dequeued item, or whenever
   another item remains pending. A paused/unarmed queue also prevents goal
   continuation until `/next`, explicit goal start/resume, or queue removal;
   an open queue editor always prevents draining. It is never bypassed. Historical
   goal mentions remain conversation context, and already-frozen requests are
   not retroactively changed. A `/ro` query exposes no goal lifecycle tools.
4. Refusals and errors retain the goal's active state and its continuation arm.
5. Provider, protocol, context, resource, output and tool failures enter paced,
   interruptible recovery without an attempt limit. Provider/protocol failures
   continue the same turn with completed tool results and live process handles.
   Local persistence/adapter failures must reconcile state before new admissions.
   Ordinary terminal tool results remain available for the model to handle. A single Ctrl-C turn
   interruption also pauses the goal. Quitting the process (Ctrl-D/EOF,
   SIGHUP or SIGTERM) interrupts the turn and preserves the current goal state;
   it is not a goal pause. A Ctrl-C interruption already processed before a
   subsequent exit keeps its recorded pause.
6. Opening a session restores the saved goal state unchanged after any
   interrupted-turn recovery. An active goal continues; paused, blocked,
   completed and cancelled goals keep their recorded states. Resume creates
   no goal pause/resume event. An explicit initial prompt runs first, and
   retained unarmed queued work still prevents automatic goal continuation.

Goal state, wording changes, locks, blockers, and status transitions are
append-only session events. Resume reconstructs them from the same validated
event log as turns and tools. Synthetic continuation turns remain identified as
goal turns in that log. For provider transport, each goal request uses a labelled
host-continuation message in the user-role input slot. This keeps the current
goal request in the conversation when gateways move developer/system messages
into top-level instructions, including when previous assistant replies remain.
Historical markers stay history; restoring them never resumes a paused goal.

The same labelled input is used as a fallback when compaction leaves only
instruction messages. That fallback is added only when no user, assistant or
tool conversation remains; Responses compaction follows the same rule. Both
forms are transport input, not new operator messages: they add no task, approval,
receipt timestamp or goal transition. They are included in request hashes/counts.
The final developer-level response boundary and compatible reasoning remain
intact. Pause, read-only, queued work and frozen-turn rules remain unchanged.

Restoration is separate from continuation. Reopening a session automatically
displays its saved goal, wording, status, revision, lock and blocker, even when
resume history is disabled. Ordinary model requests retain the current wording
and status of paused/blocked unfinished goals after replay and compaction;
their saved state is context, not an instruction to resume. No goal is recreated
and the user need not repeat its wording. Read-only/queued goal-controller
suppression and the lifecycle tool restrictions above remain unchanged.

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
the next attempt presents the current user input, admitted steering, current goal
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

`[agent] max_turn_retries` defaults to three additional attempts; zero disables
ordinary automatic recovery. Reuse the retained-turn recovery path and capture
one budget at logical turn start. A successful intermediate response or tool
never refills it; a new prompt or manual `/retry` does. Emit `turn_failed` and
its manual hint only after exhaustion. An active goal always bypasses this
limit, even when configured to zero. User interruption and goal-state changes
remain authoritative; an existing paused/blocked goal is never resumed by retry.
Fresh queued/background input reported at request failure retains the existing
next-turn handoff instead of waiting for ordinary retries to exhaust.
