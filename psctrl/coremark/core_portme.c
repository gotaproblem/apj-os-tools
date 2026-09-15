/*
 * core_portme.c - CoreMark porting layer for Atari GEM / FreeMiNT on the
 * PiSTorm, and for the Linux host test build. See core_portme.h.
 *
 * Three jobs:
 *
 *   1. the seeds and the iteration count, exactly as the reference port
 *      declares them (the values themselves come from -DPERFORMANCE_RUN
 *      and -DITERATIONS=0, and 0 means CoreMark sizes the run itself to
 *      land between 10 and 100 seconds - which is what makes the score
 *      reportable);
 *
 *   2. the clock. There is no clock() worth having here, so it is the
 *      200 Hz system counter at $4BA read through Supexec, the same one
 *      psbench.c times everything else with;
 *
 *   3. ee_printf, which captures rather than prints, because this runs
 *      inside a GEM accessory with no console.
 */

#include "coremark.h"
#include "cm_shared.h"

#ifdef CM_HOST_TEST
# include <stdio.h>
#endif

#include <stdarg.h>
#include <string.h>

/* ------------------------------------------------ seeds and run size -- */

#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PERFORMANCE_RUN
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PROFILE_RUN
volatile ee_s32 seed1_volatile = 0x8;
volatile ee_s32 seed2_volatile = 0x8;
volatile ee_s32 seed3_volatile = 0x8;
#endif
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

ee_u32 default_num_contexts = 1;

/* ------------------------------------------------------------- clock -- */

/*
 * 200 Hz. That is coarse against CoreMark's 10-second minimum only in
 * the sense that it cannot be wrong by more than 5 ms in ten seconds -
 * one part in two thousand - which is well inside the run-to-run spread
 * of a JIT that is still compiling during the first pass.
 *
 * The counter itself and the start/stop pair are in cm_shared.c: they
 * have to outlive the per-variant symbol hiding so psbench can read the
 * tick span back.
 */
#define EE_TICKS_PER_SEC	200UL

void start_time(void)	{ cm_t_start = cm_hz200(); }
void stop_time(void)	{ cm_t_stop  = cm_hz200(); }

CORE_TICKS get_time(void)
{
	return (CORE_TICKS) (cm_t_stop - cm_t_start);
}

secs_ret time_in_secs(CORE_TICKS ticks)
{
	return ((secs_ret) ticks) / (secs_ret) EE_TICKS_PER_SEC;
}

/* ---------------------------------------------------------- ee_printf -- */

static void put1(char c)	{ cm_log_putc(c); }

static void puts_(const char *s)
{
	while (*s)
		put1(*s++);
}

/* unsigned, any base up to 16, optional zero pad */
static void putu(unsigned long v, int base, int pad)
{
	char t[24];
	int n = 0;

	do {
		int d = (int) (v % (unsigned long) base);

		t[n++] = (char) (d < 10 ? '0' + d : 'a' + d - 10);
		v /= (unsigned long) base;
	} while (v && n < (int) sizeof(t));

	while (pad-- > n)
		put1('0');
	while (n--)
		put1(t[n]);
}

static void putd(long v)
{
	if (v < 0)
	{
		put1('-');
		putu((unsigned long) (-v), 10, 0);
	}
	else
		putu((unsigned long) v, 10, 0);
}

/*
 * %f to six places, which is what CoreMark's own report uses. Written
 * out by hand rather than handed to the C library: a GEM accessory's
 * printf may or may not have been linked with float support, and a
 * score that silently prints "%f" is worse than no score.
 */
static void putf(double d)
{
	unsigned long ip, fp;
	int i;

	if (d < 0)
	{
		put1('-');
		d = -d;
	}
	if (d > 4000000000.0)			/* beyond what this can say */
	{
		puts_("big");
		return;
	}
	ip = (unsigned long) d;
	d -= (double) ip;
	for (i = 0; i < 6; i++)
		d *= 10.0;
	fp = (unsigned long) (d + 0.5);
	if (fp >= 1000000UL)			/* rounded up into the units */
	{
		fp -= 1000000UL;
		ip++;
	}
	putu(ip, 10, 0);
	put1('.');
	putu(fp, 10, 6);
}

int ee_printf(const char *fmt, ...)
{
	va_list ap;
	unsigned long before = cm_log_len;

	va_start(ap, fmt);
	while (*fmt)
	{
		int pad = 0;

		if (*fmt != '%')
		{
			put1(*fmt++);
			continue;
		}
		int lng = 0;

		fmt++;
		if (*fmt == '0')
		{
			fmt++;
			while (*fmt >= '0' && *fmt <= '9')
				pad = pad * 10 + (*fmt++ - '0');
		}
		else
			while (*fmt >= '0' && *fmt <= '9')
				fmt++;		/* width without zero pad: ignored */
		/*
		 * The length modifier has to be honoured, not skipped. On m68k
		 * int and long are both 32 bits and it would not matter; on the
		 * 64-bit host the validation test runs on, reading a long where
		 * an int was passed walks off the argument list.
		 */
		while (*fmt == 'l' || *fmt == 'h')
		{
			if (*fmt == 'l')
				lng = 1;
			fmt++;
		}

		switch (*fmt)
		{
		case 'd': case 'i':
			putd(lng ? va_arg(ap, long) : (long) va_arg(ap, int));
			break;
		case 'u':
			putu(lng ? va_arg(ap, unsigned long)
			         : (unsigned long) va_arg(ap, unsigned int), 10, pad);
			break;
		case 'x': case 'X':
			putu(lng ? va_arg(ap, unsigned long)
			         : (unsigned long) va_arg(ap, unsigned int), 16, pad);
			break;
		case 'f': case 'g': case 'e': putf(va_arg(ap, double)); break;
		case 'c':           put1((char) va_arg(ap, int)); break;
		case 's':           puts_(va_arg(ap, char *)); break;
		case '%':           put1('%'); break;
		case '\0':          va_end(ap); return (int) (cm_log_len - before);
		default:            put1('%'); put1(*fmt); break;
		}
		fmt++;
	}
	va_end(ap);
#ifdef CM_HOST_TEST
	fputs(cm_log + before, stdout);
	cm_log_len = before;			/* the host test streams instead */
	cm_log[cm_log_len] = '\0';
#endif
	return (int) (cm_log_len - before);
}

/* ------------------------------------------------------------ init/fini -- */

void portable_init(core_portable *p, int *argc, char *argv[])
{
	(void) argc;
	(void) argv;

	if (sizeof(ee_ptr_int) != sizeof(ee_u8 *))
		ee_printf("ERROR! ee_ptr_int does not hold a pointer!\n");
	if (sizeof(ee_u32) != 4)
		ee_printf("ERROR! ee_u32 is not 32 bits!\n");
	p->portable_id = 1;
}

void portable_fini(core_portable *p)
{
	p->portable_id = 0;
}
