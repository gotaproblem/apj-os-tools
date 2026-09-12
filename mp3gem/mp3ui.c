/*
 * mp3ui.c - see mp3ui.h. Every control is a rectangle out of the skin
 * sheet; the only pixels the 68k touches are the coverage blend behind
 * the play mark in the selected playlist row.
 *
 * Geometry is written in POINTS and put through apj_skin_m(), so one
 * source serves 1x, 1.25x and 1.75x - the DPI rule from the GUI redesign.
 */

#include <stdio.h>
#include <string.h>

#include "mp3ui.h"

#define PAD		10	/* window inset, points          */
#define ART		96
#define GAPX		12
#define SEEKH		6
#define KNOB		12
#define ROWH		24
#define STATH		14
#define BADGEH		16
#define SCROLLW		6	/* skin metric scroll_w - XaAES's thumb width */

#define M(pt)		apj_skin_m(pt)



static void draw_plain(MP3UI *u, short vh);

static void hhmmss(char *out, long s)
{
	if (s < 0)
		s = 0;
	if (s >= 3600L)
		sprintf(out, "%ld:%02ld:%02ld", s / 3600L, (s / 60L) % 60L, s % 60L);
	else
		sprintf(out, "%ld:%02ld", s / 60L, s % 60L);
}

/*
 * Three sizes out of the APJ*.FNT set fVDI loads (9x18 up to 16x32).
 * Everything in one system-font cell is most of what made the first build
 * look flat - a 20 pt title against 11 pt badges is the difference.
 */
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

/*
 * Fit s into avail pixels of the current font: copy it to out, and if it
 * is too long cut it and end it with "...". outsz includes the NUL.
 */
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
static int vis(const MP3UI *u, short x, short y, short w, short h)
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

static void add(MP3UI *u, short id, short x, short y, short w, short h)
{
	if (u->nlay >= MP3UI_MAXLAY - 1)
		return;
	u->lay[u->nlay].id = id;
	u->lay[u->nlay].x = x;
	u->lay[u->nlay].y = y;
	u->lay[u->nlay].w = w;
	u->lay[u->nlay].h = h;
	u->nlay++;
	u->lay[u->nlay].id = -1;
}

/*
 * The cover is blitted from an MFDB, and an MFDB's width has to be a
 * multiple of 16 - so the image is the tile rounded down to 16 and sits
 * centred on the placeholder, which then reads as its frame.
 */
short mp3ui_artedge(void)
{
	return (short) (M(ART) & ~15);
}

/*
 * The decoded cover, straight from the app's buffer. Same shape as an
 * APJSKIN blit - an MFDB in device format, so fVDI copies it host-side -
 * except the pixels came from MP3PLAY sub-op 9 rather than the sheet.
 */
static void blit_art(MP3UI *u, short vh, short x, short y)
{
	MFDB src, scr;
	short pxy[8], e = u->artedge, fdw;
	short ext[57];

	if (!u->artbuf || e <= 0)
		return;
	vq_extnd(vh, 1, ext);
	fdw = (short) ((e + 15) & ~15);
	if (fdw != e)
		return;			/* mp3ui_artedge() guarantees this; be safe */
	src.fd_addr = u->artbuf;
	src.fd_w = e;
	src.fd_h = e;
	src.fd_wdwidth = (short) (e >> 4);
	src.fd_stand = 0;
	src.fd_nplanes = ext[4];
	src.fd_r1 = src.fd_r2 = src.fd_r3 = 0;
	scr.fd_addr = NULL;
	pxy[0] = 0; pxy[1] = 0; pxy[2] = (short) (e - 1); pxy[3] = (short) (e - 1);
	pxy[4] = x; pxy[5] = y; pxy[6] = (short) (x + e - 1); pxy[7] = (short) (y + e - 1);
	vro_cpyfm(vh, S_ONLY, pxy, &src, &scr);
}

void mp3ui_minsize(short *w, short *h)
{
	*w = (short) (M(PAD) * 2 + M(ART) + M(GAPX) + apj_skin_tilew() * 6);
	*h = (short) (M(PAD) * 2 + M(ART) + M(22) + apj_skin_tileh() +
	              M(10) + M(ROWH) * 2 + M(STATH));
}

/* ---------------------------------------------------------------- layout */

