/*
 * apjskin.c - APJSKIN implementation. See apjskin.h.
 *
 * File layout (big-endian, written by skins/mkskin.py):
 *
 *   0  'APJS'  u32          32 sheet w        (multiple of 16, for the MFDB)
 *   4  version u16          34 pixel format   (0 = RGB24)
 *   6  scale   u16          36 reserved
 *   8  sheet w u16
 *  10  sheet h u16          then, at the offsets in the header:
 *  12  nregion u16            REGN  24 bytes x nregion
 *  14  nglyph  u16            PALT   4 bytes x 32, 0x00RRGGBB
 *  16  glyph sz u16           MASK   8-bit coverage, glyph atlas
 *  18  glyph cols u16         PIXL  RGB24, top-down, w*h*3
 *  20  mask w  u16
 *  22  off REGN u32
 *  26  off PALT u32
 *  30  off MASK u32   <- (see the packed struct below; the C reads it
 *  34  off PIXL u32       field by field, not by overlaying a struct,
 *  38  tile w  u16         because the .PRG must not depend on padding)
 *  40  tile h  u16
 *  42  acc d   u16
 *  44  pixfmt  u16
 *  46  reserved u16
 *
 * PIXL is RGB24 on disk and expanded once, at load, into the screen's
 * device format - which is what makes every later blit a straight copy.
 * The 32 bpp layout is 00 RR GG BB and the 16 bpp one big-endian RGB565,
 * the two formats platforms/atari/psimg/psimg.h documents for fVDI.
 */

#include <gem.h>
#include <osbind.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "apjskin.h"

/*
 * On the Atari the sheet goes in alternate RAM through Mxalloc; the host
 * test harness (tests/skin) builds the same source with plain malloc so
 * the parsing and blit arithmetic can be checked before it ever runs on
 * the Pi. Mxalloc mode 3 = "prefer alternate RAM", which is what keeps
 * the sheet above 0x01000000 and out of the JIT's SMC check.
 */
#ifdef APJSKIN_HOST
# include <stdlib.h>
# define SK_ALLOC(n)	((char *) malloc((size_t) (n)))
# define SK_FREE(p)	free(p)
#else
# define SK_ALLOC(n)	((char *) Mxalloc((long) (n), 3))
# define SK_FREE(p)	Mfree(p)
#endif

#define HEAD_LEN	52
#define REG_LEN		24
#define PALT_N		32

/* two workstation pens the engine drives itself, just below the extras */
#define SK_PEN_FILL	(APJ_XPEN_BASE - 2)
#define SK_PEN_BORD	(APJ_XPEN_BASE - 1)

#define F_TILEX		1
#define F_TILEY		2
#define F_SLICE9	4

/* extras live just below apjgui's 237..255 role block; 236 is the pen
 * XaAES render_apj scribbles on when it probes the pixel layout */
#define APJ_XPEN_BASE	(APJ_PEN_BASE - 1 - APJ_X_N + APJ_R_N)

struct reg
{
	short x, y, w, h;
	short n, cols;
	short l, t, r, b;
	short flags;
	short bw;		/* border width, px            */
	short mid;		/* index into sk_mid[]         */
};

static short  sk_ok = 0;
static short  sk_scale = 100;
static short  sk_w, sk_h, sk_planes;
static short  sk_aa = 0;		/* 1 = XaAES's pens agree with the skin */
static short  sk_tilew, sk_tileh, sk_accd;
static short  sk_gsz, sk_gcols, sk_nglyph, sk_maskw;
static char   sk_name[16] = "";
static char   sk_wanted[24] = "";
static char   sk_tried[240] = "";
static struct reg sk_reg[APJ_RG_N];
static long   sk_pal[APJ_X_N];
static long  *sk_mid = NULL;	/* 2 longs per state: fill, border */
static char  *sk_pix = NULL;		/* device-format sheet, TT-RAM      */
static char  *sk_mask = NULL;		/* 8-bit coverage atlas             */
static char  *sk_blend = NULL;		/* scratch for apj_skin_glyph       */
static long   sk_blendsz = 0;
static MFDB   sk_mfdb;

/* ------------------------------------------------------------ helpers -- */

static unsigned short be16(const unsigned char *p)
{
	return (unsigned short) ((p[0] << 8) | p[1]);
}

static unsigned long be32(const unsigned char *p)
{
	return ((unsigned long) p[0] << 24) | ((unsigned long) p[1] << 16) |
	       ((unsigned long) p[2] << 8) | (unsigned long) p[3];
}

short apj_skin_ok(void)      { return sk_ok; }
const char *apj_skin_wanted(void) { return sk_wanted; }
const char *apj_skin_tried(void)  { return sk_tried; }
short apj_skin_scale(void)   { return sk_scale; }
short apj_skin_tilew(void)   { return sk_tilew; }
short apj_skin_tileh(void)   { return sk_tileh; }
short apj_skin_accd(void)    { return sk_accd; }
short apj_skin_glyphsz(void) { return sk_gsz; }

short apj_skin_m(short pt)
{
	long v = ((long) pt * sk_scale + 50L) / 100L;

	if (v < 1 && pt > 0)
		v = 1;
	return (short) v;
}

