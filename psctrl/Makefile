# PSCTRL.ACC / PSCTRL.PRG - PiStorm settings, GEM front-end
# Build:  make            (produces both)
#         make PSCTRL.ACC
#         make clean
#
# The .ACC is the one that matters: it opens from the drop-down of
# whatever is running, which is the point of a live settings dialog.

CROSS   ?= m68k-atari-mint-
MINTBIN ?= /opt/cross-mint/bin
CC       = $(MINTBIN)/$(CROSS)gcc
STRIP    = $(MINTBIN)/$(CROSS)strip

CFLAGS   = -O2 -Wall -m68000 -I../apjgui

# Dhrystone's flags are FIXED and must not be tidied: they are the flags
# of the published reference (-O2 -fomit-frame-pointer), and the point of
# the benchmark is that one run is comparable with another and with the
# 1400 Dhry/s figure for an 8 MHz ST. Changing them invalidates every
# number the JIT tab has ever printed.
DHRYFLAGS = -O2 -fomit-frame-pointer -m68000 -w \
            -DDHRY_EMBEDDED -DREG=register -DTIME

# The MFLOPS loop needs the FPU mnemonics, and gas will not accept an
# .arch directive part-way through a file, so it is its own object with
# its own flags. It is only ever CALLED when the host reports an FPU.
FPUFLAGS  = -O2 -m68030 -m68881 -Wall

# ---------------------------------------------------------------- CoreMark
#
# CoreMark is built TWICE, once for a 68000 and once for an 020-and-up,
# because the question the pair answers is what the instruction set is
# worth on the same machine. The two copies cannot simply be linked
# together - every symbol would clash - so each set is combined with
# `ld -r` and then reduced to a single global entry point with
# `objcopy -G`. Everything else in it becomes local; undefined references
# (libgcc, memset) are left alone, which is what makes this work.
#
# NOTHING in coremark/ is edited to achieve that. main() is renamed with
# -Dmain=, which is a build option, and the run rules allow the port
# files, the iteration count and the toolchain flags to change and
# nothing else. cm_shared.c is compiled ONCE and holds the capture buffer
# and the clock, so both variants report into the same place.
#
# Both spellings of the entry point are kept, with and without a leading
# underscore. m68k-atari-mint prefixes C symbols with one and several
# other m68k toolchains do not; naming only the unprefixed form matched
# nothing here and quietly made the entry point local too, so the link
# failed on the symbol the whole arrangement exists to export. objcopy
# takes -G more than once and does not mind a name that is not there.
LD       = $(MINTBIN)/$(CROSS)ld
OBJCOPY  = $(MINTBIN)/$(CROSS)objcopy

CMDIR    = coremark
CMSRC    = $(CMDIR)/core_main.c $(CMDIR)/core_list_join.c \
           $(CMDIR)/core_matrix.c $(CMDIR)/core_state.c \
           $(CMDIR)/core_util.c $(CMDIR)/core_portme.c
CMFLAGS  = -O2 -I$(CMDIR) -DPERFORMANCE_RUN=1 -DITERATIONS=0
CMHDRS   = $(CMDIR)/coremark.h $(CMDIR)/core_portme.h $(CMDIR)/cm_shared.h

OBJS = psctrl.o psui.o psbench.o psbrep.o psbench_fpu.o ps3d.o \
       dhry_1.o dhry_2.o cm000.o cm020.o cm_shared.o apjskin.o
ACCOBJS = psctrl_acc.o psui.o psbench.o psbrep.o psbench_fpu.o ps3d.o \
       dhry_1.o dhry_2.o cm000.o cm020.o cm_shared.o apjskin.o

HDRS = psui.h psbench.h ps3d.h ps3d_model.h \
       ../apjgui/apjskin.h ../apjgui/apjgui.h

all: PSCTRL.PRG PSCTRL.ACC

PSCTRL.PRG: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lgem
	$(STRIP) $@ 2>/dev/null || true

PSCTRL.ACC: $(ACCOBJS)
	$(CC) $(CFLAGS) -o $@ $(ACCOBJS) -lgem
	$(STRIP) $@ 2>/dev/null || true

psctrl.o: psctrl.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ psctrl.c

psctrl_acc.o: psctrl.c $(HDRS)
	$(CC) $(CFLAGS) -DBUILD_ACC -c -o $@ psctrl.c

psui.o: psui.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ psui.c

psbench.o: psbench.c psbench.h ps3d.h
	$(CC) $(CFLAGS) -c -o $@ psbench.c

psbrep.o: psbrep.c psbench.h
	$(CC) $(CFLAGS) -c -o $@ psbrep.c

psbench_fpu.o: psbench_fpu.c
	$(CC) $(FPUFLAGS) -c -o $@ psbench_fpu.c

dhry_1.o: dhry_1.c dhry.h
	$(CC) $(DHRYFLAGS) -c -o $@ dhry_1.c

dhry_2.o: dhry_2.c dhry.h
	$(CC) $(DHRYFLAGS) -c -o $@ dhry_2.c

ps3d.o: ps3d.c ps3d.h ps3d_model.h
	$(CC) $(CFLAGS) -c -o $@ ps3d.c

# --- the two CoreMark builds -------------------------------------------
#
# The flags are quoted into the score, so they are part of the result and
# must not be tidied: -DFLAGS_STR is what ends up after the slash in
# "CoreMark 1.0 : N / GCC4.6.4 -O2 -m68000 / STACK".

cm_shared.o: $(CMDIR)/cm_shared.c $(CMDIR)/cm_shared.h
	$(CC) -O2 -Wall -m68000 -c -o $@ $<

# Makefile is a prerequisite on purpose: the compiler flags are QUOTED
# INTO THE SCORE, so a change to them has to invalidate the objects.
cm000.o: $(CMSRC) $(CMHDRS) Makefile
	@rm -rf .cm000 && mkdir -p .cm000
	for f in $(CMSRC); do \
		$(CC) -m68000 $(CMFLAGS) -DFLAGS_STR='"-O2 -m68000"' \
		      -Dmain=cm_run_000 -c -o .cm000/`basename $$f .c`.o $$f || exit 1; \
	done
	$(LD) -r -o $@ .cm000/*.o
	$(OBJCOPY) -G cm_run_000 -G _cm_run_000 $@
	@rm -rf .cm000

cm020.o: $(CMSRC) $(CMHDRS) Makefile
	@rm -rf .cm020 && mkdir -p .cm020
	for f in $(CMSRC); do \
		$(CC) -m68020-60 $(CMFLAGS) -DFLAGS_STR='"-O2 -m68020-60"' \
		      -Dmain=cm_run_020 -c -o .cm020/`basename $$f .c`.o $$f || exit 1; \
	done
	$(LD) -r -o $@ .cm020/*.o
	$(OBJCOPY) -G cm_run_020 -G _cm_run_020 $@
	@rm -rf .cm020

apjskin.o: ../apjgui/apjskin.c ../apjgui/apjskin.h
	$(CC) $(CFLAGS) -c -o $@ ../apjgui/apjskin.c

clean:
	rm -f PSCTRL.PRG PSCTRL.ACC $(OBJS) psctrl_acc.o
	rm -rf .cm000 .cm020

.PHONY: all clean
