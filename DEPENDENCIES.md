<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Dependency and vendoring inventory

This repository intentionally does **not** vendor third-party implementation
source. Ordinary native builds use first-party source plus platform/POSIX
interfaces, system libcurl, and system Jansson. That source policy is machine-checked
by `make depscheck`; the built executable closure is captured and validated by
`make depclosurecheck`, release evidence bundles are created/checked by
`make evidencebundle` and `make evidencecheck`, the evidence validators are
self-tested by `make evidencetoolcheck`, and those bundles can be checked by `make evidencematrixcheck`. Release
publication follows `RELEASE.md` and its current `PROD_TARGETS`, not the older
four-platform defaults in those optional tools.

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

## Self-contained Linux builds

Native POSIX builds and Linux release recipes select `_FILE_OFFSET_BITS=64`,
so 32-bit libc builds retain large-file seek/stat/truncate support. Keep that
feature macro when replacing CPPFLAGS. This does not enlarge a 32-bit address
space or claim that every old kernel supports modern time/thread APIs.

`make prod-linux-riscv64` uses the pinned RISC-V musl toolchain with an
RV64GC instruction baseline and LP64D ABI. Application libraries, TLS and
trust roots are embedded. The static PIE needs no ELF interpreter or shared
libraries; archive symbols bind locally so allocator callbacks use relative
startup relocations. Existing base/configuration/SSE/IRC tests and full-agent
read-only, parallel-command, PTY, resume and TLS checks run on Alpine 3.22.5
with Linux 6.12.94 under QEMU's RV64 CPU. Earlier kernels and physical-board
performance remain unmeasured.

`make prod-linux-armv6` uses the pinned `muslpi` toolchain for an ARMv6KZ/VFPv2
hard-float baseline. One static PIE covers ARMv6 Raspberry Pi 1/Zero-class
systems and ARMv7; application libraries, TLS and trust roots are embedded.
It requires a Linux hard-float environment and the native shell/tool commands
used by the agent. It has no separate interpreter or shared-library dependency.
Soft-float userlands and older kernels need separate qualification.

The same binary runs on local Alpine 3.22.5 ARM1176 and Cortex-A7 guests.
The Raspberry Pi QEMU guest uses the original Alpine kernel/userland with a
device tree adapted for unimplemented power and SD-DMA hardware, and QEMU's
instruction-counted clock for reliable timer interrupts. These are emulator
settings; they do not establish physical-board performance. Musl carries
separate ARMv6/ARMv7 atomic routines and selects them from the kernel's CPU
capabilities at startup, so its merged ELF instruction attributes include
routines above the executable's baseline.

`make prod-linux-i686` builds the full 32-bit static-PIE agent with the same
application libraries and compressed embedded roots. It uses an i686 baseline,
64-bit musl file offsets/time_t, and no required extra runtime libraries.
Existing static units and TLS work under Pentium II/III CPU emulation, without
SSE/SSE2; actual guest tests run on Alpine 3.22.5 x86 / Linux 6.12.94 with its
own SSE2-capable CPU requirement. Guest RO, parallel commands and POSIX PTY
provider round trips pass. This is not Linux 2.4 or every earlier kernel
qualification; use the separate legacy target below for that runtime.
Output is `build/matrix/linux-i686/bin/snajpagent`, with optional matching
symbols in `.debug/`. A 32-bit process still has a 32-bit address space.

`make prod-linux-i686-legacy` builds a static **non-PIE** executable at
`build/matrix/linux-i686-legacy/bin/snajpagent`, with optional matching symbols
in `.debug/`. It retains the application libraries, embedded roots and UTF-8
locale tables but uses uClibc-ng 1.0.55/LinuxThreads and GCC 14.3 TLS emulation.
No compiler, locale package, certificate sidecar or third-party runtime library
is needed on the destination. Use `LANG=en_US.UTF-8` if its current locale name
is unavailable. Prefer the modern musl static-PIE target where it runs:
it retains address randomization and its modern thread runtime.

