<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent

snajpagent is a terminal coding agent. It talks to an OpenAI-compatible
Responses API endpoint, keeps durable local sessions, can resume previous
conversations, and exposes a small set of tools for command execution, process
continuation, file patching, and hosted web search.

The project is written in C and is still marked `0.9.0-wip`, but the normal
interactive path is usable.

## Features

- Persistent multi-turn sessions stored as append-only local event logs.
- Persistent model discovery across multiple ordered provider configurations.
- Persistent `/goal` objectives with automatic multi-turn continuation,
  pause/resume, user-controlled wording locks, and explicit completion or
  blocking from the model.
- Durable `--resume` support without a background worker or tmux session.
- First-class single-room IRC hosting and repeatable outgoing connections for
  agent/operator chat, with reconnects, bounded history, and operator-aware
  model steering.
- Interactive mode, one-shot execution mode, and session listing.
- Terminal-width wrapping for streamed model text without changing stored or
  redirected response bytes.
- Default-on terminal Markdown presentation for streamed model text, resumed
  assistant history, and non-operator model messages in the IRC transcript.
- OpenAI Responses streaming over libcurl, including hosted `web_search`.
- Codex-style instruction discovery from `AGENTS.override.md` and `AGENTS.md`.
- Tool support for `exec_command`, yielded process handles, `write_stdin`, and
  strict `apply_patch`, plus IRC chat/state/topic tools in networked mode.
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

With `-e`, omit the argument prompt to read it from non-terminal stdin. One
final LF or CRLF line terminator is removed; internal newlines are preserved.
The `-e -- PROMPT...` form remains available for an argument prompt.

Useful options:

```text
-C DIR      run in a specific workspace
-d          host the built-in IRC server
-s ENDPOINT connect to IRC, or choose the listener with -d
-c ENDPOINT connect to IRC; repeatable (default localhost:6667)
-n NAME     networked agent name (required in networked mode)
-o NAME     local operator name
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

`--daemon`, `--listen`, `--client`, `--name`, `--operator-name`, and
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
missing, the first listing imports `$CODEX_HOME/models_cache.json`, or
`$HOME/.codex/models_cache.json` when `CODEX_HOME` is unset, under the first
configured provider. `/model cache` explicitly refreshes every configured
provider through its authenticated model endpoint; there is no automatic age
check, so the user decides when it is stale. Every catalog display ends with
the cache update time.

`/model NUMBER` and `/model #NUMBER` select an exact displayed row. Typed
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

The model can rewrite an unlocked active goal, mark it complete, or record a
specific blocker through the strict `update_goal` tool. User wording changes
remain allowed while locked. Queued user turns run in FIFO order before the
next automatic goal turn, and refusal, turn failure, terminal input closure,
or process restart pauses automatic continuation. See
[`design/goals.md`](design/goals.md) for the complete lifecycle contract.

While a response is streaming, typing opens the `steer › ` composer on a new
line and briefly pauses visible model output. Tab queues the current composer
text as a future turn. `/queue` or `/q` lists queued turns with one-based
numbers. Queue mutations accept these short and long forms:

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
mode requires `-n NAME`. Initial chat text in this mode must follow `--`:

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

The timestamped terminal transcript behaves like a scrolling IRC client, not
a windowed TUI. At the default verbosity it shows operator/room chat and room
events, but hides the local model's response and all model/tool internals. The
networked verbosity ladder is additive:

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
default foreground like its output. At `-v` in either mode, the first `exec`
line shows its effective `timeout=none` or `timeout=Nms` before the command.

Markdown presentation is enabled by default. `--no-markdown` disables it and
`--markdown` explicitly enables it, overriding configuration. Headings, lists,
quotes, fenced code, inline code, emphasis, strikethrough, and links receive a
compact terminal rendering; link destinations remain visible. This is a
presentation layer only: redirected output, provider input, durable events,
and IRC frames retain the model's exact bytes. Disabling color keeps Markdown's
structural rendering while removing its attributes.

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

Minimal OpenAI configuration:

```ini
[agent]
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
name = builder
operator_name = alice
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
