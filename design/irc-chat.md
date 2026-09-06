<!-- SPDX-License-Identifier: GPL-2.0-only -->

# IRC Agent And Operator Chat

This document is the implementation contract for optional IRC networking and
terminal color. Every interactive launch keeps one coding agent/session. It
may host one local IRC room, connect to one or more snajpagent IRC servers, or
do both in the same foreground process. IRC is a first-class input, output,
tool, event-loop, durability, context, and rendering path inside snajpagent;
it is not a wrapper script, helper service, or terminal-only adapter.

## Command Line Contract

The network options are:

```text
-s, --listen[=ENDPOINT]      host the built-in IRC server
-c, --client[=ENDPOINT]      connect to a server; repeatable
    --no-listen             suppress configured hosting
    --no-client             suppress configured outgoing connections
-n, --model-nick NICK        preferred model IRC nick (default agent0)
-o, --operator-nick NICK     local operator IRC nick
-r, --room-name ROOM         hosted room name
    --color[=WHEN]           color: auto, always, or never
    --no-color               alias for --color=never
```

`-s` adds the IRC server to the normal foreground snajpagent process and
selects its endpoint. The server never forks, detaches, or hides the operator
UI. `-c` adds an outgoing server connection and may be given more than once.
Server and client roles are deliberately composable, including
`-s ENDPOINT -c ENDPOINT`. Incoming traffic is presented to the one local
agent and operator, but is not blindly bridged from one server to another.

The default endpoint is `localhost:6667`. A bare `-s`, `--listen`, `-c`, or
`--client` uses it. An explicit endpoint accepts `HOST`, `HOST:PORT`,
`[IPv6]`, or `[IPv6]:PORT`; an omitted port is 6667. Short attached arguments,
separate arguments, and long `=ENDPOINT` arguments are accepted. In networked
mode any initial chat text must follow `--`, which removes the only ambiguity
between an optional endpoint and positional text.

`-n`/`--model-nick` selects the preferred nick used for model-authored chat and
defaults to `agent0`. `-o`/`--operator-nick` controls the separate preferred nick
used for text typed in the local UI. Its default is the current valid login
identifier plus `0`, then `operator0`. The two preferred local nicks must differ
under IRC case folding.

`-r`/`--room-name` applies to current or future hosting. Nicks and room
preferences are valid without enabled networking. A leading `#`
is optional on input and is present on the wire and in the UI.

`-c`, `-r`, and `-o` replace older short aliases. Their displaced local
features, and dotdir selection, remain available without ambiguity through
long forms:

```text
--dotdir DIR
--config FILE
--resume [SESSION_ID|--last]
--effort LEVEL
```

`-C`, `-e`, `-l`, `-m`, `-v`, `-h`, and `-V` retain their meanings. Session
listing and one-shot execution do not run network roles. A durable session can
be resumed into networked mode with `--resume`. A session ID after `--resume`
is session identity rather than initial chat text even when network options
precede it; only a follow-up chat prompt requires `--`.

Examples:

```sh
snajpagent -s -n builder
snajpagent -s 0.0.0.0:6667 -n builder -o alice
snajpagent -c -n worker -o bob
snajpagent -c irc-a.example:6667 -c '[2001:db8::20]:6667' -n worker
snajpagent -s localhost:7667 -c upstream.example -n relay-worker
```

## Runtime Commands

`-s` and `-c` initialize capabilities, not permanent process identities. All
interactive sessions offer these commands during idle or active model/tool work:

- `/server` reports hosting; `/server start [ENDPOINT]` adds a listener.
- `/server stop` removes only hosting and accepted peers, leaving outgoing
  connections alive. A different listener requires this explicit stop first.
- `/connect [ENDPOINT]` adds an outgoing connection.
- `/disconnect [ENDPOINT]` removes one, or all outgoing connections if omitted;
  it never stops hosting.
- `/status` includes desired roles and actual connection health.

Omitted endpoints mean `localhost:6667`. Validation, lexical endpoint equality,
the 16-client bound and preferences are shared with startup. Duplicate additions
and absent removals are informative no-ops. Adding a listener fails visibly on
address collision without stopping the session or unrelated endpoints. Client
connection/join completion is asynchronous. These commands are process-local:
they never edit config, steer the model, cancel tools or silently change view.

## Configuration And Precedence

Every network command-line setting has a configuration equivalent:

