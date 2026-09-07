<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Qualification ledger

This file records what the source archive itself can prove and what still needs
an external release environment. It is intentionally conservative: local static
or loopback tests are implementation evidence, not a substitute for running the
binary on each advertised platform and against a live provider account.

## Source-archive evidence

The repository provides these existing checks. `make check` runs unit, CLI,
terminal and source checks; bundle collection and matrix aggregation are
separate opt-in tools, not steps implicitly run by `make check`:

| Gate | Evidence produced inside this tarball |
|---|---|
| `make depscheck` | rejects undeclared vendored dependency source/header drift and Jansson/libcurl include drift |
| `make portabilitycheck` | verifies PTY support is capability-gated for both advertised OS families, Linux and macOS, instead of being accidentally Linux-only |
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
Building the entire matrix is not the same as qualifying every platform.
Experimental builds ship with explicit limitations rather than being omitted.

For 0.99.1, Linux x86-64, AArch64 and i686 use static musl/application libraries.
Earlier Linux checks include native execution and local QEMU; the i686 CPU
baseline is not a claim of Linux 2.4 compatibility. macOS Intel, ARM64 and
universal are cross-builds targeting macOS 11; no actual macOS execution is
claimed. ARM64 is ad-hoc signed, Intel unsigned; neither is Developer ID signed
or notarized. Earlier Windows checks ran in PE build 26100 (x64) and 28000
(ARM64), not a full desktop or older Windows qualification. ARM64 requires the
OS UCRT. See [DEPENDENCIES.md](DEPENDENCIES.md) for the precise earlier scope.

Each release's notes distinguish checks of its exact binaries from earlier
implementation evidence. Local fake-provider transport/terminal checks are not
paid live-provider tests. Do not describe an unperformed platform or live-model
run as passing, and do not turn the historical four-platform bundle defaults
into the production matrix: `PROD_TARGETS` is its source of truth.

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
