# SPDX-License-Identifier: GPL-2.0-only
{ pkgs, sourcePkgs, arch }:
let
  lib = pkgs.lib;
  llvm = pkgs.llvmPackages_21;
  sdkInfo = (builtins.fromJSON (builtins.readFile
    (pkgs.path + "/pkgs/by-name/ap/apple-sdk/metadata/versions.json")))."15";
  sdk = (pkgs.callPackage
    (pkgs.path + "/pkgs/by-name/ap/apple-sdk/common/fetch-sdk.nix") {}) sdkInfo;
  target = "${arch}-apple-macos11.0";
  processor = if arch == "arm64" then "aarch64" else arch;
  compiler = "${llvm.clang-unwrapped}/bin/clang";
  tools = "${llvm.llvm}/bin";
  cflags = "-Os -g -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Werror=unguarded-availability";
  ldflags = "-fuse-ld=lld --ld-path=${llvm.lld}/bin/ld64.lld";
  cmakeLibrary = package: flags: dependencies:
    pkgs.stdenvNoCC.mkDerivation {
      pname = "${package.pname}-macos-${arch}";
      inherit (package) version src;
      patches = package.patches or [];
      nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.perl pkgs.python3 ];
      buildInputs = dependencies;
      strictDeps = true;
      enableParallelBuilding = true;
      dontStrip = true;
      cmakeBuildType = "MinSizeRel";
      preConfigure = ''
        export PKG_CONFIG_PATH=
        export PKG_CONFIG_LIBDIR=${lib.escapeShellArg
          (lib.concatMapStringsSep ":" (dep: "${dep}/lib/pkgconfig") dependencies)}
        cmakeFlagsArray+=(
          "-DCMAKE_C_FLAGS=${cflags}"
          "-DCMAKE_CXX_FLAGS=${cflags}"
          "-DCMAKE_EXE_LINKER_FLAGS=${ldflags}"
          "-DCMAKE_SHARED_LINKER_FLAGS=${ldflags}"
        )
      '';
      cmakeFlags = [
        "-DCMAKE_SYSTEM_NAME=Darwin"
        "-DCMAKE_SYSTEM_PROCESSOR=${processor}"
        "-DCMAKE_C_COMPILER=${compiler}"
        "-DCMAKE_C_COMPILER_TARGET=${target}"
        "-DCMAKE_CXX_COMPILER=${llvm.clang-unwrapped}/bin/clang++"
        "-DCMAKE_CXX_COMPILER_TARGET=${target}"
        "-DCMAKE_OSX_ARCHITECTURES=${arch}"
        "-DCMAKE_OSX_SYSROOT=${sdk}"
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0"
        "-DCMAKE_AR=${tools}/llvm-ar"
        "-DCMAKE_RANLIB=${tools}/llvm-ranlib"
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.10"
        "-DCMAKE_PREFIX_PATH=${lib.concatStringsSep ";" dependencies}"
        "-DCMAKE_INSTALL_LIBDIR=lib"
        "-DCMAKE_INSTALL_INCLUDEDIR=include"
        "-DCMAKE_INSTALL_BINDIR=bin"
        "-DBUILD_SHARED_LIBS=OFF"
        "-DBUILD_TESTING=OFF"
      ] ++ flags;
    };
  autotoolsLibrary = package: flags: dependencies:
    pkgs.stdenvNoCC.mkDerivation {
      pname = "${package.pname}-macos-${arch}";
      inherit (package) version src;
      nativeBuildInputs = [ pkgs.pkg-config pkgs.perl pkgs.texinfo llvm.llvm ];
      buildInputs = dependencies;
      strictDeps = true;
      enableParallelBuilding = true;
      dontStrip = true;
      configurePlatforms = [];
      configureFlags = [
        "--build=${pkgs.stdenv.buildPlatform.config}"
        "--host=${processor}-apple-darwin"
        "--disable-shared"
        "--enable-static"
        "--with-pic"
        "--disable-dependency-tracking"
      ] ++ flags;
      preConfigure = ''
        export CC="${compiler} --target=${target} -isysroot ${sdk}"
        export CXX="${llvm.clang-unwrapped}/bin/clang++ --target=${target} -isysroot ${sdk}"
        export AR=${tools}/llvm-ar RANLIB=${tools}/llvm-ranlib NM=${tools}/llvm-nm
        export STRIP=${tools}/llvm-strip
        export LD=${llvm.lld}/bin/ld64.lld LIPO=${tools}/llvm-lipo
        export CFLAGS='${cflags}' CXXFLAGS='${cflags}' LDFLAGS='${ldflags}'
        export MACOSX_DEPLOYMENT_TARGET=11.0
        export PKG_CONFIG_PATH=
        export PKG_CONFIG_LIBDIR=${lib.escapeShellArg
          (lib.concatMapStringsSep ":" (dep: "${dep}/lib/pkgconfig") dependencies)}
      '';
    };
  jansson = cmakeLibrary sourcePkgs.jansson [
    "-DJANSSON_BUILD_SHARED_LIBS=OFF"
    "-DJANSSON_BUILD_DOCS=OFF"
    "-DJANSSON_WITHOUT_TESTS=ON"
    "-DJANSSON_EXAMPLES=OFF"
  ] [];
  tls = (cmakeLibrary sourcePkgs.mbedtls [
    "-DUSE_SHARED_MBEDTLS_LIBRARY=OFF"
    "-DENABLE_PROGRAMS=OFF"
    "-DENABLE_TESTING=OFF"
    "-DGEN_FILES=OFF"
  ] []).overrideAttrs (_: {
    postPatch = ''
      perl scripts/config.pl set MBEDTLS_THREADING_C
      perl scripts/config.pl set MBEDTLS_THREADING_PTHREAD
    '';
  });
  zlib = cmakeLibrary sourcePkgs.zlib [
    "-DZLIB_BUILD_SHARED=OFF" "-DZLIB_BUILD_STATIC=ON" "-DZLIB_BUILD_TESTING=OFF"
  ] [];
  brotli = cmakeLibrary sourcePkgs.brotli [ "-DBROTLI_DISABLE_TESTS=ON" ] [];
  zstd = (cmakeLibrary sourcePkgs.zstd [
    "-DZSTD_BUILD_SHARED=OFF" "-DZSTD_BUILD_STATIC=ON"
    "-DZSTD_BUILD_PROGRAMS=OFF" "-DZSTD_BUILD_TESTS=OFF"
  ] []).overrideAttrs (_: { cmakeDir = "../build/cmake"; });
  cares = cmakeLibrary sourcePkgs.c-ares [
    "-DCARES_SHARED=OFF" "-DCARES_STATIC=ON" "-DCARES_STATIC_PIC=ON"
    "-DCARES_BUILD_TOOLS=OFF" "-DCARES_BUILD_TESTS=OFF"
  ] [];
  nghttp2 = cmakeLibrary sourcePkgs.nghttp2 [
    "-DENABLE_LIB_ONLY=ON" "-DBUILD_STATIC_LIBS=ON" "-DENABLE_DOC=OFF"
  ] [];
  iconv = autotoolsLibrary pkgs.libiconvReal [] [];
  unistring = autotoolsLibrary sourcePkgs.libunistring
    [ "--with-libiconv-prefix=${iconv}" ] [ iconv ];
  idn2 = (autotoolsLibrary sourcePkgs.libidn2 [
    "--disable-doc" "--with-libiconv-prefix=${iconv}"
    "--with-libunistring-prefix=${unistring}"
  ] [ iconv unistring ]).overrideAttrs (_: {
    # SDK 15.5 declares this, but it does not exist at the macOS 11 baseline.
    env.ac_cv_func_strchrnul = "no";
    env.gl_cv_onwards_func_strchrnul = "future OS version";
    buildPhase = ''
      runHook preBuild
      make -j"$NIX_BUILD_CORES" -C gl
      make -j"$NIX_BUILD_CORES" -C unistring
      make -j"$NIX_BUILD_CORES" -C lib
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      make -C lib install
      install -Dm644 libidn2.pc "$out/lib/pkgconfig/libidn2.pc"
      runHook postInstall
    '';
  });
  networkLibraries = [ tls zlib brotli zstd cares nghttp2 iconv unistring idn2 ];
  curl = cmakeLibrary sourcePkgs.curlMinimal [
    "-DBUILD_STATIC_LIBS=ON" "-DBUILD_CURL_EXE=OFF" "-DCURL_BUILD_EVERYTHING=OFF"
    "-DCURL_USE_MBEDTLS=ON" "-DCURL_USE_OPENSSL=OFF" "-DCURL_DEFAULT_SSL_BACKEND=mbedtls"
    "-DENABLE_ARES=ON" "-DUSE_NGHTTP2=ON" "-DUSE_LIBIDN2=ON"
    "-DUSE_APPLE_IDN=OFF" "-DCURL_ZLIB=ON" "-DCURL_BROTLI=ON" "-DCURL_ZSTD=ON"
    "-DCURL_USE_LIBPSL=OFF" "-DCURL_USE_LIBSSH2=OFF" "-DCURL_USE_LIBSSH=OFF"
    "-DCURL_DISABLE_LDAP=ON" "-DCURL_DISABLE_LDAPS=ON"
    "-DCURL_CA_BUNDLE=none" "-DCURL_CA_PATH=none"
  ] networkLibraries;
