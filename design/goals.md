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
or `cancel`. `/goal set TEXT` is the unambiguous spelling when the wording
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
`/goal cancel` is the user's terminal stop without a completion claim.

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
tool with two required arguments:

```json
{"action":"rewrite","text":"new wording"}
{"action":"complete","text":null}
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
event log as turns and tools. Synthetic continuation turns are identified as
goal turns in that log and do not masquerade as new user messages.

Gateways may move developer/system messages into top-level instructions. When
compaction leaves only those messages, request projection adds one labelled
host-continuation marker in the user-role input slot. It is transport input,
not a new operator message: it carries no new task, approval, receipt timestamp
or goal transition. Existing instruction roles and compact output remain intact.
The marker is regenerated only when needed, included in request hashes/counts,
and omitted whenever user, assistant or tool conversation remains. Responses
compaction uses the same rule for instruction-only source context.

Restoration is separate from continuation. Reopening a session automatically
displays its saved goal, wording, status, revision, lock and blocker, even when
resume history is disabled. Ordinary model requests retain the current wording
and status of paused/blocked unfinished goals after replay and compaction;
their saved state is context, not an instruction to resume. No goal is recreated
and the user need not repeat its wording. Read-only/queued goal-controller
suppression and the lifecycle tool restrictions above remain unchanged.

## Recovery and input provenance

`turn_recovery` closes a failed response attempt without closing its turn or
managed processes. Consecutive recovery notices are coalesced in model context;
detailed diagnostics stay in the journal. Explicit interruption remains separate.
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
