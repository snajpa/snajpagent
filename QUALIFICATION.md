<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Qualification ledger

This file records test coverage and outstanding platform and provider checks.
Static inspection, local loopback tests, target-platform execution and live-provider
runs each cover different behavior. Report the actual scope of each result.

The [2026-10-01 regression audit](design/regression-audit-20261001.md) maps the
32 audited development commits to their permanent tests and records coverage additions.

Credential-recovery PTY fixtures exercise the production authentication path with
local HTTP providers. They verify switching away from an invalid endpoint-bound
login during a failed turn and after session resume, plus explicit credential
reload. Each case asserts one completed original turn, the selected provider's
actual request and credential, and no duplicate prompt or credential in the journal.

Blocked-goal fixtures exercise real model tool calls and terminal output against
a local provider: required wait destinations, operator/IRC/timer/process/external
labels, resume/context restoration, goal listings and independent timer scheduling.
Store checks cover checkpoint preservation, invalid destinations and old records
without a wait channel. Vim transcript projection retains the destination and
redacts the blocker text through its ordinary source mapping.

## Development Vim workspace

Pager fixtures cover continued provider work and exactly-once catch-up after a
nonzero pager exit, as well as missing executables, interruption and resize.
Native terminal fixtures cover suspend/reattach with the external job retained.

History offset fixtures cover coordinates above 4 GiB, the signed 64-bit limit
and overflow rejection. The ARMv6 compiler reproduces the two original signedness
errors and accepts the corrected reader and transcript translation units.

The optional workspace and classic conversation tabs are development features.
Native macOS/Linux fixtures cover session creation/resume and controller ownership,
saved workspace layouts, independent drafts, command reports, IRC channel/query
privacy, stale-route recovery, history navigation/search, mouse selection and
clipboard publication. WITH_VM=0 builds retain classic conversations and the
remote wrapper. Restored drafts and uncertain IRC submissions require explicit
user action; the fixtures assert provider requests and recipient deliveries.

The Windows x64 executable passes five real direct-engine cases through Wine 10:
priority /fast, an actual local HTTP request, retained later typing, resume,
startup failure, workspace loss and joined cancellation during a held request.
Three full workspace cases exercise hiding a live engine, shared split drafts,
explicit quit, single-engine restrictions, explicit session resume and startup
failure recovery, plus history-search wakeups with a live engine and before
explicit engine startup after workspace resume. The Windows workspace-storage C suite and console-key cases
also pass. Regression witnesses cover held-directory reopening and Escape events
with no translated character. The runner maps private fixture directories to a
drive and owns/restores its Unix PTY raw mode. Wine inserts line breaks within
ANSI output; shutdown checks remove those bridge line breaks to check complete
escape-sequence emission. These results establish lifecycle/input behavior, not
native Windows desktop rendering. Broader Windows/base-suite qualification remains open;
the Wine base run stops in an unrelated command-quoting assertion.

Actual SSH/Mosh × Screen/tmux reconnect cases preserve the same workspace/owner,
two panes and a newer draft after transport loss. An explicit upload retry
retains exact UTF-8 file bytes and submits one prompt with its attachment.
Separate transfer tests cover resize, provider output during a picker and picker
exit when the transport dies. Native macOS clipboard integration verifies the
actual pasteboard, alongside isolated protocol/receipt/cancellation fixtures.

