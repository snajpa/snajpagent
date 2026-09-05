<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent

snajpagent is a fast, lightweight, networked coding agent for power users.
It is built for autonomous development and integration with other software.

[![snajpagent fixing and testing a small C program](www/screenshots/ordinary.png)](www/screenshots/ordinary.png)

One foreground process streams model output, runs coding tools, and keeps
resumable local sessions. Agents and operators can share an IRC room.

## Build

You need a C11/POSIX environment with pthreads, GNU make, libcurl, and Jansson.

```sh
git clone https://github.com/snajpa/snajpagent.git
cd snajpagent
make
sudo make install
```

The default prefix is `/usr/local`. Installation adds the binary and
`snajpagent(1)`. `make check` runs the tests; Python 3 is needed, and tmux
enables the terminal integration tests.

## Configure

On a fresh interactive launch, `snajpagent` offers provider setup: ChatGPT/Codex
device login, OpenRouter, OpenAI, or another Responses-compatible service.
Choose the backend and a local provider name, authenticate, optionally fetch its model list, and select
a model. Settings go into a minimal private `config.ini`; stored credentials
go separately under the dotdir's private `auth/` directory.

You can also configure a provider explicitly:

```sh
snajpagent login codex --device-auth
snajpagent login openrouter
snajpagent login status
snajpagent logout openrouter
```

Put common options before the command. For noninteractive initial setup, choose
a model and supply an API key through stdin, never through a command argument:

```sh
snajpagent -m vendor/model login openrouter --with-api-key < /private/key-file
```

Existing configuration and environment-key use still work. There is no setup
wizard for piped input, `-e`, resume, listing, or an explicit missing/invalid
configuration. Login for an existing provider leaves the default model alone;
`/model PROVIDER/MODEL/EFFORT` selects it. Codex tokens refresh on use and coordinate
across processes, while OpenRouter keeps its own credentials and capabilities.

Choose a model your Responses-compatible provider supports. For example,
create a private configuration directory:

```sh
install -d -m 700 "$HOME/.snajpagent"
```

Save this as `$HOME/.snajpagent/config.ini`:

```ini
[agent]
model = gpt-5.5

[provider openai]
base_url = https://api.openai.com
api_key = ${OPENAI_API_KEY}
```

Export the referenced credential, then start in your project:

```sh
export OPENAI_API_KEY='your-key'
cd /path/to/project
snajpagent
```

Providers need no special name: the first declaration is used for a new session
unless `[agent] provider` or `--provider NAME` selects another. Unqualified
`/model` changes stay on the current provider. `--provider NAME` also permits
explicitly resuming a session after a provider rename; history is not rewritten.
Without `--provider`, a resumed `-m`/`--effort` remains a one-turn override.

`api_key` accepts `${ENV_NAME}`, a double-quoted literal (JSON escaping), or an
unquoted file path. Relative paths use the config directory; leading `~/` uses
the user's home. Bare `OPENAI_API_KEY` means a filename, not an environment name.
Files may end with one LF/CRLF and may be symlinks for secret rotation. Literal
secrets require a private config file, normally mode `0600`. Missing/bad explicit
sources fail; they never fall back to a stored login. With no `api_key`,
`auth = api_key` (the default) uses the provider's stored login. `auth = chatgpt`
requires managed OAuth and no `api_key`. Obsolete `api_key_env`, `auth = env`,
anonymous `[provider]`, and `[tool] secret_env` are rejected.

Use repeatable `[tool] secret` entries with the same source syntax for additional
values to redact. They do not export credentials to tools; referenced environment
variables are removed from child environments. Do not put secrets in prompts.

### Provider models and token budgets

Several local models can use the same upstream model with independent limits:

