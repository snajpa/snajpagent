<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Implementation status

## 0.9.0-wip

snajpagent is usable as an interactive terminal agent, but it is still marked
work-in-progress until the remaining release checks are finished. The
`design/` directory contains the current architecture notes for this codebase.

Release blockers are limited to external qualification work: live-provider
evidence with a real API key, advertised-platform terminal reruns, and archived
per-platform dependency-closure records.

Approximate release work remaining: **1%**. Approximate completed work:
**99%**.

| Area | State | Remaining estimate |
|---|---|---:|
| Clean-source gate | closed | 0% |
| Dependency/vendoring policy | no third-party implementation or upstream header source is vendored; system libcurl/Jansson plus the first-party non-shadowing Jansson ABI declaration shim are inventoried and enforced by `make depscheck`; the current-host executable closure is captured by `make depclosurecheck` | 0% |
| Bounded base, canonical JSON, event log, replay, and private storage | implemented for current scope | 0% |
| Level-zero session loop, one-turn mode, passive resume, and fixture path | implemented with regression coverage | 0% |
| Multi-cycle turn semantics, steering, queueing, and durable tool fences | implemented for current scope | 0% |
| Terminal composer and additive rendering levels 0..6 | live SIGWINCH resize/redraw, Ctrl-Z suspend/continue draft preservation, Linux PTY TERM/width evidence, and a Linux/macOS PTY capability surface are implemented and machine-checked; external advertised-platform evidence is tracked under final integrated qualification | 0% |
| Responses HTTP/SSE/API-key path | transport, exact response input-token count path, durable standalone manual native compaction, threshold-gated automatic compaction after completed turns, pre-response active-prefix automatic compaction, exact compact-window input/output count metadata, bounded create/count/compact retry handling, and durable provider-profile response-start captures are implemented; live provider evidence is tracked under integrated qualification | 0% |
| Process tools | non-PTY `exec_command`, immediate PTY execution, yielded non-PTY and PTY managed handles, bounded `write_stdin`, PTY startup/refresh sizing, one-active-process replay tracking, strict managed-process continuation gating, durable `process_closed` records, owner-loss recovery closure, direct-child kill fallback, and process-family timeout/closure leak regressions are implemented; advertised-platform crash/restart evidence is tracked under final integrated qualification | 0% |
| Strict `apply_patch` tool | first-party parser/matcher/installer and bounded model-visible diff preview implemented with focused regression coverage; advertised-platform filesystem evidence is tracked under final integrated qualification | 0% |
| Instruction discovery, metadata, relocation/archive/delete closure, and final lifecycle polish | bounded instruction discovery, turn metadata, explicit resume-time workspace relocation, local archive/unarchive, typed-confirmation delete, active delete-intent completion, and exact post-rename trash completion are implemented for the current scope | 0% |
| Integrated qualification, deletion pass, sanitizers/static analysis/live provider/filesystem/process evidence | state, dependency/vendoring, portability, source-size, and current-host dependency-closure audit gates, clang-release/sanitizer wiring, local loopback provider-transport evidence for create/count/compact, optional live-provider evidence harness, release-evidence bundle/check tooling with path-confined record validation, single-bundle and matrix evidence-checker self-tests, and matrix-level evidence verification are implemented; external live-provider, macOS/architecture terminal runs, and actual archived checked per-platform evidence bundles remain incomplete | 1% |

The next implementation work should proceed in this order unless a regression is
found first: external live-provider evidence with a real OpenAI API key, macOS/architecture terminal reruns, and archived checked release-evidence bundles
for concrete shipped binaries on the advertised release platforms.

### Source-size check

`make sizecheck` reports the current source size directly from the tree and
enforces the preferred/hard C/header limits plus the 2,000-line per-file review
trigger. The status document no longer mirrors those numbers.

The real provider path includes production non-PTY command execution, immediate
and yielded PTY command execution, managed process handles, bounded
`write_stdin`, strict durable managed-process closure gating, process-family
shutdown evidence, bounded AGENTS instruction discovery, explicit resume-time
workspace relocation, local archive/delete lifecycle closure, exact Responses
input-token counting, durable manual plus post-turn and pre-response automatic
native compaction with compact-window count metadata, bounded provider
retry/rate-limit handling, durable provider-profile response-start captures,
live terminal SIGWINCH resize/redraw, Ctrl-Z suspend/continue handling, Linux
TERM/width matrix evidence, Linux/macOS PTY capability gating,
state/dependency/portability/source-size/dependency-closure audit gates, local
loopback provider-transport evidence for create/count/compact, current-host
dynamic dependency-closure capture, current-host release-evidence bundle/check
tooling with path-confined record validation, final matrix-evidence
verification, an optional `make livecheck` harness for real-provider evidence,
and the first production `apply_patch` implementation with bounded
model-visible diff previews.

### Working

