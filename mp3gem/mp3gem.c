/*
 * mp3gem.c - PiSTorm host MP3 player, GEM front-end (MP3GEM.PRG)
 *
 * Controls the "MP3PLAY" NatFeat: the Pi decodes MP3s (libmpg123) and mixes
 * them into HDMI audio alongside ST/STE sound. This app is the remote
 * control - playlist, transport, metadata - and nothing else; the window
 * itself is drawn by mp3ui.c out of an APJSKIN sheet.
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
#include "../apjgui/apjskin.h"
#include "mp3ui.h"

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
#define NF_MP3_INFO   8      /* 0 bitrate kbps, 1 channels, 2 layer, 3 VBR   */
#define NF_MP3_ART    9      /* ptr, size, edge -> bytes written, 0 = no art */
#define NF_MP3_VOLUME 10     /* -1 query, 0..100 set                          */
#define NF_MP3_FILELEN 11    /* p0 path -> seconds; -2 not yet (ask again), -1 no */

/* ---------------------------------------------------------------- state -- */

#define MAXTRACKS 200
#define NAMELEN   64

static long  mp3id;
static short vh;                          /* VDI handle */
static short win = -1;
static short cw, ch;                      /* char cell size */
static short wx, wy, ww, wh;              /* window work area */
static int   iconified = 0;

static char  dir[256]  = "";              /* playlist directory (GEMDOS path) */
static char  list[MAXTRACKS][NAMELEN];
static long  tlen[MAXTRACKS];             /* seconds; -1 = not asked/answered yet, 0 = unknown */
static int   scanned = 0;                 /* how many of those are filled in */
static int   scan_done = 0;               /* nothing left to ask */
static int   ntracks = 0;
static int   have_filelen = 1;            /* cleared if the host has no sub-op 11 */

static char  ui_title[128] = "PiSTorm MP3";
static char  ui_sub[192]   = "Open a file...";
static char  ui_codec[32]  = "";

static MP3UI ui;
static short vol_before_mute = 100;
static void *artbuf = NULL;         /* cover tile, TT-RAM, device format */
static long  artcap = 0;
static GRECT m1r;
static short m1flag = MO_ENTER;

/* ------------------------------------------------------------- NF glue --- */

static void nf_meta(int which, char *buf, int len)
{
    buf[0] = '\0';
    nf_call(mp3id | NF_MP3_META, (long)which, buf, (long)len);
    buf[len - 1] = '\0';
}

static const char *name_of(void *c, short i)
{
    (void)c;
    return (i >= 0 && i < ntracks) ? list[i] : "";
}

static long len_of(void *c, short i)
{
    (void)c;
    return (i >= 0 && i < ntracks && tlen[i] > 0) ? tlen[i] : 0L;
}

/*
 * Track durations for the playlist. MP3PLAY only knows the length of the
 * track it has open, so the host scans each file once (sub-op 11) and we
 * cache the answer. A CBR file is a header read; a VBR one costs a scan,
 * which is why this happens on load behind a busy bee rather than during
 * a redraw.
 */
static void scan_reset(void)
{
    int i;

    for (i = 0; i < MAXTRACKS; i++)
        tlen[i] = -1;
    scanned = 0;
    scan_done = 0;
}

/*
 * The host is not allowed to make us wait for a length. A NatFeat runs
 * inline on the 68k, and a handler that took a second to mpg123_scan() a
 * file stalled the whole guest for that second - long enough to wedge the
 * timer interrupts, which is what stopped the clock. Sub-op 11 answers -2
 * while a host thread does the reading; we ask again next tick.
 */
static int scan_shown = 1;          /* status line reflects ui.ntimed */
static int scan_wait = 0;           /* ticks since the scan last got an answer */

/*
 * One tick of the length scan. Every file still without a length is asked
 * for, not just the next one: the host answers -2 and queues a file it has
 * not read yet, so asking for all of them lets its worker run through the
 * folder back to back instead of one file per two ticks (queue on one,
 * collect on the next). The calls themselves are cheap - the host never
 * blocks in FILELEN. A folder that stops answering is polled more slowly
 * after a minute, never abandoned: results that were thrown away once
 * turned out to be merely late.
 */
