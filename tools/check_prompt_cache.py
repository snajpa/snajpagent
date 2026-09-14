#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Guard for prompt-cache continuity across every provider request path.

A provider reuses a cached prefix only while the request carries the same prompt cache key and the
earlier part of that request stays byte-identical. The key is derived once, by
snag_context_cache_key(), and every request path has to set it: a path that builds its own envelope
without the key silently loses cache reuse for that request, which is how the compaction request
shipped keyless and how a cache regression becomes invisible again. This guard fails closed — a new
request-envelope construction site must be named here deliberately, and both known sites must set
the key.
"""

from __future__ import annotations

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
# The marker every provider request envelope carries, whatever else it is packed with.
ENVELOPE = '"store", 0, "stream", 1'
KNOWN_ENVELOPES = {"src/context.c", "src/app_compact.c"}
KEY = '"prompt_cache_key"'
DERIVATION = "snag_context_cache_key("


def die(message: str) -> None:
    raise SystemExit(f"cachecheck: {message}")


def require(condition: bool, message: str) -> None:
    if not condition:
        die(message)


def main() -> int:
    sites = {path.relative_to(ROOT).as_posix()
             for path in sorted((ROOT / "src").glob("*.c")) if ENVELOPE in path.read_text()}
    require(sites == KNOWN_ENVELOPES,
            f"request envelopes are built in {sorted(sites)}, expected {sorted(KNOWN_ENVELOPES)}; a new "
            "request path must set prompt_cache_key and be named in this guard")

    for name in sorted(KNOWN_ENVELOPES):
        require(KEY in (ROOT / name).read_text(),
                f"{name} builds a provider request envelope without {KEY}")

    context = (ROOT / "src/context.c").read_text()
    require(context.count(DERIVATION) >= 2,
            "snag_context_cache_key must be defined once and used by the ordinary request")
    require(DERIVATION in (ROOT / "src/context.h").read_text(),
            "src/context.h must declare snag_context_cache_key")
    require(DERIVATION in (ROOT / "src/app_compact.c").read_text(),
            "the compaction request path must derive its key from the shared helper")

    print(f"cachecheck: ok ({len(sites)} request envelopes, both set {KEY}, one derivation)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
