/*
 * vidui.c - see vidui.h. Every control is a rectangle out of the skin
 * sheet; the 68k touches no pixels but the glyph blend in the empty pane.
 *
 * Geometry is written in POINTS and put through apj_skin_m(), so one
 * source serves 1x, 1.25x and 1.75x. Every strip that holds text is sized
 * from the cell the VDI really gives, with the sheet's metric as a floor -
 * the fonts do not scale with the sheet (MP3GEM's footer sat on its
 * playlist at 100% until that was learned).
 */

#include <stdio.h>
#include <string.h>

#include "vidui.h"

#define PAD		10	/* window inset, points          */
#define SEEKH		6
#define KNOB		12
#define ROWH		24
#define STATH		14
#define BADGEH		16
#define SCROLLW		6
#define PANEMIN		90	/* the smallest picture box, points */

#define M(pt)		apj_skin_m(pt)

static void draw_plain(VIDUI *u, short vh);

static void hhmmss(char *out, long s)
{
	if (s < 0)
		s = 0;
	if (s >= 3600L)
		sprintf(out, "%ld:%02ld:%02ld", s / 3600L, (s / 60L) % 60L, s % 60L);
	else
		sprintf(out, "%ld:%02ld", s / 60L, s % 60L);
}

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

/* fit s into avail pixels of the current font, "..." if cut */
static void fit_text(short vh, char *out, short outsz, const char *s, short avail)
{
	short cw, max, len;
	short a[10];

	vqt_attributes(vh, a);
	cw = a[8] > 0 ? a[8] : 8;
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
static int vis(const VIDUI *u, short x, short y, short w, short h)
{
	return !(x >= u->clip.g_x + u->clip.g_w || x + w <= u->clip.g_x ||
	         y >= u->clip.g_y + u->clip.g_h || y + h <= u->clip.g_y);
}

static short cellw(short vh)
{
	short a[10];

	vqt_attributes(vh, a);
	return a[8];
}

static short cellh(short vh)
{
	short a[10];

	vqt_attributes(vh, a);
	return a[9];
}

/* a widget that does not fit inside the work area is left out of the
 * layout rather than drawn over the edge: every draw looks it up first,
 * so a window shrunk under the minimum loses controls, not pixels */
static void add(VIDUI *u, short id, short x, short y, short w, short h)
{
	if (u->nlay >= VIDUI_MAXLAY - 1)
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

/* ---------------------------------------------------------------- layout */

/* the measured heights the layout is built from */
static void measure(short vh, short *sw, short *sh, short *bh, short *th,
                    short *tlab, short *stath, short *infoh)
{
	ui_font(vh, F_SMALL);
	*sw = cellw(vh);
	*sh = cellh(vh);
	ui_font(vh, F_BODY);
	*bh = cellh(vh);
	ui_font(vh, F_TITLE);
	*th = cellh(vh);
	ui_font(vh, F_BODY);

	*tlab = (short) (*sw * 6 + M(6));		/* "-88:88" and a gap */
	if (*tlab < M(46))
		*tlab = M(46);
	*stath = (short) (*sh + M(4));
	if (*stath < M(STATH))
		*stath = M(STATH);
	/* title, sub line, a row of badges */
	*infoh = (short) (*th + M(4) + *bh + M(6) + M(BADGEH) + M(8));
	if (*infoh < M(72))
		*infoh = M(72);
}

void vidui_minsize(short vh, short *w, short *h)
{
	short sw, sh, bh, th, tlab, stath, infoh;
	short tw = apj_skin_tilew(), acc = apj_skin_accd();

	if (!apj_skin_ok())
	{
		*w = (short) (cellw(vh) * 60);
		*h = (short) (cellh(vh) * 14);
		return;
	}
	measure(vh, &sw, &sh, &bh, &th, &tlab, &stath, &infoh);
	/* left transport group + a gap + the right group */
	*w = (short) (M(PAD) * 2 + tw * 5 + acc + M(2) * 5 + M(24) +
	              tw * 4 + M(72) + M(2) * 3 + M(8) + M(4));
	*h = (short) (M(PAD) * 2 + infoh + M(SEEKH) + M(6) + M(22) +
	              apj_skin_tileh() + M(10) + M(PANEMIN) + stath);
}

void vidui_layout(VIDUI *u, short vh, short wx, short wy, short ww, short wh)
{
	short pad = M(PAD);
	short tw = apj_skin_tilew(), th = apj_skin_tileh(), acc = apj_skin_accd();
	short x, y, sy, py, ph;
	short sw, sh, bh, tth, tlab, stath, infoh;

	if (!apj_skin_ok())			/* plain controls, character sized */
	{
		tw = (short) (cellw(vh) * 5);
		th = (short) (cellh(vh) + 4);
		acc = th;
		pad = 2;
	}
	measure(vh, &sw, &sh, &bh, &tth, &tlab, &stath, &infoh);

	u->work.g_x = wx; u->work.g_y = wy;
	u->work.g_w = ww; u->work.g_h = wh;
	u->clip = u->work;
	u->nlay = 0;
	u->lay[0].id = -1;
	u->infoh = infoh;
	u->smallh = sh;

	/* seek strip, under the info band */
	sy = (short) (wy + pad + infoh);
	add(u, W_SEEK, (short) (wx + pad + tlab), sy,
	    (short) (ww - 2 * pad - 2 * tlab), (short) (M(SEEKH) + M(6)));

	/* transport, left */
	y = (short) (sy + M(22));
	x = (short) (wx + pad);
	add(u, W_PREV, x, y, tw, th); x = (short) (x + tw + M(2));
	add(u, W_RW,   x, y, tw, th); x = (short) (x + tw + M(2));
	add(u, W_PLAY, x, (short) (y - (acc - th) / 2), acc, acc);
	x = (short) (x + acc + M(2));
	add(u, W_FF,   x, y, tw, th); x = (short) (x + tw + M(2));
	add(u, W_NEXT, x, y, tw, th); x = (short) (x + tw + M(2));
	add(u, W_STOP, x, y, tw, th);

	/* and right: open, fullscreen, list, volume, loop */
	x = (short) (wx + ww - pad - tw);
	add(u, W_OPEN, x, y, tw, th);
	x = (short) (x - tw - M(2));
	add(u, W_FULL, x, y, tw, th);
	x = (short) (x - tw - M(2));
	add(u, W_LISTTOG, x, y, tw, th);
	x = (short) (x - M(72) - M(8));
	add(u, W_VOL, x, (short) (y + th / 2 - M(3)), M(72), M(SEEKH) + M(6));
	x = (short) (x - tw - M(4));
	add(u, W_VOLICO, x, y, tw, th);
	x = (short) (x - tw - M(2));
	add(u, W_LOOP, x, y, tw, th);

	/* the pane: the picture, or the playlist, down to the status line */
	py = (short) (y + th + M(10));
	ph = (short) (wy + wh - py - pad - stath);
	u->rowh = apj_skin_ok() ? M(ROWH) : cellh(vh);
	if (u->rowh < bh + M(4))
		u->rowh = (short) (bh + M(4));
	if (ph < u->rowh + 2)
		ph = (short) (u->rowh + 2);
	if (u->listmode)
	{
		/* the scrollbar sits INSIDE the list, and apj_lay_hit() answers
		 * with the first match - so it goes in first */
		add(u, W_SCROLL, (short) (wx + ww - pad - M(4) - M(SCROLLW)),
		    (short) (py + M(4)), M(SCROLLW), (short) (ph - M(8)));
		add(u, W_LIST, (short) (wx + pad), py, (short) (ww - 2 * pad), ph);
	}
	add(u, W_VIDEO, (short) (wx + pad), py, (short) (ww - 2 * pad), ph);

	u->visrows = (short) ((ph - (apj_skin_ok() ? M(8) : 0)) / u->rowh);
	if (u->visrows < 0)
		u->visrows = 0;
}

void vidui_bbox(VIDUI *u, GRECT *r)
{
	short i, x0 = 0x7fff, y0 = 0x7fff, x1 = -1, y1 = -1;

	for (i = 0; i < u->nlay && u->lay[i].id >= 0; i++)
	{
		if (u->lay[i].id == W_LIST || u->lay[i].id == W_VIDEO ||
		    u->lay[i].id == W_SCROLL)
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

void vidui_pane_rect(VIDUI *u, GRECT *r)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, W_VIDEO);

	if (!l)
	{
		r->g_x = r->g_y = r->g_w = r->g_h = 0;
		return;
	}
	r->g_x = l->x; r->g_y = l->y; r->g_w = l->w; r->g_h = l->h;
}

/* --------------------------------------------------------------- scroll */

short vidui_scroll_needed(VIDUI *u)
{
	return (u->listmode && u->ntracks > u->visrows && u->visrows > 0) ? 1 : 0;
}

void vidui_thumb_rect(VIDUI *u, GRECT *r)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, W_SCROLL);
	long span = (long) u->ntracks - u->visrows;
	short th, ty;

	if (!l || !vidui_scroll_needed(u))
	{
		r->g_x = r->g_y = r->g_w = r->g_h = 0;
		return;
	}
	th = (short) ((long) l->h * u->visrows / u->ntracks);
	if (th < M(SCROLLW) * 2)
		th = (short) (M(SCROLLW) * 2);
	if (th > l->h)
		th = l->h;
	ty = (short) (l->y + (long) (l->h - th) * u->top / span);
	r->g_x = l->x;
	r->g_y = ty;
	r->g_w = l->w;
	r->g_h = th;
}

