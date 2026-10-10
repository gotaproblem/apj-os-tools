#!/bin/sh
# Build DOSGEM's drawing on the host and check every blit, every message
# line and the cost of a damage band (tests/dos/harness.c).
set -e
cd "$(dirname "$0")"
cc -std=c89 -Wall -Wextra -Wno-unused-parameter -g -O1 \
   -DAPJSKIN_HOST -DDOSUI_MAX_W=1280 -DDOSUI_MAX_H=720 -I ../skin/stub -I ../../apjgui \
   -o harness harness.c ../../apjgui/apjskin.c ../../dosgem/dosui.c -lm
./harness
