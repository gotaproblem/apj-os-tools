# JITTEST.PRG - differential correctness check for the PiStorm 68k JIT.
# Build:  make          (produces JITTEST.PRG)
#         make clean
#
# Built for the 68040: the battery exercises 68020+ instructions
# (mulu.l/muls.l, divu.l/divs.l, move from ccr) as well as the base set,
# so it must be assembled for a CPU that has them. It runs on Steve's
# 68040+FPU.

CROSS   ?= m68k-atari-mint-
MINTBIN ?= /opt/cross-mint/bin
CC       = $(MINTBIN)/$(CROSS)gcc
STRIP    = $(MINTBIN)/$(CROSS)strip

CFLAGS   = -O2 -Wall -m68040

all: JITTEST.PRG

JITTEST.PRG: jittest.o
	$(CC) $(CFLAGS) -o $@ jittest.o -lgem
	$(STRIP) $@ 2>/dev/null || true

jittest.o: jittest.c
	$(CC) $(CFLAGS) -c -o $@ jittest.c

clean:
	rm -f jittest.o JITTEST.PRG