```ini
[irc]
listen = localhost:6667
client = localhost:6667
client = irc.example:6667
model_nick = builder
operator_nick = alice
room_name = build-host
history_lines = 200

[ui]
color = auto
prompt = {activity_spinner}{goal_spinner} {hour:02}:{minute:02}:{second:02} {chat:{operator}@{host} :}{rollout-idle:{provider}/{model}/{effort} {context:3}% {queued:({queue}) }›}{rollout-active:{provider}/{model}/{effort} {context:3}% {queued:({queue}) }»}
prompt_spinner_goal = " ⚑"
prompt_spinner_provider = " ◴◷◶◵"
prompt_spinner_tool = " ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"
prompt_spinner_per_second = 8
```

`client` is the one intentionally repeatable configuration key. There may be
at most 16 distinct outgoing endpoints. Command-line `-c` occurrences replace
the configured client list and retain their order. `-s` overrides the
configured listener, while `listen` in configuration enables the built-in
server directly. Other command-line scalar values override configured values.
`--no-listen` and `--no-client` independently suppress those configured roles;
each conflicts with its positive CLI counterpart.

Runtime changes update the effective specification only. `/config` compares IRC
components with the last successfully loaded file and applies only deliberate
changes: an unchanged listener/client list cannot undo runtime commands. An
edited client list replaces the whole outgoing list, and removing `listen`
stops hosting. Startup CLI networking does not override these explicit edits.
Only affected owners change; invalid edits preserve live state, failed changes
attempt restoration, and a failed restoration reports actual remaining roles.

`history_lines` bounds both the server's in-memory room history and the fresh
history snapshot projected after compaction. It accepts 1 through 1000 and
defaults to 200. Each message carries at most 4,096 UTF-8 bytes, enough for at
least 1,024 four-byte Unicode code points. Longer text prefers a space boundary
without dropping bytes; an unbroken word is split only at a UTF-8 boundary.
The configured history count jointly bounds retained history memory. A full
1,000-message history fits the 8 MiB snapshot and 6 MiB output-queue bounds.
The separate topic limit remains 280 bytes.

Nicks are nonempty and at most 30 bytes, while room names are at most 50
bytes, valid UTF-8, free of spaces, commas, controls, and IRC separators. The
parser rejects duplicate endpoints, duplicate scalar keys, invalid ports,
invalid nicks and a client count over the bound.

## One Server, One Room

Each built-in server owns exactly one room for its lifetime. At startup it:

1. derives the default room from `gethostname(3)`, replacing bytes that IRC
   cannot carry in a room name with `_` and falling back to `#localhost`;
2. creates that room before accepting clients; and
3. sets its initial topic to the absolute working directory from which the
   server agent was launched.

The server advertises the canonical room in its registration reply. A
snajpagent client joins that advertised room automatically and treats failure
to join as a failed connection. Other IRC clients must issue `JOIN` for that
room. Attempts to create or join another room receive a normal IRC error and
do not create server state.

The local operator and ordinary human IRC clients enter the room with channel
operator mode `+o`. The agent identity does not. Channel operators may read
and change the topic with standard `TOPIC`; the resulting topic and actor are
broadcast and retained in history. The model is told that current `+o` status,
not a nick pattern or connection origin, marks operator input it must respect.

The default listener is local-only in practical effect through the
`localhost` default. The initial implementation deliberately provides neither
TLS nor authentication. Binding to a non-loopback address is therefore an
explicit trusted-network choice; deployments needing transport security use a
separate trusted tunnel or proxy.

## IRC Wire Behavior

The implementation is a bounded IRC server/client subset, not a second agent
RPC protocol. It uses CRLF framing, an 8,192-byte extended line maximum
including CRLF (advertised as `LINELEN=8192`), RFC-style
case folding, nonblocking sockets, and the normal commands needed by current
IRC clients:

- registration and liveness: `CAP`, `NICK`, `USER`, `PING`, `PONG`, `QUIT`;
- room participation: `JOIN`, `PART`, `NAMES`, `WHO`;
- chat: `PRIVMSG`, `NOTICE`;
- room state: `TOPIC`, `MODE` for membership/operator reporting; and
- standard welcome, room, `NAMES`, topic, and error numerics.

