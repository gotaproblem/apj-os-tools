/*
 * tests/skin/stub/gem.h - just enough GEM to build apjskin.c and the UI
 * drawing on the host. NOT a GEM implementation: the VDI calls the skin
 * engine makes are recorded or emulated by harness.c against a fake
 * 32 bpp screen, so the blit arithmetic can be checked against
 * skins/preview.py without an Atari.
 */
#ifndef STUB_GEM_H
#define STUB_GEM_H

#include <stddef.h>
#include <string.h>

typedef struct { short g_x, g_y, g_w, g_h; } GRECT;

typedef struct
{
	void  *fd_addr;
	short  fd_w, fd_h, fd_wdwidth, fd_stand, fd_nplanes;
	short  fd_r1, fd_r2, fd_r3;
} MFDB;

#define S_ONLY		3
#define MD_REPLACE	1
#define MD_TRANS	2
#define FIS_SOLID	1

#define G_WHITE 0
#define G_BLACK 1
#define G_RED 2
#define G_GREEN 3
#define G_BLUE 4
#define G_CYAN 5
#define G_YELLOW 6
#define G_MAGENTA 7
#define G_LWHITE 8
#define G_LBLACK 9

extern short gl_apid;

void  vro_cpyfm(short h, short mode, short *pxy, MFDB *src, MFDB *dst);
void  vq_extnd(short h, short flag, short *out);
void  vq_color(short h, short idx, short flag, short *rgb);
void  vs_color(short h, short idx, short *rgb);
void  vs_clip(short h, short on, short *xy);
void  vswr_mode(short h, short m);
void  vsf_interior(short h, short s);
void  vsf_perimeter(short h, short s);
void  vsf_color(short h, short c);
void  vsl_color(short h, short c);
void  vst_color(short h, short c);
void  v_bar(short h, short *xy);
void  v_pline(short h, short n, short *xy);
void  v_gtext(short h, short x, short y, char *s);
void  vqt_attributes(short h, short *a);
short vst_point(short h, short pt, short *cw, short *ch, short *bw, short *bh);
long  appl_control(short ap, short what, void *p);
short shel_read(char *cmd, char *tail);

#endif
