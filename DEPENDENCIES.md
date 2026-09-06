<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Dependency and vendoring inventory

This repository intentionally does **not** vendor third-party implementation
source. Ordinary native builds use first-party source plus platform/POSIX
interfaces, system libcurl, and system Jansson. That source policy is machine-checked
by `make depscheck`; the built executable closure is captured and validated by
`make depclosurecheck`, release evidence bundles are created/checked by
`make evidencebundle` and `make evidencecheck`, the evidence validators are
self-tested by `make evidencetoolcheck`, and the final multi-platform bundle set
is checked by `make evidencematrixcheck`.

Vendored third-party implementation source: none.

Vendored third-party header source: none.

The linked/runtime dependencies for a normal provider-capable build are:

| Dependency | Source in this tarball | How it is used |
|---|---|---|
| POSIX/libc/platform APIs and pthreads | system | terminal, files, processes, signals, memory allocation, and the engine/presentation thread pair (`-pthread`) |
| system Jansson | not vendored | strict JSON parsing, construction, and canonical event/request encoding |
| system libcurl | not vendored | bounded OpenAI Responses create/count/compact HTTPS transport |
| libcurl backend closure | not vendored | TLS, resolver, compression, HTTP, and other backends selected by the system libcurl build |
| tmux | not vendored; test-only | optional rendered-screen regression in `make check`; required by `make tmuxcheck` and `make terminallivecheck` |

## Self-contained Linux x86-64 build

`make prod-linux-x86_64` explicitly uses the pinned nixpkgs revision in
`nix/portable.nix`. Its independent Nix build does not replace the ordinary
host binary or objects. The executable statically links musl, Jansson and
libcurl with Mbed TLS; DNS (c-ares), IDN (libidn2/libunistring), HTTP/2 and
gzip/Brotli/Zstd support remain available. No third-party shared libraries,
Nix installation or certificate sidecar are required on the destination.
This is distinct from the smaller native executable, which uses system libraries.

The TLS CA input is nixpkgs' pinned Mozilla/NSS standard-PEM export. It is
embedded at build time, never fetched on startup. `SSL_CERT_FILE` explicitly
replaces it for provider and login/refresh connections, including HTTPS proxies.
Certificate-chain and hostname verification remain enabled. The standard-PEM
export is used because Mbed TLS does not read OpenSSL's auxiliary trusted-PEM
format; do not blindly convert auxiliary records into additional trusted roots.
Future releases must update the pinned trust/dependency inputs deliberately.

The static artifact's linkage is inspected directly with `file` and `readelf`:
it must have no ELF interpreter or dynamic dependencies. Static PIE retains
address randomization; its self-relocation dynamic section is not a shared
library dependency. The existing
`make depclosurecheck` is for the ordinary system-library build, not proof of
static TLS or application behavior. Functional tests and real execution are
still required; an empty dynamic dependency list is not feature qualification.

Select Mbed TLS's GPL-2.0-or-later license option with this GPL-2.0-only
application. libidn2 and libunistring also offer GPLv2-compatible options;
libunistring 1.4.1's upstream README explicitly documents its dual license even
though nixpkgs metadata lists LGPLv3 only. Preserve upstream notices, the
Mozilla certificate-data license, and corresponding source/build instructions
when redistributing a linked executable. Pinned dependencies live in the Nix
store; external non-Nix media/SDK inputs belong in the ignored `.assets-cache/`.

## macOS ARM64 and Intel cross-builds

`make prod-macos-arm64` and `make prod-macos-x86_64` use the same pinned upstream dependency sources via
`nix/macos.nix`, Linux-hosted LLVM 21.1.7 and the fixed-output Apple SDK 15.5
recipe from nixpkgs. The deployment target is macOS 11. The entire application
dependency stack, including GNU iconv/libunistring/libidn2, is static; the
linked artifact uses only `/usr/lib/libSystem.B.dylib`. macOS 14/15's system
iconv has known libunistring incompatibilities, so it is not substituted for
the static GNU implementation. SDK declarations do not prove old-OS symbol
availability: compile with availability warnings as errors and select genuine
upstream missing-symbol fallbacks where necessary.

LLVM's Mach-O linker creates an ad-hoc signature and llvm-strip regenerates
its hashes. Matching optional symbols are retained as a dSYM with the same
UUID. This is not Developer ID signing, notarization, or runtime qualification:
the initial targets are explicitly experimental until actual macOS tests
pass. No separately installed third-party shared libraries are introduced.

`make -jN prod-macos-universal` uses independent slice prerequisites and
`llvm-lipo` to combine the executables and their dSYM DWARF payloads. The
per-slice code signatures remain intact: thinning the combined executable
must reproduce each original payload byte-for-byte. The combined dSYM must
retain both original UUIDs. This coalescing is packaging, not proof of runtime
compatibility, signing identity or notarization.

## Parallel production matrix

`make -jN prod-matrix` explicitly builds Linux x86-64/AArch64, macOS
ARM64/Intel/universal, and Windows x86-64/ARM64. This is the full implemented
set, not the completed legacy/exotic portability roadmap. The remaining ports
are still in development. SDK availability never silently
reduces the requested set; a failed target fails the command.

Each target uses its own `build/matrix/OS-ARCH` Nix output link. Universal macOS
depends on both slices; Nix safely shares immutable dependencies and downloads.
Each running recipe allows one Nix build job and one core, so outer `-jN`
controls concurrent work without multiplying it by another per-target `N`.
Use `make -k -jN prod-matrix` to finish independent targets after a failure;
successful outputs remain available and reruns reuse the Nix store. Ordinary
`make` stays host-only, and `make help` starts no builds or network requests.
The matrix rejects `DEBUG=1`, does not replace the native executable, install
anything, boot QEMU, or contact a model. Build success is not runtime support;
the platform sections retain the actual qualification limits.

