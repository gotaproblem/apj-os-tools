/*
 * tests/web/harness.c - build APJSKIN and WEBGEM's drawing code on the
 * host, run them against a fake 32 bpp screen and a fake page buffer, and
 * check every blit, every text extent, and the cost of each partial
 * repaint.
 *
 * The fake VDI is ../stubvdi.h. Text is recorded, not drawn: XaAES draws
 * that on the real machine, so the check here is that no string leaves
 * its strip and nothing is ever written over the page pixels.
 *
 *   ./harness <file.SKN> <out.ppm>      -> 0 ok, 1 a check failed
 */
#define APJGUI_IMPL

#include "../stubvdi.h"
#include "../../webgem/webui.h"

static const char *TABS[] = {
	"Wikipedia, the free encyclopedia", "The Register", "Atari-Forum",
	"A tab title that is far longer than any tab can carry at any scale at all",
	"GitHub", "BBC News", "psweb", "eight"
};

static const char *tab_title(void *ctx, short i)
{
	(void) ctx;
	return TABS[i % 8];
}

static int inside(short x, short y, short w, short h,
                  short X, short Y, short W, short H)
{
	return x >= X && y >= Y && x + w <= X + W && y + h <= Y + H;
}

static int meets(short x, short y, short w, short h, const GRECT *r)
{
	return r->g_w > 0 && r->g_h > 0 &&
	       x + w > r->g_x && x < r->g_x + r->g_w &&
	       y + h > r->g_y && y < r->g_y + r->g_h;
}

/* every recorded string inside the window and inside its own strip;
 * nothing over the page */
static void check_text(WEBUI *u, const char *when)
{
	short i;
	GRECT v, b, s, t;

	webui_view_rect(u, &v);
	webui_band_rect(u, &b);
	webui_status_rect(u, &s);
	webui_tabs_rect(u, &t);
	for (i = 0; i < ntxt; i++)
	{
		if (!inside(txt[i].x, txt[i].y, txt[i].w, txt[i].h,
		            u->work.g_x, u->work.g_y, u->work.g_w, u->work.g_h))
		{
			fprintf(stderr, "%s: \"%s\" at %d,%d %dx%d leaves the window\n",
			        when, txt[i].s, txt[i].x, txt[i].y, txt[i].w, txt[i].h);
			fail("text outside the window", txt[i].x, txt[i].y);
		}
		/* a string that starts in a strip stays in it */
		if (meets(txt[i].x, txt[i].y, 1, 1, &b) &&
		    !inside(txt[i].x, txt[i].y, txt[i].w, txt[i].h, b.g_x, b.g_y, b.g_w, b.g_h))
		{
			fprintf(stderr, "%s: \"%s\" leaves the toolbar\n", when, txt[i].s);
			fail("text outside the toolbar", txt[i].x, txt[i].y);
		}
		if (meets(txt[i].x, txt[i].y, 1, 1, &s) &&
		    !inside(txt[i].x, txt[i].y, txt[i].w, txt[i].h, s.g_x, s.g_y, s.g_w, s.g_h))
		{
			fprintf(stderr, "%s: \"%s\" leaves the status strip\n", when, txt[i].s);
			fail("text outside the status strip", txt[i].x, txt[i].y);
		}
		if (meets(txt[i].x, txt[i].y, 1, 1, &t) &&
		    !inside(txt[i].x, txt[i].y, txt[i].w, txt[i].h, t.g_x, t.g_y, t.g_w, t.g_h))
		{
			fprintf(stderr, "%s: \"%s\" leaves the tab strip\n", when, txt[i].s);
			fail("text outside the tab strip", txt[i].x, txt[i].y);
		}
		/* nothing is ever written over the page pixels */
		if (u->pagebuf.fd_addr && u->msg == WEB_MSG_NONE &&
		    meets(txt[i].x, txt[i].y, txt[i].w, txt[i].h, &v))
		{
			fprintf(stderr, "%s: \"%s\" is drawn over the page\n", when, txt[i].s);
			fail("text over the page", txt[i].x, txt[i].y);
		}
	}
}

