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
#      exactly its four detected build keys and stays idempotent.
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

# check DESCRIPTION EXPECTED_RC unchanged|changed|either [options...]
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
	[ "$want_file" = either ] || [ "$file" = "$want_file" ] ||
		fail "$desc: config.mk $file, expected $want_file"
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
	# A native --host deliberately keeps the native compiler. Whether probes
	# rewrite config.mk depends on the shipped defaults and installed optional
	# dependencies, so acceptance—not an incidental byte change—is the contract.
	check "native --host=$triple is accepted" 0 either --host="$triple"
else
	note 'cc -dumpmachine unavailable, native --host case'
fi
check 'unknown triple is rejected' 1 unchanged --host=zzz-unknown-triple

check 'all-off run' 0 changed \
	--without-av --without-pdf --without-audio-device \
	--without-office --without-office-commands
keys=$(diff "$tmp/pristine.mk" "$tmp/config.mk" | grep -c '^[<>]')
[ "$keys" = 8 ] || fail "all-off run changed $keys lines, expected 8 (four detected keys)"

cp "$tmp/config.mk" "$tmp/once.mk"
(cd "$tmp" && sh ./configure --without-av --without-pdf --without-audio-device \
	--without-office --without-office-commands >"$tmp/out2" 2>&1)
cmp -s "$tmp/once.mk" "$tmp/config.mk" || fail 'all-off run is not idempotent'

# Cross builds select a target pkg-config explicitly.  Every query, including
# API-generation checks, must use it rather than an unrelated native command
# found through PATH.
mkdir "$tmp/native-bin"
cat >"$tmp/native-bin/pkg-config" <<'EOF'
#!/bin/sh
case "$1" in
	--atleast-version=26) exit 0 ;;
	*) exit 1 ;;
esac
EOF
cat >"$tmp/target-pkg-config" <<'EOF'
#!/bin/sh
case "$1" in
	--exists) exit 0 ;;
	--atleast-version=26) exit 1 ;;
	*) exit 1 ;;
esac
EOF
chmod +x "$tmp/native-bin/pkg-config" "$tmp/target-pkg-config"
cp "$tmp/pristine.mk" "$tmp/config.mk"
(cd "$tmp" && PATH="$tmp/native-bin:$PATH" PKG_CONFIG="$tmp/target-pkg-config" \
	sh ./configure --without-av --with-pdf --without-audio-device \
	--without-office --without-office-commands >"$tmp/out3" 2>&1)
rc=$?
[ "$rc" = 0 ] || fail "target pkg-config run exited $rc"
grep -Fqx 'HAVE_POPPLER_NEW_API ?= 0' "$tmp/config.mk" ||
	fail 'Poppler API detection did not use the selected target pkg-config'

if [ "$fails" -ne 0 ]; then
	printf 'test_configure: %s check(s) failed\n' "$fails"
	exit 1
fi
printf '%s\n' 'test_configure: ok'
exit 0