void mp3ui_layout(MP3UI *u, short vh, short wx, short wy, short ww, short wh)
{
	short pad = M(PAD), art = M(ART);
	short tw = apj_skin_tilew(), th = apj_skin_tileh(), acc = apj_skin_accd();

	if (!apj_skin_ok())			/* plain controls, character sized */
	{
		tw = (short) (cellw(vh) * 5);
		th = (short) (cellh(vh) + 4);
		acc = th;
		art = 0;
		pad = 2;
	}
	short x, y, sy, ly, lh;
	short sw, sh, bh, tlab, stath;

	/*
	 * The fonts do not scale with the sheet. fVDI picks the nearest of the
	 * sizes it has (9x18 up; APJ*.FNT), so a "10 pt" small font at 100% is
	 * still 18 px tall while a 14 pt strip is 14 px - the status line sat
	 * on top of the playlist and "-4:40" ran off the right edge. So every
	 * strip that holds text is sized from the cell the font really has,
	 * and the sheet's metric is only a floor.
	 */
	ui_font(vh, F_SMALL);
	sw = cellw(vh);
	sh = cellh(vh);
	ui_font(vh, F_BODY);
	bh = cellh(vh);
	tlab = (short) (sw * 6 + M(6));			/* "-88:88" and a gap */
	if (tlab < M(46))
		tlab = M(46);
	stath = (short) (sh + M(4));
	if (stath < M(STATH))
		stath = M(STATH);

	u->work.g_x = wx; u->work.g_y = wy;
	u->work.g_w = ww; u->work.g_h = wh;
	u->clip = u->work;
	u->nlay = 0;
	u->lay[0].id = -1;

	if (art > 0)
		add(u, W_ART, (short) (wx + pad), (short) (wy + pad), art, art);

	/* seek strip */
	sy = (short) (wy + pad + art + (art ? M(GAPX) : 3 * cellh(vh)));
	add(u, W_SEEK, (short) (wx + pad + tlab), sy,
	    (short) (ww - 2 * pad - 2 * tlab), (short) (M(SEEKH) + M(6)));

	/* transport */
	y = (short) (sy + M(22));
	x = (short) (wx + pad);
	add(u, W_PREV, x, y, tw, th); x = (short) (x + tw + M(2));
	add(u, W_RW,   x, y, tw, th); x = (short) (x + tw + M(2));
	add(u, W_PLAY, x, (short) (y - (acc - th) / 2), acc, acc);
	x = (short) (x + acc + M(2));
	add(u, W_FF,   x, y, tw, th); x = (short) (x + tw + M(2));
	add(u, W_NEXT, x, y, tw, th); x = (short) (x + tw + M(2));
	add(u, W_STOP, x, y, tw, th);

	x = (short) (wx + ww - pad - tw);
	add(u, W_OPEN, x, y, tw, th);
	x = (short) (x - M(72) - M(8));
	add(u, W_VOL, x, (short) (y + th / 2 - M(3)), M(72), M(SEEKH) + M(6));
	x = (short) (x - tw - M(4));
	add(u, W_VOLICO,  x, y, tw, th); x = (short) (x - tw - M(2));
	add(u, W_REPEAT,  x, y, tw, th); x = (short) (x - tw - M(2));
	add(u, W_SHUFFLE, x, y, tw, th);

	/* playlist */
	ly = (short) (y + th + M(10));
	lh = (short) (wy + wh - ly - pad - stath);
	u->rowh = apj_skin_ok() ? M(ROWH) : cellh(vh);
	if (u->rowh < bh + M(4))
		u->rowh = (short) (bh + M(4));		/* a row never shorter than its text */
	if (lh < u->rowh + 2)
		lh = (short) (u->rowh + 2);
	/* the scrollbar sits INSIDE the list's rectangle, and apj_lay_hit()
	 * answers with the first match - so it has to go in first */
	add(u, W_SCROLL, (short) (wx + ww - pad - M(4) - M(SCROLLW)),
	    (short) (ly + M(4)), M(SCROLLW), (short) (lh - M(8)));
	add(u, W_LIST, (short) (wx + pad), ly, (short) (ww - 2 * pad), lh);

	u->visrows = (short) ((lh - (apj_skin_ok() ? M(8) : 0)) / u->rowh);
	if (u->visrows < 0)
		u->visrows = 0;
}

