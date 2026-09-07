# SPDX-License-Identifier: GPL-2.0-only
# Internal Linux 2.4 dependency work; not a qualified production target.
{ pkgs ? (import ./portable.nix { }).pkgs }:
let
  settings = {
    localSystem = pkgs.stdenv.buildPlatform.system;
    crossSystem = {
      config = "i686-unknown-linux-uclibc";
      uclibc.extraConfig = ''
        UCLIBC_HAS_THREADS_NATIVE n
        UCLIBC_HAS_LINUXTHREADS y
        UCLIBC_HAS_TLS n
        UCLIBC_HAS_STDIO_FUTEXES n
      '';
    };
    overlays = [ (_: previous: {
      uclibc-ng = (previous.uclibc-ng.override {
        extraConfig = ''
          UCLIBC_HAS_LIBUTIL y
          UCLIBC_HAS_LOCALE y
          UCLIBC_BUILD_MINIMAL_LOCALE n
          UCLIBC_BUILD_ALL_LOCALE y
        '';
      }).overrideAttrs (old: {
        patches = (old.patches or [ ]) ++ [ ./uclibc-stat-fallback.patch ];
        configurePhase = builtins.replaceStrings
          [ "make defconfig" "make oldconfig" ]
          [ "make $makeFlags defconfig" "make $makeFlags oldconfig" ]
          old.configurePhase;
        postPatch = (old.postPatch or "") + ''
          substituteInPlace extra/locale/Makefile.in --replace-fail \
            '$(wildcard /usr/include/iconv.h)' \
            '$(wildcard ${pkgs.lib.getDev pkgs.stdenv.cc.libc}/include/iconv.h)'
        '';
        LOCALE_ARCHIVE = "${pkgs.glibcLocales}/lib/locale/locale-archive";
      });
    }) ];
  };
  target = import pkgs.path settings;
  # pkgsStatic forcibly changes Linux libc to musl; retain this target's ABI.
  staticTarget = import pkgs.path (settings // {
    crossSystem = settings.crossSystem // { isStatic = true; };
  });
in {
  libc = target.stdenv.cc.libc;
  compiler = target.stdenv.cc;
  application = args: ((import ./linux.nix {
    inherit pkgs;
    musl = target;
    static = staticTarget;
  }).application args).overrideAttrs (old: {
    preBuild = builtins.replaceStrings
      [ "-D_FILE_OFFSET_BITS=64 -Ibuild" "-static-pie -flto -Wl,--gc-sections"
        "-std=c11 -Os" ]
      [ "-D_FILE_OFFSET_BITS=64 -DSNAJPAGENT_LEGACY_LINUX_CLOCK -Ibuild"
        "-static -no-pie -flto -Wl,--gc-sections,--wrap=clock_gettime"
        "-std=c11 -Os -fno-pie" ]
      old.preBuild;
  });
}
