/*
 * psbench.h - the benchmark on PSCTRL's JIT tab.
 *
 * It runs as 68k code inside the accessory, so it measures what programs
 * actually get on the settings the emulator is running RIGHT NOW. That is
 * the whole point: move jit_power, run it again, compare. A host-side
 * figure could not tell you that.
 *
 * Dhrystone is the headline because it is the one number with a published
 * Atari reference: 1400 Dhry/s (~0.8 DMIPS) for a stock 1040 at 8 MHz,
 * Dhrystone 2.1 built with the Vincent Riviere GCC 4.6.4 port, from the
 * FireBee Dhrystone table. "x N vs ST" divides by that. Everything under
 * it is an assembly loop that says WHERE a change came from.
 *
 * The Dhrystone source in dhry_1.c / dhry_2.c / dhry.h is Weicker's 2.1
 * unchanged apart from replacing main()'s stdio and times() harness; the
 * measurement loop and every Proc_/Func_ body are byte for byte the
 * published ones, which is what makes the comparison legitimate. Build
 * flags are fixed in the Makefile (-O2 -fomit-frame-pointer) so runs stay
 * comparable to each other and to the reference.
 *
 * CoreMark (coremark/) is EEMBC's, byte for byte, with only the porting
 * layer ours - see coremark/README-PORT.md. It is the one figure here
 * that can be compared with other people's machines, which is why it is
 * worth the ten seconds it insists on.
 *
 * The graphics half is ps3d.c: a shaded solid spun in the window, with
 * the rasterising and the blit to screen timed separately, because on
 * this machine they are different subsystems and one number cannot say
 * which of them was slow.
 */

#ifndef PSBENCH_H
#define PSBENCH_H

#include "ps3d.h"

/* which CoreMark build to run. The two exist because the whole question
 * is what the instruction set is worth: the same benchmark, the same
 * machine, compiled for a 68000 and for an 020-and-up. */
enum { PSB_CM_68000, PSB_CM_68020, PSB_CM_N };

/* the five cumulative variants of the mixed unit, see psbench.c */
#define PSB_MX_N 5

/*
 * The size the 3D test renders at. Fixed, not the size of whatever panel
 * it happens to be shown in: two runs have to do identical work or the
 * numbers cannot be compared, and a window someone resized between runs
 * would silently change the answer.
 */
#define PSB_3D_W	160
#define PSB_3D_H	120

typedef struct
{
	short ran;			/* 0 = never run                     */
	short dhry_bad;			/* 0 = every final value correct     */
	/*
	 * From the host's PS_CFG_FPU_MODEL. When it is set, the MFLOPS row
	 * is measuring the PI'S OWN FPU: jit_glue.cpp runs with fpu_mode 0
	 * (host doubles, not softfloat) and compfpu on, and the AArch64
	 * back end turns a guest fmul into a native FMUL_ddd. So a 68881
	 * instruction really does land on ARM floating-point hardware.
	 *
	 * The one caveat worth knowing: a real 68881/882/040 is 80-bit
	 * extended and ARM has no 80-bit type, so this path is 64-bit
	 * double. Fast, and not bit-identical to a real chip in the last
	 * few bits. Turning compfpu off on the JIT tab drops back to an
	 * interpretive call per F-line op - still host doubles, but with
	 * the interpreter's overhead on top, which is what that switch is
	 * for and what this row will show you.
	 */
	short have_fpu;
	short stram_ok;			/* an ST-RAM buffer was available    */

	long  dhry_per_s;
	long  dmips_x100;		/* Dhry/s / 1757                     */
	long  vs_st_x10;		/* Dhry/s / 1400, tenths             */

	long  mips_mixed_x100;
	long  mips_alu_x100;
	long  mips_stram_x100;
	long  mflops_x100;
	/*
	 * The dbra self-loop. NOT a ceiling, whatever it was called
	 * before: one 68k instruction per JIT block means every iteration
	 * pays a whole block epilogue, and on this JIT that is about
	 * eight AArch64 instructions with a load-modify-store to a global
	 * in the middle. It came out at a NINTH of the ALU loop on
	 * hardware. What it measures is block dispatch, which is what a
	 * tight loop in real code costs.
	 */
	long  bogo_x100;

	/*
	 * The mixed unit, built up a piece at a time: nanoseconds per
	 * sixteen-instruction unit for ALU only, then +load/store, then
	 * +branches, then +jsr/rts, then +movem - the last being MIX16
	 * itself, so mix_ns[4] must agree with mips_mixed_x100. The jump
	 * between two neighbours is what the added piece costs IN CONTEXT,
	 * which is not what it costs alone: a jsr/rts pair is 32 ns on its
	 * own and the mixed figure still collapsed twenty-fold. 0 = not run.
	 */
	long  mix_ns[PSB_MX_N];

	/* --- CoreMark ---------------------------------------------------
	 * cm_valid is CoreMark's own verdict on its own CRCs. A score
	 * without it means the run was wrong, not slow, and it must not be
	 * quoted. cm_secs_x10 under 100 means the run was shorter than the
	 * ten seconds the run rules require, so the score is real but not
	 * reportable - the tab says so. */
	short cm_target;		/* PSB_CM_*: which build ran         */
	short cm_valid;			/* CoreMark validated itself         */
	short cm_ran;
	long  cm_score_x100;		/* iterations/sec x 100              */
	long  cm_iterations;
	long  cm_secs_x10;
	char  cm_line[104];		/* the compliance line, verbatim     */

	/* --- graphics --------------------------------------------------- */
	PS3D  g;

	/*
	 * Hard JIT cache flushes that happened DURING the run, from the
	 * host's cumulative counter. A flush mid-run means some loops were
	 * measured while their blocks were being recompiled, and the
	 * figures from that run are not comparable with a clean one - so
	 * the tab says so rather than letting a bad number look like a
	 * setting change.
	 */
	long  flushes_run;

	long  secs_x10;			/* how long the whole run took       */
} PSBENCH;

