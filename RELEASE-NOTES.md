<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Development snapshot based on 0.99.8b — September 29, 2026

This snapshot fixes OpenAI API-key token preflight and includes the terminal,
voice and context-continuity changes developed after 0.99.8b. Its version keeps
that approved base and adds the source commit's Git suffix. The stable release
and its existing downloads remain unchanged.

## Changes

- Token-count requests select input parameters separately from generation
  controls. This fixes the `Unknown parameter: 'include'` rejection and excludes
  prompt-cache routing from ordinary and compaction counts. Generation retains
  encrypted reasoning and cache affinity, including after manual compaction.
- Native terminal attachment keeps one session owner while the foreground
  terminal detaches, reattaches or switches sessions. Retained controls, process
  continuity and workstation transfers use the existing ownership boundaries.
- History paging, context-cache recovery and goal-aware retries preserve durable
  work across long-session transitions. IRC recovery retains input identities
  and reply obligations within their original input and turn lifetimes.
- Voice shares session controls and read capabilities with typed input, retains
  public dialogue and working-history observations, and keeps interface requests
  separate from coding work. Credential filtering and settled-request retention
  protect responses across credential rotation and voice shutdown.
- Validated native voice expiry and temporary transport/service failures renew
  the physical connection while retaining logical conversation state. Completed
  tool effects and previously sent audio are not replayed. Policy errors remain
  distinct from capacity recovery.
- Production recipes enable device audio and native voice backends on Linux,
  macOS, Windows and BSD. File decoding, provider audio routes and device access
  remain separate capabilities; Office rendering uses its documented runtime.
- Builds after a development tag keep the approved version base and replace the
  previous Git suffix with the current revision. Clean tagged builds retain the
  exact tag; modified checkouts add `-dirty`.

## Scope and compatibility

Permanent unit and strict HTTP-fixture regressions cover the token-count
rejections, compaction request isolation and development-version derivation.
Fixture coverage also exercises context, transport, terminal attachment,
lifecycle and packaging. Device and operating-system runtime qualification is
listed separately in `QUALIFICATION.md`; consult it together with the platform
ABI and installation guidance in the manual.

Native voice capacity recovery remains unfinished and is excluded from this
snapshot. The binary journal codecs are preparatory; live sessions continue
using the existing storage format.

## Downloads and updates

Development assets cover the complete implemented `PROD_TARGETS` matrix and
include matching symbols, manual, source, dependency notices and `SHA256SUMS`.
Application builds use `DEBUG=1`, retaining debug information and frame pointers
without application LTO or stripping. Select the exact OS, architecture and ABI
and verify its checksum before installation. macOS builds retain the documented
per-file quarantine exception for a verified download.

Git-suffixed versions use `latest-dev` and default automatic updates to off.
An update replaces the executable for the next launch; existing processes keep
running until their operators restart them. Stable installations remain on
`latest`. Older releases and their immutable assets remain available under
their original GitHub tags.