```ini
[provider codex-lb]
base_url = http://127.0.0.1:2455/backend-api/codex
api_key = ${CODEX_LB_API_KEY}

[model-limit codex-lb]
context_window_tokens = 500000

[model-alias codex-lb/astra-small]
model = gpt-6-astra

[model-alias codex-lb/astra-large]
model = gpt-6-astra

[model-limit codex-lb/astra-*]
max_output_tokens = 16000

[model-limit codex-lb/astra-small]
context_window_tokens = 128000
```

Select `--provider codex-lb -m astra-small` or `/model astra-large/high`.
These are ordinary provider-local models in sessions, prompts and limit rules;
only provider routing maps them upstream. Targets are literal, never recursive.

Limits merge per field: provider-wide, all matching `*` patterns in file order,
then the exact local model. Unspecified fields inherit. Exact rules win even
when placed earlier; duplicate targets/keys are rejected. Patterns match the
whole local name, including `/` in model IDs. Upstream model names do not add
another matching tier. Supported fields are `context_window_tokens`,
`max_input_tokens` and `max_output_tokens`.

Catalog metadata supplies remaining fields; explicit limits can override it,
but cannot grant backend capacity. Output reservation and observed backend
ceilings still constrain input. `/status` shows effective limits and their
winning rules; automatic compaction follows the selected model's usable budget.

Use `/config` to edit and reload the configuration through `$EDITOR`.
`--config FILE` selects another configuration; `--dotdir DIR` selects another
private state directory. Both paths must be absolute.

## Work

Ask for a change, then use Enter to submit. During a response, Enter steers
the active turn at its next safe boundary. Tab queues a later turn. Ctrl-J
inserts a newline, Up/Down recall prompts, and Ctrl-R searches history.

Ctrl-C clears the draft while leaving it in scrollback with `^C`. With an
empty draft it also interrupts an active turn. Five consecutive presses within
two seconds exit. Ctrl-D on an empty draft interrupts active work and exits.
Use `/exit` while idle.

For one read-only query, use `/ro`:

```text
/ro find out how the request queue is implemented
```

That turn can list directories/files, read whole files or line ranges,
and search patterns using native snajpagent tools, and use provider-hosted web
search when supported by the selected provider/model. Search queries go to
the provider; local inspection does not invoke external commands. It cannot
run commands, edit files, send IRC messages, or change goals. During active work, press Tab
to queue `/ro ...`, or enter `/queue /ro ...`; `/ro` cannot steer a running
turn. The next ordinary prompt restores the normal toolset. This also works
with `-e`. See the manual for filesystem and output bounds.

For OpenRouter, use your normal provider configuration with
`base_url = https://openrouter.ai/api/v1` and its API credential/model.
snajpagent automatically declares OpenRouter's `openrouter:web_search` server
tool in normal and `/ro` turns. No search setting, `:online` suffix, plugin,
or separate search key is needed. OpenRouter selects the search engine and
bills search usage in addition to model usage. Detection uses the URL hostname,
not the provider section's name; other backends keep their existing search tool.
Default automatic token counting also tolerates OpenRouter's absent optional
count endpoint; explicit strict counting still requires that endpoint.

For work that should continue across turns, enter a goal:

```text
/goal fix the failing tests and verify the change
```

The agent continues until the goal completes, pauses, or needs help.
Queued prompts run in FIFO order without fresh goal reminders; goal reminders
and automatic continuation resume only after the queue is empty.
`/goal pause` stops automatic continuation after the current turn;
`/goal resume` restarts it. `/status` shows the current state. `/help` lists
all commands and keys.

`/model cache` downloads your providers' model catalogs. `/model` lists the
local cache; `/model NUMBER` selects a row. Add `save` to persist a selection.
You can also use `-m MODEL` without a catalog.

Color and terminal Markdown are enabled by default when supported.
Use `--no-color` or `--no-markdown` for plain presentation. Add `-v` to inspect
tool calls and results in the rollout. More `-v`s enable diagnostic detail.
Rollout prose uses compact paragraph bullets; timestamped IRC chat messages do
not gain a synthetic bullet, while genuine Markdown lists remain lists.