short vidui_scroll_part(VIDUI *u, short my)
{
	GRECT t;

	vidui_thumb_rect(u, &t);
	if (my < t.g_y)
		return -1;
	if (my >= t.g_y + t.g_h)
		return 1;
	return 0;
}

short vidui_scroll_top_for(VIDUI *u, short my, short grab)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, W_SCROLL);
	GRECT t;
	long span = (long) u->ntracks - u->visrows, room, top;

	if (!l || span <= 0)
		return 0;
	vidui_thumb_rect(u, &t);
	room = (long) l->h - t.g_h;
	if (room <= 0)
		return 0;
	top = ((long) (my - grab - l->y) * span + room / 2) / room;
	if (top < 0)    top = 0;
	if (top > span) top = span;
	return (short) top;
}

short vidui_hit(VIDUI *u, short mx, short my)
{
	return apj_lay_hit(u->lay, u->nlay, mx, my);
}

short vidui_row_at(VIDUI *u, short my)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, W_LIST);
	short r;

	if (!l || !u->listmode)
		return -1;
	r = (short) ((my - (l->y + M(4))) / u->rowh);
	if (r < 0 || r >= u->visrows || u->top + r >= u->ntracks)
		return -1;
	return (short) (u->top + r);
}

/* ------------------------------------------------------------------ draw */

