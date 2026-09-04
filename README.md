<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent

snajpagent is a fast, lightweight, networked coding agent for power users.
It is built for autonomous development and integration with other software.

[![snajpagent working in a terminal](www/screenshots/ordinary.png)](www/screenshots/ordinary.png)

It runs in the foreground, streams model output, executes tools, and stores
durable local sessions. Agents and operators can also share one IRC room.

## Build

You need a C11/POSIX system, make, libcurl, and Jansson.

```sh
make
make check
sudo make install
```

`PREFIX` defaults to `/usr/local`. The install target adds the binary and the
`snajpagent(1)` manual.

`make check` includes `make stylecheck`: a small read-only check for source
license headers, clean C/header whitespace, and final newlines. It does not
rewrite code or substitute mechanical formatting for review.

## Configure

snajpagent reads `$HOME/.snajpagent/config.ini` by default.

```ini
[agent]
provider = openai
model = gpt-5.5
reasoning_effort = medium

[provider openai]
base_url = https://api.openai.com
api_key_env = OPENAI_API_KEY
exact_token_count = auto
```

Set the named credential in the environment:

```sh
export OPENAI_API_KEY='...'
```

Additional named providers can follow in the same file; the first is used when
a model selector omits the provider. Use `--config FILE` for another config or
`--dotdir DIR` for another private state directory.

`exact_token_count = auto` prefers the provider's Responses input-token API and
falls back only when the endpoint is definitively unsupported. `true` makes
exact preflight strict; `false` disables it. Completed response usage and typed
capacity failures can teach the source-bound model cache a conservative
byte/token estimate without treating bytes or statistical values as exact token
counts.

## Use

Change to a project and start the agent:

```sh
cd project
snajpagent
```

Enter submits a turn. During a response, Enter steers it at the next safe
boundary and Tab queues a future turn. Ctrl-J inserts a newline. Ctrl-C clears
a draft before it interrupts a turn or exits; five consecutive presses within
two seconds exit from any composer state. `/help` shows all keys and commands.

Useful commands:

```text
/help                 commands and keys
/status               current session and model state
/config               edit and reload configuration
/model                 list cached models
/model cache           refresh the model catalog
/effort LEVEL          change reasoning effort
/goal TEXT             start autonomous goal work
/queue                 inspect queued turns
/compact               compact model context
/exit                  preserve the session and exit
```

`/model` and `/model list` read `$DOTDIR/models.json` offline and show when it
was last refreshed. `/model cache` explicitly discovers every configured
provider and atomically updates this versioned provider/model registry,
including advertised token capacities and source-bound learned accounting.
There is no TTL; the user decides when to refresh it.

Use `/model NUMBER` or `/model #NUMBER` to pick a displayed row. Typed selectors
accept `MODEL`, `MODEL / EFFORT`, or `PROVIDER / MODEL / EFFORT`, ignoring
whitespace around `/`. A typed model name is trusted and sent unchanged even
when absent from the cache; snajpagent warns but does not validate or reject it.

The terminal renders model Markdown and uses color when supported. Override
those defaults with `--no-markdown` and `--color=auto|always|never`.

## Resume

Every normal session exit prints a ready-to-run resume command. You can also
list or select sessions directly:

```sh
snajpagent -l
snajpagent --resume --last
snajpagent --resume SESSION_ID
```

Sessions are append-only event logs under `$HOME/.snajpagent/sessions`.
An active goal and queued turns pause on reopen; use `/goal resume` and `/next`.

## Automate

Pass one prompt and return the final model text on stdout:

```sh
snajpagent -e -- "run the tests and summarize failures"
printf '%s\n' "review the current diff" | snajpagent -e
```

The same process interface works in scripts. IRC provides the network
interface; internal pre-1.0 event files are not a stable API.

## Network

Start a local server and its agent:

```sh
snajpagent -s
```

Connect another agent and operator:

```sh
snajpagent -c localhost:6667
```

The model nick defaults to `agent`; the operator nick defaults to a valid
`USER`, such as `root`. Override them with `-n` and `-o`. If a preferred nick
is already present, the client follows IRC convention by trying numeric
suffixes such as `agent1` or `root1` on the same connection.

The server owns one room. Human IRC clients receive operator status; model
nicks do not. Operator messages and direct mentions steer the addressed model.
Model response text stays in the local rollout; only `irc_send` posts
model-authored messages or notices. The runtime handles joining, bounded
history, and reconnects. Models may remain
quiet after peer chatter by returning no actionable output. An explicit empty
or oversized assistant message receives one terse model-facing correction and
is retried without exposing that correction as room chat or operator output.

`-c` is repeatable. `-s ENDPOINT` selects the listener and may be combined with
outgoing `-c` connections. Networked mode starts in chat view; Tab on an empty
draft switches between chat and local rollout.

## Reference

Read the installed manual with `man 1 snajpagent`, or render the source with:

```sh
man -l ./snajpagent.1
```

Implementation contracts live in [`design/`](design/). Start with
[`architecture.md`](design/architecture.md), then see
[`model-cache.md`](design/model-cache.md),
[`interactive-io.md`](design/interactive-io.md),
[`goals.md`](design/goals.md), and [`irc-chat.md`](design/irc-chat.md).
The existing checks live in [`tests/`](tests/).

All first-party material is GPL-2.0-only. See [`COPYING`](COPYING) and
[`LICENSE_SCOPE`](LICENSE_SCOPE).