Actual Debian Sarge Linux **2.4.27-3-386**, on QEMU Pentium III, passes base and
IRC units plus production RO list/read/grep and denied-write enforcement,
overlapping commands, POSIX PTY, interactive resume/exit, TLS trust/name checks
and a hostname-based local provider connection. This is not every 2.4 release
or paid live-provider qualification. Working procfs and secure OS entropy are
required; PTY commands also require devpts. No guest test helper is part of
the executable.

The legacy recipe keeps native calls first, with runtime fallbacks for metadata,
rename and flagged sockets. Nonblocking/close-on-exec socket flags are set with
checked `fcntl` operations only after the kernel rejects their atomic form;
that old-kernel setup is not atomic. The clock adapter first tries available
kernel clocks; otherwise realtime uses the old gettimeofday syscall and
monotonic time uses `/proc/uptime`, at 10 ms resolution. It extends the standard
i386 100 Hz jiffies wrap and clamps late samples; unambiguous extension requires
samples less than half a wrap apart (about 248 days). It never substitutes
wall-clock time for monotonic time.

GCC's existing pthread-key TLS emulation keeps real per-thread state; static
link roots retain its required pthread callbacks. The narrow dependency patches
retain their upstream LGPL terms in `LICENSE_SCOPE`. Preserve the matching
uClibc source/patches and GCC runtime notices/relinking materials when distributing
this executable. The compiler and stable SDK are cached independently of
implementation-only runtime libc corrections.

`make prod-linux-x86_64` explicitly uses the pinned nixpkgs revision in
`nix/portable.nix`. Its independent Nix build does not replace the ordinary
host binary or objects. The executable statically links musl, Jansson and
libcurl with Mbed TLS; DNS (c-ares), IDN (libidn2/libunistring), HTTP/2 and
gzip/Brotli/Zstd support remain available. No third-party shared libraries,
Nix installation or certificate sidecar are required on the destination.
This is distinct from the smaller native executable, which uses system libraries.

The shared Linux recipe selects libcurl's existing nonblocking pipe-and-fcntl
wakeup backend. Its eventfd/pipe2 branches assume those syscalls exist at runtime;
on older kernels their failure can surface as a misleading out-of-memory error
from multi-handle initialization. Wakeup and asynchronous DNS remain enabled;
the compatible backend requires no extra runtime library or binary variant.
Close-on-exec setup on this legacy-capable pipe path is not atomic.

The x86-64 artifact has also been exercised on Debian Sarge's
Linux 2.6.8-12-amd64-generic: descriptor/filesystem checks, internal read-only
tools, parallel commands, PTY, session resume and TLS trust/hostname checks.
The shared clock fallback extends that same static-PIE artifact to CentOS
3.9's backported **2.4.21-50.EL x86_64** kernel. Its base/IRC units, production
RO enforcement, parallel commands, PTY, resume, TLS verification and hostname
connection were exercised in local QEMU. Kernel coverage is specific to those
releases; the x86-64 build count and file size are unchanged.
Native missing-syscall fallbacks preserve an unreaped process-group leader
using checked procfs child state. A working procfs and OS entropy source
are required: Mbed TLS retains its secure `/dev/random` source, which can
block on pre-5.6 Linux when entropy is depleted, even after initial seeding.
For x86-64 and legacy i686, absent kernel clocks select the `/proc/uptime`
fallback described above. Musl's EINVAL translation is checked against the
raw syscall; the fallback requires kernel ENOSYS.

The TLS CA input is nixpkgs' pinned Mozilla/NSS standard-PEM export. It is
embedded at build time, never fetched on startup. `SSL_CERT_FILE` explicitly
replaces it for provider and login/refresh connections, including HTTPS proxies.
Portable builds compress these exact PEM bytes with pinned Zstd and decode
them in memory using the decoder already linked for HTTP content encoding.
Both origin and proxy trust receive owned copies; malformed embedded data
fails closed. This adds no runtime library or certificate-file dependency,
does not change root selection, and leaves the explicit file override intact.
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

