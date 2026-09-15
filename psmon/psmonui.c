/*
 * psmonui.c - see psmonui.h.
 *
 * Geometry is in POINTS through apj_skin_m(), so one source serves 1x,
 * 1.25x and 1.75x, the same rule as mp3ui.c and psui.c.
 *
 * The window is three group boxes, each a stack of rows:
 *
 *   | label ....... | bar ......... | value |
 *
 * A row with no meaningful 0..100 has no bar and its value runs from the
 * bar column instead, so the value column stays put down the window.
 *
 * Every figure can be absent. The host answers 0xFFFFFFFF for an index
 * it does not know, and an emulator older than this build does not know
 * all of them, so PM_NONE draws "n/a" rather than a plausible zero. The
 * old PSMON asked for indices 33..47, half of which the emulator has
 * since retired, and drew their absence as 0 - which is exactly the bug
 * this rule exists to prevent.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psmonui.h"

#define PAD		10	/* window inset, points */
#define GAP		8
#define ROWH		20
#define SECPAD		8	/* inside a group box   */
#define HEADH		18	/* a section's heading  */
#define STATH		18
#define BARH		8

#define M(pt)		apj_skin_m(pt)

enum { F_SMALL, F_BODY, F_TITLE };

static void ui_font(short vh, short kind)
{
	static const short ladder[3][3] =	/* 100%, 125%, 175% */
	{
		{ 10, 11, 13 },
		{ 11, 12, 15 },
		{ 13, 15, 20 }
	};
	short sc = apj_skin_ok() ? apj_skin_scale() : 100;
	short col = (sc >= 175) ? 2 : ((sc >= 125) ? 1 : 0);
	short d;

	if (kind < 0 || kind > F_TITLE)
		kind = F_BODY;
	vst_point(vh, ladder[kind][col], &d, &d, &d, &d);
}

static short cellw(short vh)
{
	short a[10];

	vqt_attributes(vh, a);
	return a[8] > 0 ? a[8] : 8;
}

static short cellh(short vh)
{
	short a[10];

	vqt_attributes(vh, a);
	return a[9] > 0 ? a[9] : 16;
}

static short vis(const PMUI *u, short x, short y, short w, short h)
{
	return !(x >= u->clip.g_x + u->clip.g_w || y >= u->clip.g_y + u->clip.g_h ||
	         x + w <= u->clip.g_x || y + h <= u->clip.g_y);
}

/* ------------------------------------------------------------ numbers -- */

/*
 * Everything below formats into a caller's buffer and nothing formats
 * straight to the screen. psmonui_sec_lines() hands the same strings to
 * the app, which compares them with what it last drew - so a reading
 * that moved without changing its text costs nothing.
 */

static void f_x10(char *out, long x10)		/* tenths of a percent */
{
	if (x10 < 0)
		strcpy(out, "n/a");
	else
		sprintf(out, "%ld.%01ld%%", x10 / 10L, x10 % 10L);
}

static void f_kb(char *out, long bytes)
{
	if (bytes < 0)
		strcpy(out, "n/a");
	else if (bytes >= 1048576L)
		sprintf(out, "%ld.%01ld MB", bytes / 1048576L,
		        ((bytes % 1048576L) * 10L) / 1048576L);
	else if (bytes >= 1024L)
		sprintf(out, "%ld KB", bytes / 1024L);
	else
		sprintf(out, "%ld B", bytes);
}

/* used/total as 0..100, in a way that cannot overflow a 32-bit long for
 * the sizes this shows (128 MB of TT-RAM is 2^27) */
static short f_pct(long used, long total)
{
	long p;

	if (total <= 0 || used <= 0)
		return 0;
	if (used >= total)
		return 100;
	if (used < 21000000L)
		p = (used * 100L) / total;
	else
		p = (used / 1024L) * 100L / (total / 1024L);
	return (short) (p < 0 ? 0 : (p > 100 ? 100 : p));
}

static const char *pi_name(long code)
{
	switch (code)
	{
	case 0x08: return "Pi 3B";
	case 0x0d: return "Pi 3B+";
	case 0x0e: return "Pi 3A+";
	case 0x11: return "Pi 4B";
	case 0x13: return "Pi 400";
	case 0x14: return "CM4";
	case 0x17: return "Pi 5";
	default:   return "Pi";
	}
}

