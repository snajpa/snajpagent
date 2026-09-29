# SPDX-License-Identifier: GPL-2.0-only
{ pkgs, target }:
let
  # Native subscription media needs ICE/DTLS/SRTP. libjuice keeps this closure
  # independent of desktop GLib, GStreamer and UPnP services.
  juice = target.stdenv.mkDerivation {
    pname = "libjuice";
    version = "1.7.0";
    src = pkgs.fetchFromGitHub {
      owner = "paullouisageneau";
      repo = "libjuice";
      rev = "5948a4162d37bc213d6051b67ee2876ccc5a99a6";
      sha256 = "0xa8p87m1w9192jap14rz4c9xyi8q17gyqm6675kgqsqm6ni353h";
    };
    patches = [ ./libjuice-bsd-legacy.patch ./libjuice-win0502-poll.patch ./libjuice-win0502-legacy-rand.patch ];
    nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
    cmakeFlags = [ "-DBUILD_SHARED_LIBS=OFF" "-DNO_TESTS=ON" "-DNO_SERVER=ON" ];
  };
  tls = target.mbedtls.overrideAttrs (old: {
    postConfigure = (old.postConfigure or "") + ''
      perl scripts/config.pl set MBEDTLS_SSL_DTLS_SRTP
    '';
  });
  srtp = (target.srtp.override { openssl = null; }).overrideAttrs (old: {
    buildInputs = (old.buildInputs or []) ++ [ tls ];
    mesonFlags = builtins.filter
      (flag: !(pkgs.lib.hasPrefix "-Dcrypto-library=" flag)) old.mesonFlags
      ++ [ "-Dcrypto-library=mbedtls" ];
  });
  rtc = (target.libdatachannel.override {
    libnice = null;
    openssl = null;
    inherit srtp;
  }).overrideAttrs (_: {
    buildInputs = [ juice tls srtp target.usrsctp target.plog ];
    cmakeFlags = [
      "-DBUILD_SHARED_LIBS=OFF" "-DUSE_MBEDTLS=ON"
      "-DUSE_NICE=OFF" "-DPREFER_SYSTEM_LIB=ON" "-DUSE_SYSTEM_JUICE=ON"
      "-DNO_WEBSOCKET=ON" "-DNO_EXAMPLES=ON" "-DNO_TESTS=ON"
    ];
  });
in { inherit rtc juice tls srtp; opus = target.libopus; }