static short state_of(VIDUI *u, short id, short on)
{
	if (u->press == id)
		return APJ_ST_PRESS;
	if (on)
		return APJ_ST_ON;
	if (u->hover == id)
		return APJ_ST_HOVER;
	return APJ_ST_NORM;
}

static void tilew_at(VIDUI *u, short vh, short id, short glyph, short on)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, id);

	if (l && vis(u, l->x, l->y, l->w, l->h))
		apj_skin_tile(vh, glyph, state_of(u, id, on), l->x, l->y);
}

static void badge(short vh, short *ix, short by, short state, short pen,
                  const char *s)
{
	short cw = cellw(vh), ch = cellh(vh);
	short w = (short) (cw * (short) strlen(s) + M(12));

	apj_skin_9(vh, APJ_RG_BADGE, state, *ix, by, w, M(BADGEH));
	apj_skin_text(vh, (short) (*ix + M(6)), (short) (by + (M(BADGEH) - ch) / 2),
	              apj_skin_pen(pen), s);
	*ix = (short) (*ix + w + M(6));
}

/* title, sub line, the badge row */
static void draw_info(VIDUI *u, short vh)
{
	short ix = (short) (u->work.g_x + M(PAD));
	short iy = (short) (u->work.g_y + M(PAD));
	short avail = (short) (u->work.g_w - 2 * M(PAD));
	short by;
	char fit[160], buf[40];

	if (!vis(u, u->work.g_x, iy, u->work.g_w, u->infoh))
		return;

	/* erase the band: the strings are drawn transparently */
	apj_fill(vh, ix, iy, avail, u->infoh, apj_skin_pen(APJ_R_PANEL));

	ui_font(vh, F_TITLE);
	fit_text(vh, fit, (short) sizeof(fit), u->title ? u->title : "", avail);
	apj_skin_text(vh, ix, iy, apj_skin_pen(APJ_R_TEXT), fit);
	by = (short) (iy + cellh(vh) + M(4));

	ui_font(vh, F_BODY);
	fit_text(vh, fit, (short) sizeof(fit), u->sub ? u->sub : "", avail);
	apj_skin_text(vh, ix, by, apj_skin_pen(APJ_X_MUTED), fit);
	by = (short) (by + cellh(vh) + M(6));

	ui_font(vh, F_SMALL);
	if (u->codec && u->codec[0])
		badge(vh, &ix, by, APJ_BG_PLAIN, APJ_X_MUTED, u->codec);
	if (u->vid_w > 0 && u->vid_h > 0)
	{
		sprintf(buf, "%ldx%ld", u->vid_w, u->vid_h);
		badge(vh, &ix, by, APJ_BG_PLAIN, APJ_X_MUTED, buf);
	}
	if (u->fps100 > 0)
	{
		if (u->fps100 % 100 == 0)
			sprintf(buf, "%ld fps", u->fps100 / 100);
		else
			sprintf(buf, "%ld.%02ld fps", u->fps100 / 100, u->fps100 % 100);
		badge(vh, &ix, by, APJ_BG_PLAIN, APJ_X_MUTED, buf);
	}
	{
		const char *st = !u->playing ? "STOPPED" : (u->paused ? "PAUSED" : "PLAYING");
		short on = (u->playing && !u->paused);

		badge(vh, &ix, by, on ? APJ_BG_ACCENT : APJ_BG_PLAIN,
		      on ? APJ_X_ACCENT_INK : APJ_X_MUTED, st);
	}
}

