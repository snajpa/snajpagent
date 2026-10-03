<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Binary session storage

## Status and purpose

Engineering design, September 27, 2026, with framing implementation begun
September 28. Installed builds keep the existing JSONL format. The draft header,
commit-batch codec and bounded positional reader are exercised by the store tests.
Typed payloads and data adapters cover all 73 assigned semantic kinds. Archive
profiles also preserve public checkpoint views and explicitly unassigned source
names as inert observations.
Public snapshots preserve literal or span-backed text. Canonical input, output,
graph-item, continuation and process-byte references resolve exact originals;
fixed-size output spans assemble contiguous item text across verified batches.
A test-only native reader resolves input and public-snapshot references before
projection through the strict core-state reducer. Legacy journal staging builds
matching receipt-backed turn fields and streamed-output spans, then verifies
semantic events and core state. Complete checkpoints, indexed runtime storage,
grouped durability, reference relocation and full conversion remain under
implementation. The application does not yet read or write binary sessions.

The storage contract preserves canonical history, exact input authority,
completed tool results, context lineage and single-writer ownership. Active
state and recovery work depend on the retained working set and a bounded suffix,
while history may grow to the filesystem and checked 64-bit representation
limits. There is no session byte, event or turn quota.

The measured failure makes the first optimization clear: 422 embedded full
checkpoints accounted for 2,050,400,577 of 2,113,498,737 journal bytes. Changing
serialization alone would leave that repeated-state growth intact.

The current integration also retains the runtime's IRC display snapshot in
checkpoint text provenance. Session naming, saved resume options and hosted-search
records introduced after the original native adapters still need their typed
adapters and checkpoint handling before runtime cutover. The currently passing
store/context fixtures do not establish complete coverage of those workflows.

## Format evolution and compatibility

The new encoding is provisional while implementation and recovery testing expose
missing requirements. Refine its framing, typed records and state references
together before the first binary-format release. A development conversion keeps
its source and identifies the exact draft revision; it does not freeze an
incomplete encoding as a released format. Reconvert draft fixtures from their
preserved originals when the draft changes.

A released format creates a continuing read or migration obligation. Keep the
legacy importer isolated behind the import interface, and retain decoding or
migration paths plus permanent fixtures for every released format. Writers emit
the current format; readers select the appropriate decoder from the file header.
Incompatible conversion produces a separately verified destination and preserves
the original until explicit cutover. A release number and an on-disk format
version are separate identities.

Keep major versions for incompatible interpretation. Within a major version,
minor additions use length-delimited extensions with stable numeric IDs and
explicit required/optional semantics. Never reuse retired IDs, silently change
field meanings or assume that an unknown field is optional. Unknown required
state prevents resumption or append before any mutation; optional metadata may
be skipped only when doing so preserves all execution and recovery semantics.
Read compatibility alone does not grant append compatibility. Opening a newer
file must check the writer requirements too, so an older executable cannot drop
state it does not understand while checkpointing or extending the journal.

Exercise these rules with byte-level fixtures in the existing store tests:
released predecessor imports, optional extensions, unknown required features,
unsupported record versions, truncation, checked lengths and cross-endian bytes.
Freeze the first layout only after the full session state and converter use it.
Add extension points for concrete record evolution; keep a general schema or
plugin system out of the storage implementation.

## Layout decision

Use one logical session store with four binary files:

| File | Role | Retention |
|---|---|---|
| `journal.bin` | Canonical typed events and their uniquely owned payloads | Append-only history |
| `checkpoint.0` | Complete recovery state at a verified journal boundary | Alternating generation |
| `checkpoint.1` | Previous or next complete recovery state | Alternating generation |
| `history.idx` | Fixed-width sequence/turn lookup entries | Rebuildable acceleration |

Existing private locking and retained asset files remain. Temporary checkpoint
files exist only during replacement. The journal is the sole authority;
checkpoints and indexes must identify and validate the journal they describe.
An index never determines whether a tool ran or an input was admitted.

This layout deliberately spends a few directory entries to avoid an in-file
free-space allocator, writable linked-list surgery and moving live history
extents. Atomic replacement bounds snapshot storage to two generations plus
one temporary generation. A single physical file would need comparable
copy-on-write allocation and recovery machinery to reclaim old snapshots.
That machinery has no current product benefit.

This decision supersedes the earlier design prohibition on separate checkpoint
files and durable indexes for the new format. The existing JSONL implementation
and its one-file tests remain valid until replaced. There is still one checkpoint
concept: reducer state and provider-view state at exactly the same boundary.

## Binary representation

Use explicit little-endian integer encoding and length-delimited byte strings.
C structs describe the decoded records; field-by-field codecs define disk bytes.
Pointers, native enum widths, compiler padding and host alignment never reach
the file. Avoid a general JSON/BSON-like object encoding with repeated key names.

The immutable file header contains magic, major/minor format version, header
length, required feature bits, session UUID, creation time and checksum. Record
kinds have stable numeric IDs and individual payload versions. An unknown
required feature, semantic record kind or payload version stops interpretation;
only explicitly optional metadata may be skipped.

The journal consists of bounded commit batches:

1. A batch header identifies byte length, record count, first sequence, previous
   batch offset and previous batch digest. Its own checksum validates the length
   before allocation or incomplete-tail classification; a damaged length must
   not disguise a previously committed batch as an unfinished tail.
2. Typed records contain kind, payload version, length, sequence, timestamp and
   stable semantic IDs. Scalar fields use fixed widths; strings and arrays carry
   checked lengths/counts. Length arithmetic is checked before allocation.
3. A fixed-size commit footer records the batch start/length, last sequence,
   current turn count and SHA-256 over the batch including its predecessor link.

Use the existing hash primitive once over encoded batch bytes. Avoid today's
canonicalize-for-hash followed by canonicalize-for-write duplication. Readers
verify the complete bounded batch before adopting any record in it. Footer
links support backward navigation; byte offsets support direct seek. Headers
and footers provide framing, not a promise of hardware-atomic sector writes.

### Draft 0.1 framing

The framing codec uses the following fixed widths. All integer fields are
little-endian. Checksums are SHA-256 bytes; lengths include their framing.

| Structure | Layout in bytes |
|---|---|
| File header (96) | magic8, major2, minor2, size4, reader-features8, writer-features8, UUID16, created-ms8, reserved8, digest32 |
| Batch header (112) | magic8, size8, count4, header-size4, first-sequence8, previous-offset8, previous-digest32, reserved8, header-digest32 |
| Record header (32) | size8, sequence8, timestamp-ms8, kind2, payload-version2, flags4 |
| Commit footer (80) | magic8, batch-offset8, size8, last-sequence8, turn-count8, reserved8, batch-digest32 |

Magic values are `SNAGJNL`, `SNAGBAT` and `SNAGEND`, each followed by NUL.
Header digests cover their preceding bytes. The batch digest covers the complete
batch except its final digest field. The first batch links to the file-header
digest with predecessor offset zero; subsequent batches link to their predecessor's
offset and digest. Record flag bit0 marks optional metadata; remaining flag and
feature bits are reserved. This draft rejects unsupported file versions/features.
The framing layer returns verified byte views; typed event decoders must enforce
kind/version compatibility before adopting any state. Framing verification alone
cannot establish semantic compatibility or prove a tool effect occurred.

The forward positional reader operates beneath an immutable boundary supplied by its
caller. It preserves the descriptor offset, allocates at most one permitted
batch and distinguishes incomplete tails from corruption and unexpected EOF
beneath that boundary. Source identity, exclusive writer ownership and any tail
truncation remain the session backend's responsibility. Failed verification
leaves the committed anchor unchanged. No framing API modifies a file.

`snag_binary_batch_previous` reads backward from an already trusted committed
anchor. It authenticates the current batch against that anchor, then authenticates
the predecessor against the link in the current batch. The second bounded batch
read is necessary because the predecessor's turn count occurs only in its own
footer; comparing a footer's digest field alone cannot authenticate its other
fields. The returned predecessor anchor works in either reading direction. At
the root, the reader verifies the immutable file header before returning the
beginning-of-history result. Missing bytes beneath a committed boundary are read
errors, never torn-tail indications. Caller outputs stay unchanged on error;
scratch views expire on each call, and descriptor position and file bytes stay
unchanged.

`snag_binary_batch_find` uses those authenticated links to locate an existing
sequence beneath a trusted snapshot anchor. It never scans for batch magic or
promotes a derived index offset into authority. Lookup uses at most two permitted
batch buffers and work proportional to distance from the supplied anchor; this
is not the durable index or a constant-time arbitrary-history lookup. Callers
still own journal identity, immutable-prefix lifetime, causal ordering, field
role and semantic owner checks. These test-only primitives do not establish
reference-field authority or change the wire format.

### Core-state replay integration

`snag_store_reconcile_binary` reads a stopped native journal from its immutable
header through complete commit batches. Callers hold the source writer lock and
supply an initialized, state-only destination. Replay verifies session identity,
the creation timestamp, framing and chain, semantic transitions, and each footer's
turn count. Record timestamps and sequences come from the verified framing;
the resulting log boundary and digest identify the last fully interpreted batch.
The destination is replaced only after successful replay and a source identity,
size and modification-time recheck. Source bytes and descriptor position remain
unchanged. Incomplete uncommitted tails are reported after that recheck; corruption,
unsupported records and semantic/allocation/read failures return errors without
authorizing truncation. Diagnostics identify the last interpreted batch and the
failing record or batch range.

An optional semantic-event callback receives resolved data and post-event core
state after strict reduction. Its borrowed data and accumulated effects remain
provisional until the entire replay succeeds, including the final source recheck.
A callback failure aborts replay without adopting the destination. Unknown optional
metadata has no semantic callback. Sequence and timestamp describe the callback's
logical event; native offsets and digests describe batch boundaries and cannot
serve as a per-event checkpoint. This exposes historical fields that core snapshots
do not retain, including voice transcript, request and source metadata.

The current implementation projects typed data into temporary owned JSON objects
for the shared strict reducer. Native disk payloads remain field-shaped.
Unknown optional metadata retains sequence and timestamp positions without a
state transition; known semantic kinds cannot masquerade as optional metadata.
Public voice archives remain inert observations. Queued turn-start text and content
references resolve against authenticated journal batches and the current first
pending queue item. Creation identity/sequence remains the ordering identity;
edits replace text but retain original content and creation sequence. Replay keeps
the latest text receipt separately for each pending item, updates it only after
successful reduction, and follows queue cancellation/consumption without keeping
completed history. Equal bytes from another queue, an obsolete edit or a future
record do not establish the required provenance. The matching queue receipt
determines each field's canonical source, either its own literal or an explicitly
declared older original. The turn must reuse that exact field reference.

Direct and timer turn-start text/content/instruction references bind to the currently pending
input receipt. Replay tracks its enclosing journal sequence, including input
embedded in an IRC admission, and clears it on cancellation or turn consumption.
An admission without input leaves that identity unchanged. Embedded field offsets
remain relative to the outer admission payload. The shared reducer retains its
existing model/effort/provider, read-only, content and timer-origin checks. Literal
direct turns without a pending receipt retain their existing semantics; references
require a recorded pending receipt. Equal text from cancelled or future inputs
cannot substitute for it.

Direct receipts, queue creation/edits, steering and reply reminders can explicitly
reuse earlier canonical fields. This includes input and steering embedded in IRC
admissions. A receipt retains its own sequence,
identity and metadata; only the selected field values are hydrated. Model selection,
read-only state, timer origin, queue identity and other authority come from the
new receipt and the existing reducer, never from the referenced record. A cancelled
receipt can supply literal data only when a new accepted receipt explicitly names
it; cancellation still removes the old receipt's pending authority.

Queued voice transcript and request fields can each name an earlier whole literal,
using the declared original text role. The new queue retains its own connection,
input, response and call IDs, provider/model, prompt and active-turn scope. Queue
identity is still checked against that receipt's connection/input pair. Only field
values are reused; the reference supplies no speaker authentication or approval.
Subsequent references reuse the original tuple directly, including when transcript
and request contain equal bytes with distinct roles.

Steering and reminder references preserve the new steering ID, turn, receipt time
and event kind. The shared reducer checks the active turn, duplicate IDs, response
state and pending-steering capacity. Hydrated text obeys the existing steering
size bound; reminders require their exact prompt and eligible response state.
Ordinary steering with the same bytes retains ordinary steering semantics.
Output-correction prompt text remains a literal canonical source for input-text
references. Its public-snapshot references belong to the output-span family.

Canonical targets must be whole literal fields of the declared role in an earlier
authenticated record of the same journal. Text references retain the original text
role, including compatible voice text leaves. References into another reference
are rejected; subsequent receipts and turns reuse the original tuple directly.
For receipt-backed turns, replay first authenticates the current receipt and its
declaration, then resolves the original field. It does not infer declarations
from equal bytes. Embedded originals retain outer admission-relative offsets
even when a later plain receipt uses them. Replay publishes current direct-input
and ordered queue receipt origins with the checkpoint source set. The pending-input
block below preserves those origins; complete checkpoint adoption and suffix replay
remain separate consumers.