/* ------------------------------------------------------------- layout -- */

/*
 * The same arithmetic psmonui_layout() uses, with every section at its
 * full row count - so a window opened at this size never has a section
 * clamped away. Kept next to the layout because the two drifting apart
 * is how a natural-size window ends up one row short of its content.
 */
void psmonui_minsize(short *w, short *h)
{
	*w = (short) (M(PAD) * 2 + M(SECPAD) * 2 + M(280));
	*h = (short) (M(PAD) * 2 +
	              M(HEADH) * PM_SEC_N +		/* three headings      */
	              M(SECPAD) * 2 * PM_SEC_N +	/* their inner padding */
	              M(ROWH) * (6 + 2 + 3) +		/* engine, memory, Pi  */
	              M(GAP) * (PM_SEC_N - 1) +
	              M(STATH));
}

/* how many rows each section draws; the memory one shrinks with no TT-RAM */
static short sec_rows(const PMUI *u, short sec)
{
	switch (sec)
	{
	case PM_SEC_ENGINE: return 6;
	case PM_SEC_MEMORY: return (u->d && u->d->tt_total > 0) ? 2 : 1;
	default:            return 3;
	}
}

void psmonui_layout(PMUI *u, short vh, short wx, short wy, short ww, short wh)
{
	short pad = M(PAD), gap = M(GAP), i, y, bh;

	if (!apj_skin_ok())
	{
		pad = 4;
		gap = 4;
	}

	u->work.g_x = wx; u->work.g_y = wy;
	u->work.g_w = ww; u->work.g_h = wh;
	u->clip = u->work;

	ui_font(vh, F_BODY);
	bh = cellh(vh);
	u->rowh = M(ROWH);
	if (u->rowh < bh + M(4))
		u->rowh = (short) (bh + M(4));

	/* the three columns. The label column is measured, not guessed:
	 * "SMC inv" and "Compile" are the longest and both must fit at
	 * every scale rather than being ellipsised at 100%. */
	/* ten cells: the longest label is "ARM clock" at nine, and a label
	 * that ends exactly where the bar starts reads as one thing */
	u->labw = (short) (cellw(vh) * 10);
	u->barw = M(72);
	{
		short x = (short) (wx + pad + M(SECPAD));
		short right = (short) (wx + ww - pad - M(SECPAD));

		u->barx = (short) (x + u->labw + gap);
		u->valx = (short) (u->barx + u->barw + gap);
		if (u->valx > right - M(70))
		{
			/* narrow window: the bar gives up its width first */
			u->barw = (short) (right - M(70) - gap - u->barx);
			if (u->barw < M(24))
				u->barw = 0;
			u->valx = (short) (u->barx + u->barw + (u->barw ? gap : 0));
		}
	}

	/*
	 * The sections, stacked, each as tall as its own row count - but
	 * every one clamped to the floor above the status strip. A window
	 * smaller than the minimum is the caller's business; drawing
	 * outside it is not, and the row loop only clips against its own
	 * box, so the box is where the truth has to be. A section with no
	 * room left gets zero height and draws nothing.
	 */
	{
		short floorr = (short) (wy + wh - M(STATH) - pad / 2);

		y = (short) (wy + pad);
		for (i = 0; i < PM_SEC_N; i++)
		{
			short h = (short) (M(HEADH) + sec_rows(u, i) * u->rowh +
			                   M(SECPAD) * 2);

			if (y + h > floorr)
				h = (short) (floorr - y);
			if (h < 0)
				h = 0;

			u->sec[i].g_x = (short) (wx + pad);
			u->sec[i].g_y = y;
			u->sec[i].g_w = (short) (ww - 2 * pad);
			u->sec[i].g_h = h;
			y = (short) (y + h + gap);
		}
	}
}

short psmonui_sec_rect(const PMUI *u, short sec, GRECT *r)
{
	if (sec < 0 || sec >= PM_SEC_N)
		return 0;
	*r = u->sec[sec];
	return (r->g_w > 0 && r->g_h > 0) ? 1 : 0;
}

