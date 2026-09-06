<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent

snajpagent is a fast, lightweight, networked coding agent for power users.
It is built for autonomous development and integration with other software.

[![snajpagent fixing and testing a small C program](www/screenshots/ordinary.png)](www/screenshots/ordinary.png)

## Install

You need a C11/POSIX environment with pthreads, GNU make, libcurl, and Jansson.

```sh
git clone https://github.com/snajpa/snajpagent.git
cd snajpagent
make
sudo make install
```

Installation adds the binary and `snajpagent(1)` under `/usr/local` by default.

`make` builds stripped host-native production; `make DEBUG=1` keeps debug symbols.
Use `make help` for targets and overrides. Production needs `strip` and `objcopy`
on Linux, or `strip` and `dsymutil` on macOS. Optional Nix targets build standalone
Linux executables and experimental macOS variants. See the [manual](snajpagent.1)
for commands and [dependency notes](DEPENDENCIES.md) for portability and licenses.

## Choose a provider

Start `snajpagent` in your project. Without configuration or an existing API
credential, the first interactive launch offers setup for ChatGPT/Codex,
OpenRouter, OpenAI, or a Responses-compatible service.
Choose a provider, authenticate, and select a model it supports.
Configuration and private credentials live under `$HOME/.snajpagent`.

For manual configuration, create that directory with mode `0700` and save this
as `config.ini`, using an exported `OPENAI_API_KEY` and an available model:

```ini
[agent]
provider = openai
model = gpt-5.5

[provider openai]
base_url = https://api.openai.com
api_key = ${OPENAI_API_KEY}
```

Providers have local names. `--provider NAME` selects one at startup;
`/model PROVIDER/MODEL/EFFORT` changes it in a session.
The [manual](snajpagent.1) covers login, file-backed secrets, model aliases,
and per-model limits. `/config` opens the configuration through `$EDITOR`.

## Work

```sh
cd /path/to/project
snajpagent
```

Describe the change and press Enter. In rollout, Enter during active work
steers the model; Tab queues the draft for a later turn. Ctrl-J inserts a
newline. Up/Down recall prompts, and Ctrl-R searches their history.

Ctrl-C clears the draft. With an empty draft it interrupts active work.
Ctrl-D on an empty draft exits immediately; five consecutive Ctrl-C presses
within two seconds also exit. `/help` lists commands and keys.

Use `/ro QUERY` for a read-only turn. It can inspect files and use supported
provider-hosted search, but cannot run commands or change files. During active
work, queue it with Tab or `/queue /ro QUERY`.

For autonomous work, enter a goal:

```text
/goal fix the failing tests and verify the change
```

The agent continues across turns. `/goal pause` stops automatic continuation
after the current turn; `/goal resume` continues it. Queued prompts run first.
`/status` shows the goal, queue, model, and connection state.

The model can keep independent commands running while doing other work.
Command timeouts hand execution back alive, not cancelled.

`/model cache` downloads configured providers' catalogs. `/model` lists the
cache; `/model NUMBER` selects a row. Add `save` to persist the selection.
`/effort LEVEL` changes reasoning effort while idle.

Rollout shows the model's conversation. Add `-v` for compact tool activity,
`-vv` for previews, or `-vvv` for full retained arguments and results.
Higher levels are diagnostics. `/verbose N` changes the level immediately.
During a turn, Enter or Tab applies it locally to subsequent output, without
steering the model or replaying earlier detail.
Color and terminal Markdown are automatic; disable them with `--no-color`
and `--no-markdown`.

### Working documents

The model starts looking for working notes in the workspace (startup directory,
or `-C DIR`). snajpagent advertises applicable `AGENTS.md` paths, not their full
contents; the model reads relevant guidance and follows its pointers with tools.
Add other local documentation roots with repeatable `-d DIR`:

```sh
snajpagent -d /path/to/project-notes -d /path/to/device-notes
```

Each extra root needs `AGENTS.md` (or `AGENTS.override.md`). Relative `-d` paths
use the launch directory, independently of `-C`. Duplicate paths are collapsed.
Roots are invocation-local; the printed resume command includes them.

For example, point `AGENTS.md` at your existing maintenance notes, then ask:
“Fix the reconnect bug; retain what you learn in those notes.” The model can
record the established cause, remaining work and useful references. If you
correct a mistaken assumption, it should repair that account so a later run
can read it, check current facts and continue without repeating the research.
No particular notes filename or directory layout is required. Small and
read-only tasks should not generate documentation chores. Notes do not grant
permission, control goals, or automatically synchronize between devices.

## Resume or script

Normal exit prints the exact command to resume your session. You can also use:

```sh
snajpagent -l
snajpagent --resume --last
```

Resume pauses goals and queued turns. Use `/goal resume` or `/next` when ready.

For scripts, supply a prompt as arguments or through stdin:

```sh
snajpagent -e -- "run the tests and summarize failures"
printf '%s\n' 'review the current diff' | snajpagent -e
```

One-shot model text goes to stdout; diagnostics and the resume hint go to
stderr. Redirected text has no terminal color or Markdown presentation.

## Share a room

Run these in separate terminals, each in its own working project:

```sh
snajpagent -s -n builder -o alice -r work
snajpagent -c -n reviewer -o bob
```

Both default to `localhost:6667`. A server owns one room; clients join it
automatically. Its initial topic is the server's workspace path.
`/topic TEXT` changes the selected room's topic when you have op status.

Chat is public. Only mentions of the model's accepted nick steer active work;
ordinary conversation remains background context. The model posts through
`irc_send`; its other output stays in local rollout. Empty Tab switches views,
or use `/chat` and `/rollout`. In chat, Tab completes `@nick`.

You can start networking later with `/server start` or `/connect ENDPOINT`,
even during a turn. Multiple connections are supported: `/names` lists their
numbers, `/2` selects one, `/2 TEXT` sends there once, and `/all TEXT` broadcasts
once. History and reconnects are automatic. There is no windowed TUI.

Tools act with your local permissions; there is no command approval sandbox.
Protect configuration, session logs, and prompt history. IRC has no authentication
or TLS: keep it local or use a trusted network or secure tunnel.

## Reference

Read `man snajpagent` or the [source manual](snajpagent.1) for the complete interface.
[Design notes](design/architecture.md) explain the implementation.
`make check` runs the [tests](tests/); it needs Python 3 and Perl, with tmux for terminal checks.
The license is [GPL-2.0-only](COPYING); see [LICENSE_SCOPE](LICENSE_SCOPE).
