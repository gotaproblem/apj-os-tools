#!/bin/sh
# Host test of XaAES window-move blitting with Fluent rounded windows:
# runs the real rect-list code (rectlist.c) and the real blit ordering and
# redraw-area code from set_and_update_window() (c_window.c) on a pixel
# buffer, for many move deltas, and checks the moved window ends up with
# exactly its own content (no stale or doubled pixels).
#   ./run.sh [path/to/xaaes/src.km]
set -e
cd "$(dirname "$0")"
SRC=${1:-$HOME/Workspace/ATARI/cdev/freemint/xaaes/src.km}
python3 - "$SRC/rectlist.c" "$SRC/c_window.c" <<'PY'
import sys
s=open(sys.argv[1]).read()
a=s.index('build_rect_list(struct build_rl_parms *p)'); a=s.rfind('\n',0,a-2)+1
b=s.index('struct xa_rect_list *\nmake_rect_list')
open('rl.inc','w').write(s[a:b])
c=open(sys.argv[2]).read()
f=c.index('set_and_update_window(struct xa_window *wind, bool blit, bool only_wa, GRECT *new)\n{')
a=c.index('\tif (blit && oldrl && newrl)\n', f)
b=c.index('\telse if (newrl)\n', a)
open('blit.inc','w').write(c[a:b])
i=c.find('static void\napj_blit_order')
open('order.inc','w').write(c[i:c.index('static void\nset_and_update_window',i)] if i>=0 else '')
PY
cc -O1 -w -o moveblit moveblit.c
./moveblit
