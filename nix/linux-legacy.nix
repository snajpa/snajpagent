# SPDX-License-Identifier: GPL-2.0-only
# Static i686 LinuxThreads build, exercised on Linux 2.4.27.
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
        # libjuice voice needs <ifaddrs.h>; uClibc strips it unless
        # AI_ADDRCONFIG (which needs netlink) is enabled. Netlink device
        # queries exist since 2.4.17; legacy target is 2.4.27.
        UCLIBC_USE_NETLINK y
        UCLIBC_SUPPORT_AI_ADDRCONFIG y
      '';
    };
    overlays = pkgs.overlays ++ [ (_: previous: {
      # `check` 0.15.2 is a build dependency here: it is the framework the static
      # pulseaudio links, and its own test suite does not compile against these
      # uClibc headers. tests/check_check_sub.c calls usleep, which this uClibc-ng
      # declares only behind __USE_BSD (features.h:339-341 derives that from
      # _BSD_SOURCE/_SVID_SOURCE, and -D_BSD_SOURCE through CFLAGS did not reach
      # it either), while the suite builds with -Wfatal-errors. None of that suite
      # is in the shipped artifact, so the framework library is built and the
      # self-tests are skipped rather than the upstream source being patched.
      check = previous.check.overrideAttrs (_: {
        makeFlags = [ "SUBDIRS=lib" ];
      });
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
  sdkLibc = (base.stdenv.cc.libc.override (old: {
    extraConfig = (old.extraConfig or "") + "\nUCLIBC_HAS_IPV6 y\n";
  })).overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ [ ./uclibc-thread-probe.patch ];
  });
  # These implementation fixes do not change installed headers or the C ABI.
  # Architecture/time/thread layout changes belong in the base settings.
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
      cc-cflags = toString (old.nixSupport.cc-cflags or "") + " -specs=${./legacy-link.specs}";
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
    # GMP cannot run this cross probe.  The target header and libc.a both
    # provide nl_langinfo, so retain the real result rather than compiling
    # GMP's duplicate C++ test fallback.
    gmp = previous.gmp.overrideAttrs (old: {
      preConfigure = (old.preConfigure or "") + ''
        export ac_cv_func_nl_langinfo=yes
      '';
    });
    # Font consumers use pkg-config; the optional config script pulls target Bash.
    freetype = (previous.freetype.override { makeWrapper = null; }).overrideAttrs (old: {
      configureFlags = builtins.filter (flag: flag != "--enable-freetype-config")
        old.configureFlags ++ [ "--disable-freetype-config" ];
      postInstall = "";
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
      [ "-D_FILE_OFFSET_BITS=64 -Ibuild" "-static-pie "
        "-std=c11 " ]
      [ "-D_FILE_OFFSET_BITS=64 -DSNAJPAGENT_LEGACY_LINUX_CLOCK -Ibuild"
        "-static -no-pie -Wl,--wrap=clock_gettime "
        "-std=c11 -fno-pie " ]
      old.preBuild;
  });
}
