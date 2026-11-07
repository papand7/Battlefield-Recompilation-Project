"""Textures the game used that the pack has no replacement for.

bf1943 --log-textures saves each miss's tiled base level as
textures_missing/<hash>_<w>x<h>_<xenos format>.bin (runtime/xenia_shims/
texture_replacement.cc). This decodes them:

  python missing.py <textures_missing dir> <out dir>
      writes <out>/<hash>_<w>x<h>.png and prints which source texture each one
      matches (same size, nearly the same pixels), if any.
"""
import os
import sys

import numpy as np
from PIL import Image

import xtexture as xt

# Xenos texture format -> xtexture format id
XENOS = {18: 0, 19: 1, 20: 2, 59: 3, 49: 4}


class Dump:
    def __init__(self, path):
        stem = os.path.basename(path)[:-4]
        self.hash, size, fmt = stem.split('_')
        self.width, self.height = map(int, size.split('x'))
        self.format = XENOS[int(fmt)]
        self.mips = 1
        self.cube = False
        self.data = open(path, 'rb').read()


def main():
    src_dir, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for fn in sorted(os.listdir(src_dir)):
        if not fn.endswith('.bin'):
            continue
        d = Dump(os.path.join(src_dir, fn))
        img = xt.decode(d)
        name = '%s_%dx%d.png' % (d.hash, d.width, d.height)
        Image.fromarray(img, 'RGBA').save(os.path.join(out, name))
        a = img[..., :3].astype(np.float32)
        print(name, xt.FORMATS[d.format][0], 'mean', a.mean((0, 1)).round().tolist())


if __name__ == '__main__':
    main()