## FreeBSD amd64

`make prod-freebsd-amd64` builds the executable at
`build/matrix/freebsd-amd64/bin/snajpagent`, with matching symbols in `.debug`.
The pinned 8.4 release disc supplies system libraries, CRT and headers;
the same pinned application libraries and embedded roots used by other targets
are cross-built with LLVM. Application dependencies and libutil are static;
the only shared imports are the native `libc.so.7` and `libthr.so.3`, loaded by
`/libexec/ld-elf.so.1`. Native libc handles the host release's locale-data format;
8.4's static libc cannot load 14.4's UTF-8 locale data. Keep threading dynamic
with libc rather than mixing the old static and native shared thread runtimes.
The executable is non-PIE at this old ABI baseline. Third-party notices include
the FreeBSD base components and their GCC runtime licensing alongside the
application dependency notices.

Actual FreeBSD 8.4 and 14.4 amd64 qualification covers base and IRC tests, internal
read-only inspection and denied writes, parallel commands, PTY output/status,
interactive resume and TLS distrust/trust/hostname checks with local fixtures.
Other releases remain unverified.
The platform layer uses native PTYs, non-reaping `waitpid` polling, `fsync`
and the kernel random device. Directory streams preserve caller descriptor
ownership across older libc failure paths. Native GNU make builds select BSD
API declarations and libutil automatically. Use a UTF-8 locale and mounted
devfs; the qualification guest used `en_US.UTF-8` and UFS for large sparse files.

### FreeBSD 5.1/5.5 legacy target

`make prod-freebsd-amd64-legacy` builds the full agent against the pinned 5.1
mini-install disc into `build/matrix/freebsd-amd64-legacy/bin/snajpagent`, with
matching symbols in `.debug`. Application libraries and libutil are static;
only native `libc.so.5` and `libc_r.so.5` are imported through
`/usr/libexec/ld-elf.so.1`. This is a separate ABI from the 8.4-based output, which remains available for 8.4 and 14.4.
The old compiler driver uses the release's actual CRT and libgcc ordering;
these runtimes predate crtbeginT.o, libgcc_eh and stack-protector support.
The legacy executable is non-PIE and has a non-executable stack.

The legacy target includes Gnulib's static UTF-8 regex and libunistring width
support, shared with the Windows recipe, and runs with the C character locale.
It needs no installed UTF-8 locale data. The derived SDK broadens the original
GCC-3-only attribute declarations for Clang. LLVM compiler-rt supplies 128-bit
unsigned division under Apache-2.0 WITH LLVM-exception; preserve those notices.
Compiler stack realignment accommodates the first amd64 libc_r thread ABI.
The target's join adapter waits for a joinable thread to leave libc_r's active
list before native status collection, avoiding its lost live-join notification.
The 5.1 archive provides a publisher MD5; the recipe's SHA256 pins the bytes
retrieved over official HTTPS and checked against that legacy digest.

Actual 5.1 and 5.5 amd64 QEMU qualification covers the full agent's read-only tools
and denied writes, interdependent parallel commands, PTY output/exit status,
interactive history/resume, TLS distrust/explicit trust and hostname checks.
Base, IRC and SSE units pass, including descriptor-relative operations,
same-parent directory rename, sparse files, Unicode, threads, terminal modes,
and repeated non-reaping child-exit observation. Other releases remain
unverified for this legacy artifact.

Pre-8 builds retain validated paths for managed directory descriptors and
reopen directory streams with identity checks. Closed/reused descriptors are
pruned on subsequent cache access. Same-parent renames can be rediscovered;
a move to a different parent requires reopening the directory. These pathname
fallbacks have an external rename race between validation and access. Avoid
concurrent renames of workspace/state directories; no process-wide cwd changes
or external filesystem helpers are used. Close-on-exec setup is non-atomic on
these kernels. Native at-family APIs remain selected on newer build baselines.

