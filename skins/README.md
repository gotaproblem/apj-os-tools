# APJSKIN — bitmap skins for the PiSTorm's GEM tools

A skin is one pre-composited 32-bit sheet plus a table of source rectangles.
Drawing a control is `vro_cpyfm(S_ONLY)` out of that sheet: with the MFDB in
device format fVDI does the copy host-side, so the 68k issues one trap per
rectangle and never touches a pixel. A full MP3GEM redraw at 1920×1080 is
**43 blits**.

    tokens/<skin>.json     the nineteen APJ_R_* roles, six extras, and the
                           metrics — in POINTS, not pixels
    glyphs/order.txt       glyph ids, in order (NOT the number in the filename)
    glyphs/*-<NAME>.svg    one 24×24 viewBox per glyph, black art
    mkskin.py              builds <FILE><scale>.SKN, one per scale
    preview.py             reads a .SKN and composites MP3GEM or PSCTRL
                           out of it

Build:

    python3 mkskin.py all out            # every skin, 100 / 125 / 175
    python3 mkskin.py fluent-dark out 175
    python3 preview.py out/FLTD175.SKN out/p.png
    python3 preview.py --psctrl out/FLTD175.SKN out/s.png 1   # tab 1

Install on the ST: `*.SKN` → `S:\APJ-OS\NATFEATS\SKINS\`, with the rest of
the PiSTorm tools. An app opens `<file><scale>.SKN`, where `<file>` is the
`file` field of the token JSON (8.3 names, so keep it to four characters)
and `<scale>` is chosen from the screen width: `< 1024` → 100, `< 1600` →
125, otherwise 175.

The full search order is: the folder the app named through
`apj_skin_setdir()` (from its own `.INF`), then `<progdir>SKINS\` and the
program folder, then `SKINS\` and `.` under the cwd, then
`S:\APJ-OS\NATFEATS\SKINS\`, then `C:\OPT\GEM\SKINS\`.

The first and fourth of those exist because of **desk accessories**. A
`.PRG` sits with the other tools and finds `SKINS\` beside itself; an
`.ACC` is loaded from the root of the boot drive, so its progdir is `C:\`
and the program-relative entries find nothing at all.

Requires python3, cairosvg and Pillow — the same as `icons/mkicons.py`.

On a Mac the hurdle is not the Python packages but the C library under
cairosvg, and Homebrew's python refuses a system-wide `pip install`
(PEP 668). Both at once:

    brew install cairo
    python3 -m venv ~/.venv/atari
    ~/.venv/atari/bin/pip install cairosvg Pillow
    ~/.venv/atari/bin/python skins/mkskin.py all skins/out

`preview.py` additionally wants **DejaVu Sans Mono** — not a nicety, it is
the face `mkatlas.py` renders into XaAES's antialiased text atlas, so it is
literally what the Atari draws with and anything else has the wrong advance
widths. It is hunted for in the usual Linux, Homebrew, MacPorts and
`~/Library/Fonts` locations; failing all of those it falls back to Menlo or
Liberation and says so. `brew install --cask font-dejavu` settles it.

## Adding a skin

Copy a token file, change the colours, give it a new four-letter `file`, run
`mkskin.py`. No art to draw: every region is rendered from the tokens. Add
`glyphs/*.svg` only for a genuinely new glyph, and then add its name to
`order.txt` *and* to `APJ_G_*` in `apjgui/apjskin.h` — those two and
`mkskin.py`'s region list are the contract between the builder and the app.

## What the sheet holds

| Region | What it is |
|---|---|
| PANELTOP | the mica wash, tiled across the top of a window body |
| GROUP | inset surface: playlist ground, art tile, video frame |
| ROWSEL | selected list row with its accent rail |
| SEEK | track / buffered / fill |
| KNOB | three states |
| BADGE | plain and accent pills |
| ARTPH | album-art placeholder |
| BTN, BTNACC | plates with no glyph, for labelled buttons |
| TILE | 19 glyphs × 4 states, plate and glyph pre-composited |
| TILEACC | 5 glyphs × 4 states on the round accent button |
| VSCROLL | trough, thumb, thumb held |
| TAB, TABBAR | a tab item (4 states) and the strip behind it — PSCTRL |
| RADIO, CHECK | off/on × normal, hover, pressed, disabled |
| FIELD | a value box: normal, focused, disabled |
| POPUP, CHEV | an enum plate, and its arrow as a separate small blit |
| STATUS | the bottom strip |

Plus an 8-bit coverage atlas of all 24 glyphs, for the one case that does need
blending: a glyph over the video overlay plane, or the play mark on a selected
row.

Nine-slice takes only the **corners** from the sheet; the edges and the middle
are `v_bar` fills in the two colours each state recorded, because the art is
flat there by convention. Tiling them instead cost 1800 blits for one redraw.

**That has a consequence worth knowing before you draw a region**: anything
baked into the *middle of an edge* survives only under the corners. The
popup's chevron and the selected tab's accent rail were both drawn that way
first and both came out as two stubs at the ends. The chevron is now its own
small region blitted on top (`CHEV`, pre-composited against each plate
state's fill); the rail is one `v_bar` in `psui.c`. If a region needs a mark
anywhere but its corners, that is the choice: a separate blit, or a fill.

## Sheet versions

`SKN_VERSION` is **2**. Version 1 stopped at `VSCROLL` (twelve regions);
version 2 adds the seven PSCTRL regions plus `CHEV`, and two more `BADGE`
states (warn and danger, for the apply-class badge).

`apjskin.c` loads either: the region count in the header says how many are
there, anything past it is marked absent, and `apj_skin_has(rid)` reports
it — every draw call for an absent region is a no-op. So a rebuilt MP3GEM
still runs on a skin set that has not been rebuilt, and PSCTRL falls back
to the flat controls rather than refusing to start.

## Sizes

| scale | sheet | .SKN on disk | in TT-RAM |
|---|---|---|---|
| 100 % | 560×396 | 660 KB | 887 KB |
| 125 % | 704×496 | 1.01 MB | 1.33 MB |
| 175 % | 992×616 | 1.78 MB | 2.33 MB |

The file is RGB24 and expanded once, at load, into the screen's device format;
that conversion is what makes every later blit a straight copy. The sheet must
end up in alternate RAM (`Mxalloc` mode 3) — a blit source below 4 MB goes
through `pistorm_dma_to_stram()` and the JIT's self-modifying-code check.
