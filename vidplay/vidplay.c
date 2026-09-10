/*
 * vidplay.c - PiStorm host VIDEO player front-end (TOS .ttp)
 *
 * Calls the "VIDPLAY" NatFeat exposed by the emulator. The Pi demuxes and
 * decodes the file in-process (libavformat/libavcodec), puts the picture on its
 * own DRM overlay plane above the Atari screen, and mixes the soundtrack into
 * the same SDL output as ST/STE sound and MP3. The 68k just asks.
 *
 * The file lives on the host, reached through a mounted HOSTFS drive - so give
 * a FULL path with the drive letter, e.g.
 *
 *     VIDPLAY U:\MM_MP4\FILM.MKV
 *     VIDPLAY "U:\MY FILMS\A B.MP4"
 *     VIDPLAY STOP
 *     VIDPLAY STATUS
 *
 * Keys while playing:
 *     SPACE  pause / resume        LEFT / RIGHT   seek -10s / +10s
 *     , / .  seek -60s / +60s      + / -          volume
 *     W      windowed 640x400      F              fullscreen (default)
 *     Q/ESC  stop and quit
 *
 * Build (in this folder):   make
 * Toolchain:                m68k-atari-mint-gcc
 */

#include <stdio.h>
#include <string.h>
#include <osbind.h>

/* ---- NatFeat call stubs (ARAnyM/PiStorm ABI) -----------------------------
 * 0x7300 = NF_GETID (name ptr at 4(sp) -> id in d0)
 * 0x7301 = NF_CALL  (id at 4(sp), args after -> result in d0)
 * Emitted naked so the opcode traps on the caller's stack frame. On real
 * hardware these are harmless moveq's, so nf_id() returns 0 = "not present". */
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

/* VIDPLAY subids - must match nf_vid_ops in atari_natfeat.cpp */
#define NF_VID_PLAY    0
#define NF_VID_STOP    1
#define NF_VID_STATUS  2
#define NF_VID_PAUSE   3
#define NF_VID_SEEK    4
#define NF_VID_POS     5
#define NF_VID_LEN     6
#define NF_VID_META    7
#define NF_VID_RECT    8
#define NF_VID_VOLUME  9
#define NF_VID_INFO   10

static long id;

static void put_time(long secs)
{
    if (secs < 0)
        fputs("--:--", stdout);
    else
        printf("%ld:%02ld", secs / 60, secs % 60);
}

/* Strip surrounding quotes and stray whitespace from the command line. */
static void tidy(char *s)
{
    size_t n;
    while (*s == ' ' || *s == '\t')
        memmove(s, s + 1, strlen(s));
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = 0;
        memmove(s, s + 1, strlen(s));
    }
}

static void banner(void)
{
    char buf[160];
    long w, h, fps, aud, hw;

    nf_call(id | NF_VID_META, 0L, (long)buf, 160L);
    printf("  %s\r\n", buf);
    nf_call(id | NF_VID_META, 2L, (long)buf, 160L);
    printf("  %s\r\n", buf);

    w   = nf_call(id | NF_VID_INFO, 0L);
    h   = nf_call(id | NF_VID_INFO, 1L);
    fps = nf_call(id | NF_VID_INFO, 2L);
    aud = nf_call(id | NF_VID_INFO, 3L);
    hw  = nf_call(id | NF_VID_INFO, 4L);
    printf("  %ldx%ld  %ld.%02ld fps  %s decode  %s\r\n",
           w, h, fps / 100, fps % 100, hw ? "hardware" : "software",
           aud ? "with sound" : "silent");
    fputs("  SPACE pause  <- -> 10s  , . 60s  +/- volume  W window  Q quit\r\n",
          stdout);
}

