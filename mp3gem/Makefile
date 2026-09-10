# MP3GEM.PRG - PiStorm host MP3 player, GEM front-end
# Build:  make        (produces MP3GEM.PRG)

CROSS   ?= m68k-atari-mint-
MINTBIN ?= /opt/cross-mint/bin
CC       = $(MINTBIN)/$(CROSS)gcc
STRIP    = $(MINTBIN)/$(CROSS)strip
CFLAGS   = -O2 -Wall -m68000
TARGET   = MP3GEM.PRG

$(TARGET): mp3gem.c
	$(CC) $(CFLAGS) -o $@ $< -lgem
	$(STRIP) $@ 2>/dev/null || true

clean:
	rm -f $(TARGET)

.PHONY: clean
