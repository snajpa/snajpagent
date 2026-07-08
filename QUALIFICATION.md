<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Qualification ledger

This file records what the source archive itself can prove and what still needs
an external release environment. It is intentionally conservative: local static
or loopback tests are implementation evidence, not a substitute for running the
binary on each advertised platform and against a live provider account.

## Source-archive evidence

The default `make check` path now includes the following machine-checkable gates:

| Gate | Evidence produced inside this tarball |
|---|---|
| `make depscheck` | rejects undeclared vendored dependency source/header drift and Jansson/libcurl include drift |
| `make portabilitycheck` | verifies PTY support is capability-gated for both advertised OS families, Linux and macOS, instead of being accidentally Linux-only |
| `make depclosurecheck` | captures and validates the current-host dynamic executable dependency closure, including system libcurl and Jansson linkage plus detected libcurl backend evidence |
| `make evidencebundle` / `make evidencecheck` | collects and validates a JSON evidence bundle for one concrete host, including source audits, dependency closure, and PTY terminal evidence when a fixture binary is supplied |
| `make evidencetoolcheck` | exercises the single-bundle and matrix-evidence checkers against synthetic valid and invalid bundles, including path-escape, missing-record, duplicate-platform, version-mismatch, and extra-platform cases |
| `make evidencematrixcheck` | validates a supplied final set of external per-platform bundles for required platform coverage, unique platform ids, consistent versioning, terminal evidence, and live-provider evidence |
| `make statuscheck` | verifies `IMPLEMENTATION_STATUS.md` percentages are internally consistent |
| `make sizecheck` | enforces preferred/hard line budgets and the 2,000-line per-file review trigger |
| `tests/test_provider_transport` | exercises the real libcurl create/count/compact transport against a local loopback HTTP server |
| `tests/pty_*.py` | exercises the interactive terminal composer, live resize, suspend/continue, and TERM/width fallback behavior through a PTY on the current POSIX host |

The PTY implementation is now compiled through a single `SNAJPAGENT_HAVE_PTY`
capability surface. Linux uses `<pty.h>` and macOS uses `<util.h>`; runtime PTY
behavior then uses the same `openpty`, `ioctl(TIOCGWINSZ/TIOCSWINSZ)`,
controlling-terminal, immediate-run, yielded-run, and `write_stdin` paths.

## External evidence still required

External evidence still required before a production release:

1. run the full release gate on each advertised platform/architecture,
   including at least Linux x86-64, Linux AArch64, macOS x86-64, and macOS Apple
   Silicon where those are claimed by the release notes;
2. run `make livecheck` with a real `OPENAI_API_KEY`, network access, and quota;
3. run `make releaseevidence` or otherwise archive a `make evidencebundle` output
   verified with `make evidencecheck EVIDENCE_DIR=...` for each concrete shipped
   executable, including the selected Jansson library, libcurl library, detected
   libcurl TLS/resolver/compression/HTTP backends, and host terminal evidence;
4. copy the checked platform bundles into the release workspace and run
   `make evidencematrixcheck RELEASE_EVIDENCE_DIRS="..."` with the required
   platform ids before claiming the release matrix is complete.

Until those external runs are recorded, this tarball remains a source-only
implementation checkpoint rather than a production release.

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
checker for a complete single-platform evidence record. The final release-matrix
aggregation gate is `make evidencematrixcheck`, with `RELEASE_PLATFORMS` defaulting
to `linux-x86_64 linux-aarch64 macos-x86_64 macos-arm64` and
`RELEASE_EVIDENCE_DIRS` pointing at the copied per-platform bundle directories.
