#!/bin/sh
# Build APJSKIN + PSCTRL's drawing on the host and check every blit, on
# every tab, against a descriptor table that includes the awkward cases.
set -e
cd "$(dirname "$0")"
SKN=${1:-../../skins/out/FLTD175.SKN}
cc -std=c89 -Wall -Wextra -Wno-unused-parameter -g -O1 \
   -DAPJSKIN_HOST -I ../skin/stub -I ../../apjgui \
   -o harness harness.c ../../apjgui/apjskin.c ../../psctrl/psui.c ../../psctrl/psbrep.c -lm
./harness "$SKN" out.ppm
