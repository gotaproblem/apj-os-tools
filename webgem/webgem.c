/*
 * webgem.c - WEBGEM.PRG: the PiSTorm web browser, GEM front end.
 *
 * The page is rendered by psweb on the Pi (WPE WebKit) and arrives through
 * the PSWEB NatFeat as changed rectangles in a TT-RAM buffer; this program
 * draws the toolbar out of the APJSKIN sheet, blits those rectangles into
 * its window, and sends the mouse, wheel and keyboard back. The drawing
 * lives in webui.c (host-testable); this file is the shell: NatFeat,
 * event loop, .INF, dialogs.
 *
 * Rules (from PSCTRL and PDFGEM, each learned the hard way):
 *   - repaint only what changed: one widget, one strip, the damaged
 *     rectangles of the page - never the window;
 *   - wind_update(BEG_UPDATE) is taken once per change, and the pointer
 *     is hidden only when it is over the part being drawn;
 *   - MU_M1 is armed for a condition that is NOT already true;
 *   - the pointer over the page is followed from the timer tick, not
 *     from MU_M1, so the page gets motion at a steady rate.
 *
 *     WEBGEM                          opens the home page
 *     WEBGEM https://example.org      opens that
 *
 * Keys: Ctrl+L address field, Return loads, Esc leaves the field / stops
 * a load, Alt+Left / Alt+Right or Ctrl+[ / Ctrl+] back and forward, F5 or
 * Ctrl+R reload, Ctrl+ + / - / 0 zoom, Alt+1/2/3/0 sheet scale, Alt+X or
 * Ctrl+Q quit. Everything else goes to the page. WEBGEM.INF lives in the
 * root of the boot drive: scale=, win=x,y,w,h, home=, js=, blocker=,
 * pause= (1: the page is paused while the window is not on top).
 *
 * Build:   make        (needs m68k-atari-mint-gcc + gemlib)
 */
#include <gem.h>
#include <osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APJGUI_IMPL
#include "../apjgui/apjgui.h"
#include "../apjgui/apjskin.h"
#include "webui.h"

/* ---- NatFeat call stubs (ARAnyM/PiStorm ABI) --------------------------- */
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

/* PSWEB sub-ops - must match platforms/atari/web/psweb_client.h */
#define PSWEB_VERSION     0
#define PSWEB_STATUS      1
#define PSWEB_VIEW_NEW    2
#define PSWEB_VIEW_FREE   3
#define PSWEB_VIEW_SIZE   4
#define PSWEB_VIEW_STATE  5
#define PSWEB_LOAD        6
#define PSWEB_NAV         7
#define PSWEB_POLL        8
#define PSWEB_FETCH       9
#define PSWEB_POINTER    10
#define PSWEB_SCROLL     11
#define PSWEB_KEY        12
#define PSWEB_TEXT       13
#define PSWEB_GETSTR     14
#define PSWEB_ZOOM       19
#define PSWEB_SETTING    24

#define PSWEB_ERR        (-1L)  /* results: 0 ok, -1 error, -3 busy, -4 not connected */

#define PSWEB_STR_TITLE   0
#define PSWEB_STR_URI     1
#define PSWEB_STR_LINK    2
#define PSWEB_STR_STATUS  3

#define PSWEB_SET_JAVASCRIPT 0
#define PSWEB_SET_HOME       4
#define PSWEB_SET_BLOCKER    6

#define PSWEB_FL_CAN_BACK   0x0001
#define PSWEB_FL_CAN_FWD    0x0002
#define PSWEB_FL_LOADING    0x0004
#define PSWEB_FL_SECURE     0x0008
#define PSWEB_FL_CRASHED    0x0040
#define PSWEB_FL_JS_OFF     0x0100
#define PSWEB_FL_BLOCKER    0x0200

#define NAV_BACK    0
#define NAV_FWD     1
#define NAV_RELOAD  2
#define NAV_NOCACHE 3
#define NAV_STOP    4

#define MAX_RECTS 32
#define DEFAULT_HOME "https://en.wikipedia.org/wiki/Main_Page"
#define WIN_KIND (NAME | CLOSER | MOVER | SIZER | FULLER | SMALLER)

#define VA_START    0x4711     /* AV protocol: open this URL */
#define AV_STARTED  0x4738

/* the eight big-endian longs POLL writes - native order on the 68k */
typedef struct {
    long frame_serial, n_damage, progress, flags, cursor, title_serial, uri_serial, dialog;
} POLLSTATE;

/* ------------------------------------------------------------- state ----- */

static long  webid;
static short vh, win = -1;
static short planes = 32;
static short wx, wy, ww, wh;              /* work area                          */
static short cw, ch;                      /* system font cell                   */
static short min_out_w, min_out_h;
static int   iconified = 0;

static WEBUI ui;
static void *buf;                         /* TT-RAM frame buffer                */
static short buf_w16, buf_h;
static long  buf_bytes;

static short view_ready, engine_ok;
static short view_requested;            /* VIEW_NEW sent on this connection */
static char  pending[WEB_ADDR_MAX];     /* loaded once the view exists */
static char  waittext[160];             /* what the pane says while waiting */
static long  seen_serial, seen_title, seen_uri;
static long  last_flags = -1, last_progress = -1;
static short held;                        /* left button held in the page       */
static short last_mx = -1, last_my = -1;
static short cursor_hand;                 /* POINT_HAND is up                   */
static short m1flag = MO_ENTER;
static GRECT m1r;

