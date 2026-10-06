<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Semantic owner transport, SV/1

The optional workspace module publishes `view.sock` beside a native POSIX
owner's `terminal.sock`. Both endpoints use private-directory, same-user peer
and inode-checked cleanup rules. The writer lock is borrowed; opening, probing
or closing either listener never closes a descriptor on its lock inode.
`WITH_VM=0` omits the semantic server and endpoint. Windows has no native-owner
endpoint. The terminal protocol remains SA/5.

The current backend provides observation, exclusive control, plain rollout
submission, receipts, cancellation, detach and normal owner shutdown. The
workspace frontend uses these capabilities for live attachment and prompt editing.
Local drafts and pending request identities are saved in private workspace snapshots
before submission; reconnect queries receipts without resending text. Pending
receipt queries repeat until resolved, and unknown outcomes require explicit recovery.
The owner also retains revisioned rollout drafts through controller changes.
The workspace reconciles owner and saved drafts using their last shared text
digest and owner identity; conflicts retain both copies for an explicit choice.
The backend also accepts typed commands and retains immutable command reports.
The frontend retains report buffers and hands terminal-required commands to a
bound classic attachment. Deferred-control notifications, automatic return after
asynchronous terminal effects and IRC routes remain integration work; clients use
only advertised capabilities. A classic owner without this endpoint
continues to offer its existing terminal attachment and best-effort history.

## Framing and service

Each frame has the existing eight-byte header shape: `SV`, version byte `1`,
frame type byte `1`, and a four-byte little-endian payload length. Payloads are
at most 16 KiB. Their first two little-endian u64 fields are the byte offset
and total serialized message length; remaining bytes carry JSON. Offsets start
at zero and are contiguous. The total stays identical across all fragments.
Messages are strict UTF-8 JSON objects with duplicate or unsupported fields
rejected. An unsupported route cannot silently become a rollout submission.

The message working-buffer bound is six times the existing 1 MiB input limit
plus one transport frame for metadata. It accommodates a fully escaped prompt;
the decoded prompt retains its existing limit. Each peer retains one outgoing
message. The server services one incoming or outgoing slice per peer per loop,
on the presentation thread. Input actions enter its existing heap-owned engine
queue, independently of the synchronous engine-to-presentation request slot.

Unfinished reads and stalled writes have independent five-second progress
deadlines. Handshakes and uncommitted reservations expire after fifteen seconds.
An idle, complete connection has no deadline. A malformed, stalled or lost
connection is removed without failing the owner. Its controller lease is released.
The workspace waits up to five seconds for a draft acknowledgement after the
request finishes sending, retaining its local copy on connection loss.

## Messages

All message names below are the JSON `type` value. A new client sends
`{"type":"hello","version":1}`. `capabilities` supplies `version`, the full
`session` ID, a random live-owner `instance` ID and `features`.
The implemented features are `observe`, `control`, `submit`, `cancel`, `quit`,
`detach`, `receipts`, `drafts`, `commands` and `terminal_commands`.

After hello, `state` messages contain a `state` object with committed `seq`,
byte `end`, `sha256`, journal `schema`, `active`, and the next-turn `provider`,
`model`, `effort` and `service_tier` (empty for the provider default). Publication follows owner admission. Registration and
snapshot selection run on the same presentation thread as state publication.
Pending state is coalesced; a slow observer reads the complete intervening
range from the journal. Observation never reserves a controller or changes
the classic session list's attachment status.
The frontend accepts session schemas 2, 3 and 4 and validates offset/sequence correspondence
and the digest before using a state boundary. Within one live owner connection,
offsets and sequences advance together; an unchanged boundary retains its digest.
Malformed or regressing state closes that connection and preserves local drafts.
Read-only history opens an observer and shares it between split views. Committed
bounds and previously displayed bounds are separate, so a coalesced state update
still causes the history reader to return its new page.

| Request | Response and effect |
| --- | --- |
| `reserve` | `reserved` with a `generation`, or `error`. Uses the same exclusive reservation as the classic terminal. |
| `commit` with `generation` | `bound` with that generation. The client may then submit input. Classic STATUS now reports attached. |
| `submit` with `generation`, 32-character lowercase hexadecimal `id`, and `text` | `result` with that ID and `pending`, followed by `committed` or `rejected`. Plain input addresses the rollout, including when the previous terminal showed IRC. Active work uses existing steering/queue admission. Slash commands use the separate `command` exchange; `/ro` retains the ordinary read-only prompt syntax. |
| `command` with `generation`, `id`, `text`, `route: rollout`, and optional `draft_revision` | `pending`, then `completed` with an immutable report or `terminal` before executing any effect. Uses the same mailbox, receipt and draft-revision rules as submission. |
| `receipt` with `id` | Current `result`, or `unknown`. Requires hello but no controller lease. |
| `draft_get` with `generation` and `route: rollout` | Current `draft` snapshot and subscription to later owner changes. Requires a bound controller. |
| `draft` with `generation`, `route: rollout`, expected `revision`, positive `edit`, `text` and byte `cursor` | A `draft` response echoes `edit`, with `status: accepted` or `conflict` and the current snapshot. The cursor must lie on a grapheme boundary. Unsupported routes and invalid text/cursors leave the draft unchanged. |
| `cancel` with `generation` | `control` with `intent: cancel`; the existing owner interrupt path performs cancellation. |
| `detach` with `generation` | `detached`, then connection close. The owner continues. |
| `quit` with `generation` | `control` with `intent: quit`; normal owner shutdown follows, then `exit` with its `status` and connection close. |

