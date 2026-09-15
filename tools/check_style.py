#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Style checks for C and header files.

Default mode reports // comments under src, tests and tools; the scan tracks string, character
literal and block comment state, so a // sequence inside text or inside a block comment is not a
finding. `--changed <base>` adds the changed-lines rules: lines over the hard limit and missing
keyword spaces, measured on code text, over lines added since <base>.

Both modes exit non-zero when they report. `--changed` requires an explicit base revision and
fails clearly without one.
"""

import pathlib
import re
import subprocess
import sys

ROOTS = ("src", "tests", "tools")
SUFFIXES = (".c", ".h")
LIMIT = 100
KEYWORD = re.compile(r"\b(if|for|while|switch)\(")


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


def added_lines(base):
    """Return {path: {line_number: text}} for the added C/H lines since base."""
    try:
        diff = subprocess.run(
            ("git", "diff", "--no-color", "-U0", base, "--") + ROOTS,
            check=True, stdout=subprocess.PIPE).stdout.decode("utf-8", "surrogateescape")
    except FileNotFoundError:
        print("check_style.py: git is not available", file=sys.stderr)
        sys.exit(2)
    except subprocess.CalledProcessError as exc:
        print("check_style.py: cannot diff against base '%s' (git exit %d)" % (base, exc.returncode),
              file=sys.stderr)
        sys.exit(2)
    added = {}
    path = None
    number = 0
    for line in diff.split("\n"):
        if line.startswith("+++ "):
            name = line[4:].split("\t")[0]
            path = None if name == "/dev/null" else (name[2:] if name.startswith("b/") else name)
            continue
        if line.startswith("@@"):
            try:
                number = int(line.split("+")[1].split(",")[0].split(" ")[0])
            except (IndexError, ValueError):
                number = 0
            continue
        if path is None or not line:
            continue
        if line.startswith("+"):
            if number:
                added.setdefault(path, {})[number] = line[1:]
                number += 1
        elif line.startswith("-") or line.startswith("\\"):
            continue
        else:
            number += 1
    return added


def longest_token(line):
    return max((len(token) for token in line.split()), default=0)


def check_changed(base):
    bad = False
    for path, lines in sorted(added_lines(base).items()):
        if pathlib.Path(path).suffix not in SUFFIXES:
            continue
        file_path = pathlib.Path(path)
        if not file_path.is_file():
            continue
        current = {number: (segments, comment) for number, _text, segments, comment
                   in scan_file(file_path)}
        for number in sorted(lines):
            text = lines[number]
            if len(text) > LIMIT:
                token = longest_token(text)
                if not (token > LIMIT and len(text) - token <= LIMIT):
                    print("%s:%d: line is %d columns (hard limit %d)"
                          % (path, number, len(text), LIMIT), file=sys.stderr)
                    bad = True
            entry = current.get(number)
            if entry is None:
                continue
            segments, _comment = entry
            for start, end in segments:
                match = KEYWORD.search(text[start:end])
                if match:
                    print("%s:%d: keyword needs a space before '(': %s("
                          % (path, number, match.group(1)), file=sys.stderr)
                    bad = True
    return bad


def main():
    args = sys.argv[1:]
    if not args:
        return 1 if check_comments() else 0
    if args[0] == "--changed":
        if len(args) != 2 or not args[1]:
            print("check_style.py: --changed requires an explicit base revision", file=sys.stderr)
            return 2
        base = args[1]
        probe = subprocess.run(("git", "rev-parse", "--verify", "--quiet", base + "^{commit}"),
                               stdout=subprocess.DEVNULL)
        if probe.returncode != 0:
            print("check_style.py: cannot resolve base '%s'" % base, file=sys.stderr)
            return 2
        return 1 if check_changed(base) else 0
    print("usage: check_style.py [--changed <base>]", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