/* --------------------------------------------------------------- text -- */

/*
 * The lines, as text. Layout does not come into it - the app uses this
 * to decide whether anything changed, and the harness uses it to check
 * every string fits its column. Returns the number of lines; each is
 * "label\tbar%\tvalue", with bar% empty when the row has no bar.
 */
short psmonui_sec_lines(const PMUI *u, short sec, char out[][64], short max)
{
	const PMDATA *d = u->d;
	char v[96];
	short n = 0;

#define LINE(lab, pcs, val)						\
	do {								\
		if (n < max)						\
			sprintf(out[n++], "%s\t%s\t%.44s", (lab),	\
			        (pcs), (val));				\
	} while (0)
#define PCT(p)	(sprintf(pb, "%d", (int) (p)), pb)	/* pb is 16: a
						 * percentage, clamped */

	if (!d)
		return 0;

	if (sec == PM_SEC_ENGINE)
	{
		char pb[16];

		if (!d->host)
		{
			LINE("Engine", "", "no PSCTRL: emulator too old");
			return n;
		}
		if (d->khz > 0)
			sprintf(v, "%ld MHz  (%ldx ST)", d->khz / 1000L,
			        d->khz / 8000L);
		else
			strcpy(v, "n/a");
		LINE("Speed", "", v);

		f_x10(v, d->hit_x10);
		LINE("JIT hit", d->hit_x10 < 0 ? "" : PCT(d->hit_x10 / 10L), v);

		f_x10(v, d->idle_x10);
		LINE("Idle", d->idle_x10 < 0 ? "" : PCT(d->idle_x10 / 10L), v);

		if (d->cache_total > 0 && d->cache_used >= 0)
		{
			short p = f_pct(d->cache_used, d->cache_total);
			char a[24], b[24];

			f_kb(a, d->cache_used);
			f_kb(b, d->cache_total);
			if (d->flushes >= 0)
				sprintf(v, "%s of %s, %ld flushes", a, b, d->flushes);
			else
				sprintf(v, "%s of %s", a, b);
			LINE("Cache", PCT(p), v);
		}
		else
			LINE("Cache", "", "n/a");

		/* the sampler's window is 500 ms, so double for a rate */
		if (d->compiles >= 0)
			sprintf(v, "%ld blk/s", d->compiles * 2L);
		else
			strcpy(v, "n/a");
		LINE("Compile", "", v);

		if (d->smc >= 0)
			sprintf(v, "%ld /s", d->smc * 2L);
		else
			strcpy(v, "n/a");
		LINE("SMC inv", "", v);
		return n;
	}

	if (sec == PM_SEC_MEMORY)
	{
		char pb[16], a[24], b[24];

		f_kb(a, d->st_free);
		f_kb(b, d->st_total);
		sprintf(v, "%s free of %s", a, b);
		LINE("ST RAM", PCT(f_pct(d->st_total - d->st_free, d->st_total)), v);

		if (d->tt_total > 0)
		{
			f_kb(a, d->tt_free);
			f_kb(b, d->tt_total);
			sprintf(v, "%s free of %s", a, b);
			LINE("TT RAM",
			     PCT(f_pct(d->tt_total - d->tt_free, d->tt_total)), v);
		}
		return n;
	}

	/* PM_SEC_HOST */
	{
		char pb[16];

		if (!d->host)
		{
			LINE("Board", "", "n/a");
			LINE("ARM clock", "", "n/a");
			LINE("Health", "", "n/a");
			return n;
		}
		if (d->pi_model > 0)
			sprintf(v, "%s, %ld MB", pi_name(d->pi_model), d->pi_ram_mb);
		else
			strcpy(v, "n/a");
		LINE("Board", "", v);

		if (d->arm_khz > 0)
		{
			if (d->soc_mc >= 0)
				sprintf(v, "%ld MHz, %ld.%ld C", d->arm_khz / 1000L,
				        d->soc_mc / 1000L, (d->soc_mc / 100L) % 10L);
			else
				sprintf(v, "%ld MHz", d->arm_khz / 1000L);
		}
		else
			strcpy(v, "n/a");
		LINE("ARM clock", d->load_x100 < 0 ? ""
		     : PCT(d->load_x100 > 400 ? 100 : d->load_x100 / 4L), v);

		/*
		 * The one figure the taskbar's JIT panel has no room for and
		 * the one that explains a bad reading above it: a Pi that is
		 * thermally capped or browning out runs the JIT slower and
		 * nothing on the Atari side can otherwise tell you.
		 */
		if (d->throttled < 0)
			strcpy(v, "n/a");
		else if (d->throttled & 0x0f)
			sprintf(v, "%s%s%s%s",
			        (d->throttled & 1) ? "undervolt " : "",
			        (d->throttled & 2) ? "capped " : "",
			        (d->throttled & 4) ? "throttled " : "",
			        (d->throttled & 8) ? "temp-limit" : "");
		else if (d->throttled & 0x000f0000L)
			strcpy(v, "ok now (has throttled)");
		else
			strcpy(v, "ok");
		LINE("Health", "", v);
		return n;
	}
#undef LINE
#undef PCT
}

