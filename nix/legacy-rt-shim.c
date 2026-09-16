/* SPDX-License-Identifier: GPL-2.0-only */
/* Legacy-OS runtime shims for the 2003-era BSD SDKs used by the legacy rows
 * (FreeBSD 5.1, NetBSD 2.0, OpenBSD 3.5): their libc predates the
 * stack-protector runtime and their libm predates exp2().  Built with the
 * recipe's target clang without -fstack-protector (so the shim itself does not
 * consume the guard) and appended last on the affected links only. */

#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

uintptr_t __stack_chk_guard = (uintptr_t)0x0badc0defeedfaceULL;

__attribute__((constructor)) static void
legacyrt_seed_guard(void)
{
	__stack_chk_guard ^= (uintptr_t)getpid();
	__stack_chk_guard ^= (uintptr_t)&__stack_chk_guard;
	__stack_chk_guard ^= (uintptr_t)time((time_t *)0);
}

void
__stack_chk_fail(void)
{
	abort();
}

/* 2^x without libm: split into integer and fractional parts, scale with the
 * IEEE-754 exponent field (or repeated squaring outside the normal range) and
 * evaluate 2^f (f in [0,1)) with a degree-7 series (~1e-7 rel. error). */
double
exp2(double x)
{
	union { double d; uint64_t u; } s;
	int64_t k;
	double f, p;
	static const double c1 = 0.693147180559945286;
	static const double c2 = 0.240226506959100712;
	static const double c3 = 0.055504108664821609;
	static const double c4 = 0.009618129107628477;
	static const double c5 = 0.001333355814642844;
	static const double c6 = 0.000154035303933816;
	static const double c7 = 0.000015252733804456;

	if (x != x)
		return x;			/* NaN */
	if (x >= 1024.0)
		return __builtin_huge_val();
	if (x <= -1075.0)
		return 0.0;

	k = (int64_t)x;				/* truncates toward zero */
	f = x - (double)k;
	if (f < 0.0) {				/* negative x: borrow */
		f += 1.0;
		k -= 1;
	}
	p = 1.0 + f * (c1 + f * (c2 + f * (c3 + f * (c4 + f * (c5 +
	    f * (c6 + f * c7))))));

	while (k > 1023)  { p *= 8.98846567431158e307;    k -= 1023; }
	while (k < -1022) { p *= 2.2250738585072014e-308; k += 1022; }

	s.u = ((uint64_t)(k + 1023)) << 52;
	return p * s.d;
}
