/*
 * psctrl.c - PSCTRL.ACC / PSCTRL.PRG
 *
 * PiSTorm settings: every configurable switch and tunable the emulator
 * has, in one skinned window, changing the RUNNING machine through the
 * PSCTRL NatFeat.
 *
 * It is an accessory first. An .ACC sits in the drop-down of every
 * application under TOS and XaAES and can be opened while a game or the
 * ST Box is running, which is the whole point: jit_power is worth moving
 * against a game you are actually playing, not between runs. The same
 * source builds a .PRG for the desktop.
 *
 *     make              -> PSCTRL.ACC and PSCTRL.PRG
 *
 * NOTHING about the controls is compiled in here. The emulator ships a
 * descriptor table and this program builds its tabs from it (PS_COUNT,
 * PS_DESCRIBE), so adding a switch to the emulator is one line there and
 * no rebuild of this. The window is drawn entirely out of an APJSKIN
 * sheet by psui.c; without one it falls back to flat apjgui controls and
 * says which file it wanted.
 *
 * Every control carries a badge saying how it applies - live, deferred,
 * restart - and the badge is not decoration: PS_SETINT enforces the
 * class, and a value that needs a restart is written to a shadow config
 * rather than pretending to have changed under you.
 */

#include <gem.h>
#include <osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APJGUI_IMPL
#include "../apjgui/apjgui.h"
#include "../apjgui/apjskin.h"
#include "psui.h"
#include "psbench.h"
#include "ps3d.h"

/* ---- NatFeat stubs -------------------------------------------------------
 *
 * Same shape as psmon.c, including its probe: $7300 is an illegal
 * instruction on a real 68000, so the probe installs a temporary vector-4
 * handler and steps over the opcode if it traps. This binary will only
 * ever run on a PiSTorm, but an accessory that crashes a bare ST when it
 * loads is a rude thing to leave lying about.
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

/* ---- the protocol, as psctrl_settings.h defines it ---------------------- */

#define NF_PS_VERSION	0L
#define NF_PS_GETINT	1L
#define NF_PS_GETSTR	3L
#define NF_PS_SETINT	4L
#define NF_PS_COUNT	5L
#define NF_PS_DESCRIBE	6L
#define NF_PS_SAVE	7L
#define NF_PS_SETSTR	8L
#define NF_PS_ACTION	9L
#define NF_PS_LIST	10L
#define NF_PS_SETAPI	11L

#define PS_SET_BASE	256L
#define PS_STAT_FLUSHES_TOTAL 76L
#define PS_CFG_CPU_MODEL 2L
#define PS_CFG_FPU_MODEL 3L
#define PS_DESC_FIXED	48

#define PS_LIST_FLOPPY	0L
#define PS_LIST_TOS	1L

#define APJ_SKINCHG_MSG	APJ_SKINCHG

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

static PSROW  rows[PS_MAXROW];
static PSUI   ui;
static char   status[160] = "";
static GRECT  m1r;
static short  m1flag = MO_ENTER;

static PSBENCH bench, bprev;
static short   have_prev = 0;


/* the floppy / image picker, filled from PS_LIST on demand */
#define PICK_MAX	64
static char  pick[PICK_MAX][48];
static short npick = 0;
static short picktop = 0;

/* ---- descriptor ---------------------------------------------------------- */

static long be32(const unsigned char *p)
{
	return ((long) p[0] << 24) | ((long) p[1] << 16) |
	       ((long) p[2] << 8) | (long) p[3];
}

static short be16(const unsigned char *p)
{
	return (short) (((unsigned short) p[0] << 8) | p[1]);
}

/*
 * One descriptor into one row. The record is fixed-size then a run of
 * NUL-terminated strings: nenum labels, then the title. Read byte by
 * byte rather than overlaying a struct - the guest and the host agree on
 * the layout, not on anybody's padding rules.
 */
static void parse_desc(PSROW *r, const unsigned char *b, long n)
{
	const char *p;
	long off;
	short i;

	memset(r, 0, sizeof(*r));
	memcpy(r->name, b, 23);
	r->name[23] = '\0';
	r->tab   = b[24];
	r->kind  = b[25];
	r->klass = b[26];
	r->nenum = b[27];
	r->min   = be32(b + 28);
	r->max   = be32(b + 32);
	r->step  = be32(b + 36);
	r->unit  = be16(b + 40);
	r->flags = be16(b + 42);
	r->value = be32(b + 44);

	if (r->nenum > PS_MAXENUM)
		r->nenum = PS_MAXENUM;
	if (r->step < 1)
		r->step = 1;

	off = PS_DESC_FIXED;
	for (i = 0; i < r->nenum && off < n; i++)
	{
		p = (const char *) b + off;
		strncpy(r->label[i], p, sizeof(r->label[0]) - 1);
		r->label[i][sizeof(r->label[0]) - 1] = '\0';
		off += (long) strlen(p) + 1;
	}
	if (off < n)
	{
		p = (const char *) b + off;
		strncpy(r->title, p, sizeof(r->title) - 1);
		r->title[sizeof(r->title) - 1] = '\0';
	}
	if (!r->title[0])
		strcpy(r->title, r->name);
	if (r->tab >= PS_TAB_N)
		r->tab = PS_TAB_ADV;
}

static void read_str(short i)
{
	char buf[PS_STRLEN];

	buf[0] = '\0';
	nf_call(psid | NF_PS_GETSTR, (long) i, buf, (long) sizeof(buf));
	buf[sizeof(buf) - 1] = '\0';
	strcpy(rows[i].str, buf);
}

static short load_model(void)
{
	unsigned char buf[512];
	long n, i;

	n = nf_call(psid | NF_PS_COUNT);
	if (n <= 0 || n > PS_MAXROW)
	{
		if (n > PS_MAXROW)
			n = PS_MAXROW;
		else
			return 0;
	}
	for (i = 0; i < n; i++)
	{
		long got = nf_call(psid | NF_PS_DESCRIBE, i, buf, (long) sizeof(buf));

		if (got <= 0)
		{
			n = i;
			break;
		}
		parse_desc(&rows[i], buf, got);
		if (rows[i].kind == PS_K_STR)
			read_str((short) i);
	}
	ui.nrows = (short) n;
	return (short) n;
}

