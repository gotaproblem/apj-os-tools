/*
 * vidgem.c - PiSTorm host VIDEO player, GEM front-end (VIDGEM.PRG)
 *
 * Controls the "VIDPLAY" NatFeat: the Pi demuxes and decodes the file
 * (libavformat/libavcodec, hardware H.264 on a Pi 4) and puts the picture on
 * its own DRM overlay plane, with the soundtrack mixed into HDMI audio
 * alongside ST/STE sound. This app is the remote control: playlist,
 * play/pause/stop, prev/next, rewind/FF, volume, and - the part that makes it
 * feel native - it maps the picture onto its own WINDOW, so the film plays
 * inside the window and follows it as you move and resize it.
 *
 * How the window mapping works: the Atari screen (say 640x480) is upscaled by
 * the Pi's HVS to the real display (say 1920x1080). The overlay lives in
 * display pixels, so the window's work area is scaled by disp/screen before
 * being handed to VID_RECT. VID_INFO 6/7 report the display size.
 *
 * Files must live on a HOSTFS drive (the host opens them by path).
 *
 * Build:  make            (m68k-atari-mint-gcc, links -lgem)
 * Run:    VIDGEM.PRG from the desktop (GEM app, needs AES; FreeMiNT+XaAES ok)
 */

#include <gem.h>
#include <osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#define APJGUI_IMPL
#include "../apjgui/apjgui.h"          /* APJ-OS Fluent look (degrades to plain VDI) */

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
/* NAMELEN was 64, which is not a safe size for anything that came off a NAS.
 * Two separate things went wrong with it:
 *   - a file whose name reached 64 characters was silently dropped from the
 *     playlist, so the file just picked in the selector was not in the list,
 *     the name lookup missed, and track 0 played instead;
 *   - fsel_exinput() writes the chosen name into OUR buffer and it is the AES
 *     that decides how much to write. Under MiNT with long filenames that is
 *     comfortably past 64 bytes, so a long name overran a 64-byte STACK buffer
 *     and took the return address with it. 128 is the figure usually quoted
 *     for the AES name buffer; 256 costs nothing here and ends the argument. */
#define NAMELEN   256
#define PATHLEN   512
#define VISROWS   10          /* playlist rows shown in list mode */
#define MARQW     46          /* marquee width in characters */

static long  vidid;
static short vh;                          /* VDI handle */
static short win = -1;
static short cw, ch;                      /* char cell size */
static short wx, wy, ww, wh;              /* window work area */
static short scr_w, scr_h;                /* Atari screen size, pixels */
static long  disp_w, disp_h;              /* real display size, pixels */

static char  dir[PATHLEN] = "";           /* playlist directory (GEMDOS path) */
static char  list[MAXTRACKS][NAMELEN];
static int   ntracks = 0;
static int   sel = -1;                    /* selected / playing index */
static int   top = 0;                     /* first visible playlist row */
static int   playing = 0, paused = 0, listmode = 0, fullscreen = 0;
static int   volume = 100;
static int   loopmode = 0;                /* repeat this file at the end */
static long  vid_fps = 0;                 /* file's frame rate x100 */
static long  act_fps = 0;                 /* what is really being shown x100 */
static long  pos_s = 0, len_s = 0;
static long  vid_w = 0, vid_h = 0;        /* decoded picture size */
static long  min_dw = -1, min_dh = -1;   /* -1 unknown, 0 none, >0 the floor */
static short min_out_w = 0, min_out_h = 0;   /* smallest usable window size */
static int   floor_unfit = 0;   /* the floor is bigger than the desktop */

#define WIN_KIND (NAME | CLOSER | MOVER | SIZER | FULLER)
static int   too_big = 0;                 /* picture cannot fit the window */

static char  marquee[400] = "PiSTorm video - Open a file...   ";
static int   moff = 0;

/* button row */
static const char *btxt[11] = { "|<", "<<", " > ", "||", "[]", ">>", ">|",
                                "Open", "List", "Full", "Loop" };
enum { B_PREV, B_RW, B_PLAY, B_PAUSE, B_STOP, B_FF, B_NEXT,
       B_OPEN, B_LIST, B_FULL, B_LOOP, NBUTTONS };

