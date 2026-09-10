/* psmon.c - PSMON.PRG / PSMON.ACC
 *
 * PiStorm Monitor: a GEM window showing guest CPU/JIT figures (from the host
 * over the PSCTRL NatFeat), guest memory use, and - on FreeMiNT - a list of
 * running tasks.
 *
 * Builds two ways from this one source, matching the house style of the other
 * cdev/ front-ends:
 *     make PSMON.PRG              full program, task list included
 *     make PSMON.ACC              desk accessory (-DBUILD_ACC), no task list
 *
 * The PRG is aimed at FreeMiNT, where there are tasks to list and where the
 * AES is preemptive. The ACC is aimed at plain GEM desktops - EmuTOS above all
 * - so it is deliberately lean: CPU/JIT figures and memory, nothing that calls
 * a GEMDOS function EmuTOS does not have.
 *
 * Plain GEM throughout, so both run on TOS 1.x as well as FreeMiNT/XaAES.
 *
 * The memory half needs NOTHING from the emulator - totals come from the
 * low-memory system variables and free space from GEMDOS. The CPU/JIT half
 * needs the host-side PSCTRL NatFeat (see PSCTRL-HOST.md in the emulator
 * tree); without it the window says so and still shows everything else.
 *
 * Known limitation, by design: under cooperative single-TOS GEM an accessory
 * only gets CPU time when the foreground application calls the AES, so inside
 * a game that never does, the display stops updating. Because the host samples
 * on its own wall-clock tick, the first redraw after such a stall shows current
 * values rather than a stale delta. Under MiNT it is preemptive and normal.
 */

#include <gem.h>
#include <osbind.h>
#include <stdio.h>
#include <string.h>

#ifndef BUILD_ACC
# include <mint/mintbind.h>
#endif

/* ---- NatFeat stubs (0x7300 GET_ID / 0x7301 CALL) -------------------------
 *
 * Same shape as vidgem.c / mp3gem.c: the opcodes read their arguments from the
 * caller's stack frame, so 0(sp) has to be the return address - these must be
 * real JSR subroutines, not inline asm inside a function with a prologue.
 *
 * nf_probe differs from the other front-ends deliberately. $7300 is NOT
 * harmless on a real 68000 - it is an illegal instruction - so the probe
 * installs a temporary vector-4 handler and steps over the opcode if it traps.
 * Costs 20 lines and means running this on a bare ST reports "unsupported"
 * instead of crashing. Must be called through Supexec().
 */
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
/* long nf_probe(void) - reads _nf_probe_name, returns id or 0 */
"   .globl _nf_probe\n"
"_nf_probe:\n"
"   movem.l %d2/%a2,-(%sp)\n"
"   move.l 0x10:w,%d2\n"              /* save vector 4                  */
"   move.l #_nf_trap_handler,0x10:w\n"
"   clr.l _nf_probe_trapped\n"
"   move.l _nf_probe_name,-(%sp)\n"   /* build the 4(sp) frame _nf_id wants */
"   jsr _nf_id\n"
"   addq.l #4,%sp\n"
"   tst.l _nf_probe_trapped\n"
"   beq 1f\n"
"   moveq #0,%d0\n"                   /* trapped -> not supported       */
"1:\n"
"   move.l %d2,0x10:w\n"              /* restore vector 4               */
"   movem.l (%sp)+,%d2/%a2\n"
"   rts\n"
"_nf_trap_handler:\n"
"   move.l #1,_nf_probe_trapped\n"
"   addq.l #2,2(%sp)\n"               /* step stacked PC past the opcode */
"   moveq #0,%d0\n"
"   rte\n"
);

extern long nf_id(const char *name);
extern long nf_call(long id, ...);
extern long nf_probe(void);

long nf_probe_name = 0;      /* set before calling nf_probe via Supexec */
long nf_probe_trapped = 0;

/* ---- PSCTRL ------------------------------------------------------------- */