/*
 * Re-read the values of the rows on screen. Only those: the whole table
 * is 90-odd NatFeat traps and this runs twice a second, and a trap that
 * nobody can see the result of is pure cost. The readouts on the JIT and
 * CPU tabs are the reason it happens at all.
 *
 * What comes back is compared as TEXT, not as a number. A hit rate that
 * moves from 973 to 974 tenths still reads "97.3%", and a cache figure in
 * KB sits still for minutes while the byte count underneath it never
 * stops moving. Repainting for those is the flicker: the row's ground is
 * refilled and the AES redraws the string on top of it, twice a second,
 * for a line that did not change. Rows whose string is the same are left
 * alone, and the ones that did change have their own band redrawn rather
 * than the whole list.
 *
 * Fills dirty[] with the visible slots that need it and returns how many.
 */
static short refresh_visible(short *dirty)
{
	short v, n = 0;

	for (v = 0; v < ui.visrows && ui.top + v < ui.nidx; v++)
	{
		short i = ui.idx[ui.top + v];
		char now[32];
		long got;

		if (rows[i].kind == PS_K_STR)
			continue;		/* only re-read on demand */
		got = nf_call(psid | NF_PS_GETINT, PS_SET_BASE + i);
		if (got == -1L)
			continue;
		rows[i].value = got;
		/* format every time rather than short-circuiting on the number:
		 * shown[] is then the one truth about what is on the screen,
		 * and it stays right after the user edits a row too */
		psui_row_text(&rows[i], got, now, (short) sizeof(now));
		if (!strcmp(now, rows[i].shown))
			continue;		/* moved, but not visibly */
		strcpy(rows[i].shown, now);
		dirty[n++] = v;
	}
	return n;
}

/* what the row list currently has on screen, so the first poll after a
 * draw does not count every row as changed */
static void seed_shown(void)
{
	short i;

	for (i = 0; i < ui.nrows; i++)
		psui_row_text(&rows[i], rows[i].value, rows[i].shown,
		              (short) sizeof(rows[i].shown));
}

/* ---- drawing ------------------------------------------------------------- */

static void draw_all(const GRECT *c)    { psui_draw_clip(&ui, vh, c); }
static void draw_rows(const GRECT *c)   { (void) c; psui_draw_rows(&ui, vh); }
static void draw_dropbox(const GRECT *c){ (void) c; psui_draw_drop(&ui, vh); }
static void draw_tabs(const GRECT *c)   { (void) c; psui_draw_tabs(&ui, vh); }
static void draw_stat(const GRECT *c)   { (void) c; psui_draw_status(&ui, vh); }
static void draw_bhead(const GRECT *c)  { (void) c; psui_draw_bench_head(&ui, vh); }

static void draw_iconic(const GRECT *c)
{
	(void) c;
	apj_fill(vh, wx, wy, ww, wh, apj_pen(APJ_R_PANEL));
}

/*
 * wind_update(BEG_UPDATE) is a screen-wide semaphore and graf_mouse(M_OFF)
 * hides the pointer everywhere, so a pair of them is not free and it is
 * not local to this window - taking one thirty times a second is what
 * makes the desktop and the taskbar flicker, not just the dialog. So the
 * lock is separated from the drawing: redraw_at() paints one rectangle,
 * and a caller with several to paint takes the lock once around all of
 * them.
 */
/*
 * graf_mouse(M_OFF) is not local to this window either: the pointer is
 * drawn into the framebuffer with a save-under, so hiding and showing it
 * repaints whatever is beneath it - the taskbar, if that is where the
 * pointer happens to be resting. The two-a-second poll was doing that
 * wherever the pointer was, which is why the TASKBAR flickered while the
 * pointer sat on it and this window quietly updated a readout.
 *
 * So it is hidden only when it is actually over what is about to be
 * drawn. Pass the bounding box of the painting; NULL means "assume it is
 * in the way", which is right for a hover repaint, where the pointer is
 * by definition on the widget being redrawn.
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

static void redraw_drop(short vis);	/* defined after the draw callbacks */
static void redraw_dropbox(short vis);

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

static void redraw_rows(void)
{
	redraw(draw_rows, ui.listr.g_x, ui.listr.g_y,
	       ui.listr.g_w, ui.listr.g_h);
}

/* one visible row, for the poll. Kept separate from redraw_rows() so the
 * common case - one readout moved - costs one band and not the tab. */
static short  one_vis;
static void draw_one(const GRECT *c)  { (void) c; psui_draw_row(&ui, vh, one_vis); }

static void redraw_some(const short *vis, short n)
{
	GRECT all;
	short k, got = 0;

	if (win < 0 || n <= 0)
		return;

	/* the union first, so the pointer is only hidden if it is inside it */
	all.g_x = all.g_y = all.g_w = all.g_h = 0;
	for (k = 0; k < n; k++)
	{
		GRECT rr;

		psui_value_rect(&ui, vis[k], &rr);
		if (rr.g_w <= 0 || rr.g_h <= 0)
			continue;
		if (!got)
		{
			all = rr;
			got = 1;
		}
		else
		{
			short x1 = (short) (all.g_x + all.g_w);
			short y1 = (short) (all.g_y + all.g_h);

			if (rr.g_x < all.g_x) all.g_x = rr.g_x;
			if (rr.g_y < all.g_y) all.g_y = rr.g_y;
			if (rr.g_x + rr.g_w > x1) x1 = (short) (rr.g_x + rr.g_w);
			if (rr.g_y + rr.g_h > y1) y1 = (short) (rr.g_y + rr.g_h);
			all.g_w = (short) (x1 - all.g_x);
			all.g_h = (short) (y1 - all.g_y);
		}
	}
	if (!got)
		return;

	redraw_begin(&all);
	for (k = 0; k < n; k++)
	{
		GRECT rr;

		psui_value_rect(&ui, vis[k], &rr);
		if (rr.g_w <= 0 || rr.g_h <= 0)
			continue;
		one_vis = vis[k];
		redraw_at(draw_one, rr.g_x, rr.g_y, rr.g_w, rr.g_h);
	}
	redraw_end();
}

static void say(const char *s)
{
	GRECT r;

	strncpy(status, s, sizeof(status) - 1);
	status[sizeof(status) - 1] = '\0';
	ui.status = status;
	if (iconified || win < 0)
		return;
	psui_status_rect(&ui, &r);
	redraw(draw_stat, r.g_x, r.g_y, r.g_w, r.g_h);
}

static void relayout(void)
{
	wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
	psui_layout(&ui, vh, wx, wy, ww, wh);
}

/* ---- applying a change --------------------------------------------------- */

