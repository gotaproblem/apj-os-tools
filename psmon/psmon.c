/*
 * psmon.c - PSMON.ACC / PSMON.PRG
 *
 * PiSTorm Monitor: the live JIT engine figures, guest memory, and the
 * state of the Pi underneath, in one skinned window.
 *
 *     make              -> PSMON.ACC and PSMON.PRG
 *
 * WHAT IT SHOWS is the taskbar's JIT panel (teradesk pstask.c, mn_*):
 * the same six engine figures from the same PSCTRL status indices, and
 * the same two memory gauges. That panel is the reference; this is the
 * standalone window version of it, drawn from an APJSKIN sheet, for a
 * desktop that is not TeraDesk or a screen where the taskbar is not
 * where you want to be looking. It adds one block the taskbar has no
 * room for: the Pi's own clock, temperature and firmware throttle bits,
 * because a board that is capped makes every figure above it worse and
 * nothing else on the Atari side can say so.
 *
 * WHAT IT NO LONGER SHOWS is the u:\proc task list. The XaAES task
 * manager does that job properly and the JIT panel dropped it for the
 * same reason.
 *
 * The old PSMON asked the host for indices 33..47 - MHZ_X100,
 * HITRATE_X100, SPEEDX_X100 and the rest - and the emulator retired most
 * of them. Worse, it drew a missing index as 0, so a stale binary showed
 * a confident 0.00 MHz. Every reading here can be PM_NONE and every one
 * says "n/a" instead.
 *
 * The drawing is all in psmonui.c, which builds on a Linux host against
 * a fake VDI - see tests/psmon/. Everything in this file is GEM and the
 * NatFeat, and it is deliberately the same shell as psctrl.c, including
 * every repaint rule that file learned the hard way:
 *
 *   - the 500 ms poll compares the RENDERED TEXT, not the numbers, so a
 *     figure that moves without changing its string costs nothing;
 *   - it repaints the one section whose text changed, not the window;
 *   - the screen lock and the pointer hide are separated, and the
 *     pointer is only hidden when it is actually over what is being
 *     drawn. graf_mouse(M_OFF) repaints whatever is under the cursor,
 *     which is how a window like this made the TASKBAR flicker.
 */

#include <gem.h>
#include <osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APJGUI_IMPL
#include "../apjgui/apjgui.h"
#include "../apjgui/apjskin.h"
#include "psmonui.h"

/* ---- NatFeat stubs -------------------------------------------------------
 *
 * Same shape and the same probe as psctrl.c: $7300 is an illegal
 * instruction on a real 68000, so the probe installs a temporary
 * vector-4 handler and steps over the opcode if it traps.
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
"   .globl _nf_probe\n"
"_nf_probe:\n"
"   movem.l %d2/%a2,-(%sp)\n"
"   move.l 0x10:w,%d2\n"
"   move.l #_nf_trap_handler,0x10:w\n"
"   clr.l _nf_probe_trapped\n"
"   move.l _nf_probe_name,-(%sp)\n"
"   jsr _nf_id\n"
"   addq.l #4,%sp\n"
"   tst.l _nf_probe_trapped\n"
"   beq 1f\n"
"   moveq #0,%d0\n"
"1:\n"
"   move.l %d2,0x10:w\n"
"   movem.l (%sp)+,%d2/%a2\n"
"   rts\n"
"_nf_trap_handler:\n"
"   move.l #1,_nf_probe_trapped\n"
"   addq.l #2,2(%sp)\n"
"   moveq #0,%d0\n"
"   rte\n"
);

extern long nf_id(const char *name);
extern long nf_call(long id, ...);
extern long nf_probe(void);

long nf_probe_name = 0;
long nf_probe_trapped = 0;

/* ---- the status indices, as platforms/atari/psctrl/psctrl.h defines
 * them. Only GETINT is used: PSMON reads, it does not set anything. --- */

#define NF_PS_GETINT		1L

#define PS_CFG_TTRAM_SIZE	4L
#define PS_STAT_CACHE_USED	39L
#define PS_STAT_CACHE_TOTAL	40L
#define PS_STAT_COMPILES	41L
#define PS_HOST_SOC_TEMP_MC	64L
#define PS_HOST_ARM_FREQ_KHZ	65L
#define PS_HOST_LOADAVG_X100	66L
#define PS_HOST_THROTTLED	70L
#define PS_JIT_EFF_KHZ		71L
#define PS_JIT_HITRATE_X10	72L
#define PS_JIT_IDLE_X10		73L
#define PS_PI_MODEL		74L
#define PS_PI_RAM_MB		75L
#define PS_STAT_FLUSHES_TOTAL	76L
#define PS_STAT_SMC_INV		77L

