<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent

snajpagent is a terminal coding agent. It talks to an OpenAI-compatible
Responses API endpoint, keeps durable local sessions, can resume previous
conversations, and exposes a small set of tools for command execution, process
continuation, file patching, and hosted web search.

The project is written in C and is still pre-1.0, but the normal interactive
path is usable.

## Features

- Persistent multi-turn sessions stored as append-only local event logs.
- Persistent model discovery across multiple ordered provider configurations.
- Persistent `/goal` objectives with automatic multi-turn continuation,
  pause/resume, user-controlled wording locks, and explicit completion or
  blocking from the model.
- Durable `--resume` support without a background worker or tmux session.
- A copy/paste-safe two-line `resume:` block on every resumable session exit,
  including commands that restore outgoing IRC clients or the built-in server.
- First-class single-room IRC hosting and repeatable outgoing connections for
  agent/operator chat, with reconnects, bounded history, and operator-aware
  model steering.
- Interactive mode, one-shot execution mode, and session listing.
- Terminal-width wrapping for streamed model text without changing stored or
  redirected response bytes.
- Immediate Enter steering with byte-exact interrupted-prefix replay and an
  immediately reusable active composer; Tab remains future-turn FIFO queueing.
- Default-on terminal Markdown presentation for streamed model text, resumed
  assistant history, and non-operator model messages in the IRC transcript.
- OpenAI Responses streaming over libcurl, including hosted `web_search`.
- Codex-style instruction discovery from `AGENTS.override.md` and `AGENTS.md`.
- Tool support for `exec_command`, yielded process handles, `write_stdin`
  waiting/interaction/termination, and strict `apply_patch`, plus IRC
  chat/state/topic tools in networked mode.
- Program-wide, terminal-aware 16-color presentation with an explicit
  `auto`/`always`/`never` policy.
- Secret redaction for provider credentials and configured tool environment
  variables before tool output is stored or rendered.

## Build

Requirements:

- C11 compiler
- POSIX.1-2008 environment
- make
- system libcurl
- system Jansson
- tmux (optional for `make check`, required by the strict real-screen targets)

Build and test:

```sh
make
make check
```

The top-level `META` is the sole source for the compiled product name and base
version. A clean build at the matching version tag reports that version
directly; other builds append Git's abbreviated `HEAD`, for example
`0.98-abcdef`, and dirty builds say so. Version changes and matching tags are
operator decisions. Until the operator releases `1.0`, unreleased formats may
change in place without compatibility ladders or legacy fallbacks.

When tmux is installed, `make check` also runs `make tmuxcheck`. That target
starts the fixture binary in isolated 32–48 column tmux panes and validates the
rendered screen and scrollback, not merely the PTY byte stream. It covers
streaming/status interaction, word and hard wrapping, UTF-8/control safety,
steering pause/resume snapshots, resize at the right margin, every queue
mutation, durable response/queue text, and enabled/disabled `AGENTS.md`
admission. It then runs three production binaries against loopback fake
Responses endpoints—one IRC server and two clients—and checks bidirectional
chat, all three model replies, operator/topic/membership durability, verbosity,
color, rendered timestamps, peer-leave handling, and clean shutdown. Call
`make tmuxcheck` directly when tmux coverage is mandatory.

The explicit live counterpart uses the configured default provider, the fixed
vpsAdminOS 6.12.95 status prompt documented in `design/interactive-io.md`, and
retains its screen plus event copy under a caller-selected new directory:

```sh
make terminallivecheck \
  LIVE_WORKSPACE=/root \
  LIVE_CONFIG=$HOME/.snajpagent/config.ini \
  LIVE_RESULT_ROOT=/path/to/new/result-directory
```

Live terminal checks take a nonblocking exclusive advisory lock on
`LIVE_CONFIG`, so two instances using the same configured profile cannot run at
once. The result directory must not already exist.

The repository vendors no third-party implementation source. See
`DEPENDENCIES.md` for the dependency policy and the available audit targets.

## Run

```sh
./snajpagent
./snajpagent -- "explain this repository"
./snajpagent -e -- "run the tests and summarize failures"
printf 'run the tests and summarize failures\n' | ./snajpagent -e
./snajpagent --resume --last -- "continue"
./snajpagent -l
./snajpagent -d -n builder
./snajpagent -s irc.example:6667 -n worker -o alice
./snajpagent -c irc.example:6667 -n worker -o alice
```