/*
 * The result codes are the contract, and saying which one came back is
 * most of what makes the dialog trustworthy: "set" and "needs a restart"
 * are very different answers and a settings box that shows the same tick
 * for both is lying.
 */
static void report(const PSROW *r, long res)
{
	char m[160];

	switch (res)
	{
	case PS_R_OK:
		sprintf(m, "%.40s: applied.", r->title);
		break;
	case PS_R_DEFER:
		sprintf(m, "%.40s: accepted - applies at the next block boundary.",
		        r->title);
		break;
	case PS_R_RESTART:
		sprintf(m, "%.40s: saved to the config - needs an emulator restart.",
		        r->title);
		break;
	case PS_R_BOXBOOT:
		sprintf(m, "%.40s: takes effect the next time the ST Box starts.",
		        r->title);
		break;
	case PS_R_BUSY:
		sprintf(m, "%.40s: the drive is busy - try again in a moment.",
		        r->title);
		break;
	default:
		sprintf(m, "%.40s: refused (out of range, or the host said no).",
		        r->title);
		break;
	}
	say(m);
}

static void set_value(short i, long v)
{
	long res;

	if (v == rows[i].value && rows[i].kind != PS_K_ACTION)
		return;
	res = nf_call(psid | NF_PS_SETINT, (long) i, v);
	rows[i].result = (short) res;
	if (res >= 0)
	{
		rows[i].value = v;
		rows[i].changed = 1;
	}
	else
	{
		/* a refusal must not leave the control showing the value it
		 * refused - re-read what the host still has */
		long back = nf_call(psid | NF_PS_GETINT, PS_SET_BASE + i);

		if (back != -1L)
			rows[i].value = back;
	}
	/* the row is about to be drawn with this value, so record it as
	 * shown: otherwise the next poll sees a difference it caused itself
	 * and repaints the row a second time */
	psui_row_text(&rows[i], rows[i].value, rows[i].shown,
	              (short) sizeof(rows[i].shown));
	report(&rows[i], res);
}

static void do_action(short i)
{
	long res = nf_call(psid | NF_PS_ACTION, (long) i, 0L);

	rows[i].result = (short) res;
	report(&rows[i], res);
}

static void do_save(void)
{
	long res = nf_call(psid | NF_PS_SAVE, 0L);

	if (res == 0)
	{
		short k;

		for (k = 0; k < ui.nrows; k++)
			if (rows[k].klass == PS_C_BOOT)
				rows[k].changed = 0;
		say("Config written; the previous file was kept as .cfg.bak.");
		if (!iconified)
			redraw_rows();
	}
	else
		say("Could not write the config file - see the emulator console.");
}

/* ---- the image picker ---------------------------------------------------- */

static void load_pick(long kind)
{
	long n, i;

	npick = 0;
	n = nf_call(psid | NF_PS_LIST, kind, -1L, 0L, 0L);
	if (n <= 0)
		return;
	if (n > PICK_MAX)
		n = PICK_MAX;
	for (i = 0; i < n; i++)
	{
		pick[npick][0] = '\0';
		nf_call(psid | NF_PS_LIST, kind, i,
		        pick[npick], (long) sizeof(pick[0]));
		pick[npick][sizeof(pick[0]) - 1] = '\0';
		npick++;
	}
}

/* A string field browses TOS images (the rom and box-TOS fields) or disk
 * images (the floppy drives), never both - a TOS list under Drive A: or a
 * floppy list under the box TOS is exactly the mix-up this splits. */
static long pick_kind_for(short i)
{
	if (!strcmp(rows[i].name, "rom") ||
	    !strcmp(rows[i].name, "stbox_tos"))
		return PS_LIST_TOS;
	return PS_LIST_FLOPPY;
}

/*
 * A string row opens the host's list of images if there is one, and the
 * file selector otherwise. The images live on the Pi, not on a GEMDOS
 * drive, which is why the list comes over the NatFeat at all - a
 * fsel_input() on a machine without HOSTFS would show the wrong disk.
 */
static void pick_string(short i)
{
	char path[PS_STRLEN];
	char fname[64] = "";
	short btn = 0;
	long res;

	load_pick(pick_kind_for(i));

	if (npick > 0)
	{
		/*
		 * A modal three-at-a-time chooser. It is a form_alert rather
		 * than a skinned scroller because this is the one modal moment
		 * in the program and a second scroll implementation for it
		 * would be more code than the feature is worth - and because
		 * an accessory has to work on a plain TOS desktop too.
		 */
		char alert[256];

		picktop = 0;
		for (;;)
		{
			short avail = (short) (npick - picktop);
			short chosen, k;

			if (avail > 3)
				avail = 3;
			sprintf(alert,
			        "[0][%.20s|%.24s|%.24s|%.24s][%s|%s|%s]",
			        rows[i].title,
			        avail > 0 ? pick[picktop + 0] : "",
			        avail > 1 ? pick[picktop + 1] : "",
			        avail > 2 ? pick[picktop + 2] : "",
			        avail > 0 ? "1" : "Cancel",
			        avail > 1 ? "2" : (avail > 0 ? "Cancel" : "Cancel"),
			        npick > 3 ? "More" : (avail > 2 ? "3" : "Cancel"));
			chosen = form_alert(1, alert);

			/* the third button is "More" whenever there is more */
			if (npick > 3 && chosen == 3)
			{
				picktop = (short) (picktop + 3);
				if (picktop >= npick)
					picktop = 0;
				continue;
			}
			k = (short) (picktop + chosen - 1);
			if (chosen >= 1 && chosen <= avail && k < npick)
			{
				res = nf_call(psid | NF_PS_SETSTR, (long) i, pick[k]);
				rows[i].result = (short) res;
				read_str(i);
				rows[i].changed = 1;
				report(&rows[i], res);
			}
			return;
		}
	}

	/*
	 * No host list - a TOS image. The file selector reaches a GEMDOS
	 * drive, so it browses the Pi through HOSTFS. The ROMs live on the
	 * share, so START THERE rather than at A: or a stored host path the
	 * selector cannot navigate - the host maps the picked S:\... path
	 * back to a real file when it opens it.
	 */
	if (pick_kind_for(i) == PS_LIST_TOS)
		strcpy(path, "S:\\apj-os\\stbox\\roms\\*.*");
	else
		strcpy(path, rows[i].str[0] ? rows[i].str : "A:\\*.*");
	fsel_exinput(path, fname, &btn, "Choose a file");
	if (btn != 1 || !fname[0])
		return;
	{
		char full[128];		/* the host keeps the full path; our own
					 * copy of it is re-read afterwards */
		char *bs = strrchr(path, '\\');

		if (bs)
			bs[1] = '\0';
		sprintf(full, "%.60s%.30s", path, fname);
		res = nf_call(psid | NF_PS_SETSTR, (long) i, full);
		rows[i].result = (short) res;
		read_str(i);
		rows[i].changed = 1;
		report(&rows[i], res);
	}
}