#define PS_BAD			(-1L)

#define RAMVALID_MAGIC		0x1357BD13L
#define TTRAM_BASE		0x01000000L
#define EINVFN			(-32L)

#define APJ_SKINCHG_MSG		APJ_SKINCHG

#define POLL_MS			500UL

/* ---- state -------------------------------------------------------------- */

static long  psid;
static short vh, win = -1;
static short cw, ch;
static short wx, wy, ww, wh;
static short iconified = 0;
static short apid;
#ifdef BUILD_ACC
static short menu_id = -1;
#endif

static PMDATA data;
static PMUI   ui;
static char   status[160] = "";

/* what each section currently has on the screen, line for line */
static char shown[PM_SEC_N][PM_MAXLINE][64];
static short nshown[PM_SEC_N];

static long sv_phystop, sv_ramtop, sv_ramvalid;
static short have_mxalloc = -1;

#define WIN_KIND	(NAME | CLOSER | MOVER | SMALLER)

/* ---- sampling ------------------------------------------------------------ */

/* Runs in supervisor mode through Supexec. Reading absolute low memory
 * is the point; GCC 12 sees a dereference near address zero and warns. */
#if defined(__GNUC__) && __GNUC__ >= 12
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Warray-bounds"
#endif
static long read_sysvars(void)
{
	sv_phystop  = *(volatile long *) 0x42EL;
	sv_ramtop   = *(volatile long *) 0x5A4L;
	sv_ramvalid = *(volatile long *) 0x5A8L;
	return 0;
}
#if defined(__GNUC__) && __GNUC__ >= 12
# pragma GCC diagnostic pop
#endif

/*
 * One status index. The host answers 0xFFFFFFFF for an index it does not
 * know, which arrives here as -1; that is PM_NONE and the row draws
 * "n/a". Never turn it into 0 - see the note at the top of this file.
 */
static long ps(long idx)
{
	if (!psid)
		return PM_NONE;
	return nf_call(psid | NF_PS_GETINT, idx);
}

static void sample(void)
{
	long v;

	if (have_mxalloc < 0)
		have_mxalloc = (Mxalloc(-1L, 0) != EINVFN) ? 1 : 0;

	Supexec(read_sysvars);
	data.st_total = sv_phystop;

	if (sv_ramvalid == RAMVALID_MAGIC && sv_ramtop > TTRAM_BASE)
		data.tt_total = sv_ramtop - TTRAM_BASE;
	else
		data.tt_total = 0;

	/*
	 * If TOS did not validate ramtop, believe the emulator's configured
	 * size. Seeing "0 free of 128.0 MB" is worth knowing: it means a TOS
	 * that is not initialising Fast RAM. (The taskbar's JIT panel does
	 * the same, for the same reason.)
	 */
	if (data.tt_total == 0)
	{
		v = ps(PS_CFG_TTRAM_SIZE);
		if (v > 0)
			data.tt_total = v;
	}

	if (have_mxalloc)
	{
		v = Mxalloc(-1L, 0);
		data.st_free = (v > 0) ? v : 0;
		v = (data.tt_total > 0) ? Mxalloc(-1L, 1) : 0;
		data.tt_free = (v > 0) ? v : 0;
	}
	else
	{
		v = Malloc(-1L);		/* TOS 1.x: largest free block */
		data.st_free = (v > 0) ? v : 0;
		data.tt_free = 0;
	}

	data.khz         = ps(PS_JIT_EFF_KHZ);
	data.hit_x10     = ps(PS_JIT_HITRATE_X10);
	data.idle_x10    = ps(PS_JIT_IDLE_X10);
	data.cache_used  = ps(PS_STAT_CACHE_USED);
	data.cache_total = ps(PS_STAT_CACHE_TOTAL);
	data.flushes     = ps(PS_STAT_FLUSHES_TOTAL);
	data.compiles    = ps(PS_STAT_COMPILES);
	data.smc         = ps(PS_STAT_SMC_INV);

	data.soc_mc      = ps(PS_HOST_SOC_TEMP_MC);
	data.arm_khz     = ps(PS_HOST_ARM_FREQ_KHZ);
	data.load_x100   = ps(PS_HOST_LOADAVG_X100);
	data.throttled   = ps(PS_HOST_THROTTLED);
	data.pi_model    = ps(PS_PI_MODEL);
	data.pi_ram_mb   = ps(PS_PI_RAM_MB);
}

