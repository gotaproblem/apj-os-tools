#!/usr/bin/env python3
"""
apply-pspdf.py - add the PSPDF NatFeat to a pistorm-atari-jit tree.

Does what 0021-PSPDF-host-side-PDF-rendering.patch does, but by anchor text
instead of line numbers, so it works whatever else the tree has in it, and
it is safe to run twice (each edit is skipped when it is already there).

    cd ~/pistorm-atari-jit
    python3 ../atari-share/apply-pspdf.py

platforms/atari/pdf/pspdf.h and pspdf.cpp must already be in place - copy
the ones next to this script over them first; the script then also
refreshes an existing nf_call_pspdf() to the current handler.
"""
import os
import sys

MK   = 'Makefile'
DOC  = 'NATFEATS.md'
NF   = 'platforms/atari/network/atari_natfeat.cpp'
done = []
skip = []


def read(p):
    with open(p) as f:
        return f.read()


def write(p, s):
    with open(p, 'w') as f:
        f.write(s)


def edit(path, text, what, have, anchor, insert, after=True, replace=None):
    """Insert (or replace) once. `have` is text that is only present when this
    edit has already been made, so running the script twice is harmless."""
    if have in text:
        skip.append('%s: %s' % (path, what))
        return text
    if anchor not in text:
        print('!! %s: cannot find the place for %s' % (path, what))
        print('   looked for: %r' % anchor[:60])
        sys.exit(1)
    if replace is not None:
        text = text.replace(replace, insert, 1)
    elif after:
        text = text.replace(anchor, anchor + insert, 1)
    else:
        text = text.replace(anchor, insert + anchor, 1)
    done.append('%s: %s' % (path, what))
    return text


for f in (MK, DOC, NF, 'platforms/atari/pdf/pspdf.h',
          'platforms/atari/pdf/pspdf.cpp'):
    if not os.path.exists(f):
        print('!! %s is missing - run this from the top of the tree, and make '
              'sure the two pspdf files are in place' % f)
        sys.exit(1)

# ---------------------------------------------------------------- Makefile
s = read(MK)

s = edit(MK, s, 'pspdf.cpp in CPPFILES',
         'platforms/atari/pdf/pspdf.cpp',
         '              platforms/atari/psimg/psimg.cpp \\\n',
         '              platforms/atari/pdf/pspdf.cpp \\\n')

s = edit(MK, s, 'PDF_PKGS block',
         'PDF_PKGS   = poppler-glib cairo',
         'SDL3_CFLAGS = $(shell pkg-config sdl3 --cflags)',
         '# Poppler + cairo for host-side PDF rendering '
         '(platforms/atari/pdf/pspdf.cpp).\n'
         'PDF_PKGS   = poppler-glib cairo\n'
         'PDF_CFLAGS = $(shell pkg-config $(PDF_PKGS) --cflags)\n'
         'PDF_LIBS   = $(shell pkg-config $(PDF_PKGS) --libs)\n\n',
         after=False)

s = edit(MK, s, 'dependency check',
         'MISSING_DEPS += libpoppler-glib-dev',
         'MISSING_DEPS += libjpeg-dev\nendif\n',
         'ifeq ($(shell pkg-config --exists poppler-glib && echo ok),)\n'
         'MISSING_DEPS += libpoppler-glib-dev\n'
         'endif\n'
         'ifeq ($(shell pkg-config --exists cairo && echo ok),)\n'
         'MISSING_DEPS += libcairo2-dev\n'
         'endif\n')

s = edit(MK, s, '$(PDF_LIBS) on the link line',
         '$(PDF_LIBS) $(AV_LIBS)',
         '-lmpg123 -ljpeg $(AV_LIBS)', '-lmpg123 -ljpeg $(PDF_LIBS) $(AV_LIBS)',
         replace='-lmpg123 -ljpeg $(AV_LIBS)')

