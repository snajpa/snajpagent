<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent 0.99.8 — September 23, 2026

Durable long-session recovery, truthful interactive input and complete tool and
IRC control.

These notes describe the immutable 0.99.8 tag and its companion downloads.
Later source changes are listed under Unreleased in CHANGELOG.md.

## Changes

- Long open turns that accumulate more than 12 MiB of image input now recover
  without replaying the same local projection error. The runtime retains every
  completed result and omits already-presented historical pixels from
  compaction requests. A rejected active request continues from the current
  input with bounded history access instead of re-compacting the same turn;
  durable media and completed tool effects remain intact.
- Context capacity follows the selected binding's explicit limits and advertised
  maximum. A smaller normal working window remains a policy value and does not
  replace the hard ceiling. Compaction starts at the effective boundary or a
  typed provider rejection. Normal compaction covers complete call/result
  groups in overlapping chunks; a rejected active turn rebases to current
  input, current goal and exact pending steering, with the historical journal
  reachable through bounded tools rather than replayed automatically.
- Resume preserves completed tools, unfinished outcomes and the selected
  provider/model binding. Same-model, same-family and smaller-binding changes no
  longer replay completed calls, switch back to the previous model for summary
  work or loop over already-covered history.
- Every nonblank interactive submission appears in scrollback immediately.
  Stdin remains live for type-ahead while the ready prompt stays hidden until
  acknowledgement; whitespace-only Enter remains local and creates no turn or
  request. The presentation owner also handles one-shot output, immediate view
  changes, asynchronous backlog backfill and per-room IRC tabs.
- Steering preserves valid calls already emitted in an accepted response.
  Commands can yield with live handles, retain the full tool catalog, and finish
  without lost or replayed work.
- Complete redacted command output receives durable stream/range identities.
  `read_tool_output` pages saved bytes after result truncation, cache eviction,
  compaction or resume. `set_command_shell` selects a durable per-session shell
  while preserving exact command bytes and existing security boundaries.
- Execution limits now form one visible policy with per-model overrides for
  yield, maximum wait, timeout, parallel handles, result size and output cache.
  Configured waits can use the full 32-bit millisecond range while input,
  steering, interruption and process collection remain responsive.
- The embedded IRC server and outgoing connections run on independent runtime
  workers. Models can inspect state, send, change nick/topic, host, connect and
  disconnect; `/disconnect` controls model-created clients too, and topic
  authorization follows the room's live `+t` policy.
- Provider compatibility, media token bounds, model-cache binding and the
  portable build/configuration matrix include the fixes accumulated after
  0.99.7.

## Upgrading sessions and history

Back up prompt history before manually editing it, and do not run older builds
against newer history files. Existing sessions replay unchanged; resumed work
uses current capacity, output-retrieval and recovery rules without rerunning
completed tools.

## Downloads and updates

Choose the executable for the exact OS/ABI and architecture and verify
`SHA256SUMS`. Each target includes matching symbols; the release also contains
source, build instructions, the manual and dependency license notices. Use a
legacy variant only on its matching OS ABI.

Official stable standalone binaries install matching updates in the background;
the old process keeps running and one <redacted:secret> banner gives restart advice. Set
`[agent] auto_update = false` to opt out. Development builds use latest-dev,
retain debug information and default to updates off.

## Platform requirements and scope

The release includes every target in the source `PROD_TARGETS` matrix: Linux
x86-64, AArch64, ARMv6, RISC-V 64, PowerPC 64 LE and i686; macOS
Intel, Apple Silicon and universal; Windows x64 and ARM64; current and legacy
FreeBSD, OpenBSD and NetBSD variants. Experimental labels and exact ABI/minimum
requirements remain in the download table. `DEPENDENCIES.md` and
`QUALIFICATION.md` describe runtime checks and platform limits; compilation does
not establish additional runtime qualification.

PowerPC 32-bit Linux is no longer a supported production target. Its opt-in
source-build recipe remains, but this release has no PPC32 binary or update
channel; 0.99.7 also had no PPC32 binary.
