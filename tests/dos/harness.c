/*
 * tests/dos/harness.c - build DOSGEM's drawing (dosgem/dosui.c) on the
 * host, run it against the fake 32 bpp screen in ../stubvdi.h and a fake
 * DOS screen buffer, and check what the apjskin-gem-app skill asks of
 * every app:
 *
 *   - every blit S_ONLY, 1:1, from a device-format MFDB a multiple of 16
 *     wide, never reading outside the buffer (stubvdi.h checks these);
 *   - a frame costs ONE blit, and a damage band costs one blit of exactly
 *     the band - the property that keeps a DOS game's 70 fps cheap;
 *   - the view carries the buffer's pixels edge to edge;
 *   - nothing is written outside the window, in any mode, at any size;
 *   - every message line stays inside the window, including the longest
 *     status the link can produce in the smallest window.
 *
 *   ./harness            -> 0 ok, 1 a check failed
 */
#define APJGUI_IMPL

#include "../stubvdi.h"
#include "../../dosgem/dosui.h"

static void sentinel(void)
{
	long i;

	for (i = 0; i < (long) SCR_W * SCR_H; i++)
	{
		scr[i * 4 + 0] = 0x5A; scr[i * 4 + 1] = 0xA5;
		scr[i * 4 + 2] = 0x5A; scr[i * 4 + 3] = 0xA5;
	}
}

/* nothing outside the window may have changed */
static void check_outside(const DOSUI *u, const char *when)
{
	long x, y;

	for (y = 0; y < SCR_H; y += 2)
		for (x = 0; x < SCR_W; x += 2)
		{
			const unsigned char *p = scr + (y * SCR_W + x) * 4;

			if (x >= u->work.g_x && y >= u->work.g_y &&
			    x < u->work.g_x + u->work.g_w && y < u->work.g_y + u->work.g_h)
				continue;
			if (p[0] != 0x5A || p[1] != 0xA5 || p[2] != 0x5A || p[3] != 0xA5)
			{
				fprintf(stderr, "%s: pixel %ld,%ld outside the window changed\n", when, x, y);
				fail("write outside the window", x, y);
				return;
			}
		}
}

static void check_text(const DOSUI *u, const char *when)
{
	short i;

	for (i = 0; i < ntxt; i++)
		if (txt[i].x < u->work.g_x || txt[i].y < u->work.g_y ||
		    txt[i].x + txt[i].w > u->work.g_x + u->work.g_w ||
		    txt[i].y + txt[i].h > u->work.g_y + u->work.g_h)
		{
			fprintf(stderr, "%s: \"%s\" at %d,%d %dx%d leaves the %dx%d window\n", when,
			        txt[i].s, txt[i].x, txt[i].y, txt[i].w, txt[i].h, u->work.g_w, u->work.g_h);
			fail("text outside the window", txt[i].x, txt[i].y);
		}
}

static unsigned char *give_buffer(DOSUI *u)
{
	long i, n;
	unsigned char *b;

	u->buf.fd_w = (short) ((u->work.g_w + 15) & ~15);
	u->buf.fd_h = u->work.g_h;
	u->buf.fd_wdwidth = (short) (u->buf.fd_w / 16);
	u->buf.fd_stand = 0;
	u->buf.fd_nplanes = 32;
	n = (long) u->buf.fd_w * u->buf.fd_h;
	b = (unsigned char *) malloc((size_t) n * 4);
	for (i = 0; i < n; i++)
	{
		b[i * 4 + 0] = 0;
		b[i * 4 + 1] = (unsigned char) (i & 255);
		b[i * 4 + 2] = (unsigned char) ((i >> 8) & 255);
		b[i * 4 + 3] = (unsigned char) (100 + (i % 97));
	}
	u->buf.fd_addr = b;
	return b;
}

