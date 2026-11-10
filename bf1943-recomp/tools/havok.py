"""Reads Havok 4.x binary packfiles (the HavokPhysicsData resources of
Battlefield 1943 and Bad Company levels: 32-bit, big-endian): its sections,
objects and pointers, for the navigation tool to take the collision geometry
out.

  python tools/havok.py <file.res>                sections and the objects' classes
  python tools/havok.py <file.res> --obj <out.obj>  the collision as triangles (map coordinates)

A resource has a game header first; the packfile starts at its magic
57E0E057 10C0C010. Header: magic (2 u32), user tag, file version, layout rules
(pointer size, little endian, reuse padding, empty base class), section count,
contents section index and offset, contents class name section index and
offset, the version string (16 bytes), flags, padding. Then a 48-byte header
per section: its tag (20 bytes), then (relative to the packfile) where its
data starts, and (relative to that) its local, global and virtual fixups,
exports, imports and end. Local fixups: (from, to) offsets in the section (a
pointer). Global fixups: (from, to section, to offset). Virtual fixups:
(object offset, class name section, class name offset): every object and its
class. Each table ends at the next one; 0xFFFFFFFF pads.
"""
import math
import struct
import sys

MAGIC = bytes.fromhex('57E0E05710C0C010')


class Packfile:
    def __init__(self, data):
        i = data.find(MAGIC)
        if i < 0:
            raise ValueError('no Havok packfile in it')
        self.d = data[i:]
        d = self.d
        (self.user_tag, self.version) = struct.unpack('>II', d[8:16])
        self.ptr_size, self.little, self.reuse_padding, self.empty_base = d[16:20]
        (self.num_sections, self.contents_section, self.contents_offset, self.classname_section,
         self.classname_offset) = struct.unpack('>5I', d[20:40])
        self.version_string = d[40:56].split(b'\0')[0].decode('latin1')
        self.sections = []
        for s in range(self.num_sections):
            h = d[64 + 48 * s:64 + 48 * (s + 1)]
            tag = h[:20].split(b'\0')[0].decode('latin1')
            start, local, glob, virt, exports, imports, end = struct.unpack('>7I', h[20:48])
            self.sections.append({'tag': tag, 'start': start, 'local': local, 'global': glob, 'virtual': virt,
                                  'exports': exports, 'imports': imports, 'end': end})
        # Pointers: (section, offset) -> (section, offset); objects: (section, offset) -> class name.
        self.pointers = {}
        self.objects = {}
        for si, s in enumerate(self.sections):
            base = s['start']
            # (tables end on padding that isn't a whole entry; the data can be
            # a few bytes shorter than the section says)
            for k in range(s['local'], s['global'] - 7, 8):
                if base + k + 8 > len(d):
                    break
                a, b = struct.unpack('>II', d[base + k:base + k + 8])
                if a != 0xFFFFFFFF:
                    self.pointers[(si, a)] = (si, b)
            for k in range(s['global'], s['virtual'] - 11, 12):
                if base + k + 12 > len(d):
                    break
                a, ts, b = struct.unpack('>III', d[base + k:base + k + 12])
                if a != 0xFFFFFFFF:
                    self.pointers[(si, a)] = (ts, b)
            for k in range(s['virtual'], s['exports'] - 11, 12):
                if base + k + 12 > len(d):
                    break
                a, cs, co = struct.unpack('>III', d[base + k:base + k + 12])
                if a != 0xFFFFFFFF:
                    self.objects[(si, a)] = self.cstr(cs, co)

    def section_data(self, si):
        s = self.sections[si]
        return self.d[s['start']:s['start'] + s['local']]

    def cstr(self, si, off):
        s = self.sections[si]
        a = s['start'] + off
        e = self.d.find(b'\0', a)
        return self.d[a:e].decode('latin1')

    def u32(self, si, off):
        return struct.unpack('>I', self.d[self.sections[si]['start'] + off:][:4])[0]

    def f32(self, si, off):
        return struct.unpack('>f', self.d[self.sections[si]['start'] + off:][:4])[0]

    def ptr(self, si, off):
        """Where the pointer at (si, off) points, or None."""
        return self.pointers.get((si, off))


# Havok 4.6.1 layouts used (32-bit, big-endian, empty base optimisation; an
# hkArray is pointer, size, capacity and flags):
#   hkConvexVerticesShape: +20 AABB half extents, +30 AABB centre, +40 hkArray
#     of FourVectors (48 bytes: x[4], y[4], z[4]) the vertices, +4C their
#     number, +50 hkArray of hkVector4 plane equations (normal, distance).
#   hkExtendedMeshShape: +58 hkArray of 64-byte shapes subparts: +10 hkArray of
#     child shape pointers (one), +20 translation, +30 rotation (quaternion
#     x, y, z, w).


def _vec(pf, off, n=3):
    return struct.unpack('>%df' % n, pf.d[pf.sections[1]['start'] + off:][:4 * n])