### Independent tool calls

The model can batch independent calls and keep several commands running while
doing other work. Yielded commands retain individual handles for `write_stdin`;
steering stops new launches without killing already-started work. All handles
must be settled before the turn finishes. Dependent steps need a model decision
between their results, not ordering within a batch.

Optional controls: `[tool] max_parallel_commands = 4` (1–32), and
`[provider NAME] parallel_tool_calls = true`. Existing yield/timeout/output
settings remain independent. Full redacted output is retained in journal
chunks; polls return incremental excerpts under the 6000-token default hard cap.

## Resume or script

Normal exit prints a ready-to-run resume command. You can also select sessions:

```sh
snajpagent -l
snajpagent --resume --last
snajpagent --resume SESSION_ID
```

Reopening pauses active goals and queued turns. Use `/goal resume` and `/next`
when ready. Sessions live under `$HOME/.snajpagent/sessions`.

For scripts, pass a prompt as arguments or through stdin:

```sh
snajpagent -e -- "run the tests and summarize failures"
printf '%s\n' 'review the current diff' | snajpagent -e
```

Final model text goes to stdout; diagnostics and the resume hint go to stderr.
Redirected text has no terminal Markdown formatting or color.

## Choose detail

The number of `-v` flags is the level, starting at **0** without flags.
`/verbose N` sets that same process-local level immediately, including during
active work; `/verbose` queries it. Configuration reloads do not change it.
More than six flags is a usage error. Remove any old `[ui] verbosity` config
line: verbosity is no longer a configuration setting.

| Level | Added local rollout detail |
| ---: | --- |
| 0 | Conversation, lifecycle notices, warnings and command replies |
| 1 | Compact tool start/outcome rows; no output body |
| 2 | 1,024-character argument and 512-character output previews |
| 3 | Full retained tool arguments, execution context and results |
| 4 | Runtime, accounting, durable-event and connection diagnostics |
| 5 | Redacted protocol payloads |
| 6 | Redacted transport diagnostics |

Tool previews use terminal-safe Unicode code points and explicit omission
markers. Full detail preserves existing capture/output limits. Chat always
shows the complete shared room transcript; private detail stays in rollout.
Unseen work is displayed at the current level; increasing the level does not
replay already visited history. Lowering it stops ineligible remaining detail.
Levels 4–6 are live diagnostics in visible rollout, not hidden chat history.
Redaction does not guarantee that prompts, source or tool output contain no
secrets. The printed resume command preserves your current level.

## Share a room

Every interactive launch is one coding session with optional networking. Start
plain, then use `/server start [ENDPOINT]` to host or `/connect [ENDPOINT]` to
join. These commands also work during model responses and running tools.
`-s` and `-c` are startup shortcuts for the same capabilities.

Run these in separate terminals, each in its own working project:

```sh
snajpagent -s -n builder -o alice -r work
snajpagent -c -n reviewer -o bob
```

The listener and client default to `localhost:6667`. Each server owns one room;
clients join it automatically. Without `-r`, the room is named after the host.
Its initial topic is the server's workspace path. Use `/topic TEXT` to change
the topic in the selected room.

With several connections, ordinary chat goes to the selected destination.
`/2` selects destination 2; `/2 hello` sends there once without switching.
`/all hello` broadcasts once. `/names` lists the numbers and addresses.
The prompt shows a short destination label only when needed. These commands
also work with one destination. Removed targets never redirect to another room.

Operators have channel op status. Only model-nick mentions steer active agents;
ordinary chat, including the local operator's, waits as background context.
In chat, Tab completes `@nick` at the cursor from current room members.
Only the model's `irc_send` tool posts to the room; other model
text stays in the local rollout. Joining, history, and reconnects are automatic.

