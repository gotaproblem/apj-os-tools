/*
 * dosgem.c - DOSGEM.PRG: DOS games on APJ-OS.
 *
 * The DOS machine runs on the Pi: psdos drives DOSBox Pure (with its ARM64
 * recompiler) and the PSDOS NatFeat carries its frames, sound and input
 * (psdos-design.md). This program is the window: it asks for a view the
 * size of its work area, copies the changed band of each frame into a
 * TT-RAM buffer (FETCH) and blits it, and decides who gets the keyboard.
 *
 * Input. Until you click in the window, keys typed in it go to DOS as
 * GEM key presses (fine for a DOS prompt or a menu). A left click
 * CAPTURES: from then on the emulator routes the real keyboard, mouse and
 * joysticks to DOS directly - held keys, key-up, the relative mouse, the
 * Xbox pads - and the Atari sees none of it. Undo (ST keyboard), Scroll
 * Lock or Ctrl+Alt+F12 (USB keyboard) releases; so does another window
 * coming to the top.
 * The pointer is hidden while captured.
 *
 *     DOSGEM                     file selector (Cancel = the DOS prompt)
 *     DOSGEM C:\DOSGAMES\KEEN    a folder, .ZIP, .EXE/.COM/.BAT, .ISO/.CUE/.IMG
 *
 * Keys while not captured: Alt+O open, Alt+R reset, Alt+P pause, Alt+X or
 * Ctrl+Q quit; everything else goes to DOS. DOSGEM.INF at the boot drive
 * root: win=x,y,w,h  dir=<last folder>  pause=1 (paused while not on top).
 *
 * The rules from PSCTRL/WEBGEM hold: repaint only the damaged band,
 * BEG_UPDATE once per frame, the pointer hidden only when it is over the
 * band being drawn (or always, while captured).
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
#include "dosui.h"

#ifndef APJ_SKINCHG
#define APJ_SKINCHG 0x4A50             /* apjskin.h: the desktop changed theme */
#endif

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

/* PSDOS sub-ops - must match platforms/atari/dos/psdos_client.h */
#define PSDOS_VERSION    0
#define PSDOS_STATUS     1
#define PSDOS_VIEW_NEW   2
#define PSDOS_VIEW_FREE  3
#define PSDOS_VIEW_SIZE  4
#define PSDOS_STATE      5
#define PSDOS_LOAD       6
#define PSDOS_POLL       7
#define PSDOS_FETCH      8
#define PSDOS_KEY        9
#define PSDOS_CAPTURE   10
#define PSDOS_GETSTR    11
#define PSDOS_RESET     12

#define ST_SOCKET   0x01
#define ST_CONN     0x02
#define ST_VIEW     0x04
#define ST_LOADED   0x08
#define ST_CAPTURE  0x10
#define ST_NOCORE   0x20
#define ST_FAILED   0x40
#define ST_EXITED   0x80

#define STR_TITLE   0
#define STR_STATUS  1

#define WIN_KIND (NAME | INFO | CLOSER | MOVER | SIZER | FULLER | SMALLER)

#define VA_START    0x4711     /* AV protocol: open this */
#define AV_STARTED  0x4738

/* ST scancodes for the modifiers, sent around a GEM key press */
#define SC_LSHIFT   0x2A
#define SC_CTRL     0x1D
#define SC_ALT      0x38

/* the eight big-endian longs POLL writes - native order on the 68k */
typedef struct {
    long frame_serial, flags, fps_x100, src_w, src_h, capture, title_serial, capture_serial;
} POLLSTATE;

/* ------------------------------------------------------------- state ----- */

static long  dosid;
static short vh, win = -1;
static short planes = 32;
static short wx, wy, ww, wh;
static int   iconified = 0;

static DOSUI ui;
static void *buf;
static short buf_w16, buf_h;

static short view_ready, view_requested;
static short load_done;                 /* LOAD sent on this connection */
static char  pending[256];              /* what to load once the view exists */
static short pending_set;
static char  waittext[160];
static long  seen_title = -1, seen_capture = -1, last_status = -1;
static short captured, mouse_hidden;
static short paused_by_user;
static short on_top = 1, state_sent = -1;
static char  title[128] = "DOS";
static char  info[160];

