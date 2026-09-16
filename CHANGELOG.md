<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Changelog

## Unreleased

- Omit `tool_choice` from the compaction summary request. The request declares no
  tools, so the choice was inert, but a provider that accepts only `"auto"` (Meta
  Model API) rejected `"none"` with HTTP 400 and failed the whole turn, including
  its derived input-token count request. Tool-less requests never send a tool
  choice now, and the terminal suite fails if one does.

- Finish all requests and reap the local server in the session-header transport
  test, allowing test runners to close their output pipes after completion.

- Complete context recovery across fresh chat, queued input and session resume,
  including repeated reductions of a large retained history. Avoid quadratic
  compact-prefix encoding, make preparation cancellable and show pending Ctrl-C
  feedback. Announce automatic compaction and accept completed message
  announcements with harmless empty placeholders while preserving real errors.

- Preserve prompt-cache prefixes through command yields, result collection and
  new input. Keep changing process snapshots, input timing and tool-limit
  feedback in labelled conversation data, with command-settlement rules in
  fixed policy, so instruction-hoisting gateways do not move those facts ahead
  of the retained conversation.

- Recover oversized context after changing provider/model/account binding by
  compacting a smaller complete history prefix. Keep later history and the
  previous compact output until recovery succeeds, and validate the binding
  throughout compaction instead of rejecting the smaller prefix as an invalid
  journal transition.

- Advertise the complete native tool catalog in every provider request. Runtime
  policy and availability return factual tool results for read-only calls,
  unavailable routes and other current constraints.
- Add durable one-shot timer calls with replacement and cancellation, and admit a
  fresh ordinary turn when a timer is due without resuming a blocked or paused
  goal. Add model-controlled IRC endpoint connect, host and disconnect calls;
  the runtime retains ownership of connection, room and reconnect activity.
- Record a content-free shape diagnostic for malformed Responses SSE records:
  event name, byte count, hash prefix, JSON class and capped top-level keys,
  while retaining strict parser rejection and never storing payload values.

- Keep automatic goal requests in the provider conversation when gateways lift
  instruction messages into policy, preventing metadata acknowledgments from
  replacing the current task. Preserve goal history, explicit pauses, pending
  tool results and the existing reasoning continuation boundary.

- Configure model-picker effort choices with `reasoning_efforts` in existing
  model-limit rules. Ordered provider-neutral lists override discovery metadata,
  survive cache refresh, and leave explicit effort selection available.

- Hold the composer after a submission until the submitted turn reports processing, so a
  ready-looking prompt no longer appears under the line just sent before anything could act on
  it; keys typed in that window are buffered and appear with the composer. An empty or
  slash-command or display-only submission, and a turn that never reports activity (after a bounded delay),
  still restore it immediately.
- Send the session identity to the provider in the header the proxy keys prompt-cache affinity on,
  so consecutive requests of one session can reuse the provider's cached prefix instead of being
  pinned per request; the identity is derived from the session and covers the compaction request too.

- Report the session and program usage counters in a compact form (`1.0k`, `807.4M`) and hold the
  throughput figure back until a turn has run for a second, so a long resumed session can no longer
  fill `/status` with nine-digit numbers that read as noise.

- Keep ordinary chat on its selected destination after a topology change: the
  terminal cached the destination's revision, so an endpoint that returned with
  a changed advertised room left every later typed message refused as
  "destination 1: not performed: unavailable or changed" and the prompt chip
  reading `[1 unavailable]` until the number was reselected. A number now names
  the endpoint, and the selection adopts the fresh target whenever the endpoint
  republishes its topology; frozen model requests still fail individually
  against their own revision.
- Send a stable prompt cache key on every request, derived once from the session, provider, model
  and profile, so a provider can reuse a session's cached prefix; the compaction request, the
  largest request a session sends, carried no key at all before this.
- Record prompt-cache reuse: the live provider path reads
  `input_tokens_details.cached_tokens`, the durable form keeps it when the provider reports it,
  and each session totals cached and uncached input, output, reasoning and total where a
  completion is applied, so a resumed session derives them from its own journal; each program
  totals the completions this process saw, so the two differ after a resume.
- Report throughput in `/status`: tokens per second for the running turn and for the most recent
  response, with a rate fine enough that a slow stream does not read as zero.
- Guard the caching contract: a request-prefix test requires the previous request's conversation
  head to stay byte-identical, and the suite's tree check fails closed if any request path builds
  a provider envelope without the cache key.

- Name a provider-issued tool call by the provider's own call id on both sides
  of the   request. A call carried that id only when the section being built
  matched its   continuation scope, so a scope change between requests could
  send the call under one   id and its result under another, and the provider
  then rejected the next request with   HTTP 400 "No tool output found for tool
  call"; both sides now use the recorded   provider id whenever the item carries
  one.
- Make a refused store transition explain itself: the error now names the failing
  clause, the call it concerned and the result status, instead of only the event
  type and sequence, so a crash report is diagnostic without a debug build.
