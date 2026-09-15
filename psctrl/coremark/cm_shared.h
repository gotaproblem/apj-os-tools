/*
 * cm_shared.h - the parts of the CoreMark port that must NOT be
 * duplicated per CPU variant.
 *
 * CoreMark is built twice, once for -m68000 and once for -m68020-60, and
 * the two object sets are made to coexist by hiding every symbol in each
 * except its entry point (see the Makefile: ld -r then objcopy -G). That
 * is exactly what we want for CoreMark's own code - two private copies,
 * no clash - but the capture buffer and the clock have to be reachable
 * from psbench.c afterwards, and there must be only one of each.
 *
 * So they live here, compiled once. objcopy leaves UNDEFINED symbols
 * alone, so each variant's core_portme.c refers to these and both end up
 * pointing at the same storage.
 */
#ifndef CM_SHARED_H
#define CM_SHARED_H

#define CM_LOG_MAX	1024

extern char          cm_log[CM_LOG_MAX];
extern unsigned long cm_log_len;

void cm_log_reset(void);
void cm_log_putc(char c);

/* the 200 Hz counter, and the span of the last timed section */
extern unsigned long cm_t_start, cm_t_stop;

unsigned long cm_hz200(void);
unsigned long cm_last_ticks(void);

#endif /* CM_SHARED_H */
