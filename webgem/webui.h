/*
 * webui.h - WEBGEM's window, drawn entirely out of an APJSKIN sheet.
 *
 * Split out of webgem.c so the same drawing code builds on the host by
 * tests/web and can be diffed against skins/preview.py --webgem. Nothing
 * in here talks to the PSWEB NatFeat: the app fills in WEBUI, calls
 * webui_layout(), draws, and asks webui_view_rect() where the page pixels
 * go and how big the buffer behind them has to be.
 *
 * The page is full bleed: the view IS the page widget, edge to edge, with
 * no frame. The Pi renders it and the emulator copies the changed
 * rectangles into a TT-RAM buffer the app owns (PSWEB FETCH);
 * webui_draw_pane() puts the part of that buffer that meets the clip on
 * the screen with one vro_cpyfm and draws nothing else over it.
 */

#ifndef WEBUI_H
#define WEBUI_H

#include <gem.h>
#include "../apjgui/apjskin.h"

/* widget ids - also the indices apj_lay_hit() gives back. The W_ prefix
 * clashes with gem.h's gadget constants (W_INFO...), so these are WB_. */
enum
{
	WB_BACK, WB_FWD, WB_RELOAD, WB_HOME, WB_ADDRESS,
	WB_BOOKMARK, WB_DOWNLOAD, WB_MENU,
	WB_BADGE_LOAD, WB_BADGE_BLOCK, WB_BADGE_JS,
	WB_TABNEW, WB_TAB0,			/* WB_TAB0 + i for tab i     */
	WB_TABMAX = WB_TAB0 + 8,
	WB_PAGE,
	WB_N
};

#define WEBUI_MAXLAY	(WB_N + 2)

enum { WEB_FOCUS_NONE, WEB_FOCUS_PAGE, WEB_FOCUS_ADDRESS };

/* what the pane says when there is no frame to show in it */
enum
{
	WEB_MSG_NONE,		/* the page buffer                             */
	WEB_MSG_WAIT,		/* "connecting to psweb..."                    */
	WEB_MSG_NOTT,		/* no TT-RAM for the page buffer               */
	WEB_MSG_CRASHED		/* the web process died: "reload" hint         */
};

#define WEB_ADDR_MAX	512

typedef struct
{
	/* the page, as the NatFeat described it */
	const char *uri;		/* current URI (shown when the field is idle)  */
	const char *link;		/* hovered link, "" or NULL for none           */
	const char *status;		/* status text when there is no link           */
	short  loading;			/* 1 while a load is in progress               */
	short  progress;		/* 0..1000                                     */
	short  secure;			/* https: the lock glyph in the field          */
	short  can_back, can_fwd;
	short  js_on, blocker_on;
	short  msg;			/* WEB_MSG_* for the pane                      */
	const char *waittext;		/* WEB_MSG_WAIT: what the link is doing, or NULL */

	/* tabs: only drawn when ntabs > 1 (v1 runs with one) */
	short  ntabs, tab_sel;
	const char *(*tab_title)(void *ctx, short i);
	void  *ctx;

	/* the address field */
	char   addr[WEB_ADDR_MAX];
	short  addr_all;		/* the whole text is selected (Ctrl+L)         */
	short  focus;			/* WEB_FOCUS_*                                 */

	/* interaction */
	short  hover;			/* widget id under the pointer, or -1          */
	short  press;			/* widget id held down, or -1                  */

	/* the page buffer: view pixels in screen format, from the host.
	 * fd_addr NULL = nothing fetched yet (the canvas is drawn instead) */
	MFDB   pagebuf;

	/* filled in by webui_layout() */
	GRECT  work;
	GRECT  clip;			/* what the current redraw may touch          */
	APJ_LAY lay[WEBUI_MAXLAY];
	short  nlay;
	short  smallh;			/* the small font's cell height, px            */
	GRECT  view;			/* where page pixels go (== WB_PAGE)           */
} WEBUI;

void  webui_layout(WEBUI *u, short vh, short wx, short wy, short ww, short wh);
void  webui_draw  (WEBUI *u, short vh);
/* the same, but only the parts that meet clip - what WM_REDRAW wants */
void  webui_draw_clip(WEBUI *u, short vh, const GRECT *clip);
void  webui_draw_tabs   (WEBUI *u, short vh);	/* the tab strip      */
void  webui_draw_band   (WEBUI *u, short vh);	/* the toolbar        */
void  webui_draw_pane   (WEBUI *u, short vh);	/* the page           */
void  webui_draw_status (WEBUI *u, short vh);	/* the bottom strip   */

/* rectangles for a redraw of just that part */
void  webui_tabs_rect   (WEBUI *u, GRECT *r);
void  webui_band_rect   (WEBUI *u, GRECT *r);
void  webui_pane_rect   (WEBUI *u, GRECT *r);
void  webui_status_rect (WEBUI *u, GRECT *r);
/* the view: the pixels the host fills - this is the VIEW_NEW / FETCH size */
void  webui_view_rect   (WEBUI *u, GRECT *r);
/* the rectangle of one widget, 0x0 when it is not in the layout */
void  webui_widget_rect (WEBUI *u, short id, GRECT *r);

short webui_hit    (WEBUI *u, short mx, short my);

/* smallest window the layout still works in, in device pixels */
void  webui_minsize(short vh, short *w, short *h);

/* the strip the pointer has to be in for hover to matter (tabs + toolbar
 * + status: everything but the page) */
void  webui_bbox(WEBUI *u, GRECT *r);

#endif /* WEBUI_H */