/* ------------------------------------------------------------ drawing -- */

/*
 * Fit s into avail points, ellipsising if it will not go. Returns 1 if
 * it had to cut - a truncated value draws perfectly well and stays
 * inside its box, so nothing else here would ever notice that "4.0 MB"
 * had become "4...."
 */
static short fit_text(short vh, char *out, short outsz, const char *s,
                      short avail)
{
	short cw = cellw(vh), max, len;

	max = (short) (avail / (cw > 0 ? cw : 8));
	if (max > outsz - 1)
		max = (short) (outsz - 1);
	len = (short) strlen(s);
	if (len <= max)
	{
		strcpy(out, s);
		return 0;
	}
	if (max < 4)
	{
		out[0] = '\0';
		return 1;
	}
	memcpy(out, s, (size_t) (max - 3));
	strcpy(out + max - 3, "...");
	return 1;
}

/*
 * The bar. APJ_RG_SEEK carries a track and a fill, so a gauge is two
 * nine-slices and no per-pixel work - the same three states MP3GEM's
 * seek bar uses. A sheet without it (version 1) draws a flat pair of
 * bars instead, which is what apj_skin_has() is for.
 */
static void draw_bar(short vh, short x, short y, short w, short h, short pct)
{
	short fill;

	if (w <= 0)
		return;
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	fill = (short) ((long) w * pct / 100L);

	if (apj_skin_has(APJ_RG_SEEK))
	{
		apj_skin_9(vh, APJ_RG_SEEK, APJ_SK_TRACK, x, y, w, h);
		if (fill > 0)
			apj_skin_9(vh, APJ_RG_SEEK, APJ_SK_FILL, x, y, fill, h);
		return;
	}
	apj_fill(vh, x, y, w, h, apj_skin_pen(APJ_R_DARK));
	if (fill > 0)
		apj_fill(vh, x, y, fill, h, apj_skin_pen(APJ_R_ACCENT));
}

static const char *const secname[PM_SEC_N] = { "JIT engine", "Memory", "Host" };