#define PS_VERSION  0L
#define PS_GETINT   1L

#define PS_STAT_EPOCH        32L
#define PS_STAT_MHZ_X100     33L
#define PS_STAT_SPEEDX_X100  34L
#define PS_STAT_HITRATE_X100 35L
#define PS_STAT_IDLE_X100    36L
#define PS_STAT_WAIT_X100    37L
#define PS_STAT_IDLE_VALID   38L
#define PS_STAT_CACHE_USED   39L
#define PS_STAT_CACHE_SIZE   40L
#define PS_STAT_COMPILES     41L
#define PS_STAT_FLUSHES      42L
#define PS_STAT_INVALIDATES  43L
#define PS_CFG_TTRAM_SIZE    45L
#define PS_STAT_INTERP_X100  46L
#define PS_STAT_MIPS_X100    47L

#define PS_BAD  (-1L)

/* ---- layout ------------------------------------------------------------- */

#define WIN_COLS   46
#define STAT_ROWS  15          /* rows used by the CPU/JIT + memory block  */
#define REFRESH_MS 500
#define RAMVALID_MAGIC 0x1357BD13L
#define EINVFN  (-32L)
#define TTRAM_BASE 0x01000000L

#ifdef BUILD_ACC
# define WIN_ROWS  (STAT_ROWS + 2)
#else
# define MAX_TASKS 32          /* collected; displayed count fits the window */
# define WIN_ROWS  (STAT_ROWS + 2 + 10)
#endif

/* ---- state -------------------------------------------------------------- */

static long  psid = 0;              /* PSCTRL base id, 0 = absent */
static int   have_mxalloc = 0;
static short vh;                    /* VDI handle */
static short win = -1;
static short cw = 8, ch = 16;       /* char cell */
static short apid;
#ifdef BUILD_ACC
static short menu_id = -1;
#endif

static long sv_phystop, sv_ramtop, sv_ramvalid;

static struct {
    int  host;
    long epoch, mhz, speedx, hit, idle, wait;
    int  idle_valid;
    long interp, mips;
    long cache_used, cache_size, compiles, flushes, invalidates;
    long st_total, st_free, tt_total, tt_free;
    int  free_is_largest;
} S;

static long last_sig = -1;

#ifndef BUILD_ACC
/* ---- FreeMiNT task list -------------------------------------------------
 *
 * Enumerated from u:\proc, whose layout is fixed by the kernel's procfs:
 * entries are named "<name>.<pid>" with the pid also returned in the leading
 * long of each Dxreaddir record, xattr.size is the process's memory use, and
 * xattr.attr encodes the run queue. Those attribute values are the kernel's
 * p_attr[] table, not an invention here:
 *
 *     0x00 running   0x01 ready    0x20 waiting  0x21 iobound/select
 *     0x22 zombie    0x02 tsr      0x24 stopped
 *
 * Nothing pokes at kernel structures, so this stays correct across kernel
 * versions. Under EmuTOS Dopendir does not exist, GEMDOS answers EINVFN, and
 * the whole section quietly disappears from the window.
 */

/* The MiNT XATTR, as the kernel defines it in sys/mint/stat.h. Declared here
 * because MiNTLib does not export the struct. */
typedef struct {
    unsigned short mode;
    long           index;
    unsigned short dev;
    unsigned short rdev;
    unsigned short nlink;
    unsigned short uid;
    unsigned short gid;
    long           size;
    long           blksize;
    long           nblocks;
    unsigned short mtime, mdate;
    unsigned short atime, adate;
    unsigned short ctime, cdate;
    short          attr;
    short          reserved2;
    long           reserved3[2];
} PSXATTR;

#define PROC_NAMEMAX 16         /* kernel's PNAMSIZ (8) + ".ppp" + slack */

typedef struct {
    char name[PROC_NAMEMAX];
    long pid;
    long size;
    short attr;
} TASKENT;