void mp3ui_bbox(MP3UI *u, GRECT *r)
{
	short i, x0 = 0x7fff, y0 = 0x7fff, x1 = -1, y1 = -1;

	for (i = 0; i < u->nlay && u->lay[i].id >= 0; i++)
	{
		if (u->lay[i].id == W_LIST || u->lay[i].id == W_ART ||
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

short mp3ui_scroll_needed(MP3UI *u)
{
	return (u->ntracks > u->visrows && u->visrows > 0) ? 1 : 0;
}

void mp3ui_thumb_rect(MP3UI *u, GRECT *r)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, W_SCROLL);
	long span = (long) u->ntracks - u->visrows;
	short th, ty;

	if (!l || !mp3ui_scroll_needed(u))
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

short mp3ui_scroll_part(MP3UI *u, short my)
{
	GRECT t;

	mp3ui_thumb_rect(u, &t);
	if (my < t.g_y)
		return -1;
	if (my >= t.g_y + t.g_h)
		return 1;
	return 0;
}

short mp3ui_scroll_top_for(MP3UI *u, short my, short grab)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, W_SCROLL);
	GRECT t;
	long span = (long) u->ntracks - u->visrows, room, top;

	if (!l || span <= 0)
		return 0;
	mp3ui_thumb_rect(u, &t);
	room = (long) l->h - t.g_h;
	if (room <= 0)
		return 0;
	top = ((long) (my - grab - l->y) * span + room / 2) / room;
	if (top < 0)    top = 0;
	if (top > span) top = span;
	return (short) top;
}

short mp3ui_hit(MP3UI *u, short mx, short my)
{
	return apj_lay_hit(u->lay, u->nlay, mx, my);
}

short mp3ui_row_at(MP3UI *u, short my)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, W_LIST);
	short r;

	if (!l)
		return -1;
	r = (short) ((my - (l->y + M(4))) / u->rowh);
	if (r < 0 || r >= u->visrows || u->top + r >= u->ntracks)
		return -1;
	return (short) (u->top + r);
}

/* ------------------------------------------------------------------ draw */

static short state_of(MP3UI *u, short id, short on)
{
	if (u->press == id)
		return APJ_ST_PRESS;
	if (on)
		return APJ_ST_ON;
	if (u->hover == id)
		return APJ_ST_HOVER;
	return APJ_ST_NORM;
}

static void tilew_at(MP3UI *u, short vh, short id, short glyph, short on)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, id);

	if (l && vis(u, l->x, l->y, l->w, l->h))
		apj_skin_tile(vh, glyph, state_of(u, id, on), l->x, l->y);
}

static void draw_seek(MP3UI *u, short vh)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, W_SEEK);
	short th = M(SEEKH), ty, fw, kn = M(KNOB);
	char a[24], b[32];

	if (!l || !vis(u, u->work.g_x, l->y, u->work.g_w, l->h))
		return;
	ty = (short) (l->y + (l->h - th) / 2);

	/*
	 * The whole strip first. This runs on every timer tick, the text is
	 * drawn transparently and the knob is taller than the track, so
	 * without an erase the digits pile up on each other and the knob
	 * leaves crumbs behind it.
	 */
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
	sprintf(b, "-%ld:%02ld", (u->len_s - u->pos_s) / 60L,
	        (u->len_s - u->pos_s) % 60L);
	apj_skin_text(vh, (short) (u->work.g_x + M(PAD)), (short) (ty - cellh(vh) / 3),
	         apj_skin_pen(APJ_X_MUTED), a);
	apj_skin_text(vh, (short) (l->x + l->w + M(6)), (short) (ty - cellh(vh) / 3),
	         apj_skin_pen(APJ_X_MUTED), b);
}

