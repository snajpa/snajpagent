<!-- SPDX-License-Identifier: GPL-2.0-only -->

# IRC Agent And Operator Chat

This document is the implementation contract for snajpagent's networked IRC
mode and for terminal color in every mode. Networked mode keeps one coding
agent/session and presents its terminal as an operator-facing chat client. It
may host one local IRC room, connect to one or more snajpagent IRC servers, or
do both in the same foreground process. IRC is a first-class input, output,
tool, event-loop, durability, context, and rendering path inside snajpagent;
it is not a wrapper script, helper daemon, or terminal-only adapter.

## Command Line Contract

The network options are:

```text
-d, --daemon                 host the built-in IRC server
-s, --listen[=ENDPOINT]      server listen endpoint
-c, --client[=ENDPOINT]      connect to a server; repeatable
-n, --name NAME              agent IRC name
-o, --operator-name NAME     local operator IRC name
-r, --room-name ROOM         hosted room name
    --color[=WHEN]           color: auto, always, or never
    --no-color               alias for --color=never
```

`-d` adds the IRC server to the normal foreground snajpagent process; it does
not fork, detach, or hide the operator UI. `-s` selects the endpoint used by
that server and requires `-d`. `-c` adds an outgoing server connection and may
be given more than once. Server and client roles are deliberately composable,
including `-d -s ENDPOINT -c ENDPOINT`. Incoming traffic is presented to the
one local agent and operator, but is not blindly bridged from one server to
another.

The default endpoint is `localhost:6667`. A bare `-s`, `--listen`, `-c`, or
`--client` uses it. An explicit endpoint accepts `HOST`, `HOST:PORT`,
`[IPv6]`, or `[IPv6]:PORT`; an omitted port is 6667. Short attached arguments,
separate arguments, and long `=ENDPOINT` arguments are accepted. In networked
mode any initial chat text must follow `--`, which removes the only ambiguity
between an optional endpoint and positional text.

`-n`/`--name` is required whenever a server or client role is enabled. It is
the identity used for model-authored chat. `-o`/`--operator-name` controls the
separate identity used for text typed in the local UI. Its default is the
current login name when that is a valid IRC name, then `operator`. The two
local names must differ under IRC case folding.

`-r`/`--room-name` applies to the hosted room and requires `-d`. A leading `#`
is optional on input and is present on the wire and in the UI.

The requested short options replace four older short aliases. The displaced
local features remain available, without ambiguity, through their existing
long forms:

```text
--dotdir DIR
--config FILE
--resume [SESSION_ID|--last]
--effort LEVEL
```

`-C`, `-e`, `-l`, `-m`, `-v`, `-h`, and `-V` retain their meanings. Session
listing and one-shot execution do not run network roles. A durable session can
be resumed into networked mode with `--resume`.

Examples:

```sh
snajpagent -d -n builder
snajpagent -d -s 0.0.0.0:6667 -n builder -o alice
snajpagent -c -n worker -o bob
snajpagent -c irc-a.example:6667 -c '[2001:db8::20]:6667' -n worker
snajpagent -d -s localhost:7667 -c upstream.example -n relay-worker
```

## Configuration And Precedence

Every network command-line setting has a configuration equivalent:

```ini
[irc]
daemon = true
listen = localhost:6667
client = localhost:6667
client = irc.example:6667
name = builder
operator_name = alice
room_name = build-host
history_lines = 200

[ui]
color = auto
```

`client` is the one intentionally repeatable configuration key. There may be
at most 16 distinct outgoing endpoints. Any command-line `-c` occurrences
replace the configured client list; repeated command-line occurrences retain
their order. Command-line scalar values override configured values. `-d`
enables a configured-off daemon, while `listen`, `name`, `operator_name`, and
`room_name` otherwise supply their documented defaults.

`history_lines` bounds both the server's in-memory room history and the fresh
history snapshot projected after compaction. It accepts 1 through 1000 and
defaults to 200. IRC's 512-byte wire-line limit and the configured line count
jointly bound retained history memory.

