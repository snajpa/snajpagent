<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Provider authentication and setup

`provider.c` remains the only libcurl include/transport boundary. The auth
module implements the device/refresh protocol through that bounded interface;
it does not introduce a second network library or bypass the dependency rules.

Authentication belongs to a named provider, independently of model selection.
`auth=api_key` reads the explicit `api_key` source, or a private stored key when
that setting is absent; `auth=chatgpt` reads and refreshes OAuth credentials for the
canonical HTTPS ChatGPT Codex endpoint. There is no implicit fallback between
sources, endpoints, accounts, or billing methods. OpenRouter uses its ordinary
Responses endpoint and hosted-search dialect, not Codex protocol behavior.

The program's `login`, `login status`, and `logout` commands run before the
agent/UI engine. First-run interactive startup invokes the same setup only
when default configuration and existing environment credentials are absent.
An explicit missing/invalid config is an error. Execution, piped task input,
resume, help, version, and listing never start setup. `--` preserves literal
prompts, and explicit key-stdin login cannot be confused with execution stdin.

Setup stages all user input and any tokens in memory before saving. Catalog
discovery is optional and explicit; failure permits a manually entered model.
It does not require other configured providers to authenticate. New providers
preserve the current default selection. Only declared providers exist in a
present config; config-free OpenAI key use has an ordinary `openai` preset.
First setup chooses the initial provider/model. Config replacement preserves
unrelated settings, is bounded and atomic, and checks for concurrent edits.

Managed credentials live in `DOTDIR/auth/PROVIDER.json`, with private user-owned regular
files and directories, no symlink following, bounded strict JSON, and exact
endpoint/method binding. Access tokens and API keys are limited to 16 KiB;
redaction uses the same bound. Stored-login credentials are not embedded in
config, prompt history, session events, or command arguments. Login input never reaches the
model. No Codex auth/cache file or keyring is borrowed.

Each provider has its own advisory lock. Refresh re-reads after locking and
reuses another process's newly rotated token. Credentials are reloaded for
requests; an expired token is refreshed before use and one pre-output HTTP 401
can force a refresh/retry. A second rejection is terminal, not a loop. Device
polling and lock/network waits are bounded and cancellable. Status is offline.
Logout removes the local managed provider credential only, not explicit config sources,
other providers, or all remote account sessions.

Configuration and credentials are individually atomic files, not a multi-file
crash-atomic transaction. Failed configuration installation rolls back the
new credential only if it still matches the owned write; newer concurrent
credentials are preserved. A crash between writes can leave an orphaned private
credential that can be reused or replaced on the next explicit login.

Native Codex login implements the public client's device HTTP flow independently:
request a code, poll, exchange authorization code and PKCE verifier, retain the
access/refresh token with account identity and expiry. Token claims are decoded
only from the trusted issuer exchange/private store, not accepted as untrusted
proof of identity. Refresh rejects an account mismatch. Auth response bodies
and tokens are never included in diagnostics.

Direct Codex requests use native endpoint paths and the account header. Create
requests keep `store=false`, streaming, existing developer instructions and
tools; an explicit root instructions field and encrypted reasoning inclusion
provide native request compatibility. Unsupported create parameters are omitted.
Output capacity still reserves input-budget headroom. Exact input-token
preflight is unavailable on this route; the existing auto/strict/off policy
handles that distinction. Generic/API-key transports are unchanged.

If direct Codex native compaction returns 404/405/501, close that attempt with
`compaction_interrupted` reason `endpoint_unavailable`, then perform one
Responses summary attempt using the same provider/account. Each attempt has
its own exact request hash. Do not guess alternate paths, fall back on auth
errors, or rewrite configuration. The standard hard-budget recount still runs.

Ordinary fake-agent tests explicitly bypass onboarding; focused login PTYs opt
into the real first-run decision. The existing transport fixture exercises
the real auth HTTP/file/refresh paths on loopback. Production builds cannot
override the fixed OAuth issuer using fixture environment variables.

## Explicit secret sources

`api_key = ${NAME}` reads an environment variable; `api_key = "literal"` decodes
a JSON-escaped literal without interpolation; other values name files. Relative
files bind to the active config directory, not the workspace. Only leading `~/`
expands. Source kind and expression survive config saving; saving never resolves
a file into a literal. A literal-bearing config must be user-owned and private.
Old `api_key_env`, `auth=env` and `secret_env` spellings are errors, not fallbacks.

Files are opened once, bounded, verified regular and read without FIFO blocking.
Explicit symlinks permit rotated/mounted secrets; this is a read-only trust path
and does not weaken the no-symlink managed credential store. One final LF/CRLF
is removed; no other credential bytes are trimmed. Generic protection values
support UTF-8; API keys retain printable-ASCII/no-whitespace header validation.

Repeatable `[tool] secret` entries share this parser/resolver. Protection snapshots
own their resolved values, retain earlier values while a request/command is still
using them, and scrub owned bytes on release. Missing required protection sources
fail before the operation; missing inactive-provider credentials do not force
unrelated providers to log in. Collection never refreshes OAuth or contacts a
provider. Environment references are filtered from command children; file/literal
sources are not exported as new variables.

Explicit `login NAME --with-api-key` switches that provider to managed storage
and removes its unused explicit source transactionally with config saving.
Ordinary login checks an existing explicit source instead of storing an ignored
replacement. External secret files are never rewritten, chmodded or deleted.
