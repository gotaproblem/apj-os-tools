/*
 * vidui.h - VIDGEM's window, drawn entirely out of an APJSKIN sheet.
 *
 * Split out of vidgem.c so the same drawing code builds on the host by
 * tests/vid and can be diffed against skins/preview.py. Nothing in here
 * talks to the VIDPLAY NatFeat or the overlay: the app fills in VIDUI,
 * calls vidui_layout(), draws, and asks vidui_pane_rect() where the
 * picture goes.
 *
 * The film itself is not drawn by anyone here: the Pi puts it on a DRM
 * overlay plane above the whole Atari screen, positioned by the app from
 * the pane rectangle. What the window draws under it is a black box - the
 * letterbox bars, and what shows when the overlay is hidden.
 */

#ifndef VIDUI_H
#define VIDUI_H

#include <gem.h>
#include "../apjgui/apjskin.h"

/* widget ids - also the indices apj_lay_hit() gives back */
enum
{
	W_PREV, W_RW, W_PLAY, W_FF, W_NEXT, W_STOP,
	W_LOOP, W_VOLICO, W_VOL, W_SEEK,
	W_OPEN, W_FULL, W_LISTTOG,
	W_SCROLL, W_LIST, W_VIDEO,
	W_N
};

#define VIDUI_MAXLAY	(W_N + 2)

/* what the pane says when there is no picture to show in it */
enum
{
	VID_MSG_NONE,		/* a black box, or the picture over it        */
	VID_MSG_IDLE,		/* nothing playing: glyph and "Open a file"   */
	VID_MSG_WAIT,		/* playing, first frame not decoded yet       */
	VID_MSG_TOOBIG,		/* the overlay's floor is bigger than the box */
	VID_MSG_FLOOR_UNFIT	/* ... bigger than the whole desktop         */
};

typedef struct
{
	/* what the NatFeat told us */
	const char *title;		/* container title, or the file name   */
	const char *sub;		/* artist / comment, may be ""         */
	const char *codec;		/* "H.264", NULL = unknown             */
	long   vid_w, vid_h;		/* decoded picture size, 0 = unknown   */
	long   fps100;			/* file frame rate x100, 0 = unknown   */
	short  playing, paused;
	long   pos_s, len_s;
	short  vol;			/* 0..100                              */
	short  hasvol;
	short  loop;			/* repeat this file at the end         */
	short  listmode;		/* the pane shows the playlist         */
	short  fullscreen;		/* the overlay covers the display      */
	short  msg;			/* VID_MSG_* for the pane              */
	long   min_dw, min_dh;		/* the overlay's floor, for the message*/

	/* playlist */
	short  ntracks, sel, top;
	const char *(*name_of)(void *ctx, short i);
	void  *ctx;
	const char *dir;

	/* status line, right end: what is really being shown ("24.9 fps"),
	 * or NULL. The app compares the rendered string before repainting. */
	const char *shown;

	/* interaction */
	short  hover;			/* widget id under the pointer, or -1  */
	short  dragging;		/* 1 while the scrollbar thumb is held */
	short  press;			/* widget id held down, or -1          */

	/* filled in by vidui_layout() */
	GRECT  work;
	GRECT  clip;			/* what the current redraw may touch  */
	APJ_LAY lay[VIDUI_MAXLAY];
	short  nlay;
	short  rowh, visrows;
	short  infoh;			/* the title/badge band, px            */
	short  smallh;			/* the small font's cell height, px    */
} VIDUI;

void  vidui_layout(VIDUI *u, short vh, short wx, short wy, short ww, short wh);
void  vidui_draw  (VIDUI *u, short vh);
/* the same, but only the parts that meet clip - what WM_REDRAW wants */
void  vidui_draw_clip(VIDUI *u, short vh, const GRECT *clip);
void  vidui_draw_band  (VIDUI *u, short vh);	/* seek + transport     */
void  vidui_draw_clock (VIDUI *u, short vh);	/* the seek strip only  */
void  vidui_draw_info  (VIDUI *u, short vh);	/* title and badges     */
void  vidui_draw_status(VIDUI *u, short vh);	/* the bottom line only */
void  vidui_draw_list  (VIDUI *u, short vh);	/* the playlist only    */
void  vidui_draw_pane  (VIDUI *u, short vh);	/* the picture box only */

/* rectangles for a redraw of just that part */
void  vidui_clock_rect (VIDUI *u, GRECT *r);
void  vidui_info_rect  (VIDUI *u, GRECT *r);
void  vidui_status_rect(VIDUI *u, GRECT *r);
/* the pane: where the overlay goes (list mode or not, same rectangle) */
void  vidui_pane_rect  (VIDUI *u, GRECT *r);

short vidui_hit   (VIDUI *u, short mx, short my);
short vidui_row_at(VIDUI *u, short my);		/* playlist row, or -1   */

/* the scrollbar: is it needed, where is the thumb, and what does a click
 * at my mean - -1 above the thumb, 0 on it, +1 below it */
short vidui_scroll_needed(VIDUI *u);
void  vidui_thumb_rect(VIDUI *u, GRECT *r);
short vidui_scroll_part(VIDUI *u, short my);
short vidui_scroll_top_for(VIDUI *u, short my, short grab);

/* smallest window the layout still works in, in device pixels; the app
 * raises it further to the overlay's floor once that is known */
void  vidui_minsize(short vh, short *w, short *h);

/* the strip the pointer has to be in for hover to matter */
void  vidui_bbox(VIDUI *u, GRECT *r);

#endif /* VIDUI_H */