static short inf_pause = 1;
static short inf_x = -1, inf_y = -1, inf_w = 0, inf_h = 0;
static char  inf_dir[200] = "";

static void relayout(void);
static void redraw_all(void);
static void set_captured(short on);

/* ------------------------------------------------------------- .INF ------ */

static long boot_drive_sv(void) { return *(volatile short *)0x446L; }

static void inf_path(char *out)
{
    long d = Supexec(boot_drive_sv);

    if (d < 0 || d > 25)
        d = 2;
    sprintf(out, "%c:\\DOSGEM.INF", (char)('A' + d));
}

static void inf_load(void)
{
    char path[64], b[768], *p, *e;
    long fh, got;

    inf_path(path);
    fh = Fopen(path, 0);
    if (fh < 0)
        return;
    got = Fread((short)fh, (long)sizeof b - 1, b);
    Fclose((short)fh);
    if (got <= 0)
        return;
    b[got] = 0;
    if ((p = strstr(b, "pause=")) != NULL)
        inf_pause = (short)(atoi(p + 6) != 0);
    if ((p = strstr(b, "dir=")) != NULL) {
        p += 4;
        e = p;
        while (*e && *e != '\r' && *e != '\n') e++;
        if (e - p > 0 && e - p < (long)sizeof inf_dir) {
            memcpy(inf_dir, p, (size_t)(e - p));
            inf_dir[e - p] = 0;
        }
    }
    if ((p = strstr(b, "win=")) != NULL) {
        int x, y, w, h;

        if (sscanf(p + 4, "%d,%d,%d,%d", &x, &y, &w, &h) == 4 && w > 0 && h > 0) {
            inf_x = (short)x; inf_y = (short)y; inf_w = (short)w; inf_h = (short)h;
        }
    }
}

static void inf_save(void)
{
    char path[64], b[400];
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
    sprintf(b, "win=%d,%d,%d,%d\r\ndir=%s\r\npause=%d\r\n",
            (int)inf_x, (int)inf_y, (int)inf_w, (int)inf_h, inf_dir, (int)inf_pause);
    Fwrite((short)fh, (long)strlen(b), b);
    Fclose((short)fh);
}

/* ------------------------------------------------------------- redraw ---- */

static void draw_view(const GRECT *c) { ui.clip = *c; dosui_draw(&ui, vh); ui.clip = ui.work; }

static void draw_iconic(const GRECT *c)
{
    (void)c;
    apj_fill(vh, wx, wy, ww, wh, 1);
}

static short mouse_off = 0;

