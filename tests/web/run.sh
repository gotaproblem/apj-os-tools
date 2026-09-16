#!/bin/sh
# Build APJSKIN + WEBGEM's drawing on the host and check every blit,
# every text extent and the cost of each partial repaint.
#   ./run.sh                      one sheet (FLTD175)
#   ./run.sh ../../skins/out/FUJI100.SKN
#   ./run.sh all                  every sheet in skins/out
set -e
cd "$(dirname "$0")"
cc -std=c89 -Wall -Wextra -Wno-unused-parameter -g -O1 \
   -DAPJSKIN_HOST -I ../skin/stub -I ../../apjgui \
   -o harness harness.c ../../apjgui/apjskin.c ../../webgem/webui.c -lm
if [ "$1" = all ]; then
	for s in ../../skins/out/*.SKN; do
		echo "== $s"; ./harness "$s" out.ppm
	done
else
	./harness "${1:-../../skins/out/FLTD175.SKN}" out.ppm
fi
