/*
 * ps3d.c - see ps3d.h. The rasteriser, with no GEM and no VDI in it, so
 * the host harness runs the same code the Atari runs.
 *
 * Fixed point throughout: 16.16 for the transform, and the edge walk is
 * integer with one divide per edge set-up and none in the span loop.
 * Deliberately plain - this is a benchmark, so it must be the SAME work
 * every time on every machine, not the fastest possible renderer. In
 * particular there is no early-out on small triangles and no adaptive
 * anything: two runs must differ only in how long they took.
 */

#include <string.h>

#include "ps3d.h"

#define FX	16			/* fixed point fraction bits */
#define ONE	(1L << FX)

/* ------------------------------------------------------------- maths -- */

/*
 * Sine as a table, 256 steps of a full turn, 16.16. Built once, by the
 * only floating point in this file - and only if the host has it. On the
 * Atari the table is const data, computed at build time by the same
 * expression, so no FPU is touched at run time.
 */
static const short sintab[64] = {	/* quarter turn, 0..16384 = 1.0 */
	    0,   402,   804,  1205,  1606,  2006,  2404,  2801,
	 3196,  3590,  3981,  4370,  4756,  5139,  5520,  5897,
	 6270,  6639,  7005,  7366,  7723,  8076,  8423,  8765,
	 9102,  9434,  9760, 10080, 10394, 10702, 11003, 11297,
	11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395,
	13623, 13842, 14053, 14256, 14449, 14635, 14811, 14978,
	15137, 15286, 15426, 15557, 15679, 15791, 15893, 15986,
	16069, 16143, 16207, 16261, 16305, 16340, 16364, 16379
};

/* 8.14 fixed sine, argument 0..255 = a full turn */
static long fsin(short a)
{
	a &= 255;
	if (a < 64)  return  sintab[a];
	if (a < 128) return  sintab[127 - a + 1 > 63 ? 63 : 127 - a];
	if (a < 192) return -sintab[a - 128];
	return -sintab[255 - a + 1 > 63 ? 63 : 255 - a];
}

static long fcos(short a) { return fsin((short) (a + 64)); }

/* ------------------------------------------------------------ the model */

/*
 * An icosahedron subdivided once: 42 vertices, 80 faces. Coordinates are
 * 8.8 fixed on a unit sphere, generated once and baked in so the model is
 * bit-identical on every machine and every build.
 */
#include "ps3d_model.h"

/* --------------------------------------------------------- rasterising -- */

/*
 * A span, in whatever the screen's pixel size is. Three loops rather
 * than one general one: this is the innermost thing in the benchmark and
 * a per-pixel switch would be measuring the switch.
 */
static void span(void *buf, short bpp, long off, short n, long c)
{
	short i;

	if (n <= 0)
		return;
	if (bpp == 8)
	{
		memset((unsigned char *) buf + off, (int) (c & 0xFF), (size_t) n);
	}
	else if (bpp == 16)
	{
		unsigned short *p = (unsigned short *) buf + off;
		unsigned short v = (unsigned short) c;

		for (i = 0; i < n; i++)
			p[i] = v;
	}
	else
	{
		/*
		 * unsigned INT, not unsigned long. Both are 32 bits on
		 * m68k-atari-mint, but long is 64 on the host this is
		 * tested on, and writing eight bytes into a four-byte
		 * pixel walks straight off the end of the row. The same
		 * trap as CoreMark's ee_u32, caught the same way.
		 */
		unsigned int *p = (unsigned int *) buf + off;
		unsigned int v = (unsigned int) c;

		for (i = 0; i < n; i++)
			p[i] = v;
	}
}

/*
 * Flat-shaded triangle, integer edge walk, top-down. Spans are clipped
 * to the buffer here rather than trusted to the projection: the harness
 * checks the guarantee holds, and a span walking off the end of a row on
 * the Atari lands in somebody else's memory rather than crashing.
 */