static void draw_seek(VIDUI *u, short vh)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, W_SEEK);
	short th = M(SEEKH), ty, fw, kn = M(KNOB);
	char a[24], b[32];

	if (!l || !vis(u, u->work.g_x, l->y, u->work.g_w, l->h))
		return;
	ty = (short) (l->y + (l->h - th) / 2);

	/* the whole strip first: the text is transparent and the knob taller
	 * than the track, so without an erase digits pile up and the knob
	 * leaves crumbs */
	apj_fill(vh, (short) (u->work.g_x + M(PAD)), l->y,
	         (short) (u->work.g_w - 2 * M(PAD)), l->h,
	         apj_skin_pen(APJ_R_PANEL));

	apj_skin_9(vh, APJ_RG_SEEK, APJ_SK_TRACK, l->x, ty, l->w, th);
	fw = 0;
	if (u->len_s > 0)
		fw = (short) ((long) l->w * u->pos_s / u->len_s);
	if (fw > 0)
		apj_skin_9(vh, APJ_RG_SEEK, APJ_SK_FILL, l->x, ty, fw, th);
	apj_skin_blit(vh, APJ_RG_KNOB, state_of(u, W_SEEK, 0),
	              (short) (l->x + fw - kn / 2), (short) (ty + th / 2 - kn / 2));

	ui_font(vh, F_SMALL);
	hhmmss(a, u->pos_s);
	{
		long rem = u->len_s - u->pos_s;

		if (rem < 0)
			rem = 0;
		if (rem >= 3600L)
			sprintf(b, "-%ld:%02ld:%02ld", rem / 3600L, (rem / 60L) % 60L, rem % 60L);
		else
			sprintf(b, "-%ld:%02ld", rem / 60L, rem % 60L);
	}
	apj_skin_text(vh, (short) (u->work.g_x + M(PAD)), (short) (ty - cellh(vh) / 3),
	              apj_skin_pen(APJ_X_MUTED), a);
	apj_skin_text(vh, (short) (l->x + l->w + M(6)), (short) (ty - cellh(vh) / 3),
	              apj_skin_pen(APJ_X_MUTED), b);
}

