/*
 * pdfgem.c - PiSTorm PDF viewer, GEM front-end, on the APJSKIN sheet.
 *
 * The page is NOT drawn by this program. The Pi (PSPDF NatFeat, Poppler)
 * draws it on a worker thread and, on FETCH, copies the visible part into
 * a TT-RAM buffer this window owns - already in screen format, with the
 * search highlights blended in. What this program does with a page is one
 * vro_cpyfm from that buffer into the window. The 68k never touches a
 * page pixel.
 *
 * Everything on the 68k side is about not repainting. The rules, learned
 * on MP3GEM, VIDGEM and PSMON:
 *
 *   - the status strip and its badges are STRINGS, formatted every poll
 *     and repainted only when the string on screen would change;
 *   - a scroll repaints the pane only: one FETCH, one blit;
 *   - a hover repaints exactly two widgets, a press exactly one;
 *   - wind_update(BEG_UPDATE) is taken once per change, and the pointer
 *     is hidden only when it is inside the rectangles being painted;
 *   - MU_M1 is armed for a condition that is NOT already true;
 *   - nothing waits on the host: RENDER is queued, STATUS is polled on
 *     the timer, and a PSPDF_BUSY answer means "ask again next tick".
 *
 * Layout and drawing are in pdfui.c, which also builds on a Linux host
 * (tests/pdf) so the blits and text extents are checked there.
 */

#include <gem.h>
#include <osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define APJGUI_IMPL
#include "../apjgui/apjgui.h"
#include "../apjgui/apjskin.h"
#include "pdfui.h"

/* ---- NatFeat stubs (0x7300 GET_ID / 0x7301 CALL; harmless on real HW) ---- */
__asm__(
"   .text\n"
"   .globl _nf_id\n"
"_nf_id:\n"
"   .word 0x7300\n"
"   rts\n"
"   .globl _nf_call\n"
"_nf_call:\n"
"   .word 0x7301\n"
"   rts\n"
);
extern long nf_id(const char *name);
extern long nf_call(long id, ...);

/* PSPDF sub-ops - must match platforms/atari/pdf/pspdf.h */
#define PSPDF_VERSION   0
#define PSPDF_OPEN      1
#define PSPDF_OPENMEM   2
#define PSPDF_CLOSE     3
#define PSPDF_INFO      4
#define PSPDF_PAGESIZE  5
#define PSPDF_RENDER    6
#define PSPDF_STATUS    7
#define PSPDF_FETCH     8
#define PSPDF_FIND      9
#define PSPDF_TEXT     10
#define PSPDF_LINKS    11
#define PSPDF_LINKURI  12
#define PSPDF_OUTLINE  13
#define PSPDF_META     14
#define PSPDF_HILITE   15
#define PSPDF_PREFETCH 16
#define PSPDF_CONTINUOUS 17

#define PSPDF_OK        0
#define PSPDF_ERR     (-1)
#define PSPDF_LOCKED  (-2)
#define PSPDF_BUSY    (-3)

#define PSPDF_FIND_CASE  1
#define PSPDF_FIND_WORDS 2
#define PSPDF_LINK_PAGE  0
#define PSPDF_LINK_URI   1

#define ZOOM_100    1000L
#define ZOOM_MIN     250L
#define ZOOM_MAX    4000L
#define MAXHITS       64
#define MAXLINKS      32
#define TITLE_MAX     80

/* fsel_exinput() writes the name into OUR buffer and the AES decides how
 * much: under MiNT a HOSTFS name runs well past 64 bytes. 256 ends it. */
#define NAMELEN   256
#define PATHLEN   512

/* ---------------------------------------------------------------- state -- */

static long  pdfid;
static short vh;                          /* VDI handle */
static short win = -1;
static short cw, ch;                      /* char cell size */
static short wx, wy, ww, wh;              /* window work area */
static int   iconified = 0;
static short planes = 32;                 /* screen depth: 16 or 32 */

static long  handle = 0;                  /* PSPDF document, 0 = none */
static char  fname[NAMELEN] = "";
static char  fullpath[PATHLEN] = "";
static long  w100 = 0, h100 = 0;          /* this page at 100%, pixels */

static int   pending = 0;                 /* a RENDER is in flight */
static int   prefetched = 0;              /* the next page has been queued */
static int   prefetched_prev = 0;         /* and the previous one */
static int   fetched = 0;                 /* the buffer holds this page/scroll */

/* continuous scrolling: the host stitches the neighbouring pages above and
 * below this one (PSPDF API 3), and the viewer's scroll runs through the
 * seams; only the notion of "current page" moves. */
static int   continuous = 0;
static long  gap = 16;                    /* page pixels between pages */
static long  prev_h100 = 0, next_h100 = 0; /* neighbours' heights at 100%, 0 = none */
static int   neighbours_known = 0;

/* the page buffer */
static void *buf = NULL;
static long  buf_bytes = 0;
static short buf_w16 = 0, buf_h = 0;

/* outline, loaded once per document */
typedef struct { long page; short depth; char title[TITLE_MAX]; } OLENT;
static OLENT *outline = NULL;
static long   noutline = 0;

/* links on the current page */
static long  links[MAXLINKS * 6];
static int   nlinks = 0;

/* search */
static char  needle[PDF_SEARCHTEXT] = "";
static int   searching = 0;               /* a document search is running */
static long  search_page = 0;             /* the page to ask about next */
static long  search_from = 0;             /* where it started (wrap stop) */
static int   search_dir = 1;
static int   search_wrapped = 0;
static long  hits[MAXHITS * 4];
static int   nhits = 0;
static int   cur_hit = -1;

/* the strings the status strip shows, and what is really on screen */
static char  status_txt[160] = "";
static char  status_last[160] = "";
static char  hits_txt[48] = "";
static char  hits_last[48] = "";
static char  busy_txt[48] = "";
static char  busy_last[48] = "";

static PDFUI ui;
static GRECT m1r;
static short m1flag = MO_ENTER;
static long  min_out_w = 0, min_out_h = 0;

#define WIN_KIND (NAME | CLOSER | MOVER | SIZER | FULLER | SMALLER)

static void arm_m1(void);
static void relayout(void);
static void redraw_all(void);
static void start_render(void);

/* ------------------------------------------------------------- outline --- */

static const char *outline_of(void *c, long i, short *depth, long *page)
{
    (void)c;
    if (!outline || i < 0 || i >= noutline) {
        *depth = 0; *page = 0;
        return "";
    }
    *depth = outline[i].depth;
    *page = outline[i].page;
    return outline[i].title;
}

static void load_outline(void)
{
    long n, i;

    if (outline) { Mfree(outline); outline = NULL; }
    noutline = 0;
    n = nf_call(pdfid | PSPDF_INFO, handle, 1L);
    if (n <= 0)
        return;
    if (n > 5000)
        n = 5000;
    outline = (OLENT *)Malloc(n * (long)sizeof(OLENT));
    if (!outline)
        return;
    for (i = 0; i < n; i++) {
        struct { long depth; long page; char title[TITLE_MAX]; } ent;

        if (nf_call(pdfid | PSPDF_OUTLINE, handle, i, &ent) != 0)
            break;
        outline[i].depth = (short)ent.depth;
        outline[i].page = ent.page;
        memcpy(outline[i].title, ent.title, TITLE_MAX);
        outline[i].title[TITLE_MAX - 1] = '\0';
    }
    noutline = i;
}

/* the outline entry for a page: the last one at or before it */
static long outline_for_page(long page)
{
    long i, best = -1;

    for (i = 0; i < noutline; i++)
        if (outline[i].page > 0 && outline[i].page <= page)
            best = i;
    return best;
}

/* ------------------------------------------------------------- drawing --- */

