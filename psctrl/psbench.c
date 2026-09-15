/*
 * psbench.c - see psbench.h.
 *
 * Timing is the 200 Hz system counter at $4BA, read through Supexec. It
 * is the only clock a GEM program can read on plain TOS as well as MiNT,
 * it is driven by the MFP rather than by anything the JIT does, and 5 ms
 * of resolution over a 400 ms window is a 1.2% quantisation - smaller
 * than the run-to-run spread, so a finer clock would be false precision.
 *
 * Every loop is CALIBRATED first: a short pass says roughly how fast this
 * machine is, and the real pass is sized from it to land on the target
 * window. That matters far more here than on real hardware, because
 * jit_power changes the answer by a factor of several and a fixed
 * iteration count would either take a minute or measure nothing.
 *
 * The first Dhrystone pass is thrown away. It is the pass in which the
 * JIT compiles the benchmark, and counting the compile in the score is
 * how you get a number that improves every time you press the button.
 */

#include <gem.h>
#include <osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psbench.h"
#include "ps3d.h"
#include "coremark/cm_shared.h"

/* dhry_1.c, built with -DDHRY_EMBEDDED */
extern void dhry_init(void);
extern void dhry_run(int n);
extern int  dhry_check(void);
extern int  dhry_ok(void);

/* psbench_fpu.c - its own translation unit because the FPU mnemonics
 * need -m68881, and gas will not take an .arch directive in the middle
 * of a file that has already assembled 68000 instructions. Only ever
 * CALLED when the host reports an FPU, so a 68000 never executes it. */
extern long psbench_fpu_loop(long iters);

/* --------------------------------------------------------------- clock */

#define HZ	200L

/*
 * The 200 Hz counter. Reading a fixed low address is what every TOS
 * program does and is not a null dereference, but a modern gcc cannot
 * tell the difference, so the warning is turned off for this one
 * function rather than silently left in the build output where it would
 * hide a real one. The cross-mint gcc is old enough not to warn at all,
 * which is why the pragma is guarded.
 */
#if defined(__GNUC__) && __GNUC__ >= 12
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Warray-bounds"
#endif
static long read_hz200(void)
{
	return *(volatile long *) 0x4BAL;
}
#if defined(__GNUC__) && __GNUC__ >= 12
# pragma GCC diagnostic pop
#endif

static long ticks(void)
{
	return Supexec(read_hz200);
}

/* --------------------------------------------------- the assembly loops */
/*
 * Instructions executed per outer iteration. These are counted, not
 * guessed: change a loop and change the constant beside it, because the
 * MIPS figure is (iterations x this) / seconds and nothing else checks it.
 */

#define ALU_PER	66	/* 64 ALU ops + subq + bne                        */
#define MIX_PER	70	/* 4 x (16-instruction unit + the rts it calls)
			 * = 68, + subq + bne                             */
/* One sweep is 512 dbra passes of 16 move.l, i.e. the whole 32 KB
 * buffer: 512 * 17. The sweep, not the pass, is the outer unit, because
 * the pointers have to go back to the start of the buffer somewhere and
 * an outer count that could run past the end of it is a crash waiting
 * for a fast enough machine. */
#define STR_SWEEP 512L
#define STR_PER	(STR_SWEEP * 17L)
#define FPU_PER	6	/* 4 FP ops + subq + bne                          */
/* dbra counts down a WORD, so one call can only ever be 65536 of them;
 * the outer count is sweeps of that, for the same reason as ST-RAM. */
#define BOGO_SWEEP 65536L
#define BOGO_PER BOGO_SWEEP

#define ALU4	"add.l %%d2,%%d1\n\tsub.l %%d3,%%d1\n\t" \
		"lsl.l #1,%%d1\n\teor.l %%d4,%%d1\n\t"
#define ALU16	ALU4 ALU4 ALU4 ALU4
#define ALU64	ALU16 ALU16 ALU16 ALU16

static void loop_alu(long iters)
{
	long c = iters;

	__asm__ volatile (
		"moveq	#1,%%d1\n\t"
		"moveq	#3,%%d2\n\t"
		"moveq	#2,%%d3\n\t"
		"moveq	#7,%%d4\n"
		"1:\n\t"
		ALU64
		"subq.l	#1,%0\n\t"
		"bne	1b\n"
		: "+d" (c)
		:
		: "d1", "d2", "d3", "d4", "cc");
}

