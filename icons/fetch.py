import urllib.request, os, sys
BASE="https://raw.githubusercontent.com/PapirusDevelopmentTeam/papirus-icon-theme/master/Papirus/"
MAP = {
 "FLOPPY":["devices/media-floppy"], "HARD DISC":["devices/drive-harddisk"], "CD-ROM":["devices/media-optical"],
 "RAMDISK":["devices/media-flash","devices/media-memory"], "HARD DISK 2":["devices/drive-harddisk-system"],
 "HARD DISK 3":["devices/drive-harddisk-usb","devices/drive-harddisk-ieee1394","devices/drive-multidisk","devices/drive-harddisk"], "REMOVABLE":["devices/drive-removable-media"],
 "FOLDER":["places/folder"], "GEMSYS":["places/folder-blue-activities","places/folder-blue-development","places/folder-blue-script","places/folder-grey"],
 "MAGIC FOLDER":["places/folder-violet"], "MINT FOLDER":["places/folder-green"],
 "UP":["actions/go-up"], "DOWNLOAD":["actions/download","actions/document-save"], "UPLOAD":["actions/upload","actions/document-send"],
 "FONTS":["places/folder-blue-text","places/folder-blue-documents","places/folder-orange"], "FILE":["mimetypes/unknown","mimetypes/application-x-zerosize"],
 "TEXT FILE":["mimetypes/text-x-generic"], "HELP FILE":["mimetypes/text-x-readme","mimetypes/text-x-generic-template"],
 "FONT FILE":["mimetypes/application-x-font-ttf"], "PACK FILE":["mimetypes/application-x-archive","mimetypes/package-x-generic"],
 "IMAGE FILE":["mimetypes/image-x-generic"], "ACC":["mimetypes/application-x-addon"], "APP":["mimetypes/application-x-executable"],
 "GEM APP":["mimetypes/application-x-desktop"], "TOS APP":["mimetypes/text-x-script"], "PRX":["mimetypes/application-x-object"],
 "TRASH":["places/user-trash"], "PRINTER":["devices/printer"], "MINT":["apps/utilities-terminal"], "MAGIC":["apps/preferences-system"],
 "WWW":["apps/web-browser"], "PAINT":["apps/kolourpaint","apps/gimp"], "SEARCH":["apps/system-search","actions/system-search"],
 "VIEWER":["apps/multimedia-photo-viewer","apps/image-viewer","apps/gwenview"], "MAIL":["apps/internet-mail"],
 "HELP":["apps/system-help","apps/help-browser"], "INFORMATION":["status/dialog-information"], "DISABLE":["status/dialog-error","actions/dialog-cancel"],
 "AUDIO FILE":["mimetypes/audio-x-generic","mimetypes/audio-mpeg"], "VIDEO FILE":["mimetypes/video-x-generic","mimetypes/video-mp4"],
 "MP3 PLAYER":["apps/multimedia-audio-player","apps/audio-player"], "VIDEO PLAYER":["apps/multimedia-video-player","apps/video-player"],
 "SOURCE FILE":["mimetypes/text-x-csrc","mimetypes/text-x-c"], "PDF FILE":["mimetypes/application-pdf"],
}
def get(url):
    try:
        with urllib.request.urlopen(url, timeout=20) as r: return r.read()
    except Exception as e: return None
def fetch(path, size):
    # follow papirus symlinks (raw returns the target file name)
    name=path; d=os.path.dirname(path)
    for _ in range(4):
        data=get(BASE+f"{size}x{size}/{name}.svg")
        if data is None: return None
        if len(data)<128 and b'<' not in data:
            tgt=data.decode().strip()
            if tgt.endswith('.svg'): tgt=tgt[:-4]
            if tgt.startswith('../'): tgt=tgt[3:]        # ../apps/x -> apps/x
            name = tgt if '/' in tgt else d+'/'+tgt
            continue
        return data
    return None
res={}
for key,cands in MAP.items():
    got=None
    for c in cands:
        for size in (48,32,24):
            data=fetch(c,size)
            if data: got=(c,size,data); break
        if got: break
    if got:
        fn=key.replace(' ','_')+'.svg'; open(fn,'wb').write(got[2]); res[key]=(got[0],got[1])
        print(f"{key:13s} <- {got[0]} @{got[1]}")
    else:
        print(f"{key:13s} !! none of {cands}")
import json; json.dump(res, open('map.json','w'), indent=1)