static void redraw_begin(const GRECT *area)
{
    short mx, my, mb, ks;

    wind_update(BEG_UPDATE);
    mouse_off = 0;
    if (mouse_hidden)
        ;                                   /* already hidden: captured */
    else if (area) {
        graf_mkstate(&mx, &my, &mb, &ks);
        if (mx >= area->g_x && my >= area->g_y &&
            mx < area->g_x + area->g_w && my < area->g_y + area->g_h)
            mouse_off = 1;
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

static void redraw_all(void)
{
    redraw(iconified ? draw_iconic : draw_view, wx, wy, ww, wh);
}

static void set_msg(short m)
{
    if (ui.msg == m)
        return;
    ui.msg = m;
    if (!iconified)
        redraw_all();
}

/* ------------------------------------------------------------- info line - */

static void show_info(void)
{
    char t[160];

    if (!view_ready)
        strcpy(t, " Waiting for psdos on the Pi...");
    else if (captured)
        strcpy(t, " Input captured - Undo (ST), Scroll Lock or Ctrl+Alt+F12 (USB) releases");
    else if (paused_by_user)
        strcpy(t, " Paused - Alt+P to resume");
    else if (state_sent == 0 || state_sent == 2)
        strcpy(t, " Paused while not on top (pause=0 in DOSGEM.INF to keep running)");
    else
        strcpy(t, " Click in the window to capture input   Alt+O open  Alt+R reset  Alt+P pause  Alt+X quit");
    if (strcmp(t, info) != 0) {
        strcpy(info, t);
        wind_set_str(win, WF_INFO, info);
    }
}

/* ------------------------------------------------------------- buffer ---- */

static int ensure_buffer(void)
{
    GRECT v;
    short w16;
    long need;

    dosui_view_rect(&ui, &v);
    if (v.g_w <= 0 || v.g_h <= 0)
        return 0;
    w16 = (short)((v.g_w + 15) & ~15);
    need = (long)w16 * v.g_h * (planes / 8);
    if (buf && buf_w16 == w16 && buf_h == v.g_h)
        return 1;
    if (buf) { Mfree(buf); buf = NULL; }
    buf = (void *)Mxalloc(need, 1);             /* TT-RAM only: the host writes it */
    ui.buf.fd_addr = NULL;
    if (!buf) {
        ui.msg = DOS_MSG_NOTT;
        return 0;
    }
    memset(buf, 0, (size_t)need);
    buf_w16 = w16;
    buf_h = v.g_h;
    ui.buf.fd_w = w16;
    ui.buf.fd_h = v.g_h;
    ui.buf.fd_wdwidth = (short)(w16 / 16);
    ui.buf.fd_stand = 0;
    ui.buf.fd_nplanes = planes;
    ui.buf.fd_r1 = ui.buf.fd_r2 = ui.buf.fd_r3 = 0;
    if (ui.msg == DOS_MSG_NOTT)
        ui.msg = DOS_MSG_WAIT;
    return 1;
}

/* ------------------------------------------------------------- engine ---- */

/*
 * What psdos may do: bit0 visible (not visible = the DOS machine is
 * held), bit1 focused (the capture needs it). With pause=1, a window that
 * is behind another, iconified, or paused by Alt+P is reported not
 * visible - the 68k gets its share of core 1's neighbours back.
 */
static void dos_state(void)
{
    short bits;

    if (iconified || paused_by_user)
        bits = 0;
    else if (on_top)
        bits = 3;
    else
        bits = inf_pause ? 0 : 1;
    if (!on_top && captured)
        set_captured(0);                       /* the emulator drops it too */
    if (bits != state_sent && view_ready) {
        state_sent = bits;
        nf_call(dosid | PSDOS_STATE, (long)bits);
    }
    show_info();
}

static void dos_load(const char *path)
{
    long r;
    char p[sizeof pending];

    strncpy(p, path ? path : "", sizeof p - 1);
    p[sizeof p - 1] = 0;
    strcpy(pending, p);                        /* what a reconnect reloads */
    r = nf_call(dosid | PSDOS_LOAD, (long)p);
    if (r == -1L) {
        form_alert(1, "[3][That file is not on a HOSTFS|drive - the Pi cannot read it.|Put DOS games under atari-share.][ OK ]");
        pending_set = 0;
        set_msg(DOS_MSG_FAILED);
        return;
    }
    if (r < 0) {                               /* busy / link not up: retry */
        pending_set = 1;
        load_done = 0;
        return;
    }
    pending_set = 0;
    load_done = 1;
    ui.buf.fd_addr = NULL;                     /* until the new game draws */
    set_msg(DOS_MSG_WAIT);
}

static short req_w, req_h;              /* the view size psdos was given */

static void request_view(void)
{
    GRECT v;

    dosui_view_rect(&ui, &v);
    if (nf_call(dosid | PSDOS_VIEW_NEW, (long)v.g_w, (long)v.g_h, (long)planes) > 0) {
        view_requested = 1;
        req_w = v.g_w;
        req_h = v.g_h;
    }
}

/* the link went away: back to waiting, and the game is loaded again when
 * psdos returns (a fresh DOS machine - DOS has no state to keep) */
static long last_loss;                  /* 200 Hz ticks of the last link loss */

static long hz200_sv(void) { return *(volatile long *)0x4BAL; }

static void link_lost(void)
{
    long now = Supexec(hz200_sv);

    view_ready = 0;
    view_requested = 0;
    load_done = 0;
    state_sent = -1;
    captured = 0;
    if (mouse_hidden) { graf_mouse(M_ON, NULL); mouse_hidden = 0; }
    ui.buf.fd_addr = NULL;
    /* psdos went away: load the game again when it is back - unless it
     * went away twice inside 20 s, which is a game that takes psdos down
     * with it; then wait for Alt+O / Alt+R instead of a crash loop */
    if (last_loss && now - last_loss < 20L * 200L) {
        pending_set = 0;
        set_msg(DOS_MSG_FAILED);
    } else {
        pending_set = 1;
        set_msg(DOS_MSG_WAIT);
    }
    last_loss = now;
    show_info();
}

static void set_captured(short on)
{
    if (on == captured)
        return;
    captured = on;
    if (on && !mouse_hidden) {
        graf_mouse(M_OFF, NULL);
        mouse_hidden = 1;
    } else if (!on && mouse_hidden) {
        graf_mouse(M_ON, NULL);
        mouse_hidden = 0;
    }
    show_info();
}

static void sync_title(const POLLSTATE *st)
{
    char t[120];
    long n;

    if (st->title_serial == seen_title)
        return;
    n = nf_call(dosid | PSDOS_GETSTR, (long)STR_TITLE, (long)t, (long)sizeof t);
    if (n < 0)
        return;
    seen_title = st->title_serial;
    if (n == 0)
        strcpy(t, "DOS");
    sprintf(title, "DOS - %s", t);
    wind_set_str(win, WF_NAME, title);
}

/* one tick: status, poll, fetch the band that changed, blit it */
static void tick(void)
{
    POLLSTATE st;
    long s, rect[4], n;
    GRECT r;

    s = nf_call(dosid | PSDOS_STATUS);
    if (!view_ready) {
        if (s & ST_VIEW) {
            GRECT v;

            view_ready = 1;
            state_sent = -1;
            dos_state();
            /* resized while waiting: tell psdos the size it is now */
            dosui_view_rect(&ui, &v);
            if (v.g_w != req_w || v.g_h != req_h)
                nf_call(dosid | PSDOS_VIEW_SIZE, (long)v.g_w, (long)v.g_h);
            if (ui.msg != DOS_MSG_FAILED)
                set_msg(DOS_MSG_WAIT);
            return;
        }
        if (s & ST_CONN) {
            if (!view_requested)
                request_view();
        } else
            view_requested = 0;
        {
            char t[160];

            if (nf_call(dosid | PSDOS_GETSTR, (long)STR_STATUS, (long)t, (long)sizeof t) > 0 &&
                strcmp(t, waittext) != 0) {
                strcpy(waittext, t);
                ui.msg = DOS_MSG_NONE;          /* force the repaint */
                set_msg(DOS_MSG_WAIT);
            }
        }
        return;
    }
    if (!(s & ST_VIEW)) {
        link_lost();
        return;
    }
    if (pending_set && !load_done)
        dos_load(pending);
    if (!buf && ui.msg == DOS_MSG_NOTT)        /* TT-RAM may be free by now */
        ensure_buffer();

    /* whether the window is on top, from the AES itself */
    {
        short top = 0, d;

        wind_get(0, WF_TOP, &top, &d, &d, &d);
        if ((top == win) != (on_top != 0)) {
            on_top = (top == win);
            dos_state();
        }
    }

    if (nf_call(dosid | PSDOS_POLL, (long)&st) < 0) {
        link_lost();
        return;
    }
    if (st.capture_serial != seen_capture) {
        seen_capture = st.capture_serial;
        set_captured((short)(st.capture != 0));
    }
    sync_title(&st);
    if (s != last_status) {
        last_status = s;
        if (s & ST_NOCORE)
            set_msg(DOS_MSG_NOCORE);
        else if (s & ST_FAILED)
            set_msg(DOS_MSG_FAILED);
        else if (s & ST_EXITED)
            set_msg(DOS_MSG_EXITED);
    }

    if (!buf || iconified)
        return;
    n = nf_call(dosid | PSDOS_FETCH, (long)buf, (long)buf_w16 * (planes / 8),
                (long)buf_h, (long)rect);
    if (n <= 0)
        return;
    if (ui.buf.fd_addr == NULL || ui.msg != DOS_MSG_NONE) {
        ui.buf.fd_addr = buf;                 /* the first frame: everything */
        ui.msg = DOS_MSG_NONE;
        redraw_all();
        return;
    }
    dosui_band_rect(&ui, (short)rect[1], (short)rect[3], &r);
    redraw(draw_view, r.g_x, r.g_y, r.g_w, r.g_h);
}

/* ------------------------------------------------------------- window ---- */

static void relayout(void)
{
    GRECT before, after;

    dosui_view_rect(&ui, &before);
    wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
    dosui_layout(&ui, wx, wy, ww, wh);
    dosui_view_rect(&ui, &after);
    if (after.g_w != before.g_w || after.g_h != before.g_h) {
        ensure_buffer();
        if (view_ready && after.g_w > 0 && after.g_h > 0) {
            nf_call(dosid | PSDOS_VIEW_SIZE, (long)after.g_w, (long)after.g_h);
            req_w = after.g_w;
            req_h = after.g_h;
        }
        ui.buf.fd_addr = NULL;                /* until psdos draws the new size */
    }
}

/* 640x480 at the largest whole scale the desktop takes, at most 2x */
static void natural_size(short *w, short *h)
{
    short dx, dy, dw, dh, n;

    wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
    for (n = 2; n > 1; n--)
        if (640 * n + 32 <= dw && 480 * n + 64 <= dh)
            break;
    *w = (short)(640 * n);
    *h = (short)(480 * n);
    if (*w > dw - 32) *w = (short)(dw - 32);
    if (*h > dh - 64) *h = (short)(dh - 64);
}

/* ------------------------------------------------------------- open ------ */

/* the file selector: 1 with the chosen path in out ("" = the folder
 * itself when OK is pressed with no file), 0 on Cancel */
static int ask_path(char *out, size_t outsz)
{
    char path[256], name[64], *p;
    short button = 0;

    if (inf_dir[0])
        sprintf(path, "%s*.*", inf_dir);
    else {
        long d = Dgetdrv();

        path[0] = (char)('A' + d);
        path[1] = ':';
        Dgetpath(path + 2, (short)(d + 1));
        strcat(path, "\\*.*");
    }
    name[0] = 0;
    if (!fsel_exinput(path, name, &button, "DOS game: folder, .ZIP, .EXE, .ISO") || !button)
        return 0;
    p = strrchr(path, '\\');
    if (p)
        p[1] = 0;
    strncpy(inf_dir, path, sizeof inf_dir - 1);
    inf_dir[sizeof inf_dir - 1] = 0;
    /* no file picked: the folder itself (DOSBox Pure mounts it as C:) */
    if (strlen(path) + strlen(name) + 1 > outsz)
        return 0;
    sprintf(out, "%s%s", path, name);
    inf_save();
    return 1;
}

/* Alt+O: another game; Cancel = the DOS prompt */
static void open_dialog(void)
{
    char full[320];

    last_loss = 0;
    if (ask_path(full, sizeof full))
        dos_load(full);
    else
        dos_load("");
}

/* ------------------------------------------------------------- input ----- */

/* a GEM key press as DOS key presses: modifiers around it, down then up */
static void gem_key(short kr, short ks)
{
    short scan = (short)((kr >> 8) & 0xff);

    if (!scan || scan > 0x72)
        return;
    if (ks & (K_LSHIFT | K_RSHIFT)) nf_call(dosid | PSDOS_KEY, 1L, (long)SC_LSHIFT);
    if (ks & K_CTRL)                nf_call(dosid | PSDOS_KEY, 1L, (long)SC_CTRL);
    if (ks & K_ALT)                 nf_call(dosid | PSDOS_KEY, 1L, (long)SC_ALT);
    nf_call(dosid | PSDOS_KEY, 1L, (long)scan);
    nf_call(dosid | PSDOS_KEY, 0L, (long)scan);
    if (ks & K_ALT)                 nf_call(dosid | PSDOS_KEY, 0L, (long)SC_ALT);
    if (ks & K_CTRL)                nf_call(dosid | PSDOS_KEY, 0L, (long)SC_CTRL);
    if (ks & (K_LSHIFT | K_RSHIFT)) nf_call(dosid | PSDOS_KEY, 0L, (long)SC_LSHIFT);
}

static int in_view(short mx, short my)
{
    GRECT v;

    dosui_view_rect(&ui, &v);
    return mx >= v.g_x && my >= v.g_y && mx < v.g_x + v.g_w && my < v.g_y + v.g_h;
}

/* ------------------------------------------------------------- main ------ */

int main(int argc, char **argv)
{
    short work_in[11], work_out[57];
    short d, msg[8];
    short mx, my, mb, ks, kr, brk;
    short ev, i;
    short cw, ch;

    if (appl_init() < 0)
        return 1;
    dosid = nf_id("PSDOS");
    if (!dosid) {
        form_alert(1, "[3][PSDOS NatFeat not found.|Run under the PiSTorm emulator|built with psdos support.][ OK ]");
        appl_exit();
        return 1;
    }

    vh = graf_handle(&cw, &ch, &d, &d);
    for (i = 0; i < 10; i++) work_in[i] = 1;
    work_in[10] = 2;
    v_opnvwk(work_in, &vh, work_out);
    vst_alignment(vh, 0, 5, &d, &d);
    apj_init(vh);
    inf_load();

    {
        short ext[57];

        vq_extnd(vh, 1, ext);
        planes = ext[4];
    }
    if (planes != 16 && planes != 32) {
        form_alert(1, "[3][DOSGEM needs a 16 or 32 bit|screen.][ OK ]");
        v_clsvwk(vh);
        appl_exit();
        return 1;
    }

    memset(&ui, 0, sizeof ui);
    ui.msg = DOS_MSG_WAIT;
    ui.waittext = waittext;

    {
        short dx, dy, dw, dh, cx, cy, cwid, chgt, want_w, want_h;

        natural_size(&want_w, &want_h);
        wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
        wind_calc(WC_BORDER, WIN_KIND, (short)(dx + 16), (short)(dy + 16),
                  want_w, want_h, &cx, &cy, &cwid, &chgt);
        if (inf_w > 0 && inf_h > 0) {
            cx = inf_x; cy = inf_y; cwid = inf_w; chgt = inf_h;
        }
        if (cwid > dw) cwid = dw;
        if (chgt > dh) chgt = dh;
        if (cx < dx) cx = dx;
        if (cy < dy) cy = dy;
        if (cx + cwid > dx + dw) cx = (short)(dx + dw - cwid);
        if (cy + chgt > dy + dh) cy = (short)(dy + dh - chgt);
        win = wind_create(WIN_KIND, dx, dy, dw, dh);
        if (win < 0) {
            form_alert(1, "[3][No window left.][ OK ]");
            v_clsvwk(vh);
            appl_exit();
            return 1;
        }
        wind_set_str(win, WF_NAME, title);
        info[0] = 0;
        show_info();
        wind_open(win, cx, cy, cwid, chgt);
        wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
        dosui_layout(&ui, wx, wy, ww, wh);
    }
    ensure_buffer();
    redraw_all();

    if (argc > 1) {
        strncpy(pending, argv[1], sizeof pending - 1);
        pending[sizeof pending - 1] = 0;
        pending_set = 1;
    }
    tick();
    if (argc <= 1) {
        /* no argument: ask, once the window is up; Cancel = the DOS prompt */
        if (!ask_path(pending, sizeof pending))
            pending[0] = 0;
        pending_set = 1;
        redraw_all();
    }

    for (;;) {
        ev = evnt_multi(MU_MESAG | MU_BUTTON | MU_KEYBD | MU_TIMER,
                        1, 1, 1,
                        0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0,
                        msg, view_ready && !iconified ? 14UL : 100UL,
                        &mx, &my, &mb, &ks, &kr, &brk);

        if (ev & MU_MESAG) {
            switch (msg[0]) {
                case WM_REDRAW:
                    redraw(iconified ? draw_iconic : draw_view, msg[4], msg[5], msg[6], msg[7]);
                    break;
                case WM_TOPPED:
                    wind_set(win, WF_TOP, 0, 0, 0, 0);
                    on_top = 1;
                    dos_state();
                    break;
                case WM_ONTOP:
                    on_top = 1;
                    dos_state();
                    break;
                case WM_UNTOPPED:
                case WM_BOTTOMED:
                    on_top = 0;
                    dos_state();
                    break;
                case WM_MOVED:
                    wind_set(win, WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
                    relayout();
                    break;
                case WM_SIZED:
                case WM_FULLED:
                    if (msg[0] == WM_FULLED)
                        wind_get(win, WF_FULLXYWH, &msg[4], &msg[5], &msg[6], &msg[7]);
                    if (msg[6] < 200) msg[6] = 200;
                    if (msg[7] < 160) msg[7] = 160;
                    wind_set(win, WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
                    relayout();
                    redraw_all();
                    break;
                case WM_CLOSED:
                    goto out;
                case WM_ICONIFY:
                case WM_ALLICONIFY:
                    iconified = 1;
                    wind_set(win, WF_ICONIFY, msg[4], msg[5], msg[6], msg[7]);
                    wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
                    dos_state();
                    break;
                case WM_UNICONIFY:
                    wind_set(win, WF_UNICONIFY, msg[4], msg[5], msg[6], msg[7]);
                    iconified = 0;
                    relayout();
                    redraw_all();
                    dos_state();
                    break;
                case APJ_SKINCHG:
                    apj_init(vh);
                    redraw_all();
                    break;
                case VA_START: {
                    short reply[8];
                    char *cmd = (char *)(((long)msg[3] << 16) | (unsigned short)msg[4]);

                    if (iconified) {
                        wind_set(win, WF_UNICONIFY, -1, -1, -1, -1);
                        iconified = 0;
                        relayout();
                        redraw_all();
                    }
                    if (cmd && cmd[0]) {
                        last_loss = 0;
                        if (view_ready)
                            dos_load(cmd);
                        else {
                            strncpy(pending, cmd, sizeof pending - 1);
                            pending[sizeof pending - 1] = 0;
                            pending_set = 1;
                        }
                    }
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
        if ((ev & MU_BUTTON) && !iconified && in_view(mx, my)) {
            wind_set(win, WF_TOP, 0, 0, 0, 0);
            on_top = 1;
            dos_state();
            if ((mb & 1) && view_ready && !captured) {
                short bx, by, bb, bk;

                /* let the Atari see the release first: a button still
                 * down for GEM while DOS has the mouse would stay down */
                graf_mkstate(&bx, &by, &bb, &bk);
                if (bb & 1)
                    evnt_button(1, 1, 0, &bx, &by, &bb, &bk);
                if (nf_call(dosid | PSDOS_CAPTURE, 1L) > 0) {
                    set_captured(1);
                    seen_capture = -1;            /* take the serial next tick */
                }
            }
        }
        if ((ev & MU_KEYBD) && !captured) {
            short scan = (short)((kr >> 8) & 0xff);

            if ((ks & K_ALT) && scan == 0x2d)              /* Alt+X */
                goto out;
            if ((ks & K_CTRL) && scan == 0x10)             /* Ctrl+Q */
                goto out;
            if ((ks & K_ALT) && scan == 0x18) {            /* Alt+O */
                if (view_ready) open_dialog();
            } else if ((ks & K_ALT) && scan == 0x13) {     /* Alt+R */
                if (view_ready) nf_call(dosid | PSDOS_RESET);
            } else if ((ks & K_ALT) && scan == 0x19) {     /* Alt+P */
                paused_by_user = (short)!paused_by_user;
                dos_state();
            } else if (view_ready)
                gem_key(kr, ks);
        }
        if (ev & MU_TIMER)
            tick();
    }

out:
    inf_save();
    nf_call(dosid | PSDOS_VIEW_FREE);
    if (mouse_hidden)
        graf_mouse(M_ON, NULL);
    if (buf) Mfree(buf);
    if (win >= 0) {
        wind_close(win);
        wind_delete(win);
    }
    v_clsvwk(vh);
    appl_exit();
    return 0;
}