A 2 GiB synthetic journal on Linux opened/navigated in approximately 0.8–1.4 s
with approximately 13 MiB RSS. A full cold search took 358 s and cancelled in
7 ms; navigation cost and whole-journal search cost have different scope.
A permanent 128 MiB PTY regression checks live output in another pane during an
active history search, cancellation, tiny-terminal resize and an unchanged
stored source. It reproduces stale output with the shared reader and passes with
separate page/scan readers on macOS and Linux. With the same 2 GiB Linux source and a live
pane receiving 300 KiB, completion painted during the scan in 297 ms. Canceling
took 5 ms; shrinking to 3x18 and restoring the panes took 57 ms from cancellation.
Observed workspace RSS was 23–33 MiB during these steps. A subsequent two-second
idle interval had zero CPU ticks and zero terminal output. Both stored journals
retained their size and modification time. Existing search, navigation, selection,
anchor, controller and frontend cases also pass; the report suite passes across
four cases and its corrected legacy-snapshot consumer.
A full FreeBSD executable built against the 8.4 SDK passes twelve launch,
fifteen native-owner and nineteen controller cases on FreeBSD 14.5-RELEASE-p1.
All sixteen frontend cases pass across the suite and a corrected legacy-snapshot
case. The launcher covers exec failure and explicit retry, independent process
groups, signal-mask restoration, temporary PTY modes and owner lifetime.
All eighteen production profiles compile at revision `20b94173`: Linux x86_64,
AArch64, ARMv6, RISC-V64, PPC64LE and i686; macOS ARM64, x86_64 and universal;
Windows x86_64 and ARM64; both FreeBSD profiles; all three OpenBSD profiles; and
both NetBSD profiles. The copied executables' SHA-256 hashes match their successful
build records, and the target set matches `PROD_TARGETS`. This qualifies compilation
at that revision; runtime coverage remains the platform-specific scope above.
These development artifacts are separate from a release-matrix receipt.

The full macOS ARM64 and Linux x86_64 development installations at `7277eb5f`
also pass workspace restore/navigation, query/channel panes and cross-session
addressing. The installed Linux binary passes clipboard and upload reconnect
checks through all four SSH/Mosh × Screen/tmux paths. Separate `WITH_VM=0` builds
at that revision pass all twenty-one classic IRC query cases on each host and
report the disabled frontend when invoked with `vm`.

## tmux transfers and paste display

Permanent real-tmux fixtures place the multiplexer between the workstation
wrapper and agent and verify both file directions, exact bytes, receipts, clean
pane history and subsequent input. Cases include incompressible 64 KiB files,
a 10x48 terminal with another writing pane, cancellation of 2 MiB files, ambiguous
clients, client detach/reattach, local wrapper placement, dropped files, model
send_file and stock Mosh. Loopback OpenSSH includes remote tmux. Foreground tmux
servers and Linux pidfd tracking constrain teardown to fixture-owned processes.
Mac native tmux execution and arbitrary nested tmux/remote-wrapper chains remain
unqualified. These fixtures do not test desktop drag gestures or live providers.

The paste-display PTY fixture sends a complete bracketed paste and no subsequent
key until its final text is visible. It covers short, multiline UTF-8 and long
input, directly and through the native workstation wrapper. The installed
52200a9d baseline reproduced missing redraws; the fix renders at paste completion.

## 0.99.8c voice scope

The provider-transport and native-PTY fixtures cover command-report observations
across keyboard paging, interface input, pager failure and disabled paging.
They also exercise context handover, caption accumulation and the shared voice/UI
command path. The context suite covers cached request prefixes, retained retry
notices and old-checkpoint reconstruction across repeated compaction.

