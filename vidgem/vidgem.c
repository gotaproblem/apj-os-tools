/*
 * vidgem.c - PiSTorm video player, GEM front-end, on the APJSKIN sheet.
 *
 * The picture is NOT drawn by this program. The Pi puts it on a DRM
 * overlay plane above the whole Atari screen; this window tells the host
 * where its picture box is (NF_VID_RECT), which part of that box is really
 * visible (NF_VID_CLIP, from the AES rectangle list), and draws a black
 * box underneath so the letterbox bars and the hidden state look the same.
 *
 * Everything on the 68k side is about not repainting: a timer-driven GEM
 * app is the one program that can make the WHOLE desktop flicker, and the
 * old VIDGEM's scrolling marquee did exactly that four times a second.
 * The rules, all of them learned on MP3GEM and PSMON:
 *
 *   - the seek strip is repainted once a second, when the position text
 *     changes, and nothing else in the window is touched for it;
 *   - the "shown fps" figure in the status line is formatted every poll
 *     and repainted only when the STRING changes;
 *   - the title band is repainted on events (a file opened, stopped,
 *     paused), never on the timer;
 *   - a hover repaints exactly two widgets, a press exactly one;
 *   - wind_update(BEG_UPDATE) is taken once per change, and the pointer
 *     is hidden only when it is inside the rectangles being painted;
 *   - MU_M1 is armed for a condition that is NOT already true.
 *
 * Layout and drawing are in vidui.c, which also builds on a Linux host
 * (tests/vid) so the blits and text extents are checked there.
 */

#include <gem.h>
#include <osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#define APJGUI_IMPL
#include "../apjgui/apjgui.h"
#include "../apjgui/apjskin.h"
#include "vidui.h"

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

#define NF_VID_PLAY   0
#define NF_VID_STOP   1
#define NF_VID_STATUS 2
#define NF_VID_PAUSE  3
#define NF_VID_SEEK   4
#define NF_VID_POS    5
#define NF_VID_LEN    6
#define NF_VID_META   7
#define NF_VID_RECT   8
#define NF_VID_VOLUME 9
#define NF_VID_INFO  10
#define NF_VID_CLIP  11
/* INFO sub-values */
#define VI_MIN_W     11
#define VI_MIN_H     12
#define VI_FPS_NOW   13           /* frames/s actually shown, x100 */

/* ---------------------------------------------------------------- state -- */

#define MAXTRACKS 200
/* fsel_exinput() writes the name into OUR buffer and the AES decides how
 * much: under MiNT a HOSTFS name runs well past 64 bytes. 256 ends it. */
#define NAMELEN   256
#define PATHLEN   512

static long  vidid;
static short vh;                          /* VDI handle */
static short win = -1;
static short cw, ch;                      /* char cell size */
static short wx, wy, ww, wh;              /* window work area */
static int   iconified = 0;
static short scr_w, scr_h;                /* Atari screen size, pixels */
static long  disp_w, disp_h;              /* real display size, pixels */

static char  dir[PATHLEN] = "";           /* playlist directory (GEMDOS path) */
static char  list[MAXTRACKS][NAMELEN];
static int   ntracks = 0;

static char  ui_title[160] = "PiSTorm Video";
static char  ui_sub[192]   = "Open a file...";
static char  ui_codec[32]  = "";
static char  fps_shown[24] = "";          /* what the status line shows */
static char  fps_last[24]  = "";          /* what is really on screen  */

static long  act_fps = 0;                 /* frames/s really shown, x100 */
static long  min_out_w = 0, min_out_h = 0;   /* smallest usable window, outer */
static int   floor_unfit = 0;             /* the floor is bigger than the desktop */
static short vol_before_mute = 100;

static VIDUI ui;
static GRECT m1r;
static short m1flag = MO_ENTER;

#define WIN_KIND (NAME | CLOSER | MOVER | SIZER | FULLER | SMALLER)

static const char *vexts[] = { ".MP4", ".MKV", ".AVI", ".MOV", ".M4V",
                               ".WEBM", ".TS", ".MPG", ".MPEG", ".OGV", NULL };

/* ------------------------------------------------------------- NF glue --- */

static void nf_meta(int which, char *buf, int len)
{
    buf[0] = '\0';
    nf_call(vidid | NF_VID_META, (long)which, buf, (long)len);
    buf[len - 1] = '\0';
}

static const char *name_of(void *c, short i)
{
    (void)c;
    return (i >= 0 && i < ntracks) ? list[i] : "";
}

/* ------------------------------------------------------------- drawing --- */

/*
 * Each drawing function gets the rectangle it may touch and hands it to
 * vidui as the clip, so a partial repaint prunes the parts it does not
 * meet as well as being clipped by the VDI.
 */
