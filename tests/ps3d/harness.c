/*
 * tests/ps3d/harness.c - the benchmark's rasteriser, on the host.
 *
 *   ./harness [out.pgm]     -> 0 ok, 1 a check failed
 *
 * ps3d.c has no GEM and no VDI in it, so this runs EXACTLY the code the
 * Atari runs. What it checks:
 *
 *   - nothing is written outside the buffer. The buffer is allocated
 *     inside a larger block with a guard pattern either side and under
 *     and over it, and the guards are verified after every frame. A
 *     span loop that walks off the end of a row is the classic way a
 *     software rasteriser corrupts the screen, and on a machine where
 *     the "screen" is the emulator's own framebuffer that is not a
 *     crash, it is a mystery.
 *   - every frame draws something, and the object stays inside the
 *     buffer: the projection is supposed to scale to fit, and the span
 *     loop trusts it.
 *   - the run is DETERMINISTIC. Two passes over the same frames must
 *     produce identical pixel counts and identical buffers, or two
 *     benchmark runs are not comparable and the whole thing is
 *     pointless.
 *   - it survives silly buffer sizes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../psctrl/ps3d.h"

#define GUARD	0x5A
#define PAD	64

static int fails = 0;

static void fail(const char *what, long a, long b)
{
	printf("FAIL %s (%ld, %ld)\n", what, a, b);
	fails++;
}

static long shade[16];

/* a buffer with guard bytes all round it. bytes is the whole picture. */
static unsigned char *mk(long bytes, unsigned char **base)
{
	unsigned char *b = malloc((size_t) (bytes + 2 * PAD));

	memset(b, GUARD, (size_t) (bytes + 2 * PAD));
	*base = b;
	return b + PAD;
}

static int guards_ok(const unsigned char *base, long bytes)
{
	long n = bytes + 2 * PAD;
	long i;

	for (i = 0; i < PAD; i++)
		if (base[i] != GUARD)
			return 0;
	for (i = n - PAD; i < n; i++)
		if (base[i] != GUARD)
			return 0;
	return 1;
}

/* how many distinct pixels changed. This is NOT the count the renderer
 * returns: that one counts overdraw, which is what a fill rate means. */
static long count_changed(const unsigned char *buf, short w, short h,
                          short stride, short px)
{
	long n = 0;
	short x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			const unsigned char *p = buf + ((long) y * stride + x) * px;
			short k;
			int same = 1;

			for (k = 0; k < px; k++)
				if (p[k] != GUARD)
					same = 0;
			if (!same)
				n++;
		}
	return n;
}

/*
 * The bytes between the end of a row and the start of the next. A span
 * that walks off the end of its row lands HERE, not in the guard block,
 * and on the Atari that is somebody else's screen memory rather than a
 * crash - so this is the check that matters most.
 */
static int padding_clean(const unsigned char *buf, short w, short h,
                         short stride, short px)
{
	short x, y, k;

	for (y = 0; y < h; y++)
		for (x = w; x < stride; x++)
		{
			const unsigned char *p = buf + ((long) y * stride + x) * px;

			for (k = 0; k < px; k++)
				if (p[k] != GUARD)
					return 0;
		}
	return 1;
}

/*
 * check_pix distinguishes the two jobs this does. At a usable size the
 * frame must look right: every triangle submitted, some culled, a
 * sensible share of the buffer covered. At an absurd size (1x1) the only
 * question is whether it stays inside memory - the object projects to a
 * point and draws nothing, which is correct, not a failure.
 */
static void run_size(short w, short h, short stride, short bpp, int check_pix)
{
	unsigned char *base, *buf;
	short px = (short) (bpp == 8 ? 1 : bpp == 16 ? 2 : 4);
	long bytes = (long) stride * h * px;
	short f;
	long empty = 0;

	buf = mk(bytes, &base);

	for (f = 0; f < PS3D_TURN; f++)
	{
		long tris = 0, drawn = 0, npix;
		short ang = (short) (f * 256 / PS3D_TURN);

		memset(buf, GUARD, (size_t) bytes);
		npix = ps3d_frame(buf, w, h, stride, bpp, ang, shade,
		                  &tris, &drawn);

		if (!guards_ok(base, bytes))
		{
			fail("the rasteriser wrote outside its buffer", bpp, f);
			break;
		}
		if (tris != PS3D_NTRI)
			fail("wrong triangle count", tris, PS3D_NTRI);
		if (check_pix && (drawn <= 0 || drawn >= tris))
			fail("backface cull kept everything or nothing", drawn, tris);
		if (!padding_clean(buf, w, h, stride, px))
		{
			fail("a span ran past the end of its row", bpp, f);
			break;
		}
		if (check_pix && npix <= 0)
			empty++;
		if (check_pix)
		{
			long chg = count_changed(buf, w, h, stride, px);

			if (npix < chg)
				fail("reported fewer pixels than it changed", npix, chg);
			if (chg < (long) w * h / 50)
				fail("the object nearly vanished", chg, (long) w * h / 50);
			if (chg >= (long) w * h)
				fail("the object filled the whole buffer", chg, (long) w * h);
		}
	}
	if (empty)
		fail("frames drew nothing at all", empty, 0);

	free(base);
}