static long tri(void *buf, short stride, short bpp, short w, short h,
                short x0, short y0, short x1, short y1,
                short x2, short y2, long c)
{
	short t;
	long dx01, dx02, dx12, xa, xb;
	short y;
	long n = 0;

	/* sort by y */
	if (y0 > y1) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
	if (y0 > y2) { t = y0; y0 = y2; y2 = t; t = x0; x0 = x2; x2 = t; }
	if (y1 > y2) { t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }

	if (y2 == y0)
		return 0;

	dx02 = ((long) (x2 - x0) << FX) / (y2 - y0);
	dx01 = (y1 != y0) ? ((long) (x1 - x0) << FX) / (y1 - y0) : 0;
	dx12 = (y2 != y1) ? ((long) (x2 - x1) << FX) / (y2 - y1) : 0;

	xa = xb = (long) x0 << FX;

	for (y = y0; y < y2; y++)
	{
		long l, r;
		short xl, xr;

		if (y == y1)
			xb = (long) x1 << FX;

		l = (xa < xb) ? xa : xb;
		r = (xa < xb) ? xb : xa;
		xl = (short) (l >> FX);
		xr = (short) (r >> FX);

		if (y >= 0 && y < h)
		{
			if (xl < 0) xl = 0;
			if (xr >= w) xr = (short) (w - 1);
			if (xr >= xl)
			{
				span(buf, bpp, (long) y * stride + xl,
				     (short) (xr - xl + 1), c);
				n += xr - xl + 1;
			}
		}

		xa += dx02;
		xb += (y < y1) ? dx01 : dx12;
	}
	return n;
}

/* ------------------------------------------------------------- a frame -- */

long ps3d_frame(void *buf, short w, short h, short stride, short bpp,
                short angle, const long *shade16, long *tris, long *drawn)
{
	static short px[PS3D_NVERT], py[PS3D_NVERT], pz[PS3D_NVERT];
	long sa = fsin(angle), ca = fcos(angle);
	long sb = fsin((short) (angle * 2 / 3)), cb = fcos((short) (angle * 2 / 3));
	short i;
	short cx = (short) (w / 2), cy = (short) (h / 2);
	short scale = (short) ((w < h ? w : h) * 7 / 20);
	long npix = 0;

	*tris = 0;
	*drawn = 0;

	if (!buf || !shade16 || stride < w || w <= 0 || h <= 0)
		return 0;
	if (bpp != 8 && bpp != 16 && bpp != 32)
		return 0;

	/*
	 * Transform. Two rotations, one divide-free perspective divide
	 * avoided entirely by using a fixed camera distance and a shift -
	 * a real renderer would divide, but a benchmark wants the same
	 * instruction mix on every machine and the 68000 divide is slow
	 * enough to dominate everything else if it is in here.
	 */
	for (i = 0; i < PS3D_NVERT; i++)
	{
		long x = ps3d_vx[i], y = ps3d_vy[i], z = ps3d_vz[i];
		long x1, z1, y2, z2;

		/* about Y */
		x1 = (x * ca - z * sa) >> 14;
		z1 = (x * sa + z * ca) >> 14;
		/* about X */
		y2 = (y * cb - z1 * sb) >> 14;
		z2 = (y * sb + z1 * cb) >> 14;

		px[i] = (short) (cx + ((x1 * scale) >> 8));
		py[i] = (short) (cy - ((y2 * scale) >> 8));
		pz[i] = (short) z2;
	}

	for (i = 0; i < PS3D_NTRI; i++)
	{
		short a = ps3d_fa[i], b = ps3d_fb[i], c = ps3d_fc[i];
		long cross;
		short shade;

		(*tris)++;

		/* backface cull: the sign of the 2D cross product */
		cross = (long) (px[b] - px[a]) * (py[c] - py[a]) -
		        (long) (py[b] - py[a]) * (px[c] - px[a]);
		if (cross <= 0)
			continue;

		(*drawn)++;

		/*
		 * Shade from the face's average z, sixteen steps, so the
		 * palette is sixteen entries and the same code serves 8 bpp
		 * and true colour after a lookup.
		 *
		 * The visible hemisphere is the one with NEGATIVE z after the
		 * cull keeps cross > 0, so it is (256 - z), not (256 + z) -
		 * the other way round lit the half you cannot see and drew
		 * the sphere almost black.
		 */
		shade = (short) ((256 - (pz[a] + pz[b] + pz[c]) / 3) >> 5);
		if (shade < 2)  shade = 2;	/* never pure background */
		if (shade > 15) shade = 15;

		npix += tri(buf, stride, bpp, w, h, px[a], py[a], px[b], py[b],
		            px[c], py[c], shade16[shade]);
	}
	return npix;
}
