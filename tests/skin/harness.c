/*
 * tests/skin/harness.c - build APJSKIN and MP3GEM's drawing code on the
 * host, run them against a fake 32 bpp screen, and check every blit.
 *
 * This is not an emulator. It implements exactly the VDI calls the skin
 * engine makes (vro_cpyfm S_ONLY between an MFDB and the screen, v_bar,
 * the colour registers) so that the parsing, the source rectangles and
 * the nine-slice arithmetic are exercised for real. Text is a no-op:
 * XaAES draws that, and skins/preview.py is the reference for how the
 * finished window should look.
 *
 *   ./harness <file.SKN> <out.ppm>      -> 0 ok, 1 a check failed
 */
#define APJGUI_IMPL


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stub/gem.h"
#include "../../apjgui/apjgui.h"
#include "../../apjgui/apjskin.h"
#include "../../mp3gem/mp3ui.h"

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
void v_gtext(short h, short x, short y, char *s)
{
	(void) h; (void) x; (void) y; (void) s;   /* XaAES draws text */
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

/* the APJ*.FNT set fVDI loads: point size -> cell */
static short cur_cw = 10, cur_ch = 20;

short vst_point(short h, short pt, short *cw, short *ch, short *bw, short *bh)
{
	static const short tbl[5][3] = { {11,9,18},{12,10,20},{13,11,22},
	                                 {15,12,24},{20,16,32} };
	int i, best = 1;
	(void) h;
	for (i = 0; i < 5; i++)
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
long appl_control(short ap, short what, void *p)
{
	long *rgb = (long *) p;
	short i;
	(void) ap;

	if (what == 110)
		return 1;
	if (what == 115 && apj_skin_ok())
	{
		for (i = 0; i < APJ_R_N; i++)
			rgb[i] = apj_skin_rgb(i);
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

/* --------------------------------------------------------------- main -- */
static const char *NAMES[] = {
	"Main Titles", "Tears in Rain", "Love Theme", "Blade Runner Blues",
	"Memories of Green", "Rachel's Song", "End Titles"
};
static const long LENS[] = { 222, 287, 296, 532, 344, 287, 281 };

static const char *name_of(void *c, short i) { (void) c; return NAMES[i % 7]; }
static long        len_of (void *c, short i) { (void) c; return LENS[i % 7]; }

int main(int argc, char **argv)
{
	MP3UI u;
	short vh = 1, W, H, mw, mh;
	long i;
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

	printf("scale %d%%   tile %dx%d   acc %d   glyph %d\n",
	       apj_skin_scale(), apj_skin_tilew(), apj_skin_tileh(),
	       apj_skin_accd(), apj_skin_glyphsz());

	mp3ui_minsize(&mw, &mh);
	printf("minimum window %dx%d\n", mw, mh);

	W = apj_skin_m(480);
	H = apj_skin_m(320);
	if (W < mw || H < mh)
		fail("480x320pt is under the minimum", W, mw);

	/* paint the fake screen mid-grey so anything unpainted stands out */
	for (i = 0; i < (long) SCR_W * SCR_H; i++)
	{
		scr[i*4+0] = 0; scr[i*4+1] = 0xff; scr[i*4+2] = 0x00; scr[i*4+3] = 0xff;
	}

	memset(&u, 0, sizeof(u));
	u.title = "Tears in Rain";
	u.sub = "Vangelis - Blade Runner (OST)";
	u.codec = "MPEG-1 LAYER III";
	u.bitrate = 320;
	u.playing = 1;
	u.paused = 0;
	u.pos_s = 107;
	u.len_s = 287;
	u.vol = 72;
	u.hasvol = 1;
	u.repeat = 1;
	u.ntracks = 24;
	u.sel = 1;
	u.top = 0;
	u.name_of = name_of;
	u.len_of = len_of;
	u.dir = "S:\\MUSIC\\VANGELIS";
	u.hover = W_SHUFFLE;
	u.press = -1;

	mp3ui_layout(&u, vh, 40, 40, W, H);
	mp3ui_draw(&u, vh);

	printf("%ld blits, %ld pixels moved\n", blits, pixels);
	if (blits > 400)
		fail("too many blits for one redraw", blits, 400);

	/* a WM_REDRAW for a thin strip - what a window dragged across us
	 * sends, many times - must not cost a whole redraw */
	{
		GRECT strip;
		long full = blits;

		strip.g_x = 40; strip.g_y = (short) (40 + H / 2); strip.g_w = W; strip.g_h = 6;
		blits = 0;
		mp3ui_draw_clip(&u, vh, &strip);
		printf("6 px strip redraw: %ld blits (full was %ld)\n", blits, full);
		if (blits > full / 3)
			fail("strip redraw not much cheaper than a full one", blits, full);
	}

	/* hit-testing must agree with what was drawn */
	{
		const APJ_LAY *l = apj_lay_find(u.lay, u.nlay, W_PLAY);

		if (!l)
			fail("no play button in the layout", 0, 0);
		else if (mp3ui_hit(&u, (short) (l->x + 2), (short) (l->y + 2)) != W_PLAY)
			fail("hit test missed the play button", l->x, l->y);
		if (mp3ui_hit(&u, 5, 5) != -1)
			fail("hit test found a widget outside the window", 0, 0);
	}

	o = fopen(argv[2], "wb");
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

	apj_skin_free();
	printf(fails ? "%d CHECK(S) FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
