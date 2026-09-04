<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Provider Model Cache And Selection

This document is the implementation contract for provider discovery and model
selection. The cache is only a convenience catalog. A model identifier supplied
by the user is trusted, is not checked against that catalog, and is passed to
the selected provider even when it is absent from the cache.

## Dotdir And Files

snajpagent has one private application directory, called the **dotdir**:

- the default is `$HOME/.snajpagent`;
- `--dotdir DIR` selects another absolute directory;
- the default configuration path is `DOTDIR/config.ini`;
- `--config FILE` selects a different configuration file without changing the
  dotdir;
- sessions and trash live below the dotdir, alongside the model cache; and
- the model cache is `DOTDIR/models.json`.

The dotdir and its state are private to the user. Cache replacement is atomic:
a failed refresh leaves the previous complete cache usable.

## Multiple Providers

Configuration may contain multiple named provider sections in source order:

```ini
[provider openai]
base_url = https://api.openai.com
api_key_env = OPENAI_API_KEY

[provider codex-lb]
base_url = http://127.0.0.1:2455/backend-api/codex
api_key_env = CODEX_LB_API_KEY
exact_token_count = false
native_compaction = false
```

The existing unnamed `[provider]` spelling remains valid and gives that entry
the name `default`. Provider names are unique. When a model selector does not
name a provider, the first provider loaded from the configuration is used. If
there is no provider section, the built-in OpenAI-compatible default is the
single first provider.

Provider selection is part of durable session preferences so a numbered model
choice continues to use the same provider after resume. Requests, token
counting, compaction, credentials, provider headers, and model discovery all
use that selected provider. Every configured provider credential environment
variable is removed from tool subprocess environments and treated as a
secret.

## Persistent Discovery

`/model cache` discovers models from every configured provider, records all of
them in `DOTDIR/models.json`, and then displays the new catalog. Discovery is
selected by the normalized configured API path, never the provider name:

- a path ending in the exact case-sensitive `/backend-api/codex` component
  sequence uses authenticated
  `GET <base>/models?client_version=0.146.0`; and
- every other path uses authenticated `GET <base>/v1/models`.

`0.146.0` is the dedicated Codex catalog compatibility version supported by
this decoder and its Codex response fixture, selected from the inspected Codex
0.146.0 catalog contract; it is not the snajpagent product version. The Codex
endpoint reads `models[].slug`, keeps only entries whose `visibility` is exactly `list`,
stably orders them by ascending numeric `priority`, and preserves advertised
`supported_reasoning_levels[].effort` order and `default_reasoning_level`.
Hidden, `none`, missing, and unknown visibility values do not become selectable.
Other providers read `data[].id`, including optional model metadata, in
response order. Structural bounds, valid JSON/UTF-8, and required cache fields
are checked for safe storage, but model identifiers are not judged by name.
Refreshing is transactional across all configured providers: any discovery or
write failure preserves the previous cache rather than publishing a partial
replacement. Both protocols share the provider's configured credentials,
headers, redaction, retries, timeouts, response bounds, and no-redirect policy.
A failed Codex endpoint never falls back to `/v1/models`.

`/model` and its alias `/model list` read the persistent cache without
contacting a provider and do not refresh it merely because it is old. If no
cache exists, the command asks the user to run `/model cache`. snajpagent owns
and consumes only `DOTDIR/models.json`; it does not inspect a Codex executable
or any external application's model cache. After a cache exists, the user alone
decides that it is stale and refreshes it with `/model cache`; there is no TTL
or background refresh. No unauthenticated public catalog is assumed:
authenticated provider discovery is the only online refresh path. Typed
selectors contact no provider and populate no cache.

The last line of every successful `/model`, `/model list`, and `/model cache`
catalog display reports when that cache was last updated. The timestamp is
stored in the cache rather than inferred from filesystem metadata.

## Display And Numbered Selection

The catalog begins with the selected provider, model, and reasoning effort,
then shows one flat numbered list of discovered provider/model/reasoning
variants. Models whose endpoint supplies no reasoning metadata still get one
selectable row using the configured/default reasoning effort. Provider names
are shown so duplicate model IDs from different providers remain distinct.

`/model NUMBER` selects the exact displayed row. `/model #NUMBER` is accepted
as an equivalent explicit-number spelling. Numbering is read from the same
cache file that was displayed, so the selected provider/model/effort triple is
durably saved together. An index outside the displayed cache is rejected, but
that index check does not constrain manually entered model identifiers.

Appending the separate final word `save`, or its one-letter spelling `s`, to
any valid numbered or typed selection also writes the selected provider, model,
and effort to the active configuration file. The active file is the explicit
`--config FILE` when present and otherwise `DOTDIR/config.ini`. The update
preserves every unrelated byte and the existing file mode, atomically replaces
the file, and creates a missing default file privately. A write failure leaves
the current session selection unchanged. A bare `/model save` or `/model s`
continues to mean the typed model ID `save` or `s`; the suffix is special only
when a nonempty selector precedes it.

## Typed Selector Grammar

Whitespace around the selector and around `/` separators is ignored. The
selector has these forms:

```text
MODEL
MODEL / EFFORT
PROVIDER / MODEL / EFFORT
```

- `MODEL` uses the first configured provider. If that model has advertised
  reasoning variants in the cache, snajpagent chooses the highest recognized
  thinking level; if none can be ranked, it chooses the provider's first
  advertised variant. If the typed model is not cached or has no advertised
  variants, the configured/default reasoning effort is retained.
- `MODEL / EFFORT` uses the first configured provider and the named thinking
  level.
- `PROVIDER / MODEL / EFFORT` uses the named configured provider and thinking
  level.

The three forms change the durable next-turn provider/model/effort preference.
Provider names must resolve because snajpagent needs routing and credentials;
model names are deliberately not checked against the cache. The provider API,
not snajpagent, decides whether a user-supplied model or effort is usable.
After the normal `model for next turn` confirmation, a typed selection whose
provider/model pair is not present in the readable cache prints a warning that
the model is unknown to the cache and will still be sent unchanged. The warning
does not reject or revert the selection, contact a provider, or validate the
model name. Numbered selections necessarily resolve through the cache and do
not print this warning.

Slash is selector syntax. A provider-native model ID that itself contains a
slash is selected without ambiguity through its numbered cached row; the
complete native ID stored in that row is sent unchanged.

`/effort LEVEL` remains available as the direct way to change only the current
reasoning effort.

Configuration may name `provider` alongside `model` and `reasoning_effort` in
`[agent]`. If absent, the first configured provider remains the default. The
named provider must exist, and it becomes the provider for a newly created
session. This is the representation written by `/model SELECTOR save`.