static TASKENT tasks[MAX_TASKS];
static int     ntasks = 0;
static int     have_proc = 1;   /* cleared once Dopendir says otherwise */
static int     top_task = 0;    /* first task row shown; driven by the slider */

static const char *state_name(short attr)
{
    switch (attr & 0x27) {
    case 0x00: return "run";
    case 0x01: return "ready";
    case 0x20: return "wait";
    case 0x21: return "io";
    case 0x22: return "zombie";
    case 0x02: return "tsr";
    case 0x24: return "stop";
    default:   return "?";
    }
}

static void sample_tasks(void)
{
    char     rec[4 + PROC_NAMEMAX + 4];
    PSXATTR  xa;
    long     dh, r, xret;
    char    *nm;
    int      i, j;

    ntasks = 0;
    if (!have_proc)
        return;

    dh = Dopendir("u:\\proc", 0);
    if (dh < 0) {
        /* EINVFN means this is not MiNT at all - stop asking. Anything else
         * (proc not mounted, permissions) might change, so keep trying. */
        if (dh == EINVFN)
            have_proc = 0;
        return;
    }

    while (ntasks < MAX_TASKS) {
        memset(rec, 0, sizeof rec);
        xret = -1L;
        r = Dxreaddir((int)sizeof rec, dh, rec, &xa, &xret);
        if (r != 0)
            break;                      /* ENMFILES ends the walk */

        nm = rec + 4;                   /* flag 0: leading long is the pid */
        if (nm[0] == '\0' || nm[0] == '.')
            continue;                   /* "." and ".." */
        if (xret != 0)
            continue;                   /* no xattr, nothing worth showing */

        /* Insertion sort, biggest first: with no per-process CPU figure,
         * memory is the column worth ranking on. */
        for (i = 0; i < ntasks; i++)
            if (xa.size > tasks[i].size)
                break;
        for (j = ntasks; j > i; j--)
            tasks[j] = tasks[j - 1];

        strncpy(tasks[i].name, nm, PROC_NAMEMAX - 1);
        tasks[i].name[PROC_NAMEMAX - 1] = '\0';
        {   /* trim the ".ppp" suffix; the pid has its own column */
            char *dot = strrchr(tasks[i].name, '.');
            if (dot) *dot = '\0';
        }
        tasks[i].pid  = *(long *)rec;
        tasks[i].size = xa.size;
        tasks[i].attr = xa.attr;
        ntasks++;
    }

    Dclosedir(dh);
}
#endif /* !BUILD_ACC */

/* ---- gathering ---------------------------------------------------------- */

/* Runs in supervisor mode via Supexec. */
static long read_sysvars(void)
{
    sv_phystop  = *(volatile long *)0x42EL;
    sv_ramtop   = *(volatile long *)0x5A4L;
    sv_ramvalid = *(volatile long *)0x5A8L;
    return 0;
}

static long getint(long idx)
{
    if (!psid)
        return PS_BAD;
    return nf_call(psid | PS_GETINT, idx);
}

static long getint0(long idx)       /* PS_BAD -> 0, for display */
{
    long v = getint(idx);
    return (v == PS_BAD) ? 0L : v;
}

static int pct(long used, long total)
{
    long p;
    if (total <= 0 || used <= 0) return 0;
    if (used >= total)           return 100;
    if (used < 21000000L) p = (used * 100L) / total;
    else                  p = (used / 1024L) * 100L / (total / 1024L);
    return (p < 0) ? 0 : (p > 100 ? 100 : (int)p);
}

