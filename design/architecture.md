<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Architecture

snajpagent is a single foreground terminal process. It does not rely on a
daemon or socket to keep sessions alive. Durable state is written to local event
logs, and resume reconstructs the active session from those logs.

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

At most one yielded process can be unresolved. While it is active, the request
exposes only `write_stdin` and binds that tool's handle schema to the exact
active handle. A provider-supplied nonmatching handle is durably rejected
without starting the tool or touching the process, then returned to the next
bounded provider cycle. Invalid interaction arguments likewise leave the
matching process durably active. Terminal speech, refusal, an empty response,
multiple calls, or a call to another tool remain ordering violations and close
the process before the turn fails. The active handle is cleared only by a
terminal result for that exact process or an explicit durable closure event.

## Storage

Session data is append-only at the event level. Records are synced so a later
`snajpagent -r` can rebuild the conversation, active tool state, and local
lifecycle state without depending on process memory.

The default private application directory is `$HOME/.snajpagent`, with a `-d`
override. It contains `config.ini`, the `sessions/` and `trash/` directories,
and the atomically replaced `models.json` provider catalog. Cache age never
causes an implicit refresh. A missing catalog can be seeded offline from the
local Codex model cache; only explicit `/model cache` performs provider work.

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
managed process still narrows the tool surface to its exact `write_stdin`
continuation until that process reaches a terminal result.

## Tools

The first-party tool surface is deliberately small:

- `exec_command` for shell commands, including yielded long-running processes.
- `write_stdin` for continuing a yielded process.
- `apply_patch` for strict file edits using the patch grammar.
- `update_goal` while a persistent goal is active.

Provider credentials and configured secret environment variables are removed
from child tool environments or redacted before output is persisted or shown.
