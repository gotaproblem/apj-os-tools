#!/usr/bin/env python3
"""
APJSKIN preview / reference reader.

Parses a .SKN with no other input and composites the MP3GEM and VIDGEM
windows out of it, using only the operations the 68k side can do:

    blit9 / blit   ->  vro_cpyfm(S_ONLY) rectangles out of the sheet
    fill           ->  v_bar with a theme pen
    glyph          ->  the 8-bit coverage atlas, blended (the one place
                       the guest touches pixels)
    text           ->  apj_text() through XaAES's antialiased atlas

If a window looks right here it will look right on the Atari, because
nothing here is available to preview.py that is not available to the app.

Usage: preview.py [--mp3 | --psctrl | --psmon] <file.SKN> <out.png> [tab]
"""
import os, struct, sys
from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------- fonts --
#
# DejaVu Sans Mono is not a nicety here: it is the face mkatlas.py renders
# into XaAES's antialiased text atlas, so it is literally what the Atari
# draws with, and a preview in another face has the wrong advance widths -
# which is exactly the thing this file exists to check. So DejaVu is
# hunted for in every place the three platforms put it, and only if none
# of them has it does it fall back, with a warning, to something local.
#
# The paths used to be a pair of hardcoded Linux ones, which was fine for
# as long as this only ever ran in a Linux container.

_MONO_PATHS = [
    # Linux
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
    # macOS: the Homebrew cask and MacPorts, then a manual install
    "/opt/homebrew/share/fonts/DejaVuSansMono.ttf",
    "/usr/local/share/fonts/DejaVuSansMono.ttf",
    "/opt/local/share/fonts/dejavu-fonts/DejaVuSansMono.ttf",
    os.path.expanduser("~/Library/Fonts/DejaVuSansMono.ttf"),
    "/Library/Fonts/DejaVuSansMono.ttf",
]
_SANS_PATHS = [p.replace("SansMono", "Sans") for p in _MONO_PATHS]

# last resort, per platform - the render will be readable but the text
# metrics will not match the Atari's
_MONO_FALLBACK = ["/System/Library/Fonts/Menlo.ttc",
                  "/System/Library/Fonts/Monaco.ttf",
                  "/Library/Fonts/Courier New.ttf",
                  "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"]
_SANS_FALLBACK = ["/System/Library/Fonts/Helvetica.ttc",
                  "/System/Library/Fonts/SFNS.ttf",
                  "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"]

_warned = []


def _pick(paths, fallback, what):
    for p in paths:
        if os.path.exists(p):
            return p
    for p in fallback:
        if os.path.exists(p):
            if what not in _warned:
                _warned.append(what)
                sys.stderr.write(
                    "preview.py: DejaVu %s not found, using %s. The picture "
                    "is right but the text widths are not the Atari's - "
                    "install DejaVu for a faithful preview "
                    "(brew install --cask font-dejavu).\n"
                    % (what, os.path.basename(p)))
            return p
    return None


MONO = _pick(_MONO_PATHS, _MONO_FALLBACK, "Sans Mono")
SANS = _pick(_SANS_PATHS, _SANS_FALLBACK, "Sans")

_fontcache = {}


def _font(path, px):
    """cached: tw_of() is called per string and building a face each time
    made a full render measurably slower than the blits it is checking"""
    key = (path, px)
    if key not in _fontcache:
        if path is None:
            _fontcache[key] = ImageFont.load_default()
        else:
            _fontcache[key] = ImageFont.truetype(path, px)
    return _fontcache[key]

(RG_PANELTOP, RG_GROUP, RG_ROWSEL, RG_SEEK, RG_KNOB, RG_BADGE,
 RG_ARTPH, RG_BTN, RG_BTNACC, RG_TILE, RG_TILEACC, RG_VSCROLL,
 RG_TAB, RG_TABBAR, RG_RADIO, RG_CHECK, RG_FIELD, RG_POPUP,
 RG_STATUS, RG_CHEV) = range(20)

# sheet version 2 adds RG_TAB..RG_STATUS and two BADGE states
BG_PLAIN, BG_ACCENT, BG_WARN, BG_DANGER = range(4)
FLD_NORM, FLD_FOCUS, FLD_DIS = range(3)


def CK(on, st):
    """RADIO/CHECK state: four appearances for off then four for on"""
    return (4 + st) if on else st

ROLES = ["FACE","TEXT","LIGHT","DARK","SELBG","SELFG","ALBG","ALFG","PANEL","TITBG",
         "TITFG","PAPER","BORDER","HOVER","PRESSED","FOCUS","DISABLED","ELEVATION","ACCENT"]
EXTRA = ["ACCENT_INK","ACCENT_DEEP","MUTED","PANEL_TOP","ROW","HAIR"]

G_PLAY, G_PAUSE, G_STOP, G_PREV, G_NEXT, G_RW, G_FF, G_SHUFFLE, G_REPEAT, \
G_REPEAT1, G_VOL, G_VOLLOW, G_MUTE, G_OPEN, G_LIST, G_FULL, G_UNFULL, \
G_EJECT, G_INFO, G_AUDIO, G_VIDEO = range(21)

