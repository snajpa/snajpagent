# SPDX-License-Identifier: GPL-2.0-only
{ pkgs, windows ? pkgs.pkgsCross.mingwW64, winver ? "0x0601" }:
let
  legacy = builtins.elem winver [ "0x0500" "0x0501" "0x0502" ];
  arch = if windows.stdenv.hostPlatform.isAarch64 then "arm64"
    else if windows.stdenv.hostPlatform.isx86_32 then "i686" else "x86_64";
  pthreadBase = windows.windows.pthreads.overrideAttrs (old:
    pkgs.lib.optionalAttrs (winver != "0x0601") {
      env = (old.env or { }) // {
        CFLAGS = "-Os -g -D_WIN32_WINNT=${winver} -DWINVER=${winver}";
      };
    });
  pthreads = if windows.stdenv.cc.isClang then pthreadBase.overrideAttrs (old: {
    makeFlags = (old.makeFlags or []) ++ [
      "RCFLAGS=-I${windows.windows.mingw_w64_headers}/include"
    ];
  }) else pthreadBase;
  threads = if windows.stdenv.cc.isClang then pthreads else windows.windows.mcfgthreads.overrideAttrs (old: {
    pname = "mcfgthread-static";
    mesonFlags = (old.mesonFlags or []) ++ [ "-Ddefault_library=static" ];
  });
  cmakeLibrary = package: flags: dependencies:
    windows.stdenv.mkDerivation {
      pname = "${package.pname}-windows-${arch}-static";
      inherit (package) version src;
      patches = package.patches or [];
      nativeBuildInputs = [ pkgs.cmake pkgs.ninja windows.buildPackages.pkg-config
                           pkgs.perl pkgs.python3 ];
      buildInputs = dependencies ++ [ threads ];
      strictDeps = true;
      enableParallelBuilding = true;
      cmakeBuildType = "MinSizeRel";
      preConfigure = ''
        cmakeFlagsArray+=("-DCMAKE_C_FLAGS=-D_WIN32_WINNT=${winver} -DWINVER=${winver}")
      '';
      cmakeFlags = [
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.10"
        "-DCMAKE_INSTALL_LIBDIR=lib"
        "-DCMAKE_INSTALL_INCLUDEDIR=include"
        "-DCMAKE_INSTALL_BINDIR=bin"
        "-DBUILD_SHARED_LIBS=OFF"
        "-DBUILD_TESTING=OFF"
        "-DCMAKE_EXE_LINKER_FLAGS=-static"
      ] ++ flags;
    };
  autotoolsLibrary = package: flags: dependencies:
    windows.stdenv.mkDerivation ({
      pname = "${package.pname}-windows-${arch}-static";
      inherit (package) version src;
      patches = package.patches or [];
      nativeBuildInputs = [ windows.buildPackages.pkg-config pkgs.perl pkgs.texinfo ];
      buildInputs = dependencies ++ [ threads ];
      strictDeps = true;
      enableParallelBuilding = true;
      env.CFLAGS = "-Os -g -D_WIN32_WINNT=${winver} -DWINVER=${winver}"
        + pkgs.lib.optionalString windows.stdenv.cc.isClang " -pthread";
      configureFlags = [ "--disable-shared" "--enable-static"
                         "--disable-dependency-tracking" ] ++ flags;
    } // pkgs.lib.optionalAttrs windows.stdenv.cc.isClang {
      preBuild = ''
        for variable in RC WINDRES; do
          makeFlagsArray+=("$variable=${windows.stdenv.cc.targetPrefix}windres -I${windows.windows.mingw_w64_headers}/include")
        done
      '';
    });
  voiceRtc = import ./voice-rtc-cross.nix {
    inherit pkgs cmakeLibrary tls; sourcePkgs = windows;
    # mingw-aarch64 has no RTCD arm; presume NEON (mandatory on aarch64).
    opusFlags = pkgs.lib.optional windows.stdenv.hostPlatform.isAarch64 "-DOPUS_MAY_HAVE_NEON=OFF";
    # The presumed path needs the may-have prototypes/includes; RTCD stays out.
    opusCflags = pkgs.lib.optionalString windows.stdenv.hostPlatform.isAarch64 "-DOPUS_ARM_MAY_HAVE_NEON -DOPUS_ARM_MAY_HAVE_NEON_INTR";
    # usrsctp.h's _WIN32 fallback (no _MSC_VER) defines uint{8,16,32,64}_t
    # macros; have it include "stdint.h" instead so libc++ sees real types.
    # The escaped quotes survive CMake's cache and the ninja build lines that
    # /bin/sh runs, so the compiler sees -DSCTP_STDINT_INCLUDE="stdint.h".
    rtcFlags = [ "-DCMAKE_CXX_FLAGS=${"-DJUICE_STATIC "}-DSCTP_STDINT_INCLUDE=\\\"stdint.h\\\"${pkgs.lib.optionalString (pty != null) " -D_WIN32_WINNT=${winver} -DWINVER=${winver} -nostdinc++ -isystem ${pkgs.lib.getDev pty.cxx}/include/c++/v1"}" ];
    rtcPatches = pkgs.lib.optional legacy ./libdatachannel-legacy-ai-flags.patch;
  };
  jansson = cmakeLibrary windows.jansson [
    "-DJANSSON_BUILD_SHARED_LIBS=OFF"
    "-DJANSSON_BUILD_DOCS=OFF"
    "-DJANSSON_WITHOUT_TESTS=ON"
    "-DJANSSON_EXAMPLES=OFF"
  ] [];
  tls = (cmakeLibrary windows.mbedtls [
    "-DENABLE_PROGRAMS=OFF"
    "-DENABLE_TESTING=OFF"
    "-DUSE_SHARED_MBEDTLS_LIBRARY=OFF"
    "-DUSE_STATIC_MBEDTLS_LIBRARY=ON"
    "-DGEN_FILES=OFF"
  ] [ pthreads ]).overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ pkgs.lib.optional legacy ./mbedtls-legacy-entropy.patch;
    postPatch = ''
      perl scripts/config.pl set MBEDTLS_THREADING_C
      perl scripts/config.pl set MBEDTLS_THREADING_PTHREAD
      # libdatachannel uses the DTLS-SRTP API; enable it (PROTO_DTLS is on).
      perl scripts/config.pl set MBEDTLS_SSL_DTLS_SRTP
    '' + pkgs.lib.optionalString legacy ''
      substituteInPlace library/CMakeLists.txt \
        --replace-fail 'ws2_32 bcrypt' 'ws2_32 advapi32'
      substituteInPlace include/mbedtls/debug.h \
        --replace-fail '__format__(gnu_printf,' '__format__(printf,'
    '';
    postInstall = ''
      printf '\nLibs.private: -L${pthreads}/lib -lpthread -l${if legacy then "advapi32" else "bcrypt"}\n' \
        >> "$out/lib/pkgconfig/mbedcrypto.pc"
    '';
  });
  zlib = (cmakeLibrary windows.zlib [
    "-DZLIB_BUILD_SHARED=OFF" "-DZLIB_BUILD_STATIC=ON" "-DZLIB_BUILD_TESTING=OFF"
  ] []).overrideAttrs (_: {
    postInstall = ''
      sed -i 's/ -lz$/ -lzs/' "$out/lib/pkgconfig/zlib.pc"
    '';
  });
  png = (cmakeLibrary windows.libpng [
    "-DPNG_SHARED=OFF" "-DPNG_STATIC=ON" "-DPNG_TESTS=OFF" "-DPNG_TOOLS=OFF"
    "-DZLIB_LIBRARY=${zlib}/lib/libzs.a"
  ] [ zlib ]).overrideAttrs (_: {
    postInstall = ''
      # Requires.private already names zlib, whose Windows archive is libzs.a.
      sed -i '/^Libs.private:/ s/ -lz\( \|$\)/\1/g' "$out/lib/pkgconfig/"*.pc
    '';
  });
  freetype = cmakeLibrary windows.freetype [
    "-DFT_DISABLE_BZIP2=ON" "-DFT_DISABLE_BROTLI=ON" "-DFT_DISABLE_HARFBUZZ=ON"
    "-DFT_REQUIRE_ZLIB=ON" "-DFT_REQUIRE_PNG=ON"
    "-DZLIB_LIBRARY=${zlib}/lib/libzs.a"
  ] [ zlib png ];
  jpeg = (cmakeLibrary windows.libjpeg [
    "-DENABLE_SHARED=OFF" "-DENABLE_STATIC=ON" "-DWITH_TURBOJPEG=OFF"
  ] []).overrideAttrs (old: {
    patches = builtins.filter (patch: builtins.baseNameOf patch != "mingw-boolean.patch")
      old.patches ++ [ ./jpeg-mingw-boolean.patch ];
    nativeBuildInputs = old.nativeBuildInputs ++ pkgs.lib.optional windows.stdenv.hostPlatform.isx86 pkgs.nasm;
  });
  openjpeg = (cmakeLibrary windows.openjpeg [ "-DBUILD_CODEC=OFF" ] []).overrideAttrs (_: {
    postInstall = ''
      substituteInPlace "$out/lib/pkgconfig/libopenjp2.pc" \
        --replace-fail '-l-lpthread' '-lpthread'
    '';
  });
  pdf = (cmakeLibrary windows.poppler [
    "-DENABLE_UNSTABLE_API_ABI_HEADERS=ON" "-DFONT_CONFIGURATION=win32"
    "-DENABLE_UTILS=OFF" "-DENABLE_CPP=OFF" "-DENABLE_GLIB=OFF"
    "-DENABLE_GOBJECT_INTROSPECTION=OFF" "-DENABLE_QT5=OFF" "-DENABLE_QT6=OFF"
    "-DBUILD_QT5_TESTS=OFF" "-DBUILD_QT6_TESTS=OFF" "-DBUILD_CPP_TESTS=OFF"
    "-DBUILD_MANUAL_TESTS=OFF" "-DENABLE_LCMS=OFF" "-DENABLE_LIBCURL=OFF"
    "-DENABLE_LIBTIFF=OFF" "-DENABLE_NSS3=OFF" "-DENABLE_GPGME=OFF"
    "-DZLIB_LIBRARY=${zlib}/lib/libzs.a"
  ] [ zlib png freetype jpeg openjpeg pkgs.boost ]).overrideAttrs (old: {
    cmakeBuildType = "Release";
    patches = old.patches ++ pkgs.lib.optional legacy ./poppler-ascii-metadata.patch;
    buildInputs = old.buildInputs ++ pkgs.lib.optionals (pty != null) [ pty.cxx pty.unwind ];
    preConfigure = old.preConfigure + ''
      cmakeFlagsArray+=(
        "-DCMAKE_CXX_FLAGS=-D_WIN32_WINNT=${winver} -DWINVER=${winver}${pkgs.lib.optionalString (pty != null) " -nostdinc++ -isystem ${pkgs.lib.getDev pty.cxx}/include/c++/v1"}"
      )
    '';
  });
  av = (windows.ffmpeg_8.override {
    inherit zlib;
    ffmpegVariant = "headless";
    withHeadlessDeps = false;
    withSmallDeps = false;
    withFullDeps = false;
    withGPL = false;
    withVersion3 = false;
    withZlib = true;
    withSafeBitstreamReader = true;
    withPixelutils = true;
    withNetwork = false;
    withStatic = true;
    withShared = false;
    buildFfmpeg = false;
    buildFfplay = false;
    buildFfprobe = false;
    buildAvcodec = true;
    buildAvformat = true;
    buildAvutil = true;
    buildSwresample = true;
    buildSwscale = true;
    withDocumentation = false;
  }).overrideAttrs (old: {
    # Qualify this static-only profile separately from nixpkgs' MinGW64 marker.
    meta = old.meta // { broken = false; };
    patches = (old.patches or []) ++ pkgs.lib.optional legacy ./ffmpeg-legacy-windows.patch;
    propagatedBuildInputs = (old.propagatedBuildInputs or []) ++ [ pthreads ];
    configureFlags = old.configureFlags ++ [
      "--disable-autodetect" "--disable-w32threads" "--enable-pthreads"
    ];
    env = (old.env or {}) // { CFLAGS = "-Os -D_WIN32_WINNT=${winver} -DWINVER=${winver}"; };
  });
  brotli = cmakeLibrary windows.brotli [ "-DBROTLI_DISABLE_TESTS=ON" ] [];
  zstd = (cmakeLibrary windows.zstd [
    "-DZSTD_BUILD_SHARED=OFF" "-DZSTD_BUILD_STATIC=ON"
    "-DZSTD_BUILD_PROGRAMS=OFF" "-DZSTD_BUILD_TESTS=OFF"
  ] []).overrideAttrs (_: { cmakeDir = "../build/cmake"; });
  cares = (cmakeLibrary windows.c-ares ([
    "-DCARES_SHARED=OFF" "-DCARES_STATIC=ON"
    "-DCARES_BUILD_TOOLS=OFF" "-DCARES_BUILD_TESTS=OFF"
  ] ++ pkgs.lib.optional legacy "-DCARES_THREADS=OFF") []).overrideAttrs (old: {
    patches = (old.patches or []) ++ pkgs.lib.optional legacy ./cares-legacy-windows.patch;
  });
  nghttp2 = (cmakeLibrary windows.nghttp2 [
    "-DENABLE_LIB_ONLY=ON" "-DBUILD_STATIC_LIBS=ON" "-DENABLE_DOC=OFF"
  ] []).overrideAttrs (_: {
    postInstall = ''
      substituteInPlace "$out/lib/pkgconfig/libnghttp2.pc" \
        --replace-fail 'Cflags: ' 'Cflags: -DNGHTTP2_STATICLIB '
    '';
  });
  iconv = autotoolsLibrary windows.libiconvReal [] [];
  xml = (cmakeLibrary windows.libxml2 [
    "-DLIBXML2_WITH_PROGRAMS=OFF" "-DLIBXML2_WITH_TESTS=OFF"
    "-DLIBXML2_WITH_PYTHON=OFF" "-DLIBXML2_WITH_MODULES=OFF"
    "-DLIBXML2_WITH_ICONV=ON"
  ] [ iconv ]).overrideAttrs (old: {
    patches = old.patches ++ pkgs.lib.optional legacy ./libxml2-legacy-windows.patch;
  });
  archive = (cmakeLibrary windows.libarchive [
    "-DENABLE_TAR=OFF" "-DENABLE_CPIO=OFF" "-DENABLE_CAT=OFF"
    "-DENABLE_UNZIP=OFF" "-DENABLE_TEST=OFF" "-DENABLE_INSTALL=ON"
    "-DENABLE_OPENSSL=OFF" "-DENABLE_MBEDTLS=OFF" "-DENABLE_NETTLE=OFF"
    "-DENABLE_CNG=OFF" "-DENABLE_LIBB2=OFF" "-DENABLE_LZ4=OFF"
    "-DENABLE_LZO=OFF" "-DENABLE_LZMA=OFF" "-DENABLE_ZSTD=OFF"
    "-DENABLE_BZip2=OFF" "-DENABLE_LIBXML2=OFF" "-DENABLE_EXPAT=OFF"
    "-DENABLE_WIN32_XMLLITE=OFF" "-DENABLE_PCREPOSIX=OFF"
    "-DENABLE_PCRE2POSIX=OFF" "-DENABLE_ZLIB=ON" "-DENABLE_ICONV=ON"
    "-DWINDOWS_VERSION=${if legacy then "WS03" else "WIN7"}"
    "-DZLIB_LIBRARY=${zlib}/lib/libzs.a"
  ] [ zlib iconv ]).overrideAttrs (_: {
    postInstall = ''
      # This profile installs only a static archive, including for plain --cflags.
      sed -i 's/^Cflags: /Cflags: -DLIBARCHIVE_STATIC /' "$out/lib/pkgconfig/libarchive.pc"
    '';
  });
  unistring = autotoolsLibrary windows.libunistring
    [ "--with-libiconv-prefix=${iconv}" ] [ iconv ];
  idn2 = autotoolsLibrary windows.libidn2 [
    "--disable-doc" "--with-libiconv-prefix=${iconv}"
    "--with-libunistring-prefix=${unistring}"
  ] [ iconv unistring ];
  networkLibraries = [ tls pthreads zlib brotli zstd cares nghttp2
                       iconv unistring idn2 ];
  curl = (cmakeLibrary windows.curlMinimal [
    "-DBUILD_STATIC_LIBS=ON" "-DBUILD_CURL_EXE=OFF" "-DCURL_BUILD_EVERYTHING=OFF"
    "-DCURL_USE_MBEDTLS=ON" "-DCURL_USE_OPENSSL=OFF" "-DCURL_USE_SCHANNEL=OFF"
    "-DCURL_DEFAULT_SSL_BACKEND=mbedtls" "-DENABLE_ARES=ON"
    "-DCURL_USE_PKGCONFIG=ON" "-DPKG_CONFIG_ARGN=--static"
    "-DNGHTTP2_USE_STATIC_LIBS=ON" "-DMBEDTLS_USE_STATIC_LIBS=ON"
    "-DUSE_NGHTTP2=ON" "-DUSE_LIBIDN2=ON" "-DUSE_WIN32_IDN=OFF"
    "-DCURL_ZLIB=ON" "-DCURL_BROTLI=ON" "-DCURL_ZSTD=ON"
    "-DZLIB_USE_STATIC_LIBS=ON"
    "-DZLIB_LIBRARY=${zlib}/lib/libzs.a"
    "-DCURL_USE_LIBPSL=OFF" "-DCURL_USE_LIBSSH2=OFF" "-DCURL_USE_LIBSSH=OFF"
    "-DCURL_DISABLE_LDAP=ON" "-DCURL_DISABLE_LDAPS=ON"
    "-DCURL_CA_BUNDLE=none" "-DCURL_CA_PATH=none"
  ] networkLibraries).overrideAttrs (old: {
    patches = (old.patches or []) ++ pkgs.lib.optional legacy ./curl-legacy-windows.patch;
  });
  regex = import ./windows-regex.nix { inherit pkgs windows threads unistring winver; };
  pty = if legacy && windows.stdenv.cc.isClang then
    import ./windows-pty.nix { inherit pkgs windows threads winver; } else null;
