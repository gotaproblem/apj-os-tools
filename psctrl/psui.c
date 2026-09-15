/*
 * psui.c - see psui.h. Every control is a rectangle out of the skin
 * sheet; the only per-pixel work the 68k does is the coverage blend
 * behind a glyph, and this window draws none.
 *
 * Geometry is in POINTS through apj_skin_m(), so one source serves 1x,
 * 1.25x and 1.75x - the DPI rule from the GUI redesign, the same way
 * mp3ui.c does it.
 *
 * The row layout is fixed and the same on every tab:
 *
 *   | title ................ | control ........ | badge |
 *
 * which is what lets the drawing be driven entirely from a descriptor:
 * the row's KIND picks the control, its CLASS picks the badge, and
 * nothing here knows what "jit_power" is.
 */

#include <stdio.h>
#include <string.h>

#include "psui.h"

#define PAD		10	/* window inset, points                 */
#define TABH		30
#define ROWH		26
#define STATH		18
#define SCROLLW		6
#define GAP		8
#define BADGEW		44
#define BENCHHEAD	26
#define SECPAD		8
#define BARH		8
#define FIELDH		22
#define SLIDERH		6
#define KNOB		12

#define M(pt)		apj_skin_m(pt)

static void draw_plain(PSUI *u, short vh);
static short use_radios(PSUI *u, short vh, const PSROW *r);

/* --------------------------------------------------------------- text -- */

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

/*
 * Fit s into avail points, ellipsising if it will not go. Returns 1 if
 * it had to cut.
 *
 * The return value is not decoration. A truncated value draws perfectly
 * well and stays inside its box, so every check this file has passes -
 * and the reader is quietly shown "x386.8 S" where the machine said
 * "x386.8 ST". The benchmark tab counts these and the harness insists on
 * zero for a realistic set of figures.
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

static int vis(const PSUI *u, short x, short y, short w, short h)
{
	return !(x >= u->clip.g_x + u->clip.g_w || x + w <= u->clip.g_x ||
	         y >= u->clip.g_y + u->clip.g_h || y + h <= u->clip.g_y);
}

static void add(PSUI *u, short id, short x, short y, short w, short h)
{
	short n = u->nlay;

	if (n >= (short) (sizeof(u->lay) / sizeof(u->lay[0])) - 1)
		return;
	u->lay[n].id = id;
	u->lay[n].x = x;
	u->lay[n].y = y;
	u->lay[n].w = w;
	u->lay[n].h = h;
	u->nlay = (short) (n + 1);
	u->lay[u->nlay].id = -1;
}

/*
 * The unit suffix. Everything the host sends is an integer, including the
 * two that are really fractions - x100 for a gain and a percentage in
 * tenths - so the formatting has to know which is which. Printing 250
 * where the user set 2.5 is exactly the kind of thing that makes a
 * settings dialog untrustworthy.
 */
static void value_text(char *out, const PSROW *r, long v)
{
	switch (r->unit)
	{
	case PS_U_MS:   sprintf(out, "%ld ms", v); break;
	case PS_U_NS:
		if (v >= 1000000L)
			sprintf(out, "%ld.%01ld ms", v / 1000000L, (v / 100000L) % 10L);
		else if (v >= 1000L)
			sprintf(out, "%ld.%01ld us", v / 1000L, (v / 100L) % 10L);
		else
			sprintf(out, "%ld ns", v);
		break;
	case PS_U_US:   sprintf(out, "%ld us", v); break;
	case PS_U_KB:
		if (v >= 1024L)
			sprintf(out, "%ld.%01ld MB", v / 1024L, ((v % 1024L) * 10L) / 1024L);
		else
			sprintf(out, "%ld KB", v);
		break;
	case PS_U_MB:   sprintf(out, "%ld MB", v); break;
	case PS_U_PCT:  sprintf(out, "%ld%%", v); break;
	case PS_U_HZ:   sprintf(out, "%ld Hz", v); break;
	case PS_U_KBPS: sprintf(out, "%ld kbps", v); break;
	case PS_U_CYC:  sprintf(out, "%ld cyc", v); break;
	case PS_U_SEC:  sprintf(out, "%ld s", v); break;
	case PS_U_X100:
		/* a gain or a rate carried as hundredths */
		sprintf(out, "%ld.%02ld", v / 100L, (v < 0 ? -v : v) % 100L);
		break;
	case PS_U_PCT10:
		/* a percentage carried as tenths - the JIT hit rate and the
		 * guest idle both arrive this way, and 1000 is 100.0% */
		sprintf(out, "%ld.%01ld%%", v / 10L, (v < 0 ? -v : v) % 10L);
		break;
	default:
		sprintf(out, "%ld", v);
		break;
	}
}

/*
 * jit_power and m68k_speed both have a value outside their own scale
 * that means something else, and a bare number for it is a lie. This is
 * the one place the UI knows a setting by name, and it is worth it.
 */
static void int_text(char *out, const PSROW *r, long v)
{
	if (!strcmp(r->name, "jit_power"))
	{
		if (v <= 0)
			strcpy(out, "off");
		else
			sprintf(out, "%ld  (%ld)", v, 256L << (v - 1));
		return;
	}
	/*
	 * A slowdown, not a speed-up: update_68k_cycles() makes the cycle
	 * unit CYCLE_UNIT * n, so a bigger number is a slower guest, and 0
	 * means no clock limit at all. "3" on its own reads like the
	 * opposite of what it does.
	 */
	if (!strcmp(r->name, "cpu_clock_multiplier"))
	{
		/* Short: the readout column beside the slider is narrow, and
		 * "off (fastest)" clipped to "off (fast" on screen. The row
		 * label already says "CPU slowdown", so "off" and "3 x" read
		 * correctly on their own. */
		if (v <= 0)
			strcpy(out, "off");
		else
			sprintf(out, "%ld x", v);
		return;
	}
	if (!strcmp(r->name, "m68k_speed") && v < 0)
	{
		strcpy(out, "max");
		return;
	}
	if (!strcmp(r->name, "blit_timed_ns") && v == 0)
	{
		strcpy(out, "instant");
		return;
	}
	if (!strcmp(r->name, "stbox_plane") && v == 0)
	{
		strcpy(out, "auto");
		return;
	}
	value_text(out, r, v);
}

static const char *const tabname[PS_TAB_N] =
{
	"JIT", "CPU", "Video", "Audio", "Input",
	"STBox", "Floppy", "Net", "Debug", "Adv", "Bench"
};

/*
 * Where each tab sits on the strip. Bench is tab 10 because tab numbers
 * are wire values and appending is the only safe way to add one, but it
 * belongs next to JIT, so the strip is drawn in this order instead of in
 * enum order. Everything else - widget ids, the descriptor's tab byte,
 * the hit test - still uses the tab number.
 */
static const short taborder[PS_TAB_N] =
{
	PS_TAB_JIT, PS_TAB_BENCH, PS_TAB_CPU, PS_TAB_VIDEO, PS_TAB_AUDIO,
	PS_TAB_INPUT, PS_TAB_STBOX, PS_TAB_FLOPPY, PS_TAB_NET, PS_TAB_DEBUG,
	PS_TAB_ADV
};

