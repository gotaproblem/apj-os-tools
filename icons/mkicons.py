#!/usr/bin/env python3
"""
APJ-OS icon pipeline.

  in:  cicons.rsc (TeraDesk colour icons), <name>.svg per icon (Papirus)
  out: cicons.rsc          - the same file with the NEW icons appended as
                             proper 16-colour CICONs (classic themes)
       apjicons-32.bin     - 32-bit RGBA replacements for EVERY icon, keyed by
       apjicons-48.bin       a hash of the icon's mono mask+data, for XaAES
       apjicons-64.bin       render_apj (Fluent). One file per pixel size.

Identity = FNV-1a over the mono mask then the mono data as stored in the
resource. TeraDesk copies icon blocks per desktop item but never alters
the mono part, so the hash survives; XaAES computes the same hash at draw
time (render_apj.c apj_icon_hash).

Usage: mkicons.py cicons.rsc svgdir outdir [size]   size 32 (default) keeps the original
       art and appends; 48 or 64 re-renders every icon from Papirus at that size
"""
import struct, sys, os, io, json
import cairosvg
from PIL import Image

# TeraDesk's icon names in cicons.rsc order, then the ones we add
NEW = ["AUDIO FILE", "VIDEO FILE", "MP3 PLAYER", "MP4 PLAYER", "SOURCE FILE", "PDF FILE"]   # names: 11 chars max

def fnv1a(b):
    h = 0x811C9DC5
    for c in b:
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h

