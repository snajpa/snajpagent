<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Changelog

## Unreleased

- Prefix the default IRC chat composer with the local `HH:MM:SS` prompt-open
  time and expose that value as the data-only prompt template field `{time}`.

- Use accepted IRC nicks in the prompt and local chat. Track live nick changes,
  preserve op status, and keep mentions, echo suppression, and model context
  current. Notify channel renames once without losing the input draft.

- Added a narrow `make stylecheck` to `make check` for source license headers
  and objective C/header whitespace invariants. It is read-only and leaves
  semantic readability to review.

- Add one plaintext prompt-history stream per dotdir, shared by chat and
  rollout with Up/Down and incremental Ctrl-R search. Make Ctrl-C preserve the
  cancelled line, print `^C`, open a clean prompt, and never exit. Replace the
  textual activity row with one configurable mode-aware prompt and independent
  goal, provider, and tool spinner fields; literal leading `\0` selects a
  zero-width inactive field while a space reserves a stable column.

- Split `make sizecheck` into independent production-C, production-header, and
  test-C budgets. Their soft/hard levels are 32,768/49,152,
  16,384/65,536, and 16,384/32,768 lines respectively. Production translation
  units are no longer counted or constrained; the 2,000-line per-file review
  trigger remains unchanged.

- Default the IRC model nick to `agent`. Make outgoing clients resolve nickname
  collisions in-place by trying numeric suffixes, and use the accepted
  per-server identities for echo suppression and mention recognition instead
  of reconnecting forever.

- Make `irc_send` the exclusive model-authored IRC transmission path. Final
  assistant and refusal text remains in the local rollout, and only a
  successful explicit room message satisfies the local-operator reply rule.

- Keep no-output IRC responses quiet, while giving explicit empty or oversized
  assistant messages and refusals one precise, durable model-facing correction
  without exposing it in ordinary operator output.

- Upgraded `models.json` to an explicit versioned provider/model registry that
  retains advertised token capacities, exact-count capability, one coherent
  learned canonical-byte/token pair, and lower typed-failure input ceilings.
  Exact Responses input counting now defaults to `auto`, with strict `true`
  and disabled `false`; definitive 405/501 endpoint absence is cached while
  HTTP 404 and other ambiguous or transient failures remain uncached and fail
  the count operation. Compatible response usage remains the exact rolling
  anchor, learned estimates remain visibly statistical, and an estimate alone
  cannot reject a sendable first request. Refresh resets count capability for
  a fresh probe and preserves observations only for identical provider
  source/protocol/model bindings, while uncached manually typed model names
  remain trusted and unchanged.

- Render conventional leading-pipe Markdown tables as aligned terminal grids,
  including inline styling, escaped/code-span pipes, and delimiter-selected
  alignment. Tables that do not fit become labeled vertical rows; malformed
  candidates remain readable literal Markdown and durable bytes stay exact.

- Made interactive conversation spacing block-based instead of paragraph-
  dependent. Submitted input and model output, and every completed model block
  and the next activity or input prompt, now have exactly one empty row between
  them. Existing trailing newlines are counted so headings, lists, quotes,
  fenced code, pipe tables, prose, and literal output cannot omit or multiply
  the boundary.

- Added source-bound provider/model context capacity throughout discovery,
  cache display, configuration, request accounting, compaction, status, and
  rollout prompts. The strict pre-release cache is now unversioned and retains
  nullable advertised limits; `[model-limit PROVIDER/MODEL]` supplies exact
  operator overrides. Hard-budget compaction remains active when proactive
  compaction is disabled, unknown output limits omit `max_output_tokens`, and
  a typed pre-output context rejection gets one replay-safe compact-and-retry
  recovery. Trustworthy rejection details durably lower a source-bound
  in-session safety ceiling that survives resume and is visible in `/status`.
  Rolling provider-usage anchors now recognize transcript growth before an
  unchanged trailing goal/process controller suffix, so multi-cycle tool turns
  stay in token-domain accounting instead of falling back to whole-request
  serialized size.
  Bytes and token bounds remain distinct, and rollout composers put bare `N%`
  immediately before the configurable status fields and `›|»`, with `0%`
  before compatible accounting and `?%` when only the hard budget is unknown.

- Replaced the terse exit `resume:` label with the capitalized bullet header
  `• You can resume this session with the following command:`. It uses the
  same bold-green lifecycle role as `• Compacted`, while the command remains
  uncolored on its own next line with no leading bytes.

## 0.98

- Added a two-field `META` as the sole compiled product-name/base-version
  source. Exact version-tag builds show `0.98`; other builds append Git's
  abbreviated source commit, and dirty builds are labeled. Startup orientation
  now omits the redundant model and labels its abbreviated session ID.

- Added bullet-prefixed rollout lifecycle milestones at baseline verbosity:
  `Compacted`, `Goal set`, and `Goal cleared`, with exact-once view catch-up,
  durable detail only at higher verbosity, and a dedicated semantic color.