const char *psui_tab_name(short tab)
{
	return (tab >= 0 && tab < PS_TAB_N) ? tabname[tab] : "";
}

short psui_tab_at(short slot)
{
	return (slot >= 0 && slot < PS_TAB_N) ? taborder[slot] : PS_TAB_JIT;
}

/* the left and right arrow keys walk the strip, not the enum */
short psui_tab_step(short tab, short dir)
{
	short i;

	for (i = 0; i < PS_TAB_N; i++)
		if (taborder[i] == tab)
			break;
	if (i >= PS_TAB_N)
		i = 0;
	i = (short) (i + dir);
	if (i < 0)
		i = (short) (PS_TAB_N - 1);
	if (i >= PS_TAB_N)
		i = 0;
	return taborder[i];
}

/* The value as it is put on the screen. The app uses this to tell a
 * readout that actually moved from one that only jittered underneath an
 * unchanged string, so a two-a-second poll does not repaint the row list
 * for a figure nobody can see change. */
void psui_row_text(const PSROW *r, long v, char *out, short n)
{
	char buf[96];

	if (r->kind == PS_K_ENUM && v >= 0 && v < r->nenum)
		strcpy(buf, r->label[v]);
	else if (r->kind == PS_K_STR)
		sprintf(buf, "%.60s", r->str);
	else if (r->kind == PS_K_BOOL)
		strcpy(buf, v ? "on" : "off");
	else
		int_text(buf, r, v);
	strncpy(out, buf, (size_t) (n - 1));
	out[n - 1] = '\0';
}

/* the badge a class gets: text, and which BADGE state to draw it on */
static const char *class_badge(const PSROW *r, short *state)
{
	switch (r->klass)
	{
	case PS_C_LIVE:
		*state = r->changed ? APJ_BG_ACCENT : APJ_BG_PLAIN;
		return r->changed ? "set" : "live";
	case PS_C_DEFER:
		*state = APJ_BG_WARN;
		return r->changed ? "soon" : "defer";
	case PS_C_BOOT:
		*state = r->changed ? APJ_BG_DANGER : APJ_BG_PLAIN;
		return r->changed ? "restart" : "boot";
	case PS_C_BOXBOOT:
		*state = r->changed ? APJ_BG_DANGER : APJ_BG_PLAIN;
		return r->changed ? "box" : "box";
	default:
		*state = APJ_BG_PLAIN;
		return "";
	}
}

/* --------------------------------------------------------------- layout */

void psui_minsize(short *w, short *h)
{
	*w = (short) (M(PAD) * 2 + M(240) + M(GAP) + M(180) + M(GAP) + M(BADGEW));
	*h = (short) (M(TABH) + M(ROWH) * 4 + M(STATH) + M(PAD) * 2);
}

/*
 * Which rows are on this tab, in table order. Recomputed on every layout
 * rather than cached: the table is under 200 entries and this is a single
 * pass, and caching it is how a tab ends up showing another tab's rows
 * after the model is reloaded on a theme change.
 */
static void collect(PSUI *u)
{
	short i;

	u->nidx = 0;
	for (i = 0; i < u->nrows && u->nidx < PS_MAXROW; i++)
		if (u->rows[i].tab == u->tab)
			u->idx[u->nidx++] = i;
	if (u->top > u->nidx - 1)
		u->top = (short) (u->nidx - 1);
	if (u->top < 0)
		u->top = 0;
}

void psui_layout(PSUI *u, short vh, short wx, short wy, short ww, short wh)
{
	short pad = M(PAD), tabh = M(TABH), stath = M(STATH);
	short x, y, i, tw, bh;

	if (!apj_skin_ok())
	{
		pad = 4;
		tabh = (short) (cellh(vh) + 4);
		stath = cellh(vh);
	}

	u->work.g_x = wx; u->work.g_y = wy;
	u->work.g_w = ww; u->work.g_h = wh;
	u->clip = u->work;
	u->nlay = 0;
	u->lay[0].id = -1;

	collect(u);

	/* tab strip across the top, equal widths, the remainder to the last */
	tw = (short) (ww / PS_TAB_N);
	x = wx;
	for (i = 0; i < PS_TAB_N; i++)
	{
		short w = (i == PS_TAB_N - 1) ? (short) (wx + ww - x) : tw;

		add(u, (short) (PSW_TAB0 + psui_tab_at(i)), x, wy, w, tabh);
		x = (short) (x + w);
	}

	/* the row area */
	ui_font(vh, F_BODY);
	bh = cellh(vh);
	u->rowh = apj_skin_ok() ? M(ROWH) : (short) (bh + 4);
	if (u->rowh < bh + M(6))
		u->rowh = (short) (bh + M(6));	/* never shorter than its text */

	u->listr.g_x = (short) (wx + pad);
	u->listr.g_y = (short) (wy + tabh + pad / 2);
	u->listr.g_w = (short) (ww - 2 * pad);
	u->listr.g_h = (short) (wy + wh - stath - pad / 2 - u->listr.g_y);
	if (u->listr.g_h < u->rowh)
		u->listr.g_h = u->rowh;

	/*
	 * The benchmark has its own tab now. It used to sit under the JIT
	 * tab's rows and take up to half of them, which on a 14-row tab was
	 * most of the list; and sizing it to its content instead just made
	 * the panel useless. A tab each: the JIT tab is all rows, the Bench
	 * tab is all panel.
	 */
	if (u->tab == PS_TAB_BENCH)
	{
		u->listr.g_h = 0;
		u->visrows = 0;
		u->top = 0;
	}
	else
	{
		u->visrows = (short) (u->listr.g_h / u->rowh);
		if (u->visrows < 1)
			u->visrows = 1;
	}
	if (u->top > u->nidx - u->visrows)
		u->top = (short) (u->nidx - u->visrows);
	if (u->top < 0)
		u->top = 0;

	/* the control column: the right half, minus the badge and the bar */
	{
		short right = (short) (u->listr.g_x + u->listr.g_w);
		short sb = psui_scroll_needed(u) ? (short) (M(SCROLLW) + M(GAP)) : 0;

		u->ctlw = (short) (M(180));
		u->ctlx = (short) (right - sb - M(BADGEW) - M(GAP) - u->ctlw);
		if (u->ctlx < u->listr.g_x + M(80))
		{
			u->ctlx = (short) (u->listr.g_x + M(80));
			u->ctlw = (short) (right - sb - M(BADGEW) - M(GAP) - u->ctlx);
			if (u->ctlw < M(40))
				u->ctlw = M(40);
		}
	}

	/* now that the control column is known, decide radios vs popup for
	 * every row on this tab - once, here, so nothing downstream has to
	 * ask the question again and risk answering it differently */
	for (i = 0; i < u->nidx; i++)
	{
		const PSROW *r = &u->rows[u->idx[i]];

		u->radios[i] = (r->kind == PS_K_ENUM) ? use_radios(u, vh, r) : 0;
	}
	ui_font(vh, F_BODY);

	if (psui_scroll_needed(u))
		add(u, PSW_SCROLL,
		    (short) (u->listr.g_x + u->listr.g_w - M(SCROLLW)),
		    u->listr.g_y, M(SCROLLW), u->listr.g_h);

	y = u->listr.g_y;
	for (i = 0; i < u->visrows && u->top + i < u->nidx; i++)
	{
		add(u, (short) (PSW_ROW0 + i), u->listr.g_x, y,
		    u->listr.g_w, u->rowh);
		y = (short) (y + u->rowh);
	}

	/* the CoreMark build selector, on the Bench tab's header strip */
	if (u->tab == PS_TAB_BENCH && u->bench)
	{
		GRECT hg;

		if (psui_bench_head(u, &hg))
		{
			short bw = (short) (cellw(vh) * 7);
			short bx = (short) (hg.g_x + cellw(vh) * 16);
			short k;

			for (k = 0; k < PSB_CM_N; k++)
			{
				add(u, (short) (PSW_CMTARGET + k), bx,
				    (short) (hg.g_y + M(2)), bw,
				    (short) (hg.g_h - M(4)));
				bx = (short) (bx + bw + M(GAP));
			}
		}
	}

	/* two buttons live on the status strip, right-aligned */
	{
		short by = (short) (wy + wh - stath);
		short bw = (short) (M(84));

		add(u, PSW_SAVE, (short) (wx + ww - pad - bw), by, bw, stath);
		if (u->tab == PS_TAB_BENCH)
			add(u, PSW_BENCH,
			    (short) (wx + ww - pad - 2 * bw - M(GAP)), by, bw, stath);
	}
}

