<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent 0.99.6 — September 14, 2026

Multimodal conversations, a steadier provider continuity story and cheaper long sessions.

These notes describe the immutable 0.99.6 tag and its companion downloads.
Later source changes are listed under Unreleased in CHANGELOG.md.

## Changes

- Images, documents and sampled video referenced in a conversation are read natively, and Office
  work is driven through the engine installed on the target rather than a bundled runtime. Modalities
  are compiled per target and the shipped matrix reflects what each platform actually carries.
- The provider request now carries a stable per-conversation identity in the header this proxy keys
  its prompt-cache affinity on. Consecutive requests of one session can therefore reuse the provider's
  cached prefix instead of being pinned per request: the measured baseline this addresses is 270 of 610
  large requests served under 10% from cache, with the cold class costing about five times more per
  output token than the warm one. The measured improvement is being verified against that baseline.
- Usage accounting records cached and uncached input tokens separately, per session (derived from the
  journal, so it survives a resume) and per program, and `/status` reports both with tokens per second
  for the running turn and the most recent response.
- A tool call is named by the provider's own call id on both sides of a request, so a scope change
  between requests can no longer send a call under one id and its result under another.
- `-m` accepts the catalogue index the numbered model listing prints, not only a name; an out-of-range
  index fails instead of being sent upstream.
- A journal written by an earlier build still loads: the rules that govern what the model may do now are
  enforced when an event is committed rather than while replaying history, so a goal the operator locked and
  a model block recorded before that rule existed can no longer leave the session unresumable.
- A goal the operator has locked is frozen in its objective: the model may not reword, block or cancel
  it, refused in the tool before dispatch and again in the store wherever the event names the model,
  while finishing or resuming it stays allowed. A refused store transition also names its own failing
  clause, call and status rather than only the event type and sequence.
- 32-bit x86 links statically without PIE, and the legacy pin's `check` framework builds without its
  own test suite, which unblocked the targets those two walls held.

## Known issues

- **Not a regression:** one interruption path can still refuse a state transition — a managed call whose owner
  is lost while the call is running — so an affected turn ends with `invalid <type> transition at sequence N`
  instead of a completed result. 0.99.6 does not fix it. Every shipped binary now names the failing clause, the
  call and the result status inside that refusal, so the next live occurrence explains itself, and the fix
  follows in the next development build once one occurrence names its clause.

## Upgrading sessions and history

Back up prompt history before manually editing it, and do not run older builds against newer history
files. Existing sessions replay unchanged: the cache identity is derived from the session itself, and
sessions written before it are simply sent without the header until they are used again.

## Downloads and updates

Choose the executable for the exact OS/ABI and architecture and verify `SHA256SUMS`. Each target
includes matching symbols; the release also contains source, build instructions, the manual and
dependency license notices. Use a legacy variant only on its matching OS ABI.

Official stable standalone binaries install matching updates in the background; the old process keeps
running and one local banner gives restart advice. Set `[agent] auto_update = false` to opt out.
Development builds use latest-dev, retain debug information and default to updates off.

## Platform requirements and scope

- Linux: x86-64, AArch64, ARMv6, RISC-V 64-bit, PowerPC 64-bit little-endian, PowerPC 32-bit
  big-endian and i686. `linux-i686-legacy` is **not built for 0.99.6**: its pinned uClibc source set
  cannot build it (the pin's own `check` framework test suite, then fontconfig 2.17.1's `fc-cache` link
  against undefined `Brotli*` symbols with a verified-correct link order). That is a chain inside the
  pin rather than a defect in this tree, the target stays buildable through its own recipe, and no other
  artifact inherits the pin.
- macOS: experimental Intel, Apple Silicon and universal builds, macOS 11+. ARM64 is ad-hoc signed;
  Intel is unsigned. Developer ID signing and notarization are absent, and execution is unqualified.
- Windows: experimental x64 and ARM64 builds using OS DLLs; ARM64 needs UCRT.
- FreeBSD: standard libc.so.7 ABI, including 8.4 and 14.4; the separate legacy libc.so.5/libc_r.so.5
  build supports 5.1 and 5.5.
- OpenBSD: experimental amd64 builds for 7.9, 5.9 and early 3.5, separately.
- NetBSD: experimental amd64 builds for 10.1 and legacy 2.0/5.2.3, separately.

`DEPENDENCIES.md` and `QUALIFICATION.md` describe the actual earlier runtime checks and OS limits.
Compilation does not establish new runtime qualification.
