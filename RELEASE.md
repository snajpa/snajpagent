<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Release policy

Every new version ships downloadable production binaries for **all implemented
targets**, not just the host platform. This includes patch releases and
prereleases. A tag, source push, successful local build, or source-only release
does not fulfill this policy. The website download page and published assets
are part of the release, not follow-up work.

## Required matrix

`PROD_TARGETS` in `Makefile` defines the implemented production matrix.
`make -jN prod-matrix` builds it into isolated `build/matrix/<target>/` outputs;
plain `make` remains host-only and does not build or publish a release.

The current required outputs are:

| Target | Executable in `build/matrix/<target>/bin/` | Distribution scope |
| --- | --- | --- |
| `linux-x86_64` | `snajpagent` | Self-contained static PIE |
| `linux-aarch64` | `snajpagent` | Self-contained static PIE |
| `macos-x86_64` | `snajpagent` | Intel, static application libraries; experimental |
| `macos-arm64` | `snajpagent` | Apple Silicon, static application libraries; experimental |
| `macos-universal` | `snajpagent` | Both macOS slices in one native executable; experimental |
| `windows-x86_64` | `snajpagent.exe` | Static application libraries, Windows system DLLs only; experimental |
| `windows-arm64` | `snajpagent.exe` | Native ARM64, static application libraries and OS UCRT; experimental |

Both macOS standalone slices and the universal executable ship. Experimental
does not mean optional: publish implemented builds with their actual testing
status, minimum build target, tested OS versions, and known limitations.
Compilation alone is not runtime qualification. See [DEPENDENCIES.md](DEPENDENCIES.md)
and [QUALIFICATION.md](QUALIFICATION.md); do not describe static application
libraries on macOS or Windows as a fully static operating-system interface.

The long-term requirement is the full portability matrix, including additional
architectures and legacy systems as implementation lands. A newly implemented
production target must join `PROD_TARGETS`, this table, and the download page in
the same change, and ship with every subsequent version. An unfinished port or
isolated development build is not yet a release target. Do not wait for all
planned ports before releasing the implemented matrix, or silently drop a
target to work around a build failure. Removing an implemented release target
requires an explicit scope decision, recorded in the release notes.

## Assets and identity

- Build every target from the same clean release-tag commit and version using
  the pinned recipes. Do not mix older cached binaries, dirty worktree builds,
  or the dynamically linked host binary into a release. Check build identity
  for every artifact; use runtime `-V` where runnable and embedded metadata
  otherwise. Optional matching symbols belong to that exact executable.
- Publish on the corresponding GitHub Release. Use standalone executable names
  `snajpagent-<version>-<target>` and append `.exe` for Windows. `<version>` is
  the exact release tag. The OS/architecture must be unambiguous before download.
  Retain matching debug symbols and publish them separately from the runnable
  file; never require a symbol sidecar to start the application.
- Publish `SHA256SUMS` for the downloadable files, release notes, and the exact
  source/build instructions and dependency license/source material described in
  [DEPENDENCIES.md](DEPENDENCIES.md). Include the manual and licensing notices
  as companion downloads; keep the executable usable by itself.
- Keep versioned assets available. Do not overwrite a published version with
  different executable bytes; corrections get a new version. Label prereleases
  and experimental platforms explicitly rather than implying qualification.

## Website and publication checklist

`www/downloads.html` is the public download entry point. Keep it static and
usable without JavaScript. Link it from the home page, README, and manual.

For each new version:

1. Prepare the release commit/tag and notes, build the entire implemented
   matrix, and retain the relevant existing test results. Follow the repository
   shipment/testing policy: known build or product failures must be fixed;
   build success must not be presented as unperformed runtime testing.
2. Upload all required binaries and companions to a draft release for that
   exact tag. Check names, sizes, identities, checksums, and target completeness
   against `PROD_TARGETS`. Do not publish a partial matrix as a finished release.
3. Update the download page with the version/date, a direct version-specific
   asset link and byte size for every target, checksums, source/manual/license
   links, release notes, and honest compatibility/testing notes. Preserve a
   link to older releases and distinguish stable releases from prereleases.
   For the first binary release, also remove the pending-release wording from
   the README and manual.
4. With publication authority, publish the complete release and deploy the
   updated page through the existing Pages workflow. Check the public page
   and fetch its actual linked assets to verify availability and checksums
   before announcing the version as shipped. Uploaded drafts or local Nix
   store paths are not public downloads.

Until the first binary release is published, the page must explicitly say that
downloads are not yet available and point to source-build instructions. Do not
invent version links, expose builder-local paths, or label missing assets as
downloadable. This policy does not itself authorize creating a release, pushing
a tag, accessing remote build hosts, or adding release automation.