Startup networking opens timestamped chat; otherwise startup opens rollout.
Both views are always available. Empty Tab switches between them; `/chat` and
`/rollout` select explicitly. Runtime networking changes preserve the current
view, draft and verbosity. Offline chat retains its transcript and rejects
unsendable input without losing the draft or turning it into private model input.
Implicit nicks
start at `agent0` and a valid `$USER` plus `0`; further default clients use
`agent1`/`USER1`, then `agent2`/`USER2`, without doubled zeroes. Explicit nicks
still get appended numeric collision suffixes.
Repeat `-c ENDPOINT` to join multiple servers, or combine it with `-s ENDPOINT`.
Text entered in chat goes to the rooms and local model. Text entered in rollout
stays local, starting or steering a local model turn without sending an IRC
message. Chat shows the same room messages and retained history for every
participant at every verbosity level, including each model's own sends.
Verbosity adds local rollout and diagnostic detail; it never hides chat.
`/server` reports hosting; `/server stop` stops only the listener and its accepted
peers. `/disconnect ENDPOINT` removes one outgoing connection; `/disconnect`
removes all outgoing connections, without stopping hosting. `/status` reports
roles and live connection state. Duplicate additions are no-ops; changing a
listener requires an explicit stop first. Removing a destination does not cancel
model/tool work or redirect old model replies into a different room. Starting a
server never publishes earlier private rollout or another room's history.

Commands change this process only. An unrelated `/config` edit preserves those
changes; editing a networking component deliberately replaces that component.
Nicks and hosted-room preferences can be set before networking is enabled.
`--no-listen` and `--no-client` independently suppress configured startup roles;
each conflicts with its positive flag. The printed resume command includes
enabled roles and these explicit absences, even after runtime changes.

Each message carries up to 4,096 UTF-8 bytes (at least 1,024 Unicode code
points); longer text splits at word boundaries where possible. Snajpagent's
extended IRC wire format permits 8,192-byte lines, including CRLF, advertised
as `LINELEN=8192`. Peers must support that extension for long messages.

## Trust and reference

Proactive compaction follows the selected model's usable context by default.
Under `[provider NAME]`, `auto_compact_input_tokens = auto` uses 90% of the
effective input budget, or a labelled 120,000-token fallback when unknown.
Explicit numbers remain fixed; `0` disables proactive compaction but not
hard-limit recovery. `/model` and `/status` show the effective threshold.

Under `[ui]`, `prompt` supports mode cases and separate clock components:
`{hour:02}:{minute:02}:{second:02}` gives the default chat clock. `{context:4}`
space-pads the whole percentage to four columns; widths never truncate.
Spinner settings starting with a space reserve a column; leading `\0` hides
it while idle. No whitespace is removed implicitly. See the
[prompt reference](design/interactive-io.md#prompt-identity-and-tab) for the
full syntax. Defaults start with a goal flag (`⚑` when active) and one activity
slot shared by model and tool work, with tools taking priority. Rollout then
shows the padded percentage and model identity; chat keeps its timestamp and
operator identity. Inactive slots and unused digits remain leading spaces.
Explicit pre-1.0 templates using `{time}` must replace it with clock components;
replace separate `{provider_spinner}`/`{tool_spinner}` fields with one
`{activity_spinner}`.

Tools run with your local permissions. There is no command approval sandbox.
IRC has no authentication or TLS: keep it on localhost or use a trusted,
separately secured network. Ordinary IRC clients receive operator status.

Submitted text is retained in shared plaintext `prompt_history` under the
dotdir. Session deletion does not erase it. Protect that file and your session
logs, and review terminal captures before sharing them.

Read `man 1 snajpagent` for all options, commands, configuration, and limits.
Without installing: `man -l ./snajpagent.1`. Internal pre-1.0 state formats are
not a stable integration API; use the process interface or IRC.

Implementation contracts are in [`design/`](design/), starting with
[`architecture.md`](design/architecture.md). The static site is in [`www/`](www/).
All first-party material is GPL-2.0-only; see [`COPYING`](COPYING) and
[`LICENSE_SCOPE`](LICENSE_SCOPE).
