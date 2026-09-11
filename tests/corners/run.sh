#!/bin/sh
# Host test of XaAES Fluent rounded corners (APJ phase 5): runs the real
# build_rect_list / nextwind_rect / apj_* shape code from rectlist.c and the
# outline code from win_draw.c on three stacked windows. Checks every screen
# pixel has exactly one owner, corner pixels belong to the window beneath,
# and prints the corner outline as ASCII.
#   ./run.sh [path/to/xaaes/src.km]
set -e
cd "$(dirname "$0")"
SRC=${1:-$HOME/Workspace/ATARI/cdev/freemint/xaaes/src.km}
python3 - "$SRC/rectlist.c" "$SRC/win_draw.c" <<'PY'
import sys
s=open(sys.argv[1]).read()
a=s.index('build_rect_list(struct build_rl_parms *p)'); a=s.rfind('\n',0,a-2)+1
b=s.index('struct xa_rect_list *\nmake_rect_list')
open('rl.inc','w').write(s[a:b])
PY
cc -O1 -w -o corners corners.c
./corners