/* ---- hover --------------------------------------------------------------- */

static void arm_m1(void)
{
	GRECT b;
	short mx, my, mb, ks;

	if (psui_widget_rect(&ui, ui.hover, &m1r))
	{
		m1flag = MO_LEAVE;		/* wait for it to leave this */
		return;
	}

	/*
	 * No widget under the pointer. Asking to hear about it ENTERING the
	 * window is only right while it is OUTSIDE - ask that with the
	 * pointer already inside and the AES answers at once, every time,
	 * and evnt_multi spins: the timer never elapses and the readouts
	 * stop. The pointer is inside whenever it is over the window's
	 * padding, the status strip or the whole of the Bench tab, so this
	 * is the common case, not the corner one. Inside, wait for it to
	 * leave a small box around where it is: one event per movement,
	 * nothing while it sits still.
	 */
	psui_bbox(&ui, &b);
	graf_mkstate(&mx, &my, &mb, &ks);
	if (mx >= b.g_x && my >= b.g_y &&
	    mx < b.g_x + b.g_w && my < b.g_y + b.g_h)
	{
		m1r.g_x = (short) (mx - 1); m1r.g_y = (short) (my - 1);
		m1r.g_w = 3; m1r.g_h = 3;
		m1flag = MO_LEAVE;
	}
	else
	{
		m1r = b;
		m1flag = MO_ENTER;
	}
}

/*
 * Repaint one widget, whatever kind it is. The draw functions are the
 * same ones a full redraw uses - only the clip rectangle is small - so
 * there is no second, differently-behaved drawing path to keep in step.
 */
static void redraw_widget(short id)
{
	GRECT r;

	if (id == PSW_NONE || !psui_widget_rect(&ui, id, &r))
		return;
	if (id >= PSW_ROW0)
	{
		one_vis = (short) (id - PSW_ROW0);
		redraw_at(draw_one, r.g_x, r.g_y, r.g_w, r.g_h);
	}
	else if (id >= PSW_TAB0 && id < PSW_TAB0 + PS_TAB_N)
		redraw_at(draw_tabs, r.g_x, r.g_y, r.g_w, r.g_h);
	else if (id == PSW_SCROLL)
		redraw_at(draw_rows, r.g_x, r.g_y, r.g_w, r.g_h);
	else if (id >= PSW_CMTARGET && id < PSW_CMTARGET + PSB_CM_N)
		redraw_at(draw_bhead, r.g_x, r.g_y, r.g_w, r.g_h);
	else
		redraw_at(draw_stat, r.g_x, r.g_y, r.g_w, r.g_h);
}

/*
 * MU_M1 gives one rectangle: while the pointer is over a widget we ask to
 * hear about it LEAVING, and while it is not, about it ENTERING the
 * window. MO_ENTER is 0 and MO_LEAVE is 1; having those the wrong way
 * round asks the AES for a condition that is already true, it answers at
 * once, and evnt_multi spins without ever letting the timer elapse. That
 * cost most of a morning on MP3GEM and is written down here so it costs
 * nothing on this one.
 */
/* one widget on its own, when there is only one to do */
static void redraw_one(short id)
{
	if (iconified || win < 0)
		return;
	redraw_begin(NULL);		/* the pointer is on it */
	redraw_widget(id);
	redraw_end();
}

static void redraw_row(short vis)
{
	redraw_one((short) (PSW_ROW0 + vis));
}

/* Arm MU_M1 to fire on the next small movement from where the pointer is
 * now, without spinning while it sits still. Used while a dropdown is open,
 * where the pointer is inside the window but not over a tracked widget. */
static void arm_here(void)
{
	short mx, my, mb, ks;

	graf_mkstate(&mx, &my, &mb, &ks);
	m1r.g_x = (short) (mx - 1);
	m1r.g_y = (short) (my - 1);
	m1r.g_w = 3;
	m1r.g_h = 3;
	m1flag = MO_LEAVE;
}

/*
 * While a dropdown is open, the pointer moving over it must highlight the
 * item under it - that is what openpick drives, and draw_drop paints. The
 * ordinary set_hover() tracks row widgets and never touches openpick, so
 * without this the open list showed no highlight and a click that landed a
 * pixel off the computed item just dismissed the list, leaving the next
 * click to hit the tab behind.
 */
static void set_drop_hover(short mx, short my)
{
	short sel = psui_drop_at(&ui, ui.openrow, my);
	GRECT d;

	psui_drop_rect(&ui, ui.openrow, &d);
	if (sel >= 0 && (mx < d.g_x || mx >= d.g_x + d.g_w))
		sel = -1;
	if (sel >= 0 && sel != ui.openpick)
	{
		ui.openpick = sel;
		redraw_dropbox(ui.openrow);
	}
	arm_here();
}

static void set_hover(short mx, short my)
{
	short h = psui_hit(&ui, mx, my);

	if (h == PSW_SCROLL)
		h = PSW_NONE;
	if (h != ui.hover)
	{
		short old = ui.hover;

		ui.hover = h;
		/*
		 * Exactly two widgets can look different: the one the pointer
		 * left and the one it entered. This used to repaint the whole
		 * row list and the whole tab strip on every boundary the
		 * pointer crossed, each behind its own screen lock, which is
		 * why moving the mouse over the window made the window - and
		 * the desktop behind it - flicker.
		 */
		if (!iconified && win >= 0)
		{
			redraw_begin(NULL);	/* the pointer is on one of them */
			redraw_widget(old);
			redraw_widget(h);
			redraw_end();
		}
	}
	arm_m1();
}

/* ---- scrolling ----------------------------------------------------------- */

static void set_top(long t)
{
	long max = (long) ui.nidx - ui.visrows;

	if (max < 0) max = 0;
	if (t > max) t = max;
	if (t < 0)   t = 0;
	if ((short) t != ui.top)
	{
		ui.top = (short) t;
		relayout();
		if (!iconified)
			redraw_rows();
	}
}

