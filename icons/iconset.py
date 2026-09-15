# APJ-OS line icon set - 24-unit grid, round caps/joins, one colour.
# Matches the construction of the 12-icon proof sheet (12 Sep).

STROKE = 1.6

# shared sub-drawings
PAGE   = ['<path d="M6.5 3.5h8L18.5 7.5v13h-12z"/>', '<path d="M14 3.5v4.5h4.5"/>']
FOLDER = ['<path d="M3 18.5v-12h5.6l2 2H21v10z"/>', '<path d="M3 10.5h18"/>']

ICONS = {

# ---- drives & volumes -------------------------------------------------
"FLOPPY": ['<path d="M4.5 4.5h11.5L19.5 8v11.5h-15z"/>',
           '<path d="M8.5 4.5h7V9h-7z"/>',
           '<path d="M7 19.5v-6h10v6"/>'],

"HARDDISK": ['<rect x="3" y="6.5" width="18" height="11" rx="2"/>',
             '<path d="M3 12.5h18"/>',
             '<circle cx="17.5" cy="15" r="1.05"/>'],

"CDROM": ['<circle cx="12" cy="12" r="8.5"/>',
          '<circle cx="12" cy="12" r="2.6"/>',
          '<path d="M15.8 7.4a6.6 6.6 0 0 1 1.7 3.1"/>'],

"RAMDISK": ['<path d="M2.5 8h19v8.4h-19z"/>',
            '<path d="M8.5 16.4v2.2M15.5 16.4v2.2"/>',
            '<path d="M6 11.4h2.6M10.7 11.4h2.6M15.4 11.4h2.6"/>'],

"REMOVABLE": ['<rect x="8" y="7.5" width="8" height="13" rx="1.6"/>',
              '<path d="M10 7.5V4.6a2 2 0 0 1 4 0V7.5"/>',
              '<path d="M10.3 11.6h3.4M10.3 14.3h3.4"/>'],

"NETDRIVE": ['<rect x="3" y="4" width="18" height="6" rx="1.6"/>',
             '<rect x="3" y="14" width="18" height="6" rx="1.6"/>',
             '<path d="M6.4 7h2.4M6.4 17h2.4"/>',
             '<circle cx="17.4" cy="7" r="0.95"/>',
             '<circle cx="17.4" cy="17" r="0.95"/>'],

"EJECT": ['<path d="M12 4.5 4.5 13h15z"/>',
          '<path d="M4.5 17.5h15"/>'],

# ---- folders ----------------------------------------------------------
"FOLDER": FOLDER,

"FOLDER_OPEN": ['<path d="M3 18.5V6.5h5.3l2 2H19v2.6"/>',
                '<path d="M3 18.5 5.9 12.2h15.9L18.9 18.5z"/>'],

"FOLDER_FULL": FOLDER + ['<circle cx="8.5" cy="14.6" r="0.9"/>',
                         '<circle cx="12" cy="14.6" r="0.9"/>',
                         '<circle cx="15.5" cy="14.6" r="0.9"/>'],

"FOLDER_NEW": FOLDER + ['<path d="M12 12v5.2M9.4 14.6h5.2"/>'],

"DOCUMENTS": FOLDER + ['<path d="M8.5 13.6h7M8.5 16.3h4.5"/>'],

# ---- navigation -------------------------------------------------------
"PARENT": ['<path d="M3 19.5v-12h5.6l2 2H21v10z"/>',
           '<path d="M12 17.5v-6"/>',
           '<path d="M9 14.2l3-3 3 3"/>'],

"DOWN": ['<path d="M12 4v13.6"/>', '<path d="M6.6 12.2 12 17.6l5.4-5.4"/>'],

"ROOT": ['<path d="M12 20.5V8.4"/>', '<path d="M6.6 13.8 12 8.4l5.4 5.4"/>',
         '<path d="M6.2 4.5h11.6"/>'],

"SEARCH": ['<circle cx="10.5" cy="10.5" r="6.2"/>', '<path d="M15.1 15.1 20.4 20.4"/>'],

# ---- files ------------------------------------------------------------
"FILE": PAGE,

"TEXT_FILE": PAGE + ['<path d="M9 12h7M9 15h7M9 18h4.5"/>'],

"UNKNOWN": PAGE + ['<path d="M10.2 12.8a1.95 1.95 0 0 1 3.7.8c0 1.35-1.85 1.65-1.85 3"/>',
                   '<path d="M12 18.5v.01"/>'],

"ALERT_FILE": PAGE + ['<path d="M12.4 11.4v4.2"/>', '<path d="M12.4 18.3v.01"/>'],

"FONT_FILE": PAGE + ['<path d="M9.6 18.4 12.5 11l2.9 7.4"/>', '<path d="M10.6 15.8h3.8"/>'],

"SOURCE_FILE": PAGE + ['<path d="M11 12.9 8.8 15.2 11 17.5"/>',
                       '<path d="M14 12.9l2.2 2.3-2.2 2.3"/>'],

"SCRIPT": PAGE + ['<path d="M9.3 12.4 11.6 14.7 9.3 17"/>', '<path d="M13.1 17.4h3.2"/>'],

"BINARY": PAGE + ['<rect x="9" y="11.4" width="2.6" height="3.8" rx="1.3"/>',
                  '<path d="M14.9 15.2v-3.8h-1.2"/>',
                  '<path d="M9.2 18.6h6"/>'],

"ARCHIVE": ['<rect x="3" y="4.5" width="18" height="5" rx="1.4"/>',
            '<path d="M4.6 9.5v9a1.6 1.6 0 0 0 1.6 1.6h11.6a1.6 1.6 0 0 0 1.6-1.6v-9"/>',
            '<path d="M10.2 13.4h3.6"/>'],

"IMAGE_FILE": PAGE + ['<circle cx="9.9" cy="11.7" r="1.05"/>',
                      '<path d="M8.6 18.4 11.6 14.7l1.7 2 1.4-1.6 2.4 3.3z"/>'],

"AUDIO_FILE": PAGE + ['<path d="M10 17.5v-6l5-1.2v6"/>',
                      '<circle cx="8.6" cy="17.6" r="1.5"/>',
                      '<circle cx="13.6" cy="16.3" r="1.5"/>'],

"VIDEO_FILE": PAGE + ['<path d="M10 11.5l5 3-5 3z"/>'],

"PDF_FILE": PAGE + ['<path d="M10.3 18.8v-6.6h2.5a1.9 1.9 0 0 1 0 3.8h-2.5"/>'],

# ---- programs & system ------------------------------------------------
"APP": ['<rect x="6" y="6" width="12" height="12" rx="2"/>',
        '<path d="M9.5 3v3M14.5 3v3M9.5 18v3M14.5 18v3M3 9.5h3M3 14.5h3M18 9.5h3M18 14.5h3"/>'],

"SETTINGS": ['<circle cx="12" cy="12" r="5.6"/>',
             '<circle cx="12" cy="12" r="2.1"/>',
             '<path d="M12 3.2v3M12 17.8v3M20.8 12h-3M6.2 12h-3'
             'M18.2 5.8l-2.1 2.1M7.9 16.1l-2.1 2.1M18.2 18.2l-2.1-2.1M7.9 7.9 5.8 5.8"/>'],

"SYSTEM": ['<path d="M5 20.5V14.2M5 10.6V3.5M12 20.5v-7.2M12 9.7V3.5M19 20.5v-4.4M19 12.5V3.5"/>',
           '<path d="M2.6 14.2h4.8M9.6 9.7h4.8M16.6 12.5h4.8"/>'],

"TERMINAL": ['<rect x="2.5" y="4.5" width="19" height="15" rx="2"/>',
             '<path d="M6.6 9.4 9.7 12.5 6.6 15.6"/>', '<path d="M12.2 15.6h5.4"/>'],

"BROWSER": ['<circle cx="12" cy="12" r="8.5"/>', '<path d="M3.5 12h17"/>',
            '<path d="M12 3.5c2.45 2.65 2.45 14.35 0 17c-2.45-2.65-2.45-14.35 0-17"/>'],

"MEDIA": ['<circle cx="12" cy="12" r="8.5"/>', '<circle cx="12" cy="12" r="4.6"/>',
          '<circle cx="12" cy="12" r="1.15"/>'],

"MUSIC": ['<path d="M9 18V6.4l10-2V16"/>', '<circle cx="6.6" cy="18.1" r="2.4"/>',
          '<circle cx="16.6" cy="16.1" r="2.4"/>'],

"VIDEO_PLAYER": ['<rect x="2.5" y="4.6" width="19" height="12.4" rx="2"/>',
                 '<path d="M10 8.6 15.2 11.8 10 15z"/>', '<path d="M6.5 20.2h11"/>'],

"PHOTOS": ['<rect x="3" y="5" width="18" height="14" rx="2"/>',
           '<circle cx="8.4" cy="10" r="1.6"/>',
           '<path d="M3.4 17.6 9 12l3.4 3.6 2.6-2.4 5.6 5.2z"/>'],

"PLUGIN": ['<rect x="3.6" y="3.6" width="7.2" height="7.2" rx="1.5"/>',
           '<rect x="13.2" y="3.6" width="7.2" height="7.2" rx="1.5"/>',
           '<rect x="3.6" y="13.2" width="7.2" height="7.2" rx="1.5"/>',
           '<path d="M16.8 13.4v6.8M13.4 16.8h6.8"/>'],

"TRASH": ['<path d="M3.5 6.5h17"/>', '<path d="M9 6.5V4h6v2.5"/>',
          '<path d="M5.75 6.5 7 20h10l1.25-13.5"/>', '<path d="M10 10v6.5M14 10v6.5"/>'],

"PRINTER": ['<path d="M7 8V3.5h10V8"/>',
            '<path d="M4.5 8h15a1.5 1.5 0 0 1 1.5 1.5v6h-4"/>',
            '<path d="M7 15.5H3V9.5A1.5 1.5 0 0 1 4.5 8"/>',
            '<path d="M7 13.5h10v7H7z"/>'],

"MAIL": ['<rect x="2.5" y="5.5" width="19" height="13" rx="2"/>',
         '<path d="M3.2 7 12 13.4 20.8 7"/>'],

"HELP": ['<circle cx="12" cy="12" r="8.5"/>',
         '<path d="M9.6 9.9a2.55 2.55 0 0 1 4.9.9c0 1.75-2.45 2.15-2.45 3.9"/>',
         '<path d="M12 17.7v.01"/>'],

"INFO": ['<circle cx="12" cy="12" r="8.5"/>', '<path d="M12 11.2v6"/>',
         '<path d="M12 7.5v.01"/>'],

"ERROR": ['<circle cx="12" cy="12" r="8.5"/>', '<path d="M12 7.2v6.2"/>',
          '<path d="M12 16.9v.01"/>'],
}