/*
 * The mixed loop, and the one to watch: ALU, a branch that is taken and
 * one that is not, a load and a store through ST-RAM, a jsr/rts pair and
 * a movem. It is the closest of these to a game's inner loop, and the one
 * jit_power visibly moves.
 *
 * The unit below is exactly sixteen instructions; the rts it jumps to is
 * a seventeenth. Four units per iteration.
 */
#define MIX16	"add.l	%%d2,%%d1\n\t"		/*  1 */ \
		"sub.l	%%d3,%%d1\n\t"		/*  2 */ \
		"move.l	(%1),%%d4\n\t"		/*  3 load  */ \
		"eor.l	%%d4,%%d1\n\t"		/*  4 */ \
		"move.l	%%d1,4(%1)\n\t"		/*  5 store */ \
		"lsl.l	#2,%%d1\n\t"		/*  6 */ \
		"cmp.l	%%d1,%%d1\n\t"		/*  7 sets Z */ \
		"bne	1f\n\t"			/*  8 not taken */ \
		"addq.l	#1,%%d1\n"		/*  9 */ \
		"1:\tbra 2f\n"		/* 10 taken */ \
		"2:\tjsr (%2)\n\t"		/* 11 (+ the rts) */ \
		"movem.l %%d1-%%d2,-(%%sp)\n\t"	/* 12 */ \
		"movem.l (%%sp)+,%%d1-%%d2\n\t"	/* 13 */ \
		"addq.l	#1,%%d1\n\t"		/* 14 */ \
		"subq.l	#1,%%d1\n\t"		/* 15 */ \
		"nop\n\t"			/* 16 */

static void mix_rts(void)
{
	/* the jsr target: a bare rts, so the pair costs what a call costs */
}

static void loop_mixed(long iters, void *buf)
{
	long c = iters;
	void *b = buf;
	void (*sub)(void) = mix_rts;

	__asm__ volatile (
		"moveq	#1,%%d1\n\t"
		"moveq	#3,%%d2\n\t"
		"moveq	#2,%%d3\n"
		"3:\n\t"
		MIX16 MIX16 MIX16 MIX16
		"subq.l	#1,%0\n\t"
		"bne	3b\n"
		: "+d" (c), "+a" (b), "+a" (sub)
		:
		: "d1", "d2", "d3", "d4", "cc", "memory");
}

/* One sweep = STR_SWEEP passes of 16 move.l = the whole buffer */
static void loop_stram(long sweeps, void *src, void *dst)
{
    while (sweeps-- > 0)
    {
	void *a = src, *d = dst;
	short n = (short) (STR_SWEEP - 1);

	__asm__ volatile (
		"1:\n\t"
		"move.l	(%1)+,(%2)+\n\t" "move.l (%1)+,(%2)+\n\t"
		"move.l	(%1)+,(%2)+\n\t" "move.l (%1)+,(%2)+\n\t"
		"move.l	(%1)+,(%2)+\n\t" "move.l (%1)+,(%2)+\n\t"
		"move.l	(%1)+,(%2)+\n\t" "move.l (%1)+,(%2)+\n\t"
		"move.l	(%1)+,(%2)+\n\t" "move.l (%1)+,(%2)+\n\t"
		"move.l	(%1)+,(%2)+\n\t" "move.l (%1)+,(%2)+\n\t"
		"move.l	(%1)+,(%2)+\n\t" "move.l (%1)+,(%2)+\n\t"
		"move.l	(%1)+,(%2)+\n\t" "move.l (%1)+,(%2)+\n\t"
		"dbra	%0,1b\n"
		: "+d" (n), "+a" (a), "+a" (d)
		:
		: "cc", "memory");
    }
}

/*
 * BogoMIPS, Linux style: dbra and nothing else. It is here because it is
 * the number people recognise, and it is absurd on purpose - the JIT
 * folds an empty countdown into a few ARM instructions, so this measures
 * the translator's ability to delete work rather than the machine's
 * ability to do it. Read it as a ceiling, never as a speed.
 */