- Put the exit resume command on the line immediately below a standalone
  `resume:` label. Both lines now begin at column zero for direct line
  selection and copying.

- Make Ctrl-C clear a nonempty active-turn draft without interrupting the turn;
  Ctrl-C on an already-empty active composer retains explicit interruption.

- Track the compiler and linker inputs behind every generated binary so a
  normal parallel build automatically replaces artifacts left by an ASan/UBSan
  build instead of linking instrumented objects without sanitizer runtimes.

## 0.9.0-wip

- Replaced per-event Responses compatibility fixes with one bounded protocol
  rule: strictly decode text, refusals, function calls, success, and failure;
  discard other `response.*` events and inert provider output without exposing
  it as assistant text or local tool calls.

- Repaired immediate Enter steering across provider output and managed
  commands. Interrupted visible prefixes now precede an explicit steer
  boundary and exact ordered user steer in the next request, the empty active
  composer is available again immediately, and public provider indexes may
  contain gaps for hidden items while malformed ordering retains a specific
  diagnostic. Managed commands are handed off alive with
  `reason=steering_handoff`; strict `write_stdin.terminate` lets the model use
  the existing bounded closure path. Tab remains non-steering future-turn FIFO
  queueing with explicit PTY and tmux regression coverage.

- Replaced the ordinary input label with stateful typographic prompts:
  `MODEL/EFFORT › ` starts a non-networked turn and `MODEL/EFFORT » ` adds to
  the active turn, while networked prompts use
  `OPERATOR_NICK@MACHINE_HOSTNAME` with the same single/double glyph contract.
  Terminal-unsafe selector characters are escaped only in presentation.

- Added append-only networked `chat` and `rollout` presentation views.
  `/chat`, `/rollout`, and empty Tab switch views without changing input
   routing, then print each unseen semantic item for the entered view once and
   in order before live output continues. Nonempty Tab retains completion,
   indentation, and active-turn queueing behavior.

- Stopped ordinary input editing from erasing and repainting the complete
  composer on every keystroke. Character edits and cursor motion now update in
  place, while output, status, and resize transitions retain structural redraws.

- Made terminal wrapping keep trailing punctuation on the preceding line and
  use hyphens, dashes, periods, commas, and similar closing punctuation as the
  following wrap opportunity through one shared Markdown/literal code path.

- Added exactly one copy/paste-safe resume command after every eligible
  session exit, including `/exit`, Ctrl-D/EOF, idle Ctrl-C, `/archive`,
  one-shot completion, post-session errors, and graceful SIGHUP/SIGTERM
  shutdown. It preserves config/dotdir provenance, pending one-turn and
  presentation overrides, and effective standalone or combined IRC roles
  without including prompts or secrets; `/delete` and pre-session exits do not
  print one. Network options can now precede an exact `--resume SESSION_ID`
  without misclassifying the ID as initial chat text.

- Added Codex-style bullets and visible separation to terminal model prose.
  Each paragraph starts with `• `, while wrapped continuation lines stay
  flush-left and stored, provider, redirected, and IRC bytes remain unchanged.

- Renamed IRC identity terminology consistently to nick: `-n` now has long
  form `--model-nick`, `-o` has `--operator-nick`, and their `[irc]` keys are
  `model_nick` and `operator_nick`. The superseded forms are not aliases.

- Added a post-compaction developer notice containing the current session's
  absolute `events.jsonl` rollout-log path. It is rebuilt for future provider
  requests and resume without altering the provider-produced compact output or
  its durable hash and token-count metadata.

- Made bare `-e` read its one-shot prompt from non-terminal stdin while
  preserving the established `-e -- PROMPT...` argument form.

- Changed command timeouts into non-destructive foreground handoff deadlines.
  A still-running command keeps working under its managed handle while the
  model is notified through the same continuation path used by urgent local
  steering and IRC mentions; only explicit interruption or lifecycle closure
  cancels it.

- Made single `-v` show every tool invocation, complete arguments, completion
  state, and result text in ordinary and networked modes. Tool stdout/stderr
  is retained durably without a tool-specific capture cutoff;
  `[tool] max_output_bytes` optionally limits terminal presentation only and
  defaults to unlimited (`0`).

- Required `exec_command` and `write_stdin` to select a positive
  `max_output_tokens` or explicitly use the configured default, which is 4000
  when absent. Complete redacted output remains durable; replay-stable model
  context uses a UTF-8-safe conservative token bound, independently of the
  terminal-only `max_output_bytes` setting.

- Made `[tool] max_timeout_ms` drive both the advertised `exec_command` schema
  and runtime validation. The default foreground handoff timeout remains
  disabled, and the model opts into a deadline per command.

- Made `exec_command` default to no foreground handoff deadline unless the
  model asks for a positive `timeout_ms` (or an operator configures a fallback). Ordinary
  `-v` compact tool output now shows `timeout=none` or the effective
  millisecond timeout before the command; networked mode now shows the same
  line at `-v`.

