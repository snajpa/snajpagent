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

Before `response_started`, the runtime builds the exact outgoing model-input
and request projections and accounts for them in token units. The default
`exact_token_count = auto` prefers the provider's Responses count endpoint,
remembers definitive endpoint absence, and falls back only for that known
unsupported capability. Ambiguous or transient count failures fail the turn;
`true` is strict and `false` disables preflight. A
compatible provider-reported usage record from the same source, model, effort,
and compaction lineage anchors later growth. Without either, the versioned
provider/model cache can supply a conservative estimate derived from the
largest exact canonical-byte/token pair observed, followed by one token per
canonical byte as the no-sample bound. Exact, anchored, statistical, and byte
methods remain distinct durable facts. An estimate alone does not reject an
otherwise sendable first request before a provider attempt. Source-bound
catalog limits or an exact `[model-limit PROVIDER/MODEL]` tuple determine the
hard input budget; `auto_compact_input_tokens = 0` disables only proactive
policy, never an authoritative hard guard.

Response and tool-result items grow immediately before the trailing active
goal/process controller messages. Anchor comparison reconstructs the previous
request from its transcript prefix plus that unchanged controller suffix, so
provider-reported token usage rolls forward across tool cycles. Any controller
change invalidates the anchor.

An over-budget request is not sent. Native Codex compaction or the existing
Responses summary path runs first, and the rebuilt request must be recounted
below the hard budget. Oversized historical tool/process and assistant text is
represented by a bounded provenance notice containing its byte size, digest,
and durable rollout path. The current user input, active controller state, and
tool schemas are never silently dropped. Responses-based recovery can compact
the oldest complete response/tool-group prefix hierarchically, including a
prefix inside an older turn, while preserving its remaining suffix, all newer
turns and the current turn. Cuts never separate calls from results or cross an
unresolved managed process. Deferred steering is included before its source
boundary; replay retains turn bookkeeping across summarized events without
repeating their content. The original append-only log remains untouched.
Compaction uses exact counting when available and the same source/model-bound
statistical estimator as normal requests otherwise; without a learned ratio
it retains the labeled byte upper bound. Prefix selection is scaled in bytes
after accounting for the complete provider envelope in tokens. A genuinely
indivisible group that cannot fit reports its event boundary and byte budget.

The stream decoder strictly interprets only response creation, output
structure, public text/refusals, function arguments, and terminal success or
failure. Other bounded `response.*` records are discarded after envelope
validation. Unsupported provider output occupies an inert decoder-local index
and never becomes durable response data or a local action; only exact completed
registered function calls enter the tool graph. A known successful terminal
snapshot remains required, while malformed envelopes and unknown
non-Responses event types fail closed. A response with no actionable item is
nonproductive. An explicit empty or oversized assistant message instead
creates one terse, size-specific developer correction for the next model
cycle; the normal operator UI does not present that correction as an error.

Structured non-2xx and SSE failures retain their bounded provider code,
message, and integral capacity details. A pre-output
`context_length_exceeded` closes the open response with the durable
`response_capacity_rejected` transition. Trustworthy context-limit or
requested-input detail lowers a durable in-session safety ceiling and updates
the same source/model-bound cache observation; replay restores the session
fact and later budget resolution applies either only while its source binding
still matches.
The runtime compacts and retries once only with a different request hash.
Partial output, an identical request, failed compaction, or a second rejection
terminates locally as a context-capacity failure rather than entering an
unbounded retry.

An active persistent goal schedules another ordinary turn after a normal final
answer. Durable queued user turns take precedence over that continuation.
Fresh goal-controller reminders are omitted throughout a queued turn (even
after the last item is dequeued) and whenever pending queue entries exist.
An unarmed queue blocks goal continuation until `/next`, explicit goal
start/resume, or queue removal. An open queue editor always prevents draining.
Goal turns carry a distinct `input_kind` and a developer continuation marker,
so they do not appear as new user messages. Refusal, failure, input closure,
and session reopening pause the goal; resumption is explicit.

Tool calls are handled one at a time. Before a tool runs, snajpagent records a
durable start event. After the tool finishes, snajpagent records the bounded
result and sends that result into the next provider cycle.

