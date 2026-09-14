/*
 * tests/vid/harness.c - build APJSKIN and VIDGEM's drawing code on the
 * host, run them against a fake 32 bpp screen, and check every blit, every
 * text extent, and the cost of each partial repaint.
 *
 * The fake VDI is ../stubvdi.h. Text is recorded, not drawn: XaAES draws
 * that on the real machine, so the check here is that no string leaves
 * its panel.
 *
 *   ./harness <file.SKN> <out.ppm>      -> 0 ok, 1 a check failed
 */
#define APJGUI_IMPL

#include "../stubvdi.h"
#include "../../vidgem/vidui.h"

static const char *NAMES[] = {
	"Blade Runner (1982) Final Cut 1080p.mkv", "Alien.mp4",
	"A very long file name that came off a NAS and goes on for ever and ever.mp4",
	"Clip 4.mov", "Clip 5.avi", "Holiday 2024.mp4", "Trailer.webm"
};

static const char *name_of(void *c, short i) { (void) c; return NAMES[i % 7]; }

static int inside(short x, short y, short w, short h,
                  short X, short Y, short W, short H)
{
	return x >= X && y >= Y && x + w <= X + W && y + h <= Y + H;
}

/* every recorded string inside the window; those in the pane inside the pane */
static void check_text(VIDUI *u, const char *when)
{
	short i;
	GRECT p;

	vidui_pane_rect(u, &p);
	for (i = 0; i < ntxt; i++)
	{
		if (!inside(txt[i].x, txt[i].y, txt[i].w, txt[i].h,
		            u->work.g_x, u->work.g_y, u->work.g_w, u->work.g_h))
		{
			fprintf(stderr, "%s: \"%s\" at %d,%d %dx%d leaves the window\n",
			        when, txt[i].s, txt[i].x, txt[i].y, txt[i].w, txt[i].h);
			fail("text outside the window", txt[i].x, txt[i].y);
		}
		/* a string that starts in the pane must end in it */
		if (txt[i].y >= p.g_y && txt[i].y < p.g_y + p.g_h &&
		    !inside(txt[i].x, txt[i].y, txt[i].w, txt[i].h,
		            p.g_x, p.g_y, p.g_w, p.g_h))
		{
			fprintf(stderr, "%s: \"%s\" leaves the pane\n", when, txt[i].s);
			fail("text outside the pane", txt[i].x, txt[i].y);
		}
	}
}

/* the pane must be black edge to edge (the overlay's letterbox) */
static void check_pane_black(VIDUI *u)
{
	GRECT p;
	short x, y;

	vidui_pane_rect(u, &p);
	for (y = p.g_y; y < p.g_y + p.g_h; y += 7)
		for (x = p.g_x; x < p.g_x + p.g_w; x += 7)
		{
			const unsigned char *px = scr + ((long) y * SCR_W + x) * 4;

			if (px[1] || px[2] || px[3])
			{
				fail("pane not black", x, y);
				return;
			}
		}
}

static long cost(VIDUI *u, short vh, void (*fn)(VIDUI *, short), const GRECT *r)
{
	u->clip = *r;
	blits = 0;
	txt_reset();
	fn(u, vh);
	u->clip = u->work;
	return blits + ntxt;
}