static void sample(void)
{
    long v;

    /* --- host figures --- */
    S.host = 0;
    if (psid) {
        v = getint(PS_STAT_EPOCH);
        if (v != PS_BAD) {
            S.host        = 1;
            S.epoch       = v;
            S.mhz         = getint0(PS_STAT_MHZ_X100);
            S.speedx      = getint0(PS_STAT_SPEEDX_X100);
            S.hit         = getint0(PS_STAT_HITRATE_X100);
            S.interp      = getint0(PS_STAT_INTERP_X100);
            S.mips        = getint0(PS_STAT_MIPS_X100);
            S.idle        = getint0(PS_STAT_IDLE_X100);
            S.wait        = getint0(PS_STAT_WAIT_X100);
            S.cache_used  = getint0(PS_STAT_CACHE_USED);
            S.cache_size  = getint0(PS_STAT_CACHE_SIZE);
            S.compiles    = getint0(PS_STAT_COMPILES);
            S.flushes     = getint0(PS_STAT_FLUSHES);
            S.invalidates = getint0(PS_STAT_INVALIDATES);
            S.idle_valid  = (getint0(PS_STAT_IDLE_VALID) != 0);
        }
    }

    /* --- memory: no host support needed --- */
    S.st_total = sv_phystop;

    if (sv_ramvalid == RAMVALID_MAGIC && sv_ramtop > TTRAM_BASE)
        S.tt_total = sv_ramtop - TTRAM_BASE;
    else
        S.tt_total = 0;

    /* If the emulator says TT-RAM is configured but TOS never validated
     * ramtop, believe the host - that means this TOS is not initialising Fast
     * RAM, which is worth seeing rather than hiding. */
    if (S.tt_total == 0 && psid) {
        v = getint(PS_CFG_TTRAM_SIZE);
        if (v != PS_BAD && v > 0)
            S.tt_total = v;
    }

    if (have_mxalloc) {
        v = Mxalloc(-1L, 0);                    /* ST-RAM only */
        S.st_free = (v > 0) ? v : 0;
        if (S.tt_total > 0) {
            v = Mxalloc(-1L, 1);                /* TT-RAM only */
            S.tt_free = (v > 0) ? v : 0;
        } else {
            S.tt_free = 0;
        }
    } else {
        /* TOS 1.x has only Malloc, which reports the LARGEST FREE BLOCK, not
         * total free. Walking the free list would mean allocating everything
         * and handing it back - far too intrusive for a monitor - so the UI
         * flags the difference instead of quietly showing the wrong thing. */
        v = Malloc(-1L);
        S.st_free = (v > 0) ? v : 0;
        S.tt_free = 0;
    }
    S.free_is_largest = !have_mxalloc;

#ifndef BUILD_ACC
    sample_tasks();
#endif
}

/* A cheap signature of everything on screen. Redrawing only when this moves
 * matters under cooperative GEM, where AES time is scarce and a VDI redraw is
 * not free - and it stops the window flickering twice a second on a machine
 * whose figures are not changing. */
static long signature(void)
{
    long s = S.host ? S.epoch : 0;
    s = s * 31 + S.st_free;
    s = s * 31 + S.tt_free;
#ifndef BUILD_ACC
    s = s * 31 + ntasks;
    s = s * 31 + top_task;
    if (ntasks > 0)
        s = s * 31 + tasks[0].pid + tasks[0].size;
#endif
    return s;
}

static void gather_init(void)
{
    nf_probe_name = (long)"PSCTRL";
    psid = Supexec(nf_probe);
    Supexec(read_sysvars);

    /* Detect Mxalloc rather than sniffing the TOS version: GEMDOS answers
     * EINVFN for calls it does not implement, and Mxalloc(-1,..) allocates
     * nothing, so probing is free. */
    have_mxalloc = (Mxalloc(-1L, 0) != EINVFN);
}

/* ---- drawing ------------------------------------------------------------ */

#ifndef BUILD_ACC
/* ---- task list scrolling -------------------------------------------------
 *
 * The window carries a real GEM vertical slider rather than the "... n more"
 * line it had before. That line was also a bug: it was drawn over the last
 * task row in the same pass, and being shorter than the row beneath it, the
 * tail of the previous text stayed on screen. Scrolling removes the line
 * entirely, and every row is now blank-padded so a short field can never
 * leave a fragment of a longer one behind.
 */
