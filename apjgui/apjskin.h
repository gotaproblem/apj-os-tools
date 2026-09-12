/*
 * apjskin.h - APJSKIN, the bitmap-skin engine for the PiSTorm's GEM tools
 *
 * A skin is one pre-composited 32-bit sheet plus a table of source
 * rectangles. Drawing a control is vro_cpyfm(S_ONLY) out of that sheet:
 * with an MFDB in device format fVDI does the copy host-side, so the 68k
 * issues one trap per rectangle and never touches a pixel.
 *
 * Everything in the sheet is already composited against the surface it
 * will land on, so there is no runtime alpha. The single exception is
 * apj_skin_glyph(), which blends an 8-bit coverage map for the case of a
 * glyph over the video overlay plane - use it sparingly.
 *
 *   apj_skin_load(vh, NULL)      pick the skin from the AES theme and the
 *                                screen width, load it, register its pens
 *   apj_skin_ok()                0 = no skin; every draw call is then a
 *                                no-op and the caller falls back to apjgui
 *   apj_skin_m(pt)               points -> device pixels at this scale
 *   apj_skin_blit / _9 / _tilex  the three blit shapes
 *   apj_skin_tile / _tileacc     a transport button, plate and glyph in one
 *   apj_skin_glyph               coverage blend (the slow path)
 *   apj_lay_hit                  hit-test a layout table
 *
 * The region and glyph ids below are the contract with skins/mkskin.py
 * and skins/glyphs/order.txt - keep all three in step.
 */

#ifndef APJSKIN_H
#define APJSKIN_H

#include <gem.h>
#include "apjgui.h"

/* ------------------------------------------------------------ regions -- */
enum
{
	APJ_RG_PANELTOP,	/* mica band, tiled across the top of the body   */
	APJ_RG_GROUP,		/* inset surface: playlist, art tile, video frame */
	APJ_RG_ROWSEL,		/* selected list row + accent rail                */
	APJ_RG_SEEK,		/* 3 states: track, buffered, fill                */
	APJ_RG_KNOB,		/* 3 states: normal, hover, pressed               */
	APJ_RG_BADGE,		/* 2 states: plain, accent                        */
	APJ_RG_ARTPH,		/* album-art placeholder                          */
	APJ_RG_BTN,		/* plain plate for a labelled button, 4 states    */
	APJ_RG_BTNACC,		/* round accent plate, 4 states                   */
	APJ_RG_TILE,		/* plate + glyph, APJ_G_* x 4 states              */
	APJ_RG_TILEACC,		/* round accent plate + glyph, 5 glyphs x 4       */
	APJ_RG_N
};

/* ------------------------------------------------------------- glyphs -- */
enum
{
	APJ_G_PLAY, APJ_G_PAUSE, APJ_G_STOP, APJ_G_PREV, APJ_G_NEXT,
	APJ_G_RW, APJ_G_FF, APJ_G_SHUFFLE, APJ_G_REPEAT, APJ_G_REPEAT1,
	APJ_G_VOL, APJ_G_VOLLOW, APJ_G_MUTE, APJ_G_OPEN, APJ_G_LIST,
	APJ_G_FULL, APJ_G_UNFULL, APJ_G_EJECT, APJ_G_INFO,
	APJ_G_AUDIO, APJ_G_VIDEO, APJ_G_MIN, APJ_G_MAX, APJ_G_CLOSE,
	APJ_G_N
};

#define APJ_G_NTILE	19	/* glyphs 0..18 have a TILE    */
#define APJ_G_NACC	5	/* glyphs 0..4  have a TILEACC */

/* --------------------------------------------------------- states etc -- */
enum { APJ_ST_NORM, APJ_ST_HOVER, APJ_ST_PRESS, APJ_ST_ON, APJ_ST_N };
enum { APJ_SK_TRACK, APJ_SK_BUF, APJ_SK_FILL };

/*
 * Extra palette entries the skin carries beyond the nineteen APJ_R_*
 * roles. apj_skin_pen() takes either: roles use apjgui's 237..255 block,
 * these use 231..236 just below it (236 is XaAES's probe scratch pen, so
 * the block stops at 235... see apjskin.c).
 */
enum
{
	APJ_X_ACCENT_INK = APJ_R_N,
	APJ_X_ACCENT_DEEP,
	APJ_X_MUTED,
	APJ_X_PANEL_TOP,
	APJ_X_ROW,
	APJ_X_HAIR,
	APJ_X_N
};

/* ---------------------------------------------------------------- api -- */
short apj_skin_load(short vh, const char *name);	/* NULL = follow the theme */
short apj_skin_reload(short vh);			/* after APJ_SKINCHG       */
void  apj_skin_free(void);
short apj_skin_ok(void);

/* After a failed load: the file it wanted and the folders it looked in,
 * so an app can say so on screen instead of silently looking unchanged. */
const char *apj_skin_wanted(void);
const char *apj_skin_tried(void);

short apj_skin_scale(void);				/* 100, 125, 175           */
short apj_skin_m(short pt);				/* points -> device pixels */
short apj_skin_tilew(void);
short apj_skin_tileh(void);
short apj_skin_accd(void);				/* round button diameter   */
short apj_skin_glyphsz(void);

long  apj_skin_rgb(short role);				/* 0x00RRGGBB, -1 if none  */
short apj_skin_pen(short role);				/* VDI pen for a role      */

void  apj_skin_blit (short vh, short rid, short state, short x, short y);
void  apj_skin_9    (short vh, short rid, short state,
                     short x, short y, short w, short h);
void  apj_skin_tilex(short vh, short rid, short state,
                     short x, short y, short w);
void  apj_skin_tile   (short vh, short g, short state, short x, short y);
void  apj_skin_tileacc(short vh, short g, short state, short x, short y);
void  apj_skin_glyph  (short vh, short g, short pen, short x, short y);

/* text in a skinned window - use this, not apj_text(), so a skin that does
 * not match the desktop theme still comes out readable */
void  apj_skin_text   (short vh, short x, short y, short pen, const char *s);

/* size of one state of a region, in device pixels */
void  apj_skin_size(short rid, short *w, short *h);

/* ------------------------------------------------------------- layout -- */
/*
 * The app builds this at startup from its own metrics (apj_skin_m) and
 * hands it to apj_lay_hit() instead of writing btn_rect()/click() by hand.
 * id is the app's own widget id; -1 terminates a static table.
 */
typedef struct
{
	short id;
	short x, y, w, h;
} APJ_LAY;

short apj_lay_hit(const APJ_LAY *t, short n, short mx, short my);
const APJ_LAY *apj_lay_find(const APJ_LAY *t, short n, short id);

/* ------------------------------------------------------- theme change -- */
/*
 * XaAES broadcasts this when the desktop commits a theme (appl_control
 * opcode 113) or the skin on disk changes: msg[0] = APJ_SKINCHG. On a
 * stock XaAES it never arrives and nothing is lost.
 */
#define APJ_SKINCHG	0x4A50

#endif /* APJSKIN_H */
