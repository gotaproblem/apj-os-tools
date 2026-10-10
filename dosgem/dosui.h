/*
 * dosui.h - DOSGEM's window: the DOS screen, full bleed.
 *
 * Split out of dosgem.c so the drawing builds on the host by tests/dos
 * (the apjskin-gem-app rule). Nothing here talks to the PSDOS NatFeat: the
 * app fills in DOSUI, calls dosui_layout(), and draws. The view IS the
 * work area: psdos scales the DOS screen into it (aspect kept, black
 * borders drawn by psdos), the emulator copies the changed band into a
 * TT-RAM buffer the app owns (PSDOS FETCH), and dosui_draw() puts the part
 * of that buffer that meets the clip on the screen with one vro_cpyfm.
 * With no frame yet it fills the view and says why in one centred line.
 * The status (capture, pause, title) lives in the window's info line, so
 * there is no toolbar and no skin sheet.
 */

#ifndef DOSUI_H
#define DOSUI_H

#include <gem.h>

/* what the view says when there is no frame to show */
enum
{
	DOS_MSG_NONE,		/* the DOS screen                                */
	DOS_MSG_WAIT,		/* waittext, else "Starting psdos on the Pi..."  */
	DOS_MSG_NOTT,		/* no TT-RAM for the screen buffer               */
	DOS_MSG_NOCORE,		/* psdos has no DOSBox Pure core                 */
	DOS_MSG_FAILED,		/* the game would not load                       */
	DOS_MSG_EXITED		/* the DOS machine shut down                     */
};

typedef struct
{
	GRECT  work;		/* the window's work area                        */
	GRECT  clip;		/* what the current draw may touch               */
	MFDB   buf;		/* the TT-RAM screen buffer; fd_addr NULL = none  */
	short  msg;		/* DOS_MSG_*                                      */
	const char *waittext;	/* the link's own words while waiting            */
} DOSUI;

void dosui_layout(DOSUI *u, short x, short y, short w, short h);
void dosui_view_rect(const DOSUI *u, GRECT *r);
/* draw what meets u->clip: the buffer, or the fill and the message */
void dosui_draw(DOSUI *u, short vh);
/* the view rectangle of a band copied from view row y, h rows */
void dosui_band_rect(const DOSUI *u, short y, short h, GRECT *r);

#endif /* DOSUI_H */
