<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent

snajpagent is a fast, lightweight, networked coding agent for your terminal.
Work with a model in your project, steer it while it runs, and connect people
and agents through native IRC chat. Each session has its own workspace,
tools, and local working view.

[Website](https://agent.snajpa.net) · [Install](#install-and-choose-a-provider) · [Manual](snajpagent.1)

## 1. Work in rollout

```sh
cd /path/to/your/project
snajpagent
```

Describe a change and press Enter. **Rollout** is your local working view.
A **turn** is the work started by one prompt, including tool use. Model
responses appear here; `/verbose 1` adds tool activity and `/verbose 2` adds
argument/result previews.

[![A real rollout fixing whitespace handling and passing four checks](www/screenshots/ordinary.png)](www/screenshots/ordinary.png)

### Steer now, queue for later

Typing during work opens a draft without submitting it.

- **Enter while idle:** start a turn.
- **Enter during rollout work:** steer the current turn. “Keep the public API
  unchanged” interrupts the model's response and continues from delivered text
  with your correction. An already-running command stays alive, not killed.
- **Tab during work, with text:** queue a separate turn, unless completing
  text. “Then add a regression test” waits outside the current model cycle;
  it neither interrupts nor changes the current task.

Queued turns run automatically, oldest first, after current work finishes.
They are sequential turns in one session, not parallel jobs. `(N)` in the
default rollout prompt counts pending turns, excluding the running turn.

```text
/queue                 list pending turns
/queue 2 edit          revise item 2 in the composer
/queue 2 delete        remove item 2
/queue pop             remove the newest item
/queue clear           remove all pending items
```

Failure or interruption pauses the queue. Retained turns also wait after
session resume. Inspect them, then use `/next` while idle to resume execution.
Opening a queue editor holds draining until the edit ends.

### What Tab does

In an ordinary composer:

1. **Empty draft:** switch chat ↔ rollout, including during work or offline.
   Switching does not submit anything or stop work.
2. **Completable text:** complete slash commands, numeric destinations and,
   in chat, `@nick`. Ambiguous matches fill their common prefix; another Tab
   lists choices. Completion never sends or queues text.
3. **Other text:** queue a turn while active; insert spaces while idle.

Ctrl-J inserts a newline. Up/Down recall prompts; Ctrl-R searches history.
Ctrl-C clears a draft; with an empty draft it interrupts work.
Ctrl-D on an empty draft exits. `/help` lists commands and keys.

### Goals and resuming

A completed ordinary turn returns control to you. A **goal** continues across
turns until complete or blocked:

```text
/goal fix the reconnect bug and validate the change
/goal pause
/goal resume
```

Pause takes effect at a turn boundary. Queued operator turns run before the
next automatic goal turn.

Normal exit prints a resume command; `snajpagent --resume --last` reopens the
last session for your workspace. Resume preserves saved goal state: active
goals continue; paused, blocked, or finished goals stay that way. Retained
queues wait for `/next` and take precedence over automatic goal continuation.
Quitting the process interrupts its turn without pausing its goal.
After failure, `/retry` continues from retained context and tool results,
without replaying completed tools or resuming a paused queue.

On Windows, the printed command targets a normal `cmd.exe` prompt with delayed
expansion off (the default), as its header states. Paste it there, not into
PowerShell or a batch file. The command uses a scoped child interpreter to keep
literal `%`, `!`, quotes and other path characters intact without changing your
shell's environment.

## 2. Work together over the network

snajpagent is both an IRC server and client. Each session has an operator nick
and a model nick. Chat is shared; workspaces and tools stay local. A room is
neither a shared filesystem nor a broadcast of every tool call.

Run these in separate terminals, from the projects each model should work in:

```sh
snajpagent -s -n builder -o alice -r work
snajpagent -c -n reviewer -o bob
```

The first hosts `#work`; the second joins at `localhost:6667`.
`-n` names the model; `-o` names its operator. `/names` shows accepted nicks.

Networked startup opens **chat**. Enter sends publicly to the selected room.
`@builder fix the parser's empty-input case` addresses that model. During work,
a new mention steers at a safe response/tool boundary without truncating
current generation. Unaddressed conversation remains background context.
Models publish chosen messages through `irc_send`; other output stays local
unless they choose to share it.

Empty **Tab** switches to rollout to inspect your model, then back to chat.
`/chat` and `/rollout` select a view directly. Work and connections continue;
hidden-view output appears on return. With text during work, non-completing
Tab queues a **local future turn**, even in chat. Enter sends a room message.

[![Alice, Bob, builder and reviewer discuss a regression test in a real IRC room](www/screenshots/irc.png)](www/screenshots/irc.png)

Colors indicate roles, not individuals: operator nicks are magenta and model
nicks cyan across host/client views. Mentions of your accepted operator/model
nick highlight timestamps and ordinary text, without overriding sender colors
or rendered Markdown styles. Exact hues depend on the terminal palette.

`/server start` and `/connect ENDPOINT` manage networking without restarting
work. Multiple rooms have numeric destinations: `/2` selects one, `/2 TEXT`
sends there once, `/all TEXT` broadcasts. History and reconnection are automatic.

IRC has no authentication or TLS: use localhost, a trusted network, or a
secure tunnel. Tools run with your permissions, without an approval sandbox.
Protect credentials, configuration, session logs, and prompt history.

## Install and choose a provider

Build with C11/POSIX, pthreads, GNU make, libcurl, and Jansson:

```sh
git clone https://github.com/snajpa/snajpagent.git
cd snajpagent
make
sudo make install
```

Installation adds the binary and manual under `/usr/local`. Production also
needs `strip` and `objcopy` on Linux, or `strip` and `dsymutil` on macOS.
`make DEBUG=1` builds for debugging; `make help` lists overrides and standalone
targets. See [dependency notes](DEPENDENCIES.md) for portability.

The experimental Windows x64 package is built explicitly with
`make prod-windows-x86_64` using pinned Nix dependencies. Copy
`build/matrix/windows-x86_64/bin/snajpagent.exe` to Windows: application
libraries and CA roots are included, with no third-party runtime DLLs.
It is qualified incrementally on Windows 11; legacy Windows remains in progress.
Plain `make` still builds only the host platform.

Without configuration or credentials, first interactive launch offers provider
setup. Authenticate and select a supported model. Private configuration and
credentials live under `$HOME/.snajpagent`. `/config` opens configuration;
`/model PROVIDER/MODEL/EFFORT` changes model settings. The manual covers
providers, login, and manual configuration.

## Guidance and scripts

snajpagent advertises applicable `AGENTS.md` paths; the model reads relevant
files and follows their pointers. Ordinary project notes retain findings, not
permissions. `-d DIR` adds documentation roots, each with `AGENTS.md` or
`AGENTS.override.md`. Small tasks need not create documentation chores.

```sh
snajpagent -e -- "run the tests and summarize failures"
printf '%s\n' 'review the current diff' | snajpagent -e
```

One-shot model text goes to stdout; diagnostics go to stderr.
`/ro QUERY` selects a read-only turn without command execution.

Read `man snajpagent` or the [source manual](snajpagent.1) for the complete
interface. [Design notes](design/architecture.md) explain the implementation.
Licensed [GPL-2.0-only](COPYING); see [LICENSE_SCOPE](LICENSE_SCOPE).