void psui_bbox(PSUI *u, GRECT *r)
{
	*r = u->work;
}

/* ------------------------------------------------------------ scrolling */

short psui_scroll_needed(PSUI *u)
{
	return (u->nidx > u->visrows && u->visrows > 0) ? 1 : 0;
}

void psui_thumb_rect(PSUI *u, GRECT *r)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, PSW_SCROLL);
	long span = (long) u->nidx - u->visrows;
	short th, ty;

	if (!l || !psui_scroll_needed(u))
	{
		r->g_x = r->g_y = r->g_w = r->g_h = 0;
		return;
	}
	th = (short) ((long) l->h * u->visrows / u->nidx);
	if (th < M(SCROLLW) * 2)
		th = (short) (M(SCROLLW) * 2);
	if (th > l->h)
		th = l->h;
	ty = (short) (l->y + (long) (l->h - th) * u->top / span);
	r->g_x = l->x; r->g_y = ty; r->g_w = l->w; r->g_h = th;
}

short psui_scroll_part(PSUI *u, short my)
{
	GRECT t;

	psui_thumb_rect(u, &t);
	if (my < t.g_y)
		return -1;
	if (my >= t.g_y + t.g_h)
		return 1;
	return 0;
}

short psui_scroll_top_for(PSUI *u, short my, short grab)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, PSW_SCROLL);
	GRECT t;
	long span = (long) u->nidx - u->visrows, room, top;

	if (!l || span <= 0)
		return 0;
	psui_thumb_rect(u, &t);
	room = (long) l->h - t.g_h;
	if (room <= 0)
		return 0;
	top = ((long) (my - grab - l->y) * span + room / 2) / room;
	if (top < 0)    top = 0;
	if (top > span) top = span;
	return (short) top;
}

/* ------------------------------------------------------------ hit tests */

short psui_hit(PSUI *u, short mx, short my)
{
	return apj_lay_hit(u->lay, u->nlay, mx, my);
}

void psui_row_rect(PSUI *u, short vis, GRECT *r)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, (short) (PSW_ROW0 + vis));

	if (!l)
	{
		r->g_x = r->g_y = r->g_w = r->g_h = 0;
		return;
	}
	r->g_x = l->x; r->g_y = l->y; r->g_w = l->w; r->g_h = l->h;
}

short psui_row_at(PSUI *u, short my)
{
	short v;

	if (my < u->listr.g_y || my >= u->listr.g_y + u->listr.g_h)
		return -1;
	v = (short) ((my - u->listr.g_y) / u->rowh);
	if (v < 0 || v >= u->visrows || u->top + v >= u->nidx)
		return -1;
	return v;
}

/*
 * Radios or a popup. Four choices is the design's cut-off, but the real
 * question is whether the LABELS fit: three radios reading "off",
 * "real chip", "emulated" in a 180 pt column truncate to "rea..." and
 * "emu...", which is worse than a popup. So the count is a ceiling and
 * the width decides.
 */
static short use_radios(PSUI *u, short vh, const PSROW *r)
{
	short each, rw, rh, cw, i;

	if (r->nenum < 2 || r->nenum > 4)
		return 0;
	apj_skin_size(APJ_RG_RADIO, &rw, &rh);
	each = (short) (u->ctlw / r->nenum);
	ui_font(vh, F_SMALL);
	cw = cellw(vh);
	for (i = 0; i < r->nenum; i++)
		if ((short) (cw * (short) strlen(r->label[i])) >
		    (short) (each - rw - M(6)))
			return 0;
	return 1;
}

/* where the radios for a small enum sit inside the control column */
static void radio_rect(PSUI *u, const PSROW *r, short i, GRECT *g, short y)
{
	short each = (short) (u->ctlw / (r->nenum > 0 ? r->nenum : 1));

	g->g_x = (short) (u->ctlx + i * each);
	g->g_y = y;
	g->g_w = each;
	g->g_h = u->rowh;
}

short psui_click_value(PSUI *u, short vis, short mx, short my, long *v)
{
	const PSROW *r;
	GRECT rr;
	short i;

	(void) my;		/* rows are one line tall - x is the whole story */
	if (vis < 0 || u->top + vis >= u->nidx)
		return 0;
	r = &u->rows[u->idx[u->top + vis]];
	psui_row_rect(u, vis, &rr);

	switch (r->kind)
	{
	case PS_K_BOOL:
		if (mx < u->ctlx)
			return 0;
		*v = r->value ? 0 : 1;
		return 1;

	case PS_K_ENUM:
		if (u->radios[u->top + vis])
		{
			for (i = 0; i < r->nenum; i++)
			{
				GRECT g;

				radio_rect(u, r, i, &g, rr.g_y);
				if (mx >= g.g_x && mx < g.g_x + g.g_w)
				{
					*v = i;
					return 1;
				}
			}
			return 0;
		}
		if (mx >= u->ctlx && mx < u->ctlx + u->ctlw)
			return 2;		/* open the dropdown */
		return 0;

	case PS_K_INT:
	{
		short sx = u->ctlx;
		short sw = (short) (u->ctlw - M(56));
		long span = r->max - r->min;

		if (sw < M(20) || span <= 0)
			return 0;
		if (mx < sx || mx >= sx + sw)
			return 0;
		*v = r->min + ((long) (mx - sx) * span + sw / 2) / sw;
		if (r->step > 1)
			*v = r->min + ((*v - r->min) / r->step) * r->step;
		if (*v < r->min) *v = r->min;
		if (*v > r->max) *v = r->max;
		return 1;
	}

	case PS_K_STR:
		if (mx >= u->ctlx && mx < u->ctlx + u->ctlw)
			return 2;
		return 0;

	case PS_K_ACTION:
		if (mx >= u->ctlx && mx < u->ctlx + u->ctlw)
			return 3;
		return 0;

	default:
		return 0;
	}
}

