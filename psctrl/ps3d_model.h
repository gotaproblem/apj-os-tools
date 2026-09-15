/*
 * ps3d_model.h - GENERATED, do not edit. An icosahedron subdivided once:
 * 42 vertices, 80 faces, on a unit sphere in 8.8 fixed point.
 *
 * Baked in rather than computed at start-up so the object is bit for bit
 * the same on every machine and every build - a benchmark that renders a
 * slightly different shape depending on the libm is not a benchmark.
 * Regenerate with skins/mk3dmodel.py if it ever needs to change, and
 * expect every previously recorded score to become incomparable.
 */
#ifndef PS3D_MODEL_H
#define PS3D_MODEL_H

#define PS3D_NVERT	42

static const short ps3d_vx[PS3D_NVERT] = {
	  -135,    135,   -135,    135,      0,      0,      0,      0,
	   218,    218,   -218,   -218,   -207,   -128,    -79,     79,
	     0,     79,    -79,   -128,   -207,   -256,    128,    207,
	  -128,      0,   -207,   -207,      0,   -128,    207,    128,
	   207,    128,     79,    -79,      0,    -79,     79,    128,
	   207,    256,
};

static const short ps3d_vy[PS3D_NVERT] = {
	   218,    218,   -218,   -218,   -135,    135,   -135,    135,
	     0,      0,      0,      0,    128,     79,    207,    207,
	   256,    207,    207,     79,    128,      0,     79,    128,
	   -79,      0,   -128,   -128,      0,    -79,    128,     79,
	  -128,    -79,   -207,   -207,   -256,   -207,   -207,    -79,
	  -128,      0,
};

static const short ps3d_vz[PS3D_NVERT] = {
	     0,      0,      0,      0,    218,    218,   -218,   -218,
	  -135,    135,   -135,    135,     79,    207,    128,    128,
	     0,   -128,   -128,   -207,    -79,      0,    207,     79,
	   207,    256,    -79,     79,   -256,   -207,    -79,   -207,
	    79,    207,    128,    128,      0,   -128,   -128,   -207,
	   -79,      0,
};

static const short ps3d_fa[PS3D_NTRI] = {
	 0, 11,  5, 12,  0,  5,  1, 14,  0,  1,  7, 16,
	 0,  7, 10, 18,  0, 10, 11, 20,  1,  5,  9, 15,
	 5, 11,  4, 13, 11, 10,  2, 21, 10,  7,  6, 19,
	 7,  1,  8, 17,  3,  9,  4, 32,  3,  4,  2, 34,
	 3,  2,  6, 36,  3,  6,  8, 38,  3,  8,  9, 40,
	 4,  9,  5, 33,  2,  4, 11, 35,  6,  2, 10, 37,
	 8,  6,  7, 39,  9,  8,  1, 41,
};

static const short ps3d_fb[PS3D_NTRI] = {
	12, 13, 14, 13, 14, 15, 16, 15, 16, 17, 18, 17,
	18, 19, 20, 19, 20, 21, 12, 21, 15, 22, 23, 22,
	13, 24, 25, 24, 21, 26, 27, 26, 19, 28, 29, 28,
	17, 30, 31, 30, 32, 33, 34, 33, 34, 35, 36, 35,
	36, 37, 38, 37, 38, 39, 40, 39, 40, 41, 32, 41,
	33, 22, 25, 22, 35, 24, 27, 24, 37, 26, 29, 26,
	39, 28, 31, 28, 41, 30, 23, 30,
};

static const short ps3d_fc[PS3D_NTRI] = {
	14, 12, 13, 14, 16, 14, 15, 16, 18, 16, 17, 18,
	20, 18, 19, 20, 12, 20, 21, 12, 23, 15, 22, 23,
	25, 13, 24, 25, 27, 21, 26, 27, 29, 19, 28, 29,
	31, 17, 30, 31, 34, 32, 33, 34, 36, 34, 35, 36,
	38, 36, 37, 38, 40, 38, 39, 40, 32, 40, 41, 32,
	25, 33, 22, 25, 27, 35, 24, 27, 29, 37, 26, 29,
	31, 39, 28, 31, 23, 41, 30, 23,
};

#endif /* PS3D_MODEL_H */
