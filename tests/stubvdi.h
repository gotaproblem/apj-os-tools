/*
 * tests/stubvdi.h - the fake VDI both harnesses run against.
 *
 * Not an emulator: it implements exactly the calls APJSKIN makes -
 * vro_cpyfm(S_ONLY) between an MFDB and a fake 32 bpp screen, v_bar, the
 * colour registers, the APJ*.FNT cell ladder - so that the parsing, the
 * source rectangles and the nine-slice arithmetic are exercised for real,
 * and every blit is checked against the rules that make fVDI turn it into
 * a host-side memcpy. Text is a no-op: XaAES draws that.
 *
 * Header-only, and included by exactly one translation unit per harness
 * (the same shape as apjgui.h). It was lifted out of tests/skin/harness.c
 * unchanged when PSCTRL needed the same thing.
 *
 * The including file must define SCR_W/SCR_H before including, or take
 * the 1920x1080 default, and must provide main().
 */

#ifndef STUBVDI_H
#define STUBVDI_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gem.h"
#include "apjgui.h"
#include "apjskin.h"

short gl_apid = 1;

/* -------------------------------------------------------- fake screen -- */
#define SCR_W 1920
#define SCR_H 1080
static unsigned char scr[(long) SCR_W * SCR_H * 4];
static short clip[4] = { 0, 0, SCR_W - 1, SCR_H - 1 };
static short clip_on = 0;
static short pal[256][3];
static short fill_pen = 1;

static long blits = 0, pixels = 0;
static int  fails = 0;

static void fail(const char *what, long a, long b)
{
	fprintf(stderr, "FAIL %s (%ld, %ld)\n", what, a, b);
	fails++;
}

static void put(short x, short y, const unsigned char *px)
{
	unsigned char *d;

	if (x < 0 || y < 0 || x >= SCR_W || y >= SCR_H)
	{
		fail("write off screen", x, y);
		return;
	}
	if (clip_on && (x < clip[0] || y < clip[1] || x > clip[2] || y > clip[3]))
		return;
	d = scr + ((long) y * SCR_W + x) * 4;
	d[0] = px[0]; d[1] = px[1]; d[2] = px[2]; d[3] = px[3];
}

void vro_cpyfm(short h, short mode, short *pxy, MFDB *src, MFDB *dst)
{
	short sw = (short) (pxy[2] - pxy[0] + 1);
	short sh = (short) (pxy[3] - pxy[1] + 1);
	short dw = (short) (pxy[6] - pxy[4] + 1);
	short dh = (short) (pxy[7] - pxy[5] + 1);
	short x, y;
	(void) h;

	if (mode != S_ONLY)
		fail("raster op is not S_ONLY", mode, 0);
	if (sw != dw || sh != dh)
		fail("scaling blit - fVDI would not memcpy that", sw, dw);
	if (sw <= 0 || sh <= 0)
		return;

	blits++;
	pixels += (long) sw * sh;

	if (src->fd_addr)
	{
		if (src->fd_stand != 0)
			fail("source MFDB not in device format", src->fd_stand, 0);
		if (src->fd_w & 15)
			fail("source MFDB width not a multiple of 16", src->fd_w, 0);
		/*
		 * A raster copy cannot change the pixel format. An MFDB whose
		 * plane count does not match the destination's is not a slow
		 * blit, it is a blit that does NOTHING - which is how the
		 * benchmark's 3D view came out blank on hardware while every
		 * check here passed. The stub screen is 32 bpp; a source that
		 * disagrees would have been invisible on the Atari too.
		 */
		if (src->fd_nplanes != (dst->fd_addr ? dst->fd_nplanes : 32))
			fail("source and destination planes differ - the VDI will "
			     "not convert", src->fd_nplanes,
			     dst->fd_addr ? dst->fd_nplanes : 32);
		if (pxy[0] < 0 || pxy[1] < 0 ||
		    pxy[2] >= src->fd_w || pxy[3] >= src->fd_h)
			fail("read outside the sheet", pxy[2], src->fd_w);
	}

	for (y = 0; y < sh; y++)
		for (x = 0; x < sw; x++)
		{
			unsigned char px[4];
			short sx = (short) (pxy[0] + x), sy = (short) (pxy[1] + y);
			short tx = (short) (pxy[4] + x), ty = (short) (pxy[5] + y);

			if (src->fd_addr)
			{
				const unsigned char *s = (const unsigned char *) src->fd_addr
				    + ((long) sy * src->fd_w + sx) * 4;
				px[0] = s[0]; px[1] = s[1]; px[2] = s[2]; px[3] = s[3];
			}
			else
			{
				const unsigned char *s;

				if (sx < 0 || sy < 0 || sx >= SCR_W || sy >= SCR_H)
					continue;
				s = scr + ((long) sy * SCR_W + sx) * 4;
				px[0] = s[0]; px[1] = s[1]; px[2] = s[2]; px[3] = s[3];
			}

			if (dst->fd_addr)
			{
				unsigned char *d = (unsigned char *) dst->fd_addr
				    + ((long) ty * dst->fd_w + tx) * 4;
				d[0] = px[0]; d[1] = px[1]; d[2] = px[2]; d[3] = px[3];
			}
			else
				put(tx, ty, px);
		}
}