## Experimental native Windows x86-64 and ARM64

`nix/windows.nix` builds static x86-64 Windows Jansson, Mbed TLS, compression,
c-ares, HTTP/2 and GNU Unicode/IDN libraries using the same pinned nixpkgs
sources. It takes the `pkgs` exported by `nix/portable.nix`. The compile API
baseline is Windows 7; actual execution so far is in Windows PE
10.0.26100.6584 from Microsoft's 25H2 evaluation media. A version macro does
not establish an older-OS runtime pass. `make prod-windows-x86_64` packages the
complete native executable with static application libraries and embedded roots
at `build/matrix/windows-x86_64/bin/snajpagent.exe`; `.debug/` contains optional
matching symbols. Nix and third-party runtime DLLs are not needed on Windows.
Filesystem/ACL, native Unicode console, process jobs/overlapped pipes, ConPTY,
IRC and provider integration are implemented. Runtime qualification is scoped
to the actual tested modern guests; old Windows and other architectures remain
in progress.

`make prod-windows-arm64` uses the same recipes with pinned nixpkgs
`ucrtAarch64`, LLVM 21.1.7 and statically linked winpthreads. Its native PE32+
executable is at `build/matrix/windows-arm64/bin/snajpagent.exe`; optional
symbols use the same adjacent `.debug/` layout. The OS UCRT is already part of
Windows ARM64, not an extra application DLL to install. The x64 GCC/msvcrt
closure is unchanged. LLVM resource tools receive explicit Windows headers;
autotools and Gnulib probes use their actual pthread link flags.

The ARM64 production build has run in official Windows PE 10.0.28000.1 under
QEMU TCG, with native base/console/file/process checks and real provider
round trips for internal RO, parallel commands and ConPTY. TLS rejects an
untrusted CA and wrong hostname, accepts an explicitly trusted test CA, and
transient HTTP retry preserves the request. Interactive session replay and
normal exit also pass. The executable is
approximately 4.2 MB. This is not full desktop or earliest Windows 10 ARM64
qualification. VM NIC drivers belong to the guest hardware setup, not the
agent's executable dependencies.

The existing base/SSE/JSON/wire/Responses/retry tests and upstream Mbed TLS
self-tests run there without third-party runtime DLLs. An upstream curl CLI
was built only to check the static dependency closure and reported HTTP/2,
IDN, asynchronous DNS, TLS and gzip/Brotli/Zstd. It is not a shipped helper,
an external `/ro` tool. The production agent separately completed native `/ro`,
parallel command and ConPTY provider round trips through a host-local fake
endpoint, plus TLS untrusted-CA rejection, explicit-CA success and hostname
rejection. No paid model was needed for those checks.

`src/platform.c` owns the native clock/entropy/descriptor primitives and
Unicode scalar width. Existing POSIX builds retain libc width behavior;
Windows uses the already statically linked libunistring so supplementary
characters are not truncated to 16-bit `wchar_t`. Keep the upstream license
notices and corresponding source, including the MinGW thread runtime's
GCC runtime exception. Older Windows still needs genuine thread/crypto/API
fallbacks and qualification; a DLL import archive renamed to `.a` is never
a self-contained static dependency.

The Windows-only `regex` library attribute imports Gnulib's POSIX ERE module
at pinned revision `58df1afe785d3067cfa474ab57ccf283665dfa38` through
`nix/windows-regex.nix`. Only its LGPLv2-compatible module closure is compiled;
no third-party implementation is vendored and no external grep executable is
used. The small first-party charset adapter is GPL-2.0-only. Preserve both
sets of notices and the corresponding source/build recipe when redistributing.

The static engine handles UTF-8 internally, independently of msvcrt's locale
support: its charset, multibyte width, DFA fast path and Unicode character
classes consistently use UTF-8/Unicode. It does not change the process-global
CRT locale or require UCRT or a separately installed regex DLL. POSIX builds
continue using libc regex. This dependency is part of the Windows port in
progress; it does not establish complete Windows agent support.

`src/snag_jansson.h` is the only Jansson include surface in first-party C code. It
prefers a system `<jansson.h>` when one is available. Some minimal qualification
roots carry `libjansson.so.4` without the development header; for those roots the
wrapper falls back to `src/snag_jansson_abi.h`, a GPL-2.0-only first-party ABI
declaration shim. That shim contains declarations only, no parser, encoder,
allocator, object implementation, or upstream Jansson source, and it does not use
the name `src/jansson.h` so it cannot silently shadow a system development
header.

`src/provider.c` is the only first-party file allowed to include libcurl headers.
All other code reaches HTTP transport through the provider interface.

A system-library release build still must archive the concrete executable dependency closure for each shipped platform:
the selected Jansson library, libcurl library, and libcurl's enabled TLS,
resolver, compression, HTTP, and other runtime backends. `make depclosurecheck`
uses the platform loader tools (`ldd` on Linux, `otool -L` on macOS) to reject
unresolved dependencies and missing libcurl/Jansson linkage; set
`SNAJPAGENT_DEP_CLOSURE_JSON=path` or pass `--json-out path` to retain the
JSON record for release evidence. `make evidencebundle` packages that record
with the source-audit and terminal-evidence records for the current host, while
`make releaseevidence` additionally requires live provider access. Once external
platform bundles have been copied into the release workspace, `make
evidencematrixcheck RELEASE_EVIDENCE_DIRS="..."` verifies unique platform ids,
consistent versioning, required platform coverage, terminal evidence, and
live-provider evidence across the matrix. The tree includes local transport
evidence and an optional `make livecheck` harness, but external live-provider
and advertised-platform closure evidence remain release gates.