Direct/timer instruction references select the receipt's exact whole list,
including an empty list. Literal turn instructions remain independently validated
and can differ from the receipt, for example after instruction discovery. Reusing
a receipt list requires an explicit reference; replay does not infer equality or
replace a different literal turn list. Queued receipts contain no instruction
list. The queued turn declares its instruction reference directly, using an
earlier whole literal list. Replay still checks the current queue head's identity
and creation sequence. Text and content references retain their receipt bindings.
The shared reducer
validates the resolved instruction list independently, without borrowing the source
record's model, read-only state, queue or other authority.

Goal turns have no input receipt. Their own record declares canonical text and
instruction fields; current goal state supplies authority. Replay requires an
active goal and no pending direct input before resolving these references. The
shared reducer retains its fixed continuation-prompt, read-only, no-content and
turn-transition checks. Reusing a cancelled input or an older goal turn's literal
field does not select that source's goal, wording, lock or lifecycle state. The
current goal identity and replacement lineage remain unchanged. Paused, blocked,
finished, cancelled or absent goals cannot be activated by field references.

Resolved fields are temporarily re-encoded and projected through the existing
literal validator before the shared reducer checks the complete turn transition.
Those temporary bytes are never persisted.

Interruption, failure and output-correction snapshots resolve public-item spans
from the current open response. A private response-start sequence bounds their
sources, including when turn/response IDs and cycle numbers are reused. Sources
must follow that start and precede the consuming snapshot. Authenticated lookup
locates the preceding batch anchor; the span resolver checks exact first-field
coordinates, turn/response/cycle, public metadata, the original partial-public
index and contiguous byte offsets through the declared last fragment. Optional
metadata and other non-output records may intervene. Each span supplies the
item's text through its declared last sequence, which may precede later output.
Literal entries remain unchanged; the existing reducer validates snapshots
independently of the streamed cache. Resolved bytes are preflighted against the
existing literal-record bound before per-item assembly, then re-encoded for
ordinary literal projection and canonical-JSON graph validation. This adds no
persistent checkpoint provenance or session quota.

Completed-response graphs use the same current-response span bindings. Their
public-item texts are resolved while tool calls, graph order, provider identity,
usage and continuation placement remain unchanged. A public item's graph position
can differ from its original stream index because calls may be interspersed or
the recorded graph may reorder public items. Existing graph classification and
strict reduction preserve final, refusal, call and conflict outcomes. Public text
and tool-argument bytes share the literal-record materialization preflight; full
encoding and canonical-JSON validation apply afterward.

Graph-item, result, continuation and compaction source helpers await their
checkpoint consumers. Result log coordinates and voice-adoption cursors still
return `ENOTSUP` until relocation is integrated. The reference fixture builder
uses empty optional checkpoint markers; the journal staging path below uses the
assigned import marker. Complete native checkpoints, indexed navigation, the
runtime writer and conversion/publication remain under implementation. Native
readers are linked only by store/context tests.

### Exact committed-prefix reconstruction

`snag_store_reconcile_binary_prefix` uses the same byte-zero strict replay to
reconstruct an explicitly selected committed batch boundary. The final end,
next sequence, predecessor, turn count and digest must all match the requested
anchor. A boundary inside a batch fails; it cannot adopt an earlier prefix as
if the missing bytes were an incomplete tail. Source identity, size and mtime
are rechecked against the entire stopped file. Input anchors may borrow the
previous recovery output because the reader copies them before replacing it.

Later bytes remain uninspected, whether they contain valid batches, corruption
or an incomplete append. Success reports zero incomplete-tail bytes and grants
no suffix-repair authority. The context-prefix variant captures semantic events
and historical IRC dependencies at that same boundary before joint in-memory
adoption. This supplies a checkpoint-validation primitive; it still reads from
the journal header rather than loading a checkpoint and replaying only a suffix.

### Provider capture during full replay

`snag_store_reconcile_binary_context` feeds resolved semantic events and their
post-reduction core state into the existing provider cache. The capture retains
its ordinary uncompressed event seam, including time, turn and unfinished-work
metadata, with the existing compaction/rebase and unconsumed-IRC retention rules.
It remains provisional through the complete strict replay and source identity
recheck. Only success replaces the state-only destination, its provider consumer
and optional owned core origins together. Cancellation, corruption and allocation
or read errors discard the candidate and preserve the previous destination.

The shared provider cache stores only the event-derived suffix. Rebased input,
compaction output and its rollout-location hint are installed once as the request
prefix, outside that suffix. Copying between a full request and the suffix
translates the retry-notice index as well as input-timing message bindings.
Embedded legacy provider checkpoints retain the retry count and suffix-relative
notice index. Absence of both fields identifies older views, which can include
the separately installed prefix; those views rebuild from their retained seam.
Partial or malformed cursor fields fail restoration. A view already marked for
rebuild may hold an out-of-array cursor from interrupted preparation; its event
seam, rather than that partial materialization, supplies the next request.

The shared `snag_context_capture` interface also accepts strict legacy replay.
Binding requires the matching verified core state and transfers ownership without
opening descriptors, dispatching work or contacting a provider. Its first request
uses the existing renderer over the captured seam; later requests use the ordinary
incremental cache. The in-memory JSON representation retains its signed64 sequence
and timestamp domain; larger native values fail explicitly before conversion.
Retained media lookup and the existing rollout-directory hint remain caller-owned
host bindings, supplied after reconstruction. State-only preparation does not try
to append a legacy provider checkpoint; that write requires journal/lock resources.

Historical IRC admission/recovery can refer outside the retained seam, including
old consumed-watermark errors and an adjacent checkpoint-insertion shift. Before
binding, capture resolves a source table for retained admission positions, their
adjacent repair candidates and references in the current active prompt. The native
lookup walks the verified prefix with bounded batch scratch when those dependencies
exist, decodes only IRC sources and import-marker positions, then confirms the
final anchor and source identity/size/mtime. Marker entries carry empty data and
serve only the existing exact-identity repair rule; they never enter semantic
replay. The owned table survives the source descriptor closing. It is also retained
by the existing in-memory/legacy provider snapshot interface as an optional
`history_sources` array. This is not a native checkpoint section or an indexed
lookup, and no constant-time or native-resume claim follows from it.

The provider checkpoint section below records the retained seam as canonical
sequence references. Its validation path reconstructs provider state from a full
journal; an efficient loader and suffix cursor remain separate implementation work. No opaque provider-cache JSON is persisted in a
native checkpoint. Runtime storage and application resume remain JSONL.

### Legacy journal staging

`snag_store_import_binary_journal` streams a stopped, exclusively locked JSONL
source into a caller-owned empty private file. Strict legacy replay validates
the source from byte zero. Typed adapters produce native records, grouped
under the existing 1MiB batch target; a larger record occupies its own batch up
to the existing record bound. Payload buffers and record tables cover only the
current batch. This stage keeps original sequence numbers, timestamps and all
semantic event data. It leaves source bytes and descriptor position unchanged.

For the current response, staging retains one span descriptor per streamed public
item and a reference to the strict reducer's current public array. Matching
snapshot items reuse the complete stream, or its first complete fragment, after
checking item identity, provider identity, kind, phase and exact text. Reordered
snapshots and interspersed tool calls keep their recorded order. Other text stays
literal, including independently recorded snapshots and prefixes whose ending
fragment is not in the retained descriptor. Descriptors are discarded when the
response closes; equal IDs or bytes in a later response never reuse the old scope.
This construction adds neither a per-fragment table nor a session-wide cache.

Receipt-backed turn fields reuse the current direct/timer/IRC receipt, or the
current queue head's original content and most recently edited text. Staging
retains field coordinates only while that input is pending. Strict legacy replay
has already checked the turn's text and content against its authoritative receipt;
cancellation and consumption remove the corresponding sources. Direct instructions
reuse the receipt's complete list only when identical, using an owned reference to
that pending instruction list for comparison. Queue text and content need no
additional payload cache.
Independently discovered lists, queued instructions, goal turns and receipt-free
direct turns stay literal. Selection, timing, read-only state, queue identity and
all other turn metadata remain on the consuming record. Native replay verifies
the receipt binding independently before resolving the declared originals.

Derived legacy checkpoints become optional metadata kind `0x8000`, version1,
with a48-byte payload: original JSONL start8, end8 and chain hash32. Offsets are
little-endian, half-open, positive and bounded by signed64; the end must exceed
the start. The original event envelope supplies sequence and time. Native replay
validates this known marker's shape, then advances past it without a semantic
callback. These source coordinates are diagnostic metadata, never native seek
authority. Checkpoint state and provider-view bodies are rebuilt separately.

After writing, native replay verifies the entire staged journal. A streaming
SHA-256 digest compares every semantic event's sequence, timestamp, exact type
and canonical data digest against legacy replay. Final core snapshots must also
match, excluding only the chain hash, physical log end and two checkpoint
coordinates. Source identity, size and modification time are rechecked after
native verification. Restored state is adopted only after all checks succeed.
An incomplete source tail is reported and retained; the output contains only the
verified prefix. Any failure leaves provisional output for the caller to discard.

The stage is test-linked and verifies through the legacy projection domain.
Other canonical-field deduplication, source-coordinate relocation, provider
checkpoints, indexed navigation, durability barriers and format publication remain
unfinished. Results with unresolved log coordinates and voice-adoption cursors
fail verification. The complete converter still needs those dependencies, source
retention, old-writer exclusion and publication-last ordering. The application
continues to use JSONL.

### Legacy event names

The native-kind/legacy-name mapping identifies all assigned semantic families
for conversion and projection. Names retain the source grammar exactly, including
`model_selection_changed` for native kind8 and the distinct `goal_*` events.
Unknown names or kinds are rejected; optional metadata has no invented semantic
name. Resolving a type name validates neither its payload nor its eligibility
to change session state.

### Legacy data adapters

`store_binary_legacy` converts session metadata1..12, goal and timer data objects
to native field-shaped payloads and reconstructs owned JSON objects for comparison
and projection. Creation retains semantic revision2/3/4 and its exact historical
`workspace`/`cwd` key. Session settings preserve before/after selections, empty
banner/steering overrides and context modes/counts. Deletion confirmation stores
the packed prefix and both identifiers from the historical trash name; actual
session identity and deletion eligibility are reducer checks. Path strings keep
the source platform's spelling without probing the current host.

