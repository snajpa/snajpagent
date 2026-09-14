#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Guard for the Office state every release artifact takes.

Release artifacts run the commanded engine: they never link LibreOfficeKit and never
bundle a runtime. A target that inherits the linked state with no provider cannot even
compile src/office.c, which is what left several platforms broken before 49cac94e.
"""

from __future__ import annotations

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def die(message: str) -> None:
    raise SystemExit(f"officecheck: {message}")


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


def check_office_state() -> None:
    """Every application derivation must pass the commanded-Office make flags."""
    sites = 0
    for path in sorted((ROOT / "nix").glob("*.nix")):
        text = path.read_text(encoding="utf-8")
        for match in re.finditer(r"application\s*=\s*\{", text):
            line = text[: match.start()].count("\n") + 1
            body = derivation_body(text, match.end())
            require(body is not None, f"{path.name}:{line} has no mkDerivation block")
            sites += 1
            require("'WITH_OFFICE=1'" not in body,
                    f"{path.name}:{line} links the Office import; release artifacts"
                    " must take the commanded state")
            require("'WITH_OFFICE=0'" in body,
                    f"{path.name}:{line} must also force WITH_OFFICE=0, or config.mk's"
                    " default WITH_OFFICE=1 collides with the commanded state")
            require("'WITH_OFFICE_COMMANDS=1'" in body,
                    f"{path.name}:{line} does not select the commanded Office state")
    require(sites > 0, "no application derivations found under nix/")
    print(f"officecheck: ok ({sites} application derivations take the commanded state)")


def main() -> int:
    check_office_state()
    return 0


if __name__ == "__main__":
    sys.exit(main())