static int scan_step(void)
{
    char path[400];
    int i, calls = 0, pending = 0, progress = 0, relist = 0;

    if (!have_filelen || scan_done)
        return 0;
    /* a folder that has gone quiet - no answer for a minute - is still
     * asked, but every four seconds instead of four times a second: the
     * host may simply be slow, and an answer that comes late is still
     * an answer. Nothing is ever given up. */
    if (scan_wait >= 240 && (scan_wait++ & 15) != 0)
        return 0;
    for (i = 0; i < ntracks; i++) {
        long v;

        if (tlen[i] >= 0)
            continue;
        if (calls >= 32) {            /* enough traps for one tick */
            pending++;
            continue;
        }
        snprintf(path, sizeof(path), "%.255s\\%.63s", dir, list[i]);
        v = nf_call(mp3id | NF_MP3_FILELEN, path);
        calls++;
        if (v == -2) {
            pending++;
            continue;
        }
        if (v < 0) {
            /* -1: the host could not read THIS file (or is too old to
             * have sub-op 11 at all, in which case every file says so).
             * It used to abort the whole scan, and with every file asked
             * at once one bad answer blanked the entire list. Count it
             * and carry on. */
            v = 0;
            ui.nbad++;
        }
        tlen[i] = v;
        scanned++;
        progress++;
        if (i >= ui.top && i < ui.top + ui.visrows)
            relist = 1;
    }
    if (progress) {
        scan_wait = 0;
        ui.ntimed = (short)scanned;
        scan_shown = 0;               /* the status line says how far we are */
    } else if (pending && scan_wait < 240) {
        scan_wait++;
    }
    if (!pending)
        scan_done = 1;
    return relist;
}

/*
 * The cover. MP3PLAY sub-op 9 decodes the ID3 APIC frame on the Pi - stb on
 * a 1.5 GHz ARM rather than a JPEG decoder on the 68k - and writes the tile
 * straight into our buffer in the screen's pixel format. The buffer is
 * Mxalloc'd from alternate RAM because a blit source below 4 MB goes through
 * the JIT's self-modifying-code check.
 */
static void want_art(void)
{
    short e = mp3ui_artedge();
    short ext[57];
    long  need;

    ui.hasart = 0;
    ui.artbuf = NULL;
    ui.artedge = e;
    if (e <= 0 || (e & 15))                 /* the blit needs a x16 width */
        return;
    vq_extnd(vh, 1, ext);
    need = (long)e * e * (ext[4] == 16 ? 2L : 4L);
    if (need > artcap) {
        if (artbuf)
            Mfree(artbuf);
        artbuf = (void *)Mxalloc(need, 3);
        artcap = artbuf ? need : 0;
    }
    if (!artbuf)
        return;
    ui.artbuf = artbuf;
    if (nf_call(mp3id | NF_MP3_ART, artbuf, artcap, (long)e) > 0)
        ui.hasart = 1;
}

static void read_meta(void)
{
    char t[128], a[128], al[128];
    long br;

    nf_meta(0, t, sizeof(t));
    nf_meta(1, a, sizeof(a));
    nf_meta(2, al, sizeof(al));

    if (t[0])
        strncpy(ui_title, t, sizeof(ui_title) - 1);
    else if (ui.sel >= 0 && ui.sel < ntracks)
        strncpy(ui_title, list[ui.sel], sizeof(ui_title) - 1);
    ui_title[sizeof(ui_title) - 1] = '\0';

    snprintf(ui_sub, sizeof(ui_sub), "%s%s%s",
             a, (a[0] && al[0]) ? " - " : "", al);

    br = nf_call(mp3id | NF_MP3_INFO, 0L);        /* -1 on an older host */
    ui.bitrate = (br > 0 && br < 1000) ? (short)br : 0;
    switch (nf_call(mp3id | NF_MP3_INFO, 2L)) {    /* layer */
    case 1:  strcpy(ui_codec, "MPEG LAYER I");   break;
    case 2:  strcpy(ui_codec, "MPEG LAYER II");  break;
    case 3:  strcpy(ui_codec, "MPEG LAYER III"); break;
    default: ui_codec[0] = '\0';                 break;
    }
    if (ui_codec[0] && nf_call(mp3id | NF_MP3_INFO, 3L) == 1)
        strcat(ui_codec, " VBR");
    ui.codec = ui_codec[0] ? ui_codec : NULL;

    want_art();
}

