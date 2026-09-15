# PSPDF - host-side PDF rendering, for applying on the Pi

Copy this folder to the Pi (anywhere - /tmp is fine; NOT the atari-share),
then, with pdfsrc = where you put it:

    cd ~/pistorm-atari-jit
    cp $pdfsrc/pspdf.h $pdfsrc/pspdf.cpp platforms/atari/pdf/
    python3 $pdfsrc/apply-pspdf.py
    make
    # stop and restart the emulator

`apply-pspdf.py` edits `Makefile`, `NATFEATS.md` and
`platforms/atari/network/atari_natfeat.cpp` by anchor text rather than line
numbers, never touches the git index, and is safe to run again: edits that
are already there print as `already`, and an existing `nf_call_pspdf()` is
replaced by the current one (that is how new sub-ops reach the tree).

First-time deps:  `sudo apt install libpoppler-glib-dev libcairo2-dev fonts-urw-base35`

Check it took:

    grep -c PSPDF_PREFETCH platforms/atari/network/atari_natfeat.cpp   # not 0
    strings ./emulator | grep -c PSPDF                                # not 0, after make

`../0021-PSPDF-host-side-PDF-rendering.patch` is the first version of this
change as a git patch and is now out of date; use the script.