long apj_skin_rgb(short role)
{
	if (!sk_ok || role < 0 || role >= APJ_X_N)
		return -1L;
	return sk_pal[role] & 0xffffffL;
}

/*
 * A pen for a role. The skin was baked against its own nineteen role
 * colours and carries them in PALT, so it loads all of them into this
 * workstation and answers from there - deferring to apj_pen() meant that
 * on a desktop with no theme committed every role fell back to the classic
 * G_* pens and the text came out black on a dark skin.
 *
 * XaAES's antialiased text path is the exception: it draws on the AES's own
 * workstation, so it only ever sees the theme's pens. That is fine, because
 * apj_text_aa() only takes that path when a theme IS loaded, and a skin
 * follows the theme by name.
 */
short apj_skin_pen(short role)
{
	if (role < 0 || role >= APJ_X_N)
		role = APJ_R_TEXT;
	if (!sk_ok)
		return apj_pen(role < APJ_R_N ? role : APJ_R_TEXT);
	if (role < APJ_R_N)
		return (short) (APJ_PEN_BASE + role);
	return (short) (APJ_XPEN_BASE + (role - APJ_R_N));
}

short apj_skin_has(short rid)
{
	return (sk_ok && rid >= 0 && rid < APJ_RG_N && sk_reg[rid].w > 0) ? 1 : 0;
}

void apj_skin_size(short rid, short *w, short *h)
{
	if (!apj_skin_has(rid))
	{
		*w = *h = 0;
		return;
	}
	*w = sk_reg[rid].w;
	*h = sk_reg[rid].h;
}

/* ----------------------------------------------------------- the blit -- */

static void src_of(short rid, short state, short *sx, short *sy)
{
	struct reg *r = &sk_reg[rid];
	short s = state;

	if (s < 0)
		s = 0;
	if (s >= r->n)
		s = r->n - 1;
	*sx = r->x + (s % r->cols) * r->w;
	*sy = r->y + (s / r->cols) * r->h;
}

static void copy(short vh, short sx, short sy, short sw, short sh,
                 short dx, short dy)
{
	MFDB scr;
	short pxy[8];

	if (sw <= 0 || sh <= 0)
		return;
	scr.fd_addr = NULL;
	pxy[0] = sx;          pxy[1] = sy;
	pxy[2] = sx + sw - 1; pxy[3] = sy + sh - 1;
	pxy[4] = dx;          pxy[5] = dy;
	pxy[6] = dx + sw - 1; pxy[7] = dy + sh - 1;
	vro_cpyfm(vh, S_ONLY, pxy, &sk_mfdb, &scr);
}

/*
 * Text in a skinned window. Through XaAES's atlas when its pens are ours to
 * use, and through the VDI on our own workstation when they are not - or
 * whenever the pen is one of the six extras, which only ever exist here.
 */
void apj_skin_text(short vh, short x, short y, short pen, const char *s)
{
	if (sk_ok && (!sk_aa || pen < APJ_PEN_BASE))
	{
		vswr_mode(vh, MD_TRANS);
		vst_color(vh, pen);
		v_gtext(vh, x, y, (char *) s);
		return;
	}
	apj_text(vh, x, y, pen, s);
}

void apj_skin_blit(short vh, short rid, short state, short x, short y)
{
	short sx, sy;

	if (!apj_skin_has(rid))
		return;
	src_of(rid, state, &sx, &sy);
	copy(vh, sx, sy, sk_reg[rid].w, sk_reg[rid].h, x, y);
}

void apj_skin_tilex(short vh, short rid, short state, short x, short y, short w)
{
	short sx, sy, sw, i, n;

	if (!apj_skin_has(rid) || w <= 0)
		return;
	src_of(rid, state, &sx, &sy);
	sw = sk_reg[rid].w;
	for (i = 0; i < w; i += sw)
	{
		n = (short) (w - i);
		if (n > sw)
			n = sw;
		copy(vh, sx, sy, n, sk_reg[rid].h, (short) (x + i), y);
	}
}

/* the same, but only the top h rows of the region, and only the tiles
 * that meet [x0, x0+w0): a strip repaint of a tiled band */
void apj_skin_tilexh(short vh, short rid, short state, short x, short y,
                     short w, short h, short x0, short w0)
{
	short sx, sy, sw, sh, i, n;

	if (!apj_skin_has(rid) || w <= 0 || h <= 0 || w0 <= 0)
		return;
	src_of(rid, state, &sx, &sy);
	sw = sk_reg[rid].w;
	sh = sk_reg[rid].h;
	if (h < sh)
		sh = h;
	for (i = 0; i < w; i += sw)
	{
		n = (short) (w - i);
		if (n > sw)
			n = sw;
		if (x + i >= x0 + w0 || x + i + n <= x0)
			continue;
		copy(vh, sx, sy, n, sh, (short) (x + i), y);
	}
}

