"""Reads the FbRB archives of Battlefield 1943 and Battlefield: Bad Company
(dist/xenon/**/*.fbrb; the same format in both): lists or extracts their files.
For the navigation tool: a level's collision (HavokPhysicsData) and terrain
(TerrainHeightfield) come out of these.

  python tools/fbrb.py list <archive.fbrb> [--types]        files (or counts per type)
  python tools/fbrb.py extract <archive.fbrb> <out_dir> [type ...]

Layout: "FbRB", a big-endian u32 (the compressed size of the first gzip
member), then gzip members. The first, the table of contents: u32 version (2),
u32 string table size, the strings (NUL-terminated: paths and type names),
u32 entry count, then 24-byte entries: name offset, flags (0x10000), data
offset, size, size again, type name offset (both offsets into the strings).
The other members, decompressed and joined, are the data the offsets point into.
"""
import os
import struct
import sys
import zlib


def gunzip_all(data):
    out, i = [], 0
    while i < len(data):
        dec = zlib.decompressobj(31)
        out.append(dec.decompress(data[i:]))
        if not dec.unused_data:
            break
        i = len(data) - len(dec.unused_data)
    return b''.join(out)


def read(path):
    raw = open(path, 'rb').read()
    if raw[:4] != b'FbRB':
        raise ValueError(f'{path}: not an FbRB archive')
    dec = zlib.decompressobj(31)
    toc = dec.decompress(raw[8:])
    rest = dec.unused_data
    version, slen = struct.unpack('>II', toc[:8])
    strings = toc[8:8 + slen]
    pos = 8 + slen
    count = struct.unpack('>I', toc[pos:pos + 4])[0]
    pos += 4

    def text(off):
        end = strings.find(b'\0', off)
        return strings[off:end].decode('latin1')

    entries = []
    for i in range(count):
        name, flags, offset, size, size2, typ = struct.unpack('>6I', toc[pos + 24 * i:pos + 24 * i + 24])
        entries.append({'name': text(name), 'type': text(typ), 'flags': flags, 'offset': offset, 'size': size})
    return version, entries, rest


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('list', 'extract'):
        sys.exit(__doc__)
    version, entries, rest = read(sys.argv[2])
    if sys.argv[1] == 'list':
        if '--types' in sys.argv:
            counts = {}
            for e in entries:
                c = counts.setdefault(e['type'], [0, 0])
                c[0] += 1
                c[1] += e['size']
            for t, (n, size) in sorted(counts.items(), key=lambda kv: -kv[1][1]):
                print(f'{n:6d} {size / 2**20:9.1f} MB  {t}')
        else:
            for e in entries:
                print(f"{e['offset']:10d} {e['size']:9d}  {e['type']:24s} {e['name']}")
        return
    out_dir = sys.argv[3]
    want = set(sys.argv[4:])
    data = gunzip_all(rest)
    n = 0
    for e in entries:
        if want and e['type'] not in want:
            continue
        path = os.path.join(out_dir, e['name'].replace('\\', '/'))
        os.makedirs(os.path.dirname(path) or out_dir, exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data[e['offset']:e['offset'] + e['size']])
        n += 1
    print(f'{n} files to {out_dir}')


if __name__ == '__main__':
    main()