`/ro QUERY` selects a per-turn read-only toolset: `list_files`, `read_file`,
and `grep`, implemented in `tools_read.c` using native file/directory APIs and
POSIX regex, with no subprocesses, plus the same provider-hosted `web_search`
available in ordinary turns. Hosted search is executed by the provider, not
the local dispatcher; it sends queries to the provider and depends on its
model/tool support. Both file and web contents are untrusted data.
The existing `turn_started`,
`future_turn_queued`, and `future_turn_edited` data each carry a required
`read_only` boolean alongside normalized text. The session derives active mode
and queued origin from the durable turn-start event and clears both on closure.
The application dispatcher rejects every non-read-only tool before lifecycle,
IRC, fixture, and ordinary handlers. A forged local function named
`web_search` is rejected by the response graph before dispatch.
Request/count/semantic tool schemas expose the three native
functions and the hosted search tool only. A fresh read-only controller
survives compaction; no active
goal or IRC-reply controller is projected in this mode. See the manual for
literal path handling, no-follow semantics, regular-text requirements, and
fixed pagination/scan/output bounds. This is a capability restriction, not a
filesystem confidentiality sandbox or a freeze of independent processes.

The active input pump admits queued UI and IRC events during provider streams
and tool execution. Independent UI and IRC owners service the terminal and
sockets even when the engine is busy. Rollout Enter submits immediate steering: it
interrupts a provider response or returns a running command with a live handle,
then re-arms the empty active composer before the next model cycle. Outside
command/mention completion, Tab appends a future FIFO turn and changes neither the active response nor
the managed command. Only direct-mention IRC messages
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

`[tool] max_parallel_commands` bounds unsettled commands (default 4, range
1–32). The launching call's existing opaque local ID is also its process
handle: no second ID allocator or lookup registry. Native batches contain
independent calls, not a dependency graph. One engine poller starts a bounded
wave and services all stdout/stderr/stdin while provider work continues.
Short in-process adapters stay serial. Results commit in actual completion
order, identified by call ID. One call uses this same path.

Yielded jobs keep the normal tool catalog available; `write_stdin` can address
any known handle once per response. Invalid/duplicate interactions are not run
and never touch the job. A ready, uncollected result retains its slot. Final
answers and goal completion require every handle settled. A steer stops new
admissions, returns live handles without signaling them, and marks unstarted
calls `superseded_by_steering`; no skipped call launches after handoff. A batch
shares the smallest positive effective yield interval. Pending stdin is owned
once per handle and need not finish writing before steering can yield.

Native batching is requested by per-provider `parallel_tool_calls` (default
true), independently of the local concurrency limit. Both settings are frozen
with the turn. No JS runtime, worker pool, per-tool thread or detached service.
Cleanup fans out to all owned groups under one grace window. Exited leaders
remain unreaped until output collection so a reused PID cannot be signaled.

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

Interactive `/config` opens the exact active configuration path in `$EDITOR`.
The terminal returns to ordinary cooked mode while the editor owns it. After
the editor exits, snajpagent compares the file contents, transactionally parses
and validates a changed file, reapplies command-line overrides, and replaces
the live configuration only on success. Invalid edits stay on disk for repair
while the previous in-memory configuration remains active. Unchanged files are
not reloaded. Runtime presentation, tools, providers, and IRC topology consume
the replacement without restarting the process; a changed IRC topology is
reopened and rolled back to the prior topology if replacement initialization
fails. Durable session provider/model/effort preferences remain session state,
so configuration defaults do not overwrite an existing session.

IRC configuration is process-local, while admitted room state is durable
session data. Typed events cover connection, membership, message, mode, topic,
history, and model-input snapshot transitions. They are validated before
append and replay; append occurs before transcript rendering or provider
projection.

After any exit with a resumable open session, common cleanup restores the
terminal, closes IRC, and writes one exact
`• You can resume this session with the following command:\nCOMMAND\n` block
to stderr without changing the original status if that best-effort write
fails. The bullet header and command each begin at column zero with no
intervening blank line. With color enabled, the complete header has the same
bold-green lifecycle role as `• Compacted`, and the ANSI reset precedes the
header newline so the command line begins directly with uncolored command
bytes. The command
reuses the resolved dotdir and explicit config source, exact session ID,
explicit presentation settings, unconsumed one-turn preferences, and effective
IRC settings. Thus process-local IRC launch configuration does not enter the
event log but the operator can immediately recreate a client, server, or
combined process. The command contains neither prompts nor secret values.

SIGHUP and SIGTERM set signal-safe shutdown state, as does SIGINT outside the
interactive terminal handler. Idle and active provider/tool/network pumps
observe that state and unwind through common cleanup. Interactive Ctrl-C
preserves the visible composer line, appends `^C` and a newline, discards the
draft, and opens a clean prompt. Five consecutive presses within two seconds
request exit through durable cleanup, even while the engine is busy; other
input resets that sequence. Only an already-empty active
composer additionally interrupts the current turn through the normal durable
path. Empty Ctrl-D and terminal EOF request the same priority exit and interrupt
active work before cleanup; they do not wait for turn completion. One-shot
piped input still runs to completion. Uncatchable SIGKILL, power loss, and fatal
corruption cannot execute this path.

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
discovery preserves provider model, reasoning-variant order, and optional
capacity metadata in the local catalog; Codex routes use their dedicated
catalog shape. Capacity is source-bound to the configured URL and protocol,
while typed model identifiers are not checked against the catalog.

