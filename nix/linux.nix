# SPDX-License-Identifier: GPL-2.0-only
{ pkgs, musl, static ? musl.pkgsStatic }:
let
  clockFallback = musl.stdenv.hostPlatform.isx86_64;
  atomicFallback = musl.stdenv.hostPlatform.isPower && musl.stdenv.hostPlatform.is32bit;
  # File decoding uses built-in codecs. Device I/O belongs to miniaudio;
  # FFmpeg receives private descriptors and has no network backend.
  av = (static.ffmpeg_8.override {
    ffmpegVariant = "headless";
    withHeadlessDeps = false;
    withSmallDeps = false;
    withFullDeps = false;
    withGPL = false;
    withVersion3 = false;
    withZlib = true;
    withSafeBitstreamReader = true;
    # FFmpeg's built-in FATE pixelutils test requires this library API.
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
  }).overrideAttrs (_: {
    # `make check` also builds optional tools/examples requiring avfilter and
    # device libraries. This profile builds and tests the five linked libraries.
    checkPhase = ''
      runHook preCheck
      make -j$NIX_BUILD_CORES testprogs fate
      runHook postCheck
    '';
  });
  pdf = static.poppler.override {
    minimal = true;
    qt5Support = false;
    qt6Support = false;
    introspectionSupport = false;
    utils = false;
  };
  office = import ./office-linux.nix { inherit pkgs musl static; };
  tls = static.mbedtls;
  curl = (static.curlMinimal.override {
    opensslSupport = false;
    scpSupport = false;
    gssSupport = false;
    http2Support = true;
    idnSupport = true;
    zlibSupport = true;
    brotliSupport = true;
    zstdSupport = true;
    c-aresSupport = true;
  }).overrideAttrs (old: {
    propagatedBuildInputs = old.propagatedBuildInputs ++ [ tls ];
    # Keep curl's checked, nonblocking pipe wakeup on pre-eventfd/pipe2 kernels.
    ac_cv_func_eventfd = "no";
    ac_cv_func_pipe2 = "no";
    configureFlags = builtins.filter (flag: flag != "--without-ssl") old.configureFlags ++ [
      "--with-mbedtls=${pkgs.lib.getDev tls}"
      "--without-ca-bundle"
      "--without-ca-path"
    ];
  });
in {
  inherit static tls curl av pdf office;
  application = { source, packageName, version, revision, debug ? false,
                  updateBase ? "", updateTarget ? "" }: musl.stdenv.mkDerivation {
    pname = packageName;
    inherit version;
    src = source;
    outputs = [ "out" "debug" ];
    nativeBuildInputs = [ musl.buildPackages.pkg-config ];
    buildInputs = [ static.jansson curl av pdf static.libpng static.libarchive static.libxml2 ];
    enableParallelBuilding = true;
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
        'TARGET_OS=Linux'
        "CC=$CC" "CXX=$CXX" "STRIP=$STRIP" "OBJCOPY=$OBJCOPY"
        "GIT_HEAD=${revision}" "BUILD_VERSION=${version}"
        'CPPFLAGS=-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64 -Ibuild -DSNAJPAGENT_CA_BUNDLE=\"ca_bundle.inc\"${pkgs.lib.optionalString clockFallback " -DSNAJPAGENT_LEGACY_LINUX_CLOCK"}'
        'CFLAGS=-std=c11 ${if debug then "-Og -g -fno-omit-frame-pointer" else "-Os -g -flto -ffunction-sections -fdata-sections"} -Wall -Wextra -Wpedantic -Werror'
        'LDFLAGS=-static-pie ${pkgs.lib.optionalString (!debug) "-flto"} -Wl,--gc-sections${pkgs.lib.optionalString clockFallback ",--wrap=clock_gettime"}${pkgs.lib.optionalString musl.stdenv.hostPlatform.isAarch32 " -Wl,-Bstatic,--no-dynamic-linker,-z,text"}${pkgs.lib.optionalString musl.stdenv.hostPlatform.isRiscV " -Wl,--exclude-libs,ALL"}'
        "JANSSON_CFLAGS=$($PKG_CONFIG --cflags jansson)"
        "LDLIBS=$($PKG_CONFIG --static --libs jansson)${pkgs.lib.optionalString atomicFallback " -latomic"}"
        "CURL_CFLAGS=$($PKG_CONFIG --cflags libcurl)"
        "CURL_LIBS=$($PKG_CONFIG --static --libs libcurl)"
        "AV_CFLAGS=$($PKG_CONFIG --cflags libavformat libavcodec libavutil libswresample libswscale)"
        "AV_LIBS=$($PKG_CONFIG --static --libs libavformat libavcodec libavutil libswresample libswscale)"
        "PDF_CFLAGS=$($PKG_CONFIG --cflags poppler libpng)"
        "PDF_LIBS=$($PKG_CONFIG --static --libs poppler libpng) -lstdc++"
        'MINIAUDIO_CFLAGS=-isystem ${pkgs.miniaudio.src}'
      )
    '';
    installPhase = ''
      runHook preInstall
      mkdir -p "$out/bin" "$debug"
      cp ${packageName} "$out/bin/"
      cp ${if debug then "${packageName}" else "debug-${packageName}"} "$debug/"
      ln -s "$debug" "$out/bin/.debug"
      runHook postInstall
    '';
  };
}
