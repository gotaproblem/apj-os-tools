/*
 * tests/psctrl/harness.c - build APJSKIN and PSCTRL's drawing code on the
 * host, run every tab against a fake 32 bpp screen, and check every blit.
 *
 * Same fake VDI as tests/skin (../stubvdi.h). What this one adds is that
 * PSCTRL's window is built from a DESCRIPTOR TABLE rather than from a
 * fixed layout, so the thing worth testing is that an arbitrary table -
 * every kind, every class, a tab with one row and a tab with eighty -
 * lays out and draws without reading outside the sheet or writing outside
 * the window. The model below is a stand-in for the emulator's table and
 * deliberately includes the awkward cases.
 *
 *   ./harness <file.SKN> <out.ppm>      -> 0 ok, 1 a check failed
 *
 * The PPM is the JIT tab, which is the one with the benchmark panel and
 * the most kinds of control on it.
 */
#define APJGUI_IMPL

#include "../stubvdi.h"
#include "../../psctrl/psui.h"

/* --------------------------------------------------------- the model -- */

static PSROW rows[PS_MAXROW];
static short nrows;

static void mk(const char *name, const char *title, short tab, short kind,
               short klass, short unit, long lo, long hi, long step,
               long value, const char *labels)
{
	PSROW *r = &rows[nrows++];

	memset(r, 0, sizeof(*r));
	strcpy(r->name, name);
	strcpy(r->title, title);
	r->tab = tab; r->kind = kind; r->klass = klass; r->unit = unit;
	r->min = lo; r->max = hi; r->step = step; r->value = value;
	if (labels)
	{
		const char *p = labels;

		while (*p && r->nenum < PS_MAXENUM)
		{
			strncpy(r->label[r->nenum], p, sizeof(r->label[0]) - 1);
			r->label[r->nenum][sizeof(r->label[0]) - 1] = '\0';
			r->nenum++;
			p += strlen(p) + 1;
		}
	}
}

