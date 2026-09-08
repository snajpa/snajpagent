<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent 0.99.2 — September 8, 2026

Background binary updates and clearer streamed Markdown.

## Changes

- Official standalone binaries discover, verify and install matching releases
  in a background thread. The current process keeps running its old version.
  One local banner gives restart advice and links to the release log.
- Stable builds default to automatic updates. Development builds carry a Git
  suffix, retain debugging information and use a separate channel that defaults
  off. Configure `auto_update` and `update_url` in `[agent]`.
- Plain-text code fences keep their border without a redundant type label.
  Indented and pipe-prefixed code stays literal during streaming.
- Eleven standalone targets now include Linux i686 legacy, FreeBSD amd64 and
  FreeBSD amd64 legacy. Completed tool arguments are accepted after empty
  streaming placeholders, including snapshot-only provider responses.

## Installation and updating

Download the executable for your OS and architecture and verify `SHA256SUMS`.
On Unix, make it executable and place it in a user-owned standalone directory.
Use `auto_update = false` for package-managed installations. Updates preserve
executable permissions and require ownership of the file and its directory.
The updater's trust root is the configured HTTPS publisher; expected size,
SHA-256 and product/target/publisher identity are checked before replacement.

Unix replaces the executable by a same-directory rename. Windows retains the
mapped original at `.EXECUTABLE.update-old.exe` if rename-over is unavailable;
this two-rename fallback has an interruption window. The manual describes
recovery. Windows replacement has Linux forced-path regression coverage but
has not yet been qualified on an actual Windows runtime for this release.

## Platforms

- Modern Linux: x86-64, AArch64 and i686, with static application libraries.
- Legacy Linux: i686 non-PIE build; Linux 2.4.27 runtime exercised. Requires
  procfs and secure OS entropy; PTY commands also require devpts.
- macOS: experimental Intel, Apple Silicon and universal builds, macOS 11+.
  ARM64 is ad-hoc signed; Intel is unsigned. Developer ID signing and
  notarization are absent. Actual macOS execution remains unqualified.
- Windows: experimental x64 and ARM64 builds using OS DLLs. ARM64 needs UCRT.
- FreeBSD: amd64 libc.so.7 build, exercised on 8.4 and 14.4; separate legacy
  libc.so.5/libpthread.so.1 build exercised on 5.5.

See `DEPENDENCIES.md` and `QUALIFICATION.md` for platform limits. Earlier
platform runs qualify those paths, not every behavior added in this release.
All targets include matching symbol archives, exact source/build instructions,
manual and dependency license/source companions. Development applications are
always debug builds and retain symbols without application stripping.
