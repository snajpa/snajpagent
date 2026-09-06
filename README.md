<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent

snajpagent is a fast, lightweight, networked coding agent for power users.
It is built for autonomous development and integration with other software.

[![snajpagent fixing and testing a small C program](www/screenshots/ordinary.png)](www/screenshots/ordinary.png)

One foreground C process runs coding tools and keeps resumable sessions.
Work locally or bring agents and operators together through IRC.

## Install

You need a C11/POSIX environment with pthreads, GNU make, libcurl, and Jansson.

```sh
git clone https://github.com/snajpa/snajpagent.git
cd snajpagent
make
sudo make install
```

Installation adds the binary and `snajpagent(1)` under `/usr/local` by default.
You can also run `./snajpagent` directly from the build directory.

Plain `make` builds optimized, stripped **production for the host platform**;
`make DEBUG=1` selects an unstripped debug build with frame pointers. Switch
profiles without cleaning. `make install` selects production unless you also
pass `DEBUG=1`. `make -jN` parallelizes compilation; it does not start foreign
builds or VMs. `make help` lists targets and overrides without building.

Production defaults to size optimization (`-Os`), with runtime checks and
unwind information retained. Debug defaults to `-Og`; explicit compiler flags
can select another optimization level. Neither profile enables host-specific
instruction sets or makes LTO tooling mandatory.

For a self-contained Linux x86-64 executable, use `make prod-linux-x86_64`.
This explicit target needs Nix on the build host and may download/build its
pinned dependencies. Copy `build/matrix/linux-x86_64/bin/snajpagent` to the
destination; it needs no third-party libraries or certificate sidecar there.
Matching optional symbols are under `build/matrix/linux-x86_64/bin/.debug/`.
The first implementation targets modern Linux; older-kernel qualification is
separate. See [dependencies](DEPENDENCIES.md) for embedded trust, license/source
obligations and the `SSL_CERT_FILE` override. Plain `make` remains host-only.

`make prod-macos-arm64` or `make prod-macos-x86_64` cross-builds a small macOS
11+ ARM64 or Intel executable using
pinned LLVM and the Apple SDK. Application libraries and CA data are static;
only the OS's `libSystem` remains dynamic. Copy
`build/matrix/macos-ARCH/bin/snajpagent`; the adjacent optional `.dSYM` matches
it. **Experimental: cross-built and inspected, not yet runtime-qualified on
macOS.** No Apple signing identity or notarization is implied.

`make -j2 prod-macos-universal` builds the two Mac slices in parallel, then
combines them into `build/matrix/macos-universal/bin/snajpagent` with a matching
two-architecture dSYM. It is a native Mach-O universal file: no launcher,
startup download, or payload extraction is needed. Standalone slices remain
available; the same experimental macOS runtime qualification applies.

Production keeps matching symbols beside the executable: `snajpagent.debug`
on ELF systems, `snajpagent.dSYM` on macOS. Retain these for debugging that
exact production binary; they are not installed or required at runtime.
Production packaging needs `strip` and `objcopy` on ELF, or `strip` and
`dsymutil` on macOS; debug builds need neither. Override the tools with
`STRIP=...`, `OBJCOPY=...`, `DSYMUTIL=...`. Explicit `CFLAGS`/`LDFLAGS` replace
profile defaults (required thread flags remain). Both profiles retain the
same functionality and runtime checks; debug selection is not a feature mode.

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
Command timeouts hand execution back alive, not cancelled. You still control
the session through the same prompt.

`/model cache` downloads configured providers' catalogs. `/model` lists the
cache; `/model NUMBER` selects a row. Add `save` to persist the selection.
`/effort LEVEL` changes reasoning effort while idle.

Rollout shows the model's conversation. Add `-v` for compact tool activity,
`-vv` for previews, or `-vvv` for full retained arguments and results.
Higher levels are diagnostics. `/verbose N` changes the level immediately.
Color and terminal Markdown are automatic; disable them with `--no-color`
and `--no-markdown`.

## Resume or script

Normal exit prints the exact command to resume your session. You can also use:

```sh
snajpagent -l
snajpagent --resume --last
```

Resume pauses goals and queued turns. Use `/goal resume` or `/next` when ready.
`--dotdir DIR` selects another private state directory.

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