/* Video file extensions we put in the playlist. */
static const char *vexts[] = { ".MP4", ".MKV", ".AVI", ".MOV", ".M4V",
                               ".WEBM", ".TS", ".MPG", ".MPEG", ".OGV", NULL };

/* ------------------------------------------------------------- layout ---- */
/* rows (in char cells from top of work area):
 * 0: marquee   1: time/status   2: buttons   3..: video area / playlist  */

static void btn_rect(int i, short *x, short *y, short *w, short *h)
{
    *w = 4 * cw;                 /* 11 buttons have to fit the window */
    *h = ch + 4;
    *x = wx + 2 + i * (*w + cw / 4);
    *y = wy + 2 * ch + 6;
}

static short video_y0(void) { return wy + 3 * ch + 12; }

static void video_area(short *x, short *y, short *w, short *h)
{
    *x = wx;
    *y = video_y0();
    *w = ww;
    *h = (short)(wy + wh - *y);
    if (*h < 0)
        *h = 0;
}

/* ------------------------------------------------------------- NF glue --- */

static void nf_meta(int which, char *buf, int len)
{
    buf[0] = '\0';
    nf_call(vidid | NF_VID_META, (long)which, buf, (long)len);
    buf[len - 1] = '\0';
}

/* Which part of our video area is ACTUALLY visible?
 *
 * The overlay is a hardware plane: it composites above the entire Atari screen,
 * so a GEM window overlapping ours would otherwise be drawn behind the picture.
 * The AES already knows the answer - wind_get(WF_FIRSTXYWH/WF_NEXTXYWH) walks
 * the visible fragments of our work area, excluding anything covered by windows
 * above us. Take the largest fragment that overlaps the video area and hand it
 * to the host as a clip; it crops both the destination and the source, so the
 * visible slice is not stretched.
 *
 * One plane means one rectangle, so an L-shaped visible region can only be
 * approximated by its biggest piece. In practice windows overlap from one side
 * and this is exactly right. */
