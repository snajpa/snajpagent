<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Architecture

snajpagent is a single foreground terminal process. It can host a built-in IRC
server and maintain outgoing IRC connections, but does not fork a background
worker or rely on a socket to keep sessions alive. Durable state is written to
local event logs, and resume reconstructs the active session from those logs.
The append-only rollout for a session is
`$DOTDIR/sessions/<session-id>/events.jsonl`. Once a manual or automatic
compaction succeeds, context projection places a synthetic developer notice
with that absolute path immediately after the compact output. The notice is
rebuilt during replay and does not modify the provider-produced compact output
or its recorded hash and token count.

## Runtime Loop

Each accepted user turn is projected into an OpenAI-compatible Responses API
request. Streaming events update the terminal as they arrive. A final answer,
refusal, or completed tool cycle closes the turn; it does not close the
session.

The stream decoder strictly interprets only response creation, output
structure, public text/refusals, function arguments, and terminal success or
failure. Other bounded `response.*` records are discarded after envelope
validation. Unsupported provider output occupies an inert decoder-local index
and never becomes durable response data or a local action; only exact completed
registered function calls enter the tool graph. A known successful terminal
snapshot remains required, while malformed envelopes and unknown non-Responses
event types fail.

An active persistent goal schedules another ordinary turn after a normal final
answer. Durable queued user turns take precedence over that continuation.
Goal turns carry a distinct `input_kind` and a developer continuation marker,
so they do not appear as new user messages. Refusal, failure, input closure,
and session reopening pause the goal; resumption is explicit.

Tool calls are handled one at a time. Before a tool runs, snajpagent records a
durable start event. After the tool finishes, snajpagent records the bounded
result and sends that result into the next provider cycle.

The active input pump services terminal input and IRC sockets during provider
streams and tool execution. Local Enter submits immediate steering: it
interrupts a provider response or returns a running command with a live handle,
then re-arms the empty active composer before the next model cycle. Local Tab
instead appends a future FIFO turn and changes neither the active response nor
the managed command. Current channel-operator and direct-mention IRC messages
remain durably coalesced for their documented urgent boundary. Other room
traffic is coalesced for a convenient boundary and does not compel a model
reply.

A steered provider response durably records its byte-exact public prefix. The
next request projects that prefix as prior assistant output followed by an
explicit developer boundary and each exact user-role steer in arrival order.
Public response indexes are strictly increasing but need not be consecutive,
because non-public provider items occupy indexes too.

There is no default count ceiling on response cycles or tool invocations in a
turn. A turn continues until it completes, is explicitly interrupted, or hits
an actual provider, protocol, storage, or machine-representation failure.
Per-response item, call, argument, output, and wire-size bounds remain in force.

At most one yielded process can be unresolved. While it is active, the request
always exposes `write_stdin` with a schema bound to the exact active handle. In
networked mode the IRC send/state/topic tools may precede it so urgent chat can
be handled, but the response must end with exactly one matching `write_stdin`;
other coding tools remain unavailable. A provider-supplied nonmatching handle
is durably rejected without touching the process, then returned to the next
bounded provider cycle. Invalid interaction arguments likewise leave the
matching process durably active. Terminal speech, refusal, an empty response,
or invalid call ordering closes the process before the turn fails. The active
handle is cleared only by a terminal result for that exact process or an
explicit durable closure event. A local steer returns a running result with
`reason=steering_handoff` and does not signal the process. The matching
`write_stdin` can explicitly terminate it through the same bounded
managed-process closure used by required turn/session cleanup.

## Storage

Session data is append-only at the event level. Records are synced so a later
`snajpagent --resume` can rebuild the conversation, active tool state, and local
lifecycle state without depending on process memory.

The default private application directory is `$HOME/.snajpagent`, with a
`--dotdir` override. It contains `config.ini`, the `sessions/` and `trash/`
directories, and the atomically replaced `models.json` provider catalog. Cache
age never causes an implicit refresh. A missing catalog directs the operator to
explicit `/model cache`; snajpagent neither imports nor depends on Codex CLI
cache state.

IRC configuration is process-local, while admitted room state is durable
session data. Typed events cover connection, membership, message, mode, topic,
history, and model-input snapshot transitions. They are validated before
append and replay; append occurs before transcript rendering or provider
projection.

After any exit with a resumable open session, common cleanup restores the
terminal, closes IRC, and writes one `resume: COMMAND` line to stderr without
changing the original status if that best-effort write fails. The command
reuses the resolved dotdir and explicit config source, exact session ID,
explicit presentation settings, unconsumed one-turn preferences, and effective
IRC settings. Thus process-local IRC launch configuration does not enter the
event log but the operator can immediately recreate a client, server, or
combined process. The command contains neither prompts nor secret values.