Names are nonempty IRC nicks of at most 30 bytes and room names are at most 50
bytes, valid UTF-8, free of spaces, commas, controls, and IRC separators. The
parser rejects duplicate endpoints, duplicate scalar keys, invalid ports,
invalid names, a client count over the bound, and options whose required mode
is absent.

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
not a name pattern or connection origin, marks operator input it must respect.

The default listener is local-only in practical effect through the
`localhost` default. The initial implementation deliberately provides neither
TLS nor authentication. Binding to a non-loopback address is therefore an
explicit trusted-network choice; deployments needing transport security use a
separate trusted tunnel or proxy.

## IRC Wire Behavior

The implementation is a bounded IRC server/client subset, not a second agent
RPC protocol. It uses CRLF framing, the 512-byte IRC line maximum, RFC-style
case folding, nonblocking sockets, and the normal commands needed by current
IRC clients:

- registration and liveness: `CAP`, `NICK`, `USER`, `PING`, `PONG`, `QUIT`;
- room participation: `JOIN`, `PART`, `NAMES`, `WHO`;
- chat: `PRIVMSG`, `NOTICE`;
- room state: `TOPIC`, `MODE` for membership/operator reporting; and
- standard welcome, room, names, topic, and error numerics.

The snajpagent agent connection identifies its role during capability
negotiation and is not granted `+o`. Its UI operator connection is distinct
and is granted `+o` when it joins. A regular IRC client is an operator-facing
client. Nick collisions, malformed lines, overlong lines, invalid UTF-8 chat,
messages to another room, and commands used before registration or join are
rejected without disturbing other clients.

The server supports IRCv3 `batch`, `server-time`, and a bounded channel-history
capability. On join it sends the current topic, names and modes, followed by up
to `history_lines` cached room events in one history batch. A client without
batch support receives the same bounded history as server notices. The cache
contains timestamped chat, joins, parts, quits, nick changes, topic changes,
and operator-mode changes. It is memory-bounded and reconstructed from the
resumed local session when that session contains retained IRC events.

Outgoing clients reconnect with bounded fixed backoff while the foreground
process remains alive. Disconnection is a visible room notification and does
not stop the local session, hosted server, or other connections. Registration,
join, and protocol failures remain visible diagnostics; no connection may
silently consume or invent operator input.

## Operator Chat UI

Networked interactive mode is a chat transcript, not a raw model trace or a
windowed full-screen TUI. Every transcript entry has a local display time,
sender or event marker, and readable IRC-client-style spacing; the composer
remains at the bottom while output is safely redrawn around it. At verbosity 0
it shows only:

- timestamped operator and remote-room messages with the sender name;
- a visible `@` marker on names that currently carry `+o`;
- joins, leaves, reconnects, topic changes, and other room notifications; and
- the local operator composer and actionable errors.

Model text is buffered during generation. Only terminal public assistant text
is sent to IRC as a message from the agent name. This process's own model text
is not rendered in its local verbosity-0 UI; it becomes locally visible only
after verbosity is raised. Public text emitted on an intermediate tool cycle,
raw tool calls, tool results, provider traffic, request bodies, and internal
agent activity are not written into the verbosity-0 transcript. Intermediate
and diagnostic material is never sent to the room. This preserves a usable
operator conversation while keeping implementation details private.

Networked mode deliberately spends its first added verbosity step on the
hidden local reply and keeps IRC debugging behind the more useful agent/tool
detail. Its local-only ladder is:

- verbosity 1: terminal model replies shown once as agent chat lines;
- verbosity 2: compact agent/tool activity names and completion state;
- verbosity 3: bounded tool arguments/results and runtime/provider-cycle state;
- verbosity 4: durable app events and compact IRC connection/event state;
- verbosity 5: sanitized prompt/protocol bodies and parsed IRC commands, with
  the existing sensitive-content warning; and