- everything listed as working in checkpoints 0.1 through 0.8: private durable
  sessions, hash-chained canonical events, disk-authoritative resume, multi-cycle
  turns, strict graph classification, steering/queue transactions, the
  normal-screen UTF-8 composer, strict configuration, durable preferences,
  bounded SSE and wire JSON, coordinate-based Responses reconciliation, usage
  accounting, credential admission/scrubbing, irreversible transport redaction,
  additive presentation through level six, disk-derived context projection,
  canonical create request/digests, and the bounded libcurl Responses transport;
- real-provider turns now build a separate canonical Responses input-token count
  request, call the provider count endpoint before `response_started`, and
  persist `count_method=exact`; fixture/local paths keep the conservative
  byte-sized admission value and persist `count_method=qualified_upper_bound`;
- the real provider create, input-token count, and native compact transports now
  share a bounded retry policy: at most two retries for release-qualified
  transport failures and HTTP 408/429/5xx responses before any semantic create
  stream body is accepted, with cancellable backoff and bounded delay-seconds
  `Retry-After` handling; `response_failed.retry_count` records create retries;
- every new `response_started` event now durably captures the compiled provider
  profile id, capability version, compiled model, create-request SHA-256, and
  count-request SHA-256 before provider streaming begins; replay validates those
  fields so an event log cannot silently switch profiles or counts mid-turn;
- dependency/vendoring state is now explicit and machine-checked: `DEPENDENCIES.md`
  records that no third-party implementation or upstream header source is
  vendored, `src/snj_jansson.h` prevents accidental `src/jansson.h` shadowing,
  `src/snj_jansson_abi.h` is inventoried as a first-party declarations-only
  fallback for minimal system-Jansson runtime roots, `make depscheck` enforces
  the policy, and `make depclosurecheck` validates the concrete current-host
  executable closure and can emit JSON evidence for release records; `make
  evidencebundle`, `make evidencecheck`, and `make releaseevidence` now define
  the complete single-platform evidence bundle flow, while `make
  evidencematrixcheck` validates a copied set of platform bundles before release
  matrix completion is claimed;
- idle `/compact`, threshold-gated post-turn automatic compaction, and
  pre-response active-prefix automatic compaction now run the
  durable native compact transaction when history has changed since the newest
  installed compact base: they build the bounded compact-source projection, build
  exact input/output count requests for the compact window, persist count method
  and count-request SHA-256 metadata, sync `compaction_started`, call the
  provider compact endpoint, validate the bounded `response.compaction.output`,
  and sync `compaction_completed`; ordinary future context projection installs
  that output exactly once before uncompacted suffix items, while the
  pre-response path stops the compact source before the active `turn_started`,
  installs the compact output during the active turn, then rebuilds and recounts
  the provider request before any `response_started` event;
- follow-on `response_started` events now carry the installed `compact_id` when a
  compact base is active, so replay lineage does not falsely claim a noncompact
  response context;
- the normal-screen composer now handles live terminal resize through a bounded
  `SIGWINCH` path that re-reads width and redraws the active draft, Ctrl-Z
  suspension now flushes input before restoring saved termios so resume does not
  immediately re-trigger a stale suspend byte, and bracketed-paste disablement is
  tracked separately from current redraw capability so a narrow resize cannot
  leave paste mode enabled; Linux PTY regression scripts verify resize and
  suspend/continue preserve the submitted draft, and a TERM/width matrix covers
  xterm, xterm-256color, vt100, TERM=dumb, and narrow fallback behavior;
- bounded Codex-like instruction discovery now runs exactly once before each
  accepted turn: global and project `AGENTS.override.md`/`AGENTS.md` files are
  discovered in precedence order, symlinks/nonregular files/invalid UTF-8 and
  per-file or aggregate limit breaches fail before provider I/O, `turn_started`
  records canonical path/byte/SHA-256 metadata, and the active turn projects the
  frozen in-memory instruction text to the provider request;
- `exec_command` has a real production non-PTY implementation using the
  configured absolute shell, a resolved absolute working directory, bounded
  stdin, bounded timeout, nonblocking stdout/stderr capture, process-group
  termination, and active-input pumping for cancellation;
- PTY `exec_command` is implemented on Linux and macOS hosts that provide the
  required PTY primitives; stdout and stderr are merged through the PTY stream,
  bounded, redacted, and returned as the stdout excerpt, and PTYs are opened
  with the current terminal size or the documented 24x80 fallback;
- yielded non-PTY and PTY `exec_command` calls now return a lowercase-hex
  managed process handle when the command remains live after `yield_ms`; one
  live managed process can be polled or fed with `write_stdin`, terminal
  follow-up results close the process-local handle state, PTY size is refreshed
  while the process is driven, and replay tracks the active handle until a
  terminal `write_stdin` result or durable `process_closed` record clears it;
- `write_stdin` validates the managed handle, bounded UTF-8 input, optional EOF,
  and optional `yield_ms`; it can deliver one or more input chunks to a
  still-running managed process, including a yielded PTY process, and either
  return another running result or the terminal process result; while a process
  is unresolved, context
  projection advertises only the matching `write_stdin` continuation and the app
  rejects final/refusal/zero-call/wrong-tool provider responses by closing the
  process before failing the turn;
