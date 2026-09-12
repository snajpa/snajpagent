<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Implementation status

Current source includes the shared native tool contract: required operands and
optional controls, explicit legacy spellings with ambiguity rejection, truthful
byte-limit naming, and compact verbosity-1 rejection rows in live/history views.
Fixed policy is system-level, with an explicit host continuation boundary for
all providers. In-progress interactive and one-shot resume retain original tool
calls and mark unknown outcomes without rerunning them; empty stdin needs no new
prompt. User, tool and saved reasoning data retain their provenance. These changes are development-source
behavior above stable 0.99.5; its downloadable assets remain unchanged.

snajpagent is a pre-1.0 terminal coding agent. One interactive session supports
local rollout and native IRC chat. One-shot mode runs tasks from scripts.

Implemented:
- Rule effects at the tool-call boundary now cover rejection, allowlists
  (`accept`), reusable chains (`jump`/`return`), pass-through logging, payload
  transform/override (`pass` with `value`, journaled as a `rule_transform`
  projection), policy insertion (`insert` with `to = model`) and trusted helpers
  (`command`, one strict JSON effect from stdout), plus fresh local consent
  (`confirm`: a generated challenge typed at the local terminal; non-interactive
  runs deny). Verified by `tests/test_rules.c` and `tests/rules_e2e.py`.
- Native exploration tools (list_files, read_file, grep) are declared and
  runnable in every turn; /ro remains inspection-only. New modification
  counterparts write_file (atomic whole-file create/replace) and edit_file
  (targeted exact replacement, unchanged file on mismatch) are workspace-relative
  and never follow symlinks. Verified by `tests/test_write.c`, the real-binary
  `tests/tools_e2e.py` suite (`make toolscheck`) and updated context/dispatch
  unit tests.
- Ordered `[rule NAME]` model tool-call filtering with JSON-pointer regex and
  integer-threshold matching, pass/accept/reject/jump/return verdicts and
  templated match logging. Rejected calls answer a factual `rule_rejected`
  not-run result, journaled and replayed on resume; the engine is stateless and
  bounded, and configuration load rejects invalid definitions. Verified by
  `tests/test_rules.c` and the real-binary `tests/rules_e2e.py` suite
  (`make rulescheck`). Only the `out`/tool-call boundary is wired;
  `replace`/`insert`/`confirm` and the `in`/`event` hosts remain future work.
  See `design/io-rules.md`.
- Named providers and local model settings, shared secret sources, Responses
  streaming, model discovery, token accounting and native/fallback compaction.
- Reasoning content-part streams, including direct DeepSeek V4 Pro thinking and
  V4.1 Flash tool cycles/resume. The first completed message snapshot finalizes
  its streaming phase; later completed-phase conflicts remain errors.
  Completed plaintext/encrypted reasoning is durable provider-bound continuation,
  replayed with original tool-call pairing across cycles and resume, and included
  in compatible compaction input. It is excluded from public/history rendering.
- Private durable sessions, replay, steering, queues, goals and read-only turns.
- Terminal presentation of provider citation blocks as one compact reference
  (`[cite: turn 0-2]`); unknown or malformed blocks and all redirected, durable
  and provider bytes remain exact.
- Session-local prompt-entry history, restored on resume and seeded once from a
  global archive. Orderly exit merges only newly entered lines; active sessions
  keep stable Up/Down and Ctrl-R history.
- Live verbosity/view snapshots in model context, with progress guidance based
  on effective tool visibility, including chat suppression and one-shot streams.
- One rendering contract across verbosity levels: a logical tool block or
  streamed-output burst parks and repaints the composer once, start and outcome
  rows share the same short call reference, and one dim `[…]` marks cut or
  hidden display content while complete output stays in the durable journal.
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
