/*
 * psbench_fpu.c - the MFLOPS loop, in its own translation unit.
 *
 * The FPU mnemonics need -m68881, and gas will not accept an .arch
 * directive in the middle of a file that has already assembled 68000
 * instructions - which is why this is not simply another function in
 * psbench.c. The Makefile builds this one file with the FPU flags.
 *
 * Nothing here is CALLED unless the host reports a configured FPU
 * (PS_CFG_FPU_MODEL), which on this emulator implies a 68020 or better,
 * so a machine that could not execute these instructions never reaches
 * them.
 *
 * The chain is arranged to be exactly periodic:
 *
 *     x = ((x * 2.0) + 1.0) * 0.5 + (-0.5) = x
 *
 * so fp0 never drifts into the denormal or infinite ranges where an FPU
 * changes speed - a chain that overflows measures the exception path
 * rather than the multiplier.
 */

long psbench_fpu_loop(long iters)
{
	long c = iters;
	static const double two = 2.0, one = 1.0, half = 0.5, mhalf = -0.5;

	__asm__ volatile (
		"fmove.d	%1,%%fp0\n\t"
		"fmove.d	%2,%%fp1\n\t"
		"fmove.d	%3,%%fp2\n\t"
		"fmove.d	%4,%%fp3\n\t"
		"fmove.d	%5,%%fp4\n"
		"1:\n\t"
		"fmul.x	%%fp1,%%fp0\n\t"
		"fadd.x	%%fp2,%%fp0\n\t"
		"fmul.x	%%fp3,%%fp0\n\t"
		"fadd.x	%%fp4,%%fp0\n\t"
		"subq.l	#1,%0\n\t"
		"bne.s	1b\n"
		: "+d" (c)
		: "m" (one), "m" (two), "m" (one), "m" (half), "m" (mhalf)
		: "cc");
	return iters;
}