static void status_line(int paused)
{
    fputs("\r  ", stdout);
    put_time(nf_call(id | NF_VID_POS));
    fputs(" / ", stdout);
    put_time(nf_call(id | NF_VID_LEN));
    fputs(paused ? " [paused]   " : "            ", stdout);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    char path[512];
    int  i, paused = 0, vol = 100, windowed = 0;

    id = nf_id("VIDPLAY");
    if (!id) {
        fputs("VIDPLAY NatFeat not available (not running under PiStorm?)\r\n",
              stderr);
        return 1;
    }

    if (argc < 2) {
        fputs("usage: VIDPLAY <file.mp4|.mkv|...>  |  STOP  |  STATUS\r\n"
              "       the file must be on a HOSTFS drive\r\n", stdout);
        return 1;
    }

    /* Re-join the arguments: TTP splits on spaces, and people have spaces in
     * their filenames. */
    path[0] = 0;
    for (i = 1; i < argc; i++) {
        if (i > 1 && strlen(path) + 1 < sizeof path)
            strcat(path, " ");
        if (strlen(path) + strlen(argv[i]) < sizeof path - 1)
            strcat(path, argv[i]);
    }
    tidy(path);

    if (strcasecmp(path, "stop") == 0 || strcasecmp(path, "off") == 0) {
        nf_call(id | NF_VID_STOP);
        fputs("Video stopped.\r\n", stdout);
        return 0;
    }
    if (strcasecmp(path, "status") == 0) {
        if (nf_call(id | NF_VID_STATUS)) {
            banner();
            status_line(0);
            fputs("\r\n", stdout);
        } else {
            fputs("Nothing playing.\r\n", stdout);
        }
        return 0;
    }

    if (nf_call(id | NF_VID_PLAY, (long)path) != 0) {
        printf("VIDPLAY: could not play\r\n  %s\r\n", path);
        fputs("  (is it on a HOSTFS drive? is the DRM display active?)\r\n",
              stdout);
        return 1;
    }

    banner();

    /* The Pi does the work; we read keys and print the time. */
    while (nf_call(id | NF_VID_STATUS)) {
        long key;
        int  c, sc;

        status_line(paused);

        if (!Cconis()) {
            Vsync();                 /* one frame of nothing - cheap */
            continue;
        }
        key = Cconin();
        c   = (int)(key & 0xFF);
        sc  = (int)((key >> 16) & 0xFF);

        if (c == 'q' || c == 'Q' || c == 27) {
            nf_call(id | NF_VID_STOP);
            break;
        } else if (c == ' ') {
            paused = !paused;
            nf_call(id | NF_VID_PAUSE, (long)paused);
        } else if (sc == 0x4B) {                  /* cursor left  */
            nf_call(id | NF_VID_SEEK, -10L);
        } else if (sc == 0x4D) {                  /* cursor right */
            nf_call(id | NF_VID_SEEK, 10L);
        } else if (c == ',') {
            nf_call(id | NF_VID_SEEK, -60L);
        } else if (c == '.') {
            nf_call(id | NF_VID_SEEK, 60L);
        } else if (c == '+' || c == '=') {
            vol += 10; if (vol > 200) vol = 200;
            nf_call(id | NF_VID_VOLUME, (long)vol);
        } else if (c == '-') {
            vol -= 10; if (vol < 0) vol = 0;
            nf_call(id | NF_VID_VOLUME, (long)vol);
        } else if (c == 'w' || c == 'W') {
            /* A window is only honoured if the overlay can reduce the source
             * that far - the vc4 scaler tops out around 2x, so asking for
             * 640x400 from a 4K film gets clamped up to about half the screen.
             * Half the display is a request the hardware can always meet. */
            windowed = !windowed;
            if (windowed) {
                long w = nf_call(id | NF_VID_INFO, 6L) / 2;
                long h = nf_call(id | NF_VID_INFO, 7L) / 2;
                if (w < 320) { w = 640; h = 400; }
                nf_call(id | NF_VID_RECT, w / 4, h / 4, w, h);
            } else {
                nf_call(id | NF_VID_RECT, 0L, 0L, 0L, 0L);
            }
        } else if (c == 'f' || c == 'F') {
            windowed = 0;
            nf_call(id | NF_VID_RECT, 0L, 0L, 0L, 0L);
        }
    }

    fputs("\r\n  done.\r\n", stdout);
    return 0;
}