The snajpagent agent connection identifies its role during capability
negotiation and is not granted `+o`. Its UI operator connection is distinct
and is granted `+o` when it joins. A regular IRC client is an operator-facing
client. The implicit identities form zero-based sequences: the hosting/default
instance uses `agent0` and `LOGIN0`, and outgoing default clients replace that
terminal zero with `1`, `2`, and so on after pre-registration
`433 ERR_NICKNAMEINUSE` replies. Explicitly configured nicknames retain the
existing rule of appending `1`, `2`, and so on. Both paths truncate the
preferred portion at a UTF-8 boundary when the IRC nick bound requires it. The
accepted per-server nick remains stable across reconnects and governs echo
suppression and direct-mention recognition. The welcome confirms the accepted
nick; live `NICK` changes update it and preserve channel op status. The UI
reports channel renames once. A rejected rename leaves the existing identity
and connection intact. The
server itself follows IRC convention by rejecting collisions rather than
silently assigning an identity. Malformed lines, overlong lines, invalid UTF-8
chat, messages to another room, and commands used before registration or join
are rejected without disturbing other clients.

The server supports IRCv3 `batch`, `server-time`, and a bounded channel-history
capability. snajpagent peers also negotiate `snajpagent/catchup`: each retained
room event has a stable random stream ID and monotonically increasing sequence.
Live and replay delivery carry the same ID, timestamp, kind and role metadata.
The server restores its stream/sequence from the session log, and publishes
events to the wire only after durable engine acceptance, without blocking
socket servicing on that acceptance. A new server session uses a new stream.

On reconnect the client sends `SAJCATCHUP ROOM STREAM SEQUENCE` before JOIN,
using its last durably accepted position. Only newer retained events follow.
Duplicate IDs are dropped before new journal/cache/UI/model admission. Clients
reconstruct cursors and pending model delivery from their session; receipt is
not confused with successful response consumption. Interrupted catch-up retries
from accepted state without losing input that has not reached a model request.
If retention no longer covers the cursor, replay reports a gap and sends the
available newer events. A different stream receives a fresh bounded initial
history. No timestamp/text deduplication or pre-1.0 schema compatibility exists.

On join it sends the current topic, member nicks and modes,
followed by up to `history_lines` cached room events in one history batch. A
client without batch support receives the same bounded history as server
notices. The cache contains timestamped chat, joins, parts, quits, nick
changes, topic changes, and operator-mode changes. It is memory-bounded and
reconstructed from the resumed local session when that session contains
retained IRC events. On startup, a resumed server displays that same bounded
hosted-room history in its own chat view, without re-sending it or appending
duplicate durable events. Outgoing connections receive their server's history
through the ordinary join path.

Replayed chat uses the same `HH:MM:SS` sender/message prefix as live chat;
there is no per-message `history` suffix. Each completed received or hosted
history replay ends with one `── history replayed ──` mode-style banner.
This is display-only; durable historical flags and model-input routing remain.

Outgoing clients reconnect with bounded fixed backoff while the foreground
process remains alive. Disconnection is a visible room notification and does
not stop the local session, hosted server, or other connections. Registration,
join, and protocol failures remain visible diagnostics; no connection may
silently consume or invent operator input.

## Operator Chat And Rollout UI

Ordinary chat targets one selected destination. `/N` selects a number and
`/N TEXT` sends there once without changing selection. `/all TEXT` explicitly
broadcasts to a frozen recipient set. `/names` shows numbers, addresses, current
selection and membership. Numbers are process-local, stable across reconnects,
and never reused for a removed endpoint. One hosted room or outgoing endpoint
counts once, not once per participant or model/operator socket.

Single-destination use hides the extra labels but accepts the same commands.
Multiple destinations use a short prompt chip and source numbers in the one
transcript. Missing selected targets remain visibly unavailable instead of
redirecting drafts. Completion uses the selected/explicit destination's nicks.
Explicit sends may be invoked from rollout; plain rollout input stays private.

Every interactive session has append-only `chat` and `rollout` presentation
views, not a windowed full-screen TUI. Startup roles select chat; no roles
select rollout. Runtime role changes preserve view, draft, history and verbosity.
Offline chat remains readable; Enter reports no destinations and restores the
draft instead of starting a private model turn. Every
chat entry has a local `HH:MM:SS` display time, sender or event marker, and
readable IRC-client-style spacing; the composer remains at the bottom while
output is safely redrawn around it. Chat view shows:

- all timestamped room messages, including the local model's own sends, with
  the accepted sender nick, independent of verbosity;