Packed identifiers, actor/reason enums, booleans, integer deadlines and bounded
UTF-8 text have explicit fields; closed schemas reject unknown properties. Rule log
fields, rule transformations, dictation billing and voice observations also have
adapters. Rule values and voice fields retain their historically unconstrained
canonical types, absent/null distinctions and unknown voice extensions. Scratch
views are bound after buffer growth and survive through payload serialization.
Action-digest transitions remain reducer checks; observations initiate no work.
Controls retain one-bit integer values and the paired image-boundary origin/source
sequence for compact requests. Download adapters preserve packed identifiers and
hashes, nonnegative counters, source path/name spelling and removal/clear reasons.
Pending-control transitions, causal source sequences, queue membership and source
platform/filesystem admission remain reducer/importer responsibilities. Context
rebases preserve their recovery reason and turn identity. Capacity rejections
retain response/cycle/hash bindings, the fixed error code, message and three
nullable limits; numeric zero is rejected in legacy data instead of silently
becoming null. Recorded ceilings are preserved without recomputation by an adapter.
Compaction adapters retain required nullable predecessor identity, optional scope
and compactor model, reason/method enums and signed64-compatible counts. Completion
preserves the canonical provider array and verifies its recorded digest, including
unknown provider extensions. Anchored counting is accepted only at start; unknown
counts retain zero. Source-prefix, scope, model/profile and predecessor bindings
remain reducer/importer checks. Turn outcome adapters preserve completion graph
identities, silent-completion reasons, interruption origin/reason and failure
class/message. Recovery keeps arbitrary class text and optional retry counts,
including the existing `UINT32_MAX + 1` terminal-attempt value; absent retry counts
remain absent. These records do not execute cancellation or recovery actions.
Input controls preserve empty input cancellation data, deferred-turn identity,
positive admission time and ordered IDs, queue armed state and user queue
cancellation. Their unchanged layouts project from input payload versions1/2;
encoders use version2. Membership, uniqueness and pending-state checks remain
reducer work. Admission times beyond signed64 cannot project into legacy JSON.
Input receipts, steering and IRC reply reminders also project their inline text,
optional media and receipt times. Receipts preserve model selection, read-only
state, timer origin and ordered, duplicate-free instruction paths. Rootedness is
checked against the source platform by the importer. Typed media retains text,
file and image parts, source assets and notes, with the legacy MIME, byte and
aggregate image limits. IRC reminders retain their exact required text. Scratch
views bind after all list growth. Native references return `ENOTSUP` from this
pure data API until resolved against their original journal roles; no literal is
fabricated. Version1/2 inline layouts both project. Active-turn, pending-state,
identity and asset-file bindings remain reducer/importer work.
Queued/edited input adapters preserve read-only state and optional arming, receipt
time and content, including content in edits even though legacy replay only
assigns it on addition. Plain queued input uses an empty string for no active
turn; voice uses null. Voice is exclusive to queued input without read-only state
or content. Its eight provenance fields remain typed, and queue identity retains
the first32 hex characters of the legacy canonical SHA256 over connection/input
IDs. Speaker authentication and authority are unchanged. Pending membership,
active-turn matching, no-op edit rejection and queue effects remain reducer work.
Turn starts preserve cwd/workspace spelling, optional read-only/time fields,
queue identity, all four origins, typed media and open configuration snapshots.
Execution defaults and numeric relationships match legacy validation without
inserting absent settings. Instruction metadata may mix objects and paths when
its first item is an object; a path-first list stays path-only. Source format,
pending/goal admission, cwd equality and turn sequencing remain importer/reducer
checks. Workspace spelling alone does not establish the session's source format.
Unresolved turn text/content/instruction references likewise return `ENOTSUP`.
Response starts preserve the legacy identity-only and full-accounting shapes,
ordered steering IDs, optional IRC watermark and nullable baseline/compaction IDs.
Full accounting retains capacity source, source-bound state, hashes, positive byte
counts, nonnegative counts and hard-input limit, and nullable positive output limit.
Only anchored counting carries a baseline; unknown counting retains zero, while
other methods also permit zero. Full-only host snapshots use typed text parts and
project exact user-role objects with the original required boundary messages.
Steering views rebind after snapshot growth. Wider native unsigned counters cannot
project as legacy integers. Source-format eligibility, current provider/profile,
anchor, steering, watermark and response-cycle bindings remain importer/reducer checks.
Streamed output preserves item kind/phase, both item identities, literal text,
index and byte offset. Interruptions retain origin/reason constraints; failures
retain class, message, retry count and historical optional-field prefixes, including
policy detail and handoff state. Corrections preserve their exact host text and
the assistant-only restriction for cyber clarification. Partial-public arrays
use typed items and retain ordering. The existing source validator checks unique
public IDs and canonical graph-size limits in both directions; repeated provider
IDs remain allowed. Unresolved native output spans return `ENOTSUP` before public
projection. These adapters do not append live output, enqueue correction steering
or trigger retry/handoff actions. Fragment continuity and active-response, pending
steering, correction-use and source ownership checks remain importer/reducer work.
Completed responses retain mixed public/tool-call graphs, provider response identity
and nullable usage, including absent versus explicitly null cache counts. Existing
graph classification validates the original ID namespaces, tool catalog and encoded
budget; conflicting graphs remain recorded conflicts rather than conversion errors.
Arguments and reasoning objects retain canonical provider-boundary JSON, while graph
items and continuation wrappers have typed fields. Continuation absence, null and
ordered items remain distinct; present continuation always retains its required scope.
Temporary provider-field buffers bind after growth, and graph views rebind after
continuation serialization. Projection validates both aggregate source budgets and
preserves provider extensions. Proposed calls and continuation never execute here.
Tool starts retain the turn/call/action digest and source workdir spelling. Process
output retains handle, stream, byte offset, raw bytes and original UTF-8/base64 tag.
The source decoder rejects noncanonical base64 padding bits, so re-encoding the
decoded bytes reproduces every accepted base64 value. Existing nonempty16KiB chunk
and signed64 end-offset bounds apply. Ownership, active-turn/call/permission checks
and contiguous stream admission remain reducer work; conversion starts no process
and emits no terminal output.
Tool results and process closure retain status/reason/cause enums, optional limit,
handle, content and output-reference presence. Ignored exit/signal fields preserve
all historical canonical value types. Source result validation checks dependency
order and signed64 stream/stdin accounting; native value views bind after media
serialization. Excerpts keep their original strings: historical base64 admission
checks length alone, unlike the strict process-chunk decoder. Retained UTF-8 may
exceed the original byte count. Existing image aggregate bounds apply; model text
uses the event budget rather than an invented smaller cap. Closure retains the
seven terminal statuses and six causes. Historical log coordinates are literal
metadata here; the importer must relocate them before binary-history navigation.
Call/process ownership, collection ranges and lifecycle transitions remain reducer
checks. Conversion neither executes a call nor settles a live process.
IRC observations preserve the old watermark-free shape, explicit empty stream,
stream/sequence/input presence and paired urgency/reply classification. Original
kind names, booleans, text bounds, C0/DEL exclusions and signed64 counters remain
source-validated. Snapshots retain their four reasons and bounded text. Admissions
carry strictly increasing positive sequence lists and an optional typed receipt or
steering record, preserving the mutually exclusive keys. Sequence and child views
bind after the last buffer growth. Embedded v1/v2 literal input payloads project;
unresolved input references return `ENOTSUP`. Admission sequence causality, room
membership, classification authority and embedded reducer transitions remain
journal-aware checks. Conversion does not connect to IRC or admit live input.
Voice-transfer sealing and adoption retain all three identities, positive source
sequence/count fields and adoption's original begin offset, sequence and digest.
Own-session identity, causal sequence/count bounds, live-log range and transfer
acceptance remain importer/reducer checks. Begin coordinates still require journal
relocation; projecting metadata does not adopt history or change attachment state.
Archive264 preserves public projections through its enclosed public field profile,
including the smaller checkpoint view and explicitly unassigned source names.
All73 assigned semantic kinds have data adapters. Unresolved native references
return `ENOTSUP`; known records never use a generic whole-record fallback.
The module is linked only into the store test target.

Canonical validation checks data at its original envelope nesting level before
conversion. The complete source envelope, sequence/hash chain, source-platform
path admission and reducer transitions remain importer responsibilities. Native
deadlines beyond signed64 cannot be projected into legacy JSON. Failed conversion
preserves caller outputs, including append prefixes. These adapters do not load,
repair, publish or resume a session; journal-aware reference resolution, replay
integration and the four-file backend remain implementation work.

### Typed control payloads

Payload version1 assigns session/configuration IDs1..12, timer
schedule/fire/cancel IDs32..34, goal
start/replace/reword/lock/pause/block/complete/resume/cancel IDs64..72 and initial
input/steering IDs96..105. Session control request/start/finish use IDs16..18;
IDs13..15 and19..31 remain reserved for session metadata. Required semantic kinds occupy the low
half of the 16-bit namespace. The high half permits explicitly optional metadata;
unknown required records, unsupported semantic payload versions and optional
flags on semantic kinds are errors before state adoption.

Each timer or goal payload starts with its 16-byte ID. Scheduling adds
an 8-byte due time and text. Goal start adds its prompt; replacement adds the new
16-byte ID, actor and prompt; reword adds actor and prompt. Lock/pause add one
byte for the boolean/reason enum. Block adds actor and blocker text; completion
adds actor. Fire, cancel and resume need only the ID. Text uses a 4-byte length
followed by exact UTF-8 bytes, with existing field-size limits and embedded NUL
rejection. Actor values1/2 are user/model; pause reason values1..6 retain the
legacy order input-closed, provider-policy, refusal, session-resumed, turn-stopped,
user. These are compatibility representations, not new automatic transitions.

The codec returns typed C values and borrowed text slices. It performs no JSON
serialization and preserves caller output on malformed input. Current-state,
lineage and authority checks remain with the existing reducer; valid encoding
alone does not authorize a transition. Checkpoints will refer to these canonical
payloads when checkpoint/reference integration is complete.

Session/configuration records use fixed field order:

| ID | Event | Payload fields |
|---|---|---|
| 1 | session created | source semantic revision2, protocol1, provider/model/effort text, cwd text |
| 2 | cwd changed | previous cwd text, new cwd text |
| 3/4 | retired archive/unarchive metadata | user origin1; inert legacy records |
| 5 | delete requested | confirmed prefix4, session UUID16, trash nonce16 |
| 6 | banner updated | text, including empty to clear |
| 7 | steering updated | mode1: default0, mentions1, all2 |
| 8 | model selected | previous provider/model/effort text, new provider/model/effort text |
| 9 | active-turn model changed | turn UUID16, previous provider/model/effort text, new effort text |
| 10 | effort changed | previous effort text, new effort text |
| 11 | context selection changed | previous mode1/tokens8, new mode1/tokens8 |
| 12 | command shell changed | shell path text |
| 16 | control requested | control bit1, image-boundary origin presence1, optional source sequence8 |
| 17/18 | control started/finished | control bit1 |

Control bits retain the existing values: config1, cache2, compact4, archive8,
delete16 and retry32. Exactly one bit is required. The optional request origin
is `image_boundary`, permitted only for compact; its source sequence must be
positive and signed-64 representable. Absent origin means absent source sequence.
Only presence values0/1 are valid. Pending/started state, source precedence and
admission authority remain reducer checks.

The creation record retains source semantic revision2/3/4 for the legacy reducer,
separately from binary file/payload versions. Protocol1 means Responses. Context
modes default0/maximum1 carry zero tokens; explicit2 carries a positive count
within the existing context-selection bound. Path shape, session/turn identity
matching, deletion confirmation and valid state transitions remain reducer
checks. Packing IDs and enums preserves their exact meaning; the importer will
reuse the corresponding legacy validation rather than infer missing authority.

Initial input and steering records use these layouts:

| ID | Event | Payload fields |
|---|---|---|
| 96 | input received | origin1, flags1, received time8, provider/model/effort text, instructions, original text, optional content |
| 97 | input cancelled | empty |
| 98/99 | steering added / IRC reply reminder | steering UUID16, turn UUID16, flags1, optional received time8, original text, optional content |
| 100 | steering deferred | turn UUID16 |
| 101 | input admitted | turn UUID16, first-context time8, steering UUID list |
| 102 | future queue state | armed boolean1 |
| 103 | future turn cancelled | user reason1, nonempty queued UUID list |
| 104 | future turn queued | queue UUID16, flags1, while-turn kind1 and optional UUID16, optional receipt time8, original text, optional content or voice source |
| 105 | future turn edited | queue UUID16, flags1, optional receipt time8, original text, optional content |

Input origin0 preserves the original absent origin field; origin1 means timer.
It does not elevate IRC/voice input to operator authority. Input flag bits0/1
mean read-only/content-present. Steering flag bits0/1 mean timestamp-present/
content-present, preserving absent versus explicitly zero receipt timestamps.
Remaining bits are rejected. Queue admission/order, duplicate IDs, original
text equality and authority remain reducer checks. UUID lists use count4 plus
packed16-byte entries; admission may carry an empty list, cancellation may not.

Queue flag bits0..5 mean read-only, armed-present, armed-value, timestamp-present,
content-present and voice-present. An armed value without presence is invalid.
While-turn kind0 retains the empty string used by idle ordinary input, kind1
carries an active turn UUID, and kind2 retains null for idle voice input. Voice
input cannot also carry content or read-only state, and editing carries neither
a while-turn field nor voice provenance. Voice source fields are connection
UUID16, provider input/response/call ID text, provider/model text, transcript
text and requested-action text. These remain provenance, not speaker identity
or additional authority; the existing reducer still checks queue identity and
active-turn matching. No flags or absent timestamps/arming values are inferred.

Instruction lists use count4, then per entry a metadata-present boolean1 and a
bounded NUL-terminated UTF-8 path. Unlike the usual length-prefixed text, this
uses two overhead bytes per plain path instead of five. A near-limit legacy path
array therefore fits the existing native record budget; no larger limit or path
truncation is needed. Embedded NUL remains invalid. This refines the unshipped
draft encoding; production journals remain JSONL. Workspace-era entries also
retain byte count8 and SHA25632, not instruction file contents. Direct input
receipts accept plain paths only,
as their existing schema requires. Media content lists use nonzero count4 and
typed entries: text1 plus text, file2 plus asset, or image3 plus asset followed
by a source-present boolean1 and, when present, source asset and note text.
Assets store UUID16, SHA25632, byte count8 and MIME text. These codecs check
field shape/size, UTF-8 and flags; path/MIME policy, aggregate image limits and
actual asset verification remain the existing domain validators' responsibility.
Borrowed list iterators avoid allocating a second array proportional to event
size. Counts are checked against remaining bytes before iteration. These are
typed field encodings, not embedded JSON. Turn-start records and their canonical
input references are specified below.

The ordinary event-size bound remains a resource contract. Start with a 1 MiB
batch target, admitting one larger permitted event alone. A batch never becomes
unbounded merely because many records are ready. These are I/O quanta, not
lifetime quotas; exact tuning follows measurements on the implementation.

Current turn count is in each committed footer and checkpoint header. The
immutable file header need not be rewritten at every turn. A turn boundary is a
small event/index boundary, distinct from a full recovery checkpoint. Historical
turn navigation does not require storing a full state image for every turn.