int main(int argc, char **argv)
{
	unsigned char *base, *buf, *copy;
	long p1 = 0, p2 = 0, bytes;
	short f, k;
	const short W = 160, H = 120, P = 176;	/* stride != width on purpose */
	static const short depths[3] = { 8, 16, 32 };

	for (k = 0; k < 16; k++)
		shade[k] = 0x00112200L + k * 0x00010101L;

	printf("model: %d triangles, %d frames per turn\n", PS3D_NTRI, PS3D_TURN);

	/*
	 * Every depth the screen can be. The renderer writes the screen's
	 * own pixels now - that is what removed a conversion step which
	 * measured at sixty percent of the frame on hardware - so each
	 * depth is a different inner loop and each one can walk off the
	 * end of a row in its own way.
	 */
	for (k = 0; k < 3; k++)
	{
		short bpp = depths[k];

		run_size(W, H, P, bpp, 1);
		run_size(320, 200, 320, bpp, 1);
		run_size(32, 24, 32, bpp, 0);		/* tiny */
		run_size(17, 13, 32, bpp, 0);		/* odd width */
		run_size(1, 1, 16, bpp, 0);		/* absurd */
	}

	/* a depth the screen cannot be must draw nothing, not guess */
	{
		unsigned char *b2, *e = mk(4096, &b2);
		long t, d, got = ps3d_frame(e, W, H, P, 24, 40, shade, &t, &d);

		if (got != 0)
			fail("drew at an impossible depth", got, 0);
		if (!guards_ok(b2, 4096))
			fail("wrote at an impossible depth", 24, 0);
		free(b2);
	}

	/* a stride narrower than the frame must be refused, not overrun */
	{
		unsigned char *b2, *e = mk(4096, &b2);
		long t, d;

		if (ps3d_frame(e, W, H, (short) (W - 1), 32, 40, shade, &t, &d) != 0)
			fail("a stride narrower than the frame was accepted", 0, 0);
		free(b2);
	}

	/* determinism: two passes must agree pixel for pixel */
	bytes = (long) P * H * 4;
	buf = mk(bytes, &base);
	copy = malloc((size_t) bytes);
	for (f = 0; f < PS3D_TURN; f++)
	{
		long t, d;

		memset(buf, 0, (size_t) bytes);
		p1 += ps3d_frame(buf, W, H, P, 32,
		                 (short) (f * 256 / PS3D_TURN), shade, &t, &d);
	}
	memcpy(copy, buf, (size_t) bytes);
	for (f = 0; f < PS3D_TURN; f++)
	{
		long t, d;

		memset(buf, 0, (size_t) bytes);
		p2 += ps3d_frame(buf, W, H, P, 32,
		                 (short) (f * 256 / PS3D_TURN), shade, &t, &d);
	}
	if (p1 != p2)
		fail("two runs wrote different pixel counts", p1, p2);
	if (memcmp(copy, buf, (size_t) bytes))
		fail("two runs produced different pictures", 0, 0);
	printf("%ld pixels per turn of %d frames at %dx%d\n", p1, PS3D_TURN, W, H);

	/*
	 * The three depths must draw the SAME SHAPE. They are three
	 * separate inner loops and nothing else would notice if one of
	 * them started clipping a column differently.
	 */
	{
		long t, d, n8, n16, n32;
		unsigned char *b8, *e8 = mk((long) P * H, &b8);
		unsigned char *b16, *e16 = mk((long) P * H * 2, &b16);

		memset(e8, 0, (size_t) ((long) P * H));
		memset(e16, 0, (size_t) ((long) P * H * 2));
		memset(buf, 0, (size_t) bytes);
		n8  = ps3d_frame(e8,  W, H, P, 8,  40, shade, &t, &d);
		n16 = ps3d_frame(e16, W, H, P, 16, 40, shade, &t, &d);
		n32 = ps3d_frame(buf, W, H, P, 32, 40, shade, &t, &d);
		if (n8 != n16 || n8 != n32)
			fail("the depths drew different numbers of pixels", n8, n32);
		printf("same shape at 8/16/32 bpp: %ld pixels\n", n8);
		free(b8);
		free(b16);
	}

	/* a picture, so a person can see it is a sphere and not a mess */
	if (argc > 1)
	{
		FILE *o = fopen(argv[1], "wb");
		long t, d;

		memset(buf, 0, (size_t) bytes);
		ps3d_frame(buf, W, H, P, 32, 40, shade, &t, &d);
		if (o)
		{
			short x, y;

			fprintf(o, "P5\n%d %d\n255\n", W, H);
			for (y = 0; y < H; y++)
				for (x = 0; x < W; x++)
					fputc(buf[((long) y * P + x) * 4 + 3], o);
			fclose(o);
		}
	}
	free(copy);
	free(base);

	printf(fails ? "%d CHECK(S) FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
