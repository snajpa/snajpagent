<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Changelog

## 0.9.0-wip


- Hardened release-evidence integrity checks: bundle record paths are now
  canonical relative paths confined to the evidence directory, `make
  evidencetoolcheck` exercises both the single-bundle and matrix validators,
  and negative self-tests cover path escape, absolute references, missing
  required live/terminal records, duplicate platforms, version mismatches,
  unexpected platforms, and missing required platform coverage.

- Added release-evidence matrix validation: `make evidencetoolcheck` self-tests
  the matrix verifier, and `make evidencematrixcheck` validates copied
  per-platform evidence bundles for unique platform ids, consistent versioning,
  required Linux/macOS architecture coverage, terminal evidence, and live-provider
  evidence before a release matrix can be claimed complete.

- Added release-evidence bundle tooling: `make evidencebundle` collects source-audit, dependency-closure, and PTY terminal evidence for one concrete host; `make evidencecheck` validates the JSON bundle; and `make releaseevidence` adds the live-provider requirement for final platform evidence.

- Added `make depclosurecheck` and `tools/check_dependency_closure.py` to
  capture and validate the current-host dynamic executable dependency closure,
  rejecting unresolved dependencies or missing system libcurl/Jansson linkage and
  supporting JSON output for per-release evidence records.


- Repaired advertised-platform PTY support so immediate and yielded PTY
  `exec_command` now build through one `SNAJPAGENT_HAVE_PTY` capability surface
  on Linux and macOS instead of being Linux-only; added `make portabilitycheck`
  plus `QUALIFICATION.md` to keep external live-provider, macOS/architecture,
  and archived per-platform dependency-closure evidence explicit.

- Repaired the dependency/vendoring state: the tarball now ships `DEPENDENCIES.md`,
  `make depscheck`, a non-shadowing `src/snj_jansson.h` wrapper, and an
  inventoried first-party `src/snj_jansson_abi.h` declaration shim, making clear
  that no third-party implementation or upstream header source is vendored while
  system libcurl/Jansson remain the linked dependencies.


- Added local provider-transport qualification: `tests/test_provider_transport`
  exercises the real libcurl Responses create, input-token count, and native
  compact transports against a loopback HTTP server, validating request paths,
  bearer authorization, body delivery, SSE reconciliation, and compact-response
  parsing without contacting the external provider.
- Added `make livecheck`, an explicit real-provider evidence harness that
  requires `OPENAI_API_KEY`, network access, and provider quota; it runs an
  isolated one-turn session with automatic compaction and verifies
  count/profile/compact evidence in the durable event log.
- Added machine-checkable release-state gates: `make statuscheck` verifies
  that `IMPLEMENTATION_STATUS.md` percentages are internally consistent,
  `make sizecheck` reports and enforces live source-size limits, and
  `make sanitizercheck`/`make releasecheck` provide repeatable ASan/UBSan plus
  clang release-rerun wiring.
- Added Linux terminal TERM/width matrix coverage for xterm, xterm-256color,
  vt100, TERM=dumb, and narrow fallback behavior.
- Added human-facing project documentation and current architecture notes under
  `design/`.
- Split `src/app.c` into event/state, streaming helper, managed-process, lifecycle,
  and provider/tool dispatch translation units, closing the 2,000-line
  simplicity-review breach while keeping all source budgets below their hard
  maxima.
- Added a separate canonical Responses input-token count request and real-provider
  call to the Responses input-token endpoint before `response_started`; production
  turns now persist `count_method=exact`, while fixture/local counts persist
  `count_method=qualified_upper_bound`.
- Added bounded real-provider retry/rate-limit handling shared by Responses
  create, input-token count, and native compact requests: at most two retries for
  retryable transport failures and HTTP 408/429/5xx, bounded delay-seconds
  `Retry-After` handling, cancellable backoff through the active input pump, and
  persisted `response_failed.retry_count` for failed create cycles.
- Added durable provider-profile captures to `response_started`: the event now
  records and replay-validates the compiled profile id, capability version,
  model, create-request SHA-256, and count-request SHA-256 before provider
  streaming begins.
- Added live terminal resize and suspend/continue hardening: `SIGWINCH` now
  triggers a bounded width refresh and redraw of the active composer, Ctrl-Z
  suspension flushes raw input before restoring saved termios to avoid stale
  suspend re-delivery on resume, bracketed-paste cleanup is tracked separately
  from current redraw capability after narrow resizes, and Linux PTY regressions
  verify draft preservation across resize and suspend/continue.