- verbosity 6: bounded provider and IRC wire-transport diagnostics.

Non-networked interactive and one-shot modes keep their current output model,
with the same color roles applied to prompts, labels, status, tools, warnings,
errors, and high-verbosity diagnostics.

Text entered at the network composer is sent as a room message from the local
operator identity and also admitted once as local operator input to the model;
echoes returning from several attached servers are deduplicated locally.
Useful local slash commands include `/topic [TEXT]`, `/names`, the existing
session/model/goal/queue commands, and `/exit`. `//TEXT` sends a chat message
whose first byte is `/`.

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
no background fills and no true-color assumptions. Names and symbols carry
the meaning even without color. Agent names, operator names, local prompts,
room events, tool activity, success, warning, error, durable events, protocol,
and transport diagnostics have stable roles. Red is reserved for errors,
yellow for warnings/activity, cyan/blue for agent and prompt identity,
magenta for operator identity, and dim/default text for metadata. Attributes
are always reset at field boundaries so user/model text cannot inherit them.

## Model Input And Steering

When any network role is enabled, the fixed developer harness sent before the
conversation explains:

- that this process participates in one or more views of a single IRC room;
- the separate agent and operator identities and the current agent name;
- that chat entries marked `+o` are operator instructions;
- that a direct mention of the agent requires immediate attention;
- that unprivileged agent/room traffic and membership notifications are
  conversational context;
- that terminal assistant speech becomes an IRC message while tool activity is
  local-only at normal verbosity; and
- that the declared coding tools still operate on the local workspace, not on
  the IRC server or remote peers;
- that IRC connection health, joining, history synchronization, and reconnect
  are owned by the runtime and must not be polled or babysat by the model; and
- that a quiet response to peer chatter is valid, while a local operator turn
  requires one room-facing reply.

IRC input is recorded as typed structured session events before it affects a
provider request. Each projected user message renders source endpoint, room,
timestamp, event type, sender, and the sender's operator flag. Nick text alone
never upgrades authority.

Admission priority is:

1. local operator text is urgent and starts an immediate user turn while idle;
2. a room message from a member currently carrying `+o` is urgent in the same
   way;
3. a room message that mentions the agent name, using IRC case folding and a
   nick boundary, is urgent regardless of the sender; and
4. other chat and room notifications are coalesced into a bounded user-role
   room update at a convenient provider boundary.

Background traffic is drained before an automatic goal continuation. While
idle it is briefly coalesced so ordinary chat does not create one provider
request per IRC line. During a response or tool call, the existing active
input pump services sockets as well as terminal input. Urgent entries do not
truncate an in-flight model stream or cancel a running tool: they accumulate
in one bounded, durable urgent batch and are admitted through the steering
path at the earliest safe response/tool boundary. Messages arriving before
that boundary are coalesced in arrival order, so several mentions cause one
additional model cycle rather than a cancellation/restart storm. Background
entries wait for the next response cycle or queued room-update turn. No
network read waits for a model call to finish. A room-update turn caused only
by peers or notifications may end without model-authored chat; snajpagent does
not remind, retry, or force a reaction to that traffic.

Managed commands retain the existing asynchronous handoff model. A command may
yield a live handle, after which the next model cycle receives the coalesced
urgent IRC batch and the exact `write_stdin` surface for that handle. The model
can react to chat immediately, wait for command completion, or continue the
command; neither the IRC runtime nor the model babysits the socket or process.
An urgent message received while a synchronous command is executing is
admitted as soon as that command returns. Ordinary terminal interrupt remains
an explicit cancellation and is not changed by this rule.

For a turn started by the local operator, snajpagent tracks whether terminal
public assistant text was posted to the room. If the model reaches an otherwise
terminal boundary without such a reply, the same turn receives one concise
developer reminder to reply to the local operator in IRC and gets one final
provider cycle. The turn is then considered finished, whether the model speaks
or remains quiet. This reminder is never looped and is never applied to peer
messages, membership traffic, history snapshots, or other background updates.

