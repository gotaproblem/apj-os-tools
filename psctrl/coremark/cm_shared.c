/* cm_shared.c - see cm_shared.h. Compiled ONCE, for -m68000, so both
 * CoreMark variants can call into it whichever CPU they were built for. */

#include "cm_shared.h"

#ifdef CM_HOST_TEST
# include <stdio.h>
# include <sys/time.h>
#else
# include <osbind.h>
#endif

char          cm_log[CM_LOG_MAX];
unsigned long cm_log_len = 0;
unsigned long cm_t_start = 0, cm_t_stop = 0;

void cm_log_reset(void)
{
	cm_log_len = 0;
	cm_log[0] = '\0';
}

void cm_log_putc(char c)
{
	if (cm_log_len + 1 < CM_LOG_MAX)
	{
		cm_log[cm_log_len++] = c;
		cm_log[cm_log_len] = '\0';
	}
}

#ifdef CM_HOST_TEST

unsigned long cm_hz200(void)
{
	struct timeval tv;

	gettimeofday(&tv, 0);
	return (unsigned long) (tv.tv_sec * 200UL + tv.tv_usec / 5000UL);
}

#else

# if defined(__GNUC__) && __GNUC__ >= 12
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Warray-bounds"
# endif
static long read_hz200(void)
{
	return *(volatile long *) 0x4BAL;
}
# if defined(__GNUC__) && __GNUC__ >= 12
#  pragma GCC diagnostic pop
# endif

unsigned long cm_hz200(void)
{
	return (unsigned long) Supexec(read_hz200);
}

#endif

unsigned long cm_last_ticks(void)
{
	return cm_t_stop - cm_t_start;
}
