# PSMON.ACC / PSMON.PRG - PiSTorm Monitor, GEM front-end
# Build:  make            (produces both)
#         make PSMON.ACC
#         make clean
#
# The .ACC is the one that matters: it opens from the drop-down of
# whatever is running, so the figures can be watched against a game or
# the ST Box rather than only from the desktop.

CROSS   ?= m68k-atari-mint-
MINTBIN ?= /opt/cross-mint/bin
CC       = $(MINTBIN)/$(CROSS)gcc
STRIP    = $(MINTBIN)/$(CROSS)strip

CFLAGS   = -O2 -Wall -m68000 -I../apjgui

OBJS    = psmon.o psmonui.o apjskin.o
ACCOBJS = psmon_acc.o psmonui.o apjskin.o

HDRS = psmonui.h ../apjgui/apjskin.h ../apjgui/apjgui.h

all: PSMON.PRG PSMON.ACC

PSMON.PRG: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lgem
	$(STRIP) $@ 2>/dev/null || true

PSMON.ACC: $(ACCOBJS)
	$(CC) $(CFLAGS) -o $@ $(ACCOBJS) -lgem
	$(STRIP) $@ 2>/dev/null || true

psmon.o: psmon.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ psmon.c

psmon_acc.o: psmon.c $(HDRS)
	$(CC) $(CFLAGS) -DBUILD_ACC -c -o $@ psmon.c

psmonui.o: psmonui.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ psmonui.c

apjskin.o: ../apjgui/apjskin.c ../apjgui/apjskin.h
	$(CC) $(CFLAGS) -c -o $@ ../apjgui/apjskin.c

clean:
	rm -f PSMON.PRG PSMON.ACC $(OBJS) psmon_acc.o

.PHONY: all clean