static void loop_bogo(long sweeps)
{
	while (sweeps-- > 0)
	{
		short n = (short) (BOGO_SWEEP - 1);

		__asm__ volatile (
			"1:\tdbra %0,1b\n"
			: "+d" (n)
			:
			: "cc");
	}
}

/* ------------------------------------------------------------ measuring */

/*
 * Run fn(n) with n scaled so the timed pass lands near `target` ticks,
 * and return hundredths of a million instructions per second.
 * per = instructions per outer iteration.
 */
typedef void (*BFN)(long n, void *a, void *b);

/*
 * The mixed unit, built up a piece at a time. It exists because the
 * mixed figure collapsed from 239 MIPS to 9 on the same machine and the
 * same binary - eight microseconds an iteration, two thousand nanoseconds
 * a sixteen-instruction unit - and every instruction in it is in the
 * JIT's compile table.
 *
 * The first suspects were the two stack operations, the only things in
 * the benchmark that touch the stack. Timed alone they cost 32 ns for a
 * jsr/rts pair and 27 ns for a movem pair: 236 ns of the 7982, three per
 * cent. So the cost is not in any one instruction - it is in what one of
 * them does to the block AROUND it, and the way to find that is to add
 * them to the unit one at a time and watch which addition costs two
 * thousand nanoseconds.
 *
 * Five variants, cumulative. Same registers, same buffer, same four
 * units per iteration as the real thing; the fifth IS the real thing,
 * instruction for instruction, so its figure must agree with the Mixed
 * row - if it does not, the measurement is wrong, not the machine.
 */
/*
 * MIX16 as sixteen slots in MIX16's ORDER. A variant fills the slots of
 * the pieces it has and leaves the others empty, so the fifth - all
 * pieces - assembles to exactly the bytes of MIX16, in the same order.
 * The order matters: where the JIT ends a block depends on what is
 * next, so a reordered "same instructions" is a different test.
 */
#define MX_UNIT(M1, M2, B1, B2, J, V) \
		"add.l	%%d2,%%d1\n\t"		/*  1 */ \
		"sub.l	%%d3,%%d1\n\t"		/*  2 */ \
		M1				/*  3 load  */ \
		"eor.l	%%d4,%%d1\n\t"		/*  4 */ \
		M2				/*  5 store */ \
		"lsl.l	#2,%%d1\n\t"		/*  6 */ \
		B1				/*  7-8 cmp, bne */ \
		"addq.l	#1,%%d1\n\t"		/*  9 */ \
		B2				/* 10 bra */ \
		J				/* 11 jsr */ \
		V				/* 12-13 movem pair */ \
		"addq.l	#1,%%d1\n\t"		/* 14 */ \
		"subq.l	#1,%%d1\n\t"		/* 15 */ \
		"nop\n\t"			/* 16 */

#define P_M1	"move.l	(%1),%%d4\n\t"
#define P_M2	"move.l	%%d1,4(%1)\n\t"
#define P_B1	"cmp.l	%%d1,%%d1\n\t" "bne	1f\n\t"
#define P_B2	"1:\tbra 2f\n" "2:\t"
#define P_J	"jsr (%2)\n\t"
#define P_V	"movem.l %%d1-%%d2,-(%%sp)\n\t" "movem.l (%%sp)+,%%d1-%%d2\n\t"

#define MX1	MX_UNIT("",   "",   "",   "",   "",  "")	/* alu       */
#define MX2	MX_UNIT(P_M1, P_M2, "",   "",   "",  "")	/* +ld/st    */
#define MX3	MX_UNIT(P_M1, P_M2, P_B1, P_B2, "",  "")	/* +branches */
#define MX4	MX_UNIT(P_M1, P_M2, P_B1, P_B2, P_J, "")	/* +jsr/rts  */
#define MX5	MX_UNIT(P_M1, P_M2, P_B1, P_B2, P_J, P_V)	/* == MIX16  */