Target-device and live-provider voice checks remain outstanding for this version.
Long-session voice continuity and recovery against live providers remain
unqualified by these hermetic results. Earlier platform observations retain their
recorded source revisions and do not qualify a newer executable automatically.

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
| `make sizecheck` | reports source/test line counts and the largest production file, with no thresholds or line-count failures |
| `tests/test_provider_transport` | exercises the real libcurl create/count/compact transport against a local loopback HTTP server |
| `tests/test_tools` | checks POSIX command interruption with blocked or ignored parent SIGINT through pipes and PTYs, preserving the parent's signal state |
| `tests/test_token_preflight.py` | verifies exact API-key token preflight, tool continuation, resume and count-endpoint authentication failure through the production CLI |
| `tests/pty_*.py` | exercises the interactive terminal composer, live resize, suspend/continue, and TERM/width fallback behavior through a PTY on the current POSIX host |
| `tests/test_command_pager.py` | exercises command reports and aliases through a capture pager in a six-row PTY, combined reports, retained history and queue rows, active-turn paging, template overrides and direct-output fallback; checks real less navigation after resize and physical screen contents over stock Mosh when those programs and GNU screen are installed |
| `tests/test_session_status_protocol.py` | production CLI against version 2/3/4 owner sockets: fragmented replies, legacy busy replies, malformed/closed/stalled queries, shared deadline, status grouping, stored counts and unchanged journals |
| `tests/test_session_states.py` | exercises real attached/detached/stored states, read-only status queries, short selectors, status/recency ordering, the default 10 stored rows, explicit counts including zero, invalid operands, and complete resume/slash lists |
| `tests/test_upload_client.py` | exercises ten terminal-upload PTY cases on Linux x86-64, including pinned Go trzsz client binary/text multi-file transfer, rollback after invalid image preparation, synthetic protocol failures and terminal recovery; GUI drag and nested SSH are not exercised |
| `tests/test_store` | exercises cleanup of exact crash-left upload staging on session resume and deletion, retention of existing media, and refusal of unfamiliar symlink leaves |
| `make tmuxcheck` | asserts the rendered screen/scrollback for deterministic streaming, Markdown enabled/disabled overrides, status, wrapping, steering, resize, queue, durable-text, and instruction-discovery scenarios, then runs one production IRC server plus two production clients against loopback fake Responses endpoints and checks bidirectional Markdown-rendered chat, three-agent model traffic, durable attribution, verbosity, color, peer leaves, and exact cleanup; `make check` runs it whenever tmux is installed |
| `make terminallivecheck` | runs the fixed vpsAdminOS 6.12.95 real-work prompt through the configured default provider in a 52×18 tmux, serializes checks using the same config file, and compares rendered public text with durable response events and `AGENTS.md` metadata |

The PTY implementation is now compiled through a single `SNAJPAGENT_HAVE_PTY`
capability surface. Linux uses `<pty.h>` and macOS uses `<util.h>`; runtime PTY
behavior then uses the same `openpty`, `ioctl(TIOCGWINSZ/TIOCSWINSZ)`,
controlling-terminal, immediate-run, yielded-run, and `write_stdin` paths.
The tmux layer complements those raw-PTY checks by interpreting cursor movement,
erase, wrap, and resize sequences as a real terminal does.
## Duplex voice processing

The audio-enabled `tests/test_provider_transport` suite feeds the real device
callback without opening hardware. A delayed, reflected copy of actual rendered
samples models linear speaker echo; an independent two-tone signal models
near-end input. The test measures echo-energy reduction and retained near-end
amplitude/correlation, adapts varying callback sizes, and checks partial-frame
discard and filter reset across mute/unmute. It also exercises real Opus
decoding, packet loss/reordering and the local RTC round trip. The round trip
checks received packet timestamps against callback sample positions across a
mute interval and discarded partial input, with no packets forwarded while
muted. Capture tests also cover partial reads and sample-counter wrap.

This covers synthetic sample flow. Physical devices, nonlinear loudspeaker echo,
clock drift, human speech intelligibility and live ASR require separate evidence.

## Native terminal transfers

The stock-Mosh argument regression uses an isolated recording SSH stub to verify
the hostname and literal remote command, including --resume, --help, whitespace,
Unicode and shell syntax. It covers leading Mosh option values, attached values,
absolute launcher paths and explicit separators. The loopback Mosh transfer test
omits the manual separator and verifies uploads, downloads and prompt reuse.