/* ------------------------------------------------------------- dropdown */

void psui_drop_rect(PSUI *u, short vis, GRECT *r)
{
	const PSROW *row;
	short n;
	GRECT rr;

	r->g_x = r->g_y = r->g_w = r->g_h = 0;
	if (vis < 0 || u->top + vis >= u->nidx)
		return;
	row = &u->rows[u->idx[u->top + vis]];
	n = (row->kind == PS_K_ENUM && !u->radios[u->top + vis]) ? row->nenum : 0;
	if (n <= 0)
		return;
	psui_row_rect(u, vis, &rr);

	r->g_x = u->ctlx;
	r->g_w = u->ctlw;
	r->g_h = (short) (n * u->rowh + M(4));
	r->g_y = (short) (rr.g_y + u->rowh);
	/* flip it above the row rather than off the bottom of the window */
	if (r->g_y + r->g_h > u->work.g_y + u->work.g_h)
		r->g_y = (short) (rr.g_y - r->g_h);
	if (r->g_y < u->work.g_y)
		r->g_y = u->work.g_y;
}

short psui_drop_at(PSUI *u, short vis, short my)
{
	GRECT d;
	short i;

	psui_drop_rect(u, vis, &d);
	if (d.g_h <= 0 || my < d.g_y + M(2) || my >= d.g_y + d.g_h)
		return -1;
	i = (short) ((my - d.g_y - M(2)) / u->rowh);
	if (i < 0 || i >= u->rows[u->idx[u->top + vis]].nenum)
		return -1;
	return i;
}

/* ---------------------------------------------------------------- draw -- */

static short state_of(PSUI *u, short id, short on)
{
	if (u->press == id)
		return APJ_ST_PRESS;
	if (on)
		return APJ_ST_ON;
	if (u->hover == id)
		return APJ_ST_HOVER;
	return APJ_ST_NORM;
}

static void draw_badge(PSUI *u, short vh, const PSROW *r, short y)
{
	short st, w, ch, tx;
	const char *t = class_badge(r, &st);

	if (!t[0] || !apj_skin_has(APJ_RG_BADGE))
		return;
	ui_font(vh, F_SMALL);
	ch = cellh(vh);
	w = (short) (cellw(vh) * (short) strlen(t) + M(10));
	tx = (short) (u->work.g_x + u->work.g_w - M(PAD) - w -
	              (psui_scroll_needed(u) ? M(SCROLLW) + M(GAP) : 0));
	apj_skin_9(vh, APJ_RG_BADGE, st, tx,
	           (short) (y + (u->rowh - M(16)) / 2), w, M(16));
	/* The plain badge sits on the row ground, so it takes the muted ink.
	 * The other three sit on a saturated fill - accent blue, amber, red -
	 * and white on amber is unreadable on a real screen. Black reads on
	 * all three, so the coloured badges get black however the theme
	 * defines its accent ink. */
	apj_skin_text(vh, (short) (tx + M(5)),
	              (short) (y + (u->rowh - ch) / 2),
	              st == APJ_BG_PLAIN ? apj_skin_pen(APJ_X_MUTED) : G_BLACK,
	              t);
}

static void draw_slider(PSUI *u, short vh, const PSROW *r, short y, short id)
{
	short sx = u->ctlx, sw = (short) (u->ctlw - M(56));
	short th = M(SLIDERH), ty, kn = M(KNOB), fw, ch;
	long span = r->max - r->min;
	char buf[32];

	if (sw < M(20))
		sw = M(20);
	ty = (short) (y + (u->rowh - th) / 2);

	apj_skin_9(vh, APJ_RG_SEEK, APJ_SK_TRACK, sx, ty, sw, th);
	fw = 0;
	if (span > 0)
	{
		long p = r->value - r->min;

		if (p < 0)    p = 0;
		if (p > span) p = span;
		fw = (short) ((long) sw * p / span);
	}
	if (fw > 0)
		apj_skin_9(vh, APJ_RG_SEEK, APJ_SK_FILL, sx, ty, fw, th);
	apj_skin_blit(vh, APJ_RG_KNOB, state_of(u, id, 0),
	              (short) (sx + fw - kn / 2), (short) (ty + th / 2 - kn / 2));

	ui_font(vh, F_SMALL);
	ch = cellh(vh);
	int_text(buf, r, r->value);
	apj_skin_text(vh, (short) (sx + sw + M(8)),
	              (short) (y + (u->rowh - ch) / 2),
	              apj_skin_pen(APJ_R_TEXT), buf);
}