When a process has opened or created a durable session, normal shutdown prints
exactly one two-line block to stderr after restoring the terminal:

```text
resume:
COMMAND
```

Both lines begin at column zero, with no blank line between them. This
includes `/exit`, idle Ctrl-C, Ctrl-D or terminal EOF, `/archive`, one-shot
completion, runtime failure, and graceful SIGHUP/SIGTERM shutdown. Ctrl-C
first clears a nonempty composer without affecting the turn or session. On an
already-empty composer it interrupts the active turn or exits while idle; the
command is printed only when the process actually exits. `/delete`, help,
version, listing, command-line errors, and failures before a session exists
print no command.

The displayed command names the exact session, dotdir, explicit configuration
path when one was used, presentation overrides, and any unconsumed one-turn
model/effort override. In networked mode it spells out the effective client,
server, or combined role so clients reconnect and a hosted listener restarts.
Arguments are POSIX-shell quoted. Prompts, credentials, credential values,
tool output, and configured secret environment values are never included.
SIGKILL, power loss, and fatal process corruption cannot reliably run cleanup
and therefore cannot promise this output.

With `-e`, omit the argument prompt to read it from non-terminal stdin. One
final LF or CRLF line terminator is removed; internal newlines are preserved.
The `-e -- PROMPT...` form remains available for an argument prompt.

Useful options:

```text
-C DIR      run in a specific workspace
-d          host the built-in IRC server
-s ENDPOINT connect to IRC, or choose the listener with -d
-c ENDPOINT connect to IRC; repeatable (default localhost:6667)
-n NICK     networked model nick (required in networked mode)
-o NICK     local operator nick
-r ROOM     hosted room name
-m MODEL    override the configured model
-v          increase verbosity, repeatable up to six times
--dotdir DIR
--config FILE
--effort LEVEL
--color[=auto|always|never]
--no-color  disable color
--markdown  render model Markdown (default)
--no-markdown  show model Markdown literally
```

`--daemon`, `--listen`, `--client`, `--model-nick`, `--operator-nick`, and
`--room-name` are the long forms of the network options. Without daemon mode,
`-s`/`--listen` adds an outgoing connection like `-c`/`--client`; with daemon
mode, it selects the listener while any `-c` connections remain outgoing.
Bare `-s` or `-c` uses `localhost:6667`. The former `-d`, `-c`, `-r`, and `-o`
meanings remain available as `--dotdir`, `--config`, `--resume`, and
`--effort`. Dotdir and explicit configuration paths must be absolute.

The built-in help is intentionally short:

```sh
./snajpagent -h
```

In an interactive session, `/model` and `/model list` display the persistent
provider/model/reasoning catalog without contacting a provider. If the cache is
missing, the listing asks you to run `/model cache`. That command explicitly
refreshes every configured provider through its authenticated model endpoint;
there is no automatic age check, so the user decides when it is stale. A
provider whose normalized API path ends in `/backend-api/codex` uses the Codex
`/models?client_version=0.146.0` catalog endpoint; other providers use
`/v1/models`. Every catalog display ends with the cache update time.

`/model NUMBER` and `/model #NUMBER` select an exact displayed row. Append
`save` or `s` to a selection to also store its provider, model, and effort in
the active configuration, for example `/model 2 save` or
`/model codex-lb / gpt-5.6-sol / high s`. A bare `/model save` or `/model s`
remains an ordinary typed model ID. Typed
selection accepts `MODEL`, `MODEL / EFFORT`, or
`PROVIDER / MODEL / EFFORT`, ignoring whitespace around `/`. A typed model is
trusted and sent to the provider without checking whether it appears in the
cache. With only `MODEL`, the first configured provider is used and the
highest recognized advertised effort is selected; if the cache has no effort
metadata for it, the current/configured effort is kept. Typed selection itself
makes no provider request. An unknown typed provider/model pair is accepted and
saved, then gets a warning immediately after the normal next-turn confirmation.
A provider-native ID containing `/` can be selected unambiguously by its
displayed number.

`/effort LEVEL` remains available to change only the reasoning effort.

`/goal TEXT` starts a persistent objective. A normal model final answer is a
checkpoint while that goal remains active, so snajpagent begins another goal
turn automatically. Useful lifecycle commands are:

```text
/goal                         show status and wording
/goal TEXT                    start or reword
/goal set TEXT                use wording beginning with a command word
/goal "TEXT"                  quote wording beginning with a command word
/goal pause|resume            stop or restart automatic turns
/goal lock|unlock             deny or allow model wording changes
/goal complete|cancel         finish or stop from the user side
/goal help                    show the complete grammar
```

When no unfinished goal exists, the model also receives a strict
`create_goal({"objective":"..."})` tool. It may use that tool only for an
explicit user or system/developer request to start a persistent goal; ordinary
work and Markdown goal documentation never activate continuation. Successful
creation durably starts and arms the goal, so the current direct final is a
checkpoint followed by another goal turn. An active goal exposes
`update_goal`, not `create_goal`; paused and blocked goals expose neither.

Through `update_goal`, the model can rewrite an unlocked active goal, mark it
complete, or record a specific blocker. User wording changes remain allowed
while locked. Queued user turns run in FIFO order before the next automatic
goal turn, and refusal, turn failure, terminal input closure, or process
restart pauses automatic continuation. See
[`design/goals.md`](design/goals.md) for the complete lifecycle contract.

The ordinary composer identifies both the model and reasoning effort. It uses
`MODEL/EFFORT › ` while idle and `MODEL/EFFORT » ` during a turn, where `›`
means start a turn and `»` means add input to the active turn. The idle prompt
shows the effective next-turn selection; the active prompt keeps the model and
effort frozen for that turn even if `/model` or `/effort` stages a different
next-turn value. Terminal-unsafe characters in those trusted selectors are
escaped for display without changing the values sent to the provider.

At startup the orientation identifies the product build, workspace, and
abbreviated session explicitly, for example
`snajpagent 0.98 · /workspace · session id a1b2c3d4`. Resumed sessions
add their resume, turn, and queue state; the model is not repeated there.

While a response is streaming, typing opens the active-turn composer on a new
line and briefly pauses visible model output. With a nonempty draft, Tab keeps
its contextual behavior: slash-command completion when possible, indentation
while idle, or future-turn queueing while active. Empty Tab is a no-op outside
networked mode. Ctrl-C clears a nonempty draft first; only Ctrl-C on an empty
active composer interrupts the turn. `/queue` or `/q` lists queued turns with
one-based numbers.
Queue mutations accept these short and long forms:

```text
/q 2d          /queue 2 delete
/q 2e          /queue 2 edit
/q c           /queue clear
/q p           /queue pop
```

`pop` removes the newest queued turn. Editing reopens the selected text under
an `edit N › ` prompt and saves it in the same FIFO position with Enter (or
with Tab while another turn is active).

## IRC Chat Mode

`-d` hosts the bounded built-in IRC server in the normal foreground process;
it does not detach. Without `-d`, `-s` adds an outgoing connection just like
`-c`; with `-d`, `-s` selects the listener. Each `-c` always adds an outgoing
connection, so hosting and one or more client roles can be combined. Networked
mode requires `-n NICK`. Initial chat text in this mode must follow `--`:

```sh
./snajpagent -d -n builder -- "introduce yourself"
./snajpagent -d -s 0.0.0.0:7667 -n builder -o alice -r builds
./snajpagent -s localhost:7667 -n reviewer -o bob
./snajpagent -c localhost:7667 -c irc.example:6667 -n reviewer -o bob
```

Each server has one advertised room. Its default name comes from the host
name, and its initial topic is the absolute launch workspace. snajpagent
clients join that room automatically. Local operators and ordinary IRC
clients receive channel operator mode `+o`; the agent identity does not.
Operators can change the topic with normal IRC `TOPIC` or the local
`/topic TEXT` command. `/names` shows current members, and `//TEXT` sends chat
beginning with `/`.

The network composer is `OPERATOR_NICK@MACHINE_HOSTNAME › ` while idle and
`OPERATOR_NICK@MACHINE_HOSTNAME » ` during a turn. It uses the configured local
operator nick and the local machine hostname in both presentation views; the
hostname is not the IRC endpoint or room.

The timestamped terminal interface starts in `chat`, which shows operator and
room messages plus room events. `rollout` shows the local model's streamed work
using the ordinary non-networked visibility rules, including model text at
verbosity 0. `/chat` and `/rollout` select either view, and empty Tab toggles
between them without changing where operator input is sent. A nonempty draft
never switches views.

