<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent

A coding agent for your terminal, with a built-in IRC server and client.

[Website](https://agent.snajpa.net) ·
[Downloads](https://agent.snajpa.net/downloads.html) ·
[Install](#install-and-choose-a-provider) ·
[User manual](https://agent.snajpa.net/manual.html)

## 1. Work on a project

After [installing and choosing a provider](#install-and-choose-a-provider),
start in the project you want to change:

```sh
cd /path/to/your/project
snajpagent
```

Describe a task and press Enter: “Fix the empty-input bug, keep the public API
unchanged, and run the tests.” The model can read and edit files and run commands.
Press Enter on an empty idle rollout prompt to ask it to continue.

Read its replies and scroll back normally. Tool details are hidden by default;
`/verbose 1` shows compact activity and `/verbose 2` adds input/result previews.
Type these commands and press Enter, even while the model works.

[![A local session reports fixing whitespace handling and passing four checks](www/screenshots/ordinary.png)](www/screenshots/ordinary.png)

Model output with tool details hidden.

### Correct this task or queue the next one

The local conversation and work appear in **rollout**. Your request and the
work through its final answer make up a **turn**. You can type while it runs;
typing alone does not interrupt it.

In rollout, **Enter sends a correction now**: “Use the existing parser; don't
add a dependency.” It interrupts the model's response and continues from the
text already delivered. Commands already running stay alive; the model can
wait for them or stop them.

**Tab at the end of an ordinary message queues a follow-up while work is active.**
“Then add regression coverage” waits for the current turn to finish. Queued
prompts run oldest first, one at a time. `(N)` in the prompt counts waiting
turns, excluding current work. The acknowledgement
`queued (/next or /q c) ›` confirms submission: `/next` resumes a paused queue,
while `/q c` clears waiting prompts without stopping current work.

Tab completes command names before it queues. Start a line with `/sta` and press
Tab to get `/status `; press Enter to run it. Completion applies while the cursor
is in or at the end of that first `/command`. After that token, Tab follows
the nickname-completion or idle/active behavior described below. A unique match adds a space; ambiguous matches extend the common
prefix and a second Tab lists choices. No match leaves the draft unchanged,
not queued. Completion never sends or queues text.

Outside completion, Tab inserts spaces while idle. **Empty Tab switches between
rollout and chat.** Nickname completion in chat is explained below.

### Keep working, or leave and come back

Failed turns retry automatically three times. Set `[agent] max_turn_retries`
to change the limit (`0` disables it). Active goals keep retrying without a limit.

A normal final answer ends the turn. Set a goal when you want work to continue
beyond it:

```text
/goal fix the bug and validate the change
```

Goals continue until complete, paused, cancelled, or blocked. Queued prompts
come first. `/goal pause` pauses continuation at a turn boundary; it does not
interrupt a running turn. Errors keep an active goal retrying with paced,
interruptible waits, preserving completed work and live command handles.

Ctrl-C clears a nonempty draft; with an empty draft, it interrupts the turn.
Ctrl-D on an empty draft exits. No work continues after the program exits.
The conversation, tool results, queue and goal are saved as a **session**.
After the first prompt or goal, normal exit prints its resume command. Exiting
a fresh session before submitting anything creates no saved session. You can also list sessions or reopen the
latest one for this project directory:

```sh
snajpagent -l
snajpagent --resume --last
```

**An active goal continues on resume.** Pause it
before exiting if you want it to stay paused. Paused, blocked and finished goals
retain their states. Queued prompts wait for `/next` after resume and take
priority over automatic goal work. Resume restores saved context. Command
processes that ended with the previous program stay stopped.

### Keep useful findings in files

For longer work, have the model keep findings, decisions and corrections in
project files, with pointers in `AGENTS.md`. Keep the notes current so later
tasks can use what was learned and pick up unfinished work. Record proposals
and approvals separately.

snajpagent tells the model where project guidance is; the model reads what it
needs. `-d DIR` adds a documentation root containing `AGENTS.md` or
`AGENTS.override.md`. Repeat it for several roots, including notes spanning
repositories. Relative paths use the launch directory even with `-C`; the printed
resume command retains them.

## 2. Work together

People and agents share an IRC room. One snajpagent instance hosts it; others
connect. Run these in separate terminals, from the project each model should use:

```sh
snajpagent -s -n builder -o alice -r work
snajpagent -c -n reviewer -o bob
```

The first hosts `#work` at `localhost:6667`; the second joins it on the same
machine. `-n` names the model, `-o` its operator, and `-r` the hosted room.
`/names` shows accepted names: a name already in use gets a suffix.

### Choose who receives your message

Networked startup opens **chat**, the shared room. Enter sends as your operator
name. `@builder check the empty-input case` asks builder to work; a mention during
its work steers it at a safe boundary without cutting off its current response.
Ordinary room conversation supplies background context. A direct mention starts
a task for the addressed model.

Empty Tab switches to **rollout**, where Enter directs your local agent without
sending your instruction to everyone. Each view keeps its own draft and history.
The local transcript stays in rollout. Models use `irc_send` to publish chosen
messages, which can include material from that transcript.

In chat, type `@bu` and press Tab. If `builder` is the only match, it becomes
`@builder `, including the space. Keep typing after it. `@` begins a nickname
word anywhere in a message: `please ask @bu` also completes;
bare `bu` does not. No match leaves the text unchanged.

At the end of the finished message, **Enter sends to the room**. **Tab queues
it as a local follow-up while your model is active**, even in chat; it does not
send to the room. If the cursor is still at the end of `@bu` or `@builder`, Tab
completes that name first. Outside completion while idle, Tab inserts spaces.

### Coordinate work

Give agents tasks in the room; they can exchange messages and report results.
How you divide the work is up to you. Use project files and Git for code and
handoff notes: joining IRC does not share files, credentials or command processes.
For independent edits to one repository, use separate Git worktrees.

`/server start` hosts a room; `/connect ENDPOINT` adds a connection. `/names` lists
rooms and members. `/2` selects room 2, `/2 TEXT` sends there once, and `/all TEXT`
broadcasts once. Explicit sends also work in rollout. `/status` shows whether a
requested connection has actually joined. See the manual for connection controls,
history and reconnect behavior.

**IRC has no authentication or TLS.** Use localhost, a trusted network, or a
secure tunnel. Tools run with your local permissions, without a command-approval
sandbox.

## Further controls

`/help` lists commands and editing keys; `/status` shows the current state.
Ctrl-J inserts a newline. Up/Down move through draft rows, then prompt history
at the edges; Ctrl-P/Ctrl-N go straight through history. Ctrl-R searches it.
`/queue` shows waiting work; `/queue 2 edit` revises its second item and
`/queue 2 delete` removes it. The manual covers the queue editor and slash-command
exceptions.

`/model` lists the locally cached catalog; `/model cache` explicitly refreshes
it. Select a displayed row by number, or use `/model PROVIDER/MODEL/EFFORT` while
idle. Add `save` to persist a selection.

The prompt's context percentage shows the last measured request input against
the resolved input budget, rounded up. It uses provider-reported input counts. A fresh
session starts at `0%`; after a turn, unknown or incomparable measurements show
`?%`. `/status` explains the accounting. Older context is automatically
compacted into a summary as it fills, or use `/compact` while idle. The original
transcript stays on disk, but a summary does not preserve every detail—keep
important requirements in project documents.

For inspection without commands or edits, use `/ro QUERY`. It can list, read and
search files and use provider-hosted web search, but cannot run commands, patch
files, change goals or send IRC messages. `/queue /ro QUERY` asks it next during
active work. Readable local files remain accessible and session history is recorded.

## Install and choose a provider

snajpagent is written in C so the agent itself can run on more of the systems
where development happens, including unfamiliar ones.

[Download an executable](https://agent.snajpa.net/downloads.html) for your platform
and verify `SHA256SUMS`. Runnable debug builds appear below their release in each operating-system panel.

On macOS, rename the downloaded executable to `snajpagent`, check its SHA-256
against the download row, and make it executable:

```sh
shasum -a 256 ./snajpagent
chmod +x ./snajpagent
./snajpagent
```

If macOS blocks the file and you trust it, remove quarantine from that file only:

```sh
xattr -d com.apple.quarantine ./snajpagent
./snajpagent
```

For a debug archive, compare `shasum -a 256 FILE.tar.gz` with the archive's
download row, then extract it with `tar -xzf FILE.tar.gz`. Make the extracted
executable runnable and launch it as above. Keep system-wide macOS protections enabled.

Official stable binaries check and install updates in the background on launch.
The current process keeps running; one banner links to the release log and asks
you to restart when convenient. Set `[agent] auto_update = false` to opt out.
Development binaries are debug builds and default to updates off; set the option
to `true` to follow `latest-dev`. Ordinary source builds remain updater-free.
The manual covers publisher URLs, permissions and recovery.

The normal build needs C11/POSIX with pthreads, GNU make, libcurl, and Jansson:

```sh
git clone https://github.com/snajpa/snajpagent.git
cd snajpagent
make
sudo make install
```

The binary and manual install under `/usr/local`. Production also needs `strip`
and `objcopy` on Linux, or `strip` and `dsymutil` on macOS. `make DEBUG=1` builds
for debugging; `make help` lists build options. See [dependency notes](DEPENDENCIES.md)
for platform scope.

`make -jN prod-matrix` builds all implemented standalone targets into
`build/matrix/`, without installation or VMs. See the
[platform notes](DEPENDENCIES.md) for target-specific requirements.
`make prod-linux-armv6` builds one hard-float static PIE for ARMv6 Raspberry Pi
1/Zero-class systems and ARMv7, with an ARMv6KZ/VFPv2 baseline.
`make prod-linux-riscv64` builds a RISC-V RV64GC/LP64D static PIE.
`make prod-linux-ppc64le` builds a little-endian POWER8 ELFv2 static PIE.
`make prod-linux-i686` builds modern 32-bit Linux static PIE.
`make prod-linux-i686-legacy` builds a separate static non-PIE executable for
Linux 2.4.27, with embedded TLS, roots and locale data.
It needs working procfs and secure OS entropy; see [platform limits](DEPENDENCIES.md).

For experimental Windows x64 or ARM64, use `make prod-windows-x86_64` or
`make prod-windows-arm64` with pinned Nix dependencies. Copy the resulting
`build/matrix/windows-ARCH/bin/snajpagent.exe` to Windows;
application libraries and CA roots are included without third-party runtime DLLs.

For experimental NetBSD amd64 source builds, use `make prod-netbsd-amd64`
for 10.1 or `make prod-netbsd-amd64-legacy` for 2.0 and 5.2.3. These use
separate native pthread ABIs; see the [platform notes](DEPENDENCIES.md).

Plain `make` builds only the host platform.

Without configuration or existing credentials, the first interactive launch
offers ChatGPT/Codex subscription, OpenRouter, OpenAI, or custom-provider setup.
Authenticate and choose a [supported model](#supported-providers). `snajpagent login status`
reports local credential sources without contacting a provider. The manual
explains login methods and logout.

For manual configuration, create a private directory:

```sh
install -d -m 700 "$HOME/.snajpagent"
```

Save this as `$HOME/.snajpagent/config.ini`, choosing a supported model:

```ini
[agent]
provider = openai
model = gpt-5.5

[provider openai]
base_url = https://api.openai.com
api_key = ${OPENAI_API_KEY}
```

Export `OPENAI_API_KEY` in your shell before launch. Provider names are local
labels. `/config` opens the active file in `$EDITOR` and reloads valid changes.
If an edit is invalid, the previous runtime configuration stays active, but
fix the file before restarting.

## Use it in scripts

One-shot mode runs a prompt without an interactive conversation:

```sh
snajpagent -e -- "run the tests and summarize failures"
printf '%s\n' 'review the current diff' | snajpagent -e
```

Model text goes to stdout; diagnostics and the resume hint go to stderr.
Redirected output has no terminal styling. Use exit status for failure handling;
the internal pre-1.0 event-log format can change between versions.

The [user manual](https://agent.snajpa.net/manual.html) and `man snajpagent`
cover the full reference and troubleshooting. Both come from [one source](snajpagent.1);
[project instructions](AGENTS.md) require updates alongside behavior changes.
[Design notes](design/architecture.md) cover the implementation. GPL-2.0-only;
see [COPYING](COPYING).

## Supported providers

Supported connections: OpenAI, ChatGPT/Codex subscription, OpenRouter, and custom
providers with an OpenAI-compatible Responses API.

- **OpenAI GPT-5+:** recommended; the only model family thoroughly tested and
  known to work reliably with snajpagent.
- **DeepSeek:** an open-model option known to work well.
- **Anthropic:** not supported. The project's assessment is that alignment
  problems make its models unsuitable for long-horizon autonomous work without
  oversight.
- **Other models:** use at your own risk. GLM is discouraged, as are open models
  trained heavily on Anthropic rollouts.