#define MX_LOOP(name, UNIT) \
static void name(long iters, void *buf, void *unused) \
{ \
	long c = iters; \
	void *b = buf; \
	void (*sub)(void) = mix_rts; \
	(void) unused; \
	__asm__ volatile ( \
		"moveq	#1,%%d1\n\t" "moveq	#3,%%d2\n\t" "moveq	#2,%%d3\n" \
		"3:\n\t" UNIT UNIT UNIT UNIT \
		"subq.l	#1,%0\n\t" "bne	3b\n" \
		: "+d" (c), "+a" (b), "+a" (sub) \
		: \
		: "d1", "d2", "d3", "d4", "cc", "memory"); \
}

MX_LOOP(f_mx1, MX1)
MX_LOOP(f_mx2, MX2)
MX_LOOP(f_mx3, MX3)
MX_LOOP(f_mx4, MX4)
MX_LOOP(f_mx5, MX5)

static const BFN mx_fn[PSB_MX_N] = { f_mx1, f_mx2, f_mx3, f_mx4, f_mx5 };

/*
 * Nanoseconds per iteration, rather than MIPS. Same calibration as
 * timed(): find a count that runs for about the target, then measure it.
 */
static long timed_ns(BFN fn, void *a, long target)
{
	long n = 2000L, t0, t1, dt;

	for (;;)
	{
		t0 = ticks();
		fn(n, a, NULL);
		t1 = ticks();
		dt = t1 - t0;
		if (dt >= 4L || n > 40000000L)
			break;
		n *= 8L;
	}
	if (dt > 0)
	{
		n = (n * target) / dt;
		if (n < 1L)
			n = 1L;
	}
	t0 = ticks();
	fn(n, a, NULL);
	t1 = ticks();
	dt = t1 - t0;
	if (dt <= 0)
		dt = 1;

	/* ns = dt/HZ seconds over n iterations, scaled to nanoseconds
	 * without overflowing: (dt * 1e9 / HZ) / n = dt * 5000000 / n */
	return (dt * 5000000L) / (n > 0 ? n : 1L);
}


static long timed(BFN fn, void *a, void *b, long per, long target)
{
	long n = 200L, t0, t1, dt;

	/* calibrate: double until a pass is long enough to measure at all */
	for (;;)
	{
		t0 = ticks();
		fn(n, a, b);
		t1 = ticks();
		dt = t1 - t0;
		if (dt >= 4L || n > 40000000L)
			break;
		n *= 8L;
	}
	if (dt > 0)
	{
		n = (n * target) / dt;
		if (n < 1L)
			n = 1L;
	}

	t0 = ticks();
	fn(n, a, b);
	t1 = ticks();
	dt = t1 - t0;
	if (dt <= 0)
		dt = 1;

	/*
	 * MIPS x100 = instructions * 100 * HZ / (dt * 1000000), and with
	 * HZ 200 that is simply instructions / (dt * 50). Done as a
	 * quotient and a remainder because n * per overflows a 32-bit long
	 * on the faster settings - which is exactly the case this whole
	 * dialog exists to explore.
	 */
	{
		long denom = dt * 50L;
		long q, r;

		if (denom < 1L)
			denom = 1L;
		q = n / denom;
		r = n % denom;
		return q * per + (r * per) / denom;
	}
}

/* adapters so every loop has the same shape */
static void f_alu(long n, void *a, void *b)   { (void) a; (void) b; loop_alu(n); }
static void f_mix(long n, void *a, void *b)   { (void) b; loop_mixed(n, a); }
static void f_str(long n, void *a, void *b)   { loop_stram(n, a, b); }
static void f_bogo(long n, void *a, void *b)  { (void) a; (void) b; loop_bogo(n); }
static void f_fpu(long n, void *a, void *b)   { (void) a; (void) b; psbench_fpu_loop(n); }

/* ---------------------------------------------------------------- run -- */

#define TARGET	80L	/* ticks per asm loop: 0.4 s                     */
#define DHRY_T	400L	/* ticks for the Dhrystone pass: 2 s             */

/* ---------------------------------------------------------- CoreMark -- */

/*
 * The two builds. Each is CoreMark's own main(), renamed by the Makefile
 * and with every other symbol hidden, so the two copies cannot see each
 * other. cm_shared.c holds the one capture buffer and the one clock they
 * both use - see coremark/cm_shared.h.
 */
extern int cm_run_000(void);
extern int cm_run_020(void);