static void draw_row(PSUI *u, short vh, short v)
{
	const PSROW *r;
	GRECT rr;
	short id = (short) (PSW_ROW0 + v);
	short ch, i;
	char buf[96];

	if (u->top + v >= u->nidx)
		return;
	r = &u->rows[u->idx[u->top + v]];
	psui_row_rect(u, v, &rr);
	if (!rr.g_w || !vis(u, rr.g_x, rr.g_y, rr.g_w, rr.g_h))
		return;

	/* the row's own ground: alternating, so a long tab stays readable */
	apj_fill(vh, rr.g_x, rr.g_y, rr.g_w, rr.g_h,
	         apj_skin_pen(((u->top + v) & 1) ? APJ_X_ROW : APJ_R_PANEL));
	if (u->hover == id && r->kind != PS_K_INFO)
		apj_fill(vh, rr.g_x, rr.g_y, rr.g_w, rr.g_h,
		         apj_skin_pen(APJ_R_HOVER));

	ui_font(vh, F_BODY);
	ch = cellh(vh);

	/* an action has no separate title - its button carries the words */
	if (r->kind != PS_K_ACTION)
	{
		fit_text(vh, buf, (short) sizeof(buf), r->title,
		         (short) (u->ctlx - rr.g_x - M(GAP)));
		apj_skin_text(vh, (short) (rr.g_x + M(4)),
		              (short) (rr.g_y + (u->rowh - ch) / 2),
		              apj_skin_pen(r->klass == PS_C_RO ? APJ_X_MUTED
		                                               : APJ_R_TEXT), buf);
	}

	switch (r->kind)
	{
	case PS_K_BOOL:
	{
		short cw2, ch2;

		apj_skin_size(APJ_RG_CHECK, &cw2, &ch2);
		if (cw2 > 0)
			apj_skin_blit(vh, APJ_RG_CHECK,
			              APJ_CK(r->value != 0, state_of(u, id, 0)),
			              u->ctlx, (short) (rr.g_y + (u->rowh - ch2) / 2));
		apj_skin_text(vh, (short) (u->ctlx + cw2 + M(6)),
		              (short) (rr.g_y + (u->rowh - ch) / 2),
		              apj_skin_pen(APJ_X_MUTED),
		              r->value ? (r->nenum > 1 ? r->label[1] : "on")
		                       : (r->nenum > 0 ? r->label[0] : "off"));
		break;
	}

	case PS_K_ENUM:
		if (u->radios[u->top + v])
		{
			short rw, rh;

			apj_skin_size(APJ_RG_RADIO, &rw, &rh);
			ui_font(vh, F_SMALL);
			for (i = 0; i < r->nenum; i++)
			{
				GRECT g;
				char lab[24];

				radio_rect(u, r, i, &g, rr.g_y);
				if (rw > 0)
					apj_skin_blit(vh, APJ_RG_RADIO,
					              APJ_CK(i == r->value, APJ_ST_NORM),
					              g.g_x, (short) (g.g_y + (u->rowh - rh) / 2));
				fit_text(vh, lab, (short) sizeof(lab), r->label[i],
				         (short) (g.g_w - rw - M(4)));
				apj_skin_text(vh, (short) (g.g_x + rw + M(3)),
				              (short) (g.g_y + (u->rowh - cellh(vh)) / 2),
				              apj_skin_pen(i == r->value ? APJ_R_TEXT
				                                         : APJ_X_MUTED), lab);
			}
			ui_font(vh, F_BODY);
		}
		else
		{
			short fh = M(FIELDH);
			short fy = (short) (rr.g_y + (u->rowh - fh) / 2);
			short st = state_of(u, id, u->openrow == v);
			short cvw, cvh;
			const char *lab = (r->value >= 0 && r->value < r->nenum)
			                ? r->label[r->value] : "?";

			apj_skin_9(vh, APJ_RG_POPUP, st, u->ctlx, fy, u->ctlw, fh);
			/*
			 * The arrow is its own blit, not part of the plate.
			 * Nine-slice takes the corners from the sheet and fills
			 * the edges flat, so anything baked into the middle of
			 * an edge survives only under the corners - which is
			 * exactly what happened to the first version of this.
			 */
			apj_skin_size(APJ_RG_CHEV, &cvw, &cvh);
			if (cvw > 0)
				apj_skin_blit(vh, APJ_RG_CHEV, st,
				              (short) (u->ctlx + u->ctlw - cvw - M(4)),
				              (short) (fy + (fh - cvh) / 2));
			fit_text(vh, buf, (short) sizeof(buf), lab,
			         (short) (u->ctlw - cvw - M(14)));
			apj_skin_text(vh, (short) (u->ctlx + M(7)),
			              (short) (fy + (fh - ch) / 2),
			              apj_skin_pen(APJ_R_TEXT), buf);
		}
		break;

	case PS_K_INT:
		draw_slider(u, vh, r, rr.g_y, id);
		break;

	case PS_K_STR:
	{
		short fh = M(FIELDH);
		short fy = (short) (rr.g_y + (u->rowh - fh) / 2);
		const char *s = r->str[0] ? r->str : "(none)";
		const char *tail = s;

		apj_skin_9(vh, APJ_RG_FIELD,
		           u->openrow == v ? APJ_FLD_FOCUS : APJ_FLD_NORM,
		           u->ctlx, fy, u->ctlw, fh);
		/* a path is 200 characters of which the last 20 matter, so the
		 * ellipsis goes at the FRONT - the opposite of everywhere else */
		{
			short room = (short) ((u->ctlw - M(14)) / cellw(vh));
			short len = (short) strlen(s);

			if (len > room && room > 4)
			{
				tail = s + len - (room - 3);
				sprintf(buf, "...%.80s", tail);
			}
			else
				sprintf(buf, "%.80s", s);
		}
		apj_skin_text(vh, (short) (u->ctlx + M(7)),
		              (short) (fy + (fh - ch) / 2),
		              apj_skin_pen(r->str[0] ? APJ_R_TEXT : APJ_X_MUTED), buf);
		break;
	}

	case PS_K_ACTION:
	{
		short fh = M(FIELDH);
		short fy = (short) (rr.g_y + (u->rowh - fh) / 2);
		short tw;

		apj_skin_9(vh, APJ_RG_BTN, state_of(u, id, 0), u->ctlx, fy,
		           u->ctlw, fh);
		fit_text(vh, buf, (short) sizeof(buf), r->title,
		         (short) (u->ctlw - M(10)));
		tw = (short) (cellw(vh) * (short) strlen(buf));
		apj_skin_text(vh, (short) (u->ctlx + (u->ctlw - tw) / 2),
		              (short) (fy + (fh - ch) / 2),
		              apj_skin_pen(APJ_R_TEXT), buf);
		break;
	}

	default:				/* PS_K_INFO */
	{
		short tw;

		int_text(buf, r, r->value);
		tw = (short) (cellw(vh) * (short) strlen(buf));
		apj_skin_text(vh, (short) (u->ctlx + u->ctlw - tw),
		              (short) (rr.g_y + (u->rowh - ch) / 2),
		              apj_skin_pen(APJ_R_TEXT), buf);
		break;
	}
	}

	draw_badge(u, vh, r, rr.g_y);
	ui_font(vh, F_BODY);
}

/*
 * Just the part of a row that a poll can change: the control column, plus
 * enough slack to the left of it for a right-aligned readout that is
 * wider than the column. Clipped to this, psui_draw_row() redraws the
 * whole row correctly but only the value cell is repainted - the title
 * and the badge do not flash twice a second for a figure that moved.
 */
/* Where one widget is, by id. The app repaints hover changes through
 * this: two small rectangles rather than the list and the tab strip. */
/* the status strip, so a message does not repaint the window */
void psui_status_rect(const PSUI *u, GRECT *r)
{
	r->g_x = u->work.g_x;
	r->g_y = (short) (u->work.g_y + u->work.g_h - M(STATH));
	r->g_w = u->work.g_w;
	r->g_h = M(STATH);
}

short psui_widget_rect(PSUI *u, short id, GRECT *r)
{
	const APJ_LAY *l;

	if (id == PSW_NONE)
		return 0;
	l = apj_lay_find(u->lay, u->nlay, id);
	if (!l)
		return 0;
	r->g_x = l->x; r->g_y = l->y; r->g_w = l->w; r->g_h = l->h;
	return (r->g_w > 0 && r->g_h > 0) ? 1 : 0;
}

void psui_value_rect(PSUI *u, short vis, GRECT *r)
{
	GRECT rr;
	short x0;

	psui_row_rect(u, vis, &rr);
	x0 = (short) (u->ctlx - M(80));
	if (x0 < rr.g_x)
		x0 = rr.g_x;
	r->g_x = x0;
	r->g_y = rr.g_y;
	r->g_w = (short) (u->ctlx + u->ctlw - x0);
	r->g_h = rr.g_h;
	if (r->g_w < 0)
		r->g_w = 0;
}

void psui_draw_row(PSUI *u, short vh, short v)
{
	if (!apj_skin_ok())
	{
		draw_plain(u, vh);
		return;
	}
	draw_row(u, vh, v);
}