- a visible `@` marker on nicks that currently carry `+o`;
- joins, leaves, reconnects, topic changes, and other room notifications; and
- the local operator composer and actionable errors.

Model text is buffered during generation and remains local rollout content.
Only a successful `irc_send` call sends model-authored text to IRC as a message
or notice from the model nick. Every participant sees that same message once
in chat at every verbosity level. Private model text and tool activity remain
in rollout. Final responses,
refusals, public text emitted on an intermediate tool cycle, raw tool calls,
tool results, provider traffic, request bodies, and internal agent activity
are never sent to the room implicitly.

Rollout view shows the local model's streamed work using the single shared
verbosity ladder. At verbosity 0 it
therefore shows local model text; increasing verbosity adds tools, reasoning
summaries, runtime state, protocol, and transport detail through the existing
single verbosity ladder. Actionable errors and direct local-command results
remain immediately visible in either view.

Both default composers share activity and goal slots followed by local
`HH:MM:SS`. The rollout composer then includes provider/model/effort, `N%`
space-padded to four columns, the optional queue count and `›` or `»`, using the
latest comparable durable token bound and resolved hard input budget. A fresh
session or accounting from a different provider source, selection, or
compaction lineage renders `?%`, as does compatible accounting with an unknown
hard budget. The fields supply plain numbers; the template owns `%` and the
conditional queue badge `{queued:({queue}) }`.
The default chat prompt follows the shared prefix with
`OPERATOR_NICK@MACHINE_HOSTNAME` without a meter, so switching presentation
views does not imply a token fact there. The timestamp is captured when the
prompt opens and is preserved across view, nickname, status, and editor redraws.
Submission or cancellation starts a new capture for the next prompt. Components
are independently formatted by `{hour:02}:{minute:02}:{second:02}`; see
`interactive-io.md` for numeric widths and explicit spinner optionality.

Successful durable lifecycle milestones are also first-class rollout records
at verbosity 0. They use exact terse bullet lines: `• Compacted` after
`compaction_completed`, `• Goal set` after `goal_started` or `goal_reworded`,
and `• Goal cleared` after `goal_completed` or `goal_cancelled`. No IDs, actors,
reasons, hashes, counts, or other detail appears at baseline verbosity. At
level 4, a separate live diagnostic may show its sequence and event type.
In chat view these records wait in the existing rollout queue and appear once,
in order, when rollout is entered; they never become IRC messages.

`/chat` and `/rollout` select a view from either an idle or active composer and
are not sent to IRC or admitted to the model. Selecting the current view is
idempotent. Empty Tab toggles the two views; a nonempty draft never switches
views and keeps ordinary completion, indentation, or active-turn queueing
semantics.

The selected view also selects the destination for ordinary composer input.
Chat input is sent from the local operator identity to the room and admitted
once to the model through the resulting local IRC event. Rollout input is
local-only: while idle it starts a local turn directly, and while a turn is
active it is added directly at the next safe boundary. Rollout input is never
sent as IRC `PRIVMSG`, and its submitted line remains visible exactly once
with the rollout prompt label that was visible when Enter was pressed.

Switching views never clears or repaints terminal history. It appends a short
view boundary, emits every semantic item accumulated for the entered view
since that view was last active in the current foreground run, in original
order and exactly once, then continues with live output. Each view has an
independent emitted cursor. A switch during a streamed item emits its complete
unseen prefix once and subsequent deltas continue without gaps, duplication,
or changes to stored/provider bytes. Catch-up starts with the current process;
entering a view does not dump older session history from the rollout log.
The boundary, catch-up, and destination prompt are one terminal-output
transaction: no old or new prompt may be redrawn between catch-up records or
appended to model text.

All modes use the same process-local ladder (flag count or `/verbose N`):

- verbosity 1: compact tool start/outcome rows, no output body;
- verbosity 2: 1,024 argument / 512 output character previews;
- verbosity 3: full retained arguments, execution context and results, preserving
  `[tool] max_output_bytes` and capture limits;
- verbosity 4: runtime, accounting, durable app events and IRC connection state;
- verbosity 5: sanitized prompt/protocol bodies and parsed IRC commands, with
  the existing sensitive-content warning; and
- verbosity 6: bounded provider and IRC wire-transport diagnostics.

