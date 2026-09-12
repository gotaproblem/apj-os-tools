/*
 * mp3ui.h - MP3GEM's window, drawn entirely out of an APJSKIN sheet.
 *
 * Split out of mp3gem.c so the same drawing code can be built on the
 * host by tests/skin and diffed against skins/preview.py. Nothing in
 * here talks to the MP3PLAY NatFeat: the app fills in MP3UI and calls
 * mp3ui_layout() then mp3ui_draw().
 */

#ifndef MP3UI_H
#define MP3UI_H

#include <gem.h>
#include "../apjgui/apjskin.h"

/* widget ids - also the indices apj_lay_hit() gives back */
enum
{
	W_PREV, W_RW, W_PLAY, W_FF, W_NEXT, W_STOP,
	W_SHUFFLE, W_REPEAT, W_VOLICO, W_VOL, W_SEEK,
	W_ART, W_OPEN, W_LIST, W_SCROLL,
	W_N
};

#define MP3UI_MAXLAY	(W_N + 2)

/* 1 puts the timer tick count and the raw MP3PLAY answers on the status
 * line - the readout that found the inverted MU_M1 flag. Off now. */
#define MP3UI_DEBUG	0

typedef struct
{
	/* what the NatFeat told us */
	const char *title;		/* ID3 title, or the file name         */
	const char *sub;		/* "artist - album", may be ""         */
	const char *codec;		/* NULL if MP3PLAY has no INFO sub-op  */
	short  bitrate;			/* kbps, 0 = unknown                   */
	short  playing, paused;
	long   pos_s, len_s;
	short  vol;			/* 0..100                              */
	short  shuffle, repeat;
	short  hasart;			/* 1 = artbuf holds a decoded cover    */
	void  *artbuf;			/* artedge^2 pixels, device format     */
	short  artedge;			/* device pixels; mp3ui_artedge()      */
	short  hasvol;			/* 0 = MP3PLAY has no VOLUME sub-op    */

	/* playlist */
	short  ntracks, sel, top;
	short  ntimed;			/* tracks with a length so far; -1 = not scanning */
	const char *(*name_of)(void *ctx, short i);
	long        (*len_of )(void *ctx, short i);
	void  *ctx;
	const char *dir;

	/* temporary: set MP3UI_DEBUG to show these on the status line */
	long   dbg_ticks, dbg_pos, dbg_status;

	/* interaction */
	short  hover;			/* widget id under the pointer, or -1  */
	short  dragging;		/* 1 while the scrollbar thumb is held */
	short  press;			/* widget id held down, or -1          */

	/* filled in by mp3ui_layout() */
	GRECT  work;
	GRECT  clip;			/* what the current redraw may touch  */
	APJ_LAY lay[MP3UI_MAXLAY];
	short  nlay;
	short  rowh, visrows;
} MP3UI;

void  mp3ui_layout(MP3UI *u, short vh, short wx, short wy, short ww, short wh);
void  mp3ui_draw  (MP3UI *u, short vh);
/* the same, but only the parts that meet clip - what WM_REDRAW wants */
void  mp3ui_draw_clip(MP3UI *u, short vh, const GRECT *clip);
void  mp3ui_draw_band(MP3UI *u, short vh);	/* seek + transport       */
void  mp3ui_draw_clock(MP3UI *u, short vh);	/* the seek strip only    */
void  mp3ui_draw_status(MP3UI *u, short vh);	/* the bottom line only   */
void  mp3ui_draw_list(MP3UI *u, short vh);	/* the playlist only      */

/* the seek strip's rectangle, for a redraw of just that */
void  mp3ui_clock_rect(MP3UI *u, GRECT *r);
short mp3ui_hit   (MP3UI *u, short mx, short my);
short mp3ui_row_at(MP3UI *u, short my);		/* playlist row, or -1   */

/* the scrollbar: is it needed, where is the thumb, and what does a click
 * at my mean - -1 above the thumb, 0 on it, +1 below it */
short mp3ui_scroll_needed(MP3UI *u);
void  mp3ui_thumb_rect(MP3UI *u, GRECT *r);
short mp3ui_scroll_part(MP3UI *u, short my);
/* thumb dragged so its top is at my: the ui.top that corresponds */
short mp3ui_scroll_top_for(MP3UI *u, short my, short grab);

/* smallest window the layout still works in, in device pixels */
void  mp3ui_minsize(short *w, short *h);

/* the strip the pointer has to be in for hover to matter - what the app
 * arms MU_M1 with */
void  mp3ui_bbox(MP3UI *u, GRECT *r);

/* the album-art tile's edge in device pixels - what the app should ask
 * MP3PLAY sub-op 9 to decode into, and how big artbuf has to be */
short mp3ui_artedge(void);

#endif /* MP3UI_H */