For old kernels lacking non-reaping wait flags, process-list snapshots preserve
child ownership. PID and parentage are checked before interpreting the old
zombie representation. FreeBSD 5.1 omits zombie PIDs, so it also accepts the
child-owned process group with matching parentage; the owner still collects
status with waitpid. Source checks and the focused tests cover this path independently of full-agent
qualification. The old libc realpath call uses a caller-owned buffer and checks
that the original path exists before normalization.

## OpenBSD amd64

`make prod-openbsd-amd64` builds the OpenBSD 7.9 executable at
`build/matrix/openbsd-amd64/bin/snajpagent`, with matching symbols in `.debug`.
The SHA-256-pinned official installation image supplies the native headers,
startup objects and OS libraries. The PIE executable imports only
`libc.so.103.0` and `libpthread.so.28.1`, with `/usr/libexec/ld.so` as its loader.
Application dependencies, libutil, the Unicode regex/width engine and compressed
CA bundle are static. Other OpenBSD releases require separate ABI qualification.

The shared Gnulib regex recipe uses its UTF-8 decoder and encoder replacements
and libunistring with the C character locale. This preserves non-ASCII matching,
case-folding and character classes independently of native libc regex. The
SDK-only unversioned library aliases serve the cross-linker; the executable
retains native versioned imports and requires no runtime aliases. Cross-build
helpers run on the build host; native startup/runtime bytes come from the SDK.
Preserve the base-system and dependency notices when redistributing.

OpenBSD 7.9 qualification covers base/IRC/SSE tests, internal read-only
inspection and denied writes, parallel commands, PTY execution, durable resume,
and TLS trust and hostname checks with local fixtures. The IRC capacity test
raises its own descriptor soft limit to 256 because it holds both endpoints of
64 connections in one process. Ordinary agent checks use the default limit.

### OpenBSD 5.9 legacy ABI

`make prod-openbsd-amd64-legacy` uses the pinned 5.9 installation image and
requires `libc.so.84.2`, `libpthread.so.20.1` and `/usr/libexec/ld.so`.
The 5.9 executable fails to load on 7.9 because that libc ABI is absent; the
separate output preserves both release runtimes without installing old libraries.
Application libraries and Unicode support remain static, with PIE, stack
protection and a non-executable stack. Matching symbols accompany the executable.

Clang uses emulated thread-local storage at this target. The build includes
LLVM compiler-rt's thread-local storage and 128-bit division support and the
native GCC runtime archive. Preserve their licenses with the base-system and
application dependency notices. The old system-header macro warning is scoped
to this build; all other compiler warnings remain errors.

The process adapter observes child exit through the native process query,
validating PID and parentage before checking zombie status. The owning command
retains the final `waitpid` and exit status. Actual 5.9 checks cover the base,
IRC and SSE suites, read-only enforcement, parallel commands, PTY exit status,
interactive history/resume and TLS trust and hostname verification.

### OpenBSD 3.5

`make prod-openbsd-amd64-early` builds the separate 3.5 ABI using its native
base/compiler sets. It requires `libc.so.30.3`, `libpthread.so.2.4` and
`/usr/libexec/ld.so`; the same executable cannot load on 5.9. Application
libraries, Unicode, CA data and compiler support remain static. Matching
symbols accompany the non-PIE executable. Its native startup objects and
stack-protector ABI predate the modern hardening used by the 5.9/7.9 targets;
this build uses a non-executable stack, without compiler stack protection.
The archive provides publisher MD5 checksums; the recipe pins locally verified
SHA-256 values. No cryptographic publisher signature is claimed for these sets.