static void set_tab(short t)
{
	if (t < 0) t = (short) (PS_TAB_N - 1);
	if (t >= PS_TAB_N) t = 0;
	if (t == ui.tab)
		return;
	ui.tab = t;
	ui.top = 0;
	ui.openrow = -1;
	relayout();
	arm_m1();
	/* Say how many rows this tab has. It reads as ordinary status text,
	 * and it means a tab that is quietly missing rows - a truncated
	 * descriptor table, a tab byte that did not survive the wire - shows
	 * up on the screen instead of only in a photograph of it. */
	{
		char buf[80];

		sprintf(buf, "%s: %d settings of %d.", psui_tab_name(ui.tab),
		        (int) ui.nidx, (int) ui.nrows);
		strncpy(status, buf, sizeof(status) - 1);
		status[sizeof(status) - 1] = '\0';
		ui.status = status;
	}
	if (!iconified)
		redraw(draw_all, wx, wy, ww, wh);
}

/* ---- the benchmark ------------------------------------------------------- */

/*
 * Sixteen shades for the solid, as SCREEN-FORMAT PIXELS - not VDI pens,
 * and not palette indices either.
 *
 * The renderer draws straight into a buffer in the screen's own format
 * and that buffer is blitted as it stands. It used to work a byte per
 * pixel and be expanded afterwards, which was measured on hardware at
 * SIXTY PERCENT of the frame - most of the graphics score was a
 * conversion no real program would perform. vro_cpyfm cannot convert
 * between formats (a plane-count mismatch is a blit that does nothing at
 * all, which is how the view came out blank in the first place), so the
 * only way to have neither problem is to draw in the right format from
 * the start.
 */
static long  bench_shade[16];
static void *bench_buf = NULL;
static short bench_bpp = 0;
static short bench_stride = 0;		/* pixels per row, a multiple of 16 */

static void bench_palette(void)
{
	long lo = apj_skin_rgb(APJ_R_PANEL), hi = apj_skin_rgb(APJ_R_TEXT);
	short i;

	if (lo < 0) lo = 0x00202020L;
	if (hi < 0) hi = 0x00E0E0E0L;

	for (i = 0; i < 16; i++)
	{
		short k = (short) (i * 100 / 15);
		long  rgb = 0;
		short j;

		for (j = 0; j < 3; j++)
		{
			short sh = (short) (16 - 8 * j);
			short a = (short) ((lo >> sh) & 0xFF);
			short b = (short) ((hi >> sh) & 0xFF);

			rgb |= ((long) (a + (b - a) * k / 100) & 0xFF) << sh;
		}
		bench_shade[i] = apj_skin_pack(rgb);
	}
	bench_bpp = apj_skin_planes();
}

/*
 * One frame on the screen: one blit, nothing else. Called from inside
 * psbench_run() between one frame and the next, so the object is seen
 * turning while it is measured - and so the cost of this call, and only
 * this call, is what the "Blit" row reports.
 */
static short bench_draw(void *buf, short w, short h, short stride, void *ctx)
{
	MFDB src, dst;
	short pxy[8];
	GRECT g;

	(void) ctx;
	if (win < 0 || iconified)
		return 0;
	if (!psui_bench_stage(&ui, w, h, &g))
	{
		bench.g.why = PS3D_NOROOM;
		return 0;
	}

	src.fd_addr    = buf;
	src.fd_w       = stride;
	src.fd_h       = h;
	src.fd_wdwidth = (short) (stride / 16);
	src.fd_stand   = 0;
	src.fd_nplanes = bench_bpp;
	src.fd_r1 = src.fd_r2 = src.fd_r3 = 0;

	dst.fd_addr = NULL;			/* the screen */

	pxy[0] = 0; pxy[1] = 0;
	pxy[2] = (short) (w - 1); pxy[3] = (short) (h - 1);
	pxy[4] = g.g_x; pxy[5] = g.g_y;
	pxy[6] = (short) (g.g_x + w - 1);
	pxy[7] = (short) (g.g_y + h - 1);

	apj_clip(vh, g.g_x, g.g_y, g.g_w, g.g_h);
	vro_cpyfm(vh, S_ONLY, pxy, &src, &dst);
	apj_clip_off(vh);
	return 1;
}

static void run_bench(void)
{
	short fpu = 0;
	long v;

	v = nf_call(psid | NF_PS_GETINT, PS_CFG_FPU_MODEL);
	if (v > 0 && v != -1L)
		fpu = 1;

	/* keep the run before, so the tab can show both columns */
	if (bench.ran)
	{
		bprev = bench;
		have_prev = 1;
		ui.bprev = &bprev;
	}

	say("Benchmark running - about twenty seconds. CoreMark alone "
	    "insists on ten.");
	graf_mouse(BUSY_BEE, NULL);
	bench_palette();
	{
		long f0 = psid ? nf_call(psid | NF_PS_GETINT,
		                         PS_STAT_FLUSHES_TOTAL) : -1L;

		bench.flushes_run = (f0 >= 0) ? f0 : -1L;
	}
	/*
	 * The MFDB wants a width that is a multiple of 16, so the buffer is
	 * sized to that rather than to the frame - fd_wdwidth is in words
	 * and a ragged one is how a blit reads past the end of a row.
	 */
	{
		PSB_SURF surf;
		long n;

		bench_stride = (short) (((PSB_3D_W + 15) / 16) * 16);
		n = (long) bench_stride * PSB_3D_H *
		    (bench_bpp == 16 ? 2L : 4L);
		bench_buf = (void *) Mxalloc(n, 3);
		if (!bench_buf || (long) bench_buf == -32L)
			bench_buf = (void *) Malloc(n);
		if ((long) bench_buf == -32L)
			bench_buf = NULL;

		surf.buf     = bench_buf;
		surf.stride  = bench_stride;
		surf.bpp     = bench_bpp;
		surf.shade16 = bench_shade;

		psbench_run(&bench, fpu, ui.cmtarget, &surf, bench_draw, NULL);

		if (bench_buf)
		{
			Mfree(bench_buf);
			bench_buf = NULL;
		}
	}
	graf_mouse(ARROW, NULL);

	/* what the cumulative flush counter did over the run */
	if (bench.flushes_run >= 0 && psid)
	{
		long f1 = nf_call(psid | NF_PS_GETINT, PS_STAT_FLUSHES_TOTAL);

		bench.flushes_run = (f1 >= 0 && f1 >= bench.flushes_run)
		                  ? f1 - bench.flushes_run : 0L;
	}
	else
		bench.flushes_run = 0L;

	ui.bench = &bench;
	ui.bnote = psbench_cm_note(&bench);
	relayout();
	say("Benchmark done. Change a setting and run it again - the right "
	    "column is the run before.");
	if (!iconified)
		redraw(draw_all, wx, wy, ww, wh);
}

