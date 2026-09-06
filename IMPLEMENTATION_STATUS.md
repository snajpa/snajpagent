<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Implementation status

snajpagent is a pre-1.0 terminal coding agent. One interactive session supports
chat and rollout views with optional live IRC connections; one-shot execution
and local work remain supported.

Implemented:
- Named providers and local model settings, shared secret sources, Responses
  streaming, model discovery, token accounting and native/fallback compaction.
- Private durable sessions, replay, steering, queues, goals and read-only turns.
- Independent tool-call batches, multiple managed command handles, bounded
  redacted output journals, native read/search and strict patch installation.
- UTF-8 editing, resize/suspend recovery, Markdown, local verbosity and explicit
  IRC destinations. Config edits reload without restarting the session.

Known boundaries:
- Managed child processes cannot be reattached after an agent crash; replay
  closes lost handles rather than pretending that execution can resume.
- Patch validation precedes installation, but multiple file replacements are
  not an all-files power-loss transaction.
- Linux and macOS PTY support share one capability-gated implementation.
  Local tests do not establish every platform, architecture or live-provider
  combination; see [qualification](QUALIFICATION.md) for remaining release work.

Use `make check` for existing local regressions, `make sanitizercheck` for
ASan/UBSan, and `make sizecheck` for live source counts and unchanged budgets.
No completion percentage is inferred from those checks.

Documentation ownership: [README](README.md) introduces use;
[snajpagent(1)](snajpagent.1) defines syntax, settings and defaults;
[design](design/architecture.md) records invariants;
[CHANGELOG](CHANGELOG.md) records history.