void psmonui_draw_sec(PMUI *u, short vh, short sec)
{
	char lines[PM_MAXLINE][64];
	char buf[64];
	GRECT g;
	short n, i, ch, y;

	if (!psmonui_sec_rect(u, sec, &g))
		return;
	if (!vis(u, g.g_x, g.g_y, g.g_w, g.g_h))
		return;

	if (apj_skin_has(APJ_RG_GROUP))
		apj_skin_9(vh, APJ_RG_GROUP, 0, g.g_x, g.g_y, g.g_w, g.g_h);
	else
		apj_fill(vh, g.g_x, g.g_y, g.g_w, g.g_h,
		         apj_skin_pen(APJ_X_ROW));

	ui_font(vh, F_SMALL);
	ch = cellh(vh);
	apj_skin_text(vh, (short) (g.g_x + M(SECPAD)),
	              (short) (g.g_y + (M(HEADH) - ch) / 2),
	              apj_skin_pen(APJ_X_MUTED), secname[sec]);

	ui_font(vh, F_BODY);
	ch = cellh(vh);
	n = psmonui_sec_lines(u, sec, lines, PM_MAXLINE);
	u->rows_wanted = (short) (u->rows_wanted + n);
	y = (short) (g.g_y + M(HEADH) + M(SECPAD));

	for (i = 0; i < n; i++)
	{
		char *lab = lines[i], *pcs, *val;
		short ty = (short) (y + (u->rowh - ch) / 2);
		short right = (short) (g.g_x + g.g_w - M(SECPAD));
		short vx = u->valx;

		if (y + u->rowh > g.g_y + g.g_h)
			break;

		pcs = strchr(lab, '\t');
		if (!pcs)
			continue;
		*pcs++ = '\0';
		val = strchr(pcs, '\t');
		if (!val)
			continue;
		*val++ = '\0';

		fit_text(vh, buf, (short) sizeof(buf), lab, u->labw);
		apj_skin_text(vh, (short) (g.g_x + M(SECPAD)), ty,
		              apj_skin_pen(APJ_X_MUTED), buf);

		if (pcs[0] && u->barw > 0)
			draw_bar(vh, u->barx, (short) (y + (u->rowh - M(BARH)) / 2),
			         u->barw, M(BARH), (short) atoi(pcs));
		else
			vx = u->barx;		/* no bar: the value starts here */

		if (fit_text(vh, buf, (short) sizeof(buf), val,
		             (short) (right - vx)))
			u->trunc++;
		apj_skin_text(vh, vx, ty, apj_skin_pen(APJ_R_TEXT), buf);

		y = (short) (y + u->rowh);
		u->rows_drawn++;
	}
}

static void draw_status(PMUI *u, short vh)
{
	char buf[128];
	short y, ch;

	y = (short) (u->work.g_y + u->work.g_h - M(STATH));
	if (!vis(u, u->work.g_x, y, u->work.g_w, M(STATH)))
		return;
	if (apj_skin_has(APJ_RG_STATUS))
		apj_skin_tilex(vh, APJ_RG_STATUS, 0, u->work.g_x, y, u->work.g_w);
	else
		apj_fill(vh, u->work.g_x, y, u->work.g_w, M(STATH),
		         apj_skin_pen(APJ_X_ROW));
	ui_font(vh, F_SMALL);
	ch = cellh(vh);
	fit_text(vh, buf, (short) sizeof(buf), u->status ? u->status : "",
	         (short) (u->work.g_w - M(PAD) * 2));
	apj_skin_text(vh, (short) (u->work.g_x + M(PAD)),
	              (short) (y + (M(STATH) - ch) / 2),
	              apj_skin_pen(APJ_X_MUTED), buf);
	ui_font(vh, F_BODY);
}

void psmonui_draw_clip(PMUI *u, short vh, const GRECT *clip)
{
	GRECT c = *clip;
	short i;

	if (c.g_x < u->work.g_x) { c.g_w -= u->work.g_x - c.g_x; c.g_x = u->work.g_x; }
	if (c.g_y < u->work.g_y) { c.g_h -= u->work.g_y - c.g_y; c.g_y = u->work.g_y; }
	if (c.g_x + c.g_w > u->work.g_x + u->work.g_w)
		c.g_w = (short) (u->work.g_x + u->work.g_w - c.g_x);
	if (c.g_y + c.g_h > u->work.g_y + u->work.g_h)
		c.g_h = (short) (u->work.g_y + u->work.g_h - c.g_y);
	if (c.g_w <= 0 || c.g_h <= 0)
		return;
	u->clip = c;

	apj_fill(vh, c.g_x, c.g_y, c.g_w, c.g_h,
	         apj_skin_ok() ? apj_skin_pen(APJ_R_PANEL) : apj_pen(APJ_R_PANEL));
	u->trunc = 0;
	u->rows_drawn = 0;
	u->rows_wanted = 0;
	for (i = 0; i < PM_SEC_N; i++)
		psmonui_draw_sec(u, vh, i);
	draw_status(u, vh);
	u->clip = u->work;
}

void psmonui_draw(PMUI *u, short vh)
{
	psmonui_draw_clip(u, vh, &u->work);
}