static void draw_transport(MP3UI *u, short vh)
{
	const APJ_LAY *l;
	short vg;

	tilew_at(u, vh, W_PREV, APJ_G_PREV, 0);
	tilew_at(u, vh, W_RW,   APJ_G_RW,   0);
	tilew_at(u, vh, W_FF,   APJ_G_FF,   0);
	tilew_at(u, vh, W_NEXT, APJ_G_NEXT, 0);
	tilew_at(u, vh, W_STOP, APJ_G_STOP, 0);
	tilew_at(u, vh, W_SHUFFLE, APJ_G_SHUFFLE, u->shuffle);
	tilew_at(u, vh, W_REPEAT,  APJ_G_REPEAT,  u->repeat);
	tilew_at(u, vh, W_OPEN,    APJ_G_OPEN,    0);

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

static void draw_now(MP3UI *u, short vh)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, W_ART);
	short ix, by, cw = cellw(vh), ch = cellh(vh);
	char buf[32];

	if (!l)
		return;
	if (!vis(u, u->work.g_x, l->y, u->work.g_w, l->h))
		return;
	if (vis(u, l->x, l->y, l->w, l->h))
	{
	apj_skin_blit(vh, APJ_RG_ARTPH, 0, l->x, l->y);
	if (u->hasart && u->artbuf)
		blit_art(u, vh, (short) (l->x + (l->w - u->artedge) / 2),
		         (short) (l->y + (l->h - u->artedge) / 2));
	}

	ix = (short) (l->x + l->w + M(GAPX));

	{
		char fit[160];
		short avail = (short) (u->work.g_x + u->work.g_w - M(PAD) - ix);

		ui_font(vh, F_TITLE);
		fit_text(vh, fit, (short) sizeof(fit), u->title ? u->title : "", avail);
		apj_skin_text(vh, ix, (short) (l->y + M(4)), apj_skin_pen(APJ_R_TEXT), fit);
		by = (short) (l->y + M(4) + cellh(vh) + M(4));

		ui_font(vh, F_BODY);
		fit_text(vh, fit, (short) sizeof(fit), u->sub ? u->sub : "", avail);
		apj_skin_text(vh, ix, by, apj_skin_pen(APJ_X_MUTED), fit);
		by = (short) (by + cellh(vh) + M(10));
	}

	ui_font(vh, F_SMALL);
	cw = cellw(vh);
	ch = cellh(vh);
	if (u->codec)
	{
		short w = (short) (cw * (short) strlen(u->codec) + M(12));

		apj_skin_9(vh, APJ_RG_BADGE, 0, ix, by, w, M(BADGEH));
		apj_skin_text(vh, (short) (ix + M(6)),
		         (short) (by + (M(BADGEH) - ch) / 2),
		         apj_skin_pen(APJ_X_MUTED), u->codec);
		ix = (short) (ix + w + M(6));
	}
	if (u->bitrate > 0)
	{
		short w;

		sprintf(buf, "%d kbps", (int) u->bitrate);
		w = (short) (cw * (short) strlen(buf) + M(12));
		apj_skin_9(vh, APJ_RG_BADGE, 0, ix, by, w, M(BADGEH));
		apj_skin_text(vh, (short) (ix + M(6)),
		         (short) (by + (M(BADGEH) - ch) / 2),
		         apj_skin_pen(APJ_X_MUTED), buf);
		ix = (short) (ix + w + M(6));
	}
	{
		const char *st = !u->playing ? "STOPPED" : (u->paused ? "PAUSED" : "PLAYING");
		short w = (short) (cw * (short) strlen(st) + M(12));

		apj_skin_9(vh, APJ_RG_BADGE, u->playing && !u->paused ? 1 : 0,
		           ix, by, w, M(BADGEH));
		apj_skin_text(vh, (short) (ix + M(6)),
		         (short) (by + (M(BADGEH) - ch) / 2),
		         apj_skin_pen(u->playing && !u->paused ? APJ_X_ACCENT_INK
		                                               : APJ_X_MUTED), st);
	}
}

