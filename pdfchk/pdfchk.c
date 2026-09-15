/*
 * pdfchk.c - PSPDF NatFeat test tool (phase 1 of the PDF viewer)
 *
 * Exercises every PSPDF sub-op from the guest and prints timings, so the
 * host side can be tested before PDFGEM exists.
 *
 *     PDFCHK S:\MEDIA\MANUAL.PDF
 *     PDFCHK S:\MEDIA\MANUAL.PDF 42            (render page 42)
 *     PDFCHK S:\MEDIA\MANUAL.PDF 42 engine     (and search that page)
 *
 * The file must be on a HOSTFS drive (the Pi opens the real file), or be a
 * plain host path like /home/pistorm/x.pdf.
 *
 * Build:   make        (needs m68k-atari-mint-gcc)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <osbind.h>

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

#define PSPDF_OK        0
#define PSPDF_ERR     (-1)
#define PSPDF_LOCKED  (-2)
#define PSPDF_BUSY    (-3)

#define ZOOM_100     1000

static long id;

/* 200 Hz system timer, read in supervisor mode */
static long get200(void) { return *(volatile long *)0x4baL; }
static long ticks(void)  { return Supexec(get200); }
static long ms(long t)   { return t * 5; }          /* 200 Hz -> ms */

int main(int argc, char **argv)
{
    const char *path;
    long handle, pages, outline, size, t0, t1;
    int page = 1;
    const char *needle = NULL;
    void *buf;
    long bw = 1920, bh = 1080, stride;
    int i;

    printf("\033EPDFCHK - PSPDF test\r\n\r\n");

    if (argc < 2) {
        printf("usage: PDFCHK <file.pdf> [page] [search word]\r\n");
        printf("       the file must be on a HOSTFS drive\r\n");
        goto wait;
    }
    path = argv[1];
    if (argc > 2) page = atoi(argv[2]);
    if (argc > 3) needle = argv[3];

    id = nf_id("PSPDF");
    if (!id) {
        printf("PSPDF not present - old emulator, or not built with it\r\n");
        goto wait;
    }
    printf("PSPDF API version %ld\r\n", nf_call(id | PSPDF_VERSION));

    t0 = ticks();
    handle = nf_call(id | PSPDF_OPEN, path);
    t1 = ticks();
    if (handle <= 0) {
        printf("open %s failed (%ld%s)\r\n", path, handle,
               handle == PSPDF_LOCKED ? " = needs a password" : "");
        goto wait;
    }
    printf("open  %s -> handle %ld, %ld ms\r\n", path, handle, ms(t1 - t0));

    pages   = nf_call(id | PSPDF_INFO, handle, 0L);
    outline = nf_call(id | PSPDF_INFO, handle, 1L);
    printf("pages %ld, outline entries %ld\r\n", pages, outline);
    if (page < 1 || page > pages) page = 1;

    {
        char meta[256];
        if (nf_call(id | PSPDF_META, handle, 0L, meta, 256L) > 0)
            printf("title \"%s\"\r\n", meta);
    }

    do {
        size = nf_call(id | PSPDF_PAGESIZE, handle, (long)page, (long)ZOOM_100);
    } while (size == PSPDF_BUSY);
    if (size < 0) {
        printf("pagesize failed (%ld)\r\n", size);
        goto close;
    }
    printf("page %d is %ld x %ld pixels at 100%%\r\n",
           page, (size >> 16) & 0xffff, size & 0xffff);

    /* the page buffer has to be TT-RAM: Mxalloc mode 1 */
    stride = bw * 4;
    buf = (void *)Mxalloc(stride * bh, 1);
    if (!buf) {
        printf("no TT-RAM for a %ld KB buffer\r\n", (stride * bh) >> 10);
        goto close;
    }

    t0 = ticks();
    if (nf_call(id | PSPDF_RENDER, handle, (long)page, (long)ZOOM_100, 0L) < 0) {
        printf("render failed\r\n");
        goto freebuf;
    }
    while (nf_call(id | PSPDF_STATUS, handle) == 1)
        ;
    t1 = ticks();
    if (nf_call(id | PSPDF_STATUS, handle) < 0) {
        printf("render reported an error\r\n");
        goto freebuf;
    }
    printf("render %ld ms\r\n", ms(t1 - t0));

    t0 = ticks();
    i = (int)nf_call(id | PSPDF_FETCH, handle, buf, 0L, 0L, bw, bh,
                     32L, stride, 0x808080L);
    t1 = ticks();
    if (i < 0) {
        printf("fetch failed (%d)\r\n", i);
        goto freebuf;
    }
    printf("fetch  %ldx%ld in %ld ms, first pixels %08lx %08lx\r\n",
           bw, bh, ms(t1 - t0),
           ((unsigned long *)buf)[0], ((unsigned long *)buf)[1]);

    /* second fetch of the same page: this is what scrolling costs */
    t0 = ticks();
    nf_call(id | PSPDF_FETCH, handle, buf, 100L, 200L, bw, bh, 32L, stride,
            0x808080L);
    t1 = ticks();
    printf("scroll %ld ms\r\n", ms(t1 - t0));

    {
        char text[512];
        i = (int)nf_call(id | PSPDF_TEXT, handle, (long)page, (long)ZOOM_100,
                         0L, text, 200L);
        if (i > 0) {
            text[60] = 0;
            printf("text   \"%s...\"\r\n", text);
        }
    }

    {
        long links[32 * 6];
        i = (int)nf_call(id | PSPDF_LINKS, handle, (long)page, (long)ZOOM_100,
                         links, 32L);
        printf("links  %d on page %d\r\n", i, page);
        if (i > 0) {
            printf("       first: %ld,%ld %ldx%ld kind %ld target %ld\r\n",
                   links[0], links[1], links[2], links[3], links[4], links[5]);
            if (links[4] == 1) {
                char uri[256];
                if (nf_call(id | PSPDF_LINKURI, handle, (long)page, links[5],
                            uri, 256L) > 0)
                    printf("       uri: %s\r\n", uri);
            }
        }
    }

    if (outline > 0) {
        printf("outline:\r\n");
        for (i = 0; i < outline && i < 5; i++) {
            struct { long depth; long page; char title[80]; } ent;
            if (nf_call(id | PSPDF_OUTLINE, handle, (long)i, &ent) == 0)
                printf("  %*s%s (page %ld)\r\n", (int)ent.depth * 2, "",
                       ent.title, ent.page);
        }
    }

    if (needle) {
        long rects[64 * 4];
        t0 = ticks();
        i = (int)nf_call(id | PSPDF_FIND, handle, needle, (long)page, 0L,
                         (long)ZOOM_100, rects, 64L);
        t1 = ticks();
        printf("find \"%s\" on page %d: %d hits, %ld ms\r\n",
               needle, page, i, ms(t1 - t0));
        if (i > 0)
            printf("      first at %ld,%ld %ldx%ld\r\n",
                   rects[0], rects[1], rects[2], rects[3]);
    }

freebuf:
    Mfree(buf);
close:
    nf_call(id | PSPDF_CLOSE, handle);
    printf("closed\r\n");

wait:
    /* no key wait: under TosWin2 the output stays on screen, and Cconin()
     * there just hangs when the window is not the one with the keyboard */
    return 0;
}
