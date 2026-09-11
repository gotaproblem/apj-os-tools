# APJ-OS icons

Papirus icon theme (GPL-3.0, https://github.com/PapirusDevelopmentTeam/papirus-icon-theme)
mapped onto TeraDesk's `cicons.rsc`, plus six icons TeraDesk never had
(AUDIO FILE, VIDEO FILE, MP3 PLAYER, MP4 PLAYER, SOURCE FILE, PDF FILE).

    fetch.py                 pulls the mapped SVGs from Papirus into svg/ (needs network)
    mkicons.py RSC SVGDIR OUT  builds:
        out/cicons.rsc       TeraDesk resource with the 6 new icons appended as
                             16-colour CICONs (classic themes)
        out/apjicons-32.bin  32-bit RGBA set for XaAES render_apj (Fluent),
        out/apjicons-48.bin  keyed by FNV-1a hash of each icon's mono mask+data
        out/apjicons-64.bin
        out/icons.json       name -> hash

Install on the ST:
    out/cicons.rsc      -> the TeraDesk folder (replaces the old one; back it up)
    out/apjicons-32.bin -> the XaAES folder (next to xaaes.cnf)

Then in TeraDesk: Options -> Install icon, assign *.MP3 -> AUDIO FILE,
*.MP4 -> VIDEO FILE, etc., and put MP3 PLAYER / MP4 PLAYER on the
desktop for MP3GEM.PRG / VIDGEM.PRG.

Requires: python3, cairosvg, Pillow.

FILE (TeraDesk's icon for any extension with no icon assigned) is a blank
page, derived locally from TEXT_FILE.svg (the text lines removed), not
Papirus `mimetypes/unknown` - a "?" on every .RSC/.INF looked broken.
The original is kept as svg/FILE-unknown.svg; fetch.py would overwrite
FILE.svg, so restore it from git after a fetch.
