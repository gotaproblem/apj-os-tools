#!/usr/bin/env python3
"""
APJSKIN sheet builder.

  in:  tokens/<skin>.json     colour roles + metrics, in POINTS
       glyphs/order.txt       glyph ids, in order
       glyphs/*-<NAME>.svg    one file per glyph (24x24 viewBox, black art)
  out: <SKIN>.SKN             one file per scale, big-endian, 68k-ready

Every pixel in the sheet is PRE-COMPOSITED against the surface it will be
drawn on, so the guest never blends: a blit is vro_cpyfm(S_ONLY) straight
from the sheet to the screen and fVDI turns that into a host memcpy. The
one exception is the glyph coverage atlas (chunk MASK), which is 8-bit
alpha for the rare case of a glyph over the video overlay plane.

Usage: mkskin.py <skin-name|all> <outdir> [scale ...]     scales: 100 125 175

Requires: python3, cairosvg, Pillow  (same as icons/mkicons.py)
"""
import io, json, os, struct, sys

import cairosvg
from PIL import Image, ImageDraw

SS = 4                      # supersampling factor for the vector-ish art

# ---------------------------------------------------------------- roles --
ROLES = ["FACE","TEXT","LIGHT","DARK","SELBG","SELFG","ALBG","ALFG","PANEL","TITBG",
         "TITFG","PAPER","BORDER","HOVER","PRESSED","FOCUS","DISABLED","ELEVATION","ACCENT"]
EXTRA = ["ACCENT_INK","ACCENT_DEEP","MUTED","PANEL_TOP","ROW","HAIR"]
PALT_N = 32                 # 19 roles + 6 extras + reserved

# ------------------------------------------------------------- regions --
# id, name, w, h, nstates, insets(l,t,r,b), flags
F_TILEX, F_TILEY, F_SLICE9 = 1, 2, 4

RG_PANELTOP, RG_GROUP, RG_ROWSEL, RG_SEEK, RG_KNOB, RG_BADGE, \
RG_ARTPH, RG_BTN, RG_BTNACC, RG_TILE, RG_TILEACC, RG_VSCROLL = range(12)
RG_N = 12
RG_NAME = ["PANELTOP","GROUP","ROWSEL","SEEK","KNOB","BADGE",
           "ARTPH","BTN","BTNACC","TILE","TILEACC","VSCROLL"]

NBTNGLYPH = 19              # glyphs 0..18 get a TILE
NACCGLYPH = 5               # glyphs 0..4 get a TILEACC
NSTATE    = 4               # normal, hover, pressed, on/disabled

# --------------------------------------------------------------- colour --
def rgb(h):
    h = h.lstrip("#")
    if len(h) == 8:
        return (int(h[0:2],16), int(h[2:4],16), int(h[4:6],16), int(h[6:8],16))
    return (int(h[0:2],16), int(h[2:4],16), int(h[4:6],16), 255)

def mix(a, b, t):
    return tuple(int(round(a[i] + (b[i]-a[i])*t)) for i in range(3)) + (255,)

def over(fg, bg):
    """composite straight-alpha fg over opaque bg"""
    a = fg[3] / 255.0
    return tuple(int(round(fg[i]*a + bg[i]*(1-a))) for i in range(3)) + (255,)

# ---------------------------------------------------------------- canvas --
class Art:
    """draws at SS x, downsamples on finish - gives antialiased corners"""
    def __init__(self, w, h, bg):
        self.w, self.h = w, h
        self.im = Image.new("RGBA", (w*SS, h*SS), bg)
        self.d = ImageDraw.Draw(self.im)

    def rrect(self, x, y, w, h, r, fill=None, outline=None, width=1):
        b = [x*SS, y*SS, x*SS + w*SS - 1, y*SS + h*SS - 1]
        self.d.rounded_rectangle(b, radius=max(0, r*SS), fill=fill,
                                 outline=outline, width=width*SS)

    def ellipse(self, x, y, w, h, fill=None, outline=None, width=1):
        b = [x*SS, y*SS, x*SS + w*SS - 1, y*SS + h*SS - 1]
        self.d.ellipse(b, fill=fill, outline=outline, width=width*SS)

    def rect(self, x, y, w, h, fill):
        self.d.rectangle([x*SS, y*SS, x*SS + w*SS - 1, y*SS + h*SS - 1], fill=fill)

    def vgrad(self, x, y, w, h, top, bot):
        for i in range(h*SS):
            c = mix(top, bot, i / float(max(1, h*SS - 1)))
            self.d.rectangle([x*SS, y*SS + i, x*SS + w*SS - 1, y*SS + i], fill=c)

    def paste(self, im, x, y):
        """paste an already-final-resolution RGBA image (post-downsample)"""
        self.late = getattr(self, "late", [])
        self.late.append((im, x, y))

    def finish(self):
        out = self.im.resize((self.w, self.h), Image.LANCZOS)
        for im, x, y in getattr(self, "late", []):
            out.alpha_composite(im, (x, y))
        return out