static int visible_task_rows(void)
{
    short wx, wy, ww, wh;
    int n;

    if (win < 0)
        return 0;
    wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
    n = (wh / ch) - (STAT_ROWS + 2);     /* header rows: "Tasks (n)" + columns */
    return (n < 0) ? 0 : n;
}

static void clamp_top(void)
{
    int vis = visible_task_rows();
    int maxtop = ntasks - vis;

    if (maxtop < 0) maxtop = 0;
    if (top_task > maxtop) top_task = maxtop;
    if (top_task < 0) top_task = 0;
}

/* GEM sliders are proportional: size and position are both 1..1000. */
static void update_slider(void)
{
    int vis = visible_task_rows();
    short size, pos;

    if (win < 0)
        return;

    if (ntasks <= vis || ntasks <= 0) {
        size = 1000;                    /* everything fits: full-height thumb */
        pos  = 0;
    } else {
        size = (short)((long)vis * 1000L / (long)ntasks);
        if (size < 1) size = 1;
        pos  = (short)((long)top_task * 1000L / (long)(ntasks - vis));
    }
    wind_set(win, WF_VSLSIZE, size, 0, 0, 0);
    wind_set(win, WF_VSLIDE, pos, 0, 0, 0);
}
#endif /* !BUILD_ACC */

static void txt(short col, short row, short wx, short wy, const char *s)
{
    v_gtext(vh, (short)(wx + col * cw), (short)(wy + row * ch), (char *)s);
}

static void bar(short col, short row, short cols, int p, short wx, short wy)
{
    short pxy[4];
    short x0 = (short)(wx + col * cw), y0 = (short)(wy + row * ch + 2);
    short w  = (short)(cols * cw),     h  = (short)(ch - 4);
    short fill;

    if (p < 0) p = 0;
    if (p > 100) p = 100;

    vsf_interior(vh, FIS_HOLLOW);
    vsf_perimeter(vh, 1);
    vsf_color(vh, 1);
    pxy[0] = x0; pxy[1] = y0; pxy[2] = (short)(x0 + w - 1); pxy[3] = (short)(y0 + h - 1);
    v_bar(vh, pxy);

    fill = (short)(((long)(w - 4) * p) / 100L);
    if (fill > 0) {
        vsf_interior(vh, FIS_SOLID);
        vsf_perimeter(vh, 0);
        pxy[0] = (short)(x0 + 2);          pxy[1] = (short)(y0 + 2);
        pxy[2] = (short)(x0 + 2 + fill - 1); pxy[3] = (short)(y0 + h - 3);
        v_bar(vh, pxy);
    }
    vsf_interior(vh, FIS_SOLID);
    vsf_perimeter(vh, 0);
}

static void kb(char *b, long bytes)
{
    if (bytes >= 1048576L) sprintf(b, "%ldM", bytes / 1048576L);
    else                   sprintf(b, "%ldK", bytes / 1024L);
}

