<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent 0.99.5 — September 10, 2026

Local-first prompt history, provider reasoning continuity and terminal controls.

These notes describe the immutable 0.99.5 tag and its companion downloads.
Later source changes are listed under Unreleased in CHANGELOG.md.

## Changes

- Up/Down, Ctrl-P/Ctrl-N and Ctrl-R visit session history first, then the global
  archive. Submissions remain private to a running process until orderly exit.
  Only newly entered lines are appended globally; repeated resume/exit does not
  republish saved entries. Files are neither copied wholesale nor pruned, and
  large-file searches remain cancellable.
- Provider reasoning is retained privately and replayed with matching tool calls
  and results, including after resume and during compaction. Replay is scoped
  to provider, endpoint, model and credential identity. DeepSeek thinking streams
  accept non-public reasoning parts without displaying or executing them.
- Model/provider/effort selections persist across turns and resume. Goal
  interruption settles state before showing the idle prompt; empty Enter stays
  local. Command help, queued-prompt rendering, history counts, active controls
  and manual compaction have been corrected.
- Installation and first-run instructions cover all six platform families.
  Twenty standalone production executables retain the existing target matrix.

## Upgrading sessions and history

Back up prompt history before manually editing it. Do not run older builds
against the new unbounded history files: older versions can prune the archive.
Saved local entries from an earlier process are not republished on resume,
including entries whose original process crashed before its global merge.
An explicit `/exit` submission adds its own history entry; empty Ctrl-D does not.

Reasoning continuation is stored in the private session journal, not the public
transcript. Sessions written with the extended completion records must not be
downgraded to executables predating reasoning-continuation support.

## Downloads and updates

Choose the executable for the exact OS/ABI and architecture and verify
`SHA256SUMS`. Each target includes matching symbols; the release also contains
source, build instructions, the manual and dependency license notices.
Use the separate legacy variant only on its matching OS ABI.

Official stable standalone binaries install matching updates in the background.
The old process keeps running and one local banner gives restart advice and the
release-log URL. Set `[agent] auto_update = false` to opt out. Development builds
use latest-dev, retain debug information and default to updates off; this stable
release leaves the existing development channel unchanged.

## Platform requirements and scope

- Linux: x86-64, AArch64, ARMv6, RISC-V 64-bit, PowerPC 64-bit little-endian,
  PowerPC 32-bit big-endian and i686; the separate legacy i686 build supports
  Linux 2.4.27, with procfs, secure OS entropy and devpts for PTY commands.
- macOS: experimental Intel, Apple Silicon and universal builds, macOS 11+.
  ARM64 is ad-hoc signed; Intel is unsigned. Developer ID signing and
  notarization are absent. Actual macOS execution remains unqualified.
- Windows: experimental x64 and ARM64 builds using OS DLLs; ARM64 needs UCRT.
- FreeBSD: standard libc.so.7 ABI, including 8.4 and 14.4; the separate legacy
  libc.so.5/libc_r.so.5 build supports 5.1 and 5.5.
- OpenBSD: experimental amd64 builds for 7.9, 5.9 and early 3.5, separately.
- NetBSD: experimental amd64 builds for 10.1 and legacy 2.0/5.2.3, separately.

`DEPENDENCIES.md` and `QUALIFICATION.md` describe the actual earlier runtime
checks and OS limits. Compilation does not establish new runtime qualification.
The Windows updater's two-rename fallback has interruption/recovery semantics
specified in the manual; actual Windows/macOS updater execution remains
unqualified. Exact 0.99.5 build and validation scope accompanies the assets.
