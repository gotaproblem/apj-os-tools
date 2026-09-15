/*
 * pdfui.c - see pdfui.h. Every control is a rectangle out of the skin
 * sheet; the page is one blit out of the host-filled buffer; the 68k
 * touches no pixels but the magnifier glyph in the search field.
 *
 * Geometry is written in POINTS and put through apj_skin_m(), so one
 * source serves 1x, 1.25x and 1.75x. Every strip that holds text is sized
 * from the cell the VDI really gives, with the sheet's metric as a floor.
 */

#include <stdio.h>
#include <string.h>

#include "pdfui.h"

#define PAD		10	/* window inset, points              */
#define OLW		190	/* outline panel width               */
#define ROWH		22	/* outline row                       */
#define STATH		18	/* status strip                      */
#define BADGEH		16
#define SCROLLW		6
#define INNER		6	/* pane border to the page pixels    */
#define PANEMIN		120	/* the smallest page pane, points    */
#define SEARCHW		150
#define SEARCHMIN	60
#define OLHEAD		18

#define M(pt)		apj_skin_m(pt)

static void draw_plain(PDFUI *u, short vh);

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
static int vis(const PDFUI *u, short x, short y, short w, short h)
{
	return !(x >= u->clip.g_x + u->clip.g_w || x + w <= u->clip.g_x ||
	         y >= u->clip.g_y + u->clip.g_h || y + h <= u->clip.g_y);
}

/* a widget that does not fit inside the work area is left out of the
 * layout rather than drawn over the edge */
static void add(PDFUI *u, short id, short x, short y, short w, short h)
{
	if (u->nlay >= PDFUI_MAXLAY - 1)
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

static const APJ_LAY *find(PDFUI *u, short id)
{
	return apj_lay_find(u->lay, u->nlay, id);
}

/* a fill cut to the redraw clip: an erase must not reach past the part
 * being repainted, whatever the VDI clip happens to be */
static void fill_clip(PDFUI *u, short vh, short x, short y, short w, short h, short pen)
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
                    short *tw, short *th, short *stath, short *fieldw)
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
	/* the page field: four digits and room for the caret */
	*fieldw = (short) (*bw * 5 + M(10));
}

/* the toolbar's own width, so the window minimum can carry all of it */
static short toolbar_width(short vh)
{
	short sw, sh, bw, bh, tw, th, stath, fieldw;

	measure(vh, &sw, &sh, &bw, &bh, &tw, &th, &stath, &fieldw);
	ui_font(vh, F_BODY);
	return (short) (M(PAD) * 2 +
	                tw + M(10) +                        /* open           */
	                tw + M(2) + fieldw + M(6) + bw * 6 + M(6) + tw + M(10) +
	                tw + M(2) + (sw * 4 + M(12)) + M(2) + tw + M(6) +
	                tw + M(2) + tw + M(2) + tw + M(12) +
	                M(SEARCHMIN) + M(2) + tw + M(2) + tw + M(10) +
	                tw + M(2) + tw);
}

void pdfui_minsize(short vh, short *w, short *h)
{
	short sw, sh, bw, bh, tw, th, stath, fieldw;

	measure(vh, &sw, &sh, &bw, &bh, &tw, &th, &stath, &fieldw);
	*w = toolbar_width(vh);
	*h = (short) (M(PAD) * 2 + th + M(10) + M(PANEMIN) + stath);
}

