#!/usr/bin/env python3
"""
Generate GEM bitmap system fonts (.FNT) from DejaVu Sans Mono for APJ-OS:
larger system-font sizes so the AES lays dialogs out bigger at 1080p.
The AA atlases in XaAES render_apj cover the same cells (12x24, 16x32),
so under a theme these bitmaps are only ever seen by legacy paths.

    mkfnt.py OUTDIR      -> APJ15.FNT (12x24, 15pt)  APJ20.FNT (16x32, 20pt)

Format: GEM font header (88 bytes, Motorola byte order), char offset
table, then the bitmap (all glyphs side by side, MSB = left pixel).
"""
import struct, sys, os
from PIL import Image, ImageFont, ImageDraw

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
SIZES = [(12, 24, 15), (16, 32, 20)]     # cell w, cell h, point size

# Atari ST charset -> Unicode for 0x80..0xFF (CP437 is identical for the
# accented Latin block that matters; the rest are best-effort)
def atari_unicode(c):
    if c < 0x20:
        return {0x07: 0x25C6, 0x1A: 0x2192, 0x1B: 0x2190, 0x1C: 0x2191, 0x1D: 0x2193}.get(c, 0x20)
    if c == 0x7F: return 0x2302
    if c < 0x80: return c
    return bytes([c]).decode('cp437')

def build(cw, ch, pt):
    size = 4
    while True:
        f = ImageFont.truetype(FONT, size + 1)
        asc, desc = f.getmetrics()
        if f.getlength("M") > cw or asc + desc > ch: break
        size += 1
    f = ImageFont.truetype(FONT, size)
    asc, desc = f.getmetrics()
    ytop = (ch - (asc + desc)) // 2
    xoff = int(round((cw - f.getlength("M")) / 2))
    # the whole bitmap: 256 glyphs side by side
    form_w_px = 256 * cw
    form_w = (form_w_px + 7) // 8
    if form_w & 1: form_w += 1                 # word-aligned rows
    im = Image.new('1', (form_w * 8, ch), 0)
    d = ImageDraw.Draw(im)
    for c in range(256):
        u = atari_unicode(c)
        g = Image.new('L', (cw, ch), 0)
        ImageDraw.Draw(g).text((xoff, ytop), chr(u) if isinstance(u, int) else u, font=f, fill=255)
        im.paste(g.point(lambda v: 255 if v >= 110 else 0).convert('1'), (c * cw, 0))
    # bitmap bytes, MSB = leftmost
    rows = b''
    px = im.load()
    for y in range(ch):
        row = bytearray(form_w)
        for x in range(form_w * 8):
            if px[x, y]: row[x >> 3] |= 0x80 >> (x & 7)
        rows += bytes(row)
    # header
    baseline = ytop + asc
    top = baseline; ascent = baseline - 1; half = baseline - (asc * 5 // 10); descent = ch - 1 - baseline; bottom = descent
    name = ("APJ Mono %d" % pt).encode('ascii').ljust(32, b'\0')
    flags = 0x0001 | 0x0008                    # system font, monospaced - same as the stock Atari .FNTs (no 0x0004)
    # The GEM .FNT header and the character offset table are in INTEL (little-
    # endian) byte order - the format is DRI's PC one.  fVDI's load_font()
    # unconditionally byte-swaps the header words/longs and the offset table
    # (engine/fonts.c: fixup_font(header, buffer, ~(flags & FONTF_BIGENDIAN)) -
    # '~' of anything is non-zero, so it always flips).  The bitmap rows are
    # plain bytes and are never swapped.  A big-endian header therefore loads
    # as garbage (height 0x1800 ...) and takes the screen with it.
    hdr_len = 88
    off_tab_len = (256 + 1) * 2
    hdr = struct.pack('<hh32shhhhhhhhhhhhhhhhIIIhhI',
        1, pt, name, 0, 255,
        top, ascent, half, descent, bottom,
        cw, cw, 0, 0, 1, 1, 0x5555, 0x5555, flags,
        0, hdr_len, hdr_len + off_tab_len, form_w, ch, 0)
    assert len(hdr) == 88, len(hdr)
    offs = b''.join(struct.pack('<H', c * cw) for c in range(257))
    return hdr + offs + rows, size

out = sys.argv[1]; os.makedirs(out, exist_ok=True)
for cw, ch, pt in SIZES:
    data, px = build(cw, ch, pt)
    fn = os.path.join(out, "APJ%02d.FNT" % pt)
    open(fn, 'wb').write(data)
    print(fn, len(data), 'bytes, cell %dx%d from %dpx face' % (cw, ch, px))
