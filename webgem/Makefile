# WEBGEM.PRG - PiStorm web browser, GEM front-end on the APJSKIN sheet;
# the Pi renders (psweb, WPE WebKit), the PSWEB NatFeat carries the frames
# Build:  make        (produces WEBGEM.PRG)

CROSS   ?= m68k-atari-mint-
MINTBIN ?= /opt/cross-mint/bin
CC       = $(MINTBIN)/$(CROSS)gcc
STRIP    = $(MINTBIN)/$(CROSS)strip
CFLAGS   = -O2 -Wall -m68000
TARGET   = WEBGEM.PRG
SRCS     = webgem.c webui.c ../apjgui/apjskin.c
HDRS     = webui.h ../apjgui/apjskin.h ../apjgui/apjgui.h

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lgem
	$(STRIP) $@ 2>/dev/null || true

clean:
	rm -f $(TARGET)

.PHONY: clean