# ---------------------------------------------------------------- glyphs --
def load_glyphs(gdir):
    names = [l.strip() for l in open(os.path.join(gdir, "order.txt"))
             if l.strip() and not l.startswith("#")]
    files = {}
    for f in os.listdir(gdir):
        if f.endswith(".svg"):
            files[f.rsplit("-", 1)[-1][:-4]] = os.path.join(gdir, f)
    missing = [n for n in names if n not in files]
    if missing:
        sys.exit("glyphs missing: %s" % ", ".join(missing))
    return names, [files[n] for n in names]

def raster(path, size):
    """SVG -> 8-bit coverage map at size x size"""
    png = cairosvg.svg2png(url=path, output_width=size, output_height=size)
    return Image.open(io.BytesIO(png)).convert("RGBA").split()[3]

def tinted(cov, colour):
    im = Image.new("RGBA", cov.size, colour[:3] + (0,))
    im.putalpha(cov)
    return im

# ----------------------------------------------------------------- pack --
class Shelf:
    """dumb shelf packer; width fixed, height grows"""
    def __init__(self, width):
        self.W = width
        self.x = self.y = self.rowh = 0
        self.H = 0
    def place(self, w, h):
        if self.x + w > self.W:
            self.x = 0
            self.y += self.rowh + 2
            self.rowh = 0
        p = (self.x, self.y)
        self.x += w + 2
        self.rowh = max(self.rowh, h)
        self.H = max(self.H, self.y + self.rowh)
        return p