static void draw_all(const GRECT *c)     { pdfui_draw_clip(&ui, vh, c); }
static void draw_band(const GRECT *c)    { ui.clip = *c; pdfui_draw_band(&ui, vh);    ui.clip = ui.work; }
static void draw_outline(const GRECT *c) { ui.clip = *c; pdfui_draw_outline(&ui, vh); ui.clip = ui.work; }
static void draw_pane(const GRECT *c)    { ui.clip = *c; pdfui_draw_pane(&ui, vh);    ui.clip = ui.work; }
static void draw_status(const GRECT *c)  { ui.clip = *c; pdfui_draw_status(&ui, vh);  ui.clip = ui.work; }

static void draw_iconic(const GRECT *c)
{
    (void)c;
    apj_fill(vh, wx, wy, ww, wh, apj_pen(APJ_R_PANEL));
}

static short mouse_off = 0;

static void redraw_begin(const GRECT *area)
{
    short mx, my, mb, ks;

    wind_update(BEG_UPDATE);
    mouse_off = 1;
    if (area) {
        graf_mkstate(&mx, &my, &mb, &ks);
        if (mx < area->g_x || my < area->g_y ||
            mx >= area->g_x + area->g_w || my >= area->g_y + area->g_h)
            mouse_off = 0;
    }
    if (mouse_off)
        graf_mouse(M_OFF, NULL);
}

static void redraw_end(void)
{
    apj_clip_off(vh);
    if (mouse_off)
        graf_mouse(M_ON, NULL);
    wind_update(END_UPDATE);
}