Provider-defined opaque bytes remain exact bytes. JSON originating at a provider
boundary may be stored as an opaque payload where its semantics require it;
that does not make JSON the session envelope or checkpoint encoding. Import
adapters may decode legacy JSON, while normal new-format paths use typed data.

### Turn-start payload

Kind128 starts the turn family. Its payload version1 uses the checked
literal/reference fields from input revision2. It contains turn UUID16, turn
number8, input kind1 (direct0/queued1/goal2/timer3), flags1, optional receipt
timestamp8, queued UUID16/sequence8 for queued input, cwd text, turn config,
instruction list, original text and optional content. Flag bits0/1 preserve
read-only presence/value, bit2 receipt timestamp presence, bit3 content presence
and bit4 the legacy `workspace` key instead of `cwd`. Unknown bits are invalid.

Config contains provider/model/effort selection and an optional-field mask2.
Bits0..7 select eight-byte numbers in this order: maximum parallel commands,
default yield, maximum wait, default timeout, maximum timeout, tool output bytes,
output cache bytes and maximum turn retries. Bits8..13 select six native field
values: prompt schema, replay schema, tool schema, capability version, profile ID
and maximum output tokens. Bit14 selects the parallel-tool-calls boolean; bit15
is reserved. A native extension object follows, including an empty object when
there are no unknown keys. Its top-level keys exclude all eighteen named settings.

Legacy configuration validation left the six individual fields and unknown keys
unconstrained. Their native values preserve null, booleans, signed integers,
strings, arrays and objects, including absence versus null. Named fields use
depth45 through `data/config/field`; the extension object uses depth46. This
refines the unshipped draft layout without changing production JSONL. Execution
settings remain explicit typed slots; there is no whole-config or whole-record
fallback. Legacy defaults and execution-policy bounds remain reducer/importer
checks. In particular, the old positive signed64 maximum-parallel literal is
retained even where old replay subsequently narrows it to uint32.

Turn instructions may contain legacy path/size/hash metadata. They describe the
actual turn's discovered and explicit files, which can differ from the receipt
list. References are used only for known identical payloads. Inline turn leaves
also supply canonical originals for older turns without receipt events.

### Turn control, recovery and completion payloads

Draft kinds 129/130 are `turn_yield_requested` and `turn_cancel_requested`; each
contains only the 16-byte turn ID. Kind 131 is `turn_recovery`: turn ID, a one-byte
retry-count presence flag, the optional eight-byte retry count, then class and
message as length-prefixed UTF-8. Absence and zero remain distinct. The existing
reducer permits counts through `UINT32_MAX + 1`, so four bytes would lose a valid
record. Recovery class is historically open text, including empty text; preserve
it rather than restricting it to the terminal-failure classes. Its size remains
subject to the event-size bound. Diagnostic messages retain the existing 8192-byte
bound and may be empty.

Kind 132, `turn_completed`, carries turn, final-response and final-item IDs, each
16 bytes. Kind 133, `turn_completed_silent`, carries turn and response IDs followed
by a one-byte reason: 1 `room_update_quiet`, 2 `reply_reminder_exhausted`. These are
host graph identities, not provider identifier strings or copies of reply text.

Kind 134, `turn_interrupted`, carries the turn ID, one-byte origin (1 user,
2 recovery, 3 output), and one-byte reason (1 cancelled, 2 process_lost,
3 output_lost, 4 session_recovered). Kind 135, `turn_failed`, carries the turn ID,
one-byte class (1 context, 2 provider, 3 protocol, 4 tool, 5 persistence, 6 resource,
7 output, 8 internal) and a length-prefixed diagnostic message. All seven kinds
use payload version 1. Unknown tags, unsupported versions, optional-semantic flags
and trailing bytes fail without publishing a decoded event.

These codecs preserve facts without applying state transitions. The reducer must
still check the current turn, response graph and final-item identity, unfinished
tools/processes and steering, retry state, and queue authority. Recovery retains
an unfinished turn; it is not completion, cancellation or a goal transition.
The codecs remain test-only until the complete storage backend is integrated.

### Voice observations

Draft kind 257 version 1 contains connection ID16, length-prefixed provider/model
names, and the inner observation profile. The inner kind byte assigns1..9 to
started, stopped, transcript, usage, ASR failure, interruption, response, result
and muted. A64-bit presence mask selects46 named fields, in the fixed alphabetical
order of `snag_binary_voice_fields`; present values follow in that order. Reserved
mask bits are invalid. This is a fixed versioned field set, not a session quota.

Legacy admission constrained the type name and outer metadata but left individual
inner values unconstrained. Each named value therefore uses the native field-value
representation at depth3 (`data/event/field`), preserving absent versus null and
unexpected historical types. An additional native object retains unknown fields
at their original depth. Its top-level keys exclude `type` and every named field;
known fields cannot be hidden in that extension object. Empty extension objects
are valid. The native profile has no opaque JSON event-body fallback.

The inner JSON adapters reconstruct canonical historical values for conversion
and projection. Full original-envelope depth/size checks remain import duties.
Observation codecs do not start microphones, dispatch interface actions, update
billing counters or execute imported source work. Transcript/source ownership and
current connection acceptance remain application/reducer invariants.

### Typed voice-archive snapshots

Draft kind 264 version 1 contains transfer/target/source IDs16 each, original
source sequence8, enclosed kind2/version2, payload size4 and the typed enclosed
payload. Source sequence is positive signed-64. The selected enclosed profile must
accept the entire payload before exposing a borrowed view. Archive records and
seals cannot themselves be enclosed; adopted-root metadata can be retained as an
inert source observation. Unknown required kinds, unsupported versions, malformed
payloads and recursive archive records fail closed.

This codec validates structure without applying source transitions. Reference
relocation, original-source versus target-journal identity, archive closure and
causal/role checks remain converter/reducer work. Merely enclosing a source
reference does not make it a valid target-journal reference. Original source
timestamps were absent from the JSONL transfer-data wrapper and are not invented
in the enclosed payload header.

Public history has its own enclosed version, `0x8001`, within this wrapper.
Ordinary source payload versions remain supported for literal typed snapshots.
The public version cannot be decoded as an ordinary executable event. Its need is
concrete: the history producer strips provider-only payloads, emits smaller public
checkpoint and adoption views, and redacts strings. The old archive validator also
admits arbitrary object fields. Missing or redacted ordinary fields in these inert
observations must not be guessed, restored or rejected as executable-event damage.

The public profile fixes a named field list for each supported source kind. A
presence mask uses the minimum whole bytes for that list; present named values
follow in declared order, then one native extension object. Each individual field
retains its historical canonical value domain and absence/null distinction.
Extensions cannot shadow any declared name. This uses the existing native field
value representation, not an opaque whole-known-record document or a fallback
selected after ordinary schema failure. Every imported public snapshot uses this
profile regardless of whether it happens to satisfy an ordinary schema.

Archive-only source discriminator `0x7fff` denotes the public checkpoint, with
`covers_through_seq`, `provider_view` and `snapshot_v` named fields. It is not a new
journal event kind. Discriminator zero denotes an unassigned source type: its
nonempty UTF-8 type name precedes the native extension fields. Names assigned by
this frozen profile, and the forbidden archive/seal names, cannot masquerade as
unassigned types. There is no host schema to apply to genuinely unassigned names;
they remain explicitly unassigned inert observations. Neither discriminator grants
source-session access or target-session execution authority.

The version fixes the source-kind field lists and their mask positions. New lists
or changed positions require another public profile version. Named values start
at their original depth through envelope/data/archive-data/field; extensions use
the corresponding object depth. Native payload bounds and complete legacy envelope
bounds remain distinct. Conversion must retain the original journal and refuse
publication when it cannot preserve the source data faithfully. Archive references,
coordinate relocation and actual history adoption remain separate integration work.

### Auxiliary audio billing and voice-transfer anchors

Draft kind 256 version 1 contains length-prefixed provider, model and report
text for the fixed operation `dictation`. Provider/model use their existing
name bounds; report text is 1..262143 UTF-8 bytes. The report remains auxiliary
billing information, separate from coding-token accounting and execution state.

Kinds 265/266 version 1 retain sealed/adopted voice-transfer metadata. Their
common prefix is transfer/target/source IDs (16 bytes each), source-as-of
sequence8 and record count8. Both numbers are positive signed-64 values.
Adoption appends begin-offset8, begin-sequence8 and begin-hash32; offset is
positive, sequence is at least2, and both retain signed-64 bounds. The reducer
must verify session identities, earlier sequences, count versus the adopted
range, a valid prefix boundary and its hash before changing the history root.
The sealed record does not change that root.

Historical JSONL offsets and prefix hashes belong to their original coordinate
space. Conversion must resolve the corresponding native journal boundary;
copying those numbers into a binary cursor would not establish that boundary.
These codecs preserve the metadata without reading another journal or adopting
history. Archive source-reference rules remain separate work alongside the
converter and native reducer.

### Filter-rule records

Draft kinds248/249 use payload version1. Rule logging contains exactly three
native field values in chain/message/rule order, using the tagged representation
described for historical result slots below. The old validator restricted their
keys, not their value types; null, booleans, signed integers, strings, arrays and
objects therefore retain their original types. These are three specified fields
in a typed envelope. Known host records have no opaque-JSON or generic-value
fallback. Rule fields permit relative depth46 through `data/field`; result fields
retain depth45 through `data/result/field`. The shared implementation counts each
domain's fixed starting depth against the existing canonical depth48. Complete
envelope canonical-size validation remains with import/projection.

Rule transformation contains call ID16, original/effective action hashes32 each,
and a length-prefixed UTF-8 rule name, including empty text and subject to the
event-size bound. Replay verifies the pending unfinished call and original hash
before adopting its effective digest. A log record remains observational; neither
codec evaluates a rule or dispatches a transformed action.

### Download queue records

Draft kinds240/241/242 use payload version1. A queued download carries ID16,
SHA25632, bytes/mtime/queued-time counts8 each, then length-prefixed path and name.
Counts are nonnegative signed-64 values, including zero. Both texts are nonempty
NUL-free UTF-8. Path retains the existing16KiB bound; the native name field uses
the event-size bound because the legacy name limit is source-platform dependent
(`NAME_MAX` on Unix,1020 UTF-8 bytes on Windows). Source-platform root syntax and
name admission remain source-validation obligations, separately from filesystem
use; history decoding preserves foreign path spelling and name bytes. Queue
uniqueness, removal membership, file availability and transfer permission are
state/operation checks.

Removal carries ID16 and a length-prefixed reason; clear carries only the reason.
Reasons retain the existing0..1024-byte UTF-8 domain, including empty text. Encoding
these observations neither opens a file nor starts a transfer.

### Compaction records

Draft kinds224/225/226 use payload version1 for compaction start, interruption
and completion. All retain a16-byte compaction ID. Start then carries flags1
(bit0 predecessor present, bit1 continuation scope present, bit2 compaction model
present), reason1, count method1, source sequence8, input-token bound8, source/
request/count-request hashes32 each, and model/capability/profile texts. Optional
fields follow in flag order: predecessor ID16, scope32, compaction-model text.
All texts are length-prefixed. Model retains its existing name bound; capability,
profile and optional compaction model have only the event-size bound. Each is
nonempty. Absent predecessor represents the required legacy null; the other two
flags preserve field absence.

Start reasons are manual1, proactive2, hard_budget3, provider_rejection4,
model_switch5, image_boundary6 and reduce7. Its count method uses the existing
six-value enum. Unknown requires zero tokens; every other method requires a
positive signed-64 count. Source sequence is positive and signed-64 representable.
Interruption carries only ID16 and reason1: steering1, user2,
endpoint_unavailable3, context_rejected4 or error5.

Completion carries ID16, scope-presence1 (0/1), input/output count methods1 each,
input/output token bounds8 each, output-count-request/source/output hashes32 each,
optional scope32, then length-prefixed provider output. Completion excludes the
anchored method for both counts, retaining the same unknown/zero and signed-64
rules. Provider output is a canonical JSON array of1..128 objects, each with a
string `type`, up to the existing12MiB bound. Empty or unknown type strings and
unknown provider extensions remain intact. Its canonical bytes must match the
recorded output hash. This provider-boundary value is the only opaque JSON field;
the host envelope and all accounting, identity and presence fields are typed.

Reducer checks retain active-prefix/pending-work eligibility, profile and model
qualification, compaction/predecessor identity, source precedence and reduce
exceptions, scope continuity and active source hash. Import also validates the
complete original envelope's canonical depth and size. A valid standalone codec
record does not establish these state relationships or adopt compacted history.

Canonical compaction-output references select the complete original provider
array, excluding its length prefix, only from kind226. The full source record
and output hash are validated before returning borrowed completion fields.
Metadata, nested provider objects, partial arrays and equal bytes in another
event family cannot supply this role. The caller retains journal, causal and
compaction-state authority; the reference neither reads files nor adopts history.

### Context recovery and capacity rejection

