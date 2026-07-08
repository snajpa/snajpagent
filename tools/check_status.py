#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check that IMPLEMENTATION_STATUS.md has internally consistent status math."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STATUS = ROOT / "IMPLEMENTATION_STATUS.md"


def extract_one(pattern: str, text: str, label: str) -> re.Match[str]:
    match = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    if not match:
        raise SystemExit(f"statuscheck: missing {label}")
    return match


def main() -> int:
    text = STATUS.read_text(encoding="utf-8")
    pct = extract_one(r"remaining: \*\*(\d+)%\*\*\. Approximate completed work:\s*\*\*(\d+)%\*\*", text, "percentage summary")
    remaining = int(pct.group(1))
    complete = int(pct.group(2))
    if remaining + complete != 100:
        raise SystemExit("statuscheck: remaining/completed percentages do not sum to 100")

    rows_match = extract_one(r"\| Area \| State \| Remaining estimate \|\n\|---\|---\|---:\|\n(.*?)\n\nThe next implementation work", text, "area table")
    area_total = 0
    for line in rows_match.group(1).splitlines():
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) == 3:
            m = re.fullmatch(r"(\d+)%", cells[2])
            if not m:
                raise SystemExit(f"statuscheck: malformed remaining estimate: {line}")
            area_total += int(m.group(1))
    if area_total != remaining:
        raise SystemExit(f"statuscheck: area estimates sum to {area_total}%, not {remaining}%")

    print(
        "statuscheck: ok "
        f"({complete}% complete/{remaining}% remaining)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