Hosted web search is exposed as a Responses request tool. There is no separate
helper binary for web search.
The active turn's provider URL selects `openrouter:web_search` for the exact
`openrouter.ai` hostname (case-insensitive, optional DNS dot/port); other hosts
use `web_search`. Arbitrary provider labels, model IDs, paths, userinfo, and
lookalike/subdomain hosts do not select OpenRouter. The same selection applies
to semantic, request, and count projections and to ordinary and `/ro` turns.
No search configuration or new local function is introduced. Hosted output
remains provider-owned; local function dispatch stays restricted as before.
The same hostname identity recognizes OpenRouter's absent optional token-count
route (404) in automatic counting; strict counting still fails, and generic
404/authentication failures remain errors. HTTP response status comes from
libcurl, including HTTP/2 and HTTP/3 status lines without reason phrases.
An unlabelled SSE `[DONE]` sentinel is accepted only after a valid Responses
terminal event; it cannot complete an otherwise unfinished response, and
other post-terminal events remain errors.

When a goal is active, context projection appends the current durable wording
and controller rules after replay and compaction. It also exposes the strict
`update_goal` function for rewrite, completion, or blocking. A bounded process
summary follows the goal controller; live handles do not narrow the catalog.
Active-prefix compaction retains that summary outside the summarized history
and never splits an unresolved call/result group.

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

Tool stdout and stderr are redacted and retained as bounded `process_output`
chunks in the existing session journal, without a capture cutoff. Results
reference contiguous per-stream ranges; successive polls return only newly
collected output. RAM staging is bounded and I/O service rotates among jobs.
The app owns journal writes, and the UI owns all display. `exec_command` and `write_stdin` require the
model to select a positive `max_output_tokens` or explicitly use `null` for
the configured `[tool] max_output_tokens` ceiling (6000 by default). Larger
requests are clamped to that ceiling; smaller requests are honored. Both tool
schemas advertise the ceiling, and one shared runtime selector enforces it. The
resolved value is recorded in the durable result so replay is independent of
later configuration. Model-context projection preserves a valid-UTF-8 head
and tail plus digest/provenance when the selected conservative
one-token-per-UTF-8-byte bound permits; it does not call bytes an exact token
count. The former `default_max_output_tokens` configuration key is removed.
`[tool] max_output_bytes` is presentation-only: it limits the number of
model-result UTF-8 bytes shown for each tool call. At level 3 and above, `0`
(the default) shows the complete retained result; level 2 also applies its
512-character preview budget, and levels 0/1 show no output body.
It never truncates the durable result or changes
the independently bounded model-context projection.

## Rendering

Networked interactive output is a timestamped scrolling chat transcript. All
verbosity levels show every actual room message, including the local model's
own sends, and retained room history. Private model speech and tool internals
stay in rollout. One process-local level is the exact `-v` count or `/verbose N`;
configuration reloads never replace it. Level 1 adds compact generic tool start
and outcome rows without output. Level 2 adds 1,024 argument / 512 output
character previews and reasoning summaries; level 3 has full retained tools.
Levels 4/5/6 add live runtime/durable, redacted protocol and transport diagnostics
only in visible rollout. Unseen conversation, tool and IRC records use durable
event references and current presentation policy; raising the level does not
replay visited content. All modes share this ladder. Only explicit `irc_send`
publishes model speech to the room.

Committed compaction and goal set/clear milestones are terse bullet-prefixed
semantic rollout records even at verbosity 0. Networked chat queues them for
exact-once rollout catch-up; ordinary rollout displays them immediately.
Durable-event verbosity may add event identity without changing the baseline
wording.

One semantic 16-color foreground palette serves networked and ordinary modes.
`auto` emits attributes only on terminals and honors `NO_COLOR`; `always` and
`never` are explicit overrides. Escape sequences are presentation-only and
never enter durable events, model input, or IRC frames. Successfully installed
lifecycle milestones use a dedicated bold-green role.

The public-item renderer incrementally recognizes a bounded Markdown subset
across provider and UTF-8 delta boundaries. It shares the existing wrapping,
composer, and terminal-safety path; syntax-only prefixes may remain pending,
but complete semantic text is painted before a delivery callback returns.
Buffered assistant history and non-operator IRC model messages use the same
presentation, with fenced-code state isolated by endpoint and sender. Operator
messages and other IRC events remain literal. Markdown and color can be disabled
independently, and neither changes stored, provider, redirected, or IRC bytes.