Draft kind227, `context_rebased`, carries a turn ID16 and reason1:
goal_recovery1 or turn_recovery2. Kind166, `response_capacity_rejected`, carries
turn/response IDs16 each, cycle4, provider-source hash32, request hash32,
context-limit/requested-input/observed-input counts8 each, then a length-prefixed
UTF-8 diagnostic (0..255 bytes). Its code is the fixed
`context_length_exceeded`. Cycle is positive. Zero count represents the required
JSON null; nonnull counts are positive and at most4000000000. Historical integer
zero is invalid and must not be imported as null. Both kinds use payload version1.

The reducer verifies current turn/response identity, cycle and request hash,
finished work before rebasing, and the capacity ceiling calculated from the
recorded inputs plus the active requested-output count. Provider/model/source
binding and replacement of the accounting/usage anchor remain state operations.
The codecs preserve only field values; they neither apply a rebase nor adopt a
new capacity limit.

### Response-start accounting payload

Draft kind 160, payload version 1, preserves both the legacy identity/accounting
shape and the full accounting shape. It begins with a two-byte flag mask:
bit 0 full accounting, bit 1 IRC watermark present, bit 2 host snapshot present,
bit 3 baseline hash nonnull, bit 4 compaction ID nonnull, bit 5 source-bound capacity,
bit 6 hard-input limit nonnull, bit 7 requested-output limit nonnull. Higher bits
are reserved. Bits 2/5/6/7 require the full shape. Missing legacy accounting fields
stay absent; the codec never fills them from current settings. For the full
shape, an unset limit bit means a recorded null, not an omitted field.

Fixed widths below are in bytes; text uses length-prefixed UTF-8. Common fields
follow in order: turn/response IDs 16 each, cycle 4, count method 1,
input-token bound 8, request/count-request/model-input hashes 32 each, model,
capability-version and profile-ID texts. Then come the optional baseline hash 32,
optional compaction ID 16, and the existing counted list of steering IDs.
Count methods are 1 exact, 2 media_upper_bound, 3 unknown, 4 anchored_upper_bound,
5 statistical_upper_estimate and 6 qualified_upper_bound. Unknown counts carry
zero; only an anchored count carries a baseline hash. Cycle is positive.

Full accounting adds provider and effort texts, capacity source 1, provider-source
and request-input hashes 32 each, model-input bytes 8, request-input bytes 8 and
request-input count 8, then each nonnull limit 8. Capacity sources are 1 unknown,
2 advertised, 3 configured, 4 observed and 5 stale-catalog-ignored. The two byte
counts are positive. The hard-input field preserves zero as allowed by the
existing reducer; a nonnull output limit is positive and retains the existing
configuration bound. An optional IRC watermark 8 follows either shape.

The optional host snapshot is last. It reuses the typed content-list encoding
but permits only nonempty text parts and at least two entries; each entry is a
user-role data message. Literal snapshot text is preserved, not parsed as control
or a general JSON object. The reducer checks the actual boundary texts, current
turn/response cycle, provider/profile identity, admitted steering, watermark and
compaction/accounting relationships before adoption. This codec remains test-only.

### Streamed public output and canonical fragments

Draft kind 161, payload version 1, carries turn/response IDs (16 bytes each),
cycle (4), partial-public index (8), byte offset (8), and one public item. The
item carries kind (1 assistant, 2 refusal; one byte), phase (1 commentary,
2 final_answer; one byte), host item ID (16), provider item ID and original text
as length-prefixed UTF-8. Refusal permits only final_answer. Tool-call kind 3 is
reserved for complete graphs and is rejected in this public profile.

Provider IDs retain the existing 512-byte, control-free validation. Text is
nonempty and retains the existing 2 MiB public-item bound, including the fragment's
offset. The reducer still checks the current turn/response/cycle, actual append offset,
stable item identity, partial-public index and aggregate graph limits.

Output references select the entire original text field of a verified output
record. They cannot select equal bytes from provider metadata, arbitrary slices,
other event families or another reference. Resolution returns the response and
item identities, phase, partial-public index and original byte offset with the
borrowed text. Before composing a final graph, callers must bind fragments to
the same journal, earlier sequence, actual response/item identity and consecutive
byte offsets. A complete graph may also contain tool calls, so its semantic item
index is not the partial-public index. This foundation does not yet integrate
final-graph composition or runtime binary storage.

A complete item's streamed text can use a 28-byte source span: the first
canonical text reference (16), last source sequence (8), and assembled byte count
(4). Both endpoints are output records for that item. Intervening non-output
records are skipped. Every output record in the interval must keep the same
turn/response/cycle, item metadata and partial-public index, and its byte offset
must equal the assembled length. The first fragment starts at offset zero.
The resolver checks the exact first text field and requires the last fragment
to reach the declared byte count before appending any result.

The span stays fixed-size even for one-byte fragments. An explicit
16-byte reference per one-byte fragment would expand a valid 2 MiB item beyond
the event-size bound. Spans refer directly to original output records, with no
reference chains or history quota. Resolution uses the existing positional batch
reader beneath a verified immutable boundary, one bounded batch buffer and one
public-item buffer. Journal identity, causal ordering and validation of the other
event families remain the caller's responsibility. A derived index can locate
the verified anchor for the first source record. This foundation scans the selected
source interval. Interruption/failure/correction snapshots and completed-response
graphs use it in the test-only native reader. Indexed state recovery remains pending.

### Public snapshots and response interruption

A public snapshot is a four-byte count followed by typed public items. Each item
carries the kind/phase, host item ID and provider item ID used by streamed output.
Its text is either a nonempty length-prefixed literal or the length marker
`0xffffffff` followed by a 28-byte original-output span. Empty snapshots contain
only the zero count. Literal and span values are mutually exclusive. The list
decoder preserves borrowed literal views and unresolved spans; callers resolve
spans before checking unique item IDs, aggregate graph size and reducer state.
The existing aggregate graph bound measures canonical JSON representation, so a
sum of raw text byte counts alone does not establish graph admission.

Draft kind162/version1 (`response_interrupted`) carries turn/response IDs
(16 bytes each), cycle (4), origin/reason tags (1 each), and the public snapshot.
Origins reuse user1, recovery2 and output3, with steering4 added. Reasons reuse
cancelled1, process_lost2 and output_lost3, with steered5 and control6 added;
session_recovered4 remains specific to turn interruption. Steering origin and
steered reason must occur together. Existing turn-interruption profiles retain
their previous subsets. Current response identity and pending steering remain
reducer checks. Runtime integration remains under implementation.

### Response failure and optional policy fields

Draft kind 163/version 1 (`response_failed`) carries turn/response IDs (16 bytes
each), cycle (4), failure class (1), response retry count (1), optional-field mask
(1), message (length-prefixed UTF-8, up to 8192 bytes), and a public snapshot.
The response profile admits context (1), provider (2), protocol (3), resource (6),
output (7) and internal (8). Retry count retains its existing range 0 through 2.

Optional fields follow the snapshot in mask order: policy_stopped (mask 1,
one-byte boolean), turn_retry_attempts (mask 2, eight bytes), new_input (mask 4,
one-byte boolean), then policy (mask 8). Policy contains three length-prefixed
strings: code and type (each 0 through 63 bytes), then clarification_skipped
(1 through 127 bytes). Turn retry attempts retain the existing maximum 2^32.
The valid masks 0, 1, 3, 7, 15 preserve the historical field prefixes. Absent values
remain distinct from explicitly stored false or zero; an optional policy object
requires all preceding fields. Current-response admission and resolved snapshot
semantics remain reducer checks.

### Output correction

Draft kind 164/version 1 (`response_output_correction`) carries turn/response IDs
(16 bytes each), cycle (4), correction ID (16), exact length-prefixed correction
text, and a public snapshot. Text admits the existing empty-output, oversized-
output and cyber-policy clarification prompts. Policy clarification admits only
assistant items in its snapshot; the other corrections also admit refusals.
Reference-backed items retain this metadata check and require source validation
when resolved. Prompt bytes remain in the record for exact history preservation.

The correction text is a canonical input-text leaf for pending host steering.
References select its exact field, never an equal public-snapshot text or a
substring. Correction records provide no content, instructions or voice leaves.
Current response, correction-ID uniqueness, correction-use state and aggregate
pending-steering capacity remain reducer checks.

### Completed responses, calls and provider continuation

Draft kind 165/version 1 (`response_completed`) carries turn/response IDs
(16 bytes each), cycle (4), provider response ID, typed graph, usage, and
continuation form. The kind supplies the fixed `completed` status.

A graph starts with a four-byte item count. Public items retain the snapshot
encoding, including original-output spans. Tool-call kind 3 is followed by its
host call ID (16), provider item ID, provider call ID, tool name and arguments.
The four text fields are length-prefixed. The existing tool-name validator and
provider-ID rules apply. Arguments retain the provider-supplied canonical JSON
object with the existing 2MiB bound; the surrounding host item remains typed.
Graph identity, ordering, outcome and resolved 8MiB canonical-JSON aggregate
admission remain reducer checks.

Usage starts with a byte mask. Values 1, 2, 4, 8 and 16 mark known input, output,
reasoning, total and cached-input counters, stored as eight-byte integers in that
order. Value 32 preserves the cached field's presence, including explicit null;
a known cache value requires this bit. Other unknown values reconstruct as null.
Known counters retain the signed-64-bit range of the existing JSON admission
profile. Existing reasoning/output and total/input/output consistency checks
apply. Reported cache values above input remain represented; the reducer decides
whether to include them in trusted cache accounting.

Continuation form 0 omits the historical pair. Forms 1 (null) and 2 (list) carry
the 32-byte scope; form 2 then carries a four-byte count of entries. Each entry
has an eight-byte `before` index and a length-prefixed canonical provider reasoning
object. Indexes are nondecreasing and bounded by the decoded graph count. Provider
objects retain bounded extensions and pass the existing reasoning-item validator;
the host `before`/`item` wrapper stays binary. Aggregate continuation admission
uses the existing 8MiB canonical-JSON bound in the reducer. Semantic decoding,
including provider-object validation, can fail without granting truncation
authority; framing repair is a separate operation.

Completed-response graph references cover one whole typed item, retaining its
identity and argument/output fields. They require the expected assistant,
refusal or tool-call kind and reject a public value backed by a span; that value
reuses its original span directly. Continuation references cover the whole typed
list, including placement indexes and an empty list's count. An absent or null
continuation supplies no list reference. Resolution validates the whole record
and returns turn/response/cycle plus graph index or continuation scope. Equal
metadata bytes, field substrings, cross-kind slices and reference chains cannot
substitute for these originals. Journal identity, causal ordering and state
admission remain caller obligations.

### Historical tool-result field values

The legacy result validator leaves `exit_code` and `signal` types unconstrained
for several non-exit statuses. Their binary field representation preserves every
value admitted by the canonical journal: null, booleans, signed integers,
NUL-free UTF-8 strings, arrays and objects. Known result envelopes retain explicit
fields; this representation is limited to those historically polymorphic slots.

Tags 0/1/2 are null/false/true, 3 is a signed integer, 4 a string, 5 an array and
6 an object. Tags 128..255 encode integers 0..127 directly. Other integers use
tag 3 followed by canonical unsigned base-128 encoding of `2*n` for nonnegative
values or `2*(-n-1)+1` for negative values. The unsigned representation supports
all 64 bits; redundant groups and tag-3 encodings of small nonnegative integers
are invalid. Strings end with NUL; arrays and objects end with tag 7. Object
entries contain a tagged string key and a value, sorted by UTF-8 bytes with
duplicates refused. Tags 8..127 and tag 7 outside a container ending are invalid.

Terminated strings and containers keep the native representation no larger than
canonical JSON, including arrays of long strings near the existing record bound.
The decoder validates without allocating and measures canonical JSON size,
including escaped characters, against the existing 16 MiB event bound. The full
envelope must also fit its aggregate limit. Relative field depth is at most 45:
the journal's existing depth-48 rule includes `data`, `result` and the field.
Floating-point values, malformed UTF-8 and embedded NUL remain invalid. A separate
adapter reconstructs an owned legacy value when needed; normal validation returns
an immutable borrowed view.

### Tool results and process closure

Draft kind 177/version 1 (`tool_finished`) stores turn/call IDs (16 bytes each)
and a typed result. Kind 193/version 1 (`process_closed`) stores turn/process IDs,
a one-byte cause and the same result fields. Causes 1..6 mean user interrupt,
provider failure, protocol failure, tool failure, output failure and internal
failure. Closure admits succeeded, failed, signaled, timed-out, cancelled,
outcome-unknown and I/O-failed results. The reducer retains lifecycle, active
owner, call state and stream-collection checks.

The result starts with status, reason and presence flags (one byte each), then
duration (8), native exit/signal field values, the running handle (16, only for
running status), length-prefixed model text, stdout excerpt and stderr excerpt.
Status/reason numbers are the explicit enums in `store_binary_event.h`; reason
zero represents null. The existing status-specific reason and exit/signal type
rules apply. Presence bits 1/2/4 select a token limit (8), output reference and
typed content, in that order. A reference requires the token-limit field.
The limit remains 1..4000000000. Unknown flags are invalid.