ORDER = ["FLOPPY","HARDDISK","CDROM","RAMDISK","REMOVABLE","NETDRIVE","EJECT",
         "FOLDER","FOLDER_OPEN","FOLDER_FULL","FOLDER_NEW","DOCUMENTS",
         "PARENT","DOWN","ROOT","SEARCH",
         "FILE","TEXT_FILE","UNKNOWN","ALERT_FILE","FONT_FILE","SOURCE_FILE",
         "SCRIPT","BINARY","ARCHIVE","IMAGE_FILE","AUDIO_FILE","VIDEO_FILE","PDF_FILE",
         "APP","SETTINGS","SYSTEM","TERMINAL","BROWSER","MEDIA","MUSIC","VIDEO_PLAYER",
         "PHOTOS","PLUGIN","TRASH","PRINTER","MAIL","HELP","INFO","ERROR"]

def svg(name, colour="#000000", stroke=STROKE, size=None):
    body = "\n  ".join(ICONS[name])
    dim = '' if size is None else f' width="{size}" height="{size}"'
    return (f'<svg xmlns="http://www.w3.org/2000/svg"{dim} viewBox="0 0 24 24" fill="none" '
            f'stroke="{colour}" stroke-width="{stroke}" stroke-linecap="round" '
            f'stroke-linejoin="round">\n  {body}\n</svg>\n')