static void draw_list(MP3UI *u, short vh)
{
	const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, W_LIST);
	short r, iy, ch, cw, g = apj_skin_glyphsz();
	char tbuf[16];

	ui_font(vh, F_BODY);
	ch = cellh(vh);
	cw = cellw(vh);
	if (!l || !vis(u, l->x, l->y, l->w, l->h))
		return;
	apj_skin_9(vh, APJ_RG_GROUP, 0, l->x, l->y, l->w, l->h);

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
			                    (mp3ui_scroll_needed(u) ? M(SCROLLW) + M(8) : 0)),
			           u->rowh);
			if (u->playing)
				apj_skin_glyph(vh, APJ_G_PLAY, apj_skin_pen(APJ_R_ACCENT),
				               (short) (l->x + M(10)),
				               (short) (iy + (u->rowh - g) / 2));
		}
		else
		{
			sprintf(tbuf, "%d", (int) (i + 1));
			apj_skin_text(vh, (short) (l->x + M(12)),
			         (short) (iy + (u->rowh - ch) / 2),
			         apj_skin_pen(APJ_X_MUTED), tbuf);
		}
		{
			char clip[96];
			short avail = (short) (l->w - M(34) - M(12) - cw * 6 - M(8) -
			                       (mp3ui_scroll_needed(u) ? M(SCROLLW) + M(6) : 0));

			fit_text(vh, clip, (short) sizeof(clip), nm, avail);
			apj_skin_text(vh, (short) (l->x + M(34)),
			         (short) (iy + (u->rowh - ch) / 2),
			         apj_skin_pen(sel ? APJ_R_SELFG : APJ_R_TEXT), clip);
		}
		if (u->len_of)
		{
			long secs = u->len_of(u->ctx, i);

			if (secs > 0)
			{
				short right = (short) (l->x + l->w - M(12) -
				               (mp3ui_scroll_needed(u) ? M(SCROLLW) + M(6) : 0));

				hhmmss(tbuf, secs);
				apj_skin_text(vh,
				         (short) (right - cw * (short) strlen(tbuf)),
				         (short) (iy + (u->rowh - ch) / 2),
				         apj_skin_pen(APJ_X_MUTED), tbuf);
			}
		}
		iy = (short) (iy + u->rowh);
	}

	if (mp3ui_scroll_needed(u))
	{
		const APJ_LAY *sb = apj_lay_find(u->lay, u->nlay, W_SCROLL);
		GRECT t;

		if (sb)
		{
			apj_skin_9(vh, APJ_RG_VSCROLL, APJ_VS_TROUGH, sb->x, sb->y, sb->w, sb->h);
			mp3ui_thumb_rect(u, &t);
			apj_skin_9(vh, APJ_RG_VSCROLL,
			           u->dragging ? APJ_VS_HELD : APJ_VS_THUMB,
			           t.g_x, t.g_y, t.g_w, t.g_h);
		}
	}
}

/* just the playlist - what a scroll repaints */
void mp3ui_draw_list(MP3UI *u, short vh)
{
	if (!apj_skin_ok())
	{
		draw_plain(u, vh);
		return;
	}
	draw_list(u, vh);
	ui_font(vh, F_BODY);
}

static void draw_status(MP3UI *u, short vh)
{
	char buf[128];
	short y;

	ui_font(vh, F_SMALL);
	y = (short) (u->work.g_y + u->work.g_h - M(PAD) - cellh(vh));
	if (!vis(u, u->work.g_x, y, u->work.g_w, cellh(vh)))
		return;
	apj_fill(vh, (short) (u->work.g_x + M(PAD)), y,
	         (short) (u->work.g_w - 2 * M(PAD)), cellh(vh),
	         apj_skin_pen(APJ_R_PANEL));

#if MP3UI_DEBUG
	sprintf(buf, "%d tracks   t%ld p%ld s%ld", (int) u->ntracks,
	        u->dbg_ticks, u->dbg_pos, u->dbg_status);
#else
	if (u->ntimed >= 0 && u->ntimed < u->ntracks)
		sprintf(buf, "%d tracks, timing %d", (int) u->ntracks, (int) u->ntimed);
	else
		sprintf(buf, "%d tracks", (int) u->ntracks);
#endif
	apj_skin_text(vh, (short) (u->work.g_x + M(PAD)), y,
	         apj_skin_pen(APJ_X_MUTED), buf);
	if (u->dir)
	{
		char fit[160];
		short x = (short) (u->work.g_x + M(PAD) + cellw(vh) * 26);
		short avail = (short) (u->work.g_x + u->work.g_w - M(PAD) - x);

		fit_text(vh, fit, (short) sizeof(fit), u->dir, avail);
		apj_skin_text(vh, x, y, apj_skin_pen(APJ_X_MUTED), fit);
	}
}

/* seek + transport: what a hover or a press repaints */
void mp3ui_draw_band(MP3UI *u, short vh)
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

/*
 * What the timer repaints: the seek strip and nothing else. Every redraw
 * takes the AES's update lock and blends antialiased text on the 68k; a
 * whole-band repaint four times a second was enough to make dragging
 * another window jerky while a track played.
 */
void mp3ui_draw_clock(MP3UI *u, short vh)
{
	if (!apj_skin_ok())
	{
		draw_plain(u, vh);
		return;
	}
	draw_seek(u, vh);
	ui_font(vh, F_BODY);
}

