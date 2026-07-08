<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Dependency and vendoring inventory

This implementation checkpoint is intentionally **not** a vendored-dependency
bundle. The frozen design limits the first release to first-party source plus
platform/POSIX interfaces, system libcurl, and system Jansson. That policy is now
machine-checked by `make depscheck`; the built executable closure is captured and validated by `make depclosurecheck`, release evidence bundles are created/checked by `make evidencebundle` and `make evidencecheck`, the evidence validators are self-tested by `make evidencetoolcheck`, and the final multi-platform bundle set is checked by `make evidencematrixcheck`.

Vendored third-party implementation source: none.

Vendored third-party header source: none.

The linked/runtime dependencies for a normal provider-capable build are:

| Dependency | Source in this tarball | How it is used |
|---|---|---|
| POSIX/libc/platform terminal and process APIs | system | terminal, files, processes, signals, and memory allocation |
| system Jansson | not vendored | strict JSON parsing, construction, and canonical event/request encoding |
| system libcurl | not vendored | bounded OpenAI Responses create/count/compact HTTPS transport |
| libcurl backend closure | not vendored | TLS, resolver, compression, HTTP, and other backends selected by the system libcurl build |

`src/snj_jansson.h` is the only Jansson include surface in first-party C code. It
prefers a system `<jansson.h>` when one is available. Some minimal qualification
roots carry `libjansson.so.4` without the development header; for those roots the
wrapper falls back to `src/snj_jansson_abi.h`, a GPL-2.0-only first-party ABI
declaration shim. That shim contains declarations only, no parser, encoder,
allocator, object implementation, or upstream Jansson source, and it does not use
the name `src/jansson.h` so it cannot silently shadow a system development
header.

`src/provider.c` is the only first-party file allowed to include libcurl headers.
All other code reaches HTTP transport through the provider interface.

A release build still must archive the concrete executable dependency closure for each shipped platform:
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
consistent versioning, required platform coverage, terminal evidence, and live-provider evidence across the matrix. This
checkpoint includes local transport evidence and an optional `make livecheck`
harness, but external live-provider and advertised-platform closure evidence
remain release gates.
