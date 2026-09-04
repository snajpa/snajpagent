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
- `-r` resume support without a background daemon, tmux session, or socket.
- Interactive mode, one-shot execution mode, and session listing.
- Terminal-width wrapping for streamed model text without changing stored or
  redirected response bytes.
- OpenAI Responses streaming over libcurl, including hosted `web_search`.
- Codex-style instruction discovery from `AGENTS.override.md` and `AGENTS.md`.
- Tool support for `exec_command`, yielded process handles, `write_stdin`, and
  strict `apply_patch`.
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
admission. Call `make tmuxcheck` directly when tmux coverage is mandatory.

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
./snajpagent -r --last -- "continue"
./snajpagent -l
```

Useful options:

```text
-C DIR      run in a specific workspace
-d DIR      use DIR as the private application directory
-c FILE     use FILE instead of DOTDIR/config.ini
-m MODEL    override the configured model
-o EFFORT   override reasoning effort
-v          increase verbosity, repeatable up to six times
--no-color  disable color
```

`--dotdir DIR` and `--config FILE` are the long forms of `-d` and `-c`.
The dotdir and explicit configuration paths must be absolute.

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

Use `-d DIR` to select another dotdir, or `-c FILE` to read a configuration
file elsewhere while leaving the cache and session locations unchanged.

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
```

Other supported config sections are `[ui]` and `[tool]`; the parser in
`src/config.c` is currently the source of truth for every accepted key.
The complete cache and selector contract is recorded in
`design/model-cache.md`.
`max_goal_prompt_bytes` accepts 1 through 1,048,576 and measures UTF-8 bytes.

## Design

Design notes live in `design/`. Start with `design/architecture.md` for the
runtime shape and durability model.

## License

All first-party material is licensed under GPL-2.0-only. See `COPYING` and
`LICENSE_SCOPE`.