static void push_clip(short vx, short vy, short vw, short vh)
{
    GRECT area, r, best;
    long area_px = 0;

    area.g_x = vx; area.g_y = vy; area.g_w = vw; area.g_h = vh;
    best.g_x = best.g_y = best.g_w = best.g_h = 0;

    wind_get(win, WF_FIRSTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
    while (r.g_w > 0 && r.g_h > 0) {
        GRECT i = r;
        if (rc_intersect(&area, &i)) {
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
    if (best.g_x <= vx && best.g_y <= vy &&
        best.g_x + best.g_w >= vx + vw && best.g_y + best.g_h >= vy + vh) {
        nf_call(vidid | NF_VID_CLIP, 0L, 0L, 0L, 0L);   /* all of it visible */
        return;
    }
    nf_call(vidid | NF_VID_CLIP,
            (long)best.g_x * disp_w / scr_w,
            (long)best.g_y * disp_h / scr_h,
            (long)best.g_w * disp_w / scr_w,
            (long)best.g_h * disp_h / scr_h);
}

/* Push the current window geometry to the overlay, letterboxed to the film's
 * aspect inside the window's video area. In list mode the overlay is hidden
 * (negative w/h) so the playlist underneath becomes visible; the soundtrack
 * carries on regardless. */
static void update_rect(void)
{
    short ax, ay, aw, ah;
    long  dx, dy, dw, dh, fitw, fith;

    if (!vidid)
        return;

    if (listmode) {
        nf_call(vidid | NF_VID_RECT, 0L, 0L, -1L, -1L);
        return;
    }
    if (fullscreen) {
        nf_call(vidid | NF_VID_CLIP, 0L, 0L, 0L, 0L);
        nf_call(vidid | NF_VID_RECT, 0L, 0L, 0L, 0L);   /* auto letterbox */
        return;
    }

    video_area(&ax, &ay, &aw, &ah);
    if (aw < 8 || ah < 8 || scr_w <= 0 || scr_h <= 0)
        return;

    /* Atari screen pixels -> real display pixels */
    dx = (long)ax * disp_w / scr_w;
    dy = (long)ay * disp_h / scr_h;
    dw = (long)aw * disp_w / scr_w;
    dh = (long)ah * disp_h / scr_h;

    /* If the overlay cannot be drawn small enough to sit inside this window
     * (a 4K film needs the whole 1080p screen - see VIDEO.md), do NOT show it
     * anyway: it would cover this app completely, leaving no visible way to
     * stop it, move the window or quit. Keep it hidden, say so in the window,
     * and let the user choose fullscreen with F when they actually want it. */
    /* min_dw < 0 means the host does not know yet (no frame decoded). Until
     * it does, keep the picture hidden: showing it and finding out afterwards
     * that it needs the whole screen would bury this window with no visible
     * way back. */
    /* Since the window is now resized to meet the floor, the only case that
     * still cannot be shown is a floor too big for the DESKTOP. Comparing the
     * computed rect against the floor here as well was fragile: a rounding
     * pixel either way flipped it between showing and hiding. */
    too_big = (min_dw < 0) || floor_unfit;
    if (too_big) {
        nf_call(vidid | NF_VID_RECT, 0L, 0L, -1L, -1L);   /* hide, keep sound */
        return;
    }

    /* Letterbox the picture inside that box so it is not stretched. */
    if (vid_w > 0 && vid_h > 0) {
        fitw = dw;
        fith = dw * vid_h / vid_w;
        if (fith > dh) {
            fith = dh;
            fitw = dh * vid_w / vid_h;
        }
        dx += (dw - fitw) / 2;
        dy += (dh - fith) / 2;
        dw = fitw;
        dh = fith;
    }
    if (dw < 2) dw = 2;
    if (dh < 2) dh = 2;
    nf_call(vidid | NF_VID_RECT, dx, dy, dw, dh);
    push_clip(ax, ay, aw, ah);
}

static void build_marquee(void)
{
    char t[128], a[128], c[128];
    nf_meta(0, t, sizeof(t));
    nf_meta(1, a, sizeof(a));
    nf_meta(2, c, sizeof(c));
    snprintf(marquee, sizeof(marquee), "%s%s%s   [%s]    ",
             t[0] ? t : "(no title)",
             a[0] ? " - " : "", a, c);
    moff = 0;
}

static void start_track(int i)
{
    char path[PATHLEN + NAMELEN + 2];
    if (i < 0 || i >= ntracks)
        return;
    snprintf(path, sizeof(path), "%s\\%s", dir, list[i]);
    if (nf_call(vidid | NF_VID_PLAY, path) == 0) {
        sel = i;
        playing = 1;
        paused = 0;
        len_s = nf_call(vidid | NF_VID_LEN);
        pos_s = 0;
        vid_w = nf_call(vidid | NF_VID_INFO, 0L);
        vid_h = nf_call(vidid | NF_VID_INFO, 1L);
        vid_fps = nf_call(vidid | NF_VID_INFO, 2L);
        min_dw = -1;                 /* not known until a frame is decoded */
        min_dh = -1;
        too_big = 1;
        nf_call(vidid | NF_VID_VOLUME, (long)volume);
        build_marquee();
        update_rect();
    } else {
        playing = 0;
        /* PLAY fails either because the host cannot open the file (wrong
         * drive, unsupported codec) or because the overlay is unavailable.
         * INFO 6 still answering means the overlay is fine, so it is the
         * file - worth distinguishing, they need different fixes. */
        if (nf_call(vidid | NF_VID_INFO, 6L) > 0)
            snprintf(marquee, sizeof(marquee),
                     "Cannot play %s - not on a HOSTFS drive, or the codec is "
                     "not supported   ", list[i]);
        else
            snprintf(marquee, sizeof(marquee),
                     "No video overlay - the emulator is not using its DRM "
                     "display path   ");
        moff = 0;
    }
}

static void stop_track(void)
{
    nf_call(vidid | NF_VID_STOP);
    playing = 0;
    paused = 0;
    pos_s = 0;
    vid_w = vid_h = 0;
    vid_fps = act_fps = 0;
    min_dw = min_dh = -1;
    min_out_w = min_out_h = 0;
    floor_unfit = 0;
    too_big = 0;
}

/* The overlay cannot draw the picture below a certain size (memory bandwidth -
 * see VIDEO.md), so rather than refuse to show it, make the window big enough
 * that it fits, and refuse to let the user shrink back past that. The floor is
 * reported by the host in DISPLAY pixels; the window lives in Atari pixels, so
 * it scales by screen/display - the inverse of the mapping update_rect() uses
 * on the way out. */
static void compute_min_window(void)
{
    short cx, cy, cwid, chgt;
    long need_aw, need_ah;

    min_out_w = min_out_h = 0;
    if (min_dw <= 0 || disp_w <= 0 || disp_h <= 0)
        return;

    /* Display pixels -> Atari pixels, rounding UP. update_rect() converts back
     * the other way, and two floor divisions in opposite directions were
     * landing a pixel or two short of the floor - which is why the window had
     * to be nudged slightly larger by hand before the picture appeared. */
    need_aw = (min_dw * scr_w + disp_w - 1) / disp_w + 4;
    need_ah = (min_dh * scr_h + disp_h - 1) / disp_h + 4;

    /* work area = the video area plus the three bands of controls above it */
    wind_calc(WC_BORDER, WIN_KIND, 0, 0,
              (short)need_aw, (short)(need_ah + 3 * ch + 12),
              &cx, &cy, &cwid, &chgt);
    min_out_w = cwid;
    min_out_h = chgt;
}

/* Grow the window to the floor if it is under it. Returns 1 if the floor
 * cannot be met at all (bigger than the desktop), in which case the caller
 * falls back to fullscreen-only. */
static int apply_min_size(void)
{
    short cx, cy, cwid, chgt, dx, dy, dw, dh;

    if (!min_out_w)
        return 0;
    wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
    if (min_out_w > dw || min_out_h > dh)
        return 1;                      /* the desktop itself is too small */

    wind_get(win, WF_CURRXYWH, &cx, &cy, &cwid, &chgt);
    if (cwid >= min_out_w && chgt >= min_out_h)
        return 0;
    if (cwid < min_out_w) cwid = min_out_w;
    if (chgt < min_out_h) chgt = min_out_h;
    if (cx + cwid > dx + dw) cx = dx + dw - cwid;
    if (cy + chgt > dy + dh) cy = dy + dh - chgt;
    if (cx < dx) cx = dx;
    if (cy < dy) cy = dy;
    wind_set(win, WF_CURRXYWH, cx, cy, cwid, chgt);
    wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
    return 0;
}

/* ------------------------------------------------------------- drawing --- */

static void draw_marquee(void)
{
    char out[MARQW + 1];
    int mlen = (int)strlen(marquee);
    short xy[4];
    int i;

    for (i = 0; i < MARQW; i++)
        out[i] = mlen ? marquee[(moff + i) % mlen] : ' ';
    out[MARQW] = '\0';

    (void) xy;
    apj_fill(vh, wx, wy, ww, ch, apj_pen(APJ_R_PANEL));
    apj_text(vh, wx + 2, wy, apj_pen(APJ_R_TEXT), out);
}

static void draw_time(void)
{
    char line[100];
    short xy[4];
    const char *st = !playing ? "stop" : (paused ? "paus" : "play");

    if (playing && vid_w > 0)
        snprintf(line, sizeof(line),
                 "%ld:%02ld/%ld:%02ld %s %ldx%ld %ld/%ld.%01ld %s v%d %d/%d   ",
                 pos_s / 60, pos_s % 60, len_s / 60, len_s % 60, st,
                 vid_w, vid_h,
                 vid_fps / 100,                    /* what the file claims  */
                 act_fps / 100, (act_fps % 100) / 10,  /* what you get      */
                 nf_call(vidid | NF_VID_INFO, 4L) ? "hw" : "sw",
                 volume, ntracks ? sel + 1 : 0, ntracks);
    else
        snprintf(line, sizeof(line), "%ld:%02ld/%ld:%02ld %s %d/%d          ",
                 pos_s / 60, pos_s % 60, len_s / 60, len_s % 60, st,
                 ntracks ? sel + 1 : 0, ntracks);

    (void) xy;
    apj_fill(vh, wx, wy + ch, ww, ch, apj_pen(APJ_R_PANEL));
    apj_text(vh, wx + 2, wy + ch, apj_pen(APJ_R_TEXT), line);
}

static void draw_buttons(void)
{
    short x, y, w, h, xy[10];
    int i;
    for (i = 0; i < NBUTTONS; i++) {
        const char *t = btxt[i];
        btn_rect(i, &x, &y, &w, &h);
        if (x + w > wx + ww)
            break;                         /* window too narrow - clip the row */
        (void) xy;
        /* sticky toggles show as pressed; play is the default action */
        apj_button(vh, x, y, w, h, t,
                   (i == B_LIST && listmode) || (i == B_FULL && fullscreen) ||
                   (i == B_LOOP && loopmode),
                   i == B_PLAY);
    }
}

static void draw_list(void)
{
    short xy[4];
    char line[80];
    short ax, ay, aw, ah;
    int rows, r;

    video_area(&ax, &ay, &aw, &ah);
    rows = ah / ch;
    if (rows > VISROWS) rows = VISROWS;

    (void) xy;
    apj_fill(vh, ax, ay, aw, ah, apj_pen(APJ_R_PAPER));

    if (!ntracks) {
        apj_text(vh, ax + 2, ay, apj_pen(APJ_R_DISABLED), "No files - press Open");
        return;
    }
    for (r = 0; r < rows; r++) {
        int i = top + r;
        if (i >= ntracks)
            break;
        snprintf(line, sizeof(line), "%c %-.60s",
                 (i == sel && playing) ? '>' : ' ', list[i]);
        if (i == sel) {
            apj_select(vh, ax, ay + r * ch, aw, ch);
            apj_text(vh, ax + 2, ay + r * ch, apj_pen(APJ_R_SELFG), line);
        } else
            apj_text(vh, ax + 2, ay + r * ch, apj_pen(APJ_R_TEXT), line);
    }
}

/* The video area itself: the overlay covers it while a film is on screen, so
 * all we ever draw here is the backdrop for the moments when it does not. */
static void draw_video_area(void)
{
    short ax, ay, aw, ah, xy[4];
    video_area(&ax, &ay, &aw, &ah);
    if (aw <= 0 || ah <= 0)
        return;

    vswr_mode(vh, 1);
    vsf_interior(vh, 1);
    vsf_color(vh, 1);                        /* solid: a black picture box */
    xy[0] = ax; xy[1] = ay; xy[2] = ax + aw - 1; xy[3] = ay + ah - 1;
    v_bar(vh, xy);

    if (!playing) {
        apj_text(vh, ax + 4, ay + 2, G_WHITE, "no video");
    } else if (too_big && !fullscreen) {
        char msg[80];
        if (min_dw > 0) {
            sprintf(msg, "%ldx%ld cannot be drawn below %ldx%ld",
                    vid_w, vid_h, min_dw, min_dh);
            apj_text(vh, ax + 4, ay + 2, G_WHITE, msg);
            apj_text(vh, ax + 4, ay + 2 + ch, G_WHITE,
                     "window too small - press F for fullscreen");
        } else {
            apj_text(vh, ax + 4, ay + 2, G_WHITE, "waiting for the first frame...");
        }
        apj_text(vh, ax + 4, ay + 2 + 2 * ch, G_WHITE, "sound is playing");
    }
}

static void draw_pane(void)
{
    if (listmode)
        draw_list();
    else
        draw_video_area();
}

static void draw_all(void)
{
    apj_fill(vh, wx, wy, ww, wh, apj_pen(APJ_R_PANEL));
    draw_marquee();
    draw_time();
    draw_buttons();
    draw_pane();
}

static void draw_band(void)   { draw_marquee(); draw_time(); }

/* Walk the AES rectangle list, clip, and call fn for each visible part. */
static void redraw(void (*fn)(void), short rx, short ry, short rw, short rh)
{
    short cl[4];
    GRECT r, d;

    d.g_x = rx; d.g_y = ry; d.g_w = rw; d.g_h = rh;

    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, NULL);
    wind_get(win, WF_FIRSTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
    while (r.g_w && r.g_h) {
        GRECT i = r;
        if (rc_intersect(&d, &i)) {
            (void) cl;
            apj_clip(vh, i.g_x, i.g_y, i.g_w, i.g_h);
            fn();
        }
        wind_get(win, WF_NEXTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
    }
    apj_clip_off(vh);
    graf_mouse(M_ON, NULL);
    wind_update(END_UPDATE);
}

static void update_work(void)
{
    wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
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
    top = 0;
    sel = ntracks ? 0 : -1;
    return ntracks;
}

static void do_open(void)
{
    /* BOTH of these are written by the AES, not by us, and the AES decides how
     * much to write - there is no length argument in fsel_exinput(). Under
     * MiNT a long filename off a HOSTFS share will happily run past 64 bytes.
     * These used to be 256 and 64 respectively, and fname was on the STACK,
     * so selecting a long name overwrote the return address and the emulator
     * died with a wild guest pointer whose value was the tail of the filename.
     * Both are now static and generously sized. */
    static char fpath[PATHLEN] = "";
    static char fname[NAMELEN] = "";
    short btn = 0;
    int i;

    fname[0] = '\0';
    if (!fpath[0]) {
        fpath[0] = (char)('A' + Dgetdrv());
        strcpy(fpath + 1, ":\\*.*");
    }

    /* The overlay is a HARDWARE plane: it composites above the entire Atari
     * screen, so anything GEM draws underneath it is invisible - including the
     * file selector, which would otherwise open behind a paused picture with
     * no way to see what you are clicking. Put the picture away for the
     * duration of the dialog; playback (and the sound) carry on. */
    nf_call(vidid | NF_VID_RECT, 0L, 0L, -1L, -1L);

    fsel_exinput(fpath, fname, &btn, "Select a video (HOSTFS drive)");

    if (btn != 1 || !fname[0]) {
        update_rect();            /* cancelled - put the picture back */
        redraw(draw_all, wx, wy, ww, wh);
        return;
    }

    strncpy(dir, fpath, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    { char *bs = strrchr(dir, '\\'); if (bs) *bs = '\0'; }

    if (load_dir(dir) > 0) {
        int found = -1;
        for (i = 0; i < ntracks; i++)
            if (strcasecmp(list[i], fname) == 0) { found = i; break; }
        /* If the name the user picked is not in the scan, playing sel (0)
         * anyway starts SOME OTHER FILE, which looks like the player ignoring
         * the choice. Say so instead - the usual cause is an extension that is
         * not in vexts[]. */
        if (found < 0) {
            snprintf(marquee, sizeof(marquee),
                     "%s is not a video file this player recognises   ", fname);
            moff = 0;
            redraw(draw_all, wx, wy, ww, wh);
            update_rect();
            return;
        }
        sel = found;
        if (sel < top)             top = sel;
        if (sel >= top + VISROWS)  top = sel - VISROWS + 1;
        listmode = 0;
        start_track(sel);
    } else {
        /* A deep HOSTFS path can now be longer than the marquee itself, and
         * the informative end of a path is the tail, not the drive letter -
         * so trim from the front and bound it explicitly rather than leaving
         * snprintf to truncate the interesting part off the end. */
        size_t dl = strlen(dir);
        const char *shown = dl > 120 ? dir + dl - 120 : dir;
        snprintf(marquee, sizeof(marquee), "No video files in %s%.120s   ",
                 shown == dir ? "" : "...", shown);
        moff = 0;
    }
    redraw(draw_all, wx, wy, ww, wh);
}

/* ------------------------------------------------------------- actions --- */

static void next_track(int step)
{
    int i;
    if (!ntracks)
        return;
    i = sel + step;
    if (i < 0) i = 0;
    if (i >= ntracks) { stop_track(); redraw(draw_all, wx, wy, ww, wh); return; }
    if (i < top) top = i;
    if (i >= top + VISROWS) top = i - VISROWS + 1;
    start_track(i);
    redraw(draw_all, wx, wy, ww, wh);
}

static void set_volume(int v)
{
    if (v < 0)   v = 0;
    if (v > 200) v = 200;
    volume = v;
    nf_call(vidid | NF_VID_VOLUME, (long)volume);
    redraw(draw_band, wx, wy, ww, 2 * ch);
}

static void do_button(int b)
{
    switch (b) {
        case B_PREV:  next_track(-1); break;
        case B_NEXT:  next_track(+1); break;
        case B_RW:    nf_call(vidid | NF_VID_SEEK, (long)-10); break;
        case B_FF:    nf_call(vidid | NF_VID_SEEK, (long)+10); break;
        case B_PLAY:
            if (playing && paused) {
                nf_call(vidid | NF_VID_PAUSE, 0L);
                paused = 0;
            } else if (!playing) {
                start_track(sel);
            }
            redraw(draw_all, wx, wy, ww, wh);
            break;
        case B_PAUSE:
            if (playing) {
                paused = !paused;
                nf_call(vidid | NF_VID_PAUSE, (long)paused);
                redraw(draw_band, wx, wy, ww, 2 * ch);
            }
            break;
        case B_STOP:
            stop_track();
            update_rect();
            redraw(draw_all, wx, wy, ww, wh);
            break;
        case B_OPEN:  do_open(); break;
        case B_LIST:
            listmode = !listmode;
            update_rect();               /* hides / restores the overlay */
            redraw(draw_all, wx, wy, ww, wh);
            break;
        case B_LOOP:
            loopmode = !loopmode;
            redraw(draw_all, wx, wy, ww, wh);
            break;
        case B_FULL:
            fullscreen = !fullscreen;
            if (fullscreen)
                listmode = 0;            /* you cannot see a list under a film */
            update_rect();
            redraw(draw_all, wx, wy, ww, wh);
            break;
    }
}

static void click(short mx, short my)
{
    short x, y, w, h, ax, ay, aw, ah;
    int i;

    for (i = 0; i < NBUTTONS; i++) {
        btn_rect(i, &x, &y, &w, &h);
        if (mx >= x && mx < x + w && my >= y && my < y + h) {
            do_button(i);
            return;
        }
    }
    if (listmode) {
        video_area(&ax, &ay, &aw, &ah);
        if (my >= ay && my < ay + ah && mx >= ax && mx < ax + aw) {
            int r = (my - ay) / ch;
            int t = top + r;
            if (t < ntracks) {
                listmode = 0;
                start_track(t);
                redraw(draw_all, wx, wy, ww, wh);
            }
        }
    }
}

/* ---------------------------------------------------------------- main --- */

int main(void)
{
    short work_in[11], work_out[57];
    short d, msg[8];
    short mx, my, mb, ks, kr, brk;
    short ev;
    int   i;

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
    apj_init(vh);                                 /* APJ-OS: theme + renderer, before any window */

    scr_w = work_out[0] + 1;
    scr_h = work_out[1] + 1;

    /* WHAT THIS ACTUALLY NEEDS - and it is not fVDI.
     *
     * The picture is a hardware overlay plane on the Pi, composited above
     * whatever the emulator is scanning out. It does not read, write or care
     * about the Atari framebuffer, so the screen driver is irrelevant: ST low/
     * medium/high, an ET4000 mode, fVDI, NOVA - all the same to it.
     *
     * The three real requirements:
     *   1. the VIDPLAY NatFeat            (checked above)
     *   2. the emulator's DRM display path, because the overlay shares its
     *      CRTC - with the SDL or fbdev path there is no plane to use
     *   3. an AES, for the window and the visible-region list
     *
     * The one thing that IS driver-shaped: mapping this window's coordinates
     * to display pixels assumes the whole Atari screen fills the whole display,
     * which is exactly what the DRM presenter does for every mode. If a future
     * path letterboxes or centres instead, this arithmetic needs revisiting. */
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
                      "and needs a spare overlay plane.|"
                      "The screen driver does not matter.][ OK ]");
        v_clsvwk(vh);
        appl_exit();
        return 1;
    }

    {
        short dx, dy, dw, dh, cx, cy, cwid, chgt;
        short want_w = (MARQW + 2) * cw;
        short want_h = 3 * ch + 12 + 9 * ch;      /* bands + a picture box */
        wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
        if (want_w > dw) want_w = dw;
        if (want_h > dh) want_h = dh;
        wind_calc(WC_BORDER, WIN_KIND,
                  dx + 8, dy + 8, want_w, want_h, &cx, &cy, &cwid, &chgt);
        win = wind_create(WIN_KIND, dx, dy, dw, dh);
        wind_set_str(win, WF_NAME, "PiSTorm Video");
        wind_open(win, cx, cy, cwid, chgt);
        update_work();
    }
    redraw(draw_all, wx, wy, ww, wh);

    for (;;) {
        ev = evnt_multi(MU_MESAG | MU_BUTTON | MU_KEYBD | MU_TIMER,
                        1, 1, 1,
                        0, 0, 0, 0, 0,
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
                    redraw(draw_all, wx, wy, ww, wh);
                    break;
                case WM_MOVED:
                case WM_SIZED:
                    wind_set(win, WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
                    floor_unfit = apply_min_size();   /* never shrink past it */
                    update_work();
                    redraw(draw_all, wx, wy, ww, wh);
                    break;
                case WM_FULLED: {
                    short cx, cy, cwid, chgt;
                    wind_get(win, WF_FULLXYWH, &cx, &cy, &cwid, &chgt);
                    wind_set(win, WF_CURRXYWH, cx, cy, cwid, chgt);
                    update_work();
                    redraw(draw_all, wx, wy, ww, wh);
                    break;
                }
                case WM_CLOSED:
                    goto out;
            }
        }
        if (ev & MU_BUTTON)
            click(mx, my);
        if (ev & MU_KEYBD) {
            char c = (char)(kr & 0xff);
            short sc = (short)((kr >> 8) & 0xff);
            if (c == ' ')                          do_button(B_PAUSE);
            else if (c == 'n' || c == 'N')         do_button(B_NEXT);
            else if (c == 'p' || c == 'P')         do_button(B_PREV);
            else if (c == 'l' || c == 'L')         do_button(B_LIST);
            else if (c == 'r' || c == 'R')         do_button(B_LOOP);
            else if (c == 'f' || c == 'F') {       /* F = fullscreen ON  */
                if (!fullscreen) do_button(B_FULL);
            } else if (c == 'w' || c == 'W') {     /* W = back to window */
                if (fullscreen) do_button(B_FULL);
            }
            else if (c == 'o' || c == 'O')         do_button(B_OPEN);
            else if (c == '+' || c == '=')         set_volume(volume + 10);
            else if (c == '-')                     set_volume(volume - 10);
            else if (sc == 0x4B)                   do_button(B_RW);
            else if (sc == 0x4D)                   do_button(B_FF);
            else if (c == 'q' || c == 'Q' || c == 0x1b) goto out;
        }
        if (ev & MU_TIMER) {
            /* The overlay's minimum size only becomes known once the host has
             * decoded a frame, so keep asking until it answers, then lay the
             * window out for real. */
            if (playing && min_dw < 0) {
                long w = nf_call(vidid | NF_VID_INFO, (long)VI_MIN_W);
                long h = nf_call(vidid | NF_VID_INFO, (long)VI_MIN_H);
                if (w >= 0 && h >= 0) {
                    min_dw = w;
                    min_dh = h;
                    if (vid_w <= 0) {
                        vid_w = nf_call(vidid | NF_VID_INFO, 0L);
                        vid_h = nf_call(vidid | NF_VID_INFO, 1L);
                    }
                    /* Size the window to what the hardware can actually
                     * draw, instead of hiding the picture from the user. */
                    compute_min_window();
                    floor_unfit = apply_min_size();
                    update_rect();
                    redraw(draw_all, wx, wy, ww, wh);
                }
            }
            if (playing && !paused) {
                long p = nf_call(vidid | NF_VID_POS);
                if (p >= 0) pos_s = p;
                act_fps = nf_call(vidid | NF_VID_INFO, (long)VI_FPS_NOW);
                if (act_fps < 0) act_fps = 0;
                if (nf_call(vidid | NF_VID_STATUS) == 0) {
                    /* Loop this file, or simply stop. Wandering into the next
                     * file by itself is rarely what you want from a video
                     * player; |< and >| are there for that. */
                    if (loopmode)
                        start_track(sel);
                    else
                        stop_track();
                    redraw(draw_all, wx, wy, ww, wh);
                    continue;
                }
            }
            moff++;                       /* scroll marquee 4 chars/s */
            redraw(draw_band, wx, wy, ww, 2 * ch);
        }
    }

out:
    /* Unlike MP3GEM, we stop on exit: an overlay left on screen with nothing
     * left to control it would sit on top of the desktop. */
    stop_track();
    nf_call(vidid | NF_VID_RECT, 0L, 0L, -1L, -1L);
    wind_close(win);
    wind_delete(win);
    v_clsvwk(vh);
    appl_exit();
    return 0;
}
