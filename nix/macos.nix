# SPDX-License-Identifier: GPL-2.0-only
{ pkgs, sourcePkgs, arch, deployment ? "11.0" }:
let
  lib = pkgs.lib;
  llvm = pkgs.llvmPackages_21;
  sdkInfo = (builtins.fromJSON (builtins.readFile
    (pkgs.path + "/pkgs/by-name/ap/apple-sdk/metadata/versions.json")))."15";
  sdk = if arch == "i386" then pkgs.stdenvNoCC.mkDerivation {
    pname = "macOS-SDK";
    version = "10.12";
    src = pkgs.fetchurl {
      urls = [
        "https://swcdn.apple.com/content/downloads/22/62/041-88607/wg8avdk0jo75k9a13gentz9stwqgrqmcv6/CLTools_SDK_OSX1012.pkg"
        "https://swdist.apple.com/content/downloads/22/62/041-88607/wg8avdk0jo75k9a13gentz9stwqgrqmcv6/CLTools_SDK_OSX1012.pkg"
      ];
      sha256 = "724a1a41d93d0ac8ea7e1869f5113ad2772c63b2869ff751bf2f95ae9292a10d";
    };
    nativeBuildInputs = [ pkgs.pbzx pkgs.cpio ];
    unpackPhase = ''pbzx "$src" | cpio -idm --no-absolute-filenames'';
    dontConfigure = true;
    dontBuild = true;
    dontFixup = true;
    installPhase = ''mv Library/Developer/CommandLineTools/SDKs/MacOSX.sdk "$out"'';
  } else (pkgs.callPackage
    (pkgs.path + "/pkgs/by-name/ap/apple-sdk/common/fetch-sdk.nix") {}) (sdkInfo // {
      urls = [
        (builtins.head sdkInfo.urls)
        (lib.replaceStrings [ "swcdn.apple.com" ] [ "swdist.apple.com" ]
          (builtins.head sdkInfo.urls))
      ] ++ builtins.tail sdkInfo.urls;
    });
  target = "${arch}-apple-macos${deployment}";
  processor = if arch == "arm64" then "aarch64" else arch;
  compiler = "${llvm.clang-unwrapped}/bin/clang";
  tools = "${llvm.llvm}/bin";
  cflags = "-Os -g -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Werror=unguarded-availability";
  legacyLoader = lib.versionOlder deployment "10.8";
  linker = if legacyLoader then
    "${import ./macos-linker.nix { inherit pkgs; }}/bin/x86_64-apple-darwin-ld"
    else "${llvm.lld}/bin/ld64.lld";
  ldflags = lib.optionalString (!legacyLoader) "-fuse-ld=lld " + "--ld-path=${linker}";
  compilerBuiltins = pkgs.runCommand "compiler-rt-darwin-${arch}-${deployment}" {} ''
    mkdir -p "$out/lib"
    for file in ${if arch == "i386" then
      "i386/divdi3.S i386/moddi3.S i386/udivdi3.S i386/umoddi3.S" else
      "udivti3.c udivmodti4.c"}; do
      ${compiler} --target=${target} -isysroot ${sdk} -Os -fvisibility=hidden \
        -c ${llvm.compiler-rt.src}/compiler-rt/lib/builtins/"$file" \
        -o "$(basename "$file").o"
    done
    ${tools}/llvm-ar rcs "$out/lib/libclang_rt.builtins.a" ./*.o
  '';
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
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=${deployment}"
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
        export LD=${linker} LIPO=${tools}/llvm-lipo
        export CFLAGS='${cflags}' CXXFLAGS='${cflags}' LDFLAGS='${ldflags}'
        export MACOSX_DEPLOYMENT_TARGET=${deployment}
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
  ] []).overrideAttrs (old: {
    patches = (old.patches or []) ++ lib.optional
      (lib.versionOlder deployment "10.12") ./mbedtls-legacy-darwin.patch;
    postPatch = ''
      perl scripts/config.pl set MBEDTLS_THREADING_C
      perl scripts/config.pl set MBEDTLS_THREADING_PTHREAD
    '';
  });
  zlib = cmakeLibrary sourcePkgs.zlib [
    "-DZLIB_BUILD_SHARED=OFF" "-DZLIB_BUILD_STATIC=ON" "-DZLIB_BUILD_TESTING=OFF"
  ] [];
  av = pkgs.stdenvNoCC.mkDerivation {
    pname = "ffmpeg-headless-macos-${arch}";
    inherit (sourcePkgs.ffmpeg_8) version src;
    patches = sourcePkgs.ffmpeg_8.patches ++ [ ./ffmpeg-darwin-archive-names.patch ];
    nativeBuildInputs = [ pkgs.pkg-config pkgs.perl pkgs.nasm llvm.llvm ];
    buildInputs = [ zlib ];
    strictDeps = true;
    enableParallelBuilding = true;
    dontStrip = true;
    # llvm-strip -x corrupts NASM local-constant relocations in intermediate
    # Mach-O objects. Keep them intact; final executable/dSYM packaging strips.
    makeFlags = [ "ASMSTRIPFLAGS=" ];
    configurePlatforms = [];
    configureFlags = [
      "--enable-cross-compile" "--target-os=darwin" "--arch=${processor}"
      "--enable-static" "--disable-shared" "--enable-pic"
      "--disable-autodetect" "--disable-network" "--disable-programs" "--disable-doc"
      "--disable-avdevice" "--disable-avfilter"
      "--enable-avcodec" "--enable-avformat" "--enable-avutil"
      "--enable-swresample" "--enable-swscale" "--enable-zlib"
      "--enable-pthreads" "--enable-safe-bitstream-reader" "--enable-pixelutils"
      "--disable-gpl" "--disable-version3" "--pkg-config-flags=--static"
    ];
    preConfigure = ''
      export PKG_CONFIG_PATH=
      export PKG_CONFIG_LIBDIR=${zlib}/lib/pkgconfig
      export MACOSX_DEPLOYMENT_TARGET=${deployment}
      configureFlagsArray+=(
        "--host-cc=${pkgs.stdenv.cc}/bin/cc"
        "--cc=${compiler} --target=${target} -isysroot ${sdk}"
        "--cxx=${llvm.clang-unwrapped}/bin/clang++ --target=${target} -isysroot ${sdk}"
        "--ar=${tools}/llvm-ar" "--ranlib=${tools}/llvm-ranlib"
        "--nm=${tools}/llvm-nm" "--strip=${tools}/llvm-strip"
        "--extra-cflags=${cflags}" "--extra-ldflags=${ldflags}"
      )
    '';
  };
  png = (cmakeLibrary sourcePkgs.libpng [
    "-DPNG_SHARED=OFF" "-DPNG_STATIC=ON" "-DPNG_TESTS=OFF" "-DPNG_TOOLS=OFF"
  ] [ zlib ]).overrideAttrs (old: {
    # libpng's header generator invokes Clang directly, outside CMake's target rule.
    preConfigure = old.preConfigure + ''
      cmakeFlagsArray+=("-DCMAKE_C_FLAGS=${cflags} --target=${target}")
    '';
  });
  freetype = cmakeLibrary sourcePkgs.freetype [
    "-DFT_DISABLE_BZIP2=ON" "-DFT_DISABLE_BROTLI=ON" "-DFT_DISABLE_HARFBUZZ=ON"
    "-DFT_REQUIRE_ZLIB=ON" "-DFT_REQUIRE_PNG=ON"
  ] [ zlib png ];
  expat = cmakeLibrary sourcePkgs.expat [
    "-DEXPAT_SHARED_LIBS=OFF" "-DEXPAT_BUILD_TOOLS=OFF"
    "-DEXPAT_BUILD_EXAMPLES=OFF" "-DEXPAT_BUILD_TESTS=OFF" "-DEXPAT_BUILD_DOCS=OFF"
  ] [];
  fontconfig = (autotoolsLibrary sourcePkgs.fontconfig [
    "--disable-docs" "--disable-docbook" "--disable-cache-build" "--disable-nls"
    "--sysconfdir=/etc" "--with-cache-dir=/var/cache/fontconfig"
  ] [ expat freetype png zlib ]).overrideAttrs (old: {
    nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.gperf pkgs.python3 ];
    installFlags = [ "sysconfdir=$(out)/etc" "RUN_FC_CACHE_TEST=false" "fc_cachedir=$(TMPDIR)/fontconfig-cache" ];
  });
  jpeg = (cmakeLibrary sourcePkgs.libjpeg [
    "-DENABLE_SHARED=OFF" "-DENABLE_STATIC=ON" "-DWITH_TURBOJPEG=OFF"
  ] []).overrideAttrs (old: {
    nativeBuildInputs = old.nativeBuildInputs ++ lib.optional (arch != "arm64") pkgs.nasm;
    cmakeFlags = old.cmakeFlags ++ [ "-DCMAKE_INSTALL_NAME_TOOL=${tools}/llvm-install-name-tool" ];
  });
  openjpeg = cmakeLibrary sourcePkgs.openjpeg [ "-DBUILD_CODEC=OFF" ] [];
  pdf = (cmakeLibrary sourcePkgs.poppler [
    "-DENABLE_UNSTABLE_API_ABI_HEADERS=ON" "-DFONT_CONFIGURATION=fontconfig"
    "-DENABLE_UTILS=OFF" "-DENABLE_CPP=OFF" "-DENABLE_GLIB=OFF"
    "-DENABLE_GOBJECT_INTROSPECTION=OFF" "-DENABLE_QT5=OFF" "-DENABLE_QT6=OFF"
    "-DBUILD_QT5_TESTS=OFF" "-DBUILD_QT6_TESTS=OFF" "-DBUILD_CPP_TESTS=OFF"
    "-DBUILD_MANUAL_TESTS=OFF" "-DENABLE_LCMS=OFF" "-DENABLE_LIBCURL=OFF"
    "-DENABLE_LIBTIFF=OFF" "-DENABLE_NSS3=OFF" "-DENABLE_GPGME=OFF"
  ] [ zlib png freetype expat fontconfig jpeg openjpeg pkgs.boost ]).overrideAttrs (old: {
    cmakeBuildType = "Release";
    patches = old.patches ++ [ ./poppler-static-fonts.patch ];
  });
  xml = cmakeLibrary sourcePkgs.libxml2 [
    "-DLIBXML2_WITH_PROGRAMS=OFF" "-DLIBXML2_WITH_TESTS=OFF"
    "-DLIBXML2_WITH_PYTHON=OFF" "-DLIBXML2_WITH_MODULES=OFF"
    "-DLIBXML2_WITH_ICONV=ON"
  ] [ iconv ];
  archive = cmakeLibrary sourcePkgs.libarchive [
    "-DENABLE_TAR=OFF" "-DENABLE_CPIO=OFF" "-DENABLE_CAT=OFF"
    "-DENABLE_UNZIP=OFF" "-DENABLE_TEST=OFF" "-DENABLE_INSTALL=ON"
    "-DENABLE_OPENSSL=OFF" "-DENABLE_MBEDTLS=OFF" "-DENABLE_NETTLE=OFF"
    "-DENABLE_CNG=OFF" "-DENABLE_LIBB2=OFF" "-DENABLE_LZ4=OFF"
    "-DENABLE_LZO=OFF" "-DENABLE_LZMA=OFF" "-DENABLE_ZSTD=OFF"
    "-DENABLE_BZip2=OFF" "-DENABLE_LIBXML2=OFF" "-DENABLE_EXPAT=OFF"
    "-DENABLE_WIN32_XMLLITE=OFF" "-DENABLE_PCREPOSIX=OFF"
    "-DENABLE_PCRE2POSIX=OFF" "-DENABLE_ZLIB=ON" "-DENABLE_ICONV=ON"
  ] [ zlib iconv ];
  brotli = cmakeLibrary sourcePkgs.brotli [ "-DBROTLI_DISABLE_TESTS=ON" ] [];
  zstd = (cmakeLibrary sourcePkgs.zstd [
    "-DZSTD_BUILD_SHARED=OFF" "-DZSTD_BUILD_STATIC=ON"
    "-DZSTD_BUILD_PROGRAMS=OFF" "-DZSTD_BUILD_TESTS=OFF"
  ] []).overrideAttrs (_: { cmakeDir = "../build/cmake"; });
  cares = (cmakeLibrary sourcePkgs.c-ares ([
    "-DCARES_SHARED=OFF" "-DCARES_STATIC=ON" "-DCARES_STATIC_PIC=ON"
    "-DCARES_BUILD_TOOLS=OFF" "-DCARES_BUILD_TESTS=OFF"
  ] ++ lib.optional (lib.versionOlder deployment "10.11") "-DHAVE_CONNECTX=OFF"
    ++ lib.optionals (lib.versionOlder deployment "10.7") [
      "-DHAVE_ARC4RANDOM_BUF=OFF" "-DHAVE_STRNLEN=OFF" "-DHAVE_MEMMEM=OFF"
    ])
  []).overrideAttrs (old: {
    patches = (old.patches or []) ++ lib.optional
      (lib.versionOlder deployment "10.12") ./cares-legacy-darwin.patch;
  });
  nghttp2 = (cmakeLibrary sourcePkgs.nghttp2 [
    "-DENABLE_LIB_ONLY=ON" "-DBUILD_STATIC_LIBS=ON" "-DENABLE_DOC=OFF"
  ] []).overrideAttrs (old: {
    patches = (old.patches or []) ++ lib.optional
      (lib.versionOlder deployment "10.12") ./nghttp2-legacy-darwin.patch;
  });
  iconv = (autotoolsLibrary pkgs.libiconvReal [] []).overrideAttrs (_:
    lib.optionalAttrs (lib.versionOlder deployment "10.10") {
      buildPhase = ''
        runHook preBuild
        make lib/localcharset.h
        make -j"$NIX_BUILD_CORES" -C lib
        runHook postBuild
      '';
      installPhase = ''
        runHook preInstall
        make -C libcharset install
        make -C lib install
        install -Dm644 include/iconv.h.inst "$out/include/iconv.h"
        runHook postInstall
      '';
    });
  unistring = (autotoolsLibrary sourcePkgs.libunistring
    [ "--with-libiconv-prefix=${iconv}" ] [ iconv ]).overrideAttrs (_:
    lib.optionalAttrs (lib.versionOlder deployment "10.7") {
      env.ac_cv_func_strnlen = "no";
    });
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
  inherit sdk target compiler tools cflags ldflags jansson tls curl av pdf xml archive iconv zlib;
  application = { source, packageName, version, revision, debug ? false,
                  updateBase ? "", updateTarget ? "" }:
    pkgs.stdenvNoCC.mkDerivation {
      pname = "${packageName}-macos-${arch}";
      inherit version;
      src = source;
      outputs = [ "out" "debug" ];
      nativeBuildInputs = [ pkgs.pkg-config ];
      buildInputs = [ jansson curl av pdf png freetype expat fontconfig jpeg openjpeg xml archive ] ++ networkLibraries;
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
          'TARGET_OS=Darwin'
          'CC=${compiler} --target=${target} -isysroot ${sdk}'
          'CXX=${llvm.clang-unwrapped}/bin/clang++ --target=${target} -isysroot ${sdk}'
          'STRIP=${if legacyLoader then builtins.dirOf linker + "/x86_64-apple-darwin-strip" else tools + "/llvm-strip"}'
          'DSYMUTIL=${tools}/dsymutil'
          'GIT_HEAD=${revision}' 'BUILD_VERSION=${version}'
          'CPPFLAGS=-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -Ibuild -DSNAJPAGENT_CA_BUNDLE=\"ca_bundle.inc\"'
          'CFLAGS=-std=c11 ${if debug then "-Og -g -fno-omit-frame-pointer -Werror=unguarded-availability" else cflags + " -flto"} -Wall -Wextra -Wpedantic -Werror'
          'LDFLAGS=${ldflags}${lib.optionalString legacyLoader " -Wl,-lto_library,${llvm.llvm.lib}/lib/libLTO.so"} ${lib.optionalString (!debug) "-flto -Wl,-object_path_lto,build/app-lto.o -Wl,-dead_strip -Wl,-dead_strip_dylibs"} -Wl,-pie'
          "JANSSON_CFLAGS=$(pkg-config --cflags jansson)"
          "LDLIBS=$(pkg-config --static --libs jansson)"
          "AV_CFLAGS=$(pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale)"
          "AV_LIBS=$(pkg-config --static --libs libavformat libavcodec libavutil libswresample libswscale)"
          "PDF_CFLAGS=$(pkg-config --cflags poppler libpng | sed -E 's/(^| )-I/\1-isystem /g')"
          "PDF_LIBS=$(pkg-config --static --libs poppler libpng) -lc++"
          'MINIAUDIO_CFLAGS=-isystem ${pkgs.miniaudio.src}'
          "CURL_CFLAGS=$(pkg-config --cflags libcurl)"
          "CURL_LIBS=$(pkg-config --static --libs libcurl)${lib.optionalString legacyLoader " ${compilerBuiltins}/lib/libclang_rt.builtins.a"}"
        )
      '';
      installPhase = ''
        runHook preInstall
        mkdir -p "$out/bin" "$debug"
        cp ${packageName} "$out/bin/"
        ${lib.optionalString debug "${tools}/dsymutil ${packageName} -o ${packageName}.dSYM"}
        cp -R ${packageName}.dSYM "$debug/"
        ln -s "$debug/${packageName}.dSYM" "$out/bin/${packageName}.dSYM"
        runHook postInstall
      '';
    };
}