void psui_draw_tabs(PSUI *u, short vh)
{
	short i, ch;

	if (!apj_skin_ok())
		return;
	ui_font(vh, F_BODY);
	ch = cellh(vh);

	apj_skin_tilex(vh, APJ_RG_TABBAR, 0, u->work.g_x, u->work.g_y,
	               u->work.g_w);
	for (i = 0; i < PS_TAB_N; i++)
	{
		const APJ_LAY *l = apj_lay_find(u->lay, u->nlay,
		                                (short) (PSW_TAB0 + i));
		short sel = (i == u->tab);
		char lab[16];
		short tw;

		if (!l || !vis(u, l->x, l->y, l->w, l->h))
			continue;
		apj_skin_9(vh, APJ_RG_TAB,
		           sel ? APJ_ST_ON : state_of(u, (short) (PSW_TAB0 + i), 0),
		           l->x, l->y, l->w, l->h);
		/* the accent rail, for the same nine-slice reason as the
		 * popup's arrow: an edge band cannot come out of the sheet */
		if (sel)
			apj_fill(vh, (short) (l->x + M(6)),
			         (short) (l->y + l->h - M(3)),
			         (short) (l->w - M(12)), M(3),
			         apj_skin_pen(APJ_R_ACCENT));
		fit_text(vh, lab, (short) sizeof(lab), tabname[i], (short) (l->w - M(6)));
		tw = (short) (cellw(vh) * (short) strlen(lab));
		apj_skin_text(vh, (short) (l->x + (l->w - tw) / 2),
		              (short) (l->y + (l->h - ch) / 2),
		              apj_skin_pen(sel ? APJ_R_TEXT : APJ_X_MUTED), lab);
	}
}

static void draw_button(PSUI *u, short vh, short id, const char *label)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, id);
	short ch, tw;

	if (!l)
		return;
	ui_font(vh, F_SMALL);
	ch = cellh(vh);
	apj_skin_9(vh, APJ_RG_BTN, state_of(u, id, 0), l->x, l->y, l->w, l->h);
	tw = (short) (cellw(vh) * (short) strlen(label));
	apj_skin_text(vh, (short) (l->x + (l->w - tw) / 2),
	              (short) (l->y + (l->h - ch) / 2),
	              apj_skin_pen(APJ_R_TEXT), label);
}

void psui_draw_status(PSUI *u, short vh)
{
	short y, ch;
	char buf[160];

	if (!apj_skin_ok())
		return;
	y = (short) (u->work.g_y + u->work.g_h - M(STATH));
	if (!vis(u, u->work.g_x, y, u->work.g_w, M(STATH)))
		return;

	apj_skin_tilex(vh, APJ_RG_STATUS, 0, u->work.g_x, y, u->work.g_w);
	ui_font(vh, F_SMALL);
	ch = cellh(vh);
	fit_text(vh, buf, (short) sizeof(buf), u->status ? u->status : "",
	         (short) (u->work.g_w - M(PAD) * 2 - M(180)));
	apj_skin_text(vh, (short) (u->work.g_x + M(PAD)),
	              (short) (y + (M(STATH) - ch) / 2),
	              apj_skin_pen(APJ_X_MUTED), buf);

	draw_button(u, vh, PSW_SAVE, "Save .cfg");
	if (u->tab == PS_TAB_BENCH)
		draw_button(u, vh, PSW_BENCH, "Benchmark");
	ui_font(vh, F_BODY);
}

static void draw_drop(PSUI *u, short vh)
{
	const PSROW *r;
	GRECT d;
	short i, ch;

	if (u->openrow < 0 || u->top + u->openrow >= u->nidx)
		return;
	r = &u->rows[u->idx[u->top + u->openrow]];
	if (r->kind != PS_K_ENUM || r->nenum <= 0 ||
	    u->radios[u->top + u->openrow])
		return;
	psui_drop_rect(u, u->openrow, &d);
	if (d.g_h <= 0)
		return;

	apj_skin_9(vh, APJ_RG_GROUP, 0, d.g_x, d.g_y, d.g_w, d.g_h);
	ui_font(vh, F_BODY);
	ch = cellh(vh);
	for (i = 0; i < r->nenum; i++)
	{
		short iy = (short) (d.g_y + M(2) + i * u->rowh);

		if (i == u->openpick)
			apj_skin_9(vh, APJ_RG_ROWSEL, 0, (short) (d.g_x + M(2)), iy,
			           (short) (d.g_w - M(4)), u->rowh);
		apj_skin_text(vh, (short) (d.g_x + M(10)),
		              (short) (iy + (u->rowh - ch) / 2),
		              apj_skin_pen(i == u->openpick ? APJ_R_SELFG
		                                            : APJ_R_TEXT),
		              r->label[i]);
	}
}

/* Repaint only the open dropdown box, over the rows already on screen -
 * used while tracking the pointer across items, so the list behind is not
 * redrawn on every move. */
void psui_draw_drop(PSUI *u, short vh)
{
	if (apj_skin_ok())
		draw_drop(u, vh);
}

/*
 * Where the benchmark panel goes. Exported so a test can say "no text
 * outside this box" - the group box is drawn from the framebuffer's point
 * of view but the text is drawn by the AES, so a check that only looks at
 * pixels cannot see a line running out of it.
 */
short psui_bench_rect(const PSUI *u, GRECT *g)
{
	if (!u->bench || u->tab != PS_TAB_BENCH)
		return 0;
	g->g_x = u->listr.g_x;
	g->g_y = (short) (u->listr.g_y + M(BENCHHEAD));
	g->g_w = u->listr.g_w;
	g->g_h = (short) (u->work.g_y + u->work.g_h - M(STATH) -
	                  M(PAD) / 2 - g->g_y);
	return g->g_h > 0 ? 1 : 0;
}

/*
 * Where the 3D test puts its frames while it is running: the free space
 * at the foot of the panel, above the compliance line. Anchored to the
 * bottom rather than the top so it never lands on the results table -
 * which still shows the PREVIOUS run while the current one is being
 * measured, and is the more useful thing to be looking at.
 *
 * Returns 0 when there is not enough room, and the test then runs
 * without a picture rather than drawing over something.
 */
short psui_bench_stage(const PSUI *u, short w, short h, GRECT *r)
{
	GRECT g;
	short noteh, floorr;

	if (!psui_bench_rect(u, &g))
		return 0;
	noteh = (u->bnote && u->bnote[0]) ? (short) (M(11) * 2 + M(4)) : 0;
	floorr = (short) (g.g_y + g.g_h - noteh - M(4));

	r->g_w = w;
	r->g_h = h;
	r->g_x = (short) (g.g_x + (g.g_w - w) / 2);
	r->g_y = (short) (floorr - h);
	return (r->g_y > g.g_y && r->g_x >= g.g_x) ? 1 : 0;
}

/* the strip above it, carrying the CoreMark build selector */
short psui_bench_head(const PSUI *u, GRECT *g)
{
	if (!u->bench || u->tab != PS_TAB_BENCH)
		return 0;
	g->g_x = u->listr.g_x;
	g->g_y = u->listr.g_y;
	g->g_w = u->listr.g_w;
	g->g_h = M(BENCHHEAD);
	return 1;
}