- Freeze a locked goal's objective against model changes: the model may no
  longer reword, block or cancel one, refused in the tool before dispatch and
  again in the store wherever the event names the model as actor, while
  finishing or resuming a locked goal stays allowed so it can still reach a
  successful end.
- Let `-m` take the catalogue index the numbered model listing prints (`-m 3` or
`-m '#3'`), not only a name, so a selector copied from the listing resolves to
  the entry it named; an out-of-range index now fails instead of being sent
  upstream as a
  literal model name.
- Ship the multimodal modalities the manual and website describe: linked image,
  document and video reading, audio transcription and dictation, and realtime
  voice. Office is linked from a separately installed runtime and is never
  bundled.
- Take the commanded Office state in every release artifact (`WITH_OFFICE=0`
  with `WITH_OFFICE_COMMANDS=1`) rather than the linked import, so no artifact
  bundles a runtime and the engine installed on the target is resolved when it
  runs; host builds keep the linked import by default and load a separately
  installed runtime via `OFFICE_ROOT`.
- Add the commanded Office engine (`WITH_OFFICE_COMMANDS=1`): it links no Office
  library, prepares a private profile, resolves an installed `soffice` at run
  time from PATH and the usual and versioned installation roots, exports
  page-range PDF, and refuses sheet-area selection and out-of-range pages with a
  clear message because the command line has no equivalent for them.
- Add a `./configure` entry point that probes the compiler and the four optional
  modalities, tunes the tracked `config.mk`, and prints what it enabled and what
  it disabled with the reason. A component that is not found disables its
  modality unless its `--require-*` option is given, and the `make WITH_*=0/1`
  flags remain explicit overrides.
- Keep a tool call that refuses its own arguments after dispatch. `edit_file`
  reports `not_run`/`invalid_arguments` once it reads the target and the old text
  does not occur exactly once, but the journal rejected that result for a call
  that had already recorded `tool_started`, so the run ended in an invalid
  `tool_finished` transition and a failed repair instead of reporting "nothing
  changed". A started call may now report a refusal when it owns no spawned
  process; process-owning calls keep the strict rule.
- Detect the installed LibreOffice runtime before loading it: verify the runtime
  root by component presence (the LibreOfficeKit library or the engine
  executable under a program directory, or a root naming that directory) and
  report a missing or incomplete installation with the root and the expected
  component instead of failing at load. No PATH search, and compatibility is
  never judged from the conversion command's exit status.
- Keep the minimal Linux PDF build free of unused gettext/Bash utility dependencies.
- Honor terminal exit before admitting queued voice or typed work. Pending
  inputs remain available on resume instead of keeping an exiting session busy.
- Add the FreeBSD 8.4 static PDF/font dependency chain and select its modern
  static C++ runtime at application link time. Build the C++ runtime against
  the 5.1 SDK as well. Legacy PDF dependencies use the SDK entropy API and
  native compiler math operations. The 5.1 AV/PDF/audio application cross-links
  with the SDK's exception and process-exit interfaces.
- Add OpenBSD 5.9/3.5 and NetBSD 2.0 static PDF/C++ application dependencies
  with native math operations and distinct
  portable C++ error conditions. Supply OpenBSD 3.5's missing OpenJPEG integer
  formats from the compiler's target ABI and use its native unsupported-operation
  error in Fontconfig's atomic-file fallback. Share the early-BSD Poppler math
  adaptations across the NetBSD 2.0 and OpenBSD 3.5 builds. Preserve OpenBSD 3.5
  signal linkage and PDF page-dimension validation with legacy math headers.
  Keep its C printf compatibility aliases out of C++ standard-library headers.

- Let the goal tool act on a paused or blocked goal, and add a `resume` action.
  `update_goal` refused every action unless the goal was active, so a blocked
  goal could not be reworded, completed, blocked again or resumed, and there was
  no model-driven resume at all even though `/goal resume` exists. Any
  unfinished goal is now manipulable; the wording lock, not the status, is what
  stops the objective from being rewritten by the model.
- Tolerate a repeated argument key in provider tool arguments. A provider that
  emitted the same field twice (for example two `stdin` members) made the call
  fail as "function arguments are not one strict object" and stalled the turn.
  Arguments now resolve a duplicate key last-wins, as the provider's own parser
  does; wire records, events and configuration still reject duplicates so the
  durable trail stays unambiguous.
- Keep admitted room events out of tool exchanges in model requests. A room
  event admitted while a call was outstanding was appended where the admission
  was recorded, so it landed between the call and its output and the provider
  read the call as unanswered (HTTP 400 "No tool output found"). Admitted
  payloads now wait for the same safe boundary as steering and snapshots.
- Stop echoing an IRC admission batch as operator input. Room traffic admitted
  as a turn prompt is runtime plumbing, so conversation level no longer prints
  the internal `[IRC update id=...]` marker with the prompt label; the chat view
  and the durable trail already report the admission.
- Name each IRC update in the turn prompt instead of referring to a room event
  the reader may not have, and resolve those references to the retained room
  event when replaying history. The prompt is the turn's user message, so the
  internal pointer text used to reach the operator verbatim during replay.