static void start_track(int i)
{
    char path[400];

    if (i < 0 || i >= ntracks)
        return;
    snprintf(path, sizeof(path), "%.255s\\%.63s", dir, list[i]);
    if (nf_call(mp3id | NF_MP3_PLAY, path) == 0) {
        ui.sel = (short)i;
        ui.playing = 1;
        ui.paused = 0;
        ui.len_s = nf_call(mp3id | NF_MP3_LEN);
        ui.pos_s = 0;
        read_meta();
    } else {
        ui.playing = 0;
        snprintf(ui_title, sizeof(ui_title), "Cannot play %s", list[i]);
        ui_sub[0] = '\0';
    }
}

static void stop_track(void)
{
    nf_call(mp3id | NF_MP3_STOP);
    ui.playing = 0;
    ui.paused = 0;
    ui.pos_s = 0;
}

/* ------------------------------------------------------------- drawing --- */

/*
 * Each drawing function gets the rectangle it is being asked for. Most
 * ignore it - they draw one small thing - but draw_all draws only what
 * meets it, which is what makes a WM_REDRAW for a thin exposed strip
 * cheap.
 */
static void draw_all(const GRECT *c)    { mp3ui_draw_clip(&ui, vh, c); }
static void draw_band(const GRECT *c)   { (void)c; mp3ui_draw_band(&ui, vh); }
static void draw_clock(const GRECT *c)  { (void)c; mp3ui_draw_clock(&ui, vh); }
static void draw_status(const GRECT *c) { (void)c; mp3ui_draw_status(&ui, vh); }

static void draw_iconic(const GRECT *c)
{
    (void)c;
    apj_fill(vh, wx, wy, ww, wh, apj_pen(APJ_R_PANEL));
}

