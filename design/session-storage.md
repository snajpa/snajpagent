<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Binary session storage

## Status and purpose

Engineering design, September 27, 2026. The installed quota rescue keeps the
existing JSONL format. This document defines its successor; binary storage,
checkpoint replacement and grouped durability are not implemented yet.

The storage contract preserves canonical history, exact input authority,
completed tool results, context lineage and single-writer ownership. Active
state and recovery work depend on the retained working set and a bounded suffix,
while history may grow to the filesystem and checked 64-bit representation
limits. There is no session byte, event or turn quota.

The measured failure makes the first optimization clear: 422 embedded full
checkpoints accounted for 2,050,400,577 of 2,113,498,737 journal bytes. Changing
serialization alone would leave that repeated-state growth intact.

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
   batch offset and previous batch digest.
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

## Payload ownership and checkpoint contents

Store each original input, accepted correction, tool argument/result, retained
output chunk, provider item and compaction summary once. Later events and
checkpoints refer to stable record IDs and bounded payload slices. Use direct
references for known reuse; a global content-deduplication table is unnecessary.

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
