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
	W_ART, W_OPEN, W_LIST,
	W_N
};

#define MP3UI_MAXLAY	(W_N + 2)

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
	const char *(*name_of)(void *ctx, short i);
	long        (*len_of )(void *ctx, short i);
	void  *ctx;
	const char *dir;

	/* temporary: set MP3UI_DEBUG to show these on the status line */
	long   dbg_ticks, dbg_pos, dbg_status;

	/* interaction */
	short  hover;			/* widget id under the pointer, or -1  */
	short  press;			/* widget id held down, or -1          */

	/* filled in by mp3ui_layout() */
	GRECT  work;
	APJ_LAY lay[MP3UI_MAXLAY];
	short  nlay;
	short  rowh, visrows;
} MP3UI;

void  mp3ui_layout(MP3UI *u, short vh, short wx, short wy, short ww, short wh);
void  mp3ui_draw  (MP3UI *u, short vh);
void  mp3ui_draw_band(MP3UI *u, short vh);	/* seek + transport only */
short mp3ui_hit   (MP3UI *u, short mx, short my);
short mp3ui_row_at(MP3UI *u, short my);		/* playlist row, or -1   */

/* smallest window the layout still works in, in device pixels */
void  mp3ui_minsize(short *w, short *h);

/* the strip the pointer has to be in for hover to matter - what the app
 * arms MU_M1 with */
void  mp3ui_bbox(MP3UI *u, GRECT *r);

/* the album-art tile's edge in device pixels - what the app should ask
 * MP3PLAY sub-op 9 to decode into, and how big artbuf has to be */
short mp3ui_artedge(void);

#endif /* MP3UI_H */