static void draw_all(const GRECT *c)    { vidui_draw_clip(&ui, vh, c); }
static void draw_band(const GRECT *c)   { ui.clip = *c; vidui_draw_band(&ui, vh);   ui.clip = ui.work; }
static void draw_clock(const GRECT *c)  { ui.clip = *c; vidui_draw_clock(&ui, vh);  ui.clip = ui.work; }
static void draw_info(const GRECT *c)   { ui.clip = *c; vidui_draw_info(&ui, vh);   ui.clip = ui.work; }
static void draw_status(const GRECT *c) { ui.clip = *c; vidui_draw_status(&ui, vh); ui.clip = ui.work; }
static void draw_list(const GRECT *c)   { ui.clip = *c; vidui_draw_list(&ui, vh);   ui.clip = ui.work; }

static void draw_iconic(const GRECT *c)
{
    (void)c;
    apj_fill(vh, wx, wy, ww, wh, apj_pen(APJ_R_PANEL));
}

/*
 * wind_update(BEG_UPDATE) is a screen-wide semaphore and graf_mouse(M_OFF)
 * repaints whatever is under the pointer - the taskbar, if that is where
 * it is resting. So the lock is taken once per change however many
 * rectangles it covers, and the pointer is hidden only when it is inside
 * the area being painted.
 */
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

static void redraw_all(void)
{
    if (!iconified)
        redraw(draw_all, wx, wy, ww, wh);
    strcpy(fps_last, fps_shown);  /* the status line is current now */
}

static void redraw_rect(void (*fn)(const GRECT *), const GRECT *r)
{
    if (!iconified)
        redraw(fn, r->g_x, r->g_y, r->g_w, r->g_h);
}

/* one widget out of the band: press feedback, one half of a hover */
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

static void redraw_clock(void)
{
    GRECT r;

    vidui_clock_rect(&ui, &r);
    redraw_rect(draw_clock, &r);
}

static void redraw_info(void)
{
    GRECT r;

    vidui_info_rect(&ui, &r);
    redraw_rect(draw_info, &r);
}

static void redraw_status(void)
{
    GRECT r;

    vidui_status_rect(&ui, &r);
    redraw_rect(draw_status, &r);
}

/* the pane (picture box or playlist), nothing else */
static void redraw_pane(void)
{
    GRECT r;

    vidui_pane_rect(&ui, &r);
    redraw_rect(draw_list, &r);
}

static void set_top(long t)
{
    long max = (long)ui.ntracks - ui.visrows;

    if (max < 0) max = 0;
    if (t > max) t = max;
    if (t < 0)   t = 0;
    if ((short)t != ui.top) {
        ui.top = (short)t;
        if (ui.listmode)
            redraw_pane();
    }
}

/* ------------------------------------------------------------- overlay --- */

/* Which part of the picture box is ACTUALLY visible? The overlay is a
 * hardware plane above the whole screen, so a window overlapping ours would
 * otherwise sit BEHIND the picture. The AES rectangle list already knows
 * the visible fragments; hand the host the largest one as a clip. One
 * plane means one rectangle, so an L-shaped region is approximated by its
 * biggest piece - windows overlap from one side and that is exactly right. */
