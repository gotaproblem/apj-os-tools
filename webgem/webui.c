/*
 * webui.c - see webui.h. Every control is a rectangle out of the skin
 * sheet; the page is one blit out of the host-filled buffer; the 68k
 * touches no pixels but the lock glyph in the address field.
 *
 * Geometry is written in POINTS and put through apj_skin_m(), so one
 * source serves 1x, 1.25x and 1.75x. Every strip that holds text is sized
 * from the cell the VDI really gives, with the sheet's metric as a floor.
 * skins/preview.py webgem_layout() is the same geometry in Python; keep
 * the two in step.
 */

#include <stdio.h>
#include <string.h>

#include "webui.h"

#define PAD		8	/* window inset, points              */
#define TABH		30	/* tab strip                         */
#define TABW		200	/* one tab                           */
#define STATH		18	/* status strip                      */
#define BADGEH		16
#define ADDRMIN		40	/* the address field can shrink to   */
#define PAGEMIN		120	/* the smallest page, points         */

#define M(pt)		apj_skin_m(pt)

static void draw_plain(WEBUI *u, short vh);

/* three sizes out of the APJ*.FNT set fVDI loads */
enum { F_SMALL, F_BODY, F_TITLE };

static void ui_font(short vh, short kind)
{
	static const short ladder[3][3] =	/* columns: 100%, 125%, 175% */
	{
		{ 10, 11, 13 },		/* small */
		{ 11, 12, 15 },		/* body  */
		{ 13, 15, 20 }		/* title */
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

/* fit s into avail pixels of the current font, "..." if cut */
static void fit_text(short vh, char *out, short outsz, const char *s, short avail)
{
	short cw = cellw(vh), max, len;

	max = (short) (avail / cw);
	if (max > outsz - 1)
		max = (short) (outsz - 1);
	len = (short) strlen(s);
	if (len <= max)
	{
		strcpy(out, s);
		return;
	}
	if (max < 4)
	{
		out[0] = '\0';
		return;
	}
	memcpy(out, s, (size_t) (max - 3));
	strcpy(out + max - 3, "...");
}

/* does this rectangle meet the redraw clip? */
static int vis(const WEBUI *u, short x, short y, short w, short h)
{
	return !(x >= u->clip.g_x + u->clip.g_w || x + w <= u->clip.g_x ||
	         y >= u->clip.g_y + u->clip.g_h || y + h <= u->clip.g_y);
}

/* a widget that does not fit inside the work area is left out of the
 * layout rather than drawn over the edge */
static void add(WEBUI *u, short id, short x, short y, short w, short h)
{
	if (u->nlay >= WEBUI_MAXLAY - 1)
		return;
	if (w <= 0 || h <= 0 || x < u->work.g_x || y < u->work.g_y ||
	    x + w > u->work.g_x + u->work.g_w || y + h > u->work.g_y + u->work.g_h)
		return;
	u->lay[u->nlay].id = id;
	u->lay[u->nlay].x = x;
	u->lay[u->nlay].y = y;
	u->lay[u->nlay].w = w;
	u->lay[u->nlay].h = h;
	u->nlay++;
	u->lay[u->nlay].id = -1;
}

static const APJ_LAY *find(WEBUI *u, short id)
{
	return apj_lay_find(u->lay, u->nlay, id);
}

/* a fill cut to the redraw clip: an erase must not reach past the part
 * being repainted, whatever the VDI clip happens to be */
static void fill_clip(WEBUI *u, short vh, short x, short y, short w, short h, short pen)
{
	short x1 = (short) (x + w), y1 = (short) (y + h);

	if (x < u->clip.g_x) x = u->clip.g_x;
	if (y < u->clip.g_y) y = u->clip.g_y;
	if (x1 > u->clip.g_x + u->clip.g_w) x1 = (short) (u->clip.g_x + u->clip.g_w);
	if (y1 > u->clip.g_y + u->clip.g_h) y1 = (short) (u->clip.g_y + u->clip.g_h);
	if (x1 > x && y1 > y)
		apj_fill(vh, x, y, (short) (x1 - x), (short) (y1 - y), pen);
}

/* ---------------------------------------------------------------- layout */

/* the measured sizes the layout is built from */
static void measure(short vh, short *sw, short *sh, short *bw, short *bh,
                    short *tw, short *th, short *stath, short *tabh)
{
	ui_font(vh, F_SMALL);
	*sw = cellw(vh);
	*sh = cellh(vh);
	ui_font(vh, F_BODY);
	*bw = cellw(vh);
	*bh = cellh(vh);

	if (apj_skin_ok())
	{
		*tw = apj_skin_tilew();
		*th = apj_skin_tileh();
	}
	else
	{
		*tw = (short) (*bw * 5);
		*th = (short) (*bh + 4);
	}
	if (*th < *bh + M(6))
		*th = (short) (*bh + M(6));

	*stath = (short) (*sh + M(6));
	if (*stath < M(STATH))
		*stath = M(STATH);
	*tabh = (short) (*sh + M(12));
	if (*tabh < M(TABH))
		*tabh = M(TABH);
}

/* the toolbar's own width, so the window minimum can carry all of it */
static short toolbar_width(short vh)
{
	short sw, sh, bw, bh, tw, th, stath, tabh;

	measure(vh, &sw, &sh, &bw, &bh, &tw, &th, &stath, &tabh);
	return (short) (M(PAD) * 2 +
	                tw * 4 + M(2) * 3 + M(10) +          /* back fwd reload home */
	                M(ADDRMIN) + M(10) +                 /* the field           */
	                tw * 3 + M(2) * 2);                  /* bookmark dl menu    */
}

void webui_minsize(short vh, short *w, short *h)
{
	short sw, sh, bw, bh, tw, th, stath, tabh;

	measure(vh, &sw, &sh, &bw, &bh, &tw, &th, &stath, &tabh);
	*w = toolbar_width(vh);
	*h = (short) (M(PAD) * 2 + th + M(PAGEMIN) + stath);
}

void webui_layout(WEBUI *u, short vh, short wx, short wy, short ww, short wh)
{
	short pad = M(PAD), gap = M(2);
	short sw, sh, bw, bh, tw, th, stath, tabh;
	short x, rx, y, py, ph, i, tabw, badgew;

	measure(vh, &sw, &sh, &bw, &bh, &tw, &th, &stath, &tabh);
	if (!apj_skin_ok())
		pad = 2;

	u->work.g_x = wx; u->work.g_y = wy;
	u->work.g_w = ww; u->work.g_h = wh;
	u->clip = u->work;
	u->nlay = 0;
	u->lay[0].id = -1;
	u->smallh = sh;

	/* ---- tab strip: only past one tab -------------------------------- */
	y = wy;
	if (u->ntabs > 1)
	{
		tabw = M(TABW);
		x = wx;
		for (i = 0; i < u->ntabs && i < WB_TABMAX - WB_TAB0; i++)
		{
			if (x + tabw > wx + ww - M(8) - tw)
				break;					/* the rest are not shown */
			add(u, (short) (WB_TAB0 + i), x, y, tabw, tabh);
			x = (short) (x + tabw + gap);
		}
		add(u, WB_TABNEW, (short) (x + M(6)), (short) (y + (tabh - tw) / 2), tw, tw);
		y = (short) (y + tabh);
	}

	/* ---- toolbar, left group ---------------------------------------- */
	y = (short) (y + pad);
	x = (short) (wx + pad);
	add(u, WB_BACK, x, y, tw, th);   x = (short) (x + tw + gap);
	add(u, WB_FWD, x, y, tw, th);    x = (short) (x + tw + gap);
	add(u, WB_RELOAD, x, y, tw, th); x = (short) (x + tw + gap);
	add(u, WB_HOME, x, y, tw, th);   x = (short) (x + tw + M(10));

	/* ---- toolbar, right group: from the right edge back ------------- */
	rx = (short) (wx + ww - pad - tw);
	add(u, WB_MENU, rx, y, tw, th);
	rx = (short) (rx - gap - tw);
	add(u, WB_DOWNLOAD, rx, y, tw, th);
	rx = (short) (rx - gap - tw);
	add(u, WB_BOOKMARK, rx, y, tw, th);
	rx = (short) (rx - M(10));

	/* the address field fills what is left; when even the minimum will
	 * not fit, the right group goes first, then home */
	if (rx - x < M(ADDRMIN))
	{
		u->nlay = 0;
		u->lay[0].id = -1;
		x = (short) (wx + pad);
		add(u, WB_BACK, x, y, tw, th);   x = (short) (x + tw + gap);
		add(u, WB_FWD, x, y, tw, th);    x = (short) (x + tw + gap);
		add(u, WB_RELOAD, x, y, tw, th); x = (short) (x + tw + M(10));
		rx = (short) (wx + ww - pad);
	}
	if (rx - x >= M(ADDRMIN))
		add(u, WB_ADDRESS, x, y, (short) (rx - x), th);

	/* ---- the page: full bleed between the band and the status strip - */
	py = (short) (y + th + pad);
	ph = (short) (wy + wh - py - stath);
	u->view.g_x = wx;
	u->view.g_y = py;
	u->view.g_w = ww;
	u->view.g_h = ph;
	if (ph >= 1)
		add(u, WB_PAGE, wx, py, ww, ph);
	if (!find(u, WB_PAGE))
		u->view.g_w = u->view.g_h = 0;		/* no room: nothing to fetch */

	/* ---- status strip: the badges, right to left, as hit targets ----- */
	ui_font(vh, F_SMALL);
	rx = (short) (wx + ww - pad);
	badgew = (short) (sw * 5 + M(12));			/* "no JS"          */
	add(u, WB_BADGE_JS, (short) (rx - badgew), (short) (wy + wh - stath + (stath - M(BADGEH)) / 2),
	    badgew, M(BADGEH));
	rx = (short) (rx - badgew - M(6));
	badgew = (short) (sw * 10 + M(12));			/* "no blocker" */
	add(u, WB_BADGE_BLOCK, (short) (rx - badgew), (short) (wy + wh - stath + (stath - M(BADGEH)) / 2),
	    badgew, M(BADGEH));
	rx = (short) (rx - badgew - M(6));
	badgew = (short) (sw * 12 + M(12));			/* "loading 100%" */
	add(u, WB_BADGE_LOAD, (short) (rx - badgew), (short) (wy + wh - stath + (stath - M(BADGEH)) / 2),
	    badgew, M(BADGEH));
	ui_font(vh, F_BODY);
}

void webui_bbox(WEBUI *u, GRECT *r)
{
	short i, x0 = 0x7fff, y0 = 0x7fff, x1 = -1, y1 = -1;

	for (i = 0; i < u->nlay && u->lay[i].id >= 0; i++)
	{
		if (u->lay[i].id == WB_PAGE)
			continue;
		if (u->lay[i].x < x0) x0 = u->lay[i].x;
		if (u->lay[i].y < y0) y0 = u->lay[i].y;
		if (u->lay[i].x + u->lay[i].w > x1) x1 = (short) (u->lay[i].x + u->lay[i].w);
		if (u->lay[i].y + u->lay[i].h > y1) y1 = (short) (u->lay[i].y + u->lay[i].h);
	}
	if (x1 < 0)
	{
		*r = u->work;
		return;
	}
	r->g_x = x0;
	r->g_y = y0;
	r->g_w = (short) (x1 - x0);
	r->g_h = (short) (y1 - y0);
}

/* ------------------------------------------------------------- rectangles */

void webui_widget_rect(WEBUI *u, short id, GRECT *r)
{
	const APJ_LAY *l = find(u, id);

	if (!l)
	{
		r->g_x = r->g_y = r->g_w = r->g_h = 0;
		return;
	}
	r->g_x = l->x; r->g_y = l->y; r->g_w = l->w; r->g_h = l->h;
}

void webui_tabs_rect(WEBUI *u, GRECT *r)
{
	short tabh;

	if (u->ntabs <= 1)
	{
		r->g_x = r->g_y = r->g_w = r->g_h = 0;
		return;
	}
	tabh = (short) (u->smallh + M(12));
	if (tabh < M(TABH))
		tabh = M(TABH);
	r->g_x = u->work.g_x;
	r->g_y = u->work.g_y;
	r->g_w = u->work.g_w;
	r->g_h = tabh;
}

/* the toolbar band: from under the tab strip to the top of the page */
void webui_band_rect(WEBUI *u, GRECT *r)
{
	GRECT t;

	webui_tabs_rect(u, &t);
	r->g_x = u->work.g_x;
	r->g_y = (short) (u->work.g_y + t.g_h);
	r->g_w = u->work.g_w;
	r->g_h = (short) (u->view.g_y - r->g_y);
	if (r->g_h < 0)
		r->g_h = 0;
}

void webui_pane_rect(WEBUI *u, GRECT *r) { webui_widget_rect(u, WB_PAGE, r); }
void webui_view_rect(WEBUI *u, GRECT *r) { *r = u->view; }

void webui_status_rect(WEBUI *u, GRECT *r)
{
	short sh = u->smallh, stath = (short) (sh + M(6));

	if (stath < M(STATH))
		stath = M(STATH);
	r->g_x = u->work.g_x;
	r->g_y = (short) (u->work.g_y + u->work.g_h - stath);
	r->g_w = u->work.g_w;
	r->g_h = stath;
}

short webui_hit(WEBUI *u, short mx, short my)
{
	return apj_lay_hit(u->lay, u->nlay, mx, my);
}

/* ------------------------------------------------------------------ draw */

static short state_of(WEBUI *u, short id, short on)
{
	if (u->press == id)
		return APJ_ST_PRESS;
	if (on)
		return APJ_ST_ON;
	if (u->hover == id)
		return APJ_ST_HOVER;
	return APJ_ST_NORM;
}

/* a toolbar tile; on a sheet without TILE3 the glyph gets a plate and a
 * one-character label instead, so an old skin set still has a toolbar */
static void tile_at(WEBUI *u, short vh, short id, short glyph, short on,
                    const char *fallback)
{
	const APJ_LAY *l = find(u, id);
	short st;

	if (!l || !vis(u, l->x, l->y, l->w, l->h))
		return;
	st = state_of(u, id, on);
	if (apj_skin_has_tile(glyph))
	{
		apj_skin_tile(vh, glyph, st, l->x, l->y);
		return;
	}
	apj_skin_9(vh, APJ_RG_BTN, st, l->x, l->y, l->w, l->h);
	ui_font(vh, F_BODY);
	apj_skin_text(vh, (short) (l->x + (l->w - cellw(vh) * (short) strlen(fallback)) / 2),
	              (short) (l->y + (l->h - cellh(vh)) / 2),
	              apj_skin_pen(st == APJ_ST_ON ? APJ_R_ACCENT : APJ_R_TEXT), fallback);
}

/* The address field. Idle: the lock (https) and the URI, cut with "..."
 * at the right. Focused: the text with a caret after it; when it is longer
 * than the box the START is scrolled off, so the end being typed stays in
 * view. With addr_all the text sits on SELBG: the next key replaces it. */
static void draw_address(WEBUI *u, short vh)
{
	const APJ_LAY *l = find(u, WB_ADDRESS);
	short g = apj_skin_glyphsz(), cw, ch, tx, ty, avail, len, show;
	short focused = u->focus == WEB_FOCUS_ADDRESS;
	const char *s;
	char fit[WEB_ADDR_MAX];

	if (!l || !vis(u, l->x, l->y, l->w, l->h))
		return;
	apj_skin_9(vh, APJ_RG_FIELD, focused ? APJ_FLD_FOCUS : APJ_FLD_NORM,
	           l->x, l->y, l->w, l->h);
	tx = (short) (l->x + M(8));
	if (!focused && u->secure && apj_skin_ok())
	{
		apj_skin_glyph(vh, APJ_G_LOCK, apj_skin_pen(APJ_X_MUTED),
		               tx, (short) (l->y + (l->h - g) / 2));
		tx = (short) (tx + g + M(6));
	}
	ui_font(vh, F_BODY);
	cw = cellw(vh);
	ch = cellh(vh);
	ty = (short) (l->y + (l->h - ch) / 2);
	avail = (short) (l->x + l->w - M(8) - tx - (focused ? cw : 0));
	if (avail < cw)
		return;
	s = focused ? u->addr : (u->uri ? u->uri : "");
	if (!focused)
	{
		fit_text(vh, fit, (short) sizeof(fit), s, avail);
		if (fit[0])
			apj_skin_text(vh, tx, ty, apj_skin_pen(APJ_R_TEXT), fit);
		else if (!s[0])
			apj_skin_text(vh, tx, ty, apj_skin_pen(APJ_X_MUTED), "address or search");
		return;
	}
	len = (short) strlen(s);
	show = (short) (avail / cw);
	if (show > (short) sizeof(fit) - 1)
		show = (short) (sizeof(fit) - 1);
	if (len > show)
		s += len - show;				/* the tail, where the caret is */
	strcpy(fit, s);
	len = (short) strlen(fit);
	if (u->addr_all && len)
	{
		apj_fill(vh, (short) (tx - M(2)), (short) (l->y + M(4)),
		         (short) (cw * len + M(4)), (short) (l->h - M(8)),
		         apj_skin_pen(APJ_R_SELBG));
		apj_skin_text(vh, tx, ty, apj_skin_pen(APJ_R_SELFG), fit);
	}
	else if (len)
		apj_skin_text(vh, tx, ty, apj_skin_pen(APJ_R_TEXT), fit);
	apj_fill(vh, (short) (tx + cw * len + 1), (short) (l->y + M(5)),
	         (short) (M(1) > 1 ? M(1) : 1), (short) (l->h - M(10)),
	         apj_skin_pen(APJ_R_ACCENT));
	/* progress: a thin accent rail along the bottom of the field */
	if (u->loading && u->progress > 0)
	{
		long w = (long) (l->w - M(8)) * u->progress / 1000;

		apj_fill(vh, (short) (l->x + M(4)), (short) (l->y + l->h - M(3)),
		         (short) w, M(2), apj_skin_pen(APJ_R_ACCENT));
	}
}

static void draw_band(WEBUI *u, short vh)
{
	GRECT b;
	const APJ_LAY *l;

	webui_band_rect(u, &b);
	if (!vis(u, b.g_x, b.g_y, b.g_w, b.g_h))
		return;
	/* the mica wash under the toolbar - only the tiles the clip meets,
	 * and never below the band (the wash is taller than the band at
	 * every scale) - then the controls on top */
	if (apj_skin_has(APJ_RG_PANELTOP))
	{
		short ww, wh;

		apj_skin_size(APJ_RG_PANELTOP, &ww, &wh);
		apj_skin_tilexh(vh, APJ_RG_PANELTOP, 0, b.g_x, b.g_y, b.g_w, b.g_h,
		                u->clip.g_x, u->clip.g_w);
		if (wh < b.g_h)
			fill_clip(u, vh, b.g_x, (short) (b.g_y + wh), b.g_w, (short) (b.g_h - wh),
			          apj_skin_pen(APJ_R_PANEL));
	}
	else
		fill_clip(u, vh, b.g_x, b.g_y, b.g_w, b.g_h, apj_skin_pen(APJ_R_PANEL));

	/* the hairline between the band and the page: the band's last row */
	if (b.g_h > 0 && vis(u, b.g_x, (short) (b.g_y + b.g_h - 1), b.g_w, 1))
		apj_fill(vh, b.g_x, (short) (b.g_y + b.g_h - 1), b.g_w, 1, apj_skin_pen(APJ_R_BORDER));

	tile_at(u, vh, WB_BACK,     APJ_G_BACK,     0, "<");
	tile_at(u, vh, WB_FWD,      APJ_G_FWD,      0, ">");
	tile_at(u, vh, WB_RELOAD,   u->loading ? APJ_G_STOPX : APJ_G_RELOAD, 0,
	        u->loading ? "x" : "R");
	tile_at(u, vh, WB_HOME,     APJ_G_HOME,     0, "H");
	tile_at(u, vh, WB_BOOKMARK, APJ_G_BOOKMARK, 0, "*");
	tile_at(u, vh, WB_DOWNLOAD, APJ_G_DOWNLOAD, 0, "v");
	tile_at(u, vh, WB_MENU,     APJ_G_LIST,     0, "=");
	draw_address(u, vh);

	/* the progress rail when the field is idle: under the URI */
	l = find(u, WB_ADDRESS);
	if (l && u->focus != WEB_FOCUS_ADDRESS && u->loading && u->progress > 0 &&
	    vis(u, l->x, (short) (l->y + l->h - M(3)), l->w, M(3)))
	{
		long w = (long) (l->w - M(8)) * u->progress / 1000;

		apj_fill(vh, (short) (l->x + M(4)), (short) (l->y + l->h - M(3)),
		         (short) w, M(2), apj_skin_pen(APJ_R_ACCENT));
	}
}

static void draw_tabs(WEBUI *u, short vh)
{
	GRECT t;
	const APJ_LAY *l;
	short i, ch, g = apj_skin_glyphsz();
	char fit[64];

	webui_tabs_rect(u, &t);
	if (t.g_h <= 0 || !vis(u, t.g_x, t.g_y, t.g_w, t.g_h))
		return;
	if (apj_skin_has(APJ_RG_TABBAR))
		apj_skin_tilexh(vh, APJ_RG_TABBAR, 0, t.g_x, t.g_y, t.g_w, t.g_h,
		                u->clip.g_x, u->clip.g_w);
	else
		fill_clip(u, vh, t.g_x, t.g_y, t.g_w, t.g_h, apj_skin_pen(APJ_R_PANEL));
	ui_font(vh, F_SMALL);
	ch = cellh(vh);
	for (i = 0; i < u->ntabs && i < WB_TABMAX - WB_TAB0; i++)
	{
		short sel = i == u->tab_sel;
		const char *name;

		l = find(u, (short) (WB_TAB0 + i));
		if (!l || !vis(u, l->x, l->y, l->w, l->h))
			continue;
		apj_skin_9(vh, APJ_RG_TAB, sel ? APJ_ST_ON : state_of(u, (short) (WB_TAB0 + i), 0),
		           l->x, l->y, l->w, l->h);
		if (sel)
			/* the rail is a v_bar, not part of the plate: nine-slice fills
			 * the edge bands flat */
			apj_fill(vh, (short) (l->x + M(6)), (short) (l->y + l->h - M(3)),
			         (short) (l->w - M(12)), M(3), apj_skin_pen(APJ_R_ACCENT));
		name = u->tab_title ? u->tab_title(u->ctx, i) : "";
		fit_text(vh, fit, (short) sizeof(fit), name ? name : "",
		         (short) (l->w - M(16) - (sel ? g + M(8) : 0)));
		apj_skin_text(vh, (short) (l->x + M(10)), (short) (l->y + (l->h - ch) / 2),
		              apj_skin_pen(sel ? APJ_R_TEXT : APJ_X_MUTED), fit);
		if (sel && apj_skin_ok())
			apj_skin_glyph(vh, APJ_G_TABCLOSE, apj_skin_pen(APJ_X_MUTED),
			               (short) (l->x + l->w - M(8) - g), (short) (l->y + (l->h - g) / 2));
	}
	l = find(u, WB_TABNEW);
	if (l && vis(u, l->x, l->y, l->w, l->h) && apj_skin_ok())
		apj_skin_glyph(vh, APJ_G_TABNEW, apj_skin_pen(APJ_X_MUTED),
		               (short) (l->x + (l->w - g) / 2), (short) (l->y + (l->h - g) / 2));
}

static short canvas_pen(void)
{
	return apj_skin_pen(APJ_R_PAPER);
}

/* the page: the part of the buffer that meets the clip, in one blit */
static void draw_pane(WEBUI *u, short vh)
{
	GRECT v = u->view, c;
	const char *m = NULL;

	if (v.g_w <= 0 || v.g_h <= 0 || !vis(u, v.g_x, v.g_y, v.g_w, v.g_h))
		return;
	c = v;
	if (c.g_x < u->clip.g_x) { c.g_w -= u->clip.g_x - c.g_x; c.g_x = u->clip.g_x; }
	if (c.g_y < u->clip.g_y) { c.g_h -= u->clip.g_y - c.g_y; c.g_y = u->clip.g_y; }
	if (c.g_x + c.g_w > u->clip.g_x + u->clip.g_w) c.g_w = (short) (u->clip.g_x + u->clip.g_w - c.g_x);
	if (c.g_y + c.g_h > u->clip.g_y + u->clip.g_h) c.g_h = (short) (u->clip.g_y + u->clip.g_h - c.g_y);
	if (c.g_w <= 0 || c.g_h <= 0)
		return;

	if (u->msg == WEB_MSG_NONE && u->pagebuf.fd_addr)
	{
		MFDB scr;
		short pxy[8];

		scr.fd_addr = NULL;
		pxy[0] = (short) (c.g_x - v.g_x);
		pxy[1] = (short) (c.g_y - v.g_y);
		pxy[2] = (short) (pxy[0] + c.g_w - 1);
		pxy[3] = (short) (pxy[1] + c.g_h - 1);
		pxy[4] = c.g_x;
		pxy[5] = c.g_y;
		pxy[6] = (short) (c.g_x + c.g_w - 1);
		pxy[7] = (short) (c.g_y + c.g_h - 1);
		vro_cpyfm(vh, S_ONLY, pxy, &u->pagebuf, &scr);
		return;
	}

	/* no frame yet: the canvas and one line */
	apj_fill(vh, c.g_x, c.g_y, c.g_w, c.g_h, canvas_pen());
	switch (u->msg)
	{
		case WEB_MSG_WAIT:    m = (u->waittext && u->waittext[0]) ? u->waittext
		                                                        : "Connecting to psweb on the Pi..."; break;
		case WEB_MSG_NOTT:    m = "No TT-RAM for the page buffer"; break;
		case WEB_MSG_CRASHED: m = "The page crashed - reload to try again"; break;
		default:              m = u->loading ? "Loading..." : ""; break;
	}
	if (m && m[0])
	{
		ui_font(vh, F_BODY);
		apj_skin_text(vh, (short) (v.g_x + M(16)), (short) (v.g_y + M(16)),
		              apj_skin_pen(APJ_X_MUTED), m);
	}
}

static void badge_at(WEBUI *u, short vh, short id, short state, short pen, const char *s)
{
	const APJ_LAY *l = find(u, id);
	short cw, ch;
	char fit[32];

	if (!l || !vis(u, l->x, l->y, l->w, l->h))
		return;
	cw = cellw(vh);
	ch = cellh(vh);
	fit_text(vh, fit, (short) sizeof(fit), s, (short) (l->w - M(12)));
	apj_skin_9(vh, APJ_RG_BADGE, state, l->x, l->y, l->w, l->h);
	apj_skin_text(vh, (short) (l->x + (l->w - cw * (short) strlen(fit)) / 2),
	              (short) (l->y + (l->h - ch) / 2), apj_skin_pen(pen), fit);
}

static void draw_status(WEBUI *u, short vh)
{
	GRECT s;
	const APJ_LAY *l;
	short ch, avail;
	const char *left;
	char fit[200], buf[24];

	webui_status_rect(u, &s);
	if (!vis(u, s.g_x, s.g_y, s.g_w, s.g_h))
		return;
	/* flat PANEL by the sheet's own recipe: one bar, not 35 tile blits */
	fill_clip(u, vh, s.g_x, s.g_y, s.g_w, s.g_h, apj_skin_pen(APJ_R_PANEL));
	fill_clip(u, vh, s.g_x, s.g_y, s.g_w, 1, apj_skin_pen(APJ_R_BORDER));

	ui_font(vh, F_SMALL);
	ch = cellh(vh);
	badge_at(u, vh, WB_BADGE_JS, u->js_on ? APJ_BG_PLAIN : APJ_BG_WARN,
	         u->js_on ? APJ_X_MUTED : APJ_R_TEXT, u->js_on ? "JS" : "no JS");
	badge_at(u, vh, WB_BADGE_BLOCK, u->blocker_on ? APJ_BG_ACCENT : APJ_BG_PLAIN,
	         u->blocker_on ? APJ_X_ACCENT_INK : APJ_X_MUTED,
	         u->blocker_on ? "blocker" : "no blocker");
	if (u->loading)
	{
		sprintf(buf, "loading %d%%", (int) (u->progress / 10));
		badge_at(u, vh, WB_BADGE_LOAD, APJ_BG_PLAIN, APJ_X_MUTED, buf);
	}

	l = find(u, WB_BADGE_LOAD);
	avail = (short) ((l ? l->x : s.g_x + s.g_w - M(PAD)) - s.g_x - M(PAD) - M(6));
	left = (u->link && u->link[0]) ? u->link : (u->status ? u->status : "");
	if (avail > 0 && left[0])
	{
		fit_text(vh, fit, (short) sizeof(fit), left, avail);
		apj_skin_text(vh, (short) (s.g_x + M(PAD)), (short) (s.g_y + (s.g_h - ch) / 2),
		              apj_skin_pen(APJ_X_MUTED), fit);
	}
}

void webui_draw_tabs(WEBUI *u, short vh)   { draw_tabs(u, vh); }
void webui_draw_band(WEBUI *u, short vh)   { draw_band(u, vh); }
void webui_draw_pane(WEBUI *u, short vh)   { draw_pane(u, vh); }
void webui_draw_status(WEBUI *u, short vh) { draw_status(u, vh); }

/* no skin: the flat apjgui controls, so the browser still runs */
static void draw_plain(WEBUI *u, short vh)
{
	static const short wid[] = { WB_BACK, WB_FWD, WB_RELOAD, WB_HOME,
	                             WB_BOOKMARK, WB_DOWNLOAD, WB_MENU };
	static const char *lbl[] = { "<", ">", "R", "H", "*", "v", "=" };
	short i, ch;
	const APJ_LAY *l;

	ui_font(vh, F_BODY);
	ch = cellh(vh);
	apj_fill(vh, u->clip.g_x, u->clip.g_y, u->clip.g_w, u->clip.g_h, apj_pen(APJ_R_PANEL));
	if (apj_skin_wanted() && apj_skin_wanted()[0])
	{
		char why[200];

		sprintf(why, "No skin: %s not in %s", apj_skin_wanted(), apj_skin_tried());
		apj_text(vh, (short) (u->work.g_x + 2),
		         (short) (u->work.g_y + u->work.g_h - ch),
		         apj_pen(APJ_R_DISABLED), why);
	}
	for (i = 0; i < 7; i++)
	{
		l = find(u, wid[i]);
		if (l)
			apj_button(vh, l->x, l->y, l->w, l->h, lbl[i], u->press == wid[i], 0);
	}
	l = find(u, WB_ADDRESS);
	if (l)
	{
		const char *s = u->focus == WEB_FOCUS_ADDRESS ? u->addr : (u->uri ? u->uri : "");

		apj_fill(vh, l->x, l->y, l->w, l->h, apj_pen(APJ_R_PAPER));
		apj_text(vh, (short) (l->x + 2), (short) (l->y + (l->h - ch) / 2), apj_pen(APJ_R_TEXT), s);
	}
	l = find(u, WB_PAGE);
	if (l)
	{
		if (u->msg == WEB_MSG_NONE && u->pagebuf.fd_addr && u->view.g_w > 0 && u->view.g_h > 0)
		{
			MFDB scr;
			short pxy[8];

			scr.fd_addr = NULL;
			pxy[0] = 0; pxy[1] = 0;
			pxy[2] = (short) (u->view.g_w - 1); pxy[3] = (short) (u->view.g_h - 1);
			pxy[4] = u->view.g_x; pxy[5] = u->view.g_y;
			pxy[6] = (short) (u->view.g_x + u->view.g_w - 1);
			pxy[7] = (short) (u->view.g_y + u->view.g_h - 1);
			vro_cpyfm(vh, S_ONLY, pxy, &u->pagebuf, &scr);
		}
		else
			apj_fill(vh, l->x, l->y, l->w, l->h, apj_pen(APJ_R_PAPER));
	}
	if (u->link && u->link[0])
		apj_text(vh, (short) (u->work.g_x + 2), (short) (u->work.g_y + u->work.g_h - ch),
		         apj_pen(APJ_R_TEXT), u->link);
}

/*
 * A redraw of the parts that meet clip - what a WM_REDRAW wants. A window
 * dragged across this one exposes it a thin strip at a time; drawing
 * everything for each strip is what made that drag stutter over MP3GEM.
 */
void webui_draw_clip(WEBUI *u, short vh, const GRECT *clip)
{
	GRECT c = *clip;

	if (c.g_x < u->work.g_x) { c.g_w -= u->work.g_x - c.g_x; c.g_x = u->work.g_x; }
	if (c.g_y < u->work.g_y) { c.g_h -= u->work.g_y - c.g_y; c.g_y = u->work.g_y; }
	if (c.g_x + c.g_w > u->work.g_x + u->work.g_w) c.g_w = (short) (u->work.g_x + u->work.g_w - c.g_x);
	if (c.g_y + c.g_h > u->work.g_y + u->work.g_h) c.g_h = (short) (u->work.g_y + u->work.g_h - c.g_y);
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
	draw_tabs(u, vh);
	draw_band(u, vh);
	draw_pane(u, vh);
	draw_status(u, vh);
	ui_font(vh, F_BODY);
	u->clip = u->work;
}

void webui_draw(WEBUI *u, short vh)
{
	webui_draw_clip(u, vh, &u->work);
}
