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
| `linux-i686` | `snajpagent` | 32-bit self-contained static PIE; not Linux 2.4 qualification |
| `linux-i686-legacy` | `snajpagent` | Static non-PIE LinuxThreads; Linux 2.4.27 exercised; added after 0.99.1 |
| `macos-x86_64` | `snajpagent` | Intel, static application libraries; experimental |
| `macos-arm64` | `snajpagent` | Apple Silicon, static application libraries; experimental |
| `macos-universal` | `snajpagent` | Both macOS slices in one native executable; experimental |
| `windows-x86_64` | `snajpagent.exe` | Static application libraries, Windows system DLLs only; experimental |
| `windows-arm64` | `snajpagent.exe` | Native ARM64, static application libraries and OS UCRT; experimental |
| `freebsd-amd64` | `snajpagent` | Static application libraries, native libc/threads; FreeBSD 8.4 and 14.4 exercised |
| `freebsd-amd64-legacy` | `snajpagent` | Separate libc.so.5/libc_r.so.5 ABI; FreeBSD 5.1 and 5.5 exercised |

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

Binary publication starts with **0.99.1**; older tags need not be backfilled.
Pages is manual-only: branch pushes, tags and releases do not deploy it.
Complete the matrix and download entries, then explicitly run Pages from the
authorized release revision. Check public pages and assets before announcing.
Local previews never require deployment. Source-only changes may ship without
publishing the site.

The binary release tag stays fixed after publication. Download-page-only
updates may follow on master once exact asset sizes are known; those updates
do not change the binaries' tagged source identity.

`www/downloads.html` is the public download entry point. Keep it static and
usable without JavaScript. Link it from the home page, README, and manual.
The main table serves executable selection, not release maintenance. Keep debug
builds in a separate section; symbol archives for production crash diagnosis
remain available on the Release, not as a column beside every normal download.
Runnable debug builds must contain an executable, identify the actual build
profile/source and retain their platform qualifications. Do not relabel a
symbol-only file as a debug build. Additions to a published release use distinct
names and checksums; do not replace existing executable bytes or tags.


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
   the homepage, README and manual.
4. With publication authority, publish the complete release and deploy the
   updated page through the existing Pages workflow. Check the public page
   and fetch its actual linked assets to verify availability and checksums
   before announcing the version as shipped. Uploaded drafts or local Nix
   store paths are not public downloads.

Do not expose builder-local paths or label missing assets as downloadable.
Before publication, keep the live page on the previous release or its honest
pending-release notice; draft assets are not public downloads. This policy does not itself authorize creating a release, pushing
a tag, accessing remote build hosts, or adding release automation.

## Official updater builds and development channel

Ordinary `make` and `prod-matrix` do not enable updating. Package recipes must
leave the publisher parameters unset; enabled binaries are standalone installs. Official publishers set
`UPDATE_BASE_URL=https://agent.snajpa.net`; native custom builds also provide
`UPDATE_TARGET` explicitly. The matrix supplies its own exact target IDs. A
custom publisher supplies its own base URL and keeps that identity across updates.

Stable tag builds default `[agent] auto_update=true`. Commit-suffixed development
versions default false and use `latest-dev`; stable uses `latest`. All published
development builds are **DEBUG=1**, with debug information and frame pointers,
without application LTO or stripping. Optimized dependencies remain unchanged.
Never publish an ordinary stripped build under a development version. Both
channels ship the full current `PROD_TARGETS`, with dev downloads in a secondary
expandable section. Keep stable and dev immutable versioned assets available.

For each channel, publish one small `.json` beside its latest executable URL.
It contains `name`, `target`, `version`, immutable HTTPS `url`, `sha256`, `size`
and HTTPS `changelog`. The latter can include `#changelog`. Hash and size are
computed from the final executable bytes, after signing/strip operations.
Publish all versioned assets before switching the channel; never point a channel
at a draft or partial matrix. HTTPS is the publisher trust root; hashes detect
corruption and mixed-version publication, not compromise of that publisher.

The existing manual Pages workflow fetches the channel executables from their
immutable release URLs, verifies them, and includes them in the atomic site
artifact. Git stores only the tiny channel descriptions, not executables.
No automatic deploy-on-push or updater service is required.

Maintain compact highlights in `www/downloads.html#changelog`: version/date,
one headline and a few user-facing bullets, 80-column lines, newest first.
Keep download selection first and detailed development evidence elsewhere.
The banner links to this page; it does not parse or inject release prose.

Stage a completed matrix with `python3 tools/release.py stage --version VERSION
--revision REVISION --output STAGE --release https://github.com/snajpa/snajpagent/releases/download/VERSION`.
This copies standalone executables and symbols and writes the channel descriptors.
`REVISION` must be the exact clean source commit used to build the entire matrix;
its source, manual and notices are archived even if publishing edits follow.
Development version suffixes must identify that commit. Each staged release
includes the target matrix at its source revision; older channels retain their
original targets when a newer release adds a platform.
Add the corresponding dependency sources, notices, and checksums to the stage;
publish those immutable files, then copy the descriptors to `www/latest/` or
`www/latest-dev/`. Update the downloads page and run the manual Pages workflow.
`python3 tools/release.py pages` performs the same channel materialization locally.