/* pull a "label : number" out of CoreMark's own output. Returns -1 if
 * the line is not there, which is how a run that fell over is told from
 * one that scored zero. */
static long cm_field(const char *key)
{
	const char *p = strstr(cm_log, key);
	long v = 0;
	int any = 0;

	if (!p)
		return -1L;
	p += strlen(key);
	while (*p == ' ' || *p == ':')
		p++;
	while (*p >= '0' && *p <= '9')
	{
		v = v * 10L + (*p++ - '0');
		any = 1;
	}
	return any ? v : -1L;
}

static void cm_grab_line(char *out, long n)
{
	const char *p = strstr(cm_log, "CoreMark 1.0 :");
	long i = 0;

	out[0] = '\0';
	if (!p)
		return;
	while (p[i] && p[i] != '\n' && i < n - 1)
	{
		out[i] = p[i];
		i++;
	}
	out[i] = '\0';
}

static void run_coremark(PSBENCH *b, short target)
{
	long ticks_taken, iters;

	b->cm_target = target;
	b->cm_ran = 0;
	b->cm_valid = 0;
	b->cm_line[0] = '\0';

	cm_log_reset();
	if (target == PSB_CM_68020)
		cm_run_020();
	else
		cm_run_000();

	iters       = cm_field("Iterations       ");
	ticks_taken = cm_field("Total ticks      ");
	if (iters <= 0 || ticks_taken <= 0)
		return;			/* it did not get as far as reporting */

	b->cm_ran        = 1;
	b->cm_iterations = iters;
	b->cm_secs_x10   = (ticks_taken * 10L) / HZ;

	/*
	 * The score, computed here from the two integers rather than read
	 * from CoreMark's own printed float: iterations/sec, x100. Ordered
	 * to keep the intermediate inside a long - iterations can be
	 * millions and HZ is 200.
	 */
	b->cm_score_x100 = ((iters / ticks_taken) * HZ * 100L) +
	                   (((iters % ticks_taken) * HZ * 100L) / ticks_taken);

	b->cm_valid = strstr(cm_log, "Correct operation validated") ? 1 : 0;
	cm_grab_line(b->cm_line, (long) sizeof b->cm_line);
}

/* --------------------------------------------------------------- 3D -- */

/*
 * The graphics test. Rasterise into an off-screen byte buffer, hand it
 * to the caller to put on the screen, and time the two separately -
 * that split is the whole reason this exists, because on the PiSTorm the
 * blit goes through fVDI to a host memcpy and has nothing to do with the
 * JIT the rest of the benchmark is measuring.
 */
