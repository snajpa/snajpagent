<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Persistent voice interface

## Required behavior

Voice is a live input/output interface to the existing coding session. A user can
continue speaking while the agent works, ask what it is doing, correct its
direction, give it another task, and hear a concise account of the result.
The application is one coding agent with CLI and voice interfaces. In conversation,
the model means the working model; the voice model provides the spoken interface.
Its initial context explains that environment and includes the current slash-command
help from the same catalog used by the CLI.
Starting, completing, failing or interrupting coding work leaves voice enabled.
The requested mode stays enabled until the user turns it off. Muting affects
microphone forwarding independently of coding and playback.

Prolonged listening and conversation coexist with asynchronous discussion and
control in the same enabled session. Connection renewal and bounded live-context
maintenance preserve that continuity without replaying audio or accepted work.
There is no separate asynchronous interaction mode or fixed turn-taking script.

Current source tracks concurrent protocol and application handoffs, with final
transcript gating, matching results and nonfatal native capacity refusal. The
initial native/public instructions and interface requests include the CLI help
text from the shared command catalog and formatter. The remaining sections
specify the broader session interface, live progress and
connection recovery; these are not yet all implemented.

Delegations use shared read-only file schemas, an independent provider-request
owner, and session-owner helpers for queue admission, exact-turn steering and
cancellation. Steering reuses its durable identity after reopen, refuses changed
source text and respects deferred boundaries. Interface requests, tool outcomes
and replies retain their source correlation in the session. The main agent's
`voice_output` tool queries readiness or queues speech through the existing
output mailbox. Native speech uses speakable session context; public Realtime
uses a host-labelled message and its normal response scheduler. Queue and
protocol-preparation records make no playback claim. Live connection renewal
and long-lived context maintenance remain in implementation.

## Ownership and lifetime

Keep the existing audio/connection owner and the existing coding/session owner.
The voice owner continues capture, playback, speech interruption and transcript
processing while coding is idle, queued, running or awaiting a tool. The session
owner admits its tool requests and remains the sole journal and execution owner.

Track requested voice mode separately from connection/device readiness. Report
listening, muted, reconnecting and suspended states accurately. A recoverable
connection loss uses paced, interruptible recovery while voice remains requested;
discard unsent captured audio and never replay it automatically. Authentication,
protocol-integrity and device-permission failures require a visible explanation
and an actionable recovery path. Voice-off cancels pending reconnect work.

Provider error handling reuses the existing structured-error parser. Record its
validated code, category, bounded message and reported input/context counts through
the ordinary voice notice and session-owner journal path. Missing counts remain
unknown; invalid metadata must not become a capacity estimate. The outbound copy
uses the configured-secret filter. These observations supply recovery facts without
selecting retry policy or admitting actions; connection recovery remains separate.

Terminal detach, loss, switching away and job suspension retain the existing
privacy boundary: stop microphone forwarding and playback immediately. A newly
attached terminal requires explicit reactivation; attachment alone never opens
its microphone. Accepted coding work and finalized transcripts survive.

## Shared context and tool boundary

Maintain an ordered voice conversation history. Before each voice-model turn,
append new dialogue, model output and current session observations through an
identified cursor. Stream model output while work runs. State observations carry
their session, request identity and freshness; an accepted operation retains its
identity through completion, interruption or refusal.

The durable journal is the backlog. Forward observation uses the existing journal
cursor's sequence, byte offset and previous-record hash, verifying each envelope
and chain link without replaying the reducer or mutating live session state.
Each service step reads a bounded byte quantum; a consumer can pause before an
event or after consuming it. Advance the cursor only after consumption. New
commits remain available on later steps, so a slow voice connection needs neither
a lifetime scan nor an unbounded parallel queue of model output.
Live progress packets identify their session, journal sequence, event type and
fragment byte offset. Each packet is an observation, not a command. A single
pending packet applies backpressure. The owner retains one redacted record while
fragmenting it, so larger descriptions continue without repeated disk reads or
lost remainder bytes. Existing voice snapshots remain separate from this live
cursor while conversation retention is developed.

Initial and refreshed textual context snapshots use the same configured-secret
filter as recorded voice events. Filtering operates on an outbound copy; the
working session's prompt, queue contents and authority records stay unchanged.
Snapshot history uses an owner-local derived projection and the verified forward
cursor. Each call consumes at most the existing 4 MiB scan quantum plus one
complete record; later calls continue from the saved position. The projection
keeps the latest speaker excerpts and voice-queue outcome together, avoiding a
second status scan. Its storage stays bounded as the journal grows. Current
session fields carry `state_as_of_seq`; historical fields carry
`history_as_of_seq`, transcript sequences and `history_complete`. A partial view
does not claim freshness beyond its cursor. Voice service continues catch-up
between ordinary event-loop steps, including when no new utterance arrives.
The cache survives commits and pending-to-durable persistence, is freed on session
close and rebuilds incrementally after reopen. It is neither checkpoint state
nor a second authority. Live dialogue restoration on reopen is separate from
this latest-state projection.