static void build_model(void)
{
	short i;

	nrows = 0;

	/* JIT: a slider whose value is special-cased, two enums (one small,
	 * one that has to become a popup), an action and two readouts */
	mk("jit_power", "JIT power", PS_TAB_JIT, PS_K_INT, PS_C_LIVE,
	   PS_U_NONE, 0, 6, 1, 3, NULL);
	mk("m68k_speed", "68k speed", PS_TAB_JIT, PS_K_INT, PS_C_LIVE,
	   PS_U_NONE, -1, 20, 1, -1, NULL);
	mk("jit_cache", "Translation cache", PS_TAB_JIT, PS_K_ENUM, PS_C_DEFER,
	   PS_U_NONE, 0, 4, 1, 3, "off\0" "2048K\0" "4096K\0" "8192K\0" "16384K\0");
	mk("compnf", "Skip dead flags", PS_TAB_JIT, PS_K_BOOL, PS_C_DEFER,
	   PS_U_NONE, 0, 1, 1, 1, "off\0on\0");
	mk("jit_flush", "Flush cache now", PS_TAB_JIT, PS_K_ACTION, PS_C_DEFER,
	   PS_U_NONE, 0, 0, 0, 0, NULL);
	mk("jit_hitrate", "JIT hit rate", PS_TAB_JIT, PS_K_INFO, PS_C_RO,
	   PS_U_X100, 0, 0, 0, 9987, NULL);
	mk("jit_cache_used", "Cache used", PS_TAB_JIT, PS_K_INFO, PS_C_RO,
	   PS_U_KB, 0, 0, 0, 1204, NULL);

	/* CPU: the six-way enum that must come out as a popup, and a string */
	mk("cpu", "CPU", PS_TAB_CPU, PS_K_ENUM, PS_C_BOOT, PS_U_NONE, 0, 5, 1, 3,
	   "68000\0" "68010\0" "68020\0" "68030\0" "68040\0" "68060\0");
	mk("machine", "Machine", PS_TAB_CPU, PS_K_ENUM, PS_C_BOOT, PS_U_NONE,
	   0, 2, 1, 1, "st\0ste\0megast\0");
	mk("rom", "TOS image", PS_TAB_CPU, PS_K_STR, PS_C_BOOT, PS_U_NONE,
	   0, 0, 0, 0, NULL);
	strcpy(rows[nrows - 1].str,
	       "/home/pistorm/roms/a-very-long-tos-image-name-206uk.img");
	mk("blit_timed_ns", "Blitter bus cost", PS_TAB_CPU, PS_K_INT, PS_C_LIVE,
	   PS_U_NS, 0, 2000, 50, 0, NULL);

	/* Video / Audio: units that are not plain integers */
	mk("fps", "Host frame rate", PS_TAB_VIDEO, PS_K_INT, PS_C_LIVE,
	   PS_U_HZ, 10, 60, 1, 60, NULL);
	mk("vbl_refract_ns", "VBL refractory", PS_TAB_VIDEO, PS_K_INT,
	   PS_C_LIVE, PS_U_NS, 0, 200000, 1000, 5000, NULL);
	mk("ym_gain", "YM gain", PS_TAB_AUDIO, PS_K_INT, PS_C_LIVE,
	   PS_U_X100, 0, 400, 5, 125, NULL);
	mk("lmc", "LMC1992 shadow", PS_TAB_AUDIO, PS_K_BOOL, PS_C_LIVE,
	   PS_U_NONE, 0, 1, 1, 1, "off\0on\0");

	/* Floppy: string rows and actions, the busy/refused paths */
	mk("floppy_a", "Drive A:", PS_TAB_FLOPPY, PS_K_STR, PS_C_LIVE,
	   PS_U_NONE, 0, 0, 0, 0, NULL);
	strcpy(rows[nrows - 1].str, "GAME_D1.ST");
	mk("floppy_swap", "Swap A: <-> B:", PS_TAB_FLOPPY, PS_K_ACTION,
	   PS_C_LIVE, PS_U_NONE, 0, 0, 0, 0, NULL);

	/* ST Box has exactly one row: the degenerate case for the scrollbar */
	mk("stbox_running", "Box running", PS_TAB_STBOX, PS_K_INFO, PS_C_RO,
	   PS_U_NONE, 0, 0, 0, 0, NULL);

	/* Debug: long enough to need scrolling, and a three-way enum */
	mk("hostfs_debug", "HOSTFS trace", PS_TAB_DEBUG, PS_K_ENUM, PS_C_LIVE,
	   PS_U_NONE, 0, 2, 1, 0, "follow cfg\0off\0on\0");
	for (i = 0; i < 40 && nrows < PS_MAXROW - 1; i++)
	{
		char nm[24], ti[40];

		sprintf(nm, "dbg_%d", (int) i);
		sprintf(ti, "A debug flag with a fairly long name %d", (int) i);
		mk(nm, ti, PS_TAB_DEBUG, PS_K_BOOL, PS_C_LIVE, PS_U_NONE,
		   0, 1, 1, (i & 1), "off\0on\0");
	}

	/* Adv is left EMPTY on purpose - a tab with no rows at all */
}

/*
 * Benchmark results, as the window will get them. Three states worth
 * drawing: never run, a normal run with a previous one beside it, and a
 * run where every figure is at its widest - which is the one that finds
 * a value overflowing its column.
 */
static PSBENCH b_none, b_cur, b_prev, b_wide;

static void mk_bench(PSBENCH *b, int wide)
{
	memset(b, 0, sizeof(*b));
	b->ran = 1;
	b->have_fpu = 1;
	b->stram_ok = 1;
	b->dhry_per_s      = wide ? 99999999L : 541638L;
	b->dmips_x100      = wide ? 999999L : 30827L;
	b->vs_st_x10       = wide ? 99999L : 3868L;
	b->mips_alu_x100   = wide ? 999999L : 129891L;
	b->mips_mixed_x100 = wide ? 1148L : 23893L;	/* wide: the not-compiled case */
	b->mips_stram_x100 = wide ? 999999L : 8192L;
	b->mflops_x100     = wide ? 999999L : 41040L;
	b->bogo_x100       = wide ? 999999L : 14194L;
	b->secs_x10        = 205;

	b->cm_ran        = 1;
	b->cm_valid      = 1;
	b->cm_target     = wide ? PSB_CM_68020 : PSB_CM_68000;
	b->cm_score_x100 = wide ? 99999999L : 105097L;
	b->cm_iterations = 4000L;
	b->cm_secs_x10   = wide ? 9 : 190;	/* wide: under the 10 s minimum */
	strcpy(b->cm_line, "CoreMark 1.0 : 12.40 / GCC4.6.4 -O2 -m68020-60 "
	                   "/ STACK");

	b->g.ran      = 1;
	b->g.w        = PSB_3D_W;
	b->g.h        = PSB_3D_H;
	b->g.frames   = wide ? 128000L : 3712L;
	b->g.ms_total = wide ? 999999L : 1855L;
	b->g.us_frame = wide ? 999999L : 499L;
	b->g.ms_raster= wide ? 888888L : 1640L;
	b->g.ms_conv  = 0;
	b->g.ms_blit  = wide ? 111111L : 215L;
	b->g.fps_x10  = wide ? 99999L : 20010L;
	b->g.ktris_s  = wide ? 999999L : 89758L;
	b->g.kpix_s   = wide ? 999999L : 13059L;
	b->g.blit_kb_s= wide ? -1L : 12948L;	/* wide: too fast to time */
	b->g.bpp      = 32;
	b->flushes_run = wide ? 9L : 0L;
	/* the bisection: realistic = the 8.77 MIPS run, where movem is the
	 * jump; wide = every figure five digits */
	b->mix_ns[0]   = wide ? 99999L : 44L;
	b->mix_ns[1]   = wide ? 99999L : 58L;
	b->mix_ns[2]   = wide ? 99999L : 67L;
	b->mix_ns[3]   = wide ? 99999L : 101L;
	b->mix_ns[4]   = wide ? 99999L : 1995L;
	b->g.shown    = wide ? 0 : 3712L;	/* wide: nothing reached the screen */
}