/* Walk the AES rectangle list, clip, and call fn for each visible part. */
static void redraw(void (*fn)(const GRECT *), short rx, short ry, short rw, short rh)
{
    GRECT r, d = { rx, ry, rw, rh };

    if (iconified)
        fn = draw_iconic;

    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, NULL);
    wind_get(win, WF_FIRSTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
    while (r.g_w && r.g_h) {
        GRECT i = r;
        if (rc_intersect(&d, &i)) {
            apj_clip(vh, i.g_x, i.g_y, i.g_w, i.g_h);
            fn(&i);
        }
        wind_get(win, WF_NEXTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
    }
    apj_clip_off(vh);
    graf_mouse(M_ON, NULL);
    wind_update(END_UPDATE);
}

static void draw_list(const GRECT *c) { (void)c; mp3ui_draw_list(&ui, vh); }

/* just the seek strip: the position moved */
static void redraw_clock(void)
{
    GRECT r;

    mp3ui_clock_rect(&ui, &r);
    redraw(draw_clock, r.g_x, r.g_y, r.g_w, r.g_h);
}

/* the list's own rectangle, so a scroll repaints nothing else */
static void redraw_list(void)
{
    const APJ_LAY *l = apj_lay_find(ui.lay, ui.nlay, W_LIST);

    if (l)
        redraw(draw_list, l->x, l->y, l->w, l->h);
}

static void set_top(long t)
{
    long max = (long)ui.ntracks - ui.visrows;

    if (max < 0) max = 0;
    if (t > max) t = max;
    if (t < 0)   t = 0;
    if ((short)t != ui.top) {
        ui.top = (short)t;
        if (!iconified)
            redraw_list();
    }
}

static void relayout(void)
{
    wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
    mp3ui_layout(&ui, vh, wx, wy, ww, wh);
    set_top(ui.top);                  /* clamps after a resize */
}

#define WIN_KIND    (NAME | CLOSER | MOVER | SMALLER)

/*
 * Scale. The skin comes in 100, 125 and 175 percent sheets; by default the
 * screen width picks one (1920 wide gets 175), which is a lot of player on
 * a 1080-line desktop. Keys 1, 2 and 3 pick a sheet, 0 goes back to the
 * screen's choice, and the choice is kept in MP3GEM.INF next to the .PRG:
 *
 *     scale=125
 *
 * A number other than 100, 125 or 175 (or no file) means "by screen".
 */
static void inf_path(char *out, long n)
{
    char pd[192];

    apj_skin_progdir(pd, (long)sizeof pd);
    sprintf(out, "%.*sMP3GEM.INF", (int)(n - 12), pd);
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

/* the window size this scale wants: the skin's 480x320 pt, never under the
 * layout's minimum, never over the desktop */
static void natural_size(short *w, short *h)
{
    short dx, dy, dw, dh, mw, mh;

    *w = apj_skin_ok() ? apj_skin_m(480) : (short)(48 * cw);
    *h = apj_skin_ok() ? apj_skin_m(320) : (short)(16 * ch);
    mp3ui_minsize(&mw, &mh);
    if (*w < mw) *w = mw;
    if (*h < mh) *h = mh;
    wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
    if (*w > dw) *w = dw;
    if (*h > dh) *h = dh;
}

static void arm_m1(void);

static void set_scale(short scale)
{
    short x, y, w, h, cx, cy, cw2, ch2, nw, nh;

    if (scale == apj_skin_preferred() && apj_skin_ok())
        return;
    apj_skin_prefer(scale);
    apj_skin_reload(vh);
    inf_save(scale);
    if (ui.playing)
        want_art();                   /* the cover tile is a different size now */

    /* keep the top-left corner, take the new natural size */
    wind_get(win, WF_CURRXYWH, &x, &y, &w, &h);
    natural_size(&nw, &nh);
    wind_calc(WC_BORDER, WIN_KIND, wx, wy, nw, nh, &cx, &cy, &cw2, &ch2);
    wind_set(win, WF_CURRXYWH, x, y, cw2, ch2);
    relayout();
    arm_m1();
    if (!iconified)
        redraw(draw_all, wx, wy, ww, wh);
}

/*
 * Hover. MU_M1 gives one rectangle: while the pointer is over a widget we
 * ask to hear about it LEAVING that widget, and while it is not we ask to
 * hear about it ENTERING the strip. That is the whole hover machine.
 *
 * The flag is MO_ENTER (0) / MO_LEAVE (1). Getting those two the wrong way
 * round asks the AES for a condition that is already true, it answers at
 * once, and evnt_multi() spins without ever letting the timer elapse -
 * but only while this window is on top, since rectangle events are not
 * delivered to anyone else. Which is precisely the "clock runs only when
 * the player is unfocused" that took most of a morning to find.
 */
static void arm_m1(void)
{
    const APJ_LAY *l = ui.hover >= 0
        ? apj_lay_find(ui.lay, ui.nlay, ui.hover) : NULL;

    if (l) {
        m1r.g_x = l->x; m1r.g_y = l->y; m1r.g_w = l->w; m1r.g_h = l->h;
        m1flag = MO_LEAVE;
    } else {
        mp3ui_bbox(&ui, &m1r);
        m1flag = MO_ENTER;
    }
}

static void set_hover(short mx, short my)
{
    short h = mp3ui_hit(&ui, mx, my);

    if (h == W_LIST || h == W_ART)
        h = -1;
    if (h != ui.hover) {
        ui.hover = h;
        if (!iconified)
            redraw(draw_band, wx, wy, ww, wh);
    }
    arm_m1();
}

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
        if (n > 4 && strcasecmp(e->d_name + n - 4, ".mp3") == 0 && n < NAMELEN) {
            strcpy(list[ntracks], e->d_name);
            ntracks++;
        }
    }
    closedir(dp);
    qsort(list, ntracks, NAMELEN, cmpname);
    ui.ntracks = (short)ntracks;
    ui.ntimed = have_filelen ? 0 : -1;
    ui.nbad = 0;
    scan_shown = 0;
    scan_wait = 0;
    scan_done = !have_filelen;
    ui.top = 0;
    ui.sel = ntracks ? 0 : -1;
    scan_reset();
    return ntracks;
}

