/*
 * tests/psmon/harness.c - build APJSKIN and PSMON's drawing on the host,
 * against a fake 32 bpp screen, and check every blit and every string.
 *
 *   ./harness <file.SKN> <out.ppm>      -> 0 ok, 1 a check failed
 *
 * The rules are the ones the other two harnesses check, plus the ones the
 * PSCTRL work added after they were learned on hardware:
 *
 *   - every blit S_ONLY, source size == destination size, source MFDB in
 *     device format and a multiple of 16 wide, nothing read outside the
 *     sheet, nothing written outside the window;
 *   - no TEXT outside the window, and no text outside the section box it
 *     belongs to - the AES draws text, so a pixel check cannot see this;
 *   - a repaint of one section must cost a fraction of the window, which
 *     is what stops the 500 ms poll flickering.
 *
 * The model below is deliberately awkward: absent readings, a machine
 * with no TT-RAM, a throttled Pi, and the widest strings each row can
 * produce.
 */
#define APJGUI_IMPL

#include "../stubvdi.h"
#include "../../psmon/psmonui.h"

/* --------------------------------------------------------- the models -- */

static void model_normal(PMDATA *d)
{
	memset(d, 0, sizeof(*d));
	d->host        = 1;
	d->khz         = 32000;
	d->hit_x10     = 964;
	d->idle_x10    = 120;
	d->cache_total = 8192L * 1024L;
	d->cache_used  = 3900L * 1024L;
	d->flushes     = 3;
	d->compiles    = 128;
	d->smc         = 0;
	d->st_total    = 4L * 1024L * 1024L;
	d->st_free     = 1200L * 1024L;
	d->tt_total    = 128L * 1024L * 1024L;
	d->tt_free     = 88L * 1024L * 1024L;
	d->soc_mc      = 62400;
	d->arm_khz     = 1500000;
	d->load_x100   = 180;
	d->throttled   = 0;
	d->pi_model    = 0x11;
	d->pi_ram_mb   = 4096;
	d->net         = 1 | 2 | (78L << 8);
	d->ipv4        = (192L << 24) | (168L << 16) | (1L << 8) | 23L;
	d->input       = 1 | 2 | 4 | 8 | (3L << 8);
	d->web_state   = 3;
	d->web_fps_x10 = 128;
	d->web_kbps    = 4200;
	d->web_rss_mb  = 412;
}

/* every reading absent: an emulator older than this build */
static void model_absent(PMDATA *d)
{
	memset(d, 0, sizeof(*d));
	d->host        = 1;
	d->khz = d->hit_x10 = d->idle_x10 = PM_NONE;
	d->cache_used = d->cache_total = PM_NONE;
	d->flushes = d->compiles = d->smc = PM_NONE;
	d->soc_mc = d->arm_khz = d->load_x100 = d->throttled = PM_NONE;
	d->pi_model = d->pi_ram_mb = PM_NONE;
	d->net = d->ipv4 = d->input = PM_NONE;
	d->web_state = d->web_fps_x10 = d->web_kbps = d->web_rss_mb = PM_NONE;
	d->st_total = 4L * 1024L * 1024L;
	d->st_free  = 512L * 1024L;
	d->tt_total = 0;			/* no TT-RAM: one memory row */
}

/* the widest each row can get, and a Pi in trouble */
static void model_wide(PMDATA *d)
{
	model_normal(d);
	d->khz         = 999000;
	d->hit_x10     = 1000;
	d->cache_used  = 16383L * 1024L;
	d->cache_total = 16384L * 1024L;
	d->flushes     = 999999;
	d->compiles    = 999999;
	d->smc         = 999999;
	d->st_total    = 14L * 1024L * 1024L;
	d->st_free     = 13L * 1024L * 1024L;
	d->tt_total    = 512L * 1024L * 1024L;
	d->tt_free     = 511L * 1024L * 1024L;
	d->pi_model    = 0x17;
	d->net         = 1 | 2 | (100L << 8);
	d->ipv4        = (255L << 24) | (255L << 16) | (255L << 8) | 255L;
	d->input       = 1 | 2 | 4 | 8 | (255L << 8);
	d->web_state   = 3;
	d->web_fps_x10 = 9999;
	d->web_kbps    = 999999;
	d->web_rss_mb  = 99999;
	d->soc_mc      = 85000;
	d->throttled   = 0x000f000fL;		/* everything, now and before */
	d->pi_ram_mb   = 8192;
}

/* no emulator at all */
static void model_nohost(PMDATA *d)
{
	model_absent(d);
	d->host = 0;
}

/* ------------------------------------------------------------- checks -- */

static void check_text(PMUI *u)
{
	short i, k;

	for (i = 0; i < ntxt; i++)
	{
		short x0 = txt[i].x, y0 = txt[i].y;
		short x1 = (short) (x0 + txt[i].w), y1 = (short) (y0 + txt[i].h);
		short inside = 0;

		if (x0 < u->work.g_x || y0 < u->work.g_y ||
		    x1 > u->work.g_x + u->work.g_w ||
		    y1 > u->work.g_y + u->work.g_h)
		{
			printf("  \"%s\" at %d,%d %dx%d leaves the window\n",
			       txt[i].s, x0, y0, txt[i].w, txt[i].h);
			fail("text outside the window", x1,
			     u->work.g_x + u->work.g_w);
			continue;
		}

		/* a string that overlaps a section box must be inside it -
		 * the status line is the only text below them all */
		for (k = 0; k < PM_SEC_N; k++)
		{
			GRECT g;

			if (!psmonui_sec_rect(u, k, &g))
				continue;
			if (y0 >= g.g_y + g.g_h || y1 <= g.g_y)
				continue;
			inside = 1;
			if (x0 < g.g_x || x1 > g.g_x + g.g_w ||
			    y0 < g.g_y || y1 > g.g_y + g.g_h)
			{
				printf("  \"%s\" at %d,%d %dx%d leaves section %d "
				       "%d,%d %dx%d\n", txt[i].s, x0, y0,
				       txt[i].w, txt[i].h, k,
				       g.g_x, g.g_y, g.g_w, g.g_h);
				fail("text outside its section", x1,
				     g.g_x + g.g_w);
			}
		}
		(void) inside;
	}
}

