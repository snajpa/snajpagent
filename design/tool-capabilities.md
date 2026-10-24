# Unconditional model tool capabilities

## 0.99.7 contract

Every ordinary provider request receives the complete native tool catalog. Goal
status, read-only mode, queue state, network configuration, continuation mode,
and session-leader role do not remove schemas. The catalog is a capability
contract, not an authorization grant: native dispatch returns a durable factual
`not_run`/terminal result when the current runtime cannot or must not perform a
call.

The catalog includes exploration, file modification, command/process, media,
web-search, goal lifecycle, timer, and IRC tools. Provider-native web search
keeps its provider-specific type while remaining present in every projection.
Configured audio routes and live IRC endpoints affect execution only.

Read-only turns keep their existing no-side-effect policy. Their full catalog is
still visible; side-effecting calls receive the existing read-only refusal. The
same rule applies to missing credentials, unavailable audio routes, absent IRC
runtime, invalid process handles, and configured tool rules.

## Timer

`timer` is an ordinary always-visible tool. Positive `delay_ms` and nonblank
`text` create an independent timer; `id` replaces one armed timer. `action=list`
returns IDs and deadlines. `action=cancel` or `delay_ms=0` cancels a named timer,
requiring omitted or null text. Without an ID, cancellation requires a sole armed
timer; no timers is a successful no-op. `recurring=true` repeats at `delay_ms`
intervals; false, null or omission selects a one-shot.

Schedules survive restart. A due receipt atomically consumes the one-shot or
rearms the recurring timer and admits its text as host-originated input. The next
recurring deadline is in the future; missed intervals coalesce into one wakeup.
Recovery retains an accepted pending input rather than firing the timer again.
Timer turns follow ordinary policy fences and leave paused or blocked goals
unchanged unless an explicit valid lifecycle action resumes one.

## IRC lifecycle

`irc_connect`, `irc_host`, and `irc_disconnect` expose the existing engine-owned
endpoint transitions. `irc_send`, `irc_state`, and `irc_topic` remain available
in the same catalog. Endpoint/privilege/connection checks happen at dispatch;
there is no schema gate based on whether the process started networked. A
successful dynamic connect/host enables the existing runtime tick and future
room-context projection. `irc_sleep` holds model delivery with timeout, accepted-nick
mention and message-count wakeups. `irc_compact` configures asynchronous IRC-only
context summaries through a branch of the same model and context. Both controls
persist; the operator transcript remains complete. See [IRC attention](irc-chat.md#model-controlled-attention-and-irc-context-summaries).

## Audit

`tests/test_context.c` checks that every projection variant has the same complete
function-name set, with only the provider-native search type varying by provider.
The check covers no goal, active/paused/blocked/completed goal state, read-only,
queued state, and network-disabled configuration. Existing runtime refusal tests
remain responsible for proving that visibility does not bypass read-only or
other execution constraints.

## Workstation file delivery

`send_file` is always advertised and requires `path`. Dispatch first commits a
pending export in the existing remote session, then probes for a live native
workstation client. A queued result reports intent, not delivery; acknowledged
completion requires digest confirmation and EXIT. The configured tool wait
bounds foreground admission; expiry or an admitted `/yield` returns pending
intent while one transfer worker retains the wire. `download_queue list` exposes
the stable pending ID until acknowledgement. Synchronous native receipts enter
the model result; background receipts appear in ordinary UI after restoration. Source bytes
stay outside tool output. Read-only rejects export creation.

`download_queue` is always advertised with list/remove/clear actions and stable
item IDs. Listing is allowed in read-only mode; mutations follow its ordinary
refusal. Removal cancels matching active transfer work and intent while preserving
source and completed local files.
Detached, unwrapped, interrupted and changed-source exports remain pending.
Explicit `/receive` and `/send` retain trzsz-go interoperability.
See [remote-terminal.md](remote-terminal.md) and the manual's Model tools and
Terminal file transfers sections for the wire and recovery lifecycle.

## Pinned commands

`running_commands` explicitly owns a full-PTY child for the session rather than
for one pending tool call. Actions start/list/show/write/stop use the existing
managed-process pump, redaction, input queue and cancellation machinery. Ordinary
exec/write_stdin handles remain separate and must settle normally. Read-only
turns admit list/show only. Each request receives derived job states without
captured output, keeping compaction aware of the existing children.

Definitions (stable ID, name, initial command, shell, directory, explicit resume
command, enabled flag) use required journal event kind274/version1 and optional
checkpoint payload version5. A newly resumed owner executes enabled hooks once;
live attachment and compaction do not. Stop commits disabled flags before signals.
Failures preserve definitions and report start_failed. Output stays in private
session files, inspected as bounded redacted base64 tails rather than repeated
conversation output. Natural exits never trigger implicit restarts.
