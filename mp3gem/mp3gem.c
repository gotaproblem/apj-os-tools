/*
 * mp3gem.c - PiSTorm host MP3 player, GEM front-end (MP3GEM.PRG)
 *
 * Controls the "MP3PLAY" NatFeat: the Pi decodes MP3s (libmpg123) and mixes
 * them into HDMI audio alongside ST/STE sound. This app is just the remote
 * control: playlist, play/pause/stop, prev/next, rewind/ff, scrolling ID3
 * metadata and a position readout.
 *
 * Files must live on a HOSTFS drive (the host opens them by path).
 *
 * Build:  make            (m68k-atari-mint-gcc, links -lgem)
 * Run:    MP3GEM.PRG from the desktop (GEM app, needs AES; FreeMiNT+XaAES ok)
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

#define NF_MP3_PLAY   0
#define NF_MP3_STOP   1
#define NF_MP3_STATUS 2
#define NF_MP3_PAUSE  3
#define NF_MP3_SEEK   4
#define NF_MP3_POS    5
#define NF_MP3_LEN    6
#define NF_MP3_META   7

/* ---------------------------------------------------------------- state -- */

#define MAXTRACKS 200
#define NAMELEN   64
#define VISROWS   10          /* playlist rows shown */
#define MARQW     46          /* marquee width in characters */

static long  mp3id;
static short vh;                          /* VDI handle */
static short win = -1;
static short cw, ch;                      /* char cell size */
static short wx, wy, ww, wh;              /* window work area */
static int   iconified = 0;               /* minimised: nothing of ours to draw */

static char  dir[256]  = "";              /* playlist directory (GEMDOS path) */
static char  list[MAXTRACKS][NAMELEN];
static int   ntracks = 0;
static int   sel = -1;                    /* selected / playing index */
static int   top = 0;                     /* first visible playlist row */
static int   playing = 0, paused = 0;
static long  pos_s = 0, len_s = 0;

static char  marquee[400] = "PiSTorm MP3 - open a file...   ";
static int   moff = 0;

/* button row */
static const char *btxt[8] = { "|<", "<<", " > ", "||", "[]", ">>", ">|", "Open" };
enum { B_PREV, B_RW, B_PLAY, B_PAUSE, B_STOP, B_FF, B_NEXT, B_OPEN };

/* ------------------------------------------------------------- layout ---- */
/* rows (in char cells from top of work area):
 * 0: marquee   1: time/status   2: buttons   3: separator   4..: playlist  */

static void btn_rect(int i, short *x, short *y, short *w, short *h)
{
    *w = 5 * cw;
    *h = ch + 4;
    *x = wx + 2 + i * (*w + cw / 2);
    *y = wy + 2 * ch + 6;
    (void)i;
}

static short list_y0(void) { return wy + 4 * ch; }

/* ------------------------------------------------------------- NF glue --- */

static void nf_meta(int which, char *buf, int len)
{
    buf[0] = '\0';
    nf_call(mp3id | NF_MP3_META, (long)which, buf, (long)len);
    buf[len - 1] = '\0';
}

static void build_marquee(void)
{
    char t[128], a[128], al[128];
    nf_meta(0, t, sizeof(t));
    nf_meta(1, a, sizeof(a));
    nf_meta(2, al, sizeof(al));
    snprintf(marquee, sizeof(marquee), "%s%s%s%s%s    ",
             t[0] ? t : "(no title)",
             a[0] ? " - " : "", a,
             al[0] ? " - " : "", al);
    moff = 0;
}

static void start_track(int i)
{
    char path[400];
    if (i < 0 || i >= ntracks)
        return;
    snprintf(path, sizeof(path), "%s\\%s", dir, list[i]);
    if (nf_call(mp3id | NF_MP3_PLAY, path) == 0) {
        sel = i;
        playing = 1;
        paused = 0;
        len_s = nf_call(mp3id | NF_MP3_LEN);
        pos_s = 0;
        build_marquee();
    } else {
        playing = 0;
        snprintf(marquee, sizeof(marquee), "Cannot play %s   ", list[i]);
        moff = 0;
    }
}

