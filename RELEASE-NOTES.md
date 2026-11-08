<!-- SPDX-License-Identifier: GPL-2.0-only -->

# 0.99.9

Indexed native sessions, independent goals and timers, split-pane workspaces,
project roles and skills extend the terminal workflow. Session monitoring,
remote transfers, provider recovery and context handling have also changed.

## Sessions and recovery

- New sessions use indexed native journals with paired state and provider
  checkpoints. Resume reads the retained working set and a verified history
  suffix. History remains available for inspection and context reconstruction.
- `snajpagent convert --jobs N` converts stopped legacy sessions, preserves their
  original journals and reports individual results. `snajpagent optimize` upgrades
  stopped sessions and compresses history losslessly. Both skip running sessions.
- Session listings cache verified metadata, keeping the session picker and
  `snajpagent top` responsive while history changes. The monitor adds sorting,
  field selection, saved layouts and explicit live-owner termination controls.
- `snajpagent send SESSION_ID -- "prompt"` submits work from another shell while
  preserving the attached terminal and draft. Receipts distinguish acceptance,
  queuing, model-free discard and an uncertain outcome.
- An unavailable saved working directory moves to home before model work resumes.
  The session retains its goals, commands and an explanation of the recovery.

## Goals, commands and project guidance

- Repeat `/goal TEXT` to maintain independent goals. `/goal` lists their states;
  pause/resume can address all unfinished goals or a selected ID. Work continues
  while any goal is active, with queued prompts taking precedence.
- Model timers have independent IDs and support one-shot or recurring schedules.
  Missed recurring intervals coalesce when the session resumes processing.
- Session-owned pinned PTY jobs retain definitions and explicit resume hooks.
  `/r` lists active calls and jobs; inspection, input and stop controls preserve
  their identity across turns. A stopped job's resume hook is disabled.
- `work_context` enters and leaves temporary project roles without losing the
  broader conversation or model selection. Project and configured skills provide
  reusable guidance; activation, replacement and unloading retain provenance.
- `/model none` and `-m none` keep local controls and IRC available without model
  activity. Model-bound input received in that mode is discarded.

## Terminal and remote work

- `snajpagent vm` provides saved split-pane workspaces, session selection,
  retained-history navigation, search, selection and clipboard copy. POSIX panes
  run the ordinary terminal frontend and share the session's draft and undo
  history. The Windows workspace retains its single-live-agent limitation.
- Detach, attach and interactive resume preserve terminal ownership and drafts.
  Interactive resume confirms takeover of an occupied classic terminal. Native
  macOS attachment deadlines account for sleep without expiring the session.
- Command reports use the configured pager while managed work and IRC continue.
  Fenced code displays its literal contents for copying, and restored sessions
  retain their selected view and verbosity.
- Native workstation transfers support SSH, Mosh and focused remote tmux panes.
  Detached downloads retain queued intent; completion requires verified transfer
  acknowledgements. Mosh batching reduces title-channel exchanges while keeping
  protocol frames out of ordinary input and scrollback.
- Hosted IRC accepts up to 1,024 remote connections, subject to the process's
  file-descriptor limit. Exact private/channel routing survives recovery;
  `irc_sleep` and `irc_compact` control model-context admission.

## Providers and tools

- Meta subscription login and direct DeepSeek V4 Pro Responses support join the
  existing provider paths. DeepSeek reasoning continuity survives tool calls
  and resume within its originating model and account.
- Configured automatic fallback applies to one failed API call; subsequent calls
  return to the primary model. Named policy-refusal codes can use that fallback;
  other provider-specific policy families remain terminal. Without an eligible
  configured fallback, policy rejections retain their diagnostic and pause
  active goals.
- Context preparation retains completed tool results and pending input across
  compaction, model changes and recovery. Rate-limited native compaction can
  recover a smaller-model turn through durable history.
- Configured MCP Streamable HTTP servers expose enabled, non-denied tools for
  direct execution with OAuth credentials. `deny_tools` withholds tools and
  `read_only_tools` selects availability during read-only turns. Transport loss
  after dispatch reports an unknown outcome without automatically replaying the
  tool call. The former per-call approval queue has been removed.
- PDF builds support the pinned Poppler API variants. macOS PDF rendering uses
  native fonts when the implicit Fontconfig configuration is absent. Windows
  credential inspection and reuse retain owner/ACL checks for Unicode paths.

## Upgrade and compatibility

Keep backups before offline conversion or optimization. Release 0.99.8c uses
legacy JSONL storage and cannot open native sessions. Running owners retain
loaded code until deliberately exited and resumed; use compatible executables
for terminal attachment and workstation wrappers.

Filesystem writes use kernel-managed asynchronous writeback. Successful writes
and atomic renames are immediately visible, but abrupt failure can lose the
newest tail. Resume restores a verified prefix; logical acknowledgements do not
promise power-loss persistence.

The current production matrix has 18 OS/architecture/ABI targets. Choose the
exact variant and verify its SHA-256 before installation. Experimental platforms
retain their labels and recorded runtime-qualification limits. macOS downloads
are not Developer ID signed or notarized; the manual describes the per-file
quarantine exception for a verified executable. Office rendering requires a
separately installed compatible LibreOffice runtime.

The changelog contains the complete change list. `QUALIFICATION.md` records
runtime and fixture scope; building an executable does not establish runtime
qualification on every supported operating system. Physical audio devices,
live-provider behavior and legacy operating systems retain their separately
recorded testing boundaries.

## Downloads and updates

Release companions include matching symbols, the manual, application and
required dependency sources, licensing notices and `SHA256SUMS`. Executables
run independently of their symbol files.

Official stable builds use `latest` and default automatic updates to on. An
update replaces the executable for its next launch; existing processes retain
their loaded code. Git-suffixed development builds use `latest-dev` and default
updates to off. Previously published tags and asset bytes remain immutable.
