<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Context lifecycle

This is the common contract for active context, compaction, checkpoint recovery,
request accounting and original-history navigation. `architecture.md` describes
component ownership; `goals.md` owns goal lifecycle; `interactive-io.md` owns
terminal interaction. Those surfaces must use the transitions defined here.
Implementation and test reconciliation is in progress; this design is not a
claim that every existing path already satisfies it.

## Requirements

1. Continue from retained state. Normal requests and indexed resume use the
   materialized view plus new events, not repeated reconstruction of the entire
   journal. A successful compaction replaces its covered prefix exactly once.
2. Keep one durable source of truth. Original input, admitted corrections,
   completed outputs, tool effects and goal lineage remain in the append-only
   journal. Summaries and host reminders neither create input nor grant authority.
3. Use one checkpoint concept: a committed state at a known journal boundary,
   with sufficient information to interpret the uncovered suffix. Compaction
   changes the model view in that state; it does not create another state store.
4. Preserve current work through compaction, retries and model changes: exact
   active input, admitted steering and its timing, read-only restrictions, goal
   state, queued work, current room identity, pending tool calls, completed
   results and unsettled command ownership. Never execute a completed call again
   because its transcript left the automatic context.
5. Compact only complete admissible groups. A cut cannot strand a call/result,
   move past unadmitted steering, swallow an unconsumed room event, or confuse
   another turn with the current one. Repeated cuts in one long turn must work
   live and after reopening when the original turn-start event is already covered.
6. Keep successful summaries unless there is an explicit reason they cannot be
   used. An initial over-budget measurement is not permission to discard a
   later successful, fitting compacted request.
7. Enforce the selected model's actual capacity. Exact counts, provider usage,
   conservative media bounds, statistical estimates and byte sizes have distinct
   meanings. A byte size is never displayed or persisted as measured tokens.
   An upper bound above the capacity does not prove actual overflow. It cannot
   trigger token-budget compaction or discard a successful summary. Without an
   exact count, use compatible provider measurements for proactive decisions and
   the existing provider-rejection recovery path for actual overflow. Native
   image/body byte limits remain independent, enforced transport constraints.
8. Present consistent context information while working and at idle. The prompt
   and `/status` use the same observation validity and selected-capacity rules;
   issuing a command must not be necessary to refresh them.
9. Preserve active goals across every error. Recovery must perform a real paced,
   interruptible attempt, rather than repeatedly reporting a permanently latched
   failure. Error budgets govern attempts, not goal lifecycle decisions.
10. Make original history navigable by the model in bounded pages, including raw
    substantive input, goal revisions and retained command output. A summary is
    an aid to navigation, not the only remaining copy. Expose truthful cursors,
    truncation and missing-data diagnostics; never claim exhaustion because a
    page could not fit. Provider-private continuation is not model-readable
    history and must not crowd out the useful record.
11. Keep durable state, execution state and presentation failure distinct. A
    failed display does not undo a committed event, authorize tool replay, or
    prove that a submitted line exceeded its input limit.
12. Test interactions and failure paths with the existing unit/CLI/PTY/tmux
    fixtures. A current executable and an older running process are different
    artifacts; installation must not alter the latter's open executable or state.

## One state and its derived views

The session journal is authoritative. Its indexed `session_checkpoint` record
contains the reducer state and materialized provider view at the same sequence,
plus deferred input, call pairing and the bounded event seam needed to extend
that view. The journal suffix advances that state through the same interpreter
used live. Checkpoint creation changes no conversation meaning or goal state.
The cache, checkpoint encoding and request projection are representations of
that state, not competing owners of it.

There are two explicit ways to change coverage in that state:

- **Compaction completion:** a validated summary with source boundary, predecessor
  and continuation scope replaces the covered prefix. The uncovered suffix is
  retained. Native opaque items remain byte-for-byte provider data.
- **Recovery rebase:** an explicitly recorded summary-less boundary retains exact
  current input and controller state and supplies navigation to the older record.
  This is lossy automatic-context recovery, not successful summarization. It must
  be distinguishable in both diagnostics and tests.

A goal turn's final answer is a continuation boundary, not a storage checkpoint
or compaction. Describe it as a *goal continuation boundary* when distinguishing
these operations. No separate checkpoint files, history database, process
controller or session-wide quota is needed.

### Durable transaction

Validate and stage the reducer transition, append its canonical event, then adopt
it. An append failure leaves the old live state intact. Cache update follows the
durable commit; its failure cannot retroactively uncommit an event. Stop new
admissions until a failed derived view is reconciled from the same trusted
boundary. Treat a damaged indexed checkpoint as an error, not permission for an
unbounded fallback scan. Legacy journals may need their documented one-time
initial scan before an indexed checkpoint exists.