/* Walk the AES rectangle list, clip, and call fn for each visible part. */
static void redraw_at(void (*fn)(const GRECT *), short rx, short ry, short rw, short rh)
{
    GRECT r, d;

    if (rw <= 0 || rh <= 0)
        return;
    d.g_x = rx; d.g_y = ry; d.g_w = rw; d.g_h = rh;
    if (iconified)
        fn = draw_iconic;

    wind_get(win, WF_FIRSTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
    while (r.g_w && r.g_h) {
        GRECT i = r;
        if (rc_intersect(&d, &i)) {
            apj_clip(vh, i.g_x, i.g_y, i.g_w, i.g_h);
            fn(&i);
        }
        wind_get(win, WF_NEXTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
    }
}

static void redraw(void (*fn)(const GRECT *), short rx, short ry, short rw, short rh)
{
    GRECT d;

    if (win < 0)
        return;
    d.g_x = rx; d.g_y = ry; d.g_w = rw; d.g_h = rh;
    redraw_begin(&d);
    redraw_at(fn, rx, ry, rw, rh);
    redraw_end();
}

static void redraw_rect(void (*fn)(const GRECT *), const GRECT *r)
{
    if (!iconified)
        redraw(fn, r->g_x, r->g_y, r->g_w, r->g_h);
}

static void redraw_all(void)
{
    if (!iconified)
        redraw(draw_all, wx, wy, ww, wh);
    strcpy(status_last, status_txt);
    strcpy(hits_last, hits_txt);
    strcpy(busy_last, busy_txt);
}

static void widget_rect(short id, GRECT *r)
{
    const APJ_LAY *l = id >= 0 ? apj_lay_find(ui.lay, ui.nlay, id) : NULL;

    if (l) {
        r->g_x = l->x; r->g_y = l->y; r->g_w = l->w; r->g_h = l->h;
    } else
        r->g_x = r->g_y = r->g_w = r->g_h = 0;
}

static void redraw_widget(short id)
{
    GRECT r;

    widget_rect(id, &r);
    if (r.g_w > 0)
        redraw_rect(draw_band, &r);
}

static void redraw_band(void)
{
    GRECT r;

    pdfui_band_rect(&ui, &r);
    redraw_rect(draw_band, &r);
}

static void redraw_outline(void)
{
    GRECT r;

    pdfui_outline_rect(&ui, &r);
    if (r.g_w > 0)
        redraw_rect(draw_outline, &r);
}

static void redraw_pane(void)
{
    GRECT r;

    pdfui_pane_rect(&ui, &r);
    redraw_rect(draw_pane, &r);
}

/* the status strip: only when one of its strings changed on screen */
static void status_sync(void)
{
    GRECT r;

    if (strcmp(status_txt, status_last) == 0 &&
        strcmp(hits_txt, hits_last) == 0 &&
        strcmp(busy_txt, busy_last) == 0)
        return;
    pdfui_status_rect(&ui, &r);
    redraw_rect(draw_status, &r);
    strcpy(status_last, status_txt);
    strcpy(hits_last, hits_txt);
    strcpy(busy_last, busy_txt);
}

static void format_status(void)
{
    if (!handle) {
        status_txt[0] = '\0';
        return;
    }
    sprintf(status_txt, "%.40s  .  %ld page%s  .  %ldx%ld px  .  %ld%%  .  %s",
            fname, ui.pages, ui.pages == 1 ? "" : "s", ui.page_w, ui.page_h,
            ui.zoom / 10,
            ui.fit == PDF_FIT_WIDTH ? "fit width" :
            ui.fit == PDF_FIT_PAGE ? "fit page" : "free zoom");
}

/* ------------------------------------------------------------- buffer ---- */

/* the viewport's buffer: TT-RAM, width rounded to 16 pixels for the MFDB */
static int ensure_buffer(void)
{
    GRECT v;
    short w16;
    long need;

    pdfui_view_rect(&ui, &v);
    if (v.g_w <= 0 || v.g_h <= 0)
        return 0;
    w16 = (short)((v.g_w + 15) & ~15);
    need = (long)w16 * v.g_h * (planes / 8);
    if (buf && buf_w16 == w16 && buf_h == v.g_h)
        return 1;
    if (buf) { Mfree(buf); buf = NULL; }
    buf = (void *)Mxalloc(need, 1);             /* TT-RAM only: the host writes it */
    if (!buf) {
        buf_bytes = 0;
        ui.pagebuf.fd_addr = NULL;
        ui.msg = PDF_MSG_NOTT;
        return 0;
    }
    buf_bytes = need;
    buf_w16 = w16;
    buf_h = v.g_h;
    ui.pagebuf.fd_addr = NULL;          /* shown once a FETCH has filled it */
    ui.pagebuf.fd_w = w16;
    ui.pagebuf.fd_h = v.g_h;
    ui.pagebuf.fd_wdwidth = (short)(w16 / 16);
    ui.pagebuf.fd_stand = 0;
    ui.pagebuf.fd_nplanes = planes;
    ui.pagebuf.fd_r1 = ui.pagebuf.fd_r2 = ui.pagebuf.fd_r3 = 0;
    fetched = 0;
    return 1;
}

/* the colour the host fills around the page: the pane's canvas */
static long canvas_rgb(void)
{
    long p = apj_skin_rgb(APJ_R_PANEL), g = apj_skin_rgb(APJ_R_PAPER);
    long lp, lg;

    if (p < 0 || g < 0)
        return 0x808080L;
    lp = ((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255);
    lg = ((g >> 16) & 255) + ((g >> 8) & 255) + (g & 255);
    return lg < lp ? g : p;
}

static long prev_h(void) { return prev_h100 * ui.zoom / 1000L; }
static long next_h(void) { return next_h100 * ui.zoom / 1000L; }

/* the neighbours' heights, without waiting: PAGESIZE answers at once for
 * a page the host has seen, BUSY otherwise - then the tick asks again */
static void try_neighbours(void)
{
    long v;

    if (!handle || neighbours_known)
        return;
    neighbours_known = 1;
    prev_h100 = next_h100 = 0;
    if (ui.page > 1) {
        v = nf_call(pdfid | PSPDF_PAGESIZE, handle, ui.page - 1, ZOOM_100);
        if (v == PSPDF_BUSY) neighbours_known = 0;
        else if (v > 0) prev_h100 = (ui.rot == 90 || ui.rot == 270) ? ((v >> 16) & 0xffff) : (v & 0xffff);
    }
    if (ui.page < ui.pages) {
        v = nf_call(pdfid | PSPDF_PAGESIZE, handle, ui.page + 1, ZOOM_100);
        if (v == PSPDF_BUSY) neighbours_known = 0;
        else if (v > 0) next_h100 = (ui.rot == 90 || ui.rot == 270) ? ((v >> 16) & 0xffff) : (v & 0xffff);
    }
}

/* clamp the scroll, centring a page smaller than the viewport - or, in
 * continuous mode, letting it run into the neighbouring pages */
static void clamp_scroll(void)
{
    GRECT v;
    long miny, maxy;

    pdfui_view_rect(&ui, &v);
    if (ui.page_w <= v.g_w)
        ui.scroll_x = -((v.g_w - ui.page_w) / 2);
    else {
        if (ui.scroll_x < 0) ui.scroll_x = 0;
        if (ui.scroll_x > ui.page_w - v.g_w) ui.scroll_x = ui.page_w - v.g_w;
    }
    if (!continuous || ui.pages <= 1) {
        if (ui.page_h <= v.g_h)
            ui.scroll_y = -((v.g_h - ui.page_h) / 2);
        else {
            if (ui.scroll_y < 0) ui.scroll_y = 0;
            if (ui.scroll_y > ui.page_h - v.g_h) ui.scroll_y = ui.page_h - v.g_h;
        }
        return;
    }
    miny = (ui.page > 1 && prev_h() > 0) ? -(gap + prev_h()) : 0;
    maxy = (ui.page < ui.pages && next_h() > 0) ? ui.page_h + gap + next_h() - v.g_h
                                                : ui.page_h - v.g_h;
    if (maxy < miny) maxy = miny;
    if (ui.scroll_y < miny) ui.scroll_y = miny;
    if (ui.scroll_y > maxy) ui.scroll_y = maxy;
}

/* copy the viewport out of the host's page: one call, then one blit */
static int fetch(void)
{
    GRECT v;
    long rc;

    if (!handle || !buf || pending || ui.msg == PDF_MSG_FAILED)
        return 0;
    pdfui_view_rect(&ui, &v);
    clamp_scroll();
    rc = nf_call(pdfid | PSPDF_FETCH, handle, buf, ui.scroll_x, ui.scroll_y,
                 (long)v.g_w, (long)v.g_h, (long)planes,
                 (long)buf_w16 * (planes / 8), canvas_rgb());
    if (rc == PSPDF_BUSY)
        return 0;                       /* next tick */
    if (rc < 0) {
        ui.msg = PDF_MSG_FAILED;
        return 0;
    }
    ui.msg = PDF_MSG_NONE;
    ui.pagebuf.fd_addr = buf;
    fetched = 1;
    return 1;
}

static void fetch_and_show(void)
{
    if (fetch())
        redraw_pane();
}

/* ------------------------------------------------------------- pages ----- */

/* PAGESIZE can answer BUSY while the worker holds the document (a prefetch
 * of the next page, at most a second); wait it out briefly */
static long page_size(long page, long zoom)
{
    long v;
    int tries;

    for (tries = 0; tries < 100; tries++) {
        v = nf_call(pdfid | PSPDF_PAGESIZE, handle, page, zoom);
        if (v != PSPDF_BUSY)
            return v;
        evnt_timer(10L);
    }
    return PSPDF_ERR;
}

/* set the zoom for the current page from the fit mode, and the page size */
static void apply_zoom(void)
{
    long v;

    v = page_size(ui.page, ZOOM_100);
    if (v < 0) { w100 = h100 = 0; ui.page_w = ui.page_h = 0; return; }
    w100 = (v >> 16) & 0xffff;
    h100 = v & 0xffff;
    if (ui.rot == 90 || ui.rot == 270) { long t = w100; w100 = h100; h100 = t; }
    if (ui.fit != PDF_FIT_NONE)
        ui.zoom = pdfui_fit_zoom(&ui, w100, h100, ui.fit);
    if (ui.zoom < ZOOM_MIN) ui.zoom = ZOOM_MIN;
    if (ui.zoom > ZOOM_MAX) ui.zoom = ZOOM_MAX;
    ui.page_w = w100 * ui.zoom / 1000L;
    ui.page_h = h100 * ui.zoom / 1000L;
    if (ui.page_w < 1) ui.page_w = 1;
    if (ui.page_h < 1) ui.page_h = 1;
}

static void load_links(void)
{
    long n = nf_call(pdfid | PSPDF_LINKS, handle, ui.page, ui.zoom, links, (long)MAXLINKS);

    nlinks = (n > 0) ? (int)n : 0;
}

/* push this page's search hits to the host as highlights */
static void push_hilites(void)
{
    nf_call(pdfid | PSPDF_HILITE, handle, ui.page, hits, (long)nhits,
            apj_skin_rgb(APJ_R_ACCENT) >= 0 ? apj_skin_rgb(APJ_R_ACCENT) : 0x4cc2ffL);
}

/* the hits on the current page, at the current zoom */
static void refresh_hits(void)
{
    long n;

    nhits = 0;
    cur_hit = -1;
    hits_txt[0] = '\0';
    if (!handle || !needle[0])
        { push_hilites(); return; }
    n = nf_call(pdfid | PSPDF_FIND, handle, needle, ui.page, 0L, ui.zoom, hits, (long)MAXHITS);
    if (n > 0) {
        nhits = (int)n;
        cur_hit = 0;
        sprintf(hits_txt, "%d hit%s on this page", nhits, nhits == 1 ? "" : "s");
    }
    push_hilites();
}

static void start_render(void)
{
    if (!handle)
        return;
    nlinks = 0;
    if (nf_call(pdfid | PSPDF_RENDER, handle, ui.page, ui.zoom, (long)ui.rot) < 0) {
        ui.msg = PDF_MSG_FAILED;
        return;
    }
    pending = 1;
    prefetched = prefetched_prev = 0;
    fetched = 0;
    if (ui.msg != PDF_MSG_NOTT)
        ui.msg = PDF_MSG_NONE;
    /* a page the cache already holds (prefetched, or seen before) is ready
     * now: no busy badge, no wait for the timer */
    if (nf_call(pdfid | PSPDF_STATUS, handle) == 0) {
        pending = 0;
        load_links();
        busy_txt[0] = '\0';
    } else
        sprintf(busy_txt, "rendering %ld...", ui.page);
}

/* repaint what a page change touches - the field, the outline selection,
 * the status - and the pane once its pixels are in. Never the window. */
static void page_repaint(long old_sel, long old_top)
{
    const APJ_LAY *f = apj_lay_find(ui.lay, ui.nlay, W_PAGEFLD);
    const APJ_LAY *n = apj_lay_find(ui.lay, ui.nlay, W_PGNEXT);

    if (f) {
        GRECT r;

        r.g_x = f->x; r.g_y = f->y; r.g_h = f->h;
        r.g_w = (short)((n ? n->x : f->x + f->w + apj_skin_m(80)) - f->x);
        redraw_rect(draw_band, &r);
    }
    if (ui.ol_sel != old_sel || ui.ol_top != old_top)
        redraw_outline();
    status_sync();
    if (!pending)
        fetch_and_show();
}

/* show page n: size, zoom, hits, render; the scroll goes to the top
 * unless keep says otherwise */
static void goto_page(long n, int keep_scroll)
{
    long old_sel = ui.ol_sel, old_top = ui.ol_top;

    if (!handle)
        return;
    if (n < 1) n = 1;
    if (n > ui.pages) n = ui.pages;
    ui.page = n;
    sprintf(ui.pagetext, "%ld", n);
    neighbours_known = 0;
    apply_zoom();
    try_neighbours();
    if (!keep_scroll)
        ui.scroll_y = ui.scroll_x = 0;
    clamp_scroll();
    refresh_hits();
    start_render();
    ui.ol_sel = outline_for_page(n);
    if (ui.ol_sel >= 0 && ui.visrows > 0 &&
        (ui.ol_sel < ui.ol_top || ui.ol_sel >= ui.ol_top + ui.visrows))
        ui.ol_top = ui.ol_sel - ui.visrows / 3;
    if (ui.ol_top < 0) ui.ol_top = 0;
    if (ui.ol_top > noutline - ui.visrows) ui.ol_top = noutline - ui.visrows;
    if (ui.ol_top < 0) ui.ol_top = 0;
    format_status();
    page_repaint(old_sel, old_top);
}

/* the scroll crossed a seam: the same pixels are on screen either side of
 * this, only the current page and the origin change */
static int cross_seam(void)
{
    if (!continuous)
        return 0;
    if (ui.page < ui.pages && next_h() > 0 && ui.scroll_y >= ui.page_h + gap) {
        ui.scroll_y -= ui.page_h + gap;     /* the same rows, from the next page */
        goto_page(ui.page + 1, 1);          /* clamps, fetches, shows */
        return 1;
    }
    if (ui.page > 1 && prev_h() > 0 && ui.scroll_y < -gap) {
        ui.scroll_y += prev_h() + gap;
        goto_page(ui.page - 1, 1);
        return 1;
    }
    return 0;
}

static void set_zoom(long z, short fit)
{
    long oldw = ui.page_w, oldh = ui.page_h, cx, cy;
    GRECT v;

    if (!handle)
        return;
    pdfui_view_rect(&ui, &v);
    /* keep the point in the middle of the viewport where it is */
    cx = ui.scroll_x + v.g_w / 2;
    cy = ui.scroll_y + v.g_h / 2;
    ui.fit = fit;
    ui.zoom = z;
    apply_zoom();
    if (oldw > 0 && oldh > 0) {
        ui.scroll_x = cx * ui.page_w / oldw - v.g_w / 2;
        ui.scroll_y = cy * ui.page_h / oldh - v.g_h / 2;
    }
    clamp_scroll();
    refresh_hits();
    start_render();
    format_status();
    redraw_band();                          /* the zoom badge, the fit tiles */
    status_sync();
    if (!pending)
        fetch_and_show();
}

/* Scroll the page; past its bottom (or top) the wheel carries on into the
 * next (or previous) page, so a manual reads as one long strip. The turn
 * happens on the notch AFTER the edge is reached, so there is a stop. */
static void scroll_by(long dx, long dy)
{
    long ox = ui.scroll_x, oy = ui.scroll_y;
    GRECT v;

    if (!handle)
        return;
    pdfui_view_rect(&ui, &v);
    if (!continuous) {
        if (dy > 0 && (ui.page_h <= v.g_h || ui.scroll_y >= ui.page_h - v.g_h)) {
            if (ui.page < ui.pages)
                goto_page(ui.page + 1, 0);       /* top of the next page */
            return;
        }
        if (dy < 0 && (ui.page_h <= v.g_h || ui.scroll_y <= 0)) {
            if (ui.page > 1) {
                goto_page(ui.page - 1, 0);
                ui.scroll_y = 0x7fffffffL;       /* bottom of the previous */
                clamp_scroll();
            }
            return;
        }
    }
    ui.scroll_x += dx;
    ui.scroll_y += dy;
    clamp_scroll();
    if (cross_seam())
        return;                             /* goto_page() fetched and showed */
    if (ui.scroll_x != ox || ui.scroll_y != oy)
        fetch_and_show();
}

static void scroll_to_hit(int i)
{
    GRECT v;

    if (i < 0 || i >= nhits)
        return;
    cur_hit = i;
    pdfui_view_rect(&ui, &v);
    if (ui.page_h > v.g_h) {
        long y = hits[i * 4 + 1] - v.g_h / 3;

        if (y != ui.scroll_y) {
            ui.scroll_y = y;
            clamp_scroll();
            fetch_and_show();
        }
    }
}

/* ------------------------------------------------------------- open ------ */

static void close_doc(void)
{
    if (handle)
        nf_call(pdfid | PSPDF_CLOSE, handle);
    handle = 0;
    pending = 0;
    fetched = 0;
    searching = 0;
    nhits = 0;
    nlinks = 0;
    if (outline) { Mfree(outline); outline = NULL; }
    noutline = 0;
    ui.pages = 0;
    ui.page = 0;
    ui.noutline = 0;
    ui.ol_sel = -1;
    ui.ol_top = 0;
    ui.pagetext[0] = '\0';
    ui.msg = PDF_MSG_IDLE;
    status_txt[0] = hits_txt[0] = busy_txt[0] = '\0';
}

static int open_doc(const char *path)
{
    long h;
    const char *bs;

    close_doc();
    h = nf_call(pdfid | PSPDF_OPEN, path);
    if (h == PSPDF_LOCKED) {
        form_alert(1, "[1][This PDF needs a password.|"
                      "Password entry is not supported yet.][ OK ]");
        return 0;
    }
    if (h <= 0) {
        form_alert(1, "[1][The PDF could not be opened.|"
                      "It must be on a HOSTFS drive|"
                      "(S: or U:), and readable by the Pi.][ OK ]");
        return 0;
    }
    handle = h;
    strncpy(fullpath, path, sizeof(fullpath) - 1);
    fullpath[sizeof(fullpath) - 1] = '\0';
    bs = strrchr(path, '\\');
    if (!bs) bs = strrchr(path, '/');
    strncpy(fname, bs ? bs + 1 : path, sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = '\0';
    ui.fname = fname;
    ui.pages = nf_call(pdfid | PSPDF_INFO, handle, 0L);
    if (ui.pages < 1) ui.pages = 1;
    load_outline();
    ui.noutline = noutline;
    ui.ol_top = 0;
    ui.fit = PDF_FIT_WIDTH;
    ui.rot = 0;
    ui.page = 1;
    ui.zoom = ZOOM_100;
    gap = apj_skin_ok() ? apj_skin_m(10) : 12;
    continuous = (nf_call(pdfid | PSPDF_VERSION) >= 3) &&
                 nf_call(pdfid | PSPDF_CONTINUOUS, handle, 1L, gap) == 0;
    relayout();
    ensure_buffer();
    goto_page(1, 0);
    {
        char t[NAMELEN];

        sprintf(t, "%.60s", fname);
        wind_set_str(win, WF_NAME, t);
    }
    return 1;
}

static void do_open(void)
{
    static char fpath[PATHLEN] = "";
    static char fname_[NAMELEN] = "";
    short btn = 0;
    char full[PATHLEN];
    char *bs;

    fname_[0] = '\0';
    if (!fpath[0]) {
        fpath[0] = (char)('A' + Dgetdrv());
        strcpy(fpath + 1, ":\\*.PDF");
    }
    fsel_exinput(fpath, fname_, &btn, "Select a PDF (HOSTFS drive)");
    if (btn != 1 || !fname_[0])
        return;
    strncpy(full, fpath, sizeof(full) - 1);
    full[sizeof(full) - 1] = '\0';
    bs = strrchr(full, '\\');
    if (bs) bs[1] = '\0'; else full[0] = '\0';
    strncat(full, fname_, sizeof(full) - strlen(full) - 1);
    if (open_doc(full)) {
        arm_m1();
        redraw_all();
    }
}

/* A file handed to us - on the command line (double-clicked in the desktop)
 * or in a VA_START while running: "S:\DOCS\X.PDF", possibly quoted. */
static void open_path(const char *arg)
{
    static char full[PATHLEN];
    char *bs;
    size_t n;

    while (*arg == ' ')
        arg++;
    if (*arg == '\'') {
        arg++;
        strncpy(full, arg, sizeof(full) - 1);
        full[sizeof(full) - 1] = '\0';
        if ((bs = strchr(full, '\'')) != NULL)
            *bs = '\0';
    } else {
        strncpy(full, arg, sizeof(full) - 1);
        full[sizeof(full) - 1] = '\0';
    }
    n = strlen(full);
    while (n > 0 && (full[n - 1] == ' ' || full[n - 1] == '\r' || full[n - 1] == '\n'))
        full[--n] = '\0';
    if (!full[0])
        return;
    if (open_doc(full)) {
        arm_m1();
        redraw_all();
    }
}

/* ------------------------------------------------------------- window ---- */

static void compute_min_window(void)
{
    short mw, mh, cx, cy, cwid, chgt;

    pdfui_minsize(vh, &mw, &mh);
    wind_calc(WC_BORDER, WIN_KIND, 0, 0, mw, mh, &cx, &cy, &cwid, &chgt);
    min_out_w = cwid;
    min_out_h = chgt;
}

static void apply_min_size(void)
{
    short cx, cy, cwid, chgt, dx, dy, dw, dh;

    if (!min_out_w)
        return;
    wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
    wind_get(win, WF_CURRXYWH, &cx, &cy, &cwid, &chgt);
    if (cwid >= min_out_w && chgt >= min_out_h)
        return;
    if (cwid < min_out_w) cwid = (short)min_out_w;
    if (chgt < min_out_h) chgt = (short)min_out_h;
    if (cwid > dw) cwid = dw;
    if (chgt > dh) chgt = dh;
    if (cx + cwid > dx + dw) cx = (short)(dx + dw - cwid);
    if (cy + chgt > dy + dh) cy = (short)(dy + dh - chgt);
    if (cx < dx) cx = dx;
    if (cy < dy) cy = dy;
    wind_set(win, WF_CURRXYWH, cx, cy, cwid, chgt);
}

/* after any geometry change: relayout, and if the viewport changed size
 * a new buffer, a fit-zoom re-fit and a fresh fetch */
static void relayout(void)
{
    GRECT before, after;

    pdfui_view_rect(&ui, &before);
    wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
    pdfui_layout(&ui, vh, wx, wy, ww, wh);
    pdfui_view_rect(&ui, &after);
    if (ui.ol_top > noutline - ui.visrows) ui.ol_top = noutline - ui.visrows;
    if (ui.ol_top < 0) ui.ol_top = 0;
    if (!handle)
        return;
    if (after.g_w != before.g_w || after.g_h != before.g_h) {
        ensure_buffer();
        if (ui.fit != PDF_FIT_NONE) {
            long z = ui.zoom;

            apply_zoom();
            if (z != ui.zoom) {
                refresh_hits();
                start_render();
                format_status();
                return;
            }
        }
        clamp_scroll();
        fetch();
    }
}

/* the natural work-area size at this scale: never under the minimum,
 * never over the desktop */
static void natural_size(short *w, short *h)
{
    short dx, dy, dw, dh, mw, mh;

    *w = apj_skin_ok() ? apj_skin_m(700) : (short)(80 * cw);
    *h = apj_skin_ok() ? apj_skin_m(520) : (short)(30 * ch);
    pdfui_minsize(vh, &mw, &mh);
    if (*w < mw) *w = mw;
    if (*h < mh) *h = mh;
    wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
    if (*w > dw) *w = dw;
    if (*h > dh) *h = dh;
}

/* ------------------------------------------------------------- scale ----- */

/*
 * PDFGEM.INF lives in the root of the boot drive (C:\PDFGEM.INF), not
 * beside the program: the tools live on the HOSTFS share, which the Atari
 * cannot write to (EROFS), so a setting saved there is a setting lost.
 * Kept: the sheet scale (keys 1/2/3/0), the window's outer rectangle and
 * whether the outline panel is open.
 */
static long boot_drive_sv(void) { return *(volatile short *)0x446L; }

static void inf_path(char *out, long n)
{
    long d = Supexec(boot_drive_sv);

    if (d < 0 || d > 25)
        d = 2;                              /* C: */
    sprintf(out, "%c:\\PDFGEM.INF", (char)('A' + d));
    (void)n;
}

static short inf_scale_v = 0, inf_ol = -1;
static short inf_x = -1, inf_y = -1, inf_w = 0, inf_h = 0;

static void inf_load(void)
{
    char path[64], buf_[256], *p;
    long fh, got;

    inf_path(path, (long)sizeof path);
    fh = Fopen(path, 0);
    if (fh < 0)
        return;
    got = Fread((short)fh, (long)sizeof buf_ - 1, buf_);
    Fclose((short)fh);
    if (got <= 0)
        return;
    buf_[got] = 0;
    if ((p = strstr(buf_, "scale=")) != NULL)
        inf_scale_v = (short)atoi(p + 6);
    if ((p = strstr(buf_, "outline=")) != NULL)
        inf_ol = (short)atoi(p + 8);
    if ((p = strstr(buf_, "win=")) != NULL) {
        int x, y, w, h;

        if (sscanf(p + 4, "%d,%d,%d,%d", &x, &y, &w, &h) == 4 && w > 0 && h > 0) {
            inf_x = (short)x; inf_y = (short)y; inf_w = (short)w; inf_h = (short)h;
        }
    }
}

static void inf_save(void)
{
    char path[64], buf_[96];
    long fh;
    short cx, cy, cwid, chgt;

    inf_path(path, (long)sizeof path);
    fh = Fcreate(path, 0);
    if (fh < 0)
        return;
    if (win >= 0 && !iconified) {
        wind_get(win, WF_CURRXYWH, &cx, &cy, &cwid, &chgt);
        inf_x = cx; inf_y = cy; inf_w = cwid; inf_h = chgt;
    }
    sprintf(buf_, "scale=%d\r\nwin=%d,%d,%d,%d\r\noutline=%d\r\n",
            (int)apj_skin_preferred(), (int)inf_x, (int)inf_y, (int)inf_w, (int)inf_h,
            (int)ui.ol_show);
    Fwrite((short)fh, (long)strlen(buf_), buf_);
    Fclose((short)fh);
}

static void set_scale(short scale)
{
    if (scale == apj_skin_preferred() && apj_skin_ok())
        return;
    apj_skin_prefer(scale);
    apj_skin_reload(vh);
    inf_save();
    compute_min_window();
    apply_min_size();
    relayout();
    arm_m1();
    redraw_all();
}

/* ------------------------------------------------------------- hover ----- */

static void arm_m1(void)
{
    const APJ_LAY *l = ui.hover >= 0
        ? apj_lay_find(ui.lay, ui.nlay, ui.hover) : NULL;
    short mx, my, mb, ks;

    if (l) {
        m1r.g_x = l->x; m1r.g_y = l->y; m1r.g_w = l->w; m1r.g_h = l->h;
        m1flag = MO_LEAVE;
        return;
    }
    pdfui_bbox(&ui, &m1r);
    graf_mkstate(&mx, &my, &mb, &ks);
    if (mx >= m1r.g_x && mx < m1r.g_x + m1r.g_w &&
        my >= m1r.g_y && my < m1r.g_y + m1r.g_h) {
        m1r.g_x = (short)(mx - 1); m1r.g_y = (short)(my - 1);
        m1r.g_w = 3; m1r.g_h = 3;
        m1flag = MO_LEAVE;
    } else
        m1flag = MO_ENTER;
}

/* exactly two widgets: the one left and the one entered */
static void set_hover(short mx, short my)
{
    short h = pdfui_hit(&ui, mx, my);
    short old = ui.hover;

    if (h >= W_OLSCROLL || h == W_PAGEFLD || h == W_SEARCHFLD || h == W_ZOOMBADGE)
        h = -1;
    if (h != old) {
        GRECT a, b, u;

        ui.hover = h;
        widget_rect(old, &a);
        widget_rect(h, &b);
        if (!iconified && (a.g_w > 0 || b.g_w > 0)) {
            u = a.g_w > 0 ? a : b;
            if (a.g_w > 0 && b.g_w > 0) {
                short x1 = (short)(a.g_x + a.g_w > b.g_x + b.g_w ? a.g_x + a.g_w : b.g_x + b.g_w);
                short y1 = (short)(a.g_y + a.g_h > b.g_y + b.g_h ? a.g_y + a.g_h : b.g_y + b.g_h);

                u.g_x = a.g_x < b.g_x ? a.g_x : b.g_x;
                u.g_y = a.g_y < b.g_y ? a.g_y : b.g_y;
                u.g_w = (short)(x1 - u.g_x);
                u.g_h = (short)(y1 - u.g_y);
            }
            redraw_begin(&u);
            if (a.g_w > 0) redraw_at(draw_band, a.g_x, a.g_y, a.g_w, a.g_h);
            if (b.g_w > 0) redraw_at(draw_band, b.g_x, b.g_y, b.g_w, b.g_h);
            redraw_end();
        }
    }
    arm_m1();
}

/* ------------------------------------------------------------- search ---- */

static void search_start(int dir, long from)
{
    if (!handle || !needle[0])
        return;
    searching = 1;
    search_dir = dir;
    search_from = from;
    search_page = from;
    search_wrapped = 0;
    hits_txt[0] = '\0';
}

static void search_stop(void)
{
    searching = 0;
    busy_txt[0] = '\0';
}

/* a few pages per tick: FIND is ~7 ms a page on the Pi, and the window
 * must keep answering the mouse while a 3000-page manual is searched */
static void search_step(void)
{
    int n;
    long found[MAXHITS * 4];

    if (!searching)
        return;
    for (n = 0; n < 6 && searching; n++) {
        long r = nf_call(pdfid | PSPDF_FIND, handle, needle, search_page, 0L, ui.zoom,
                         found, (long)MAXHITS);

        if (r == PSPDF_BUSY)
            return;                         /* the worker has the document */
        if (r > 0) {
            search_stop();
            if (search_page != ui.page)
                goto_page(search_page, 0);  /* refreshes the hits there */
            else
                refresh_hits();
            if (nhits > 0)
                scroll_to_hit(search_dir > 0 ? 0 : nhits - 1);
            status_sync();
            return;
        }
        search_page += search_dir;
        if (search_page > ui.pages) { search_page = 1; search_wrapped = 1; }
        if (search_page < 1)        { search_page = ui.pages; search_wrapped = 1; }
        if (search_wrapped && search_page == search_from) {
            search_stop();
            strcpy(hits_txt, "not found");
            return;
        }
    }
    sprintf(busy_txt, "searching %ld of %ld...", search_page, ui.pages);
}

static void find_next(int dir)
{
    if (!handle)
        return;
    if (!needle[0]) {
        ui.focus = PDF_FOCUS_SEARCH;
        redraw_band();
        return;
    }
    if (nhits > 0 && cur_hit + dir >= 0 && cur_hit + dir < nhits) {
        scroll_to_hit(cur_hit + dir);
        return;
    }
    search_start(dir, dir > 0 ? (ui.page >= ui.pages ? 1 : ui.page + 1)
                              : (ui.page <= 1 ? ui.pages : ui.page - 1));
}

/* ------------------------------------------------------------- actions --- */

static void do_info(void)
{
    char t[64], a[64], msg[200];

    if (!handle) {
        form_alert(1, "[1][PDFGEM - PiSTorm PDF viewer|"
                      "Pages are drawn by the Pi (Poppler).|"
                      "Open a PDF from a HOSTFS drive.][ OK ]");
        return;
    }
    t[0] = a[0] = '\0';
    nf_call(pdfid | PSPDF_META, handle, 0L, t, 40L);
    nf_call(pdfid | PSPDF_META, handle, 1L, a, 40L);
    t[39] = a[39] = '\0';
    sprintf(msg, "[1][%.36s|%.36s|%ld pages, %ld outline entries|%.36s][ OK ]",
            t[0] ? t : fname, a[0] ? a : "-", ui.pages, noutline, fname);
    form_alert(1, msg);
}

static void toggle_outline(void)
{
    ui.ol_show = !ui.ol_show;
    relayout();
    arm_m1();
    redraw_all();
}

static void do_widget(short id, short mx, short my)
{
    (void)mx; (void)my;
    switch (id) {
    case W_OPEN:    do_open(); break;
    case W_PGPREV:  if (handle && ui.page > 1)        goto_page(ui.page - 1, 0); break;
    case W_PGNEXT:  if (handle && ui.page < ui.pages) goto_page(ui.page + 1, 0); break;
    case W_ZOOMOUT: if (handle) set_zoom(ui.zoom * 4 / 5, PDF_FIT_NONE); break;
    case W_ZOOMIN:  if (handle) set_zoom(ui.zoom * 5 / 4, PDF_FIT_NONE); break;
    case W_ZOOMBADGE: if (handle) set_zoom(ZOOM_100, PDF_FIT_NONE); break;
    case W_FITW:    if (handle) set_zoom(ui.zoom, PDF_FIT_WIDTH); break;
    case W_FITP:    if (handle) set_zoom(ui.zoom, PDF_FIT_PAGE); break;
    case W_ROTATE:
        if (handle) {
            ui.rot = (short)((ui.rot + 90) % 360);
            neighbours_known = 0;
            set_zoom(ui.zoom, ui.fit);
        }
        break;
    case W_FINDPREV: find_next(-1); break;
    case W_FINDNEXT: find_next(+1); break;
    case W_OUTLINE:  toggle_outline(); break;
    case W_ABOUT:     do_info(); break;
    default: break;
    }
}

static void set_ol_top(long t)
{
    long max = noutline - ui.visrows;

    if (max < 0) max = 0;
    if (t > max) t = max;
    if (t < 0)   t = 0;
    if (t != ui.ol_top) {
        ui.ol_top = t;
        redraw_outline();
    }
}

/* a click in the page: a link under it? */
static void click_page(short mx, short my)
{
    GRECT v;
    long px, py;
    int i;

    if (!handle || !fetched)
        return;
    pdfui_view_rect(&ui, &v);
    px = mx - v.g_x + ui.scroll_x;
    py = my - v.g_y + ui.scroll_y;
    for (i = 0; i < nlinks; i++) {
        long *l = links + i * 6;

        if (px >= l[0] && px < l[0] + l[2] && py >= l[1] && py < l[1] + l[3]) {
            if (l[4] == PSPDF_LINK_PAGE && l[5] > 0)
                goto_page(l[5], 0); else if (l[4] == PSPDF_LINK_URI) {
                char uri[200], msg[240];

                uri[0] = '\0';
                nf_call(pdfid | PSPDF_LINKURI, handle, ui.page, l[5], uri, 120L);
                uri[110] = '\0';
                sprintf(msg, "[1][Link:|%.36s|%.36s|%.36s][ OK ]", uri,
                        strlen(uri) > 36 ? uri + 36 : "",
                        strlen(uri) > 72 ? uri + 72 : "");
                form_alert(1, msg);
            }
            return;
        }
    }
}

static void click(short mx, short my)
{
    short id = pdfui_hit(&ui, mx, my);
    long row;

    /* a click anywhere takes the keyboard focus off a field */
    if (id != W_PAGEFLD && id != W_SEARCHFLD && ui.focus != PDF_FOCUS_NONE) {
        ui.focus = PDF_FOCUS_NONE;
        redraw_band();
    }

    if (id == W_OLSCROLL || id == W_PGSCROLL) {
        short part = (id == W_OLSCROLL) ? pdfui_ol_scroll_part(&ui, my)
                                        : pdfui_pg_scroll_part(&ui, my);
        GRECT v;

        pdfui_view_rect(&ui, &v);
        if (id == W_OLSCROLL && !pdfui_ol_scroll_needed(&ui))
            return;
        if (id == W_PGSCROLL && !pdfui_pg_scroll_needed(&ui))
            return;
        if (part < 0) {
            if (id == W_OLSCROLL) set_ol_top(ui.ol_top - ui.visrows);
            else scroll_by(0, -(v.g_h - v.g_h / 8));
        } else if (part > 0) {
            if (id == W_OLSCROLL) set_ol_top(ui.ol_top + ui.visrows);
            else scroll_by(0, v.g_h - v.g_h / 8);
        } else {
            /* drag the thumb: follow the mouse until the button goes up */
            GRECT t;
            short grab, bmx, bmy, bst, bks;

            if (id == W_OLSCROLL) pdfui_ol_thumb_rect(&ui, &t);
            else pdfui_pg_thumb_rect(&ui, &t);
            grab = (short)(my - t.g_y);
            ui.dragging = (id == W_OLSCROLL) ? 1 : 2;
            if (id == W_OLSCROLL) redraw_outline(); else redraw_pane();
            for (;;) {
                graf_mkstate(&bmx, &bmy, &bst, &bks);
                if (!(bst & 1))
                    break;
                if (id == W_OLSCROLL)
                    set_ol_top(pdfui_ol_top_for(&ui, bmy, grab));
                else {
                    long want = pdfui_pg_top_for(&ui, bmy, grab);

                    if (want != ui.scroll_y) {
                        ui.scroll_y = want;
                        clamp_scroll();
                        fetch_and_show();
                    }
                }
                evnt_timer(20L);
            }
            ui.dragging = 0;
            if (id == W_OLSCROLL) redraw_outline(); else redraw_pane();
        }
        return;
    }

    if (id == W_PAGEFLD) {
        ui.focus = PDF_FOCUS_PAGE;
        redraw_band();
        return;
    }
    if (id == W_SEARCHFLD) {
        ui.focus = PDF_FOCUS_SEARCH;
        redraw_band();
        return;
    }
    if (id == W_OLIST) {
        row = pdfui_ol_row_at(&ui, my);
        if (row >= 0 && outline && outline[row].page > 0) {
            ui.ol_sel = row;
            goto_page(outline[row].page, 0);
        }
        return;
    }
    if (id == W_PAGE) {
        click_page(mx, my);
        return;
    }
    if (id >= 0) {
        /* press feedback on that one widget, then the action */
        ui.press = id;
        redraw_widget(id);
        evnt_timer(70L);
        ui.press = -1;
        do_widget(id, mx, my);
        redraw_widget(id);
        return;
    }
}

/* ------------------------------------------------------------- keys ------ */

static void field_key(char c, short scan)
{
    char *t = (ui.focus == PDF_FOCUS_PAGE) ? ui.pagetext : ui.searchtext;
    size_t max = (ui.focus == PDF_FOCUS_PAGE) ? PDF_PAGETEXT - 1 : PDF_SEARCHTEXT - 1;
    size_t n = strlen(t);

    if (c == 0x1b) {                        /* escape: give the focus up */
        if (ui.focus == PDF_FOCUS_PAGE)
            sprintf(ui.pagetext, "%ld", ui.page);
        ui.focus = PDF_FOCUS_NONE;
        redraw_band();
        return;
    }
    if (c == '\r' || c == '\n') {
        if (ui.focus == PDF_FOCUS_PAGE) {
            long n_ = atol(ui.pagetext);

            ui.focus = PDF_FOCUS_NONE;
            if (handle && n_ >= 1 && n_ <= ui.pages && n_ != ui.page) {
                goto_page(n_, 0);
                redraw_band();
            } else {
                sprintf(ui.pagetext, "%ld", ui.page);
                redraw_band();
            }
        } else {
            strncpy(needle, ui.searchtext, sizeof(needle) - 1);
            needle[sizeof(needle) - 1] = '\0';
            if (handle && needle[0]) {
                refresh_hits();
                if (nhits > 0) {
                    scroll_to_hit(0);
                    status_sync();
                } else
                    search_start(+1, ui.page >= ui.pages ? 1 : ui.page + 1);
            }
            redraw_band();
        }
        return;
    }
    if (c == 8 || scan == 0x0E) {           /* backspace */
        if (n) t[n - 1] = '\0';
        redraw_band();
        return;
    }
    if (ui.focus == PDF_FOCUS_PAGE && (c < '0' || c > '9'))
        return;
    if ((unsigned char)c >= 32 && n < max) {
        t[n] = c;
        t[n + 1] = '\0';
        redraw_band();
    }
}

static void key(short kr)
{
    char  c    = (char)(kr & 0xff);
    short scan = (short)((kr >> 8) & 0xff);
    GRECT v;

    if (ui.focus != PDF_FOCUS_NONE) {
        field_key(c, scan);
        return;
    }
    pdfui_view_rect(&ui, &v);

    /* the PiSTorm's USB bridge turns wheel clicks into cursor Up/Down
     * taps, so the wheel arrives here */
    if      (scan == 0x48) scroll_by(0, -(v.g_h / 6));
    else if (scan == 0x50) scroll_by(0,  (v.g_h / 6));
    else if (scan == 0x4B) scroll_by(-(v.g_w / 6), 0);
    else if (scan == 0x4D) scroll_by( (v.g_w / 6), 0);
    else if (scan == 0x49) scroll_by(0, -(v.g_h - v.g_h / 8));   /* PgUp */
    else if (scan == 0x51) scroll_by(0,  (v.g_h - v.g_h / 8));   /* PgDn */
    else if (scan == 0x47) { if (handle) goto_page(1, 0); }        /* Home */
    else if (scan == 0x4F) { if (handle) goto_page(ui.pages, 0); } /* End  */
    else if (scan == 0x3D) find_next(+1);                                            /* F3   */
    else if (c == '+' || c == '=') do_widget(W_ZOOMIN, 0, 0);
    else if (c == '-')             do_widget(W_ZOOMOUT, 0, 0);
    else if (c == 'w' || c == 'W') do_widget(W_FITW, 0, 0);
    else if (c == 'p' || c == 'P') do_widget(W_FITP, 0, 0);
    else if (c == 'r' || c == 'R') do_widget(W_ROTATE, 0, 0);
    else if (c == 'l' || c == 'L') toggle_outline();
    else if (c == 'o' || c == 'O') do_open();
    else if (c == 'i' || c == 'I') do_info();
    else if (c == 'f' || c == 'F' || c == 6 || c == '/') {   /* F, ^F, / : search */
        ui.focus = PDF_FOCUS_SEARCH;
        redraw_band();
    }
    else if (c == 'g' || c == 'G') {                         /* G: go to page */
        ui.pagetext[0] = '\0';
        ui.focus = PDF_FOCUS_PAGE;
        redraw_band();
    }
    else if (c == 'n' || c == 'N') find_next(+1);
    else if (c == 0x1b) { if (searching) search_stop(); }
    else if (c == '1') set_scale(100);
    else if (c == '2') set_scale(125);
    else if (c == '3') set_scale(175);
    else if (c == '0') set_scale(0);
}

/* ------------------------------------------------------------- timer ----- */

static void tick(void)
{
    if (!handle)
        return;

    if (pending) {
        long st = nf_call(pdfid | PSPDF_STATUS, handle);

        if (st == 0) {
            pending = 0;
            busy_txt[0] = '\0';
            load_links();
            if (fetch())
                redraw_pane();
        } else if (st < 0) {
            pending = 0;
            busy_txt[0] = '\0';
            ui.msg = PDF_MSG_FAILED;
            redraw_pane();
        }
    } else if (!fetched) {
        fetch_and_show();                   /* a FETCH that answered BUSY */
    } else if (!prefetched && ui.page < ui.pages && !searching) {
        /* the page is on screen: draw the next one behind it */
        nf_call(pdfid | PSPDF_PREFETCH, handle, ui.page + 1, ui.zoom, (long)ui.rot);
        prefetched = 1;
    } else if (!prefetched_prev && ui.page > 1 && !searching) {
        nf_call(pdfid | PSPDF_PREFETCH, handle, ui.page - 1, ui.zoom, (long)ui.rot);
        prefetched_prev = 1;
    }
    if (!neighbours_known) {
        try_neighbours();
        if (neighbours_known) {
            long y = ui.scroll_y;

            clamp_scroll();             /* the seams are known now */
            if (y != ui.scroll_y)
                fetch_and_show();
        }
    }

    if (searching && !pending)
        search_step();

    status_sync();
}

/* ------------------------------------------------------------- main ------ */

#define VA_START    0x4711     /* AV protocol: open these files */
#define AV_STARTED  0x4738

int main(int argc, char *argv[])
{
    short work_in[11], work_out[57];
    short d, msg[8];
    short mx, my, mb, ks, kr, brk;
    short ev, i;

    if (appl_init() < 0)
        return 1;
    pdfid = nf_id("PSPDF");
    if (!pdfid) {
        form_alert(1, "[3][PSPDF NatFeat not found.|"
                      "Run under the PiSTorm emulator|"
                      "(built with Poppler).][ OK ]");
        appl_exit();
        return 1;
    }

    vh = graf_handle(&cw, &ch, &d, &d);
    for (i = 0; i < 10; i++) work_in[i] = 1;
    work_in[10] = 2;
    v_opnvwk(work_in, &vh, work_out);
    vst_alignment(vh, 0, 5, &d, &d);              /* left / top text origin */
    apj_init(vh);                                 /* theme + renderer, before any window */
    inf_load();                                   /* C:\PDFGEM.INF */
    apj_skin_prefer(inf_scale_v);                 /* saved scale, else by screen */
    apj_skin_load(vh, NULL);                      /* follows the theme; may fail */

    planes = apj_skin_planes();
    if (planes != 16 && planes != 32) {
        short ext[57];

        vq_extnd(vh, 1, ext);
        planes = ext[4];
    }
    if (planes != 16 && planes != 32) {
        form_alert(1, "[3][PDFGEM needs a 16 or 32 bit|screen.][ OK ]");
        v_clsvwk(vh);
        appl_exit();
        return 1;
    }

    memset(&ui, 0, sizeof(ui));
    ui.outline_of = outline_of;
    ui.ol_sel = -1;
    ui.ol_show = inf_ol < 0 ? 1 : (inf_ol != 0);
    ui.hover = -1;
    ui.press = -1;
    ui.fit = PDF_FIT_WIDTH;
    ui.zoom = ZOOM_100;
    ui.msg = PDF_MSG_IDLE;
    ui.status = status_txt;
    ui.hits = hits_txt;
    ui.busy = busy_txt;

    {
        short dx, dy, dw, dh, cx, cy, cwid, chgt;
        short want_w, want_h;

        natural_size(&want_w, &want_h);
        wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
        wind_calc(WC_BORDER, WIN_KIND,
                  dx + 16, dy + 16, want_w, want_h, &cx, &cy, &cwid, &chgt);
        if (inf_w > 0 && inf_h > 0) {          /* the saved window, if it fits */
            cx = inf_x; cy = inf_y; cwid = inf_w; chgt = inf_h;
            if (cwid > dw) cwid = dw;
            if (chgt > dh) chgt = dh;
            if (cx < dx) cx = dx;
            if (cy < dy) cy = dy;
            if (cx + cwid > dx + dw) cx = (short)(dx + dw - cwid);
            if (cy + chgt > dy + dh) cy = (short)(dy + dh - chgt);
        }
        if (cx + cwid > dx + dw) cwid = (short)(dx + dw - cx);
        if (cy + chgt > dy + dh) chgt = (short)(dy + dh - cy);
        win = wind_create(WIN_KIND, dx, dy, dw, dh);
        wind_set_str(win, WF_NAME, "PiSTorm PDF");
        wind_open(win, cx, cy, cwid, chgt);
        wind_set(win, WF_WHEEL, 1, WHEEL_ARROWED, 0, 0);
        compute_min_window();
        apply_min_size();                  /* a saved window under the minimum grows */
        relayout();
    }
    arm_m1();
    redraw_all();

    if (argc > 1)                         /* a file double-clicked in the desktop */
        open_path(argv[1]);

    for (;;) {
        ev = evnt_multi(MU_MESAG | MU_BUTTON | MU_KEYBD | MU_TIMER | MU_M1,
                        1, 1, 1,
                        m1flag, m1r.g_x, m1r.g_y, m1r.g_w, m1r.g_h,
                        0, 0, 0, 0, 0,
                        msg, (handle && (pending || searching || !fetched)) ? 30UL : 250UL,
                        &mx, &my, &mb, &ks, &kr, &brk);

        if (ev & MU_MESAG) {
            switch (msg[0]) {
                case WM_REDRAW:
                    redraw(draw_all, msg[4], msg[5], msg[6], msg[7]);
                    break;
                case WM_TOPPED:
                    wind_set(win, WF_TOP, 0, 0, 0, 0);
                    break;
                case WM_MOVED:
                    wind_set(win, WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
                    relayout();           /* the AES moves the pixels itself */
                    arm_m1();
                    break;
                case WM_SIZED: {
                    short w = msg[6], h = msg[7];

                    if (w < min_out_w) w = (short)min_out_w;
                    if (h < min_out_h) h = (short)min_out_h;
                    wind_set(win, WF_CURRXYWH, msg[4], msg[5], w, h);
                    relayout();
                    arm_m1();
                    redraw_all();
                    break;
                }
                case WM_FULLED: {
                    short cx, cy, cwid, chgt;

                    wind_get(win, WF_FULLXYWH, &cx, &cy, &cwid, &chgt);
                    wind_set(win, WF_CURRXYWH, cx, cy, cwid, chgt);
                    relayout();
                    arm_m1();
                    redraw_all();
                    break;
                }
                case WM_ARROWED: {    /* the mouse wheel, when the AES sends it */
                    short n = (short)((msg[4] >> 8) & 0xff);
                    GRECT v;

                    pdfui_view_rect(&ui, &v);
                    if (n < 1) n = 1;
                    switch (msg[4] & 15) {
                        case WA_UPLINE: scroll_by(0, -(v.g_h / 6) * n); break;
                        case WA_DNLINE: scroll_by(0,  (v.g_h / 6) * n); break;
                        case WA_UPPAGE: scroll_by(0, -(v.g_h - v.g_h / 8)); break;
                        case WA_DNPAGE: scroll_by(0,  (v.g_h - v.g_h / 8)); break;
                        case WA_LFLINE: scroll_by(-(v.g_w / 6) * n, 0); break;
                        case WA_RTLINE: scroll_by( (v.g_w / 6) * n, 0); break;
                    }
                    break;
                }
                case WM_CLOSED:
                    goto out;
                case WM_ICONIFY:
                case WM_ALLICONIFY:
                    iconified = 1;
                    wind_set(win, WF_ICONIFY, msg[4], msg[5], msg[6], msg[7]);
                    wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
                    break;
                case WM_UNICONIFY:
                    wind_set(win, WF_UNICONIFY, msg[4], msg[5], msg[6], msg[7]);
                    iconified = 0;
                    relayout();
                    arm_m1();
                    redraw_all();
                    break;
                case APJ_SKINCHG:         /* the desktop changed theme */
                    apj_init(vh);
                    apj_skin_reload(vh);
                    compute_min_window();
                    apply_min_size();
                    relayout();
                    push_hilites();       /* the accent may have changed */
                    fetched = 0;          /* and the canvas colour with it */
                    arm_m1();
                    redraw_all();
                    break;
                case VA_START: {          /* opened again while running */
                    short reply[8];
                    char *cmd = (char *)(((long)msg[3] << 16) | (unsigned short)msg[4]);

                    if (iconified) {
                        wind_set(win, WF_UNICONIFY, -1, -1, -1, -1);
                        iconified = 0;
                        relayout();
                        redraw_all();
                    }
                    if (cmd)
                        open_path(cmd);
                    wind_set(win, WF_TOP, 0, 0, 0, 0);
                    reply[0] = AV_STARTED; reply[1] = gl_apid; reply[2] = 0;
                    reply[3] = msg[3]; reply[4] = msg[4];
                    reply[5] = reply[6] = reply[7] = 0;
                    appl_write(msg[1], 16, reply);
                    break;
                }
            }
        }
        if ((ev & MU_M1) && !iconified)
            set_hover(mx, my);
        if ((ev & MU_BUTTON) && !iconified)
            click(mx, my);
        if (ev & MU_KEYBD) {
            if (ui.focus == PDF_FOCUS_NONE && ((kr & 0xff) == 'q' || (kr & 0xff) == 'Q'))
                goto out;
            key(kr);
        }
        if (ev & MU_TIMER)
            tick();
    }

out:
    inf_save();                           /* window, scale, outline */
    close_doc();
    if (buf) Mfree(buf);
    apj_skin_free();
    wind_close(win);
    wind_delete(win);
    v_clsvwk(vh);
    appl_exit();
    return 0;
}
