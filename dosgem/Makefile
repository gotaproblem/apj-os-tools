# DOSGEM.PRG - DOS games on APJ-OS: the window. The Pi runs DOSBox Pure
# (psdos); the PSDOS NatFeat carries frames, sound and input
# (pistorm-atari-jit psdos-design.md).
# Build:  make        (produces DOSGEM.PRG)

CROSS   ?= m68k-atari-mint-
MINTBIN ?= /opt/cross-mint/bin
CC       = $(MINTBIN)/$(CROSS)gcc
STRIP    = $(MINTBIN)/$(CROSS)strip
CFLAGS   = -O2 -Wall -m68000
TARGET   = DOSGEM.PRG
SRCS     = dosgem.c dosui.c
HDRS     = dosui.h ../apjgui/apjgui.h

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lgem
	$(STRIP) $@ 2>/dev/null || true

clean:
	rm -f $(TARGET)

.PHONY: clean
