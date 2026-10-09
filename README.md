<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent

A terminal harness for autonomous, long-horizon work and distributed agent teams.

Goals and saved sessions carry work across turns. IRC connects agents across
machines; each runs tools locally. Point your home directory's `AGENTS.md` to
project notes that retain findings and unfinished work.

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

Describe the outcome, constraints and repository; press Enter. A persistent
goal carries implementation, testing and follow-through across turns until
complete or blocked.

New sessions start at `~`. The model manages its working directory and can read
and edit files and run commands across the filesystem with your OS permissions,
without per-call approval. Absolute paths and explicit `./` paths are supported.
Empty Enter opens a fresh prompt, like a shell.

Scroll back normally. Tool details start hidden: `/verbose 1` shows activity,
`/verbose 2` adds input/result previews and call references; `[…]` marks omissions.
`/help` lists commands/keys, available even during work.

[![A local session reports fixing whitespace handling and passing four checks](www/screenshots/ordinary.png)](www/screenshots/ordinary.png)

### Correct this task or queue the next one

A **turn** runs from your request through the final answer in **rollout**. Type
while it runs; Enter sends a correction at a safe boundary and keeps running
commands alive. Blank Enter starts no work.
If the model replies while a command is running, the turn waits for your input
or command completion. Output keeps buffering; `/yield` returns to the model.

Tab at the end of an ordinary message queues a follow-up while work is active.
Waiting prompts run oldest first; `(N)` counts them. `/next` resumes a paused
queue, and `/q c` clears it. Tab first completes `/command` names; completion
never submits text.

### Commands, history and context

Foreground commands and `$EDITOR` own input until finished. Controls acknowledge
safe-boundary waits; session deletion requires confirmation.

`/history` shows the last turn, `/history 10` the last ten, and `/history 0`
counts. Up/Ctrl-R navigate prompt history. `/cat PATH` opens a file in `$PAGER`
without adding it to the conversation.

`/ro QUERY` queues inspection without commands, edits, goal changes or IRC sends.
`/yield` returns a tool wait to the model while preserving its process.
Command output remains pageable after resume. `/compact` summarizes context
while retaining the full log; failure preserves previous context.

### Keep working, or leave and come back

Automatic retry is on by default. `/retry auto` toggles it for the session;
`/retry auto on|off` sets it explicitly. `[agent] retry_auto` sets the default.
Failed turns retry five times; `max_turn_retries` changes that budget. With
automatic retry enabled, active goals retry ordinary errors without a limit,
while policy stops pause them.

A normal final answer ends the turn; set a goal to continue work beyond it:

```text
/goal fix the bug and validate the change
```

Goals continue until complete or blocked; queued prompts come first. `/goal pause`
pauses at a turn boundary, `/goal resume` continues paused or blocked work, and
`/goal clear` cancels while retaining history.
Blocked goals name their wait channel, such as `irc: endpoint/nick`, a timer or
an external dependency. `operator` means operator input is required; `/goal`
shows the recorded destination and blocker.

Ctrl-C clears a nonempty draft; with an empty draft it interrupts work and
pauses goal continuation. Empty Enter leaves the goal paused; use `/goal resume`.
Ctrl-D on an empty draft exits, and no work continues after exit. Pending room
updates remain available for later admission; direct mentions remain urgent.

The conversation, tools, queue and goal are saved as a **session**. Resume
continues unfinished work; calls with uncertain outcomes require inspection.
Linux sessions started on ANSI terminals at least 20 columns wide survive
terminal loss. Use `/s d` to detach, then `snajpagent --attach SESSION_ID`
(or `-A`) to reconnect with the retained draft. Without an ID, attach offers a
running-session picker. `/s a ID` switches live sessions; failure keeps the source.
Detach and normal exit print a copyable resume command using the full session ID.
It reconnects to a running owner and retains session settings.

File transfers and local audio stop on disconnect; queued downloads remain
saved. Restart microphone capture after reconnecting. New editors and pagers
use the replacement terminal's profile; running programs keep their environment.
List sessions or reopen the latest one:

```sh
snajpagent -l
snajpagent -l 25
snajpagent --resume --last
```

`-l` groups attached, detached and other running sessions, then the ten most
recent stored sessions. Groups sort by saved activity. `-l N` changes the stored count; `-l 0` lists running sessions.

Convert stopped legacy sessions with `snajpagent convert --jobs 4`. The command
retains original journals, skips locked writers and reports individual results.
The [manual](snajpagent.1) covers interruption and recovery.

Name sessions with `-N lead` or `/session name lead`; select them with
`--attach -N lead` or `--resume -N lead`. Duplicate names require IDs.
**Active goals continue on resume**; pause before exiting to keep one paused.

### Transfer files through the terminal

Wrap your connection with the native workstation client:

```sh
snajpagent remote ssh -t target snajpagent
snajpagent remote mosh target snajpagent
# Reattach a running agent session:
snajpagent remote ssh -t target snajpagent --attach SESSION_ID
snajpagent remote mosh target snajpagent --attach SESSION_ID
```

Use `/session detach` to leave work running; omit `SESSION_ID` to choose one.
Keep the wrapper outside SSH or Mosh, with compatible builds at both ends.
`remote` passes arguments literally through a PTY. Put Mosh options before the
hostname; the wrapper separates remote application options. Transfers use the
same terminal connection: a fast SSH stream or a slower checked Mosh channel.
For tmux, replace the final command with `tmux attach`. During transfers, keep
the agent pane focused with one writable client viewing it.

Drop one regular file into the POSIX composer, or use `/receive`. Uploads reject
directories and empty files; verified files become unsent attachments. In a Vim
rollout composer, `/receive` and `/attach PATH` return to the workspace after file
preparation. Review `/attachments` before submitting. `/send PATH` and model
`send_file` download files, including empty files and `asset:ID` references. Transfers show progress
and saved-path receipts, then restore the draft. Downloads default to
`~/Downloads`; set `[terminal] download_dir` on the workstation to change it.

Detached sends queue exports. Fast-stream reattachment delivers them at idle;
after Mosh reattachment, use `/send PATH` and remove the delivered queue ID.
Changed or uncertain exports stay pending. The manual covers recovery and the
alternative trzsz-go client.

### Attach files and use voice

Stage files with `/attach PATH`, review `/attachments`, then submit. The agent
can inspect documents, sample video and transcribe audio; originals and results
stay with the session.

