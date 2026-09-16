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


def check_riscv64_matrix() -> None:
    """riscv64 stays in PROD only with its pin fix present."""
    makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
    prod = re.search(r"^PROD_TARGETS = (.+)$", makefile, re.M)
    deferred = re.search(r"^DEFERRED_TARGETS = (.+)$", makefile, re.M)
    require(prod is not None, "Makefile lacks PROD_TARGETS")
    require(deferred is not None, "Makefile lacks DEFERRED_TARGETS")
    require("prod-linux-riscv64" in prod.group(1).split(),
            "prod-linux-riscv64 missing from PROD_TARGETS")
    require("prod-linux-riscv64" not in deferred.group(1).split(),
            "prod-linux-riscv64 still deferred")
    linux_nix = (ROOT / "nix" / "linux.nix").read_text(encoding="utf-8")
    require("simdcoverage" in linux_nix and "isRiscV" in linux_nix,
            "nix/linux.nix lacks riscv64 simdcoverage guard")
    require("libjpeg_turbo" in linux_nix and "staticFixed" in linux_nix,
            "nix/linux.nix lacks set-wide jpeg fix")
    print("nixcheck: ok (riscv64 in PROD with pin fix)")


def main() -> int:
    check_derivations()
    check_riscv64_matrix()
    return 0


if __name__ == "__main__":
    sys.exit(main())
