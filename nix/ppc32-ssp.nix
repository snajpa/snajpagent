# SPDX-License-Identifier: GPL-2.0-only
final: prev:
{
  coreutils = prev.coreutils.overrideAttrs (old: {
    postPatch = (old.postPatch or "") + ''
      cat >> src/libstdbuf.c <<'EOF'

      /* powerpc musl: the 32-bit CRT/spec introduces a reference to the
         glibc-only __stack_chk_fail_local for shared libstdbuf.so, while musl
         provides only __stack_chk_fail. Supply the local alias here so the
         link keeps stack protection (rebuilding with -fno-stack-protector
         also links but drops it). Scoped to the ppc32 target via its overlay. */
      void __stack_chk_fail(void);
      void __stack_chk_fail_local(void) { __stack_chk_fail(); }
      EOF
    '';
  });
}
