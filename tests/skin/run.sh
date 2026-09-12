#!/bin/sh
# Build APJSKIN + MP3GEM's drawing on the host and check every blit.
set -e
cd "$(dirname "$0")"
SKN=${1:-../../skins/out/FLTD175.SKN}
cc -std=c89 -Wall -Wextra -Wno-unused-parameter -g -O1 \
   -DAPJSKIN_HOST -I stub \
   -o harness harness.c ../../apjgui/apjskin.c ../../mp3gem/mp3ui.c -lm
./harness "$SKN" out.ppm
