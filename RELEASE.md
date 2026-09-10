<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Release policy

Every new version ships downloadable production binaries for **all implemented
targets**, not just the host platform. This includes patch releases and
prereleases. A tag, source push, successful local build, or source-only release
does not fulfill this policy. The website download page and published assets
are part of the release, not follow-up work.

## Version authority

The operator decides when the version changes and selects or explicitly approves
its value. Permission to implement or ship a feature, deploy the website or
finish a goal does not authorize a version bump or a new stable release.
The matrix requirements below describe an authorized release's contents; they
do not require creating a new version for each change.

Before changing release versions in metadata, notes or build commands, creating
release tags, or publishing a newly versioned release, confirm the operator's
actual version decision. If absent, retain the approved version, continue safe
in-scope implementation and ask before the version-changing step. Test fixtures,
agent-written plans, successful builds and push-approval tokens are not version
authority. An explicitly authorized development-snapshot workflow uses the
existing approved base plus its actual Git suffix; it does not advance the base
or promote the snapshot to stable. Previously published assets remain immutable.

## Documentation shipment

Every feature and behavior change updates its manual, affected guides/design/
status/examples and Unreleased changelog before source shipment. Render the
manual, verify changed examples and links, and review the whole affected workflow
under AGENTS.md and EDITORIAL.md. Source-only or documentation shipment may use
the approved base plus Git suffix; it is not a newly versioned stable release.

When web publication is authorized, dispatch the existing manual Pages workflow
from the exact shipped commit and verify the live manual and pages. It generates
`manual.html` from `snajpagent.1`; keep only that one manual source. A documentation
refresh preserves existing downloadable assets, hashes, channel descriptors and
release history. The online manual tracks current source, while each release's
companion manual describes its exact immutable executable.

## Canonical tag-driven release

Create the operator-approved annotated Git tag on the clean release commit,
then build normally. Both native and production-matrix builds derive their
version from that tag. After the operator supplies a new approved version,
set `APPROVED_VERSION` to that exact value; do not reuse an already published tag:

```sh
: "${APPROVED_VERSION:?set the operator-approved new release version}"
git tag -a "$APPROVED_VERSION" -m "snajpagent $APPROVED_VERSION"
make -j4 UPDATE_BASE_URL=https://agent.snajpa.net prod-matrix
python3 tools/release.py stage --revision "$APPROVED_VERSION" --output /path/to/new-stage
```

Staging derives the version and default immutable GitHub download URL from the
exact tag and archives that tagged source, even if later website edits exist.
`BUILD_VERSION` and staging's `--version`/`--release` remain available for manual
builds and custom publishers; the ordinary release procedure does not need them.
Changing prose or supplying an override does not create a release tag.

## Required matrix

`PROD_TARGETS` in `Makefile` defines the implemented production matrix.
`make -jN prod-matrix` builds it into isolated `build/matrix/<target>/` outputs;
plain `make` remains host-only and does not build or publish a release.

The current required outputs are:

| Target | Executable in `build/matrix/<target>/bin/` | Distribution scope |
| --- | --- | --- |
| `linux-x86_64` | `snajpagent` | Self-contained static PIE |
| `linux-aarch64` | `snajpagent` | Self-contained static PIE |
| `linux-armv6` | `snajpagent` | Shared ARMv6/ARMv7 hard-float static PIE; ARMv6KZ/VFPv2 baseline; experimental |
| `linux-riscv64` | `snajpagent` | RV64GC/LP64D static PIE with embedded application libraries and trust roots; experimental |
| `linux-ppc64le` | `snajpagent` | POWER8 little-endian ELFv2 static PIE with embedded application libraries and trust roots; experimental |
| `linux-ppc32` | `snajpagent` | 32-bit big-endian PowerPC hard-float static PIE; needs secure OS entropy; experimental |
| `linux-i686` | `snajpagent` | 32-bit self-contained static PIE; not Linux 2.4 qualification |
| `linux-i686-legacy` | `snajpagent` | Static non-PIE LinuxThreads; Linux 2.4.27 exercised; added after 0.99.1 |
| `macos-x86_64` | `snajpagent` | Intel, static application libraries; experimental |
| `macos-arm64` | `snajpagent` | Apple Silicon, static application libraries; experimental |
| `macos-universal` | `snajpagent` | Both macOS slices in one native executable; experimental |
| `windows-x86_64` | `snajpagent.exe` | Static application libraries, Windows system DLLs only; experimental |
| `windows-arm64` | `snajpagent.exe` | Native ARM64, static application libraries and OS UCRT; experimental |
| `freebsd-amd64` | `snajpagent` | Static application libraries, native libc/threads; FreeBSD 8.4 and 14.4 exercised |
| `freebsd-amd64-legacy` | `snajpagent` | Separate libc.so.5/libc_r.so.5 ABI; FreeBSD 5.1 and 5.5 exercised |
| `netbsd-amd64` | `snajpagent` | NetBSD 10.1 libc.so.12/libpthread.so.1 ABI, PIE/full RELRO, static application libraries; experimental |
| `netbsd-amd64-legacy` | `snajpagent` | NetBSD 2.0/5.2.3 libc.so.12/libpthread.so.0 ABI, non-PIE/NX, static application libraries; experimental |
| `openbsd-amd64` | `snajpagent` | OpenBSD 7.9, static application libraries, native libc/thread ABI; experimental |
| `openbsd-amd64-legacy` | `snajpagent` | OpenBSD 5.9 libc/thread ABI, static application libraries; experimental |
| `openbsd-amd64-early` | `snajpagent` | OpenBSD 3.5 libc/thread ABI, non-PIE, static application libraries; experimental |

Current macOS builds are not Developer ID signed or notarized. Keep the
per-file quarantine exception in the macOS installation instructions and README;
apply it only to the selected file after comparing its SHA-256.

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
  source/build instructions and dependency license notices described in
  [DEPENDENCIES.md](DEPENDENCIES.md). Include the manual and licensing notices
  as companion downloads; keep the executable usable by itself.
- Keep versioned assets available. Do not overwrite a published version with
  different executable bytes; corrections get a new version. Label prereleases
  and experimental platforms explicitly rather than implying qualification.
- Preserve release-tag ancestry when integrating later work. Once a commit is
  tagged for publication, integrate that history without rebasing it away from
  master. If equivalent patches were already rebased, reconcile the original
  tagged history explicitly while retaining current source. Never move an
  existing release tag to newer source or relabel different bytes as that release.

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
Keep source-build commands and additional source-only platform guidance in the
README and build documentation, off the downloads page. Downloadable source
archives and their license companions remain appropriate release downloads.
List only the latest stable executable for every implemented architecture/ABI
variant. Link older releases and development/debug downloads to GitHub instead
of repeating their tables. Published OS families use closed details/summary
panels; omit roadmap tiers, planned platforms and repeated setup prose.
Each row gives the architecture, clearly labelled minimum OS or required ABI,
size and full SHA-256 (a closed checksum disclosure is acceptable). Link each
family to the manual's installation instructions. Preserve the macOS quarantine
command for verified downloads. Source, symbols and licensing companions remain
available from the release page.

A tested OS version is not a minimum. Linux kernel baselines must distinguish
libc/architecture requirements from actual qualification; exact oldest-working
kernels must not be inferred from a recent test host. BSD rows identify the
required library ABI; a release-specific ABI is not an open-ended OS minimum.
Keep detailed qualification evidence in QUALIFICATION.md and dependency records.
Never replace previously published executable bytes or move their tags.


For each operator-approved new version:

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

Stage a completed tagged matrix with `python3 tools/release.py stage
--revision TAG --output STAGE`. This copies standalone executables and symbols,
writes the channel descriptors, and archives source/manual/notices from that
exact tag. Staging defaults to the exact tag at HEAD when `--revision` is absent.
For an explicitly versioned custom build, use `--version VERSION --revision
REVISION --release HTTPS_PREFIX`; development suffixes must identify that commit.
Each staged release includes the matrix at its source revision; older channels
retain their original targets when a newer release adds a platform.
Add dependency notices and checksums to the stage;
publish those immutable files, then copy the descriptors to `www/latest/` or
`www/latest-dev/`. Update the downloads page and run the manual Pages workflow.
`python3 tools/release.py pages` performs the same channel materialization locally.
