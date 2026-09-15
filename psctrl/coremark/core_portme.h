/*
 * core_portme.h - CoreMark porting layer for Atari GEM / FreeMiNT on the
 * PiSTorm, and for the Linux host test build.
 *
 * This file and core_portme.c are the ONLY files in this directory that
 * are ours. core_main.c, core_list_join.c, core_matrix.c, core_state.c
 * and core_util.c are EEMBC's, byte for byte, and every one of them
 * matches the md5 published in coremark.md5. The run rules allow exactly
 * this: the port files, the iteration count, and the toolchain flags.
 *
 * See README-PORT.md in this directory for what that means for quoting a
 * score.
 */
#ifndef CORE_PORTME_H
#define CORE_PORTME_H

/* ---------------------------------------------------------- platform -- */

/*
 * No stdio: this runs inside a GEM accessory with no console, so
 * ee_printf() is ours and captures into a buffer the benchmark tab
 * reads. HAS_PRINTF 0 tells core_util.c not to define its own.
 */
#define HAS_STDIO	0
#define HAS_PRINTF	0
#define HAS_TIME_H	0
#define USE_CLOCK	0

/*
 * Floating point is used by CoreMark ONLY to report - never inside the
 * timed loop, which is pure integer. Leaving it on keeps the reference
 * arithmetic (including the "did it run for 10 seconds" check) exactly
 * as published. The score PSCTRL displays is computed from the integer
 * tick count and iteration count instead, so a soft-float rounding
 * difference cannot move it.
 */
#define HAS_FLOAT	1

/* With HAS_STDIO 0, coremark.h pulls in no headers at all, so the port
 * owes it NULL and a declaration of ee_printf - the same two things the
 * barebones port provides. */
#ifndef NULL
# define NULL ((void *) 0)
#endif

int ee_printf(const char *fmt, ...);

/* 200 Hz ticks from _hz_200 at $4BA. */
typedef unsigned long CORE_TICKS;

#ifndef COMPILER_VERSION
# ifdef __GNUC__
#  define COMPILER_VERSION "GCC" __VERSION__
# else
#  define COMPILER_VERSION "unknown"
# endif
#endif

/* The Makefile passes -DFLAGS_STR="..." per CPU variant, because the run
 * rules require the flags to be quoted with the score and the whole point
 * of the two variants is that they differ. */
#ifndef COMPILER_FLAGS
# define COMPILER_FLAGS	FLAGS_STR
#endif

#ifndef MEM_LOCATION
# define MEM_LOCATION	"STACK"
#endif

/* ------------------------------------------------------------- types -- */

/*
 * These must be EXACTLY these widths - the run rules say so, and they
 * are not a formality: getting ee_u32 wrong changes the list and state
 * CRCs and CoreMark reports "Errors detected" rather than a wrong score.
 * (Found exactly that way: `long` is 32 bits on m68k but 64 on the host
 * the validation test runs on.) `int` is 32 bits on both m68k-atari-mint
 * and the host, so it is the one spelling that is right in both builds.
 */
typedef signed short   ee_s16;
typedef unsigned short ee_u16;
typedef signed int     ee_s32;
typedef double         ee_f32;
typedef unsigned char  ee_u8;
typedef unsigned int   ee_u32;
typedef unsigned long  ee_size_t;

/*
 * ee_ptr_int must hold a pointer. On m68k that is 32 bits and ee_u32 is
 * right; on the 64-bit host used for the validation test it is not, and
 * getting this wrong is a segfault in core_matrix.c rather than a wrong
 * answer. unsigned long is correct on both.
 */
typedef unsigned long  ee_ptr_int;

#define align_mem(x)	(void *) (4 + (((ee_ptr_int) (x) - 1) & ~3))

/* ------------------------------------------------------------ config -- */

#ifndef SEED_METHOD
# define SEED_METHOD	SEED_VOLATILE
#endif
#ifndef MEM_METHOD
# define MEM_METHOD	MEM_STACK
#endif
#ifndef MULTITHREAD
# define MULTITHREAD	1
# define USE_PTHREAD	0
# define USE_FORK	0
# define USE_SOCKET	0
#endif

/* main() takes no arguments here - the Makefile renames it and PSCTRL
 * calls it directly. */
#define MAIN_HAS_NOARGC		1
#define MAIN_HAS_NORETURN	0

extern ee_u32 default_num_contexts;

typedef struct CORE_PORTABLE_S
{
	ee_u8 portable_id;
} core_portable;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

/* ------------------------------------------------- what PSCTRL reads -- */

/*
 * Everything CoreMark prints is captured. PSCTRL takes the integer tick
 * and iteration counts out of it and computes the score itself, and
 * shows the compliance line verbatim - the run rules want the score
 * quoted as "CoreMark 1.0 : N / <compiler> <flags> / <memory>", and the
 * only honest way to show that is the text CoreMark itself produced.
 *
 * See cm_shared.h - the buffer and the clock live there, compiled once,
 * because objcopy hides everything else per variant.
 */

#if !defined(PROFILE_RUN) && !defined(PERFORMANCE_RUN) && !defined(VALIDATION_RUN)
# if (TOTAL_DATA_SIZE == 1200)
#  define PROFILE_RUN 1
# elif (TOTAL_DATA_SIZE == 2000)
#  define PERFORMANCE_RUN 1
# else
#  define VALIDATION_RUN 1
# endif
#endif

#endif /* CORE_PORTME_H */