Distinguish a lost source-cache append from an interrupted derived-view update.
The latter keeps its complete retained event seam and marks the view for rebuilding;
it must not latch a permanent invalid-checkpoint error. Checkpoints preserve that
rebuild disposition so reopening cannot apply pending events twice. Keep the old
source-bearing cache until a complete, cancellation-aware replacement is ready.
A source-cache append failure instead requires recovery from a trusted durable
checkpoint and suffix; clearing its invalid flag would conceal a missing event.
Use the restart path's checkpoint reader without replacing current controller or
process-owner state. Reconstruct from original history only when no provider-view
checkpoint has been established yet. A restored view stages suffix records in an
independently owned list: interruption or allocation failure must leave its saved
checkpoint document unchanged for the next attempt.
Normal requests and compaction acquire this source through the same recovery
path. Initial capture without a saved provider view produces a rebuildable cache
that either caller can reuse, including across checkpoint/reopen before a normal
request has materialized its derived view.

A checkpoint restore must retain input timestamps, deferred IRC/steering,
call/result identities, coverage boundaries, scope, accounting provenance and
process ownership facts. A dead process owner is reported as lost/unknown; a
restored record cannot recreate an operating-system process or a successful exit.

## Request preparation

All ordinary, manual, proactive, hard-budget, rejected-request, resumed and
model-switched paths share this order:

1. Resolve the selected provider/model/effort, credential continuation scope,
   capacity and current instructions. Admit eligible input once, with its
   original receipt time and first-admission time.
2. Build one canonical item sequence: fixed instructions, compatible retained
   summary, uncovered complete transcript groups, then current host facts.
   Create/count/accounting envelopes refer to those same items and tool shapes.
3. Measure or classify the outgoing request under that exact binding. Freeze its
   hashes, capacity provenance and current host snapshot for the response attempt.
4. If reduction is required, compact a bounded complete prefix and rebuild and
   remeasure. Every successful chunk must advance source coverage. Keep its
   summary even if a later chunk fails. Repeated source/request hashes or absent
   complete prefixes demonstrate no progress; arbitrary event counts do not.
5. Bound consecutive reductions before a provider response using the existing
   attempt budget. If a usable context still cannot fit, try one explicit minimal
   recovery projection. A fitting summary takes precedence over that fallback.
   An unchanged minimal request that still cannot fit fails this attempt; it
   must not oscillate between old full context and empty rebases. Fresh durable
   model/tool progress starts a new preparation episode.
6. Commit a selected rebase only for the actual projected recovery attempt, then
   commit `response_started` before network I/O. This records an attempt, not
   provider acceptance or success. A rejected or interrupted compact operation
   never records `compaction_completed`.
7. A successful complete response advances history and resets consecutive
   preparation failure/progress guards. Failed responses retain already committed
   public output and completed effects without admitting proposed calls.

A transient transport error, policy rejection or generic decoder error is not
by itself evidence of context overflow. Only its classified cause may choose a
capacity transition. Active-goal retry pacing is separate from this finite
per-attempt preparation algorithm. Existing policy-stop and process-finalization
rules remain intact.

### Binding changes and opaque state

Ordinary conversation and local call/result pairing are portable. Private
reasoning, upstream IDs and native encrypted compact items are replayable only
under their recorded compatible binding. Portable summary text retains its
coverage on a binding change. When a summary is opaque-only and incompatible,
the new request must explicitly orient to available history; it must neither send
foreign opaque state nor silently replay the entire covered archive. The target
binding owns any new summarization/counting. Switching never returns to an old
provider merely to reconstruct context.

## Compaction transaction and boundaries

Source selection uses the materialized view and retained validated seam. Select
complete response/tool-result groups, preserving the exact covered end independently
of any small source overlap. Overlap is source material, not a backward movement
of the committed coverage cursor. Boundary identity comes from validated turn
state even when the original `turn_started` is outside the seam. Pending steering
and unconsumed referenced room events remain outside coverage until admitted.

Native compact and Responses-summary adapters produce the same validated
completion transition. Native output is opaque continuation, not parseable prose.
A fallback summary is explicitly summary data, not a newly authoritative user
instruction. Summary reduction operates on the retained summary, not on a second
copy of all already covered events.

Manual controls remain pending when no safe prefix exists and are revisited at
a safe boundary. Cancel/failure clears only the current attempt and leaves the
last committed context available. Completion clears consecutive failure state,
including callers that do not request an output flag. Active goals continue real
attempts after errors; ordinary turn bounds remain explicitly reported and reset
by new input or an explicit retry command.