- Keep IRC topology updates after complete tool exchanges in model requests,
  replay and compaction. Existing affected sessions resume with their saved
  tool results, avoiding missing-tool-output HTTP 400 errors. Cover interactive
  resume with listener, client, nick and room startup overrides.
- Record current IRC state before interactive resumed work, including offline
  and client-only starts, so startup overrides replace stale hosted snapshots.
- Keep a session alive when another worker joins its IRC room. Background room
  traffic now waits for the active turn instead of admitting a second
  `input_received`, which the store rejects; the rejected transition previously
  ended the process, so parallel workers saw nobody join.
- Align every prose continuation line, including provider source line breaks,
  two spaces under the paragraph text instead of at column zero.
- Rules can replace a model tool call's payload: `pass` with a `value` rewrites
  the arguments (fields the replacement omits are removed) and records a
  `rule_transform` projection linking the original and effective action digests,
  so the original stays in the journal and replay reconstructs the same state.
- Rules can insert policy text: `action = insert` with `to = model` attaches the
  rendered text to that call's model-visible result as a labelled `[policy]`
  block, journaled with the outcome.
- Rules can run a trusted helper: `action = command` writes the canonical
  envelope to the helper's stdin and accepts either an empty successful stdout
  (pass) or one strict JSON effect (`pass` with optional `value`, `reject`, or
  `insert`). Invalid JSON, a nonzero exit, a signal, a timeout or more than
  64 KiB of output denies the pending call. A helper is trusted host-user code,
  not a sandbox.
- Rules can require fresh local consent: `action = confirm` shows the rendered
  reason with a generated challenge that must be typed back at the local
  terminal. A cancelled, mismatched or unanswerable confirmation (for example a
  one-shot run) denies the call; model, IRC and helper output can never answer
  it.
- Native exploration tools (list_files, read_file, grep) are declared and
  runnable in every turn, not only /ro; /ro remains inspection-only. Reading and
  searching no longer requires shelling out through exec_command.
- Add write_file (atomic whole-file create or replace) and edit_file (targeted
  exact replacement; an ambiguous or missing match changes nothing). Both are
  workspace-relative, reject symlink traversal and keep the previous file on
  failure; apply_patch remains for several files or hunks.
- The -C switch sets the workspace (default: the launch directory); every file
  tool is workspace-relative.
- Rule effects that are designed but not yet wired (payload transform via
  pass+value, insert, confirm, helper command) are refused at configuration
  load with a clear message instead of being accepted and ignored.
- Rules gain an `accept` action: it stops evaluation and allows the operation,
  so an allowlist can sit in front of a catch-all `reject`. `pass` only
  continues and never exempts a call from a later denial.
- A rule-rejected tool call now records the sanctioned `rule_rejected` not-run
  reason, so the turn continues and the model sees the rule's message instead of
  failing on an invalid completion event.
- Rule `text`/`log` values written as JSON strings render without their quoting,
  and `rule_log` events are accepted by session replay so a session that used a
  log rule resumes.
- Add `tests/rules_e2e.py` (run by `make rulescheck` and `make check`): real
  product binary against the fixture provider, covering deny, allow, argument
  matching, allowlist default-deny, jump chains, logging, thresholds, return,
  multi-call batches, resume durability and invalid-config startup refusals.
- Filter model tool calls with ordered `[rule NAME]` configuration chains.
  Rules match immutable envelope facts and tool arguments (JSON-pointer regex
  and integer thresholds) and pass, reject, jump to a reusable chain, or log a
  templated match. Rejected calls answer a factual not-run result instead of
  disappearing. The engine is stateless and bounded; configuration load rejects
  unknown keys, invalid regexes, duplicate names and unreachable jump targets.
  Filtering is not containment, and only the tool-call boundary is wired yet.
  See `design/io-rules.md` for syntax and examples.
- Supply NetBSD 10.1 static PDF/font dependencies with native C++ runtime and
  system font configuration; legacy PDF closure remains in progress.

- Add OpenBSD 7.9 static PDF/font dependencies with native SDK C++ linkage.
  Preserve private font-library dependencies; legacy PDF closure remains pending.

- Supply NetBSD static ZIP/XML Office package-reader dependencies; PDF runtime
  closure remains in progress; Office loads a LibreOffice runtime installed on
  the target.

- Keep NetBSD audio thread creation working when native priority scheduling is
  unavailable; preserve normal-priority fallback and error handling.

- Add NetBSD static media dependencies and native audio-loader linkage. Preserve
  nanosecond file snapshots and the existing legacy threading/TLS ABI.

- Preserve actual sndio capture frame counts and split-frame bytes. Report short
  blocking playback writes as failures instead of fabricated completion.

- Accept omitted media selectors with their documented defaults. Keep crop/sheet
  exclusivity, ranges and strict local argument validation through shared parsing.

- Document early OpenBSD media/audio and package-reader cross-link scope and
  normal-priority audio threads; target execution and PDF/Office remain pending.

- Keep early OpenBSD audio thread creation without unavailable scheduler ranges.