static void contents(short wx, short wy, short wh)
{
    char l[80], a[16], b[16];
    int  p;
#ifndef BUILD_ACC
    short rows_avail = (short)(wh / ch);
#else
    (void)wh;
#endif

    txt(1, 0, wx, wy, "PiStorm Monitor");

    if (!S.host) {
        txt(1, 2, wx, wy, "CPU/JIT figures unavailable:");
        txt(1, 3, wx, wy, "this emulator has no PSCTRL NatFeat.");
        txt(1, 4, wx, wy, "Memory below is live.");
    } else {
        /* 68k instructions retired per second, not a synthesised clock speed.
         * The host counts instructions exactly; converting that to "MHz" would
         * mean asserting a cycles-per-instruction figure the emulator does not
         * actually model (it charges a flat 4), which overstates a real 68000
         * by 2-2.5x. For a speed multiplier against a stock ST, benchmark with
         * CoreMark rather than believing a number derived from a cycle count
         * nobody calibrated. */
        sprintf(l, "Speed   %ld.%02ld MIPS (68k)",
                S.mips / 100L, S.mips % 100L);
        txt(1, 2, wx, wy, l);

        /* Two decimals deliberately: once the cache is warm this legitimately
         * sits at 100.00, and the interesting movement is in the hundredths.
         * An integer percentage would just read "100%" forever. */
        p = (int)(S.hit / 100L);
        txt(1, 3, wx, wy, "JIT hit");
        bar(9, 3, 12, p, wx, wy);
        sprintf(l, "%3ld.%02ld%%", S.hit / 100L, S.hit % 100L);
        txt(22, 3, wx, wy, l);

        if (S.idle_valid) { p = (int)(S.idle / 100L); txt(1, 4, wx, wy, "IDLE"); }
        else              { p = (int)(S.wait / 100L); txt(1, 4, wx, wy, "WAIT*"); }
        bar(9, 4, 12, p, wx, wy);
        sprintf(l, "%3d%%", p); txt(22, 4, wx, wy, l);

        p = pct(S.cache_used, S.cache_size);
        txt(1, 6, wx, wy, "JIT cache");
        bar(11, 6, 10, p, wx, wy);
        sprintf(l, "%3d%%", p); txt(22, 6, wx, wy, l);

        kb(a, S.cache_used); kb(b, S.cache_size);
        sprintf(l, "  %s / %s", a, b);            txt(1, 7, wx, wy, l);
        sprintf(l, "  compiles %ld  flush %ld  inval %ld",
                S.compiles, S.flushes, S.invalidates);
        txt(1, 8, wx, wy, l);
        sprintf(l, "  interpreted %ld.%02ld%%",
                S.interp / 100L, S.interp % 100L);
        txt(1, 9, wx, wy, l);
    }

    p = pct(S.st_total - S.st_free, S.st_total);
    txt(1, 10, wx, wy, "ST RAM");
    bar(11, 10, 10, p, wx, wy);
    sprintf(l, "%3d%%", p); txt(22, 10, wx, wy, l);
    kb(a, S.st_free); kb(b, S.st_total);
    sprintf(l, "  %s free of %s", a, b); txt(1, 11, wx, wy, l);

    if (S.tt_total > 0) {
        p = pct(S.tt_total - S.tt_free, S.tt_total);
        txt(1, 12, wx, wy, "TT RAM");
        bar(11, 12, 10, p, wx, wy);
        sprintf(l, "%3d%%", p); txt(22, 12, wx, wy, l);
        kb(a, S.tt_free); kb(b, S.tt_total);
        sprintf(l, "  %s free of %s", a, b); txt(1, 13, wx, wy, l);
    } else {
        txt(1, 12, wx, wy, "TT RAM  (none configured)");
    }

#ifndef BUILD_ACC
    if (have_proc) {
        short row = STAT_ROWS;          /* first free row below the stats */
        int   vis, i;

        /* Draw only as many task rows as the window really has, starting at
         * top_task. The window is clamped to the desktop at open time, and on
         * a 640x400 ST screen that bites - the slider is how you reach the
         * rest rather than a truncation notice. */
        vis = rows_avail - (row + 2);
        if (vis < 0) vis = 0;

        sprintf(l, "Tasks (%d)", ntasks);
        txt(1, row, wx, wy, l);
        txt(1, (short)(row + 1), wx, wy, "NAME       PID    MEM  STATE");

        for (i = 0; i < vis; i++) {
            int t = top_task + i;
            if (t >= ntasks)
                break;
            kb(a, tasks[t].size);
            /* Every field blank-padded: a short state or name can then never
             * leave the tail of a longer one behind it. */
            sprintf(l, "%-8.8s  %3ld %6s  %-6s",
                    tasks[t].name, tasks[t].pid, a, state_name(tasks[t].attr));
            txt(1, (short)(row + 2 + i), wx, wy, l);
        }
    } else if (rows_avail > STAT_ROWS) {
        txt(1, STAT_ROWS, wx, wy, "No task list: this is not FreeMiNT.");
    }
#else
    if (S.free_is_largest)
        txt(1, STAT_ROWS, wx, wy, "* free = largest block (no Mxalloc)");
    else if (S.host && !S.idle_valid)
        txt(1, STAT_ROWS, wx, wy, "* WAIT is a heuristic, not true idle");
#endif
}