static char  title[200];
static char  uri[WEB_ADDR_MAX];
static char  link[WEB_ADDR_MAX];
static char  home[WEB_ADDR_MAX] = DEFAULT_HOME;
static char  status_txt[96];

static short inf_scale_v = 0, inf_js = 1, inf_blocker = 1;
static short inf_pause = 1;             /* pause the page while not on top */
static short view_state = -1;           /* last VIEW_STATE bits sent */
static short on_top = 1;
static short inf_x = -1, inf_y = -1, inf_w = 0, inf_h = 0;

static void arm_m1(void);
static void relayout(void);
static void redraw_all(void);
static void redraw_status(void);
static void web_state(void);

/* ------------------------------------------------------------- .INF ------ */

static long boot_drive_sv(void) { return *(volatile short *)0x446L; }

static void inf_path(char *out)
{
    long d = Supexec(boot_drive_sv);

    if (d < 0 || d > 25)
        d = 2;                              /* C: */
    sprintf(out, "%c:\\WEBGEM.INF", (char)('A' + d));
}

static void inf_load(void)
{
    char path[64], buf_[1024], *p, *e;
    long fh, got;

    inf_path(path);
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
    if ((p = strstr(buf_, "js=")) != NULL)
        inf_js = (short)(atoi(p + 3) != 0);
    if ((p = strstr(buf_, "blocker=")) != NULL)
        inf_blocker = (short)(atoi(p + 8) != 0);
    if ((p = strstr(buf_, "pause=")) != NULL)
        inf_pause = (short)(atoi(p + 6) != 0);
    if ((p = strstr(buf_, "home=")) != NULL) {
        p += 5;
        e = p;
        while (*e && *e != '\r' && *e != '\n') e++;
        if (e - p > 0 && e - p < WEB_ADDR_MAX) {
            memcpy(home, p, (size_t)(e - p));
            home[e - p] = 0;
        }
    }
    if ((p = strstr(buf_, "win=")) != NULL) {
        int x, y, w, h;

        if (sscanf(p + 4, "%d,%d,%d,%d", &x, &y, &w, &h) == 4 && w > 0 && h > 0) {
            inf_x = (short)x; inf_y = (short)y; inf_w = (short)w; inf_h = (short)h;
        }
    }
}

static void inf_save(void)
{
    char path[64], buf_[WEB_ADDR_MAX + 128];
    long fh;
    short cx, cy, cwid, chgt;

    inf_path(path);
    fh = Fcreate(path, 0);
    if (fh < 0)
        return;
    if (win >= 0 && !iconified) {
        wind_get(win, WF_CURRXYWH, &cx, &cy, &cwid, &chgt);
        inf_x = cx; inf_y = cy; inf_w = cwid; inf_h = chgt;
    }
    sprintf(buf_, "scale=%d\r\nwin=%d,%d,%d,%d\r\nhome=%s\r\njs=%d\r\nblocker=%d\r\npause=%d\r\n",
            (int)apj_skin_preferred(), (int)inf_x, (int)inf_y, (int)inf_w, (int)inf_h,
            home, (int)ui.js_on, (int)ui.blocker_on, (int)inf_pause);
    Fwrite((short)fh, (long)strlen(buf_), buf_);
    Fclose((short)fh);
}

/* ------------------------------------------------------------- redraw ---- */

static void draw_all(const GRECT *c)     { webui_draw_clip(&ui, vh, c); }
static void draw_band(const GRECT *c)    { ui.clip = *c; webui_draw_band(&ui, vh);   ui.clip = ui.work; }
static void draw_pane(const GRECT *c)    { ui.clip = *c; webui_draw_pane(&ui, vh);   ui.clip = ui.work; }
static void draw_status(const GRECT *c)  { ui.clip = *c; webui_draw_status(&ui, vh); ui.clip = ui.work; }

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

    if (win < 0 || rw <= 0 || rh <= 0)
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
    else
        redraw(draw_iconic, wx, wy, ww, wh);
}

static void redraw_status(void) { GRECT r; webui_status_rect(&ui, &r); redraw_rect(draw_status, &r); }

static void redraw_widget(short id)
{
    GRECT r;

    webui_widget_rect(&ui, id, &r);
    if (r.g_w > 0)
        redraw_rect(id >= WB_BADGE_LOAD && id <= WB_BADGE_JS ? draw_status : draw_band, &r);
}

/* the damaged rectangles of a new frame, view coordinates -> screen */
static void redraw_damage(const long *rects, int n)
{
    GRECT v, all;
    int i;

    webui_view_rect(&ui, &v);
    if (n <= 0 || iconified)
        return;
    all.g_x = v.g_x; all.g_y = v.g_y; all.g_w = v.g_w; all.g_h = v.g_h;
    redraw_begin(&all);
    for (i = 0; i < n; i++) {
        short x = (short)rects[i * 4], y = (short)rects[i * 4 + 1];
        short w = (short)rects[i * 4 + 2], h = (short)rects[i * 4 + 3];

        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > v.g_w) w = (short)(v.g_w - x);
        if (y + h > v.g_h) h = (short)(v.g_h - y);
        if (w > 0 && h > 0)
            redraw_at(draw_pane, (short)(v.g_x + x), (short)(v.g_y + y), w, h);
    }
    redraw_end();
}

