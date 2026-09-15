/*
 * psbrep.c - the benchmark's RESULTS, turned into rows for the window.
 *
 * Split out of psbench.c so it can be built on the host: psbench.c is
 * full of Mxalloc and Supexec and cannot be, and the strings are the
 * part most likely to be wrong in a way a person notices - a value that
 * overflows its column, a previous-run figure that says nothing, a
 * CoreMark score quoted without the run rules being met.
 *
 * Nothing in here runs anything or touches GEM.
 */

#include <stdio.h>
#include <string.h>

#include "psbench.h"

/* ------------------------------------------------------------ reporting */

/*
 * Rows, not lines. The window lays them out as label / bar / value /
 * previous, so the interesting comparison - change a setting, run it
 * again - is the one the eye lands on.
 *
 * The bar is only filled in where a 0..100 means something honest. That
 * is the graphics split: what share of a frame went on rasterising and
 * what share went on getting it to the screen. Putting a bar on a
 * Dhrystone score would need a full-scale value, and any number chosen
 * for that would be made up.
 */

static void x100(char *out, long v)
{
	sprintf(out, "%ld.%02ld", v / 100L, (v < 0 ? -v : v) % 100L);
}

static const char *const secname[PSB_SEC_N] =
{
	"Processor", "Memory", "Graphics"
};

const char *psbench_sec_name(short sec)
{
	return (sec >= 0 && sec < PSB_SEC_N) ? secname[sec] : "";
}

const char *psbench_cm_note(const PSBENCH *b)
{
	static char note[176];

	if (!b->ran || !b->cm_ran)
		return "CoreMark: not run.";
	if (!b->cm_valid)
		return "CoreMark FAILED ITS OWN CHECKS - the score is wrong, "
		       "not slow. Do not quote it.";
	if (b->cm_secs_x10 < 100)
	{
		sprintf(note, "%.60s   (ran %ld.%ld s - under the 10 s minimum, "
		              "so not reportable)", b->cm_line,
		        b->cm_secs_x10 / 10L, b->cm_secs_x10 % 10L);
		return note;
	}
	return b->cm_line[0] ? b->cm_line : "CoreMark: no compliance line.";
}

static const char *cm_target_name(short t)
{
	return (t == PSB_CM_68020) ? "020+" : "68000";
}

/* add a row; every field is optional except the label */
static short addrow(PSBROW *out, short n, short max, const char *lab,
                    const char *val, const char *prev, short pct)
{
	if (n >= max)
		return n;
	out[n].cut = 0;
	if (strlen(lab) >= sizeof(out[0].label) ||
	    strlen(val) >= sizeof(out[0].value) ||
	    (prev && strlen(prev) >= sizeof(out[0].prev)))
		out[n].cut = 1;		/* the harness fails on this */
	strncpy(out[n].label, lab, sizeof(out[0].label) - 1);
	out[n].label[sizeof(out[0].label) - 1] = '\0';
	strncpy(out[n].value, val, sizeof(out[0].value) - 1);
	out[n].value[sizeof(out[0].value) - 1] = '\0';
	strncpy(out[n].prev, prev ? prev : "", sizeof(out[0].prev) - 1);
	out[n].prev[sizeof(out[0].prev) - 1] = '\0';
	out[n].pct = pct;
	return (short) (n + 1);
}