# ---- added for the cicons.rsc name set ---------------------------------
ICONS["HARDDISK2"] = ['<rect x="3" y="6.5" width="18" height="11" rx="2"/>',
                      '<path d="M3 12.5h18"/>',
                      '<circle cx="17.5" cy="15" r="1.05"/>',
                      '<path d="M6.2 9.5h6"/>']

ICONS["FOLDER_SYS"] = FOLDER + ['<circle cx="12" cy="14.9" r="1.9"/>',
                                '<path d="M12 11.5v1.5M12 16.8v1.5M15.4 14.9h-1.5M10.1 14.9H8.6"/>']

ICONS["FOLDER_STAR"] = FOLDER + ['<path d="M12 11.9l1.15 2.35 2.6.38-1.88 1.83.44 2.59L12 17.83'
                                 'l-2.31 1.22.44-2.59-1.88-1.83 2.6-.38z"/>']

ICONS["FOLDER_FONT"] = FOLDER + ['<path d="M9.6 18.4 12 12.3l2.4 6.1"/>', '<path d="M10.4 16.5h3.2"/>']

ICONS["GEM_WINDOW"] = ['<rect x="3" y="5" width="18" height="14" rx="2"/>',
                       '<path d="M3 9.3h18"/>',
                       '<circle cx="6.3" cy="7.15" r="0.85"/>',
                       '<circle cx="9.1" cy="7.15" r="0.85"/>']