/* every line must actually say something: a row that silently formats to
 * an empty value is how a retired status index used to read as "0" */
static void check_lines(PMUI *u)
{
	char lines[PM_MAXLINE][64];
	short s, n, i;

	for (s = 0; s < PM_SEC_N; s++)
	{
		n = psmonui_sec_lines(u, s, lines, PM_MAXLINE);
		if (n <= 0)
			fail("a section produced no lines", s, 0);
		for (i = 0; i < n; i++)
		{
			const char *t1 = strchr(lines[i], '\t');
			const char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;

			if (!t1 || !t2)
			{
				fail("a line is not label/bar/value", s, i);
				continue;
			}
			if (t1 == lines[i])
				fail("a line has no label", s, i);
			if (!t2[1])
				fail("a line has no value", s, i);
		}
	}
}

/* --------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
	static PMDATA d;
	static PMUI u;
	short vh = 1, W, H, mw, mh;
	long full, one, worst = 0;
	int m;
	void (*models[4])(PMDATA *) = { model_normal, model_absent,
	                                model_wide, model_nohost };
	static const char *const mname[4] = { "normal", "absent", "wide",
	                                      "no host" };

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

	W = apj_skin_m(PM_W_PT);
	H = apj_skin_m(PM_H_PT);
	psmonui_minsize(&mw, &mh);
	printf("minimum window %dx%d, showing %dx%d\n", mw, mh, W, H);
	if (W < mw || H < mh)
		fail("the natural size is under the minimum", W, mw);

	memset(&u, 0, sizeof(u));
	u.d = &d;
	u.status = "Sampling every 500 ms.";

	for (m = 0; m < 4; m++)
	{
		long before = blits;

		models[m](&d);
		psmonui_layout(&u, vh, 40, 40, W, H);
		txt_reset();
		psmonui_draw(&u, vh);
		check_text(&u);
		check_lines(&u);
		/* the normal model's address is 192.168.1.23 - a negative long
		 * on the 68k, which once printed as "no address" */
		if (m == 0)
		{
			short i, seen = 0;

			for (i = 0; i < ntxt; i++)
				if (strstr(txt[i].s, "192.168.1.23"))
					seen = 1;
			if (!seen)
				fail("the Network row does not show the address", 0, 0);
		}
		/*
		 * Every line must be ON the window and WHOLE. A row that
		 * runs off the bottom draws nothing and an ellipsised value
		 * draws correctly, so neither is visible to a pixel or a
		 * text-extent check - and both were happening on hardware.
		 */
		if (u.rows_drawn != u.rows_wanted)
			fail("rows fell off the window", u.rows_drawn,
			     u.rows_wanted);
		if (u.trunc)
		{
			char lines[PM_MAXLINE][64];
			short sc, ln, k2;

			for (sc = 0; sc < PM_SEC_N; sc++)
			{
				ln = psmonui_sec_lines(&u, sc, lines, PM_MAXLINE);
				for (k2 = 0; k2 < ln; k2++)
					printf("    %s | %s\n", mname[m], lines[k2]);
			}
			fail("a value was cut short", u.trunc, 0);
		}
		printf("  %-8s %4ld blits, %d rows\n", mname[m],
		       blits - before, u.rows_drawn);
		if (blits - before > worst)
			worst = blits - before;
	}
	printf("worst model: %ld blits, %ld pixels moved in all\n", worst, pixels);
	if (worst > 200)
		fail("too many blits for one redraw", worst, 200);

	/* --- one section must be much cheaper than the window ----------- */
	model_normal(&d);
	psmonui_layout(&u, vh, 40, 40, W, H);
	blits = 0;
	psmonui_draw(&u, vh);
	full = blits;
	{
		GRECT g;

		if (!psmonui_sec_rect(&u, PM_SEC_ENGINE, &g))
			fail("no rect for the engine section", 0, 0);
		blits = 0;
		txt_reset();
		psmonui_draw_clip(&u, vh, &g);
		one = blits;
		check_text(&u);
		printf("one section: %ld blits (whole window was %ld)\n", one, full);
		if (one * 2 > full)
			fail("a section repaint costs too much of a full one",
			     one, full / 2);
	}

	/* --- and the window must survive being far too small ------------ */
	psmonui_layout(&u, vh, 40, 40, (short) (mw / 2), (short) (mh / 2));
	txt_reset();
	psmonui_draw(&u, vh);
	check_text(&u);

	model_normal(&d);
	psmonui_layout(&u, vh, 40, 40, W, H);
	psmonui_draw(&u, vh);

	{
		FILE *o = fopen(argv[2], "wb");
		long i;

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
	}

	apj_skin_free();
	printf(fails ? "%d CHECK(S) FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