void mp3ui_draw_status(MP3UI *u, short vh)
{
	if (!apj_skin_ok())
		return;
	draw_status(u, vh);
	ui_font(vh, F_BODY);
}

void mp3ui_clock_rect(MP3UI *u, GRECT *r)
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

/*
 * No skin file: fall back to the flat apjgui controls the app had before,
 * so a machine without SKINS\\ installed still runs.
 */
static void draw_plain(MP3UI *u, short vh)
{
	static const char *lbl[7] = { "|<", "<<", ">", ">>", ">|", "[]", "Open" };
	static const short wid[7] = { W_PREV, W_RW, W_PLAY, W_FF, W_NEXT, W_STOP,
	                              W_OPEN };
	short i, ch = cellh(vh), y;
	char buf[80];

	apj_fill(vh, u->work.g_x, u->work.g_y, u->work.g_w, u->work.g_h,
	         apj_pen(APJ_R_PANEL));

	/* Say why this is the plain look. Falling back silently made a missing
	 * skin file indistinguishable from the app simply not being rebuilt. */
	{
		char why[256];

		sprintf(why, "No skin: %s not in %s",
		        apj_skin_wanted(), apj_skin_tried());
		apj_text(vh, (short) (u->work.g_x + 2),
		         (short) (u->work.g_y + u->work.g_h - ch),
		         apj_pen(APJ_R_DISABLED), why);
	}

	apj_text(vh, (short) (u->work.g_x + 2), (short) (u->work.g_y + 2),
	         apj_pen(APJ_R_TEXT), u->title ? u->title : "");
	apj_text(vh, (short) (u->work.g_x + 2), (short) (u->work.g_y + 2 + ch),
	         apj_pen(APJ_R_TEXT), u->sub ? u->sub : "");
	sprintf(buf, "%ld:%02ld / %ld:%02ld  %s  %d/%d",
	        u->pos_s / 60L, u->pos_s % 60L, u->len_s / 60L, u->len_s % 60L,
	        !u->playing ? "stopped" : (u->paused ? "paused" : "playing"),
	        (int) (u->ntracks ? u->sel + 1 : 0), (int) u->ntracks);
	apj_text(vh, (short) (u->work.g_x + 2), (short) (u->work.g_y + 2 + 2 * ch),
	         apj_pen(APJ_R_TEXT), buf);

	for (i = 0; i < 7; i++)
	{
		const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, wid[i]);

		if (l)
			apj_button(vh, l->x, l->y, l->w, l->h, lbl[i],
			           u->press == wid[i], wid[i] == W_PLAY);
	}

	{
		const APJ_LAY *l = apj_lay_find(u->lay, u->nlay, W_LIST);

		if (!l)
			return;
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
 * A redraw of the parts that meet clip - what a WM_REDRAW wants. While
 * another window is dragged across this one XaAES exposes it a thin strip
 * at a time, and drawing everything clipped to each strip - forty-odd
 * blits and a dozen antialiased strings per strip - was what made the
 * drag stutter over the player.
 */
void mp3ui_draw_clip(MP3UI *u, short vh, const GRECT *clip)
{
	GRECT c = *clip;

	/* intersect with the work area */
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
	if (0)
		apj_skin_tilex(vh, APJ_RG_PANELTOP, 0, u->work.g_x, u->work.g_y,
		               u->work.g_w);
	draw_now(u, vh);
	draw_seek(u, vh);
	draw_transport(u, vh);
	draw_list(u, vh);
	draw_status(u, vh);
	ui_font(vh, F_BODY);
	u->clip = u->work;
}

void mp3ui_draw(MP3UI *u, short vh)
{
	u->clip = u->work;
	if (!apj_skin_ok())
	{
		draw_plain(u, vh);
		return;
	}
	ui_font(vh, F_BODY);
	apj_fill(vh, u->work.g_x, u->work.g_y, u->work.g_w, u->work.g_h,
	         apj_skin_pen(APJ_R_PANEL));
	apj_skin_tilex(vh, APJ_RG_PANELTOP, 0, u->work.g_x, u->work.g_y,
	               u->work.g_w);
	draw_now(u, vh);
	draw_seek(u, vh);
	draw_transport(u, vh);
	draw_list(u, vh);
	draw_status(u, vh);
	ui_font(vh, F_BODY);
}