- Preserve immediate Office worker termination with legacy libc exit interfaces.

- Preserve nonfinite media-value rejection with early OpenBSD math headers.

- Preserve ZIP filename encoding detection on early OpenBSD without CODESET.

- Reuse native integer parsing and zero comparisons in old BSD media dependencies.

- Reconcile multimedia schemas and retained-input context with the common tool
  contract and system-role host instructions.

- Preserve native unsupported-operation errors in early OpenBSD archive code.

- Keep native math declarations visible in the early OpenBSD FFmpeg allocator.

- Preserve early OpenBSD archive timestamps without the unavailable lldiv ABI.

- Supply OpenBSD static Office package-reader dependencies. Share libarchive's
  old-system wide-string fallbacks and preserve native overflow errors.
- Make old FFmpeg integer formats available through its compatibility include
  path, and reuse the exported Gnulib error definitions on early OpenBSD.

- Reuse legacy FFmpeg min/max compatibility on early OpenBSD, preserving signed
  zero with available copysign and avoiding unavailable optimized math imports.

- Parse early OpenBSD HLS start offsets with native double-precision strtod.

- Supply missing early OpenBSD FFmpeg integer-format macros from the target ABI.

- Compile the OpenBSD 3.5 audio adapter without unavailable wide-file APIs and
  preserve empty pthread feature-macro support.

- Preserve nanosecond retained-file checks with early OpenBSD stat layouts.

- Support OpenBSD 5.9 direct audio block sizes and pause/start/stop controls.
  Preserve both audio backends; physical audio remains unqualified.

- Cover multimedia context projection with current operator-visibility hints.

- Add OpenBSD static media dependency wiring and native audio-loader linkage.
  Legacy audio API compatibility and PDF/Office dependencies remain unfinished.

- Supply FreeBSD static ZIP/XML Office package readers with SDK-native digest
  linkage. Office runs from a separately installed runtime, verified before
  loading.

- Add FreeBSD static media dependencies and OSS3 default-device compatibility.
  Preserve native threading, libc loader linkage, compile-time assertions and
  nanosecond retained-file checks; PDF/Office and audio qualification remain open.

- Supply macOS static ZIP/XML dependencies for Office package checks. Office runs
  from a separately installed runtime, verified before loading.

- Add macOS static PDF/font dependencies with SDK-targeted header generation
  and C++ linkage. Target rendering qualification for Office remains in
  progress.

- Give macOS FFmpeg archive members distinct source-derived names so dependency
  debug information remains attributable during dSYM generation.

- Paint provider citation blocks as one compact terminal reference: distinct
  turns ascending, consecutive turns coalesced into ranges, repeated turns once.
  Unknown, malformed, oversized and unterminated blocks pass through unchanged;
  redirected output, durable events and provider traffic keep exact bytes.
- Make verbosity rendering one consistent contract: a logical tool block or
  streamed-output burst parks and repaints the composer once instead of once per
  internal slice; start and outcome rows share the same short call reference in
  the same column, including ids longer than the stored id limit; and one dim
  `[…]` marker replaces the several long truncation/omission wordings while
  complete output stays in the durable journal.
- Fix thinking-mode HTTP 400 on resumed/non-thinking history by restoring an
  explicit host continuation boundary after fixed policy and current state.
  Preserve exact saved reasoning and use the same layout for every provider.
- Continue saved work with `-e --resume` and no new prompt; preserve nonempty
  piped input, completed results, original commands with unknown outcomes,
  queued work, read-only mode and explicit stops. Cover real process/provider
  interruption and no-result recovery without injecting a new user prompt.

- Add the macOS static FFmpeg file-codec dependency profile and audio-device
  headers. Preserve intermediate assembly relocations through linking; PDF/Office
  and target-runtime qualification remain in progress.

- Wrap long Markdown table cells within aligned columns instead of immediately
  switching to vertical rows. Preserve styles and record boundaries, and retain
  the left border on every continuation line in the narrow fallback.

- Inform the model of current local verbosity, view and effective tool visibility
  on each request. Guide useful progress when traces are hidden and reduce
  duplicate narration when details are visible, without changing task authority.

- Describe every multimedia tool parameter, including nested crop/sheet fields,
  index origins, interval defaults and configured audio behavior in model context.

- Describe every native tool parameter and project actual command settings into
  model context. Explain argument corrections, handoff deadlines and patch syntax.
- Report missing/extra fields, types, ranges and cross-field errors; disclose
  requested/applied output ceilings and host wait limits. Keep latest-batch host
  feedback visible independently of command-output budgets, including on resume.
- Add Windows static Office ZIP/XML dependencies. Preserve legacy threading and
  fail-closed XML hash seeding without newer Windows initialization/RNG imports;
  LibreOffice itself is never bundled and is loaded from a separately installed
  runtime.

- Retain accepted attachments with pending direct input through preparation
  failure and resume; preserve durable queue state for voice handoffs and local
  blank-Enter behavior alongside interactive attachment submission.
