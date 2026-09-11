#!/bin/sh
# Host test of XaAES's Fluent menu geometry (APJ phase 2) against TeraDesk's
# real desktop.rsc: runs the actual fix_menu()/apj_menu_* code from
# menuwidg.c with stubs. Checks every drop-down validates, re-fix is
# idempotent, and theme off == a stock fix of the pristine tree.
#   ./run.sh [path/to/menuwidg.c] [path/to/desktop.rsc]
set -e
cd "$(dirname "$0")"
MW=${1:-$HOME/Workspace/ATARI/cdev/freemint/xaaes/src.km/menuwidg.c}
RSC=${2:-$HOME/teradesk/teradesk/desktop.rsc}
python3 - "$MW" <<'PY'
import sys
s=open(sys.argv[1]).read()
a=s.index('struct apj_menu_metrics\n{'); b=s.index('static const OBJECT drop_box =')
open('block.inc','w').write(s[a:b])
fa=s.index('void\nfix_menu(XA_TREE *menu, struct xa_window *wind)\n{'); fb=s.index('\n}\n',fa)+3
open('fix.inc','w').write(s[fa:fb])
PY
cp "$RSC" desktop.rsc
cc -w -o harness harness.c
./harness
