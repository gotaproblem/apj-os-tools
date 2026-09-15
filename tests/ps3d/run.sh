#!/bin/sh
# The benchmark's software rasteriser, on the build host - the same
# ps3d.c the Atari runs, with guard bytes round the buffer.
set -e
cd "$(dirname "$0")"
cc -std=c89 -Wall -Wextra -g -O1 -o harness harness.c ../../psctrl/ps3d.c
./harness out.pgm
