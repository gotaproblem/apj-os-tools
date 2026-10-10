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
	u->view = u->work;
	if (u->view.g_w > DOSUI_MAX_W)
	{
		u->view.g_x = (short) (x + (w - DOSUI_MAX_W) / 2);
		u->view.g_w = DOSUI_MAX_W;
	}
	if (u->view.g_h > DOSUI_MAX_H)
	{
		u->view.g_y = (short) (y + (h - DOSUI_MAX_H) / 2);
		u->view.g_h = DOSUI_MAX_H;
	}
	u->clip = u->work;
}

void dosui_view_rect(const DOSUI *u, GRECT *r)
{
	*r = u->view;
}

void dosui_band_rect(const DOSUI *u, short y, short h, GRECT *r)
{
	const GRECT *v = &u->view;

	r->g_x = v->g_x;
	r->g_w = v->g_w;
	r->g_y = (short) (v->g_y + y);
	r->g_h = h;
	if (y < 0)
	{
		r->g_h = (short) (r->g_h + y);
		r->g_y = v->g_y;
	}
	if (r->g_y + r->g_h > v->g_y + v->g_h)
		r->g_h = (short) (v->g_y + v->g_h - r->g_y);
	if (r->g_h < 0)
		r->g_h = 0;
}

/* r clipped to c; 0 when nothing is left */
static int cut(GRECT *r, const GRECT *c)
{
	short x1 = (short) (r->g_x + r->g_w), y1 = (short) (r->g_y + r->g_h);

	if (r->g_x < c->g_x) r->g_x = c->g_x;
	if (r->g_y < c->g_y) r->g_y = c->g_y;
	if (x1 > c->g_x + c->g_w) x1 = (short) (c->g_x + c->g_w);
	if (y1 > c->g_y + c->g_h) y1 = (short) (c->g_y + c->g_h);
	r->g_w = (short) (x1 - r->g_x);
	r->g_h = (short) (y1 - r->g_y);
	return r->g_w > 0 && r->g_h > 0;
}

/* black around a view smaller than the window (a window over 1920x1080) */
static void fill_margins(DOSUI *u, short vh, const GRECT *c)
{
	GRECT m[4];
	short i;
	const GRECT *w = &u->work, *v = &u->view;

	m[0].g_x = w->g_x; m[0].g_y = w->g_y; m[0].g_w = w->g_w;
	m[0].g_h = (short) (v->g_y - w->g_y);                              /* above */
	m[1].g_x = w->g_x; m[1].g_y = (short) (v->g_y + v->g_h); m[1].g_w = w->g_w;
	m[1].g_h = (short) (w->g_y + w->g_h - m[1].g_y);                   /* below */
	m[2].g_x = w->g_x; m[2].g_y = v->g_y; m[2].g_h = v->g_h;
	m[2].g_w = (short) (v->g_x - w->g_x);                              /* left  */
	m[3].g_x = (short) (v->g_x + v->g_w); m[3].g_y = v->g_y; m[3].g_h = v->g_h;
	m[3].g_w = (short) (w->g_x + w->g_w - m[3].g_x);                   /* right */
	for (i = 0; i < 4; i++)
		if (m[i].g_w > 0 && m[i].g_h > 0 && cut(&m[i], c))
			apj_fill(vh, m[i].g_x, m[i].g_y, m[i].g_w, m[i].g_h, 1);
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
	GRECT v = u->view, c = u->work, wc = u->work;
	const char *m;

	if (!cut(&wc, &u->clip))
		return;                         /* the clip misses the window */
	if (u->msg == DOS_MSG_NONE && u->buf.fd_addr)
	{
		MFDB scr;
		short pxy[8];

		fill_margins(u, vh, &wc);
		c = v;
		if (!cut(&c, &wc))
			return;

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
	c = wc;
	v = u->work;
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