Keep the stable instruction/help prefix and append new context to the history.
Choose batched compaction versus a sliding window from measured prefill latency,
cache reuse and conversational behavior on the selected provider. Establish the
actual model capacity; byte staging limits and another model's window are different
quantities. Preserve active work, corrections, recent referents and unresolved
requests through compaction, with older detail recoverable from the session.
Updating the voice history leaves the working model's in-flight request, cached
prefix and useful progress intact. Discussing work does not implicitly steer it.
The interface request has a distinct, stable cache identity derived from the
session/provider/model identity. Both its prompt-cache key and transport affinity
header use that identity. It does not reuse or mutate the working model's cache
identity merely because both conversations select the same model.

The live text interface now retains public dialogue and tool results across
delegations, with the orientation/help prefix kept verbatim. Each ordinary
request appends a fresh, secret-filtered state snapshot; this includes tool
continuations. Provider-private continuation stays within its current delegation.
Public assistant output is recorded separately from the eventual spoken reply.
Public history copies and returned replies pass through the existing secret
filter. Public protocol identities must survive filtering unchanged; a response
whose identity contains a configured secret fails before action dispatch.
Turning voice off releases the in-memory text history. A later text-interface
connection restores prior public voice transcripts, interface requests, outputs
and outcomes from the journal as identified historical observations. It captures
the activation sequence boundary before admitting new dialogue, so live interface
items are appended once and saved actions remain historical context. Provider-private
continuation is not restored. Native audio-context renewal and capacity recovery remain open.

Native restoration uses the same verified journal and public formatter as
text restoration. Its source boundary is captured at activation. Prior public
speech, interface actions and outcomes are historical observations; active native
speech already belongs to the current provider conversation. The existing bounded
observation stream carries the archive in order. A fresh, complete state snapshot
follows the initial history. Device activation waits for the initial cursor and
queued transport writes to finish, while cancellation and
working-model progress remain serviceable. Later working output follows through
the same cursor. Provider-window recovery and shared attachment still require
separate implementation; this boundary supplies neither a model-capacity estimate
nor a proactive retention policy. Socket-write completion is a local ordering
barrier, not evidence of remote context adoption across native media channels.

The text interface reads working inputs, public output, outcomes and controls
from the verified journal before its first delegation. Before each ordinary
request, it catches up to a captured sequence boundary, retaining the same
forward cursor across requests. Each quoted observation carries its session,
sequence and event type. Provider-private continuation is omitted by the shared
history formatter; registered secrets are filtered on the outbound copy.
The same ordered cursor restores public voice records preceding the activation
boundary, including after session reopen. Restoration neither dispatches their
tools nor sends their old audio, and needs no separate history store.
Native transcripts and deferred spoken outcomes continue through this cursor
during the live connection. Their speaker and source fields remain in the quoted
record. Live interface requests/outputs already have their ordinary conversation
entries, so only their archived records are restored through this path.
Working-event selection is shared with the native observer. It includes control
requests, starts and finishes, goal wording/lock/cancellation changes, and both
default-selection and active-turn model changes. Control finish records mark the
end of an attempt; its success/error output requires separate observation.
A separate cursor keeps pending audio packets from stalling text history. Each
owner step reads the existing 4 MiB quantum plus one atomic record. Reaching the
request byte bound schedules the existing compactor before reading more history. Work and
voice cancellation remain serviceable between steps. Newer events remain in
the journal for the following request.

Capacity recovery reuses the existing request worker and the working compactor's
tool-free summary-request builder. An actual provider capacity rejection or the
existing request-serialization bound triggers recovery; no guessed token window
is used. The owner summarizes an older prefix while keeping the current
delegation's unsummarized tail, or summarizes the whole available dialogue when
there is no older prefix. If the reduced prefix still leaves the request too
large, the next recovery includes the active tail. The instruction/help item
remains outside the summary.
Whole-dialogue recovery continues with a correlated host marker rather than
resubmitting the original speech as a new user message. Failed seeded delegations
leave a recorded failure marker with any previously accepted queue identity, so
their source does not silently become a new pending request on a later handoff.
Source JSON is streamed to summary requests in UTF-8-aligned fragments with item
positions and byte offsets. The initial 4 MiB quantum is a staging bound, not
model capacity. Capacity rejection halves the actual source bytes sent, with
the existing interruptible retry delay. Consecutive normal capacity rejections
are paced too; retry counts govern delay, not a lifetime quota.