`/dictate` inserts speech into your draft. `/voice on`, `/voice mute` and
`/voice off` control conversation audio; `/play asset:ID` plays saved audio.
Speech can inspect, steer or queue coding work while preserving typed drafts.
Voice uses your selected provider's credentials. A codex-lb gateway needs its
`/backend-api/codex` base. Use a headset and HTTPS or a secure tunnel outside
trusted networks. The [manual](https://agent.snajpa.net/manual.html) covers setup,
data destinations and capture controls.

### Keep useful findings in files

Project notes and `AGENTS.md` carry findings across sessions. Keep decisions
current and proposals separate from approvals. `-d DIR` adds a documentation
root; repeat it for shared or cross-repository notes. Relative documentation
paths use the launch directory and persist in the session.

## 2. Work together

Run agents beside their tools and repositories. One instance hosts an IRC room;
others connect to assign work and exchange findings. Each can join several endpoints.

In two terminals:

```sh
snajpagent -s -n builder -o alice -r work
snajpagent -c -n reviewer -o bob
```

The first hosts `#work` at `localhost:6667` and the second joins it on the same
machine; `-n` names the model, `-o` its operator and `-r` the hosted room.
`/names` shows accepted names, where a name already in use gets a suffix.

### Choose who receives your message

Networked startup opens **chat**; Enter sends as your operator name.
`@builder check the empty-input case` starts or steers builder at a safe boundary.
Ordinary conversation supplies background context.

Empty Tab cycles through **rollout**, connected rooms and opened private chats.
Shift-Tab moves backwards with a draft present. Each view keeps its own draft.
Incoming messages preserve focus. The chat prompt names the destination when
multiple channels or queries are present. In rollout,
Enter directs your local agent; in operator chat, Enter sends to its selected
room or peer. Open a private tab with `/query nick`; `/msg nick text` and
`/notice nick text` send without changing focus. `/me text` sends an action in
the selected operator conversation. Use `endpoint/nick` or `endpoint/#channel`
when choosing among connections. `/join endpoint/#channel` joins a channel as
the operator; `/chat endpoint/#channel` opens its history and composer. The
built-in server offers its configured room through the same commands.
Agent private chats are read-only. The working transcript
stays in rollout; models use `irc_send` to publish chosen messages, which can
include material from that transcript.

Models can use `irc_sleep` to hold updates until a timeout, mention or message
threshold, and `irc_compact` to summarize IRC context asynchronously. Your
transcript stays complete. See the manual's model IRC controls.

In chat, Tab completes `@nickname`, then cycles open conversations. Without
conversation tabs, Tab at the end queues a local follow-up during work.

### Coordinate work

Assign work in the room; agents exchange findings. Keep code/handoff notes in
project files and Git: IRC shares no files, credentials or processes. Independent
edits use separate Git worktrees.

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

`/help` lists controls; `/status` shows state; `/queue` lists and edits waiting work.
Keyboard reports use `$PAGER`, defaulting to `less -FX` when unset or just `less`,
if available. Short reports return automatically; longer reports wait for quit.
Work and IRC keep buffering output. `[ui] pager = off` displays reports directly.

`/model` selects provider, model and effort; `/model cache` refreshes the catalog.
Append `:context` to select its window, for example
`/model openai/MODEL/high:200000 save`. Selection survives resume. Model-callable switching requires
`[agent] allow_model_change=true`. `/fast` toggles priority service; provider
support and pricing apply. `/context default|max|NUMBER` selects the context
budget; append `save` for a provider/model default. Larger windows may change
pricing. `/status` explains accounting; `?%` means usage is unknown.

### Connect MCP tools

Current development builds expose configured Streamable HTTP MCP tools directly
in model turns. Add `[mcp NAME]` with the endpoint and registered OAuth client,
then run `snajpagent mcp login NAME` and complete the displayed consent URL.
`/configure` adopts the login in an existing session; `/mcp tools NAME` shows
exact declarations and `/mcp status NAME` shows identity, scopes and diagnostics.
Calls require exact operator approval unless local policy allows their names.
See the manual's **Native MCP** section for setup, approval and recovery, or
[the configuration example](examples/mcp.ini).

### Restrict what the model may do

Configuration filters model tool calls through `[rule NAME]` sections,
validated at load:

```ini
[rule deny-recursive-delete]
match   = {"/tool":"^exec_command$","/text":"rm[[:space:]]+-[^[:space:]]*[rf]"}
action  = deny
message = "Recursive force-delete is disabled; delete explicit paths."
```

The first matching rule decides; rejected calls are reported as not run.
Rules filter calls without providing OS confinement. Use separate accounts for
isolation. [Rule syntax](design/io-rules.md) and [example policies](examples/io-rules/)
cover configuration and migration.

## Install and choose a provider

[Choose an executable](https://agent.snajpa.net/downloads.html) for your OS,
architecture and ABI. Rename it to `snajpagent` (`snajpagent.exe` on Windows)
and verify its SHA-256 against the download row or `SHA256SUMS` before running.
Verify archives before extraction. Keep the executable in a user-owned directory.

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

In Command Prompt, verify with `certutil -hashfile .\snajpagent.exe SHA256`.
Keep the `.exe` extension; add its directory to PATH or invoke its full path.

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

Official stable binaries update in the background and show a restart banner;
`[agent] auto_update = false` opts out. Development debug binaries default to
updates off and follow `latest-dev` when enabled. Ordinary source builds remain
updater-free. The manual covers channels, publisher URLs and recovery.

`./configure` probes the toolchain and the four optional modalities and tunes the
tracked `config.mk`; `make WITH_*=…` stays an explicit override.

A session is one agent run. A workspace is a saved layout of panes showing
sessions and conversations; several panes can show the same session.
The development `snajpagent vm` frontend provides named workspaces, splits,
retained transcripts, retrospective verbosity, search, mouse controls and Vim
editing. Click an attached pane to enter **INSERT** for its prompts and `/commands`.
Escape returns to **NORMAL**; `i`/`a`/`I`/`A` enter its composer again. From NORMAL,
`:` enters **COMMAND-LINE** for workspace commands. The workspace command line names
the current mode; dragging transcript text enters **VISUAL** selection.
Each session prompt shows its name and live status. Conversation labels appear
when there are multiple channels or queries to switch between.
`:new [NAME]` creates an agent; `:session ID` resumes one. On POSIX,
`:workspace detach` (or `:workspace d`) saves and detaches the whole workspace while
agents continue. Tab/Shift-Tab complete commands and their options.
`:w` saves the workspace; `:q` / `:x` / `:wq` save and detach it. `:q!` stops
all controlled sessions and exits, preserving layout and drafts. `:close` closes
a pane; `:session quit` stops the focused session.
Leaving a saved workspace prints its `snajpagent vm --resume WORKSPACE_ID` command
after restoring the shell screen. `:session detach` (also `:session d` or
`:detach`) detaches only the focused session and shows `:attach SESSION_ID`.
Restoring a workspace leaves stopped owners stopped.
The experimental Windows workspace runs one live agent: `:close` hides it and
`:q!` stops it before exiting. Additional live agents and persistent detach are unavailable.

`/query NICK` opens a private conversation; `/chat #CHANNEL` opens a known channel.
`:buffers` lists conversations;
`:vsp ADDRESS` opens one in a split. Each conversation keeps its draft and undo
history; agent conversations are read-only.
Use `/query SESSION/ENDPOINT/NICK` to select another attached session's query;
`:attach ID` in another split gives the workspace control of that session.

Pane transcripts retain commands and output. `:reports` lists retained reports;
`:history` returns to rollout. Search with `/TEXT`, `?TEXT`, `n`/`N`; select with
`v`, `V` or Ctrl-V and yank with `y`. Clipboard transfer works through `remote`;
OSC 52 delivery is unconfirmed. The [manual](snajpagent.1) covers reports,
clipboard policy and `:classic` terminal access. `WITH_VM=0` omits the frontend.

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

This installs the binary and manual under `$HOME/.local`; the default prefix
is `/usr/local`. Production builds need `strip` and ELF `objcopy` or macOS
`dsymutil`. `make DEBUG=1` enables debugging.
`make prod-matrix` builds standalone targets into `build/matrix/` without
installation. [Dependency notes](DEPENDENCIES.md) cover recipes, ABI requirements
and experimental platforms.

Without configuration or credentials, the first interactive launch offers
ChatGPT/Codex or Meta subscription, OpenRouter, OpenAI or custom-provider setup;
authenticate and choose a [supported model](#supported-providers). `snajpagent login status`
reports local credential sources without contacting a provider, and the manual
explains login methods and logout.

For an Enterprise Admin Console token with Codex permission:

```sh
printf '%s\n' "$CODEX_ACCESS_TOKEN" | snajpagent login codex --with-access-token
snajpagent -m codex/gpt-6.1-sol/xhigh
```

First setup also needs `-m MODEL` before `login`. Existing defaults stay unchanged;
use `/model codex/MODEL` to switch a running session. Replace expired or revoked
tokens by repeating login. Platform API keys use `login openai --with-api-key`.

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

Export `OPENAI_API_KEY` before launch; provider names are local labels retained
in saved model selections. Each running owner keeps its settings and credentials.
Use `/configure` to reload saved settings, credentials and the local model cache;
`/config` opens `$EDITOR` first. Invalid changes leave the previous state active.

Windows uses `HOME/.snajpagent`, falling back to `USERPROFILE/.snajpagent`;
`--dotdir DIR` overrides it. The same configuration works there. Set credentials
with PowerShell's `$env:OPENAI_API_KEY = 'your-key'` or Command Prompt's
`set "OPENAI_API_KEY=your-key"`; masked setup entry avoids shell history.
Windows tools use `cmd.exe`; POSIX command examples need adaptation.

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
