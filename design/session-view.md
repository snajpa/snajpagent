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
The owner retains revisioned rollout and operator-query drafts through controller changes.
The workspace reconciles owner and saved drafts using their last shared text
digest and owner identity; conflicts retain both copies for an explicit choice.
The backend also accepts typed commands and retains immutable command reports.
The frontend retains report buffers and hands terminal-required commands to a
bound classic attachment. Finite editor and transfer commands return after
completion; native /cat retains a report snapshot. Deferred-control reports
replay across reconnect, and their catalogue survives owner restart. The owner
accepts frozen private-query routes. Vim windows select separate rollout/query
buffers while sharing one transport and controller per session. Draft replies
are dispatched by exact route and receipts by request ID. Each buffer retains
its own editor, pending submission, conflict and originating-window identity.
Query history filters on stable connection/conversation IDs and local identity;
its send route remains pinned to the captured generation and nick.
Clients use only advertised capabilities. An owner without this endpoint
continues to offer its existing terminal attachment and best-effort history.

The optional `editor_feedback` control capability retains completion choices and
cancelled drafts without submitting input or changing model context. The shared
editor formats the choices or the visible prompt and draft followed by `^C`;
the controller queues the result behind an in-flight frame. The owner checks the
bound controller generation and writable conversation route, then records it
through the same presentation writer used by standalone input. Cancellation resets
the prompt clock. A disconnected transport does not retry feedback whose outcome
is unknown. Older owners keep their advertised controls.

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
`detach`, `receipts`, `drafts`, `commands`, `queue`, `terminal_commands`, `reports`,
`irc_queries`, `irc_channels`, `irc_connections` and `editor_feedback`.

After hello, `state` messages contain a `state` object with committed `seq`,
byte `end`, `sha256`, journal `schema`, `active`, and the next-turn `provider`,
`model`, `effort` and `service_tier` (empty for the provider default). `queries`
contains compact rows with `route`, `endpoint` and per-identity connection status
in `connected`. Message bodies remain in the journal. Publication follows owner admission. Registration and
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
| `submit` with `generation`, 32-character lowercase hexadecimal `id`, and `text` | `result` with that ID and `pending`, followed by `committed` or `rejected`. Input without `route` addresses the rollout, including when the previous terminal showed IRC. A query route addresses that exact operator conversation. Active work uses existing steering/queue admission. Slash commands use the separate `command` exchange; `/ro` retains the ordinary read-only prompt syntax. |
| `queue` with the same fields as `submit` | Retains the draft as a future rollout turn through ordinary queue admission. Only the rollout route is accepted. The capability is required; older owners retain `/queue TEXT`. Receipt identity includes queued versus immediate intent. |
| `command` with `generation`, `id`, `text`, `route`, and optional `draft_revision` | `pending`, then `completed` with an immutable report or `terminal` before executing any effect. Uses the same mailbox, receipt and draft-revision rules as submission. |
| `receipt` with `id` | Current `result`, or `unknown`. Requires hello but no controller lease. |
| `draft_get` with `generation` and `route` | Current `draft` snapshot and subscription to later changes to that route. Requires a bound controller. |
| `draft` with `generation`, `route`, expected `revision`, positive `edit`, `text` and byte `cursor` | A `draft` response echoes `edit`, with `status: accepted` or `conflict` and the current snapshot. The cursor must lie on a grapheme boundary. Unsupported routes and invalid text/cursors leave the draft unchanged. |
| `editor_feedback` with `generation`, `kind`, `route`, `label` and `text` | `control` with `intent: editor_feedback`. `kind` is `choices` or `cancelled`; an empty label is allowed. Retains presentation in the rollout or exact operator conversation. |
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
`steering_added`, a future-turn admission, or durable private-send admission). Receipt publication follows the
successful store commit and precedes presentation or provider execution.
The journal format stays unchanged.

The owner retains request IDs, exact routes, text digests and results across
connection loss. Repeating the same ID/route/text returns the receipt; changing
its route, text or immediate/queued intent is rejected.
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
steering/banner, configure/compact/yield/retry, file snapshots, session list/name and
private `/query`, `/msg`, `/notice`, `/me`. Commands needing terminal input return
`terminal` before dispatch from the rollout; query-scoped requests are rejected
with instructions to open the rollout for that transaction. The client can
refer to that original request through a bound whole-terminal transaction;
this result never contains a shell command supplied by the owner.

