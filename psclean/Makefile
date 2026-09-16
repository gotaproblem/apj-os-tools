# PSCLEAN.PRG - remove Mac litter (._* .DS_Store .smbdelete* .fseventsd ...) from a folder
# Build:  make        (produces PSCLEAN.PRG)

CROSS   ?= m68k-atari-mint-
MINTBIN ?= /opt/cross-mint/bin
CC       = $(MINTBIN)/$(CROSS)gcc
STRIP    = $(MINTBIN)/$(CROSS)strip
CFLAGS   = -O2 -Wall -m68000
TARGET   = PSCLEAN.PRG

$(TARGET): psclean.c
	$(CC) $(CFLAGS) -o $@ psclean.c -lgem
	$(STRIP) $@ 2>/dev/null || true

clean:
	rm -f $(TARGET)

.PHONY: clean
