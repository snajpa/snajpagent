<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent 0.99.4 — September 9, 2026

Goal controls, durable session recovery and additional Linux builds.

## Changes

- `/goal clear` cancels the current goal and stops automatic continuation.
  Pause, resume and clear accept whitespace around their command words.
  An in-progress turn finishes normally; Ctrl-C interrupts it immediately.
- Session recovery preserves admitted input and active-turn controls. Provider
  policy clarification retains running commands and partial response evidence.
- Twenty standalone executables cover Linux, macOS, Windows, FreeBSD, OpenBSD
  and NetBSD. Linux adds ARMv6, RISC-V 64-bit, PowerPC 64-bit little-endian and
  PowerPC 32-bit big-endian builds.

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
unqualified. Exact 0.99.4 build and validation scope accompanies the assets.