# NOTE: the window layout below is illustrative. mp3gem/mp3ui.c is the
# authority for MP3GEM's real geometry; this file exists to prove a .SKN
# is self-describing and to show what the finished window should look like.

ST_NORM, ST_HOVER, ST_PRESS, ST_ON = range(4)


class Skin:
    def __init__(self, path):
        d = open(path, "rb").read()
        (magic, ver, self.scale, self.W, self.H, nreg, self.nglyph,
         self.gsz, self.gcols, self.mw, o_regn, o_palt, o_mask,
         o_pixl) = struct.unpack(">4s7H2H4I", d[:38])
        self.tw, self.th, self.acc, pixfmt, _ = struct.unpack(">5H", d[38:48])
        o_midc, = struct.unpack(">I", d[48:52])
        if magic != b"APJS" or ver not in (1, 2):
            sys.exit("not an APJSKIN v1 or v2 file")
        self.ver = ver
        self.nreg = nreg
        if pixfmt != 0:
            sys.exit("unknown pixel format %d" % pixfmt)

        self.reg = []
        for i in range(nreg):
            b = d[o_regn + i*24: o_regn + i*24 + 24]
            x, y, w, h, nst, cols, l, t, r, bo, flags, bw, _hash = \
                struct.unpack(">6H4B2HI", b)
            self.reg.append(dict(x=x, y=y, w=w, h=h, n=nst, cols=cols,
                                 ins=(l, t, r, bo), flags=flags, bw=bw))

        # MIDC: fill and border colour per region state, so the flat parts
        # of a nine-slice are drawn as fills instead of tiled blits
        k = o_midc
        for rg in self.reg:
            rg["mid"] = []
            for _ in range(rg["n"]):
                a1, r1, g1, b1, a2, r2, g2, b2 = struct.unpack(">8B", d[k:k+8])
                rg["mid"].append(((r1, g1, b1, 255), (r2, g2, b2, 255)))
                k += 8

        self.pal = {}
        for i, name in enumerate(ROLES + EXTRA):
            a, r, g, b = struct.unpack(">4B", d[o_palt + i*4: o_palt + i*4 + 4])
            self.pal[name] = (r, g, b, 255 if name != "HAIR" else a)

        mh = (self.nglyph + self.gcols - 1) // self.gcols * self.gsz
        self.mask = Image.frombytes("L", (self.mw, mh), d[o_mask: o_mask + self.mw*mh])
        self.sheet = Image.frombytes("RGB", (self.W, self.H),
                                     d[o_pixl: o_pixl + self.W*self.H*3]).convert("RGBA")
        self.S = self.scale / 100.0

    # ---- metrics -------------------------------------------------------
    def m(self, pt):
        return max(1, int(round(pt * self.S)))

    # ---- primitives the 68k side has ----------------------------------
    def src(self, rid, state):
        r = self.reg[rid]
        s = min(state, r["n"] - 1)
        return (r["x"] + (s % r["cols"]) * r["w"],
                r["y"] + (s // r["cols"]) * r["h"], r["w"], r["h"])

    def blit(self, dst, rid, state, x, y):
        sx, sy, w, h = self.src(rid, state)
        dst.paste(self.sheet.crop((sx, sy, sx+w, sy+h)), (x, y))

    def blit9(self, dst, rid, state, x, y, w, h):
        """corners out of the sheet, edges and middle filled - exactly what
        apj_skin_9() does, so this stays a faithful reference"""
        rg = self.reg[rid]
        sx, sy, sw, sh = self.src(rid, state)
        l, t, r, b = rg["ins"]
        bw = rg["bw"]
        st = min(state, rg["n"] - 1)
        fill, bord = rg["mid"][st]
        d = ImageDraw.Draw(dst)

        def bar(bx, by, bwid, bhgt, col):
            if bwid > 0 and bhgt > 0:
                d.rectangle([bx, by, bx + bwid - 1, by + bhgt - 1], fill=col)

        def piece(px, py, pw, ph, dx, dy):
            if pw > 0 and ph > 0:
                dst.paste(self.sheet.crop((px, py, px + pw, py + ph)), (dx, dy))

        if l + r >= w:
            l, r = w // 2, w - w // 2
        if t + b >= h:
            t, b = h // 2, h - h // 2
        bw = min(bw, l, r)
        dmw, dmh = w - l - r, h - t - b

        if rg["ins"][0] == 0 and rg["ins"][2] == 0:      # vertical pill
            piece(sx, sy, sw, t, x, y)
            piece(sx, sy + sh - b, sw, b, x, y + h - b)
            if bw:
                bar(x, y + t, bw, dmh, bord)
                bar(x + w - bw, y + t, bw, dmh, bord)
            bar(x + bw, y + t, w - 2*bw, dmh, fill)
            return

        if rg["ins"][1] == 0 and rg["ins"][3] == 0:      # pill
            piece(sx, sy, l, sh, x, y)
            piece(sx + sw - r, sy, r, sh, x + w - r, y)
            if bw:
                bar(x + l, y, dmw, bw, bord)
                bar(x + l, y + h - bw, dmw, bw, bord)
            bar(x + l, y + bw, dmw, h - 2*bw, fill)
            return

        piece(sx, sy, l, t, x, y)
        piece(sx + sw - r, sy, r, t, x + w - r, y)
        piece(sx, sy + sh - b, l, b, x, y + h - b)
        piece(sx + sw - r, sy + sh - b, r, b, x + w - r, y + h - b)
        if bw:
            bar(x + l, y, dmw, bw, bord)
            bar(x + l, y + h - bw, dmw, bw, bord)
            bar(x, y + t, bw, dmh, bord)
            bar(x + w - bw, y + t, bw, dmh, bord)
        bar(x + l, y + bw, dmw, t - bw, fill)
        bar(x + l, y + h - b, dmw, b - bw, fill)
        bar(x + bw, y + t, l - bw, dmh, fill)
        bar(x + w - r, y + t, r - bw, dmh, fill)
        bar(x + l, y + t, dmw, dmh, fill)

    def tilex(self, dst, rid, state, x, y, w):
        sx, sy, sw, sh = self.src(rid, state)
        piece = self.sheet.crop((sx, sy, sx+sw, sy+sh))
        for i in range(0, w, sw):
            dst.paste(piece.crop((0, 0, min(sw, w-i), sh)), (x+i, y))

    def fill(self, dst, x, y, w, h, role):
        ImageDraw.Draw(dst).rectangle([x, y, x+w-1, y+h-1], fill=self.pal[role])

    def glyph(self, dst, gid, x, y, role):
        g = self.gsz
        cov = self.mask.crop(((gid % self.gcols)*g, (gid // self.gcols)*g,
                              (gid % self.gcols)*g + g, (gid // self.gcols)*g + g))
        im = Image.new("RGBA", (g, g), self.pal[role][:3] + (0,))
        im.putalpha(cov)
        dst.alpha_composite(im, (x, y))

    # ---- text (XaAES draws this for us through opcode 114) -------------
    def text(self, dst, x, y, s, role, pt=11, bold=False, mono=True):
        f = _font(MONO if mono else SANS, self.m(pt))
        col = (0, 0, 0, 255) if role == "BLACK" else self.pal[role]
        ImageDraw.Draw(dst).text((x, y), s, font=f, fill=col)

    def tw_of(self, s, pt=11, mono=True):
        f = _font(MONO if mono else SANS, self.m(pt))
        return int(ImageDraw.Draw(Image.new("RGB", (1, 1))).textlength(s, font=f))


# ======================================================== MP3GEM window ==
def mp3gem(sk, W_pt=480, H_pt=320):
    m = sk.m
    W, H = m(W_pt), m(H_pt)
    im = Image.new("RGBA", (W, H), sk.pal["PANEL"])
    pad = m(10)

    # body: flat panel + the mica band across the top
    sk.fill(im, 0, 0, W, H, "PANEL")
    sk.tilex(im, RG_PANELTOP, 0, 0, 0, W)

    # --- now playing ----------------------------------------------------
    art = m(96)
    sk.blit(im, RG_ARTPH, 0, pad, pad)
    ix = pad + art + m(12)
    sk.text(im, ix, pad + m(2), "Tears in Rain", "TEXT", 14, mono=False)
    sk.text(im, ix, pad + m(22), "Vangelis - Blade Runner (OST)", "MUTED", 11, mono=False)

    bx = ix
    for label, st in (("MPEG-1 LAYER III", 0), ("320 kbps", 0), ("PLAYING", 1)):
        tw = sk.tw_of(label, 9)
        bw = tw + m(12)
        sk.blit9(im, RG_BADGE, st, bx, pad + m(46), bw, m(16))
        sk.text(im, bx + m(6), pad + m(48), label,
                "ACCENT_INK" if st else "MUTED", 9)
        bx += bw + m(6)

    # --- seek -----------------------------------------------------------
    sy = pad + art + m(12)
    trk_h = m(6)
    t0 = pad + m(46)
    t1 = W - pad - m(46)
    sk.text(im, pad, sy - m(1), "01:47", "MUTED", 11)
    sk.text(im, W - pad - m(42), sy - m(1), "-03:00", "MUTED", 11)
    sk.blit9(im, RG_SEEK, 0, t0, sy + m(3), t1 - t0, trk_h)
    fw = int((t1 - t0) * 0.38)
    sk.blit9(im, RG_SEEK, 2, t0, sy + m(3), fw, trk_h)
    kn = m(12)
    sk.blit(im, RG_KNOB, 0, t0 + fw - kn // 2, sy + m(3) + trk_h // 2 - kn // 2)

    # --- transport ------------------------------------------------------
    ty = sy + m(22)
    tw_, th_ = sk.tw, sk.th
    x = pad
    for gid, st in ((G_PREV, ST_NORM), (G_RW, ST_NORM)):
        sk.blit(im, RG_TILE, gid*4 + st, x, ty); x += tw_ + m(2)
    accd = sk.acc
    sk.blit(im, RG_TILEACC, G_PAUSE*4 + ST_NORM, x, ty - (accd - th_)//2)
    x += accd + m(2)
    for gid, st in ((G_FF, ST_NORM), (G_NEXT, ST_NORM), (G_STOP, ST_NORM)):
        sk.blit(im, RG_TILE, gid*4 + st, x, ty); x += tw_ + m(2)

    rx = W - pad - tw_
    sk.blit(im, RG_TILE, G_OPEN*4 + ST_NORM, rx, ty)
    vol_w = m(72)
    rx -= vol_w + m(8)
    sk.blit9(im, RG_SEEK, 0, rx, ty + th_//2 - m(3), vol_w, trk_h)
    sk.blit9(im, RG_SEEK, 2, rx, ty + th_//2 - m(3), int(vol_w*0.72), trk_h)
    rx -= tw_ + m(4)
    sk.blit(im, RG_TILE, G_VOL*4 + ST_NORM, rx, ty)
    rx -= tw_ + m(2)
    sk.blit(im, RG_TILE, G_REPEAT*4 + ST_ON, rx, ty)
    rx -= tw_ + m(2)
    sk.blit(im, RG_TILE, G_SHUFFLE*4 + ST_HOVER, rx, ty)

    # --- playlist -------------------------------------------------------
    ly = ty + th_ + m(10)
    lh = H - ly - pad - m(14)
    sk.blit9(im, RG_GROUP, 0, pad, ly, W - 2*pad, lh)
    rowh = m(24)
    rows = [("1", "Main Titles", "3:42", 0),
            ("", "Tears in Rain", "4:47", 1),
            ("3", "Love Theme", "4:56", 0),
            ("4", "Blade Runner Blues", "8:52", 0),
            ("5", "Memories of Green", "5:44", 0)]
    iy = ly + m(4)
    for n, t, d, sel in rows:
        if iy + rowh > ly + lh - m(4):
            break
        if sel:
            sk.blit9(im, RG_ROWSEL, 0, pad + m(2), iy, W - 2*pad - m(4), rowh)
            sk.glyph(im, G_PLAY, pad + m(10), iy + (rowh - sk.gsz)//2, "ACCENT")
        else:
            sk.text(im, pad + m(12), iy + m(4), n, "MUTED", 11)
        sk.text(im, pad + m(34), iy + m(4), t, "SELFG" if sel else "TEXT", 11)
        sk.text(im, W - pad - m(46), iy + m(4), d, "MUTED", 11)
        iy += rowh

    # the scrollbar, inside the list on the right
    svw = m(10)
    sx_ = pad + (W - 2*pad) - m(4) - svw
    sy_ = ly + m(4)
    sh_ = lh - m(8)
    sk.blit9(im, RG_VSCROLL, 0, sx_, sy_, svw, sh_)
    sk.blit9(im, RG_VSCROLL, 1, sx_, sy_, svw, sh_ * 5 // 24 + m(10))

    sk.text(im, pad, H - pad - m(10), "24 tracks - 1:52:08   S:\\MUSIC\\VANGELIS",
            "MUTED", 9)
    sk.text(im, W - pad - sk.tw_of("HOSTFS - mpg123 core 1", 9),
            H - pad - m(10), "HOSTFS - mpg123 core 1", "MUTED", 9)
    return im


# ======================================================== PSCTRL window ==
#
# The reference for psctrl/psui.c: the same geometry, drawn with the same
# primitives, from the same file. If a tab looks right here it will look
# right on the Atari.
#
# The model below stands in for the emulator's descriptor table. Nothing
# about it is compiled into the real app either - it asks the host.

# Strip order, not enum order. Bench is tab 10 on the wire (tab numbers
# are wire values, so they can only be appended) but sits beside JIT.
TAB_NAMES  = ["JIT", "Bench", "CPU", "Video", "Audio", "Input",
              "STBox", "Floppy", "Net", "Debug", "Adv"]
TAB_ORDER  = [0, 10, 1, 2, 3, 4, 5, 6, 7, 8, 9]
TAB_BENCH  = 10

K_BOOL, K_ENUM, K_INT, K_STR, K_ACTION, K_INFO = range(6)
C_LIVE, C_DEFER, C_BOOT, C_BOXBOOT, C_RO = range(5)

# title, kind, class, value, extra
JIT_ROWS = [
    ("JIT power",          K_INT,    C_LIVE,  3,   dict(lo=0, hi=6, txt="3  (1024)")),
    ("68k speed",          K_INT,    C_LIVE,  -1,  dict(lo=-1, hi=20, txt="max")),
    ("Clock multiplier",   K_INT,    C_LIVE,  2,   dict(lo=1, hi=8, txt="2")),
    ("Translation cache",  K_ENUM,   C_DEFER, 3,   dict(labels=["off","2048K","4096K","8192K","16384K"])),
    ("Follow constant jumps", K_BOOL, C_DEFER, 1,  dict()),
    ("Skip dead flags",    K_BOOL,   C_DEFER, 1,   dict()),
    ("Flush cache now",    K_ACTION, C_DEFER, 0,   dict()),
    ("JIT hit rate",       K_INFO,   C_RO,    0,   dict(txt="99.8%")),
    ("Guest idle",         K_INFO,   C_RO,    0,   dict(txt="41.2%")),
    ("Cache used",         K_INFO,   C_RO,    0,   dict(txt="1.1 MB")),
]

CPU_ROWS = [
    ("Machine",            K_ENUM,   C_BOOT,  1,   dict(labels=["st","ste","megast"])),
    ("CPU",                K_ENUM,   C_BOOT,  3,   dict(labels=["68000","68010","68020","68030","68040","68060"])),
    ("FPU",                K_BOOL,   C_BOOT,  1,   dict()),
    ("Prefetch-accurate 68000", K_BOOL, C_BOOT, 0, dict()),
    ("Blitter",            K_ENUM,   C_BOOT,  2,   dict(labels=["off","real chip","emulated"])),
    ("TT-RAM",             K_ENUM,   C_BOOT,  4,   dict(labels=["off","16M","32M","64M","128M","256M"])),
    ("TOS image",          K_STR,    C_BOOT,  0,   dict(txt="...ms/tos206uk.img")),
    ("Blitter bus cost",   K_INT,    C_LIVE,  0,   dict(lo=0, hi=2000, txt="instant")),
]

BENCH_SECS = [
    ("Processor", [
        ("Dhrystone", None, "308.27 DMIPS  541638 Dhry/s  x386.8 ST", "298.1 DMIPS"),
        ("CoreMark",  None, "1050.97  (020+ build, 19.0 s)", "1012.4 020+"),
        ("ALU",       None, "1298.91 MIPS", "1240.0 MIPS"),
        ("FPU",       None, "410.40 MFLOPS", "402.1 MFLOPS"),
        ("Tight loop", None, "141.94 MIPS  (~6.3 ns per block exit)", "140.02 MIPS"),
    ]),
    ("Memory", [
        ("ST RAM", None, "81.92 MIPS  (move.l through ST RAM)", "80.11 MIPS"),
        ("Mixed",  None, "238.93 MIPS  (ALU, branch, ld/st, jsr)", "245.0 MIPS"),
    ]),
    ("Graphics", [
        ("Frame rate", None, "2001.0 fps  499 us/frame  160x120 32bpp", "1880.0 fps"),
        ("Rasterise",    86, "86%  (1600 of 1855 ms)", "88%"),
        ("Blit",         13, "13%  (255 ms, fVDI)", "11%"),
        ("Throughput", None, "92002 ktri/s  13386 kpix/s  1091764 KB/s", "84120 ktri/s"),
    ]),
]

BENCH = [
    "Dhrystone 2.1   18400 Dhry/s   10.47 DMIPS   x13.1 vs 8 MHz ST",
    "MIPS   mixed 7.82   ALU 21.40",
    "       ST-RAM move.l 4.11",
    "MFLOPS 1.92   (fmul/fadd chain, FPU on)",
    "BogoMIPS 412.00   (dbra only - the JIT deletes it; a ceiling)",
    "Reference: 1400 Dhry/s = 1040 ST at 8 MHz, GCC 4.6.4 (firebee.org)",
]

BADGE_FOR = {
    C_LIVE:    ("live",    BG_PLAIN),
    C_DEFER:   ("defer",   BG_WARN),
    C_BOOT:    ("boot",    BG_PLAIN),
    C_BOXBOOT: ("box",     BG_PLAIN),
    C_RO:      ("",        BG_PLAIN),
}


def use_radios(sk, labels, ctlw, pt):
    """psui.c's rule: at most four choices AND every label fits"""
    if not (2 <= len(labels) <= 4):
        return False
    each = ctlw // len(labels)
    rw = sk.reg[RG_RADIO]["w"]
    return all(sk.tw_of(l, pt) <= each - rw - sk.m(6) for l in labels)


def psctrl(sk, tab=0, W_pt=620, H_pt=510):
    m = sk.m
    W, H = m(W_pt), m(H_pt)
    im = Image.new("RGBA", (W, H), sk.pal["PANEL"])
    sk.fill(im, 0, 0, W, H, "PANEL")

    pad, tabh, stath = m(10), m(30), m(18)
    rowh, fieldh = m(26), m(22)
    small, body = 9, 10

    # --- the tab strip ---------------------------------------------------
    sk.tilex(im, RG_TABBAR, 0, 0, 0, W)
    tw = W // len(TAB_NAMES)
    for i, nm in enumerate(TAB_NAMES):
        x = i * tw
        w = (W - x) if i == len(TAB_NAMES) - 1 else tw
        sel = (TAB_ORDER[i] == tab)
        sk.blit9(im, RG_TAB, ST_ON if sel else ST_NORM, x, 0, w, tabh)
        if sel:
            # the rail is a v_bar, not part of the plate - nine-slice
            # fills the edge bands flat (see psui.c)
            sk.fill(im, x + m(6), tabh - m(3), w - m(12), m(3), "ACCENT")
        t = sk.tw_of(nm, body)
        sk.text(im, x + (w - t)//2, (tabh - m(body) - m(3))//2 + m(1), nm,
                "TEXT" if sel else "MUTED", body)

    rows = [] if tab == TAB_BENCH else (JIT_ROWS if tab == 0 else CPU_ROWS)

    ly = tabh + pad//2
    lh = H - stath - pad//2 - ly
    benchh = 0
    if tab == TAB_BENCH:
        # its own tab: all panel, no rows
        benchh, lh = lh, 0

    lx, lw = pad, W - 2*pad
    visible = min(len(rows), lh // rowh)
    need_sb = len(rows) > visible
    sbw = m(6) + m(8) if need_sb else 0

    ctlw = m(180)
    ctlx = lx + lw - sbw - m(44) - m(8) - ctlw

    # --- rows ------------------------------------------------------------
    for i in range(visible):
        title, kind, klass, val, ex = rows[i]
        y = ly + i * rowh
        sk.fill(im, lx, y, lw, rowh, "ROW" if (i & 1) else "PANEL")
        if i == 1:
            sk.fill(im, lx, y, lw, rowh, "HOVER")

        ty = y + (rowh - m(body) - m(3))//2 + m(1)
        if kind != K_ACTION:
            sk.text(im, lx + m(4), ty, title,
                    "MUTED" if klass == C_RO else "TEXT", body)

        if kind == K_BOOL:
            cw_, ch_ = sk.reg[RG_CHECK]["w"], sk.reg[RG_CHECK]["h"]
            sk.blit(im, RG_CHECK, CK(val, ST_NORM), ctlx, y + (rowh - ch_)//2)
            sk.text(im, ctlx + cw_ + m(6), ty, "on" if val else "off",
                    "MUTED", body)

        elif kind == K_ENUM and use_radios(sk, ex.get("labels", []), ctlw, small):
            labels = ex["labels"]
            each = ctlw // len(labels)
            rw, rh = sk.reg[RG_RADIO]["w"], sk.reg[RG_RADIO]["h"]
            for k, lab in enumerate(labels):
                gx = ctlx + k * each
                sk.blit(im, RG_RADIO, CK(k == val, ST_NORM), gx,
                        y + (rowh - rh)//2)
                sk.text(im, gx + rw + m(3), ty + m(1), lab,
                        "TEXT" if k == val else "MUTED", small)

        elif kind == K_ENUM:
            fy = y + (rowh - fieldh)//2
            sk.blit9(im, RG_POPUP, ST_NORM, ctlx, fy, ctlw, fieldh)
            cvw, cvh = sk.reg[RG_CHEV]["w"], sk.reg[RG_CHEV]["h"]
            sk.blit(im, RG_CHEV, ST_NORM, ctlx + ctlw - cvw - m(4),
                    fy + (fieldh - cvh)//2)
            sk.text(im, ctlx + m(7), fy + (fieldh - m(body) - m(3))//2 + m(1),
                    ex["labels"][val], "TEXT", body)

        elif kind == K_INT:
            sw = ctlw - m(56)
            th = m(6)
            sy = y + (rowh - th)//2
            sk.blit9(im, RG_SEEK, 0, ctlx, sy, sw, th)
            lo, hi = ex.get("lo", 0), ex.get("hi", 1)
            fw = int(sw * (val - lo) / float(hi - lo)) if hi > lo else 0
            if fw > 0:
                sk.blit9(im, RG_SEEK, 2, ctlx, sy, fw, th)
            kn = m(12)
            sk.blit(im, RG_KNOB, ST_NORM, ctlx + fw - kn//2, sy + th//2 - kn//2)
            sk.text(im, ctlx + sw + m(8), ty + m(1), ex.get("txt", str(val)),
                    "TEXT", small)

        elif kind == K_STR:
            fy = y + (rowh - fieldh)//2
            sk.blit9(im, RG_FIELD, FLD_NORM, ctlx, fy, ctlw, fieldh)
            sk.text(im, ctlx + m(7), fy + (fieldh - m(body) - m(3))//2 + m(1),
                    ex.get("txt", ""), "TEXT", body)

        elif kind == K_ACTION:
            fy = y + (rowh - fieldh)//2
            sk.blit9(im, RG_BTN, ST_NORM, ctlx, fy, ctlw, fieldh)
            t = sk.tw_of(title, body)
            sk.text(im, ctlx + (ctlw - t)//2,
                    fy + (fieldh - m(body) - m(3))//2 + m(1), title, "TEXT", body)

        else:                                   # K_INFO
            t = sk.tw_of(ex.get("txt", ""), body)
            sk.text(im, ctlx + ctlw - t, ty, ex.get("txt", ""), "TEXT", body)

        lab, st = BADGE_FOR[klass]
        if lab:
            bw_ = sk.tw_of(lab, small) + m(10)
            bx = lx + lw - sbw - bw_
            sk.blit9(im, RG_BADGE, st, bx, y + (rowh - m(16))//2, bw_, m(16))
            # black, not ACCENT_INK: white on the amber warn fill is
            # unreadable on a real screen. Matches draw_badge() in psui.c.
            sk.text(im, bx + m(5), ty + m(1), lab,
                    "MUTED" if st == BG_PLAIN else "BLACK", small)

    if need_sb:
        sbx = lx + lw - m(6)
        sk.blit9(im, RG_VSCROLL, 0, sbx, ly, m(6), lh)
        th = max(m(12), int(lh * visible / float(len(rows))))
        sk.blit9(im, RG_VSCROLL, 1, sbx, ly, m(6), th)

    # --- the benchmark panel --------------------------------------------
    if benchh:
        # the header strip: which CoreMark build the button will run
        hh = m(26)
        sk.text(im, lx, ly + (hh - m(body) - m(3)) // 2,
                "CoreMark build:", "MUTED", body)
        bx = lx + sk.tw_of("X" * 16, body)
        bw = sk.tw_of("X" * 7, body)
        for k, nm in enumerate(("68000", "020+")):
            sel = (k == 1)
            sk.blit9(im, RG_BTN, ST_ON if sel else ST_NORM,
                     bx, ly + m(2), bw, hh - m(4))
            t = sk.tw_of(nm, body)
            sk.text(im, bx + (bw - t) // 2, ly + (hh - m(body) - m(3)) // 2,
                    nm, "TEXT", body)
            bx += bw + m(8)

        by = ly + hh
        bh2 = benchh - hh
        sk.blit9(im, RG_GROUP, 0, lx, by, lw, bh2)

        # a tighter row than the settings tabs: text only, nothing to hit
        secpad, rowh, barh = m(8), m(body) + m(3) + m(6), m(8)
        noteh = 2 * (m(small) + m(3)) + m(4)
        labw = sk.tw_of("X" * 10, body)
        barw = m(64)
        barx = lx + secpad + labw + m(8)
        valx = barx + barw + m(8)
        right = lx + lw - secpad
        prevx = right - sk.tw_of("X" * 12, body)

        ty = by + secpad
        for name, rows in BENCH_SECS:
            sk.text(im, lx + secpad, ty, name, "MUTED", small)
            ty += m(small) + m(3) + m(2)
            for i, (lab, pct, val, prev) in enumerate(rows):
                if i & 1:
                    sk.fill(im, lx + m(4), ty, lw - m(8), rowh, "ROW")
                cy = ty + (rowh - m(body) - m(3)) // 2
                sk.text(im, lx + secpad, cy, lab, "MUTED", body)
                vx = valx
                if pct is None:
                    vx = barx
                else:
                    yb = ty + (rowh - barh) // 2
                    sk.blit9(im, RG_SEEK, 0, barx, yb, barw, barh)
                    f = barw * pct // 100
                    if f > 0:
                        sk.blit9(im, RG_SEEK, 2, barx, yb, f, barh)
                sk.text(im, vx, cy, val, "TEXT", body)
                if prev:
                    t = sk.tw_of(prev, body)
                    sk.text(im, right - t, cy, prev, "MUTED", body)
                ty += rowh
            ty += m(3)

        sk.text(im, lx + secpad, by + bh2 - noteh,
                "CoreMark 1.0 : 12.40 / GCC4.6.4 -O2 -m68020-60 / STACK",
                "MUTED", small)

    # --- the status strip ------------------------------------------------
    sy = H - stath
    sk.tilex(im, RG_STATUS, 0, 0, sy, W)
    sk.text(im, pad, sy + (stath - m(small) - m(3))//2 + m(1),
            "JIT power: applied.", "MUTED", small)
    bw_ = m(84)
    btns = ("Benchmark", "Save .cfg") if tab == TAB_BENCH else ("Save .cfg",)
    for k, lab in enumerate(btns):
        bx = W - pad - (len(btns) - k) * bw_ - (m(8) if k == 0 and
                                                len(btns) > 1 else 0)
        sk.blit9(im, RG_BTN, ST_NORM, bx, sy, bw_, stath)
        t = sk.tw_of(lab, small)
        sk.text(im, bx + (bw_ - t)//2, sy + (stath - m(small) - m(3))//2 + m(1),
                lab, "TEXT", small)

    return im


# ========================================================= PSMON window ==
# The reference for psmon/psmonui.c, the same way psctrl() is for psui.c:
# same geometry, same primitives, from the same .SKN, drawn independently
# in Python. Two rendering bugs in PSCTRL were found this way and neither
# would have been caught by the blit checks.

PM_SECTIONS = [
    ("JIT engine", [
        ("Speed",     None, "32 MHz  (4x ST)"),
        ("JIT hit",     96, "96.4%"),
        ("Idle",        12, "12.0%"),
        ("Cache",       47, "3.8 MB of 8.0 MB, 3 flushes"),
        ("Compile",   None, "256 blk/s"),
        ("SMC inv",   None, "0 /s"),
    ]),
    ("Memory", [
        ("ST RAM",      71, "1.1 MB free of 4.0 MB"),
        ("TT RAM",      31, "88.0 MB free of 128.0 MB"),
    ]),
    ("Host", [
        ("Board",     None, "Pi 4B, 4096 MB"),
        ("ARM clock",   45, "1500 MHz, 62.4 C"),
        ("Health",    None, "ok"),
    ]),
]


def psmon(sk, W_pt=530, H_pt=392):
    m = sk.m
    W, H = m(W_pt), m(H_pt)
    im = Image.new("RGBA", (W, H), sk.pal["PANEL"])
    sk.fill(im, 0, 0, W, H, "PANEL")

    pad, gap, stath = m(10), m(8), m(18)
    rowh, headh, secpad, barh = m(20), m(18), m(8), m(8)
    small, body = 9, 10

    labw = sk.tw_of("X" * 10, body)
    barw = m(72)
    barx = pad + secpad + labw + gap
    valx = barx + barw + gap

    y = pad
    for name, rows in PM_SECTIONS:
        h = headh + len(rows) * rowh + secpad * 2
        sk.blit9(im, RG_GROUP, 0, pad, y, W - 2 * pad, h)
        sk.text(im, pad + secpad, y + (headh - m(small) - m(3)) // 2,
                name, "MUTED", small)

        ry = y + headh + secpad
        for lab, pct, val in rows:
            ty = ry + (rowh - m(body) - m(3)) // 2
            vx = valx
            sk.text(im, pad + secpad, ty, lab, "MUTED", body)
            if pct is None:
                vx = barx
            else:
                by = ry + (rowh - barh) // 2
                sk.blit9(im, RG_SEEK, 0, barx, by, barw, barh)
                fill = barw * pct // 100
                if fill > 0:
                    sk.blit9(im, RG_SEEK, 2, barx, by, fill, barh)
            sk.text(im, vx, ty, val, "TEXT", body)
            ry += rowh
        y += h + gap

    sy = H - stath
    sk.tilex(im, RG_STATUS, 0, 0, sy, W)
    sk.text(im, pad, sy + (stath - m(small) - m(3)) // 2 + m(1),
            "Sampling every 500 ms.  1/2/3 scale, 0 auto.", "MUTED", small)
    return im


def chrome(sk, body, title):
    """XaAES draws this, not the skin - here only so the shot reads right"""
    W = body.size[0]
    tb = sk.m(28)
    im = Image.new("RGBA", (W, body.size[1] + tb), sk.pal["TITBG"])
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, W-1, tb-1], fill=sk.pal["TITBG"])
    f = _font(SANS, sk.m(12))
    d.text((sk.m(10), tb//2 - sk.m(8)), title, font=f, fill=sk.pal["TITFG"])
    im.alpha_composite(body, (0, tb))
    return im


def main(a):
    if len(a) < 3:
        sys.exit(__doc__.strip())
    which = "mp3"
    if a[1] in ("--mp3", "--psctrl", "--psmon"):
        which = a[1][2:]
        a = a[:1] + a[2:]
    sk = Skin(a[1])
    if which == "psmon":
        if sk.nreg <= RG_VSCROLL:
            sys.exit("%s is a version 1 sheet - it has no STATUS region" % a[1])
        body = psmon(sk)
        out = chrome(sk, body, "PiSTorm Monitor")
    elif which == "psctrl":
        if sk.nreg <= RG_VSCROLL:
            sys.exit("%s is a version 1 sheet - it has no PSCTRL regions" % a[1])
        body = psctrl(sk, tab=int(a[3]) if len(a) > 3 else 0)
        out = chrome(sk, body, "PiSTorm Settings")
    else:
        body = mp3gem(sk)
        out = chrome(sk, body, "PiSTorm MP3 - S:\\MUSIC\\VANGELIS")
    pad = sk.m(24)
    bg = Image.new("RGBA", (out.size[0] + 2*pad, out.size[1] + 2*pad),
                   sk.pal["ELEVATION"])
    bg.alpha_composite(out, (pad, pad))
    bg.convert("RGB").save(a[2])
    print("%s  %dx%d  scale %d%%  ->  %s" %
          (a[1], out.size[0], out.size[1], sk.scale, a[2]))


if __name__ == "__main__":
    main(sys.argv)
