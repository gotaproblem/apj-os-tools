/*
 * apjgui.h - APJ-OS Fluent look for the PiSTorm's own GEM tools
 *
 * Header-only. For a GEM program that draws its whole UI itself on its
 * own VDI workstation (MP3GEM, VIDGEM, PSMON): gives it the desktop's
 * theme colours, antialiased text, and flat controls, through the
 * bespoke XaAES appl_control opcodes. On a stock or older XaAES every
 * call degrades to the plain VDI look the program had before.
 *
 *   apj_init(vh)          once, after v_opnvwk() and BEFORE the first
 *                         wind_create(): opts into the APJ renderer
 *                         (window chrome), fetches the theme, loads it
 *                         into this workstation's colour registers
 *   apj_pen(APJ_R_x)      a VDI colour index for a role (falls back to
 *                         the standard pens when there is no theme)
 *   apj_clip(vh, r)/apj_clip_off(vh)
 *                         use instead of vs_clip so text drawn by XaAES
 *                         on your behalf is clipped the same way
 *   apj_text(vh,x,y,pen,s)  antialiased text, (x,y) = cell top-left
 *                         (vst_alignment 0,5 assumed - set it once);
 *                         apj_text_aa() draws only if XaAES can, else 0
 *   apj_fill / apj_box / apj_button / apj_select
 *                         flat controls
 *
 * Opcodes: 110 attach, 111-113 theme (desktop only), 114 text, 115 query.
 * Role order is the contract with XaAES render_apj.h / TeraDesk btheme.h.
 */

#ifndef APJGUI_H
#define APJGUI_H

#include <gem.h>
#include <string.h>

enum
{
	APJ_R_FACE, APJ_R_TEXT, APJ_R_LIGHT, APJ_R_DARK, APJ_R_SELBG, APJ_R_SELFG,
	APJ_R_ALBG, APJ_R_ALFG, APJ_R_PANEL, APJ_R_TITBG, APJ_R_TITFG, APJ_R_PAPER,
	APJ_R_BORDER, APJ_R_HOVER, APJ_R_PRESSED, APJ_R_FOCUS, APJ_R_DISABLED,
	APJ_R_ELEVATION, APJ_R_ACCENT,
	APJ_R_N
};

#define APJ_PEN_BASE	(256 - APJ_R_N)		/* 237..255, as the desktop */


/*
 * One translation unit per program defines APJGUI_IMPL before including
 * this header (that instantiates the state and the functions); every
 * other unit just includes it. A single-file program defines it too.
 */

extern short apj_on;					/* 1 = theme loaded, pens valid */
extern short apj_attached;				/* 1 = XaAES draws our objects/chrome */

short apj_pen(short role);
void  apj_init(short vh);
void  apj_clip(short vh, short x, short y, short w, short h);
void  apj_clip_off(short vh);
void  apj_clip_note(short x, short y, short w, short h, short on);
short apj_text_aa(short vh, short x, short y, short pen, const char *s);
short apj_text(short vh, short x, short y, short pen, const char *s);
void  apj_fill(short vh, short x, short y, short w, short h, short pen);
void  apj_line(short vh, short x1, short y1, short x2, short y2, short pen);
void  apj_box(short vh, short x, short y, short w, short h, short fill, short border);
void  apj_button(short vh, short x, short y, short w, short h, const char *label, short pressed, short def);
void  apj_select(short vh, short x, short y, short w, short h);

#ifdef APJGUI_IMPL
short apj_on = 0;					/* 1 = theme loaded, pens valid */
short apj_attached = 0;				/* 1 = XaAES draws our objects/chrome */
long  apj_rgb[APJ_R_N];
short apj_clip_rect[4];
short apj_clip_on = 0;

/* fallback pens for the classic look */
const short apj_fb[APJ_R_N] =
{
	G_WHITE, G_BLACK, G_WHITE, G_LBLACK, G_BLACK, G_WHITE,
	G_RED,   G_WHITE, G_LWHITE, G_LBLACK, G_WHITE, G_WHITE,
	G_LBLACK, G_WHITE, G_LBLACK, G_BLACK, G_LBLACK,
	G_LBLACK, G_BLACK
};

short apj_pen(short role)
{
	if (role < 0 || role >= APJ_R_N)
		return G_BLACK;
	return apj_on ? (short) (APJ_PEN_BASE + role) : apj_fb[role];
}

void apj_init(short vh)
{
	short i;

	apj_attached = (appl_control(-1, 110, NULL) != 0) ? 1 : 0;

	if (appl_control(-1, 115, apj_rgb) != APJ_R_N)
	{
		apj_on = 0;
		return;
	}

	for (i = 0; i < APJ_R_N; i++)
	{
		short rgb[3];

		rgb[0] = (short) ((((apj_rgb[i] >> 16) & 0xff) * 1000L + 127L) / 255L);
		rgb[1] = (short) ((((apj_rgb[i] >>  8) & 0xff) * 1000L + 127L) / 255L);
		rgb[2] = (short) ((( apj_rgb[i]        & 0xff) * 1000L + 127L) / 255L);
		vs_color(vh, APJ_PEN_BASE + i, rgb);
	}
	apj_on = 1;
}

