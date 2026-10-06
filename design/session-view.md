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
Owner draft synchronization, commands/reports,
external-terminal transactions and IRC routes are subsequent protocol features;
clients use only advertised capabilities. A classic owner without this endpoint
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

## Messages

All message names below are the JSON `type` value. A new client sends
`{"type":"hello","version":1}`. `capabilities` supplies `version`, the full
`session` ID, a random live-owner `instance` ID and `features`.
The implemented features are `observe`, `control`, `submit`, `cancel`, `quit`,
`detach` and `receipts`.

After hello, `state` messages contain a `state` object with committed `seq`,
byte `end`, `sha256`, journal `schema`, `active`, and the next-turn `provider`,
`model` and `effort`. Publication follows owner admission. Registration and
snapshot selection run on the same presentation thread as state publication.
Pending state is coalesced; a slow observer reads the complete intervening
range from the journal. Observation never reserves a controller or changes
the classic session list's attachment status.

| Request | Response and effect |
| --- | --- |
| `reserve` | `reserved` with a `generation`, or `error`. Uses the same exclusive reservation as the classic terminal. |
| `commit` with `generation` | `bound` with that generation. The client may then submit input. Classic STATUS now reports attached. |
| `submit` with `generation`, 32-character lowercase hexadecimal `id`, and `text` | `result` with that ID and `pending`, followed by `committed` or `rejected`. Plain input addresses the rollout, including when the previous terminal showed IRC. Active work uses existing steering/queue admission. Slash commands require a future command capability; `/ro` retains the ordinary read-only prompt syntax. |
| `receipt` with `id` | Current `result`, or `unknown`. Requires hello but no controller lease. |
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

## Presentation boundary

A bound semantic controller retains the canonical owner editor and prompt
state. Transcript fragments go to durable history and typed state publication;
the owner skips the second classic transcript render. A classic reattachment
restores its ordinary prompt and history catch-up boundary. Full command reports
and external I/O will get explicit typed adapters before those capabilities are
advertised. The current protocol grants no frontend-supplied shell operation.

## Verification

`tests/test_session_view.py` drives real native owners and the local fake provider.
It covers private sockets, committed-prefix snapshots, observer status,
classic/semantic controller exclusion, generations, classic return, fragmented
and multiframe Unicode submissions, duplicate/reconnect/lost-ack receipts,
active steering, provider-wait cancellation, rejection, stalled/malformed peers,
inode-preserving cleanup and distinct detach/quit lifetimes. Existing native
session and workspace PTY suites cover compatibility at the surrounding boundary.