- Continue local multimodal portability: link Windows media/PDF dependencies,
  correct static dependency metadata, adapt legacy file metadata reads, and
  avoid newer CRT locale imports in PDF parsing/formatting. Portable Office
  packaging and target-runtime qualification remain unfinished.

- Accept assistant phases finalized at message completion, including DeepSeek
  Flash's pre-tool commentary. Preserve completed phases through tool follow-ups
  and resume, reconcile final text once and reject conflicting completed phases.
- Document Flash's explicit context-limit setup when the direct provider catalog
  supplies only IDs, and clarify that model-cache refresh visits every provider.

- Drive provider activity from actual response/count/compaction/model-list
  operations. Parked policy-stopped turns keep saved work and empty-draft
  Ctrl-C cancellation without displaying a busy provider.
- Allow the five scope-preserving cyber-policy clarification attempts after
  unexecuted local function proposals. Discard failed-response calls; retain
  prior completed effects, public text and explicit refusal/hosted-tool stops.
  Record the output-item type when stream activity prevents clarification.

- Show only the latest stable downloads per architecture/ABI, with older
  releases on GitHub. Clarify kernel baselines and required OS/library ABIs;
  collapse checksums and remove historical tables, tiers and repeated setup.

- Share one provider-independent native tool and trusted-instruction contract:
  optional controls, command-only execution, handle-only polling and system-role
  fixed policy with distinct host continuation. Preserve user/tool provenance and original journal arguments.
- Accept the explicit command/wait legacy spellings and advertise byte-based
  output limits; reject ambiguous double spellings and invalid supplied values.
- Show rejected/unexecuted attempts at verbosity 1 in live and history rollout,
  with compact status reasons and higher-detail arguments/output kept private.

## 0.99.5 — September 10, 2026

- Traverse prompt history locally first, then globally, using bounded-memory
  disk-backed navigation and Ctrl-R. Keep submissions private to the running
  process until exit, then append only new entries under the global writer lock.
  Repeated resume/exit does not republish saved entries; global and local archives
  are not pruned or copied wholesale, and long searches remain cancellable.

- Preserve provider reasoning continuation in the private session journal and
  replay it in order with matching tool calls/results, including after resume.
  Bind replay to provider, endpoint, model and credential identity; retain
  plaintext and opaque encrypted state without displaying or executing it.
  Compaction includes compatible reasoning before replacing the selected history.

- Accept DeepSeek thinking streams whose reasoning items emit content-part
  events. Keep these parts out of public output while retaining reasoning
  continuity, message identity, tool-call, index, size and terminal validation.

- Complete first-run manual and README instructions for Linux, macOS, Windows,
  FreeBSD, OpenBSD and NetBSD: verification, installation paths, native shells,
  provider setup, ABI selection and troubleshooting. Correct legacy NetBSD
  checksum guidance and link each download family to its manual instructions.

- Keep model/provider/effort selections from the next full turn onward until
  changed, across later turns and resume. Remove one-turn CLI override state;
  `/model` and `-m` share durable session preferences, while `save` writes config.
  Preserve startup cache refresh before selection without painting an unopened prompt.

- Clear stale goal indicators on interruption: settle goal pause before the idle
  prompt and refresh committed goal state before notices redraw it. Keep blank
  Enter local after Ctrl-C and preserve explicit goal resume and draft cancellation.

- Make `/help` a consistent concise syntax reference for every command, including
  goal verbs, queue actions, model selectors, aliases and defaults. Share goal
  usage with `/goal help`, separate syntax from descriptions, and wrap fitting
  words without disturbing the live composer or model output.

- Make empty and whitespace-only Enter shell-like in idle and active rollout and
  chat: retain the prompt line and open a fresh prompt locally. Remove automatic
  `Continue.` requests; preserve model work, chat, goals, queues and history.

- Wrap startup, resume, history and installed-update banners at word boundaries
  on narrow terminals, preserving counts, paths, notices and the live draft.
  Redirected banner output retains its original bytes.

- Reconcile the manual, tutorials, design and status documentation with current
  command admission, recovery, history, queued dispatch and tool byte limits.
  Require complete affected documentation in every feature or behavior change;
  distinguish the current-source manual from immutable release downloads.

- Show total session turns and completed turns before history replay, retaining
  the shown/completed/total footer for every replay, including empty history.

- Admit commands throughout active work, provider waits, queue editing and delete
  confirmation. Drain nested controls at safe boundaries and during parked
  recovery; preserve queued text captured before a turn starts. Keep cache
  discovery interruptible and preserve deferred control intent across resume.

- Preserve submitted slash-command prompt lines above output in both views, at
  idle and during active work. Render queued prompts as ordinary submissions
  at dispatch, with fresh timestamps and effective model settings, retaining
  live drafts and original input provenance.

- Run manual compaction independently of previous turn cancellation. Show its
  progress and outcome, queue idle input, and keep the session open after
  provider errors. Preserve deferred requests until a safe context boundary
  and across session resume.

## 0.99.4 — September 9, 2026