Generation checks apply to every mutating request. A reservation alone counts
as detached; a bound controller counts as attached even if its window is not
focused. Input waits behind the `bound` response. Another semantic controller
or classic terminal receives a busy response while the lease is held.

Only one submission awaits engine admission at a time. `pending` acknowledges
transport/queue receipt, never durable acceptance. `committed` includes `seq`
and `event` referencing the existing journal admission (`input_received`,
`steering_added`, or a future-turn admission). Receipt publication follows the
successful store commit and precedes presentation or provider execution.
The journal format stays unchanged.

The owner retains request IDs, text digests and results across connection loss.
Repeating the same ID/text returns the receipt; changing its text is rejected.
The workspace must query an uncertain ID before taking further action and must
never automatically resubmit it as a new request. An owner restart changes
`instance`; an unknown receipt remains uncertain until explicitly reconciled
against retained history. Queue entries from a detached generation are rejected
before admission and receive a rejected receipt.
Submission refusals include the request ID when it is valid. A control error
does not resolve a pending submission; clients query that submission's receipt
before offering recovery.

Command `completed` means dispatch finished. Its `seq` is the resulting committed
journal watermark, and `outcome` is `ok` or `error`; it does not invent an input
admission event. A deferred operation such as `/configure` reports that it was
scheduled. Later completion is separate from this receipt. Native adapters cover
help/status/history, model/effort/context/fast settings, verbosity, goal/state,
steering/banner, configure/compact/yield and session list/name. Commands needing
terminal input or IRC scope return `terminal` before dispatch. The client can
refer to that original request through a bound whole-terminal transaction;
this result never contains a shell command supplied by the owner.

A completed receipt includes `report: {id, bytes, sha256, command}` and an empty
`report_error`, or `report: null` plus a retention error. Failure to retain output
does not undo side effects or permit automatic re-execution. Reports contain the
command echo and immediate report/host/error output. The private session file
`.view-report-ID` is exclusive-created, written and synced before publication.
The random 32-character lowercase hexadecimal ID is a basename component, never
an arbitrary path. Report readers must validate the private regular file, exact
byte length and SHA256, then apply presentation redaction and inert-control
rendering. The frontend loads reports on its cancellable background reader.

The report content is outside the transport message buffer and has no report-size
quota. Existing commands retain their own output policies, including `/history`'s
scan window. Files are immutable, survive owner and workspace restarts, and remain
until explicit session deletion. Deletion recognizes only exact report basenames
and private regular files, including in builds with the workspace omitted.
Reports add no session event or provider context. A changed owner instance still
returns an unknown receipt; surviving report bytes do not authorize replay.

Draft snapshots contain `route`, `revision`, `text` and byte `cursor`.
Draft refusals echo a valid `edit` token, independently of submission IDs.
Unsolicited snapshots use `edit: 0` and `status: snapshot`; they begin only
after a controller requests the draft capability. Observers and older clients
receive no unsolicited draft text. Updates and submissions run on the same
presentation owner. Drafts are ephemeral owner state; editing them adds no
journal event or model input.

A revision-aware submission adds `route: rollout` and `draft_revision`.
The revision and exact text must match the current owner draft. Legacy plain
submissions remain supported. Duplicate request IDs return their saved receipt
before consulting the current draft, so later edits cannot invalidate a receipt.
On successful admission, the owner clears the draft only if that exact revision
is still current. A newer edit, including one from a replacement controller,
survives the older admission. The receipt's `draft_cleared` is the resulting
empty-draft revision, or zero when no draft was cleared. Clients use that identity
to distinguish admission's clear from an independently edited remote draft.

## Terminal command references

`terminal_commands` advertises a capability-gated SA/5 frame, type20 (`COMMAND`).
Its64-byte request contains the32 ASCII lowercase-hex owner-instance bytes followed
by the32 request-ID bytes. Only a bound classic terminal can send it, after its
BOUND barrier. It refers to a saved command whose semantic receipt is `terminal`;
no command text is interpreted from terminal input bytes.

The owner retains the command text until dispatch and verifies its instance,
receipt, current attachment and queue availability. Its65-byte reply echoes the
reference and appends0 (refused, no new effect) or1 (admitted/already admitted).
Following keyboard input waits for this acknowledgement. Admission queues the
same engine command while preserving the classic editor's independent draft.
The receipt changes to `pending`, then `completed` or `rejected`; duplicate
references never enqueue another execution. A completed receipt can still mean
accepted deferred work, as for ordinary commands. The terminal remains attached
until /s d, ensuring an editor or transfer can complete before returning.

VM automatically hands off only a newly submitted terminal-required command.
Saved pending requests and failed/uncertain handoffs require explicit :classic
or :recover. An owner restart changes instance and cannot consume an old reference.
Classic SA/5 clients send their existing frames; older owners never receive the
extension because they do not advertise this capability.

## Presentation boundary

A bound semantic controller retains the canonical owner editor and prompt
state. Transcript fragments go to durable history and typed state publication;
the owner skips the second classic transcript render. A classic reattachment
restores its ordinary prompt and history catch-up boundary. Command reports use
immutable presentation files; external I/O uses the capability-gated terminal
transaction above. The protocol grants no frontend-supplied shell operation.

## Verification

`tests/test_session_view.py` drives real native owners and the local fake provider.
It covers private sockets, committed-prefix snapshots, observer status,
classic/semantic controller exclusion, generations, classic return, fragmented
and multiframe Unicode submissions, duplicate/reconnect/lost-ack receipts,
active steering, provider-wait cancellation, rejection, stalled/malformed peers,
inode-preserving cleanup and distinct detach/quit lifetimes. Existing native
session and workspace PTY suites cover compatibility at the surrounding boundary.
