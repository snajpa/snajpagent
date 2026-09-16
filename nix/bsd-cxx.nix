# SPDX-License-Identifier: GPL-2.0-only
{ pkgs, autotoolsLibrary, cflags, llvm, os, osVersion }:
(autotoolsLibrary pkgs.gcc14.cc [
  "--disable-multilib" "--disable-nls" "--disable-libstdcxx-pch" "--with-target-subdir=."
  "--disable-symvers" # No shared library or shared-only version-compatibility objects.
  "--enable-clocale=generic" "--enable-libstdcxx-threads"
  "--enable-libstdcxx-time=yes" "--disable-libstdcxx-backtrace"
  "--with-gxx-include-dir=${placeholder "out"}/include/c++"
] []).overrideAttrs (old: {
  pname = "libstdcxx-${os}-${osVersion}";
  sourceRoot = "gcc-${pkgs.gcc14.cc.version}/libstdc++-v3";
  # config-ml.in replays --disable-static but deliberately ignores a later
  # --enable-static. Do not let stdenv prepend the conflicting default.
  dontDisableStatic = true;
  postPatch = ''
    # Clang rejects the old FreeBSD shim's unconditional noexcept redeclarations
    # of functions already declared by libc. Keep native declarations when
    # visible and the existing dynamic fallback when strict headers hide them.
    substituteInPlace config/os/bsd/freebsd/os_defines.h \
      --replace-fail '#define _GLIBCXX_USE_C99_CHECK 1' '#define _GLIBCXX_USE_C99_CHECK 0' \
      --replace-fail '#define _GLIBCXX_USE_C99_LONG_LONG_CHECK 1' '#define _GLIBCXX_USE_C99_LONG_LONG_CHECK 0'
    # Make the mandated zero initialization explicit for Clang's constinit
    # check when configure selects the pthread-key exception-state path.
    substituteInPlace libsupc++/eh_globals.cc \
      --replace-fail '__constinit __cxa_eh_globals eh_globals;' \
                     '__constinit __cxa_eh_globals eh_globals = {};'
    # Match the public declaration's GNU attribute spelling under Clang.
    substituteInPlace src/c++11/assert_fail.cc \
      --replace-fail '[[__noreturn__]]' '__attribute__((__noreturn__))'
    substituteInPlace src/c++11/random.cc \
      --replace-fail '__builtin_ia32_rdseed_si_step' '__builtin_ia32_rdseed32_step'
    # Clang emits the bridge's DWARF with its ordinary assembly output; it
    # does not accept GCC's assembler-location capability override.
    substituteInPlace src/c++11/Makefile.am src/c++11/Makefile.in \
      --replace-fail ' -gno-as-loc-support' ""
    # Native FreeBSD headers already define the four wide-character
    # classification methods inline. Keep those native methods and the
    # generic locale's remaining transformation/initialization methods.
    substituteInPlace config/locale/generic/ctype_members.cc \
      --replace-fail '  bool
      ctype<wchar_t>::
      do_is(mask' '#ifndef __FreeBSD__
      bool
      ctype<wchar_t>::
      do_is(mask' \
      --replace-fail '  wchar_t
      ctype<wchar_t>::
      do_widen(char' '#endif
      wchar_t
      ctype<wchar_t>::
      do_widen(char'
  '' + pkgs.lib.optionalString (os == "netbsd" && pkgs.lib.versionOlder osVersion "6.0") ''
    # GCC's NetBSD ctype port expects the newer 16-bit table ABI. Use its
    # portable implementation over libc's classification functions instead
    # of reinterpreting the old SDK's 8-bit table or inventing mask aliases.
    for file in ctype_base.h ctype_inline.h ctype_configure_char.cc; do
      cp "config/os/generic/$file" "config/os/bsd/netbsd/$file"
    done
    # The C classification/conversion domain is unsigned char, including
    # bytes with the high bit set when this target's char is signed.
    substituteInPlace config/os/bsd/netbsd/ctype_inline.h \
      --replace-fail '(__c);' '(static_cast<unsigned char>(__c));'
    substituteInPlace config/os/bsd/netbsd/ctype_configure_char.cc \
      --replace-fail '((int) __c)' '((int)(unsigned char) __c)' \
      --replace-fail '((int) *__low)' '((int)(unsigned char) *__low)'
  '' + pkgs.lib.optionalString (os == "openbsd") ''
    # Older libtool assumes native ranlib's timestamp-only option. LLVM
    # rebuilds the deterministic archive index instead, as in the IDN recipe.
    substituteInPlace configure --replace-fail 'RANLIB -t' 'RANLIB'
    # OpenBSD 7.x renamed the ctype masks to _CTYPE_*; keep the historical
    # spellings compiling against either SDK generation.
    patch -p1 < ${./libstdcxx-openbsd-ctype-masks.patch}
  '' + pkgs.lib.optionalString (os == "openbsd" && pkgs.lib.versionOlder osVersion "4.0") ''
    # The SDK exposes only part of C99 stdio. Keep the upstream declarations,
    # using the compiler's native varargs type and matching visible prototypes.
    substituteInPlace include/c_global/cstdio \
      --replace-fail '__gnuc_va_list' '__builtin_va_list' \
      --replace-fail 'throw ()' ""
    patch -p1 < ${./libstdcxx-openbsd35-errors.patch}
  '' + pkgs.lib.optionalString ((os == "freebsd" && pkgs.lib.versionOlder osVersion "5.3")
    || (os == "netbsd" && pkgs.lib.versionOlder osVersion "6.0")
    || (os == "openbsd" && pkgs.lib.versionOlder osVersion "4.0")) ''
    # These SDK unwinders predate GetIPInfo. Configure otherwise assumes it
    # exists; select libstdc++'s existing GetIP fallback.
    substituteInPlace configure \
      --replace-fail 'if test x$have_unwind_getipinfo = xyes; then' \
        'have_unwind_getipinfo=no; if test x$have_unwind_getipinfo = xyes; then'
  '' + pkgs.lib.optionalString (os == "freebsd" && pkgs.lib.versionOlder osVersion "5.3") ''
    # The 5.1 unwinder also predates the combined resume/rethrow entry point.
    patch -p1 < ${./libstdcxx-freebsd51-unwind.patch}
  '';
  preConfigure = old.preConfigure + ''
    # A standalone libstdc++ build has no libgcc configure step to select
    # the target's gthread header. Supply the real POSIX implementation so
    # feature probes discover mutexes, condition variables and std::thread.
    ln -s gthr-posix.h ../libgcc/gthr-default.h
    # These SDKs hide long-long declarations in strict C++ even though the
    # compiler supports them. Expose the real functions to configure probes.
    export CXXFLAGS="${cflags} -fPIC -stdlib=libstdc++ -D__LONG_LONG_SUPPORTED=${pkgs.lib.optionalString ((os == "freebsd" && pkgs.lib.versionOlder osVersion "5.3") || (os == "openbsd" && pkgs.lib.versionOlder osVersion "4.0")) " -fno-use-cxa-atexit"}${pkgs.lib.optionalString (pkgs.lib.elem os [ "netbsd" "openbsd" ] && pkgs.lib.versionOlder osVersion "4.0") " -include ${./bsd-legacy-cxx.h}"}"
    export CFLAGS="${cflags} -fPIC"
    export LDFLAGS="--ld-path=${llvm.lld}/bin/ld.lld"
  '';
  doCheck = false; # Cross-built runtime; target execution is separate.
})