Levels 4–6 are live-only in visible rollout, never accumulated behind chat.
Completed tool details refer to durable events and are formatted at the current
level when first visited. Raising the level does not replay visited records;
lowering it stops now-ineligible remaining detail. `/verbose` is UI-local and
remains usable during engine work; its reply never becomes a room message.
There is no config verbosity setting or per-mode level resolver.

Non-networked interactive and one-shot modes keep their current output model,
with the same color roles applied to prompts, labels, status, tools, warnings,
errors, and high-verbosity diagnostics.

The default chat-view composer is
`   HH:MM:SS OPERATOR_NICK@MACHINE_HOSTNAME : `, with one literal space
between the indicator slots and clock. The default rollout view uses
`    0% PROVIDER/MODEL/EFFORT › ` while idle and the double-angle `»` while
active. Both start with a goal flag slot and one shared activity slot;
tool work takes priority over model work in the same column. Reserved slots
and unused digits in the four-column context field remain spaces. The
prompt and local chat use the accepted operator nick on the first configured
server, or the hosted nick when serving a room. Nick changes refresh the prompt
without losing the draft; configured nicks remain registration preferences.
The hostname comes from the local machine, not the IRC endpoint, room, or
remote server. Both views use the one shared dotdir prompt history and Ctrl-R
search.

Text entered in chat view is sent as a room message from the local operator
identity and also admitted once as local operator input to the model; echoes
returning from several attached servers are deduplicated locally. Text entered
in rollout view stays local and is never transmitted to the room.
Useful local slash commands include `/chat`, `/rollout`, `/topic [TEXT]`,
`/names`, the existing session/model/goal/queue commands, and `/exit`. `//TEXT`
sends input whose first byte is `/` to the destination selected by the current
view.

## Color Contract

Color is a presentation property for the whole program. It never enters IRC
frames, durable events, provider input, redirected output, or stored model
text.

- `auto` is the default and emits color only to a terminal; it also respects
  `NO_COLOR`.
- `always` emits color on terminal-facing output even when terminal detection
  is unavailable and is selected by bare `--color`.
- `never` emits no SGR sequences and is selected by `--no-color`.

The palette uses only broadly supported 16-color ANSI foreground attributes,
no background fills and no true-color assumptions. Nicks and symbols carry
the meaning even without color. Model nicks, operator nicks, local prompts,
room events, tool activity, success, warning, error, durable events, protocol,
and transport diagnostics have stable roles. Red is reserved for errors,
yellow for warnings/activity, blue for chat agents, cyan for chat operators
and local prompts, magenta for mention headers, and dim/default text for metadata. Every viewer
uses the same sender-role colors, whether hosting or connected as a client.
Locality, nick changes and history replay do not change a sender's palette.
Attributes are always reset at field boundaries so user/model text cannot
inherit them.
Messages and notices mentioning the local operator's or model's currently
accepted nick for their room highlight only the timestamp and sender nick in
bold magenta, including the `›` (or notice `-`) separator. Ordinary chat agent
nicks use blue and operator nicks cyan; neither duplicates the mention palette.
The destination label and entire message body keep
their normal appearance, including plain text, wrapped lines and rendered
Markdown (bold, emphasis, headings, quotes, code, links and table headers).
Matching uses IRC case folding and nick boundaries,
not substrings or another server's aliases. The same display rule applies to
retained chat; it does not change model steering or promote history or operator
mentions into model input. Color disabled means no highlight escapes.
Terse lifecycle milestone lines have their own bold-green role spanning the
bullet and text. Durable identities are separate live debug records at level 4.
Chat timestamps and sender labels already frame messages, so Markdown rendering
does not add a synthetic bullet to ordinary agent prose there. An actual
Markdown list item still renders with its structural bullet.
On a compact tool-start line, color covers only the arrow and tool label; an
`exec` command payload uses the default foreground, matching the uncolored
workdir, arguments, and captured command-output lines that follow it.

## Model Input And Steering

When any network role is enabled, the fixed developer harness sent before the
conversation explains:

- that this process participates in one or more views of a single IRC room;
- the preferred model and operator identities, with room snapshots identifying
  accepted per-server aliases after collisions;
- that chat entries marked `+o` are operator instructions;
- that a direct mention of the agent requires immediate attention;
- that unmentioned chat, including local/channel operator messages, and
  membership notifications are conversational context;
- that assistant speech remains in the local rollout and `irc_send` is the
  only way for the model to address the room; and
- that the declared coding tools still operate on the local workspace, not on
  the IRC server or remote peers;
