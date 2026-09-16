#!/usr/bin/env python3
"""
apply-psweb.py - add the PSWEB NatFeat (the browser's engine link) to a
pistorm-atari-jit tree. Anchor-based like apply-pspdf.py, safe to run twice.

    cd ~/pistorm-atari-jit
    python3 <this dir>/apply-psweb.py

platforms/atari/web/{psweb_proto.h,psweb_client.h,psweb_client.c} must be
in place first (the script copies them from beside itself when they are
not). An existing nf_call_psweb() is replaced by the current handler.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
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
    if have in text:
        skip.append('%s: %s' % (path, what))
        return text
    if anchor not in text:
        print('!! %s: cannot find the place for %s' % (path, what))
        print('   looked for: %r' % anchor[:70])
        sys.exit(1)
    if replace is not None:
        text = text.replace(replace, insert, 1)
    elif after:
        text = text.replace(anchor, anchor + insert, 1)
    else:
        text = text.replace(anchor, insert + anchor, 1)
    done.append('%s: %s' % (path, what))
    return text


for f in (MK, DOC, NF):
    if not os.path.exists(f):
        print('!! %s is missing - run this from the top of the tree' % f)
        sys.exit(1)

os.makedirs('platforms/atari/web', exist_ok=True)
for f in ('psweb_proto.h', 'psweb_client.h', 'psweb_client.c'):
    dst = os.path.join('platforms/atari/web', f)
    src = os.path.join(HERE, f)
    if os.path.exists(src):
        if not os.path.exists(dst) or read(src) != read(dst):
            shutil.copyfile(src, dst)
            done.append('%s: copied' % dst)
        else:
            skip.append('%s: up to date' % dst)
    elif not os.path.exists(dst):
        print('!! %s is missing and there is no copy next to this script' % dst)
        sys.exit(1)

# ---------------------------------------------------------------- Makefile
s = read(MK)
s = edit(MK, s, 'psweb_client.c in CFILES',
         'platforms/atari/web/psweb_client.c',
         '         platforms/atari/kbd_usb.c \\\n',
         '         platforms/atari/web/psweb_client.c \\\n')
write(MK, s)

# ------------------------------------------------------- atari_natfeat.cpp
s = read(NF)
s = edit(NF, s, 'NF_FEATURE_PSWEB',
         'NF_FEATURE_PSWEB,',
         '  NF_FEATURE_PSPDF,\n  NF_FEATURE_COUNT',
         '  NF_FEATURE_PSPDF,\n  NF_FEATURE_PSWEB,\n  NF_FEATURE_COUNT',
         replace='  NF_FEATURE_PSPDF,\n  NF_FEATURE_COUNT')
s = edit(NF, s, '"PSWEB" in the name table',
         '  "PSWEB"',
         '  "PSPDF"\n};', '  "PSPDF",\n  "PSWEB"\n};',
         replace='  "PSPDF"\n};')
s = edit(NF, s, 'psweb includes',
         '#include "platforms/atari/web/psweb_client.h"',
         '#include "platforms/atari/pdf/pspdf.h"',
         '\n#include "platforms/atari/web/psweb_proto.h"'
         '\n#include "platforms/atari/web/psweb_client.h"')

HANDLER = read(os.path.join(HERE, 'psweb-handler.inc'))
s = edit(NF, s, 'nf_call_psweb()',
         'static uae_u32 nf_call_psweb(',
         'static uae_u32 nf_call(uaecptr stack)', HANDLER, after=False)
if 'static uae_u32 nf_call_psweb(' in s:
    a = s.index("/* PSWEB: the web browser's engine link")
    b = s.index('static uae_u32 nf_call(uaecptr stack)')
    if s[a:b] != HANDLER:
        s = s[:a] + HANDLER + s[b:]
        done.append('%s: nf_call_psweb() refreshed' % NF)

s = edit(NF, s, 'PSWEB dispatch',
         'return nf_call_psweb(subid, params);',
         '    case NF_FEATURE_PSPDF:\n      return nf_call_pspdf(subid, params);',
         '    case NF_FEATURE_PSPDF:\n      return nf_call_pspdf(subid, params);\n'
         '    case NF_FEATURE_PSWEB:\n      return nf_call_psweb(subid, params);',
         replace='    case NF_FEATURE_PSPDF:\n      return nf_call_pspdf(subid, params);')
write(NF, s)

# ------------------------------------------------------------- NATFEATS.md
s = read(DOC)
SECTION = read(os.path.join(HERE, 'psweb-natfeats.inc'))
s = edit(DOC, s, 'PSWEB section',
         '### PSWEB',
         '## Audio architecture (context for MP3PLAY and VIDPLAY)',
         SECTION, after=False)
s = edit(DOC, s, 'PISTORM_WEB_* environment rows',
         '| `PISTORM_WEB_SOCK`',
         '| `PISTORM_PDF_MAX_MPIX`         | Largest page drawn in one piece (default 24 MP) |',
         '\n| `PISTORM_WEB_SOCK`             | psweb socket (default /tmp/psweb.sock)          |'
         '\n| `PISTORM_WEB_CPUS`             | Hex affinity mask for the psweb connector thread |'
         '\n| `PISTORM_WEB_DEBUG=1`          | Trace the psweb link                             |')
write(DOC, s)

for d in done:
    print('   added   %s' % d)
for d in skip:
    print('   already %s' % d)
print('\nNow: make    and restart the emulator; start psweb/psweb separately')