The early C compiler retains GNU89 extern-inline semantics for the native
headers and avoids synthesizing calls to absent `wcslen`. The existing Gnulib
closure supplies LGPLv2-compatible `snprintf`/`vsnprintf` and missing errno
values. First-party stream adapters use that formatter for `printf`/`fprintf`.
Modern POSIX-format modules with incompatible licensing are excluded. Native
`/dev/urandom` supplies both application and TLS entropy; `/dev/random` returns
EIO on the exercised 3.5 installation. Optional zstd trace hooks are disabled
because the old loader reports unresolved weak hooks.

Filesystem operations use the validated pathname fallback: directory identity
and no-follow checks remain, but external rename races and non-atomic close-on-
exec setup remain possible. Moves across parents require reopening a directory.
Child exit observation uses native `KERN_PROC2` with PID/parent verification,
leaving the final status to its owner. Native filesystem calls receive explicit
negative-length checks. Base/configuration/SSE/IRC tests and full-agent read-only,
parallel, PTY, durable/interactive resume and TLS trust/hostname checks run on 3.5.
The IRC test re-executes after raising its descriptor limit because this
libpthread sizes its descriptor table before `main`; ordinary agent checks use
the default soft limit of 128. No product limit is silently raised.

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

LLVM's Mach-O linker creates an ad-hoc signature for ARM64 and llvm-strip
regenerates its hashes. The Intel slice remains unsigned, including inside
the universal file. Matching optional symbols are retained as a dSYM with the same
UUID. This is not Developer ID signing, notarization, or runtime qualification:
the initial targets are explicitly experimental until actual macOS tests
pass. No separately installed third-party shared libraries are introduced.
Application link-time optimization uses the pinned LLVM toolchain. Its merged
object stays in the isolated build directory until dSYM generation, preserving
optimized application debug information; it is not a runtime file or a new
host-build default.

The macOS recipe also accepts an internal `deployment` argument for legacy
builds. Targets before 10.8 select the pinned cctools-port classic linker;
it retains the old Mach-O startup and relocation formats. Its Linux-only host
dependencies include TAPI and libdispatch. These tools are build dependencies,
not libraries loaded by the agent. The i386 recipe uses Apple's complete
10.12 SDK, downloaded from two official URLs with a pinned SHA-256.
Full x86-64 and i386 agents cross-link at deployment 10.5 with static
application dependencies, embedded roots, LTO and matching dSYMs. The classic
linker loads the pinned LLVM LTO library explicitly; its companion strip tool
preserves the old loader format. Required integer-division builtins from
LLVM compiler-rt 21.1.7 are linked statically. Preserve their Apache-2.0 with
LLVM-exception notices and corresponding source with redistributed binaries.
Production targets still use macOS 11; legacy Darwin execution remains
unverified.

Legacy source builds use optional native at-family calls where available.
Otherwise, held-directory identity checks and `F_GETPATH` resolve an older
pathname operation inside the platform layer. External directory renames can
race that resolution, and removed directories may be inaccessible. Descriptor
close-on-exec setup is non-atomic on older systems. Read-only tools retain
no-follow/type checks and internal file operations. Legacy monotonic time uses
Mach absolute time, which excludes system sleep; realtime uses gettimeofday.
Pre-10.7 terminal ownership uses a pthread key with real per-thread isolation.

`make -jN prod-macos-universal` uses independent slice prerequisites and
`llvm-lipo` to combine the executables and their dSYM DWARF payloads. The
per-slice code signatures remain intact: thinning the combined executable
must reproduce each original payload byte-for-byte. The combined dSYM must
retain both original UUIDs. This coalescing is packaging, not proof of runtime
compatibility, signing identity or notarization.

## Parallel production matrix

`make -jN prod-matrix` explicitly builds Linux x86-64/AArch64/i686 and legacy i686, macOS
ARM64/Intel/universal, Windows x86-64/ARM64, FreeBSD amd64/current and legacy,
and OpenBSD 7.9/5.9 amd64. This is the full implemented
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

