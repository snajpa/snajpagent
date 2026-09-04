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
   automatic continuation.
4. A refusal pauses the goal instead of repeating the same refusal.
5. A provider, protocol, context, resource, output, or tool failure that stops
   the current turn pauses the goal. An ordinary terminal tool result remains
   available to the model to handle within that turn. Ctrl-C interruption and
   terminal input closure also pause the goal.
6. Opening a session that still has an active goal durably pauses it after any
   interrupted-turn recovery. The user must issue `/goal resume`; merely
   opening a session never starts work unexpectedly.

Goal state, wording changes, locks, blockers, and status transitions are
append-only session events. Resume reconstructs them from the same validated
event log as turns and tools. Synthetic continuation turns are identified as
goal turns in that log and do not masquerade as new user messages.