s = edit(MK, s, 'pspdf.o rule',
         'platforms/atari/pdf/pspdf.o:',
         '# The video overlay plane only needs libdrm',
         '# Host PDF rendering: poppler-glib + cairo headers for this unit only.\n'
         'platforms/atari/pdf/pspdf.o: platforms/atari/pdf/pspdf.cpp\n'
         '\t$(CXX) $(CXXFLAGS) $(PDF_CFLAGS) -MMD -MP -c -o $@ $<\n\n',
         after=False)
write(MK, s)

# ------------------------------------------------------- atari_natfeat.cpp
s = read(NF)

s = edit(NF, s, 'NF_FEATURE_PSPDF',
         'NF_FEATURE_PSPDF,',
         '  NF_FEATURE_STBOX,\n  NF_FEATURE_COUNT',
         '  NF_FEATURE_STBOX,\n  NF_FEATURE_PSPDF,\n  NF_FEATURE_COUNT',
         replace='  NF_FEATURE_STBOX,\n  NF_FEATURE_COUNT')

s = edit(NF, s, '"PSPDF" in the name table',
         '  "PSPDF"',
         '  "STBOX"\n};', '  "STBOX",\n  "PSPDF"\n};',
         replace='  "STBOX"\n};')

s = edit(NF, s, 'pspdf.h include',
         '#include "platforms/atari/pdf/pspdf.h"',
         '#include "platforms/atari/psimg/psimg.h"',
         '\n#include "platforms/atari/pdf/pspdf.h"')

HANDLER = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            'pspdf-handler.inc')).read()
s = edit(NF, s, 'nf_call_pspdf()',
         'static uae_u32 nf_call_pspdf(',
         'static uae_u32 nf_call(uaecptr stack)', HANDLER, after=False)

# an existing handler is REPLACED by the current one, so re-running the
# script after pspdf-handler.inc changed (new sub-ops) updates the tree
if 'static uae_u32 nf_call_pspdf(' in s:
    a = s.index('/* PSPDF: host-side PDF rendering')
    b = s.index('static uae_u32 nf_call(uaecptr stack)')
    if s[a:b] != HANDLER:
        s = s[:a] + HANDLER + s[b:]
        done.append('%s: nf_call_pspdf() refreshed' % NF)

s = edit(NF, s, 'PSPDF dispatch',
         'return nf_call_pspdf(subid, params);',
         '    case NF_FEATURE_STBOX:\n      return nf_call_stbox(subid, params);',
         '    case NF_FEATURE_STBOX:\n      return nf_call_stbox(subid, params);\n'
         '    case NF_FEATURE_PSPDF:\n      return nf_call_pspdf(subid, params);',
         replace='    case NF_FEATURE_STBOX:\n      return nf_call_stbox(subid, params);')
write(NF, s)

# ------------------------------------------------------------- NATFEATS.md
s = read(DOC)
SECTION = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            'pspdf-natfeats.inc')).read()
s = edit(DOC, s, 'PSPDF section',
         '### PSPDF',
         '## Audio architecture (context for MP3PLAY and VIDPLAY)',
         SECTION, after=False)
s = edit(DOC, s, 'PISTORM_PDF_* environment rows',
         '| `PISTORM_PDF_CPUS`',
         '| `PISTORM_VID_DEBUG=1`          | Per-second video decode/present statistics      |',
         '\n| `PISTORM_PDF_CPUS`             | Hex affinity mask for the PDF render thread     |'
         '\n| `PISTORM_PDF_CACHE_MB`         | Rendered-page cache per document (default 192)  |'
         '\n| `PISTORM_PDF_MAX_MPIX`         | Largest page drawn in one piece (default 24 MP) |')
write(DOC, s)

for d in done:
    print('   added   %s' % d)
for d in skip:
    print('   already %s' % d)
print('\nNow: sudo apt install libpoppler-glib-dev libcairo2-dev fonts-urw-base35')
print('     make       and restart the emulator')
