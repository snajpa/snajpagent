# SPDX-License-Identifier: GPL-2.0-only
{ pkgs }:
let
  windows = pkgs.pkgsCross.mingwW64;
  threads = windows.windows.mcfgthreads.overrideAttrs (old: {
    pname = "mcfgthread-static";
    mesonFlags = (old.mesonFlags or []) ++ [ "-Ddefault_library=static" ];
  });
  cmakeLibrary = package: flags: dependencies:
    windows.stdenv.mkDerivation {
      pname = "${package.pname}-windows-x86_64-static";
      inherit (package) version src;
      patches = package.patches or [];
      nativeBuildInputs = [ pkgs.cmake pkgs.ninja windows.buildPackages.pkg-config
                           pkgs.perl pkgs.python3 ];
      buildInputs = dependencies ++ [ threads ];
      strictDeps = true;
      enableParallelBuilding = true;
      cmakeBuildType = "MinSizeRel";
      preConfigure = ''
        cmakeFlagsArray+=("-DCMAKE_C_FLAGS=-D_WIN32_WINNT=0x0601 -DWINVER=0x0601")
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
    windows.stdenv.mkDerivation {
      pname = "${package.pname}-windows-x86_64-static";
      inherit (package) version src;
      patches = package.patches or [];
      nativeBuildInputs = [ windows.buildPackages.pkg-config pkgs.perl pkgs.texinfo ];
      buildInputs = dependencies ++ [ threads ];
      strictDeps = true;
      enableParallelBuilding = true;
      env.CFLAGS = "-Os -g -D_WIN32_WINNT=0x0601 -DWINVER=0x0601";
      configureFlags = [ "--disable-shared" "--enable-static"
                         "--disable-dependency-tracking" ] ++ flags;
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
  ] [ windows.windows.pthreads ]).overrideAttrs (_: {
    postPatch = ''
      perl scripts/config.pl set MBEDTLS_THREADING_C
      perl scripts/config.pl set MBEDTLS_THREADING_PTHREAD
    '';
    postInstall = ''
      printf '\nLibs.private: -L${windows.windows.pthreads}/lib -lpthread -lbcrypt\n' \
        >> "$out/lib/pkgconfig/mbedcrypto.pc"
    '';
  });
  zlib = cmakeLibrary windows.zlib [
    "-DZLIB_BUILD_SHARED=OFF" "-DZLIB_BUILD_STATIC=ON" "-DZLIB_BUILD_TESTING=OFF"
  ] [];
  brotli = cmakeLibrary windows.brotli [ "-DBROTLI_DISABLE_TESTS=ON" ] [];
  zstd = (cmakeLibrary windows.zstd [
    "-DZSTD_BUILD_SHARED=OFF" "-DZSTD_BUILD_STATIC=ON"
    "-DZSTD_BUILD_PROGRAMS=OFF" "-DZSTD_BUILD_TESTS=OFF"
  ] []).overrideAttrs (_: { cmakeDir = "../build/cmake"; });
  cares = cmakeLibrary windows.c-ares [
    "-DCARES_SHARED=OFF" "-DCARES_STATIC=ON"
    "-DCARES_BUILD_TOOLS=OFF" "-DCARES_BUILD_TESTS=OFF"
  ] [];
  nghttp2 = (cmakeLibrary windows.nghttp2 [
    "-DENABLE_LIB_ONLY=ON" "-DBUILD_STATIC_LIBS=ON" "-DENABLE_DOC=OFF"
  ] []).overrideAttrs (_: {
    postInstall = ''
      substituteInPlace "$out/lib/pkgconfig/libnghttp2.pc" \
        --replace-fail 'Cflags: ' 'Cflags: -DNGHTTP2_STATICLIB '
    '';
  });
  iconv = autotoolsLibrary windows.libiconvReal [] [];
  unistring = autotoolsLibrary windows.libunistring
    [ "--with-libiconv-prefix=${iconv}" ] [ iconv ];
  idn2 = autotoolsLibrary windows.libidn2 [
    "--disable-doc" "--with-libiconv-prefix=${iconv}"
    "--with-libunistring-prefix=${unistring}"
  ] [ iconv unistring ];
  networkLibraries = [ tls windows.windows.pthreads zlib brotli zstd cares nghttp2
                       iconv unistring idn2 ];
  curl = cmakeLibrary windows.curlMinimal [
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
  ] networkLibraries;
  regex = import ./windows-regex.nix { inherit pkgs windows threads unistring; };
in {
  inherit windows threads jansson tls curl networkLibraries regex;
  application = { source, packageName, version, revision }: windows.stdenv.mkDerivation {
    pname = "${packageName}-windows-x86_64";
    inherit version;
    src = source;
    outputs = [ "out" "debug" ];
    nativeBuildInputs = [ windows.buildPackages.pkg-config ];
    buildInputs = [ threads jansson curl regex ] ++ networkLibraries;
    enableParallelBuilding = true;
    dontStrip = true;
    preBuild = ''
      mkdir -p build
      od -An -v -t u1 ${pkgs.cacert}/etc/ssl/certs/ca-no-trust-rules-bundle.crt |
        sed -E 's/([0-9]+)/\1,/g' > build/ca_bundle.inc
      makeFlagsArray+=(
        'TARGET_OS=Windows' 'BIN=${packageName}.exe'
        'DEBUG_SYMBOLS=debug-${packageName}.exe'
        "CC=$CC" "STRIP=$STRIP" "OBJCOPY=$OBJCOPY"
        'GIT_HEAD=${revision}' 'BUILD_VERSION=${version}'
        'CPPFLAGS=-D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -Ibuild -DSNAJPAGENT_CA_BUNDLE=\"ca_bundle.inc\"'
        'CFLAGS=-std=c11 -Os -g -flto -ffunction-sections -fdata-sections -Wall -Wextra -Wpedantic -Werror'
        'LDFLAGS=-static -municode -flto -Wl,--gc-sections'
        "JANSSON_CFLAGS=$($PKG_CONFIG --cflags jansson)"
        "LDLIBS=$($PKG_CONFIG --static --libs jansson) -lsnagregex -lunistring -liconv -ladvapi32 -lntdll -lws2_32 -lwinpthread"
        "CURL_CFLAGS=$($PKG_CONFIG --cflags libcurl)"
        "CURL_LIBS=$($PKG_CONFIG --static --libs libcurl)"
      )
      # Upstream zlib.pc still advertises -lz; its static Windows archive is libzs.a.
      makeFlagsArray+=("CURL_LIBS=$($PKG_CONFIG --static --libs libcurl | sed 's/-lz /-lzs /g')")
    '';
    installPhase = ''
      runHook preInstall
      mkdir -p "$out/bin" "$debug"
      cp ${packageName}.exe "$out/bin/"
      cp debug-${packageName}.exe "$debug/"
      ln -s "$debug" "$out/bin/.debug"
      runHook postInstall
    '';
  };
}
