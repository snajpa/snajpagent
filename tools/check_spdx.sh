#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
find src tests tools -type f \( -name '*.c' -o -name '*.h' -o -name '*.sh' \) -print |
while IFS= read -r f; do
    grep -q 'SPDX-License-Identifier: GPL-2.0-only' "$f" || {
        echo "missing GPL-2.0-only SPDX tag: $f" >&2
        exit 1
    }
done
