<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Implementation status

Native session detach prints a copyable resume command. Saved workspaces print
their return command after terminal restoration, using the current full ID and
nondefault dotdir. Workspace agent detach shows :attach SESSION_ID after release.

Enterprise Codex personal access tokens use native `--with-access-token` login,
validated workspace metadata and the Codex backend. Managed tokens are reloaded
for each request and use the same Codex formatting/compaction/count behavior as
device login. Token rotation requires a new login; OAuth refresh is exclusive
to the existing device flows. Local HTTP tests exercise the real CLI/transport.

Remote tmux supports bidirectional native file transfers using its single writable
client focused on the agent pane. Checked envelopes preserve pane history while
other panes render; byte streams negotiate larger frames and stock Mosh retains
short title frames. The attachment profile includes TMUX/TMUX_PANE. Native file
drops over SSH and model send_file share this path; detached/control-mode or
ambiguous clients leave delivery unavailable. Existing owners require normal
exit/resume to load the updated native attachment draft and in-session code.

Completed bracketed pastes request their final composer redraw, including when
the closing marker is the last byte in a coalesced native input batch.

Session lists group attached, detached, running (attachment status unavailable)
and stored sessions under the STATUS heading, newest saved activity first
within each group. `-l` includes all running
sessions and up to 10 stored sessions; `-l N` sets the stored count, with zero
showing only running sessions. Resume pickers and `/session list` retain all
stored sessions. Listing remains read-only and preserves terminal width fitting.

Informational slash-command reports share the configured pager. Each invocation
collects its report before handing over the terminal; `/state` combines status
and goals, and `/history` reuses the existing event selection and renderer with
a thread-local report destination. Automatic resume history keeps its normal
display. Report capture excludes ordinary engine notifications and model output.
Verbosity queries use the engine report path; setting verbosity stays a local UI
control. Disabled/unavailable paging and startup failures retain direct output.
Pagers run asynchronously while provider turns, managed processes and IRC keep
progressing. Rendering retains incoming output until terminal ownership returns;
durable IRC records use exact journal event ranges across automatic checkpoints.
Model downloads stay queued during pager ownership.

The optional development Vim workspace provides named persistent workspaces,
session/conversation buffers, split panes, retained-history navigation/search,
retrospective verbosity, visual selection, mouse controls and clipboard copy.
POSIX workspaces control existing native owners; Windows runs one workspace-owned
engine and keeps it alive when its panes close. Explicit quit ends that engine.
Classic Tab/Shift-Tab cycles opened channels, direct queries and connection views
with independent drafts. Vim Tab changes pane focus; :bn/:bp changes buffers.
Runtime activation remains explicit and WITH_VM=0 omits the module. See the
[design](design/vim-mode.md) and [qualification scope](QUALIFICATION.md).

`snajpagent remote mosh HOST COMMAND...` separates Mosh options from the remote
command automatically, including commands with --resume or --attach. Leading
Mosh option values and explicit separators remain supported; abbreviated or
unrecognized Mosh options keep their native parsing and use an explicit `--`.

Provider HTTP requests explicitly use HTTP/1.1. The HTTPS ALPN regression
reproduces an HTTP/2 stream reset with the previous executable, then verifies
completion, no replay after partial output and rejection of an untrusted TLS
certificate. Existing transport retries and Responses validation remain intact.

Custom `[ui] prompt` templates expose `{operator}`, `{model_nick}` and
`{session_name}`. Nick fields follow accepted endpoint identities; saved names
survive resume and update on rename. Default prompts retain their existing layout.

Model-driven `select_model` choices now require an entry in the current model
cache, including its provider and effort. The tool's `cache` selector refreshes
provider catalogs and returns selectable rows; failed refresh leaves the prior
cache and model selection intact. Operator `/model` retains typed uncached
selection.

Version 0.99.8c includes client-only `snajpagent remote COMMAND…`
and native POSIX workstation file endpoints via `/receive` (into the agent)
and `/send PATH` (to the workstation). The wrapper prefers the verified fast
SSH byte stream; stock Mosh uses a nonce-negotiated title-state channel with
short checked frames and keyboard acknowledgements. Workstation configuration
reads only `[terminal] download_dir`, defaulting to `~/Downloads`. Verified
uploads become private unsent attachments; acknowledged downloads display actual
local-path receipts. The trzsz-go client remains usable with explicit transfer
commands. Model `send_file` journals export intent before delivery and tries
screen-state mode when an attached interactive client lacks the fast reply.
The saved remote-session queue supports stable-ID listing, removal and clearing,
including stale sources; read-only permits listing only. Detached GNU screen
exports remain pending without protocol frames. Fast-client reattachment
redelivers at idle boundaries; Mosh reattachment requires an explicit send for
older queued items. Changed sources and uncertain outcomes remain pending.
Native selector input accepts a regular-file path. Stock Mosh 1.4.0 transfers
passed over local loopback and from a Mac to a Linux VM, including `--no-init`
and default prediction, with exact bytes, receipts and prompt reuse. The
screen-state PTY relay covers loss, replay, cancellation and resize. Desktop
GUI drag and bulk-throughput qualification remain separate. See QUALIFICATION.md
for terminal/SSH scenarios and file sizes. Native remote mode, downloads and the
durable outbox are included in the 0.99.8c stable downloads.

Voice supports native Codex subscription calls and public Realtime BYOK.
The selected provider supplies credentials and defaults. Native media uses
libdatachannel/libjuice and Opus; coding handoffs wait for finalized transcripts.

Context-capacity recovery survives new-input handoff and replay, reducing further
complete prefixes after repeated rejection. Prefix measurement is incremental;
request/summary preparation checks cancellation and the local prompt shows pending
interruptions. Automatic compaction announces progress. Response decoding accepts
completed announcements and empty text placeholders alongside usable output,
retaining final/refusal validation and corrections for genuinely empty replies.