Original live entries remain until all fragments have contributed to a smaller
summary and the owner has recorded `interface_compacted`. Empty, actionable or
non-reducing summary responses fail that delegation while leaving history and
the voice connection available. Summary requests have separate
`interface_compaction_settled` observations and never dispatch their returned
tool calls. The pending handoff and its accepted queue identity survive recovery;
summary work changes neither the coding request nor the working-model cache key.
This reactive recovery does not select a proactive retention policy: measured
provider latency/cache behavior and broader continuity acceptance remain required.

Settled interface requests return their reported usage and observed timing to the
session owner. Before acting on the response, that owner records an
`interface_request_settled` voice event with the delegation identity, provider/model/effort,
outcome, usage, retry count and elapsed milliseconds. Response-ready and first-text
timings are nullable when those events were not observed; unknown token counts
remain unknown, including absent cache counts. Timing begins before authentication
and includes transport and retries. These are client-observed durations, not a
measurement of the provider's internal prefill time. Tool-only responses need not
have a first-text observation. The records remain separate from coding-token totals
and are available through the existing history reader. Capacity and retention
policy still require measurements on the selected provider.

Voice uses the current session's environment, working directory and effective
instructions automatically. It requires no additional memory configuration and
creates no separate documentation tree, memory file or index. Its conversation
and request bookkeeping belong inside the session; existing project files stay
the common source of long-term context.

Offer general read-only file tools and the same UI commands available to the user.
Dispatch slash commands through the existing catalog and session-owner control
path, with ordinary permissions, confirmations and actual results. The voice
thread never becomes another journal or execution owner. File and documentation
work reaches the model through ordinary input admission. The `ui_input` capability
admits UI commands and explicit replies through the shared keyboard-input path.
Continuous observation of command output and confirmation prompts remains to be
implemented alongside retained conversation context.

Voice UI input enters the presentation owner's existing action queue. That owner
applies immediate UI commands and routes the remaining input to the ordinary
session dispatcher, including its current confirmation state. Admission preserves
the typed draft, checks the originating attachment and reports queue acceptance
separately from execution. A full queue or lost attachment refuses admission.
Avoid recursively executing a UI command from inside voice service: commands can
stop voice, transfer the terminal or wait for subsequent confirmation input.

The model chooses which files to consult and how to use the available controls.
The interface also has `read_session_history`, using the working model's existing
schema and verified paged reader. Older utterances, controls and results remain
retrievable by journal sequence without submitting a coding task. History is
context rather than fresh approval; provider-only continuation payloads stay out
of the displayed history.
`inspect_session` also pages the current queue by immutable enqueue sequence,
returning exact IDs, read-only flags and bounded text previews. Removed earlier
entries do not shift this cursor. Queue edits retain their identity and are
reported as current text; clipped previews are marked. Inspection never arms or
starts queued work.
Tool descriptions explain their real operations and outcomes. Do not encode a
particular documentation path, language, phrase vocabulary or conversation
script. The presence of readable files, effective instructions and useful tools
should make the workflow discoverable without a special documentation prompt.
An environment with no project notes still supports conversation and controls.

The native Codex client protocol carries text delegation and context messages;
its session setup does not declare local function schemas. Handle that delegation
with a restricted interface-model request using the session's selected provider,
model and effective instructions. Keep its provider I/O independent of the audio
loop and coding request. Its tool calls return to the existing session owner for
validation and execution. This adds a reasoning conversation behind the native
audio frontend, not a second filesystem writer. Retain its context in the session
and expose actual typed tools; do not invent a text command language for speech.
Public Realtime's function calls can use the same host capability implementations.
The transport adapter must preserve the original transcript and distinguish an
interface-model reply, a committed control and a completed coding result.

## Spoken input and task control

The voice conversation can discuss a task without submitting each utterance.
When it requests an action, carry the finalized source utterance identities and
transcripts separately from the voice model's interpretation. Preserve their
order and relation to the current task. Fragmented speech must not silently
reduce an instruction to its final fragment. Clarify ambiguous intent or targets
before admission, particularly when speech could mean either a new task or a
correction to existing work.

Route a clear correction to the active turn through the ordinary steering path.
Route an independent task through the existing durable queue. Route explicit
work cancellation through the ordinary cancellation controls. Interrupting
spoken playback affects playback only. Voice start/stop/mute and status requests
are interface controls and need no coding turn.