void pdfui_layout(PDFUI *u, short vh, short wx, short wy, short ww, short wh)
{
	short pad = M(PAD), gap = M(2);
	short sw, sh, bw, bh, tw, th, stath, fieldw;
	short x, rx, y, by, bh_, olw, searchw, zoomw;

	measure(vh, &sw, &sh, &bw, &bh, &tw, &th, &stath, &fieldw);
	if (!apj_skin_ok())
		pad = 2;

	u->work.g_x = wx; u->work.g_y = wy;
	u->work.g_w = ww; u->work.g_h = wh;
	u->clip = u->work;
	u->nlay = 0;
	u->lay[0].id = -1;
	u->smallh = sh;

	/* ---- toolbar, left group ---------------------------------------- */
	y = (short) (wy + pad);
	x = (short) (wx + pad);
	add(u, W_OPEN, x, y, tw, th);    x = (short) (x + tw + M(10));
	add(u, W_PGPREV, x, y, tw, th);  x = (short) (x + tw + gap);
	add(u, W_PAGEFLD, x, y, fieldw, th);
	x = (short) (x + fieldw + M(6) + bw * 6 + M(6));	/* "/ 9999" text */
	add(u, W_PGNEXT, x, y, tw, th);  x = (short) (x + tw + M(10));
	add(u, W_ZOOMOUT, x, y, tw, th); x = (short) (x + tw + gap);
	zoomw = (short) (sw * 4 + M(12));
	add(u, W_ZOOMBADGE, x, (short) (y + (th - M(BADGEH)) / 2), zoomw, M(BADGEH));
	x = (short) (x + zoomw + gap);
	add(u, W_ZOOMIN, x, y, tw, th);  x = (short) (x + tw + M(6));
	add(u, W_FITW, x, y, tw, th);    x = (short) (x + tw + gap);
	add(u, W_FITP, x, y, tw, th);    x = (short) (x + tw + gap);
	add(u, W_ROTATE, x, y, tw, th);  x = (short) (x + tw + M(12));

	/* ---- toolbar, right group: from the right edge back ------------- */
	rx = (short) (wx + ww - pad - tw);
	add(u, W_ABOUT, rx, y, tw, th);
	rx = (short) (rx - gap - tw);
	add(u, W_OUTLINE, rx, y, tw, th);
	rx = (short) (rx - M(10) - tw);
	add(u, W_FINDNEXT, rx, y, tw, th);
	rx = (short) (rx - gap - tw);
	add(u, W_FINDPREV, rx, y, tw, th);
	searchw = M(SEARCHW);
	if (rx - gap - searchw < x)			/* narrow: shrink the field */
		searchw = (short) (rx - gap - x);
	if (searchw < M(SEARCHMIN))
		searchw = M(SEARCHMIN);
	rx = (short) (rx - gap - searchw);
	add(u, W_SEARCHFLD, rx, y, searchw, th);

	/* ---- body: outline panel and page pane -------------------------- */
	by = (short) (y + th + M(10));
	bh_ = (short) (wy + wh - by - pad - stath);
	if (bh_ < M(20))
		bh_ = M(20);

	u->rowh = (short) (sh + M(6));
	if (u->rowh < M(ROWH))
		u->rowh = M(ROWH);

	olw = 0;
	if (u->ol_show && u->noutline > 0)
	{
		olw = M(OLW);
		if (ww - 2 * pad - olw - M(8) < M(PANEMIN))
			olw = 0;			/* no room: the page wins */
	}
	if (olw)
	{
		/* the scrollbar sits INSIDE the group, and apj_lay_hit() answers
		 * with the first match - so it goes in first */
		add(u, W_OLSCROLL, (short) (wx + pad + olw - M(4) - M(SCROLLW)),
		    (short) (by + M(OLHEAD) + M(4)), M(SCROLLW),
		    (short) (bh_ - M(OLHEAD) - M(8)));
		add(u, W_OLIST, (short) (wx + pad), by, olw, bh_);
		u->visrows = (short) ((bh_ - M(OLHEAD) - M(8)) / u->rowh);
		if (u->visrows < 0)
			u->visrows = 0;
	}
	else
		u->visrows = 0;

	x = (short) (wx + pad + (olw ? olw + M(8) : 0));
	add(u, W_PGSCROLL, (short) (wx + ww - pad - M(4) - M(SCROLLW)),
	    (short) (by + M(INNER)), M(SCROLLW), (short) (bh_ - 2 * M(INNER)));
	add(u, W_PAGE, x, by, (short) (wx + ww - pad - x), bh_);

	/* the viewport: inside the pane's border, left of the scrollbar */
	u->view.g_x = (short) (x + M(INNER));
	u->view.g_y = (short) (by + M(INNER));
	u->view.g_w = (short) (wx + ww - pad - x - 2 * M(INNER) - M(SCROLLW) - M(4));
	u->view.g_h = (short) (bh_ - 2 * M(INNER));
	if (u->view.g_w < 16)
		u->view.g_w = 16;
	if (u->view.g_h < 1)
		u->view.g_h = 1;
	if (!find(u, W_PAGE))
		u->view.g_w = u->view.g_h = 0;
}

