/*
 * psmonui.h - the PSMON window: layout, drawing, hit-testing.
 *
 * Separated from psmon.c for the same reason psui.h is separated from
 * psctrl.c: everything in here can be built on a Linux host against the
 * fake VDI in tests/stubvdi.h and checked blit by blit, which is the only
 * way any of this gets tested without a Pi and an Atari in the loop.
 *
 * The model is a fixed set of readings, not a descriptor table - PSMON
 * shows what the host samples and nothing configurable - so it is a plain
 * struct the app fills in and this file draws.
 */
#ifndef PSMONUI_H
#define PSMONUI_H

#include "apjskin.h"

/* points; apj_skin_m() turns these into pixels at the sheet's scale */
#define PM_W_PT		530
#define PM_H_PT		470	/* 14 rows; at 100% a row is the 18 px cell + 4, not ROWH */

/* A reading with no value. The host answers 0xFFFFFFFF for an index it
 * does not know, and an emulator older than this build will not know
 * some of them, so every field can be absent and every field says so
 * rather than drawing a plausible zero. */
#define PM_NONE		(-1L)

enum { PM_SEC_ENGINE, PM_SEC_MEMORY, PM_SEC_HOST, PM_SEC_N };

typedef struct
{
	/* --- the engine block, the same six figures the taskbar's JIT
	 * panel shows, from the same status indices --------------------- */
	long khz;		/* PS_JIT_EFF_KHZ    effective 68k speed      */
	long hit_x10;		/* PS_JIT_HITRATE_X10                         */
	long idle_x10;		/* PS_JIT_IDLE_X10                            */
	long cache_used;	/* PS_STAT_CACHE_USED   bytes                 */
	long cache_total;	/* PS_STAT_CACHE_TOTAL  bytes                 */
	long flushes;		/* PS_STAT_FLUSHES_TOTAL  since boot          */
	long compiles;		/* PS_STAT_COMPILES     per 500 ms window     */
	long smc;		/* PS_STAT_SMC_INV      per 500 ms window     */

	/* --- guest memory, from the low-memory system variables and
	 * GEMDOS - nothing here needs the emulator ---------------------- */
	long st_free, st_total;
	long tt_free, tt_total;

	/* --- the Pi itself. The JIT panel does not show these and the
	 * emulator already samples them: a board that is thermally capped
	 * makes every figure above it worse, and nothing else on the Atari
	 * side can say so. ---------------------------------------------- */
	long soc_mc;		/* PS_HOST_SOC_TEMP_MC   millidegrees C       */
	long arm_khz;		/* PS_HOST_ARM_FREQ_KHZ                       */
	long load_x100;		/* PS_HOST_LOADAVG_X100                       */
	long throttled;		/* PS_HOST_THROTTLED     firmware bits        */
	long pi_model;		/* PS_PI_MODEL           board code           */
	long pi_ram_mb;		/* PS_PI_RAM_MB                               */
	long net;		/* PS_HOST_NET  bit0 up, bit1 Wi-Fi, 8-15 quality */
	long ipv4;		/* PS_HOST_IPV4  a.b.c.d as one long          */
	long input;		/* PS_HOST_INPUT bit1 keyboard, bit2 mouse,
				 * bit3 real IKBD, 8-15 seconds since an event */
	long web_state;		/* PS_WEB_STATE  0 none 1 socket 2 conn 3 view */
	long web_fps_x10;	/* PS_WEB_FPS_X10                             */
	long web_kbps;		/* PS_WEB_KBPS                                */
	long web_rss_mb;	/* PS_WEB_RSS_MB                              */

	short host;		/* 0 = no PSCTRL NatFeat: engine/Pi are n/a   */
} PMDATA;

typedef struct
{
	const PMDATA *d;
	const char *status;

	/* what the last draw managed. A value that had to be ellipsised,
	 * or a row that fell off the bottom, draws correctly and stays
	 * inside every box - so nothing else notices, and the reader is
	 * shown "3.4 MB free of 4...." */
	short trunc, rows_drawn, rows_wanted;

	/* filled in by psmonui_layout() */
	GRECT work, clip;
	GRECT sec[PM_SEC_N];	/* the three group boxes                      */
	short rowh, labw, barx, barw, valx;
} PMUI;

void psmonui_layout(PMUI *u, short vh, short wx, short wy, short ww, short wh);
void psmonui_draw(PMUI *u, short vh);
void psmonui_draw_clip(PMUI *u, short vh, const GRECT *clip);
void psmonui_minsize(short *w, short *h);

/* The rectangle each section occupies, so the app can repaint just the
 * one whose figures moved instead of the window. */
short psmonui_sec_rect(const PMUI *u, short sec, GRECT *r);
void  psmonui_draw_sec(PMUI *u, short vh, short sec);

/* One line of text as it will appear. The app compares these rather than
 * the raw numbers: a hit rate wobbling in the third decimal must not
 * repaint anything, and the cache figure in KB sits still for minutes
 * while the byte count under it never stops. */
#define PM_MAXLINE	12
short psmonui_sec_lines(const PMUI *u, short sec, char out[][64], short max);

#endif /* PSMONUI_H */
