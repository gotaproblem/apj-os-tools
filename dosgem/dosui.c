/*
 * dosui.c - DOSGEM's drawing: one vro_cpyfm of the DOS screen buffer, or
 * a fill and one line while there is nothing to show. See dosui.h.
 */
#include <stdio.h>
#include <string.h>

#include "../apjgui/apjgui.h"
#include "dosui.h"

void dosui_layout(DOSUI *u, short x, short y, short w, short h)
{
	u->work.g_x = x;
	u->work.g_y = y;
	u->work.g_w = w;
	u->work.g_h = h;
	u->clip = u->work;
}

void dosui_view_rect(const DOSUI *u, GRECT *r)
{
	*r = u->work;
}

void dosui_band_rect(const DOSUI *u, short y, short h, GRECT *r)
{
	r->g_x = u->work.g_x;
	r->g_w = u->work.g_w;
	r->g_y = (short) (u->work.g_y + y);
	r->g_h = h;
	if (y < 0)
	{
		r->g_h = (short) (r->g_h + y);
		r->g_y = u->work.g_y;
	}
	if (r->g_y + r->g_h > u->work.g_y + u->work.g_h)
		r->g_h = (short) (u->work.g_y + u->work.g_h - r->g_y);
	if (r->g_h < 0)
		r->g_h = 0;
}

static const char *message(const DOSUI *u)
{
	switch (u->msg)
	{
		case DOS_MSG_WAIT:   return (u->waittext && u->waittext[0]) ? u->waittext
		                                                          : "Starting psdos on the Pi...";
		case DOS_MSG_NOTT:   return "No TT-RAM for the DOS screen buffer";
		case DOS_MSG_NOCORE: return "psdos has no DOSBox Pure core - run install-full.sh with DOS=1";
		case DOS_MSG_FAILED: return "DOSBox Pure could not load that - Alt+O to open another";
		case DOS_MSG_EXITED: return "The DOS machine has shut down - Alt+O or Alt+R";
		default:             return "";
	}
}

void dosui_draw(DOSUI *u, short vh)
{
	GRECT v = u->work, c = v;
	const char *m;

	if (c.g_x < u->clip.g_x) { c.g_w = (short) (c.g_w - (u->clip.g_x - c.g_x)); c.g_x = u->clip.g_x; }
	if (c.g_y < u->clip.g_y) { c.g_h = (short) (c.g_h - (u->clip.g_y - c.g_y)); c.g_y = u->clip.g_y; }
	if (c.g_x + c.g_w > u->clip.g_x + u->clip.g_w) c.g_w = (short) (u->clip.g_x + u->clip.g_w - c.g_x);
	if (c.g_y + c.g_h > u->clip.g_y + u->clip.g_h) c.g_h = (short) (u->clip.g_y + u->clip.g_h - c.g_y);
	if (c.g_w <= 0 || c.g_h <= 0)
		return;

	if (u->msg == DOS_MSG_NONE && u->buf.fd_addr)
	{
		MFDB scr;
		short pxy[8];

		/* the buffer covers the view exactly (width rounded up to 16) */
		if (c.g_x + c.g_w > v.g_x + u->buf.fd_w) c.g_w = (short) (v.g_x + u->buf.fd_w - c.g_x);
		if (c.g_y + c.g_h > v.g_y + u->buf.fd_h) c.g_h = (short) (v.g_y + u->buf.fd_h - c.g_y);
		if (c.g_w <= 0 || c.g_h <= 0)
			return;
		scr.fd_addr = NULL;
		pxy[0] = (short) (c.g_x - v.g_x);
		pxy[1] = (short) (c.g_y - v.g_y);
		pxy[2] = (short) (pxy[0] + c.g_w - 1);
		pxy[3] = (short) (pxy[1] + c.g_h - 1);
		pxy[4] = c.g_x;
		pxy[5] = c.g_y;
		pxy[6] = (short) (c.g_x + c.g_w - 1);
		pxy[7] = (short) (c.g_y + c.g_h - 1);
		vro_cpyfm(vh, S_ONLY, pxy, &u->buf, &scr);
		return;
	}

	/* no frame: black, like the DOS screen it stands for, and one line */
	apj_fill(vh, c.g_x, c.g_y, c.g_w, c.g_h, 1);   /* VDI pen 1: black */
	m = message(u);
	if (m && m[0])
	{
		short cw, ch, d, tw, x, y;
		char line[120];

		vst_point(vh, 10, &cw, &ch, &d, &d);
		strncpy(line, m, sizeof line - 1);
		line[sizeof line - 1] = '\0';
		/* a line wider than the view is cut to fit, never run past it */
		if (cw > 0 && (long) strlen(line) * cw > v.g_w - 2 * cw)
		{
			long n = (v.g_w - 2 * cw) / cw;

			line[n > 0 ? n : 0] = '\0';
		}
		tw = (short) (strlen(line) * cw);
		x = (short) (v.g_x + (v.g_w - tw) / 2);
		y = (short) (v.g_y + (v.g_h - ch) / 2);
		apj_text(vh, x, y, apj_pen(APJ_R_DISABLED), line);
	}
}
