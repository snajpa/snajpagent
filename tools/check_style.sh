#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

find src tests tools -type f \( -name '*.c' -o -name '*.h' -o -name '*.sh' \) -print |
while IFS= read -r file; do
    case "$file" in
    *.c|*.h)
        awk -v file="$file" '
            NR == 1 && $0 != "/* SPDX-License-Identifier: GPL-2.0-only */" {
                print file ":1: expected GPL-2.0-only SPDX header" > "/dev/stderr"
                bad = 1
            }
            index($0, "\r") {
                print file ":" NR ": carriage return" > "/dev/stderr"
                bad = 1
            }
            index($0, "\t") {
                print file ":" NR ": tab character" > "/dev/stderr"
                bad = 1
            }
            /[[:blank:]]$/ {
                print file ":" NR ": trailing whitespace" > "/dev/stderr"
                bad = 1
            }
            $0 == "" && blank {
                print file ":" NR ": repeated blank line" > "/dev/stderr"
                bad = 1
            }
            { blank = $0 == "" }
            END { exit bad }
        ' "$file" || exit 1
        test "$(tail -c 1 "$file" | wc -l | tr -d ' ')" = 1 || {
            echo "missing final newline: $file" >&2
            exit 1
        }
        ;;
    *.sh)
        grep -q 'SPDX-License-Identifier: GPL-2.0-only' "$file" || {
            echo "missing GPL-2.0-only SPDX tag: $file" >&2
            exit 1
        }
        ;;
    esac
done
printf '%s\n' 'stylecheck: ok'
