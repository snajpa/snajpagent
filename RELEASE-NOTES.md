<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent 0.99.6 — September 14, 2026

Multimodal conversations, a steadier provider continuity story and cheaper long sessions.

These notes describe the immutable 0.99.6 tag and its companion downloads.
Later source changes are listed under Unreleased in CHANGELOG.md.

## Changes

- The composer returns once the submitted turn reports processing instead of repainting a
  ready-looking prompt immediately underneath it. Text typed in that window is buffered and
  appears with the composer; empty, slash-command and display-only submissions stay immediate,
  and a turn that never reports processing returns it after a brief bounded delay.
- Images, documents and sampled video referenced in a conversation are read natively, and Office
  work is driven through the engine installed on the target rather than a bundled runtime. Modalities
  are compiled per target and the shipped matrix reflects what each platform actually carries.
- The provider request now carries a stable per-conversation identity in the header this proxy keys
  its prompt-cache affinity on. Consecutive requests of one session can therefore reuse the provider's
  cached prefix instead of being pinned per request: the measured baseline this addresses is 270 of 610
  large requests served under 10% from cache, with the cold class costing about five times more per
  output token than the warm one. The first measurement after the change — one fresh session, 20
  requests — shows 87.5% of input tokens served from cache against 30.0% for the same proxy before it,
  with the reuse growing as the session does; one request in that set was still cold, so this is a
  measured improvement rather than a guarantee.
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

- **Not a regression:** one interruption path can still refuse a state transition, so an affected turn ends with `invalid <type> transition at sequence N`
  instead of a completed result. 0.99.6 does not fix it. Every 0.99.6 binary names the failing clause, the
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

- Linux: x86-64, AArch64, ARMv6, PowerPC 64-bit little-endian and i686. `linux-i686-legacy`,
  `linux-riscv64` and `linux-ppc32` are **not built for 0.99.6**: the legacy target's pinned uClibc
  source set cannot build it (the pin's own `check` framework test suite, then fontconfig 2.17.1's
  `fc-cache` link against undefined `Brotli*` symbols with a verified-correct link order), the RISC-V
  target fails one wall past its fixed FFmpeg syscall constant (libjpeg-turbo's static-SIMD coverage
  tool does not compile), and the 32-bit big-endian PowerPC target cannot link coreutils 9.8's
  `libstdbuf.so` in its pinned musl closure (`__stack_chk_fail_local` is undefined outside glibc).
  Each is a chain inside its own pin rather than a defect in this tree, all three stay buildable
  through their own recipes, and no other artifact inherits them.
- macOS: experimental Intel, Apple Silicon and universal builds, macOS 11+. ARM64 is ad-hoc signed;
  Intel is unsigned. Developer ID signing and notarization are absent, and execution is unqualified.
- Windows: experimental x64 and ARM64 builds using OS DLLs; ARM64 needs UCRT. Execution is unqualified.
- FreeBSD: standard libc.so.7 ABI, including 8.4 and 14.4; the separate legacy libc.so.5/libc_r.so.5
  build supports 5.1 and 5.5.
- OpenBSD: experimental amd64 builds for 7.9, 5.9 and early 3.5, separately.
- NetBSD: experimental amd64 builds for 10.1 and legacy 2.0/5.2.3, separately.

`DEPENDENCIES.md` and `QUALIFICATION.md` describe the actual earlier runtime checks and OS limits.
Compilation does not establish new runtime qualification.
