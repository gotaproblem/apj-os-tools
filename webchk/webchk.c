/*
 * webchk.c - PSWEB NatFeat test tool: a web page in a GEM window.
 *
 * The smallest program that shows a page rendered by psweb on the Pi and
 * lets you click, scroll and type in it. No toolbar, no skin: this is the
 * engine link on its own, so that the link can be tested before WEBGEM
 * exists. Everything WEBGEM will do with the page pane is here.
 *
 *     WEBCHK                          opens the default page
 *     WEBCHK https://example.org      opens that
 *
 * Keys in the window: Alt+Left / Alt+Right back and forward, F5 reload,
 * Esc stop, Alt+X quit. Everything else goes to the page. The mouse wheel
 * scrolls (WM_ARROWED, and the USB bridge's cursor-key taps).
 *
 * Build:   make        (needs m68k-atari-mint-gcc + gemlib)
 */
#include <gem.h>
#include <osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define PSWEB_STR_TITLE   0
#define PSWEB_STR_URI     1
#define PSWEB_STR_LINK    2
#define PSWEB_STR_STATUS  3

#define PSWEB_FL_LOADING  0x0004
#define PSWEB_FL_CRASHED  0x0040

#define MAX_RECTS 32
#define DEFAULT_URL "https://en.wikipedia.org/wiki/Atari_ST"
#define WIN_KIND (NAME | CLOSER | MOVER | SIZER | FULLER)

/* the eight big-endian longs POLL writes - native order on the 68k */
typedef struct {
    long frame_serial, n_damage, progress, flags, cursor, title_serial, uri_serial, dialog;
} POLLSTATE;

static long  webid;
static short vh, win = -1;
static short planes = 32;
static GRECT work;                 /* window work area on screen        */
static void *buf;                  /* TT-RAM frame buffer               */
static short buf_w16, buf_h;
static MFDB  fb;
static long  seen_serial, seen_title;
static short view_ready;
static short held;                 /* mouse button held in the page     */
static short last_mx = -1, last_my = -1;
static char  title[256];

static long get200(void) { return *(volatile long *)0x4baL; }
static long ticks(void)  { return Supexec(get200); }

/* ------------------------------------------------------------ buffer ---- */

static int ensure_buffer(void)
{
    short w16 = (short)((work.g_w + 15) & ~15);
    long need = (long)w16 * work.g_h * (planes / 8);

    if (work.g_w <= 0 || work.g_h <= 0)
        return 0;
    if (buf && buf_w16 == w16 && buf_h == work.g_h)
        return 1;
    if (buf) { Mfree(buf); buf = NULL; }
    buf = (void *)Mxalloc(need, 1);            /* TT-RAM only: the host writes it */
    if (!buf)
        return 0;
    memset(buf, 0, need);
    buf_w16 = w16;
    buf_h = work.g_h;
    fb.fd_addr = buf;
    fb.fd_w = w16;
    fb.fd_h = work.g_h;
    fb.fd_wdwidth = (short)(w16 / 16);
    fb.fd_stand = 0;
    fb.fd_nplanes = planes;
    fb.fd_r1 = fb.fd_r2 = fb.fd_r3 = 0;
    return 1;
}

/* ------------------------------------------------------------ drawing --- */

static short mouse_off;

static void redraw_begin(const GRECT *area)
{
    short mx, my, mb, ks;

    wind_update(BEG_UPDATE);
    mouse_off = 1;
    graf_mkstate(&mx, &my, &mb, &ks);
    if (area && (mx < area->g_x || my < area->g_y ||
                 mx >= area->g_x + area->g_w || my >= area->g_y + area->g_h))
        mouse_off = 0;
    if (mouse_off)
        graf_mouse(M_OFF, NULL);
}

static void redraw_end(void)
{
    short d[4];

    vs_clip(vh, 0, d);
    if (mouse_off)
        graf_mouse(M_ON, NULL);
    wind_update(END_UPDATE);
}

