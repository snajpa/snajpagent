<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent 0.99.1 — September 7, 2026

The first binary release. Eight standalone executables cover Linux x86-64,
AArch64 and i686; macOS Intel, Apple Silicon and universal; Windows x64 and
ARM64. macOS and Windows are experimental. Older tags remain source releases.

## Changes

- Queue acknowledgements say `queued (/next or /q c) ›`: `/next` resumes a
  paused queue, `/q c` clears waiting prompts without stopping current work.
- Standalone work, steering, goals and native IRC remain one program. IRC
  reconnects catch up by durable event ID rather than repeating conversation.
- Prompts share activity/goal indicators and time; context shows measured request
  input, not a local estimate. Wrapped draft navigation and paragraph spacing
  are improved. Active goals survive exit/resume; unused fresh sessions are not
  saved. Validated shell aliases retain their intended behavior.
- The README/homepage explain concrete workflows. The complete man page now
  supplies a readable web reference with grouped, keyboard-accessible contents.

See [CHANGELOG.md](CHANGELOG.md) for detailed development history, including
changes already present in the earlier 0.99.0 source tag.

## Choosing a binary

| Target | Requirements and limits |
| --- | --- |
| Linux x86-64 | Static musl/application libraries; no Nix, glibc, libcurl or Jansson installation needed. |
| Linux AArch64 | The same self-contained build for 64-bit ARM. |
| Linux i686 | 32-bit x86 baseline, not a promise of Linux 2.4 or arbitrary old-kernel support. |
| macOS Intel / ARM64 / universal | Experimental cross-builds, deployment target macOS 11, only Apple's libSystem dynamically linked. No actual macOS execution claimed. ARM64 is ad-hoc signed; Intel is unsigned, including its universal slice. Neither is Developer ID signed or notarized. |
| Windows x64 / ARM64 | Experimental, static application libraries, system DLLs only; ARM64 uses the OS UCRT. Earlier tests used Windows PE 26100 (x64) and 28000 (ARM64), not full-desktop or old-Windows qualification. |

Earlier platform tests are described in [DEPENDENCIES.md](DEPENDENCIES.md) and
[QUALIFICATION.md](QUALIFICATION.md). Build success is not runtime qualification.
The published release includes `BUILDING-0.99.1.md` with exact binary identity,
checks actually performed for this release, dependencies and reproduction steps.
No paid live-provider test or unperformed Windows/macOS run is implied.

## Downloads and use

Download the executable for your OS/architecture and verify `SHA256SUMS`.
On Unix rename it to `snajpagent` and run `chmod +x snajpagent`; on Windows keep
`.exe`. Matching symbols are separate optional archives, not needed to run.
The manual, source, dependency sources and licensing/build material accompany
the release. TLS roots and application libraries are included in each binary.

Run in your project directory. Fresh interactive setup can guide provider and
model selection; existing credentials/configuration are not overwritten.
Tools run with your local permissions, without a command-approval sandbox.
IRC has no authentication or TLS: use localhost, a trusted network or a tunnel.