Rollout also shows terse durable lifecycle milestones at verbosity 0:
`• Compacted`, `• Goal set`, and `• Goal cleared`. Goal creation/rewording maps
to “set,” while completion/cancellation maps to “cleared.” In chat view these
records wait for exact-once rollout catch-up and never enter IRC. Verbosity 4
may append the durable sequence/type to the same line.

The interface remains a scrolling, append-only IRC client rather than a
windowed TUI. Switching appends a short view boundary and then prints unseen
content for the entered view once, in order, before continuing live output;
terminal history is never cleared or repainted. The catch-up begins with the
current foreground run rather than replaying the complete session log.

Chat view hides the local model's response and model/tool internals at default
verbosity. The networked verbosity ladder is additive:

1. `-v` reveals terminal model replies and every tool call with complete
   arguments, completion state, and configured result display;
2. `-vv` adds intermediate model commentary and runtime/provider-cycle state;
3. `-vvv` adds further agent runtime detail;
4. higher levels add durable/IRC state, sanitized protocol bodies, then
   bounded transport diagnostics.

Only terminal public assistant text is posted to the room. Local operator,
current `+o`, and direct-mention messages steer the model at the earliest safe
boundary; in-flight generation and commands finish normally, and multiple
urgent messages are coalesced. Other chat and membership/topic events are
admitted when convenient and do not force a reply. First join and every
successful compaction inject a fresh bounded room snapshot. The runtime owns
sockets, joining, history, and reconnects; the model may use `irc_send`,
`irc_state`, and privilege-checked `irc_topic`, but never needs to babysit the
network.

Model messages in the terminal transcript use the same Markdown presentation
as locally streamed answers. Operator messages, topics, membership notices,
and protocol diagnostics stay literal. Fenced code can span consecutive IRC
messages from one endpoint and sender without leaking parser state to another
sender.

`--color` selects `always`; `--color=auto|always|never` makes the policy
explicit, and `--no-color` selects `never`. The default `auto` mode colors only
terminal output and honors `NO_COLOR`. Color applies to networked and ordinary
UI roles, uses broadly supported 16-color foreground attributes, and never
enters stored text, provider input, or IRC traffic. Compact tool-start lines
color the arrow and tool name, while an `exec` command itself remains in the
default foreground like its output. Lifecycle milestone bullet lines use a
dedicated bold-green role and reset before subsequent output. At `-v` in either
mode, the first `exec`
line shows its effective `timeout=none` or `timeout=Nms` before the command.

Markdown presentation is enabled by default. `--no-markdown` disables it and
`--markdown` explicitly enables it, overriding configuration. Headings, lists,
quotes, fenced code, inline code, emphasis, strikethrough, and links receive a
compact terminal rendering; link destinations remain visible. This is a
presentation layer only: redirected output, provider input, durable events,
and IRC frames retain the model's exact bytes. Disabling color keeps Markdown's
structural rendering while removing its attributes. Prose paragraphs receive a
`• ` prefix and an empty row between paragraphs; wrapped continuation lines
start at the left margin without a hanging indent. Terminal wrapping keeps
trailing punctuation with the preceding text and breaks after hyphens, dashes,
periods, commas, and similar closing punctuation in both Markdown modes.

## Configuration

By default snajpagent keeps all private application data under:

```text
$HOME/.snajpagent
```

This contains:

```text
config.ini
models.json
sessions/
trash/
```

Each session's append-only rollout is stored at
`$DOTDIR/sessions/<session-id>/events.jsonl`. After successful manual or
automatic compaction, every subsequent provider request includes a developer
notice with that current session's absolute rollout-log path, so the model can
inspect the full local history when the compacted context lacks detail.

Use `--dotdir DIR` to select another dotdir, or `--config FILE` to read a
configuration file elsewhere while leaving cache and session locations
unchanged.

Interactive `/config` opens that exact active file in `$EDITOR`. If its bytes
changed when the editor exits, snajpagent validates and reloads it; an invalid
edit reports the error and leaves the previous runtime configuration active.
Command-line overrides remain authoritative. Existing durable session model
preferences are not replaced by edited defaults.

Minimal OpenAI configuration:

```ini
[agent]
provider = openai
model = gpt-5.5
reasoning_effort = default
max_goal_prompt_bytes = 262144
read_agents_md = true

[provider openai]
base_url = https://api.openai.com
api_key_env = OPENAI_API_KEY
```

Multiple named `[provider NAME]` sections are loaded in file order. The first
is used when a model selector omits a provider. Provider names must be unique;
the legacy unnamed `[provider]` form is still accepted with the name `default`.
For example:

```ini
[provider openai]
base_url = https://api.openai.com
api_key_env = OPENAI_API_KEY

[provider codex-lb]
base_url = http://127.0.0.1:2455/backend-api/codex
api_key_env = CODEX_LB_API_KEY
exact_token_count = false
native_compaction = false
```

OpenRouter uses the same OpenAI-compatible Responses path. Its `/api/v1`
base URL is accepted directly:

```ini
[agent]
model = openai/gpt-5.5
reasoning_effort = default

[provider]
base_url = https://openrouter.ai/api/v1
api_key_env = OPENROUTER_API_KEY
openrouter_referer = https://github.com/snajpa/snajpagent
openrouter_title = snajpagent
exact_token_count = false
native_compaction = false
```

Local codex-lb proxy configuration:

```ini
[agent]
model = gpt-5.5
reasoning_effort = default

[provider]
base_url = http://127.0.0.1:2455/backend-api/codex
api_key_env = CODEX_LB_API_KEY
auto_compact_input_tokens = 0
exact_token_count = false
native_compaction = false
```

Set the environment variable named by `api_key_env` before starting the agent.

Project instruction discovery is enabled by default. Before every new turn,
snajpagent reads the applicable `AGENTS.override.md` or `AGENTS.md` files from
the workspace's repository root through the workspace itself, including an
`AGENTS.md` in the current workspace, and supplies their contents to the model
as project guidance. Set `read_agents_md = false` in `[agent]` to disable all
such discovery. Instruction paths, sizes, and hashes are frozen in the durable
`turn_started` event so every response cycle in that turn uses the same files.

The optional typing pause is measured from the most recent input edit, accepts
`0` through `5000` milliseconds, and defaults to `500`:

Character insertion, deletion, and cursor movement update the composer in
place, so ordinary typing does not blank and repaint the complete input line.
Status changes, interposed output, and terminal resize still perform the
structural redraw needed to keep the scrolling transcript coherent.

```ini
[ui]
typing_pause_ms = 500
color = auto
markdown = true
verbosity = 0
```

Commands have no built-in foreground handoff deadline. A model-requested
positive `timeout_ms` sets one; `[tool] default_timeout_ms` can instead impose
an operator fallback, with `0` retaining no automatic handoff. If the deadline
expires, the command continues in the background and the model receives its
live handle through the same continuation used for urgent steering and IRC
mentions; timeout never kills the process. The configured `max_timeout_ms` is
both the tool-schema and runtime ceiling. Tool output is captured completely
for the model; `max_output_bytes` limits only how many result bytes each call
displays, and defaults to unlimited with `0`:

```ini
[tool]
default_timeout_ms = 0
max_timeout_ms = 86400000
max_output_bytes = 0
```

IRC settings have direct command-line equivalents. `client` may appear up to
16 times; command-line clients replace the configured list. History accepts 1
through 1000 lines and defaults to 200:

```ini
[irc]
daemon = true
listen = localhost:6667
client = localhost:7667
client = irc.example:6667
model_nick = builder
operator_nick = alice
room_name = builds
history_lines = 200
```

Other supported config sections are `[irc]`, `[ui]`, and `[tool]`; the parser in
`src/config.c` is currently the source of truth for every accepted key.
The complete cache and selector contract is recorded in
`design/model-cache.md`.
`max_goal_prompt_bytes` accepts 1 through 1,048,576 and measures UTF-8 bytes.

## Design

Design notes live in `design/`. Start with `design/architecture.md` for the
runtime shape and durability model. The IRC server/client, operator chat,
steering, history, compaction, and program-wide color contract is recorded in
[`design/irc-chat.md`](design/irc-chat.md).
The streamed and static model-text presentation contract is recorded in
[`design/markdown-rendering.md`](design/markdown-rendering.md).

## License

All first-party material is licensed under GPL-2.0-only. See `COPYING` and
`LICENSE_SCOPE`.