/* ------------------------------------------------------------ the VDI -- */
void vq_extnd(short h, short flag, short *out)
{
	(void) h;
	memset(out, 0, 57 * sizeof(short));
	if (flag == 0)
	{
		out[0] = SCR_W - 1;
		out[1] = SCR_H - 1;
	}
	else
		out[4] = 32;
}

void vs_color(short h, short i, short *rgb)
{
	(void) h;
	if (i >= 0 && i < 256)
	{
		pal[i][0] = rgb[0]; pal[i][1] = rgb[1]; pal[i][2] = rgb[2];
	}
}

void vq_color(short h, short i, short flag, short *rgb)
{
	(void) h; (void) flag;
	rgb[0] = pal[i & 255][0];
	rgb[1] = pal[i & 255][1];
	rgb[2] = pal[i & 255][2];
}

void vs_clip(short h, short on, short *xy)
{
	(void) h;
	clip_on = on;
	if (on)
		memcpy(clip, xy, 4 * sizeof(short));
}

void vswr_mode(short h, short m)      { (void) h; (void) m; }
void vsf_interior(short h, short s)   { (void) h; (void) s; }
void vsf_perimeter(short h, short s)  { (void) h; (void) s; }
void vsf_color(short h, short c)      { (void) h; fill_pen = c; }
void vsl_color(short h, short c)      { (void) h; fill_pen = c; }
void vst_color(short h, short c)      { (void) h; (void) c; }
void v_pline(short h, short n, short *xy) { (void) h; (void) n; (void) xy; }
/* the APJ*.FNT set fVDI loads: point size -> cell */
static short cur_cw = 10, cur_ch = 20;

/*
 * XaAES draws the text, so nothing lands in the framebuffer here and the
 * pixel checks can never see it. Record the extent instead: a string that
 * runs out of its panel is invisible to a blit check but is exactly the
 * kind of bug that reaches a real screen, so the harness gets a list of
 * text rectangles to test against.
 */
#define TXT_MAX	512
static struct { short x, y, w, h; char s[96]; } txt[TXT_MAX];
static short ntxt = 0;

#define txt_reset()	(ntxt = 0)

void v_gtext(short h, short x, short y, char *s)
{
	(void) h;
	if (ntxt >= TXT_MAX)
		return;
	txt[ntxt].x = x;
	txt[ntxt].y = y;
	txt[ntxt].w = (short) (cur_cw * (short) strlen(s));
	txt[ntxt].h = cur_ch;
	strncpy(txt[ntxt].s, s, sizeof(txt[0].s) - 1);
	txt[ntxt].s[sizeof(txt[0].s) - 1] = '\0';
	ntxt++;
}