- local lifecycle commands now cover idle `/archive`, `/delete` with typed
  8-hex confirmation, durable `session_archived`, `session_unarchived`, and
  `session_delete_requested` replay, active-list and `--last` omission of
  archived sessions, `-l` visibility for archived sessions, same-filesystem
  trash rename/removal for ordinary deletes, exact active delete-intent
  completion, and exact post-rename trash delete completion after validation of
  the carried final delete intent;
- `apply_patch` now has a first-party implementation for the version-1 frame,
  add/update/delete operations, strict relative path validation, duplicate-target
  rejection, UTF-8 and line-ending validation for update targets, exact and
  unambiguous hunk matching, `@start`/`@end` insertions, no-follow parent/target
  checks on POSIX platforms with the relevant primitives, same-directory staged
  writes, target revalidation before install, parent-directory sync, bounded
  model-visible diff previews, and bounded structured tool results;
- tool calls run sequentially only after a durable `tool_started` fence, and
  their structured results are committed before any level-one-or-higher tool
  result rendering;
- tool output excerpts keep first/last bytes without overlap, preserve the
  original byte count, and redact admitted credentials/configured secrets before
  persistence or rendering;
- failed, signaled, timed-out, cancelled, denied, not-run, outcome-unknown,
  running, patch-rejected, and I/O-failed tool outcomes share one validated
  result shape;
- focused tool coverage exercises stdout/stderr capture, failing exit codes,
  timeout killing, large-output preservation, delayed stdin delivery, basic PTY
  merged-stream capture, yielded PTY `write_stdin`, provider-secret redaction,
  provider-secret environment removal, yielded managed-process output,
  `write_stdin` completion, repeated stdin writes, unknown handle rejection,
  successful add/update/delete patches,
  bounded diff previews, preview truncation, ambiguous hunk rejection, path
  escape rejection, symlink-target rejection, validate-before-install behavior,
  managed-process closure results, process-family timeout/closure leak checks, Linux terminal TERM/width matrix
  checks, Linux/macOS PTY portability guards, restricted continuation tool schemas, and durable `process_closed` replay,
  instruction discovery precedence, invalid instruction rejection, metadata
  validation, active-turn instruction context projection, resume-time workspace
  relocation, archive/list/unarchive/delete lifecycle replay, and post-rename
  trash delete completion.

### Intentionally unfinished

Managed process state is still process-local and cannot reconnect to an
already-running child across an agent crash/restart. Replay now remembers the
active handle, context projection restricts the next request to the matching
`write_stdin`, and the app emits durable `process_closed` records before ending
a turn when an unresolved process must be abandoned. PTY execution now supports
yielded handles, `write_stdin`, and polling-based terminal-size refresh while the
process is driven. Process-family timeout/closure leak evidence exists, and the local fixture
path covers restart recovery for interrupted provider and tool boundaries.
Remaining release evidence is limited to external live-provider qualification,
macOS/architecture terminal reruns, and actual archived checked per-platform evidence bundles;
Linux PTY evidence now covers live resize/redraw, Ctrl-Z suspend/continue draft
preservation, and TERM/width fallback behavior, while `make portabilitycheck`
verifies the PTY implementation is no longer Linux-only.

Threshold-gated post-turn automatic compaction, pre-response active-prefix
automatic compaction, and exact compact-window input/output count metadata are
implemented for the current scope. The remaining Responses evidence is live-provider evidence and proof that the
active compaction policy is effective across pinned provider profiles; post-turn compaction is intentionally installed for future turns rather
than rewriting the already-completed response cycle.

The `apply_patch` implementation now emits a bounded diff preview in successful
tool results. The local regression path covers exact matching, path/symlink
rejection, validation-before-install, bounded previews, and ordinary POSIX
staged-write installation. It still does not claim an impossible multi-file
power-loss transaction; cross-platform filesystem evidence remains part of the
final advertised-platform qualification.

For real-provider runs, `input_tokens_bound` is now populated from the exact
Responses input-token count response before the durable `response_started` event.
For fixture and local non-provider tests, `input_tokens_bound` remains a
conservative byte-sized admission bound and is marked with
`count_method=qualified_upper_bound`. Opaque response replay items still fail
closed during projection until live pinned-provider captures prove the complete
create/count/compact and replay contract for the selected profile.

The source archive now includes machine-checkable release/audit gates: `make
portabilitycheck` verifies that advertised PTY support is capability-gated for
Linux and macOS, `make statuscheck` verifies that this file's percentage math
is internally consistent, `make sizecheck` reports and enforces live source-size
limits,
`tests/test_provider_transport` exercises the real libcurl create/count/compact
transport against a local loopback HTTP server, `make evidencebundle` and
`make evidencecheck` create and validate current-host release-evidence JSON,
`make evidencematrixcheck` validates the final copied multi-platform evidence set,
`make releaseevidence` adds the live-provider requirement when `OPENAI_API_KEY`
and network access are available, and `make sanitizercheck` plus `make releasecheck` provide
repeatable ASan/UBSan and clang rerun wiring for release qualification.

The deterministic fixture remains separately linked test code and is absent from
the production executable.
