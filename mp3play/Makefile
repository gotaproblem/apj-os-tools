# MP3PLAY.TTP - PiStorm host MP3 player front-end
# Build:  make        (produces MP3PLAY.TTP)
#         make clean

CROSS   ?= m68k-atari-mint-
# Toolchain install prefix - same one CoreMark's build_all.sh uses.
# Override on the command line if yours lives elsewhere:  make MINTBIN=/path/bin
MINTBIN ?= /opt/cross-mint/bin
CC       = $(MINTBIN)/$(CROSS)gcc
STRIP    = $(MINTBIN)/$(CROSS)strip
CFLAGS   = -O2 -Wall -m68000
TARGET   = MP3PLAY.TTP

$(TARGET): mp3play.c
	$(CC) $(CFLAGS) -o $@ $<
	$(STRIP) $@ 2>/dev/null || true

clean:
	rm -f $(TARGET)

.PHONY: clean