/* blit view rectangle (x,y,w,h) from the buffer to the screen, inside clip */
static void blit(short x, short y, short w, short h, const GRECT *clip)
{
    MFDB scr;
    short pxy[8], c[4];
    GRECT d;

    d.g_x = (short)(work.g_x + x);
    d.g_y = (short)(work.g_y + y);
    d.g_w = w;
    d.g_h = h;
    if (!rc_intersect(clip, &d))
        return;
    c[0] = d.g_x; c[1] = d.g_y;
    c[2] = (short)(d.g_x + d.g_w - 1); c[3] = (short)(d.g_y + d.g_h - 1);
    vs_clip(vh, 1, c);

    scr.fd_addr = NULL;
    pxy[0] = (short)(d.g_x - work.g_x);
    pxy[1] = (short)(d.g_y - work.g_y);
    pxy[2] = (short)(pxy[0] + d.g_w - 1);
    pxy[3] = (short)(pxy[1] + d.g_h - 1);
    pxy[4] = d.g_x;
    pxy[5] = d.g_y;
    pxy[6] = (short)(d.g_x + d.g_w - 1);
    pxy[7] = (short)(d.g_y + d.g_h - 1);
    vro_cpyfm(vh, S_ONLY, pxy, &fb, &scr);
}

/* draw the given view rects through the AES rectangle list */
static void draw_rects(const long *rects, int n)
{
    GRECT r;
    int i;

    if (!buf || win < 0)
        return;
    wind_get(win, WF_FIRSTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
    while (r.g_w && r.g_h) {
        for (i = 0; i < n; i++)
            blit((short)rects[i * 4], (short)rects[i * 4 + 1],
                 (short)rects[i * 4 + 2], (short)rects[i * 4 + 3], &r);
        wind_get(win, WF_NEXTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
    }
}

static void redraw_area(const GRECT *a)
{
    GRECT r, d = *a;
    long full[4];

    if (!buf)
        return;
    full[0] = 0; full[1] = 0; full[2] = work.g_w; full[3] = work.g_h;
    redraw_begin(&d);
    wind_get(win, WF_FIRSTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
    while (r.g_w && r.g_h) {
        GRECT i = r;
        if (rc_intersect(&d, &i))
            blit(0, 0, work.g_w, work.g_h, &i);
        wind_get(win, WF_NEXTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
    }
    (void)full;
    redraw_end();
}

/* ------------------------------------------------------------ the link -- */

static void set_title(void)
{
    char t[256];
    long n = nf_call(webid | PSWEB_GETSTR, 1L, (long)PSWEB_STR_TITLE, (long)t, 200L);

    if (n < 0)
        return;                          /* busy: next tick */
    if (n == 0)
        strcpy(t, "PiSTorm Web");
    if (strcmp(t, title) != 0) {
        strcpy(title, t);
        wind_set_str(win, WF_NAME, title);
    }
}

static void resize_view(void)
{
    if (!ensure_buffer())
        return;
    nf_call(webid | PSWEB_VIEW_SIZE, 1L, (long)work.g_w, (long)work.g_h);
}

static void pointer(long kind, short mx, short my, long button, short ks)
{
    nf_call(webid | PSWEB_POINTER, 1L, kind, (long)(mx - work.g_x),
            (long)(my - work.g_y), button, (long)ks);
}

static int in_work(short mx, short my)
{
    return mx >= work.g_x && my >= work.g_y &&
           mx < work.g_x + work.g_w && my < work.g_y + work.g_h;
}

/* one frame: poll, fetch what changed, put it on the screen */
static void tick(void)
{
    POLLSTATE st;
    long rects[MAX_RECTS * 4];
    long n;
    short mx, my, mb, ks;

    if (!view_ready) {
        long s = nf_call(webid | PSWEB_STATUS);
        if (s & 4) {
            view_ready = 1;
            wind_set_str(win, WF_NAME, "PiSTorm Web - loading");
        }
        return;
    }

    /* drags: the AES reports the press, we follow it ourselves */
    graf_mkstate(&mx, &my, &mb, &ks);
    if (held) {
        if (mb & 1) {
            if (mx != last_mx || my != last_my) {
                pointer(0L, mx, my, 0L, ks);
                last_mx = mx; last_my = my;
            }
        } else {
            pointer(2L, mx, my, 1L, ks);
            held = 0;
        }
    } else if (in_work(mx, my) && (mx != last_mx || my != last_my)) {
        pointer(0L, mx, my, 0L, ks);
        last_mx = mx; last_my = my;
    }

    if (nf_call(webid | PSWEB_POLL, 1L, (long)&st) < 0)
        return;
    if (st.title_serial != seen_title) {
        set_title();
        seen_title = st.title_serial;
    }
    if (st.frame_serial == seen_serial || !buf)
        return;

    n = nf_call(webid | PSWEB_FETCH, 1L, (long)buf,
                (long)buf_w16 * (planes / 8), (long)buf_h, (long)rects, (long)MAX_RECTS);
    if (n <= 0)
        return;
    seen_serial = st.frame_serial;
    if (n > MAX_RECTS) n = MAX_RECTS;
    redraw_begin(&work);
    draw_rects(rects, (int)n);
    redraw_end();
}

static void scroll_notches(long n)
{
    short mx, my, mb, ks;

    graf_mkstate(&mx, &my, &mb, &ks);
    nf_call(webid | PSWEB_SCROLL, 1L, 0L, n * 120L,
            (long)(mx - work.g_x), (long)(my - work.g_y), 0L);
}

static void key(short kr, short ks)
{
    short scan = (short)((kr >> 8) & 0xff);
    short ch = (short)(kr & 0xff);

    if (ks & K_ALT) {
        if (scan == 0x4b) { nf_call(webid | PSWEB_NAV, 1L, 0L); return; }   /* back    */
        if (scan == 0x4d) { nf_call(webid | PSWEB_NAV, 1L, 1L); return; }   /* forward */
    }
    if (scan == 0x3f) { nf_call(webid | PSWEB_NAV, 1L, 2L); return; }       /* F5 reload */
    if (scan == 0x01) { nf_call(webid | PSWEB_NAV, 1L, 4L); return; }       /* Esc stop  */
    (void)ch;
    nf_call(webid | PSWEB_KEY, 1L, 1L, (long)(kr & 0xffff), (long)ks);
    nf_call(webid | PSWEB_KEY, 1L, 0L, (long)(kr & 0xffff), (long)ks);
}

/* ------------------------------------------------------------- main ----- */

int main(int argc, char **argv)
{
    short work_in[11], work_out[57], ext[57];
    short d, msg[8], reply[8];
    short mx, my, mb, ks, kr, brk, ev, i;
    short dx, dy, dw, dh, cx, cy, cw, ch;
    long status, t0;
    const char *url = argc > 1 ? argv[1] : DEFAULT_URL;

    if (appl_init() < 0)
        return 1;
    webid = nf_id("PSWEB");
    if (!webid) {
        form_alert(1, "[3][PSWEB NatFeat not found.|Run under the PiSTorm emulator|built with psweb support.][ OK ]");
        appl_exit();
        return 1;
    }
    status = nf_call(webid | PSWEB_STATUS);
    if (!(status & 2)) {
        /* the emulator connects by itself; give psweb a moment */
        t0 = ticks();
        while (!(nf_call(webid | PSWEB_STATUS) & 2) && ticks() - t0 < 600)
            ;
        if (!(nf_call(webid | PSWEB_STATUS) & 2)) {
            form_alert(1, "[3][psweb is not running on the Pi.|Start it: psweb/psweb|then try again.][ OK ]");
            appl_exit();
            return 1;
        }
    }

    vh = graf_handle(&d, &d, &d, &d);
    for (i = 0; i < 10; i++) work_in[i] = 1;
    work_in[10] = 2;
    v_opnvwk(work_in, &vh, work_out);
    vq_extnd(vh, 1, ext);
    planes = ext[4];
    if (planes != 16 && planes != 32) {
        form_alert(1, "[3][WEBCHK needs a 16 or 32 bit|screen.][ OK ]");
        v_clsvwk(vh);
        appl_exit();
        return 1;
    }

    wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
    cw = 1280; ch = 678;                       /* the size phase 0 liked */
    if (cw > dw - 32) cw = (short)(dw - 32);
    if (ch > dh - 32) ch = (short)(dh - 32);
    wind_calc(WC_BORDER, WIN_KIND, (short)(dx + 16), (short)(dy + 16), cw, ch, &cx, &cy, &cw, &ch);
    if (cx + cw > dx + dw) cw = (short)(dx + dw - cx);
    if (cy + ch > dy + dh) ch = (short)(dy + dh - cy);
    win = wind_create(WIN_KIND, dx, dy, dw, dh);
    wind_set_str(win, WF_NAME, "PiSTorm Web - starting psweb");
    wind_open(win, cx, cy, cw, ch);
    wind_set(win, WF_WHEEL, 1, WHEEL_ARROWED, 0, 0);
    wind_get(win, WF_WORKXYWH, &work.g_x, &work.g_y, &work.g_w, &work.g_h);

    if (!ensure_buffer()) {
        form_alert(1, "[3][No TT-RAM for the page buffer.][ OK ]");
        goto out;
    }
    nf_call(webid | PSWEB_VIEW_NEW, (long)work.g_w, (long)work.g_h, (long)planes, 0L);
    nf_call(webid | PSWEB_LOAD, 1L, (long)url);

    for (;;) {
        ev = evnt_multi(MU_MESAG | MU_BUTTON | MU_KEYBD | MU_TIMER,
                        1, 1, 1,
                        0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0,
                        msg, 16UL,
                        &mx, &my, &mb, &ks, &kr, &brk);

        if (ev & MU_MESAG) {
            switch (msg[0]) {
                case WM_REDRAW: {
                    GRECT a;
                    a.g_x = msg[4]; a.g_y = msg[5]; a.g_w = msg[6]; a.g_h = msg[7];
                    redraw_area(&a);
                    break;
                }
                case WM_TOPPED:
                    wind_set(win, WF_TOP, 0, 0, 0, 0);
                    break;
                case WM_MOVED:
                    wind_set(win, WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
                    wind_get(win, WF_WORKXYWH, &work.g_x, &work.g_y, &work.g_w, &work.g_h);
                    break;
                case WM_SIZED:
                case WM_FULLED: {
                    if (msg[0] == WM_FULLED) {
                        wind_get(win, WF_FULLXYWH, &msg[4], &msg[5], &msg[6], &msg[7]);
                    }
                    if (msg[6] < 200) msg[6] = 200;
                    if (msg[7] < 120) msg[7] = 120;
                    wind_set(win, WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
                    wind_get(win, WF_WORKXYWH, &work.g_x, &work.g_y, &work.g_w, &work.g_h);
                    resize_view();
                    seen_serial = 0;         /* the next frame is a full one */
                    break;
                }
                case WM_ARROWED: {
                    short n = (short)((msg[4] >> 8) & 0xff);
                    if (n < 1) n = 1;
                    switch (msg[4] & 15) {
                        case WA_UPLINE: scroll_notches(-n); break;
                        case WA_DNLINE: scroll_notches(n); break;
                        case WA_UPPAGE: scroll_notches(-5); break;
                        case WA_DNPAGE: scroll_notches(5); break;
                    }
                    break;
                }
                case WM_CLOSED:
                    goto out;
                case AP_TERM:
                    goto out;
                default:
                    if (msg[0] == 40) {                 /* AP_TERM on old headers */
                        reply[3] = msg[3]; reply[4] = msg[4];
                        (void)reply;
                        goto out;
                    }
                    break;
            }
        }
        if (ev & MU_BUTTON) {
            if (in_work(mx, my) && (mb & 1)) {
                wind_set(win, WF_TOP, 0, 0, 0, 0);
                pointer(0L, mx, my, 0L, ks);
                pointer(1L, mx, my, 1L, ks);
                held = 1;
                last_mx = mx; last_my = my;
            } else if (in_work(mx, my) && (mb & 2)) {
                pointer(1L, mx, my, 2L, ks);
                pointer(2L, mx, my, 2L, ks);
            }
        }
        if (ev & MU_KEYBD) {
            if ((ks & K_ALT) && ((kr & 0xff) == 'x' || (kr & 0xff) == 'X' || ((kr >> 8) & 0xff) == 0x2d))
                goto out;
            key(kr, ks);
        }
        if (ev & MU_TIMER)
            tick();
    }

out:
    nf_call(webid | PSWEB_VIEW_FREE);
    if (buf) Mfree(buf);
    if (win >= 0) {
        wind_close(win);
        wind_delete(win);
    }
    v_clsvwk(vh);
    appl_exit();
    return 0;
}