/*
 * Text is drawn by the AES, so it never reaches the framebuffer and the
 * blit checks are blind to it. These are the two rules it has to obey:
 * nothing outside the window, and nothing on the JIT tab that overlaps
 * the benchmark group box may stick out of it. The second one is here
 * because an unwrapped Dhrystone line did exactly that on hardware.
 */
static void check_text(PSUI *u, short tab)
{
	GRECT b;
	short i, haveb = psui_bench_rect(u, &b);

	for (i = 0; i < ntxt; i++)
	{
		short x0 = txt[i].x, y0 = txt[i].y;
		short x1 = (short) (x0 + txt[i].w), y1 = (short) (y0 + txt[i].h);

		if (x0 < u->work.g_x || y0 < u->work.g_y ||
		    x1 > u->work.g_x + u->work.g_w ||
		    y1 > u->work.g_y + u->work.g_h)
		{
			printf("  tab %d: \"%s\" at %d,%d %dx%d leaves the window\n",
			       tab, txt[i].s, x0, y0, txt[i].w, txt[i].h);
			fail("text outside the window", x1, u->work.g_x + u->work.g_w);
		}
		if (haveb && y0 < b.g_y + b.g_h && y1 > b.g_y &&
		    (x0 < b.g_x || x1 > b.g_x + b.g_w ||
		     y0 < b.g_y || y1 > b.g_y + b.g_h))
		{
			printf("  tab %d: \"%s\" at %d,%d %dx%d leaves the "
			       "benchmark box %d,%d %dx%d\n", tab, txt[i].s,
			       x0, y0, txt[i].w, txt[i].h,
			       b.g_x, b.g_y, b.g_w, b.g_h);
			fail("benchmark text outside its box", x1, b.g_x + b.g_w);
		}
	}
}