/*
 * The benchmark readout: three sections of label / bar / result /
 * previous.
 *
 * The previous column is the reason the tab exists in this shape. The
 * useful question is never "what does this machine score", it is "did
 * that setting help", and that is a comparison - so the run before last
 * is kept and drawn beside the current one rather than being thrown
 * away the moment you press the button again.
 *
 * The bar is only drawn where a 0..100 is honest, which here is the
 * graphics split: what share of a frame went on the 68k rasterising and
 * what share went on getting the result to the screen. psbench decides;
 * this file does not know what any of the numbers mean.
 */
static void draw_bench(PSUI *u, short vh)
{
	PSBROW rows[12];
	char buf[96];
	GRECT g;
	short sec, ch, y, n, i, rowh, noteh, floorr;
	short labw, barw, barx, valx, prevx, right;

	u->bdrawn = 0;
	u->bwanted = 0;
	u->btrunc = 0;
	if (!psui_bench_rect(u, &g))
		return;
	if (!vis(u, g.g_x, g.g_y, g.g_w, g.g_h))
		return;

	apj_skin_9(vh, APJ_RG_GROUP, 0, g.g_x, g.g_y, g.g_w, g.g_h);

	ui_font(vh, F_BODY);
	ch = cellh(vh);
	right = (short) (g.g_x + g.g_w - M(SECPAD));

	/*
	 * A tighter row than the settings tabs use. Those rows carry
	 * controls and have to be big enough to hit; these carry text and
	 * nothing else, and there are a dozen of them plus three headings
	 * plus the compliance line, which does not fit at 26 points a row
	 * in a window this size. Nothing here is clickable, so nothing is
	 * lost by closing them up.
	 */
	rowh = (short) (ch + M(4));

	/*
	 * Reserve the compliance line BEFORE laying the rows out. Drawing
	 * it at a fixed offset from the bottom afterwards is how it ended
	 * up printed through the last two rows.
	 */
	ui_font(vh, F_SMALL);
	noteh = (u->bnote && u->bnote[0])
	      ? (short) (cellh(vh) * 2 + M(4)) : 0;
	ui_font(vh, F_BODY);
	floorr = (short) (g.g_y + g.g_h - noteh);

	/* four columns, and the value column gets whatever is left */
	/* ten cells: the longest label is "Frame rate" / "Throughput" /
	 * "Tight loop", all exactly ten, and every cell given to this
	 * column comes off the value beside it */
	labw  = (short) (cellw(vh) * 10);
	barw  = M(64);
	barx  = (short) (g.g_x + M(SECPAD) + labw + M(GAP));
	valx  = (short) (barx + barw + M(GAP));
	/* twelve characters: the widest previous figure any row produces is
	 * "10.90 68000" at eleven, and every character given to this column
	 * is one taken off the value beside it */
	prevx = (short) (right - cellw(vh) * 12);
	if (prevx < valx + M(80))
	{
		prevx = right;			/* too narrow: drop the column */
		barw = 0;
		valx = barx;
	}

	y = (short) (g.g_y + M(SECPAD));

	for (sec = 0; sec < PSB_SEC_N; sec++)
	{
		n = psbench_rows(u->bench, u->bprev, sec, rows,
		                 (short) (sizeof(rows) / sizeof(rows[0])));
		if (n <= 0)
			continue;
		u->bwanted = (short) (u->bwanted + n);
		/* a value that was already cut on its way into the row -
		 * before this file saw it - counts as truncated too */
		for (i = 0; i < n; i++)
			if (rows[i].cut)
				u->btrunc++;

		if (y + rowh > floorr)
			break;

		ui_font(vh, F_SMALL);
		apj_skin_text(vh, (short) (g.g_x + M(SECPAD)), y,
		              apj_skin_pen(APJ_X_MUTED), psbench_sec_name(sec));
		y = (short) (y + cellh(vh) + M(2));
		ui_font(vh, F_BODY);
		ch = cellh(vh);

		for (i = 0; i < n; i++)
		{
			short ty = (short) (y + (rowh - ch) / 2);
			short vx = valx;

			if (y + rowh > floorr)
				break;

			/* alternating ground, so a long section stays readable */
			if (i & 1)
				apj_fill(vh, (short) (g.g_x + M(4)), y,
				         (short) (g.g_w - M(8)), rowh,
				         apj_skin_pen(APJ_X_ROW));

			fit_text(vh, buf, (short) sizeof(buf), rows[i].label, labw);
			apj_skin_text(vh, (short) (g.g_x + M(SECPAD)), ty,
			              apj_skin_pen(APJ_X_MUTED), buf);

			if (rows[i].pct >= 0 && barw > 0)
			{
				short by = (short) (y + (rowh - M(BARH)) / 2);
				short fill = (short) ((long) barw * rows[i].pct / 100L);

				if (apj_skin_has(APJ_RG_SEEK))
				{
					apj_skin_9(vh, APJ_RG_SEEK, APJ_SK_TRACK,
					           barx, by, barw, M(BARH));
					if (fill > 0)
						apj_skin_9(vh, APJ_RG_SEEK, APJ_SK_FILL,
						           barx, by, fill, M(BARH));
				}
				else
				{
					apj_fill(vh, barx, by, barw, M(BARH),
					         apj_skin_pen(APJ_R_DARK));
					if (fill > 0)
						apj_fill(vh, barx, by, fill, M(BARH),
						         apj_skin_pen(APJ_R_ACCENT));
				}
			}
			else if (barw > 0)
				vx = barx;	/* no bar: the value starts here */

			/*
			 * Only give up the width for a previous figure if
			 * this row HAS one. On a first run there is nothing
			 * in that column and every value was being cut to
			 * make room for it.
			 */
			if (fit_text(vh, buf, (short) sizeof(buf), rows[i].value,
			             (short) ((rows[i].prev[0] && prevx < right)
			                      ? prevx - vx - M(GAP)
			                      : right - vx)))
				u->btrunc++;
			apj_skin_text(vh, vx, ty, apj_skin_pen(APJ_R_TEXT), buf);

			if (rows[i].prev[0] && prevx < right)
			{
				short tw;

				fit_text(vh, buf, (short) sizeof(buf), rows[i].prev,
				         (short) (right - prevx));
				tw = (short) (cellw(vh) * (short) strlen(buf));
				apj_skin_text(vh, (short) (right - tw), ty,
				              apj_skin_pen(APJ_X_MUTED), buf);
			}
			y = (short) (y + rowh);
			u->bdrawn++;
		}
		y = (short) (y + M(3));
	}

	/*
	 * The CoreMark compliance line, at the foot, verbatim. The run
	 * rules want a score quoted as "CoreMark 1.0 : N / <compiler>
	 * <flags> / <memory>", and the only honest way to show that is the
	 * string CoreMark itself produced - so it is wrapped, not
	 * reformatted, and it says so when the run was too short to report.
	 */
	if (u->bnote && u->bnote[0])
	{
		const char *p = u->bnote;
		short indent = M(SECPAD);

		ui_font(vh, F_SMALL);
		ch = cellh(vh);
		y = floorr;			/* the space kept back for it */
		while (*p && y + ch <= g.g_y + g.g_h)
		{
			short max = (short) ((g.g_w - indent - M(SECPAD)) / cellw(vh));
			short take, brk;

			if (max < 4)
				break;
			if (max > (short) sizeof(buf) - 1)
				max = (short) (sizeof(buf) - 1);
			take = (short) strlen(p);
			if (take > max)
			{
				for (brk = max; brk > 0 && p[brk] != ' '; brk--)
					;
				take = brk > 0 ? brk : max;
			}
			memcpy(buf, p, (size_t) take);
			buf[take] = '\0';
			apj_skin_text(vh, (short) (g.g_x + indent), y,
			              apj_skin_pen(APJ_X_MUTED), buf);
			y = (short) (y + ch);
			p += take;
			while (*p == ' ')
				p++;
			indent = M(16);
		}
		ui_font(vh, F_BODY);
	}
}