static void bar(short vh, short x, short y, short w, short h, short pen)
{
	short xy[4];

	if (w <= 0 || h <= 0)
		return;
	vswr_mode(vh, MD_REPLACE);
	vsf_interior(vh, FIS_SOLID);
	vsf_perimeter(vh, 0);
	vsf_color(vh, pen);
	xy[0] = x; xy[1] = y; xy[2] = (short) (x + w - 1); xy[3] = (short) (y + h - 1);
	v_bar(vh, xy);
}

static short scratch(short vh, short pen, long v)
{
	short c[3];

	c[0] = (short) ((((v >> 16) & 0xff) * 1000L + 127L) / 255L);
	c[1] = (short) ((((v >> 8) & 0xff) * 1000L + 127L) / 255L);
	c[2] = (short) (((v & 0xff) * 1000L + 127L) / 255L);
	vs_color(vh, pen, c);
	return pen;
}

/*
 * Nine-slice. Only the CORNERS come out of the sheet - everything the
 * stretch touches is flat by authoring convention, so the edges and the
 * middle are v_bar fills in the two colours the skin recorded for this
 * state. Four blits and nine bars, whatever the size; tiling the middle
 * instead cost 1800 blits for one MP3GEM redraw.
 */
void apj_skin_9(short vh, short rid, short state,
                short x, short y, short w, short h)
{
	struct reg *r;
	short sx, sy, l, t, rr, b, bw, s, dmw, dmh, fp, bp;

	if (!apj_skin_has(rid) || w <= 0 || h <= 0)
		return;
	r = &sk_reg[rid];
	src_of(rid, state, &sx, &sy);

	s = state;
	if (s < 0)
		s = 0;
	if (s >= r->n)
		s = r->n - 1;
	fp = scratch(vh, SK_PEN_FILL, sk_mid ? sk_mid[r->mid + 2 * s] : 0L);
	bp = scratch(vh, SK_PEN_BORD, sk_mid ? sk_mid[r->mid + 2 * s + 1] : 0L);

	l = r->l; t = r->t; rr = r->r; b = r->b;
	bw = r->bw;
	if (l + rr >= w)
	{
		l = (short) (w / 2);
		rr = (short) (w - l);
	}
	if (t + b >= h)
	{
		t = (short) (h / 2);
		b = (short) (h - t);
	}
	if (bw > l)  bw = l;
	if (bw > rr) bw = rr;
	dmw = (short) (w - l - rr);
	dmh = (short) (h - t - b);

	if (r->l == 0 && r->r == 0)		/* vertical pill: caps top and bottom */
	{
		copy(vh, sx, sy, r->w, t, x, y);
		copy(vh, sx, (short) (sy + r->h - r->b), r->w, b,
		     x, (short) (y + h - b));
		if (bw > 0)
		{
			bar(vh, x, (short) (y + t), bw, dmh, bp);
			bar(vh, (short) (x + w - bw), (short) (y + t), bw, dmh, bp);
		}
		bar(vh, (short) (x + bw), (short) (y + t), (short) (w - 2 * bw),
		    dmh, fp);
		return;
	}

	if (r->t == 0 && r->b == 0)		/* pill: two caps, bars between */
	{
		copy(vh, sx, sy, l, r->h, x, y);
		copy(vh, (short) (sx + r->w - r->r), sy, rr, r->h,
		     (short) (x + w - rr), y);
		if (bw > 0)
		{
			bar(vh, (short) (x + l), y, dmw, bw, bp);
			bar(vh, (short) (x + l), (short) (y + h - bw), dmw, bw, bp);
		}
		bar(vh, (short) (x + l), (short) (y + bw), dmw,
		    (short) (h - 2 * bw), fp);
		return;
	}

	/* corners, straight out of the sheet */
	copy(vh, sx, sy, l, t, x, y);
	copy(vh, (short) (sx + r->w - r->r), sy, rr, t, (short) (x + w - rr), y);
	copy(vh, sx, (short) (sy + r->h - r->b), l, b, x, (short) (y + h - b));
	copy(vh, (short) (sx + r->w - r->r), (short) (sy + r->h - r->b), rr, b,
	     (short) (x + w - rr), (short) (y + h - b));

	/* edges */
	if (bw > 0)
	{
		bar(vh, (short) (x + l), y, dmw, bw, bp);
		bar(vh, (short) (x + l), (short) (y + h - bw), dmw, bw, bp);
		bar(vh, x, (short) (y + t), bw, dmh, bp);
		bar(vh, (short) (x + w - bw), (short) (y + t), bw, dmh, bp);
	}
	bar(vh, (short) (x + l), (short) (y + bw), dmw, (short) (t - bw), fp);
	bar(vh, (short) (x + l), (short) (y + h - b), dmw, (short) (b - bw), fp);
	bar(vh, (short) (x + bw), (short) (y + t), (short) (l - bw), dmh, fp);
	bar(vh, (short) (x + w - rr), (short) (y + t), (short) (rr - bw), dmh, fp);

	/* middle */
	bar(vh, (short) (x + l), (short) (y + t), dmw, dmh, fp);
}

