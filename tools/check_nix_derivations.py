#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Guard for the Nix application derivations' configure contract.

stdenv's default configurePhase auto-runs any ./configure it finds and passes autoconf
flags, and our script rejects the first unknown option with exit 2, so every application
derivation must set dontConfigure = true. None of them did before the configure landing,
which is why every Nix release build failed until the flag was added.
"""

from __future__ import annotations

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def die(message: str) -> None:
    raise SystemExit(f"nixcheck: {message}")


def require(condition: bool, message: str) -> None:
    if not condition:
        die(message)


def derivation_body(text: str, start: int) -> str | None:
    """Brace-matched body of the mkDerivation block following `start`."""
    opening = re.compile(r"mkDerivation\s*\{").search(text, start)
    if opening is None:
        return None
    begin = opening.end() - 1
    depth = 0
    for index in range(begin, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[begin : index + 1]
    return None


def check_derivations() -> None:
    """Every application derivation must keep stdenv's configure phase disabled."""
    sites = 0
    for path in sorted((ROOT / "nix").glob("*.nix")):
        text = path.read_text(encoding="utf-8")
        for match in re.finditer(r"application\s*=\s*\{", text):
            line = text[: match.start()].count("\n") + 1
            body = derivation_body(text, match.end())
            require(body is not None, f"{path.name}:{line} has no mkDerivation block")
            sites += 1
            require("dontConfigure" in body,
                    f"{path.name}:{line} leaves stdenv free to run ./configure,"
                    " which exits 2 on autoconf flags")
    require(sites > 0, "no application derivations found under nix/")
    print(f"nixcheck: ok ({sites} application derivations keep dontConfigure)")


def check_ppc32_ssp() -> None:
    """linux-ppc32 must keep its musl ssp alias and release-matrix membership."""
    overlay = ROOT / "nix" / "ppc32-ssp.nix"
    require(overlay.is_file(), "nix/ppc32-ssp.nix is missing")
    overlay_text = overlay.read_text(encoding="utf-8")
    require("__stack_chk_fail_local" in overlay_text,
            "nix/ppc32-ssp.nix must supply __stack_chk_fail_local")
    portable = (ROOT / "nix" / "portable.nix").read_text(encoding="utf-8")
    require("ppc32-ssp.nix" in portable,
            "nix/portable.nix must apply the ppc32-ssp overlay to linux-ppc32")
    makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
    require("prod-linux-ppc32" in makefile, "Makefile must list prod-linux-ppc32")
    prod = re.search(r"^PROD_TARGETS\s*=\s*(.*)$", makefile, re.MULTILINE)
    deferred = re.search(r"^DEFERRED_TARGETS\s*=\s*(.*)$", makefile, re.MULTILINE)
    require(prod is not None and "prod-linux-ppc32" in prod.group(1),
            "PROD_TARGETS must contain prod-linux-ppc32")
    require(deferred is None or "prod-linux-ppc32" not in deferred.group(1),
            "DEFERRED_TARGETS must not contain prod-linux-ppc32")
    release = (ROOT / "RELEASE.md").read_text(encoding="utf-8")
    row = next((line for line in release.splitlines() if "`linux-ppc32`" in line), "")
    require(row != "", "RELEASE.md must contain a linux-ppc32 row")
    require("not built" not in row, "RELEASE.md linux-ppc32 row must not say not built")
    print("nixcheck: ok (linux-ppc32 ssp alias + matrix membership)")


def main() -> int:
    check_derivations()
    check_ppc32_ssp()
    return 0


if __name__ == "__main__":
    sys.exit(main())