void apj_clip(short vh, short x, short y, short w, short h)
{
	apj_clip_rect[0] = x;
	apj_clip_rect[1] = y;
	apj_clip_rect[2] = x + w - 1;
	apj_clip_rect[3] = y + h - 1;
	apj_clip_on = 1;
	vs_clip(vh, 1, apj_clip_rect);
}

void apj_clip_off(short vh)
{
	short d[4];

	apj_clip_on = 0;
	vs_clip(vh, 0, d);
}

/* for programs that set the VDI clip through some other wrapper (cflib
 * set_clipping etc): only record what it is, do not touch the VDI */
void apj_clip_note(short x, short y, short w, short h, short on)
{
	if (on)
	{
		apj_clip_rect[0] = x;
		apj_clip_rect[1] = y;
		apj_clip_rect[2] = x + w - 1;
		apj_clip_rect[3] = y + h - 1;
	}
	apj_clip_on = on ? 1 : 0;
}

/* layout is the contract with XaAES render_apj.h - keep identical */
typedef struct
{
	short x, y;
	short pen;
	short cw, ch;
	short clip[4];
	const char *s;
} APJ_TEXTREQ;

/* XaAES draws it antialiased, or returns 0 and draws nothing - for a
 * caller with its own text alignment / fallback (a terminal) */
short apj_text_aa(short vh, short x, short y, short pen, const char *s)
{
	if (apj_on)
	{
		APJ_TEXTREQ rq;
		short attr[10], wo[57];

		vqt_attributes(vh, attr);	/* [8] cell w, [9] cell h */
		rq.x = x;
		rq.y = y;
		rq.pen = pen;
		rq.cw = attr[8];
		rq.ch = attr[9];
		rq.s = s;
		if (apj_clip_on)
		{
			rq.clip[0] = apj_clip_rect[0];
			rq.clip[1] = apj_clip_rect[1];
			rq.clip[2] = apj_clip_rect[2];
			rq.clip[3] = apj_clip_rect[3];
		}
		else
		{
			vq_extnd(vh, 0, wo);
			rq.clip[0] = 0;
			rq.clip[1] = 0;
			rq.clip[2] = wo[0];
			rq.clip[3] = wo[1];
		}
		if (appl_control(-1, 114, &rq) != 0)
			return 1;
	}
	return 0;
}

/* antialiased if possible, else v_gtext with (x,y) as the cell top */
short apj_text(short vh, short x, short y, short pen, const char *s)
{
	if (apj_text_aa(vh, x, y, pen, s))
		return 1;
	vswr_mode(vh, MD_TRANS);
	vst_color(vh, pen);
	v_gtext(vh, x, y, (char *) s);
	return 1;
}

void apj_fill(short vh, short x, short y, short w, short h, short pen)
{
	short xy[4];

	vswr_mode(vh, MD_REPLACE);
	vsf_interior(vh, FIS_SOLID);
	vsf_perimeter(vh, 0);
	vsf_color(vh, pen);
	xy[0] = x; xy[1] = y; xy[2] = x + w - 1; xy[3] = y + h - 1;
	v_bar(vh, xy);
}

void apj_line(short vh, short x1, short y1, short x2, short y2, short pen)
{
	short xy[4];

	vsl_color(vh, pen);
	xy[0] = x1; xy[1] = y1; xy[2] = x2; xy[3] = y2;
	v_pline(vh, 2, xy);
}

/* flat box: fill + 1px border, corner pixels skipped (reads as rounded) */
void apj_box(short vh, short x, short y, short w, short h, short fill, short border)
{
	short x2 = x + w - 1, y2 = y + h - 1;
	short c = (w > 2 && h > 2) ? 1 : 0;

	apj_fill(vh, x, y, w, h, fill);
	vswr_mode(vh, MD_REPLACE);
	apj_line(vh, x + c, y, x2 - c, y, border);
	apj_line(vh, x + c, y2, x2 - c, y2, border);
	apj_line(vh, x, y + c, x, y2 - c, border);
	apj_line(vh, x2, y + c, x2, y2 - c, border);
}

/* a push button; label centred. pressed/def as the AES draws them */
void apj_button(short vh, short x, short y, short w, short h, const char *label, short pressed, short def)
{
	short attr[10], tw, fill, fg, border;

	vqt_attributes(vh, attr);
	if (pressed)
	{
		fill = def ? apj_pen(APJ_R_SELBG) : apj_pen(APJ_R_PRESSED);
		fg   = def ? apj_pen(APJ_R_SELFG) : apj_pen(APJ_R_TEXT);
	}
	else if (def)
	{
		fill = apj_pen(APJ_R_ACCENT);
		fg   = apj_pen(APJ_R_SELFG);
	}
	else
	{
		fill = apj_pen(APJ_R_FACE);
		fg   = apj_pen(APJ_R_TEXT);
	}
	border = apj_on ? apj_pen(APJ_R_BORDER) : G_BLACK;

	apj_box(vh, x, y, w, h, fill, border);
	tw = (short) strlen(label) * attr[8];
	apj_text(vh, x + (w - tw) / 2, y + (h - attr[9]) / 2, fg, label);
}

/* a selected list row: selection fill, then the caller draws the text
 * in apj_pen(APJ_R_SELFG) - instead of XOR-inverting the row */
void apj_select(short vh, short x, short y, short w, short h)
{
	apj_fill(vh, x, y, w, h, apj_pen(APJ_R_SELBG));
}


#endif /* APJGUI_IMPL */

#endif /* APJGUI_H */