/*
 * The strip above the results: which CoreMark build the button will run.
 * Two of them, because that is the whole question the pair answers - the
 * same benchmark on the same machine, compiled for a 68000 and for an
 * 020-and-up. The 020 build is not offered on a CPU that cannot run it.
 */
void psui_draw_bench_head(PSUI *u, short vh)
{
	static const char *const tname[PSB_CM_N] = { "68000", "020+" };
	GRECT g;
	short ch, i, x, bw;

	if (!psui_bench_head(u, &g))
		return;
	if (!vis(u, g.g_x, g.g_y, g.g_w, g.g_h))
		return;

	ui_font(vh, F_BODY);
	ch = cellh(vh);
	apj_skin_text(vh, g.g_x, (short) (g.g_y + (g.g_h - ch) / 2),
	              apj_skin_pen(APJ_X_MUTED), "CoreMark build:");

	bw = (short) (cellw(vh) * 7);
	x = (short) (g.g_x + cellw(vh) * 16);
	for (i = 0; i < PSB_CM_N; i++)
	{
		short on = (i == u->cmtarget);
		short off = (i > u->cmmax);
		short tw;
		char buf[16];

		apj_skin_9(vh, APJ_RG_BTN,
		           off ? APJ_ST_ON
		               : (on ? APJ_ST_ON
		                     : state_of(u, (short) (PSW_CMTARGET + i), 0)),
		           x, (short) (g.g_y + M(2)), bw, (short) (g.g_h - M(4)));
		fit_text(vh, buf, (short) sizeof(buf), tname[i], (short) (bw - M(6)));
		tw = (short) (cellw(vh) * (short) strlen(buf));
		apj_skin_text(vh, (short) (x + (bw - tw) / 2),
		              (short) (g.g_y + (g.g_h - ch) / 2),
		              apj_skin_pen(off ? APJ_R_DISABLED : APJ_R_TEXT), buf);
		x = (short) (x + bw + M(GAP));
	}
}

void psui_draw_rows(PSUI *u, short vh)
{
	short i;

	if (!apj_skin_ok())
	{
		draw_plain(u, vh);
		return;
	}
	if (u->listr.g_h <= 0)
		return;
	apj_fill(vh, u->listr.g_x, u->listr.g_y, u->listr.g_w, u->listr.g_h,
	         apj_skin_pen(APJ_R_PANEL));
	for (i = 0; i < u->visrows; i++)
		draw_row(u, vh, i);

	if (psui_scroll_needed(u))
	{
		const APJ_LAY *sb = apj_lay_find(u->lay, u->nlay, PSW_SCROLL);
		GRECT t;

		if (sb)
		{
			apj_skin_9(vh, APJ_RG_VSCROLL, APJ_VS_TROUGH,
			           sb->x, sb->y, sb->w, sb->h);
			psui_thumb_rect(u, &t);
			apj_skin_9(vh, APJ_RG_VSCROLL,
			           u->dragging ? APJ_VS_HELD : APJ_VS_THUMB,
			           t.g_x, t.g_y, t.g_w, t.g_h);
		}
	}
	draw_drop(u, vh);
	ui_font(vh, F_BODY);
}

void psui_draw_clip(PSUI *u, short vh, const GRECT *clip)
{
	GRECT c = *clip;

	if (c.g_x < u->work.g_x) { c.g_w -= u->work.g_x - c.g_x; c.g_x = u->work.g_x; }
	if (c.g_y < u->work.g_y) { c.g_h -= u->work.g_y - c.g_y; c.g_y = u->work.g_y; }
	if (c.g_x + c.g_w > u->work.g_x + u->work.g_w)
		c.g_w = (short) (u->work.g_x + u->work.g_w - c.g_x);
	if (c.g_y + c.g_h > u->work.g_y + u->work.g_h)
		c.g_h = (short) (u->work.g_y + u->work.g_h - c.g_y);
	if (c.g_w <= 0 || c.g_h <= 0)
		return;
	u->clip = c;

	if (!apj_skin_ok())
	{
		draw_plain(u, vh);
		u->clip = u->work;
		return;
	}
	ui_font(vh, F_BODY);
	apj_fill(vh, c.g_x, c.g_y, c.g_w, c.g_h, apj_skin_pen(APJ_R_PANEL));
	psui_draw_tabs(u, vh);
	psui_draw_rows(u, vh);
	psui_draw_bench_head(u, vh);
	draw_bench(u, vh);
	psui_draw_status(u, vh);
	ui_font(vh, F_BODY);
	u->clip = u->work;
}

void psui_draw(PSUI *u, short vh)
{
	u->clip = u->work;
	psui_draw_clip(u, vh, &u->work);
}

/*
 * No skin file: the flat apjgui controls, and a line saying which file
 * was wanted and where it was looked for. Falling back silently makes a
 * missing SKINS\ directory indistinguishable from a stale build, which is
 * a lesson this project has already paid for once.
 */
static void draw_plain(PSUI *u, short vh)
{
	short ch = cellh(vh), y, i;
	char buf[160];

	apj_fill(vh, u->work.g_x, u->work.g_y, u->work.g_w, u->work.g_h,
	         apj_pen(APJ_R_PANEL));

	sprintf(buf, "No skin: %s not in %s", apj_skin_wanted(), apj_skin_tried());
	apj_text(vh, (short) (u->work.g_x + 2),
	         (short) (u->work.g_y + u->work.g_h - ch),
	         apj_pen(APJ_R_DISABLED), buf);

	sprintf(buf, "PiSTorm settings - %s   (%d items)",
	        psui_tab_name(u->tab), (int) u->nidx);
	apj_text(vh, (short) (u->work.g_x + 2), (short) (u->work.g_y + 2),
	         apj_pen(APJ_R_TEXT), buf);

	y = (short) (u->work.g_y + 2 + ch * 2);
	for (i = 0; i < u->visrows && u->top + i < u->nidx; i++)
	{
		const PSROW *r = &u->rows[u->idx[u->top + i]];
		char val[32];

		if (r->kind == PS_K_ENUM && r->value >= 0 && r->value < r->nenum)
			strcpy(val, r->label[r->value]);
		else if (r->kind == PS_K_STR)
			sprintf(val, "%.24s", r->str);
		else
			int_text(val, r, r->value);
		sprintf(buf, "%-28.28s %s", r->title, val);
		apj_text(vh, (short) (u->work.g_x + 2), y, apj_pen(APJ_R_TEXT), buf);
		y = (short) (y + ch);
	}
}