## Accounting and presentation

An observation identifies its provider source, model, effort, covered context,
request hashes and measured token count. A measurement of a prior request is a
last-known input observation, not an exact count of unsubmitted later tool output.
The hard guard uses the current request's accounting, never just the displayed
percentage. A provider-reported lower ceiling remains scoped to its source/model.

The prompt and `/status` select the same compatible observation. Compute the
rounded-up percentage against the effective hard input budget. A fresh unused
session shows zero; unknown/incompatible observation or capacity shows unknown.
Both summary completion and recovery rebase invalidate pre-boundary measurements
and growth anchors. Failed compaction retains them because coverage did not change.
The compact output's count alone is not the full next-request count.

An exact outgoing count becomes durable at `response_started`; provider usage
may replace it at `response_completed`. There is no `input_token_count` journal
transition. Refresh at those real transitions and at completed compaction/rebase.
Use the same binding/compaction matcher for the meter, status annotation and
context-selection budget advice. Status may retain a historical observation for
diagnostics, but must label it ineligible rather than implying current occupancy.

Refresh presentation after committed observation, coverage or selected-identity
changes and when entering idle. Preserve the user's draft, terminal ownership,
input timestamp and immutable submitted-label history. Repainting the prefix must
not depend on a later status command or provider public text. Foreground commands
may hide the composer; its next visible frame must use current state.

## Model-driven history navigation

Use the existing history, goal and command-output tools, not raw journal dumps as
the normal recovery workflow. History pages identify session, sequence range,
ordering, truncation and exclusive next cursor. An event-type filter lets the
model retrieve original input/corrections without walking every output fragment.
Filtering preserves original event identity; it does not reinterpret old messages
as fresh instructions. Return meaningful event fields, omit private continuation
and summarize checkpoint metadata instead of embedding saved provider transcripts.

Use the journal's existing indexed boundaries and derived in-process cursors for
paging. Recent navigation must not reconstruct historical reducer state from the
beginning for each page. Deliberate old-history discovery may traverse older
segments, with cancellation and progress boundaries; it must report incompleteness
truthfully. Do not add a separate durable history index or silently truncate the
stored originals. Full retained command bytes remain pageable after settlement
and resume. Goal pages preserve creation identity, final/current status and
replacement links, even when paging older creations past newer revisions.

Retained-output cache replacement is transactional: a failed fill cannot attach
partial new bytes to the previous handle, offset or total. Corrupt/unreadable
history is not evidence that a handle has no output, and scans remain cancellable.
An unsettled process's journal cursor advances after collection; it covers only
the uncollected suffix, not the command's entire lifetime. Use that cursor only
when the requested byte offset is at or beyond the stream's collected boundary.
Older and settled ranges still require historical discovery; this cursor alone
does not make those lookups bounded.

Recent event pages walk the journal backwards from its verified live boundary.
Startup display history uses the same scan quantum. It collects recent public
turn events, then renders them chronologically; count zero reads no journal.
Completion counts describe shown turns, not a lifetime total obtained by replay.
A window beginning inside a turn labels the omitted prefix as display metadata,
never as invented user input. Older IRC references may remain unresolved when
outside the window. They remain recoverable from the original journal. Display
restoration is separate from authoritative IRC recovery: deduplication cursors,
pending admissions and reply obligations cannot be dropped to bound a preview.

Each record uses the existing envelope/digest validator and links to the previous
record. One derived in-memory position per session avoids rediscovering the last
page boundary; it is discarded on reopen and is not another durable index.
An arbitrary older sequence is located by searching complete JSONL records by
their monotonic sequence, validating the inspected records. Unread older records
are not represented as checked. Malformed records or broken links remain errors;
sequence one must be at the physical beginning, not a self-consistent suffix
mistaken for complete history.

After locating a sequence boundary, bound each history page scan to 4 MiB of
visited records, allowing one complete record under the existing journal-record
limit. Boundary lookup additionally inspects logarithmically many records when
the in-memory cursor is unavailable. This bounds filtered search work even when
few events match; it is a paging quantum, not a storage or session quota.
Report scanned range, returned count, whether the beginning was
reached and the next exclusive sequence. A record that cannot fit the output
page stays eligible for the next request. Checkpoint details expose coverage
metadata, not embedded provider transcripts. Event filters preserve original
event identities and may return an empty but explicitly incomplete search page.

Zero-fitting-record pages are budget failures with a usable recovery instruction,
not successful empty/end-of-history responses. UTF-8 clipping and redaction apply
before presentation; large payload fields cannot consume all navigation metadata.

## Response and display boundaries