static void draw_transport(VIDUI *u, short vh)
{
	const APJ_LAY *l;
	short vg;

	tilew_at(u, vh, W_PREV, APJ_G_PREV, 0);
	tilew_at(u, vh, W_RW,   APJ_G_RW,   0);
	tilew_at(u, vh, W_FF,   APJ_G_FF,   0);
	tilew_at(u, vh, W_NEXT, APJ_G_NEXT, 0);
	tilew_at(u, vh, W_STOP, APJ_G_STOP, 0);
	tilew_at(u, vh, W_LOOP, APJ_G_REPEAT, u->loop);
	tilew_at(u, vh, W_LISTTOG, APJ_G_LIST, u->listmode);
	tilew_at(u, vh, W_FULL, u->fullscreen ? APJ_G_UNFULL : APJ_G_FULL, u->fullscreen);
	tilew_at(u, vh, W_OPEN, APJ_G_OPEN, 0);

	if (u->hasvol)
	{
		vg = u->vol == 0 ? APJ_G_MUTE : (u->vol < 50 ? APJ_G_VOLLOW : APJ_G_VOL);
		tilew_at(u, vh, W_VOLICO, vg, 0);
	}

	l = apj_lay_find(u->lay, u->nlay, W_PLAY);
	if (l && vis(u, l->x, l->y, l->w, l->h))
		apj_skin_tileacc(vh,
		    (u->playing && !u->paused) ? APJ_G_PAUSE : APJ_G_PLAY,
		    state_of(u, W_PLAY, 0), l->x, l->y);

	l = u->hasvol ? apj_lay_find(u->lay, u->nlay, W_VOL) : NULL;
	if (l && vis(u, l->x, l->y, l->w, l->h))
	{
		short th = M(SEEKH), ty = (short) (l->y + (l->h - th) / 2);

		apj_skin_9(vh, APJ_RG_SEEK, APJ_SK_TRACK, l->x, ty, l->w, th);
		if (u->vol > 0)
			apj_skin_9(vh, APJ_RG_SEEK, APJ_SK_FILL, l->x, ty,
			           (short) ((long) l->w * u->vol / 100L), th);
	}
}

static void draw_list(VIDUI *u, short vh)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, W_LIST);
	short r, iy, ch, cw, g = apj_skin_glyphsz();
	short numx, namex;
	char tbuf[16];

	ui_font(vh, F_BODY);
	ch = cellh(vh);
	cw = cellw(vh);
	if (!l || !vis(u, l->x, l->y, l->w, l->h))
		return;
	apj_skin_9(vh, APJ_RG_GROUP, 0, l->x, l->y, l->w, l->h);

	numx = M(12);
	namex = (short) (numx + cw * 3);
	if (namex < M(34))
		namex = M(34);

	iy = (short) (l->y + M(4));
	for (r = 0; r < u->visrows; r++)
	{
		short i = (short) (u->top + r);
		short sel = (i == u->sel);
		const char *nm;

		if (i >= u->ntracks)
			break;
		if (!vis(u, l->x, iy, l->w, u->rowh))
		{
			iy = (short) (iy + u->rowh);
			continue;
		}
		nm = u->name_of ? u->name_of(u->ctx, i) : "";

		if (sel)
		{
			apj_skin_9(vh, APJ_RG_ROWSEL, 0, (short) (l->x + M(2)), iy,
			           (short) (l->w - M(4) -
			                    (vidui_scroll_needed(u) ? M(SCROLLW) + M(8) : 0)),
			           u->rowh);
			if (u->playing)
				apj_skin_glyph(vh, APJ_G_PLAY, apj_skin_pen(APJ_R_ACCENT),
				               (short) (l->x + M(10)),
				               (short) (iy + (u->rowh - g) / 2));
		}
		else
		{
			sprintf(tbuf, "%d", (int) (i + 1));
			apj_skin_text(vh, (short) (l->x + numx + cw * (2 - (short) strlen(tbuf))),
			              (short) (iy + (u->rowh - ch) / 2),
			              apj_skin_pen(APJ_X_MUTED), tbuf);
		}
		{
			char clip[96];
			short avail = (short) (l->w - namex - M(12) -
			                       (vidui_scroll_needed(u) ? M(SCROLLW) + M(6) : 0));

			fit_text(vh, clip, (short) sizeof(clip), nm, avail);
			apj_skin_text(vh, (short) (l->x + namex),
			              (short) (iy + (u->rowh - ch) / 2),
			              apj_skin_pen(sel ? APJ_R_SELFG : APJ_R_TEXT), clip);
		}
		iy = (short) (iy + u->rowh);
	}

	if (vidui_scroll_needed(u))
	{
		const APJ_LAY *sb = apj_lay_find(u->lay, u->nlay, W_SCROLL);
		GRECT t;

		if (sb && vis(u, sb->x, sb->y, sb->w, sb->h))
		{
			apj_skin_9(vh, APJ_RG_VSCROLL, APJ_VS_TROUGH, sb->x, sb->y, sb->w, sb->h);
			vidui_thumb_rect(u, &t);
			apj_skin_9(vh, APJ_RG_VSCROLL,
			           u->dragging ? APJ_VS_HELD : APJ_VS_THUMB,
			           t.g_x, t.g_y, t.g_w, t.g_h);
		}
	}
}