ICONS["PAINT"] = ['<path d="M12 3.5a8.5 8.5 0 1 0 0 17c1.15 0 2-.85 2-1.9 0-.5-.2-.95-.55-1.3'
                  '-.3-.3-.5-.7-.5-1.15 0-1.05.85-1.9 1.95-1.9h2.2A4.4 4.4 0 0 0 21.5 9.9'
                  'C21.5 6.2 17.4 3.5 12 3.5z"/>',
                  '<circle cx="7.9" cy="9.4" r="1.1"/>',
                  '<circle cx="12.1" cy="7.3" r="1.1"/>',
                  '<circle cx="7.2" cy="13.9" r="1.1"/>']

ORDER += ["HARDDISK2","FOLDER_SYS","FOLDER_STAR","FOLDER_FONT","GEM_WINDOW","PAINT"]

# resource name -> icon in this set (cicons.rsc, 44 entries)
RSC_MAP = {
 "FLOPPY":"FLOPPY",          "HARD DISC":"HARDDISK",    "CD-ROM":"CDROM",
 "RAMDISK":"RAMDISK",        "HARD DISK 2":"HARDDISK2", "HARD DISK 3":"NETDRIVE",
 "REMOVABLE":"REMOVABLE",    "FOLDER":"FOLDER",         "GEMSYS":"FOLDER_SYS",
 "MAGIC FOLDER":"FOLDER_STAR","MINT FOLDER":"FOLDER_FULL","UP":"PARENT",
 "DOWNLOAD":"DOWN",          "UPLOAD":"ROOT",           "FONTS":"FOLDER_FONT",
 "FILE":"FILE",              "TEXT FILE":"TEXT_FILE",   "HELP FILE":"UNKNOWN",
 "FONT FILE":"FONT_FILE",    "PACK FILE":"ARCHIVE",     "IMAGE FILE":"IMAGE_FILE",
 "ACC":"PLUGIN",             "APP":"APP",               "GEM APP":"GEM_WINDOW",
 "TOS APP":"SCRIPT",         "PRX":"BINARY",            "TRASH":"TRASH",
 "PRINTER":"PRINTER",        "MINT":"TERMINAL",         "MAGIC":"SYSTEM",
 "WWW":"BROWSER",            "PAINT":"PAINT",           "SEARCH":"SEARCH",
 "VIEWER":"PHOTOS",          "MAIL":"MAIL",             "HELP":"HELP",
 "INFORMATION":"INFO",       "DISABLE":"ERROR",         "AUDIO FILE":"AUDIO_FILE",
 "VIDEO FILE":"VIDEO_FILE",  "MP3 PLAYER":"MUSIC",      "MP4 PLAYER":"VIDEO_PLAYER",
 "SOURCE FILE":"SOURCE_FILE","PDF FILE":"PDF_FILE",
}