/* --------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
	PSUI u;
	short vh = 1, W, H, mw, mh, t;
	long i, worst = 0;
	FILE *o;

	if (argc < 3)
	{
		fprintf(stderr, "usage: harness <file.SKN> <out.ppm>\n");
		return 2;
	}
	if (!apj_skin_load(vh, argv[1]))
	{
		fprintf(stderr, "could not load %s\n", argv[1]);
		return 2;
	}
	apj_init(vh);
	if (!apj_on)
		fail("theme pens not loaded", 0, 0);

	printf("scale %d%%\n", apj_skin_scale());

	/*
	 * Version 2 regions. A sheet without them is not an error - the
	 * engine says so and the app falls back - but this harness is
	 * pointless against one, so say which it is rather than silently
	 * testing the fallback.
	 */
	{
		static const short need[] = { APJ_RG_TAB, APJ_RG_TABBAR, APJ_RG_RADIO,
		                              APJ_RG_CHECK, APJ_RG_FIELD, APJ_RG_POPUP,
		                              APJ_RG_STATUS };
		static const char *nm[] = { "TAB", "TABBAR", "RADIO", "CHECK",
		                            "FIELD", "POPUP", "STATUS" };
		int k, missing = 0;

		for (k = 0; k < 7; k++)
			if (!apj_skin_has(need[k]))
			{
				fprintf(stderr, "sheet has no %s region\n", nm[k]);
				missing++;
			}
		if (missing)
		{
			fail("sheet is version 1 - rebuild it with mkskin.py", missing, 0);
			return 1;
		}
	}

	build_model();
	psui_minsize(&mw, &mh);
	printf("minimum window %dx%d, %d rows in the model\n", mw, mh, nrows);

	W = apj_skin_m(620);
	H = apj_skin_m(510);
	if (W < mw || H < mh)
		fail("620x510pt is under the minimum", W, mw);

	for (i = 0; i < (long) SCR_W * SCR_H; i++)
	{
		scr[i*4+0] = 0; scr[i*4+1] = 0xff; scr[i*4+2] = 0x00; scr[i*4+3] = 0xff;
	}

	memset(&u, 0, sizeof(u));
	u.rows = rows;
	u.nrows = nrows;
	u.hover = PSW_NONE;
	u.press = PSW_NONE;
	u.openrow = -1;
	u.status = "12 settings, API 1. Live changes take effect at once.";
	u.host = 1;
	memset(&b_none, 0, sizeof(b_none));
	mk_bench(&b_cur, 0);
	mk_bench(&b_prev, 0);
	b_prev.dmips_x100 = 821;
	b_prev.cm_score_x100 = 1090;
	b_prev.g.fps_x10 = 154;
	mk_bench(&b_wide, 1);
	u.bench = &b_cur;
	u.bprev = &b_prev;
	u.bnote = psbench_cm_note(&b_cur);
	u.cmmax = PSB_CM_68020;
	u.cmtarget = PSB_CM_68020;
	memset(&b_none, 0, sizeof(b_none));
	mk_bench(&b_cur, 0);
	mk_bench(&b_prev, 0);
	b_prev.dmips_x100 = 821;
	b_prev.cm_score_x100 = 1090;
	b_prev.g.fps_x10 = 154;
	mk_bench(&b_wide, 1);
	u.bench = &b_cur;
	u.bprev = &b_prev;
	u.bnote = psbench_cm_note(&b_cur);
	u.cmmax = PSB_CM_68020;
	u.cmtarget = PSB_CM_68020;

	/* --- every tab, including the empty one and the one-row one ----- */
	for (t = 0; t < PS_TAB_N; t++)
	{
		long before = blits;

		u.tab = t;
		u.top = 0;
		psui_layout(&u, vh, 40, 40, W, H);
		txt_reset();
		psui_draw(&u, vh);
		check_text(&u, t);
		printf("  %-7s %2d rows, %2d visible, %4ld blits\n",
		       psui_tab_name(t), u.nidx, u.visrows, blits - before);
		if (blits - before > worst)
			worst = blits - before;

		/*
		 * The Bench tab has three shapes, not one: nothing run yet,
		 * a run with a previous run beside it, and every figure at
		 * its widest. The last is the one that catches a value
		 * running out of its column, and it is checked with AND
		 * without a previous run - dropping that column widens the
		 * value column, so a different string is the one at risk.
		 */
		if (t == PS_TAB_BENCH)
		{
			const PSBENCH *models[4];
			const PSBENCH *prevs[4];
			short k;

			models[0] = &b_none; prevs[0] = NULL;
			models[1] = &b_wide; prevs[1] = &b_prev;
			models[2] = &b_wide; prevs[2] = NULL;
			/* the one that has to look right: real figures from
			 * a 68040 at 1.5 GHz, with a previous run beside it */
			models[3] = &b_cur;  prevs[3] = &b_prev;

			for (k = 0; k < 4; k++)
			{
				u.bench = models[k];
				u.bprev = prevs[k];
				u.bnote = psbench_cm_note(models[k]);
				psui_layout(&u, vh, 40, 40, W, H);
				txt_reset();
				psui_draw(&u, vh);
				check_text(&u, t);
				/*
				 * Every row psbench produced has to be ON the
				 * screen. One that runs off the bottom draws
				 * nothing, so it is invisible to the text
				 * check - and a benchmark quietly missing its
				 * last figure is worse than one that says it
				 * could not fit.
				 */
				if (u.bdrawn != u.bwanted)
					printf("  model %d: %d of %d bench rows "
					       "drawn\n", k, u.bdrawn, u.bwanted);
				if (u.bdrawn != u.bwanted)
					fail("benchmark rows fell off the panel",
					     u.bdrawn, u.bwanted);
				/*
				 * And nothing may be ELLIPSISED. A cut value
				 * draws correctly and stays inside its box,
				 * so every other check here passes while the
				 * reader is shown "x386.8 S" for "x386.8 ST".
				 * Model 0 has nothing to show and models 1
				 * and 2 are deliberately absurd; model 3 is
				 * the realistic one and it has to fit.
				 */
				if (k == 3 && u.btrunc)
					fail("a benchmark value was cut short",
					     u.btrunc, 0);
			}
			u.bench = &b_cur;
			u.bprev = &b_prev;
			u.bnote = psbench_cm_note(&b_cur);
			psui_layout(&u, vh, 40, 40, W, H);
		}
		/* scrolled to the very bottom, where an off-by-one shows up */
		if (psui_scroll_needed(&u))
		{
			u.top = (short) (u.nidx - u.visrows);
			psui_layout(&u, vh, 40, 40, W, H);
			psui_draw(&u, vh);
		}
	}
	printf("worst tab: %ld blits, %ld pixels moved in all\n", worst, pixels);
	if (worst > 400)
		fail("too many blits for one redraw", worst, 400);

	/* --- a thin strip must be much cheaper than a whole redraw ------ */
	u.tab = PS_TAB_DEBUG;
	u.top = 0;
	psui_layout(&u, vh, 40, 40, W, H);
	{
		GRECT strip;
		long full;

		blits = 0;
		psui_draw(&u, vh);
		full = blits;
		strip.g_x = 40; strip.g_y = (short) (40 + H / 2);
		strip.g_w = W; strip.g_h = 6;
		blits = 0;
		psui_draw_clip(&u, vh, &strip);
		printf("6 px strip redraw: %ld blits (full was %ld)\n", blits, full);
		if (blits > full)
			fail("strip redraw cost more than a full one", blits, full);
	}

	/*
	 * The poll path. Twice a second the app re-reads the readouts on
	 * screen and repaints the ones whose TEXT changed - clipped to the
	 * value cell, so the row's ground, its title and its badge are not
	 * refilled underneath a figure that moved. That is what stopped the
	 * readouts flickering, and it is only true while one value cell
	 * stays a small fraction of a full redraw.
	 */
	{
		GRECT vr;
		long full, one;

		u.tab = PS_TAB_JIT;
		u.top = 0;
		psui_layout(&u, vh, 40, 40, W, H);
		blits = 0;
		psui_draw(&u, vh);
		full = blits;

		psui_value_rect(&u, 0, &vr);
		if (vr.g_w <= 0 || vr.g_h <= 0)
			fail("the value cell is empty", vr.g_w, vr.g_h);
		if (vr.g_x < u.work.g_x ||
		    vr.g_x + vr.g_w > u.work.g_x + u.work.g_w)
			fail("the value cell leaves the window", vr.g_x, vr.g_w);

		blits = 0;
		txt_reset();
		psui_draw_clip(&u, vh, &vr);
		one = blits;
		check_text(&u, PS_TAB_JIT);
		printf("one value cell: %ld blits (full tab was %ld)\n", one, full);
		if (one * 4 > full)
			fail("a value repaint costs too much of a full one",
			     one, full / 4);
	}

	/*
	 * Hover. Moving the pointer across the window changes at most two
	 * widgets - the one it left and the one it entered - and the app
	 * repaints exactly those. It used to repaint the row list and the
	 * tab strip on every boundary crossed, which is what made the whole
	 * window, and the desktop behind it, flicker under a moving mouse.
	 */
	{
		GRECT wr;
		long full, hov;
		short id;

		u.tab = PS_TAB_JIT;
		u.top = 0;
		u.hover = PSW_NONE;
		psui_layout(&u, vh, 40, 40, W, H);
		blits = 0;
		psui_draw(&u, vh);
		full = blits;

		/* a row, and a tab: the two kinds a moving pointer crosses */
		for (id = 0; id < 2; id++)
		{
			short w = id ? (short) (PSW_TAB0 + PS_TAB_CPU) : PSW_ROW0;

			if (!psui_widget_rect(&u, w, &wr))
			{
				fail("no rect for a widget the pointer can hover",
				     w, 0);
				continue;
			}
			if (wr.g_x < u.work.g_x || wr.g_y < u.work.g_y ||
			    wr.g_x + wr.g_w > u.work.g_x + u.work.g_w ||
			    wr.g_y + wr.g_h > u.work.g_y + u.work.g_h)
				fail("a widget rect leaves the window", wr.g_x, wr.g_y);

			u.hover = w;
			blits = 0;
			txt_reset();
			psui_draw_clip(&u, vh, &wr);
			hov = blits;
			check_text(&u, PS_TAB_JIT);
			printf("hover %s: %ld blits (full tab was %ld)\n",
			       id ? "a tab" : "a row", hov, full);
			if (hov * 3 > full)
				fail("a hover repaint costs too much of a full one",
				     hov, full / 3);
			u.hover = PSW_NONE;
		}
	}

	/*
	 * Changing a setting. The row's control and its badge change, and
	 * the status line says what the host answered - so a click repaints
	 * one row and one thin strip, never the window. Both are checked
	 * here against a full tab redraw, and the status strip is checked
	 * for being a strip: a "status" rectangle as tall as the window
	 * would pass a blit-count test and still repaint everything.
	 */
	{
		GRECT rr, sr;
		long full, row, stat;

		u.tab = PS_TAB_JIT;
		u.top = 0;
		u.hover = PSW_NONE;
		psui_layout(&u, vh, 40, 40, W, H);
		blits = 0;
		psui_draw(&u, vh);
		full = blits;

		if (!psui_widget_rect(&u, PSW_ROW0, &rr))
			fail("no rect for the first row", 0, 0);
		blits = 0;
		psui_draw_clip(&u, vh, &rr);
		row = blits;

		psui_status_rect(&u, &sr);
		if (sr.g_h <= 0 || sr.g_h > u.work.g_h / 4)
			fail("the status strip is not a strip", sr.g_h, u.work.g_h / 4);
		if (sr.g_y + sr.g_h != u.work.g_y + u.work.g_h)
			fail("the status strip is not at the bottom",
			     sr.g_y + sr.g_h, u.work.g_y + u.work.g_h);
		blits = 0;
		txt_reset();
		psui_draw_clip(&u, vh, &sr);
		stat = blits;
		check_text(&u, PS_TAB_JIT);

		printf("a setting change: %ld blits row + %ld status "
		       "(full tab was %ld)\n", row, stat, full);
		if ((row + stat) * 2 > full)
			fail("a setting change costs too much of a full redraw",
			     row + stat, full / 2);
	}

	/*
	 * The 3D test's stage. It is drawn INTO the benchmark panel while
	 * a run is going, so it has to be inside it and clear of the
	 * results table and the compliance line - a blit is not clipped by
	 * anything this file can see, and one landing on the table would
	 * be a mess nobody could explain.
	 */
	{
		GRECT g, st;

		u.tab = PS_TAB_BENCH;
		u.bench = &b_cur;
		u.bprev = &b_prev;
		u.bnote = psbench_cm_note(&b_cur);
		psui_layout(&u, vh, 40, 40, W, H);

		if (!psui_bench_rect(&u, &g))
			fail("no benchmark panel on the Bench tab", 0, 0);
		else if (!psui_bench_stage(&u, PSB_3D_W, PSB_3D_H, &st))
			fail("no room for the 3D stage at the natural size",
			     g.g_w, g.g_h);
		else
		{
			if (st.g_x < g.g_x || st.g_y < g.g_y ||
			    st.g_x + st.g_w > g.g_x + g.g_w ||
			    st.g_y + st.g_h > g.g_y + g.g_h)
				fail("the 3D stage is outside the panel",
				     st.g_y, g.g_y + g.g_h);
			printf("3D stage %dx%d at %d,%d in a panel %dx%d\n",
			       st.g_w, st.g_h, st.g_x, st.g_y, g.g_w, g.g_h);
		}
		u.tab = PS_TAB_JIT;
		psui_layout(&u, vh, 40, 40, W, H);
	}

	/* --- hit testing must agree with what was drawn ----------------- */
	{
		const APJ_LAY *l = apj_lay_find(u.lay, u.nlay, PSW_TAB0 + PS_TAB_DEBUG);

		if (!l)
			fail("no Debug tab in the layout", 0, 0);
		else if (psui_hit(&u, (short) (l->x + 2), (short) (l->y + 2))
		         != PSW_TAB0 + PS_TAB_DEBUG)
			fail("hit test missed the Debug tab", l->x, l->y);
		if (psui_hit(&u, 5, 5) != PSW_NONE)
			fail("hit test found a widget outside the window", 0, 0);

		/* the first visible row, and the control column inside it */
		l = apj_lay_find(u.lay, u.nlay, PSW_ROW0);
		if (!l)
			fail("no first row in the layout", 0, 0);
		else if (psui_row_at(&u, (short) (l->y + 2)) != 0)
			fail("row_at disagrees with the layout",
			     psui_row_at(&u, (short) (l->y + 2)), 0);

		/* a checkbox: clicking the control toggles it, clicking the
		 * label does nothing. Found by kind rather than by position,
		 * because which row is first is the model's business. */
		{
			short v;

			for (v = 0; v < u.visrows && u.top + v < u.nidx; v++)
				if (rows[u.idx[u.top + v]].kind == PS_K_BOOL)
				{
					const PSROW *r = &rows[u.idx[u.top + v]];
					GRECT rr;
					long got = r->value;

					psui_row_rect(&u, v, &rr);
					if (psui_click_value(&u, v, (short) (u.ctlx + 2),
					                     (short) (rr.g_y + 2), &got) != 1)
						fail("a click on a checkbox yielded nothing", v, 0);
					else if (got == r->value)
						fail("a checkbox click did not toggle", got, 0);
					got = r->value;
					if (psui_click_value(&u, v, (short) (rr.g_x + 2),
					                     (short) (rr.g_y + 2), &got) != 0)
						fail("a click on the label changed the value", v, 0);
					break;
				}
			if (v >= u.visrows)
				fail("no checkbox on the Debug tab to test", v, 0);
		}
	}

	/* --- the slider covers its whole range, ends included ----------- */
	{
		short v;

		u.tab = PS_TAB_JIT;
		u.top = 0;
		psui_layout(&u, vh, 40, 40, W, H);
		for (v = 0; v < u.visrows && u.top + v < u.nidx; v++)
			if (rows[u.idx[u.top + v]].kind == PS_K_INT)
			{
				const PSROW *r = &rows[u.idx[u.top + v]];
				GRECT rr;
				long lo = 0, hi = 0;

				psui_row_rect(&u, v, &rr);
				psui_click_value(&u, v, u.ctlx, (short) (rr.g_y + 2), &lo);
				psui_click_value(&u, v, (short) (u.ctlx + u.ctlw),
				                 (short) (rr.g_y + 2), &hi);
				if (lo != r->min)
					fail("slider left end is not min", lo, r->min);
				if (hi > r->max)
					fail("slider went past max", hi, r->max);
				break;
			}
	}

	/* --- a dropdown must stay inside the window -------------------- */
	{
		short v;

		u.tab = PS_TAB_CPU;
		u.top = 0;
		psui_layout(&u, vh, 40, 40, W, H);
		for (v = 0; v < u.visrows && u.top + v < u.nidx; v++)
			if (rows[u.idx[u.top + v]].kind == PS_K_ENUM &&
			    rows[u.idx[u.top + v]].nenum > 4)
			{
				GRECT d;

				psui_drop_rect(&u, v, &d);
				if (d.g_h <= 0)
					fail("popup row has no dropdown", v, 0);
				if (d.g_y < u.work.g_y ||
				    d.g_y + d.g_h > u.work.g_y + u.work.g_h)
					fail("dropdown falls outside the window", d.g_y, d.g_h);
				u.openrow = v;
				u.openpick = 2;
				psui_draw(&u, vh);
				u.openrow = -1;
				break;
			}
	}

	/* --- the picture: the JIT tab, benchmark panel and all ---------- */
	u.tab = PS_TAB_JIT;
	u.top = 0;
	u.hover = PSW_ROW0 + 1;
	psui_layout(&u, vh, 40, 40, W, H);
	psui_draw(&u, vh);

	o = fopen(argv[2], "wb");
	if (o)
	{
		fprintf(o, "P6\n%d %d\n255\n", W + 80, H + 80);
		for (i = 0; i < (long) (H + 80); i++)
		{
			long x;

			for (x = 0; x < W + 80; x++)
			{
				const unsigned char *p = scr + (i * SCR_W + x) * 4;

				fputc(p[1], o); fputc(p[2], o); fputc(p[3], o);
			}
		}
		fclose(o);
	}

	apj_skin_free();
	printf(fails ? "%d CHECK(S) FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
