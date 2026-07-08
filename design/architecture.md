<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Architecture

snajpagent is a single foreground terminal process. It does not rely on a
daemon or socket to keep sessions alive. Durable state is written to local event
logs, and resume reconstructs the active session from those logs.

## Runtime Loop

Each accepted user turn is projected into an OpenAI-compatible Responses API
request. Streaming events update the terminal as they arrive. A final answer,
refusal, or completed tool cycle closes the turn; it does not close the
session.

Tool calls are handled one at a time. Before a tool runs, snajpagent records a
durable start event. After the tool finishes, snajpagent records the bounded
result and sends that result into the next provider cycle.

## Storage

Session data is append-only at the event level. Records are synced so a later
`snajpagent -r` can rebuild the conversation, active tool state, and local
lifecycle state without depending on process memory.

## Provider

The provider layer targets the Responses API over HTTP/SSE. The base URL and
credential environment variable are configured in `config.ini`, which lets the
same binary talk either to OpenAI directly or to an OpenAI-compatible local
proxy such as codex-lb.

Hosted web search is exposed as a Responses request tool. There is no separate
helper binary for web search.

## Tools

The first-party tool surface is deliberately small:

- `exec_command` for shell commands, including yielded long-running processes.
- `write_stdin` for continuing a yielded process.
- `apply_patch` for strict file edits using the patch grammar.

Provider credentials and configured secret environment variables are removed
from child tool environments or redacted before output is persisted or shown.
