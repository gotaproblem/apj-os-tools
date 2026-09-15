/*
 * tests/pdf/harness.c - build APJSKIN and PDFGEM's drawing code on the
 * host, run them against a fake 32 bpp screen and a fake page buffer, and
 * check every blit, every text extent, and the cost of each partial
 * repaint.
 *
 * The fake VDI is ../stubvdi.h. Text is recorded, not drawn: XaAES draws
 * that on the real machine, so the check here is that no string leaves
 * its panel.
 *
 *   ./harness <file.SKN> <out.ppm>      -> 0 ok, 1 a check failed
 */
#define APJGUI_IMPL

#include "../stubvdi.h"
#include "../../pdfgem/pdfui.h"

static const char *TITLES[] = {
	"Title Page", "Simulator Information", "List of Tests",
	"Text Revision List", "List of Required Signatures",
	"1  Performance", "1.1  Engine start", "1.2  Taxi", "1.3  Take-off",
	"A very long outline entry that came out of a manual and goes on for ever and ever",
	"2  Handling qualities", "2.1  Static control checks"
};

static const char *outline_of(void *c, long i, short *depth, long *page)
{
	(void) c;
	*depth = (short) ((i % 12) >= 6 && (i % 12) != 10 ? 1 : 0);
	*page = i * 7 + 1;
	return TITLES[i % 12];
}

static int inside(short x, short y, short w, short h,
                  short X, short Y, short W, short H)
{
	return x >= X && y >= Y && x + w <= X + W && y + h <= Y + H;
}

/* every recorded string inside the window; those in the outline inside it */
static void check_text(PDFUI *u, const char *when)
{
	short i;
	GRECT o, v;

	pdfui_outline_rect(u, &o);
	pdfui_view_rect(u, &v);
	for (i = 0; i < ntxt; i++)
	{
		if (!inside(txt[i].x, txt[i].y, txt[i].w, txt[i].h,
		            u->work.g_x, u->work.g_y, u->work.g_w, u->work.g_h))
		{
			fprintf(stderr, "%s: \"%s\" at %d,%d %dx%d leaves the window\n",
			        when, txt[i].s, txt[i].x, txt[i].y, txt[i].w, txt[i].h);
			fail("text outside the window", txt[i].x, txt[i].y);
		}
		if (o.g_w > 0 && txt[i].x >= o.g_x && txt[i].x < o.g_x + o.g_w &&
		    txt[i].y >= o.g_y && txt[i].y < o.g_y + o.g_h &&
		    !inside(txt[i].x, txt[i].y, txt[i].w, txt[i].h,
		            o.g_x, o.g_y, o.g_w, o.g_h))
		{
			fprintf(stderr, "%s: \"%s\" leaves the outline panel\n", when, txt[i].s);
			fail("text outside the outline", txt[i].x, txt[i].y);
		}
		/* nothing is ever written over the page pixels */
		if (v.g_w > 0 && u->pagebuf.fd_addr && u->msg == PDF_MSG_NONE &&
		    txt[i].y + txt[i].h > v.g_y && txt[i].y < v.g_y + v.g_h &&
		    txt[i].x + txt[i].w > v.g_x && txt[i].x < v.g_x + v.g_w)
		{
			fprintf(stderr, "%s: \"%s\" is drawn over the page\n", when, txt[i].s);
			fail("text over the page", txt[i].x, txt[i].y);
		}
	}
}

/* the viewport must carry the buffer's pixels edge to edge */
static void check_view(PDFUI *u, const unsigned char *pattern)
{
	GRECT v;
	short x, y;

	pdfui_view_rect(u, &v);
	for (y = v.g_y; y < v.g_y + v.g_h; y += 5)
		for (x = v.g_x; x < v.g_x + v.g_w; x += 5)
		{
			const unsigned char *px = scr + ((long) y * SCR_W + x) * 4;
			long bx = x - v.g_x, by = y - v.g_y;
			const unsigned char *want = pattern + (by * u->pagebuf.fd_w + bx) * 4;

			if (px[1] != want[1] || px[2] != want[2] || px[3] != want[3])
			{
				fail("viewport pixel is not the buffer's", x, y);
				return;
			}
		}
}