- Added default-on, presentation-only Markdown rendering for live streamed
  model text across arbitrary delta and UTF-8 boundaries, resumed assistant
  history, and non-operator model messages in the IRC transcript. The
  `markdown` setting in `[ui]`, `--markdown`, and `--no-markdown` control it
  without changing
  provider, durable, redirected, or IRC bytes; no-color mode retains readable
  structural rendering.

- Added first-class IRC agent/operator chat: `-s`/`--listen` hosts one bounded
  room, repeatable `-c`/`--client` connections join advertised rooms and
  reconnect autonomously, and combined roles share one durable session and
  timestamped non-windowed chat UI.
  Networked mode requires `-n`/`--model-nick`; `-o`/`--operator-nick` and
  `-r`/`--room-name` select the local operator and hosted room identities.

- Added durable room events and bounded first-join/post-compaction snapshots,
  `+o`- and mention-aware coalesced steering, managed-command IRC handoff, a
  one-shot local-operator reply reminder, and native `irc_send`, `irc_state`,
  and privilege-checked `irc_topic` tools. Socket, join, history, and reconnect
  work remains runtime-owned rather than model-polled.

- Added program-wide `--color[=auto|always|never]` and `--no-color` behavior
  with a terminal-safe 16-color semantic palette. Network verbosity 0 now
  presents room/operator traffic without local model or tool traces; `-v`
  reveals terminal model replies and complete tool activity, `-vv` adds
  commentary, and higher levels add lower-priority runtime/IRC diagnostics.

- Reassigned the `-c`, `-r`, and `-o` short options to client, room name, and
  operator nick. Their former config, resume, and effort functions remain
  available as `--config`, `--resume`, and `--effort`.

- Added permanent narrow-tmux rendered-screen regression coverage, including
  one production IRC server and two production clients using loopback fake
  Responses endpoints, plus an explicit serialized live terminal target that
  compares the configured provider run with durable response and `AGENTS.md`
  metadata.
- Fixed exact-right-margin composer redraws so VT pending-wrap state cannot make
  a later edit or resize erase the model-output row above it.
- Prevented the transient activity status from redrawing in the middle of an
  open streamed public item, and made post-steering output resume directly
  below the preserved draft without an extra wrap line.

- Made Codex-style project instruction discovery explicitly configurable with
  `[agent] read_agents_md`; it remains enabled by default so an applicable
  workspace `AGENTS.md` is supplied to the model automatically.

- Added a configurable `$HOME/.snajpagent` application directory containing
  default configuration, sessions, trash, and an atomically replaced model
  cache; `--dotdir` and `--config` override those defaults.

- Added ordered named provider configurations and authenticated all-provider
  model/reasoning discovery. `/model`, `/model list`, and `/model cache` expose
  the persistent user-refreshed catalog and its update time; numbered and
  typed selectors durably retain provider, model, and effort across resume,
  while manually entered model and effort names pass through without catalog
  validation. Codex API providers use their versioned `/models` catalog with
  visibility and priority semantics, while other providers retain `/v1/models`;
  snajpagent no longer imports Codex CLI cache state.

- Added durable persistent goals: `/goal TEXT` and quoted or explicit `set`
  forms start or reword an objective; status/help, pause/resume, lock/unlock,
  complete/cancel commands control it; `[agent] max_goal_prompt_bytes` bounds
  new wording; a strict no-unfinished-goal `create_goal` tool lets the model
  honor explicit goal-start requests without treating ordinary work or
  Markdown documentation as activation; and the mutually exclusive
  active-only `update_goal` tool lets the model rewrite unlocked wording,
  complete, or record a blocker. Successful model creation persists and arms
  the same continuation path as `/goal`. Normal finals continue the goal
  automatically after queued FIFO turns, while refusal, failure, input
  closure, and session reopening pause it safely. Goal wording is projected
  after replay/compaction, and managed-process gates hide both lifecycle tools.

- Added terminal-width word wrapping for streamed public model text while
  preserving exact stored and redirected bytes, plus a configurable
  `[ui] typing_pause_ms` interval that keeps live steering drafts readable as
  output resumes below composer snapshots.

- Added numbered `/queue` and `/q` views with indexed edit/delete actions,
  queue clearing, newest-item `pop`, and durable in-place edit replay.

- Added `/?` as an exact interactive alias for `/help`; both render the same
  centralized command catalog and key reference.

- Added Tab completion for interactive slash-command names from the same
  catalog rendered by `/help`, while retaining indentation and active-turn
  queueing for input outside a command-name token.

- Recovered safely from invalid managed-process continuations: the active
  handle is bound into the strict `write_stdin` schema, wrong handles become
  durable retryable not-run results without touching the real process,
  repeated invalid responses remain recoverable, malformed matching
  interactions retain process ownership, and genuine tool adapter failures
  clean runtime ownership before durable state advances.
  Terminal, wrong-tool, and multi-call ordering violations remain fail-closed.

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
- Added durable resume-time workspace relocation for explicit `--resume -C NEW`
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
- Added the implemented `/queue`, `/next`, `/status`, and `/history`
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