int main(int argc, char **argv)
{
	VIDUI u;
	short vh = 1, W, H, mw, mh;
	long i, full;
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

	vidui_minsize(vh, &mw, &mh);
	printf("minimum window %dx%d\n", mw, mh);
	W = apj_skin_m(640);
	H = apj_skin_m(480);
	if (W < mw || H < mh)
		fail("640x480pt is under the minimum", W, mw);

	for (i = 0; i < (long) SCR_W * SCR_H; i++)
	{
		scr[i*4+0] = 0; scr[i*4+1] = 0xff; scr[i*4+2] = 0x00; scr[i*4+3] = 0xff;
	}

	memset(&u, 0, sizeof(u));
	u.title = "Blade Runner (1982) Final Cut";
	u.sub = "Ridley Scott";
	u.codec = "H.264";
	u.vid_w = 1920; u.vid_h = 800;
	u.fps100 = 2397;
	u.playing = 1;
	u.pos_s = 3671; u.len_s = 7020;
	u.vol = 72; u.hasvol = 1;
	u.loop = 1;
	u.ntracks = 24; u.sel = 1; u.top = 0;
	u.name_of = name_of;
	u.dir = "S:\\MEDIA\\FILMS\\SCIENCE FICTION\\1980S";
	u.shown = "23.9 fps shown";
	u.hover = W_FULL;
	u.press = -1;
	u.min_dw = u.min_dh = 0;
	u.msg = VID_MSG_NONE;

	/* -------- the playing window: picture box, everything visible ------ */
	vidui_layout(&u, vh, 40, 40, W, H);
	txt_reset();
	blits = 0;
	vidui_draw(&u, vh);
	full = blits + ntxt;
	printf("full redraw: %ld blits, %d strings, %ld pixels moved\n", blits, ntxt, pixels);
	if (blits > 400)
		fail("too many blits for one redraw", blits, 400);
	check_text(&u, "playing");
	check_pane_black(&u);

	/* -------- partial repaints: each a small fraction of a full one ---- */
	{
		GRECT r;
		long c;

		vidui_clock_rect(&u, &r);
		c = cost(&u, vh, vidui_draw_clock, &r);
		printf("clock repaint: %ld (full %ld)\n", c, full);
		if (c * 4 > full)
			fail("clock repaint not a small fraction of a full one", c, full);
		check_text(&u, "clock");

		vidui_status_rect(&u, &r);
		c = cost(&u, vh, vidui_draw_status, &r);
		printf("status repaint: %ld\n", c);
		if (c > 4)
			fail("status repaint costs more than its three strings", c, 4);
		check_text(&u, "status");
		if (r.g_y + r.g_h > u.work.g_y + u.work.g_h)
			fail("status rect leaves the window", r.g_y, r.g_h);

		vidui_info_rect(&u, &r);
		c = cost(&u, vh, vidui_draw_info, &r);
		printf("info repaint: %ld\n", c);
		if (c * 3 > full)
			fail("info repaint not a small fraction of a full one", c, full);

		/* a hover: one widget */
		{
			const APJ_LAY *l = apj_lay_find(u.lay, u.nlay, W_FULL);

			if (!l)
				fail("no fullscreen button in the layout", 0, 0);
			else
			{
				r.g_x = l->x; r.g_y = l->y; r.g_w = l->w; r.g_h = l->h;
				c = cost(&u, vh, vidui_draw_band, &r);
				printf("one widget repaint: %ld\n", c);
				if (c > 12)
					fail("one widget repaint touched more than one tile", c, 12);
			}
		}
	}

	/* a WM_REDRAW for a thin strip must not cost a whole redraw */
	{
		GRECT strip;

		strip.g_x = 40; strip.g_y = (short) (40 + H / 2); strip.g_w = W; strip.g_h = 6;
		blits = 0;
		txt_reset();
		vidui_draw_clip(&u, vh, &strip);
		printf("6 px strip redraw: %ld blits (full was %ld)\n", blits, full);
		if (blits + ntxt > full / 3)
			fail("strip redraw not much cheaper than a full one", blits, full);
	}

	/* hit-testing must agree with what was drawn */
	{
		const APJ_LAY *l = apj_lay_find(u.lay, u.nlay, W_PLAY);
		GRECT p;

		if (!l)
			fail("no play button in the layout", 0, 0);
		else if (vidui_hit(&u, (short) (l->x + 2), (short) (l->y + 2)) != W_PLAY)
			fail("hit test missed the play button", l->x, l->y);
		if (vidui_hit(&u, 5, 5) != -1)
			fail("hit test found a widget outside the window", 0, 0);
		vidui_pane_rect(&u, &p);
		if (vidui_hit(&u, (short) (p.g_x + 5), (short) (p.g_y + 5)) != W_VIDEO)
			fail("the pane is not the video widget", 0, 0);
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

	/* -------- list mode: the scrollbar must be hit before the list ----- */
	u.listmode = 1;
	vidui_layout(&u, vh, 40, 40, W, H);
	txt_reset();
	vidui_draw(&u, vh);
	check_text(&u, "list");
	{
		const APJ_LAY *sb = apj_lay_find(u.lay, u.nlay, W_SCROLL);

		if (!vidui_scroll_needed(&u))
			fail("24 files in this window should need a scrollbar", u.visrows, 24);
		else if (!sb)
			fail("no scrollbar in the layout", 0, 0);
		else if (vidui_hit(&u, (short) (sb->x + 1), (short) (sb->y + 1)) != W_SCROLL)
			fail("the list is hit before its scrollbar", 0, 0);
		if (vidui_row_at(&u, 40 + H - 2) >= 0 &&
		    vidui_row_at(&u, 40 + H - 2) >= u.ntracks)
			fail("row_at answered past the end", 0, 0);
	}
	/* one row: not the list */
	{
		const APJ_LAY *l = apj_lay_find(u.lay, u.nlay, W_LIST);
		GRECT r;
		long c;

		if (l)
		{
			r.g_x = l->x; r.g_y = (short) (l->y + apj_skin_m(4) + u.rowh); r.g_w = l->w; r.g_h = u.rowh;
			c = cost(&u, vh, vidui_draw_list, &r);
			printf("one row repaint: %ld\n", c);
			if (c > 24)
				fail("one row repaint costs as much as the list", c, 24);
		}
	}

	/* -------- awkward models -------------------------------------------- */
	/* every reading absent, nothing playing, the message in the pane */
	u.listmode = 0;
	u.playing = 0;
	u.codec = NULL;
	u.vid_w = u.vid_h = 0;
	u.fps100 = 0;
	u.shown = NULL;
	u.title = "";
	u.sub = "";
	u.dir = NULL;
	u.msg = VID_MSG_IDLE;
	vidui_layout(&u, vh, 40, 40, W, H);
	txt_reset();
	vidui_draw(&u, vh);
	check_text(&u, "idle");

	/* the floor message, with the widest figures it can carry */
	u.playing = 1;
	u.vid_w = 3840; u.vid_h = 2160;
	u.min_dw = 1920; u.min_dh = 1080;
	u.msg = VID_MSG_FLOOR_UNFIT;
	u.title = "A very long title that came off a NAS and goes on for ever and ever and ever and ever.mkv";
	u.sub = "An artist field with far more in it than the window has room for, twice over, and then some";
	u.shown = "1000.0 fps shown";
	u.dir = "S:\\A\\VERY\\DEEP\\PATH\\THAT\\GOES\\ON\\AND\\ON\\FOR\\LONGER\\THAN\\THE\\STATUS\\LINE\\CAN\\SHOW";
	txt_reset();
	vidui_draw(&u, vh);
	check_text(&u, "floor");

	/* far below the minimum: nothing may leave the window */
	vidui_layout(&u, vh, 40, 40, (short) (mw / 2), (short) (mh / 2));
	txt_reset();
	vidui_draw(&u, vh);
	check_text(&u, "tiny");
	u.listmode = 1;
	vidui_layout(&u, vh, 40, 40, (short) (mw / 2), (short) (mh / 2));
	txt_reset();
	vidui_draw(&u, vh);
	check_text(&u, "tiny list");

	apj_skin_free();
	printf(fails ? "%d CHECK(S) FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