The development-source native wrapper is exercised on Linux x86-64 by
`tests/test_remote_terminal.py`: isolated client startup, literal argv, PTY size,
exit/signals and terminal restoration, binary upload and binary/empty download,
collision destinations and actual-path receipts, nested native endpoint ownership,
local GNU screen containing a wrapper, remote GNU screen attach/detach, pending
outbox delivery on wrapped resume, unwrapped resume, and changed-source retention.
The command-name regression checks `/receive` and `/send PATH` help/usage, rejects
the former command names and malformed prefixes; transfer cases use the new names.
A synthetic native peer advertises 64 KiB blocks and verifies every upload DATA
block remains at most 1 KiB, plus complete bytes and digest/EXIT. The nested-screen
binary upload covers the upstream input queue hidden by an intermediate relay.
The detached-screen regression checks receipt scrollback and restoration of a
partly edited draft and its cursor on reattachment. Changed-source checks also
cover same-size edits with restored mtime, requiring the saved SHA256.

On 2026-09-30 a local macOS PTY fixture ran the unmodified Mosh 1.4.0 client
and server with `mosh --local --no-init` inside the native wrapper. It sent one
128-byte file each way, checked the landed bytes and digest/EXIT receipt, kept
protocol text out of captured terminal output, accepted another prompt and
restored the transfer terminal before the wrapper exited on SIGTERM. Mosh
may split a visible long path with cursor-position
sequences; the assertion checks the actual file and final receipt instead of a
contiguous raw PTY path. A separate disposable screen-state relay checks short
title bounds, dropped and duplicate frames, stale replay, cancellation and
resize with exact bytes, prompt reuse and no exposed protocol text.

On 2026-10-01 the full Mac ARM64 wrapper transferred a 1,024-byte file each way
through stock Mosh 1.4.0 to a Linux VM. Both `--no-init --predict=never` and
`--no-init` with default prediction passed content/digest receipts, hidden title
frames, prompt reuse and terminal-mode restoration. Isolated state and a runner
that reaps its own descendants kept the probe separate from existing sessions.
These small-file checks do not qualify bulk throughput or desktop GUI behavior.
The Linux remote-terminal suite passed all 39 cases, including prompt return
when an exited transport leaves a background descendant holding its PTY. Linux
upload/download suites passed with three/five optional skips respectively.

The macOS lean build (`WITH_AV=0 WITH_PDF=0 WITH_OFFICE=0`) ran 39
remote-terminal cases: 36 passed and three platform cases skipped, including
passing handshake replay during transfer and after END. Upload-client and
download-client suites ran 12 and 16 cases respectively, with five and six
existing optional skips and the remaining cases passing.
The complete `make check` stopped at `tests/test_tools`' direct-argv test:
the existing converter sandbox requests a 2 GiB `RLIMIT_DATA` value that this
host rejects, causing helper exit 125. An untouched-baseline build reproduced
the same assertion; this transfer change leaves that sandbox policy unchanged.
The linked macOS libcurl also lacks WebSocket support; the voice-socket test
checks the unsupported response and skips its socket cases by capability.
Fixtures use `TMPDIR=/private/tmp` so symlinked macOS temporary ancestors do
not conflict with the store's real-directory safety checks. The full integration
gate is not recorded as passing on this host.

`tests/test_download_client.py` covers server framing, wrong digests/cancellation,
uncertain model-transfer retention, durable queue IDs/list/remove/clear and
read-only guards. `tests/test_upload_client.py` and the download suite also exercise
pinned trzsz-go revision `665084211187` for explicit transfers. Go clients do not
answer the native availability probe; model exports stay pending until a native
attachment, or can be fetched explicitly and removed from the queue.

`tests/test_remote_ssh.py` uses an isolated loopback OpenSSH daemon, temporary keys
and pinned host verification. It exercises first-hop SSH, remote UTF-8 GNU screen
and two nested SSH hops. This root-only fixture skips when its prerequisites are
absent. All three routes assert bytes, actual workstation-path receipts and
confirmed outbox removal.

Actual macOS terminal UI, desktop file pickers/drag-and-drop, external-network SSH,
arbitrary nested multiplexers and tmux transfer relay remain unqualified. Native
selection is a one-file terminal path prompt. The transfer endpoints and wrapper
are POSIX-only; Windows terminal transfers are outside this implementation.