/* one line of text centred in the pane, in the small font */
static void pane_line(VIDUI *u, short vh, const APJ_LAY *l, short y,
                      short pen, const char *s)
{
	char fit[160];
	short w;

	fit_text(vh, fit, (short) sizeof(fit), s, (short) (l->w - M(16)));
	w = (short) (cellw(vh) * (short) strlen(fit));
	apj_skin_text(vh, (short) (l->x + (l->w - w) / 2), y, apj_skin_pen(pen), fit);
	(void) u;
}

/*
 * The picture box. Black, always: the overlay plane sits over it while a
 * film plays, and what it does not cover - the letterbox bars, the whole
 * box when it is hidden - is black too, so there is no edge to see when
 * the picture starts or stops. Anything the box has to SAY is drawn on
 * that black in the theme's muted pen.
 */
static void draw_pane(VIDUI *u, short vh)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, W_VIDEO);
	short g = apj_skin_glyphsz(), y, ch;
	char buf[96];

	if (!l || !vis(u, l->x, l->y, l->w, l->h))
		return;
	apj_fill(vh, l->x, l->y, l->w, l->h, G_BLACK);

	if (u->msg == VID_MSG_NONE)
		return;

	ui_font(vh, F_SMALL);
	ch = cellh(vh);
	if (u->msg == VID_MSG_IDLE)
	{
		/* the video glyph, and one line under it, as a group in the middle */
		y = (short) (l->y + (l->h - (g + M(8) + ch)) / 2);
		if (y < l->y + M(4))
			y = (short) (l->y + M(4));
		apj_skin_glyph(vh, APJ_G_VIDEO, apj_skin_pen(APJ_X_MUTED),
		               (short) (l->x + (l->w - g) / 2), y);
		pane_line(u, vh, l, (short) (y + g + M(8)), APJ_X_MUTED,
		          u->ntracks ? "Pick a file from the list, or press play"
		                     : "Open a file...");
		return;
	}

	y = (short) (l->y + (l->h - 3 * ch - M(8)) / 2);
	if (y < l->y + M(4))
		y = (short) (l->y + M(4));
	switch (u->msg)
	{
		case VID_MSG_WAIT:
			pane_line(u, vh, l, y, APJ_X_MUTED, "Waiting for the first frame...");
			break;
		case VID_MSG_TOOBIG:
			sprintf(buf, "%ldx%ld cannot be drawn smaller than %ldx%ld",
			        u->vid_w, u->vid_h, u->min_dw, u->min_dh);
			pane_line(u, vh, l, y, APJ_X_MUTED, buf);
			pane_line(u, vh, l, (short) (y + ch + M(4)), APJ_X_MUTED,
			          "Make the window bigger, or press F for fullscreen");
			break;
		case VID_MSG_FLOOR_UNFIT:
			sprintf(buf, "%ldx%ld needs more than this screen",
			        u->vid_w, u->vid_h);
			pane_line(u, vh, l, y, APJ_X_MUTED, buf);
			pane_line(u, vh, l, (short) (y + ch + M(4)), APJ_X_MUTED,
			          "Press F for fullscreen");
			break;
	}
	pane_line(u, vh, l, (short) (y + 2 * (ch + M(4))), APJ_X_MUTED,
	          "The sound is playing");
}