/* ------------------------------------------------------------- buffer ---- */

/* the view's buffer: TT-RAM, width rounded to 16 pixels for the MFDB */
static int ensure_buffer(void)
{
    GRECT v;
    short w16;
    long need;

    webui_view_rect(&ui, &v);
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
        ui.msg = WEB_MSG_NOTT;
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
    if (ui.msg == WEB_MSG_NOTT)
        ui.msg = WEB_MSG_NONE;
    return 1;
}

/* ------------------------------------------------------------- engine ---- */

/* A .URL file (the Windows "internet shortcut" TeraDesk can hand us:
 * "[InternetShortcut]" then "URL=..."): the address inside it. Returns 0
 * when the file is something else. */
static int read_url_file(const char *path, char *out, short outsz)
{
    long fh, n;
    char buf[1024], *p, *e;

    fh = Fopen(path, 0);
    if (fh < 0)
        return 0;
    n = Fread((short)fh, sizeof buf - 1, buf);
    Fclose((short)fh);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    p = strstr(buf, "URL=");
    if (!p)
        return 0;
    p += 4;
    e = p;
    while (*e && *e != '\r' && *e != '\n')
        e++;
    if (e - p >= outsz)
        e = p + outsz - 1;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = '\0';
    return out[0] != '\0';
}

/* Load an address, or a file: a GEMDOS path ending .URL is opened for the
 * address inside it; any other GEMDOS path (an .HTM from TeraDesk) goes to
 * the NatFeat, which maps a HOSTFS path to file:// on the Pi and answers
 * PSWEB_ERR for a drive the Pi cannot see. */
static void web_load(const char *text)
{
    char url[WEB_ADDR_MAX];
    size_t n;
    long r;

    if (!text || !text[0])
        return;
    n = strlen(text);
    if (n > 4 && text[1] == ':' && strcmp(text + n - 4, ".URL") == 0 && read_url_file(text, url, sizeof url))
        text = url;
    on_top = 1;                         /* typed here: it is being looked at */
    web_state();
    r = nf_call(webid | PSWEB_LOAD, 1L, (long)text);
    /* every outcome is said, in the status strip: a load that silently
     * went nowhere is not a thing this window does */
    switch (r) {
        case 0:   sprintf(status_txt, "Loading %.60s", text); break;
        case -1L: sprintf(status_txt, "Not on a HOSTFS drive - the Pi cannot read it"); break;
        case -3L: sprintf(status_txt, "psweb link busy - try again"); break;
        case -4L: sprintf(status_txt, "psweb not connected"); break;
        default:  sprintf(status_txt, "PSWEB LOAD answered %ld", r); break;
    }
    redraw_status();
}

static void web_nav(long op)
{
    nf_call(webid | PSWEB_NAV, 1L, op);
}

static void web_setting(long key, long value)
{
    nf_call(webid | PSWEB_SETTING, 1L, key, value, 0L);
}

/*
 * What the engine may do: bit0 visible, bit1 focused, bit2 topped. With
 * pause=1 (the default) a window that is behind another or iconified is
 * reported hidden, so WebKit stops painting and throttles the page's
 * timers - the JIT's share of the shared L2 comes back while the browser
 * is not the thing being looked at. The last frame stays on screen.
 */
static void web_state(void)
{
    short bits;

    if (iconified)
        bits = 0;
    else if (on_top)
        bits = 7;
    else
        bits = inf_pause ? 0 : 1;
    if (bits == view_state || !view_ready)
        return;
    view_state = bits;
    nf_call(webid | PSWEB_VIEW_STATE, 1L, (long)bits);
    if (bits == 0 && !iconified) {
        strcpy(status_txt, "Paused while not on top (pause=0 in WEBGEM.INF to keep running)");
        redraw_status();
    } else if (bits == 7 && status_txt[0] == 'P') {
        status_txt[0] = '\0';
        redraw_status();
    }
}

static void web_pointer(long kind, short mx, short my, long button, short ks)
{
    GRECT v;

    webui_view_rect(&ui, &v);
    nf_call(webid | PSWEB_POINTER, 1L, kind, (long)(mx - v.g_x),
            (long)(my - v.g_y), button, (long)ks);
}

static void web_scroll(long notches)
{
    short mx, my, mb, ks;
    GRECT v;

    webui_view_rect(&ui, &v);
    graf_mkstate(&mx, &my, &mb, &ks);
    nf_call(webid | PSWEB_SCROLL, 1L, 0L, notches * 120L,
            (long)(mx - v.g_x), (long)(my - v.g_y), 0L);
}

static void web_key(short kr, short ks)
{
    nf_call(webid | PSWEB_KEY, 1L, 1L, (long)(kr & 0xffff), (long)ks);
    nf_call(webid | PSWEB_KEY, 1L, 0L, (long)(kr & 0xffff), (long)ks);
}

static int in_view(short mx, short my)
{
    GRECT v;

    webui_view_rect(&ui, &v);
    return mx >= v.g_x && my >= v.g_y && mx < v.g_x + v.g_w && my < v.g_y + v.g_h;
}