short apj_skin_has_tile(short g)
{
	if (g >= 0 && g < APJ_G_NTILE)
		return apj_skin_has(APJ_RG_TILE);
	if (g >= APJ_G_TILE2_FIRST && g < APJ_G_TILE2_FIRST + APJ_G_NTILE2)
		return apj_skin_has(APJ_RG_TILE2) &&
		       sk_reg[APJ_RG_TILE2].n >= APJ_G_NTILE2 * APJ_ST_N;
	if (g >= APJ_G_TILE3_FIRST && g < APJ_G_TILE3_FIRST + APJ_G_NTILE3)
		return apj_skin_has(APJ_RG_TILE3) &&
		       sk_reg[APJ_RG_TILE3].n >= APJ_G_NTILE3 * APJ_ST_N;
	return 0;
}

void apj_skin_tile(short vh, short g, short state, short x, short y)
{
	if (!apj_skin_has_tile(g))
		return;
	if (g < APJ_G_NTILE)
		apj_skin_blit(vh, APJ_RG_TILE, (short) (g * APJ_ST_N + state), x, y);
	else if (g < APJ_G_TILE3_FIRST)
		apj_skin_blit(vh, APJ_RG_TILE2,
		              (short) ((g - APJ_G_TILE2_FIRST) * APJ_ST_N + state), x, y);
	else
		apj_skin_blit(vh, APJ_RG_TILE3,
		              (short) ((g - APJ_G_TILE3_FIRST) * APJ_ST_N + state), x, y);
}

void apj_skin_tileacc(short vh, short g, short state, short x, short y)
{
	if (g < 0 || g >= APJ_G_NACC)
		return;
	apj_skin_blit(vh, APJ_RG_TILEACC, (short) (g * APJ_ST_N + state), x, y);
}

/* ------------------------------------------------- coverage blend path -- */
/*
 * The only place the 68k touches pixels. Reads the destination back,
 * blends the glyph's coverage towards the pen colour, writes it out.
 * gsz*gsz pixels - 784 at 1.75x - so it is fine for a glyph over the
 * video plane or a mark in a list row, and wrong for anything larger.
 */
void apj_skin_glyph(short vh, short g, short pen, short x, short y)
{
	MFDB buf, scr;
	short pxy[8], fdw, xx, yy, rgbc[3];
	long size;
	unsigned char ink[4];
	const unsigned char *cov;
	long crow;

	if (!sk_ok || !sk_mask || g < 0 || g >= sk_nglyph || sk_planes != 32)
		return;

	fdw = (short) ((sk_gsz + 15) & ~15);
	size = (long) fdw * sk_gsz * 4;
	if (size > sk_blendsz)
	{
		if (sk_blend)
			SK_FREE(sk_blend);
		sk_blend = SK_ALLOC(size);
		sk_blendsz = sk_blend ? size : 0;
		if (!sk_blend)
			return;
	}
	buf.fd_addr = sk_blend;
	buf.fd_w = fdw;
	buf.fd_h = sk_gsz;
	buf.fd_wdwidth = (short) (fdw >> 4);
	buf.fd_stand = 0;
	buf.fd_nplanes = sk_planes;
	buf.fd_r1 = buf.fd_r2 = buf.fd_r3 = 0;
	scr.fd_addr = NULL;

	pxy[0] = x; pxy[1] = y;
	pxy[2] = (short) (x + sk_gsz - 1); pxy[3] = (short) (y + sk_gsz - 1);
	pxy[4] = 0; pxy[5] = 0;
	pxy[6] = (short) (sk_gsz - 1); pxy[7] = (short) (sk_gsz - 1);
	vro_cpyfm(vh, S_ONLY, pxy, &scr, &buf);

	vq_color(vh, pen, 1, rgbc);
	ink[1] = (unsigned char) ((rgbc[0] * 255L + 500L) / 1000L);
	ink[2] = (unsigned char) ((rgbc[1] * 255L + 500L) / 1000L);
	ink[3] = (unsigned char) ((rgbc[2] * 255L + 500L) / 1000L);

	crow = (long) sk_maskw;
	cov = (const unsigned char *) sk_mask
	    + (long) (g / sk_gcols) * sk_gsz * crow
	    + (long) (g % sk_gcols) * sk_gsz;

	for (yy = 0; yy < sk_gsz; yy++)
	{
		unsigned char *p = (unsigned char *) sk_blend + (long) yy * fdw * 4;
		const unsigned char *c = cov + (long) yy * crow;

		for (xx = 0; xx < sk_gsz; xx++, p += 4)
		{
			unsigned short a = c[xx];

			if (!a)
				continue;
			if (a >= 255)
			{
				p[1] = ink[1]; p[2] = ink[2]; p[3] = ink[3];
				continue;
			}
			p[1] = (unsigned char) (p[1] + (((short) ink[1] - p[1]) * a >> 8));
			p[2] = (unsigned char) (p[2] + (((short) ink[2] - p[2]) * a >> 8));
			p[3] = (unsigned char) (p[3] + (((short) ink[3] - p[3]) * a >> 8));
		}
	}

	pxy[0] = 0; pxy[1] = 0;
	pxy[2] = (short) (sk_gsz - 1); pxy[3] = (short) (sk_gsz - 1);
	pxy[4] = x; pxy[5] = y;
	pxy[6] = (short) (x + sk_gsz - 1); pxy[7] = (short) (y + sk_gsz - 1);
	vro_cpyfm(vh, S_ONLY, pxy, &buf, &scr);
}

