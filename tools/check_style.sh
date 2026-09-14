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

# Prompt-cache contract: a provider reuses a cached prefix only while the request carries the same
# cache key, so every provider request envelope must set it and take it from the shared derivation.
# Fails closed: a new request path cannot quietly ship without cache keys.
envelopes=$(grep -l '"store", 0, "stream", 1' src/*.c | sort | tr '\n' ' ')
test "$envelopes" = "src/app_compact.c src/context.c " || {
    echo "unexpected provider request envelope set: $envelopes" >&2
    exit 1
}
for file in src/app_compact.c src/context.c; do
    grep -q '"prompt_cache_key"' "$file" || {
        echo "$file builds a provider request envelope without prompt_cache_key" >&2
        exit 1
    }
done
grep -q 'snag_context_cache_key' src/context.h || {
    echo "src/context.h must declare the shared cache key derivation" >&2
    exit 1
}
grep -q 'snag_context_cache_key(' src/app_compact.c || {
    echo "the compaction request must take its key from the shared derivation" >&2
    exit 1
}