- that IRC connection health, joining, history synchronization, and reconnect
  are owned by the runtime and must not be polled or babysat by the model; and
- that a quiet response to ordinary chatter is valid, while a local operator
  mention turn requires one room-facing reply.

IRC input is recorded as typed structured session events before it affects a
provider request. Each projected user message renders source endpoint, room,
timestamp, event type, sender, and the sender's operator flag. Nick text alone
never upgrades authority.

Admission priority is:

1. a room message that mentions the accepted endpoint-local model nick, using
   IRC case folding and a nick boundary, is urgent regardless of the sender.
   Local chat carries its actual destination and uses that endpoint's alias; and
2. other chat (including local operator and `+o` messages) and room notifications
   are coalesced into a bounded user-role room update when active work finishes.

Background traffic is drained before an automatic goal continuation. While
idle it is briefly coalesced so ordinary chat does not create one provider
request per IRC line. During a response or tool call, the existing active
input pump services sockets as well as terminal input. Urgent entries do not
truncate an in-flight model stream or cancel a running tool: they accumulate
in complete message batches and are admitted through the steering
path at the earliest safe response/tool boundary. Messages arriving before
that boundary are coalesced in arrival order, so several mentions cause one
additional model cycle rather than a cancellation/restart storm. Background
entries wait for the next response cycle or queued room-update turn. No
network read waits for a model call to finish. A room-update turn caused only
by peers or notifications may end without model-authored chat; snajpagent does
not remind, retry, or force a reaction to that traffic.

A provider response with no actionable item expresses that quiet outcome. An
explicit empty or oversized assistant message is not silence: snajpagent tells
the model which condition occurred in one terse developer correction and lets
the next cycle recover. The correction stays out of the normal operator UI and
appears only in higher-verbosity durable diagnostics.

Every command uses the asynchronous-capable managed path. A configured or
model-requested timeout is a foreground handoff deadline, not a kill deadline:
if it expires while the command is still live, the same handoff used for local
steering and urgent IRC mentions returns a live handle without signaling the
process. The next model cycle receives the timeout notice or coalesced urgent
IRC batch and the exact `write_stdin` surface for that handle. The model can
react immediately, wait for command completion, continue the command, or use
the handle-bound explicit termination option. A steering-triggered result is
distinguished as `reason=steering_handoff`; neither handoff itself signals the
process. The IRC runtime does not require the model to babysit the socket or
process. Ordinary terminal interrupt remains an explicit cancellation and is
not changed by this rule.

For a turn requested by a local operator mention, snajpagent tracks whether a
successful `irc_send` message was posted to the room. A notice does not count
as a reply. If the model reaches an otherwise terminal boundary without such a
reply, the same turn receives one concise developer reminder to use `irc_send`
and gets one final provider cycle. The turn is then considered finished,
whether the model sends or remains quiet. This reminder is never looped and is
never applied to peer messages, membership traffic, history snapshots, or
other background updates.

Pending accepted input belongs to the session, not its current sockets. Both
priority classes retain complete projections within an 8 MiB bound, enough for
the supported 1,000-message retained history. Whole-message batches respect the
256 KiB steering/input limit without splitting Unicode or multiline records.
Exhaustion is an explicit error, never silent eviction. Final disconnect cannot
strand pending input. Topology and nick snapshots contain current state only,
so runtime changes cannot sneak background history into an urgent request.
Removing a destination clears its outstanding local-operator reply obligation.

On successful room join/reconnect, the topic and member nicks are admitted as
a state-only snapshot. Identified live/replayed event payloads are projected
once from their durable records; scheduling references do not copy their text.
Ordinary live background chat still follows the existing queue/turn boundaries;
durable `irc_admitted` sequence references record when accepted payloads become
model context, without storing another copy of their text. Mention and
historical catch-up timing is unchanged. The received-event watermark of a
frozen request cannot cross withheld background input, and only successful
completion advances consumption. Resume schedules durable
input still awaiting consumption, even if its socket or original batch is gone.
If a
snapshot would make the active context cross its normal compaction threshold,
normal compaction runs first and the fresh bounded snapshot is admitted
afterward.

Every successful manual, automatic, native, or Responses-based compaction is
followed before the next provider response by a fresh user-role snapshot of
the current topic, membership/operator state, and up to `history_lines` recent
events from the local IRC cache. This keeps live room state outside the text
that compaction may summarize. Snapshot insertion is itself a durable event,
so restart and replay cannot change which network context the model saw.