static void scroll_to(short i)
{
    if (i < ui.top)
        ui.top = i;
    if (ui.visrows > 0 && i >= ui.top + ui.visrows)
        ui.top = (short)(i - ui.visrows + 1);
}

static void open_in_dir(const char *path, const char *fname)
{
    int i;

    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    { char *bs = strrchr(dir, '\\'); if (bs) *bs = '\0'; }
    ui.dir = dir;

    if (load_dir(dir) > 0) {
        for (i = 0; i < ntracks; i++)
            if (strcasecmp(list[i], fname) == 0) { ui.sel = (short)i; break; }
        scroll_to(ui.sel);
        start_track(ui.sel);
    }
    redraw(draw_all, wx, wy, ww, wh);
}

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
    open_in_dir(fpath, fname);
}

/* A file handed to us - on the command line (double-clicked in the desktop,
 * which runs MP3GEM for *.MP3) or in a VA_START while running. */
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
    int i;

    if (!ntracks)
        return;
    i = ui.sel + step;
    if (i < 0)
        i = 0;
    if (i >= ntracks) {
        stop_track();
        redraw(draw_all, wx, wy, ww, wh);
        return;
    }
    scroll_to((short)i);
    start_track(i);
    redraw(draw_all, wx, wy, ww, wh);
}

static void set_volume(short v)
{
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    if (nf_call(mp3id | NF_MP3_VOLUME, (long)v) >= 0)
        ui.vol = v;
}

static void do_widget(short id, short mx)
{
    const APJ_LAY *l;

    switch (id) {
    case W_PREV: next_track(-1); break;
    case W_NEXT: next_track(+1); break;
    case W_RW:   nf_call(mp3id | NF_MP3_SEEK, (long)-10); break;
    case W_FF:   nf_call(mp3id | NF_MP3_SEEK, (long)+10); break;
    case W_PLAY:
        if (ui.playing) {
            ui.paused = !ui.paused;
            nf_call(mp3id | NF_MP3_PAUSE, (long)ui.paused);
        } else
            start_track(ui.sel);
        redraw(draw_all, wx, wy, ww, wh);
        break;
    case W_STOP:
        stop_track();
        redraw(draw_all, wx, wy, ww, wh);
        break;
    case W_SHUFFLE: ui.shuffle = !ui.shuffle; break;
    case W_REPEAT:  ui.repeat  = !ui.repeat;  break;
    case W_OPEN:
        do_open();
        redraw(draw_all, wx, wy, ww, wh);
        break;
    case W_VOLICO:                    /* mute toggles, and remembers */
        if (ui.hasvol) {
            if (ui.vol) {
                vol_before_mute = ui.vol;
                set_volume(0);
            } else
                set_volume(vol_before_mute > 0 ? vol_before_mute : 100);
        }
        break;
    case W_VOL:
        l = apj_lay_find(ui.lay, ui.nlay, W_VOL);
        if (l && l->w > 0 && ui.hasvol)
            set_volume((short)((long)(mx - l->x) * 100L / l->w));
        break;
    case W_SEEK:
        /* MP3PLAY seeks by a delta, so aim at the clicked position */
        l = apj_lay_find(ui.lay, ui.nlay, W_SEEK);
        if (l && l->w > 0 && ui.len_s > 0) {
            long want = (long)(mx - l->x) * ui.len_s / l->w;
            nf_call(mp3id | NF_MP3_SEEK, want - ui.pos_s);
        }
        break;
    default:
        return;
    }
}