# ---------------------------------------------------------------- build --
def build(skin, scale, gnames, gfiles):
    S = scale / 100.0
    M = {k: max(1, int(round(v * S))) for k, v in skin["metrics"].items()}
    C = {k: rgb(v) for k, v in skin["roles"].items()}
    C.update({k: rgb(v) for k, v in skin["extra"].items()})
    hair = over(C["HAIR"], C["PANEL"])

    gsz  = M["glyph"]
    tw, th = max(1, int(round(30*S))), max(1, int(round(28*S)))
    acc  = M["btnacc"]

    parts = {}          # region id -> list of PIL images, one per state

    # --- 0 PANELTOP: vertical mica wash, tileable across ------------------
    ptw = 16 * ((max(16, int(round(64 * S))) + 15) // 16)
    a = Art(ptw, M["paneltop_h"], C["PANEL"])
    a.vgrad(0, 0, ptw, M["paneltop_h"], C["PANEL_TOP"], C["PANEL"])
    a.rect(0, 0, ptw, max(1, M["border"]), hair)
    parts[RG_PANELTOP] = [a.finish()]

    # --- 1 GROUP: inset surface (playlist ground, art tile, video frame) --
    side = M["radius_group"] * 3 + 2 * M["border"]
    a = Art(side, side, C["PANEL"])
    a.rrect(0, 0, side, side, M["radius_group"], fill=C["PAPER"],
            outline=C["BORDER"], width=M["border"])
    parts[RG_GROUP] = [a.finish()]
    ins_group = M["radius_group"] + M["border"]

    # --- 2 ROWSEL: selected playlist row + accent rail --------------------
    rh = M["row_h"]
    a = Art(side, rh, C["PAPER"])
    a.rect(0, 0, side, rh, C["SELBG"])
    a.rect(0, 0, M["rail"], rh, C["ACCENT"])
    parts[RG_ROWSEL] = [a.finish()]
    ins_row = (M["rail"] + 2, 2, 2, 2)

    # --- 3 SEEK: track / buffered / fill ----------------------------------
    sh = M["seek_h"]
    sw = sh * 4
    seek = []
    for fill, base in ((C["PAPER"], C["PANEL"]), (C["BORDER"], C["PANEL"]),
                       (C["ACCENT"], C["PANEL"])):
        a = Art(sw, sh, base)
        a.rrect(0, 0, sw, sh, sh // 2, fill=fill)
        seek.append(a.finish())
    parts[RG_SEEK] = seek
    ins_seek = (sh // 2 + 1, 0, sh // 2 + 1, 0)

    # --- 4 KNOB -----------------------------------------------------------
    kn = M["knob"]
    knob = []
    for c in (C["ACCENT"], mix(C["ACCENT"], (255,255,255), .18), C["ACCENT_DEEP"]):
        a = Art(kn, kn, C["PANEL"])
        a.ellipse(0, 0, kn, kn, fill=C["PANEL_TOP"])
        a.ellipse(max(1, kn//6), max(1, kn//6), kn - 2*max(1, kn//6), kn - 2*max(1, kn//6), fill=c)
        knob.append(a.finish())
    parts[RG_KNOB] = knob

    # --- 5 BADGE ----------------------------------------------------------
    bh = M["badge_h"]
    bw = M["radius_badge"] * 4 + 4
    badge = []
    for fill, out in ((C["PANEL"], C["BORDER"]), (C["ACCENT"], None)):
        a = Art(bw, bh, C["PANEL"])
        a.rrect(0, 0, bw, bh, M["radius_badge"], fill=fill, outline=out,
                width=M["border"])
        badge.append(a.finish())
    parts[RG_BADGE] = badge
    ins_badge = (M["radius_badge"] + 1, 0, M["radius_badge"] + 1, 0)

    # --- 6 ARTPH: album-art placeholder -----------------------------------
    ap = M["art"]
    a = Art(ap, ap, C["PANEL"])
    a.vgrad(0, 0, ap, ap, C["FACE"], C["PAPER"])
    a.rrect(0, 0, ap, ap, M["radius_group"], fill=None,
            outline=C["BORDER"], width=M["border"])
    cov = raster(gfiles[gnames.index("AUDIO")], ap // 2)
    a.paste(tinted(cov, C["MUTED"][:3] + (140,)), ap // 4, ap // 4)
    parts[RG_ARTPH] = [a.finish()]

    # --- 7 BTN: plain plate, no glyph (for labelled buttons) --------------
    bs = M["radius_btn"] * 3 + 2 * M["border"]
    plates = []
    for st in range(NSTATE):
        a = Art(bs, bs, C["PANEL"])
        if st == 0:
            pass                                   # normal == bare panel
        elif st == 1:
            a.rrect(0, 0, bs, bs, M["radius_btn"], fill=C["HOVER"], outline=hair,
                    width=M["border"])
        elif st == 2:
            a.rrect(0, 0, bs, bs, M["radius_btn"], fill=C["PRESSED"],
                    outline=C["BORDER"], width=M["border"])
        else:
            a.rrect(0, 0, bs, bs, M["radius_btn"], fill=C["HOVER"], outline=hair,
                    width=M["border"])
        plates.append(a.finish())
    parts[RG_BTN] = plates
    ins_btn = M["radius_btn"] + M["border"]

    # --- 8 BTNACC: round accent plate, no glyph ---------------------------
    accplate = []
    for st in range(NSTATE):
        col = (C["ACCENT"], mix(C["ACCENT"], (255,255,255), .16),
               C["ACCENT_DEEP"], C["DISABLED"])[st]
        a = Art(acc, acc, C["PANEL"])
        a.ellipse(0, 0, acc, acc, fill=col)
        accplate.append(a.finish())
    parts[RG_BTNACC] = accplate

    # --- 9 TILE: plate + glyph, pre-composited ----------------------------
    gcov = [raster(f, gsz) for f in gfiles]
    tiles = []
    gx, gy = (tw - gsz) // 2, (th - gsz) // 2
    for gi in range(NBTNGLYPH):
        for st in range(NSTATE):
            a = Art(tw, th, C["PANEL"])
            r = M["radius_btn"]
            if st == 1:
                a.rrect(0, 0, tw, th, r, fill=C["HOVER"], outline=hair, width=M["border"])
            elif st == 2:
                a.rrect(0, 0, tw, th, r, fill=C["PRESSED"], outline=C["BORDER"], width=M["border"])
            elif st == 3:
                a.rrect(0, 0, tw, th, r, fill=C["HOVER"], outline=hair, width=M["border"])
            ink = C["ACCENT"] if st == 3 else C["TEXT"]
            a.paste(tinted(gcov[gi], ink), gx, gy)
            tiles.append(a.finish())
    parts[RG_TILE] = tiles

    # --- 10 TILEACC: round accent button with its glyph -------------------
    acov = [raster(f, max(4, int(round(gsz * 1.2)))) for f in gfiles[:NACCGLYPH]]
    ag = acov[0].size[0]
    atiles = []
    for gi in range(NACCGLYPH):
        for st in range(NSTATE):
            col = (C["ACCENT"], mix(C["ACCENT"], (255,255,255), .16),
                   C["ACCENT_DEEP"], C["DISABLED"])[st]
            a = Art(acc, acc, C["PANEL"])
            a.ellipse(0, 0, acc, acc, fill=col)
            a.paste(tinted(acov[gi], C["ACCENT_INK"]), (acc - ag)//2, (acc - ag)//2)
            atiles.append(a.finish())
    parts[RG_TILEACC] = atiles

    # --- 11 VSCROLL: the playlist scrollbar. The same recipe as XaAES's
    # Fluent window slider (win_draw.c apj_thumb): flat track the colour
    # of the ground, a thin mid-grey (DISABLED) thumb, darker while held.
    vw = M["scroll_w"]
    vh_ = vw * 3 + 2
    vs = []
    for col in (C["PAPER"], C["DISABLED"], C["MUTED"]):
        a = Art(vw, vh_, C["PAPER"])
        a.rrect(0, 0, vw, vh_, vw // 2, fill=col)
        vs.append(a.finish())
    parts[RG_VSCROLL] = vs
    ins_vscroll = (0, vw // 2 + 1, 0, vw // 2 + 1)

    bw = M["border"]
    P, B = C["PANEL"], C["BORDER"]
    #   region -> [(fill, border)] per state, and the border width in px
    midc = {
        RG_PANELTOP: ([(P, P)], 0),
        RG_GROUP:    ([(C["PAPER"], B)], bw),
        RG_ROWSEL:   ([(C["SELBG"], C["SELBG"])], 0),
        RG_SEEK:     ([(C["PAPER"], C["PAPER"]), (B, B),
                       (C["ACCENT"], C["ACCENT"])], 0),
        RG_KNOB:     ([(P, P)] * 3, 0),
        RG_BADGE:    ([(P, B), (C["ACCENT"], C["ACCENT"])], bw),
        RG_ARTPH:    ([(P, P)], 0),
        RG_BTN:      ([(P, P), (C["HOVER"], hair),
                       (C["PRESSED"], B), (C["HOVER"], hair)], bw),
        RG_BTNACC:   ([(P, P)] * 4, 0),
        RG_TILE:     ([(P, P)] * (NBTNGLYPH * NSTATE), 0),
        RG_TILEACC:  ([(P, P)] * (NACCGLYPH * NSTATE), 0),
        RG_VSCROLL:  ([(C["PAPER"], C["PAPER"]), (C["DISABLED"], C["DISABLED"]),
                       (C["MUTED"], C["MUTED"])], 0),
    }

    # ------------------------------------------------------------ layout --
    meta = {
        RG_PANELTOP: (1,  (0,0,0,0),                   F_TILEX),
        RG_GROUP:    (1,  (ins_group,)*4,              F_SLICE9),
        RG_ROWSEL:   (1,  ins_row,                     F_SLICE9),
        RG_SEEK:     (3,  ins_seek,                    F_SLICE9),
        RG_KNOB:     (3,  (0,0,0,0),                   0),
        RG_BADGE:    (2,  ins_badge,                   F_SLICE9),
        RG_ARTPH:    (1,  (0,0,0,0),                   0),
        RG_BTN:      (4,  (ins_btn,)*4,                F_SLICE9),
        RG_BTNACC:   (4,  (0,0,0,0),                   0),
        RG_TILE:     (NBTNGLYPH*NSTATE, (0,0,0,0),     0),
        RG_TILEACC:  (NACCGLYPH*NSTATE, (0,0,0,0),     0),
        RG_VSCROLL:  (3,  ins_vscroll,                 F_SLICE9),
    }
    COLS = {RG_TILE: NSTATE * 4, RG_TILEACC: NSTATE * 2}

    SHEET_W = 16 * ((int(round(560 * S)) + 15) // 16)
    shelf = Shelf(SHEET_W)
    place = {}
    for rid in range(RG_N):
        ims = parts[rid]
        w, h = ims[0].size
        cols = COLS.get(rid, len(ims))
        rows = (len(ims) + cols - 1) // cols
        place[rid] = shelf.place(w * cols, h * rows) + (w, h, cols)

    # glyph coverage atlas rides in the same sheet's coordinate space but in
    # its own 8-bit chunk, laid out as one row of nglyph cells
    gcols = max(1, SHEET_W // gsz)
    grows = (len(gcov) + gcols - 1) // gcols

    SHEET_H = ((shelf.H + 3) // 4) * 4
    sheet = Image.new("RGBA", (SHEET_W, SHEET_H), C["PANEL"])
    for rid in range(RG_N):
        x, y, w, h, cols = place[rid]
        for i, im in enumerate(parts[rid]):
            sheet.alpha_composite(im, (x + (i % cols) * w, y + (i // cols) * h))

    mask = Image.new("L", (gcols * gsz, grows * gsz), 0)
    for i, cv in enumerate(gcov):
        mask.paste(cv, ((i % gcols) * gsz, (i // gcols) * gsz))

    return dict(scale=scale, sheet=sheet, place=place, meta=meta, midc=midc,
                mask=mask, gsz=gsz, gcols=gcols, nglyph=len(gcov),
                colours=C, metrics=M, tile=(tw, th), acc=acc)

# ----------------------------------------------------------------- write --
def fnv1a(b):
    h = 0x811C9DC5
    for c in b:
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h

def write_skn(b, path):
    sheet, place, meta = b["sheet"], b["place"], b["meta"]
    W, H = sheet.size
    C = b["colours"]

    regn = b""
    midc = b""
    for rid in range(RG_N):
        x, y, w, h, cols = place[rid]
        nst, ins, flags = meta[rid]
        states, bw = b["midc"][rid]
        assert len(states) == nst, (RG_NAME[rid], len(states), nst)
        regn += struct.pack(">6H4B2HI", x, y, w, h, nst, cols,
                            ins[0], ins[1], ins[2], ins[3],
                            flags, bw, fnv1a(RG_NAME[rid].encode()))
        for f, bo in states:
            midc += struct.pack(">4B4B", 0, f[0], f[1], f[2],
                                         0, bo[0], bo[1], bo[2])

    palt = b""
    for name in ROLES + EXTRA:
        c = C[name]
        palt += struct.pack(">4B", c[3] if name == "HAIR" else 0, c[0], c[1], c[2])
    palt += b"\0" * (4 * (PALT_N - len(ROLES) - len(EXTRA)))

    mask = b["mask"]
    mw, mh = mask.size
    head_len = 52
    off_regn = head_len
    off_palt = off_regn + len(regn)
    off_midc = off_palt + len(palt)
    off_mask = off_midc + len(midc)
    off_pixl = (off_mask + mw * mh + 15) & ~15
    PIXFMT_RGB24 = 0

    pixl = sheet.convert("RGB").tobytes()        # RGB24, row-major, top-down

    head = struct.pack(">4s7H2H4I", b"APJS", 1, b["scale"], W, H, RG_N,
                       b["nglyph"], b["gsz"], b["gcols"], mw,
                       off_regn, off_palt, off_mask, off_pixl)
    head += struct.pack(">2H", b["tile"][0], b["tile"][1])
    head += struct.pack(">3H", b["acc"], PIXFMT_RGB24, 0)
    head += struct.pack(">I", off_midc)
    assert len(head) == head_len, len(head)

    out = bytearray()
    out += head
    out += regn
    out += palt
    out += midc
    out += mask.tobytes()
    out += b"\0" * (off_pixl - len(out))
    out += pixl
    open(path, "wb").write(bytes(out))
    return len(out), W, H

# ------------------------------------------------------------------ main --
def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__.strip())
    here = os.path.dirname(os.path.abspath(__file__))
    which, outdir = argv[1], argv[2]
    scales = [int(s) for s in argv[3:]] or [100, 125, 175]
    os.makedirs(outdir, exist_ok=True)

    gnames, gfiles = load_glyphs(os.path.join(here, "glyphs"))
    tdir = os.path.join(here, "tokens")
    skins = sorted(f[:-5] for f in os.listdir(tdir) if f.endswith(".json")) \
            if which == "all" else [which]

    for name in skins:
        skin = json.load(open(os.path.join(tdir, name + ".json")))
        for sc in scales:
            b = build(skin, sc, gnames, gfiles)
            stem = skin.get("file") or name.upper()[:4]
            base = "%s%d" % (stem, sc)
            if len(base) > 8:
                sys.exit("%s: '%s' is not an 8.3 name - shorten \"file\" in the token file" % (name, base))
            p = os.path.join(outdir, base + ".SKN")
            n, W, Hh = write_skn(b, p)
            b["sheet"].convert("RGB").save(p[:-4] + ".png")
            print("%-14s %3d%%  %4dx%-4d  %7.1f KB  %s" %
                  (name, sc, W, Hh, n / 1024.0, os.path.basename(p)))

if __name__ == "__main__":
    main(sys.argv)
