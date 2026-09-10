# VIDGEM.PRG - PiStorm host VIDEO player, GEM front-end
# Build:  make        (produces VIDGEM.PRG)

CROSS   ?= m68k-atari-mint-
MINTBIN ?= /opt/cross-mint/bin
CC       = $(MINTBIN)/$(CROSS)gcc
STRIP    = $(MINTBIN)/$(CROSS)strip
CFLAGS   = -O2 -Wall -m68000
TARGET   = VIDGEM.PRG

$(TARGET): vidgem.c
	$(CC) $(CFLAGS) -o $@ $< -lgem
	$(STRIP) $@ 2>/dev/null || true

clean:
	rm -f $(TARGET)

.PHONY: clean