static void click(short mx, short my)
{
    short id = mp3ui_hit(&ui, mx, my);
    short row;

    /*
     * The scrollbar is not a button. It was falling into the button branch
     * below - press feedback, do_widget() doing nothing, two redraws of the
     * whole band - and the scroll code after it was never reached.
     */
    if (id == W_SCROLL) {
        if (!mp3ui_scroll_needed(&ui))
            return;
        {
            short part = mp3ui_scroll_part(&ui, my);

            if (part < 0)
                set_top(ui.top - ui.visrows);
            else if (part > 0)
                set_top(ui.top + ui.visrows);
            else {
                /* drag the thumb: follow the mouse until the button goes up */
                GRECT t;
                short grab, bmx, bmy, bst, bks;

                mp3ui_thumb_rect(&ui, &t);
                grab = (short)(my - t.g_y);
                ui.dragging = 1;
                redraw_list();
                for (;;) {
                    graf_mkstate(&bmx, &bmy, &bst, &bks);
                    if (!(bst & 1))
                        break;
                    set_top(mp3ui_scroll_top_for(&ui, bmy, grab));
                    evnt_timer(20L);
                }
                ui.dragging = 0;
                redraw_list();
            }
        }
        return;
    }

    if (id >= 0 && id != W_LIST && id != W_ART) {
        ui.press = id;
        redraw(draw_band, wx, wy, ww, wh);
        evnt_timer(70L);
        ui.press = -1;
        do_widget(id, mx);
        if (!iconified)
            redraw(draw_band, wx, wy, ww, wh);
        return;
    }
    row = mp3ui_row_at(&ui, my);
    if (row >= 0 && id == W_LIST) {
        start_track(row);
        redraw(draw_all, wx, wy, ww, wh);
    }
    (void)mx;
}

/* ---------------------------------------------------------------- main --- */

/*
 * No SIZER: XaAES's Fluent chrome reserves a 12 px column on the right of
 * a sizeable window for it, which put the player off-centre. This is a
 * fixed-layout player; the window is the size the skin says it is.
 */

#define VA_START    0x4711     /* AV protocol: open these files */
#define AV_STARTED  0x4738

