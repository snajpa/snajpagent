#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Report // comments in C and header files.

The scan tracks string, character literal and block comment state, so a // sequence inside text,
or inside a block comment, is not a finding. Run from the repository root; `make stylecheck`
invokes it.
"""

import pathlib
import sys

ROOTS = ("src", "tests", "tools")
SUFFIXES = (".c", ".h")


def scan_line(line, state):
    """Split one line into code segments under a carried lexer state.

    Returns (segments, state, comment_column): segments are (start, end) offsets of code text;
    comment_column marks a // comment that opens in code context. A trailing backslash inside a
    string or character literal carries the state to the next line.
    """
    segments = []
    start = None
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if state == "code":
            if c == "/" and i + 1 < n and line[i + 1] == "/":
                if start is not None:
                    segments.append((start, i))
                return segments, state, i
            if c == "/" and i + 1 < n and line[i + 1] == "*":
                if start is not None:
                    segments.append((start, i))
                    start = None
                state = "block"
                i += 2
                continue
            if c == '"' or c == "'":
                if start is not None:
                    segments.append((start, i))
                    start = None
                state = "string" if c == '"' else "char"
                i += 1
                continue
            if start is None:
                start = i
        elif state == "block":
            if c == "*" and i + 1 < n and line[i + 1] == "/":
                state = "code"
                i += 2
                continue
        else:
            if c == "\\" and i + 1 < n:
                i += 2
                continue
            if (state == "string" and c == '"') or (state == "char" and c == "'"):
                state = "code"
        i += 1
    if start is not None:
        segments.append((start, n))
    if state in ("string", "char") and not line.endswith("\\"):
        state = "code"
    return segments, state, None


def scan_file(path):
    """Yield (line_number, text, segments, comment_column) for every line of the file."""
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    state = "code"
    for number, line in enumerate(text.split("\n"), 1):
        segments, state, comment = scan_line(line, state)
        yield number, line, segments, comment


def c_files():
    for root in ROOTS:
        base = pathlib.Path(root)
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix in SUFFIXES:
                yield path


def check_comments():
    bad = False
    for path in c_files():
        for number, _line, _segments, comment in scan_file(path):
            if comment is not None:
                print("%s:%d: // comment" % (path, number), file=sys.stderr)
                bad = True
    return bad


def main():
    if len(sys.argv) > 1:
        print("usage: check_style.py", file=sys.stderr)
        return 2
    return 1 if check_comments() else 0


if __name__ == "__main__":
    sys.exit(main())