in {
  inherit windows threads jansson tls curl av png freetype jpeg openjpeg pdf archive xml networkLibraries regex pty;
  application = { source, packageName, version, revision, debug ? false,
                  updateBase ? "", updateTarget ? "" }: windows.stdenv.mkDerivation {
    pname = "${packageName}-windows-${arch}";
    inherit version;
    src = source;
    outputs = [ "out" "debug" ];
    nativeBuildInputs = [ windows.buildPackages.pkg-config ];
    buildInputs = [ threads jansson curl regex av png pdf freetype jpeg openjpeg archive xml ] ++ voiceRtc.dependencies ++ networkLibraries
      ++ pkgs.lib.optionals (pty != null) [ pty.collector pty.cxx pty.unwind ];
    enableParallelBuilding = true;
    dontConfigure = true;
    dontStrip = true;
    preBuild = ''
      mkdir -p build
      ${pkgs.zstd}/bin/zstd -q -19 \
        ${pkgs.cacert}/etc/ssl/certs/ca-no-trust-rules-bundle.crt -o build/ca_bundle.zst
      od -An -v -t u1 build/ca_bundle.zst |
        sed -E 's/([0-9]+)/\1,/g' > build/ca_bundle.inc
      makeFlagsArray+=(
        'DEBUG=${if debug then "1" else "0"}'
        ${pkgs.lib.optionalString (updateBase != "") "'UPDATE_BASE_URL=${updateBase}' 'UPDATE_TARGET=${updateTarget}'"}
        'TARGET_OS=Windows' 'BIN=${packageName}.exe'
        'DEBUG_SYMBOLS=debug-${packageName}.exe'
        # The commanded state needs no LibreOfficeKit headers, no import library
        # and no bundled runtime: the target's own installed engine is resolved
        # at run time and reported clearly when absent.
        'WITH_OFFICE=0' 'WITH_OFFICE_COMMANDS=1'
        "CC=$CC" "CXX=$CXX" "STRIP=$STRIP" "OBJCOPY=$OBJCOPY"
        'GIT_HEAD=${revision}' 'BUILD_VERSION=${version}'
        'CPPFLAGS=-D_WIN32_WINNT=${winver} -DWINVER=${winver} -Ibuild -DSNAJPAGENT_CA_BUNDLE=\"ca_bundle.inc\"${pkgs.lib.optionalString (pty != null) " -DSNAG_LEGACY_PTY -nostdinc++ -isystem ${pkgs.lib.getDev pty.cxx}/include/c++/v1"}'
        'CFLAGS=-std=c11 ${if debug then "-Og -g -fno-omit-frame-pointer" else "-Os -g -flto -ffunction-sections -fdata-sections"} -Wall -Wextra -Wpedantic -Werror'
        'LDFLAGS=-static -municode ${pkgs.lib.optionalString (!debug) "-flto"} -Wl,--gc-sections${pkgs.lib.optionalString legacy ",--major-os-version,5,--minor-os-version,${if arch == "x86_64" then "2" else "0"},--major-subsystem-version,5,--minor-subsystem-version,${if arch == "x86_64" then "2" else "0"}"}'
        "JANSSON_CFLAGS=$($PKG_CONFIG --cflags jansson)"
        "LDLIBS=$($PKG_CONFIG --static --libs jansson) -lsnagregex -lunistring -liconv -ladvapi32 -lntdll -lws2_32 -lwinpthread${pkgs.lib.optionalString (pty != null) " -lsnagpty -L${pty.cxx}/lib -lc++ -L${pty.unwind}/lib -lunwind -luser32 -lshell32"}"
        "CURL_CFLAGS=$($PKG_CONFIG --cflags libcurl)"
        "CURL_LIBS=$($PKG_CONFIG --static --libs libcurl)"
        "AV_CFLAGS=$($PKG_CONFIG --cflags libavformat libavcodec libavutil libswresample libswscale)"
        "AV_LIBS=$($PKG_CONFIG --static --libs libavformat libavcodec libavutil libswresample libswscale)"
        "PDF_CFLAGS=$($PKG_CONFIG --cflags poppler libpng | sed -E 's/(^| )-I/\1-isystem /g')${pkgs.lib.optionalString (pty != null) " -nostdinc++ -isystem ${pkgs.lib.getDev pty.cxx}/include/c++/v1"}"
        "PDF_LIBS=$($PKG_CONFIG --static --libs poppler libpng)${if pty != null then " -L${pty.cxx}/lib -lc++ -L${pty.unwind}/lib -lunwind" else if windows.stdenv.cc.isClang then " -lc++" else " -lstdc++"}"
        "RTC_CFLAGS=${voiceRtc.cflags}"
        "RTC_LIBS=${voiceRtc.libs} ${if pty != null then "-L${pty.cxx}/lib -lc++ -L${pty.unwind}/lib -lunwind" else if windows.stdenv.cc.isClang then "-lc++" else "-lstdc++"}"
        'MINIAUDIO_CFLAGS=-isystem ${pkgs.miniaudio.src}'
      )
    '';
    installPhase = ''
      runHook preInstall
      mkdir -p "$out/bin" "$debug"
      cp ${packageName}.exe "$out/bin/"
      cp ${if debug then "${packageName}.exe" else "debug-${packageName}.exe"} "$debug/"
      ln -s "$debug" "$out/bin/.debug"
      runHook postInstall
    '';
  };
}