static void stop_track(void)
{
    nf_call(mp3id | NF_MP3_STOP);
    playing = 0;
    paused = 0;
    pos_s = 0;
}

/* ------------------------------------------------------------- drawing --- */

static void draw_marquee(void)
{
    char out[MARQW + 1];
    int mlen = (int)strlen(marquee);
    short xy[4];

    for (int i = 0; i < MARQW; i++)
        out[i] = mlen ? marquee[(moff + i) % mlen] : ' ';
    out[MARQW] = '\0';

    (void) xy;
    apj_fill(vh, wx, wy, ww, ch, apj_pen(APJ_R_PANEL));
    apj_text(vh, wx + 2, wy, apj_pen(APJ_R_TEXT), out);
}

static void draw_time(void)
{
    char line[80];
    short xy[4];
    const char *st = !playing ? "stopped" : (paused ? "paused " : "playing");

    snprintf(line, sizeof(line), "%02ld:%02ld / %02ld:%02ld  %s  %d/%d      ",
             pos_s / 60, pos_s % 60, len_s / 60, len_s % 60,
             st, ntracks ? sel + 1 : 0, ntracks);

    (void) xy;
    apj_fill(vh, wx, wy + ch, ww, ch, apj_pen(APJ_R_PANEL));
    apj_text(vh, wx + 2, wy + ch, apj_pen(APJ_R_TEXT), line);
}

static void draw_buttons(void)
{
    short x, y, w, h, xy[10];
    for (int i = 0; i < 8; i++) {
        btn_rect(i, &x, &y, &w, &h);
        (void) xy;
        /* the play button is the default action; it shows pressed while
         * playing, pause while paused */
        apj_button(vh, x, y, w, h, btxt[i],
                   (i == 2 && playing && !paused) || (i == 3 && paused),
                   i == 2);
    }
}

static void draw_list(void)
{
    short xy[4];
    char line[80];
    short y0 = list_y0();

    (void) xy;
    apj_fill(vh, wx, y0, ww, VISROWS * ch, apj_pen(APJ_R_PAPER));

    for (int r = 0; r < VISROWS; r++) {
        int i = top + r;
        if (i >= ntracks)
            break;
        snprintf(line, sizeof(line), "%c %-.60s",
                 (i == sel && playing) ? '>' : ' ', list[i]);
        if (i == sel) {                      /* selected row: fill, then light text */
            apj_select(vh, wx, y0 + r * ch, ww, ch);
            apj_text(vh, wx + 2, y0 + r * ch, apj_pen(APJ_R_SELFG), line);
        } else
            apj_text(vh, wx + 2, y0 + r * ch, apj_pen(APJ_R_TEXT), line);
    }
}

static void draw_all(void)
{
    apj_fill(vh, wx, wy, ww, wh, apj_pen(APJ_R_PANEL));
    draw_marquee();
    draw_time();
    draw_buttons();
    draw_list();
}

/* Walk the AES rectangle list, clip, and call fn for each visible part. */
static void draw_iconic(void)
{
    apj_fill(vh, wx, wy, ww, wh, apj_pen(APJ_R_PANEL));
}

static void redraw(void (*fn)(void), short rx, short ry, short rw, short rh)
{
    short cl[4];
    GRECT r, d = { rx, ry, rw, rh };

    if (iconified)                        /* only an icon box, if anything */
        fn = draw_iconic;

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
}

static void draw_band(void)   { draw_marquee(); draw_time(); }