- Added durable standalone native manual compaction for idle `/compact`: bounded
  compact-source projection, `compaction_started`/`compaction_completed` replay,
  OpenAI Responses compact transport, compact-output validation, compact output
  installation in later context projection, and response-start lineage via the
  installed `compact_id`.
- Added threshold-gated automatic compaction after completed turns and
  before active-turn provider requests, with `[provider] auto_compact_input_tokens`,
  exact compact-source and compact-output count requests on the real provider
  path, persisted count methods and count-request SHA-256 metadata, active-prefix
  replay coverage, fixture coverage for durable automatic compaction, and
  managed-process regression stabilization for yielded follow-up completion.
- Added the first production `exec_command` runner for non-PTY commands, with
  bounded stdin, timeout, stdout/stderr capture, process-group killing, and
  active-input cancellation.
- Added immediate and yielded PTY `exec_command` execution on Linux/macOS PTY-capable hosts,
  with one merged bounded redacted stream, startup/current terminal sizing,
  polling-based size refresh while the process is driven, and focused regression
  coverage.
- Added yielded managed process handles and bounded `write_stdin` for one live
  process, with regression coverage for delayed stdin delivery, PTY interaction,
  terminal polling, repeated stdin writes, and unknown-handle rejection.
- Added durable managed-process closure semantics: replay now records the one
  active process handle, context projection restricts unresolved-process cycles
  to the matching `write_stdin`, provider attempts to finish or call the wrong
  tool are failed after a durable `process_closed` event, and recovery closes
  owner-lost processes before ending the turn.
- Hardened process shutdown with direct-child fallback when a process group is
  not yet observable, and added process-family leak regressions for immediate
  timeout and managed-process closure paths.
- Added bounded Codex-like instruction discovery for global and project
  `AGENTS.override.md`/`AGENTS.md` files, with strict path/UTF-8/size/symlink
  rejection, `turn_started` path/byte/SHA-256 metadata, frozen active-turn
  instruction projection, and focused regression coverage.
- Added durable resume-time workspace relocation for explicit `-r -C NEW`
  follow-up execution, including `workspace_changed` replay validation and CLI
  coverage that the next `turn_started` uses the relocated workspace.
- Added local lifecycle closure for `/archive`, `/delete`, archived-session
  listing rules, active-session picker/`--last` omission, typed 8-hex delete
  confirmation, same-filesystem trash rename/removal, exact active delete-intent
  completion, and exact post-rename trash delete completion with focused store
  regression coverage.
- Fixed the fixture link recipe so fresh `make check` uses deterministic object
  ordering without relying on shell command substitution behavior.
- Added a first-party `apply_patch` implementation for version-1 framed patches,
  with add/update/delete operations, exact hunk matching, path escape rejection,
  symlink-target rejection, validate-before-install behavior, staged writes, a
  bounded model-visible diff preview, and focused regression coverage.
- Persisted tool results now cover succeeded, failed, signaled, timed-out,
  cancelled, denied, not-run, outcome-unknown, patch-rejected, and I/O-failed
  outcomes under one strict shape.
- Tool output capture now keeps non-overlapping first/last excerpts, tracks
  original byte counts, and redacts admitted credentials/configured secrets
  before persistence or rendering.
- Kept external live provider evidence, macOS/architecture terminal reruns, and
  final archived dependency-closure records explicitly unfinished instead of
  claiming release-complete behavior.

## 0.8.0-wip

- Added the first production OpenAI Responses HTTP/SSE transport using libcurl,
  with bounded request bodies, bounded response/error bodies, cancellation via
  the active terminal pump, and no fixture fallback in production.
- Routed level-six diagnostics through irreversible request/response header
  redaction and level-five diagnostics through redacted canonical request bodies
  after the durable `response_started` fence.
- Accepted provider-native message snapshots that omit snajpagent phases; streamed
  text remains visible immediately, the final assistant message becomes terminal
  when no tool call follows, and pre-tool text remains commentary.
- Made streamed partial-output durability independent of the final response graph
  by carrying provider item identity through the streaming callback.
- Kept production tools fail-closed while provider transport, parser, and UI
  wiring move into the real binary.

## 0.7.0-wip

- Added verified event-log traversal for disk-derived context projection without
  mutating live session state.
- Added canonical model-input and Responses create-request projection from
  durable events, including user turns, steering, assistant speech/refusals,
  reasoning summaries, tool calls, tool results, and failed/interrupted-turn
  host outcomes.