Each excerpt stores encoding (one byte: 1 UTF-8, 2 base64), discarded/original/
retained byte counters (8 each) and the length-prefixed original retained string.
UTF-8 retained count equals its string size. Historical base64 admission only
requires a string byte length divisible by four; the codec preserves the string
without decoding or normalizing it. Original count must cover discarded count;
retained count need not fit original count. Nonnegative counters retain their
signed-64-bit domain. Strings remain NUL-free UTF-8. Model text and excerpts
share the record's existing 16 MiB wire bound; no separate 2 MiB text cap is added.
Typed content keeps the existing 12 MiB aggregate image-byte bound.

The output reference carries process ID (16), stdout start/end then stderr
start/end (8 each), stdin accepted/written/pending (8 each), stdin-open (one
boolean byte) and log start/end (8 each). Ranges, stream original-byte counts and
stdin accounting retain the legacy validator's relationships. The saved log
coordinates remain original data. Conversion and history rendering must resolve
their source identity and mapped range; they cannot be treated as byte positions
in a different journal. Canonical source validation remains part of import, and
resolved projection limits remain with their composers.

Canonical result references select the complete result after its owner prefix,
including all optional fields. Creation and resolution require the expected
tool-finished or process-closed kind and validate the whole record. Resolution
returns the source kind, turn, call/process owner, closure cause when applicable
and borrowed result fields. Metadata, nested leaves, partial results and other
payload roles supply no result reference. Journal identity, causal ordering,
owner state and historical-coordinate mapping remain caller responsibilities.

### Tool starts and process output

Draft kind 176/version 1 (`tool_started`) carries turn and call IDs (16 bytes each),
action SHA-256 (32) and length-prefixed resolved working directory. It retains
the existing path bound. Pending-call identity, action digest, cwd, read-only
permission and process-ownership checks remain reducer obligations.

Kind 192/version 1 (`process_output`) carries turn and process IDs (16 each),
byte offset (8), stream (1: stdout=0 or stderr=1), original JSON encoding tag
(1: UTF-8=1 or base64=2), raw byte count (4) and bytes. The encoding tag preserves
the original representation while the binary record stores decoded bytes once.
UTF-8-tagged bytes retain the existing UTF-8/NUL rules; base64-tagged bytes may
contain arbitrary octets. The existing replay limit is 1–16,384 bytes per chunk,
with offset plus size at most INT64_MAX. Process existence, source-turn ownership
and contiguous stream offsets remain state checks.

Canonical process-output references select the complete raw byte field and
return its turn/process/stream/offset/encoding metadata. They validate the whole
source record, reject metadata and partial slices, and preserve arbitrary bytes.
Input, assistant-output, graph and continuation reference roles remain separate.
Journal identity, causal sequence and process-range admission stay with callers.

### IRC observations and snapshots

Draft kind 208/version 1 (`irc_event`) carries the room-event kind 1, flags 2,
timestamp 8, an optional stream UUID16 and sequence8, then length-delimited
endpoint, room, nick and text. Room-event kinds 1..11 are connected, disconnected,
join, part, quit, nick, message, notice, topic, mode and history-ready.
Flag bits are historical=1, local=2, operator=4, watermark fields present=8,
nonempty stream=16, input=32, classification present=64, urgent=128 and reply=256.
Legacy absence of stream/sequence/input remains distinct from explicit empty
stream and zero sequence. Classification presence retains both urgent and reply.

Timestamps are positive signed-64-bit values. A nonempty stream requires a
positive signed-64-bit sequence and watermark presence; input also requires
watermark presence. Urgent/reply require classification presence. The existing
IRC admission bounds apply: endpoint 1..255 bytes, room 0..51, nick 0..30 and
text 0..4096, with valid NUL-free UTF-8 and no ASCII control bytes or DEL.
These are durable field checks; live membership, authority and delivery remain
separate. No relationship between historical, operator, input or urgency flags
is inferred by the codec.

Draft kind 209/version 1 (`irc_snapshot`) carries reason 1, timestamp 8 and
length-delimited text. Reasons 1..4 are join, nick, topology and compaction.
Timestamp is positive signed-64-bit; snapshot text is 1..8 MiB of NUL-free UTF-8.
Snapshot text preserves line breaks.

Draft kind 210/version 1 (`irc_admitted`) starts with a nonempty sequence list:
count4 followed by strictly increasing positive signed-64-bit values. Each value
uses canonical unsigned base-128 groups, low seven bits first, with the high bit
set on every byte except the last. Overlong values and more than nine bytes are
invalid. This avoids widening historically admitted short decimal sequence
lists beyond the record bound: 2,100,000 ascending sequences take 15,688,897 JSON
bytes, 16,800,004 bytes with fixed64 entries, and 6,286,343 bytes in this encoding.
Count is constrained by the containing record's available bytes, without a
separate lifetime or admission-count quota.

The list is followed by enclosed kind2: zero for no input, 96 for input received,
or 98 for steering added. A nonzero kind is followed by enclosed version2,
payload length4 and the complete typed payload for that input kind. Existing
versions 1 and 2 retain their literal/reference rules; other nested kinds,
optional semantics, unsupported versions, malformed fields and trailing bytes
are rejected. Canonical input references can select original text, content or
instructions inside this wrapper, using offsets in the outer record payload.
An enclosed reference does not become another original.

The reducer checks that admitted sequences precede this record, identify the
intended original observations and obey admission/state/authority rules. It
applies the enclosed input once at the admission record's sequence. Indexed
observation lookup and projection composition remain implementation work.

## Payload ownership and checkpoint contents

Store each original input, accepted correction, tool argument/result, retained
output chunk, provider item and compaction summary once. Later events and
checkpoints refer to stable record IDs and bounded payload slices. Use direct
references for known reuse; a global content-deduplication table is unnecessary.

Draft references occupy16 bytes: canonical sequence8, payload offset4 and slice
length4, all little-endian. They identify the same journal's record payload,
excluding its framing. Resolution checks the verified batch's sequence range,
the containing field's expected kind/version and the record's actual slice
bounds before returning borrowed bytes. Required state cannot refer to optional
metadata. Empty slices are permitted at the end of a payload. Zero/overflowing
sequences and wrapped or out-of-range slices are
invalid. Framing/reference support does not yet replace runtime checkpoint
payload copies; that follows typed checkpoint integration.

Input-field reference helpers additionally decode the source event and require
the exact bounds of the selected field: original text, complete content list,
instruction list, voice transcript or voice request. They accept receipt,
turn-start, steering/reminder and queued/edited input sources where that field exists.
Identical bytes in provider metadata, partial strings and nested content text
do not identify the original-input field. Empty instruction lists retain their
four-byte encoding; absent fields cannot supply a reference. The caller must
still check causal ordering, receipt/provenance identity and reducer authority.
Input payload version 2 preserves version 1's literal encodings and adds a
reference alternative at each original text/content/instruction/voice-text
field. A four-byte `0xffffffff` replaces the literal length or list count and
is followed by a one-byte source-field ID and the 16-byte reference. Source-field IDs are
1 original text, 2 complete content, 3 instructions, 4 voice transcript and 5 voice
request. Text fields can reuse any of the three text leaf roles; lists require
the matching list role. Existing field-size bounds apply to referenced sizes.
Each C value supplies either its literal or its reference; conflicting values
are rejected. Optional content retains its presence bit even when referenced.

The decoder accepts input payload versions 1/2; an input-family version 1 record
cannot carry reference markers. Turn-start version 1 includes references from its
first definition. Other current families remain at version 1. Encoders report
their required version through `snag_binary_event_version`. Structural decode
retains unresolved references, with empty adjacent literal views. Before reducer
adoption, callers resolve every required reference against the same journal,
check causal ordering and target-field constraints, and preserve provenance.
Only inline fields supply canonical leaves: metadata-only edits copy the
original reference instead of creating reference chains. Runtime integration
and import construction remain pending; codecs are test-only.

Control-text reference helpers select the created directory(1), changed directory's
`after` field(2), banner(6), scheduled timer text(32), goal prompt(64..66), or blocker(69).
They pin the caller's exact source kind, validate the entire record and require the
whole field's original coordinates. Identical text in provider metadata or the
directory's `before` field cannot substitute for the selected leaf. A zero-length
banner is a present clearing field; timer fire/cancel, goal status/lock and steering
mode records supply no text leaf. Resolution returns the decoded source event and
borrowed text, preserving goal/replacement IDs, actor, timer ID/deadline and other
payload metadata for the caller's owner, lifecycle and causal checks. These
test-linked selectors prepare canonical checkpoint references; they do not validate
current checkpoint state, grant source authority, or change any event wire form.

The test-linked accounting block in `store_binary_checkpoint.c` encodes four
independent observations in this order: active request, completed usage anchor,
context meter and capacity rejection; session usage totals follow. Version 1
starts with a little-endian u16 version. Each observation has one flags byte:
bit 0 is validity, bits 1..5 mark the presence of the compaction ID and four
source/input/request digests. Bits 6..7 are reserved zero. Provider, model and
effort are u16-byte-length UTF-8 fields within their existing C storage widths.
The optional ID/digests use 16/32 binary bytes: compaction ID, provider-source
hash, model-input hash, request-input hash and request hash. Five u64
values follow: model-input bytes, request-input bytes, request-input count,
input tokens and requested-output tokens. Totals use seven u64 values (responses,
input, cached input, uncached input, output, reasoning and total tokens), then a
boolean cached-seen byte. All integers are little-endian; counters retain their
full unsigned range. Invalid observations retain their fields, and absent IDs
remain distinct from present all-zero IDs. The decoder consumes exactly one
block, rejects unknown versions/flags, malformed text and non-boolean flags,
and preserves the destination on failure. This block carries metadata only;
complete core/provider bodies, reference authority and snapshot adoption remain
separate work.

The control-metadata block has its own u16 version (currently1), followed by a
u32 bitmap for21 booleans; higher bits are reserved zero. The draft omits the
retired session-archive flag, matching the current runtime state. Its24 fixed metadata
fields follow `control_texts` order in `store_binary_checkpoint.c`: optional
IDs/digests have a boolean presence byte and16/32 binary bytes, while other
metadata uses u16-length UTF-8. Absent IDs remain distinct from present zero
IDs. The37 numeric fields follow `control_numbers` order, with explicit u32/u64
wire widths independent of C member layout. Current enum and control-mask values
are checked; the retained source semantic format is2,3 or4. Size-dependent C
counters use u64 on disk and fail with overflow if a reader cannot represent them.
Six u64 control-event sequences finish the block. `control_flags` defines the
boolean bit order. The existing bounded text/hex primitives are shared with the
accounting block without changing its wire layout.

Control decoding stages all fields before updating the caller's provisional
state. It preserves every unlisted byte, including descriptors, callbacks,
payload ownership, arrays, accounting and voice-history state. Session identity,
journal digest/end/next sequence/turn count and legacy checkpoint coordinates
are excluded; the complete loader must bind the common frame and determine its
own checkpoint positions. Last-event time and other scalar state are retained,
not inferred from a frame checksum. Exact framing, field ranges and text validity
are structural checks; lifecycle coherence, causal bounds and reference authority
remain the complete snapshot consumer's responsibility. Both metadata blocks
remain test-linked building blocks rather than complete core/provider decoders.

The fixed-text origin block in `store_binary_checkpoint_text.c` records cwd,
first/last user text, active prompt, goal prompt/blocker, timer text, banner and
steering mode, then the latest IRC display snapshot. It is260 bytes: LEu16 version1, LEu64 last observed
semantic sequence, then ten25-byte entries. An entry contains the accepting
LEu64 declaration sequence, a u8 field selector and the16-byte original-field
reference. Absent entries are entirely zero. Selector0 identifies control text
in the declaration itself; input selectors1/4/5 identify original text, voice
transcript or voice request bytes. These references name literal fields directly,
not another reference. Steering retains its declaration but no byte slice: its
text is the label of the stored enum. The initial empty IRC display snapshot
names only its creation declaration at sequence1, with no byte slice. A later
IRC snapshot names its own exact text field and declaration; it supplies display
metadata, never execution authority or a restored network connection. This block
stores no payload bytes.

The allocation-free producer hook runs after strict native reduction and keeps
original byte provenance separate from the accepting declaration. It starts at
creation, advances monotonically through semantic events, and drops slots only
when the reducer clears them. Optional metadata may follow its last observed
sequence. Model goal replacements/rewords do not replace last-user text; user
changes do. A blocker can survive user replacements while the goal stays blocked,
even across multiple parent IDs; its source must not be falsely rebound to the
newest goal. Equal text never substitutes for these transitions. Native replay
returns this table only after whole-prefix validation and source recheck; staging
exposes it only after its complete equivalence/source checks succeed.

