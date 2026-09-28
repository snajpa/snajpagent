<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Persistent voice interface

## Required behavior

Voice is a live input/output interface to the existing coding session. A user can
continue speaking while the agent works, ask what it is doing, correct its
direction, give it another task, and hear a concise account of the result.
Starting, completing, failing or interrupting coding work leaves voice enabled.
The requested mode stays enabled until the user turns it off. Muting affects
microphone forwarding independently of coding and playback.

Current source tracks concurrent protocol and application handoffs, with final
transcript gating, matching results and nonfatal native capacity refusal. The
remaining sections specify the broader session interface, live progress and
connection recovery; these are not yet all implemented.

The host has shared read-only file schemas, an independent provider-request
owner, and session-owner helpers for queue admission, exact-turn steering and
cancellation. Steering reuses its durable identity after reopen, refuses changed
source text and respects deferred boundaries. These helpers are awaiting live
delegation routing; their presence does not yet expose spoken session controls.

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

Voice uses the current session's environment, working directory and effective
instructions automatically. It requires no additional memory configuration and
creates no separate documentation tree, memory file or index. Its conversation
and request bookkeeping belong inside the session; existing project files stay
the common source of long-term context.

Offer general read-only file tools and the session's actual inspection and
control capabilities. File and documentation changes are requests to the main
agent, delivered through the ordinary steering or queue path. Voice has no direct
file-writing or command-execution capability. Capability availability and host
validation enforce these boundaries, rather than relying on a prompt alone.

The model chooses which files to consult and how to use the available controls.
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

Expose a bounded, redacted view of actual session state: active task, current
operation, queued work, accepted steering, meaningful milestones, blockers and
final results. Refresh it during execution, including process waits and provider
streaming. Status questions are answered from this view without waiting for a
coding turn to finish or adding a new coding request.

Speak a short gist at useful milestones, on completion, when user input is
needed, or when asked. Coalesce repetitive progress. Let the user interrupt
speech immediately and continue their sentence. Avoid speaking every tool call,
raw output block or internal reasoning. Preserve the ordinary transcript for
details. Distinguish planned, running, failed and completed operations explicitly.
Generated speech is not evidence that playback reached the user.

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