/* ---- clicks -------------------------------------------------------------- */

/*
 * Dragging a slider used to repaint the whole row list on every step -
 * thirty times a second, the panel and every other row on the tab, for a
 * knob moving inside one of them. One row, and the status line is left
 * alone until the button comes up: report() at the end is the only
 * message the drag produces.
 */
static void drag_slider(short vis, short i)
{
	short bmx, bmy, bst, bks;

	for (;;)
	{
		long v = rows[i].value;

		graf_mkstate(&bmx, &bmy, &bst, &bks);
		if (!(bst & 1))
			break;
		if (psui_click_value(&ui, vis, bmx, bmy, &v) == 1 &&
		    v != rows[i].value)
		{
			long res = nf_call(psid | NF_PS_SETINT, (long) i, v);

			rows[i].result = (short) res;
			if (res >= 0)
			{
				rows[i].value = v;
				rows[i].changed = 1;
			}
			psui_row_text(&rows[i], rows[i].value, rows[i].shown,
			              (short) sizeof(rows[i].shown));
			redraw_row(vis);
		}
		evnt_timer(30L);
	}
	report(&rows[i], rows[i].result);
}

/*
 * The dropdown covers its own row and the list below it, so opening or
 * closing one repaints that much and no more - not the tab.
 */
static void redraw_drop(short vis)
{
	GRECT r, d;
	short y0, y1;

	if (iconified || win < 0 || vis < 0)
		return;
	if (!psui_widget_rect(&ui, (short) (PSW_ROW0 + vis), &r))
		return;
	psui_drop_rect(&ui, vis, &d);
	y0 = r.g_y;
	y1 = (short) (r.g_y + r.g_h);
	if (d.g_h > 0)
	{
		if (d.g_y < y0)
			y0 = d.g_y;
		if (d.g_y + d.g_h > y1)
			y1 = (short) (d.g_y + d.g_h);
	}
	redraw(draw_rows, r.g_x, y0, r.g_w, (short) (y1 - y0));
}

/* Repaint just the open list box, over the rows already on screen. Used
 * while the pointer tracks across items: redraw_drop() above repaints the
 * whole row list behind the box as well, which on a live tab is the
 * "everything behind, then the list" jump seen on every move. Opening
 * paints the box over existing rows too, so this serves both; only
 * closing needs the full repaint, to erase the box and restore the rows. */
static void redraw_dropbox(short vis)
{
	GRECT d;

	if (iconified || win < 0 || vis < 0)
		return;
	psui_drop_rect(&ui, vis, &d);
	if (d.g_h > 0)
		redraw(draw_dropbox, d.g_x, d.g_y, d.g_w, d.g_h);
}

static void open_drop(short vis)
{
	ui.openrow = vis;
	ui.openpick = (short) rows[ui.idx[ui.top + vis]].value;
	redraw_dropbox(vis);
	arm_here();		/* so the first mouse move tracks the list */
}

static void close_drop(void)
{
	short was = ui.openrow;

	ui.openrow = -1;
	redraw_drop(was);		/* the rect the open list occupied */
}

static void click(short mx, short my)
{
	short id, vis, i;
	long v;

	/* a dropdown is modal-ish: the next click either picks or dismisses */
	if (ui.openrow >= 0)
	{
		short sel = psui_drop_at(&ui, ui.openrow, my);
		GRECT d;

		psui_drop_rect(&ui, ui.openrow, &d);
		if (sel >= 0 && mx >= d.g_x && mx < d.g_x + d.g_w)
		{
			short vr = ui.openrow;

			i = ui.idx[ui.top + vr];
			close_drop();
			set_value(i, (long) sel);
			redraw_row(vr);
			return;
		}
		close_drop();
		return;
	}

	id = psui_hit(&ui, mx, my);

	if (id >= PSW_TAB0 && id < PSW_TAB0 + PS_TAB_N)
	{
		set_tab((short) (id - PSW_TAB0));
		return;
	}

	if (id == PSW_SCROLL)
	{
		short part;

		if (!psui_scroll_needed(&ui))
			return;
		part = psui_scroll_part(&ui, my);
		if (part < 0)
			set_top(ui.top - ui.visrows);
		else if (part > 0)
			set_top(ui.top + ui.visrows);
		else
		{
			GRECT t;
			short grab, bmx, bmy, bst, bks;

			psui_thumb_rect(&ui, &t);
			grab = (short) (my - t.g_y);
			ui.dragging = 1;
			redraw_one(PSW_SCROLL);
			for (;;)
			{
				graf_mkstate(&bmx, &bmy, &bst, &bks);
				if (!(bst & 1))
					break;
				set_top(psui_scroll_top_for(&ui, bmy, grab));
				evnt_timer(20L);
			}
			ui.dragging = 0;
			redraw_one(PSW_SCROLL);
		}
		return;
	}

	if (id == PSW_SAVE)
	{
		ui.press = id;
		redraw_one(id);
		evnt_timer(70L);
		ui.press = PSW_NONE;
		do_save();
		redraw_one(id);
		return;
	}

	if (id >= PSW_CMTARGET && id < PSW_CMTARGET + PSB_CM_N)
	{
		short t = (short) (id - PSW_CMTARGET);

		if (t > ui.cmmax)
		{
			say("This CPU cannot run the 020 build - it would be an "
			    "illegal instruction, not a slow score.");
			return;
		}
		if (t != ui.cmtarget)
		{
			short old = ui.cmtarget;

			ui.cmtarget = t;
			redraw_one((short) (PSW_CMTARGET + old));
			redraw_one(id);
		}
		return;
	}

	if (id == PSW_BENCH)
	{
		ui.press = id;
		redraw_one(id);
		evnt_timer(70L);
		ui.press = PSW_NONE;
		run_bench();
		return;
	}

	if (id < PSW_ROW0)
		return;

	vis = (short) (id - PSW_ROW0);
	if (ui.top + vis >= ui.nidx)
		return;
	i = ui.idx[ui.top + vis];

	v = rows[i].value;
	switch (psui_click_value(&ui, vis, mx, my, &v))
	{
	case 1:
		if (rows[i].kind == PS_K_INT)
		{
			ui.press = id;
			set_value(i, v);
			redraw_row(vis);
			drag_slider(vis, i);
			ui.press = PSW_NONE;
			redraw_row(vis);
		}
		else
		{
			set_value(i, v);
			redraw_row(vis);
		}
		break;

	case 2:
		if (rows[i].kind == PS_K_STR)
		{
			pick_string(i);	/* a form_alert: the window comes back */
			if (!iconified)
				redraw_rows();
		}
		else
			open_drop(vis);	/* paints its own row and list */
		break;

	case 3:
		ui.press = id;
		redraw_row(vis);
		evnt_timer(70L);
		ui.press = PSW_NONE;
		do_action(i);
		redraw_row(vis);
		break;

	default:
		break;
	}
}