The text reader checks each declaration's field role and exact original tuple,
then resolves the whole canonical literal using verified backward lookup. It
returns a fresh owning dictionary of these fixed slots without changing an
existing session or file position; failures preserve the output pointer. Buffer
encode/decode is atomic and exact-length. The dictionary's JSON representation is
an in-memory adapter, not checkpoint payload storage. Scratch uses the existing
batch bound; lookup cost depends on source distance, not an index or constant-time
promise. This component does not establish that a decoded table is the current
snapshot: the complete consumer still owns snapshot authority, owner/lifetime
coherence, remaining core/provider reconstruction and atomic adoption.

The pending-call block in `store_binary_checkpoint_calls.c` stores one completed
graph sequence and its graph-time cwd origin, followed by call lifecycle bits in
graph call order. Its version1 header is42 bytes: LEu16 version, LEu64 graph
sequence, LEu64 cwd declaration, the16-byte canonical cwd reference and LEu64
call count. Each call adds one byte (bit0 started, bit1 finished; higher bits
zero). An empty list has zero origins/count. A nonempty list requires a positive
cwd declaration before the graph and a same-declaration canonical control-text
reference. Count must account for the input exactly, without a new session quota.

Native replay keeps graph-time cwd independent of subsequent directory changes;
it drops the graph origin when the reducer clears pending calls. Replay and import
return fixed-text and pending-call origins together, only after their existing
complete-prefix/source checks. Encoding reads status bits from the same provisional
core state and stages one atomic append. Decode borrows the status bytes; the
encoded block must remain alive and unchanged while that view is used.

The reader checks the completed graph's turn, response and cycle against the
provisional snapshot and requires its exact call count/order. It reconstructs
metadata from original arguments and the canonical created/changed cwd leaf.
The reducer and reader share the pending-call derivation routine, including
UTF-8-safe command/workdir previews and the graph-time action digest. No command
or tool is executed. Status bits preserve independently started/finished values;
source checks alone do not establish their current lifecycle authority. Success
returns a new owned array; failure preserves the caller's output pointer. Like
fixed text, lookup uses an independently trusted same-journal immutable prefix
and bounded batch scratch, with cost proportional to source distance. Process
ownership, whole-snapshot coherence and atomic session adoption remain separate.

Process checkpoint entries retain each accepting tool-start sequence and its
original graph/cwd origins independently of the current response. The
version1 block has an LEu16 version and LEu64 count, then97 bytes per process:
start sequence8, graph sequence8, cwd declaration8, cwd reference16, seven u64
counters (stdout/stderr output, stdout/stderr collected, stdin accepted/written/
pending), and one flags byte (ready bit0, draining bit1). Higher flag bits are
zero. Handles and command/workdir previews are derived from the original call,
not copied into this block. The source reader binds the tool start's call, turn
and action digest to the original graph. A legacy write_stdin-created process
retains its historical empty previews and referenced handle.

Action digests can change through accepted `rule_transform` records after a graph
completes. Pending-call readers apply each matching original/effective digest pair
in sequence through the trusted checkpoint boundary; process readers stop before
the accepting tool start. A new response or turn boundary cannot be crossed. This
uses bounded batch scratch, not another payload copy or a session-wide provenance
table. Lookup plus transformation scanning is proportional to the source window;
it is not constant-time restore. Labels remain the original graph's previews,
as in the reducer, even when a transformation changes the dispatched payload.

Process source sets returned by replay/import now own an array; callers release
it with `snag_binary_checkpoint_sources_free` before reuse. Only complete success
transfers new ownership. The wire table has strictly increasing start sequences;
read rejects duplicate derived handles. Metadata counters retain their full u64
domain; cross-field coherence and lifecycle authority are complete-consumer work.
Decode borrows its immutable input; encode stages an atomic append, and read
returns a newly owned array or preserves the old output pointer on failure.

The reducer does not derive the process log_offset/log_seq/log_hash scan caches;
the JSONL app updates them after starts and collections. These physical caches
are excluded from native process metadata. A complete native consumer must build
its own verified scan cursors and retain byte-collection semantics, not reinterpret
old JSONL offsets. Saved ready/draining flags describe snapshot observations and
do not recreate a live process or prove its current OS ownership. Complete
snapshot/lifecycle authority and native cursor construction remain separate.

The pending-input block in `store_binary_checkpoint_inputs.c` references accepted
direct input, queued work and steering without copying their payloads. Version1
has a 26-byte header: LEu16 version, then LEu64 direct-receipt sequence (zero when
absent), queue count and steering count. Queue entries are 24 bytes: creation
sequence, latest text/edit receipt and first-context timestamp. Steering entries
are 16 bytes: receipt sequence and first-context timestamp. Both lists retain
strict receipt order; queue edits may occur in any order. Timestamps use unsigned
64-bit fields. Exact length/count validation and checked size arithmetic precede
allocation; the block adds no history quota. Decode borrows its input; encode
stages an atomic append even when its source arrays alias the output buffer.

The reader authenticates each receipt against an independently trusted immutable
journal anchor. Direct and embedded IRC input use the same reference hydration as
replay, preserving selection, read-only, timer origin, instructions and content.
Queue creation supplies identity, order and content; the latest matching receipt
supplies text, read-only and received time. An edit's content remains ignored by
queue state, as in the reducer. Steering accepts plain, embedded IRC, reminder
and output-correction receipts, checks the provisional active turn, and restores
first-context times separately from receipt times. Corrections supply their ID
and prompt with absent content; their partial-public snapshots remain separate
history. Historical missing receipt times use the journal timestamp. Returned
arrays retain text through their owned dictionary and own their content references. Existing pending-text limits and duplicate-ID checks
apply. Failure leaves the old output unchanged; success returns new ownership,
released with `snag_binary_checkpoint_inputs_free`.

These source and shape checks do not prove that a receipt remains pending at the
snapshot boundary, that a selected edit is the latest accepted edit, or that a
timestamp is a valid admission. The full consumer must bind snapshot membership,
ID-reuse epochs and lifecycle authority, merge the string dictionary, and adopt
all core/provider fields together. This component performs no reduction, dispatch
or live input admission. Active-turn instructions and provider-view state remain
outside this pending-input block.

The test-linked dynamic-core payload block stores version1 in a 51-byte header:
LEu16 version, then six LEu64 values (accepting turn, compaction start, compaction
completion, response start, last streamed output, download count), followed by a
one-byte download-presence flag. Ordered eight-byte download receipt sequences
follow. Zero declarations mean absent payloads; compaction and response pairs
must be complete and ordered. A response range starts after its accepting turn.
The flag distinguishes an absent download queue from a present empty queue.
There are no copied instruction lists, compaction arrays, stream strings or
file metadata in the checkpoint. Decode borrows receipt bytes; encode stages an
atomic append, including when source storage aliases its destination.

Full replay retains the streamed response's start origin and last output,
the most recent completed compaction's start/completion pair, and only pending
download origins. Download removals compact that list without changing order;
reusing a removed ID creates a new receipt. An interrupted/new compaction leaves
the previous accepted compaction output intact. Instruction ownership uses the
existing active-prompt accepting-turn declaration, not text equality.

The reader checks the active turn ID and number before restoring its instruction
list, resolving a declared earlier canonical list with the existing field-role
and coordinate checks. Path-only and original snapshot metadata entries retain
their individual forms, including empty lists. Compaction restoration validates
the start/completion IDs, source sequence/hash, scope and canonical output hash
against the provisional core metadata. It preserves the complete provider output
array. Stream restoration checks the current response owner/cycle, reads the
authenticated response range, rejects crossed response/turn endings or restarts,
and feeds only streamed fragments into the strict reducer in private state.
Original item metadata, indexes, offsets and aggregate byte counts are checked
there; non-output events may intervene. A closed response can retain its streamed
prefix until explicit response clearing or restart. This retained-stream origin
has a different lifetime from the existing open-response reference guard; closure
still clears that guard. Completion/partial-public snapshot data does not replace
the retained stream. Work is proportional to the selected response range and
backward source distance; this is not indexed constant-time resume. Downloads use the same strict reducer for original receipt data and
uniqueness, with no file opening, file transfer or command dispatch.

Read returns new owned JSON payloads only on success, preserving the caller's
previous output and file position on failure. JSON exists only as the existing
in-memory core representation. Source checks alone do not prove that a checkpoint
contains the latest accepted payloads or precisely the still-pending downloads;
the enclosing complete snapshot consumer owns membership, lifecycle, immutable
journal identity and atomic core/provider adoption. Provider-view snapshots and
native process collection cursors remain separate dependencies.

The version1 core candidate in `store_binary_checkpoint_core.c` joins these
seven components in fixed order: controls, accounting, fixed texts, pending
calls, processes, pending inputs and dynamic payloads. Its76-byte header contains
LEu16 version1, LEu16 component count7, LEu64 active-compaction accepting sequence,
LEu64 retained-response accepting sequence, then seven LEu64 component sizes.
Every component retains its own version and exact-length validation. The active
compaction attempt is separate from the last completed compaction; a retained
response epoch also survives when no public stream has been emitted. Neither
field copies payloads or replaces the strict replay response-open guard.

Encoding stages the whole append. Reading requires an already verified frame
bound to the same immutable journal and returns a provisional state-only session
and owning origin table together. It restores identity and batch coordinates
from that common frame, merges the fixed and pending-input string owners, and
checks reconstructed queue/steering/stream byte totals. Source positions must
fit the saved semantic boundary. Response and active-compaction declarations
must match their control metadata and accepting boundaries. Re-encoding uses
the returned origins without copying journal payloads into the core body.

Only reachable string owners are retained. Process scan caches, callbacks,
resources and derived provider/history views start empty. Existing voice-history
adoption cursors still require relocation; a core producer with such a cursor
returns `ENOTSUP`. The native replay guard for adoption remains in place. Full
latest-membership/lifecycle validation, provider decoding, native process scan
cursors and atomic joint adoption remain the enclosing consumer's work. This
assembly layer remains test-linked; runtime storage continues to use JSONL.

A full checkpoint captures all current semantic state:

- Journal boundary, sequence, chain digest, session identity and turn count.
- Active turn/request, selection and accounting provenance, context coverage,
  compaction/rebase lineage and exact retained provider-view references.
- Original input references, deferred/admitted steering and IRC watermarks,
  queued work, goals, timers and current configuration-derived state.
- Pending calls, completed-result references, unsettled process ownership,
  accepted versus written stdin, collection cursors and output positions.
- Derived-view invalidation/rebuild disposition and the bounded event seam.

Full state means sufficient state to resume, including every relevant reference.
It does not mean another copy of the lifetime transcript or active payloads.
Snapshot decoding loads the bounded active payload set, not the complete history.
A dead process remains lost/unknown; its saved ownership record cannot recreate
it. Provider-private data stays excluded from model-readable history.

Canonical output chunks remain history after checkpointing. Presentation deltas
that are merely repeated prefixes, temporary serialization buffers and previous
snapshots can be discarded. Unique inputs, output, tool outcomes and authority
changes cannot be discarded merely because a summary or checkpoint covers them.
Text correction and replacement retain explicit provenance.

## Provider checkpoint recipe

The test-linked version1 provider section declares a rebuild disposition. A first
request rebuilds through the shared renderer using the retained event seam and
current caller configuration; later requests reuse the ordinary incremental cache.
Retry notices, input timing and host snapshots derive from those canonical events.
Materialized request arrays, serialized provider scope and opaque cache JSON stay
outside the section. Canonical prompts, output, media and IRC payloads remain in
the journal, referenced by sequence under the frame's identity and exact boundary.

The48-byte little-endian header contains version1 (u16), disposition1 (u16),
zero reserved bits (u32), then five u64 values: next sequence, compaction sequence,
rebase sequence, recent-row count and historical-source count. Each36-byte recent
row contains sequence and post-reduction time (two u64), four flag bits in a u32
(active turn, unfinished calls, processes, turn-ID presence), and a16-byte turn ID.
Absent IDs have all-zero bytes; present all-zero IDs remain distinguishable. Each
8-byte historical row names one canonical IRC source or import-marker position.
Both lists have strictly increasing positive sequences below the recorded next
sequence. The recent list is nonempty. Counts consume exactly the section; unknown
versions/dispositions, reserved bits, impossible active/ID combinations, overflow
and trailing bytes fail without changing the decoded output. Encoding stages its
bytes before append. No per-event size limit is imposed on this active-state section.

Structural decoding produces borrowed row views. Source membership, source role,
post-event metadata, retention completeness and current lifecycle authority require
semantic verification. `snag_store_verify_binary_context_checkpoint` establishes
the requested boundary by strict header-to-prefix replay, decodes the complete
frame against that identity/boundary, and compares both sections with the canonical
core and provider encodings reconstructed there. A valid checksum cannot substitute
for this common-boundary check. An omitted seam entry or stale core field fails
even when each section is structurally valid.

