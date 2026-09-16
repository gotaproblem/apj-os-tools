#!/usr/bin/env python3
"""
mkglyphs.py - the status glyphs -> apjglyphs.bin for XaAES (render_apj.c).

One-colour symbols in the line-icon style (24-unit grid, 1.8 stroke) that
the AES tints to a theme pen when it draws them, so one file serves the
dark and the light themes: the menu-bar network and USB-input icons
(opcode 122) and anything an app asks for through opcode 123.

    svg-glyph/NN-NAME.svg     NN is the glyph id (render_apj.h APJ_GLYPH_*)
    python3 mkglyphs.py       writes apjglyphs.bin + glyphs-sheet.png

Install: copy apjglyphs.bin to the XaAES folder on the ST, next to
apjicons-*.bin.

File: "APJG", u16 version (1), u16 count, u16 0, u16 0; then count x
{ u16 id, u16 size, u16 w, u16 h, u32 offset }, then 8-bit coverage,
w*h bytes per glyph, row-major. Every glyph is rendered at each of SIZES;
the AES picks the largest not over the size it wants.
"""
import glob
import os
import struct
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = (16, 20, 24, 32, 48)


def render(path, size):
    """8-bit coverage of the SVG at size x size, as a PIL 'L' image"""
    try:
        import cairosvg
        png = cairosvg.svg2png(url=path, output_width=size, output_height=size)
        import io
        return Image.open(io.BytesIO(png)).convert("RGBA").getchannel("A")
    except ImportError:
        sys.path.insert(0, os.path.join(HERE, "..", "skins"))
        from svgraster import coverage
        cov = coverage(path, size)
        return cov if isinstance(cov, Image.Image) else Image.frombytes("L", (size, size), bytes(cov))


def main():
    entries = []
    blobs = []
    files = sorted(glob.glob(os.path.join(HERE, "svg-glyph", "*.svg")))
    if not files:
        sys.exit("no svg-glyph/*.svg")
    for f in files:
        gid = int(os.path.basename(f).split("-")[0])
        for sz in SIZES:
            im = render(f, sz)
            entries.append((gid, sz, im.width, im.height))
            blobs.append(im.tobytes())
    hdr = 12 + 12 * len(entries)
    off = hdr
    table = b""
    for (gid, sz, w, h), blob in zip(entries, blobs):
        table += struct.pack(">HHHHI", gid, sz, w, h, off)
        off += len(blob)
    out = os.path.join(HERE, "apjglyphs.bin")
    with open(out, "wb") as fh:
        fh.write(b"APJG" + struct.pack(">HHHH", 1, len(entries), 0, 0))
        fh.write(table)
        for b in blobs:
            fh.write(b)
    print("%s: %d glyphs x %d sizes, %d bytes" % (out, len(files), len(SIZES), off))

    # a contact sheet to eyeball: black on white at 48 and 24
    sheet = Image.new("L", (len(files) * 64 + 16, 64 + 40), 255)
    for i, f in enumerate(files):
        for sz, y in ((48, 8), (24, 62)):
            im = render(f, sz)
            sheet.paste(Image.eval(im, lambda a: 255 - a), (8 + i * 64, y))
    sheet.save(os.path.join(HERE, "glyphs-sheet.png"))


if __name__ == "__main__":
    main()