void pdfui_bbox(PDFUI *u, GRECT *r)
{
	short i, x0 = 0x7fff, y0 = 0x7fff, x1 = -1, y1 = -1;

	for (i = 0; i < u->nlay && u->lay[i].id >= 0; i++)
	{
		if (u->lay[i].id >= W_OLSCROLL)
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

static void lay_rect(PDFUI *u, short id, GRECT *r)
{
	const APJ_LAY *l = find(u, id);

	if (!l)
	{
		r->g_x = r->g_y = r->g_w = r->g_h = 0;
		return;
	}
	r->g_x = l->x; r->g_y = l->y; r->g_w = l->w; r->g_h = l->h;
}

void pdfui_band_rect(PDFUI *u, GRECT *r)
{
	GRECT b;

	pdfui_bbox(u, &b);
	r->g_x = (short) (u->work.g_x + M(PAD));
	r->g_y = b.g_y;
	r->g_w = (short) (u->work.g_w - 2 * M(PAD));
	r->g_h = b.g_h;
}

void pdfui_outline_rect(PDFUI *u, GRECT *r) { lay_rect(u, W_OLIST, r); }
void pdfui_pane_rect(PDFUI *u, GRECT *r)    { lay_rect(u, W_PAGE, r); }
void pdfui_view_rect(PDFUI *u, GRECT *r)    { *r = u->view; }

void pdfui_status_rect(PDFUI *u, GRECT *r)
{
	short sh = u->smallh, stath = (short) (sh + M(6));

	if (stath < M(STATH))
		stath = M(STATH);
	r->g_x = u->work.g_x;
	r->g_y = (short) (u->work.g_y + u->work.g_h - stath);
	r->g_w = u->work.g_w;
	r->g_h = stath;
}

/* ---------------------------------------------------------------- scroll */

short pdfui_ol_scroll_needed(PDFUI *u)
{
	return (find(u, W_OLSCROLL) && u->noutline > u->visrows && u->visrows > 0) ? 1 : 0;
}

void pdfui_ol_thumb_rect(PDFUI *u, GRECT *r)
{
	const APJ_LAY *l = find(u, W_OLSCROLL);
	long span = u->noutline - u->visrows;
	short th, ty;

	if (!l || !pdfui_ol_scroll_needed(u))
	{
		r->g_x = r->g_y = r->g_w = r->g_h = 0;
		return;
	}
	th = (short) ((long) l->h * u->visrows / u->noutline);
	if (th < M(SCROLLW) * 2)
		th = (short) (M(SCROLLW) * 2);
	if (th > l->h)
		th = l->h;
	ty = (short) (l->y + (long) (l->h - th) * u->ol_top / span);
	r->g_x = l->x;
	r->g_y = ty;
	r->g_w = l->w;
	r->g_h = th;
}

short pdfui_ol_scroll_part(PDFUI *u, short my)
{
	GRECT t;

	pdfui_ol_thumb_rect(u, &t);
	if (my < t.g_y)
		return -1;
	if (my >= t.g_y + t.g_h)
		return 1;
	return 0;
}

long pdfui_ol_top_for(PDFUI *u, short my, short grab)
{
	const APJ_LAY *l = find(u, W_OLSCROLL);
	GRECT t;
	long span = u->noutline - u->visrows, room, top;

	if (!l || span <= 0)
		return 0;
	pdfui_ol_thumb_rect(u, &t);
	room = (long) l->h - t.g_h;
	if (room <= 0)
		return 0;
	top = ((long) (my - grab - l->y) * span + room / 2) / room;
	if (top < 0)    top = 0;
	if (top > span) top = span;
	return top;
}

short pdfui_pg_scroll_needed(PDFUI *u)
{
	return (find(u, W_PGSCROLL) && u->pages > 0 && u->page_h > u->view.g_h) ? 1 : 0;
}

void pdfui_pg_thumb_rect(PDFUI *u, GRECT *r)
{
	const APJ_LAY *l = find(u, W_PGSCROLL);
	long span, th, ty;

	if (!l || !pdfui_pg_scroll_needed(u))
	{
		r->g_x = r->g_y = r->g_w = r->g_h = 0;
		return;
	}
	span = u->page_h - u->view.g_h;
	th = (long) l->h * u->view.g_h / u->page_h;
	if (th < M(SCROLLW) * 3)
		th = M(SCROLLW) * 3;
	if (th > l->h)
		th = l->h;
	ty = l->y + ((long) l->h - th) * (u->scroll_y < 0 ? 0 : u->scroll_y) / span;
	r->g_x = l->x;
	r->g_y = (short) ty;
	r->g_w = l->w;
	r->g_h = (short) th;
}

short pdfui_pg_scroll_part(PDFUI *u, short my)
{
	GRECT t;

	pdfui_pg_thumb_rect(u, &t);
	if (my < t.g_y)
		return -1;
	if (my >= t.g_y + t.g_h)
		return 1;
	return 0;
}

long pdfui_pg_top_for(PDFUI *u, short my, short grab)
{
	const APJ_LAY *l = find(u, W_PGSCROLL);
	GRECT t;
	long span, room, top;

	if (!l || !pdfui_pg_scroll_needed(u))
		return 0;
	span = u->page_h - u->view.g_h;
	pdfui_pg_thumb_rect(u, &t);
	room = (long) l->h - t.g_h;
	if (room <= 0)
		return 0;
	top = ((long) (my - grab - l->y) * span + room / 2) / room;
	if (top < 0)    top = 0;
	if (top > span) top = span;
	return top;
}

long pdfui_fit_zoom(PDFUI *u, long w100, long h100, short fit)
{
	long zw, zh, z;

	if (w100 <= 0 || h100 <= 0 || u->view.g_w <= 0 || u->view.g_h <= 0)
		return 1000;
	zw = (long) u->view.g_w * 1000L / w100;
	zh = (long) u->view.g_h * 1000L / h100;
	z = (fit == PDF_FIT_PAGE) ? (zw < zh ? zw : zh) : zw;
	if (z < 100)  z = 100;
	if (z > 4000) z = 4000;
	return z;
}

short pdfui_hit(PDFUI *u, short mx, short my)
{
	return apj_lay_hit(u->lay, u->nlay, mx, my);
}

long pdfui_ol_row_at(PDFUI *u, short my)
{
	const APJ_LAY *l = find(u, W_OLIST);
	long r;

	if (!l || u->visrows <= 0)
		return -1;
	r = (my - (l->y + M(OLHEAD) + M(4))) / u->rowh;
	if (r < 0 || r >= u->visrows || u->ol_top + r >= u->noutline)
		return -1;
	return u->ol_top + r;
}

/* ------------------------------------------------------------------ draw */

static short state_of(PDFUI *u, short id, short on)
{
	if (u->press == id)
		return APJ_ST_PRESS;
	if (on)
		return APJ_ST_ON;
	if (u->hover == id)
		return APJ_ST_HOVER;
	return APJ_ST_NORM;
}

/* a toolbar tile; on a sheet without TILE2 the glyph gets a plate and a
 * one-character label instead, so an old skin set still has a toolbar */
static void tile_at(PDFUI *u, short vh, short id, short glyph, short on,
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

static void field_at(PDFUI *u, short vh, short id, short focused,
                     const char *text, short right, short caret)
{
	const APJ_LAY *l = find(u, id);
	short cw, ch, tx, ty, avail;
	char fit[80];

	if (!l || !vis(u, l->x, l->y, l->w, l->h))
		return;
	apj_skin_9(vh, APJ_RG_FIELD, focused ? APJ_FLD_FOCUS : APJ_FLD_NORM,
	           l->x, l->y, l->w, l->h);
	ui_font(vh, F_BODY);
	cw = cellw(vh);
	ch = cellh(vh);
	avail = (short) (l->w - M(14) - (caret ? cw : 0));
	fit_text(vh, fit, (short) sizeof(fit), text, avail);
	if (right)
		tx = (short) (l->x + l->w - M(7) - cw * (short) strlen(fit) - (caret ? cw / 2 : 0));
	else
		tx = (short) (l->x + M(7));
	ty = (short) (l->y + (l->h - ch) / 2);
	apj_skin_text(vh, tx, ty, apj_skin_pen(APJ_R_TEXT), fit);
	if (caret)
		apj_fill(vh, (short) (tx + cw * (short) strlen(fit) + 1), (short) (l->y + M(5)),
		         (short) (M(1) > 1 ? M(1) : 1), (short) (l->h - M(10)),
		         apj_skin_pen(APJ_R_ACCENT));
}

static void draw_band(PDFUI *u, short vh)
{
	GRECT b;
	const APJ_LAY *l;
	char buf[32];

	pdfui_band_rect(u, &b);
	if (!vis(u, b.g_x, b.g_y, b.g_w, b.g_h))
		return;
	/* erase the strip: the strings are drawn transparently */
	fill_clip(u, vh, b.g_x, b.g_y, b.g_w, b.g_h, apj_skin_pen(APJ_R_PANEL));

	tile_at(u, vh, W_OPEN,    APJ_G_OPEN,     0, "Open");
	tile_at(u, vh, W_PGPREV,  APJ_G_PGPREV,   0, "<");
	tile_at(u, vh, W_PGNEXT,  APJ_G_PGNEXT,   0, ">");
	tile_at(u, vh, W_ZOOMOUT, APJ_G_ZOOMOUT,  0, "-");
	tile_at(u, vh, W_ZOOMIN,  APJ_G_ZOOMIN,   0, "+");
	tile_at(u, vh, W_FITW,    APJ_G_FITW,     u->fit == PDF_FIT_WIDTH, "W");
	tile_at(u, vh, W_FITP,    APJ_G_FITP,     u->fit == PDF_FIT_PAGE, "P");
	tile_at(u, vh, W_ROTATE,  APJ_G_ROTATE,   0, "R");
	tile_at(u, vh, W_FINDPREV, APJ_G_FINDPREV, 0, "^");
	tile_at(u, vh, W_FINDNEXT, APJ_G_FINDNEXT, 0, "v");
	tile_at(u, vh, W_OUTLINE, APJ_G_LIST,     u->ol_show, "=");
	tile_at(u, vh, W_ABOUT,    APJ_G_INFO,     0, "i");

	/* page field, and "/ N" after it */
	field_at(u, vh, W_PAGEFLD, u->focus == PDF_FOCUS_PAGE, u->pagetext, 1,
	         u->focus == PDF_FOCUS_PAGE);
	l = find(u, W_PAGEFLD);
	if (l && vis(u, (short) (l->x + l->w), l->y, M(80), l->h))
	{
		ui_font(vh, F_BODY);
		if (u->pages > 0)
			sprintf(buf, "/ %ld", u->pages);
		else
			strcpy(buf, "/ -");
		apj_skin_text(vh, (short) (l->x + l->w + M(6)),
		              (short) (l->y + (l->h - cellh(vh)) / 2),
		              apj_skin_pen(APJ_X_MUTED), buf);
	}

	/* zoom badge */
	l = find(u, W_ZOOMBADGE);
	if (l && vis(u, l->x, l->y, l->w, l->h))
	{
		ui_font(vh, F_SMALL);
		apj_skin_9(vh, APJ_RG_BADGE, APJ_BG_PLAIN, l->x, l->y, l->w, l->h);
		sprintf(buf, "%ld%%", u->zoom / 10);
		apj_skin_text(vh, (short) (l->x + (l->w - cellw(vh) * (short) strlen(buf)) / 2),
		              (short) (l->y + (l->h - cellh(vh)) / 2),
		              apj_skin_pen(APJ_X_MUTED), buf);
	}

	/* search field: magnifier, text, caret */
	l = find(u, W_SEARCHFLD);
	if (l && vis(u, l->x, l->y, l->w, l->h))
	{
		short g = apj_skin_glyphsz(), cw, ch, tx, ty;
		char fit[80];

		apj_skin_9(vh, APJ_RG_FIELD,
		           u->focus == PDF_FOCUS_SEARCH ? APJ_FLD_FOCUS : APJ_FLD_NORM,
		           l->x, l->y, l->w, l->h);
		if (apj_skin_ok())
			apj_skin_glyph(vh, APJ_G_SEARCH, apj_skin_pen(APJ_X_MUTED),
			               (short) (l->x + M(6)), (short) (l->y + (l->h - g) / 2));
		else
			g = 0;
		ui_font(vh, F_BODY);
		cw = cellw(vh);
		ch = cellh(vh);
		tx = (short) (l->x + M(6) + g + M(6));
		ty = (short) (l->y + (l->h - ch) / 2);
		fit_text(vh, fit, (short) sizeof(fit), u->searchtext,
		         (short) (l->x + l->w - M(8) - cw - tx));
		if (fit[0])
			apj_skin_text(vh, tx, ty, apj_skin_pen(APJ_R_TEXT), fit);
		else if (u->focus != PDF_FOCUS_SEARCH)
			apj_skin_text(vh, tx, ty, apj_skin_pen(APJ_X_MUTED), "find");
		if (u->focus == PDF_FOCUS_SEARCH)
			apj_fill(vh, (short) (tx + cw * (short) strlen(fit) + 1),
			         (short) (l->y + M(5)), (short) (M(1) > 1 ? M(1) : 1),
			         (short) (l->h - M(10)), apj_skin_pen(APJ_R_ACCENT));
	}
}

static void draw_outline(PDFUI *u, short vh)
{
	const APJ_LAY *l = find(u, W_OLIST);
	short r, iy, ch, cw, avail;
	char buf[24], fit[96];

	if (!l || !vis(u, l->x, l->y, l->w, l->h))
		return;
	apj_skin_9(vh, APJ_RG_GROUP, 0, l->x, l->y, l->w, l->h);

	ui_font(vh, F_SMALL);
	ch = cellh(vh);
	cw = cellw(vh);
	sprintf(buf, "OUTLINE  %ld", u->noutline);
	if (vis(u, l->x, l->y, l->w, M(OLHEAD)))
		apj_skin_text(vh, (short) (l->x + M(8)), (short) (l->y + (M(OLHEAD) - ch) / 2 + M(2)),
		              apj_skin_pen(APJ_X_MUTED), buf);

	iy = (short) (l->y + M(OLHEAD) + M(4));
	for (r = 0; r < u->visrows; r++)
	{
		long i = u->ol_top + r;
		short depth = 0, sel, tx;
		long page = 0;
		const char *title;

		if (i >= u->noutline)
			break;
		if (!vis(u, l->x, iy, l->w, u->rowh))
		{
			iy = (short) (iy + u->rowh);
			continue;
		}
		title = u->outline_of ? u->outline_of(u->ctx, i, &depth, &page) : "";
		if (!title)
			title = "";
		if (depth > 6)
			depth = 6;
		sel = (i == u->ol_sel);
		if (sel)
			apj_skin_9(vh, APJ_RG_ROWSEL, 0, (short) (l->x + M(2)), iy,
			           (short) (l->w - M(4) - M(SCROLLW) - M(8)), u->rowh);

		tx = (short) (l->x + M(10) + depth * M(12));
		sprintf(buf, "%ld", page);
		avail = (short) (l->x + l->w - M(14) - M(SCROLLW) - cw * (short) (strlen(buf) + 1) - tx);
		fit_text(vh, fit, (short) sizeof(fit), title, avail);
		apj_skin_text(vh, tx, (short) (iy + (u->rowh - ch) / 2),
		              apj_skin_pen(sel ? APJ_R_SELFG : (depth ? APJ_X_MUTED : APJ_R_TEXT)), fit);
		if (page > 0)
			apj_skin_text(vh, (short) (l->x + l->w - M(14) - M(SCROLLW) - cw * (short) strlen(buf)),
			              (short) (iy + (u->rowh - ch) / 2),
			              apj_skin_pen(sel ? APJ_R_SELFG : APJ_X_MUTED), buf);
		iy = (short) (iy + u->rowh);
	}

	if (pdfui_ol_scroll_needed(u))
	{
		const APJ_LAY *sb = find(u, W_OLSCROLL);
		GRECT t;

		if (sb && vis(u, sb->x, sb->y, sb->w, sb->h))
		{
			apj_skin_9(vh, APJ_RG_VSCROLL, APJ_VS_TROUGH, sb->x, sb->y, sb->w, sb->h);
			pdfui_ol_thumb_rect(u, &t);
			apj_skin_9(vh, APJ_RG_VSCROLL,
			           u->dragging == 1 ? APJ_VS_HELD : APJ_VS_THUMB,
			           t.g_x, t.g_y, t.g_w, t.g_h);
		}
	}
}

/* one line of text centred in the pane, in the small font */
static void pane_line(PDFUI *u, short vh, const GRECT *v, short y, short pen,
                      const char *s)
{
	char fit[160];
	short w;

	fit_text(vh, fit, (short) sizeof(fit), s, (short) (v->g_w - M(16)));
	w = (short) (cellw(vh) * (short) strlen(fit));
	apj_skin_text(vh, (short) (v->g_x + (v->g_w - w) / 2), y, apj_skin_pen(pen), fit);
	(void) u;
}

/* the canvas the page sits on: the darker of the pane's fill and PANEL,
 * so a white page stands off it under both light and dark skins */
static short canvas_pen(void)
{
	long p = apj_skin_rgb(APJ_R_PANEL), g = apj_skin_rgb(APJ_R_PAPER);
	long lp, lg;

	if (p < 0 || g < 0)
		return apj_skin_pen(APJ_R_PAPER);
	lp = ((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255);
	lg = ((g >> 16) & 255) + ((g >> 8) & 255) + (g & 255);
	return apj_skin_pen(lg < lp ? APJ_R_PAPER : APJ_R_PANEL);
}

static void draw_pane(PDFUI *u, short vh)
{
	const APJ_LAY *l = find(u, W_PAGE);
	GRECT v = u->view;
	short g = apj_skin_glyphsz(), y, ch;

	if (!l || !vis(u, l->x, l->y, l->w, l->h))
		return;
	apj_skin_9(vh, APJ_RG_GROUP, 0, l->x, l->y, l->w, l->h);

	if (v.g_w <= 0 || v.g_h <= 0)
		return;

	if (u->msg == PDF_MSG_NONE && u->pagebuf.fd_addr && u->pages > 0)
	{
		if (vis(u, v.g_x, v.g_y, v.g_w, v.g_h))
		{
			MFDB scr;
			short pxy[8];
			long px0 = (long) v.g_x - u->scroll_x, py0 = (long) v.g_y - u->scroll_y;

			scr.fd_addr = NULL;
			pxy[0] = 0;              pxy[1] = 0;
			pxy[2] = (short) (v.g_w - 1); pxy[3] = (short) (v.g_h - 1);
			pxy[4] = v.g_x;          pxy[5] = v.g_y;
			pxy[6] = (short) (v.g_x + v.g_w - 1); pxy[7] = (short) (v.g_y + v.g_h - 1);
			vro_cpyfm(vh, S_ONLY, pxy, &u->pagebuf, &scr);

			/* a 2 px shadow right of and under a page that does not fill
			 * the viewport: two bars, clipped to the viewport by hand */
			if (u->page_w < v.g_w && px0 + u->page_w + M(2) <= v.g_x + v.g_w)
			{
				long sy = py0 + M(2), sh = u->page_h;

				if (sy < v.g_y) { sh -= v.g_y - sy; sy = v.g_y; }
				if (sy + sh > v.g_y + v.g_h) sh = v.g_y + v.g_h - sy;
				if (sh > 0)
					apj_fill(vh, (short) (px0 + u->page_w), (short) sy, M(2), (short) sh,
					         apj_skin_pen(APJ_R_ELEVATION));
			}
			if (u->page_h < v.g_h && py0 + u->page_h + M(2) <= v.g_y + v.g_h)
			{
				long sx = px0 + M(2), sw = u->page_w;

				if (sx < v.g_x) { sw -= v.g_x - sx; sx = v.g_x; }
				if (sx + sw > v.g_x + v.g_w) sw = v.g_x + v.g_w - sx;
				if (sw > 0)
					apj_fill(vh, (short) sx, (short) (py0 + u->page_h), (short) sw, M(2),
					         apj_skin_pen(APJ_R_ELEVATION));
			}
		}
	}
	else
	{
		apj_fill(vh, v.g_x, v.g_y, v.g_w, v.g_h, canvas_pen());
		ui_font(vh, F_SMALL);
		ch = cellh(vh);
		y = (short) (v.g_y + (v.g_h - (g + M(8) + ch)) / 2);
		if (y < v.g_y + M(4))
			y = (short) (v.g_y + M(4));
		switch (u->msg)
		{
			case PDF_MSG_IDLE:
				if (apj_skin_ok())
					apj_skin_glyph(vh, APJ_G_INFO, apj_skin_pen(APJ_X_MUTED),
					               (short) (v.g_x + (v.g_w - g) / 2), y);
				pane_line(u, vh, &v, (short) (y + g + M(8)), APJ_X_MUTED,
				          "Open a PDF from a HOSTFS drive...");
				break;
			case PDF_MSG_NOTT:
				pane_line(u, vh, &v, (short) (y + g + M(8)), APJ_X_MUTED,
				          "No TT-RAM for the page buffer");
				break;
			case PDF_MSG_FAILED:
				pane_line(u, vh, &v, (short) (y + g + M(8)), APJ_X_MUTED,
				          "The host could not draw this page");
				break;
			default:
				break;
		}
	}

	if (pdfui_pg_scroll_needed(u))
	{
		const APJ_LAY *sb = find(u, W_PGSCROLL);
		GRECT t;

		if (sb && vis(u, sb->x, sb->y, sb->w, sb->h))
		{
			apj_skin_9(vh, APJ_RG_VSCROLL, APJ_VS_TROUGH, sb->x, sb->y, sb->w, sb->h);
			pdfui_pg_thumb_rect(u, &t);
			apj_skin_9(vh, APJ_RG_VSCROLL,
			           u->dragging == 2 ? APJ_VS_HELD : APJ_VS_THUMB,
			           t.g_x, t.g_y, t.g_w, t.g_h);
		}
	}
}

/* a badge growing leftwards from *rx; cut to what fits left of it down to
 * left, or not drawn at all - a tiny window must lose the badge, never
 * paint outside the strip */
static void badge_right(short vh, short *rx, short left, short by, short bh,
                        short state, short pen, const char *s)
{
	short cw = cellw(vh), ch = cellh(vh), w;
	char fit[96];

	fit_text(vh, fit, (short) sizeof(fit), s, (short) (*rx - left - M(12)));
	if (!fit[0])
		return;
	w = (short) (cw * (short) strlen(fit) + M(12));
	*rx = (short) (*rx - w);
	apj_skin_9(vh, APJ_RG_BADGE, state, *rx, by, w, bh);
	apj_skin_text(vh, (short) (*rx + M(6)), (short) (by + (bh - ch) / 2),
	              apj_skin_pen(pen), fit);
	*rx = (short) (*rx - M(6));
}

static void draw_status(PDFUI *u, short vh)
{
	GRECT s;
	short ch, rx, by, bh, avail;
	char fit[160];

	pdfui_status_rect(u, &s);
	if (!vis(u, s.g_x, s.g_y, s.g_w, s.g_h))
		return;
	/* the strip is flat PANEL by the sheet's own recipe (mkskin's STATUS
	 * fill), so one bar does what 35 tile blits would - this is repainted
	 * on every search step, and quiet matters more than the region */
	fill_clip(u, vh, s.g_x, s.g_y, s.g_w, s.g_h, apj_skin_pen(APJ_R_PANEL));
	fill_clip(u, vh, s.g_x, s.g_y, s.g_w, 1, apj_skin_pen(APJ_R_BORDER));

	ui_font(vh, F_SMALL);
	ch = cellh(vh);
	bh = M(BADGEH);
	by = (short) (s.g_y + (s.g_h - bh) / 2);
	rx = (short) (s.g_x + s.g_w - M(PAD));
	if (u->busy && u->busy[0])
		badge_right(vh, &rx, (short) (s.g_x + M(PAD)), by, bh,
		            APJ_BG_PLAIN, APJ_X_MUTED, u->busy);
	if (u->hits && u->hits[0])
		badge_right(vh, &rx, (short) (s.g_x + M(PAD)), by, bh,
		            APJ_BG_ACCENT, APJ_X_ACCENT_INK, u->hits);

	avail = (short) (rx - s.g_x - M(PAD) - M(6));
	if (avail > 0 && u->status && u->status[0])
	{
		fit_text(vh, fit, (short) sizeof(fit), u->status, avail);
		apj_skin_text(vh, (short) (s.g_x + M(PAD)), (short) (s.g_y + (s.g_h - ch) / 2),
		              apj_skin_pen(APJ_X_MUTED), fit);
	}
}

void pdfui_draw_band(PDFUI *u, short vh)    { draw_band(u, vh); }
void pdfui_draw_outline(PDFUI *u, short vh) { draw_outline(u, vh); }
void pdfui_draw_pane(PDFUI *u, short vh)    { draw_pane(u, vh); }
void pdfui_draw_status(PDFUI *u, short vh)  { draw_status(u, vh); }

/* no skin: the flat apjgui controls, so the viewer still runs */
static void draw_plain(PDFUI *u, short vh)
{
	static const short wid[] = { W_OPEN, W_PGPREV, W_PGNEXT, W_ZOOMOUT, W_ZOOMIN,
	                             W_FITW, W_FITP, W_ROTATE, W_FINDPREV, W_FINDNEXT,
	                             W_OUTLINE, W_ABOUT };
	static const char *lbl[] = { "Open", "<", ">", "-", "+", "W", "P", "R",
	                             "^", "v", "=", "i" };
	short i, ch;
	char buf[96];
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
	for (i = 0; i < 12; i++)
	{
		l = find(u, wid[i]);
		if (l)
			apj_button(vh, l->x, l->y, l->w, l->h, lbl[i], u->press == wid[i], 0);
	}
	l = find(u, W_PAGEFLD);
	if (l)
	{
		apj_fill(vh, l->x, l->y, l->w, l->h, apj_pen(APJ_R_PAPER));
		apj_text(vh, (short) (l->x + 2), (short) (l->y + (l->h - ch) / 2), apj_pen(APJ_R_TEXT), u->pagetext);
		sprintf(buf, "/ %ld  %ld%%", u->pages, u->zoom / 10);
		apj_text(vh, (short) (l->x + l->w + 4), (short) (l->y + (l->h - ch) / 2), apj_pen(APJ_R_TEXT), buf);
	}
	l = find(u, W_SEARCHFLD);
	if (l)
	{
		apj_fill(vh, l->x, l->y, l->w, l->h, apj_pen(APJ_R_PAPER));
		apj_text(vh, (short) (l->x + 2), (short) (l->y + (l->h - ch) / 2), apj_pen(APJ_R_TEXT), u->searchtext);
	}
	l = find(u, W_OLIST);
	if (l)
	{
		short y = (short) (l->y + 2);
		long k;

		apj_fill(vh, l->x, l->y, l->w, l->h, apj_pen(APJ_R_PAPER));
		for (k = u->ol_top; k < u->noutline && y + ch <= l->y + l->h; k++)
		{
			short depth = 0;
			long page = 0;
			const char *t = u->outline_of ? u->outline_of(u->ctx, k, &depth, &page) : "";

			if (k == u->ol_sel)
				apj_select(vh, l->x, y, l->w, ch);
			apj_text(vh, (short) (l->x + 2 + depth * 8), y,
			         apj_pen(k == u->ol_sel ? APJ_R_SELFG : APJ_R_TEXT), t ? t : "");
			y = (short) (y + ch);
		}
	}
	l = find(u, W_PAGE);
	if (l)
	{
		if (u->msg == PDF_MSG_NONE && u->pagebuf.fd_addr && u->pages > 0 &&
		    u->view.g_w > 0 && u->view.g_h > 0)
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
	if (u->status)
		apj_text(vh, (short) (u->work.g_x + 2), (short) (u->work.g_y + u->work.g_h - ch),
		         apj_pen(APJ_R_TEXT), u->status);
}

/*
 * A redraw of the parts that meet clip - what a WM_REDRAW wants. A window
 * dragged across this one exposes it a thin strip at a time; drawing
 * everything for each strip is what made that drag stutter over MP3GEM.
 */
void pdfui_draw_clip(PDFUI *u, short vh, const GRECT *clip)
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
	apj_fill(vh, c.g_x, c.g_y, c.g_w, c.g_h, apj_skin_pen(APJ_R_PANEL));
	{
		short bw_, bh_;

		apj_skin_size(APJ_RG_PANELTOP, &bw_, &bh_);
		if (vis(u, u->work.g_x, u->work.g_y, u->work.g_w, bh_))
			apj_skin_tilex(vh, APJ_RG_PANELTOP, 0, u->work.g_x, u->work.g_y,
			               u->work.g_w);
	}
	draw_band(u, vh);
	if (find(u, W_OLIST))
		draw_outline(u, vh);
	draw_pane(u, vh);
	draw_status(u, vh);
	ui_font(vh, F_BODY);
	u->clip = u->work;
}

void pdfui_draw(PDFUI *u, short vh)
{
	pdfui_draw_clip(u, vh, &u->work);
}
