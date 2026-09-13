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
  # The standalone binary must use host fonts, not a build-host store path.
  fontconfig = static.fontconfig.overrideAttrs (old: {
    configureFlags = builtins.filter
      (flag: !(pkgs.lib.hasPrefix "--with-default-fonts=" flag)) old.configureFlags ++ [
      "--with-default-fonts=/usr/share/fonts,/usr/local/share/fonts"
    ];
  });
  pdf = (static.poppler.override {
    inherit fontconfig;
    # Intl is used by the disabled pdfsig utility, not the rendering library.
    libintl = null;
    minimal = true;
    qt5Support = false;
    qt6Support = false;
    introspectionSupport = false;
    utils = false;
  }).overrideAttrs (old: {
    patches = (old.patches or []) ++ [ ./poppler-static-fonts.patch ];
  });
  office = import ./office-linux.nix { inherit pkgs musl static; };
  # Static musl cannot load miniaudio's usual shared backend libraries.
  alsa = static.alsa-lib.overrideAttrs (old: {
    # Change the runtime default only; installation stays inside the Nix output.
    postConfigure = (old.postConfigure or "") + ''
      substituteInPlace include/config.h \
        --replace-fail "#define ALSA_CONFIG_DIR \"$out/share/alsa\"" \
                       '#define ALSA_CONFIG_DIR "/usr/share/alsa"'
    '';
  });
  pulse = static.libpulseaudio.overrideAttrs (old: {
    # Upstream forces shared client libraries. Build only the client statically.
    meta = old.meta // { badPlatforms = []; };
    nativeBuildInputs = [ pkgs.meson pkgs.ninja pkgs.pkg-config pkgs.gettext pkgs.perl pkgs.m4 ];
    buildInputs = [ static.check ];
    propagatedBuildInputs = [ static.libsndfile ];
    postPatch = (old.postPatch or "") + ''
      substituteInPlace src/meson.build src/pulse/meson.build \
        --replace-fail 'shared_library(' 'library('
      substituteInPlace meson.build \
        --replace-fail "dependency('sndfile', version : '>= 1.0.20')" \
                       "dependency('sndfile', version : '>= 1.0.20', static : true)"
    '';
    mesonFlags = [
      "--default-library=static" "-Dauto_features=disabled" "--sysconfdir=/etc"
      "-Dsysconfdir_install=${placeholder "out"}/etc"
      "-Ddaemon=false" "-Dclient=true" "-Ddoxygen=false" "-Dman=false"
      "-Dtests=true" "-Ddatabase=simple" "-Ddbus=disabled" "-Dglib=disabled"
      "-Dsystemd=disabled" "-Doss-output=disabled"
    ];
    doInstallCheck = false;
    postInstall = ''
      sed -i '/^Libs.private:/ s/$/ -lm -pthread -lrt/' "$out/lib/pkgconfig/libpulse.pc"
      printf '\nRequires.private: sndfile\n' >> "$out/lib/pkgconfig/libpulse.pc"
    '';
    preFixup = "";
  });
  tls = static.mbedtls;
  curl = (static.curlMinimal.override {
    opensslSupport = false;
    scpSupport = false;
    gssSupport = false;
    http2Support = true;
    websocketSupport = true;
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
  inherit static tls curl av pdf office fontconfig alsa pulse;
  application = { source, packageName, version, revision, debug ? false,
                  updateBase ? "", updateTarget ? "" }: musl.stdenv.mkDerivation {
    pname = packageName;
    inherit version;
    src = source;
    outputs = [ "out" "debug" ];
    nativeBuildInputs = [ musl.buildPackages.pkg-config ];
    buildInputs = [ static.jansson curl av pdf static.libpng static.libarchive static.libxml2 alsa pulse ];
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
        'TARGET_OS=Linux'
        # Release artifacts must not bundle LibreOffice: the standing rule is
        # installed runtimes only, never bundled on any platform, and the Office
        # dependency here would be linked into the artifact. Host builds keep
        # WITH_OFFICE=1 by default and link a separately installed runtime via
        # OFFICE_ROOT, so only these target builds take the flag off.
        'WITH_OFFICE=0'
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
        "PDF_CFLAGS=$($PKG_CONFIG --cflags poppler libpng | sed -E 's/(^| )-I/\1-isystem /g')"
        "PDF_LIBS=$($PKG_CONFIG --static --libs poppler libpng) -lstdc++"
        "MINIAUDIO_CFLAGS=-isystem ${pkgs.miniaudio.src} $($PKG_CONFIG --cflags alsa libpulse) -DMA_NO_RUNTIME_LINKING -DMA_ENABLE_ONLY_SPECIFIC_BACKENDS -DMA_ENABLE_ALSA -DMA_ENABLE_PULSEAUDIO"
        "AUDIO_DEVICE_LIBS=$($PKG_CONFIG --static --libs alsa libpulse)"
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
