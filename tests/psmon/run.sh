#!/bin/sh
# Build APJSKIN + PSMON's drawing on the host and check every blit and
# every string, against four models: normal, every reading absent, the
# widest strings each row can make, and no emulator at all.
set -e
cd "$(dirname "$0")"
SKN=${1:-../../skins/out/FLTD175.SKN}
cc -std=c89 -Wall -Wextra -Wno-unused-parameter -g -O1 \
   -DAPJSKIN_HOST -I ../skin/stub -I ../../apjgui -I ../../psmon \
   -o harness harness.c ../../apjgui/apjskin.c ../../psmon/psmonui.c -lm
./harness "$SKN" out.ppm
