# Line icon set — how it is built and what to install

The hand-drawn monochrome set (24-unit grid, 1.6 stroke) in place of Papirus.

    iconset.py      all 51 drawings + RSC_MAP: cicons.rsc name -> drawing
    svg-line/       the 44 SVGs the pipeline wants, named for the resource
    mkbins.py       builds out-line/{dark,light}/apjicons-{32,48,64}.bin
    checkbins.py    decodes a bin and writes a contact sheet to eyeball

## cicons.rsc is NOT modified

The bins are keyed by the FNV-1a hash of each icon's mono mask+data in the
resource. Building them against the cicons.rsc you already run keeps every
hash identical, so XaAES finds the new artwork with no resource change, no
reinstall, and no change to your TeraDesk assignments.

Verified: all 44 hashes in the new bins match `out/icons.json` from the
current build.

## Install

Copy ONE of these three-file sets to the XaAES folder on the ST
(next to xaaes.cnf), replacing the current apjicons-*.bin:

    out-line/dark/    black strokes  -> light wallpaper + white windows
    out-line/light/   white strokes  -> dark wallpaper + dark windows

Pick the one that matches the theme you are running. Once XaAES tints a
one-colour icon to the surface behind it, one set will serve both and this
choice goes away.

Classic (non-Fluent) themes still draw the Papirus CICONs from cicons.rsc.
To give them the line art too, the resource has to be rebuilt with the new
mono masks — which changes every hash, so the rsc and the bins would then
have to be installed as a matched pair. Not done here.

## Name mapping

    HARD DISK 3  -> netdrive (stacked server)   assign S: and U: to this
    HARD DISK 2  -> hard disk with two bands
    GEMSYS       -> folder + cog
    MAGIC FOLDER -> folder + star
    MINT FOLDER  -> folder + dots
    FONTS        -> folder + A
    ACC          -> plugin blocks
    GEM APP      -> window
    TOS APP      -> page + >_        MINT -> terminal      MAGIC -> sliders
    PRX          -> page + 01        HELP FILE -> page + ?
    VIEWER       -> photos           PAINT -> palette      DISABLE -> (!)
    MP3 PLAYER   -> notes            MP4 PLAYER -> screen + play

Six drawings not used by this resource are in the set for later:
eject, folder_open, folder_new, documents, settings, media.
