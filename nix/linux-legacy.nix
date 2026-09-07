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
  base = import pkgs.path settings;
  # These implementation fixes do not change installed headers or the C ABI.
  # ABI/configuration changes belong in settings and rebuild the base compiler.
  libc = base.stdenv.cc.libc.overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ [ ./uclibc-legacy-fs.patch ];
  });
  compiler = base.stdenv.cc.override {
    inherit libc;
    bintools = base.stdenv.cc.bintools.override {
      inherit libc;
      sharedLibraryLoader = pkgs.lib.getLib libc;
    };
  };
  # pkgsStatic forces musl on Linux. Keep this ABI and reuse the compiler;
  # only the dependency build/link modes differ between these package sets.
  runtime = isStatic: import pkgs.path (settings // {
    overlays = settings.overlays ++ [ (_: previous:
      pkgs.lib.optionalAttrs
        (previous.stdenv.hostPlatform.config == settings.crossSystem.config) {
        uclibc-ng = libc;
        stdenv = let
          platform = pkgs.lib.systems.elaborate (settings.crossSystem // { inherit isStatic; });
          env = previous.stdenv.override {
            cc = compiler;
            hostPlatform = platform;
            targetPlatform = platform;
          };
        in if isStatic then previous.stdenvAdapters.makeStatic env else env;
      }) ];
  });
  target = runtime false;
  staticTarget = runtime true;
in {
  inherit libc compiler;
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
