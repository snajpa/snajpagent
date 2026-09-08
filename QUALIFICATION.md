<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Qualification ledger

This file records test coverage and outstanding platform and provider checks.
Static inspection, local loopback tests, target-platform execution and live-provider
runs each cover different behavior. Report the actual scope of each result.

## Source-archive evidence

The repository provides these existing checks. `make check` runs unit, CLI,
terminal and source checks; bundle collection and matrix aggregation are
separate opt-in tools:

| Gate | Evidence produced inside this tarball |
|---|---|
| `make depscheck` | rejects undeclared vendored dependency source/header drift and Jansson/libcurl include drift |
| `make portabilitycheck` | verifies that PTY support is capability-gated for Linux and macOS |
| `make depclosurecheck` | captures and validates the current-host dynamic executable dependency closure, including system libcurl and Jansson linkage plus detected libcurl backend evidence |
| `make evidencebundle` / `make evidencecheck` | collects and validates a JSON evidence bundle for one concrete host, including source audits, dependency closure, and PTY terminal evidence when a fixture binary is supplied |
| `make evidencetoolcheck` | exercises the single-bundle and matrix-evidence checkers against generated valid and invalid bundles, including path-escape, missing-record, duplicate-platform, version-mismatch, and extra-platform cases |
| `make evidencematrixcheck` | validates a supplied final set of external per-platform bundles for required platform coverage, unique platform ids, consistent versioning, terminal evidence, and live-provider evidence |
| `make sizecheck` | enforces preferred/hard line budgets and the 2,000-line per-file review trigger |
| `tests/test_provider_transport` | exercises the real libcurl create/count/compact transport against a local loopback HTTP server |
| `tests/pty_*.py` | exercises the interactive terminal composer, live resize, suspend/continue, and TERM/width fallback behavior through a PTY on the current POSIX host |
| `make tmuxcheck` | asserts the rendered screen/scrollback for deterministic streaming, Markdown enabled/disabled overrides, status, wrapping, steering, resize, queue, durable-text, and instruction-discovery scenarios, then runs one production IRC server plus two production clients against loopback fake Responses endpoints and checks bidirectional Markdown-rendered chat, three-agent model traffic, durable attribution, verbosity, color, peer leaves, and exact cleanup; `make check` runs it whenever tmux is installed |
| `make terminallivecheck` | runs the fixed vpsAdminOS 6.12.95 real-work prompt through the configured default provider in a 52×18 tmux, serializes checks using the same config file, and compares rendered public text with durable response events and `AGENTS.md` metadata |

The PTY implementation is now compiled through a single `SNAJPAGENT_HAVE_PTY`
capability surface. Linux uses `<pty.h>` and macOS uses `<util.h>`; runtime PTY
behavior then uses the same `openpty`, `ioctl(TIOCGWINSZ/TIOCSWINSZ)`,
controlling-terminal, immediate-run, yielded-run, and `write_stdin` paths.
The tmux layer complements those raw-PTY checks by interpreting cursor movement,
erase, wrap, and resize sequences as a real terminal does.

## Release qualification

[RELEASE.md](RELEASE.md) defines publication requirements: every implemented
`PROD_TARGETS` executable ships from one clean tag with matching companions.
Each target needs its own runtime coverage. Experimental builds ship with
explicit limitations and outstanding checks.

For 0.99.1, Linux x86-64, AArch64 and i686 use static musl/application libraries.
Earlier Linux checks include native execution and local QEMU. The 0.99.1 i686
build remains unqualified on Linux 2.4. macOS Intel, ARM64 and universal are
cross-builds targeting macOS 11; actual macOS execution is still untested. ARM64 is ad-hoc signed, Intel unsigned; neither is Developer ID signed
or notarized. Earlier Windows checks ran in PE build 26100 (x64) and 28000
(ARM64). Full desktop and older Windows coverage is pending. ARM64 requires the
OS UCRT. See [DEPENDENCIES.md](DEPENDENCIES.md) for the precise earlier scope.

