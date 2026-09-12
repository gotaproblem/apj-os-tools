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

Usage: preview.py <file.SKN> <out.png>
"""
import struct, sys
from PIL import Image, ImageDraw, ImageFont

MONO = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
SANS = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

(RG_PANELTOP, RG_GROUP, RG_ROWSEL, RG_SEEK, RG_KNOB, RG_BADGE,
 RG_ARTPH, RG_BTN, RG_BTNACC, RG_TILE, RG_TILEACC, RG_VSCROLL) = range(12)

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
        if magic != b"APJS" or ver != 1:
            sys.exit("not an APJSKIN v1 file")
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
        f = ImageFont.truetype(SANS if not mono else MONO, self.m(pt))
        ImageDraw.Draw(dst).text((x, y), s, font=f, fill=self.pal[role])

    def tw_of(self, s, pt=11, mono=True):
        f = ImageFont.truetype(SANS if not mono else MONO, self.m(pt))
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


def chrome(sk, body, title):
    """XaAES draws this, not the skin - here only so the shot reads right"""
    W = body.size[0]
    tb = sk.m(28)
    im = Image.new("RGBA", (W, body.size[1] + tb), sk.pal["TITBG"])
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, W-1, tb-1], fill=sk.pal["TITBG"])
    f = ImageFont.truetype(SANS, sk.m(12))
    d.text((sk.m(10), tb//2 - sk.m(8)), title, font=f, fill=sk.pal["TITFG"])
    im.alpha_composite(body, (0, tb))
    return im


def main(a):
    if len(a) < 3:
        sys.exit(__doc__.strip())
    sk = Skin(a[1])
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