in {
  inherit sdk target compiler tools cflags ldflags jansson tls curl;
  application = { source, packageName, version, revision }:
    pkgs.stdenvNoCC.mkDerivation {
      pname = "${packageName}-macos-${arch}";
      inherit version;
      src = source;
      outputs = [ "out" "debug" ];
      nativeBuildInputs = [ pkgs.pkg-config ];
      buildInputs = [ jansson curl ] ++ networkLibraries;
      enableParallelBuilding = true;
      dontStrip = true;
      preBuild = ''
        mkdir -p build
        ${pkgs.zstd}/bin/zstd -q -19 \
          ${pkgs.cacert}/etc/ssl/certs/ca-no-trust-rules-bundle.crt -o build/ca_bundle.zst
        od -An -v -t u1 build/ca_bundle.zst |
          sed -E 's/([0-9]+)/\1,/g' > build/ca_bundle.inc
        makeFlagsArray+=(
          'TARGET_OS=Darwin'
          'CC=${compiler} --target=${target} -isysroot ${sdk}'
          'STRIP=${tools}/llvm-strip' 'DSYMUTIL=${tools}/dsymutil'
          'GIT_HEAD=${revision}' 'BUILD_VERSION=${version}'
          'CPPFLAGS=-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -Ibuild -DSNAJPAGENT_CA_BUNDLE=\"ca_bundle.inc\"'
          'CFLAGS=-std=c11 ${cflags} -flto -Wall -Wextra -Wpedantic -Werror'
          'LDFLAGS=${ldflags} -flto -Wl,-object_path_lto,build/app-lto.o -Wl,-dead_strip -Wl,-dead_strip_dylibs -Wl,-pie'
          "JANSSON_CFLAGS=$(pkg-config --cflags jansson)"
          "LDLIBS=$(pkg-config --static --libs jansson)"
          "CURL_CFLAGS=$(pkg-config --cflags libcurl)"
          "CURL_LIBS=$(pkg-config --static --libs libcurl)"
        )
      '';
      installPhase = ''
        runHook preInstall
        mkdir -p "$out/bin" "$debug"
        cp ${packageName} "$out/bin/"
        cp -R ${packageName}.dSYM "$debug/"
        ln -s "$debug/${packageName}.dSYM" "$out/bin/${packageName}.dSYM"
        runHook postInstall
      '';
    };
}