static void run_3d(PSBENCH *b, const PSB_SURF *surf, PSB_DRAW draw,
                   void *ctx, short w, short h)
{
	long t0, tr, tb, f, nframe, probe, pxbytes;

	memset(&b->g, 0, sizeof(b->g));
	if (w < 32 || h < 24)
	{
		b->g.why = PS3D_TOOSMALL;
		return;
	}
	if (!surf || !surf->buf || surf->stride < w)
	{
		b->g.why = PS3D_NOMEM;
		return;
	}
	if (surf->bpp != 8 && surf->bpp != 16 && surf->bpp != 32)
	{
		b->g.why = PS3D_NOSCR;
		return;
	}

	b->g.w = w;
	b->g.h = h;
	b->g.bpp = surf->bpp;
	pxbytes = (surf->bpp == 8) ? 1L : (surf->bpp == 16) ? 2L : 4L;

	/*
	 * A warm-up turn, thrown away. It is the pass in which the JIT
	 * compiles the rasteriser, exactly as the first Dhrystone pass is
	 * discarded for the same reason - and it doubles as the estimate
	 * of what a turn costs.
	 */
	t0 = ticks();
	for (f = 0; f < PS3D_TURN; f++)
	{
		long tris = 0, drawn = 0;

		memset(surf->buf, 0,
		       (size_t) ((long) surf->stride * h * pxbytes));
		ps3d_frame(surf->buf, w, h, surf->stride, surf->bpp,
		           (short) (f * 256L / PS3D_TURN), surf->shade16,
		           &tris, &drawn);
		if (draw)
			draw(surf->buf, w, h, surf->stride, ctx);
	}
	probe = ticks() - t0;
	if (probe < 1)
		probe = 1;

	/* whole turns, so the object always ends where it started */
	nframe = (PS3D_TURN * (long) PS3D_SECS * HZ) / probe;
	nframe = ((nframe + PS3D_TURN - 1) / PS3D_TURN) * PS3D_TURN;
	if (nframe < PS3D_TURN)
		nframe = PS3D_TURN;
	if (nframe > PS3D_TURN * 2000L)		/* a sane ceiling */
		nframe = PS3D_TURN * 2000L;

	/*
	 * TWO passes, not one interleaved.
	 *
	 * The obvious way is to read the clock either side of the raster
	 * and either side of the blit, every frame. It is also wrong here:
	 * the clock is the 200 Hz counter read through Supexec, so that is
	 * several supervisor traps a frame, and at eight hundred frames a
	 * second the instrument costs more than the thing it measures -
	 * and it lands INSIDE the split it is trying to establish. It
	 * cannot see a one-millisecond frame either: a tick is 5 ms.
	 *
	 * So: the long pass with the picture, a short one without, four
	 * clock reads in total, and the blit is the difference.
	 *
	 * There used to be a third pass, for converting the frame into the
	 * screen's format. There is nothing to convert now - the renderer
	 * draws screen pixels - which is the whole reason that step went:
	 * on hardware it was sixty percent of the frame, and none of it
	 * was work a real program would do.
	 */
	t0 = ticks();
	for (f = 0; f < nframe; f++)
	{
		long tris = 0, drawn = 0, npix;

		memset(surf->buf, 0,
		       (size_t) ((long) surf->stride * h * pxbytes));
		npix = ps3d_frame(surf->buf, w, h, surf->stride, surf->bpp,
		                  (short) ((f % PS3D_TURN) * 256L / PS3D_TURN),
		                  surf->shade16, &tris, &drawn);
		if (draw && draw(surf->buf, w, h, surf->stride, ctx))
			b->g.shown++;

		b->g.tris       += tris;
		b->g.tris_drawn += drawn;
		b->g.pixels     += npix;
		b->g.frames++;
	}
	tb = ticks() - t0;

	{
		long nr = ((nframe / 4 + PS3D_TURN - 1) / PS3D_TURN) * PS3D_TURN;

		if (nr < PS3D_TURN)
			nr = PS3D_TURN;

		t0 = ticks();
		for (f = 0; f < nr; f++)
		{
			long tris = 0, drawn = 0;

			memset(surf->buf, 0,
			       (size_t) ((long) surf->stride * h * pxbytes));
			ps3d_frame(surf->buf, w, h, surf->stride, surf->bpp,
			           (short) ((f % PS3D_TURN) * 256L / PS3D_TURN),
			           surf->shade16, &tris, &drawn);
		}
		tr = (ticks() - t0);
		tr = (tr * nframe) / nr;	/* scale to the long pass */
	}
	if (tr > tb)
		tr = tb;			/* noise: never a negative blit */

	b->g.ms_total  = (tb * 1000L) / HZ;
	b->g.ms_raster = (tr * 1000L) / HZ;
	b->g.ms_conv   = 0;
	b->g.ms_blit   = ((tb - tr) * 1000L) / HZ;
	if (b->g.frames > 0)
		b->g.us_frame = (b->g.ms_total * 1000L) / b->g.frames;

	if (b->g.ms_total > 0)
		b->g.fps_x10 = (b->g.frames * 10000L) / b->g.ms_total;
	if (b->g.ms_raster > 0)
	{
		b->g.ktris_s = (b->g.tris_drawn * 1000L) / b->g.ms_raster;
		b->g.kpix_s  = ((b->g.pixels / 1000L) * 1000L) / b->g.ms_raster;
	}
	if (b->g.ms_blit > 0)
		b->g.blit_kb_s = (((long) surf->stride * h * pxbytes / 1024L)
		                  * b->g.frames * 1000L) / b->g.ms_blit;
	else
		b->g.blit_kb_s = -1L;		/* too fast for this clock to see */
	b->g.ran = 1;
}

