<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent 0.9.0-wip

This source-only checkpoint is a clean implementation slice built from
frozen design 1.0.0. The rejected prototype is absent and has no authority.

The implemented core remains a persistent, multi-turn, normal-screen agent
process backed by one append-and-sync event log. A final answer completes a turn,
not the session. `snajpagent -r` reconstructs the session from disk without tmux,
a daemon, a socket, or other runtime state.

This checkpoint includes real production `exec_command` support for non-PTY
commands, immediate and yielded PTY commands, managed process handles,
bounded `write_stdin`, durable managed-process closure gating, process-family
shutdown hardening, bounded AGENTS instruction discovery, explicit resume-time
workspace relocation, local archive/delete lifecycle commands, exact delete-intent
completion, exact Responses input-token counting for real provider runs, durable
manual plus post-turn and pre-response automatic native compaction with
compact-window count metadata, bounded provider retry/rate-limit handling, durable provider-profile
response-start captures, live terminal resize/suspend handling, Linux terminal matrix checks, Linux/macOS PTY portability guards, clean-source
manifest/status/dependency/dependency-closure audit gates, local loopback provider-transport evidence,
release-evidence bundle/check/matrix tooling with path-confined evidence records, an explicit live-provider evidence harness, and the first production `apply_patch` implementation
with bounded diff previews. Tool work is still driven by the durable multi-cycle
turn engine: a provider tool call is approved or denied, fenced with
`tool_started` when it runs, captured or summarized under bounded canonical
results, committed as `tool_finished`, and only then fed into the next provider
cycle. Provider credentials and configured secrets are removed from the child
environment or redacted before any tool output is persisted or rendered.

The output ladder remains additive:

- no `-v`: assistant commentary, final answers, refusals, required orientation,
  safety/control acknowledgements, and errors;
- `-v`: public reasoning summaries and compact tool activity;
- `-vv`: complete bounded tool arguments and results;
- `-vvv`: model/profile/configuration, identifiers, timing, decisions, and usage;
- `-vvvv`: canonical durable-event type/sequence/sync records;
- `-vvvvv`: sanitized structured request/response JSON and SSE;
- `-vvvvvv`: sanitized request/response headers and transport diagnostics once
  transport is connected.

Level five now renders the canonical create-request body after the durable
`response_started` fence. Oversized bodies are represented by byte length and
SHA-256 digest. Secret classification/redaction from 0.6 remains irreversible;
verbosity cannot re-enable authorization material, cookies, proxy authorization,
configured secrets, encrypted reasoning, or secret-bearing JSON keys.

The normal binary now contains the bounded libcurl Responses transport,
exact Responses input-token counting for real provider runs, durable manual plus post-turn and pre-response automatic native compaction,
bounded Responses create/count/compact retry handling, durable provider-profile
response-start captures, live terminal resize/suspend handling, Linux terminal
matrix checks, Linux/macOS PTY portability guards, status/source-size/dependency/portability/dependency-closure audit gates, local provider-transport
checks, optional `make evidencebundle`/`make evidencecheck` release-evidence records,
`make evidencematrixcheck` for final multi-platform evidence aggregation, path-confined release-evidence record validation,
an optional `make livecheck` real-provider harness, non-PTY
`exec_command`, immediate and yielded PTY
`exec_command`, managed process handles, bounded `write_stdin`, strict durable
managed-process closure gating, process-family shutdown hardening, bounded AGENTS
instruction discovery, explicit `-r -C` workspace relocation, local `/archive`
and `/delete` lifecycle commands, exact completion of validated active or
post-rename delete intents, and a strict first-party `apply_patch`
parser/matcher/installer for add, update, and delete operations with bounded
model-visible diff previews. The `[provider] base_url` and `api_key_env`
settings select an OpenAI-compatible Responses provider and credential
environment variable. They default to `https://api.openai.com` and
`OPENAI_API_KEY`. The `[provider] auto_compact_input_tokens` setting controls
the post-turn and pre-response automatic compaction threshold; `0` disables it.
Providers that expose `/v1/responses` but not the auxiliary count or native
compact endpoints can set `[provider] exact_token_count = false` and
`native_compaction = false`; both default to `true`.
External live-provider evidence, macOS/architecture terminal reruns, and actual archived checked release-evidence bundles for each shipped platform remain later gates; the matrix verifier for those bundles is now included. The authoritative continuation state and
remaining-work percentage are in
`IMPLEMENTATION_STATUS.md`. The frozen `design/` subtree is kept as the original
design baseline; its historical "0%" wording is not the state of this
0.9.0-wip source archive.

Build requirements are a C11 compiler, POSIX.1-2008, make, system libcurl,
and system Jansson. This tarball intentionally vendors **no** third-party
implementation or upstream header source. `src/snj_jansson.h` prefers a system
`<jansson.h>` and falls back only to the inventoried first-party
`src/snj_jansson_abi.h` declaration shim when a build root has the Jansson
runtime library but not the development header. See `DEPENDENCIES.md`;
`make depscheck` enforces the no-vendored-dependency policy,
`make depclosurecheck` captures/validates the current-host executable closure,
and `make evidencebundle`/`make evidencecheck` create and verify path-confined release-evidence JSON bundles. `make evidencetoolcheck` self-tests the evidence validators. `make evidencematrixcheck` verifies the final set of per-platform bundles against the required release platform ids.

```sh
make
make check
make statuscheck
make depscheck
make portabilitycheck
make depclosurecheck
make evidencetoolcheck
make sizecheck
# Current-host evidence bundle, no external provider by default:
make evidencebundle
make evidencecheck
# Final multi-platform evidence aggregation, after external bundles exist:
make evidencematrixcheck RELEASE_EVIDENCE_DIRS="build/release-evidence/linux-x86_64 build/release-evidence/linux-aarch64 build/release-evidence/macos-x86_64 build/release-evidence/macos-arm64"
# Heavier local release gates:
make sanitizercheck
make releasecheck
# Requires the configured provider API key, network access, and provider quota.
# For an OpenAI-compatible proxy, set SNAJPAGENT_LIVE_BASE_URL and
# SNAJPAGENT_LIVE_API_KEY_ENV before running:
make livecheck
make releaseevidence
```

All first-party material is GPL-2.0-only. See `COPYING` and `LICENSE_SCOPE`.
The frozen design is retained byte-for-byte under `design/`.