void v_bar(short h, short *xy)
{
	short x, y;
	unsigned char px[4];
	(void) h;

	px[0] = 0;
	px[1] = (unsigned char) ((pal[fill_pen & 255][0] * 255L + 500L) / 1000L);
	px[2] = (unsigned char) ((pal[fill_pen & 255][1] * 255L + 500L) / 1000L);
	px[3] = (unsigned char) ((pal[fill_pen & 255][2] * 255L + 500L) / 1000L);
	for (y = xy[1]; y <= xy[3]; y++)
		for (x = xy[0]; x <= xy[2]; x++)
			if (x >= 0 && y >= 0 && x < SCR_W && y < SCR_H)
				put(x, y, px);
}

short vst_point(short h, short pt, short *cw, short *ch, short *bw, short *bh)
{
	/* The APJ*.FNT set fVDI loads starts at 10pt. It was missing here,
	 * so a 10pt request - which is what F_SMALL asks for at 100% - fell
	 * through to the 12pt cell and made every small string two pixels
	 * taller than the real one. */
	static const short tbl[6][3] = { {10,8,16},{11,9,18},{12,10,20},
	                                 {13,11,22},{15,12,24},{20,16,32} };
	int i, best = 0;
	(void) h;
	for (i = 0; i < 6; i++)
		if (tbl[i][0] <= pt)
			best = i;
	cur_cw = tbl[best][1];
	cur_ch = tbl[best][2];
	*cw = *bw = cur_cw;
	*ch = *bh = cur_ch;
	return tbl[best][0];
}

void vqt_attributes(short h, short *a)
{
	(void) h;
	memset(a, 0, 10 * sizeof(short));
	a[8] = cur_cw;
	a[9] = cur_ch;
}

/*
 * appl_control: pretend to be XaAES with the APJ renderer, and hand back
 * the skin's own role colours (opcode 115) so apj_pen() lines up with the
 * pens the sheet was baked against.
 */
static long theme_pal[APJ_R_N];		/* "the desktop's theme": the sheet under test */
static short theme_set = 0;

/* Pretend the desktop committed a new theme: take the nineteen roles of
 * whatever sheet is loaded RIGHT NOW and make them the answer to opcode
 * 115 from here on. Lets a harness switch themes the way TeraDesk does. */
/* used by tests/skin; not every harness needs it */
static void theme_take_from_loaded(void);
static void theme_take_from_loaded(void)
{
	short i;

	for (i = 0; i < APJ_R_N; i++)
		theme_pal[i] = apj_skin_rgb(i);
	theme_set = 1;
}

long appl_control(short ap, short what, void *p)
{
	long *rgb = (long *) p;
	short i;
	(void) ap;

	if (what == 110)
		return 1;
	if (what == 115)
	{
		if (!theme_set && apj_skin_ok())
		{
			for (i = 0; i < APJ_R_N; i++)
				theme_pal[i] = apj_skin_rgb(i);
			theme_set = 1;
		}
		if (!theme_set)
			return 0;
		for (i = 0; i < APJ_R_N; i++)
			rgb[i] = theme_pal[i];
		return APJ_R_N;
	}
	return 0;
}

/* ------------------------------------------------------------ GEMDOS --- */
static FILE *fp[8];

long Fopen(const char *path, short mode)
{
	int i;
	(void) mode;
	for (i = 1; i < 8; i++)
		if (!fp[i])
		{
			fp[i] = fopen(path, "rb");
			return fp[i] ? i : -33L;
		}
	return -35L;
}
long Fread(short h, long n, void *buf)  { return (long) fread(buf, 1, (size_t) n, fp[h]); }
long Fseek(long off, short h, short m)  { return fseek(fp[h], off, m == 0 ? SEEK_SET : SEEK_CUR); }
long Fclose(short h)                    { fclose(fp[h]); fp[h] = NULL; return 0; }
short Dgetdrv(void)                     { return 2; }

/* the harness runs from tests/skin, so point the engine's program-directory
 * search at the built sheets */
short shel_read(char *cmd, char *tail)
{
    strcpy(cmd, "../../skins/out/HARNESS.PRG");
    tail[0] = 0;
    return 1;
}


#endif /* STUBVDI_H */
