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
  # IPv6 is a library feature, not a compiler ABI change. Rebuild dependency
  # archives against its updated feature header while retaining the compiler.
  sdkLibc = base.stdenv.cc.libc.override (old: {
    extraConfig = (old.extraConfig or "") + "\nUCLIBC_HAS_IPV6 y\n";
  });
  # These implementation fixes do not change installed headers or the C ABI.
  # ABI/configuration changes belong in settings and rebuild the base compiler.
  libc = sdkLibc.overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ [ ./uclibc-legacy.patch ];
  });
  # LinuxThreads has no native ELF TLS. Use GCC's pthread-key TLS emulation,
  # including for C11 _Thread_local in the agent and its static dependencies.
  gcc = base.stdenv.cc.cc.overrideAttrs (old: {
    configureFlags = old.configureFlags ++ [ "--disable-tls" ];
  });
  wrapCompiler = runtimeLibc: base.stdenv.cc.override (old: {
    cc = gcc;
    libc = runtimeLibc;
    bintools = base.stdenv.cc.bintools.override {
      libc = runtimeLibc;
      sharedLibraryLoader = pkgs.lib.getLib runtimeLibc;
    };
    nixSupport = (old.nixSupport or { }) // {
      cc-cflags = toString (old.nixSupport.cc-cflags or "") + " -specs=${./legacy-ssp.specs}";
    };
  });
  compiler = wrapCompiler libc;
  # Static dependency archives only need the stable ABI. The final executable
  # links the patched libc; runtime-only changes need not rebuild every archive.
  sdkCompiler = wrapCompiler sdkLibc;
  # pkgsStatic forces musl on Linux. Keep this ABI and reuse the compiler;
  # only the dependency build/link modes differ between these package sets.
  runtime = isStatic: cc: import pkgs.path (settings // {
    overlays = settings.overlays ++ [ (_: previous:
      pkgs.lib.optionalAttrs
        (previous.stdenv.hostPlatform.config == settings.crossSystem.config) {
        uclibc-ng = cc.libc;
        stdenv = let
          platform = pkgs.lib.systems.elaborate (settings.crossSystem // { inherit isStatic; });
          env = previous.stdenv.override {
            inherit cc;
            hostPlatform = platform;
            targetPlatform = platform;
          };
        in if isStatic then previous.stdenvAdapters.makeStatic env else env;
      }) ];
  });
  target = runtime false compiler;
  staticTarget = (runtime true sdkCompiler).extend (_: previous:
    pkgs.lib.optionalAttrs
      (previous.stdenv.hostPlatform.config == settings.crossSystem.config) {
    # The agent needs libzstd, not its Bash/grep-dependent CLI scripts.
    zstd = previous.zstd.overrideAttrs (old: {
      buildInputs = [ ];
      propagatedBuildInputs = [ ];
      cmakeFlags = old.cmakeFlags ++ [ "-DZSTD_BUILD_PROGRAMS=OFF" ];
      preInstall = "";
      outputs = [ "out" "dev" ];
    });
  });
in {
  inherit libc compiler sdkCompiler;
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