/* ---- drawing ------------------------------------------------------------- */

static void draw_all(const GRECT *c)   { psmonui_draw_clip(&ui, vh, c); }

static short one_sec;
static void draw_one(const GRECT *c)
{
	(void) c;
	psmonui_draw_sec(&ui, vh, one_sec);
}

static void draw_iconic(const GRECT *c)
{
	(void) c;
	apj_fill(vh, wx, wy, ww, wh, apj_pen(APJ_R_PANEL));
}

/*
 * wind_update(BEG_UPDATE) is a screen-wide semaphore and graf_mouse(M_OFF)
 * repaints whatever is under the pointer - the taskbar, if that is where
 * it is resting. Neither is local to this window, and a monitor redrawing
 * twice a second is exactly the program that makes the rest of the
 * desktop flicker. So: the lock is taken once per change however many
 * rectangles it covers, and the pointer is hidden only when it is over
 * one of them.
 */
static short mouse_off = 0;

static void redraw_begin(const GRECT *area)
{
	short mx, my, mb, ks;

	wind_update(BEG_UPDATE);
	mouse_off = 1;
	if (area)
	{
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

static void redraw_at(void (*fn)(const GRECT *), short rx, short ry,
                      short rw, short rh)
{
	GRECT r, d;

	d.g_x = rx; d.g_y = ry; d.g_w = rw; d.g_h = rh;
	if (rw <= 0 || rh <= 0)
		return;
	if (iconified)
		fn = draw_iconic;

	wind_get(win, WF_FIRSTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
	while (r.g_w && r.g_h)
	{
		GRECT i = r;

		if (rc_intersect(&d, &i))
		{
			apj_clip(vh, i.g_x, i.g_y, i.g_w, i.g_h);
			fn(&i);
		}
		wind_get(win, WF_NEXTXYWH, &r.g_x, &r.g_y, &r.g_w, &r.g_h);
	}
}

static void redraw(void (*fn)(const GRECT *), short rx, short ry,
                   short rw, short rh)
{
	GRECT d;

	if (win < 0)
		return;
	d.g_x = rx; d.g_y = ry; d.g_w = rw; d.g_h = rh;
	redraw_begin(&d);
	redraw_at(fn, rx, ry, rw, rh);
	redraw_end();
}

static void relayout(void)
{
	wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
	psmonui_layout(&ui, vh, wx, wy, ww, wh);
}

/*
 * The poll. Compare the RENDERED LINES, not the readings: the hit rate
 * moves every window and the cache figure in KB does not move for
 * minutes, and repainting for a number that draws the same string is the
 * whole of the flicker. Repaint the sections whose text changed, under
 * one screen lock.
 */
static void refresh(void)
{
	char now[PM_MAXLINE][64];
	short dirty[PM_SEC_N], nd = 0, s, i, n;
	GRECT all;
	short got = 0;

	sample();

	for (s = 0; s < PM_SEC_N; s++)
	{
		short changed = 0;

		n = psmonui_sec_lines(&ui, s, now, PM_MAXLINE);
		if (n != nshown[s])
			changed = 1;
		for (i = 0; i < n && !changed; i++)
			if (strcmp(now[i], shown[s][i]))
				changed = 1;
		if (!changed)
			continue;
		for (i = 0; i < n; i++)
			strcpy(shown[s][i], now[i]);
		nshown[s] = n;
		dirty[nd++] = s;
	}
	if (!nd || win < 0 || iconified)
		return;

	for (i = 0; i < nd; i++)
	{
		GRECT r;

		if (!psmonui_sec_rect(&ui, dirty[i], &r))
			continue;
		if (!got)
		{
			all = r;
			got = 1;
		}
		else
		{
			short x1 = (short) (all.g_x + all.g_w);
			short y1 = (short) (all.g_y + all.g_h);

			if (r.g_x < all.g_x) all.g_x = r.g_x;
			if (r.g_y < all.g_y) all.g_y = r.g_y;
			if (r.g_x + r.g_w > x1) x1 = (short) (r.g_x + r.g_w);
			if (r.g_y + r.g_h > y1) y1 = (short) (r.g_y + r.g_h);
			all.g_w = (short) (x1 - all.g_x);
			all.g_h = (short) (y1 - all.g_y);
		}
	}
	if (!got)
		return;

	redraw_begin(&all);
	for (i = 0; i < nd; i++)
	{
		GRECT r;

		if (!psmonui_sec_rect(&ui, dirty[i], &r))
			continue;
		one_sec = dirty[i];
		redraw_at(draw_one, r.g_x, r.g_y, r.g_w, r.g_h);
	}
	redraw_end();
}

/* what is on the screen right now, so the first poll is not a full repaint */
static void seed_shown(void)
{
	short s;

	for (s = 0; s < PM_SEC_N; s++)
		nshown[s] = psmonui_sec_lines(&ui, s, shown[s], PM_MAXLINE);
}

/* ---- window -------------------------------------------------------------- */

static void natural_size(short *w, short *h)
{
	short dx, dy, dw, dh, mw, mh;

	*w = apj_skin_ok() ? apj_skin_m(PM_W_PT) : (short) (46 * cw);
	*h = apj_skin_ok() ? apj_skin_m(PM_H_PT) : (short) (16 * ch);
	psmonui_minsize(&mw, &mh);
	if (*w < mw) *w = mw;
	if (*h < mh) *h = mh;
	wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
	if (*w > dw) *w = dw;
	if (*h > dh) *h = dh;
}

/*
 * An accessory is loaded at BOOT, before the desktop has committed its
 * theme, so the answer to appl_control(115) in main() is not what
 * TeraDesk is about to set. Asking once at startup is how PSCTRL's
 * window came up in Fluent Dark on a Fuji desktop. The theme is
 * re-checked every time the window opens, and the sheet reloaded only if
 * the nineteen role colours actually moved.
 */
static long skin_theme_sig = -1;

static long theme_sig(void)
{
	long s = 0;
	short i;

	for (i = 0; i < APJ_R_N; i++)
		s = s * 31L + apj_rgb[i];
	return s;
}

static void check_theme(void)
{
	apj_init(vh);
	if (theme_sig() == skin_theme_sig && apj_skin_ok())
		return;
	apj_skin_reload(vh);
	skin_theme_sig = theme_sig();
	if (win >= 0)
	{
		relayout();
		if (!iconified)
			redraw(draw_all, wx, wy, ww, wh);
	}
}

static char wtitle[] = "  PiSTorm Monitor  ";

static void open_win(void)
{
	short dx, dy, dw, dh, cx, cy, cwid, chgt, want_w, want_h;

	check_theme();
	if (win >= 0)
	{
		wind_set(win, WF_TOP, 0, 0, 0, 0);
		return;
	}
	natural_size(&want_w, &want_h);
	wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
	wind_calc(WC_BORDER, WIN_KIND, dx + 16, dy + 16, want_w, want_h,
	          &cx, &cy, &cwid, &chgt);
	win = wind_create(WIN_KIND, cx, cy, cwid, chgt);
	if (win < 0)
		return;
	wind_set(win, WF_NAME, (short) (((long) wtitle) >> 16),
	                       (short) (((long) wtitle) & 0xFFFFL), 0, 0);
	wind_open(win, cx, cy, cwid, chgt);
	iconified = 0;
	relayout();
	sample();
	seed_shown();
	redraw(draw_all, wx, wy, ww, wh);
}

static void close_win(void)
{
	if (win < 0)
		return;
	wind_close(win);
	wind_delete(win);
	win = -1;
}

/* ---- PSMON.INF ----------------------------------------------------------- */

/*
 * Where PSMON.INF lives: the root of the boot device, for BOTH builds.
 * The .PRG once derived its folder from shel_read() (apj_skin_progdir),
 * but PSMON is launched from the PiSTorm menu bar, not double-clicked,
 * and then shel_read() returns the launcher's path, not PSMON's - so the
 * file was written and read in inconsistent places and the saved scale
 * never stuck. _bootdev at $446 is launch-independent and is where
 * PSCTRL.INF and TERADESK.INF already live, so PSMON.INF joins them.
 *
 *     scale=125
 *     skins=S:\APJ-OS\NATFEATS\SKINS
 */

# if defined(__GNUC__) && __GNUC__ >= 12
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Warray-bounds"
# endif
static long read_bootdev(void)
{
	return (long) *(volatile short *) 0x446L;
}
# if defined(__GNUC__) && __GNUC__ >= 12
#  pragma GCC diagnostic pop
# endif

static void inf_path(char *out, long n)
{
	long d = Supexec(read_bootdev);

	(void) n;
	if (d < 0 || d > 25)
		d = 2;				/* C: */
	sprintf(out, "%c:\\PSMON.INF", (int) ('A' + d));
}


static char inf_buf[256];

static const char *inf_read(void)
{
	char path[224];
	long fh, got;

	inf_buf[0] = '\0';
	inf_path(path, (long) sizeof path);
	fh = Fopen(path, 0);
	if (fh < 0)
		return inf_buf;
	got = Fread((short) fh, (long) sizeof inf_buf - 1, inf_buf);
	Fclose((short) fh);
	if (got <= 0)
		got = 0;
	inf_buf[got] = '\0';
	return inf_buf;
}

static short inf_scale(void)
{
	const char *p = strstr(inf_read(), "scale=");

	return p ? (short) atoi(p + 6) : 0;
}

static void inf_skindir(char *out, long n)
{
	const char *p = strstr(inf_read(), "skins=");
	long i = 0;

	out[0] = '\0';
	if (!p)
		return;
	p += 6;
	while (*p == ' ' || *p == '\t')
		p++;
	while (*p && *p != '\r' && *p != '\n' && i < n - 1)
		out[i++] = *p++;
	while (i > 0 && out[i - 1] == ' ')
		i--;
	out[i] = '\0';
}

static void inf_save(short scale)
{
	char path[224], buf[200], dir[160];
	long fh;

	inf_path(path, (long) sizeof path);
	inf_skindir(dir, (long) sizeof dir);	/* before Fcreate truncates it */
	fh = Fcreate(path, 0);
	if (fh < 0)
		return;
	if (dir[0])
		sprintf(buf, "scale=%d\r\nskins=%.120s\r\n", (int) scale, dir);
	else
		sprintf(buf, "scale=%d\r\n", (int) scale);
	Fwrite((short) fh, (long) strlen(buf), buf);
	Fclose((short) fh);
}

static void set_scale(short scale)
{
	short x, y, w, h, cx, cy, cw2, ch2, nw, nh;

	if (win < 0)
		return;
	if (scale == apj_skin_preferred() && apj_skin_ok())
		return;
	apj_skin_prefer(scale);
	apj_skin_reload(vh);
	inf_save(scale);

	wind_get(win, WF_CURRXYWH, &x, &y, &w, &h);
	natural_size(&nw, &nh);
	wind_calc(WC_BORDER, WIN_KIND, wx, wy, nw, nh, &cx, &cy, &cw2, &ch2);
	wind_set(win, WF_CURRXYWH, x, y, cw2, ch2);
	relayout();
	if (!iconified)
		redraw(draw_all, wx, wy, ww, wh);
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
	short work_in[11], work_out[57];
	short d, msg[8];
	short mx, my, mb, ks, kr, brk;
	short ev, i;

	apid = appl_init();
	if (apid < 0)
		return 1;

	nf_probe_name = (long) "PSCTRL";
	psid = Supexec(nf_probe);

	vh = graf_handle(&cw, &ch, &d, &d);
	for (i = 0; i < 10; i++)
		work_in[i] = 1;
	work_in[10] = 2;
	v_opnvwk(work_in, &vh, work_out);
	if (vh == 0)
	{
		appl_exit();
		return 1;
	}
	vswr_mode(vh, MD_REPLACE);
	vsf_interior(vh, FIS_SOLID);
	vsf_perimeter(vh, 0);
	vst_alignment(vh, 0, 5, &d, &d);
	apj_init(vh);
	{
		char dir[160];

		inf_skindir(dir, (long) sizeof dir);
		if (dir[0])
			apj_skin_setdir(dir);
	}
	apj_skin_prefer(inf_scale());
#ifndef BUILD_ACC
	apj_skin_load(vh, NULL);
	skin_theme_sig = theme_sig();
#endif
	/* the .ACC deliberately does not load the sheet here: at boot the
	 * theme is not settled, so this would pick the wrong one and pay a
	 * couple of megabytes of file read to find out. check_theme() does
	 * it when the window first opens. */

	memset(&data, 0, sizeof(data));
	memset(&ui, 0, sizeof(ui));
	ui.d = &data;
	ui.status = status;

	data.host = psid ? 1 : 0;
	if (!psid)
	{
		strcpy(status, "No PSCTRL NatFeat: engine and Pi figures are "
		               "unavailable.");
		data.khz = data.hit_x10 = data.idle_x10 = PM_NONE;
		data.cache_used = data.cache_total = PM_NONE;
		data.flushes = data.compiles = data.smc = PM_NONE;
		data.soc_mc = data.arm_khz = data.load_x100 = PM_NONE;
		data.throttled = data.pi_model = data.pi_ram_mb = PM_NONE;
	}
	else
		strcpy(status, "Sampling every 500 ms.  1/2/3 scale, 0 auto.");

	sample();
	seed_shown();

#ifdef BUILD_ACC
	menu_id = menu_register(apid, "  PiSTorm Monitor");
#else
	open_win();
	if (win < 0)
	{
		form_alert(1, "[1][PSMON: no window available][ OK ]");
		v_clsvwk(vh);
		appl_exit();
		return 1;
	}
#endif

	for (;;)
	{
		ev = evnt_multi(MU_MESAG | MU_BUTTON | MU_KEYBD | MU_TIMER,
		                1, 1, 1,
		                0, 0, 0, 0, 0,
		                0, 0, 0, 0, 0,
		                msg, POLL_MS,
		                &mx, &my, &mb, &ks, &kr, &brk);

		if (ev & MU_MESAG)
		{
			switch (msg[0])
			{
			case WM_REDRAW:
				if (msg[3] == win)
					redraw(draw_all, msg[4], msg[5], msg[6], msg[7]);
				break;
			case WM_TOPPED:
			case WM_NEWTOP:
				if (msg[3] == win)
					wind_set(win, WF_TOP, 0, 0, 0, 0);
				break;
			case WM_MOVED:
			case WM_SIZED:
				if (msg[3] == win)
				{
					wind_set(win, WF_CURRXYWH,
					         msg[4], msg[5], msg[6], msg[7]);
					relayout();
					/*
					 * A move needs no repaint here: the AES relocates
					 * the window's pixels and sends WM_REDRAW for
					 * whatever is newly exposed. Painting the whole
					 * window as well - on every WM_MOVED of a live
					 * drag - is the second paint the eye sees as
					 * flicker. Only a size change re-lays-out the
					 * rows that are already on screen. (PSCTRL has
					 * always done this; PSMON did not.)
					 */
					if (msg[0] == WM_SIZED && !iconified)
						redraw(draw_all, wx, wy, ww, wh);
				}
				break;
			case WM_ICONIFY:
				if (msg[3] == win)
				{
					wind_set(win, WF_ICONIFY,
					         msg[4], msg[5], msg[6], msg[7]);
					iconified = 1;
					relayout();
				}
				break;
			case WM_UNICONIFY:
				if (msg[3] == win)
				{
					wind_set(win, WF_UNICONIFY,
					         msg[4], msg[5], msg[6], msg[7]);
					iconified = 0;
					relayout();
					redraw(draw_all, wx, wy, ww, wh);
				}
				break;
			case WM_CLOSED:
				if (msg[3] == win)
				{
#ifdef BUILD_ACC
					close_win();	/* an accessory never exits */
#else
					goto out;
#endif
				}
				break;
			case APJ_SKINCHG_MSG:
				skin_theme_sig = -1;
				check_theme();
				break;
#ifdef BUILD_ACC
			case AC_OPEN:
				if (msg[4] == menu_id)
					open_win();
				break;
			case AC_CLOSE:
				if (msg[3] == menu_id)
					win = -1;	/* the AES closed it for us */
				break;
#endif
			default:
				break;
			}
		}

		if ((ev & MU_KEYBD) && win >= 0)
		{
			char  c = (char) (kr & 0xff);

			if      (c == '1') set_scale(100);
			else if (c == '2') set_scale(125);
			else if (c == '3') set_scale(175);
			else if (c == '0') set_scale(0);
			else if (c == 0x1b || c == 'q' || c == 'Q')
			{
#ifdef BUILD_ACC
				close_win();
#else
				goto out;
#endif
			}
		}

		if ((ev & MU_TIMER) && win >= 0 && !iconified)
			refresh();
	}

#ifndef BUILD_ACC
out:
	close_win();
	apj_skin_free();
	v_clsvwk(vh);
	appl_exit();
	return 0;
#endif
}
