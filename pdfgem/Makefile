# PDFGEM.PRG - PiStorm PDF viewer, GEM front-end on the APJSKIN sheet
# Build:  make        (produces PDFGEM.PRG)

CROSS   ?= m68k-atari-mint-
MINTBIN ?= /opt/cross-mint/bin
CC       = $(MINTBIN)/$(CROSS)gcc
STRIP    = $(MINTBIN)/$(CROSS)strip
CFLAGS   = -O2 -Wall -m68000
TARGET   = PDFGEM.PRG
SRCS     = pdfgem.c pdfui.c ../apjgui/apjskin.c
HDRS     = pdfui.h ../apjgui/apjskin.h ../apjgui/apjgui.h

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lgem
	$(STRIP) $@ 2>/dev/null || true

clean:
	rm -f $(TARGET)

.PHONY: clean