Model-limit rules support provider-neutral `reasoning_efforts` arrays for catalog
rows and selection, plus an `image_tokens` per-image ceiling that local media
bounding uses on any route when exact counting is unavailable. Configuration
overrides discovery without altering its cache; explicit efforts remain available.
Numeric capacity provenance stays independent.

Version 0.99.8c includes session-owned image, audio, video and document
assets, normalized image/page/frame input, audio API operations, local dictation
and playback, and realtime voice with durable handoff to the existing coding
turn/queue owner. Native generated-media, context/replay and voice transport
fixtures pass. The complete production matrix builds with device audio and
native voice backends. LibreOffice is never bundled: host builds resolve a
separately installed runtime, and release artifacts take the commanded engine
(`WITH_OFFICE=0` with `WITH_OFFICE_COMMANDS=1`).
Source builds can probe the compiler and the optional modalities with
`./configure`, which reports what it enabled or disabled and records the result
in `config.mk`.
AV/PDF/audio application cross-linking passes for FreeBSD 8.4/5.1,
OpenBSD 7.9/5.9/3.5 and NetBSD 10.1/2.0, with matching symbols and static
rendering/font libraries. The older SDKs use a static GCC 14 C++ runtime.
macOS ARM64 and Intel AV/PDF/audio applications also cross-link; combining their
executables and dSYMs preserves each original slice's bytes and matching UUID.
These diagnostics exclude Office; cross-linking does not qualify target-OS
execution, physical audio devices or live providers.
Target-OS execution, physical audio devices and live media-provider qualification
are separate from these local fixture and production-build results.

Current source includes the shared native tool contract: required operands and
optional controls, explicit legacy spellings with ambiguity rejection, truthful
byte-limit naming, and compact verbosity-1 rejection rows in live/history views.
Fixed policy is system-level, with an explicit host continuation boundary for
all providers. Goal-turn requests retain labelled conversation-level transport
input through instruction-hoisting gateways, including with retained history;
original goal events and explicit pauses stay intact. In-progress interactive and one-shot resume retain original tool
calls and mark unknown outcomes without rerunning them; empty stdin needs no new
prompt. User, tool and saved reasoning data retain their provenance. These
behaviors are included in 0.99.8c; older release assets remain unchanged.

IRC snapshots and steering are projected after complete tool exchanges, including
on replay of affected sessions. Interactive resume applies current startup
network roles and identity overrides before work starts. Existing journals retain
their original events and completed tool outcomes.

Development source opens existing native journals through verified checkpoints
and a bounded suffix. Routed IRC records retain connection/conversation identity,
generation, membership, actions, delivery receipts and captured replies. Native
checkpoints retain the IRC directory through canonical event references. New
session creation still uses the legacy log while native creation and conversion
are being integrated.

snajpagent is a pre-1.0 terminal coding agent. One interactive session supports
local rollout and native IRC chat. One-shot mode runs tasks from scripts.

Implemented:
- Shared-IRC worker coordination: one discovered server per host, workers
  joining it as clients, and background room traffic that waits for the active
  turn, so a peer joining, being opped or leaving never ends another session.
  `~/ai/tests/test_snajpagent_shared_irc.sh` covers host survival and discovery,
  fresh and resumed workers, `--no-listen`/`--no-client` independence and a
  worker hosting its own server alongside the shared one.
- Rule effects at the tool-call boundary now cover rejection and allowlists
  (first matching rule decides; a trailing match-all rule audits without
  deciding). Verified by `tests/test_rules.c` and `tests/rules_e2e.py`.
- Native exploration tools (list_files, read_file, grep) are declared and
  runnable in every turn; /ro remains inspection-only. New modification
  counterparts write_file (atomic whole-file create/replace) and edit_file
  (targeted exact replacement, unchanged file on mismatch) are workspace-relative
  and never follow symlinks. Verified by `tests/test_write.c`, the real-binary
  `tests/tools_e2e.py` suite (`make toolscheck`) and updated context/dispatch
  unit tests.
- Ordered `[rule NAME]` model tool-call filtering with JSON-pointer regex
  matching and allow/deny verdicts. Rejected calls answer a factual `rule_rejected`
  not-run result, journaled and replayed on resume; the engine is stateless and
  bounded, and configuration load rejects invalid definitions. Verified by
  `tests/test_rules.c` and the real-binary `tests/rules_e2e.py` suite
  (`make rulescheck`). Only the `out`/tool-call boundary is wired.
  See `design/io-rules.md`.
- Named providers and local model settings, shared secret sources, Responses
  streaming, model discovery, token accounting and native/fallback compaction.
- Reasoning content-part streams, including direct DeepSeek V4 Pro thinking and
  V4.1 Flash tool cycles/resume. The first completed message snapshot finalizes
  its streaming phase; later completed-phase conflicts remain errors.
  Completed plaintext/encrypted reasoning is durable provider-bound continuation,
  replayed with original tool-call pairing across cycles and resume, and included
  in compatible compaction input. It is excluded from public/history rendering.
  Compaction start/completion share that binding; overflow recovery after a
  binding change can rebuild an earlier complete prefix while retaining later
  history and preserving the previous output on interruption.
  Prompt-cache projection keeps timing, steering, recovery/correction and
  closure facts in conversation order. Each sent goal/display/process and
  work-note/banner snapshot is durably retained before its response; unchanged
  snapshots are deduplicated and the latest snapshot supersedes older facts.
  Fixed execution policy is invariant
  across ordinary continuations and IRC topology changes, including through
  instruction-hoisting gateways. Completed history retains its projection
  across turn boundaries until compaction.
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