After that comparison, the core reader loads the candidate's field-shaped sections.
The provider materializer reconstructs fresh event metadata from recipe rows and
shares immutable payload values from the replay-verified canonical source pool.
That pool has already resolved native references with the ordinary response-epoch,
receipt and source-role checks. Missing
sources, wrong source classes and values outside the existing JSON projection
domain fail without replacing either output array. The seeded capture owns separate
recent and pending arrays; both initially cover the recipe because its first
projection explicitly rebuilds. That incremental cursor has no effect while rebuild
is required. A final cancellation/source identity/size/time check precedes joint
adoption of the loaded core, capture and optional origins.

This verified materialization still reads the whole requested prefix to establish
authority and its canonical payload pool. Direct selected-record loading, suffix
resume, index authority, alternating publication and a runtime selector remain open.
The core encoder's outstanding voice-root relocation restriction remains ENOTSUP.
Generation selection and durable publication remain writer responsibilities.

## Checkpoint file framing

The test-linked frame codec binds separately versioned core and provider sections
to one generation, journal identity and committed boundary. Its draft0.1 envelope
uses a160-byte header, the core bytes, the provider bytes, and a48-byte footer.
All integer fields are little-endian. Header offsets are:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | Magic `SNAGCHK` followed by NUL |
| 8 | 2+2 | Frame major0, minor1 |
| 12 | 4 | Header size160 |
| 16 | 8 | Complete file size, including footer |
| 24 | 8 | Nonzero checkpoint generation |
| 32 | 16 | Journal session UUID |
| 48 | 8 | Journal creation time in milliseconds |
| 56 | 8 | Committed journal end |
| 64 | 8 | Next canonical sequence |
| 72 | 8 | Committed turn count |
| 80 | 8 | Start of the last committed batch, or zero at the journal header |
| 88 | 32 | Committed boundary digest |
| 120 | 2+2 | Core and provider schema versions, each nonzero |
| 124 | 4 | Reserved feature bits, zero |
| 128 | 8+8 | Core and provider section sizes, each nonzero |
| 144 | 16 | Reserved, zero |

The footer contains `SNAGCPE` followed by NUL, the repeated complete file size,
and SHA-256 of every preceding byte, including the footer magic and size. The
declared lengths must account for the file exactly; trailing bytes are invalid.
Checked lengths fit the host address space and signed64 file coordinates. The
frame has no per-event size cap; its caller supplies the buffer resource budget.

Decode requires an independently authenticated identity and boundary from the
same immutable journal, and matches every member before returning borrowed
section views. A short header or an otherwise valid incomplete frame returns the
incomplete result without changing the output. Malformed complete headers,
overflow, mismatched boundaries,
unsupported frame versions/features and checksum failures are errors. Encode
appends atomically and permits source sections to borrow its destination buffer.

Each section's own version is retained for its required field-shaped semantic
decoder. Those decoders must validate complete core/provider state, common-boundary
consistency and canonical reference provenance before adoption; an unknown required
section version fails state loading. Frame decoding alone supplies no state or
resume authority. The current tests use synthetic section bytes to exercise
framing, including snapshots larger than one event. Complete typed checkpoint
bodies, generation selection, alternating durable publication and suffix resume
remain unfinished. Runtime storage remains JSONL.

## Checkpoint cadence and publication

The default timer is five minutes of dirty state, measured monotonically.
Unchanged idle sessions write nothing. A byte-work trigger also bounds recovery
for high-output sessions: initially target 32 MiB of committed suffix, allowing
one final permitted batch. Checkpoint before admitting another batch beyond that
budget. This is a bound on replay work, independent of session lifetime.
Checkpoint after a material context compaction/rebase when it substantially
reduces retained state, and on orderly shutdown when dirty. Repeated goal-turn
final answers do not by themselves force full snapshots.

The engine captures an immutable view of one committed boundary. New commits
may continue after it, subject to the suffix budget. Publication is:

1. Ensure the journal through the captured boundary is durably committed.
2. Encode the checkpoint with that exact boundary and content checksum into a
   new file in the same private session directory.
3. Synchronize the new file; replace the older checkpoint slot by rename.
4. Synchronize the directory before considering the new generation published.

Keep the other valid generation until its next alternating replacement. A
snapshot failure leaves journal commitments intact and the previous recovery
point available. Pace retries; never change ACTIVE goal status to hide failure.
At the replay-work boundary, backpressure new admissions and report the storage
failure. Existing process collectors retain their ownership and ordinary bounded
backpressure behavior; they must not claim discarded bytes were captured.

A valid previous checkpoint may be used only after validating its journal anchor
and replaying every complete subsequent commit. Never rewind acknowledged work.
A damaged canonical batch is an error, not permission to skip it. If both
snapshots are unusable, repair is an explicit, interruptible operation; normal
resume does not silently scan an arbitrarily long lifetime prefix.

## Index and bounded navigation

Use a fixed-width entry per canonical sequence in `history.idx`, containing the
batch offset, record position, record kind and monotonically increasing session
turn ordinal. The latter identifies the surrounding turn epoch; an event's
originating tool/turn ID remains separate because old processes may produce
output during later turns. The index header binds format, session UUID and
journal identity. Index entries have independent corruption checks.

Sequence lookup is a checked offset calculation. Turn lookup is a binary search
on the monotone ordinal, followed by verification of the referenced turn-start
record. This avoids a second turn-index file. Sequential/reverse history pages
read bounded index windows and bounded canonical batches. Filters report scan
progress, skipped ranges, returned counts and next cursors honestly.

Readers validate index hints against canonical record kind, sequence, identity
and batch digest. The index may lag, be truncated, or be discarded without
changing session meaning. Rebuild missing ranges in bounded work units; explicit
history tools report incomplete indexing and resumable progress. New work and
normal checkpoint-based resume do not require a full index rebuild. Index writes
need no independent durability barrier because the journal can reproduce them.

## One I/O owner and sparse durability barriers

Use one session I/O worker for ordered append, hashing, checkpoint encoding and
publication, and bounded indexed reads. Reuse the existing portable thread/wake
facilities. Keep the engine/reducer on its current owner thread. The worker owns
immutable buffers/references, not mutable application state or a second reducer.
Prioritize required commit acknowledgements over checkpoint/index maintenance;
large maintenance operations yield between bounded chunks.

Distinguish queued, written and durable sequence positions. The engine stages a
batch and adopts it only on a successful durable acknowledgement. A queue offer
or successful write syscall cannot report a tool result as saved. Batch all
already-ready independent transitions at one engine boundary. Returning success
from today's synchronous per-event API and syncing later is not group commit;
callers must be changed to express the shared transaction explicitly.

Required barriers are causal boundaries:

| Boundary | Rule |
|---|---|
| Accepted input / saved state | Durable before acknowledgement that promises persistence |
| Tool or external request dispatch | Durable input, intent and required prior results before the external effect |
| Result supplied to the model / sealed output | Durable before releasing the dependent action or sealed output |
| Checkpoint publication | Journal boundary first, snapshot file next, directory last |
| Orderly close | Drain accepted work, commit, then publish dirty state |
| Rebuildable indexes / unchanged idle state | No independent sync |

Transient streaming may remain an explicitly unsealed preview. Sealing waits
for the corresponding group commit. Do not weaken the existing sealed-output
contract by quietly declaring already-visible permanent output expendable.
Collect consecutive output fragments into bounded chunks and flush at meaningful
render/transition boundaries; never issue a durability barrier for each network
delta or each cosmetic status repaint. Preserve responsive cancellation and
steering while waiting for I/O. Avoid an artificial delay on isolated input or a
ready tool dispatch merely to chase a larger batch.

On Linux, use `fdatasync` for ordinary journal durability when supported; keep
platform-specific file/directory synchronization behind the existing portability
layer. A sync failure is ambiguous, not proof that no bytes persisted. Preserve
the pending batch identity, reconcile/retry it once, and never allocate duplicate
sequences or repeat an external effect to conceal the error. An incomplete final
batch can be removed under the exclusive writer lock; corruption inside a known
committed prefix fails visibly.

Five minutes controls recovery snapshots. It is not the durability interval for
acknowledged input, completed tool work or committed history. The design adds no
silent five-minute loss mode. A timed lossy mode would be a separate product
contract and is not selected here.

## Compatibility and implementation order

Ship new-format writing only after typed event coverage and recovery semantics
are complete. Preserve current JSONL reading and writing for existing sessions
initially. Migration is a separate stopped-session operation under its writer
lock: stream the old journal, preserve public event IDs and provenance, replace
superseded checkpoint payloads with small import markers where sequence
continuity requires them, build the new checkpoint/index, verify equivalence,
synchronize, and publish the format selection last. Retain the old source for
rollback and reject attempts by an older writer to append to migrated state.
Never change a live operator journal as an implementation experiment.

The converter is built into `snajpagent`, including bulk conversion of the saved
session collection with bounded parallel workers. Each worker holds one stopped
session's exclusive writer lock throughout validation and publication. Report
locked sessions as skipped, and continue independent sessions after an individual
failure. Worker count bounds aggregate decoding memory and disk pressure; do not
spawn one worker per session. Preserve source journals and per-session diagnostics.
Bulk success requires every selected unlocked session to pass equivalence checks;
a partial run reports converted, already-current, skipped and failed counts.

Reuse the existing JSONL reader, reducer, checkpoint decoder and compatibility
rules behind a legacy import adapter. Keep semantic recovery in one place;
the binary backend changes storage and references, not the meaning of tool,
input, goal or context transitions. A legacy checkpoint that cannot be decoded
does not condemn its canonical history: explicit conversion tries verified
anchors and replay, reconstructs derived state, and reports repairs performed.
Rebuild indexes and use original defaults for absent optional legacy fields.
Retain original source bytes even when replacing superseded snapshots.

The internal `snag_store_reconcile_legacy` primitive now provides strict,
read-only replay from byte zero under a caller-held source writer lock. It uses
the existing bounded reader, canonical JSON/hash checks and reducer, discards
derived checkpoint bodies, repairs derived checkpoint pointers in memory and
reports exact unresolved sequence/byte ranges. It retains known legacy defaults
but does not silently drop invalid format2 transitions or write cwd refusal
logs. It reports torn-tail bytes without truncating them, checks source identity
and size around replay, and publishes the reconstructed state only on success.
Callback state is provisional. Provider context still needs reconstruction;
this primitive does not implement anchor-based suffix recovery, binary output,
bulk scheduling, full equivalence verification or cutover. Ordinary resume
retains its existing checkpoint and validation behavior.

If corruption prevents complete state, preserve independently verified history
and recovery evidence without publishing an executable session that invents
missing authority or completed effects. Report exact unresolved sequence/byte
ranges and continuation constraints. Distinguish a torn uncommitted tail from a
corrupt committed record. Recovery should reconcile everything supported by the
available evidence, rather than treating the first validation failure as final.

Old instances may keep appending for hours after a test conversion. Record the
source file identity, verified sequence/hash and length; copied-source outputs
are provisional and never select the active format. Acquire the original writer
lock before final validation and cutover. If the source has advanced, reconvert
the stopped source. Incremental catch-up is an optional optimization only when
the verified prefix still matches and the implementation stays simple. Do not
invent a live replication protocol. Publication must also prevent an older
executable from resuming writes into the superseded legacy journal; preserve
that journal under a rollback name with crash-recoverable publication ordering.

Implement in dependency order:

1. Typed framing/codecs, bounded batch reader and tail recovery in existing
   store tests; exact byte fixtures independent of native struct layout.
2. Event/reference representation and alternating full-state snapshots; replace
   per-128-event embedded snapshots without weakening lineage or durability.
3. Derived fixed-width index and bounded history/process-output navigation.
4. I/O worker and caller transaction batching, preserving every acknowledgement
   and effect boundary. Measure CPU, bytes written and sync count separately.
5. Legacy import and private fixture compatibility before any real migration.

Use existing store/context/CLI/PTY fixtures. Cover short writes, sync errors,
torn batch/header/footer/snapshot, crash points around rename, corrupt index,
missing snapshot, overflow, dirty/idle timer behavior, huge old prefixes, long
single turns, live tools, steering and model changes. Kill-based tests exercise
process recovery; they do not prove power-loss persistence. Reordered/torn-byte
fixtures and the documented filesystem/device contract are also required.

## Durability references

Checked September 27, 2026:

- Linux man-pages `fsync(2)`: file sync and directory-entry persistence are
  separate; `fdatasync` also flushes metadata necessary for data retrieval.
  Source: `https://man7.org/linux/man-pages/man2/fsync.2.html`.
- PostgreSQL documentation, Write-Ahead Logging: log-before-data ordering and
  grouping concurrent commitments behind one log flush.
  Source: `https://www.postgresql.org/docs/current/wal-intro.html`.
- SQLite, Atomic Commit: partial/reordered writes, explicit flush boundaries and
  the dependence of crash guarantees on the storage stack's actual behavior.
  Source: `https://www.sqlite.org/atomiccommit.html`.

These sources support the persistence primitives and failure model. The file
layout and scheduling policy above are this project's engineering decisions;
the references do not validate an implementation that has not been built.