## Model IRC Tools

Requests constructed with network roles expose native bounded IRC tools alongside the existing
coding tools:

- `irc_send` sends a room message or notice as the agent identity;
- `irc_state` returns the current endpoints, joins, room, topic, accepted local
  aliases, member nicks, and operator flags from already-maintained runtime
  state; and
- `irc_topic` requests a topic change as the agent identity and succeeds only
  where that identity currently has the required channel mode.

Sends and topic tools require nullable string `destination`: a numbered target
from maintained state, `all` for an explicit broadcast, or null when the frozen
request has exactly one destination. Operator UI selection never redirects a
model reply. Each target produces its own attributed local echo and queue/failure
result. Queue acceptance is not proof of remote delivery. Local-mention reply
obligations belong to their originating targets, not an unrelated successful send.

Tool calls are durable and use the same start/result ordering and secret-safe
rendering as other tools. One `-v` shows compact calls without output; `-vv`
previews 1,024 argument and 512 output characters, and `-vvv` shows full retained
calls/results. Debug detail starts at four flags. `[tool] max_output_bytes` bounds only terminal
presentation; its default `0` is unlimited, and the complete redacted output
is always persisted. Command output supplied to the model is separately
bounded by the calling command tool's `max_output_tokens`. IRC tools never open sockets,
join, poll, reconnect, wait for traffic, or expose a manual reconnect action.
The event loop owns those operations continuously. `irc_send` is the exclusive
room-speech path and may be used any number of times during a turn. Assistant
response text remains local even at a terminal response boundary.

The request freezes its tool contract and per-destination identities/revisions when constructed,
including before token counting. Later topology changes do not rewrite or
cancel it. Stale `irc_send`/`irc_topic` actions return individual not-performed
results instead of migrating to new rooms; read-only `irc_state` returns live or
offline state. Accompanying `write_stdin` calls retain their original valid
classification and live handles. Real destination changes (also remove/re-add
and a different advertised room) advance the revision; no-ops and same-room
reconnects do not. A topology change alone does not force an extra model turn.
Unrelated additions do not invalidate a send to an unchanged explicit target.
The IRC owner also checks the room revision before execution, covering changes
not yet admitted by the engine. Reconnect to a different advertised room discards
the old queued messages rather than sending them to the replacement room.

## Durability, Bounds, And Failure Semantics

Network configuration is process configuration; IRC room traffic that reaches
the agent is session state. Typed events cover connections, joins/leaves,
messages, nick/mode/topic changes, outbound model/operator chat, delivered
history, and projected snapshots. Replay validates sequence, UTF-8, line and
history bounds, source identity, and operator-state transitions before those
events can become model input.

On every resumable exit, the process prints a POSIX-shell-quoted command whose
effective network arguments reconstruct that process configuration. A
client-only command carries every outgoing endpoint; a server command carries
its listener, room, and local nicks; a combined command carries both sets. It
also reuses the same dotdir and explicit config path and resumes
the exact durable session. The output contains no credentials or chat text.
Absent roles are explicit `--no-listen`/`--no-client`, preventing reused config
defaults from resurrecting them. Desired clients remain present during temporary
outages. Cleanup does not mutate the desired role specification.
SIGHUP and SIGTERM unwind through this path; SIGKILL and machine loss cannot.

The server stores no provider credential and never transports tool arguments
or results. Existing secret redaction remains in force at higher verbosity.
IRC writes use bounded per-connection queues so one slow peer cannot block the
model, UI, or other peers; a peer that exceeds its queue bound is disconnected
with a visible reason. The application handles `SIGPIPE` safely and treats a
socket loss as a connection event rather than a process crash.

One thread owns the hosted server and all its accepted peers. Each outgoing
endpoint has its own thread owning the paired model/operator connections.
Each owner has a stable allocation, even as ordered primary selection changes:
hosting first, otherwise the first requested outgoing endpoint. Removing an
owner stops it and drains accepted records before freeing it; other owners
continue. Unsent transport bytes are discarded with an explicit local notice,
never moved to a new destination. A new host restores only public history for
its matching endpoint/room, not private rollout or another room's records.
They run private copies of the same protocol implementation; no application
or session callback executes there. Bounded owned events and room/alias
snapshots reach the engine in order, and only the engine persists, displays or
admits them. Commands acknowledge their result after all their earlier records
have been admitted. Saturation backpressures only the producing owner; joined
shutdown wakes blocked publishers and socket waits. See `interactive-io.md`.