static void draw_status(VIDUI *u, short vh)
{
	char buf[128];
	short y, ch;

	ui_font(vh, F_SMALL);
	ch = cellh(vh);
	y = (short) (u->work.g_y + u->work.g_h - M(PAD) - ch);
	if (!vis(u, u->work.g_x, y, u->work.g_w, ch))
		return;
	apj_fill(vh, (short) (u->work.g_x + M(PAD)), y,
	         (short) (u->work.g_w - 2 * M(PAD)), ch,
	         apj_skin_pen(APJ_R_PANEL));

	sprintf(buf, "%d file%s", (int) u->ntracks, u->ntracks == 1 ? "" : "s");
	apj_skin_text(vh, (short) (u->work.g_x + M(PAD)), y,
	              apj_skin_pen(APJ_X_MUTED), buf);
	{
		short right = (short) (u->work.g_x + u->work.g_w - M(PAD));
		short x = (short) (u->work.g_x + M(PAD) + cellw(vh) * 12);
		short avail;

		if (u->shown && u->shown[0])
		{
			short w = (short) (cellw(vh) * (short) strlen(u->shown));

			apj_skin_text(vh, (short) (right - w), y,
			              apj_skin_pen(APJ_X_MUTED), u->shown);
			right = (short) (right - w - M(12));
		}
		avail = (short) (right - x);
		if (u->dir && avail > 0)
		{
			char fit[160];

			fit_text(vh, fit, (short) sizeof(fit), u->dir, avail);
			apj_skin_text(vh, x, y, apj_skin_pen(APJ_X_MUTED), fit);
		}
	}
}

/* ------------------------------------------------------------ partials */

void vidui_draw_band(VIDUI *u, short vh)
{
	if (!apj_skin_ok())
	{
		draw_plain(u, vh);
		return;
	}
	draw_seek(u, vh);
	draw_transport(u, vh);
	ui_font(vh, F_BODY);
}

void vidui_draw_clock(VIDUI *u, short vh)
{
	if (!apj_skin_ok())
	{
		draw_plain(u, vh);
		return;
	}
	draw_seek(u, vh);
	ui_font(vh, F_BODY);
}

void vidui_draw_info(VIDUI *u, short vh)
{
	if (!apj_skin_ok())
		return;
	draw_info(u, vh);
	ui_font(vh, F_BODY);
}

void vidui_draw_status(VIDUI *u, short vh)
{
	if (!apj_skin_ok())
		return;
	draw_status(u, vh);
	ui_font(vh, F_BODY);
}

void vidui_draw_list(VIDUI *u, short vh)
{
	if (!apj_skin_ok())
	{
		draw_plain(u, vh);
		return;
	}
	if (u->listmode)
		draw_list(u, vh);
	else
		draw_pane(u, vh);
	ui_font(vh, F_BODY);
}

void vidui_draw_pane(VIDUI *u, short vh)
{
	vidui_draw_list(u, vh);
}

void vidui_clock_rect(VIDUI *u, GRECT *r)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, W_SEEK);

	if (!l)
	{
		*r = u->work;
		return;
	}
	r->g_x = (short) (u->work.g_x + M(PAD));
	r->g_y = l->y;
	r->g_w = (short) (u->work.g_w - 2 * M(PAD));
	r->g_h = l->h;
}

void vidui_info_rect(VIDUI *u, GRECT *r)
{
	r->g_x = (short) (u->work.g_x + M(PAD));
	r->g_y = (short) (u->work.g_y + M(PAD));
	r->g_w = (short) (u->work.g_w - 2 * M(PAD));
	r->g_h = u->infoh;
}

