<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Provider Model Cache And Selection

This document is the implementation contract for provider discovery and model
selection. The cache is only a convenience catalog. A model identifier supplied
by the user is trusted, is not checked against that catalog, and is passed to
the selected provider even when it is absent from the cache.

## Dotdir And Files

snajpagent has one private application directory, called the **dotdir**:

- the default is `$HOME/.snajpagent`;
- `-d DIR` (with `--dotdir DIR` as its long spelling) selects another absolute
  directory;
- the default configuration path is `DOTDIR/config.ini`;
- `-c FILE` (with `--config FILE` as its long spelling) selects a different
  configuration file without changing the dotdir;
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
them in `DOTDIR/models.json`, and then displays the new catalog. Discovery uses
the provider's authenticated `GET /v1/models` endpoint and accepts both common
OpenAI-compatible shapes:

- `data[].id`, including optional model metadata; and
- Codex-compatible `models[].slug`, including
  `supported_reasoning_levels[].effort` and `default_reasoning_level`.

Provider order, model order, and advertised reasoning-variant order are
preserved. Structural bounds, valid JSON/UTF-8, and required cache fields are
checked for safe storage, but model identifiers are not judged by name.
Refreshing is transactional across all configured providers: any discovery or
write failure preserves the previous cache rather than publishing a partial
replacement.

`/model` and its alias `/model list` read the persistent cache without
contacting a provider and do not refresh it merely because it is old. If no
cache exists yet, the first listing imports the bounded local Codex catalog
from `$CODEX_HOME/models_cache.json`, or from
`$HOME/.codex/models_cache.json` when `CODEX_HOME` is unset, and associates
that provider-neutral bootstrap catalog with the first configured provider.
If neither local cache exists, the command asks the user to run
`/model cache`. After a cache exists, the user alone decides that it is stale
and refreshes it with `/model cache`; there is no TTL or background refresh.
Typed selectors never trigger either bootstrap or provider discovery.

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

Slash is selector syntax. A provider-native model ID that itself contains a
slash is selected without ambiguity through its numbered cached row; the
complete native ID stored in that row is sent unchanged.

`/effort LEVEL` remains available as the direct way to change only the current
reasoning effort.
