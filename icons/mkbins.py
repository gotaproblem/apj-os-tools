"""Rebuild apjicons-{32,48,64}.bin from the INSTALLED cicons.rsc using the
line-icon set. The resource is not modified, so every FNV-1a hash is
unchanged and XaAES finds the new artwork with no other change."""
import struct, os, io, sys, importlib.util, cairosvg
from PIL import Image
import iconset

spec = importlib.util.spec_from_file_location('mk','pipeline/mkicons.py')
mk = importlib.util.module_from_spec(spec); spec.loader.exec_module(mk)

def build(rsc, outdir, ink):
    P = mk.parse(open(rsc,'rb').read())
    os.makedirs(outdir, exist_ok=True)
    seen = set(); icons = []
    for ic in P['icons']:
        if ic['name'] in seen: continue          # the rsc repeats none, but be safe
        seen.add(ic['name']); icons.append(ic)
    for bsize in (32,48,64):
        entries=[]; data=b''
        for ic in icons:
            name = iconset.RSC_MAP[ic['name']]
            w = ic['w']*bsize//32; h = ic['h']*bsize//32
            png = cairosvg.svg2png(bytestring=iconset.svg(name, ink).encode(),
                                   output_width=min(w,h), output_height=min(w,h))
            im = mk.fit(Image.open(io.BytesIO(png)).convert('RGBA'), w, h)
            key = mk.fnv1a(ic['mask']+ic['mono'])
            entries.append((key,w,h,len(data))); data += im.tobytes()
        hdr = struct.pack('>4sHHHH', b'APJI', 1, len(entries), bsize, 0)
        out = bytearray(hdr); base = len(hdr) + len(entries)*12
        for key,w,h,off in entries: out += struct.pack('>IHHI', key, w, h, base+off)
        out += data
        fn = os.path.join(outdir,'apjicons-%d.bin'%bsize)
        open(fn,'wb').write(out)
        print(fn, len(out), 'bytes,', len(entries), 'entries')

if __name__ == '__main__':
    build('pipeline/cicons-in.rsc','bins-dark',  '#000000')   # dark ink: light themes
    build('pipeline/cicons-in.rsc','bins-light', '#ffffff')   # light ink: dark themes
