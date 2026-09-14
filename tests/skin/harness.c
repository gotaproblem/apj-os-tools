/*
 * tests/skin/harness.c - build APJSKIN and MP3GEM's drawing code on the
 * host, run them against a fake 32 bpp screen, and check every blit.
 *
 * The fake VDI lives in ../stubvdi.h, shared with tests/psctrl. Text is a
 * no-op: XaAES draws that, and skins/preview.py is the reference for how
 * the finished window should look.
 *
 *   ./harness <file.SKN> <out.ppm>      -> 0 ok, 1 a check failed
 */
#define APJGUI_IMPL

#include "../stubvdi.h"
#include "../../mp3gem/mp3ui.h"

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

	/*
	 * The skin follows the theme. Opcode 115 above answers with THIS
	 * sheet's palette, so a load with no name - the app's normal start -
	 * must come back to this same sheet out of the whole family, at the
	 * scale the fake screen wants.
	 */
	{
		const char *base = strrchr(argv[1], '/');
		char want[16];

		base = base ? base + 1 : argv[1];
		sprintf(want, "%.4s%d.SKN", base, (int) apj_skin_scale());
		(void) appl_control(-1, 115, theme_pal);	/* latch the palette before the free */
		if (!apj_skin_load(vh, NULL))
			fail("follow-the-theme reload failed", 0, 0);
		else if (strncmp(apj_skin_wanted(), want, 4) != 0)
		{
			fprintf(stderr, "theme %s picked %s\n", want, apj_skin_wanted());
			fail("skin did not follow the theme", 0, 0);
		}
	}

	/*
	 * A theme CHANGE must move the skin, not just the pens.
	 *
	 * apj_skin_reload() used to keep the stem the previous load had
	 * resolved to and re-read that same sheet, so "follow the theme"
	 * happened exactly once, at startup: switch the desktop from Fluent
	 * Dark to Fuji and the app stayed dark, with only its nineteen pens
	 * changing under it. Found on hardware, so it gets a test.
	 *
	 * Load a sibling family, make ITS palette the desktop theme, then
	 * reload the first one and require that the reload lands on the
	 * sibling.
	 */
	{
		static const char *const fam[] = { "FLTL", "FLTD", "GRPH", "FUJI" };
		char sib[256], here[256], *slash;
		const char *base;
		int k;

		strcpy(here, argv[1]);
		slash = strrchr(here, '/');
		base = slash ? slash + 1 : here;

		sib[0] = '\0';
		for (k = 0; k < 4; k++)
			if (strncmp(base, fam[k], 4) != 0)
			{
				sprintf(sib, "%.*s%s%s", (int) (base - here), here,
				        fam[k], base + 4);
				break;
			}

		if (sib[0] && apj_skin_load(vh, sib))
		{
			char wantfam[8];

			strncpy(wantfam, strrchr(sib, '/') ? strrchr(sib, '/') + 1 : sib, 4);
			wantfam[4] = '\0';
			theme_take_from_loaded();	/* the desktop is now that theme */

			/* back to the sheet under test, by name: pinned */
			if (!apj_skin_load(vh, argv[1]))
				fail("could not reload the sheet under test", 0, 0);
			else if (!apj_skin_reload(vh))
				fail("reload of a pinned skin failed", 0, 0);
			else if (strncmp(apj_skin_wanted(), base, 4) != 0)
				fail("a PINNED skin followed the theme - it must not", 0, 0);

			/* now the same thing following the theme: must move */
			if (!apj_skin_load(vh, NULL))
				fail("theme-following load failed", 0, 0);
			else if (strncmp(apj_skin_wanted(), wantfam, 4) != 0)
			{
				fprintf(stderr, "theme is %s but the skin is %s\n",
				        wantfam, apj_skin_wanted());
				fail("load did not follow the changed theme", 0, 0);
			}
			else if (!apj_skin_reload(vh) ||
			         strncmp(apj_skin_wanted(), wantfam, 4) != 0)
				fail("reload did not follow the changed theme", 0, 0);
		}
	}

	apj_skin_free();
	printf(fails ? "%d CHECK(S) FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