static void push_clip(const GRECT *area)
{
    GRECT r, best;
    long area_px = 0;

    best.g_x = best.g_y = best.g_w = best.g_h = 0;
    wind_get(win, WF_FIRSTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
    while (r.g_w > 0 && r.g_h > 0) {
        GRECT i = r;
        if (rc_intersect((GRECT *)area, &i)) {
            long px = (long)i.g_w * i.g_h;
            if (px > area_px) {
                area_px = px;
                best = i;
            }
        }
        wind_get(win, WF_NEXTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
    }

    if (area_px <= 0) {                    /* completely covered */
        nf_call(vidid | NF_VID_CLIP, 0L, 0L, 1L, 1L);
        return;
    }
    if (best.g_x <= area->g_x && best.g_y <= area->g_y &&
        best.g_x + best.g_w >= area->g_x + area->g_w &&
        best.g_y + best.g_h >= area->g_y + area->g_h) {
        nf_call(vidid | NF_VID_CLIP, 0L, 0L, 0L, 0L);   /* all of it visible */
        return;
    }
    nf_call(vidid | NF_VID_CLIP,
            (long)best.g_x * disp_w / scr_w,
            (long)best.g_y * disp_h / scr_h,
            (long)best.g_w * disp_w / scr_w,
            (long)best.g_h * disp_h / scr_h);
}

/* the pane message for the current state */
static void set_msg(void)
{
    if (!ui.playing)
        ui.msg = VID_MSG_IDLE;
    else if (floor_unfit)
        ui.msg = VID_MSG_FLOOR_UNFIT;
    else if (ui.min_dw < 0)
        ui.msg = VID_MSG_WAIT;
    else
        ui.msg = VID_MSG_NONE;
}

/* Push the window geometry to the overlay, letterboxed to the film's aspect
 * inside the picture box. Hidden (negative w/h) while iconified, in list
 * mode, before the host knows its floor, or when the floor cannot fit the
 * desktop - the sound carries on regardless. */
static void update_rect(void)
{
    GRECT a;
    long  dx, dy, dw, dh, fitw, fith;

    if (!vidid)
        return;
    set_msg();

    if (iconified || ui.listmode || !ui.playing ||
        ui.min_dw < 0 || floor_unfit) {
        nf_call(vidid | NF_VID_RECT, 0L, 0L, -1L, -1L);
        return;
    }
    if (ui.fullscreen) {
        nf_call(vidid | NF_VID_CLIP, 0L, 0L, 0L, 0L);
        nf_call(vidid | NF_VID_RECT, 0L, 0L, 0L, 0L);   /* auto letterbox */
        return;
    }

    vidui_pane_rect(&ui, &a);
    if (a.g_w < 8 || a.g_h < 8 || scr_w <= 0 || scr_h <= 0)
        return;

    /* Atari screen pixels -> real display pixels */
    dx = (long)a.g_x * disp_w / scr_w;
    dy = (long)a.g_y * disp_h / scr_h;
    dw = (long)a.g_w * disp_w / scr_w;
    dh = (long)a.g_h * disp_h / scr_h;

    if (ui.vid_w > 0 && ui.vid_h > 0) {
        fitw = dw;
        fith = dw * ui.vid_h / ui.vid_w;
        if (fith > dh) {
            fith = dh;
            fitw = dh * ui.vid_w / ui.vid_h;
        }
        dx += (dw - fitw) / 2;
        dy += (dh - fith) / 2;
        dw = fitw;
        dh = fith;
    }
    if (dw < 2) dw = 2;
    if (dh < 2) dh = 2;
    nf_call(vidid | NF_VID_RECT, dx, dy, dw, dh);
    push_clip(&a);
}

/* The window's minimum, outer size: the layout's own floor, raised to the
 * overlay's floor once the host has decoded a frame (it cannot draw the
 * picture smaller than that - memory bandwidth, see VIDEO.md). The floor
 * comes in DISPLAY pixels and is converted rounding UP: two floor
 * divisions in opposite directions used to land a pixel short. */
static void compute_min_window(void)
{
    short mw, mh, cx, cy, cwid, chgt;
    long  need_aw, need_ah, pane_min;

    vidui_minsize(vh, &mw, &mh);
    if (ui.min_dw > 0 && disp_w > 0 && disp_h > 0) {
        need_aw = (ui.min_dw * scr_w + disp_w - 1) / disp_w + 4;
        need_ah = (ui.min_dh * scr_h + disp_h - 1) / disp_h + 4;
        pane_min = apj_skin_ok() ? apj_skin_m(90) : 90;
        if (need_aw + 2 * apj_skin_m(10) > mw)
            mw = (short)(need_aw + 2 * apj_skin_m(10));
        if (mh - pane_min + need_ah > mh)
            mh = (short)(mh - pane_min + need_ah);
    }
    wind_calc(WC_BORDER, WIN_KIND, 0, 0, mw, mh, &cx, &cy, &cwid, &chgt);
    min_out_w = cwid;
    min_out_h = chgt;
}

/* Grow the window to its minimum if it is under it. Returns 1 if the
 * minimum cannot be met at all (bigger than the desktop). */
static int apply_min_size(void)
{
    short cx, cy, cwid, chgt, dx, dy, dw, dh;

    if (!min_out_w)
        return 0;
    wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
    if (min_out_w > dw || min_out_h > dh)
        return 1;

    wind_get(win, WF_CURRXYWH, &cx, &cy, &cwid, &chgt);
    if (cwid >= min_out_w && chgt >= min_out_h)
        return 0;
    if (cwid < min_out_w) cwid = (short)min_out_w;
    if (chgt < min_out_h) chgt = (short)min_out_h;
    if (cx + cwid > dx + dw) cx = (short)(dx + dw - cwid);
    if (cy + chgt > dy + dh) cy = (short)(dy + dh - chgt);
    if (cx < dx) cx = dx;
    if (cy < dy) cy = dy;
    wind_set(win, WF_CURRXYWH, cx, cy, cwid, chgt);
    return 0;
}

static void arm_m1(void);

static void relayout(void)
{
    wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
    vidui_layout(&ui, vh, wx, wy, ww, wh);
    set_top(ui.top);                  /* clamps after a resize */
    update_rect();
}

/* ------------------------------------------------------------- playback -- */

static void read_meta(void)
{
    char t[128], a[128], c[64];

    nf_meta(0, t, sizeof(t));
    nf_meta(1, a, sizeof(a));
    nf_meta(2, c, sizeof(c));

    if (t[0])
        strncpy(ui_title, t, sizeof(ui_title) - 1);
    else if (ui.sel >= 0 && ui.sel < ntracks)
        strncpy(ui_title, list[ui.sel], sizeof(ui_title) - 1);
    ui_title[sizeof(ui_title) - 1] = '\0';
    strncpy(ui_sub, a, sizeof(ui_sub) - 1);
    ui_sub[sizeof(ui_sub) - 1] = '\0';
    strncpy(ui_codec, c, sizeof(ui_codec) - 1);
    ui_codec[sizeof(ui_codec) - 1] = '\0';
    ui.codec = ui_codec[0] ? ui_codec : NULL;
}

static void start_track(int i)
{
    char path[PATHLEN + NAMELEN + 2];

    if (i < 0 || i >= ntracks)
        return;
    sprintf(path, "%.511s\\%.255s", dir, list[i]);
    if (nf_call(vidid | NF_VID_PLAY, path) == 0) {
        ui.sel = (short)i;
        ui.playing = 1;
        ui.paused = 0;
        ui.len_s = nf_call(vidid | NF_VID_LEN);
        ui.pos_s = 0;
        ui.vid_w = nf_call(vidid | NF_VID_INFO, 0L);
        ui.vid_h = nf_call(vidid | NF_VID_INFO, 1L);
        ui.fps100 = nf_call(vidid | NF_VID_INFO, 2L);
        if (ui.fps100 < 0) ui.fps100 = 0;
        ui.min_dw = ui.min_dh = -1;   /* not known until a frame is decoded */
        floor_unfit = 0;
        nf_call(vidid | NF_VID_VOLUME, (long)ui.vol);
        read_meta();
    } else {
        ui.playing = 0;
        /* PLAY fails because the host cannot open the file (wrong drive,
         * unsupported codec) or because there is no overlay. INFO 6 still
         * answering means the overlay is fine, so it is the file. */
        sprintf(ui_title, "Cannot play %.120s", list[i]);
        strcpy(ui_sub, nf_call(vidid | NF_VID_INFO, 6L) > 0
                       ? "Not on a HOSTFS drive, or the codec is not supported"
                       : "No video overlay - the emulator is not on its DRM display path");
        ui.codec = NULL;
        ui.vid_w = ui.vid_h = 0;
        ui.fps100 = 0;
    }
    act_fps = 0;
    fps_shown[0] = '\0';
    compute_min_window();
    update_rect();
}

static void stop_track(void)
{
    nf_call(vidid | NF_VID_STOP);
    ui.playing = 0;
    ui.paused = 0;
    ui.pos_s = 0;
    ui.vid_w = ui.vid_h = 0;
    ui.fps100 = 0;
    ui.min_dw = ui.min_dh = -1;
    floor_unfit = 0;
    act_fps = 0;
    fps_shown[0] = '\0';
    compute_min_window();
    update_rect();
}

/* ------------------------------------------------------------- playlist -- */

static int is_video(const char *name)
{
    size_t n = strlen(name);
    int i;

    for (i = 0; vexts[i]; i++) {
        size_t e = strlen(vexts[i]);
        if (n > e && strcasecmp(name + n - e, vexts[i]) == 0)
            return 1;
    }
    return 0;
}

static int cmpname(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static int load_dir(const char *d)
{
    DIR *dp = opendir(d);
    struct dirent *e;

    if (!dp)
        return -1;
    ntracks = 0;
    while ((e = readdir(dp)) != NULL && ntracks < MAXTRACKS) {
        if (is_video(e->d_name) && strlen(e->d_name) < NAMELEN) {
            strcpy(list[ntracks], e->d_name);
            ntracks++;
        }
    }
    closedir(dp);
    qsort(list, ntracks, NAMELEN, cmpname);
    ui.ntracks = (short)ntracks;
    ui.top = 0;
    ui.sel = ntracks ? 0 : -1;
    return ntracks;
}

static void scroll_to(short i)
{
    if (i < ui.top)
        ui.top = i;
    if (ui.visrows > 0 && i >= ui.top + ui.visrows)
        ui.top = (short)(i - ui.visrows + 1);
}

/* Load the folder of path as the playlist and start fname in it */
static void open_in_dir(const char *fpath, const char *fname)
{
    int i, found = -1;

    strncpy(dir, fpath, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    { char *bs = strrchr(dir, '\\'); if (bs) *bs = '\0'; }
    ui.dir = dir;

    if (load_dir(dir) > 0) {
        for (i = 0; i < ntracks; i++)
            if (strcasecmp(list[i], fname) == 0) { found = i; break; }
        if (found < 0) {
            /* playing sel (0) anyway would start SOME OTHER FILE, which
             * looks like the player ignoring the choice */
            sprintf(ui_title, "%.120s", fname);
            strcpy(ui_sub, "Not a video file this player recognises");
        } else {
            ui.sel = (short)found;
            scroll_to(ui.sel);
            ui.listmode = 0;
            start_track(found);
        }
    } else {
        strcpy(ui_title, "No video files here");
        sprintf(ui_sub, "%.180s", dir);
    }
    relayout();
    redraw_all();
}

static void do_open(void)
{
    /* BOTH are written by the AES, which decides how much to write; keep
     * them static and generously sized (a long HOSTFS name once took the
     * return address with it). */
    static char fpath[PATHLEN] = "";
    static char fname[NAMELEN] = "";
    short btn = 0;

    fname[0] = '\0';
    if (!fpath[0]) {
        fpath[0] = (char)('A' + Dgetdrv());
        strcpy(fpath + 1, ":\\*.*");
    }

    /* The overlay composites above the whole screen - the selector would
     * open BEHIND a paused picture. Put the picture away for the dialog;
     * playback and sound carry on. */
    nf_call(vidid | NF_VID_RECT, 0L, 0L, -1L, -1L);

    fsel_exinput(fpath, fname, &btn, "Select a video (HOSTFS drive)");

    if (btn != 1 || !fname[0]) {
        update_rect();            /* cancelled - put the picture back */
        return;
    }
    open_in_dir(fpath, fname);
}

/* A file handed to us - on the command line (double-clicked in the desktop)
 * or in a VA_START while running: "S:\MEDIA\CLIP.MP4", possibly quoted. */
static void play_path(const char *arg)
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
    if ((bs = strrchr(full, '\\')) == NULL || !bs[1])
        return;
    open_in_dir(full, bs + 1);
}

/* ------------------------------------------------------------- scale ----- */

/*
 * Keys 1, 2 and 3 pick the 100, 125 or 175 percent sheet, 0 goes back to
 * the screen's choice; kept in <boot>:\VIDGEM.INF as scale=NNN.
 */
/* The .PRG lives on the read-only HOSTFS share, so the .INF goes to the
 * root of the boot drive (_bootdev at $446; C: if that looks wrong). */
static long boot_drive_sv(void) { return *(volatile short *)0x446L; }

static void inf_path(char *out, long n)
{
    long d = Supexec(boot_drive_sv);

    if (d < 0 || d > 25)
        d = 2;                              /* C: */
    sprintf(out, "%c:\\VIDGEM.INF", (char)('A' + d));
    (void)n;
}

static short inf_scale(void)
{
    char path[224], buf[128], *p;
    long fh, got;

    inf_path(path, (long)sizeof path);
    fh = Fopen(path, 0);
    if (fh < 0)
        return 0;
    got = Fread((short)fh, (long)sizeof buf - 1, buf);
    Fclose((short)fh);
    if (got <= 0)
        return 0;
    buf[got] = 0;
    p = strstr(buf, "scale=");
    return p ? (short)atoi(p + 6) : 0;
}

static void inf_save(short scale)
{
    char path[224], buf[32];
    long fh;

    inf_path(path, (long)sizeof path);
    fh = Fcreate(path, 0);
    if (fh < 0)
        return;
    sprintf(buf, "scale=%d\r\n", (int)scale);
    Fwrite((short)fh, (long)strlen(buf), buf);
    Fclose((short)fh);
}

/* the work-area size this scale wants when nothing else decides: never
 * under the layout's minimum, never over the desktop */
static void natural_size(short *w, short *h)
{
    short dx, dy, dw, dh, mw, mh;

    *w = apj_skin_ok() ? apj_skin_m(640) : (short)(70 * cw);
    *h = apj_skin_ok() ? apj_skin_m(480) : (short)(24 * ch);
    vidui_minsize(vh, &mw, &mh);
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
    inf_save(scale);
    /* the window keeps its size: the chrome changes, the picture box gives */
    compute_min_window();
    floor_unfit = apply_min_size();
    relayout();
    arm_m1();
    redraw_all();
}

/* ------------------------------------------------------------- hover ----- */

/*
 * MU_M1 gives one rectangle. Over a widget: hear about the pointer LEAVING
 * it. Elsewhere inside the strip: LEAVING a 3x3 box around where it is,
 * because asking to ENTER a strip the pointer is already in is a condition
 * that is already true - the AES answers at once, forever, and the timer
 * never elapses. Outside the strip: ENTERING it.
 */
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
    vidui_bbox(&ui, &m1r);
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
    short h = vidui_hit(&ui, mx, my);
    short old = ui.hover;

    if (h == W_LIST || h == W_VIDEO || h == W_SCROLL)
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

/* ------------------------------------------------------------- actions --- */

static void next_track(int step)
{
    int i;

    if (!ntracks)
        return;
    i = ui.sel + step;
    if (i < 0)
        i = 0;
    if (i >= ntracks) {
        stop_track();
        redraw_all();
        return;
    }
    scroll_to((short)i);
    start_track(i);
    redraw_all();
}

static void set_volume(short v)
{
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    if (v == ui.vol)
        return;
    ui.vol = v;
    nf_call(vidid | NF_VID_VOLUME, (long)v);
    redraw_widget(W_VOL);
    redraw_widget(W_VOLICO);
}

static void set_pause(short p)
{
    if (!ui.playing || p == ui.paused)
        return;
    ui.paused = p;
    nf_call(vidid | NF_VID_PAUSE, (long)p);
    redraw_widget(W_PLAY);
    redraw_info();                /* the PLAYING / PAUSED badge */
}

static void set_fullscreen(short f)
{
    if (f == ui.fullscreen)
        return;
    ui.fullscreen = f;
    if (f)
        ui.listmode = 0;          /* you cannot see a list under a film */
    relayout();
    redraw_all();
}

static void do_widget(short id, short mx)
{
    const APJ_LAY *l;

    switch (id) {
    case W_PREV: next_track(-1); break;
    case W_NEXT: next_track(+1); break;
    case W_RW:   nf_call(vidid | NF_VID_SEEK, (long)-10); break;
    case W_FF:   nf_call(vidid | NF_VID_SEEK, (long)+10); break;
    case W_PLAY:
        if (ui.playing)
            set_pause(!ui.paused);
        else {
            start_track(ui.sel);
            relayout();
            redraw_all();
        }
        break;
    case W_STOP:
        stop_track();
        relayout();
        redraw_all();
        break;
    case W_LOOP:
        ui.loop = !ui.loop;
        break;
    case W_OPEN:
        do_open();
        break;
    case W_LISTTOG:
        ui.listmode = !ui.listmode;
        if (ui.listmode)
            ui.fullscreen = 0;
        relayout();               /* hides / restores the overlay */
        redraw_all();
        break;
    case W_FULL:
        set_fullscreen(!ui.fullscreen);
        break;
    case W_VOLICO:                    /* mute toggles, and remembers */
        if (ui.vol) {
            vol_before_mute = ui.vol;
            set_volume(0);
        } else
            set_volume(vol_before_mute > 0 ? vol_before_mute : 100);
        break;
    case W_VOL:
        l = apj_lay_find(ui.lay, ui.nlay, W_VOL);
        if (l && l->w > 0)
            set_volume((short)((long)(mx - l->x) * 100L / l->w));
        break;
    case W_SEEK:
        /* VIDPLAY seeks by a delta, so aim at the clicked position */
        l = apj_lay_find(ui.lay, ui.nlay, W_SEEK);
        if (l && l->w > 0 && ui.len_s > 0 && ui.playing) {
            long want = (long)(mx - l->x) * ui.len_s / l->w;
            nf_call(vidid | NF_VID_SEEK, want - ui.pos_s);
        }
        break;
    default:
        return;
    }
}

static void click(short mx, short my)
{
    short id = vidui_hit(&ui, mx, my);
    short row;

    if (id == W_SCROLL) {
        short part;

        if (!vidui_scroll_needed(&ui))
            return;
        part = vidui_scroll_part(&ui, my);
        if (part < 0)
            set_top(ui.top - ui.visrows);
        else if (part > 0)
            set_top(ui.top + ui.visrows);
        else {
            /* drag the thumb: follow the mouse until the button goes up */
            GRECT t;
            short grab, bmx, bmy, bst, bks;

            vidui_thumb_rect(&ui, &t);
            grab = (short)(my - t.g_y);
            ui.dragging = 1;
            redraw_pane();
            for (;;) {
                graf_mkstate(&bmx, &bmy, &bst, &bks);
                if (!(bst & 1))
                    break;
                set_top(vidui_scroll_top_for(&ui, bmy, grab));
                evnt_timer(20L);
            }
            ui.dragging = 0;
            redraw_pane();
        }
        return;
    }

    if (id >= 0 && id != W_LIST && id != W_VIDEO) {
        /* press feedback on that one widget, then the action */
        ui.press = id;
        redraw_widget(id);
        evnt_timer(70L);
        ui.press = -1;
        do_widget(id, mx);
        redraw_widget(id);        /* back out of the pressed look */
        return;
    }
    if (id == W_LIST) {
        row = vidui_row_at(&ui, my);
        if (row >= 0) {
            ui.listmode = 0;
            start_track(row);
            relayout();
            redraw_all();
        }
    }
}

/* ------------------------------------------------------------- timer ----- */

/* the host knows its floor once a frame is decoded: grow the window to it */
static void poll_floor(void)
{
    long w = nf_call(vidid | NF_VID_INFO, (long)VI_MIN_W);
    long h = nf_call(vidid | NF_VID_INFO, (long)VI_MIN_H);

    if (w < 0 || h < 0)
        return;
    ui.min_dw = w;
    ui.min_dh = h;
    if (ui.vid_w <= 0) {
        ui.vid_w = nf_call(vidid | NF_VID_INFO, 0L);
        ui.vid_h = nf_call(vidid | NF_VID_INFO, 1L);
    }
    compute_min_window();
    floor_unfit = apply_min_size();
    relayout();
    redraw_all();                 /* the badge row and the pane message */
}

static void tick(void)
{
    int clock = 0;

    if (!ui.playing)
        return;
    if (ui.min_dw < 0 && !iconified)
        poll_floor();
    if (ui.paused)
        return;

    {
        long p = nf_call(vidid | NF_VID_POS);

        if (p >= 0 && p != ui.pos_s) {
            ui.pos_s = p;
            clock = 1;            /* once a second, not every tick */
        }
    }
    if (nf_call(vidid | NF_VID_STATUS) == 0) {
        /* Loop this file, or stop. Wandering into the next file by
         * itself is rarely what you want from a video player. */
        if (ui.loop)
            start_track(ui.sel);
        else
            stop_track();
        relayout();
        redraw_all();
        return;
    }

    act_fps = nf_call(vidid | NF_VID_INFO, (long)VI_FPS_NOW);
    if (act_fps < 0)
        act_fps = 0;
    if (act_fps > 0)
        sprintf(fps_shown, "%ld.%ld fps shown", act_fps / 100, (act_fps / 10) % 10);
    else
        fps_shown[0] = '\0';
    ui.shown = fps_shown;

    if (iconified)
        return;
    if (clock)
        redraw_clock();
    /* the figure moves without changing its string most of the time; and
     * it is rounded to a tenth, so at most once a second it can differ */
    if (clock && strcmp(fps_shown, fps_last) != 0) {
        strcpy(fps_last, fps_shown);
        redraw_status();
    }
}

/* ---------------------------------------------------------------- main --- */

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
    vidid = nf_id("VIDPLAY");
    if (!vidid) {
        form_alert(1, "[3][VIDPLAY NatFeat not found.|"
                      "Run under the PiSTorm emulator.][ OK ]");
        appl_exit();
        return 1;
    }

    vh = graf_handle(&cw, &ch, &d, &d);
    for (i = 0; i < 10; i++) work_in[i] = 1;
    work_in[10] = 2;
    v_opnvwk(work_in, &vh, work_out);
    vst_alignment(vh, 0, 5, &d, &d);              /* left / top text origin */
    apj_init(vh);                                 /* theme + renderer, before any window */
    apj_skin_prefer(inf_scale());                 /* VIDGEM.INF, else by screen */
    apj_skin_load(vh, NULL);                      /* follows the theme; may fail */

    scr_w = (short)(work_out[0] + 1);
    scr_h = (short)(work_out[1] + 1);

    /* The picture is a hardware overlay above whatever the emulator scans
     * out: the screen driver is irrelevant. What matters is the VIDPLAY
     * NatFeat, the emulator's DRM display path (the overlay shares its
     * CRTC), and an AES for the window and the visible-region list. The
     * one driver-shaped assumption: the Atari screen fills the display,
     * which is what the DRM presenter does for every mode. */
    if (scr_w < 32 || scr_h < 32) {
        form_alert(1, "[3][VIDGEM cannot read the screen size.|"
                      "This screen driver is not usable.][ OK ]");
        v_clsvwk(vh);
        appl_exit();
        return 1;
    }
    disp_w = nf_call(vidid | NF_VID_INFO, 6L);
    disp_h = nf_call(vidid | NF_VID_INFO, 7L);
    if (disp_w <= 0 || disp_h <= 0) {
        form_alert(1, "[3][No video overlay is available.|"
                      "The emulator must be using its DRM|"
                      "display path (PISTORM_VGA_DRM=1),|"
                      "and needs a spare overlay plane.][ OK ]");
        v_clsvwk(vh);
        appl_exit();
        return 1;
    }

    memset(&ui, 0, sizeof(ui));
    ui.title = ui_title;
    ui.sub = ui_sub;
    ui.name_of = name_of;
    ui.sel = -1;
    ui.hover = -1;
    ui.press = -1;
    ui.vol = 100;
    ui.hasvol = 1;
    ui.min_dw = ui.min_dh = -1;
    ui.msg = VID_MSG_IDLE;
    ui.shown = fps_shown;

    {
        short dx, dy, dw, dh, cx, cy, cwid, chgt;
        short want_w, want_h;

        natural_size(&want_w, &want_h);
        wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
        wind_calc(WC_BORDER, WIN_KIND,
                  dx + 16, dy + 16, want_w, want_h, &cx, &cy, &cwid, &chgt);
        if (cx + cwid > dx + dw) cwid = (short)(dx + dw - cx);
        if (cy + chgt > dy + dh) chgt = (short)(dy + dh - cy);
        win = wind_create(WIN_KIND, dx, dy, dw, dh);
        wind_set_str(win, WF_NAME, "PiSTorm Video");
        wind_open(win, cx, cy, cwid, chgt);
        wind_set(win, WF_WHEEL, 1, WHEEL_ARROWED, 0, 0);
        compute_min_window();
        relayout();
    }
    arm_m1();
    redraw_all();

    if (argc > 1)                         /* a file double-clicked in the desktop */
        play_path(argv[1]);

    for (;;) {
        ev = evnt_multi(MU_MESAG | MU_BUTTON | MU_KEYBD | MU_TIMER | MU_M1,
                        1, 1, 1,
                        m1flag, m1r.g_x, m1r.g_y, m1r.g_w, m1r.g_h,
                        0, 0, 0, 0, 0,
                        msg, 250UL,
                        &mx, &my, &mb, &ks, &kr, &brk);

        if (ev & MU_MESAG) {
            switch (msg[0]) {
                case WM_REDRAW:
                    redraw(draw_all, msg[4], msg[5], msg[6], msg[7]);
                    update_rect();        /* occlusion may have changed */
                    break;
                case WM_TOPPED:
                    wind_set(win, WF_TOP, 0, 0, 0, 0);
                    update_rect();
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

                    if (n < 1) n = 1;
                    switch (msg[4] & 15) {
                        case WA_UPLINE: set_top(ui.top - n); break;
                        case WA_DNLINE: set_top(ui.top + n); break;
                        case WA_UPPAGE: set_top(ui.top - ui.visrows); break;
                        case WA_DNPAGE: set_top(ui.top + ui.visrows); break;
                    }
                    break;
                }
                case WM_CLOSED:
                    goto out;
                case WM_ICONIFY:
                case WM_ALLICONIFY:       /* minimise (to the taskbar under APJ-OS) */
                    iconified = 1;
                    update_rect();        /* picture off first - it sits above GEM */
                    wind_set(win, WF_ICONIFY, msg[4], msg[5], msg[6], msg[7]);
                    relayout();
                    break;
                case WM_UNICONIFY:
                    wind_set(win, WF_UNICONIFY, msg[4], msg[5], msg[6], msg[7]);
                    iconified = 0;
                    relayout();           /* puts the picture back */
                    arm_m1();
                    redraw_all();
                    break;
                case APJ_SKINCHG:         /* the desktop changed theme */
                    apj_init(vh);
                    apj_skin_reload(vh);
                    compute_min_window();
                    floor_unfit = apply_min_size();
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
                    if (cmd)
                        play_path(cmd);
                    wind_set(win, WF_TOP, 0, 0, 0, 0);
                    update_rect();
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
            char  c    = (char)(kr & 0xff);
            short scan = (short)((kr >> 8) & 0xff);

            /* the PiSTorm's USB bridge turns wheel clicks into cursor
             * Up/Down taps, so the wheel arrives here */
            if      (scan == 0x48) set_top(ui.top - 1);
            else if (scan == 0x50) set_top(ui.top + 1);
            else if (scan == 0x49) set_top(ui.top - ui.visrows);
            else if (scan == 0x51) set_top(ui.top + ui.visrows);
            else if (scan == 0x4B) do_widget(W_RW, 0);
            else if (scan == 0x4D) do_widget(W_FF, 0);
            else if (c == ' ')             do_widget(W_PLAY, 0);
            else if (c == 'n' || c == 'N') do_widget(W_NEXT, 0);
            else if (c == 'p' || c == 'P') do_widget(W_PREV, 0);
            else if (c == 'l' || c == 'L') do_widget(W_LISTTOG, 0);
            else if (c == 'r' || c == 'R') { ui.loop = !ui.loop; redraw_widget(W_LOOP); }
            else if (c == 'o' || c == 'O') do_widget(W_OPEN, 0);
            else if (c == 'f' || c == 'F') set_fullscreen(1);
            else if (c == 'w' || c == 'W') set_fullscreen(0);
            else if (c == 0x1b)            { if (ui.fullscreen) set_fullscreen(0); else goto out; }
            else if (c == '+' || c == '=') set_volume((short)(ui.vol + 10));
            else if (c == '-')             set_volume((short)(ui.vol - 10));
            else if (c == 'q' || c == 'Q') goto out;
            else if (c == '1') set_scale(100);   /* the sheet to use ... */
            else if (c == '2') set_scale(125);
            else if (c == '3') set_scale(175);
            else if (c == '0') set_scale(0);     /* ... or the screen's choice */
        }
        if (ev & MU_TIMER)
            tick();
    }

out:
    /* an overlay left on screen with nothing to control it would sit on
     * top of the desktop: stop, and hide it */
    nf_call(vidid | NF_VID_STOP);
    nf_call(vidid | NF_VID_RECT, 0L, 0L, -1L, -1L);
    apj_skin_free();
    wind_close(win);
    wind_delete(win);
    v_clsvwk(vh);
    appl_exit();
    return 0;
}
