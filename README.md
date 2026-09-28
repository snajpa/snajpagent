<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent

A terminal harness for autonomous, long-horizon work and distributed agent teams.

Give a model an objective, tools and project context. Persistent goals carry work
across turns; steering, saved sessions and retained tool results keep it under
your direction. Built-in IRC connects people and agents across machines and
rooms, with each instance running tools in its own environment.

Model-owned notes in your repositories carry findings, decisions and unfinished
work between sessions. Sessions start in your home directory and discover its
`AGENTS.md` instructions, where you can point the model to that shared memory.

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

Describe the outcome, constraints and repository, then press Enter. Use a
persistent goal for work spanning multiple turns: implementation, testing and
follow-through continue until the goal is complete or blocked.

New sessions start at `~`. The model manages its working directory and can read
and edit files and run commands across the filesystem with your OS permissions,
without per-call approval. Absolute paths and explicit `./` paths are supported.
Empty Enter opens a fresh prompt, like a shell.

Read its replies and scroll back normally. Tool details are hidden by default;
`/verbose 1` shows compact activity and `/verbose 2` adds input/result previews,
with short call references and omitted content marked `[…]`. `/help` lists
commands and keys; enter commands even while the model works.

[![A local session reports fixing whitespace handling and passing four checks](www/screenshots/ordinary.png)](www/screenshots/ordinary.png)

Model output with tool details hidden.

### Correct this task or queue the next one

Your request and the work through its final answer make up a **turn**, which
appears in **rollout**. You can type while it runs; typing alone does not
interrupt it.

**Enter sends a correction during active work**: “Use the existing parser;
don't add a dependency.” The steer remains visible and is admitted at a safe
boundary, continuing from delivered text while running commands stay alive.
The active prompt remains editable through request preparation and retries.
Blank Enter starts no work.

**Tab at the end of an ordinary message queues a follow-up while work is
active.** Waiting prompts run oldest first; `(N)` counts them. `/next` resumes
a paused queue, and `/q c` clears waiting prompts without stopping current work.
Tab first completes a `/command` name: `/sta` becomes `/status `; a second Tab
lists ambiguous choices. Completion never submits text.

### Commands, history and context

Commands remain available while work is steerable. Foreground commands own
input until they finish; later input waits. Controls needing a safe boundary
acknowledge it. The external `$EDITOR` owns terminal input while open, and
session deletion requires confirmation.

`/history` shows the last turn, `/history 10` the last ten, and `/history 0`
counts. Up/Ctrl-R navigate prompt-entry history separately. `/cat src/app.c`
opens a file in `$PAGER` without adding its contents to the conversation.

Use `/ro QUERY` for inspection without commands, edits, goal changes or IRC
sends. During work it queues a separate read-only turn. `/yield` returns a tool
wait to the model while preserving its process and handle. Durable command
output remains pageable after resume without rerunning commands.

`/compact` summarizes model context while retaining the full local log. It
reports progress and interruptions; failed compaction preserves the previous
context. Unsettled command handles must be collected before a final answer.

### Keep working, or leave and come back

Failed turns retry five times by default; `[agent] max_turn_retries` changes
this. Goals retry ordinary errors without a limit, while policy stops pause them.

A normal final answer ends the turn; set a goal to continue work beyond it:

```text
/goal fix the bug and validate the change
```

Goals continue until complete or blocked; queued prompts come first. `/goal pause`
pauses at a turn boundary, `/goal resume` continues paused or blocked work, and
`/goal clear` cancels while retaining history.

Ctrl-C clears a nonempty draft; with an empty draft it interrupts work and
pauses goal continuation. Empty Enter leaves the goal paused; use `/goal resume`.
Ctrl-D on an empty draft exits, and no work continues after exit. Pending room
updates remain available for later admission; direct mentions remain urgent.

The conversation, tools, queue and goal are saved as a **session**. Resume
continues unfinished work; started calls without results are reported as
unknown rather than blindly repeated. Linux sessions started on ANSI terminals
at least 20 columns wide are saved at startup and continue through terminal loss.
Use `/s d` to detach, then
`snajpagent --attach SESSION_ID` (or `-A`) to reconnect with the retained draft.
Without an ID, attach offers a running-session picker. `/s a ID` switches
between live sessions; a failed switch keeps the source. Normal exit
prints its resume command. An unfinished terminal file transfer ends when its
controlling terminal disconnects; queued downloads retain their saved state.
New editors and pagers use the replacement terminal's profile; a running
external program keeps its launch environment.
Local voice, dictation and playback stop on terminal detach, loss or suspension;
restart them explicitly after reconnecting.
List sessions or reopen the latest one:

```sh
snajpagent -l
snajpagent --resume --last
```

**Active goals continue on resume**; pause before exiting to keep one paused.
Armed queues run before goal work, while paused queues need `/next`. Resume
retains public history and completed tool results. Keep important requirements
in project files alongside the session log.

### Transfer files through the terminal

Wrap your connection with the native workstation client:

```sh
snajpagent remote ssh -t target snajpagent
# Reattach a running agent session:
snajpagent remote ssh -t target snajpagent --attach SESSION_ID
```

