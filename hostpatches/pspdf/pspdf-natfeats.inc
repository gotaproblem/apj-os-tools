### PSPDF
Host-side PDF rendering (Poppler + cairo). The guest names a file, asks for a
page at a zoom, and copies the visible part of the drawn page into its own
buffer in the fVDI pixel format. Drawing happens on a **worker thread** (core 1
by default: core 0 takes the interrupts, core 2 is the 68k thread, core 3
the IPL poller), so no
sub-op below stalls the guest for more than a mutex trylock. Front end:
`PDFGEM.PRG`; test tool `PDFCHK.TTP` (source in `apj-os-tools/pdfchk/`).

| Sub-op | Name     | Arguments / result                                        |
|--------|----------|-----------------------------------------------------------|
| 0      | VERSION  | -> PSPDF API version                                       |
| 1      | OPEN     | p0 path -> handle > 0; -1 error, -2 needs a password       |
| 2      | OPENMEM  | p0 guest buffer, p1 length -> handle (for files not on HOSTFS) |
| 3      | CLOSE    | p0 handle                                                  |
| 4      | INFO     | p0 handle, p1 0=pages 1=outline entries 2=flags            |
| 5      | PAGESIZE | p0 handle, p1 page, p2 zoom -> (w<<16)\|h in pixels        |
| 6      | RENDER   | p0 handle, p1 page, p2 zoom, p3 rotation -> 0 queued       |
| 7      | STATUS   | p0 handle -> 0 ready, 1 busy, -1 failed                    |
| 8      | FETCH    | p0 handle, p1 dest, p2 x, p3 y, p4 w, p5 h, p6 bpp (16/32), p7 row bytes, p8 background RGB |
| 9      | FIND     | p0 handle, p1 needle, p2 page, p3 flags, p4 zoom, p5 result buf, p6 max -> hits on that page |
| 10     | TEXT     | p0 handle, p1 page, p2 zoom, p3 rect or 0, p4 buf, p5 len  |
| 11     | LINKS    | p0 handle, p1 page, p2 zoom, p3 buf, p4 max -> count       |
| 12     | LINKURI  | p0 handle, p1 page, p2 index, p3 buf, p4 len               |
| 13     | OUTLINE  | p0 handle, p1 index, p2 buf {depth, page, title[80]}       |
| 14     | META     | p0 handle, p1 0=title 1=author 2=subject 3=producer 4=creator, p2 buf, p3 len |
| 15     | HILITE   | p0 handle, p1 page, p2 rects (int32 x,y,w,h x n), p3 n (0 clears, max 64), p4 RGB: blended into the page by FETCH |
| 16     | PREFETCH | like RENDER, but only fills the cache; the current page and FETCH are untouched |
| 17     | CONTINUOUS | p0 handle, p1 on/off, p2 gap px: FETCH stitches the previous/next page above/below the current one (when cached), with a gap of background |

- **Zoom** is percent x 10, and 100% is 96 DPI (PDF points x 96/72). All
  geometry - page sizes, fetch rectangles, search hits, link boxes - is in
  device pixels at that zoom, so the guest never handles points.
- **Paths** take the MP3PLAY forms (`S:\DOCS\X.PDF` or `/s/docs/x.pdf` on a
  **HOSTFS drive**) or a plain host path. A PDF on an IDE image is loaded by
  the guest and handed over with OPENMEM instead.
- **The fetch buffer must be TT-RAM** (`Mxalloc(size, 1)`): the copy is a
  host-side memcpy through a direct pointer, and a write below 4 MB would go
  through the ST-RAM DMA mirror and the JIT's self-modifying-code check one
  byte at a time.
- Conversion to the fVDI format (32 bpp `00 RR GG BB`, 16 bpp big-endian
  RGB565) happens **during the fetch**, on the visible pixels only. Converting
  a whole 19 MP page costs ~110 ms on a Pi 4; a 1920x1080 viewport ~12 ms.
- Rendered pages are cached per document (`PISTORM_PDF_CACHE_MB`, default
  192). Rendering a page the guest already has costs nothing, so the front end
  can pre-draw the next page. A page bigger than `PISTORM_PDF_MAX_MPIX`
  (default 24 MP, i.e. roughly 300% zoom on A4) is drawn in bands around the
  viewport, and scrolling past the band queues the next one automatically.
- **Search is one page per call** - about 7 ms per page on a Pi 4, so a
  whole-document search is the guest's loop to drive (and to cancel).
- **Highlights are host-side.** The guest hands FETCH's page the search
  hits (or a selection) as rectangles with HILITE; the Pi blends them
  into the pixels it copies, translucent with a stronger edge. The 68k
  never draws or blends anything for them.
- **PREFETCH** draws the next page into the cache while the user reads
  this one, so the following RENDER answers from the cache at once.
- **CONTINUOUS** makes FETCH treat the document as one strip: rows above
  the current page come from the bottom of the previous one and rows below
  it from the top of the next (centred on the current page, a gap of
  background between), whenever the cache holds them. The guest scrolls
  through the seam and switches its notion of the current page when the
  seam passes the top of the viewport - the pixels are the same either
  side of the switch, so nothing visible happens.
- Page sizes are remembered once asked for or drawn, so PAGESIZE answers
  without the render lock for a page the worker has already seen.
- Implementation: `platforms/atari/pdf/pspdf.cpp` and `nf_call_pspdf()` in
  `atari_natfeat.cpp`. Build deps: `libpoppler-glib-dev libcairo2-dev`;
  install `fonts-urw-base35` as well for PDFs with no embedded fonts.
- Poppler parses untrusted files inside the emulator process, and the
  no-child-process rule means no sandbox: a Poppler crash takes the machine
  down. Use the distribution's patched Poppler, do not vendor an old one.

