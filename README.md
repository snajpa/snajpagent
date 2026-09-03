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
provider; there is no automatic age check, so the user decides when it is
stale. Every catalog display ends with the cache update time.

`/model NUMBER` and `/model #NUMBER` select an exact displayed row. Typed
selection accepts `MODEL`, `MODEL / EFFORT`, or
`PROVIDER / MODEL / EFFORT`, ignoring whitespace around `/`. A typed model is
trusted and sent to the provider without checking whether it appears in the
cache. With only `MODEL`, the first configured provider is used and the
highest recognized advertised effort is selected; if the cache has no effort
metadata for it, the current/configured effort is kept. A provider-native ID
containing `/` can be selected unambiguously by its displayed number.

`/effort LEVEL` remains available to change only the reasoning effort.

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

Other supported config sections are `[ui]` and `[tool]`; the parser in
`src/config.c` is currently the source of truth for every accepted key.
The complete cache and selector contract is recorded in
`design/model-cache.md`.

## License

All first-party material is licensed under GPL-2.0-only. See `COPYING` and
`LICENSE_SCOPE`.