static void set_cursor(short hand)
{
    if (hand == cursor_hand)
        return;
    cursor_hand = hand;
    graf_mouse(hand ? POINT_HAND : ARROW, NULL);
}

/* ------------------------------------------------------------- window ---- */

static void compute_min_window(void)
{
    short mw, mh, cx, cy, cwid, chgt;

    webui_minsize(vh, &mw, &mh);
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

/* after any geometry change: relayout, and if the view changed size a new
 * buffer and a resized engine view; the next frame is a full one */
static void relayout(void)
{
    GRECT before, after;

    webui_view_rect(&ui, &before);
    wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
    webui_layout(&ui, vh, wx, wy, ww, wh);
    webui_view_rect(&ui, &after);
    if (after.g_w != before.g_w || after.g_h != before.g_h) {
        ensure_buffer();
        if (view_ready && after.g_w > 0 && after.g_h > 0)
            nf_call(webid | PSWEB_VIEW_SIZE, 1L, (long)after.g_w, (long)after.g_h);
        ui.pagebuf.fd_addr = NULL;          /* until the engine draws the new size */
        seen_serial = 0;
    }
}

/* the natural work-area size at this scale: the 1280x678 view phase 0
 * liked plus the bands, never under the minimum, never over the desktop */
static void natural_size(short *w, short *h)
{
    short dx, dy, dw, dh, mw, mh;
    short sw, sh, tw, th, stath, d;

    webui_minsize(vh, &mw, &mh);
    *w = 1280;
    /* the bands: what minsize reserves above and below a PAGEMIN page */
    vst_point(vh, 10, &d, &d, &d, &d);
    (void)sw; (void)sh; (void)tw; (void)th; (void)stath;
    *h = (short)(678 + (mh - apj_skin_m(120)));
    if (*w < mw) *w = mw;
    if (*h < mh) *h = mh;
    wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
    if (*w > dw) *w = dw;
    if (*h > dh) *h = dh;
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
    const APJ_LAY *l = ui.hover >= 0 ? apj_lay_find(ui.lay, ui.nlay, ui.hover) : NULL;
    short mx, my, mb, ks;

    if (l) {
        m1r.g_x = l->x; m1r.g_y = l->y; m1r.g_w = l->w; m1r.g_h = l->h;
        m1flag = MO_LEAVE;
        return;
    }
    webui_bbox(&ui, &m1r);
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
    short h = webui_hit(&ui, mx, my);
    short old = ui.hover;

    if (h == WB_PAGE || h == WB_ADDRESS)
        h = -1;
    if (h != old) {
        GRECT a, b;

        ui.hover = h;
        webui_widget_rect(&ui, old, &a);
        webui_widget_rect(&ui, h, &b);
        if (!iconified && (a.g_w > 0 || b.g_w > 0)) {
            GRECT u = a.g_w > 0 ? a : b;

            if (a.g_w > 0 && b.g_w > 0) {
                short x1 = (short)(a.g_x + a.g_w > b.g_x + b.g_w ? a.g_x + a.g_w : b.g_x + b.g_w);
                short y1 = (short)(a.g_y + a.g_h > b.g_y + b.g_h ? a.g_y + a.g_h : b.g_y + b.g_h);

                u.g_x = a.g_x < b.g_x ? a.g_x : b.g_x;
                u.g_y = a.g_y < b.g_y ? a.g_y : b.g_y;
                u.g_w = (short)(x1 - u.g_x);
                u.g_h = (short)(y1 - u.g_y);
            }
            redraw_begin(&u);
            if (a.g_w > 0) redraw_at(old >= WB_BADGE_LOAD && old <= WB_BADGE_JS ? draw_status : draw_band, a.g_x, a.g_y, a.g_w, a.g_h);
            if (b.g_w > 0) redraw_at(h >= WB_BADGE_LOAD && h <= WB_BADGE_JS ? draw_status : draw_band, b.g_x, b.g_y, b.g_w, b.g_h);
            redraw_end();
        }
    }
    arm_m1();
}

/* ------------------------------------------------------------- address --- */

static void address_focus(void)
{
    strncpy(ui.addr, uri, WEB_ADDR_MAX - 1);
    ui.addr[WEB_ADDR_MAX - 1] = '\0';
    ui.addr_all = 1;
    ui.focus = WEB_FOCUS_ADDRESS;
    redraw_widget(WB_ADDRESS);
}

static void address_leave(void)
{
    ui.focus = WEB_FOCUS_NONE;
    ui.addr_all = 0;
    redraw_widget(WB_ADDRESS);
}

static void field_key(char c, short scan)
{
    size_t n = strlen(ui.addr);

    if (c == 0x1b) {                        /* escape: give the focus up */
        address_leave();
        return;
    }
    if (c == '\r' || c == '\n') {
        char text[WEB_ADDR_MAX];

        strcpy(text, ui.addr);
        address_leave();
        web_load(text);
        return;
    }
    if (c == 8 || scan == 0x0E) {           /* backspace */
        if (ui.addr_all) { ui.addr[0] = '\0'; ui.addr_all = 0; }
        else if (n) ui.addr[n - 1] = '\0';
        redraw_widget(WB_ADDRESS);
        return;
    }
    if ((unsigned char)c >= 32) {
        if (ui.addr_all) { ui.addr[0] = '\0'; ui.addr_all = 0; n = 0; }
        if (n < WEB_ADDR_MAX - 1) {
            ui.addr[n] = c;
            ui.addr[n + 1] = '\0';
        }
        redraw_widget(WB_ADDRESS);
    }
}

/* ------------------------------------------------------------- state ----- */

static void sync_strings(const POLLSTATE *st)
{
    char t[200];
    long n;

    if (st->title_serial != seen_title) {
        n = nf_call(webid | PSWEB_GETSTR, 1L, (long)PSWEB_STR_TITLE, (long)t, 190L);
        if (n >= 0) {
            seen_title = st->title_serial;
            if (n == 0) strcpy(t, "PiSTorm Web");
            if (strcmp(t, title) != 0) {
                strcpy(title, t);
                wind_set_str(win, WF_NAME, title);
            }
        }
    }
    if (st->uri_serial != seen_uri) {
        char u[WEB_ADDR_MAX];

        n = nf_call(webid | PSWEB_GETSTR, 1L, (long)PSWEB_STR_URI, (long)u, (long)WEB_ADDR_MAX);
        if (n >= 0) {
            seen_uri = st->uri_serial;
            if (strcmp(u, uri) != 0) {
                strcpy(uri, u);
                if (ui.focus != WEB_FOCUS_ADDRESS)
                    redraw_widget(WB_ADDRESS);
            }
        }
    }
}

static void sync_flags(const POLLSTATE *st)
{
    short band = 0, stat = 0;
    long f = st->flags;

    if (f != last_flags) {
        short loading = (f & PSWEB_FL_LOADING) != 0;
        short js = (f & PSWEB_FL_JS_OFF) == 0;
        short blocker = (f & PSWEB_FL_BLOCKER) != 0;
        short secure = (f & PSWEB_FL_SECURE) != 0;

        if (loading != ui.loading || secure != ui.secure) band = 1;
        if (loading != ui.loading || js != ui.js_on || blocker != ui.blocker_on) stat = 1;
        ui.loading = loading;
        ui.secure = secure;
        ui.js_on = js;
        ui.blocker_on = blocker;
        ui.can_back = (f & PSWEB_FL_CAN_BACK) != 0;
        ui.can_fwd = (f & PSWEB_FL_CAN_FWD) != 0;
        if ((f & PSWEB_FL_CRASHED) && ui.msg == WEB_MSG_NONE) {
            ui.msg = WEB_MSG_CRASHED;
            ui.pagebuf.fd_addr = NULL;
            redraw_rect(draw_pane, &ui.view);
        }
        last_flags = f;
    }
    if (st->progress != last_progress) {
        short p = (short)st->progress;

        /* the rail and the badge move in steps of 5%: cheaper than 1% */
        if (ui.loading && (p / 50 != ui.progress / 50 || p == 1000)) {
            ui.progress = p;
            band = 1;
            stat = 1;
        } else
            ui.progress = p;
        last_progress = st->progress;
    }
    if (band) redraw_widget(WB_ADDRESS), redraw_widget(WB_RELOAD);
    if (stat) redraw_status();
}

/* one tick: poll, fetch what changed, put it on the screen */
/*
 * The view is asked for only once the link is up, and again after every
 * reconnect (psweb is socket-activated and exits when idle, and the
 * emulator's link comes up a moment after the first NatFeat call): a
 * VIEW_NEW sent before "connected" is dropped, which is what left the
 * window on "Connecting..." for good.
 */
static void request_view(void)
{
    web_setting(PSWEB_SET_JAVASCRIPT, ui.js_on ? 1L : 0L);
    web_setting(PSWEB_SET_BLOCKER, ui.blocker_on ? 1L : 0L);
    if (nf_call(webid | PSWEB_VIEW_NEW, (long)ui.view.g_w, (long)ui.view.g_h, (long)planes, 0L) > 0)
        view_requested = 1;
}

/* the link went away (psweb gone, or never there yet): back to waiting,
 * and whatever was showing is loaded again when it returns */
static void link_lost(void)
{
    if (view_ready && uri[0] && !pending[0])
        strcpy(pending, uri);
    view_ready = 0;
    view_requested = 0;
    view_state = -1;
    ui.pagebuf.fd_addr = NULL;
    seen_serial = 0;
    if (ui.msg != WEB_MSG_WAIT) {
        ui.msg = WEB_MSG_WAIT;
        redraw_rect(draw_pane, &ui.view);
    }
}

static void tick(void)
{
    POLLSTATE st;
    long rects[MAX_RECTS * 4];
    long n;
    short mx, my, mb, ks;
    int inside;

    if (!view_ready) {
        long s = nf_call(webid | PSWEB_STATUS);
        char t[160];

        engine_ok = (s & 2) != 0;
        if (s & 4) {
            view_ready = 1;
            ui.msg = WEB_MSG_NONE;
            status_txt[0] = '\0';
            waittext[0] = '\0';
            redraw_rect(draw_pane, &ui.view);
            view_state = -1;
            web_state();
            if (pending[0]) {
                web_load(pending);
                pending[0] = '\0';
            }
            return;
        }
        if (engine_ok) {
            if (!view_requested)
                request_view();
        } else
            view_requested = 0;
        /* say what the link is doing - the emulator knows why it waits */
        if (nf_call(webid | PSWEB_GETSTR, 1L, (long)PSWEB_STR_STATUS, (long)t, (long)sizeof t) > 0 &&
            strcmp(t, waittext) != 0) {
            strcpy(waittext, t);
            ui.msg = WEB_MSG_WAIT;
            redraw_rect(draw_pane, &ui.view);
        } else if (ui.msg != WEB_MSG_WAIT) {
            ui.msg = WEB_MSG_WAIT;
            redraw_rect(draw_pane, &ui.view);
        }
        return;
    }

    /* whether the window is on top, from the AES itself rather than
     * from the WM_TOPPED/WM_UNTOPPED stream, which can miss a step: a
     * page reported hidden by mistake would load without painting */
    {
        short top = 0, d;

        wind_get(0, WF_TOP, &top, &d, &d, &d);
        if ((top == win) != (on_top != 0)) {
            on_top = (top == win);
            web_state();
        }
    }

    /* the pointer over the page: motion and drags, from here, at the
     * tick rate; MU_M1 is for the toolbar only */
    graf_mkstate(&mx, &my, &mb, &ks);
    inside = in_view(mx, my);
    if (held) {
        if (mb & 1) {
            if (mx != last_mx || my != last_my) {
                web_pointer(0L, mx, my, 0L, ks);
                last_mx = mx; last_my = my;
            }
        } else {
            web_pointer(2L, mx, my, 1L, ks);
            held = 0;
        }
    } else if (inside && (mx != last_mx || my != last_my)) {
        web_pointer(0L, mx, my, 0L, ks);
        last_mx = mx; last_my = my;
    }
    if (!inside && !held && (last_mx >= 0)) {
        /* the pointer left the page: one move outside, and back to an arrow */
        if (in_view(last_mx, last_my))
            web_pointer(0L, mx, my, 0L, ks);
        last_mx = mx; last_my = my;
        set_cursor(0);
    }

    if (nf_call(webid | PSWEB_POLL, 1L, (long)&st) < 0) {
        if (!(nf_call(webid | PSWEB_STATUS) & 4))
            link_lost();
        return;
    }
    sync_strings(&st);
    sync_flags(&st);
    if (inside && !held)
        set_cursor((short)(st.cursor != 0));
    if (st.cursor != 0 || link[0]) {
        long ln = nf_call(webid | PSWEB_GETSTR, 1L, (long)PSWEB_STR_LINK, (long)rects, (long)sizeof rects);

        if (ln >= 0 && strcmp((char *)rects, link) != 0) {
            strncpy(link, (char *)rects, WEB_ADDR_MAX - 1);
            link[WEB_ADDR_MAX - 1] = '\0';
            redraw_status();
        }
    }

    if (st.frame_serial == seen_serial || !buf)
        return;
    n = nf_call(webid | PSWEB_FETCH, 1L, (long)buf,
                (long)buf_w16 * (planes / 8), (long)buf_h, (long)rects, (long)MAX_RECTS);
    if (n <= 0)
        return;
    seen_serial = st.frame_serial;
    if (n > MAX_RECTS) n = MAX_RECTS;
    if (ui.pagebuf.fd_addr == NULL || ui.msg != WEB_MSG_NONE) {
        /* the first frame at this size: the whole view */
        ui.pagebuf.fd_addr = buf;
        ui.msg = WEB_MSG_NONE;
        redraw_rect(draw_pane, &ui.view);
        return;
    }
    redraw_damage(rects, (int)n);
}

/* ------------------------------------------------------------- input ----- */

static void menu_popup_simple(void)
{
    short r = form_alert(1, "[2][WEBGEM|The PiSTorm web browser.|Pages are rendered by psweb|on the Pi with WPE WebKit.][Reload|No cache| OK ]");

    if (r == 1) web_nav(NAV_RELOAD);
    else if (r == 2) web_nav(NAV_NOCACHE);
}

static void click(short mx, short my, short mb, short ks)
{
    short id = webui_hit(&ui, mx, my);

    /* a click anywhere takes the keyboard focus off the field */
    if (id != WB_ADDRESS && ui.focus == WEB_FOCUS_ADDRESS)
        address_leave();

    switch (id) {
        case WB_PAGE:
            ui.focus = WEB_FOCUS_PAGE;
            if (mb & 1) {
                web_pointer(0L, mx, my, 0L, ks);
                web_pointer(1L, mx, my, 1L, ks);
                held = 1;
                last_mx = mx; last_my = my;
            } else if (mb & 2) {
                web_pointer(1L, mx, my, 2L, ks);
                web_pointer(2L, mx, my, 2L, ks);
            }
            break;
        case WB_BACK:    if (mb & 1) web_nav(NAV_BACK); break;
        case WB_FWD:     if (mb & 1) web_nav(NAV_FWD); break;
        case WB_RELOAD:  if (mb & 1) web_nav(ui.loading ? NAV_STOP : NAV_RELOAD); break;
        case WB_HOME:    if (mb & 1) web_load(home); break;
        case WB_ADDRESS:
            if (ui.focus != WEB_FOCUS_ADDRESS) address_focus();
            else if (ui.addr_all) { ui.addr_all = 0; redraw_widget(WB_ADDRESS); }
            break;
        case WB_BOOKMARK:
            /* phase 6: bookmarks. For now: the home page is the bookmark */
            if (mb & 1) {
                strncpy(home, uri, WEB_ADDR_MAX - 1);
                home[WEB_ADDR_MAX - 1] = '\0';
                inf_save();
                sprintf(status_txt, "Home page set");
                redraw_status();
            }
            break;
        case WB_DOWNLOAD:
            if (mb & 1) {
                sprintf(status_txt, "Downloads go to S:\\DOWNLOADS (phase 5)");
                redraw_status();
            }
            break;
        case WB_MENU:    if (mb & 1) menu_popup_simple(); break;
        case WB_BADGE_JS:
            if (mb & 1) web_setting(PSWEB_SET_JAVASCRIPT, ui.js_on ? 0L : 1L);
            break;
        case WB_BADGE_BLOCK:
            if (mb & 1) web_setting(PSWEB_SET_BLOCKER, ui.blocker_on ? 0L : 1L);
            break;
        default:
            break;
    }
}

static void key(short kr, short ks)
{
    char  c    = (char)(kr & 0xff);
    short scan = (short)((kr >> 8) & 0xff);

    if (ui.focus == WEB_FOCUS_ADDRESS) {
        field_key(c, scan);
        return;
    }
    /* the browser's own keys */
    if (ks & K_CTRL) {
        switch (scan) {
            case 0x26: address_focus(); return;                 /* Ctrl+L */
            case 0x13: web_nav(NAV_RELOAD); return;             /* Ctrl+R */
            case 0x1a: web_nav(NAV_BACK); return;               /* Ctrl+[ */
            case 0x1b: web_nav(NAV_FWD); return;                /* Ctrl+] */
            case 0x0d: case 0x4e: nf_call(webid | PSWEB_ZOOM, 1L, 125L); return; /* Ctrl++ */
            case 0x0c: case 0x4a: nf_call(webid | PSWEB_ZOOM, 1L, 80L); return;  /* Ctrl+- */
            case 0x0b: nf_call(webid | PSWEB_ZOOM, 1L, 100L); return;            /* Ctrl+0 */
            case 0x10: return;                                  /* Ctrl+Q: main quits */
            default: break;
        }
    }
    if (ks & K_ALT) {
        switch (scan) {
            case 0x4b: web_nav(NAV_BACK); return;               /* Alt+Left  */
            case 0x4d: web_nav(NAV_FWD); return;                /* Alt+Right */
            case 0x02: set_scale(100); return;                  /* Alt+1     */
            case 0x03: set_scale(125); return;                  /* Alt+2     */
            case 0x04: set_scale(175); return;                  /* Alt+3     */
            case 0x0b: set_scale(0); return;                    /* Alt+0     */
            default: break;
        }
    }
    if (scan == 0x3f) { web_nav(NAV_RELOAD); return; }          /* F5 */
    if (scan == 0x01 && ui.loading) { web_nav(NAV_STOP); return; } /* Esc */
    web_key(kr, ks);
}

/* ------------------------------------------------------------- main ------ */

int main(int argc, char **argv)
{
    short work_in[11], work_out[57];
    short d, msg[8];
    short mx, my, mb, ks, kr, brk;
    short ev, i;
    const char *url = argc > 1 ? argv[1] : NULL;
    if (appl_init() < 0)
        return 1;
    webid = nf_id("PSWEB");
    if (!webid) {
        form_alert(1, "[3][PSWEB NatFeat not found.|Run under the PiSTorm emulator|built with psweb support.][ OK ]");
        appl_exit();
        return 1;
    }

    vh = graf_handle(&cw, &ch, &d, &d);
    for (i = 0; i < 10; i++) work_in[i] = 1;
    work_in[10] = 2;
    v_opnvwk(work_in, &vh, work_out);
    vst_alignment(vh, 0, 5, &d, &d);              /* left / top text origin */
    apj_init(vh);                                 /* theme + renderer, before any window */
    inf_load();                                   /* C:\WEBGEM.INF */
    apj_skin_prefer(inf_scale_v);                 /* saved scale, else by screen */
    apj_skin_load(vh, NULL);                      /* follows the theme; may fail */

    planes = apj_skin_planes();
    if (planes != 16 && planes != 32) {
        short ext[57];

        vq_extnd(vh, 1, ext);
        planes = ext[4];
    }
    if (planes != 16 && planes != 32) {
        form_alert(1, "[3][WEBGEM needs a 16 or 32 bit|screen.][ OK ]");
        v_clsvwk(vh);
        appl_exit();
        return 1;
    }

    memset(&ui, 0, sizeof(ui));
    ui.uri = uri;
    ui.link = link;
    ui.status = status_txt;
    ui.hover = -1;
    ui.press = -1;
    ui.js_on = inf_js;
    ui.blocker_on = inf_blocker;
    ui.ntabs = 1;
    ui.msg = WEB_MSG_WAIT;
    strcpy(title, "PiSTorm Web");
    strcpy(status_txt, "Starting...");

    {
        short dx, dy, dw, dh, cx, cy, cwid, chgt;
        short want_w, want_h;

        compute_min_window();
        natural_size(&want_w, &want_h);
        wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
        wind_calc(WC_BORDER, WIN_KIND,
                  (short)(dx + 16), (short)(dy + 16), want_w, want_h, &cx, &cy, &cwid, &chgt);
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
        wind_set_str(win, WF_NAME, title);
        wind_open(win, cx, cy, cwid, chgt);
        wind_set(win, WF_WHEEL, 1, WHEEL_ARROWED, 0, 0);
        apply_min_size();                  /* a saved window under the minimum grows */
        wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
        webui_layout(&ui, vh, wx, wy, ww, wh);
    }
    ensure_buffer();
    arm_m1();
    redraw_all();

    /* the engine: the emulator reconnects by itself; wait a moment for it */
    {
        long s = nf_call(webid | PSWEB_STATUS);

        if (!(s & 2)) {
            long t = Supexec(boot_drive_sv);     /* any cheap supervisor call */

            (void)t;
        }
    }
    /* the view and the first page follow once the link is up (tick) */
    strncpy(pending, url ? url : home, WEB_ADDR_MAX - 1);
    pending[WEB_ADDR_MAX - 1] = '\0';
    ui.waittext = waittext;
    tick();

    for (;;) {
        ev = evnt_multi(MU_MESAG | MU_BUTTON | MU_KEYBD | MU_TIMER | MU_M1,
                        1, 3, 1,
                        m1flag, m1r.g_x, m1r.g_y, m1r.g_w, m1r.g_h,
                        0, 0, 0, 0, 0,
                        msg, (view_ready && (ui.loading || held || in_view(last_mx, last_my))) ? 16UL : 50UL,
                        &mx, &my, &mb, &ks, &kr, &brk);

        if (ev & MU_MESAG) {
            switch (msg[0]) {
                case WM_REDRAW:
                    redraw(draw_all, msg[4], msg[5], msg[6], msg[7]);
                    break;
                case WM_TOPPED:
                    wind_set(win, WF_TOP, 0, 0, 0, 0);
                    on_top = 1;
                    web_state();
                    break;
                case WM_ONTOP:
                    on_top = 1;
                    web_state();
                    break;
                case WM_UNTOPPED:
                case WM_BOTTOMED:
                    on_top = 0;
                    web_state();
                    break;
                case WM_MOVED:
                    wind_set(win, WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
                    relayout();           /* the AES moves the pixels itself */
                    arm_m1();
                    break;
                case WM_SIZED:
                case WM_FULLED: {
                    if (msg[0] == WM_FULLED)
                        wind_get(win, WF_FULLXYWH, &msg[4], &msg[5], &msg[6], &msg[7]);
                    if (msg[6] < min_out_w) msg[6] = min_out_w;
                    if (msg[7] < min_out_h) msg[7] = min_out_h;
                    wind_set(win, WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
                    relayout();
                    arm_m1();
                    redraw_all();
                    break;
                }
                case WM_ARROWED: {    /* the mouse wheel, when the AES sends it */
                    short n = (short)((msg[4] >> 8) & 0xff);

                    if (n < 1) n = 1;
                    switch (msg[4] & 15) {
                        case WA_UPLINE: web_scroll(-n); break;
                        case WA_DNLINE: web_scroll(n); break;
                        case WA_UPPAGE: web_scroll(-5); break;
                        case WA_DNPAGE: web_scroll(5); break;
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
                    web_state();
                    break;
                case WM_UNICONIFY:
                    wind_set(win, WF_UNICONIFY, msg[4], msg[5], msg[6], msg[7]);
                    iconified = 0;
                    relayout();
                    arm_m1();
                    redraw_all();
                    web_state();
                    break;
                case APJ_SKINCHG:         /* the desktop changed theme */
                    apj_init(vh);
                    apj_skin_reload(vh);
                    compute_min_window();
                    apply_min_size();
                    relayout();
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
                    if (cmd && cmd[0])
                        web_load(cmd);
                    wind_set(win, WF_TOP, 0, 0, 0, 0);
                    reply[0] = AV_STARTED; reply[1] = gl_apid; reply[2] = 0;
                    reply[3] = msg[3]; reply[4] = msg[4];
                    reply[5] = reply[6] = reply[7] = 0;
                    appl_write(msg[1], 16, reply);
                    break;
                }
                case AP_TERM:
                    goto out;
            }
        }
        if ((ev & MU_M1) && !iconified)
            set_hover(mx, my);
        if ((ev & MU_BUTTON) && !iconified) {
            if (in_view(mx, my) || webui_hit(&ui, mx, my) >= 0)
                wind_set(win, WF_TOP, 0, 0, 0, 0);
            click(mx, my, mb, ks);
        }
        if (ev & MU_KEYBD) {
            if ((ks & K_ALT) && (((kr >> 8) & 0xff) == 0x2d))          /* Alt+X */
                goto out;
            if ((ks & K_CTRL) && (((kr >> 8) & 0xff) == 0x10))         /* Ctrl+Q */
                goto out;
            key(kr, ks);
        }
        if (ev & MU_TIMER)
            tick();
    }

out:
    inf_save();
    nf_call(webid | PSWEB_VIEW_FREE);
    if (buf) Mfree(buf);
    apj_skin_free();
    set_cursor(0);
    if (win >= 0) {
        wind_close(win);
        wind_delete(win);
    }
    v_clsvwk(vh);
    appl_exit();
    return 0;
}