int main(int argc, char *argv[])
{
    short work_in[11], work_out[57];
    short d, msg[8];
    short mx, my, mb, ks, kr, brk;
    short ev, i;
    long  v;

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
    for (i = 0; i < 10; i++) work_in[i] = 1;
    work_in[10] = 2;
    v_opnvwk(work_in, &vh, work_out);
    vst_alignment(vh, 0, 5, &d, &d);              /* left / top text origin */
    apj_init(vh);                                 /* theme + renderer, before any window */
    apj_skin_prefer(inf_scale());                 /* MP3GEM.INF, else by screen */
    apj_skin_load(vh, NULL);                      /* follows the theme; may fail */

    memset(&ui, 0, sizeof(ui));
    ui.title = ui_title;
    ui.sub = ui_sub;
    ui.name_of = name_of;
    ui.len_of = len_of;
    ui.sel = -1;
    ui.hover = -1;
    ui.press = -1;
    ui.vol = 100;
    v = nf_call(mp3id | NF_MP3_VOLUME, -1L);
    if (v >= 0) { ui.hasvol = 1; ui.vol = (short)v; }

    {
        short dx, dy, dw, dh, cx, cy, cwid, chgt;
        short want_w, want_h;

        natural_size(&want_w, &want_h);
        wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
        wind_calc(WC_BORDER, WIN_KIND,
                  dx + 16, dy + 16, want_w, want_h, &cx, &cy, &cwid, &chgt);
        win = wind_create(WIN_KIND, cx, cy, cwid, chgt);
        wind_set_str(win, WF_NAME, "PiSTorm MP3");
        wind_open(win, cx, cy, cwid, chgt);
        /* XaAES only sends wheel events to a window that asked for them;
         * as WM_ARROWED with the click count in the high byte of msg[4] */
        wind_set(win, WF_WHEEL, 1, WHEEL_ARROWED, 0, 0);
        relayout();
    }
    arm_m1();
    redraw(draw_all, wx, wy, ww, wh);

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
                    break;
                case WM_TOPPED:
                    wind_set(win, WF_TOP, 0, 0, 0, 0);
                    break;
                case WM_MOVED:
                case WM_SIZED:
                    wind_set(win, WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
                    relayout();
                    arm_m1();
                    if (msg[0] == WM_SIZED)
                        redraw(draw_all, wx, wy, ww, wh);
                    break;
                case WM_ARROWED: {    /* the mouse wheel */
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
                    wind_set(win, WF_ICONIFY, msg[4], msg[5], msg[6], msg[7]);
                    iconified = 1;
                    relayout();
                    break;
                case WM_UNICONIFY:
                    wind_set(win, WF_UNICONIFY, msg[4], msg[5], msg[6], msg[7]);
                    iconified = 0;
                    relayout();
                    arm_m1();
                    redraw(draw_all, wx, wy, ww, wh);
                    break;
                case APJ_SKINCHG:         /* the desktop changed theme */
                    apj_init(vh);
                    apj_skin_reload(vh);
                    if (ui.playing)
                        want_art();
                    relayout();
                    arm_m1();
                    if (!iconified)
                        redraw(draw_all, wx, wy, ww, wh);
                    break;
                case VA_START: {          /* opened again while running */
                    short reply[8];
                    char *cmd = (char *)(((long)msg[3] << 16) | (unsigned short)msg[4]);
                    if (iconified) {
                        wind_set(win, WF_UNICONIFY, -1, -1, -1, -1);
                        iconified = 0;
                        relayout();
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
        if ((ev & MU_M1) && !iconified)
            set_hover(mx, my);
        if ((ev & MU_BUTTON) && !iconified)
            click(mx, my);
        if (ev & MU_KEYBD) {
            char  c    = (char)(kr & 0xff);
            short scan = (short)((kr >> 8) & 0xff);

            /*
             * The mouse wheel arrives HERE, not as a wheel event: the
             * PiStorm's USB bridge (kbd_usb.c) turns each wheel click into
             * a cursor Up/Down key tap. XaAES never sees a wheel at all,
             * so WF_WHEEL has nothing to deliver. Arrow keys scroll the
             * list one row; PgUp/PgDn a page.
             */
            if      (scan == 0x48) set_top(ui.top - 1);
            else if (scan == 0x50) set_top(ui.top + 1);
            else if (scan == 0x49) set_top(ui.top - ui.visrows);
            else if (scan == 0x51) set_top(ui.top + ui.visrows);
            else if (c == ' ')             do_widget(W_PLAY, 0);
            else if (c == 'n' || c == 'N') do_widget(W_NEXT, 0);
            else if (c == 'p' || c == 'P') do_widget(W_PREV, 0);
            else if (c == 'q' || c == 'Q' || c == 0x1b) goto out;
            else if (c == '1') set_scale(100);   /* the sheet to use ... */
            else if (c == '2') set_scale(125);
            else if (c == '3') set_scale(175);
            else if (c == '0') set_scale(0);     /* ... or the screen's choice */
        }
        if (ev & MU_TIMER) {
            int relist = scan_step();
            int clock = 0;

            ui.dbg_ticks++;
            if (ui.playing && !ui.paused) {
                long p = nf_call(mp3id | NF_MP3_POS);
                ui.dbg_pos = p;
                if (p >= 0 && p != ui.pos_s) {
                    ui.pos_s = p;
                    clock = 1;            /* once a second, not every tick */
                }
                ui.dbg_status = nf_call(mp3id | NF_MP3_STATUS);
                if (ui.dbg_status == 0) {
                    if (ui.repeat)
                        start_track(ui.sel);
                    else
                        next_track(+1);
                    redraw(draw_all, wx, wy, ww, wh);
                    continue;
                }
            }
            /*
             * Repaint only what changed. Every redraw takes the AES update
             * lock and blends antialiased text on the 68k, and doing the
             * whole band four times a second made dragging other windows
             * jerky while a track played.
             */
            if (!iconified) {
                if (clock)
                    redraw_clock();
                if (relist)
                    redraw_list();
                if (!scan_shown) {
                    scan_shown = 1;
                    redraw(draw_status, wx, wy, ww, wh);
                }
#if MP3UI_DEBUG
                if (clock || (ui.dbg_ticks & 3) == 0)
                    redraw(draw_status, wx, wy, ww, wh);
#endif
            }
        }
    }

out:
    /* closing the player is stopping it - a track playing on with nothing
     * on screen to stop it was the wrong kind of surprise */
    nf_call(mp3id | NF_MP3_STOP);
    apj_skin_free();
    if (artbuf)
        Mfree(artbuf);
    wind_close(win);
    wind_delete(win);
    v_clsvwk(vh);
    appl_exit();
    return 0;
}