- Make `/goal clear` cancel the current goal and stop automatic continuation,
  retaining its durable history. Accept whitespace around goal controls,
  including pause and resume, while preserving explicit/quoted wording.
- Preserve active-turn controls and admitted input across interruption and
  session resume. Provider-policy clarification retains running commands.

- Remove all source/test line-count limits and per-file review thresholds.
  `make sizecheck` remains an informational report without budget warnings
  or line-count failures.

## 0.99.3 — 2026-09-08

- Publish the sixteen-target matrix, adding OpenBSD 7.9, 5.9 and 3.5 and
  NetBSD 10.1 and 5.2.3; extend the legacy FreeBSD build to 5.1.
- Derive normal builds and release staging from the approved Git tag, retaining
  manual overrides for custom builds.
- Consolidate terminal, process, provider and turn ownership paths and cover
  old-BSD filesystem, threading and terminal edge cases.
- Add experimental NetBSD 5.2.3 and 10.1 amd64 source-build targets with static
  application libraries and native filesystem, process and terminal support.
  The 10.1 target uses PIE and full RELRO with its separate native pthread ABI.

## 0.99.2 — 2026-09-08

- Official standalone binaries install matching updates in the background and
  keep the current process running. One local banner links to the release log.
  Stable builds default on; debug development builds use a separate channel
  and default off. Ordinary source builds remain updater-free.
- Hide plain-text fence labels and keep indentation and pipe-prefixed content
  literal inside code blocks, including bytewise streamed output.
- Add the Linux i686 legacy and FreeBSD amd64/legacy binary variants.

- Accept completed tool arguments after an empty streaming placeholder, including
  snapshot-only responses from Codex Spark. Conflicting arguments still fail
  before tool execution.

## 0.99.1 — 2026-09-07

- First full binary release: Linux x86-64, AArch64 and i686; experimental macOS
  Intel, Apple Silicon and universal; experimental Windows x64 and ARM64.
  Publish matching symbols, checksums, manual and source/license companions.
- Replace the queued-input acknowledgement with `queued (/next or /q c) ›`.
- Use measured request input for the context meter, with 0% for a fresh session
  and ?% for unavailable measurements. Keep fresh sessions in memory until their
  first prompt or goal, and preserve validated shell aliases such as BusyBox.
- Navigate wrapped drafts with Up/Down; use Ctrl-P/Ctrl-N for direct history.
  Improve paragraph/prompt spacing and briefly retain the tool activity marker.
- Rewrite introductory copy around actual workflows, and make the complete
  single-source manual navigable with semantic HTML and a grouped outline.

### Earlier changes since 0.98 (including the 0.99.0 source tag)

- Share the default prompt's activity/goal indicators and timestamp across chat
  and rollout, putting the busy indicator before the goal flag in both views.
  Move rollout context usage after the model, expose plain context/queue numbers,
  and let templates own `%`, queue parentheses and optional `{queued:TEXT}` badges.
- Negotiate incremental IRC catch-up with durable stream/event IDs and client
  cursors. Reconnect admits only missing events; interrupted delivery is recovered
  from the session, retention gaps are explicit, and current room state refreshes
  without reinserting old conversation into model context.
- Use blue agent/cyan operator chat nicks and extend magenta mention headers
  through the `›`/notice separator, preserving uncolored message bodies.
- Derive build versions from the latest reachable Git tag instead of META's
  fixed VERSION, retaining revision/dirty suffixes between tagged releases.

- Limit mention highlighting to the timestamp, sender nick and header separator.
  Leave message bodies, Markdown and wrapped continuations unchanged. Match the
  local operator's or model's accepted room nickname, excluding the sender's own
  nick with IRC case folding; honor disabled colors.

- Preserve active goals across orderly process exit, including Ctrl-D/EOF
  and shutdown signals. Closing the process no longer saves
  an unintended paused goal before the printed resume command is used.

- Preserve the session's saved goal state on resume instead of forcing an
  active goal to pause. Continue active goals with existing queue priority;
  leave inactive goals unchanged. Replace per-message chat history labels
  with one mode-style history-replayed banner per completed replay.

- Restore saved goal visibility on session resume: show its wording and state
  automatically, and retain paused/blocked goal context in ordinary model
  requests after replay and compaction without restarting automatic work.

- Add `/retry` to continue a failed turn from retained context and tool results,
  preserving read-only mode and supporting resumed sessions. Turn failures hint
  the command.

- Publish hosted IRC wire traffic and local events from the same fields.
  JOIN precedes operator promotion everywhere; automatic MODE events name the
  server actor and target consistently, so lifecycle colors match clients.

- Share Bash-like completion for slash commands, numeric IRC destinations and
  nicknames: expand common prefixes, list ambiguous choices on double Tab, and
  add a separating space for unique matches. Preserve the draft/cursor, room
  membership scope, and input responsiveness while output is stalled.

- Use the same cyan agent and magenta operator chat colors on servers and
  clients. Remove presentation-only local-agent tracking and nickname updates;
  preserve runtime identity, routing and private rollout boundaries.