Failure to start the explicitly requested local listener is a startup error.
An address-in-use collision stops startup with the endpoint and reason visible
at verbosity zero. It does not fall through to another resolved address or
increment the port. Other failures may try another address, allowing operation
when an address family is unavailable.
Failure of one outgoing connection is recoverable and visible. If every
network link is down, the local UI and agent session remain usable and queued
outbound chat is bounded rather than allowed to grow without limit.

## Compatibility And Non-Goals

- The implementation does not support multiple hosted rooms, room discovery,
  direct messages as model control, file transfer, IRC services, TLS, accounts,
  persistence independent of the snajpagent session, or server-to-server IRC.
- Combined server/client mode does not relay third-party messages between
  servers. It gives one local agent and operator access to several endpoints.
- IRC formatting/control codes from peers are stripped or rendered harmless;
  terminal escape bytes are never emitted as control sequences.
- Network mode does not change tool authority, workspace selection, provider
  credentials, goal lifecycle, queue ordering, or the local-only nature of
  command execution.
- Network support is integrated into the existing process lifecycle, active
  input pump, response/tool cycle, durable session log, context builder,
  compaction hooks and renderer. A parallel IPC/control layer is not an
  acceptable implementation.

## Source-Size Discipline

This feature remains subject to the repository's existing `make sizecheck`
limits: production C has a 32,768-line soft limit and 49,152-line hard limit;
production headers have 16,384-line soft and 65,536-line hard limits; test C
has 16,384-line soft and 32,768-line hard limits. The 2,000-line-file level
remains a review trigger. Implementation should reuse the session, event-loop,
tool, rendering, and managed-process abstractions, consolidate a genuinely
trivial unit where needed, and omit duplication. Every retained line must serve
a contract item, failure bound, portability requirement, or focused validation;
source-budget pressure must not remove required IRC, color, durability,
reconnect, steering, or UI behavior.

## Acceptance

Implementation is complete when the existing build and validation commands
pass and focused local smoke checks demonstrate all of the following:

1. the new short/long forms, compatibility long forms, config equivalents,
   default model and operator nicks, repeated clients, combined roles, and invalid
   combinations;
2. localhost server startup on port 6667, one advertised/default room, default
   path topic, automatic client joins, operator `+o`, agent non-op membership,
   topic changes, ordinary chat, and bounded history delivery;
3. distinct operator and agent transcript lines, identical public chat-message
   visibility at all verbosity levels, private model/tool output kept in
   rollout, rollout view showing ordinary local
   model output, useful agent/tool detail before lower-priority IRC debugging,
   safe terminal rendering, and `auto`/`always`/`never` color behavior in
   networked and non-networked modes;
4. `/chat`, `/rollout`, and empty-Tab switching in idle and active composers,
   exact U+203A/U+00BB prompts, and ordered exact-once catch-up for both views,
   including a switch during streamed output; atomic catch-up without an
   interposed prompt; visible exact-once local rollout submissions; and absence
   of rollout input from IRC wire output;
5. earliest-safe, coalesced urgent admission for model-nick mentions;
   ordinary local/channel-operator messages stay background without interrupting active
   generation or tools; asynchronous managed-command handoff; coalesced
   background events; socket servicing during provider and tool work;
   first-join history projection; and fresh post-compaction history projection;
   one non-looping `irc_send` reminder for otherwise-silent local operator
   mention turns; and no forced response to other traffic;
6. `irc_send` as the only model-authored IRC transmission path, final assistant
   text remaining local, and `irc_state` plus privilege-correct `irc_topic`
   behavior without model-driven polling, joining, or reconnection; and
7. autonomous reconnect/disconnect behavior, slow/malformed peer isolation, bounded
   buffers, durable replay/resume, clean shutdown, and no credential, tool
   detail, escape-sequence, or cross-server leakage.

Use the repository's existing `make`, `make check`, and `make sanitizercheck`
flows. The permanent tmux check runs the production binary as one server and
two clients against loopback fake Responses endpoints, exercising both client
message directions, all three model paths, durable room attribution, and the
rendered verbosity/color UI. It exits the clients in sequence, admits their
durable leave notifications, then exits the server cleanly. Do not introduce a
separate validation or evidence framework for this feature.
