# APJ-OS system fonts

`mkfnt.py OUTDIR` renders DejaVu Sans Mono into GEM bitmap fonts at the
larger system-font cells XaAES render_apj already has AA atlases for:

    APJ15.FNT   12x24 cell, 15 pt
    APJ20.FNT   16x32 cell, 20 pt

Both carry font id 1 (the system font) and the system/monospaced flags,
so to fVDI they are extra sizes of the system font.

Install: copy to C:\GEMSYS (fVDI's PATH), then in fvdi.sys after the
driver line:

    s APJ15.FNT
    s APJ20.FNT

Then pick the size:
  xaaes.cnf:  STANDARD_POINT = 15      (or 20)  - AES dialogs, menus, chrome
  TeraDesk:   Options -> Fonts        - directory windows

Under a theme the antialiased atlas is drawn instead of these bitmaps
(same cell, same advance); the bitmaps are what legacy paths see.