- Retry transient structured provider failures and safely truncated streams in
  the existing bounded request loop. Access/quota failures, unknown codes,
  partial output and tool activity are not replayed. A pre-output cyber-policy
  rejection gets up to three explicit, scope-preserving clarification chances
  instead of blind retries. New chat, steering and queued input veto further
  retries; rollout shows concise progress at every verbosity. Incomplete SSE
  cannot report success.

- Remove the unused environment-only credential reader and room-name getter;
  credential boundary tests now exercise the production secret-source resolver.

- Share bounded path joining and patch-result construction without merging
  operation-specific filesystem checks. Consolidate resize/suspend tests onto
  the existing PTY runner; replace stale implementation-status history and
  percentage checking with a short capability/known-boundary summary.

- Share CLI/config presentation precedence, durable model/effort selection,
  fixed-shape JSON construction and provider endpoint joining. Keep one-turn
  overrides and owned secret snapshots; retain replay of earlier effort events.
  Update remaining positive/negative provider fixtures to named sections.

- Use one cyan local composer/echo style in every view and connection state.
  Remove the obsolete prompt painter and terminal/render network-style state;
  retain public role colors. Make PTY setup independent and check goal completion
  durably instead of requiring uninterrupted terminal chunks.

- Select IRC destinations with `/N`, send once with `/N TEXT`, or explicitly
  broadcast with `/all TEXT`. Scope ordinary chat, topic changes, completion
  and model tools to their destinations; keep stable numbers and reject stale
  targets after removal or room changes. Show extra labels only when needed.

- Keep only supported messages, refusals and tool calls in the response graph.
  Remove fixture-only opaque output and reasoning-summary storage/presentation;
  inert provider events stay ignored. Reasoning effort, usage and compaction are
  unchanged. Fixtures now use the production stream callback directly.

- Remove unused render/terminal/context APIs and the provider header's terminal
  dependency. Share prompt hostname preparation and validated trash-name parsing.

- Allow `/server start/stop`, `/connect` and `/disconnect` during idle or active
  work in any interactive session. Keep chat/rollout available offline and
  preserve drafts, history, verbosity, provider work and live command handles.
  Target only changed endpoints, reject stale model sends without rerouting,
  and deliver accepted mention/background input after final disconnection.
  Preserve runtime roles across unrelated config reloads; resume exact role
  presence and absence using independent `--no-listen`/`--no-client` flags.
  Retain complete chat input in bounded batches instead of evicting earlier
  pending messages when a single steering batch fills.

- Recover oversized historical context by compacting complete response/tool
  groups within older turns. Preserve replay, steering and managed-process
  pairing across each cut, and share normal statistical token estimation with
  compaction instead of treating its entire source as one token per byte.

- Use one semantic configuration validator for loading and saving, rejecting
  invalid auth/model-limit combinations before replacement. Route cursor-only
  input through the retained prompt painter and remove obsolete spinner paint
  metadata/copies while retaining owned settings and animation timing.

- Fail visibly when an IRC listen address is already in use, even without
  `-v`. Do not silently host a separate room on another localhost address.

- Support native independent call batches and multiple command handles using
  one engine poller. Add local concurrency and per-provider batching settings;
  preserve steering cutovers, cancellation, recovery and active compaction.
  Retain complete redacted command output as bounded journal chunks and return
  incremental result excerpts instead of repeating cumulative output.

- Make `[tool] max_output_tokens` a hard command-result context ceiling,
  defaulting to 6000. Clamp larger `exec_command` and `write_stdin` requests;
  honor smaller bounds and use the ceiling for null. Replace the former
  `default_max_output_tokens` key; full capture and display limits are unchanged.

- Replace additive/config verbosity with exact `-v` counts and UI-local
  `/verbose 0..6`, preserved by reload and printed resume commands. Remove the
  obsolete `[ui] verbosity` key. Level 1 has compact tool rows without output;
  2 has 1,024/512-character argument/output previews and reasoning summaries;
  3 has full retained tools. Debug/protocol/wire detail starts at 4/5/6 and is
  live-only in visible rollout. Keep room history invariant and filter unseen
  semantic work at presentation time. Continue keyboard/control processing
  during output backpressure using privately owned nonblocking terminal output.

- Exit promptly on empty Ctrl-D or interactive EOF, interrupting active model
  and command work through the existing cleanup path. Preserve the resumable
  session and nonempty-draft forward delete. Stop interrupted tool turns before
  starting another model request.

- Retain the bottom composer and repaint only changed rows/spans, including
  status, search and spinner updates. Batch terminal controls and erasures,
  preserve Unicode/cursor layout, and avoid automatic prompt reveal during
  short inter-token gaps. Identical prompt updates retain animation timing.

- In chat, steer active work only for model-nick mentions, not ordinary local
  or channel-operator messages. Tab completes `@nick` at the cursor from live
  room membership without submitting or queueing the draft.

- Correct HTTP/2 response-status handling and automatically fall back when
  OpenRouter's optional token-count endpoint is absent, without weakening
  strict counting or treating authentication errors as unsupported endpoints.
  Preserve the conservative input bound when an exact-count probe is skipped.
  Accept OpenRouter's trailing SSE `[DONE]` only after a valid Responses
  terminal event.