short psbench_rows(const PSBENCH *cur, const PSBENCH *prev, short sec,
                   PSBROW *out, short max)
{
	char v[96], p[32], a[24];
	short n = 0;

	if (!cur || !cur->ran)
	{
		if (sec == PSB_SEC_CPU)
			n = addrow(out, n, max, "Not run",
			           "Press Benchmark. About 20 seconds.", "", -1);
		return n;
	}

	if (sec == PSB_SEC_CPU)
	{
		/* --- Dhrystone --- */
		x100(a, cur->dmips_x100);
		sprintf(v, "%s DMIPS  %ld Dhry/s  x%ld.%ld ST", a,
		        cur->dhry_per_s, cur->vs_st_x10 / 10L, cur->vs_st_x10 % 10L);
		p[0] = '\0';
		if (prev && prev->ran)
		{
			x100(a, prev->dmips_x100);
			sprintf(p, "%s DMIPS", a);
		}
		n = addrow(out, n, max, "Dhrystone", v, p, -1);
		if (cur->dhry_bad)
			n = addrow(out, n, max, "",
			           "WRONG RESULTS - do not trust the score", "", -1);

		/*
		 * A hard cache flush during the run means some of these
		 * numbers were taken while their code was being recompiled.
		 * Worth one row, because otherwise a bad figure looks like
		 * the setting you just changed.
		 */
		if (cur->flushes_run > 0)
		{
			sprintf(v, "%ld JIT cache flush%s during this run",
			        cur->flushes_run,
			        cur->flushes_run == 1 ? "" : "es");
			n = addrow(out, n, max, "Warning", v, "", -1);
		}

		/* --- CoreMark --- */
		if (cur->cm_ran)
		{
			x100(a, cur->cm_score_x100);
			sprintf(v, "%s  (%s build, %ld.%ld s)%s", a,
			        cm_target_name(cur->cm_target),
			        cur->cm_secs_x10 / 10L, cur->cm_secs_x10 % 10L,
			        cur->cm_valid ? "" : "  INVALID");
		}
		else
			strcpy(v, "did not run");
		p[0] = '\0';
		if (prev && prev->cm_ran)
		{
			x100(a, prev->cm_score_x100);
			sprintf(p, "%s %s", a, cm_target_name(prev->cm_target));
		}
		n = addrow(out, n, max, "CoreMark", v, p, -1);

		/* --- the assembly loops --- */
		x100(a, cur->mips_alu_x100);
		sprintf(v, "%s MIPS", a);
		p[0] = '\0';
		if (prev && prev->ran)
		{
			x100(a, prev->mips_alu_x100);
			sprintf(p, "%s MIPS", a);
		}
		n = addrow(out, n, max, "ALU", v, p, -1);

		if (cur->have_fpu)
		{
			x100(a, cur->mflops_x100);
			sprintf(v, "%s MFLOPS", a);
			p[0] = '\0';
			if (prev && prev->have_fpu)
			{
				x100(a, prev->mflops_x100);
				sprintf(p, "%s MFLOPS", a);
			}
			n = addrow(out, n, max, "FPU", v, p, -1);
		}
		else
			n = addrow(out, n, max, "FPU", "none configured", "", -1);

		/*
		 * The dbra self-loop. This was labelled BogoMIPS and called a
		 * ceiling, on the assumption that a one-instruction loop is
		 * the fastest thing a machine can do. On this JIT it is the
		 * SLOWEST - measured at a ninth of the ALU loop - and the
		 * reason is in the emitted code, not in the 68k:
		 *
		 * compemu_raw_endblock_pc_isconst() (codegen_arm64.cpp) ends
		 * every translated block by materialising the 64-bit address
		 * of `countdown`, loading it, subtracting the block's cycles,
		 * storing it back and testing the sign. That is about eight
		 * AArch64 instructions with a dependent load-modify-store to
		 * a global on the critical path. Amortised over a 66-op ALU
		 * block it is nothing. A block containing ONE 68k instruction
		 * pays all of it every iteration.
		 *
		 * So this row measures JIT BLOCK DISPATCH, which is what a
		 * tight loop in real code costs - and the difference between
		 * it and the ALU row is the per-block overhead itself, which
		 * is the number worth watching.
		 */
		x100(a, cur->bogo_x100);
		if (cur->bogo_x100 > 0 && cur->mips_alu_x100 > cur->bogo_x100)
		{
			long ns10 = 1000000L / cur->bogo_x100
			          - 1000000L / cur->mips_alu_x100;

			sprintf(v, "%s MIPS  (~%ld.%ld ns per block exit)", a,
			        ns10 / 10L, ns10 % 10L);
		}
		else
			sprintf(v, "%s MIPS  (one 68k op per JIT block)", a);
		p[0] = '\0';
		if (prev && prev->ran)
		{
			x100(a, prev->bogo_x100);
			sprintf(p, "%s MIPS", a);
		}
		n = addrow(out, n, max, "Tight loop", v, p, -1);
		return n;
	}

	if (sec == PSB_SEC_MEM)
	{
		if (!cur->stram_ok)
		{
			n = addrow(out, n, max, "ST RAM",
			           "no buffer available - not measured", "", -1);
			return n;
		}
		x100(a, cur->mips_stram_x100);
		sprintf(v, "%s MIPS  (move.l through ST RAM)", a);
		p[0] = '\0';
		if (prev && prev->stram_ok)
		{
			x100(a, prev->mips_stram_x100);
			sprintf(p, "%s MIPS", a);
		}
		n = addrow(out, n, max, "ST RAM", v, p, -1);

		/*
		 * The mixed loop is four jsr/rts pairs plus sixty-four ALU
		 * ops, so its blocks are long and it must always beat the
		 * one-instruction tight loop. When it does not, its code was
		 * not running compiled - the figure is the interpreter, not
		 * the setting - and saying so is the difference between a
		 * measurement and a mystery. Seen once on hardware: 11.48
		 * against 137.95, on a run where the cache had just been
		 * flushed.
		 */
		x100(a, cur->mips_mixed_x100);
		sprintf(v, "%s MIPS  (ALU, branch, ld/st, jsr)", a);
		p[0] = '\0';
		if (prev && prev->stram_ok)
		{
			x100(a, prev->mips_mixed_x100);
			sprintf(p, "%s MIPS", a);
		}
		n = addrow(out, n, max, "Mixed", v, p, -1);

		/*
		 * What the mixed loop is made of: the unit built up a piece
		 * at a time, in ns per unit, so the reader sees which addition
		 * cost the time. A jsr/rts pair costs 32 ns ALONE and the
		 * loop still collapsed twenty-fold, so the pieces are timed
		 * in context, cumulatively, not in isolation. The last figure
		 * is MIX16 itself and must agree with the Mixed row above.
		 *
		 * No previous column: the row needs the whole width, and the
		 * comparison that matters is between the five figures.
		 */
		if (cur->mix_ns[PSB_MX_N - 1] > 0)
		{
			static const char *const piece[PSB_MX_N] =
				{ "alu", "+ld/st", "+br", "+jsr", "+movem" };
			static const char *const loud[PSB_MX_N] =
				{ "alu", "+LD/ST", "+BR", "+JSR", "+MOVEM" };
			short i, worst = 0;
			long  jump = 0, base = cur->mix_ns[0];
			char *w = v;

			for (i = 1; i < PSB_MX_N; i++)
			{
				long d = cur->mix_ns[i] - cur->mix_ns[i - 1];

				if (d > jump)
				{
					jump = d;
					worst = i;
				}
			}
			/* the culprit is spelled in capitals - but only when it
			 * is one: a piece costing more than all the rest of
			 * the unit together. Otherwise nothing is singled out */
			if (!(jump > base + (cur->mix_ns[PSB_MX_N - 1] - jump)))
				worst = 0;
			for (i = 0; i < PSB_MX_N; i++)
				w += sprintf(w, "%s%s %ld", i ? " " : "",
				             (i && i == worst) ? loud[i] : piece[i],
				             cur->mix_ns[i]);
			strcpy(w, " ns");
			n = addrow(out, n, max, "Mix parts", v, "", -1);
		}
		return n;
	}

	/* --- graphics --- */
	if (!cur->g.ran)
	{
		static const char *const whynot[] = {
			"not run",
			"no room in the panel for the view",
			"could not get a frame buffer",
			"the screen format is not one this can draw to",
			"the view is too small to render"
		};
		short w = cur->g.why;

		if (w < 0 || w > PS3D_TOOSMALL)
			w = 0;
		n = addrow(out, n, max, "3D", whynot[w], "", -1);
		return n;
	}
	{
		long  tot = cur->g.ms_total > 0 ? cur->g.ms_total : 1;
		short rp = (short) ((cur->g.ms_raster * 100L) / tot);
		short bp = (short) ((cur->g.ms_blit * 100L) / tot);

		sprintf(v, "%ld.%ld fps  %ld us/frame  %dx%d %dbpp",
		        cur->g.fps_x10 / 10L, cur->g.fps_x10 % 10L,
		        cur->g.us_frame, (int) cur->g.w, (int) cur->g.h,
		        (int) cur->g.bpp);
		p[0] = '\0';
		if (prev && prev->g.ran)
			sprintf(p, "%ld.%ld fps", prev->g.fps_x10 / 10L,
			        prev->g.fps_x10 % 10L);
		n = addrow(out, n, max, "Frame rate", v, p, -1);

		/*
		 * The split, as a share of the frame. Not milliseconds per
		 * frame: the clock is 200 Hz and a frame here can be nearer
		 * one millisecond than five, so a per-frame figure in whole
		 * milliseconds reads "0 of 1" and says nothing. The totals
		 * are measured over thousands of frames and the percentage
		 * is what survives that.
		 */
		sprintf(v, "%d%%  (%ld of %ld ms)",
		        (int) rp, cur->g.ms_raster, cur->g.ms_total);
		p[0] = '\0';
		if (prev && prev->g.ran && prev->g.ms_total > 0)
			sprintf(p, "%ld%%",
			        (prev->g.ms_raster * 100L) / prev->g.ms_total);
		n = addrow(out, n, max, "Rasterise", v, p, rp);

		sprintf(v, "%d%%  (%ld ms, fVDI)",
		        (int) bp, cur->g.ms_blit);
		p[0] = '\0';
		if (prev && prev->g.ran && prev->g.ms_total > 0)
			sprintf(p, "%ld%%",
			        (prev->g.ms_blit * 100L) / prev->g.ms_total);
		n = addrow(out, n, max, "Blit", v, p, bp);

		/*
		 * Throughput, both ends, on one row: what the 68k pushed
		 * through the rasteriser and what came out the other side as
		 * screen bandwidth. Two rows for these was a row too many
		 * for the panel, and they answer the same question.
		 */
		if (cur->g.blit_kb_s >= 0)
			sprintf(v, "%ld ktri/s  %ld kpix/s  %ld KB/s",
			        cur->g.ktris_s, cur->g.kpix_s,
			        cur->g.blit_kb_s);
		else
			sprintf(v, "%ld ktri/s  %ld kpix/s  blit too fast",
			        cur->g.ktris_s, cur->g.kpix_s);
		p[0] = '\0';
		if (prev && prev->g.ran)
			sprintf(p, "%ld k tri/s", prev->g.ktris_s);
		n = addrow(out, n, max, "Throughput", v, p, -1);

		/*
		 * Whether the frames actually reached the screen. They are
		 * measured either way, so a blank view with good numbers is
		 * possible and has to be said out loud - which is exactly
		 * what happened when the source MFDB's plane count did not
		 * match the screen's and every blit silently did nothing.
		 */
		if (cur->g.shown < cur->g.frames)
		{
			sprintf(v, "%d of %ld frames reached the screen",
			        (int) cur->g.shown, cur->g.frames);
			n = addrow(out, n, max, "View", v, "", -1);
		}
	}
	return n;
}