The x86-64 recipe and internal i686 port link WinPTY 0.4.3 (MIT) console
collection into the same executable. Its first-party C interface
retains the agent's authenticated pipe handles, suspended/job-owned spawning
and explicit parent-death cleanup. The collector has its own hidden console;
no WinPTY DLL, launcher or auxiliary executable is installed. LLVM 21.1.7
libc++/libc++abi use static winpthreads, and the patched static libunwind uses
pthread locks plus VirtualQuery image lookup. These LLVM components retain
Apache-2.0 WITH LLVM-exception, including the GPLv2 compatibility provision.
Preserve the pinned upstream sources, patches and license notices with binary
distributions. The internal i686 port remains outside `PROD_TARGETS` pending
complete runtime qualification.

`nix/windows-legacy.nix` selects LLVM/msvcrt and the shared `nix/windows.nix`
recipe builds static x86-64 Windows Jansson, Mbed TLS, compression,
c-ares, HTTP/2 and GNU Unicode/IDN libraries using the same pinned nixpkgs
sources. It takes the `pkgs` exported by `nix/portable.nix`. The compile API and
PE subsystem baseline is NT 5.2 (XP x64/Server 2003); actual execution is in
Windows PE 10.0.26100.6584 from Microsoft's 25H2 evaluation media. The old-OS
runtime remains unverified. One x64 artifact selects modern native APIs where
available and uses the legacy adapters otherwise. `make prod-windows-x86_64` packages the
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
Windows ARM64, not an extra application DLL to install. Its UCRT recipe is
independent of the x64 msvcrt baseline. LLVM resource tools receive explicit Windows headers;
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

The x64 guest also ran existing base/SSE/JSON/wire/Responses/retry tests and
upstream Mbed TLS self-tests without third-party runtime DLLs. An upstream curl CLI
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
notices and corresponding source, including winpthreads' MIT/BSD notices
and LLVM's Apache-2.0 WITH LLVM-exception terms. Older Windows still needs
runtime qualification; a DLL import archive renamed to `.a` is never
a self-contained static dependency.

The Windows, early-FreeBSD and OpenBSD `regex` library attribute imports Gnulib's POSIX ERE module
at pinned revision `58df1afe785d3067cfa474ab57ccf283665dfa38` through
`nix/windows-regex.nix`. Only its LGPLv2-compatible module closure is compiled;
no third-party implementation is vendored and no external grep executable is
used. The small first-party charset adapter is GPL-2.0-only. Preserve both
sets of notices and the corresponding source/build recipe when redistributing.

The static engine handles UTF-8 internally, independently of msvcrt's locale
support: its charset, multibyte width, DFA fast path and Unicode character
classes consistently use UTF-8/Unicode. It does not change the process-global
CRT locale or require UCRT or a separately installed regex DLL. The FreeBSD
5.1-based and OpenBSD targets use the same UTF-8 engine with Gnulib multibyte-state and
encoding replacements. Other POSIX builds continue using libc regex.

`src/snag_jansson.h` is the only Jansson include surface in first-party C code. It
prefers a system `<jansson.h>` when one is available. Some minimal qualification
roots carry `libjansson.so.4` without the development header; for those roots the
wrapper falls back to `src/snag_jansson_abi.h`, a GPL-2.0-only first-party ABI
declaration shim. That shim contains declarations only, no parser, encoder,
allocator, object implementation, or upstream Jansson source, and it does not use
the name `src/jansson.h` so it cannot silently shadow a system development
header.

`src/http.h` is the shared libcurl include boundary. Provider transport and the
optional binary-update worker own separate curl handles and share only process
initialization and certificate-root setup.
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
evidence and an optional `make livecheck` harness, but neither local fixtures nor a successful cross-build prove live-provider
or target-platform behavior. Follow RELEASE.md: ship all implemented targets
with actual test scope and experimental qualifications. Optional bundle tools
do not replace that matrix or require unperformed runs to be called passing.
