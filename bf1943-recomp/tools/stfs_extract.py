#!/usr/bin/env python3
"""Extract files from an Xbox 360 STFS package (CON/LIVE/PIRS).

Every data block is checked against its SHA-1 in the level-0 hash table, so a
wrong block mapping fails loudly instead of producing a corrupt file.

usage: stfs_extract.py <package> <out_dir> [--list]
"""
import hashlib
import os
import struct
import sys

BLOCK = 0x1000
HASHES_PER_TABLE = 0xAA


def u24le(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def u24be(b, o):
    return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]


class Stfs:
    def __init__(self, path):
        self.f = open(path, "rb")
        hdr = self.f.read(0xA000)
        self.magic = hdr[0:4]
        if self.magic not in (b"CON ", b"LIVE", b"PIRS"):
            raise ValueError("not an STFS package: %r" % self.magic)
        self.header_size = struct.unpack(">I", hdr[0x340:0x344])[0]
        self.content_type = struct.unpack(">I", hdr[0x344:0x348])[0]
        self.title_id = struct.unpack(">I", hdr[0x360:0x364])[0]
        self.version = struct.unpack(">I", hdr[0x358:0x35C])[0]
        vd = hdr[0x379:0x379 + 0x24]
        self.block_sep = vd[2]
        self.ft_blocks = struct.unpack("<H", vd[3:5])[0]
        self.ft_start = u24le(vd, 5)
        self.top_hash = vd[8:0x1C]
        self.total_blocks = struct.unpack(">I", vd[0x1C:0x20])[0]
        # "female" packages (bit 0 set) keep one copy of each hash table, "male" two.
        self.shift = 0 if (self.block_sep & 1) else 1
        self.base = (self.header_size + 0xFFF) & ~0xFFF
        self.step0 = 0xAB if self.shift == 0 else 0xAC
        self.step1 = 0x718F if self.shift == 0 else 0x723A

    # logical block -> physical block (skips interleaved hash tables)
    def backing(self, n):
        s = self.shift
        r = (((n + 0xAA) // 0xAA) << s) + n
        if n < 0xAA:
            return r
        if n < 0x70E4:
            return r + (((n + 0x70E4) // 0x70E4) << s)
        return (1 << s) + r + (((n + 0x70E4) // 0x70E4) << s)

    def level0_table(self, n):
        r = (n // 0xAA) * self.step0
        if n >= 0xAA:
            r += ((n // 0x70E4) + 1) << self.shift
            if n >= 0x70E4:
                r += 1 << self.shift
        return r

    def read_phys(self, phys):
        self.f.seek(self.base + phys * BLOCK)
        d = self.f.read(BLOCK)
        return d + b"\0" * (BLOCK - len(d))

    def hash_entry(self, n):
        t = self.level0_table(n)
        tab = self.read_phys(t)
        e = tab[(n % HASHES_PER_TABLE) * 0x18:][:0x18]
        if self.shift == 1:
            # male: pick whichever copy matches; try both, primary first
            alt = self.read_phys(t + 1)[(n % HASHES_PER_TABLE) * 0x18:][:0x18]
            return e, alt
        return e, None

    def read_block(self, n, verify=True):
        d = self.read_phys(self.backing(n))
        nxt = None
        if verify:
            e, alt = self.hash_entry(n)
            h = hashlib.sha1(d).digest()
            if h == e[:20]:
                nxt = u24be(e, 0x15)
            elif alt is not None and h == alt[:20]:
                nxt = u24be(alt, 0x15)
            else:
                raise IOError("SHA-1 mismatch at logical block %d" % n)
        return d, nxt

    def entries(self):
        out = []
        blk = self.ft_start
        for _ in range(self.ft_blocks):
            d, nxt = self.read_block(blk)
            for i in range(0, BLOCK, 0x40):
                e = d[i:i + 0x40]
                nl = e[0x28] & 0x3F
                if nl == 0:
                    continue
                out.append(dict(
                    name=e[:nl].decode("latin-1"),
                    contiguous=bool(e[0x28] & 0x40),
                    directory=bool(e[0x28] & 0x80),
                    blocks=u24le(e, 0x29),
                    start=u24le(e, 0x2F),
                    parent=struct.unpack(">h", e[0x32:0x34])[0],
                    size=struct.unpack(">I", e[0x34:0x38])[0],
                ))
            blk = nxt if nxt is not None else blk + 1
        return out

    def path_of(self, ents, idx):
        parts = []
        while idx != -1 and 0 <= idx < len(ents):
            parts.append(ents[idx]["name"])
            idx = ents[idx]["parent"]
        return "/".join(reversed(parts))

    def extract(self, ent, dst):
        left = ent["size"]
        blk = ent["start"]
        with open(dst, "wb") as o:
            for i in range(ent["blocks"]):
                d, nxt = self.read_block(blk)
                o.write(d[:min(left, BLOCK)])
                left -= BLOCK
                if left <= 0:
                    break
                blk = blk + 1 if ent["contiguous"] or nxt is None else nxt


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    p = Stfs(sys.argv[1])
    print("magic=%s title=%08X type=%08X version=%08X header=0x%X sep=%d ft=%d@%d blocks=%d"
          % (p.magic.decode(), p.title_id, p.content_type, p.version, p.header_size,
             p.block_sep, p.ft_blocks, p.ft_start, p.total_blocks))
    ents = p.entries()
    for i, e in enumerate(ents):
        rel = p.path_of(ents, i)
        print("%s %10d %s" % ("d" if e["directory"] else "f", e["size"], rel))
        if "--list" in sys.argv:
            continue
        dst = os.path.join(sys.argv[2], *rel.split("/"))
        if e["directory"]:
            os.makedirs(dst, exist_ok=True)
        else:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            p.extract(e, dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