- Add provider-scoped login/status/logout and first-run setup for native Codex
  device authentication, stored API keys, and existing environment credentials.
  Keep OpenRouter and other Responses providers independent; coordinate OAuth
  refresh across instances and adapt direct Codex requests without a proxy.
  If the native compact endpoint is unavailable, use one Responses summary
  attempt with the same account and durable, replayable attempt boundaries.

- Automatically use OpenRouter's hosted web-search tool for its provider URL,
  in normal and `/ro` turns, without extra search configuration.

- Rename internal `snj_`/`SNJ_` symbols to `snag_`/`SNAG_`. Share one context
  transcript/replay, retain request projections through each response cycle,
  adopt staged durable state after append, and remove redundant JSON/patch/UI
  ownership. Keep the engine/UI and independent IRC owner boundaries intact.
  Consolidate bounded formatting and UTF-8 decoding; move config/catalog
  acceptance cases from C into the existing CLI and terminal tests.
  Represent positive-only capacity facts without duplicate known flags, share
  rejection-ceiling logic with durable replay, and walk the catalog once for
  display. Update the private loaded cache directly instead of copying it again;
  unsuccessful writes retain the previous live cache.
  Remove the obsolete pre-1.0 session-format-1 and `model_changed` readers.
  Old semantic-input accounting samples are invalidated by the current cache
  shape; use `/model cache` to refresh them. Existing on-disk files are untouched.

- Default `auto_compact_input_tokens` to `auto`, using 90% of each selected
  model's effective hard input budget after output/headroom and learned lower
  ceilings. Unknown capacity falls back to 120,000 tokens. Preserve fixed
  numeric thresholds and explicit `0`; show the resolved policy in `/model`
  and `/status` and share it across all proactive compaction checks.

- Allow the existing provider-hosted web search in `/ro` queries alongside
  native file inspection, while keeping all other local tool dispatch blocked.
- Isolate the editor and renderer on one presentation thread in every runtime
  mode. Keep engine work, history locks, provider/tool waits, and durable sync
  off that thread; use bounded queues and input-aware rendering checkpoints.
- Give the hosted IRC server and each outgoing endpoint independent protocol
  threads with ordered engine admission and joined shutdown. Preserve streamed
  words when temporary spinner prompts appear, including at the right margin.
- Restore exit on five consecutive Ctrl-C presses within two seconds,
  including while busy or input admission is full. Other input resets the
  sequence; ordinary draft cancellation and durable interruption are preserved.

- Add minimum numeric prompt widths (`{context:4}`, `{hour:2}`) and clock-only
  zero padding (`{hour:02}:{minute:02}:{second:02}`). Default context uses four
  columns, including queue editing. Keep one component-valued clock capture
  per composer across redraws, with a new capture after submission or Ctrl-C.
  Reserved spinner spaces and explicit `\0` absence remain independent of
  numeric padding. The pre-1.0 `{time}` field is removed; update explicit
  templates to the component fields (no compatibility alias).

- Show all public IRC messages and room history, including each agent's own
  sends, at every verbosity level. Preserve private rollout and suppress only
  live wire echoes, not retained messages from the accepted local nickname.
- Raise IRC message payloads to 4,096 UTF-8 bytes and the advertised wire-line
  limit to 8,192 bytes including CRLF. Preserve full history and durable text,
  prefer word boundaries when splitting, and tidy actorless notifications.

- Add per-prompt `/ro` queries with native directory listing, whole/ranged
  file reading, and POSIX-regex/literal search. Enforce a read-only toolset
  without subprocesses, preserve mode through queue editing/replay, and reject
  active `/ro` steering while allowing Tab or `/queue` submission.
- Suppress fresh goal reminders during queued turns and while any queue item
  remains; never bypass a paused queue with automatic goal continuation.

- Refresh the README, complete command/configuration manual, and minimal static
  website against the current UI and runtime. Replace the terminal screenshots
  with current real-provider sessions.

- Route networked composer input by view: chat submissions go to IRC and the
  local model, while rollout submissions stay local and remain visibly labeled.
  Make view boundary, catch-up, and destination-prompt rendering atomic so an
  idle prompt cannot leak into caught-up model output. Keep Markdown formatting
  for IRC agent messages without adding synthetic prose bullets.

- Prefix the default IRC chat composer with the local `HH:MM:SS` prompt-open
  time, assembled from individually configurable clock components.

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

- Default IRC identities to `agent0` and `LOGIN0`; subsequent implicit clients
  replace the terminal zero with `1`, `2`, and so on. Make outgoing clients
  resolve explicit nickname collisions by appending numeric suffixes, and use
  the accepted per-server identities for echo suppression and mention
  recognition instead of reconnecting forever.

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
  `make depscheck`, a non-shadowing `src/snag_jansson.h` wrapper, and an
  inventoried first-party `src/snag_jansson_abi.h` declaration shim, making clear
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
