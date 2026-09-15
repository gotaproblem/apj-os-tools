/*
 * pdfui.h - PDFGEM's window, drawn entirely out of an APJSKIN sheet.
 *
 * Split out of pdfgem.c so the same drawing code builds on the host by
 * tests/pdf and can be diffed against skins/preview.py --pdfgem. Nothing
 * in here talks to the PSPDF NatFeat: the app fills in PDFUI, calls
 * pdfui_layout(), draws, and asks pdfui_view_rect() where the page pixels
 * go and how big the buffer behind them has to be.
 *
 * The page itself is one blit: the Pi draws it and copies the visible
 * part, already in screen format and with the search highlights blended
 * in, into a TT-RAM buffer the app owns (PSPDF FETCH). pdfui_draw_pane()
 * puts that buffer on the screen with vro_cpyfm and draws nothing else
 * over it, so the 68k never touches a page pixel.
 */

#ifndef PDFUI_H
#define PDFUI_H

#include <gem.h>
#include "../apjgui/apjskin.h"

/* widget ids - also the indices apj_lay_hit() gives back */
enum
{
	W_OPEN, W_PGPREV, W_PAGEFLD, W_PGNEXT,
	W_ZOOMOUT, W_ZOOMBADGE, W_ZOOMIN, W_FITW, W_FITP, W_ROTATE,
	W_SEARCHFLD, W_FINDPREV, W_FINDNEXT, W_OUTLINE, W_ABOUT,
	W_OLSCROLL, W_OLIST, W_PGSCROLL, W_PAGE,
	W_N
};

#define PDFUI_MAXLAY	(W_N + 2)

enum { PDF_FIT_NONE, PDF_FIT_WIDTH, PDF_FIT_PAGE };
enum { PDF_FOCUS_NONE, PDF_FOCUS_PAGE, PDF_FOCUS_SEARCH };

/* what the pane says when there is no page to show in it */
enum
{
	PDF_MSG_NONE,		/* the page buffer, or the canvas             */
	PDF_MSG_IDLE,		/* no document: glyph and "Open a PDF..."     */
	PDF_MSG_NOTT,		/* no TT-RAM for the page buffer              */
	PDF_MSG_FAILED		/* the host could not draw this page          */
};

#define PDF_PAGETEXT	8
#define PDF_SEARCHTEXT	64

typedef struct
{
	/* the document, as the NatFeat described it */
	const char *fname;		/* file name for the status line       */
	long   pages;			/* 0 = nothing open                    */
	long   page;			/* 1-based                             */
	long   zoom;			/* percent x 10, 1000 = 100%           */
	short  fit;			/* PDF_FIT_*                           */
	short  rot;			/* 0, 90, 180, 270                     */
	long   page_w, page_h;		/* page size in pixels at this zoom    */
	long   scroll_x, scroll_y;	/* viewport origin in page pixels;
					 * negative when the page is narrower
					 * or shorter than the viewport (it is
					 * centred, the host fills the rest)   */
	short  msg;			/* PDF_MSG_* for the pane              */

	/* the status strip and its two badges: strings the app formats and
	 * compares before repainting, so a figure that has not changed on
	 * screen costs nothing. NULL = not shown. */
	const char *status;
	const char *hits;		/* accent badge: "3 hits on this page" */
	const char *busy;		/* plain badge: "rendering 43..."      */

	/* outline */
	long   noutline;
	long   ol_sel, ol_top;		/* selected entry, first visible entry */
	short  ol_show;			/* the panel is open                   */
	const char *(*outline_of)(void *ctx, long i, short *depth, long *page);
	void  *ctx;

	/* the two text fields */
	char   pagetext[PDF_PAGETEXT];
	char   searchtext[PDF_SEARCHTEXT];
	short  focus;			/* PDF_FOCUS_*                         */

	/* interaction */
	short  hover;			/* widget id under the pointer, or -1  */
	short  press;			/* widget id held down, or -1          */
	short  dragging;		/* 1 outline thumb, 2 page thumb held  */

	/* the page buffer: viewport pixels in screen format, from the host.
	 * fd_addr NULL = nothing fetched yet (the canvas is drawn instead) */
	MFDB   pagebuf;

	/* filled in by pdfui_layout() */
	GRECT  work;
	GRECT  clip;			/* what the current redraw may touch  */
	APJ_LAY lay[PDFUI_MAXLAY];
	short  nlay;
	short  rowh, visrows;		/* outline rows                        */
	short  smallh;			/* the small font's cell height, px    */
	GRECT  view;			/* where page pixels go (inside W_PAGE)*/
} PDFUI;

void  pdfui_layout(PDFUI *u, short vh, short wx, short wy, short ww, short wh);
void  pdfui_draw  (PDFUI *u, short vh);
/* the same, but only the parts that meet clip - what WM_REDRAW wants */
void  pdfui_draw_clip(PDFUI *u, short vh, const GRECT *clip);
void  pdfui_draw_band   (PDFUI *u, short vh);	/* the toolbar        */
void  pdfui_draw_outline(PDFUI *u, short vh);	/* the outline panel  */
void  pdfui_draw_pane   (PDFUI *u, short vh);	/* the page pane      */
void  pdfui_draw_status (PDFUI *u, short vh);	/* the bottom strip   */

/* rectangles for a redraw of just that part */
void  pdfui_band_rect   (PDFUI *u, GRECT *r);
void  pdfui_outline_rect(PDFUI *u, GRECT *r);
void  pdfui_pane_rect   (PDFUI *u, GRECT *r);
void  pdfui_status_rect (PDFUI *u, GRECT *r);
/* the viewport: the pixels the host fills - this is the FETCH size */
void  pdfui_view_rect   (PDFUI *u, GRECT *r);

short pdfui_hit    (PDFUI *u, short mx, short my);
long  pdfui_ol_row_at(PDFUI *u, short my);	/* outline entry, or -1  */

/* the outline scrollbar */
short pdfui_ol_scroll_needed(PDFUI *u);
void  pdfui_ol_thumb_rect(PDFUI *u, GRECT *r);
short pdfui_ol_scroll_part(PDFUI *u, short my);	/* -1 above, 0 on, +1 below */
long  pdfui_ol_top_for(PDFUI *u, short my, short grab);

/* the page scrollbar (vertical; the page scrolls sideways with the keys) */
short pdfui_pg_scroll_needed(PDFUI *u);
void  pdfui_pg_thumb_rect(PDFUI *u, GRECT *r);
short pdfui_pg_scroll_part(PDFUI *u, short my);
long  pdfui_pg_top_for(PDFUI *u, short my, short grab);

/* the zoom that fits the page's width / whole page into the viewport,
 * from the page size at 100% (percent x 10) */
long  pdfui_fit_zoom(PDFUI *u, long w100, long h100, short fit);

/* smallest window the layout still works in, in device pixels */
void  pdfui_minsize(short vh, short *w, short *h);

/* the strip the pointer has to be in for hover to matter */
void  pdfui_bbox(PDFUI *u, GRECT *r);

#endif /* PDFUI_H */
