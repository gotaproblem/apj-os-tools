### PSWEB
The web browser's link to **psweb**, a separate process on the Pi that
renders pages with WPE WebKit off-screen and hands each frame over in the
guest's pixel format through shared memory. The emulator side is a connector
thread on core 1 (`platforms/atari/web/psweb_client.c`); the NatFeat handler
only pushes command records into a ring, reads state words, or copies the
frame into a TT-RAM buffer. One frame is ever in flight: psweb releases the
next to WebKit only after the guest has fetched the last, so the engine runs
at the guest's pace. Front end: `WEBGEM.PRG`; test tool `WEBCHK.PRG`
(`apj-os-tools/webchk/`). Design: `psweb-design.md`.

| Sub-op | Name       | Arguments / result                                   |
|--------|------------|------------------------------------------------------|
| 0      | VERSION    | -> PSWEB API version                                  |
| 1      | STATUS     | -> bit0 psweb socket exists, bit1 connected, bit2 view ready |
| 2      | VIEW_NEW   | p0 w, p1 h, p2 bpp (16/32), p3 flags -> 1 queued; poll STATUS bit2 |
| 3      | VIEW_FREE  |                                                       |
| 4      | VIEW_SIZE  | p0 view, p1 w, p2 h                                   |
| 5      | VIEW_STATE | p0 view, p1 bits (1 visible, 2 focused, 4 topped)    |
| 6      | LOAD       | p0 view, p1 text: URL, host name, words to search, or a GEMDOS path on a HOSTFS drive (-> file:// on the Pi; -1 for any other drive) |
| 7      | NAV        | p0 view, p1 0 back 1 forward 2 reload 3 reload-nocache 4 stop |
| 8      | POLL       | p0 view, p1 ptr to 8 longs: frame serial, damage count, progress (0..1000), flags, cursor (1 = over a link), title serial, URI serial, dialog |
| 9      | FETCH      | p0 view, p1 TT-RAM buffer, p2 bytes per row, p3 rows, p4 rect array (x y w h longs), p5 max -> rects copied; 0 = no new frame |
| 10     | POINTER    | p0 view, p1 0 move 1 press 2 release, p2 x, p3 y, p4 button (1 left 2 right 3 middle), p5 kstate |
| 11     | SCROLL     | p0 view, p1 dx, p2 dy (wheel notches x 120, or pixels when p5 & 1), p3 x, p4 y, p5 flags |
| 12     | KEY        | p0 view, p1 1 down / 0 up, p2 AES key word (scancode<<8 \| ascii), p3 kstate |
| 13     | TEXT       | p0 view, p1 string typed into the page                |
| 14     | GETSTR     | p0 view, p1 0 title 1 URI 2 hovered link 3 status 6 engine, p2 buf, p3 len -> bytes (Atari charset); -3 = try again |
| 19     | ZOOM       | p0 view, p1 percent                                   |
| 24     | SETTING    | p0 view, p1 key (0 JavaScript 1 images 2 UA preset 3 search URL 4 home 5 zoom 6 blocker), p2 value, p3 string or 0 |

Flags in POLL: 1 can go back, 2 can go forward, 4 loading, 8 https, 0x40 the
web process crashed, 0x100 JavaScript off, 0x200 content blocker active.
Results: -1 error, -3 busy (ask again next tick), -4 psweb not connected.

- psweb is socket-activated: `install-full.sh` (the "web" step, `WEB=1`)
  installs `psweb.socket` / `psweb.service`, and connecting to
  `/run/psweb/psweb.sock` starts it; it exits again after ten idle
  minutes (`PSWEB_IDLE_S`) so the 250-480 MB a page costs is only spent
  while browsing. The emulator tries that socket first, then
  `/tmp/psweb.sock` for a psweb run by hand; `PISTORM_WEB_SOCK` names one
  explicitly. The emulator reconnects by itself whenever psweb appears.
- Memory: the web process polices itself (`PSWEB_MEM_MB`, default 40% of
  RAM; killed at 1.5x, which WEBGEM shows as "crashed - reload"), because
  a Pi booted with `cgroup_disable=memory` ignores the unit's MemoryMax.
- Content blocker: `/etc/psweb/adblock.json`, EasyList + EasyPrivacy in
  WebKit's rule format from `psweb/mkblocker.py` (68k rules; `adblock-lite.json`
  is the hosts-only list for a 1 GB Pi). Compiled once into WebKit's DFA
  by `psweb --compile-filter` at install and cached; recompiled only when
  the JSON changes.
- Frames arrive whole in v1 (one damage rectangle covering the view); the
  guest blits the rectangles FETCH returns.