/* ------------------------------------------------------------- layout -- */

short apj_lay_hit(const APJ_LAY *t, short n, short mx, short my)
{
	short i;

	for (i = 0; i < n && t[i].id >= 0; i++)
		if (mx >= t[i].x && mx < t[i].x + t[i].w &&
		    my >= t[i].y && my < t[i].y + t[i].h)
			return t[i].id;
	return -1;
}

const APJ_LAY *apj_lay_find(const APJ_LAY *t, short n, short id)
{
	short i;

	for (i = 0; i < n && t[i].id >= 0; i++)
		if (t[i].id == id)
			return &t[i];
	return NULL;
}

/* --------------------------------------------------------------- load -- */

/*
 * What the screen is, and how to make one of its pixels. Exported
 * because the sheet is not the only thing that gets blitted: the
 * benchmark's 3D test builds its own frames and has to match, or
 * vro_cpyfm quietly does nothing (which is exactly what it did).
 */
short apj_skin_planes(void)
{
	return sk_planes;
}

long apj_skin_pack(long rgb)
{
	short r = (short) ((rgb >> 16) & 0xFF);
	short g = (short) ((rgb >> 8) & 0xFF);
	short b = (short) (rgb & 0xFF);

	if (sk_planes == 16)
		return (long) (((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
	return ((long) r << 16) | ((long) g << 8) | (long) b;	/* 00RRGGBB */
}

static short screen_planes(short vh)
{
	short ext[57];

	vq_extnd(vh, 1, ext);
	return ext[4];
}

static short screen_width(short vh)
{
	short wo[57];

	vq_extnd(vh, 0, wo);
	return (short) (wo[0] + 1);
}

/* 0 = from the screen width; 100/125/175 = what the app (or its user)
 * asked for. Set before apj_skin_load()/apj_skin_reload(). */
static short sk_prefer = 0;

void apj_skin_prefer(short scale)
{
	sk_prefer = (scale == 100 || scale == 125 || scale == 175) ? scale : 0;
}

short apj_skin_preferred(void)
{
	return sk_prefer;
}

static short pick_scale(short vh)
{
	short w = screen_width(vh);

	if (sk_prefer)
		return sk_prefer;
	if (w < 1024)
		return 100;
	if (w < 1600)
		return 125;
	return 175;
}

/* perceived brightness of an 0xRRGGBB, 0..255 */
static short lum(long c)
{
	return (short) ((((c >> 16) & 0xff) * 30L + ((c >> 8) & 0xff) * 59L +
	                  (c & 0xff) * 11L) / 100L);
}

/* the desktop's nineteen role colours, if a theme is loaded at all */
static short theme_rgb(long *rgb)
{
	return appl_control(-1, 115, rgb) == APJ_R_N ? 1 : 0;
}

static long find_skin(const char *stem, short scale, char *path, short note);

/*
 * "The skin follows the theme." XaAES cannot tell us the theme's NAME, but
 * opcode 115 gives us its nineteen colours, and every sheet carries the
 * nineteen it was baked with (PALT). So open each sheet of the family we
 * ship, read its palette, and take the one nearest the theme - a theme
 * built from a skin's own tokens (TeraDesk's Fluent presets are) matches
 * exactly, and any other theme still gets the skin closest to it. With no
 * theme at all, the dark one.
 */
static const char *const sk_family[] = { "FLTL", "FLTD", "GRPH", "FUJI" };

static const char *theme_skin(short scale)
{
	long rgb[APJ_R_N];
	long best = 0x7fffffffL;
	const char *pick = "FLTD";
	short i;

	if (!theme_rgb(rgb))
		return pick;

	for (i = 0; i < (short) (sizeof(sk_family) / sizeof(sk_family[0])); i++)
	{
		unsigned char head[HEAD_LEN], pb[APJ_R_N * 4];
		char path[256];
		long fh, d = 0;
		short r;

		fh = find_skin(sk_family[i], scale, path, 0);
		if (fh < 0)
			continue;
		if (Fread((short) fh, (long) HEAD_LEN, head) == HEAD_LEN &&
		    memcmp(head, "APJS", 4) == 0 &&
		    Fseek((long) be32(head + 26), (short) fh, 0) >= 0 &&
		    Fread((short) fh, (long) sizeof(pb), pb) == (long) sizeof(pb))
		{
			for (r = 0; r < APJ_R_N; r++)
			{
				long c = be32(pb + r * 4), t = rgb[r];
				long dr = ((c >> 16) & 0xff) - ((t >> 16) & 0xff);
				long dg = ((c >> 8) & 0xff) - ((t >> 8) & 0xff);
				long db = (c & 0xff) - (t & 0xff);

				d += (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
			}
			if (d < best)
			{
				best = d;
				pick = sk_family[i];
			}
		}
		Fclose((short) fh);
	}
	return pick;
}

/*
 * The folder the .PRG was started from. TeraDesk launches with the program
 * directory as the working directory, but only for entries set that way, and
 * an app started any other way would look in the wrong place - so ask the
 * shell where we came from rather than trusting the cwd.
 */
static void progdir(char *out, long n);

void apj_skin_progdir(char *out, long n)
{
	progdir(out, n);
}

static void progdir(char *out, long n)
{
	char cmd[160], tail[132], *bs;		/* cmd shorter than out: no truncation */

	out[0] = '\0';
	cmd[0] = '\0';
	shel_read(cmd, tail);
	if (!cmd[0])
		return;
	strncpy(out, cmd, (size_t) n - 1);
	out[n - 1] = '\0';
	bs = strrchr(out, '\\');
	if (!bs)
		bs = strrchr(out, '/');
	if (bs)
		bs[1] = '\0';
	else
		out[0] = '\0';
}

/* set by apj_skin_setdir(), searched before anything else */
static char sk_dir[160] = "";

/*
 * 1 = the app asked for one skin by name; 0 = follow the theme.
 * sk_pin holds what it asked for, IN FULL: sk_name is sixteen bytes and
 * holds a four-letter family stem, so a skin loaded by path had its path
 * truncated there and could never be reloaded. Apps load by stem or by
 * NULL, so that only ever bit the test harness - but reload has to work
 * for all three.
 */
static short sk_pinned = 0;
static char  sk_pin[256] = "";

void apj_skin_setdir(const char *dir)
{
	if (!dir || !*dir)
	{
		sk_dir[0] = '\0';
		return;
	}
	strncpy(sk_dir, dir, sizeof(sk_dir) - 2);
	sk_dir[sizeof(sk_dir) - 2] = '\0';
	/* a folder, with or without the trailing backslash */
	{
		long n = (long) strlen(sk_dir);

		if (n && sk_dir[n - 1] != '\\' && sk_dir[n - 1] != '/')
		{
			sk_dir[n] = '\\';
			sk_dir[n + 1] = '\0';
		}
	}
}

const char *apj_skin_dir(void)
{
	return sk_dir;
}

static long try_open(const char *dir, const char *file, char *out, short note)
{
	long h;
	long n = (long) strlen(sk_tried);

	sprintf(out, "%s%s", dir, file);
	h = Fopen(out, 0);
	if (note && h < 0 && n < (long) sizeof(sk_tried) - 80)
		sprintf(sk_tried + n, "%s%s", n ? "  " : "", dir[0] ? dir : ".\\");
	return h;
}

/*
 * <stem><scale>.SKN, looked for in:
 *
 *   1  the folder apj_skin_setdir() named, if any (an app's .INF)
 *   2  the program's own SKINS\ folder, and the program folder
 *   3  SKINS\ and . under the cwd
 *   4  C:\GEMSYS\SKINS\  - THE common home: every PiSTorm app looks
 *      here, whatever drive or folder it was started from
 *   5  S:\APJ-OS\NATFEATS\SKINS\  - where the tools used to keep them
 *      on the share; and C:\OPT\GEM\SKINS\
 *
 * 1 and 4/5 both exist because of accessories. A .PRG sits with the rest of
 * the tools, so 2 finds the sheets beside it; an .ACC is loaded from the
 * ROOT of the boot drive, so its progdir is C:\ and 2 finds nothing.
 *
 * note: record the folders tried in sk_tried for the "No skin" message.
 */
static long find_skin(const char *stem, short scale, char *path, short note)
{
	char pd[192], sub[224], file[24];
	long fh = -1;

	sprintf(file, "%s%d.SKN", stem, (int) scale);

	if (sk_dir[0])
		fh = try_open(sk_dir, file, path, note);

	if (fh < 0)
	{
		progdir(pd, (long) sizeof(pd));
		if (pd[0])
		{
			sprintf(sub, "%sSKINS\\", pd);
			fh = try_open(sub, file, path, note);
			if (fh < 0)
				fh = try_open(pd, file, path, note);
		}
	}
	if (fh < 0)
		fh = try_open("SKINS\\", file, path, note);
	if (fh < 0)
		fh = try_open("", file, path, note);
	if (fh < 0)
		fh = try_open("C:\\GEMSYS\\SKINS\\", file, path, note);
	if (fh < 0)
		fh = try_open("S:\\APJ-OS\\NATFEATS\\SKINS\\", file, path, note);
	if (fh < 0)
		fh = try_open("C:\\OPT\\GEM\\SKINS\\", file, path, note);
	return fh;
}

/*
 * Load the skin's whole palette into this workstation: the nineteen roles
 * at 237..255, where apjgui and XaAES also put them, and the six extras
 * just below at 230..235. Without a theme these are the only correct
 * colours the app has.
 */
static void put_pens(short vh)
{
	short i, rgbc[3];

	for (i = 0; i < APJ_X_N; i++)
	{
		long c = sk_pal[i];
		short pen = (i < APJ_R_N) ? (short) (APJ_PEN_BASE + i)
		                          : (short) (APJ_XPEN_BASE + (i - APJ_R_N));

		rgbc[0] = (short) ((((c >> 16) & 0xff) * 1000L + 127L) / 255L);
		rgbc[1] = (short) ((((c >> 8) & 0xff) * 1000L + 127L) / 255L);
		rgbc[2] = (short) (((c & 0xff) * 1000L + 127L) / 255L);
		vs_color(vh, pen, rgbc);
	}
}

void apj_skin_free(void)
{
	if (sk_mid)   { SK_FREE(sk_mid);   sk_mid = NULL; }
	if (sk_pix)   { SK_FREE(sk_pix);   sk_pix = NULL; }
	if (sk_mask)  { SK_FREE(sk_mask);  sk_mask = NULL; }
	if (sk_blend) { SK_FREE(sk_blend); sk_blend = NULL; sk_blendsz = 0; }
	sk_ok = 0;
}

short apj_skin_load(short vh, const char *name)
{
	unsigned char head[HEAD_LEN];
	unsigned char rb[REG_LEN * APJ_RG_N];
	unsigned char pb[PALT_N * 4];
	char path[256];
	const char *stem;
	long fh, off_regn, off_palt, off_mask, off_pixl, off_midc;
	long nmid;
	long npix, rowsrc, i;
	short nreg, pixfmt, mh, scale;
	char *rgbbuf;

	apj_skin_free();

	scale = pick_scale(vh);
	sk_pinned = name ? 1 : 0;
	if (name)
	{
		strncpy(sk_pin, name, sizeof(sk_pin) - 1);
		sk_pin[sizeof(sk_pin) - 1] = '\0';
	}
	else
		sk_pin[0] = '\0';
	stem = name ? name : theme_skin(scale);
	strncpy(sk_name, stem, sizeof(sk_name) - 1);
	sk_name[sizeof(sk_name) - 1] = '\0';

	/* a name that already looks like a file is opened as given, so an
	 * .INF can pin one skin and the host test can pass a path */
	if (strstr(stem, ".SKN") || strstr(stem, ".skn"))
	{
		strncpy(path, stem, sizeof(path) - 1);
		path[sizeof(path) - 1] = '\0';
		fh = Fopen(path, 0);
		scale = 0;
	}
	else
	{
		sprintf(sk_wanted, "%s%d.SKN", stem, (int) scale);
		sk_tried[0] = '\0';
		fh = find_skin(stem, scale, path, 1);
	}
	if (fh < 0)
		return 0;

	/*
	 * Version 2 added the seven PSCTRL regions at the end of the table,
	 * version 3 the PDFGEM tiles after those, version 4 the WEBGEM
	 * tiles. All of them load: the
	 * region count in the header is what says how many are there, and
	 * anything past it is marked absent rather than making the whole
	 * file unreadable. That is what lets a rebuilt MP3GEM keep running
	 * on a skin set that has not been rebuilt yet.
	 */
	if (Fread((short) fh, (long) HEAD_LEN, head) != HEAD_LEN ||
	    memcmp(head, "APJS", 4) != 0 ||
	    be16(head + 4) < 1 || be16(head + 4) > 4)
	{
		Fclose((short) fh);
		return 0;
	}

	sk_scale  = (short) be16(head + 6);
	sk_w      = (short) be16(head + 8);
	sk_h      = (short) be16(head + 10);
	nreg      = (short) be16(head + 12);
	sk_nglyph = (short) be16(head + 14);
	sk_gsz    = (short) be16(head + 16);
	sk_gcols  = (short) be16(head + 18);
	sk_maskw  = (short) be16(head + 20);
	off_regn  = (long) be32(head + 22);
	off_palt  = (long) be32(head + 26);
	off_mask  = (long) be32(head + 30);
	off_pixl  = (long) be32(head + 34);
	sk_tilew  = (short) be16(head + 38);
	sk_tileh  = (short) be16(head + 40);
	sk_accd   = (short) be16(head + 42);
	pixfmt    = (short) be16(head + 44);
	off_midc  = (long) be32(head + 48);

	if (nreg < 1 || nreg > APJ_RG_N || pixfmt != 0 || (sk_w & 15) != 0)
	{
		Fclose((short) fh);
		return 0;
	}
	/* regions this sheet does not have read back as zero-sized, and
	 * every draw call checks that */
	memset(sk_reg, 0, sizeof(sk_reg));

	Fseek(off_regn, (short) fh, 0);
	Fread((short) fh, (long) (REG_LEN * nreg), rb);
	for (i = 0; i < nreg; i++)
	{
		unsigned char *p = rb + i * REG_LEN;

		sk_reg[i].x     = (short) be16(p);
		sk_reg[i].y     = (short) be16(p + 2);
		sk_reg[i].w     = (short) be16(p + 4);
		sk_reg[i].h     = (short) be16(p + 6);
		sk_reg[i].n     = (short) be16(p + 8);
		sk_reg[i].cols  = (short) be16(p + 10);
		sk_reg[i].l     = p[12];
		sk_reg[i].t     = p[13];
		sk_reg[i].r     = p[14];
		sk_reg[i].b     = p[15];
		sk_reg[i].flags = (short) be16(p + 16);
		sk_reg[i].bw    = (short) be16(p + 18);
		if (sk_reg[i].cols < 1)
			sk_reg[i].cols = 1;
	}

	nmid = 0;
	for (i = 0; i < nreg; i++)
	{
		sk_reg[i].mid = (short) nmid;
		nmid += 2 * sk_reg[i].n;
	}
	sk_mid = (long *) SK_ALLOC(nmid * (long) sizeof(long));
	if (sk_mid)
	{
		unsigned char c[8];
		long k;

		Fseek(off_midc, (short) fh, 0);
		for (k = 0; k < nmid / 2; k++)
		{
			Fread((short) fh, 8L, c);
			sk_mid[k * 2]     = (long) be32(c);
			sk_mid[k * 2 + 1] = (long) be32(c + 4);
		}
	}

	Fseek(off_palt, (short) fh, 0);
	Fread((short) fh, (long) sizeof(pb), pb);
	for (i = 0; i < APJ_X_N; i++)
		sk_pal[i] = (long) be32(pb + i * 4);

	mh = (short) (((sk_nglyph + sk_gcols - 1) / sk_gcols) * sk_gsz);
	sk_mask = SK_ALLOC((long) sk_maskw * mh);
	if (sk_mask)
	{
		Fseek(off_mask, (short) fh, 0);
		Fread((short) fh, (long) sk_maskw * mh, sk_mask);
	}

	sk_planes = screen_planes(vh);
	npix = (long) sk_w * sk_h;
	rowsrc = (long) sk_w * 3;

	sk_pix = SK_ALLOC(npix * (sk_planes == 16 ? 2L : 4L));
	rgbbuf = SK_ALLOC(rowsrc);
	if (!sk_pix || !rgbbuf)
	{
		if (rgbbuf)
			SK_FREE(rgbbuf);
		Fclose((short) fh);
		apj_skin_free();
		return 0;
	}

	Fseek(off_pixl, (short) fh, 0);
	for (i = 0; i < sk_h; i++)
	{
		unsigned char *s = (unsigned char *) rgbbuf;
		long x;

		if (Fread((short) fh, rowsrc, rgbbuf) != rowsrc)
			break;
		if (sk_planes == 16)
		{
			unsigned short *d = (unsigned short *) sk_pix + i * (long) sk_w;

			for (x = 0; x < sk_w; x++, s += 3)
				d[x] = (unsigned short) (((s[0] & 0xf8) << 8) |
				                         ((s[1] & 0xfc) << 3) |
				                          (s[2] >> 3));
		}
		else
		{
			unsigned char *d = (unsigned char *) sk_pix + i * (long) sk_w * 4;

			for (x = 0; x < sk_w; x++, s += 3, d += 4)
			{
				d[0] = 0;
				d[1] = s[0];
				d[2] = s[1];
				d[3] = s[2];
			}
		}
	}
	SK_FREE(rgbbuf);
	Fclose((short) fh);

	sk_mfdb.fd_addr    = sk_pix;
	sk_mfdb.fd_w       = sk_w;
	sk_mfdb.fd_h       = sk_h;
	sk_mfdb.fd_wdwidth = (short) (sk_w >> 4);
	sk_mfdb.fd_stand   = 0;
	sk_mfdb.fd_nplanes = sk_planes;
	sk_mfdb.fd_r1 = sk_mfdb.fd_r2 = sk_mfdb.fd_r3 = 0;

	sk_ok = 1;
	put_pens(vh);

	/*
	 * XaAES's antialiased text is drawn on the AES's own workstation with
	 * the AES's own pens; ours are invisible to it. Fine while the theme
	 * and the skin agree, unreadable when they do not - a dark skin under a
	 * light theme paints the theme's dark text on the skin's dark panel.
	 * When they disagree apj_skin_text() gives up the antialiasing and
	 * draws in the skin's own colours, which is the lesser loss.
	 */
	{
		long rgb[APJ_R_N];

		sk_aa = theme_rgb(rgb) &&
		        ((lum(rgb[APJ_R_PANEL]) >= 128) ==
		         (lum(sk_pal[APJ_R_PANEL]) >= 128));
	}
	return 1;
}

/*
 * Reload after the desktop changed theme.
 *
 * This used to keep sk_name and reload THE SAME SHEET, which is not a
 * reload at all: sk_name holds the stem the last load resolved to, so
 * following the theme resolved "the theme" exactly once, at startup, and
 * every APJ_SKINCHG after that re-read the sheet it already had. Only the
 * pens changed. Switch the desktop from Fluent Dark to Fuji and the app
 * stayed dark.
 *
 * A load with an explicit name is a different thing - an app pinning one
 * skin, or the test harness passing a path - and that one does have to
 * come back to the same sheet. Hence the flag rather than a bare NULL.
 */
short apj_skin_reload(short vh)
{
	char keep[256];

	if (!sk_pinned)
		return apj_skin_load(vh, NULL);
	strcpy(keep, sk_pin);		/* load() overwrites sk_pin - copy first */
	return apj_skin_load(vh, keep[0] ? keep : NULL);
}