void vidui_status_rect(VIDUI *u, GRECT *r)
{
	short sh = u->smallh > 0 ? u->smallh : 16;

	/* the same strip draw_status() erases */
	r->g_x = (short) (u->work.g_x + M(PAD));
	r->g_y = (short) (u->work.g_y + u->work.g_h - M(PAD) - sh);
	r->g_w = (short) (u->work.g_w - 2 * M(PAD));
	r->g_h = sh;
}

/*
 * No skin file: the flat apjgui controls, and a line saying why.
 */
static void draw_plain(VIDUI *u, short vh)
{
	static const char *lbl[9] = { "|<", "<<", ">", ">>", ">|", "[]", "Open", "Full", "List" };
	static const short wid[9] = { W_PREV, W_RW, W_PLAY, W_FF, W_NEXT, W_STOP,
	                              W_OPEN, W_FULL, W_LISTTOG };
	short i, ch = cellh(vh), y;
	char buf[80];

	apj_fill(vh, u->work.g_x, u->work.g_y, u->work.g_w, u->work.g_h,
	         apj_pen(APJ_R_PANEL));
	{
		char why[256];

		sprintf(why, "No skin: %s not in %s", apj_skin_wanted(), apj_skin_tried());
		apj_text(vh, (short) (u->work.g_x + 2),
		         (short) (u->work.g_y + u->work.g_h - ch),
		         apj_pen(APJ_R_DISABLED), why);
	}
	apj_text(vh, (short) (u->work.g_x + 2), (short) (u->work.g_y + 2),
	         apj_pen(APJ_R_TEXT), u->title ? u->title : "");
	sprintf(buf, "%ld:%02ld / %ld:%02ld  %s  %d/%d",
	        u->pos_s / 60L, u->pos_s % 60L, u->len_s / 60L, u->len_s % 60L,
	        !u->playing ? "stopped" : (u->paused ? "paused" : "playing"),
	        (int) (u->ntracks ? u->sel + 1 : 0), (int) u->ntracks);
	apj_text(vh, (short) (u->work.g_x + 2), (short) (u->work.g_y + 2 + ch),
	         apj_pen(APJ_R_TEXT), buf);

	for (i = 0; i < 9; i++)
	{
		const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, wid[i]);

		if (l)
			apj_button(vh, l->x, l->y, l->w, l->h, lbl[i],
			           u->press == wid[i], wid[i] == W_PLAY);
	}
	{
		const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, u->listmode ? W_LIST : W_VIDEO);

		if (!l)
			return;
		if (!u->listmode)
		{
			apj_fill(vh, l->x, l->y, l->w, l->h, G_BLACK);
			return;
		}
		apj_fill(vh, l->x, l->y, l->w, l->h, apj_pen(APJ_R_PAPER));
		y = l->y;
		for (i = 0; i < u->visrows && u->top + i < u->ntracks; i++)
		{
			short k = (short) (u->top + i);
			const char *nm = u->name_of ? u->name_of(u->ctx, k) : "";

			if (k == u->sel)
			{
				apj_select(vh, l->x, y, l->w, ch);
				apj_text(vh, (short) (l->x + 2), y, apj_pen(APJ_R_SELFG), nm);
			}
			else
				apj_text(vh, (short) (l->x + 2), y, apj_pen(APJ_R_TEXT), nm);
			y = (short) (y + ch);
		}
	}
}

/*
 * A redraw of the parts that meet clip - what a WM_REDRAW wants. A window
 * dragged across this one exposes it a thin strip at a time; drawing
 * everything for each strip is what made that drag stutter over MP3GEM.
 */
void vidui_draw_clip(VIDUI *u, short vh, const GRECT *clip)
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
	draw_info(u, vh);
	draw_seek(u, vh);
	draw_transport(u, vh);
	if (u->listmode)
		draw_list(u, vh);
	else
		draw_pane(u, vh);
	draw_status(u, vh);
	ui_font(vh, F_BODY);
	u->clip = u->work;
}

void vidui_draw(VIDUI *u, short vh)
{
	vidui_draw_clip(u, vh, &u->work);
}