/*
 * Runs the lot: Dhrystone and the assembly loops (about four seconds),
 * then CoreMark at the requested target (ten to fifteen - it sizes
 * itself past its own minimum), then the graphics test (about three).
 * Blocks. The caller puts up a busy bee, and draw() is called once per
 * 3D frame so the object can be seen turning while it is measured.
 */
/*
 * Put the frame on the screen. The renderer has already drawn it in the
 * screen's own pixel format, so this is one vro_cpyfm and nothing else -
 * which is the point: it isolates the fVDI path from the 68k work.
 *
 * Returns 1 if the frame reached the screen, 0 if it could not - no room
 * in the panel, no buffer, an unusable screen format. psbench counts
 * those, because "the 3D view is blank" has to be answerable from the
 * tab rather than by guessing.
 */
typedef short (*PSB_DRAW)(void *buf, short w, short h, short stride,
                          void *ctx);

/*
 * Where the frames are drawn, and in what. The app owns the buffer,
 * because only the app knows what the screen is; psbench only measures.
 * shade16 is sixteen pixel values in the screen's format.
 */
typedef struct
{
	void       *buf;
	short       stride;		/* pixels, a multiple of 16 */
	short       bpp;		/* 8, 16 or 32              */
	const long *shade16;
} PSB_SURF;

void psbench_run(PSBENCH *b, short have_fpu, short cm_target,
                 const PSB_SURF *surf, PSB_DRAW draw, void *ctx);

/* ------------------------------------------------------------ reporting */

enum { PSB_SEC_CPU, PSB_SEC_MEM, PSB_SEC_GFX, PSB_SEC_N };

typedef struct
{
	char  label[14];
	/*
	 * 64, not 40. At 40 the three longest values - the mixed-MIPS
	 * line, the frame-rate line and the throughput line - were cut at
	 * thirty-nine characters by the strncpy that fills this field,
	 * BEFORE the window ever saw them. No ellipsis, because strncpy
	 * does not add one, so it did not look like truncation and the
	 * drawing code's own width checks all passed on a string that had
	 * already lost its tail. `cut` exists so that can never be silent
	 * again.
	 */
	char  value[80];
	char  prev[24];			/* "" when there is no previous run  */
	short pct;			/* 0..100 for a bar, -1 for none     */
	short cut;			/* the text did not fit THIS struct  */
} PSBROW;

const char *psbench_sec_name(short sec);

/*
 * One section as rows, with the previous run's figures alongside. prev
 * may be NULL. Returns how many rows were written.
 */
short psbench_rows(const PSBENCH *cur, const PSBENCH *prev, short sec,
                   PSBROW *out, short max);

/* the CoreMark compliance line, or a note saying why there is not one */
const char *psbench_cm_note(const PSBENCH *b);

#endif /* PSBENCH_H */