static void redraw(short *area)
{
    short work[4], r[4], pxy[4];

    if (win < 0) return;

    pxy[0] = pxy[1] = pxy[2] = pxy[3] = 0;

    wind_update(BEG_UPDATE);
    wind_get(win, WF_WORKXYWH, &work[0], &work[1], &work[2], &work[3]);
    wind_get(win, WF_FIRSTXYWH, &r[0], &r[1], &r[2], &r[3]);

    while (r[2] && r[3]) {
        short x = r[0], y = r[1], w = r[2], h = r[3];

        if (area) {                                   /* clip to requested */
            short x2 = (short)(x + w), y2 = (short)(y + h);
            short ax2 = (short)(area[0] + area[2]), ay2 = (short)(area[1] + area[3]);
            if (x < area[0]) x = area[0];
            if (y < area[1]) y = area[1];
            if (x2 > ax2) x2 = ax2;
            if (y2 > ay2) y2 = ay2;
            w = (short)(x2 - x); h = (short)(y2 - y);
        }

        if (w > 0 && h > 0) {
            pxy[0] = x; pxy[1] = y; pxy[2] = (short)(x + w - 1); pxy[3] = (short)(y + h - 1);
            vs_clip(vh, 1, pxy);
            vsf_interior(vh, FIS_SOLID);
            vsf_perimeter(vh, 0);
            vsf_color(vh, 0);
            v_bar(vh, pxy);
            vsf_color(vh, 1);
            contents(work[0], work[1], work[3]);
        }
        wind_get(win, WF_NEXTXYWH, &r[0], &r[1], &r[2], &r[3]);
    }

    vs_clip(vh, 0, pxy);
    wind_update(END_UPDATE);
}

/* ---- window ------------------------------------------------------------- */

static char title[] = "  PiStorm Monitor  ";

static void open_win(void)
{
    short dx, dy, dw, dh, fx, fy, fw, fh;
#ifdef BUILD_ACC
    short kind = NAME | CLOSER | MOVER;
#else
    /* The PRG carries the task list, so it gets a real vertical slider and
     * arrows. The ACC has no list and stays as lean as it was. */
    short kind = NAME | CLOSER | MOVER | VSLIDE | UPARROW | DNARROW;
#endif

    if (win >= 0) { wind_set(win, WF_TOP, 0, 0, 0, 0); return; }

    wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
    wind_calc(WC_BORDER, kind, 0, 0,
              (short)(WIN_COLS * cw), (short)(WIN_ROWS * ch), &fx, &fy, &fw, &fh);
    fx = (short)(dx + 8);
    fy = (short)(dy + 8);
    if (fw > dw) fw = dw;
    if (fh > dh) fh = dh;

    win = wind_create(kind, fx, fy, fw, fh);
    if (win < 0) return;

    wind_set(win, WF_NAME, (short)(((long)title) >> 16),
                           (short)(((long)title) & 0xFFFFL), 0, 0);
    wind_open(win, fx, fy, fw, fh);
#ifndef BUILD_ACC
    clamp_top();
    update_slider();
#endif
    last_sig = -1;
    redraw(0L);
}

static void close_win(void)
{
    if (win < 0) return;
    wind_close(win);
    wind_delete(win);
    win = -1;
}

