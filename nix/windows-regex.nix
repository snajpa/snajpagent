# SPDX-License-Identifier: GPL-2.0-only
{ pkgs, windows ? null, threads ? null, unistring, winver ? "0x0601", cross ? null }:
let
  nativeCharset = cross == null;
  stdenv = if nativeCharset then windows.stdenv else pkgs.stdenvNoCC;
  revision = "58df1afe785d3067cfa474ab57ccf283665dfa38";
  gnulib = pkgs.fetchzip {
    name = "gnulib-${revision}";
    urls = [
      "https://github.com/coreutils/gnulib/archive/${revision}.tar.gz"
      "https://codeload.github.com/coreutils/gnulib/tar.gz/${revision}"
    ];
    sha256 = "1n6mxibw59nx30jmz634qfpkrmgy9cnnjfs61m1kkmv5g08pvvpj";
  };
  configure = pkgs.writeText "configure.ac" ''
    AC_INIT([snajpagent-regex], [20260905])
    AC_CONFIG_SRCDIR([Makefile.am])
    AC_CONFIG_AUX_DIR([build-aux])
    AC_CONFIG_MACRO_DIRS([m4])
    AM_INIT_AUTOMAKE([foreign subdir-objects])
    AC_PROG_CC
    gl_EARLY
    AC_PROG_RANLIB
    AM_PROG_AR
    gl_INIT
    REPLACE_MB_CUR_MAX=4
    AC_DEFINE([locale_charset], [snag_regex_charset], [Private UTF-8 regex charset.])
    AC_CONFIG_HEADERS([config.h])
    AC_CONFIG_FILES([Makefile lib/Makefile])
    AC_OUTPUT
  '';
  makefile = pkgs.writeText "Makefile.am" ''
    SUBDIRS = lib
    ACLOCAL_AMFLAGS = -I m4
  '';
  publicHeader = pkgs.writeText "regex.h" ''
    /* Match the standalone Gnulib library ABI, without leaking config.h. */
    #ifndef SNAJPAGENT_GNULIB_REGEX_H
    #define SNAJPAGENT_GNULIB_REGEX_H
    #include <stddef.h>
    #define _REGEX_LARGE_OFFSETS 1
    #define regcomp rpl_regcomp
    #define regexec rpl_regexec
    #define regerror rpl_regerror
    #define regfree rpl_regfree
    #include "snajpagent-gnulib-regex.h"
    #endif
  '';
  charset = pkgs.writeText "localcharset.c" ''
    /* SPDX-License-Identifier: GPL-2.0-only */
    #include <config.h>
    #include "localcharset.h"
    const char *locale_charset(void) { return "UTF-8"; }
  '';
in stdenv.mkDerivation {
  pname = "snajpagent-regex-${if nativeCharset then "windows" else cross.target}-static";
  version = "20260905";
  dontUnpack = true;
  strictDeps = true;
  configurePlatforms = [];
  dontStrip = true;
  nativeBuildInputs = [ pkgs.autoconf pkgs.automake pkgs.python3 pkgs.perl
                        pkgs.gettext (if nativeCharset then windows.buildPackages.pkg-config else pkgs.pkg-config) ];
  buildInputs = [ unistring ] ++ pkgs.lib.optional (threads != null) threads;
  env.CFLAGS = if !nativeCharset then cross.cflags else "-Os -g -D_WIN32_WINNT=${winver} -DWINVER=${winver}"
    + pkgs.lib.optionalString windows.stdenv.cc.isClang " -pthread";
  preConfigure = pkgs.lib.optionalString (!nativeCharset) ''
    export CC="${cross.compiler} --target=${cross.target} --sysroot=${cross.sdk}"
    export CXX="${cross.cxxCompiler} --target=${cross.target} --sysroot=${cross.sdk}"
    export AR=${cross.tools}/llvm-ar RANLIB=${cross.tools}/llvm-ranlib
    export NM=${cross.tools}/llvm-nm STRIP=${cross.tools}/llvm-strip
    export LDFLAGS='${cross.ldflags}'
    # Use Gnulib's complete UTF-8 state/conversion replacements. Old BSD's
    # libc multibyte and regex implementations require missing locale data.
    export gl_cv_func_mbrtowc_incomplete_state=no
    export gl_cv_func_mbrtowc_sanitycheck=no
  '' + ''
    cp ${configure} configure.ac
    cp ${makefile} Makefile.am
    chmod u+w configure.ac Makefile.am
    bash ${gnulib}/gnulib-tool --import --lgpl=2 --no-vc-files \
      --lib=libsnagregex --source-base=lib --m4-base=m4 regex
    cp ${charset} lib/localcharset.c
    substituteInPlace lib/regcomp.c \
      --replace-fail 'codeset_name = nl_langinfo (CODESET);' 'codeset_name = "UTF-8";'
    substituteInPlace lib/c32is-impl.h \
      --replace-fail 'if (wc == WEOF || wc == (wchar_t) wc)' 'if (wc == WEOF)'
    autoreconf -fiv
  '';
  configureFlags = [ "--disable-nls" "--disable-dependency-tracking"
                     "--with-libunistring-prefix=${unistring}" ]
    ++ pkgs.lib.optionals (!nativeCharset) [
      "--build=${pkgs.stdenv.buildPlatform.config}" "--host=${cross.target}"
      "--disable-shared" "--enable-static" "--with-pic"
    ];
  enableParallelBuilding = true;
  installPhase = ''
    runHook preInstall
    mkdir -p "$out/lib" "$out/include" "$out/share/licenses/snajpagent-regex"
    cp lib/libsnagregex.a "$out/lib/"
    cp lib/regex.h "$out/include/snajpagent-gnulib-regex.h"
    cp ${publicHeader} "$out/include/regex.h"
    cp ${gnulib}/doc/COPYING.LESSERv2 "$out/share/licenses/snajpagent-regex/"
    runHook postInstall
  '';
  meta.license = with pkgs.lib.licenses; [ lgpl21Plus gpl2Only ];
}
