# WEBCHK.PRG - PSWEB NatFeat test tool: a web page in a GEM window
# Build:  make        (produces WEBCHK.PRG)

CROSS   ?= m68k-atari-mint-
MINTBIN ?= /opt/cross-mint/bin
CC       = $(MINTBIN)/$(CROSS)gcc
STRIP    = $(MINTBIN)/$(CROSS)strip
CFLAGS   = -O2 -Wall -m68000
TARGET   = WEBCHK.PRG

$(TARGET): webchk.c
	$(CC) $(CFLAGS) -o $@ webchk.c -lgem
	$(STRIP) $@ 2>/dev/null || true

clean:
	rm -f $(TARGET)

.PHONY: clean