def convex_triangles(pf, off):
    """A convex vertices shape's faces as triangles (local space): the
    vertices on each plane, ordered around it, fanned."""
    vp = pf.ptr(1, off + 0x40)
    n = pf.u32(1, off + 0x4C)
    pp, np_ = pf.ptr(1, off + 0x50), pf.u32(1, off + 0x54)
    if not vp or not pp or n == 0 or n > 4096 or np_ > 4096:
        return []
    verts = []
    for i in range(n):
        block = vp[1] + 48 * (i // 4)
        j = i % 4
        x, y, z = _vec(pf, block + 4 * j, 1)[0], _vec(pf, block + 16 + 4 * j, 1)[0], _vec(pf, block + 32 + 4 * j, 1)[0]
        verts.append((x, y, z))
    tris = []
    size = max(max(abs(c) for v in verts for c in v), 0.01)
    for k in range(np_):
        nx, ny, nz, d = _vec(pf, pp[1] + 16 * k, 4)
        on = [v for v in verts if abs(nx * v[0] + ny * v[1] + nz * v[2] + d) < 0.01 * size + 1e-3]
        if len(on) < 3:
            continue
        cx = sum(v[0] for v in on) / len(on)
        cy = sum(v[1] for v in on) / len(on)
        cz = sum(v[2] for v in on) / len(on)
        # two axes in the plane, to order the face's vertices by angle
        ax = (1.0, 0.0, 0.0) if abs(nx) < 0.9 else (0.0, 1.0, 0.0)
        ux = (ny * ax[2] - nz * ax[1], nz * ax[0] - nx * ax[2], nx * ax[1] - ny * ax[0])
        ul = math.sqrt(sum(c * c for c in ux)) or 1.0
        ux = tuple(c / ul for c in ux)
        vx = (ny * ux[2] - nz * ux[1], nz * ux[0] - nx * ux[2], nx * ux[1] - ny * ux[0])
        on.sort(key=lambda v: math.atan2(sum((v[i] - (cx, cy, cz)[i]) * vx[i] for i in range(3)),
                                         sum((v[i] - (cx, cy, cz)[i]) * ux[i] for i in range(3))))
        for i in range(1, len(on) - 1):
            tris.append((on[0], on[i], on[i + 1]))
    return tris


def rotate(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    # v + 2w(q x v) + 2 q x (q x v)
    cx, cy, cz = y * vz - z * vy, z * vx - x * vz, x * vy - y * vx
    cx2, cy2, cz2 = y * cz - z * cy, z * cx - x * cz, x * cy - y * cx
    return (vx + 2 * (w * cx + cx2), vy + 2 * (w * cy + cy2), vz + 2 * (w * cz + cz2))


def collision_triangles(pf):
    """World-space collision triangles: the extended mesh shapes' placed
    convex pieces."""
    out = []
    for (si, off), cls in pf.objects.items():
        if cls != 'hkExtendedMeshShape':
            continue
        arr, count = pf.ptr(1, off + 0x58), pf.u32(1, off + 0x5C)
        if not arr:
            continue
        for e in range(count):
            sub = arr[1] + 64 * e
            child_arr = pf.ptr(1, sub + 0x10)
            child = pf.ptr(1, child_arr[1]) if child_arr else None
            if not child or pf.objects.get(child) != 'hkConvexVerticesShape':
                continue
            t = _vec(pf, sub + 0x20)
            q = _vec(pf, sub + 0x30, 4)
            for tri in convex_triangles(pf, child[1]):
                out.append(tuple(tuple(r + tt for r, tt in zip(rotate(q, v), t)) for v in tri))
    return out


def write_obj(tris, path):
    with open(path, 'w') as f:
        for tri in tris:
            for v in tri:
                f.write('v %.3f %.3f %.3f\n' % v)
        for i in range(len(tris)):
            f.write('f %d %d %d\n' % (3 * i + 1, 3 * i + 2, 3 * i + 3))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    pf = Packfile(open(sys.argv[1], 'rb').read())
    if '--obj' in sys.argv:
        out = sys.argv[sys.argv.index('--obj') + 1]
        tris = collision_triangles(pf)
        write_obj(tris, out)
        xs = [v[0] for t in tris for v in t]
        ys = [v[1] for t in tris for v in t]
        zs = [v[2] for t in tris for v in t]
        print(f'{len(tris)} triangles to {out}; bounds x {min(xs):.0f}..{max(xs):.0f} '
              f'y {min(ys):.0f}..{max(ys):.0f} z {min(zs):.0f}..{max(zs):.0f}')
        return
    print(f'{pf.version_string}, version {pf.version}, pointer {pf.ptr_size}, little endian {pf.little}, '
          f'empty base {pf.empty_base}; contents in section {pf.contents_section} at {pf.contents_offset:#x}')
    for si, s in enumerate(pf.sections):
        print(f"section {si} {s['tag']:16s} data {s['local']:8d} bytes")
    counts = {}
    for (si, off), cls in pf.objects.items():
        counts[cls] = counts.get(cls, 0) + 1
    print(f'{len(pf.objects)} objects, {len(pf.pointers)} pointers')
    for cls, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f'{n:6d}  {cls}')


if __name__ == '__main__':
    main()
