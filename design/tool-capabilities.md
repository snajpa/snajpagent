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

`timer` is an ordinary always-visible tool. It accepts a positive `delay_ms`
and nonblank `text`; `delay_ms=0` cancels the current one-shot timer and permits
`text=null`. Scheduling replaces an earlier one-shot timer. The schedule is
durable in the session journal, so resume retains it. When due, the foreground
runtime records the firing and admits one fresh ordinary model turn containing
the reminder text. The turn is not a goal continuation and is not suppressed by
a blocked or paused goal; the full catalog is available so the model can inspect,
act, resume a goal, or report that a dependency remains. Explicit completion,
cancellation, or process exit still follows its existing semantics; a timer is
not an authorization to perform an otherwise refused side effect.

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
completion requires digest confirmation and EXIT. Actual native receipt paths
enter the model result and ordinary UI after terminal restoration. Source bytes
stay outside tool output. Read-only rejects export creation.

`download_queue` is always advertised with list/remove/clear actions and stable
item IDs. Listing is allowed in read-only mode; mutations follow its ordinary
refusal. Removal cancels intent without deleting source or completed local files.
Detached, unwrapped, interrupted and changed-source exports remain pending.
Explicit `/receive` and `/send` retain trzsz-go interoperability.
See [remote-terminal.md](remote-terminal.md) and the manual's Model tools and
Terminal file transfers sections for the wire and recovery lifecycle.