/* ---- window -------------------------------------------------------------- */

#define WIN_KIND	(NAME | CLOSER | MOVER | SMALLER)

/*
 * No SIZER: XaAES's Fluent chrome reserves a column on the right of a
 * sizeable window for it, which puts a fixed layout off-centre. The list
 * scrolls, so there is nothing a resize would show that scrolling does
 * not - the same decision MP3GEM made, for the same reason.
 */
static void natural_size(short *w, short *h)
{
	short dx, dy, dw, dh, mw, mh;

	/*
	 * 510 rather than 430: the Bench tab is a fourteen-row table with
	 * three headings and the CoreMark compliance line under it. It has
	 * grown twice, both times because a row quietly fell off the
	 * bottom - which draws nothing, so nothing notices. The harness
	 * counts the rows drawn against the rows produced now, and this
	 * height is what makes that check pass at every scale - the
	 * fourteenth row cost two points off the row height rather than
	 * another thirty off the screen.
	 */
	*w = apj_skin_ok() ? apj_skin_m(620) : (short) (62 * cw);
	*h = apj_skin_ok() ? apj_skin_m(510) : (short) (28 * ch);
	psui_minsize(&mw, &mh);
	if (*w < mw) *w = mw;
	if (*h < mh) *h = mh;
	wind_get(0, WF_WORKXYWH, &dx, &dy, &dw, &dh);
	if (*w > dw) *w = dw;
	if (*h > dh) *h = dh;
}

/*
 * An accessory is loaded at BOOT, before the desktop has started, let
 * alone committed its theme - so the answer to appl_control(115) in
 * main() is whatever XaAES happened to be holding, not what TeraDesk is
 * about to set. Asking once at startup is how the window came up in
 * Fluent Dark on a Fuji desktop.
 *
 * So the theme is re-checked every time the window is opened, which for
 * an accessory is always long after the desktop has settled, and the
 * skin is reloaded only if the nineteen role colours actually moved. A
 * .PRG gets the same check for free and never needs it.
 *
 * (The other half of that bug was in apj_skin_reload(), which reloaded
 * the sheet it already had rather than re-picking by theme. Fixed there.)
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
	apj_init(vh);				/* re-reads the nineteen roles */
	if (theme_sig() == skin_theme_sig && apj_skin_ok())
		return;
	apj_skin_reload(vh);
	skin_theme_sig = theme_sig();
	if (win >= 0)
	{
		relayout();
		arm_m1();
		if (!iconified)
			redraw(draw_all, wx, wy, ww, wh);
	}
}

static char wtitle[] = "  PiSTorm Settings  ";

static void open_win(void)
{
	short dx, dy, dw, dh, cx, cy, cwid, chgt, want_w, want_h;

	check_theme();				/* the desktop may have re-themed */
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
	arm_m1();
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

/* ---- scale --------------------------------------------------------------- */

/*
 * Where PSCTRL.INF lives.
 *
 * A .PRG has a program folder and the .INF sits in it. An ACCESSORY does
 * not: shel_read() answers with the path of the CURRENTLY RUNNING
 * application, not the accessory's own, so using it here meant the .INF
 * was read from - and written to - next to whatever program happened to
 * be in the foreground at the time. Open the settings from the desktop
 * and you got one file; open it from inside a game and you got another,
 * or none.
 *
 * An .ACC is loaded from the ROOT OF THE BOOT DEVICE, and _bootdev (the
 * word at $446) is which device that was. That is the only self-consistent
 * answer available to an accessory, and it is where the README says to put
 * the file.
 */
#ifdef BUILD_ACC

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
	sprintf(out, "%c:\\PSCTRL.INF", (int) ('A' + d));
}

#else

static void inf_path(char *out, long n)
{
	char pd[192];

	apj_skin_progdir(pd, (long) sizeof pd);
	sprintf(out, "%.*sPSCTRL.INF", (int) (n - 12), pd);
}

#endif

/*
 * PSCTRL.INF, next to the .PRG - or, for the accessory, in the root of
 * the boot drive, because that is where an .ACC is loaded from and
 * therefore what shel_read() reports as its program folder.
 *
 *     scale=125
 *     skins=S:\APJ-OS\NATFEATS\SKINS
 *
 * The skins line matters far more for the .ACC than for the .PRG. A .PRG
 * sits with the rest of the PiSTorm tools and finds SKINS\ beside itself;
 * an accessory's progdir is C:\ and it never will. There is a built-in
 * fallback to the same path in apjskin.c, so this only needs writing if
 * the sheets live somewhere else.
 */
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

