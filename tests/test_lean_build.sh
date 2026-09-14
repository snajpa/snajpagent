#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# test_lean_build.sh - the modality-off combinations must keep compiling.
#
# `make check` builds only the default full profile, so a -Werror break in an
# off combination reaches the shipping line unnoticed (src/office.c did, at the
# all-off pair). This compiles every unit carrying a modality guard in the two
# off states the default build never exercises, using the project's own flags
# taken from `make -n`, and writes the objects into a temporary directory so the
# tree and its build products are left alone.

set -u

[ -f Makefile ] || { printf '%s\n' 'test_lean_build: skipped (no Makefile)'; exit 0; }
command -v make >/dev/null 2>&1 || { printf '%s\n' 'test_lean_build: skipped (no make)'; exit 0; }

units=$(grep -l -E 'SNAJPAGENT_(AV|PDF|AUDIO_DEVICE|OFFICE)' src/*.c 2>/dev/null |
	sed 's#^src/##; s#\.c$##' | sort)
[ -n "$units" ] || { printf '%s\n' 'test_lean_build: skipped (no modality-guarded units)'; exit 0; }

tmp=${TMPDIR:-/tmp}/snajpagent-lean-build.$$
mkdir -p "$tmp" || { printf '%s\n' 'test_lean_build: FAIL cannot create a temp dir'; exit 1; }
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fails=0
ok=0

compile_state() {
	desc=$1
	shift
	for u in $units; do
		cmd=$(make -n "$@" "src/$u.o" 2>/dev/null | grep -m1 -- " -c src/$u.c")
		if [ -z "$cmd" ]; then
			printf 'test_lean_build: FAIL %s: no compile rule for src/%s.o\n' "$desc" "$u"
			fails=$((fails + 1))
			continue
		fi
		run=$(printf '%s' "$cmd" | sed "s#-o src/$u.o#-o $tmp/$u.o#")
		if sh -c "$run" >"$tmp/$u.log" 2>&1; then
			ok=$((ok + 1))
		else
			printf 'test_lean_build: FAIL %s: src/%s.c: %s\n' "$desc" "$u" \
				"$(grep -m1 'error:' "$tmp/$u.log" | sed 's/.*error: //')"
			fails=$((fails + 1))
		fi
	done
}

compile_state 'all modalities off' \
	WITH_AV=0 WITH_PDF=0 WITH_AUDIO_DEVICE=0 WITH_OFFICE=0 WITH_OFFICE_COMMANDS=0
compile_state 'commanded Office, others off' \
	WITH_AV=0 WITH_PDF=0 WITH_AUDIO_DEVICE=0 WITH_OFFICE=0 WITH_OFFICE_COMMANDS=1

if [ "$fails" -ne 0 ]; then
	printf 'test_lean_build: %s of %s compiles failed\n' "$fails" "$((fails + ok))"
	exit 1
fi
printf 'test_lean_build: ok (%s units x 2 off states compiled)\n' "$ok"
exit 0