- Replaced placeholder request digests with SHA-256 over the actual bounded
  canonical model input and create-request JSON.
- Added the 32 MiB request/projection boundary and level-five request-body
  rendering after the durable `response_started` fence.
- Added exact steering-snapshot verification for projection and focused context
  coverage for multi-turn `ping`/`pong` history.
- Added a GPL-2.0-only local Jansson ABI header plus a runtime-library linker
  fallback for hosts without Jansson development headers.

## 0.6.0-wip

- Added one bounded incremental SSE parser and strict provider-wire JSON loader.
- Added coordinate-based Responses reconciliation for assistant text, refusals,
  multipart messages, and function calls; repeated identical deltas remain data,
  and empty terminal output cannot erase earlier verified speech.
- Added bounded provider usage parsing, consistency validation, durable usage
  records, and level-three usage presentation.
- Added the ephemeral `OPENAI_API_KEY` admission/scrubbing boundary.
- Added irreversible JSON/header/URL/body redaction primitives and renderer gates
  for levels five and six, including the mandatory exposure warning.
- Added focused framing, reconciliation, usage, credential, redaction, and
  presentation tests while leaving the production provider fail-closed.

## 0.5.0-wip

- Added one strict, bounded configuration file with exact sections/keys,
  symlink and malformed-input rejection, timeout validation, shell checks, and
  validated secret-environment names.
- Added durable `/model` and `/effort` preferences plus process-local
  `/verbose`; resume CLI overrides now apply to exactly one turn.
- Implemented additive verbosity routing through level four: compact/complete
  tools, runtime facts, and post-sync event records without leaking raw state.
- Moved tool rendering behind its durable start/result fences and made recovery
  output composer-aware.
- Hardened root-event replay and added focused config, replay, CLI, and PTY
  conformance coverage.

## 0.4.0-wip

- Replaced the line reader with a bounded normal-screen UTF-8 composer shared
  with the output renderer.
- Added code-point editing, multiline input, bracketed paste, bounded history,
  Enter steering, Tab queueing, Ctrl-C interruption, suspend/resume, and redraw.
- Made terminal and actual SIGINT cancellation converge on the same durable
  `response_interrupted`/`turn_interrupted` transaction.
- Added buffered terminal reads so input following one completed action is never
  discarded, plus exact-limit admission and history-draft restoration.
- Added terminal-safe rendering and a no-ANSI line fallback for `TERM=dumb` and
  narrow terminals.
- Added PTY coverage for native Tab queueing, active interruption, multiline
  input, bracketed paste, and the existing recovery/steering paths.

## 0.3.0-wip

- Added durable in-flight steering with exact ordered consumption, response
  cancellation, completion-race precedence, and exact delivered-prefix records.
- Added the bounded durable future-turn FIFO, atomic queue cancellation and
  consumption, same-process automatic continuation, and restart-paused `/next`.
- Added the implemented `/queue`, `/unqueue`, `/next`, `/status`, and `/history`
  paths, including literal-slash queue admission.
- Made persisted response-graph parsing deterministic and removed invented
  runtime identities from replayed tool and opaque items.
- Tightened partial-public event validation and terminal-safe UTF-8 rendering.
- Added focused PTY coverage for steering, split-code-point cancellation,
  automatic FIFO continuation, and reboot-style passive queue resume.

## 0.2.0-wip

- Added the bounded canonical response graph and one complete-graph classifier.
- Added multi-cycle turns, ordered tool batches, replay-verified action
  digests, durable action boundaries, and restart-safe unfinished-tool
  terminalization.
- Added correct minimal routing for commentary, reasoning summaries, terminal
  answers, and refusals in interactive and `-e` modes.
- Added protocol-conflict neutralization: calls beside terminal speech are
  durably marked not-run and cannot execute.
- Made event commits transactional by validating a staged semantic state before
  append and sync.
- Added focused graph, tool-cycle, conflict, and tool-owner-loss coverage.

## 0.1.0-wip

- Began a clean implementation.
- Added bounded primitives, canonical JSON, and a durable hash-chained event log.
- Added private session storage, locking, multi-turn level-zero I/O, passive
  resume/listing, and the one-turn durable stdout fence.
- Added split-UTF-8 streaming, orderly provider-failure terminalization, and
  passive restart-safe recovery for the implemented response-cycle states.
- Added a separately linked deterministic provider fixture; production bytes
  contain no fixture reply or provider fallback.