/* the skins= line, trimmed at the end of the line; "" if absent */
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
	char path[224], buf[200];
	long fh;

	inf_path(path, (long) sizeof path);
	fh = Fcreate(path, 0);
	if (fh < 0)
		return;
	/* the skins line is preserved: it is the one thing in here a person
	 * is likely to have typed by hand, and losing it on a scale change
	 * would send the accessory back to the flat controls */
	{
		char dir[160];

		inf_skindir(dir, (long) sizeof dir);
		if (dir[0])
			sprintf(buf, "scale=%d\r\nskins=%.120s\r\n", (int) scale, dir);
		else
			sprintf(buf, "scale=%d\r\n", (int) scale);
	}
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
	arm_m1();
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
	long  api;

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
	apj_init(vh);				/* theme, before any window */
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
	/* the .ACC deliberately does NOT load here: at boot the theme is not
	 * settled, so the sheet chosen now would be the wrong one AND would
	 * cost a couple of megabytes of file read to find that out.
	 * check_theme() does it when the window is first opened. */

	memset(&ui, 0, sizeof(ui));
	ui.rows = rows;
	ui.hover = PSW_NONE;
	ui.press = PSW_NONE;
	ui.openrow = -1;
	ui.status = status;
	ui.tab = PS_TAB_JIT;

	if (!psid)
	{
		ui.host = 0;
		strcpy(status, "No PSCTRL NatFeat: this emulator is too old, "
		               "or this is not a PiSTorm.");
#ifndef BUILD_ACC
		form_alert(1, "[3][PSCTRL NatFeat not found.|"
		              "Run under the PiSTorm emulator.][ OK ]");
		v_clsvwk(vh);
		appl_exit();
		return 1;
#endif
	}
	else
	{
		ui.host = 1;
		api = nf_call(psid | NF_PS_SETAPI);
		if (api <= 0 || api == -1L)
		{
			/* the status half of PSCTRL exists (PSMON uses it) but
			 * the settings sub-ops do not - say which half is
			 * missing rather than "not found" */
			ui.host = 0;
			strcpy(status, "This emulator has PSCTRL status but no "
			               "settings sub-ops - update the host.");
		}
		else if (!load_model())
		{
			ui.host = 0;
			strcpy(status, "The host answered no settings.");
		}
		else
		{
			seed_shown();
			sprintf(status, "%d settings, API %ld. Live changes take "
			                "effect at once.", (int) ui.nrows, api);
		}
	}

	/*
	 * The Bench tab exists whether or not anything has been run - it
	 * says "press Benchmark" until it has. The CoreMark build defaults
	 * to the best one this CPU can actually execute: offering the 020
	 * build on a 68000 would be offering a crash.
	 */
	ui.bench = &bench;
	ui.bprev = NULL;
	ui.bnote = psbench_cm_note(&bench);
	{
		long cpu = psid ? nf_call(psid | NF_PS_GETINT, PS_CFG_CPU_MODEL) : 0;

		ui.cmmax = (cpu >= 68020L && cpu != -1L) ? PSB_CM_68020
		                                         : PSB_CM_68000;
		ui.cmtarget = ui.cmmax;
	}

#ifdef BUILD_ACC
	menu_id = menu_register(apid, "  PiSTorm Settings");
#else
	open_win();
	if (win < 0)
	{
		form_alert(1, "[1][PSCTRL: no window available][ OK ]");
		v_clsvwk(vh);
		appl_exit();
		return 1;
	}
#endif

	for (;;)
	{
		ev = evnt_multi(MU_MESAG | MU_BUTTON | MU_KEYBD | MU_TIMER | MU_M1,
		                1, 1, 1,
		                m1flag, m1r.g_x, m1r.g_y, m1r.g_w, m1r.g_h,
		                0, 0, 0, 0, 0,
		                msg, 500UL,
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
				if (msg[3] == win)
					wind_set(win, WF_TOP, 0, 0, 0, 0);
				break;
			case WM_MOVED:
			case WM_SIZED:
				if (msg[3] == win)
				{
					wind_set(win, WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
					relayout();
					arm_m1();
					if (msg[0] == WM_SIZED)
						redraw(draw_all, wx, wy, ww, wh);
				}
				break;
			case WM_ARROWED:
				if (msg[3] == win)
				{
					short n = (short) ((msg[4] >> 8) & 0xff);

					if (n < 1) n = 1;
					switch (msg[4] & 15)
					{
					case WA_UPLINE: set_top(ui.top - n); break;
					case WA_DNLINE: set_top(ui.top + n); break;
					case WA_UPPAGE: set_top(ui.top - ui.visrows); break;
					case WA_DNPAGE: set_top(ui.top + ui.visrows); break;
					}
				}
				break;
			case WM_ICONIFY:
			case WM_ALLICONIFY:
				if (msg[3] == win)
				{
					wind_set(win, WF_ICONIFY, msg[4], msg[5], msg[6], msg[7]);
					iconified = 1;
					relayout();
				}
				break;
			case WM_UNICONIFY:
				if (msg[3] == win)
				{
					wind_set(win, WF_UNICONIFY, msg[4], msg[5], msg[6], msg[7]);
					iconified = 0;
					relayout();
					arm_m1();
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
				/* the desktop committed a theme while we were open */
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
					win = -1;	/* the AES tore it down */
				break;
#endif
			default:
				break;
			}
		}

		if ((ev & MU_M1) && win >= 0 && !iconified)
		{
			if (ui.openrow >= 0)
				set_drop_hover(mx, my);
			else
				set_hover(mx, my);
		}
		if ((ev & MU_BUTTON) && win >= 0 && !iconified)
			click(mx, my);

		if ((ev & MU_KEYBD) && win >= 0)
		{
			char  c = (char) (kr & 0xff);
			short scan = (short) ((kr >> 8) & 0xff);

			/*
			 * The PiSTorm's USB bridge turns a mouse wheel click
			 * into a cursor key tap (kbd_usb.c), so XaAES never
			 * sees a wheel and the arrows are how this scrolls.
			 */
			if      (scan == 0x48) set_top(ui.top - 1);
			else if (scan == 0x50) set_top(ui.top + 1);
			else if (scan == 0x49) set_top(ui.top - ui.visrows);
			else if (scan == 0x51) set_top(ui.top + ui.visrows);
			else if (scan == 0x4B) set_tab(psui_tab_step(ui.tab, -1));
			else if (scan == 0x4D) set_tab(psui_tab_step(ui.tab, 1));
			else if (c == '1') set_scale(100);
			else if (c == '2') set_scale(125);
			else if (c == '3') set_scale(175);
			else if (c == '0') set_scale(0);
			else if (c == 'b' || c == 'B') run_bench();
			else if (c == 's' || c == 'S') do_save();
			else if (c == 0x1b || c == 'q' || c == 'Q')
			{
#ifdef BUILD_ACC
				close_win();
#else
				goto out;
#endif
			}
		}

		if ((ev & MU_TIMER) && win >= 0 && !iconified && ui.host
		    && ui.openrow < 0)
		{
			/* frozen while a dropdown is open: repainting a live
			 * readout row under the open list would paint over it,
			 * and the click that followed would miss and hit the
			 * tab behind. */
			short dirty[PS_MAXROW], nd = refresh_visible(dirty);

			if (nd > 0)
				redraw_some(dirty, nd);
		}
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