SIGHUP and SIGTERM set signal-safe shutdown state, as does SIGINT outside the
interactive terminal handler. Idle and active provider/tool/network pumps
observe that state and unwind through common cleanup. Active terminal Ctrl-C
continues to interrupt only the current turn, while an empty Ctrl-D closes
input. Uncatchable SIGKILL, power loss, and fatal corruption cannot execute
this path.

## IRC Runtime

The integrated nonblocking IRC runtime can own one bounded single-room server,
up to 16 outgoing endpoints, and distinct local agent/operator connections.
The server advertises its sole room, grants ordinary operator clients `+o`,
keeps bounded timestamped history, and supports the registration, room,
liveness, topic, mode, and chat subset needed by normal clients. Outgoing
connections join the advertised room and reconnect autonomously after socket
or protocol failure. Per-connection read/write bounds isolate malformed or
slow peers.

First join produces a user-role snapshot of topic, members/operator flags, and
bounded history before the first networked model response. Every successful
manual or automatic compaction appends another fresh snapshot before the next
provider response. Direct messages are deliberately excluded from model
control; only traffic for the advertised room participates.

## Provider

The provider layer targets the Responses API over HTTP/SSE. Ordered named
provider sections independently configure base URLs, credential environment
variables, timeouts, counting, and compaction. Turns route through the durable
provider/model/effort session selection. Authenticated `GET /v1/models`
discovery preserves provider model and reasoning-variant order in the local
catalog, but typed model identifiers are not checked against the catalog.

Hosted web search is exposed as a Responses request tool. There is no separate
helper binary for web search.

When a goal is active, context projection appends the current durable wording
and controller rules after replay and compaction. It also exposes the strict
`update_goal` function for rewrite, completion, or blocking. An unresolved
managed process narrows the coding-tool surface to its exact `write_stdin`
continuation until that process reaches a terminal result; networked requests
retain the IRC tools before that required continuation.

## Tools

The first-party tool surface is deliberately small:

- `exec_command` for shell commands, including yielded long-running processes.
- `write_stdin` for waiting on, interacting with, or explicitly terminating a
  yielded process.
- `apply_patch` for strict file edits using the patch grammar.
- `update_goal` while a persistent goal is active.
- `irc_send`, `irc_state`, and privilege-checked `irc_topic` in networked mode.

Provider credentials and configured secret environment variables are removed
from child tool environments or redacted before output is persisted or shown.
`exec_command.timeout_ms` is nullable: the built-in default is no foreground
handoff deadline, and the model supplies a positive millisecond value only
when the particular command should be returned to it if still running.
`[tool] default_timeout_ms` may impose an operator-configured fallback,
including `0` for no automatic handoff.
`[tool] max_timeout_ms` is the operator-selected ceiling for positive
timeouts; the same value is advertised in the model-facing tool schema and
enforced by the runtime instead of a separate fixed policy ceiling.

Every command uses the managed-process path even when it normally completes
in the first tool call. If its timeout expires, or urgent local steering or an
IRC mention arrives, that same path returns a live handle and continues the
command in the background. The next model cycle receives the elapsed-timeout
notice or coalesced urgent input plus the exact `write_stdin` continuation; it
may react immediately, wait for completion, interact, or explicitly terminate
the process. Timeout expiry and steering handoff never signal or kill the
command. Explicit user interruption and required turn/session closure retain
their existing cancellation behavior.

Tool stdout and stderr are redacted before capture, retained without a
tool-specific length cutoff, and supplied completely to the next model cycle.
`[tool] max_output_bytes` is presentation-only: it limits the number of
model-result UTF-8 bytes shown for each tool call, while `0` (the default)
shows the complete result. It never truncates the durable result or the output
given to the model.

## Rendering

Networked interactive output is a timestamped scrolling chat transcript. At
verbosity 0 it shows room/operator traffic and notifications but suppresses
local model speech and tool internals. Verbosity 1 reveals terminal model
replies plus every tool call, its complete arguments, completion state, and
configured amount of result text. Verbosity 2 adds intermediate commentary,
and higher levels progressively add runtime, durable, protocol, and transport
detail. Ordinary mode uses the same single-`-v` tool visibility. Only terminal
public assistant speech is sent to IRC.

One semantic 16-color foreground palette serves networked and ordinary modes.
`auto` emits attributes only on terminals and honors `NO_COLOR`; `always` and
`never` are explicit overrides. Escape sequences are presentation-only and
never enter durable events, model input, or IRC frames.

The public-item renderer incrementally recognizes a bounded Markdown subset
across provider and UTF-8 delta boundaries. It shares the existing wrapping,
composer, and terminal-safety path; syntax-only prefixes may remain pending,
but complete semantic text is painted before a delivery callback returns.
Buffered assistant history and non-operator IRC model messages use the same
presentation, with fenced-code state isolated by endpoint and sender. Operator
messages and other IRC events remain literal. Markdown and color can be disabled
independently, and neither changes stored, provider, redirected, or IRC bytes.
