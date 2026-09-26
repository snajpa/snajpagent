<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Operator-first execution, directories and model handoff

This design describes the current-source change requested September 23, 2026.
The exact operator requests are preserved in
`~/ai/docs/projects/snajpagent/README.md`. This is an implementation contract,
not a new process gate, version decision or release plan.

## Operator-facing behavior

- New sessions start with the home directory as their default working directory,
  regardless of the shell directory from which the binary was launched. `get_cwd`
  returns it. `cd` changes it for subsequent tool calls and persists the change
  for resume. Existing logs retain their bytes and stay resumable: a format-2
  journal maps its stored `workspace` onto the session's working directory and
  replays its older record shapes where the recorded meaning is clear. A record
  that cannot be reconstructed contributes no state instead of blocking the
  load; new journals and live appends keep strict validation.
- There is no workspace selection or filesystem workspace boundary. Remove `-C`,
  relocation, workspace-filtered list/last, `--all`, and old workspace events,
  prompts and references. Bare relative paths and explicit `./` use the
  recorded cwd; absolute paths refer to the host filesystem. Commands may also
  select an explicit absolute or `./` workdir without changing the saved cwd.
- Native reads, media, writes and patches share the cwd path rule. Preserve each
  tool's actual file-type, content-size, symlink, permission and atomic-write
  behavior; those are file-operation contracts, not a workspace permission
  boundary. An external command has the user's OS permissions and can reach
  the filesystem that the process can reach. There is no per-tool approval;
  default presentation does not show tool calls. These are plain user-facing
  facts in the introduction, home page and manual, not marketing or warnings.
- `/cat PATH` opens the chosen regular file in the configured pager and returns
  when it exits. File bytes stay out of the session conversation. The pager
  owns terminal input while it runs; no duplicate application prompt is shown.
- One operator action owns the foreground. Submission displays its line, but a
  new app prompt appears only when another action can actually be accepted.
  A foreground slash command finishes before later app commands run; untrusted
  typed-ahead bytes can be retained without painting a misleading prompt.
  An in-progress provider request is the exception: once admitted and running
  it can receive steers promptly. Before acceptance, incoming text waits rather
  than being advertised as steerable. Ctrl-C/exit keep their priority semantics.
- A model-callable selection tool and `/model` share one selector/event path.
  When invoked from a completed tool-call wave, change the next request in the
  *same turn*, after the tool result is durable. During a streaming response,
  `/model` interrupts the in-flight request at a safe boundary and restarts on
  the chosen model. Never repeat settled calls, silently settle live handles,
  or present a fresh turn as if it were the interrupted turn. Unfinished work
  remains in the ordinary local history/process journal. The next provider
  request is rebuilt from that state and uses newly selected provider/model
  parameters; backend response IDs are not adopted as local durable IDs.

## Ownership and state boundaries

The session owns one absolute cwd string. New-format `session_created` records
it; `cwd_changed` is an append-and-sync-before-adopt event. The old workspace
schema is removed from replay. Listing and `--resume --last` range over all
valid sessions. Explicit sessions always use their own recorded cwd. The process
launch directory is not a hidden routing input for tools.

The engine is the only owner of session cwd, selected model, tool admission
and journal events. `get_cwd` is read-only and available to read-only turns.
`cd` validates an existing directory, resolves `./` against the current cwd,
then commits its event before reporting success. File/path helpers take the
current cwd from the session at each call; none captures it in a global. A
batch containing `cd` is sequenced so later dependent calls observe the new
value. Existing in-flight processes retain their launch workdir.

The UI owner is the only owner of the composer and readiness display. It
distinguishes an active turn, a foreground slash command and idle independently
of provider readiness. `response_started` is a local journal boundary,
not a provider acknowledgement: the stream's validated `response.created`
event still marks provider acceptance. The engine displays the active composer
at turn start, without waiting for that event, and does not hide or repaint it
at automatic response boundaries. An early steer is durably handled at a safe
request boundary; Ctrl-C/exit remain priority controls. Submitted text is
never applied twice, and slash-command bodies never become unintended
steering. The external pager/editor takes exclusive terminal ownership until
it exits. Only a foreground slash command temporarily hides the composer until
that command finishes.

Operator decision (September 26, 2026): if the prompt visibility rule is
proposed for change again, first explain the earlier blinking and annoyance,
strongly challenge the change, and refuse it unless the operator explicitly
insists in an additional turn. See
`~/ai/docs/projects/snajpagent/state/operator-prompt-contract-2026-09-26.md`.

Model selection commits the default selection and the active turn selection
at the same serialized boundary. The following response projection uses the
new model, its resolved limits and provider configuration; prior completed
tool outputs and outstanding handles remain local journal facts. An accepted
call wave is admitted before a model switch caused by later operator input,
and pending output is truthful. When a request is cancelled, record an
interrupted response, not a spurious completed one.

## External API and actual evidence

Official Responses API documentation selects `model` on each new response
request and permits replaying previous output items as input. It does not
promise a midstream model mutation or document the subscription gateway's
internal protocol. The implementation should use new requests and locally
replayed items; inspect this repository's Codex gateway adapter and exercise
the subscription-shaped fixture path. Report live-subscription evidence only
if a live authorized check was actually performed.

## Review and verification passes

1. Inspect all workspace references and classify them as user-facing session
   scope, directory-scoped internal workers, or historical tests. Remove the
   first category; keep unrelated temporary conversion work directories.
   Inspect replay, list, resume and status output after the schema change.
2. Make `/cat` and cwd/file-tool cases fail on the old behavior, then pass on
   the new behavior: HOME launch, `get_cwd`, `cd`, absolute and `./` paths in
   reads/writes/patches/commands/media, symlinks and error paths. A dedicated
   mixed-path patch case guards ordering and duplicate targeting.
3. Exercise selected-model tool calls, midstream operator switches, accepted
   tool-call handoff, interrupted streams, durable replay and open process
   handles under the existing fake provider. Check changing to another
   configured provider, invalid selectors and live output backpressure.
4. Exercise idle submission, delayed slash commands, provider request not yet
   accepted, steering after acceptance, typeahead, external pager/editor,
   Ctrl-C and EOF in PTY/tmux. The operator prompt must match actual readiness.
5. Review changed-line style, source/documentation surfaces and rendered
   manual. Run the complete fixture `make check` on staging, compile the
   final combined source after rebasing, and smoke-test a fresh local build
   against its declared tool catalog. Keep observed caveats factual. The
   operator authorized source shipment upstream and a local Git-suffixed
   binary install, not a new stable version or tag.
