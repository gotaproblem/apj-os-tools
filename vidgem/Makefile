# VIDGEM.PRG - PiStorm host video player, GEM front-end on the APJSKIN sheet
# Build:  make        (produces VIDGEM.PRG)

CROSS   ?= m68k-atari-mint-
MINTBIN ?= /opt/cross-mint/bin
CC       = $(MINTBIN)/$(CROSS)gcc
STRIP    = $(MINTBIN)/$(CROSS)strip
CFLAGS   = -O2 -Wall -m68000
TARGET   = VIDGEM.PRG
SRCS     = vidgem.c vidui.c ../apjgui/apjskin.c
HDRS     = vidui.h ../apjgui/apjskin.h ../apjgui/apjgui.h

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lgem
	$(STRIP) $@ 2>/dev/null || true

clean:
	rm -f $(TARGET)

.PHONY: clean
