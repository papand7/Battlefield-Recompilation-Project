"""Reads a level's terrain heightfield (TerrainHeightfield resources from
terrain-00.fbrb, extracted with tools/fbrb.py) and writes the ground as
triangles, for the navigation generator.

  python tools/terrain.py <Heightfield-000.res> <out.obj> [x0 z0 x1 z1] [--step N]
  python tools/terrain.py <Heightfield-000.res> --hf <out.hf>

--hf: the heights for the bots' line of sight and their ways out of water
(Bad Company, nav/<map>.hf): "BFHF", little-endian x0, y0, z0, x1, y1, z1
(floats), the samples per side (u32), then the samples as in the tile.

The tile: a bounding box (6 big-endian floats: min x, y, z, max x, y, z), five
big-endian u32 (the first its samples per side, the second its level), then
from 0x2E the samples, little-endian u16 rows along z, columns along x, one
per (box size / samples) metres; height = sample / 65535 * box height. The
level's -000 tile is the detailed one around the play area (Valley Run: 2048
samples over -1024..1024, 1 m; a soldier's standing height matches it); the
other tiles are coarser surroundings.
"""
import struct
import sys

DATA = 0x2E


class Heightfield:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.x0, self.y0, self.z0, self.x1, self.y1, self.z1 = struct.unpack('>6f', d[:24])
        self.n = struct.unpack('>I', d[24:28])[0]
        self.d = d
        self.cell = (self.x1 - self.x0) / self.n

    def height(self, x, z):
        col = min(max(int((x - self.x0) / self.cell), 0), self.n - 1)
        row = min(max(int((z - self.z0) / self.cell), 0), self.n - 1)
        i = DATA + 2 * (row * self.n + col)
        return self.y0 + struct.unpack('<H', self.d[i:i + 2])[0] / 65535 * (self.y1 - self.y0)


def write_obj(hf, path, x0, z0, x1, z1, step):
    xs = [x0 + step * i for i in range(int((x1 - x0) / step) + 1)]
    zs = [z0 + step * i for i in range(int((z1 - z0) / step) + 1)]
    with open(path, 'w') as f:
        for z in zs:
            for x in xs:
                f.write('v %.2f %.2f %.2f\n' % (x, hf.height(x, z), z))
        w = len(xs)
        for r in range(len(zs) - 1):
            for c in range(w - 1):
                a = r * w + c + 1
                f.write('f %d %d %d\nf %d %d %d\n' % (a, a + w, a + 1, a + 1, a + w, a + w + 1))
    return (len(xs) - 1) * (len(zs) - 1) * 2


def write_hf(hf, path):
    with open(path, 'wb') as f:
        f.write(b'BFHF' + struct.pack('<6fI', hf.x0, hf.y0, hf.z0, hf.x1, hf.y1, hf.z1, hf.n))
        f.write(hf.d[DATA:DATA + 2 * hf.n * hf.n])
    print(f'{hf.n} x {hf.n} heights to {path}')


def main():
    if '--hf' in sys.argv:
        write_hf(Heightfield(sys.argv[1]), sys.argv[sys.argv.index('--hf') + 1])
        return
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) < 2:
        sys.exit(__doc__)
    hf = Heightfield(args[0])
    box = [float(v) for v in args[2:6]] if len(args) >= 6 else [hf.x0, hf.z0, hf.x1 - hf.cell, hf.z1 - hf.cell]
    step = float(sys.argv[sys.argv.index('--step') + 1]) if '--step' in sys.argv else hf.cell
    n = write_obj(hf, args[1], *box, step)
    print(f'{n} ground triangles ({step:g} m) to {args[1]}')


if __name__ == '__main__':
    main()