/* the view must carry the buffer's pixels edge to edge */
static void check_view(WEBUI *u, const unsigned char *pattern, const char *when)
{
	GRECT v;
	short x, y;

	webui_view_rect(u, &v);
	for (y = v.g_y; y < v.g_y + v.g_h; y += 3)
		for (x = v.g_x; x < v.g_x + v.g_w; x += 3)
		{
			const unsigned char *px = scr + ((long) y * SCR_W + x) * 4;
			long bx = x - v.g_x, by = y - v.g_y;
			const unsigned char *want = pattern + (by * u->pagebuf.fd_w + bx) * 4;

			if (px[1] != want[1] || px[2] != want[2] || px[3] != want[3])
			{
				fprintf(stderr, "%s: view pixel %d,%d is not the buffer's\n", when, x, y);
				fail("view pixel is not the buffer's", x, y);
				return;
			}
		}
	/* and its four corner pixels exactly: full bleed, no frame */
	{
		short cx[2], cy[2], i, j;

		cx[0] = v.g_x; cx[1] = (short) (v.g_x + v.g_w - 1);
		cy[0] = v.g_y; cy[1] = (short) (v.g_y + v.g_h - 1);
		for (j = 0; j < 2; j++)
			for (i = 0; i < 2; i++)
			{
				const unsigned char *px = scr + ((long) cy[j] * SCR_W + cx[i]) * 4;
				long bx = cx[i] - v.g_x, by = cy[j] - v.g_y;
				const unsigned char *want = pattern + (by * u->pagebuf.fd_w + bx) * 4;

				if (px[1] != want[1] || px[2] != want[2] || px[3] != want[3])
					fail("a view corner is not the buffer's (a frame?)", cx[i], cy[j]);
			}
	}
}

static long cost(WEBUI *u, short vh, void (*fn)(WEBUI *, short), const GRECT *r)
{
	u->clip = *r;
	blits = 0;
	txt_reset();
	fn(u, vh);
	u->clip = u->work;
	return blits + ntxt;
}

static void give_buffer(WEBUI *u, unsigned char **buf)
{
	GRECT v;
	long i;

	webui_view_rect(u, &v);
	free(*buf);
	*buf = NULL;
	u->pagebuf.fd_addr = NULL;
	if (v.g_w <= 0 || v.g_h <= 0)
		return;
	u->pagebuf.fd_w = (short) ((v.g_w + 15) & ~15);
	u->pagebuf.fd_h = v.g_h;
	u->pagebuf.fd_wdwidth = (short) (u->pagebuf.fd_w / 16);
	u->pagebuf.fd_stand = 0;
	u->pagebuf.fd_nplanes = 32;
	*buf = (unsigned char *) malloc((size_t) u->pagebuf.fd_w * u->pagebuf.fd_h * 4);
	for (i = 0; i < (long) u->pagebuf.fd_w * u->pagebuf.fd_h; i++)
	{
		(*buf)[i*4+0] = 0;
		(*buf)[i*4+1] = (unsigned char) (i & 255);
		(*buf)[i*4+2] = (unsigned char) ((i >> 8) & 255);
		(*buf)[i*4+3] = (unsigned char) (200 + (i % 37));
	}
	u->pagebuf.fd_addr = *buf;
}