An idle `/retry` that starts a turn receives `committed` at the same durable input
admission as an ordinary prompt. An active retry receives its control receipt;
an idle retry with no failed work completes with an error. All three stay in the pane.

A completed receipt includes `report: {id, bytes, sha256, command}` and an empty
`report_error`, or `report: null` plus a retention error. Failure to retain output
does not undo side effects or permit automatic re-execution. Reports contain the
command echo and immediate report/host/error output. The private session file
`.view-report-ID` is exclusive-created, written and synced before publication.
The random 32-character lowercase hexadecimal ID is a basename component, never
an arbitrary path. Report readers must validate the private regular file, exact
byte length and SHA256, then apply presentation redaction and inert-control
rendering. The frontend loads reports on its cancellable background reader.

After syncing report bytes and their directory entry, the owner appends the
reference as one private `.view-reports.jsonl` line and syncs that catalogue before
publication. The catalogue preserves creation order and allows discovery after
owner restart. A failed append is rolled back when possible; complete report bytes
remain available if catalogue publication fails. Subsequent writers remove only
an incomplete final append. Readers validate complete entries in a fixed file-size
snapshot and expose an incomplete tail separately. The row bound derives from
the existing command field's maximum JSON expansion; the catalogue has no row quota.

`:reports` and `:report` request this catalogue on the background reader and merge
it with saved references and notifications that arrived during the read. Exact
duplicate IDs are deduplicated; changed metadata for an existing ID is an error.
Old workspace references remain usable when their owner has no catalogue. Explicit
deletion removes the catalogue even in a build with the workspace omitted.

Native `/cat` copies an open regular-file descriptor into a private report in
64KiB chunks, checks cancellation between chunks and detects changed size or
modification time. Its command echo precedes the retained bytes. It records ordinary
file reads while source writers remain unlocked. New captures have new report IDs;
refreshing a rendered report
only rereads its immutable bytes. File bytes never enter session events or
provider requests. Classic `/cat` retains its configured external pager.

The report content is outside the transport message buffer and has no report-size
quota. Existing commands retain their own output policies, including `/history`'s
scan window. Files are immutable, survive owner and workspace restarts, and remain
until explicit session deletion. Deletion recognizes only exact report basenames
and private regular files, including in builds with the workspace omitted.
Reports add no session event or provider context. A changed owner instance still
returns an unknown receipt; surviving report bytes do not authorize replay.

Clients opt into report notifications by sending `{"type":"reports"}` after
hello. This works for observers as well as controllers. The owner replays its
ordered retained `{"type":"report","report":{id,bytes,sha256,command},"error":""}`
messages, sends `{"type":"reports_ready"}` after catch-up, and publishes new
reports on that subscription. A retention failure uses `report:null` and a
nonempty `error`. Unsubscribed clients receive no notifications. Repeating the
subscription restarts replay; clients deduplicate report IDs and reject changed
metadata for an existing ID.

Deferred semantic controls retain their actual completion output separately from
the immediate admission receipt. Coalesced requests share one completion report.
Notifications add report references without changing frontend focus or drafts;
only the response to its own submitted command can auto-open a report. Notification
replay covers the live owner instance; disk-catalogue discovery covers owner restart.

Draft snapshots contain `route`, `revision`, `text` and byte `cursor`.
Draft refusals echo a valid `edit` token, independently of submission IDs.
Unsolicited snapshots use `edit: 0` and `status: snapshot`; they begin only
after a controller requests the draft capability. Observers and older clients
receive no unsolicited draft text. Updates and submissions run on the same
presentation owner. Drafts are ephemeral owner state; editing them adds no
journal event or model input.

A revision-aware submission adds `route` and `draft_revision`.
The revision and exact text must match the owner draft for that exact route. Legacy plain
submissions remain supported. Duplicate request IDs return their saved receipt
before consulting the current draft, so later edits cannot invalidate a receipt.
On successful admission, the owner clears the draft only if that exact revision
is still current. A newer edit, including one from a replacement controller,
survives the older admission. The receipt's `draft_cleared` is the resulting
empty-draft revision, or zero when no draft was cleared. Clients use that identity
to distinguish admission's clear from an independently edited remote draft.

## Private query routes

With `irc_queries`, `route` is either the string `rollout` or an object with
exactly `connection`, `conversation`, `generation`, `identity` and `peer`.
The first two fields are lowercase32-hex IDs; generation is a positive integer,
identity is `operator` or `agent`, and peer is the retained nick. The selected
owner supplies the session namespace. Query sends and writable drafts require
operator identity. Agent rows support observation; writing them is rejected.
The owner resolves its current destination number and checks the frozen handle
on the IRC connection's owner thread. Unknown, foreign, stale and wrong-role
routes cannot become channel messages or model prompts.

