<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Implementation status

snajpagent is a pre-1.0 terminal coding agent. One interactive session supports
local rollout and native IRC chat. One-shot mode runs tasks from scripts.

Implemented:
- Named providers and local model settings, shared secret sources, Responses
  streaming, model discovery, token accounting and native/fallback compaction.
- Private durable sessions, replay, steering, queues, goals and read-only turns.
- Independent tool-call batches, multiple managed command handles, bounded
  redacted output journals, native read/search and strict patch installation.
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
