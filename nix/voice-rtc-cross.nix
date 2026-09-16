# SPDX-License-Identifier: GPL-2.0-only
{ pkgs, sourcePkgs, cmakeLibrary, tls, cxxFlags ? null, cxxLibraries ? null,
  rtcFlags ? [], srtpPatches ? [], sctpPatches ? [] }:
let
  source = import ./voice-rtc.nix { inherit pkgs; target = sourcePkgs; };
  juice = cmakeLibrary source.juice [ "-DNO_TESTS=ON" "-DNO_SERVER=ON" ] [];
  srtp = (cmakeLibrary sourcePkgs.srtp [ "-DENABLE_OPENSSL=OFF" "-DTEST_APPS=OFF" ] []).overrideAttrs (old: {
    patches = (old.patches or []) ++ srtpPatches;
    postPatch = (old.postPatch or "") + ''
      # ntohl() is u_long under Windows (winsock); align the %08x debug formats.
      substituteInPlace srtp/srtp.c \
        --replace-fail 'ntohl(ssrc))' '(unsigned int)ntohl(ssrc))' \
        --replace-fail 'ntohl(hdr->ssrc))' '(unsigned int)ntohl(hdr->ssrc))'
    '';
  });
  sctp = cmakeLibrary (sourcePkgs.usrsctp // {
    patches = (sourcePkgs.usrsctp.patches or []) ++ sctpPatches;
  }) [
    "-Dsctp_build_programs=OFF" "-Dsctp_build_shared_lib=OFF" "-Dsctp_werror=OFF"
  ] [];
  plog = cmakeLibrary sourcePkgs.plog [ "-DPLOG_BUILD_SAMPLES=OFF" "-DPLOG_BUILD_TESTS=OFF" ] [];
  opus = cmakeLibrary sourcePkgs.libopus [ "-DOPUS_BUILD_PROGRAMS=OFF" "-DOPUS_BUILD_TESTING=OFF" ] [];
  rtc = (cmakeLibrary sourcePkgs.libdatachannel ([
    "-DUSE_NICE=OFF" "-DUSE_MBEDTLS=ON" "-DPREFER_SYSTEM_LIB=ON" "-DUSE_SYSTEM_JUICE=ON"
    "-DNO_WEBSOCKET=ON" "-DNO_EXAMPLES=ON" "-DNO_TESTS=ON"
  ] ++ rtcFlags) [ juice srtp sctp plog tls ]).overrideAttrs (old: {
    preConfigure = old.preConfigure + pkgs.lib.optionalString (cxxFlags != null) ''
      cmakeFlagsArray+=("-DCMAKE_CXX_FLAGS=${cxxFlags}" "-DCMAKE_EXE_LINKER_FLAGS=${cxxLibraries}")
    '';
  });
in {
  inherit rtc juice srtp sctp opus;
  dependencies = [ rtc juice srtp sctp opus ];
  cflags = "-DRTC_STATIC -I${rtc}/include -I${opus}/include";
  libs = "-L${rtc}/lib -ldatachannel -L${juice}/lib -ljuice -L${srtp}/lib -lsrtp2 -L${sctp}/lib -lusrsctp -L${opus}/lib -lopus -L${tls}/lib -lmbedtls -lmbedx509 -lmbedcrypto";
}
