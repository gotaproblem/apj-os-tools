/*
 * psui.h - PSCTRL's window, drawn entirely out of an APJSKIN sheet.
 *
 * Split out of psctrl.c the way mp3ui is split out of mp3gem, so the same
 * drawing code can be built on the host by tests/psctrl and checked
 * without a Pi. Nothing in here talks to the PSCTRL NatFeat: the app
 * fills in PSUI from PS_DESCRIBE and calls psui_layout() then psui_draw().
 *
 * The window is SELF-DESCRIBING. There is no compiled-in list of settings
 * and no per-tab layout code: the emulator ships a descriptor for every
 * control, the app turns each into a PSROW, and the drawing below decides
 * what a row looks like from its kind alone. Adding a switch to the
 * emulator therefore costs nothing here.
 */

#ifndef PSUI_H
#define PSUI_H

#include <gem.h>
#include "../apjgui/apjskin.h"
#include "psbench.h"

/* --- the protocol, as psctrl_settings.h defines it on the host ------- */

/*
 * Tab numbers are wire values - the host's descriptor table puts one in
 * every record - so they are append-only. PS_TAB_BENCH is guest-only: no
 * descriptor names it and the host does not know it exists. Where it sits
 * on the strip is a separate question, answered by psui_tab_at().
 */
enum { PS_TAB_JIT, PS_TAB_CPU, PS_TAB_VIDEO, PS_TAB_AUDIO, PS_TAB_INPUT,
       PS_TAB_STBOX, PS_TAB_FLOPPY, PS_TAB_NET, PS_TAB_DEBUG, PS_TAB_ADV,
       PS_TAB_BENCH,
       PS_TAB_N };

enum { PS_K_BOOL, PS_K_ENUM, PS_K_INT, PS_K_STR, PS_K_ACTION, PS_K_INFO };
enum { PS_C_LIVE, PS_C_DEFER, PS_C_BOOT, PS_C_BOXBOOT, PS_C_RO };
enum { PS_U_NONE, PS_U_MS, PS_U_NS, PS_U_US, PS_U_KB, PS_U_MB, PS_U_PCT,
       PS_U_HZ, PS_U_KBPS, PS_U_CYC, PS_U_X100, PS_U_SEC,
       PS_U_PCT10 };		/* tenths of a percent: 1000 -> "100.0%" */

#define PS_F_ADVANCED	0x0001
#define PS_F_NEWLINE	0x0002
#define PS_F_DANGER	0x0004

/* PS_SETINT results */
#define PS_R_OK		 0
#define PS_R_DEFER	 1
#define PS_R_RESTART	 2
#define PS_R_BOXBOOT	 3
#define PS_R_REJECT	(-1)
#define PS_R_BUSY	(-2)

/* --- the model ------------------------------------------------------- */

#define PS_MAXROW	160
#define PS_MAXENUM	12
#define PS_NAMELEN	24
#define PS_STRLEN	72

typedef struct
{
	char  name[PS_NAMELEN];		/* the .cfg key                      */
	char  title[40];		/* what the row is called            */
	short tab, kind, klass, unit;
	long  min, max, step;
	short nenum;
	short flags;
	long  value;			/* current, from DESCRIBE or GETINT  */
	char  label[PS_MAXENUM][16];	/* enum choices                      */
	char  str[PS_STRLEN];		/* PS_K_STR value                    */
	short changed;			/* set here this session             */
	short result;			/* last PS_R_* the host answered     */
	char  shown[32];		/* the value text last put on screen */
} PSROW;

/* widget ids handed back by psui_hit() */
enum
{
	PSW_NONE  = -1,
	PSW_TAB0  = 0,			/* .. PSW_TAB0 + PS_TAB_N - 1        */
	PSW_SCROLL = 64,
	PSW_BENCH,			/* "Benchmark" on the Bench tab      */
	PSW_SAVE,			/* "Save to .cfg", every tab         */
	/* a block of its own: the selector is PSB_CM_N buttons and running
	 * it on from PSW_SAVE would have made the second one BE PSW_SAVE */
	PSW_CMTARGET = 96,		/* .. + PSB_CM_N - 1                 */
	PSW_ROW0 = 128			/* + the row's index in rows[]       */
};

