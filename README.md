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
- `-r` resume support without a background daemon, tmux session, or socket.
- Interactive mode, one-shot execution mode, and session listing.
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

Build and test:

```sh
make
make check
```

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
-c FILE     use a specific config file
-m MODEL    override the configured model
-o EFFORT   override reasoning effort
-v          increase verbosity, repeatable up to six times
--no-color  disable color
```

The built-in help is intentionally short:

```sh
./snajpagent -h
```

## Configuration

By default snajpagent reads:

```text
$XDG_CONFIG_HOME/snajpagent/config.ini
```

or, when `XDG_CONFIG_HOME` is not set:

```text
$HOME/.config/snajpagent/config.ini
```

Minimal OpenAI configuration:

```ini
[agent]
model = gpt-5.5
reasoning_effort = default

[provider]
base_url = https://api.openai.com
api_key_env = OPENAI_API_KEY
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

Other supported config sections are `[ui]` and `[tool]`; the parser in
`src/config.c` is currently the source of truth for every accepted key.

## Design

Design notes live in `design/`. Start with `design/architecture.md` for the
runtime shape and durability model.

## License

All first-party material is licensed under GPL-2.0-only. See `COPYING` and
`LICENSE_SCOPE`.
