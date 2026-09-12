<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent

A coding agent for your terminal, with a built-in IRC server and client.

[Website](https://agent.snajpa.net) ·
[Downloads](https://agent.snajpa.net/downloads.html) ·
[Install](#install-and-choose-a-provider) ·
[User manual](https://agent.snajpa.net/manual.html)

This guide and the web manual describe current source. Downloaded releases ship
with their own matching manual. Check `snajpagent -V` when behavior differs;
rebuilding or updating an executable affects new launches, not an open session.

## 1. Work on a project

After [installing and choosing a provider](#install-and-choose-a-provider),
start in the project you want to change:

```sh
cd /path/to/your/project
snajpagent
```

Describe a task and press Enter: “Fix the empty-input bug, keep the public API
unchanged, and run the tests.” The model can read and edit files and run commands.
Empty Enter leaves a prompt line and opens a fresh prompt, like a shell.

Read its replies and scroll back normally. Tool details are hidden by default;
`/verbose 1` shows compact activity and `/verbose 2` adds input/result previews.
Type these commands and press Enter, even while the model works. `/help` lists
command syntax and keys; brackets mark optional arguments.

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
prefix and a second Tab lists choices. No match leaves the draft unchanged. Completion never sends or queues text.
At actual queue dispatch, the prompt appears with its dispatch-time clock and
effective model settings. Its durable provenance keeps the original receipt time;
same-turn retries do not print a duplicate submission.

Outside completion, Tab inserts spaces while idle. **Empty Tab switches between
rollout and chat.** Nickname completion in chat is explained below.

### Commands, history and context

Every command can be entered while a turn is active. Inspection and presentation
commands act immediately; commands such as `/config`, `/model cache`, `/compact`,
`/archive` and `/delete` acknowledge a safe boundary when they must wait. Accepted
controls in an established session survive resume. Model and effort selections
apply to the next full turn. The external `$EDITOR` owns terminal input while it
is open, and deletion always requires explicit confirmation.

Submitted slash commands remain in scrollback above their output. `/history`
shows the last turn, including unfinished work; `/history 10` shows the last ten.
The opening header gives total session turns and completed turns, and the footer
reports shown, completed and total counts. `/history 0` shows only counts.
These are conversation turns, separate from the Up/Ctrl-R prompt-entry history.

Use `/ro QUERY` for a read-only query. During work it queues a separate read-only
turn; it does not change the current turn's permissions. `/yield` returns an
active tool wait to the model while leaving the process and its handle alive.

`/compact` reduces model context while retaining the full local log. It reports
progress, completion, waiting or interruption. Empty-draft Ctrl-C interrupts it;
text entered during idle compaction becomes future queued work. A provider error
keeps the previous context and session available so `/compact` can be retried.

### Keep working, or leave and come back

Failed turns retry automatically five times. Set `[agent] max_turn_retries`
to change the limit (`0` disables ordinary automatic retries). A successful
actionable response resets the consecutive-failure budget. Active goals retry ordinary errors without a limit; policy stops and refusals pause them.

A normal final answer ends the turn. Set a goal when you want work to continue
beyond it:

```text
/goal fix the bug and validate the change
```

Goals continue until complete, paused, cancelled, or blocked. Queued prompts
come first. `/goal pause` pauses continuation at a turn boundary; it does not
interrupt a running turn. `/goal resume` continues a paused or blocked goal;
`/goal clear` cancels the goal and stops automatic continuation while retaining
its history. Errors keep an active goal retrying with paced,
interruptible waits, preserving completed work and live command handles.

Ctrl-C clears a nonempty draft; with an empty draft, it interrupts the turn and
pauses automatic goal continuation. The next idle prompt clears the active goal
flag. Empty Enter leaves the goal paused; use `/goal resume` to restart it.
Ctrl-D on an empty draft exits. No work continues after the program exits.
Tool argument errors identify the correction, and capped output reports requested
and applied limits. The model receives current tool definitions, runtime settings and local display
visibility: hidden tools call for meaningful progress updates; fuller traces
reduce duplicate narration. Decisions and outcomes remain explicit.

The conversation, tool results, queue and goal are saved as a **session**.
After accepted work or other retained session state, normal exit prints its
resume command. Exiting an unused session creates no saved session. You can also list sessions or reopen the
latest one for this project directory:

```sh
snajpagent -l
snajpagent --resume --last
```

**An active goal continues on resume.** Pause it
before exiting if you want it to stay paused. Paused, blocked and finished goals
retain their states. Armed queues continue after recovered work; paused queues
need `/next` and take priority over automatic goal work. Resume shows retained public
history and continues unfinished turns from saved input and tool results. Commands
with uncertain outcomes are reported honestly rather than restarted automatically.

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
History navigation and Ctrl-R visit the session's own entries first, then the
global archive. New submissions stay local until orderly exit, when only those
new entries are appended globally. Other running sessions can recall them on a
later navigation or search. Neither archive is copied into memory or pruned.
`/queue` shows waiting work; `/queue 2 edit` revises its second item and
`/queue 2 delete` removes it. The manual covers the queue editor and slash-command
exceptions.

`/model` lists the locally cached catalog; `/model cache` explicitly refreshes
every configured provider. Select a displayed row by number, or use `/model PROVIDER/MODEL/EFFORT`.
Both `/model` and `-m` select from the next full turn onward, until changed,
including across session resume. Add `save` to write the selection to the
configuration file for new sessions.

The prompt's context percentage shows the last measured request input against
the resolved input budget, rounded up. It uses provider-reported input counts. A fresh
session starts at `0%`; after a turn, unknown or incomparable measurements show
`?%`. `/status` explains the accounting. Older context is automatically
compacted into a summary as it fills, or request `/compact` during work or at idle. The original
transcript stays on disk, but a summary does not preserve every detail—keep
important requirements in project documents.

For inspection without commands or edits, use `/ro QUERY`. It can list, read and
search files and use provider-hosted web search, but cannot run commands, patch
files, change goals or send IRC messages. `/queue /ro QUERY` asks it next during
active work. Readable local files remain accessible and session history is recorded.

## Install and choose a provider

[Choose an executable](https://agent.snajpa.net/downloads.html) for your OS,
architecture and listed ABI. Rename it to `snajpagent` (`snajpagent.exe` on
Windows). Compare its SHA-256 with the download row or `SHA256SUMS` **before
running it**. For an archive, verify the archive before extraction; its hash
belongs to the archive, not the executable inside. Keep the program in a
user-owned directory. Debug builds and production symbols are separate.

### Install on Linux

Choose the matching CPU/kernel variant, including modern or legacy i686:

```sh
sha256sum ./snajpagent
chmod +x ./snajpagent
./snajpagent
```

### Install on macOS

Choose Apple Silicon, Intel or universal for macOS 11 or later:

```sh
shasum -a 256 ./snajpagent
chmod +x ./snajpagent
./snajpagent
```

These experimental downloads are not Developer ID signed or notarized. If macOS
blocks a verified file that you trust, use `xattr -d com.apple.quarantine ./snajpagent`
and launch again. Keep system-wide protections enabled.

### Install on Windows

Choose experimental x64 or ARM64. In PowerShell:

```powershell
Get-FileHash .\snajpagent.exe -Algorithm SHA256
.\snajpagent.exe
```

In Command Prompt, use `certutil -hashfile .\snajpagent.exe SHA256` where
available, then `.\snajpagent.exe`. Keep the `.exe` extension. Add its folder to
your user PATH, or invoke the full path after changing to your project directory.
PowerShell uses `& "C:\path\snajpagent.exe"` for a quoted executable path.

### Install on FreeBSD

FreeBSD 5.1/5.5 needs the legacy amd64 variant:

```sh
sha256 -q ./snajpagent
chmod +x ./snajpagent
./snajpagent
```

### Install on OpenBSD

Choose amd64 for OpenBSD 7.9, 5.9 or 3.5. Use `sha256` where available:

```sh
sha256 -q ./snajpagent
chmod +x ./snajpagent
./snajpagent
```

### Install on NetBSD

Choose amd64 for 10.1 or the separate 2.0/5.2.3 legacy pthread ABI.
On 5.2.3 and 10.1:

```sh
cksum -a SHA256 ./snajpagent
chmod +x ./snajpagent
./snajpagent
```

NetBSD 2.0 lacks SHA-256 in base `cksum`. If a legacy system lacks a SHA-256
utility, verify on a trusted newer machine and transfer over a trusted channel.

### Installation paths, updates and source builds

On Linux, macOS and the BSDs, place the verified executable in `$HOME/.local/bin`
and add that directory to PATH in your shell startup file. Start it from your
project directory. The [manual](https://agent.snajpa.net/manual.html#Getting_started)
has complete user-local installation, ABI requirements and startup troubleshooting.
Android remains experimental source work without a production download target.

Official stable binaries check and install updates in the background on launch.
The current process keeps running; one banner links to the release log and asks
you to restart when convenient. Set `[agent] auto_update = false` to opt out.
Development binaries are debug builds and default to updates off; set the option
to `true` to follow `latest-dev`. Ordinary source builds remain updater-free.
The manual covers publisher URLs, permissions and recovery.

The normal POSIX build needs C11 with pthreads, GNU make, pkg-config, and
libcurl/Jansson development files. On the BSDs, install GNU make and use `gmake`
in place of `make` throughout. On Linux and macOS:

```sh
git clone https://github.com/snajpa/snajpagent.git
cd snajpagent
make
make PREFIX="$HOME/.local" install
```

This installs the binary and manual under `$HOME/.local`; the default prefix
is `/usr/local`, which usually requires administrator privileges. Production needs
`strip` and `objcopy` on ELF systems, or `strip` and `dsymutil` on macOS. `make DEBUG=1` builds
for debugging; `make help` lists build options. See [dependency notes](DEPENDENCIES.md)
for platform scope.

`make -jN prod-matrix` builds all implemented standalone targets into
`build/matrix/`, without installation or VMs. See the
[platform notes](DEPENDENCIES.md) for target-specific requirements.
`make prod-linux-armv6` builds one hard-float static PIE for ARMv6 Raspberry Pi
1/Zero-class systems and ARMv7, with an ARMv6KZ/VFPv2 baseline.
`make prod-linux-riscv64` builds a RISC-V RV64GC/LP64D static PIE.
`make prod-linux-ppc64le` builds a little-endian POWER8 ELFv2 static PIE.
`make prod-linux-ppc32` builds a separate 32-bit big-endian PowerPC static PIE.
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

Export `OPENAI_API_KEY` in your shell before launch. Provider names are local
labels. `/config` opens the active file in `$EDITOR` and reloads valid changes.
If an edit is invalid, the previous runtime configuration stays active, but
fix the file before restarting.

On Windows, setup uses `HOME/.snajpagent`, falling back to
`USERPROFILE/.snajpagent`; `--dotdir DIR` overrides it. The same `config.ini`
works there. For credentials, PowerShell uses
`$env:OPENAI_API_KEY = 'your-key'`; Command Prompt uses
`set "OPENAI_API_KEY=your-key"`. Masked-key entry during setup avoids shell
history. The agent's default Windows tool shell is `cmd.exe`, independently of
its launching shell. POSIX examples need a POSIX shell or adaptation.

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
- **DeepSeek:** an open-model option. V4 Pro supports direct Responses streaming
  with thinking, for example `-m deepseek/deepseek-v4-pro/high` after configuring
  a provider named `deepseek`. Reasoning continuity is preserved across tool calls
  and resume; provider-private state stays with its originating model/account.
- **Anthropic:** not supported. The project's assessment is that alignment
  problems make its models unsuitable for long-horizon autonomous work without
  oversight.
- **Other models:** use at your own risk. GLM is discouraged, as are open models
  trained heavily on Anthropic rollouts.