static long cost(PDFUI *u, short vh, void (*fn)(PDFUI *, short), const GRECT *r)
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
	PDFUI u;
	short vh = 1, W, H, mw, mh;
	long i, full;
	unsigned char *buf;
	FILE *o;
	GRECT v;

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

	printf("scale %d%%   tile %dx%d   glyph %d   TILE2 %s\n",
	       apj_skin_scale(), apj_skin_tilew(), apj_skin_tileh(),
	       apj_skin_glyphsz(), apj_skin_has(APJ_RG_TILE2) ? "yes" : "no (fallback plates)");

	pdfui_minsize(vh, &mw, &mh);
	printf("minimum window %dx%d\n", mw, mh);
	W = apj_skin_m(680);
	H = apj_skin_m(520);
	if (W < mw || H < mh)
		fail("680x520pt is under the minimum", W, mw);

	for (i = 0; i < (long) SCR_W * SCR_H; i++)
	{
		scr[i*4+0] = 0; scr[i*4+1] = 0xff; scr[i*4+2] = 0x00; scr[i*4+3] = 0xff;
	}

	memset(&u, 0, sizeof(u));
	u.fname = "747-MQTG.PDF";
	u.pages = 3272;
	u.page = 42;
	u.zoom = 1000;
	u.fit = PDF_FIT_WIDTH;
	u.page_w = 816; u.page_h = 1056;
	u.scroll_x = 0; u.scroll_y = 64;
	u.msg = PDF_MSG_NONE;
	u.status = "747-MQTG.PDF  .  3272 pages  .  816x1056 px  .  100%  .  fit width";
	u.hits = "3 hits on this page";
	u.busy = "rendering 43...";
	u.noutline = 758;
	u.ol_sel = 6; u.ol_top = 0; u.ol_show = 1;
	u.outline_of = outline_of;
	strcpy(u.pagetext, "42");
	strcpy(u.searchtext, "engine");
	u.focus = PDF_FOCUS_SEARCH;
	u.hover = W_FINDNEXT;
	u.press = -1;

	/* -------- lay out, then give the pane a buffer of the viewport's size */
	pdfui_layout(&u, vh, 40, 40, W, H);
	pdfui_view_rect(&u, &v);
	printf("viewport %dx%d at %d,%d\n", v.g_w, v.g_h, v.g_x, v.g_y);
	if (v.g_w <= 0 || v.g_h <= 0)
		fail("no viewport", v.g_w, v.g_h);
	u.pagebuf.fd_w = (short) ((v.g_w + 15) & ~15);
	u.pagebuf.fd_h = v.g_h;
	u.pagebuf.fd_wdwidth = (short) (u.pagebuf.fd_w / 16);
	u.pagebuf.fd_stand = 0;
	u.pagebuf.fd_nplanes = 32;
	buf = (unsigned char *) malloc((size_t) u.pagebuf.fd_w * u.pagebuf.fd_h * 4);
	for (i = 0; i < (long) u.pagebuf.fd_w * u.pagebuf.fd_h; i++)
	{
		buf[i*4+0] = 0;
		buf[i*4+1] = (unsigned char) (i & 255);
		buf[i*4+2] = (unsigned char) ((i >> 8) & 255);
		buf[i*4+3] = (unsigned char) (200 + (i % 37));
	}
	u.pagebuf.fd_addr = buf;
	u.page_w = v.g_w;			/* fit width: the page is as wide as the view */
	u.page_h = (long) v.g_w * 1056 / 816;

	/* -------- the full window --------------------------------------- */
	txt_reset();
	blits = 0;
	pdfui_draw(&u, vh);
	full = blits + ntxt;
	printf("full redraw: %ld blits, %d strings, %ld pixels moved\n", blits, ntxt, pixels);
	if (blits > 400)
		fail("too many blits for one redraw", blits, 400);
	check_text(&u, "full");
	check_view(&u, buf);

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
		GRECT r;
		long c;

		/* a scroll: the pane only - one page blit plus the scrollbar */
		pdfui_pane_rect(&u, &r);
		c = cost(&u, vh, pdfui_draw_pane, &r);
		printf("pane repaint (a scroll): %ld ops\n", c);
		if (c > 12)
			fail("a scroll costs more than the page blit and the scrollbar", c, 12);
		check_view(&u, buf);

		pdfui_status_rect(&u, &r);
		c = cost(&u, vh, pdfui_draw_status, &r);
		printf("status repaint: %ld ops\n", c);
		if (c > 14)
			fail("status repaint costs more than its strip and two badges", c, 14);
		check_text(&u, "status");
		if (r.g_y + r.g_h > u.work.g_y + u.work.g_h)
			fail("status rect leaves the window", r.g_y, r.g_h);

		/* a hover: one widget */
		{
			const APJ_LAY *l = apj_lay_find(u.lay, u.nlay, W_FINDNEXT);

			if (!l)
				fail("no find-next button in the layout", 0, 0);
			else
			{
				r.g_x = l->x; r.g_y = l->y; r.g_w = l->w; r.g_h = l->h;
				c = cost(&u, vh, pdfui_draw_band, &r);
				printf("one widget repaint: %ld ops\n", c);
				if (c > 12)
					fail("one widget repaint touched more than one tile", c, 12);
			}
		}

		/* one outline row */
		{
			const APJ_LAY *l = apj_lay_find(u.lay, u.nlay, W_OLIST);

			if (l)
			{
				r.g_x = l->x; r.g_y = (short) (l->y + apj_skin_m(18) + apj_skin_m(4) + u.rowh);
				r.g_w = l->w; r.g_h = u.rowh;
				c = cost(&u, vh, pdfui_draw_outline, &r);
				printf("one outline row repaint: %ld ops\n", c);
				if (c > 24)
					fail("one row repaint costs as much as the list", c, 24);
			}
		}
	}

	/* a WM_REDRAW for a thin strip must not cost a whole redraw */
	{
		GRECT strip;

		strip.g_x = 40; strip.g_y = (short) (40 + H / 2); strip.g_w = W; strip.g_h = 6;
		blits = 0;
		txt_reset();
		pdfui_draw_clip(&u, vh, &strip);
		printf("6 px strip redraw: %ld blits (full was %ld)\n", blits, full);
		if (blits + ntxt > full / 3)
			fail("strip redraw not much cheaper than a full one", blits, full);
	}

	/* hit-testing must agree with what was drawn */
	{
		const APJ_LAY *l = apj_lay_find(u.lay, u.nlay, W_PGNEXT);
		const APJ_LAY *sb = apj_lay_find(u.lay, u.nlay, W_OLSCROLL);
		GRECT p;

		if (!l)
			fail("no page-next button in the layout", 0, 0);
		else if (pdfui_hit(&u, (short) (l->x + 2), (short) (l->y + 2)) != W_PGNEXT)
			fail("hit test missed page-next", l->x, l->y);
		if (pdfui_hit(&u, 5, 5) != -1)
			fail("hit test found a widget outside the window", 0, 0);
		pdfui_view_rect(&u, &p);
		if (pdfui_hit(&u, (short) (p.g_x + 5), (short) (p.g_y + 5)) != W_PAGE)
			fail("the viewport is not the page widget", 0, 0);
		if (!pdfui_ol_scroll_needed(&u))
			fail("758 entries should need a scrollbar", u.visrows, 758);
		else if (!sb)
			fail("no outline scrollbar in the layout", 0, 0);
		else if (pdfui_hit(&u, (short) (sb->x + 1), (short) (sb->y + 1)) != W_OLSCROLL)
			fail("the outline is hit before its scrollbar", 0, 0);
		if (pdfui_ol_row_at(&u, 40 + H - 2) >= u.noutline)
			fail("row_at answered past the end", 0, 0);
		if (!pdfui_pg_scroll_needed(&u))
			fail("a page taller than the view should need a scrollbar", u.page_h, v.g_h);
	}


	/* -------- awkward models -------------------------------------------- */
	/* outline closed: the page takes the width, the buffer is resized */
	u.ol_show = 0;
	pdfui_layout(&u, vh, 40, 40, W, H);
	pdfui_view_rect(&u, &v);
	if (apj_lay_find(u.lay, u.nlay, W_OLIST))
		fail("outline panel drawn while closed", 0, 0);
	u.pagebuf.fd_addr = NULL;		/* the app would refetch; here: canvas */
	txt_reset();
	pdfui_draw(&u, vh);
	check_text(&u, "no outline");

	/* nothing open */
	u.pages = 0;
	u.page = 0;
	u.noutline = 0;
	u.ol_show = 1;
	u.status = NULL;
	u.hits = NULL;
	u.busy = NULL;
	u.msg = PDF_MSG_IDLE;
	u.pagetext[0] = '\0';
	u.searchtext[0] = '\0';
	u.focus = PDF_FOCUS_NONE;
	pdfui_layout(&u, vh, 40, 40, W, H);
	txt_reset();
	pdfui_draw(&u, vh);
	check_text(&u, "idle");
	if (apj_lay_find(u.lay, u.nlay, W_OLIST))
		fail("outline panel drawn with no entries", 0, 0);

	/* the widest strings everything can carry */
	u.pages = 99999;
	u.page = 99999;
	u.zoom = 4000;
	u.noutline = 99999;
	u.ol_sel = 99998; u.ol_top = 99990;
	u.msg = PDF_MSG_FAILED;
	u.status = "A-VERY-LONG-FILE-NAME-THAT-CAME-OFF-A-NAS-AND-GOES-ON.PDF  .  99999 pages  .  9999x9999 px  .  400%  .  fit page  .  and more";
	u.hits = "64 hits on this page, and that is the most it can say";
	u.busy = "searching 99999 of 99999...";
	strcpy(u.pagetext, "99999");
	strcpy(u.searchtext, "a needle far longer than the search field can show at any scale");
	u.focus = PDF_FOCUS_SEARCH;
	pdfui_layout(&u, vh, 40, 40, W, H);
	txt_reset();
	pdfui_draw(&u, vh);
	check_text(&u, "widest");

	/* far below the minimum: nothing may leave the window */
	pdfui_layout(&u, vh, 40, 40, (short) (mw / 2), (short) (mh / 2));
	txt_reset();
	pdfui_draw(&u, vh);
	check_text(&u, "tiny");
	u.ol_show = 1;
	u.noutline = 12;
	pdfui_layout(&u, vh, 40, 40, (short) (mw / 2), (short) (mh / 2));
	txt_reset();
	pdfui_draw(&u, vh);
	check_text(&u, "tiny outline");

	free(buf);
	apj_skin_free();
	printf(fails ? "%d CHECK(S) FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