After 0.99.1, `linux-i686-legacy` adds a separate static non-PIE build with
uClibc-ng/LinuxThreads, compiler TLS emulation, embedded locale data and modern
TLS/CA roots. Its development binary ran on Debian Sarge's Linux 2.4.27-3-386
under QEMU Pentium III: base/IRC units, internal RO and denied-write enforcement,
parallel commands, PTY, interactive resume/exit, TLS distrust/explicit trust/
wrong-hostname rejection and a hostname-based local provider connection.
Coverage is limited to those local fixtures and that kernel. Paid-provider
testing and other 2.4 kernel versions remain outstanding.
Working procfs and replenished secure OS entropy are required; legacy descriptor
flags are non-atomic. This target currently requires a source build.

The existing `linux-x86_64` target also runs on CentOS 3.9's backported
Linux **2.4.21-50.EL** kernel after enabling the shared clock fallback.
Its 3,790,320-byte development executable passed base/IRC, production RO,
parallel commands, PTY, resume, TLS trust/name and hostname-connection checks
in QEMU. That scope applies to the tested CentOS kernel; upstream 2.4 AMD64
coverage remains unverified. The target list is unchanged.

The experimental `netbsd-amd64-legacy` target uses NetBSD 5.2.3's native
libc.so.12/libpthread.so.0 ABI, static application libraries and Unicode,
compiler-rt thread-local emulation, TLS and embedded CA roots. Its 4,172,296-byte
candidate `a3383b75` passed base/configuration/SSE/IRC, read-only tools and denied
writes, parallel commands, PTY exit status, CLI/interactive resume and TLS
trust/hostname checks on an installed NetBSD 5.2.3 guest. Its native two-segment
ELF has a non-executable stack and stack protection, without PIE or RELRO.
NetBSD 10.1's libpthread.so.1 cannot load this artifact. Earlier NetBSD releases
require separate qualification.

The `netbsd-amd64` target uses NetBSD 10.1's native libc.so.12/libpthread.so.1
ABI with PIE, full RELRO, stack protection and a non-executable stack. Candidate
`e5b7a8d8` is 4,268,152 bytes with matching debug symbols. Its static Unicode
engine handles case folding in the C locale. Base/configuration/SSE/IRC,
read-only enforcement, parallel commands, PTY exit status, CLI/interactive
resume and TLS trust/hostname checks passed on the installed 10.1 guest.

Each release's notes distinguish checks of its exact binaries from earlier
implementation evidence. Record local fake-provider checks and paid-provider
runs separately. Do not describe an unperformed platform or live-model
run as passing, and do not turn the historical four-platform bundle defaults
into the production matrix: `PROD_TARGETS` is its source of truth.

External evidence still required for stronger platform claims includes actual
macOS execution and broader Windows desktop/legacy coverage. These disclosed
gaps do not prevent shipping explicitly experimental builds under RELEASE.md.

## Evidence bundle layout

`make evidencebundle` writes `$(EVIDENCE_DIR)` (default
`build/release-evidence/current-host`) and intentionally skips the external
provider unless a release operator uses `make releaseevidence`. The bundle
contains `release_evidence.json`, `source_audit.json`,
`dependency_closure.json`, and, when built with the fixture,
`terminal_evidence.json`. Record references inside `release_evidence.json` must be
canonical relative paths confined to that evidence directory; absolute paths,
`..`, `./`, duplicate separators, backslashes, and symlink escapes are rejected
by the checker. `make releaseevidence` additionally requires
`OPENAI_API_KEY` and writes `live_provider_evidence.json`;
`tools/check_release_evidence.py --require-terminal --require-live` is the
checker for a complete single-platform evidence record. The optional historical bundle
aggregation command is `make evidencematrixcheck`, with `RELEASE_PLATFORMS` defaulting
to `linux-x86_64 linux-aarch64 macos-x86_64 macos-arm64` and
`RELEASE_EVIDENCE_DIRS` pointing at the copied per-platform bundle directories.