int main(int argc, char **argv)
{
	WEBUI u;
	short vh = 1, W, H, mw, mh;
	long i, full;
	unsigned char *buf = NULL;
	FILE *o;
	GRECT v, r;

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

	printf("scale %d%%   tile %dx%d   glyph %d   TILE3 %s\n",
	       apj_skin_scale(), apj_skin_tilew(), apj_skin_tileh(),
	       apj_skin_glyphsz(), apj_skin_has(APJ_RG_TILE3) ? "yes" : "no (fallback plates)");
	if (!apj_skin_has_tile(APJ_G_BACK) || !apj_skin_has_tile(APJ_G_DOWNLOAD))
		fail("sheet has no TILE3 toolbar tiles (mkskin.py not rebuilt?)", APJ_G_BACK, APJ_G_DOWNLOAD);

	webui_minsize(vh, &mw, &mh);
	printf("minimum window %dx%d\n", mw, mh);
	W = apj_skin_m(760);
	H = apj_skin_m(440);
	if (W < mw || H < mh)
		fail("760x440pt is under the minimum", W, mw);

	for (i = 0; i < (long) SCR_W * SCR_H; i++)
	{
		scr[i*4+0] = 0; scr[i*4+1] = 0xff; scr[i*4+2] = 0x00; scr[i*4+3] = 0xff;
	}

	memset(&u, 0, sizeof(u));
	u.uri = "https://en.wikipedia.org/wiki/Atari_ST";
	u.link = "https://en.wikipedia.org/wiki/Motorola_68000";
	u.status = "Done";
	u.loading = 1;
	u.progress = 640;
	u.secure = 1;
	u.can_back = 1; u.can_fwd = 0;
	u.js_on = 1; u.blocker_on = 1;
	u.msg = WEB_MSG_NONE;
	u.ntabs = 1; u.tab_sel = 0;
	u.tab_title = tab_title;
	u.focus = WEB_FOCUS_PAGE;
	u.hover = WB_RELOAD;
	u.press = -1;

	/* -------- lay out, then give the pane a buffer of the view's size */
	webui_layout(&u, vh, 40, 40, W, H);
	webui_view_rect(&u, &v);
	printf("view %dx%d at %d,%d\n", v.g_w, v.g_h, v.g_x, v.g_y);
	if (v.g_w <= 0 || v.g_h <= 0)
		fail("no view", v.g_w, v.g_h);
	if (v.g_x != 40 || v.g_w != W)
		fail("the view is not full bleed across the window", v.g_x, v.g_w);
	webui_pane_rect(&u, &r);
	if (r.g_x != v.g_x || r.g_y != v.g_y || r.g_w != v.g_w || r.g_h != v.g_h)
		fail("the pane and the view differ", r.g_w, v.g_w);
	give_buffer(&u, &buf);

	/* -------- the full window --------------------------------------- */
	txt_reset();
	blits = 0;
	webui_draw(&u, vh);
	full = blits + ntxt;
	printf("full redraw: %ld blits, %d strings, %ld pixels moved\n", blits, ntxt, pixels);
	if (blits > 200)
		fail("too many blits for one redraw", blits, 200);
	check_text(&u, "full");
	check_view(&u, buf, "full");

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

	/* -------- partial repaints: each a small fraction of a full one ---- */
	{
		long c;

		/* a new frame: the pane only - one blit and nothing else */
		webui_pane_rect(&u, &r);
		c = cost(&u, vh, webui_draw_pane, &r);
		printf("pane repaint (a new frame): %ld ops\n", c);
		if (c != 1)
			fail("a frame costs more than one blit", c, 1);
		check_view(&u, buf, "pane");

		/* a damage rectangle inside the page: one blit of just that */
		r.g_x = (short) (v.g_x + 100); r.g_y = (short) (v.g_y + 50);
		r.g_w = 64; r.g_h = 32;
		pixels = 0;
		c = cost(&u, vh, webui_draw_pane, &r);
		printf("64x32 damage repaint: %ld ops, %ld pixels\n", c, pixels);
		if (c != 1 || pixels != 64L * 32)
			fail("a damage rect did not blit exactly itself", c, pixels);
		check_view(&u, buf, "damage");

		webui_status_rect(&u, &r);
		c = cost(&u, vh, webui_draw_status, &r);
		printf("status repaint: %ld ops\n", c);
		if (c > 20)
			fail("status repaint costs more than its strip and three badges", c, 20);
		check_text(&u, "status");
		if (r.g_y + r.g_h > u.work.g_y + u.work.g_h)
			fail("status rect leaves the window", r.g_y, r.g_h);

		/* a hover: one widget */
		{
			const APJ_LAY *l = apj_lay_find(u.lay, u.nlay, WB_RELOAD);

			if (!l)
				fail("no reload button in the layout", 0, 0);
			else
			{
				r.g_x = l->x; r.g_y = l->y; r.g_w = l->w; r.g_h = l->h;
				c = cost(&u, vh, webui_draw_band, &r);
				printf("one widget repaint: %ld ops\n", c);
				if (c > 12)
					fail("one widget repaint touched more than one tile", c, 12);
			}
		}

		/* the progress rail: the bottom rows of the field only */
		{
			const APJ_LAY *l = apj_lay_find(u.lay, u.nlay, WB_ADDRESS);

			if (!l)
				fail("no address field in the layout", 0, 0);
			else
			{
				r.g_x = l->x; r.g_y = (short) (l->y + l->h - apj_skin_m(3));
				r.g_w = l->w; r.g_h = apj_skin_m(3);
				c = cost(&u, vh, webui_draw_band, &r);
				printf("progress rail repaint: %ld ops\n", c);
				if (c > 40)
					fail("progress rail repaint redraws the toolbar", c, 40);
			}
		}
	}

	/* a WM_REDRAW for a thin strip across the page must be one blit */
	{
		GRECT strip;

		strip.g_x = 40; strip.g_y = (short) (v.g_y + v.g_h / 2); strip.g_w = W; strip.g_h = 6;
		blits = 0;
		txt_reset();
		pixels = 0;
		webui_draw_clip(&u, vh, &strip);
		printf("6 px strip redraw over the page: %ld blits, %ld pixels (full was %ld)\n",
		       blits, pixels, full);
		if (blits != 1 || pixels != 6L * v.g_w)
			fail("a strip over the page is not exactly one blit of the strip", blits, pixels);
		check_view(&u, buf, "strip");
	}

	/* hit-testing must agree with what was drawn */
	{
		const APJ_LAY *l = apj_lay_find(u.lay, u.nlay, WB_BACK);
		GRECT p, bb;

		if (!l)
			fail("no back button in the layout", 0, 0);
		else if (webui_hit(&u, (short) (l->x + 2), (short) (l->y + 2)) != WB_BACK)
			fail("hit test missed back", l->x, l->y);
		if (webui_hit(&u, 5, 5) != -1)
			fail("hit test found a widget outside the window", 0, 0);
		webui_view_rect(&u, &p);
		if (webui_hit(&u, (short) (p.g_x + 5), (short) (p.g_y + 5)) != WB_PAGE)
			fail("the view is not the page widget", 0, 0);
		if (webui_hit(&u, p.g_x, p.g_y) != WB_PAGE)
			fail("the view's top-left corner is not the page", 0, 0);
		if (webui_hit(&u, (short) (p.g_x + p.g_w - 1), (short) (p.g_y + p.g_h - 1)) != WB_PAGE)
			fail("the view's bottom-right corner is not the page", 0, 0);
		webui_widget_rect(&u, WB_BADGE_JS, &p);
		if (p.g_w <= 0 || webui_hit(&u, (short) (p.g_x + 1), (short) (p.g_y + 1)) != WB_BADGE_JS)
			fail("the JS badge is not a hit target", p.g_x, p.g_y);
		webui_widget_rect(&u, WB_MENU, &p);
		if (p.g_w <= 0 || p.g_x + p.g_w > 40 + W)
			fail("the menu tile is not inside the window", p.g_x, p.g_w);
		webui_bbox(&u, &bb);
		webui_view_rect(&u, &p);
		if (bb.g_h < u.work.g_h - p.g_h)
			fail("bbox does not span the strips around the page", bb.g_h, p.g_h);
	}

	/* -------- awkward models -------------------------------------------- */
	/* the address field focused, selected, longer than the box */
	u.focus = WEB_FOCUS_ADDRESS;
	u.addr_all = 1;
	strcpy(u.addr, "https://a-very-long-host-name-that-goes-on-and-on.example.org/with/a/path/that/does/not/end/either?and=a&query=string#fragment");
	u.loading = 0;
	txt_reset();
	webui_draw(&u, vh);
	check_text(&u, "focused");
	check_view(&u, buf, "focused");
	u.addr_all = 0;
	u.addr[0] = '\0';
	txt_reset();
	webui_draw(&u, vh);
	check_text(&u, "empty field");
	u.focus = WEB_FOCUS_PAGE;

	/* eight tabs */
	u.ntabs = 8; u.tab_sel = 3;
	webui_layout(&u, vh, 40, 40, W, H);
	give_buffer(&u, &buf);
	txt_reset();
	webui_draw(&u, vh);
	check_text(&u, "8 tabs");
	check_view(&u, buf, "8 tabs");
	webui_tabs_rect(&u, &r);
	if (r.g_h <= 0)
		fail("no tab strip with 8 tabs", 0, 0);
	if (!apj_lay_find(u.lay, u.nlay, WB_TABNEW))
		fail("no new-tab button with 8 tabs", 0, 0);
	{
		const APJ_LAY *l = apj_lay_find(u.lay, u.nlay, WB_TAB0 + 3);
		if (l && webui_hit(&u, (short) (l->x + 3), (short) (l->y + 3)) != WB_TAB0 + 3)
			fail("hit test missed tab 3", l->x, l->y);
	}
	u.ntabs = 1; u.tab_sel = 0;

	/* no frame yet: waiting, then crashed - the message may cross the view */
	u.pagebuf.fd_addr = NULL;
	u.msg = WEB_MSG_WAIT;
	webui_layout(&u, vh, 40, 40, W, H);
	txt_reset();
	webui_draw(&u, vh);
	check_text(&u, "waiting");
	u.msg = WEB_MSG_CRASHED;
	txt_reset();
	webui_draw(&u, vh);
	check_text(&u, "crashed");
	u.msg = WEB_MSG_NONE;

	/* the widest strings everything can carry */
	u.uri = "https://a-very-long-host-name-that-goes-on-and-on.example.org/with/a/path/that/does/not/end/either?and=a&query=string#fragment";
	u.link = "https://another-very-long-host-name.example.org/with/a/path/that/does/not/end/either?and=a&query=string#fragment-with-more";
	u.loading = 1; u.progress = 1000;
	u.js_on = 0; u.blocker_on = 0;
	u.secure = 0;
	webui_layout(&u, vh, 40, 40, W, H);
	give_buffer(&u, &buf);
	txt_reset();
	webui_draw(&u, vh);
	check_text(&u, "widest");
	check_view(&u, buf, "widest");

	/* the minimum: everything still there */
	webui_layout(&u, vh, 40, 40, mw, mh);
	if (!apj_lay_find(u.lay, u.nlay, WB_ADDRESS) || !apj_lay_find(u.lay, u.nlay, WB_MENU) ||
	    !apj_lay_find(u.lay, u.nlay, WB_HOME))
		fail("the minimum window loses a toolbar widget", mw, mh);
	give_buffer(&u, &buf);
	txt_reset();
	webui_draw(&u, vh);
	check_text(&u, "minimum");
	check_view(&u, buf, "minimum");

	/* far below the minimum: nothing may leave the window */
	webui_layout(&u, vh, 40, 40, (short) (mw / 2), (short) (mh / 2));
	give_buffer(&u, &buf);
	txt_reset();
	webui_draw(&u, vh);
	check_text(&u, "tiny");
	if (u.pagebuf.fd_addr)
		check_view(&u, buf, "tiny");
	u.ntabs = 8;
	webui_layout(&u, vh, 40, 40, (short) (mw / 2), (short) (mh / 2));
	give_buffer(&u, &buf);
	txt_reset();
	webui_draw(&u, vh);
	check_text(&u, "tiny tabs");

	/* the 1920x1080 case: full screen work area at 175% on the real box */
	u.ntabs = 1;
	webui_layout(&u, vh, 0, 0, 1920, 1016);
	webui_view_rect(&u, &v);
	printf("full screen view %dx%d\n", v.g_w, v.g_h);
	if (v.g_w != 1920 || v.g_h > 1016 || v.g_h < 800)
		fail("full screen view is not sensible", v.g_w, v.g_h);

	free(buf);
	apj_skin_free();
	printf(fails ? "%d CHECK(S) FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
