# MP3GEM.PRG - PiStorm host MP3 player, GEM front-end
# Build:  make        (produces MP3GEM.PRG)

CROSS   ?= m68k-atari-mint-
MINTBIN ?= /opt/cross-mint/bin
CC       = $(MINTBIN)/$(CROSS)gcc
STRIP    = $(MINTBIN)/$(CROSS)strip
CFLAGS   = -O2 -Wall -m68000
TARGET   = MP3GEM.PRG
SRCS     = mp3gem.c mp3ui.c ../apjgui/apjskin.c
HDRS     = mp3ui.h ../apjgui/apjskin.h ../apjgui/apjgui.h

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lgem
	$(STRIP) $@ 2>/dev/null || true

clean:
	rm -f $(TARGET)

.PHONY: clean
