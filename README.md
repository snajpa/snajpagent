<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent

snajpagent is a fast, lightweight, networked coding agent for your terminal.
Ask an AI model to work on a project: it can inspect files, make changes, and
run commands. You can correct it while it works or line up the next task.
Native IRC chat lets people and agents work together without sharing their
entire local conversation.

Everything runs in a normal scrolling terminal. Each session has its own
project directory and tools. There is no background service: when you exit
snajpagent, its work stops, and you can resume the saved session later.

[Website](https://agent.snajpa.net) · [Install](#install-and-choose-a-provider) ·
[User manual](https://agent.snajpa.net/manual.html)

## 1. Work in rollout

After [installing and choosing a provider](#install-and-choose-a-provider),
start in the project you want to change. This directory is your **workspace**.

```sh
cd /path/to/your/project
snajpagent
```

Describe a task and press Enter. For example:
“Fix the empty-input bug, keep the public API unchanged, and run the tests.”

The model can read files, edit them, and run commands to carry out your request.
That prompt and the work it starts make up a **turn**. One turn may involve
several exchanges with the model before it gives a final answer.

**Rollout** is the local view of that conversation and work. It opens first
when no networking is configured.

Text beginning with `/` is a command to snajpagent, not a request to the model.
Type these commands and press Enter unless another key is specified.
`/help` lists commands and keys; `/status` shows the current state.

By default you see model text. `/verbose 1` adds compact tool activity;
`/verbose 2` adds previews of tool inputs and results. You can change this
while the model works.

[![A real rollout fixing whitespace handling and passing four checks](www/screenshots/ordinary.png)](www/screenshots/ordinary.png)

In this real run, the model changed whitespace handling and ran four checks.
The input prompt shows the selected model and context percentage.
`›` means **idle**, waiting for a task; `»` means **active**, working on a turn.

### Correct the current task or add the next one

You can type while work is active. Your unsent text is a **draft**; typing alone
does not interrupt the model.

**Press Enter to steer the current turn.** For example, “Use the existing
parser; don't add a dependency” changes what the model is doing now.
In rollout, this interrupts the model's response and continues from the text
already delivered. A command that has started keeps running; steering does
not kill or restart it.

**Press Tab at the end of an ordinary message to queue a later turn.**
“Then add regression coverage” waits until current work finishes. The model
does not receive that queued prompt as a change to its current task.

This queue runs oldest first, one turn at a time in the same session.
It is not a set of parallel agents. The prompt shows `(N)` for the number
of waiting turns, excluding the one currently running.

### Completion is for `/commands`, not ordinary words

Start a draft with `/` to complete a command name. Type `/sta`, then press
Tab: it becomes `/status `, with a space after it. Press **Enter** to run it.
The same completion works for numbered room commands such as `/2`, explained below.

Tab is only completing the command name while the cursor is in that first
`/word`. It does not try to complete ordinary sentences. In chat, it also
completes nickname words beginning with `@`; see the example in chapter two.

A unique completion adds a space and leaves you ready to keep typing.
If several names match, Tab extends their common beginning; a second Tab lists
the choices. If none match, the text stays unchanged—it is not queued instead.

For ordinary text outside a completion, Tab queues while your model is active
and inserts spaces while idle. **With an empty draft, Tab switches views.**
Completion itself never sends a message or queues a turn.

### Inspect and edit the queue

Use `/queue` to see the waiting prompts. These commands change the queue,
not the task already running:

```text
/queue 2 edit          revise item 2
/queue 2 delete        remove item 2
/queue pop             remove the newest item
/queue clear           remove all waiting items
```

The queue editor opens the chosen prompt for editing in the input line.
Enter saves your replacement in the same position; Ctrl-C abandons the edit.
Waiting work does not start while the editor is open.

After interruption or failure, the queue can be paused. Queued prompts also
wait after session resume. Use `/next` while idle to restart the queue,
beginning with the oldest prompt and then continuing through the rest.

### Edit a draft and stop work

- **Ctrl-J:** insert a newline. Pasting multiple lines also keeps them in the
  draft rather than sending each line.
- **Up/Down:** recall prompts. **Ctrl-R:** search prompt history.
- **Ctrl-C with text in the draft:** clear it without stopping the model.
- **Ctrl-C with an empty draft:** interrupt the current turn.
- **Ctrl-D with an empty draft:** exit snajpagent.

After a failed turn, `/retry` continues from the conversation and tool results
already saved. It does not repeat completed tools or restart a paused queue.

### Keep working toward a goal

Normally, a final answer ends the turn and returns control to you. A **goal**
asks snajpagent to keep taking turns toward a result:

```text
/goal fix the reconnect bug, add tests, and validate the change
```

It continues until complete, paused, cancelled, or blocked. Waiting operator
prompts run before the next automatic goal turn. `/goal status` shows progress
and any blocker. `/goal pause` stops automatic continuation at a turn boundary;
`/goal resume` starts it again.

Pausing a goal does not kill a command already running. To end the goal
explicitly, use `/goal complete` or `/goal cancel`. Other goal controls,
including locking its wording against model changes, are in the manual.

### Leave and come back

A **session** is the saved conversation, settings, tool results, queue, and goal.
Normal exit prints a command to reopen that exact session. You can also list
sessions or reopen the newest for the current workspace:

```sh
snajpagent -l
snajpagent --resume --last
```

**Exit is not goal pause.** No work runs after exit, but a saved active goal
continues when you resume. Use `/goal pause` first if you want it to stay paused.
Paused, blocked, and finished goals retain their saved states.

Queued prompts still wait for `/next` after resume, ahead of automatic goal
work. Resume restores saved context, not command processes that ended with
the previous program. `/history` shows the latest retained exchange;
`/archive` archives an idle session without deleting its history.

On Windows, paste the printed resume command into a normal `cmd.exe` prompt
with delayed expansion off, as its header specifies—not PowerShell or a batch
file. The command preserves literal argument characters without changing your
shell's environment.

## 2. Work together over the network

IRC is a chat protocol built into snajpagent. One instance can host a room;
others connect to it. Each session has two chat names, or **nicks**: one for
the operator (you) and one for its model.

Run these in separate terminals, from the workspace each model should use:

```sh
snajpagent -s -n builder -o alice -r work
snajpagent -c -n reviewer -o bob
```

The first command hosts `#work` at `localhost:6667`. The second connects to
that room on the same machine. `-n` names the model, `-o` names its operator,
and `-r` names the hosted room.

This shares conversation, not files, credentials, or command processes.
For independent edits to the same repository, use separate Git worktrees—
working directories with their own checked-out files.

### Send a room message or speak to your own model

Networked startup opens **chat**, the shared room conversation. Enter sends
as your operator nick. For example, `@builder check the empty-input case`
asks builder to work. A mention during its work steers it at a safe boundary.
Unlike rollout Enter, it does not cut off the current model response.

Ordinary room conversation is background context; it does not start a new
turn by itself. A model uses the `irc_send` tool to publish a chosen reply.
Its other responses and tool activity stay in its local rollout.

**Press Tab on an empty draft to switch between chat and rollout.**
`/chat` and `/rollout` select a view directly. Work continues while you switch;
returning displays output that arrived while the view was hidden.

In rollout, ordinary Enter input goes to your own model, not the room.
In chat, Enter sends to the room; if disconnected, the draft is kept rather
than silently used as a private prompt.

### Complete a nickname, then finish the message

In chat, type `@bu` and press **Tab**. If `builder` is the unique match,
this becomes `@builder `, including the space. Continue writing:

```text
@builder check the empty-input case
```

Now choose how to submit the finished text:

- **Enter:** send it to the room.
- **Tab at the end, while your local model is active:** queue it as a local
  follow-up for that model. It is not sent to the room.

`@` starts the nickname word, not necessarily the whole message:
`please ask @bu` also completes. Bare `bu` does not.
If the cursor is still at the end of `@bu` or `@builder`, Tab completes that
name first. Once past the added space and at the end of your finished message,
Tab can queue it. While idle, that Tab inserts spaces instead.

[![Operators and models discuss a regression test in a real IRC room](www/screenshots/irc.png)](www/screenshots/irc.png)

Operator nicks are cyan and model nicks blue. Mentions of your operator or
model highlight the timestamp, sender, and `›` in magenta, leaving the body
unchanged. A sender mentioning only its own nick does not trigger highlighting.
Exact colors depend on the terminal palette.

### Add connections and choose a room

`/names` lists rooms and their members. It also shows the actual nicknames:
a name already in use gets a suffix, so address the accepted name.

You can change connections without restarting the session. `/server start`
hosts a room; `/connect ENDPOINT` adds a connection. An endpoint is an address
such as `localhost:6667`. `/server stop` stops hosting only, while `/disconnect`
removes outgoing connections. `/status` distinguishes a requested connection
from one that has joined successfully.

When there are several rooms, `/names` gives each a number. `/2` selects one
for ordinary chat input. `/2 TEXT` sends there once without changing your
selection; `/all TEXT` broadcasts once. These explicit sends also work in
rollout. The model chooses its own send destination rather than following
your current selection.

The program handles reconnects and retrieves missing room events without
repeating already-received conversation. New clients get bounded initial
history; gaps beyond the server's retained history are reported.

**IRC has no authentication or TLS.** Use localhost, a trusted network, or a
secure tunnel. Local rollout is not a secrecy guarantee: the model can choose
to share its contents. Tools run with your permissions, without command-approval
sandboxing. Protect credentials, session logs, and prompt history.

## Choose a model and manage context

A **provider** is the service that runs your chosen model. `/model` lists its
locally cached catalog without a network request. `/model cache` explicitly
refreshes catalogs. Select a displayed row by number, or use
`/model PROVIDER/MODEL/EFFORT` while idle. Effort is the model's reasoning-effort
setting. Add `save` to persist a selection.

**Context** is the information sent to the model for its next response. It has
a size limit. The prompt's percentage compares a request-token bound with the
usable input budget; it is not a bill or necessarily an exact token count.
`?%` means capacity is unknown. `/status` explains the current accounting,
and the manual covers capacity rules and output reservations.

**Compaction** replaces older active context with a summary so work can
continue. It normally happens automatically as context fills. `/compact`
requests it while idle. The original transcript stays on disk, but not every
detail survives in the summary. Keep important requirements in project documents.

For inspection without commands or edits, use `/ro QUERY`. This turn can list,
read, and search files and use provider-hosted web search. It cannot run commands,
patch files, change goals, or send IRC messages. During active work, use
`/queue /ro QUERY` to ask it next. Read-only limits tool actions, not confidentiality:
readable local files remain accessible, and normal session history is recorded.

## Install and choose a provider

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
`build/matrix/`, without installation or VMs. Legacy and other planned ports
remain unfinished; successful cross-builds are not runtime qualification.

For experimental Windows x64, `make prod-windows-x86_64` builds using pinned
Nix dependencies. Copy `build/matrix/windows-x86_64/bin/snajpagent.exe` to Windows;
application libraries and CA roots are included without third-party runtime DLLs.
Qualification is incremental on Windows 11; legacy Windows remains in progress.
Plain `make` builds only the host platform.

Without configuration or existing credentials, the first interactive launch
offers ChatGPT/Codex, OpenRouter, OpenAI, or custom-provider setup. Authenticate
and choose a supported model. `snajpagent login status` reports local credential
sources without contacting a provider. The manual explains login methods and logout.

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

## Project guidance and scripts

Put project instructions in `AGENTS.md`, with pointers to relevant documents.
snajpagent tells the model where those files are; it reads the guidance it
needs. Notes can preserve findings and decisions between sessions, but a
recorded proposal is not permission to act.

`-d DIR` adds a documentation root containing `AGENTS.md` or `AGENTS.override.md`.
Repeat it for several roots. Relative paths use the launch directory, even
with `-C`; the printed resume command retains them. Small tasks need not
create documentation chores.

One-shot mode runs a prompt without an interactive conversation:

```sh
snajpagent -e -- "run the tests and summarize failures"
printf '%s\n' 'review the current diff' | snajpagent -e
```

Model text goes to stdout; diagnostics and the resume hint go to stderr.
Redirected output has no terminal styling. Use exit status for failure handling;
the internal pre-1.0 event-log format is not a stable integration API.

The [user manual](https://agent.snajpa.net/manual.html) and `man snajpagent`
cover the full reference and troubleshooting. Both come from [one source](snajpagent.1);
[project instructions](AGENTS.md) require updates alongside behavior changes.
[Design notes](design/architecture.md) cover implementation.
Licensed [GPL-2.0-only](COPYING); see [LICENSE_SCOPE](LICENSE_SCOPE).
