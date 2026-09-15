# PDFCHK.TTP - PSPDF NatFeat test tool
# Build:  make        (produces PDFCHK.TTP)
#         make clean

CROSS   ?= m68k-atari-mint-
MINTBIN ?= /opt/cross-mint/bin
CC       = $(MINTBIN)/$(CROSS)gcc
STRIP    = $(MINTBIN)/$(CROSS)strip
CFLAGS   = -O2 -Wall -m68000
TARGET   = PDFCHK.TTP

$(TARGET): pdfchk.c
	$(CC) $(CFLAGS) -o $@ $<
	$(STRIP) $@ 2>/dev/null || true

clean:
	rm -f $(TARGET)

.PHONY: clean
