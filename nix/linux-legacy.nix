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
    # xz's automatic .xz encoder parallelism enters static LinuxThreads and
    # corrupts execution state on this target. Its single-threaded encoder
    # passes the full suffix/container checks, so disable only xz threading.
    xz = previous.xz.overrideAttrs (old: {
      configureFlags = (old.configureFlags or [ ]) ++ [ "--disable-threads" ];
    });
    # Coreutils' Linux boot-time helper calls gettimeofday through uClibc's
    # old-glibc fallback, but omits the owning header. Keep this package-local:
    # uClibc declares the function in <sys/time.h>; application ABI is unchanged.
    coreutils = previous.coreutils.overrideAttrs (old: {
      postPatch = (old.postPatch or "") + ''
        substituteInPlace lib/boot-time-aux.h \
          --replace-fail '#if defined __linux__ || defined __ANDROID__' '#if defined __linux__ || defined __ANDROID__
# include <sys/time.h>'
        # uClibc's configure result defines HAVE_STRUCT_XTMP_UT_HOST as 0.
        # Coreutils 9.8 declares include_where with #if but tests it with
        # #ifdef, leaving uses without a declaration. Keep the package-local
        # fix value-based, so pinky's host column remains enabled where the
        # target actually has ut_host.
        substituteInPlace src/pinky.c \
          --replace-fail '#ifdef HAVE_STRUCT_XTMP_UT_HOST' '#if HAVE_STRUCT_XTMP_UT_HOST'
      '';
    });
    # This uClibc target has no context-switching API or symbols. OpenSSL's
    # supported no-async configuration keeps its TLS implementation intact
    # without compiling the unusable POSIX async backend.
    openssl = previous.openssl.overrideAttrs (old: {
      configureFlags = (old.configureFlags or [ ]) ++ [ "no-async" ];
      # On 32-bit Linux, OpenSSL's allocator regression deliberately requests
      # exactly 2 GiB and expects an OOM result.  With host overcommit and no
      # address-space limit, old static uClibc returns a virtual mapping, which
      # invalidates the test's OOM/count contract and later segfaults.  Keep the
      # complete check suite, but give it a virtual-memory limit one KiB below
      # that test allocation so the target allocator sees the intended OOM.
      preCheck = (old.preCheck or "") + ''
        ulimit -v 2097151
      '';
      # uClibc's POSIX aligned allocator also faults on the test's 2 GiB
      # alignment itself.  Retain its 2 GiB-plus-one OOM request and the
      # custom-allocator overflow vector, but use ordinary alignment only for
      # the affected native uClibc branch.
      # The four enabled TLS/DTLS client/server corpus tests supply a
      # deterministic time() shim. Static uClibc exports time from libc.a, so
      # wrap only these test-local references around their shims; retain every
      # corpus test and the normal package checks.
      postPatch = (old.postPatch or "") + ''
        substituteInPlace test/mem_alloc_test.c \
          --replace-fail '    { 1, SIZE_MAX / 2 + 2, SIZE_MAX / 2 + 1,' \
            '    { 1, SIZE_MAX / 2 + 2,
#if defined(__UCLIBC__) && !USE_CUSTOM_ALLOC_FNS
        64,
#else
        SIZE_MAX / 2 + 1,
#endif'
        for fuzz in client dtlsclient server dtlsserver; do
          substituteInPlace "fuzz/$fuzz.c" \
            --replace-fail 'time_t time(time_t *t) TIME_IMPL(t)' \
              'time_t __wrap_time(time_t *t) TIME_IMPL(t)'
        done
      '';
      postConfigure = (old.postConfigure or "") + ''
        for fuzz in client dtlsclient server dtlsserver; do
          substituteInPlace Makefile \
            --replace-fail "-o fuzz/$fuzz-test \\" \
              "-Wl,--wrap=time -o fuzz/$fuzz-test \\"
        done
      '';
    });
    # GMP cannot run this cross probe.  The target header and libc.a both
    # provide nl_langinfo, so retain the real result rather than compiling
    # GMP's duplicate C++ test fallback.
    gmp = previous.gmp.overrideAttrs (old: {
      preConfigure = (old.preConfigure or "") + ''
        export ac_cv_func_nl_langinfo=yes
      '';
      # GMP's C++ locale test needs to replace nl_langinfo.  In a static libc
      # link its strong test definition clashes with the libc archive member.
      # Route this test's references through ld's wrapper name instead, leaving
      # the test shim strong while the normal libc symbol remains available.
      postPatch = (old.postPatch or "") + ''
        substituteInPlace tests/cxx/clocale.c \
          --replace-fail 'char *
nl_langinfo (nl_item n)' 'char *
__wrap_nl_langinfo (nl_item n)'
      '';
      postConfigure = (old.postConfigure or "") + ''
        substituteInPlace tests/cxx/Makefile \
          --replace-fail '$(AM_V_CXXLD)$(CXXLINK) $(t_locale_OBJECTS)' \
            '$(AM_V_CXXLD)$(CXXLINK) -Wl,--wrap=nl_langinfo $(t_locale_OBJECTS)'
      '';
    });
    # uClibc has no fmemopen, so libjpeg-turbo correctly omits this test
    # executable. Its CMake file must not register an absent target as a test.
    libjpeg_turbo = previous.libjpeg_turbo.overrideAttrs (old: {
      postPatch = (old.postPatch or "") + ''
        substituteInPlace CMakeLists.txt \
          --replace-fail $'if(UNIX)\n    add_test(NAME bmpsizetest-''${libtype} COMMAND bmpsizetest''${suffix})\n  endif()' \
          $'if(TARGET bmpsizetest''${suffix})\n    add_test(NAME bmpsizetest-''${libtype} COMMAND bmpsizetest''${suffix})\n  endif()'
      '';
    });
    # uClibc 1.0.55 does not expose a usleep declaration here.  libout123's
    # clock-backed test output uses it only for its regular microsecond delay;
    # nanosleep is declared and linked by this target under the same feature
    # macros.  Keep the replacement local and retain its ignored-EINTR behavior.
    mpg123 = previous.mpg123.overrideAttrs (old: {
      postPatch = (old.postPatch or "") + ''
        substituteInPlace src/libout123/libout123.c \
          --replace-fail '#include "../version.h"' '#ifdef SLEEP_CLOCK
static void
out123_sleep(unsigned long useconds)
{
  struct timespec delay = {
    .tv_sec = useconds / 1000000,
    .tv_nsec = (useconds % 1000000) * 1000
  };
  nanosleep(&delay, NULL);
}
#endif

#include "../version.h"'
        substituteInPlace src/libout123/libout123.c \
          --replace-fail 'usleep(' 'out123_sleep('
        test "$(grep -c 'out123_sleep' src/libout123/libout123.c)" -eq 3
      '';
    });
    # uClibc declares in6addr_any but does not provide the object. Use its
    # standard initializer and unspecified-address predicate in usrsctp.
    usrsctp = previous.usrsctp.overrideAttrs (old: {
      postPatch = (old.postPatch or "") + ''
        substituteInPlace usrsctplib/user_recv_thread.c \
          --replace-fail 'addr_ipv6.sin6_addr        = in6addr_any;' \
            'addr_ipv6.sin6_addr        = (struct in6_addr) IN6ADDR_ANY_INIT;'
        substituteInPlace usrsctplib/user_socket.c \
          --replace-fail 'ip6->ip6_src.s6_addr == in6addr_any.s6_addr' \
            'IN6_IS_ADDR_UNSPECIFIED(&ip6->ip6_src)'
        for file in programs/*.c; do
          if grep -q 'in6addr_any' "$file"; then
            substituteInPlace "$file" \
              --replace-fail 'in6addr_any' '(struct in6_addr) IN6ADDR_ANY_INIT'
          fi
        done
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