def parse(d):
    h = list(struct.unpack('>18H', d[:36]))
    obj_off, trindex, nobs, rssize = h[1], h[9], h[10], h[17]
    objs = [list(struct.unpack('>3h3H1I4h', d[obj_off + i*24: obj_off + i*24 + 24])) for i in range(nobs)]
    ext = list(struct.unpack('>4I', d[rssize:rssize+16]))
    cicon_tab = ext[1]
    p = cicon_tab; counts = []
    while True:
        v, = struct.unpack('>i', d[p:p+4]); p += 4
        if v == -1: break
        counts.append(v)
    icons = []
    icon0_off = p
    for n in counts:
        start = p
        ib = struct.unpack('>3I11h', d[p:p+34]); p += 34
        ncic, = struct.unpack('>i', d[p:p+4]); p += 4
        w, hh = ib[8], ib[9]; msize = ((w+15)//16)*2*hh
        mono = d[p:p+msize]; mask = d[p+msize:p+2*msize]; p += 2*msize
        text = d[p:p+12].split(b'\0')[0].decode('latin-1'); p += 12
        for c in range(ncic):
            npl, = struct.unpack('>h', d[p:p+2]); p += 2
            cic = struct.unpack('>5i', d[p:p+20]); p += 20
            p += msize*npl + msize
            if cic[2]: p += msize*npl + msize
        icons.append(dict(name=text, w=w, h=hh, mono=mono, mask=mask, blob=d[start:p], ib=ib, start=start))
    data_end = p
    tail = d[data_end:]          # palette extension etc. - kept verbatim
    return dict(hdr=h, objs=objs, ext=ext, counts=counts, icons=icons, tail=tail, tail_off=data_end, icon0_off=icon0_off)

def palette(tail):
    pal = []
    for i in range(16):
        r, g, b, pen = struct.unpack('>4h', tail[i*8:i*8+8])
        pal.append((r*255//1000, g*255//1000, b*255//1000))
    return pal

def render(svg, size):
    png = cairosvg.svg2png(url=svg, output_width=size, output_height=size)
    return Image.open(io.BytesIO(png)).convert('RGBA')

def fit(im, w, h):
    """centre a rendered square icon in a w x h canvas"""
    if im.size == (w, h): return im
    c = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    c.paste(im, ((w - im.width)//2, (h - im.height)//2), im)
    return c

def to_planes(im, pal):
    """RGBA -> (mono data, mono mask, 4 colour planes, colour mask), standard format"""
    w, h = im.size; wd = (w+15)//16
    px = im.load()
    mono = bytearray(wd*2*h); mask = bytearray(wd*2*h)
    planes = [bytearray(wd*2*h) for _ in range(4)]
    def setbit(buf, x, y):
        i = (y*wd + x//16)*2; bit = 0x8000 >> (x % 16)
        v = (buf[i] << 8 | buf[i+1]) | bit; buf[i] = v >> 8; buf[i+1] = v & 0xFF
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 96: continue
            setbit(mask, x, y)
            if (r*299 + g*587 + b*114)//1000 < 140: setbit(mono, x, y)
            best = min(range(16), key=lambda i: (pal[i][0]-r)**2 + (pal[i][1]-g)**2 + (pal[i][2]-b)**2)
            for pl in range(4):
                if best >> pl & 1: setbit(planes[pl], x, y)
    return bytes(mono), bytes(mask), b''.join(planes), bytes(mask)

def make_icon_blob(name, im, pal, template_ib, cell=None):
    w, h = im.size
    mono, mask, col, cmask = to_planes(im, pal)
    ib = list(template_ib); ib[8] = w; ib[9] = h
    if cell:
        # cell = (object w, object h, label h): icon centred, label below
        ow, oh, lh = cell
        ib[6] = (ow - w) // 2; ib[7] = 0
        ib[10] = 0; ib[11] = h; ib[12] = ow; ib[13] = lh
        ib[4] = ib[4] * w // 32; ib[5] = ib[5] * h // 32      # drive letter position
    blob = struct.pack('>3I11h', *ib) + struct.pack('>i', 1) + mono + mask
    blob += name.encode('latin-1')[:11].ljust(12, b'\0')
    blob += struct.pack('>h', 4) + struct.pack('>5i', 0, 0, 0, 0, 0) + col + cmask
    return blob, mono, mask

def rebuild_rsc(d, P, new_blobs, all_blobs=None, objsize=None):
    """all_blobs: replace every existing icon's blob (a full re-render);
       objsize: (w, h) pixels for every icon object"""
    hdr = P['hdr'][:]; objs = [o[:] for o in P['objs']]
    n_old = len(objs); n_new = len(new_blobs)
    if all_blobs:
        P = dict(P); P['icons'] = [dict(ic, blob=b, start=0, replaced=True) for ic, b in zip(P['icons'], all_blobs)]
    # object table grows: new icon objects continue the picker grid
    last = objs[-1]; last[4] &= ~0x20            # clear OF_LASTOB on the old last
    last[0] = n_old                              # its next is the first new one
    tmpl = objs[1]
    xs = [o[7] for o in objs[1:]]; ys = [o[8] for o in objs[1:]]
    cols = sorted(set(xs)); rowstep = 3
    x_i = cols.index(objs[-1][7]); y = objs[-1][8]
    for i in range(n_new):
        x_i += 1
        if x_i >= len(cols): x_i = 0; y += rowstep
        o = tmpl[:]; o[0] = n_old + i + 1; o[6] = n_old - 1 + i; o[7] = cols[x_i]; o[8] = y
        objs.append(o)
    objs[-1][0] = 0; objs[-1][4] |= 0x20         # last -> parent, OF_LASTOB
    if objsize:
        for o in objs[1:]:
            o[9] = (objsize[0] << 8) & 0xFFFF; o[10] = (objsize[1] << 8) & 0xFFFF
            o[7] = o[7] * objsize[0] // 72; o[8] = o[8] * objsize[1] // 40   # picker grid
        objs[0][9] = objs[0][9] * objsize[0] // 72; objs[0][10] = objs[0][10] * objsize[1] // 40
    objs[0][2] = len(objs) - 1                   # root ob_tail
    if objs[0][10] < y + 4: objs[0][10] = y + 4  # root height in chars
    grow = n_new * 24
    hdr[9] += grow; hdr[10] = len(objs); hdr[17] += grow
    out = bytearray(struct.pack('>18H', *hdr))
    for o in objs: out += struct.pack('>3h3H1I4h', *o)
    out += struct.pack('>I', 36)                  # trindex table: tree 0 at 36
    ext_off = len(out); assert ext_off == hdr[17]
    counts = P['counts'] + [1]*n_new
    tab_off = ext_off + 16
    data_off = tab_off + (len(counts)+1)*4
    ext = P['ext'][:]; ext[1] = tab_off
    # ICONBLK.ib_ptext is an absolute file offset to the 12-byte name that
    # follows the mono mask inside each blob: every icon moves, so patch it
    body = b''
    for ic in P['icons']:
        blob = bytearray(ic['blob'])
        if ic.get('replaced'):
            new_blobs_all = True
            w, h = struct.unpack('>2h', blob[22:26]); msize = ((w+15)//16)*2*h
            struct.pack_into('>I', blob, 8, data_off + len(body) + 34 + 4 + 2*msize)
        else:
            old_ptext = struct.unpack('>I', blob[8:12])[0]
            if old_ptext:
                # name sits at the same place inside the blob; the blob itself moves
                struct.pack_into('>I', blob, 8, old_ptext - ic['start'] + data_off + len(body))
        body += bytes(blob)
    for blob in new_blobs:
        blob = bytearray(blob)
        w, h = struct.unpack('>2h', blob[22:26])
        msize = ((w+15)//16)*2*h
        struct.pack_into('>I', blob, 8, data_off + len(body) + 34 + 4 + 2*msize)
        body += bytes(blob)
    tail_off = data_off + len(body)
    if ext[2]: ext[2] = tail_off                  # palette extension follows the icons
    ext[0] = tail_off + len(P['tail'])
    out += struct.pack('>4I', *ext)
    for c in counts: out += struct.pack('>i', c)
    out += struct.pack('>i', -1)
    assert len(out) == data_off
    out += body + P['tail']
    return bytes(out)

def main(rsc, svgdir, outdir, size=32):
    size = int(size)
    d = open(rsc, 'rb').read()
    P = parse(d); pal = palette(P['tail'])
    os.makedirs(outdir, exist_ok=True)
    names = [ic['name'] for ic in P['icons']]
    print('existing icons:', len(names), 'target size', size)
    # icon cell for this size: label below, 16px label font from 48 up
    cell = (size * 2, size + (8 if size <= 32 else 16), 8 if size <= 32 else 16)
    tmpl = P['icons'][1]['ib']
    def make(nm, w, h):
        svg = os.path.join(svgdir, nm.replace(' ', '_') + '.svg')
        im = fit(render(svg, min(w, h)), w, h)
        return make_icon_blob(nm, im, pal, tmpl, cell if size != 32 else None)
    # 1. the resource: at 32 the originals stay and six are appended; at any
    #    other size every icon is re-rendered from the SVG at that size
    new_blobs = []; new_icons = []
    for nm in NEW:
        blob, mono, mask = make(nm, size, size)
        new_blobs.append(blob); new_icons.append(dict(name=nm, w=size, h=size, mono=mono, mask=mask))
    if size == 32:
        open(os.path.join(outdir, 'cicons.rsc'), 'wb').write(rebuild_rsc(d, P, new_blobs))
    else:
        all_blobs = []
        for ic in P['icons']:
            w = ic['w'] * size // 32 if ic['w'] != ic['h'] else size    # RAM disk stays wide
            blob, mono, mask = make(ic['name'], w, size)
            all_blobs.append(blob); ic.update(w=w, h=size, mono=mono, mask=mask)
        open(os.path.join(outdir, 'cicons.rsc'), 'wb').write(rebuild_rsc(d, P, new_blobs, all_blobs, (cell[0], cell[1])))
    # sanity: re-parse the output
    P2 = parse(open(os.path.join(outdir, 'cicons.rsc'), 'rb').read())
    print('rebuilt resource:', len(P2['icons']), 'icons,', P2['hdr'][10], 'objects')
    # 2. the ARGB sets, keyed by hash
    all_icons = P['icons'] + new_icons
    for bsize in ((32, 48, 64) if size == 32 else (size,)):
        entries = []; data = b''
        for ic in all_icons:
            svg = os.path.join(svgdir, ic['name'].replace(' ', '_') + '.svg')
            w = ic['w'] * bsize // size; h = ic['h'] * bsize // size
            im = fit(render(svg, min(w, h)), w, h)
            key = fnv1a(ic['mask'] + ic['mono'])
            entries.append((key, w, h, len(data))); data += im.tobytes()
        hdr = struct.pack('>4sHHHH', b'APJI', 1, len(entries), bsize, 0)
        tab_size = len(entries) * 12
        out = bytearray(hdr)
        base = len(hdr) + tab_size
        for key, w, h, off in entries: out += struct.pack('>IHHI', key, w, h, base + off)
        out += data
        fn = os.path.join(outdir, 'apjicons-%d.bin' % bsize)
        open(fn, 'wb').write(out); print(fn, len(out), 'bytes')
    json.dump([dict(name=ic['name'], hash='%08x' % fnv1a(ic['mask']+ic['mono'])) for ic in all_icons],
              open(os.path.join(outdir, 'icons.json'), 'w'), indent=1)

if __name__ == '__main__':
    main(*sys.argv[1:5])
