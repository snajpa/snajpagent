<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent

A coding agent for your terminal, with a built-in IRC server and client.

[Website](https://agent.snajpa.net) ·
[Downloads](https://agent.snajpa.net/downloads.html) ·
[Install](#install-and-choose-a-provider) ·
[User manual](https://agent.snajpa.net/manual.html)

This guide and the web manual describe current source; downloaded releases ship
a matching manual. Check `snajpagent -V` when behavior differs.

## 1. Work on a project

After [installing and choosing a provider](#install-and-choose-a-provider),
start snajpagent:

```sh
snajpagent
```

New sessions start with their working directory at `~`. Ask the model to use
`get_cwd` to inspect it and `cd` to change it, or give an absolute file path.
File tools also accept explicit `./` paths relative to the current directory.
Describe a task and press Enter: “Fix the empty-input bug, keep the public API
unchanged, and run the tests.” The model can read and edit files and run
commands across the filesystem with your OS permissions. Tool calls require
no per-call approval. Empty Enter opens a fresh prompt, like a shell.

Read its replies and scroll back normally. Tool details are hidden by default;
`/verbose 1` shows compact activity and `/verbose 2` adds input/result previews,
with each tool row carrying the same short call reference and cut or hidden
content marked by a dim `[…]`. Type these commands and press Enter, even while
the model works; `/help` lists command syntax and keys, and brackets mark
optional arguments.

[![A local session reports fixing whitespace handling and passing four checks](www/screenshots/ordinary.png)](www/screenshots/ordinary.png)

Model output with tool details hidden.

### Correct this task or queue the next one

Your request and the work through its final answer make up a **turn**, which
appears in **rollout**. You can type while it runs; typing alone does not
interrupt it.

Each nonblank submission appears in scrollback immediately. The active prompt
appears as the turn starts, before the provider accepts a response, and stays
visible and editable through request preparation, retries and handoffs. Only a
foreground slash command hides it until that command finishes. Enter submits
a steer at the next safe boundary, including before provider acceptance. Blank or
whitespace-only Enter stays local and starts no turn, command or provider call.

**Enter sends a correction during a steerable provider request**: “Use the
existing parser; don't add a dependency.” It interrupts that response and
continues from the text already delivered; running commands stay alive while
the model waits for or stops them. Input submitted before the provider is ready
waits for the first safe boundary.

**Tab at the end of an ordinary message queues a follow-up while work is
active**, so “Then add regression coverage” waits for the current turn. Queued
prompts run oldest first, one at a time; `(N)` counts waiting turns, excluding
current work. The acknowledgement `queued (/next or /q c) ›` confirms
submission: `/next` resumes a paused queue, while `/q c` clears waiting prompts
without stopping current work.

Tab completes command names before it queues: `/sta` plus Tab gives `/status `,
and Enter runs it. Completion applies while the cursor is in or at the end of
that first `/command`; afterwards Tab follows the nickname-completion or
idle/active behavior below. A unique match adds a space, an ambiguous one
extends the common prefix and a second Tab lists choices, and no match leaves
the draft unchanged. Completion never sends or queues text. At queue dispatch
the prompt shows its dispatch-time clock and effective model settings, while its
durable provenance keeps the original receipt time; same-turn retries do not
print a duplicate submission.

### Commands, history and context

Commands can be entered while a turn is steerable. A foreground slash command
finishes before the next app command is accepted; input typed during it waits.
`/config`, `/model cache`, `/compact`, `/archive` and `/delete` acknowledge a
safe boundary when they must wait. Accepted controls survive resume. `/model`
and the model's `select_model` tool switch the next response in the current turn;
an already streaming request is interrupted and rebuilt from durable history.
The external `$EDITOR` owns terminal input while open, and deletion always
requires explicit confirmation.

Submitted slash commands remain in scrollback above their output. `/history`
shows the last turn (including unfinished work), `/history 10` the last ten and
`/history 0` only counts, while the header and footer report session and
shown/completed totals. These are conversation turns, separate from the Up/Ctrl-R
prompt-entry history.

Use `/cat src/app.c` to open a local file in `$PAGER` (or the configured
`[ui] pager` command). Relative paths use the current working directory; file contents
stay in the pager rather than the conversation.

Use `/ro QUERY` for a read-only query; during work it queues a separate read-only
turn without changing the current turn's permissions. Every request declares the
same native tool catalog. A read-only turn runs its permitted inspection calls
and returns a refusal for a state-changing call. `/yield` returns an active tool
wait to the model while leaving the process and its handle alive.

Command results include durable stdout/stderr references. The model can page
older bytes from the saved session without rerunning a command, including after
resume. It can also select an absolute executable shell for later commands in
that session; commands keep their exact bytes and must use that shell's syntax.

`/compact` reduces model context while retaining the full local log, reporting
progress, completion, waiting or interruption. Empty-draft Ctrl-C interrupts it,
text entered during idle compaction becomes future queued work, and a provider
error keeps the previous context and session available for a retry.

### Keep working, or leave and come back

Failed turns retry automatically five times; `[agent] max_turn_retries` changes
the limit (`0` disables ordinary automatic retries), and a successful actionable
response resets the consecutive-failure budget. Active goals retry ordinary
errors without a limit, while policy stops and refusals pause them.

A normal final answer ends the turn; set a goal to continue work beyond it:

```text
/goal fix the bug and validate the change
```

Goals continue until complete, paused, cancelled or blocked; queued prompts
come first. `/goal pause` pauses continuation at a turn boundary without
interrupting a running turn, `/goal resume` continues a paused or blocked goal,
and `/goal clear` cancels the goal while retaining its history. An active goal
retries errors with paced, interruptible waits, preserving completed work and
live command handles.

Ctrl-C clears a nonempty draft; with an empty draft it interrupts the turn and
pauses goal continuation. The next idle prompt clears the active goal flag, and
empty Enter leaves the goal paused (use `/goal resume`). Ordinary IRC updates
that were already pending stay pending at that boundary, including after exit
and resume; a direct mention remains eligible for urgent handling. Ctrl-D on an
empty
draft exits; no work continues after exit. Tools require operands and default
optional controls. Verbosity 1 shows rejected attempts as compact outcome rows;
argument errors identify corrections, and capped output reports requested and
applied limits. The model sees current tool definitions, runtime settings and
local display visibility, so hidden tools call for progress updates and fuller
traces reduce duplication. Decisions and outcomes stay explicit.

The conversation, tool results, queue and goal are saved as a **session**.
Resume continues in-progress work without a fresh prompt, including from
scripts: `snajpagent -e --resume SESSION_ID </dev/null`. Calls started without
results retain their original command and an explicit unknown outcome rather
than re-running blindly, and explicit cancellations and paused automatic work
stay stopped. After accepted work, normal exit prints its resume command;
preparing an attachment, `/dictate` and `/voice on` also retain the session,
while exiting an unused one saves nothing. You can also list sessions or reopen
the latest one for this project directory:

```sh
snajpagent -l
snajpagent --resume --last
```

**An active goal continues on resume**, so pause it before exiting to keep it
paused; paused, blocked and finished goals retain their states. Armed queues
continue after recovered work, while paused queues need `/next` and take priority
over goal work. Resume shows retained public history and continues unfinished
turns from saved input and tool results; commands with uncertain outcomes are
reported rather than restarted.
The versioned session journal stores state and provider-context checkpoints
inside `events.jsonl`. Resume validates the latest checkpoint and its recent
suffix. Older long journals need one initial migration scan, while explicit requests
for older history still read the relevant records. A corrupt checkpoint is
reported instead of triggering an unnoticed full-journal rebuild. Successful
compaction keeps its summary and uncovered continuation, not the covered
conversation or completed tool results.

### Attach files and use voice

Use `/attach PATH` to stage an image, inspect the staged list with
`/attachments`, then submit it with your prompt. Ask the agent to inspect PDF,
Office or text documents, sample a video interval, or transcribe an audio file;
accepted originals and prepared results stay with the saved session.

`/dictate` inserts speech into your editable draft. `/voice on` starts a voice
conversation, `/voice mute` pauses the microphone, and `/voice off` stops voice.
These commands use the selected provider and its credentials: a Codex subscription,
codex-lb, or a compatible BYOK provider. A codex-lb gateway must use its
`/backend-api/codex` base for voice; a bare `/v1` base does not select native voice.
Use a headset for duplex voice.
`/play asset:ID` plays a saved audio asset. The
[manual](https://agent.snajpa.net/manual.html) covers provider setup, selectors,
data destinations and capture controls.

### Keep useful findings in files

For longer work, have the model keep findings, decisions and corrections in
project files pointed to from `AGENTS.md`, kept current so later tasks reuse what
was learned. Record proposals and approvals separately.

`-d DIR` adds a documentation root containing `AGENTS.md` or
`AGENTS.override.md`; repeat it for several roots, including notes spanning
repositories. Relative paths use the launch directory even with `-C`, and the
printed resume command retains them.

## 2. Work together

People and agents share an IRC room: one snajpagent instance hosts it and others
connect. Run these in separate terminals, from the project each model should use:

```sh
snajpagent -s -n builder -o alice -r work
snajpagent -c -n reviewer -o bob
```

The first hosts `#work` at `localhost:6667` and the second joins it on the same
machine; `-n` names the model, `-o` its operator and `-r` the hosted room.
`/names` shows accepted names, where a name already in use gets a suffix.

### Choose who receives your message

Networked startup opens **chat**, the shared room, where Enter sends as your
operator name. `@builder check the empty-input case` asks builder to work, and a
mention during its work steers it at a safe boundary without cutting off its
current response; ordinary conversation supplies background context, while a
direct mention starts a task for the addressed model.

Empty Tab switches to **rollout**, where Enter directs your local agent without
sending your instruction to everyone; each view keeps its own draft and history,
and the local transcript stays in rollout. Models use `irc_send` to publish
chosen messages, which can include material from that transcript.

In chat, `@bu` plus Tab becomes `@builder ` when that is the only match, including
the space. `@` begins a nickname word anywhere in a message, so `please ask @bu`
also completes while bare `bu` does not.

At the end of the finished message, **Enter sends to the room**, while **Tab
queues it as a local follow-up while your model is active**, even in chat; with
the cursor still at the end of `@bu` or `@builder`, Tab completes that name
first.

### Coordinate work

Give agents tasks in the room and they exchange messages and report results. Use
project files and Git for code and handoff
notes, since joining IRC shares no files, credentials or command processes, and
use separate Git worktrees for independent edits to one repository.

`/server start` hosts a room and `/connect ENDPOINT` adds a connection; `/names`
lists rooms and members. `/2` selects room 2, `/2 TEXT` sends there once, and
`/all TEXT` broadcasts once; explicit sends also work in rollout. `/status` shows
whether a requested connection has joined. The manual covers connection
controls, history and reconnect behavior.

**IRC has no authentication or TLS.** Use localhost, a trusted network, or a
secure tunnel. Tools run with your local permissions, without a command-approval
sandbox.

## Further controls

`/help` lists commands and editing keys; `/status` shows the current state.
Ctrl-J inserts a newline, Up/Down move through draft rows and then prompt
history, Ctrl-P/Ctrl-N step through history, and Ctrl-R searches it; navigation
visits the session's own entries before the global archive. `/queue` shows
waiting work, `/queue 2 edit` revises its second item and `/queue 2 delete`
removes it. The manual covers editing keys, history, search, the queue editor
and slash-command exceptions.

`/model` lists the locally cached catalog and `/model cache` refreshes
providers; select a row by number or with `/model PROVIDER/MODEL/EFFORT`. Both
An active `/model` switches the next response in the same turn; an idle
selection and CLI `-m` set the preference for subsequent requests. The choice
persists across resume; add `save` to write it into the configuration file. A model
change alone does not compact. If the selected model needs a smaller context,
compaction runs through that model in bounded chunks. Exiting immediately after
the selection, or after a failed or cancelled turn, keeps both the selection
and completed tool results for resume. When a catalog advertises both a normal
working window and a larger maximum context, the normal window is the
hard-capacity basis; an explicit model-limit context selects a larger value when
wanted. A source that publishes only a maximum still uses it. The normal window
is the provider's default, and requests above it can move to a higher price tier
on routes that bill large input separately.
`/context` sets the window for the session: `default` uses the advertised
normal window, `max` the advertised maximum, and a number an explicit token
count; the output reservation and compaction budget stay derived. The choice is
recorded in the session log and restored on resume; during active work it ends
the current response and rebuilds the turn under the new window.
Model-limit rules can supply `reasoning_efforts = ["max", "high", "low", "none"]`
when a provider omits effort choices, or an optional `image_tokens` value for a
provider-documented per-image ceiling that differs from the built-in
resized-image budget; the manual covers precedence and selection.

The prompt's context percentage compares the last measured request input with
the resolved input budget, from provider-reported counts. A fresh session starts
at `0%` and unknown measurements show `?%`; `/status` explains the accounting.
Older context is compacted into a summary as it fills, or on `/compact`; the
original transcript stays on disk, but keep important requirements in project
documents because a summary omits detail.

For inspection without commands or edits, use `/ro QUERY`: it can list, read and
search files and use provider-hosted web search, but cannot run commands, patch
files, change goals or send IRC messages. `/queue /ro QUERY` asks it next during
active work.

### Restrict what the model may do

Configuration can filter model tool calls before they run. Add `[rule NAME]`
sections; they are validated at load, so a typo fails startup instead of
becoming silent policy.

```ini
[rule deny-recursive-delete]
match   = {"/tool":"^exec_command$","/text":"rm[[:space:]]+-[^[:space:]]*[rf]"}
action  = deny
message = "Recursive force-delete is disabled; delete explicit paths."
```

A rejected call is reported to the model as not run, never quietly dropped.
The first matching rule decides; a trailing match-all `allow` audits the whole
session without changing any verdict. **This is filtering, not a sandbox**:
a regular expression over a command is not confinement, so use read-only turns
and separate accounts for real isolation. `design/io-rules.md` has the full
syntax, worked examples and the 0.99.7 migration table; ready-made policies
live in `examples/io-rules/`.

## Install and choose a provider

[Choose an executable](https://agent.snajpa.net/downloads.html) for your OS,
architecture and listed ABI, and rename it to `snajpagent` (`snajpagent.exe` on
Windows). Compare its SHA-256 with the download row or `SHA256SUMS` **before
running it**; for an archive, verify the archive before extraction, as its
hash covers the archive, not the executable inside. Keep the program in a
user-owned directory; debug builds and production symbols are separate.

### Install on Linux

Choose the matching CPU/kernel variant:

```sh
sha256sum ./snajpagent
chmod +x ./snajpagent
./snajpagent
```

### Install on macOS

Choose Apple Silicon, Intel or universal (macOS 11 or later):

```sh
shasum -a 256 ./snajpagent
chmod +x ./snajpagent
./snajpagent
```

These experimental downloads are not Developer ID signed or notarized. If macOS
blocks a verified file you trust, clear the quarantine attribute with
`xattr -d com.apple.quarantine ./snajpagent` and launch again, keeping system-wide
protections enabled.

### Install on Windows

Choose experimental x64 or ARM64; in PowerShell:

```powershell
Get-FileHash .\snajpagent.exe -Algorithm SHA256
.\snajpagent.exe
```

In Command Prompt, use `certutil -hashfile .\snajpagent.exe SHA256` where
available, then `.\snajpagent.exe`. Keep the `.exe` extension. Add its folder to
your user PATH or invoke the full path from your project directory; PowerShell
uses `& "C:\path\snajpagent.exe"` for a quoted path.

### Install on FreeBSD

FreeBSD 5.1/5.5 needs the legacy amd64 variant:

```sh
sha256 -q ./snajpagent
chmod +x ./snajpagent
./snajpagent
```

### Install on OpenBSD

Choose amd64 for OpenBSD 7.9, 5.9 or 3.5; `sha256` where available:

```sh
sha256 -q ./snajpagent
chmod +x ./snajpagent
./snajpagent
```

### Install on NetBSD

Choose amd64 for 10.1 or the separate 2.0/5.2.3 legacy pthread ABI; on 5.2.3
and 10.1:

```sh
cksum -a SHA256 ./snajpagent
chmod +x ./snajpagent
./snajpagent
```

NetBSD 2.0 lacks SHA-256 in base `cksum`; on a legacy system without one, verify
on a trusted newer machine and transfer over a trusted channel.

### Installation paths, updates and source builds

On Linux, macOS and the BSDs, place the verified executable in `$HOME/.local/bin`,
add that directory to PATH, and start it from your
project directory; the [manual](https://agent.snajpa.net/manual.html#Getting_started)
has complete user-local installation, ABI requirements and startup troubleshooting.
Android remains experimental source work without a production download target.

Official stable binaries check and install updates in the background on launch
while the current process keeps running; one banner links to the release log and
asks you to restart when convenient, and `[agent] auto_update = false` opts out.
Development binaries are debug builds that default to updates off, following
`latest-dev` when set to `true`. Ordinary source builds remain updater-free; the
manual covers publisher URLs, permissions and recovery.

`./configure` probes the toolchain and the four optional modalities and tunes the
tracked `config.mk`; `make WITH_*=…` stays an explicit override.

The POSIX build needs C11 with pthreads, GNU make, pkg-config and
libcurl/Jansson development files. On the BSDs, install GNU make and use `gmake`
throughout. On Linux and macOS:

```sh
git clone https://github.com/snajpa/snajpagent.git
cd snajpagent
./configure
make
make PREFIX="$HOME/.local" install
```

This installs the binary and manual under `$HOME/.local`; the default prefix is
`/usr/local`, which usually needs administrator rights. Production needs
`strip` and `objcopy` on ELF systems, or `strip` and `dsymutil` on macOS.
`make DEBUG=1` builds for debugging; `make help` lists build options, and
[dependency notes](DEPENDENCIES.md) cover platform scope.

`make prod-matrix` builds all implemented standalone targets into
`build/matrix/` with host-load and available-memory bounded parallelism,
without installation or VMs; the
[platform notes](DEPENDENCIES.md) cover target requirements.
`make prod-linux-armv6` builds a hard-float static PIE for ARMv6 Raspberry Pi
1/Zero-class systems and ARMv7 with an ARMv6KZ/VFPv2 baseline;
`make prod-linux-riscv64` a RISC-V RV64GC/LP64D static PIE;
`make prod-linux-ppc64le` a little-endian POWER8 ELFv2 static PIE;
`make prod-linux-i686` a modern 32-bit Linux static executable, linked non-PIE; and
`make prod-linux-i686-legacy` a static non-PIE executable for Linux 2.4.27 with
embedded TLS, roots and locale data. The legacy build needs working procfs and
secure OS entropy; see [platform limits](DEPENDENCIES.md).

An opt-in `make prod-linux-ppc32` recipe remains for unsupported PowerPC 32-bit
experiments; it is outside the production matrix and has no stable release
download or update channel.

For experimental Windows x64 or ARM64, use `make prod-windows-x86_64` or
`make prod-windows-arm64` with pinned Nix dependencies, and copy the resulting
`build/matrix/windows-ARCH/bin/snajpagent.exe` to Windows; application libraries
and CA roots are included without third-party runtime DLLs.

For experimental NetBSD amd64 source builds, `make prod-netbsd-amd64` targets
10.1 and `make prod-netbsd-amd64-legacy` targets 2.0 and 5.2.3, with separate
native pthread ABIs; see the [platform notes](DEPENDENCIES.md). Plain `make`
builds only the host platform.

Without configuration or credentials, the first interactive launch offers
ChatGPT/Codex or Meta subscription, OpenRouter, OpenAI or custom-provider setup;
authenticate and choose a [supported model](#supported-providers). `snajpagent login status`
reports local credential sources without contacting a provider, and the manual
explains login methods and logout.

For manual configuration on POSIX systems, create a private directory:

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

Export `OPENAI_API_KEY` before launch; provider names are local labels. `/config`
opens the active file in `$EDITOR` and reloads valid changes, and an invalid
edit leaves the previous configuration active until you fix the file and restart.

On Windows, setup uses `HOME/.snajpagent`, falling back to
`USERPROFILE/.snajpagent`, and `--dotdir DIR` overrides it; the same `config.ini`
works there. For credentials, PowerShell uses
`$env:OPENAI_API_KEY = 'your-key'` and Command Prompt
`set "OPENAI_API_KEY=your-key"`; masked-key entry during setup avoids shell
history. The default Windows tool shell is `cmd.exe`, independently of the
launching shell, and POSIX examples need a POSIX shell or adaptation.

## Use it in scripts

One-shot mode runs a prompt without an interactive conversation:

```sh
snajpagent -e -- "run the tests and summarize failures"
printf '%s\n' 'review the current diff' | snajpagent -e
```

Model text goes to stdout, diagnostics and the resume hint to stderr, and
redirected output has no styling. Use exit status for failure handling; the
pre-1.0 event-log format can change between versions.

The [user manual](https://agent.snajpa.net/manual.html) and `man snajpagent`
cover the full reference and troubleshooting, both generated from
[one source](snajpagent.1). [Design notes](design/architecture.md) cover the
implementation. GPL-2.0-only; see [COPYING](COPYING).

## Supported providers

Supported connections: OpenAI, ChatGPT/Codex and Meta subscriptions, OpenRouter, and custom
providers with an OpenAI-compatible Responses API.

- **OpenAI GPT-5+:** recommended; the only model family thoroughly tested with
  snajpagent.
- **DeepSeek:** an open-model option. V4 Pro supports direct Responses streaming
  with thinking, for example `-m deepseek/deepseek-v4-pro/high` after configuring
  a provider named `deepseek`; reasoning continuity survives tool calls and resume,
  and provider-private state stays with its originating model and account.
- **Anthropic:** not supported; the project's assessment is that alignment
  problems make its models unsuitable for long-horizon autonomous work without
  oversight.
- **Other models:** use at your own risk. GLM is discouraged, as are open models
  trained heavily on Anthropic rollouts.