Each exact route has its own draft, cursor and revision. Two windows using the
same route share that draft. A renamed peer or changed connection generation
has a different handle; the earlier draft stays available under its old handle
and sends fail until the user explicitly chooses a current recipient. Clearing
one submitted revision leaves every other route and newer revision unchanged.
Draft subscriptions follow the last requested/edited route; a client switches
with `draft_get` and reconciles other clears through their submission receipts.

`/query ADDRESS [TEXT]` uses the captured endpoint scope and returns an optional
`selection` route in its completed receipt. The frontend applies that selection
to the originating window. It leaves the classic selected tab intact. `/msg`
and `/notice` return ordinary command receipts, and `/me` uses the supplied query
route. Plain private input is admitted during active provider work without
adding model steering. Private `committed` confirms durable local send admission;
IRC delivery state remains in the typed journal events. A partially admitted
send cannot be replayed merely because its remaining chunks failed. Input
consisting only of empty or stripped lines is rejected without clearing its draft.


## Retained session presentation

New owners publish an optional `presentation` position alongside the independently
validated canonical journal position. Its `origin` is the first canonical ordinal
covered by the retained display stream. `tail` contains the native-framing end,
next sequence, turn count, previous batch offset and base64 digest. Display
ordinals after `origin` belong to this auxiliary stream; they must never be passed
to model-journal APIs. Missing positions retain the older canonical-history path.

The private `.view-presentation.snb` file uses the existing checked native batch
framing and session identity. It stores ordered typed UI operations, including
submitted labels and transient notices, plus references into canonical native or
legacy history. The presentation thread owns append and publication. A failed
append freezes the last successful position, publishes `presentation_error` and
leaves session input available. The frontend displays a changed error once.
Subsequent state updates retain that bound until the owner is restarted.

Local feedback records carry their captured route. Query selection matches stable
connection, conversation and local identity; public channel events share the two
local identities under the canonical event selection rules. Unrouted output belongs
to the rollout. The canonical prefix and retained operations use one IRC event
predicate, preserving private query separation through paging and search.

The history worker applies the shared session renderer into its styled sink.
Public-byte filtering joins fragments before rendering, including protected
prefixes at an incomplete boundary. The shared rollout path carries source
positions through UTF-8, citations, Markdown and wrapping. Prompt separation comes
from the same terminal output bookkeeping used by standalone sessions. Window code
places that result and responds to new owner positions without waiting for the
stored-snapshot polling interval; an in-flight read completes before a newer bound
is read. Anchored reads resolve the source position and fill forward from it.

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
references never enqueue another execution. Terminal /config and /send keep
their receipt pending through the editor, transfer or asynchronous pager. Their
final report includes deferred configuration output. The owner publishes the result
before detaching that command's original terminal generation. Every service step
rechecks the generation, so a lost/replaced attachment cannot be detached by an
older completion. Other terminal commands remain attached until /s d.

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

`tests/test_session_queries.py` covers exact query routing, independent drafts,
request deduplication across routes, nickname reuse, active-provider privacy,
query catalogue/reconnect, incoming queries and preserved classic focus.
`tests/test_session_draft.c` withholds engine admission to verify deterministic
cross-conversation edit/clear ordering and connection-epoch draft isolation.

## Pane verbosity

The optional `command_verbosity` capability allows `/verbose` requests to carry
`verbosity`, the submitting pane's current level. The shared renderer helper
parses the command and formats the same feedback as the standalone UI. The owner
returns the resulting level in the completed receipt without changing its own
renderer. Invalid levels and context attached to other commands are refused;
request-ID deduplication includes the supplied level.

The workspace retains this context with its pending command and applies a
successful result only to the original window while it still views that buffer.
The existing Ex verbosity path owns reprojection, selection invalidation and
reading-position preservation for both callers. A closed or repurposed window
receives no effect. Older saved submissions remain readable; older owners receive
the previous request shape, with explicit level changes also applied to the pane.
Argumentless queries to older owners report their standalone level. Retained
pending commands with pane context require a workspace build supporting this
capability for restoration.

Shared terminal output tracks every trailing tool-detail newline before repainting
the composer. An expired held-tool deadline remains due until animation rendering
consumes it, so an idle prompt receives its final redraw without input. Both the
standalone display and pane projection use these output and animation contracts.