typedef struct
{
	/* the model - filled in by the app */
	PSROW *rows;
	short  nrows;
	short  tab;			/* which tab is showing              */
	short  top;			/* first row of this tab shown       */
	const char *status;		/* the bottom line                   */
	short  host;			/* 0 = no PSCTRL NatFeat             */

	/*
	 * The benchmark readout - its own tab. The app owns the results
	 * and this file only lays them out, so PSCTRL keeps the previous
	 * run and psui draws both columns without knowing what a DMIPS is.
	 */
	const PSBENCH *bench;		/* NULL = no benchmark support       */
	const PSBENCH *bprev;		/* the run before, or NULL           */
	const char *bnote;		/* the CoreMark compliance line      */
	short  cmtarget;		/* PSB_CM_*: which build to run      */
	short  cmmax;			/* highest PSB_CM_* this CPU can run */
	/* how many benchmark rows the last draw actually got on screen. A
	 * row that quietly falls off the bottom of the panel is invisible
	 * to a text check - it draws nothing to be outside anything - so
	 * the count is published and the harness compares it. */
	short  bdrawn, bwanted;
	short  btrunc;			/* values that had to be ellipsised  */

	/* interaction */
	short  hover;			/* widget id under the pointer       */
	short  press;			/* widget id held down               */
	short  dragging;		/* scrollbar thumb held              */
	short  openrow;			/* row whose dropdown is open, or -1 */
	short  openpick;		/* highlighted entry in it           */

	/* filled in by psui_layout() */
	GRECT  work, clip;
	GRECT  listr;			/* the scrolling area                */
	APJ_LAY lay[PS_MAXROW + PS_TAB_N + 8];
	short  nlay;
	short  rowh, visrows;
	short  idx[PS_MAXROW];		/* rows[] indices on this tab        */
	short  nidx;
	/* per visible-order row: 1 = this enum is drawn as radios, 0 = as a
	 * popup. Decided in psui_layout() from the label widths, not from
	 * the choice count alone, so the hit test and the drawing cannot
	 * disagree about which it is. */
	short  radios[PS_MAXROW];
	short  ctlx, ctlw;		/* the control column                */
} PSUI;

void  psui_layout(PSUI *u, short vh, short wx, short wy, short ww, short wh);
void  psui_draw(PSUI *u, short vh);
void  psui_draw_clip(PSUI *u, short vh, const GRECT *clip);
void  psui_draw_rows(PSUI *u, short vh);	/* the list only        */
void  psui_draw_drop(PSUI *u, short vh);	/* the open dropdown box only */
void  psui_draw_tabs(PSUI *u, short vh);
void  psui_draw_status(PSUI *u, short vh);
void  psui_draw_row(PSUI *u, short vh, short vis);   /* one visible row */

short psui_hit(PSUI *u, short mx, short my);
void  psui_row_rect(PSUI *u, short vis, GRECT *r);
void  psui_value_rect(PSUI *u, short vis, GRECT *r);
short psui_widget_rect(PSUI *u, short id, GRECT *r);
void  psui_status_rect(const PSUI *u, GRECT *r);
short psui_row_at(PSUI *u, short my);		/* visible row, or -1   */

/* where in a row did the click land: the value a click at mx means for
 * this row's control. Returns 1 if it changed *v, 0 if the click was not
 * on the control. For an enum with a dropdown it returns 2 and the caller
 * opens the list instead. */
short psui_click_value(PSUI *u, short vis, short mx, short my, long *v);

/* the dropdown an enum or a string row opens */
void  psui_drop_rect(PSUI *u, short vis, GRECT *r);
short psui_drop_at(PSUI *u, short vis, short my);

short psui_bench_rect(const PSUI *u, GRECT *g);
short psui_bench_head(const PSUI *u, GRECT *g);
void  psui_draw_bench_head(PSUI *u, short vh);
short psui_bench_stage(const PSUI *u, short w, short h, GRECT *r);
short psui_tab_at(short slot);			/* strip position -> tab   */
short psui_tab_step(short tab, short dir);	/* the arrow keys          */
void  psui_row_text(const PSROW *r, long v, char *out, short n);
short psui_scroll_needed(PSUI *u);
void  psui_thumb_rect(PSUI *u, GRECT *r);
short psui_scroll_part(PSUI *u, short my);
short psui_scroll_top_for(PSUI *u, short my, short grab);

void  psui_minsize(short *w, short *h);
void  psui_bbox(PSUI *u, GRECT *r);

/* the tab a row belongs to, and the tab names - the app needs these for
 * the keyboard shortcuts */
const char *psui_tab_name(short tab);

#endif /* PSUI_H */