/* ---- main --------------------------------------------------------------- */

static void tick(void)
{
    long sig;

    sample();
#ifndef BUILD_ACC
    /* The list can shrink under us - a process exits while scrolled to the
     * bottom - so re-clamp and resize the thumb before deciding to redraw. */
    clamp_top();
    update_slider();
#endif
    sig = signature();
    if (sig == last_sig)
        return;
    last_sig = sig;
    redraw(0L);
}

int main(void)
{
    short msg[8], work_in[11], work_out[57], d;
    short mx, my, mb, ks, kr, bc, ev;
    int   i, running = 1;

    apid = appl_init();
    if (apid < 0) return 1;

    vh = graf_handle(&cw, &ch, &d, &d);
    for (i = 0; i < 10; i++) work_in[i] = 1;
    work_in[10] = 2;
    v_opnvwk(work_in, &vh, work_out);
    if (vh == 0) { appl_exit(); return 1; }

    vswr_mode(vh, MD_REPLACE);
    vsf_interior(vh, FIS_SOLID);
    vsf_perimeter(vh, 0);
    vst_alignment(vh, 0, 5, &d, &d);        /* left / top - see txt() */

    gather_init();
    sample();

#ifdef BUILD_ACC
    menu_id = menu_register(apid, "  PiStorm Monitor");
#else
    open_win();
    if (win < 0) {
        form_alert(1, "[1][PSMON: no window available][ OK ]");
        v_clsvwk(vh); appl_exit(); return 1;
    }
#endif

    while (running) {
        ev = evnt_multi(MU_MESAG | MU_TIMER, 0, 0, 0,
                        0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0,
                        msg, (unsigned long)REFRESH_MS,
                        &mx, &my, &mb, &ks, &kr, &bc);

        if (ev & MU_MESAG) {
            switch (msg[0]) {
            case WM_REDRAW:
                if (msg[3] == win) redraw(&msg[4]);
                break;
            case WM_TOPPED:
                if (msg[3] == win) wind_set(win, WF_TOP, 0, 0, 0, 0);
                break;
            case WM_MOVED:
                if (msg[3] == win) {
                    wind_set(win, WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
                    redraw(0L);
                }
                break;
#ifndef BUILD_ACC
            case WM_ARROWED:
                if (msg[3] == win) {
                    int vis = visible_task_rows();
                    switch (msg[4]) {
                    case WA_UPLINE: top_task -= 1;   break;
                    case WA_DNLINE: top_task += 1;   break;
                    case WA_UPPAGE: top_task -= vis; break;
                    case WA_DNPAGE: top_task += vis; break;
                    default: break;
                    }
                    clamp_top();
                    update_slider();
                    last_sig = -1;      /* scrolling is a change the sampler
                                         * cannot see, so force the redraw */
                    redraw(0L);
                }
                break;
            case WM_VSLID:
                if (msg[3] == win) {
                    int vis = visible_task_rows();
                    if (ntasks > vis)
                        top_task = (int)(((long)msg[4] * (long)(ntasks - vis)
                                          + 500L) / 1000L);
                    else
                        top_task = 0;
                    clamp_top();
                    update_slider();
                    last_sig = -1;
                    redraw(0L);
                }
                break;
#endif
            case WM_CLOSED:
                if (msg[3] == win) {
#ifdef BUILD_ACC
                    close_win();            /* an accessory never exits */
#else
                    running = 0;
#endif
                }
                break;
#ifdef BUILD_ACC
            case AC_OPEN:
                if (msg[4] == menu_id) open_win();
                break;
            case AC_CLOSE:
                if (msg[3] == menu_id) win = -1;   /* AES tore it down */
                break;
#endif
            default:
                break;
            }
        }

        if (ev & MU_TIMER) tick();
    }

    close_win();
    v_clsvwk(vh);
    appl_exit();
    return 0;
}
