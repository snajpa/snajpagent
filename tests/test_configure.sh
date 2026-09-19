#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# test_configure.sh - option handling of the ./configure entry point.
#
# Two load-bearing properties:
#   1. an option the script does not know fails loudly (rc=2) and leaves the
#      tracked config.mk byte-identical, so a rejected invocation cannot turn a
#      hard failure into a silently lean artifact;
#   2. the GNU-standard packaging set a distro recipe passes is accepted, with
#      --mandir mapping onto MANPREFIX, while the all-off run still rewrites
#      exactly the four modality keys and stays idempotent.
# Skips cleanly when ./configure is absent, like tools/check_portability.py.

set -u

if [ ! -f ./configure ]; then
	printf '%s\n' 'test_configure: skipped (no ./configure in this tree)'
	exit 0
fi
if [ ! -f ./config.mk ]; then
	printf '%s\n' 'test_configure: FAIL config.mk missing'
	exit 1
fi

tmp=${TMPDIR:-/tmp}/snajpagent-configure-test.$$
mkdir -p "$tmp" || { printf '%s\n' 'test_configure: FAIL cannot make a temp dir'; exit 1; }
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cp ./configure "$tmp/configure" || exit 1
cp ./config.mk "$tmp/pristine.mk" || exit 1
chmod +x "$tmp/configure"

fails=0
fail() { printf 'test_configure: FAIL %s\n' "$1"; fails=$((fails + 1)); }
note() { printf 'test_configure: skip %s\n' "$1"; }

# check DESCRIPTION EXPECTED_RC unchanged|changed [options...]
check() {
	desc=$1
	want_rc=$2
	want_file=$3
	shift 3
	cp "$tmp/pristine.mk" "$tmp/config.mk"
	(cd "$tmp" && sh ./configure "$@" >"$tmp/out" 2>&1)
	rc=$?
	cmp -s "$tmp/pristine.mk" "$tmp/config.mk" && file=unchanged || file=changed
	[ "$rc" = "$want_rc" ] || fail "$desc: exit $rc, expected $want_rc"
	[ "$file" = "$want_file" ] || fail "$desc: config.mk $file, expected $want_file"
}

check 'unknown option is rejected' 2 unchanged --frobnicate
check 'unknown modality spelling is rejected' 2 unchanged --enable-pdf
check 'unsupported modality spelling is rejected' 2 unchanged --disable-av
check 'packaging set is accepted' 0 changed \
	--prefix=/usr --bindir=/usr/bin --sbindir=/usr/sbin --libdir=/usr/lib \
	--sysconfdir=/etc --mandir=/usr/share/man --build=x86_64-linux-gnu \
	--disable-dependency-tracking
grep -q '^MANPREFIX = /usr/share/man$' "$tmp/config.mk" ||
	fail '--mandir was not recorded as MANPREFIX'

triple=$(cc -dumpmachine 2>/dev/null) || triple=
if [ -n "$triple" ]; then
	check "native --host=$triple is accepted" 0 changed --host="$triple"
else
	note 'cc -dumpmachine unavailable, native --host case'
fi
check 'unknown triple is rejected' 1 unchanged --host=zzz-unknown-triple

check 'all-off run' 0 changed \
	--without-av --without-pdf --without-audio-device \
	--without-office --without-office-commands
keys=$(diff "$tmp/pristine.mk" "$tmp/config.mk" | grep -c '^[<>]')
[ "$keys" = 8 ] || fail "all-off run changed $keys lines, expected 8 (four WITH_* keys)"

cp "$tmp/config.mk" "$tmp/once.mk"
(cd "$tmp" && sh ./configure --without-av --without-pdf --without-audio-device \
	--without-office --without-office-commands >"$tmp/out2" 2>&1)
cmp -s "$tmp/once.mk" "$tmp/config.mk" || fail 'all-off run is not idempotent'

if [ "$fails" -ne 0 ]; then
	printf 'test_configure: %s check(s) failed\n' "$fails"
	exit 1
fi
printf '%s\n' 'test_configure: ok'
exit 0
