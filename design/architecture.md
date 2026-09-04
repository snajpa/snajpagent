<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Architecture

snajpagent is a single foreground terminal process. It can host a built-in IRC
server and maintain outgoing IRC connections, but does not fork a background
worker or rely on a socket to keep sessions alive. Durable state is written to
local event logs, and resume reconstructs the active session from those logs.

## Runtime Loop

Each accepted user turn is projected into an OpenAI-compatible Responses API
request. Streaming events update the terminal as they arrive. A final answer,
refusal, or completed tool cycle closes the turn; it does not close the
session.

An active persistent goal schedules another ordinary turn after a normal final
answer. Durable queued user turns take precedence over that continuation.
Goal turns carry a distinct `input_kind` and a developer continuation marker,
so they do not appear as new user messages. Refusal, failure, input closure,
and session reopening pause the goal; resumption is explicit.

Tool calls are handled one at a time. Before a tool runs, snajpagent records a
durable start event. After the tool finishes, snajpagent records the bounded
result and sends that result into the next provider cycle.

The active input pump services terminal input and IRC sockets during provider
streams and tool execution. Local-operator, current channel-operator, and
direct-mention messages are durably coalesced as urgent steering and admitted
at the earliest safe response/tool boundary. They do not truncate a generation
or cancel a command. Other room traffic is coalesced for a convenient boundary
and does not compel a model reply.

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
explicit durable closure event.

## Storage

Session data is append-only at the event level. Records are synced so a later
`snajpagent --resume` can rebuild the conversation, active tool state, and local
lifecycle state without depending on process memory.

The default private application directory is `$HOME/.snajpagent`, with a
`--dotdir` override. It contains `config.ini`, the `sessions/` and `trash/`
directories, and the atomically replaced `models.json` provider catalog. Cache
age never causes an implicit refresh. A missing catalog can be seeded offline
from the local Codex model cache; only explicit `/model cache` performs
provider work.

IRC configuration is process-local, while admitted room state is durable
session data. Typed events cover connection, membership, message, mode, topic,
history, and model-input snapshot transitions. They are validated before
append and replay; append occurs before transcript rendering or provider
projection.

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
- `write_stdin` for continuing a yielded process.
- `apply_patch` for strict file edits using the patch grammar.
- `update_goal` while a persistent goal is active.
- `irc_send`, `irc_state`, and privilege-checked `irc_topic` in networked mode.

Provider credentials and configured secret environment variables are removed
from child tool environments or redacted before output is persisted or shown.

## Rendering

Networked interactive output is a timestamped scrolling chat transcript. At
verbosity 0 it shows room/operator traffic and notifications but suppresses
local model speech and tool internals. Verbosity 1 reveals terminal model
replies, verbosity 2 adds intermediate commentary and compact tool activity,
and higher levels progressively add bounded runtime, durable, protocol, and
transport detail. Only terminal public assistant speech is sent to IRC.

One semantic 16-color foreground palette serves networked and ordinary modes.
`auto` emits attributes only on terminals and honors `NO_COLOR`; `always` and
`never` are explicit overrides. Escape sequences are presentation-only and
never enter durable events, model input, or IRC frames.