static void check_view(const DOSUI *u, const unsigned char *b, const char *when)
{
	long x, y;

	for (y = 0; y < u->work.g_h; y += 3)
		for (x = 0; x < u->work.g_w; x += 3)
		{
			const unsigned char *s = scr + ((u->work.g_y + y) * (long) SCR_W + u->work.g_x + x) * 4;
			const unsigned char *w = b + (y * (long) u->buf.fd_w + x) * 4;

			if (s[1] != w[1] || s[2] != w[2] || s[3] != w[3])
			{
				fprintf(stderr, "%s: view %ld,%ld is not the buffer's\n", when, x, y);
				fail("view pixel is not the buffer's", x, y);
				return;
			}
		}
}

static void one_size(short x, short y, short w, short h)
{
	DOSUI u;
	unsigned char *b;
	char when[64];
	static const char *longwait =
	    "cannot connect to psdos's socket: /run/psdos/psdos.sock (Connection refused) "
	    "- and a great deal more text than any window could ever carry on one line";
	short m;
	GRECT band;

	sprintf(when, "%dx%d at %d,%d", w, h, x, y);
	memset(&u, 0, sizeof u);
	dosui_layout(&u, x, y, w, h);

	/* every message, nothing outside, text inside */
	for (m = DOS_MSG_WAIT; m <= DOS_MSG_EXITED; m++)
	{
		blits = 0;
		sentinel();
		txt_reset();
		u.msg = m;
		u.waittext = (m == DOS_MSG_WAIT) ? longwait : NULL;
		dosui_draw(&u, 1);
		check_outside(&u, when);
		check_text(&u, when);
		if (blits)
			fail("a message draws a blit", blits, 0);
		blits = 0;
	}

	/* a frame: one blit, the whole view, pixel for pixel */
	b = give_buffer(&u);
	u.msg = DOS_MSG_NONE;
	sentinel();
	blits = pixels = 0;
	txt_reset();
	dosui_draw(&u, 1);
	if (blits != 1)
		fail("a frame is not one blit", blits, 1);
	if (pixels != (long) w * h)
		fail("a frame does not cover the view exactly", pixels, (long) w * h);
	if (ntxt)
		fail("text drawn over the DOS screen", ntxt, 0);
	check_view(&u, b, when);
	check_outside(&u, when);

	/* a damage band: one blit of exactly the band */
	{
		short by = (short) (h / 3), bh = 2;

		dosui_band_rect(&u, by, bh, &band);
		u.clip = band;
		blits = pixels = 0;
		dosui_draw(&u, 1);
		u.clip = u.work;
		if (blits != 1 || pixels != (long) w * bh)
			fail("a two-row band is not one two-row blit", pixels, (long) w * bh);
	}
	/* a band that runs past the bottom is cut at the view */
	dosui_band_rect(&u, (short) (h - 5), 40, &band);
	if (band.g_y + band.g_h > u.work.g_y + u.work.g_h || band.g_h != 5)
		fail("band past the bottom not cut", band.g_h, 5);
	dosui_band_rect(&u, -3, 10, &band);
	if (band.g_y != u.work.g_y || band.g_h != 7)
		fail("band above the top not cut", band.g_h, 7);

	/* a clip partly outside the window: still nothing outside */
	sentinel();
	u.clip.g_x = (short) (x - 50); u.clip.g_y = (short) (y - 50);
	u.clip.g_w = (short) (w + 100); u.clip.g_h = (short) (h + 100);
	dosui_draw(&u, 1);
	u.clip = u.work;
	check_outside(&u, when);

	free(b);
}

int main(void)
{
	one_size(16, 40, 1280, 960);    /* the natural 2x window at 1080p   */
	one_size(100, 60, 1277, 957);   /* not a multiple of 16 wide        */
	one_size(0, 0, 200, 160);       /* the smallest DOSGEM allows       */
	one_size(0, 30, 1920, 1050);    /* fulled                           */
	one_size(1700, 900, 220, 180);  /* in a corner                      */
	printf("tests/dos: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
	return fails ? 1 : 0;
}