/* ------------------------------------------------------------- playlist -- */

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
        size_t n = strlen(e->d_name);
        if (n > 4 && strcasecmp(e->d_name + n - 4, ".mp3") == 0 &&
            n < NAMELEN) {
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

/* Load the folder of path (a GEMDOS path whose last component is a file or
 * mask) as the playlist and start fname in it */
static void open_in_dir(const char *path, const char *fname)
{
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    { char *bs = strrchr(dir, '\\'); if (bs) *bs = '\0'; }

    if (load_dir(dir) > 0) {
        for (int i = 0; i < ntracks; i++)
            if (strcasecmp(list[i], fname) == 0) { sel = i; break; }
        if (sel < top) top = sel;
        if (sel >= top + VISROWS) top = sel - VISROWS + 1;
        start_track(sel);
    }
    redraw(draw_all, wx, wy, ww, wh);
}

static void play_path(const char *arg);

static void do_open(void)
{
    static char fpath[256] = "";
    char fname[64] = "";
    short btn = 0;

    if (!fpath[0]) {
        fpath[0] = (char)('A' + Dgetdrv());
        strcpy(fpath + 1, ":\\*.MP3");
    }
    fsel_exinput(fpath, fname, &btn, "Select an MP3 (HOSTFS drive)");
    if (btn != 1 || !fname[0])
        return;

    /* dir = path up to the last backslash */
    open_in_dir(fpath, fname);
}

/* A file handed to us - on the command line (double-clicked in the desktop,
 * which runs MP3GEM for *.MP3) or in a VA_START while running: "S:\MUSIC\A.MP3",
 * possibly quoted. Its folder becomes the playlist and it starts playing. */
static void play_path(const char *arg)
{
    char full[256], *bs;
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

/* ------------------------------------------------------------- actions --- */

static void next_track(int step)
{
    if (!ntracks)
        return;
    int i = sel + step;
    if (i < 0) i = 0;
    if (i >= ntracks) { stop_track(); redraw(draw_all, wx, wy, ww, wh); return; }
    if (i < top) top = i;
    if (i >= top + VISROWS) top = i - VISROWS + 1;
    start_track(i);
    redraw(draw_all, wx, wy, ww, wh);
}

static void do_button(int b)
{
    switch (b) {
        case B_PREV:  next_track(-1); break;
        case B_NEXT:  next_track(+1); break;
        case B_RW:    nf_call(mp3id | NF_MP3_SEEK, (long)-10); break;
        case B_FF:    nf_call(mp3id | NF_MP3_SEEK, (long)+10); break;
        case B_PLAY:
            if (playing && paused) { nf_call(mp3id | NF_MP3_PAUSE, 0L); paused = 0; }
            else if (!playing)     start_track(sel);
            redraw(draw_band, wx, wy, ww, 2 * ch);
            break;
        case B_PAUSE:
            if (playing) {
                paused = !paused;
                nf_call(mp3id | NF_MP3_PAUSE, (long)paused);
                redraw(draw_band, wx, wy, ww, 2 * ch);
            }
            break;
        case B_STOP:
            stop_track();
            redraw(draw_all, wx, wy, ww, wh);
            break;
        case B_OPEN:  do_open(); break;
    }
}

static void click(short mx, short my)
{
    short x, y, w, h;

    for (int i = 0; i < 8; i++) {
        btn_rect(i, &x, &y, &w, &h);
        if (mx >= x && mx < x + w && my >= y && my < y + h) {
            do_button(i);
            return;
        }
    }
    if (my >= list_y0() && my < list_y0() + VISROWS * ch) {
        int r = (my - list_y0()) / ch;
        int i = top + r;
        if (i < ntracks) {
            start_track(i);
            redraw(draw_all, wx, wy, ww, wh);
        }
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
    short ev;
    int   tick = 0;

    if (appl_init() < 0)
        return 1;
    mp3id = nf_id("MP3PLAY");
    if (!mp3id) {
        form_alert(1, "[3][MP3PLAY NatFeat not found.|"
                      "Run under PiSTorm emulator.][ OK ]");
        appl_exit();
        return 1;
    }

    vh = graf_handle(&cw, &ch, &d, &d);
    for (int i = 0; i < 10; i++) work_in[i] = 1;
    work_in[10] = 2;
    v_opnvwk(work_in, &vh, work_out);
    vst_alignment(vh, 0, 5, &d, &d);              /* left / top text origin */
    apj_init(vh);                                 /* APJ-OS: theme + renderer, before any window */

    {
        short dx, dy, dw, dh, cx, cy, cwid, chgt;
        short want_w = (MARQW + 2) * cw, want_h = (4 + VISROWS) * ch + 8;
        wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
        wind_calc(WC_BORDER, NAME | CLOSER | MOVER | SMALLER,
                  dx + 16, dy + 16, want_w, want_h, &cx, &cy, &cwid, &chgt);
        win = wind_create(NAME | CLOSER | MOVER | SMALLER, cx, cy, cwid, chgt);
        wind_set_str(win, WF_NAME, "PiSTorm MP3");
        wind_open(win, cx, cy, cwid, chgt);
        update_work();
    }
    redraw(draw_all, wx, wy, ww, wh);

    if (argc > 1)                         /* a file double-clicked in the desktop */
        play_path(argv[1]);

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
                    break;
                case WM_TOPPED:
                    wind_set(win, WF_TOP, 0, 0, 0, 0);
                    break;
                case WM_MOVED:
                    wind_set(win, WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
                    update_work();
                    break;
                case WM_CLOSED:
                    goto out;
                case WM_ICONIFY:
                case WM_ALLICONIFY:       /* minimise (to the taskbar under APJ-OS) */
                    wind_set(win, WF_ICONIFY, msg[4], msg[5], msg[6], msg[7]);
                    iconified = 1;
                    update_work();
                    break;
                case WM_UNICONIFY:
                    wind_set(win, WF_UNICONIFY, msg[4], msg[5], msg[6], msg[7]);
                    iconified = 0;
                    update_work();
                    redraw(draw_all, wx, wy, ww, wh);
                    break;
                case VA_START: {          /* opened again while running */
                    short reply[8];
                    char *cmd = (char *)(((long)msg[3] << 16) | (unsigned short)msg[4]);
                    if (iconified) {      /* bring the window back first */
                        wind_set(win, WF_UNICONIFY, -1, -1, -1, -1);
                        iconified = 0;
                        update_work();
                        redraw(draw_all, wx, wy, ww, wh);
                    }
                    if (cmd)
                        play_path(cmd);
                    wind_set(win, WF_TOP, 0, 0, 0, 0);
                    reply[0] = AV_STARTED; reply[1] = gl_apid; reply[2] = 0;
                    reply[3] = msg[3]; reply[4] = msg[4];
                    reply[5] = reply[6] = reply[7] = 0;
                    appl_write(msg[1], 16, reply);
                    break;
                }
            }
        }
        if ((ev & MU_BUTTON) && !iconified)
            click(mx, my);
        if (ev & MU_KEYBD) {
            char c = (char)(kr & 0xff);
            if (c == ' ')                 do_button(B_PAUSE);
            else if (c == 'n' || c == 'N') do_button(B_NEXT);
            else if (c == 'p' || c == 'P') do_button(B_PREV);
            else if (c == 'q' || c == 'Q' || c == 0x1b) goto out;
        }
        if (ev & MU_TIMER) {
            tick++;
            if (playing && !paused) {
                long p = nf_call(mp3id | NF_MP3_POS);
                if (p >= 0) pos_s = p;
                if (nf_call(mp3id | NF_MP3_STATUS) == 0) {
                    next_track(+1);       /* track ended -> advance */
                    continue;
                }
            }
            moff++;                       /* scroll marquee 4 chars/s */
            if (!iconified)
                redraw(draw_band, wx, wy, ww, 2 * ch);
            (void)tick;
        }
    }

out:
    /* leave the music playing on exit; use [] or MP3PLAY STOP to silence */
    wind_close(win);
    wind_delete(win);
    v_clsvwk(vh);
    appl_exit();
    return 0;
}