On the first successful room join, the received topic, names and server
history are immediately admitted together as a user-role room snapshot. If a
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

Networked requests expose native bounded IRC tools alongside the existing
coding tools:

- `irc_send` sends a room message or notice as the agent identity;
- `irc_state` returns the current endpoints, joins, room, topic, names and
  operator flags from already-maintained runtime state; and
- `irc_topic` requests a topic change as the agent identity and succeeds only
  where that identity currently has the required channel mode.

Tool calls are durable and use the same start/result ordering and secret-safe
rendering as other tools. They never open sockets, join, poll, reconnect, wait
for traffic, or expose a manual reconnect action. The event loop owns those
operations continuously. Ordinary terminal assistant text remains the simple
way to reply; `irc_send` exists for an explicit mid-turn chat action and is not
required as a completion ritual.

## Durability, Bounds, And Failure Semantics

Network configuration is process configuration; IRC room traffic that reaches
the agent is session state. Typed events cover connections, joins/leaves,
messages, nick/mode/topic changes, outbound model/operator chat, delivered
history, and projected snapshots. Replay validates sequence, UTF-8, line and
history bounds, source identity, and operator-state transitions before those
events can become model input.

The server stores no provider credential and never transports tool arguments
or results. Existing secret redaction remains in force at higher verbosity.
IRC writes use bounded per-connection queues so one slow peer cannot block the
model, UI, or other peers; a peer that exceeds its queue bound is disconnected
with a visible reason. The application handles `SIGPIPE` safely and treats a
socket loss as a connection event rather than a process crash.

Failure to start the explicitly requested local listener is a startup error.
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
limits: no more than 35,000 production C/header lines, 50,000 shipped C/header
lines, and 30 production translation units. The 30,000-production-line level
and 2,000-line-file level remain review triggers rather than excuses to raise a
limit. Implementation should reuse the session, event-loop, tool, rendering,
and managed-process abstractions, consolidate a genuinely trivial unit where
needed, and omit duplication. Every retained line must serve a contract item,
failure bound, portability requirement, or focused validation; source-budget
pressure must not remove required IRC, color, durability, reconnect, steering,
or UI behavior.

## Acceptance

Implementation is complete when the existing build and validation commands
pass and focused local smoke checks demonstrate all of the following:

1. the new short/long forms, compatibility long forms, config equivalents,
   defaults, required name, repeated clients, combined roles, and invalid
   combinations;
2. localhost server startup on port 6667, one advertised/default room, default
   path topic, automatic client joins, operator `+o`, agent non-op membership,
   topic changes, ordinary chat, and bounded history delivery;
3. distinct operator and agent transcript lines, no local model or raw tool
   trace at verbosity 0, local model display beginning at verbosity 1, useful
   agent/tool detail before lower-priority IRC debugging, safe terminal
   rendering, and `auto`/`always`/`never` color behavior in networked and
   non-networked modes;
4. earliest-safe, coalesced urgent admission for local operator, current
   channel operator, and agent mentions without truncating active generation or
   tools; asynchronous managed-command handoff; coalesced background events;
   socket servicing during provider and tool work; first-join history
   projection; and fresh post-compaction history projection; one non-looping
   reply reminder for otherwise-silent local operator turns; and no forced
   response to other traffic;
5. `irc_send`, `irc_state`, and privilege-correct `irc_topic` behavior without
   model-driven polling, joining, or reconnection; and
6. autonomous reconnect/disconnect behavior, slow/malformed peer isolation, bounded
   buffers, durable replay/resume, clean shutdown, and no credential, tool
   detail, escape-sequence, or cross-server leakage.

Use the repository's existing `make`, `make check`, and `make sanitizercheck`
flows plus narrow local multi-process smoke runs. Do not introduce a separate
validation or evidence framework for this feature.