Provider stream indexes are identifiers in their own namespace, not counts of
items already received. Reconcile sparse/out-of-order stream identities and a
terminal output array by stable item identity while preserving order, kind,
content and call-ID checks. Unsupported/hosted/reasoning items cannot shift the
identity of a public message or executable function call. A successful terminal
response is required before calls become admissible. Failed-response snapshots
must obey the same identity rules without promoting proposed calls.

Public text is journaled before display. A renderer that buffers a split UTF-8
character, citation or markdown construct must budget both the pending fragment
and the new bytes. Overflow of that internal buffer is a presentation failure,
not an operator input-length error. The input poll path must preserve the origin
of a terminal failure rather than infer it only from `errno`. Regression tests
include every split point and prior buffered content, not only complete strings.

## Authoritative IRC checkpoint state

The native IRC replay state has three distinct parts: per-endpoint/room stream
watermarks, current replay membership, and the bounded visible history ring.
The ring alone cannot restore the other two. In particular, a member's JOIN may
have left the ring while later messages and MODE/NICK transitions still depend
on its recorded operator status. A quiet endpoint's watermark can also outlive
every one of its visible messages.

Serialize those parts as typed data, separate from the human-readable room
snapshot. Validate a restored image into detached storage and replace the old
image only after every entry succeeds. Importing a shorter history ring keeps
the newest fitting records without reducing membership or deduplication state.
Sockets, connection attempts and uncommitted owner-thread events are outside
this serialized replay state.

The session checkpoint integration must capture committed IRC state at its
journal boundary, together with pending admissions and current reply obligations,
then apply only the uncovered suffix. The native replay codec is a prerequisite,
not by itself a replacement for the current application-level restore scan.
Legacy checkpoints without this state need explicit initialization; malformed
present state must fail rather than silently turn into an empty room.

Unconsumed IRC messages and notices remain journal-backed pending input even
when a plain IRC server supplies no catch-up stream identity. Stream watermarks
control duplicate suppression, not whether current conversation input survives
resume. Admission removes the exact saved event once; it cannot replay that
input merely because its original display buffer was lost.

Reply obligations belong to the input/turn that admitted them. Replay stages
pending-input obligations separately, promotes them at that input's
`turn_started`, and clears them when the canonical pending/active lifetime ends.
An older input, including one cancelled before a turn starts, cannot lend its
obligations to an unrelated current turn. Current-turn steering retains its
own obligations. The restored reply routes are published only after replay
succeeds.

## Reconciliation and acceptance

Use focused additions to the existing tests for these combinations, preserving
already obtained results for unchanged trees:

| Area | Required evidence |
| --- | --- |
| Checkpoint transaction | Failed append, truncated/corrupt tail, indexed restore, legacy initial scan; no ordinary full-journal read |
| Repeated compaction | Multiple groups in one long turn, trimmed start marker, live/cache/checkpoint reopen, suffix and steering preservation |
| Request preparation | Fitting summary retained; still-over-budget progress/fallback; exact-count rejection; native unavailable; interrupted/failing compaction |
| Scope change | Same-binding continuation retained, portable summary across model/provider change, opaque-only mismatch with explicit recovery |
| Accounting/UI | Pre/post-compact and rebase validity, prompt/status agreement, active tool cycles, idle/manual compact, hidden composer and later show |
| Retry lifecycle | More than eight failed compactions then recovery without new input or goal pause; ordinary bounded failure; cancellation remains actionable |
| Navigation | Useful filtered input history, no reasoning payloads, cursor continuity with new appends, old goal revisions, small page budget, UTF-8, corruption and cancellation |
| Tool/process safety | Completed effects retained, unsettled handles preserved during recoverable error, terminal ownership unchanged, output retrieval after resume |
| Streaming/display | Sparse indexes, reordered terminal reconciliation, conflicting IDs/kinds/content rejected, no calls from failure, fragmented citation and accurate failure origin |

A full fixture integration gate qualifies the combined changed tree. Source
history must distinguish design, tested correction and delivered artifact.
Rescue delivery uses the approved existing base with a development suffix,
atomic local installs of binary and matching manual, preserved rollback bytes,
and explicit version/digest checks after reconciling current integration and
installed tips. It does not restart live processes, alter journals, publish a
stable release or imply an upstream push.

## Protocol references

Checked September 27, 2026: OpenAI's official compaction guide describes opaque
compaction output; the official Python Responses stream accumulator maps provider
output indexes separately from its accumulated output list. These inform adapter
behavior, not guarantees about an unrecorded live failing payload.

- `https://developers.openai.com/api/docs/guides/compaction`
- `https://github.com/openai/openai-python/blob/main/src/openai/lib/streaming/responses/_responses.py`