The voice model proposes the route; the session owner validates and commits it.
Use the same permissions, approval requirements, control boundaries and journal
acknowledgements as typed input. ASR does not authenticate the speaker, and a
derived paraphrase cannot grant authority absent from the source instruction.
An acknowledgement says whether an instruction was received, queued, admitted
as steering, refused or completed. Receipt alone cannot be reported as execution.

## Concurrent request correlation

Use bounded outstanding-request records keyed by connection and delegation/call
identity, with source utterances and the resulting queue, steering or control
identity. The existing session journal remains the durable authority; the live
map is only a connection-local projection. Results and acknowledgements must
return to the matching request even when they complete out of order.

Repeated delivery of an accepted request reuses its original identity. A duplicate
cannot execute again. Busy work, capacity exhaustion and a refused control are
ordinary per-request outcomes; they leave capture and conversation responsive.
At capacity, acknowledge that the new action was not accepted and preserve the
existing work. Malformed protocol input and broken correlation remain integrity
errors and must never be reassigned to an unrelated request.

## Agent feedback in speech

The model can use a generic voice-output capability to address the user
asynchronously. Its tool surface reports actual availability and delivery state;
existing user instructions determine when and how the model uses it. Do not bake
contact conditions, milestones, a dialogue style or attention-seeking scripts
into model-facing text. Future hands-free devices can use the same conversation
and control interface. An unavailable or privacy-suspended channel reports that
state without activating a microphone or silently choosing another channel.

Expose a bounded, redacted view of actual session state: active task, current
operation, queued work, accepted steering, meaningful milestones, blockers and
final results. Refresh it during execution, including process waits and provider
streaming. Status questions are answered from this view without waiting for a
coding turn to finish or adding a new coding request.

Keep progress context current without automatically speaking every tool event.
The models choose spoken output under the existing user instructions. Let the
user interrupt speech immediately and continue their sentence. Preserve the
ordinary transcript for details. Distinguish planned, running, failed and
completed operations explicitly.
Generated speech is not evidence that playback reached the user.

Distinguish user speech and voice-model replies with interface-owned labels in
conversation output and retained history. Preserve exact written text alongside
speech. Render voice-initiated actions and their outcomes through the ordinary
tool verbosity controls, including actions that affect the model. A spoken claim
does not replace the action result.

Voice activity now persists a prepared session before its first voice event.
Interface actions record their invocation before execution and their result
afterwards. Those records use ordinary tool-row and preview rendering with a
voice source label, independently of the working model's current response.

Session resume and `/history` include finalized voice transcripts and interface
action rows within the same bounded journal window as coding turns. The renderer
owns the ASR/generated labels in both live output and replay. Voice-only sessions
also replay; voice records leave coding-turn totals unchanged. Interleaved voice
records retain the current coding turn, with earlier committed response text
shown first. In-progress response fragments are consolidated when that response
finishes. Replay displays stored text and results without starting audio or
executing actions.

## Shared attachment transition

Shell attachment, typed session switching and voice-requested switching use the
same attachment operation. Voice invokes the existing session command. Keep the
source usable until the destination acknowledges attachment; a failed switch
preserves the original target and conversation. Source-session work continues.

An intentional successful switch preserves the requested voice mode independently
of whether keyboard or voice initiated it. Append an explicit session boundary
and destination context to the voice history. Pending actions, output and late
speech retain their origin, so switching cannot execute old work in the new target.
Actual detach, terminal loss and suspension stop capture and playback; ordinary
reattachment requires explicit activation. Shared voice continuity across a
successful attachment transition remains to be implemented and tested.

## Implementation and regression sequence

1. Reproduce a second native delegation while a coding request is pending.
   Keep the live conversation intact while reporting a correlated per-request
   outcome. Cover duplicates and transcripts arriving after delegation.
2. Replace singleton protocol/application bookkeeping with bounded correlation;
   connect queue admission, steering and controls to their existing durable paths.
3. Supply execution-progress context and concise spoken feedback independently
   of coding-response completion, with playback interruption and output limits.
4. Keep requested voice mode across coding failures and recoverable transport
   events, retaining explicit mute/off and attachment privacy transitions.

Extend existing protocol, session and interactive regressions. Exercise continued
conversation during a slow coding request and tool wait; corrections and a new
queued task; cancellation; out-of-order results; duplicate delegation; delayed
ASR; capacity refusal; playback interruption; failed coding; mute/off; and terminal
detach/reattach. Verify no lost source instructions, duplicate work, incorrect
result attribution, microphone restart on attachment or claims of unheard speech.
Use the existing synthetic device fixtures, then qualify the corrected Mac build.
