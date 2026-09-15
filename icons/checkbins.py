import struct, json, sys
from PIL import Image, ImageDraw, ImageFont
want = {e['hash']: e['name'] for e in json.load(open('/mnt/user-data/uploads/apj-os-tools/icons/out/icons.json'))}
def read(fn):
    d=open(fn,'rb').read()
    magic,ver,n,size,_=struct.unpack('>4sHHHH',d[:12])
    assert magic==b'APJI', magic
    out=[]
    for i in range(n):
        key,w,h,off=struct.unpack('>IHHI', d[12+i*12:24+i*12])
        im=Image.frombytes('RGBA',(w,h),d[off:off+w*h*4])
        out.append(('%08x'%key,w,h,im))
    return ver,size,out
def sheet(fn,out,bg,fg):
    ver,size,ic=read(fn)
    miss=[k for k,_,_,_ in ic if k not in want]
    print(fn,'v%d size%d'%(ver,size),len(ic),'entries; unknown hashes:',miss)
    try: F=ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",10)
    except: F=ImageFont.load_default()
    cols=8; cw,ch=120,90; rows=(len(ic)+cols-1)//cols
    im=Image.new("RGB",(cols*cw,rows*ch),bg); d=ImageDraw.Draw(im)
    for i,(k,w,h,g) in enumerate(ic):
        x=(i%cols)*cw; y=(i//cols)*ch
        im.paste(g,(x+(cw-w)//2,y+14),g)
        t=want.get(k,'?'+k)
        d.text((x+(cw-d.textlength(t,font=F))/2, y+14+h+8),t,font=F,fill=fg)
    im.save(out); print(' ->',out)
sheet('bins-dark/apjicons-48.bin','bins-check-dark.png','#ffffff','#333333')
sheet('bins-light/apjicons-48.bin','bins-check-light.png','#0f141b','#b9c4d0')
