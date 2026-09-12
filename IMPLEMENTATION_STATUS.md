<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Implementation status

snajpagent is a pre-1.0 terminal coding agent. One interactive session supports
local rollout and native IRC chat. One-shot mode runs tasks from scripts.

Implemented:
- Named providers and local model settings, shared secret sources, Responses
  streaming, model discovery, token accounting and native/fallback compaction.
- Reasoning content-part streams, including direct DeepSeek V4 Pro thinking and
  V4.1 Flash tool cycles/resume. The first completed message snapshot finalizes
  its streaming phase; later completed-phase conflicts remain errors.
  Completed plaintext/encrypted reasoning is durable provider-bound continuation,
  replayed with original tool-call pairing across cycles and resume, and included
  in compatible compaction input. It is excluded from public/history rendering.
- Private durable sessions, replay, steering, queues, goals and read-only turns.
- Session-local prompt-entry history, restored on resume and seeded once from a
  global archive. Orderly exit merges only newly entered lines; active sessions
  keep stable Up/Down and Ctrl-R history.
- Live verbosity/view snapshots in model context, with progress guidance based
  on effective tool visibility, including chat suppression and one-shot streams.
- Parameter descriptions and current runtime settings in provider requests;
  actionable argument diagnostics and explicit requested/applied output limits,
  with latest-batch host feedback preserved across tiny output budgets.
- Independent tool-call batches, multiple managed command handles, bounded
  redacted output journals, native read/search and strict patch installation.
- Width-constrained Markdown table grids with styled cell wrapping and framed
  narrow fallbacks; streamed/replayed source text stays exact.
- UTF-8 editing, resize/suspend recovery, Markdown, local verbosity and explicit
  IRC destinations. Config edits reload without restarting the session.
- Shared active/idle command admission, durable deferred controls, responsive
  catalog/compaction waits, command handling in queue edit/delete confirmation,
  and retained submitted-command transcript lines.
- History totals before/after replay; default one retained turn, including
  unfinished work. Queued prompts render once at dispatch with effective settings
  and a fresh display clock while original input provenance remains fixed.

This describes current source. Downloadable releases and their manuals reflect
their tagged revisions; source fixes do not replace immutable release assets.
Existing sessions keep the code loaded at startup until normal exit/resume.

Known boundaries:
- Managed child processes cannot be reattached after an agent crash; replay
  closes lost handles.
- Patch validation precedes installation, but multiple file replacements are
  not an all-files power-loss transaction.
- Linux and macOS PTY support share one capability-gated implementation.
  [Qualification](QUALIFICATION.md) records tested configurations and outstanding
  platform and live-provider coverage.

Use `make check` for existing local regressions, `make sanitizercheck` for
ASan/UBSan, and `make sizecheck` for informational source counts.

Documentation ownership: [README](README.md) introduces use;
[snajpagent(1)](snajpagent.1) defines syntax, settings and defaults;
[design](design/architecture.md) records invariants;
[CHANGELOG](CHANGELOG.md) records history.
