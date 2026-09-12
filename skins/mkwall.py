#!/usr/bin/env python3
"""
mkwall.py - the APJ-OS desktop backgrounds, one per skin.

The same picture the skin-kit mockup drew behind the players: a diagonal
wash from colour A to colour C with two soft radial highlights of colour B
(top-left and bottom-right), exactly the CSS the mockup used:

    radial-gradient(120% 90% at 18% 6%,  B, transparent 62%),
    radial-gradient(90% 80%  at 88% 88%, B, transparent 58%),
    linear-gradient(160deg, A, C)

Output: wall/<FILE>.PNG at the screen size given (default 1920x1080), plus
the mean colour of each, which is what TeraDesk's matching Fluent preset
uses as its flat desktop colour when no wallpaper is set (btheme.c desk[]).

    python3 mkwall.py            # 1920x1080 into wall/
    python3 mkwall.py 1280 720   # another size
"""
import sys, os, json, glob
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

# A (wash start), B (highlight), C (wash end) - the mockup's --g-desk-a/b/c
WASH = {
    'FLTD': ('#0a1927', '#16324b', '#071019'),
    'FLTL': ('#b9cee6', '#e7eef7', '#9fb8d4'),
    'FUJI': ('#24140f', '#4a2118', '#160c09'),
    'GRPH': ('#15181b', '#2a3036', '#0e1113'),
}

def hx(s):
    return np.array([int(s[i:i + 2], 16) for i in (1, 3, 5)], dtype=np.float64)

def render(a, b, c, W, H):
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)
    ang = np.deg2rad(160.0)                     # CSS: 0deg = up, clockwise
    dx, dy = np.sin(ang), -np.cos(ang)
    L = abs(W * dx) + abs(H * dy)               # CSS gradient-line length
    t = ((xx - W / 2) * dx + (yy - H / 2) * dy) / L + 0.5
    t = np.clip(t, 0, 1)[..., None]
    A, B, C = hx(a), hx(b), hx(c)
    img = A * (1 - t) + C * t
    for px, py, rx, ry, stop in ((0.18 * W, 0.06 * H, 1.20 * W, 0.90 * H, 0.62),
                                 (0.88 * W, 0.88 * H, 0.90 * W, 0.80 * H, 0.58)):
        d = np.sqrt(((xx - px) / rx) ** 2 + ((yy - py) / ry) ** 2)
        al = np.clip(1 - d / stop, 0, 1)[..., None]
        img = img * (1 - al) + B * al
    return img

def main():
    W = int(sys.argv[1]) if len(sys.argv) > 1 else 1920
    H = int(sys.argv[2]) if len(sys.argv) > 2 else 1080
    out = os.path.join(HERE, 'wall')
    os.makedirs(out, exist_ok=True)
    for stem, (a, b, c) in WASH.items():
        img = render(a, b, c, W, H)
        Image.fromarray(np.clip(img + 0.5, 0, 255).astype(np.uint8)).save(
            os.path.join(out, stem + '.PNG'), optimize=True)
        m = img.reshape(-1, 3).mean(0)
        print('%s.PNG  %dx%d  mean #%02x%02x%02x' % (stem, W, H, *[int(v + 0.5) for v in m]))

if __name__ == '__main__':
    main()
