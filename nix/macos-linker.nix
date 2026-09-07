# SPDX-License-Identifier: GPL-2.0-only
{ pkgs }:
let
  llvm = pkgs.llvmPackages_21;
  dispatch = pkgs.swift-corelibs-libdispatch.override {
    stdenv = llvm.stdenv;
    useSwift = false;
  };
in llvm.stdenv.mkDerivation {
  pname = "cctools-port";
  version = "1030.6.3-ld64-956.6";
  src = pkgs.fetchurl {
    urls = [
      "https://github.com/tpoechtrager/cctools-port/archive/904de2a71d4da6a9b30d2efaf912a10ddc7d9ddb.tar.gz"
      "https://codeload.github.com/tpoechtrager/cctools-port/tar.gz/904de2a71d4da6a9b30d2efaf912a10ddc7d9ddb"
    ];
    sha256 = "6809ba18b6b4f4646b17b1baba48f9a0cb94425cc334272918ba944fd7399b32";
  };
  postUnpack = ''sourceRoot+=/cctools'';
  nativeBuildInputs = [ pkgs.autoreconfHook ];
  buildInputs = [ pkgs.libtapi dispatch pkgs.libuuid llvm.llvm ];
  configurePlatforms = [];
  configureFlags = [
    "--target=x86_64-apple-darwin"
    "--with-llvm-config=${llvm.llvm.dev}/bin/llvm-config"
    "--disable-xar-support"
  ];
  enableParallelBuilding = true;
  strictDeps = true;
  meta = {
    description = "Darwin cross-linker with legacy Mach-O loader support";
    homepage = "https://github.com/tpoechtrager/cctools-port";
    license = pkgs.lib.licenses.apple-psl20;
    platforms = pkgs.lib.platforms.unix;
  };
}