void psbench_run(PSBENCH *b, short have_fpu, short cm_target,
                 const PSB_SURF *surf, PSB_DRAW draw, void *ctx)
{
	long t0, t1, dt, n;
	void *sbuf = NULL, *dbuf = NULL;
	long started = ticks();

	memset(b, 0, sizeof(*b));
	b->have_fpu = have_fpu;

	/* --- Dhrystone ------------------------------------------------- */
	dhry_init();
	if (dhry_ok())
	{
		dhry_run(2000);			/* warm-up: the JIT compiles it here */

		n = 20000L;
		for (;;)
		{
			t0 = ticks();
			dhry_run((int) n);
			t1 = ticks();
			dt = t1 - t0;
			if (dt >= 4L || n > 20000000L)
				break;
			n *= 8L;
		}
		if (dt > 0)
			n = (n * DHRY_T) / dt;
		if (n < 1000L)
			n = 1000L;
		/* dhry_run takes an int, and on this target that is 32-bit,
		 * but Arr_2_Glob[8][7] accumulates Number_Of_Runs + 10 and the
		 * published warning about 16-bit overflow is worth honouring */
		if (n > 20000000L)
			n = 20000000L;

		t0 = ticks();
		dhry_run((int) n);
		t1 = ticks();
		dt = t1 - t0;
		if (dt <= 0)
			dt = 1;

		b->dhry_bad = (short) dhry_check();
		/* Dhry/s = n * HZ / dt, and n can be twenty million, so scale */
		b->dhry_per_s = (n / dt) * HZ + ((n % dt) * HZ) / dt;
		b->dmips_x100 = (b->dhry_per_s * 100L) / 1757L;
		b->vs_st_x10  = (b->dhry_per_s * 10L) / 1400L;
	}

	/* --- the assembly loops ---------------------------------------- */
	b->mips_alu_x100   = timed(f_alu,  NULL, NULL, ALU_PER, TARGET);

	/*
	 * The mixed and ST-RAM loops want ST-RAM specifically - that is
	 * what they are measuring. Mxalloc mode 0 is ST-RAM only; on a TOS
	 * without Mxalloc, Malloc gives ST-RAM anyway because there is
	 * nothing else.
	 */
	sbuf = (void *) Mxalloc(32768L, 0);
	dbuf = (void *) Mxalloc(32768L, 0);
	if (!sbuf || (long) sbuf == -32L)
		sbuf = (void *) Malloc(32768L);
	if (!dbuf || (long) dbuf == -32L)
		dbuf = (void *) Malloc(32768L);
	b->stram_ok = (sbuf && dbuf) ? 1 : 0;

	if (b->stram_ok)
	{
		memset(sbuf, 0x5A, 32768L);
		b->mips_mixed_x100 = timed(f_mix, sbuf, NULL, MIX_PER, TARGET);
		/* 512 dbra passes move the whole 32 KB; the loop is sized in
		 * passes, so the figure is instructions, not bytes */
		b->mips_stram_x100 = timed(f_str, sbuf, dbuf, STR_PER, TARGET);
	}

	if (have_fpu)
		b->mflops_x100 = timed(f_fpu, NULL, NULL, FPU_PER, TARGET) / 1L;

	b->bogo_x100 = timed(f_bogo, NULL, NULL, BOGO_PER, TARGET);

	/* what the mixed loop is made of: the unit built up a piece at a
	 * time, in ns per unit, against the same buffer the Mixed row used.
	 * Shorter passes than the headline rows - these are ratios */
	if (sbuf)
	{
		short i;

		for (i = 0; i < PSB_MX_N; i++)
			b->mix_ns[i] = timed_ns(mx_fn[i], sbuf, TARGET / 2) / 4L;
	}

	if (sbuf) Mfree(sbuf);
	if (dbuf) Mfree(dbuf);

	/* --- CoreMark --------------------------------------------------
	 * Last of the CPU work, because it is the long one: it sizes its
	 * own run to land past the ten seconds its rules require, and a
	 * shorter score is not reportable. */
	run_coremark(b, cm_target);

	/* --- graphics --------------------------------------------------- */
	run_3d(b, surf, draw, ctx, PSB_3D_W, PSB_3D_H);

	b->secs_x10 = ((ticks() - started) * 10L) / HZ;
	b->ran = 1;
}
