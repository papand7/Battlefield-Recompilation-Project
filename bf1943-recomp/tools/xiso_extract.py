"""Extracts an Xbox 360 disc image (XDVDFS / XGD2-3) to a folder, skipping files
already there at the right size.
Usage: xiso_extract.py <image.iso> <out dir>"""
import os
import struct
import sys

iso_path, out = sys.argv[1:3]
f = open(iso_path, "rb")
SECTOR = 2048
base = None
for off in (0, 0xFD90000, 0x18300000, 0x2080000):  # plain, XGD2, XGD1, XGD3
    f.seek(off + 32 * SECTOR)
    if f.read(20) == b"MICROSOFT*XBOX*MEDIA":
        base = off
        break
if base is None:
    sys.exit("no XDVDFS volume found")
root_sector, root_size = struct.unpack("<II", f.read(8))
count = 0


def walk(sector, size, rel):
    global count
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    stack, seen = [0], set()
    while stack:
        o = stack.pop() * 4
        if o in seen or o + 14 > len(table) or table[o:o + 2] == b"\xff\xff":
            continue
        seen.add(o)
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, o)
        name = table[o + 14:o + 14 + nlen].decode("latin-1")
        if left: stack.append(left)
        if right: stack.append(right)
        path = os.path.join(rel, name)
        if attr & 0x10:
            os.makedirs(os.path.join(out, path), exist_ok=True)
            if length: walk(start, length, path)
            continue
        dst = os.path.join(out, path)
        if os.path.exists(dst) and os.path.getsize(dst) == length:
            continue
        f.seek(base + start * SECTOR)
        with open(dst, "wb") as w:
            left_b = length
            while left_b:
                chunk = f.read(min(left_b, 1 << 24))
                w.write(chunk)
                left_b -= len(chunk)
        count += 1
        print(f"{length:12d} {path}", flush=True)


os.makedirs(out, exist_ok=True)
walk(root_sector, root_size, "")
print(f"# {count} files written")
