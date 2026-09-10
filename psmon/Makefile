# PSMON.PRG / PSMON.ACC - PiStorm Monitor, GEM front-end
# Build:  make        (produces both)
#         make PSMON.PRG
#         make PSMON.ACC
#         make clean

CROSS   ?= m68k-atari-mint-
MINTBIN ?= /opt/cross-mint/bin
CC       = $(MINTBIN)/$(CROSS)gcc
STRIP    = $(MINTBIN)/$(CROSS)strip
CFLAGS   = -O2 -Wall -m68000

all: PSMON.PRG PSMON.ACC

PSMON.PRG: psmon.c
	$(CC) $(CFLAGS) -o $@ $< -lgem
	$(STRIP) $@ 2>/dev/null || true

PSMON.ACC: psmon.c
	$(CC) $(CFLAGS) -DBUILD_ACC -o $@ $< -lgem
	$(STRIP) $@ 2>/dev/null || true

clean:
	rm -f PSMON.PRG PSMON.ACC

.PHONY: all clean
