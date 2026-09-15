/*
 * ps3d.h - the graphics half of the benchmark: a shaded solid, spun in
 * the window for a fixed time, measured.
 *
 * WHY A 3D TEST AT ALL. Dhrystone and CoreMark say what the CPU does with
 * integers in cache. Neither says anything about the thing an Atari user
 * actually watches, which is pixels arriving on screen - and on this
 * machine that is two very different costs stacked on each other:
 *
 *   the RASTERISER   fixed-point transform, backface cull, flat-shaded
 *                    triangle fill into an off-screen buffer. Pure 68k
 *                    integer work, and the part the JIT can help with.
 *
 *   the BLIT         vro_cpyfm of that buffer to the screen, which on
 *                    the PiSTorm goes through fVDI to a host-side memcpy
 *                    and has nothing to do with the JIT at all.
 *
 * They are timed separately and reported separately, because a bad
 * frame rate means completely different things depending on which one
 * ate the time, and one number cannot tell you.
 *
 * Everything is integer. No FPU, no divides in the inner loop, so the
 * result does not depend on whether an FPU is fitted or on how the FPU
 * emulation is configured. It is the same object and the same rotation
 * sequence every run, so two runs are comparable - which is the whole
 * point of having it next to a settings dialog.
 *
 * IT RASTERISES IN THE SCREEN'S OWN PIXEL FORMAT. The first version
 * worked a byte per pixel and expanded to the screen format afterwards,
 * which measured well and meant nothing: on hardware that expansion was
 * SIXTY PERCENT of the frame, so most of the graphics score was a glue
 * step no real program would perform. A program that draws on this
 * machine writes screen pixels, so this writes screen pixels.
 *
 * The cost therefore depends on the depth - four bytes a pixel is more
 * work than two - which is exactly true of the machine and is why the
 * depth is reported alongside the score.
 */
#ifndef PS3D_H
#define PS3D_H

/* why a run produced nothing, so the tab can say rather than sit blank */
enum { PS3D_OK, PS3D_NOROOM, PS3D_NOMEM, PS3D_NOSCR, PS3D_TOOSMALL };

typedef struct
{
	short ran;
	short why;			/* PS3D_*                            */
	short w, h;			/* the buffer that was rendered      */
	short bpp;			/* screen bits per pixel             */
	short shown;			/* frames the app actually put up    */

	long  frames;			/* frames actually drawn             */
	long  ms_total;			/* wall time for the whole run       */
	long  us_frame;			/* microseconds per frame            */
	long  ms_raster;		/* of which, rasterising             */
	long  ms_conv;			/* of which, converting to screen fmt */
	long  ms_blit;			/* of which, the blit itself          */

	long  tris;			/* triangles submitted (all frames)  */
	long  tris_drawn;		/* survived the backface cull        */
	long  pixels;			/* pixels actually written           */

	long  fps_x10;
	long  ktris_s;			/* thousands of triangles / second   */
	long  kpix_s;			/* thousands of pixels / second      */
	long  blit_kb_s;		/* screen bandwidth, KB/s            */
} PS3D;

/*
 * The renderer on its own, with no GEM in it, so the host harness can
 * run exactly the code the Atari runs and check it never writes outside
 * the buffer.
 *
 *   buf      stride * h pixels of `bpp` bits each
 *   stride   PIXELS per row; a multiple of 16, because that is what the
 *            MFDB will claim as its width
 *   bpp      8, 16 or 32. Anything else draws nothing and returns 0.
 *   shade16  sixteen pixel values, darkest first, already in the
 *            screen's format - this file does not know what a colour is
 *
 * Returns the number of pixels written, overdraw included, which is what
 * a fill rate means.
 */
long ps3d_frame(void *buf, short w, short h, short stride, short bpp,
                short angle, const long *shade16, long *tris, long *drawn);

/*
 * One full turn is 64 rotation steps. A RUN is however many whole turns
 * fit in the target time - the work per frame is fixed, so frames/second
 * stays comparable between runs while the run itself lasts long enough
 * to measure and long enough to watch.
 *
 * A fixed 64 frames was the first attempt and it was wrong twice over:
 * on a JIT'd 68040 the whole thing was over in under a second, which is
 * both invisible and only eighty ticks of a 200 Hz clock to divide by.
 */
#define PS3D_TURN	64
#define PS3D_SECS	2		/* target length of the measured pass */

/* the object: an icosphere, subdivided once - 80 triangles. Small enough
 * that a 68000 finishes a frame, big enough that the fill dominates. */
#define PS3D_NTRI	80

#endif /* PS3D_H */
