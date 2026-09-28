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

Keep the stable instruction/help prefix and append new context to the history.
Choose batched compaction versus a sliding window from measured prefill latency,
cache reuse and conversational behavior on the selected provider. Establish the
actual model capacity; byte staging limits and another model's window are different
quantities. Preserve active work, corrections, recent referents and unresolved
requests through compaction, with older detail recoverable from the session.
Updating the voice history leaves the working model's in-flight request, cached
prefix and useful progress intact. Discussing work does not implicitly steer it.
The interface request has a distinct, stable cache identity derived from the
session/provider/model identity. It does not reuse or mutate the working model's
cache identity merely because both conversations select the same model.

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