## Download-page kernel baselines

The download page's Linux baselines are conservative libc/architecture floors,
not experimentally established oldest-working kernels for every executable.
Linux 6.12 was a test environment, not a minimum requirement.

| Target | Conservative baseline | Basis |
|---|---|---|
| x86-64, i686, ARMv6/ARMv7, PowerPC32 | 2.6.39 | musl's fully POSIX-conformant kernel floor |
| AArch64 | 3.7 | upstream ARM64 architecture ABI |
| PowerPC64 little-endian | 3.13 | upstream little-endian/ELFv2 ABI |
| RISC-V64 | 4.15 | stable upstream RISC-V userspace ABI |
| i686 legacy | 2.4.27 | oldest qualified LinuxThreads build kernel; not a libc-wide guarantee |

Sources: [musl platforms](https://wiki.musl-libc.org/supported-platforms),
[Linux 3.7 ARM64](https://github.com/torvalds/linux/blob/v3.7/arch/arm64/Kconfig),
[Linux 3.13 PowerPC](https://github.com/torvalds/linux/blob/v3.13/arch/powerpc/platforms/Kconfig.cputype),
and the [RISC-V port developer's ABI announcement](https://www.sifive.com/blog/all-aboard-part-8-the-risc-v-linux-port-is-upstream).

Earlier x86-64 builds also run on Debian Sarge's Linux 2.6.8 and CentOS 3.9's
backported 2.4.21-50.EL kernel. The latter depends on that vendor's threading
support and does not establish arbitrary upstream Linux 2.4 compatibility.
Those results and their exact scope remain in the manual and records below.
No new oldest-kernel runtime qualification was performed for 0.99.5.

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

For 0.99.6, `PROD_TARGETS` is seventeen entries and three targets are explicitly deferred inside the tagged
tree — `linux-i686-legacy`, `linux-riscv64` and `linux-ppc32` — each with its reason recorded in the Makefile
and restated in the release table, the release notes and the download page. Build evidence for this release is
artifact-level: every shipped executable was verified by size, update marker and embedded version, which is
build evidence and not runtime testing, and no new operating-system or kernel runtime qualification was
performed. Linux x86-64 carries the release's one live-provider result — the prompt-cache reuse measurement,
87.5% of input tokens served from cache against 30.0% before the change in a single session — which is
live-provider evidence for that host and provider only. The i686 target is now linked static non-PIE after the
pinned FFmpeg's CELT object refused a static-PIE link; the RISC-V target's FFmpeg wall was fixed before the
target was deferred at the wall behind it; and the 32-bit big-endian PowerPC target fails at a link its pinned
toolchain resolves through the 32-bit powerpc CRT/spec, which introduces a reference to
`__stack_chk_fail_local` (the package's own object carries no such undefined reference) that musl does not
provide. A remedy shape is identified for that last one — supplying the alias, which keeps stack protection;
rebuilding without stack protection also links but drops it — and the fix is carried into the development
build, where the next item to check is boost's `No best alternative for libs/mpi/build/boost_mpi` line.
Prior platform evidence above retains its original scope. By operator decision
on September 23, 2026, PPC32 is an unsupported opt-in recipe outside the 0.99.8
production matrix; its earlier emulated checks do not imply shipment.

## Documentation and current-source coverage

The current manual and design contracts are checked against source behavior;
that review does not add runtime qualification to any platform or provider.
September 9 source follow-ups have focused local HTTP/PTY/tmux and component
coverage for active command admission, manual compaction, transcript/queue
rendering and history totals. Those commits postdate the immutable 0.99.4
release. Keep exact source/build identity with test evidence and use each
release's companion manual when inspecting that release.

A feature or behavior fix updates its affected documentation in the same change
under AGENTS.md and EDITORIAL.md. Retain prior platform evidence with its original
scope; neither compilation nor documentation publication establishes a new live
provider or operating-system pass.

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