Use `/session detach` in the agent to return to the shell while work continues.
Omit `SESSION_ID` from the attach command to choose a running session.

`remote` passes a generic child command and its arguments literally through a
PTY, without starting a local agent session. Drop one workstation regular file
into the POSIX agent's composer, or use `/receive` and enter its path. Native
drops require current binaries at both ends and preserve the draft and active
work. Verified files become unsent attachments.
Review `/attachments` before submitting. Uploads reject directories and empty files.

Use `/send PATH` to send an agent file to the workstation, or ask the model to
use `send_file`. Downloads accept empty files and accepted `asset:ID` references.
Transfers keep the conversation visible; the native wrapper shows a compact progress line.
Native downloads default to `~/Downloads`; set `[terminal] download_dir` in the
workstation's `~/.snajpagent/config.ini` to change it. Receipts list actual saved
paths, and transfers restore the prompt and editable draft.

Model sends while detached queue durable exports in the existing remote session.
Wrapped reattachment delivers them at an idle boundary. Changed sources and
uncertain transfers remain pending; ask the model to list, remove or clear them
with `download_queue`. Source and completed local files are preserved.

The trzsz-go wrapper remains an alternative for explicit `/receive` and
`/send` commands: on macOS, install `trzsz-go` with Homebrew and connect using
`trzsz --dragfile ssh target`. Its `~/.trzsz.conf` `DefaultDownloadPath` controls
saving. Go-client stock drag sends Ctrl-C first, which can cancel a draft or work.
See the manual's **Terminal file transfers**, **Remote terminal mode (snajpagent remote)**
and **Model tools** sections for setup, recovery and the complete tool catalog.
Native wrapper, downloads and outbox are development-source features; the
0.99.8b stable binary supports uploads.

### Attach files and use voice

Use `/attach PATH` to stage an image or file, inspect the staged list with
`/attachments`, then submit it with your prompt. Ask the agent to inspect PDF, Office or text documents, sample a video interval,
or transcribe an audio file; accepted originals and prepared results stay with
the saved session.

`/dictate` inserts speech into your editable draft. `/voice on` starts a voice
conversation, `/voice mute` pauses the microphone, and `/voice off` stops voice.
You can continue speaking and submit another request while coding work runs;
accepted requests use the same session queue and receive their own results.
These commands use the selected provider and its credentials: a Codex subscription,
codex-lb, or a compatible BYOK provider. A codex-lb gateway must use its
`/backend-api/codex` base for voice; a bare `/v1` base does not select native voice.
Use a headset for duplex voice. Voice follows the configured HTTP/HTTPS route;
use HTTPS or a secure tunnel outside a trusted network.
`/play asset:ID` plays a saved audio asset. The
[manual](https://agent.snajpa.net/manual.html) covers provider setup, selectors,
data destinations and capture controls.

### Keep useful findings in files

Project notes and `AGENTS.md` carry findings across sessions. Keep decisions
current and proposals separate from approvals. `-d DIR` adds a documentation
root; repeat it for shared or cross-repository notes. Relative documentation
paths use the launch directory and persist in the printed resume command.

## 2. Work together

Run agents where their repositories, tools and services are available, and use
IRC rooms to assign work, exchange findings and coordinate handoffs. One instance
hosts a room and others connect; an instance can join several endpoints at once.

Start two instances in separate terminals:

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

Empty Tab cycles through **rollout** and every connected room. In rollout,
Enter directs your local agent; in chat, Enter sends to the selected room.
Rollout and chat retain separate drafts and history. The working transcript
stays in rollout; models use `irc_send` to publish chosen messages, which can
include material from that transcript.

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

`/server start` hosts a room and `/connect ENDPOINT` adds a connection to that
endpoint's advertised room; repeat it for other endpoints. `/names` lists rooms
and members. `/2` selects room 2, `/2 TEXT` sends there once, and
`/all TEXT` broadcasts once; explicit sends also work in rollout. `/status` shows
whether a requested connection has joined. The manual covers connection
controls, history and reconnect behavior.

**IRC has no authentication or TLS.** Use localhost, a trusted network, or a
secure tunnel. Tools run with your local permissions, without a command-approval
sandbox.

## Further controls

`/help` lists commands and editing keys; `/status` shows the current state.
Ctrl-J inserts a newline, Up/Down navigate the draft and prompt history, and
Ctrl-R searches history. `/queue` lists waiting work; its editor revises or
removes entries. The manual covers editing keys and slash-command exceptions.

`/model` selects the next response's provider, model and effort; `/model cache`
refreshes the catalog. Selection persists across resume. Model-callable switching
is available with `[agent] allow_model_change=true` (default off). Switching
retains completed tool results and running commands; a smaller context triggers
bounded compaction when needed. The manual covers saved defaults and effort rules.

`/context default` uses the advertised normal window, `max` the maximum, and a
number an explicit token count. Larger windows may change provider pricing.
The prompt's percentage shows measured input against the resolved budget;
`?%` means unknown. `/status` explains accounting, and `/compact` summarizes
older context while preserving the transcript on disk. The manual covers
model-limit rules, effort choices and context changes during active work.

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
An approved letter-suffixed stable release such as `0.99.8b` follows its
numeric base in updater ordering and stays on the stable `latest` channel.

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
