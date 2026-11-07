"""Finds relative-offset jump tables that XenonAnalyse misses and prints them as
XenonRecomp [[switch]] entries.

Pattern (MSVC Xbox 360), ending in `mtctr r12; bctr`:
    cmplwi crN, rI, MAX ; bgt crN, DEFAULT
    lis r12, T@h ; rlwinm r0, rI, k ; addi r12, r12, T@l
    l{b,h,w}zx r0, r12, r0              # load table entry
    lis r12, B@h ; addi r12, r12, B@l
    nop | rlwinm r0, r0, s              # entry scale
    add r12, r12, r0 ; mtctr r12 ; bctr

Usage: find_offset_switches.py <image.bin> <known_switch_tables.toml> > extra.toml
"""
import re
import struct
import sys

BASE = 0x82000000
data = open(sys.argv[1], "rb").read()
known_bases = [int(m, 16) for m in re.findall(r"base = (0x[0-9A-Fa-f]+)", open(sys.argv[2]).read())]


def word(addr):
    return struct.unpack_from(">I", data, addr - BASE)[0]


def next_bctr(addr):
    while word(addr) != 0x4E800420:
        addr += 4
    return addr


# XenonAnalyse's base is where the table is armed; the recompiler applies it to
# the next bctr, so compare on that address.
known = set(next_bctr(b) for b in known_bases)


def simm(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def lis_addi(lis, addi):
    # lis rD, hi ; addi rD, rD, lo
    if lis >> 26 != 15 or addi >> 26 != 14:
        return None
    return ((simm(lis) << 16) + simm(addi)) & 0xFFFFFFFF


found = 0
for off in range(0x160000, len(data) - 4, 4):
    addr = BASE + off
    if word(addr) != 0x4E800420 or word(addr - 4) != 0x7D8903A6 or word(addr - 8) != 0x7D8C0214:
        continue
    if addr in known:
        continue
    # Walk backwards from `add r12,r12,r0`, skipping nops.
    seq = []
    a = addr - 12
    while len(seq) < 12 and a > addr - 80:
        w = word(a)
        if w != 0x60000000:
            seq.append((a, w))
        a -= 4
    i = 0
    scale = 0
    if seq[i][1] >> 26 == 21 and (seq[i][1] >> 16) & 0x3FF == 0:  # rlwinm r0,r0,SH
        scale = (seq[i][1] >> 11) & 31
        i += 1
    # target base: addi r12 then lis r12 (either order relative to the scale op)
    if seq[i][1] >> 26 == 14 and seq[i + 1][1] >> 26 == 15:
        target_base = lis_addi(seq[i + 1][1], seq[i][1]); i += 2
    else:
        continue
    if seq[i][1] >> 26 == 21 and (seq[i][1] >> 16) & 0x3FF == 0 and scale == 0:
        scale = (seq[i][1] >> 11) & 31
        i += 1
    load = seq[i][1]; i += 1
    xo = (load >> 1) & 0x3FF
    size = {87: 1, 279: 2, 23: 4}.get(xo) if load >> 26 == 31 else None
    if target_base is None or size is None:
        continue
    load_rb = (load >> 11) & 31
    # table address: addi r12 / lis r12 (+ rlwinm r0,rI when indexing through r0)
    table_lo = table_hi = None
    index_reg = load_rb if load_rb != 0 else None
    while i < len(seq) and None in (table_lo, table_hi, index_reg):
        w = seq[i][1]; i += 1
        if w >> 26 == 14 and table_lo is None: table_lo = w
        elif w >> 26 == 15 and table_hi is None: table_hi = w
        elif w >> 26 == 21 and index_reg is None: index_reg = (w >> 21) & 31
        else: break
    if None in (table_lo, table_hi, index_reg):
        continue
    table = lis_addi(table_hi, table_lo)
    # bgt, then the cmplwi on the index register a few instructions earlier
    if i >= len(seq) or seq[i][1] >> 26 != 16:
        continue
    bgt_addr, bgt = seq[i]
    cmp = next((w for _, w in seq[i + 1:i + 5] if w >> 26 == 10 and (w >> 16) & 31 == index_reg), None)
    if cmp is None:  # compare done on a copy of the index register
        cmp = next((w for _, w in seq[i + 1:i + 5] if w >> 26 == 10), None)
    if cmp is None:
        continue
    count = (cmp & 0xFFFF) + 1
    default = (bgt_addr + simm(bgt & 0xFFFC)) & 0xFFFFFFFF
    labels = []
    for i in range(count):
        o = table - BASE + i * size
        entry = int.from_bytes(data[o:o + size], "big")
        labels.append((target_base + (entry << scale)) & 0xFFFFFFFF)
    print("[[switch]]")
    print(f"base = 0x{addr:08X}")
    print(f"r = {index_reg}")
    print(f"default = 0x{default:08X}")
    print("labels = [")
    for l in labels:
        print(f"    0x{l:08X},")
    print("]\n")
    found += 1

# Absolute tables with no bounds check next to them:
#   lis r12,T@h ; rlwinm r0,rI,2 ; addi r12,r12,T@l ; lwzx r0,r12,r0 ; mtctr r0 ; bctr
# The entry count comes from a nearby cmplwi on rI if there is one, otherwise
# from how many consecutive entries point into the code that follows the table.
absolute = 0
for off in range(0x160000, len(data) - 4, 4):
    addr = BASE + off
    if word(addr) != 0x4E800420 or word(addr - 4) != 0x7C0903A6 or word(addr - 8) != 0x7C0C002E:
        continue
    if addr in known:
        continue
    # lis r12 / addi r12 / rlwinm r0,rI,2 in any order before the lwzx
    lis = addi = index_insn = None
    for k in (12, 16, 20):
        w = word(addr - k)
        if w >> 16 == 0x3D80: lis = w            # lis r12
        elif w >> 16 == 0x398C: addi = w         # addi r12,r12
        elif w >> 26 == 21 and (w >> 16) & 31 == 0: index_insn = w  # rlwinm r0,rI
    if None in (lis, addi, index_insn):
        continue
    table = lis_addi(lis, addi)
    index_reg = (index_insn >> 21) & 31
    count = None
    default = None
    for back in range(6, 40):
        w = word(addr - 4 * back)
        if w >> 26 == 10 and (w >> 16) & 31 == index_reg:  # cmplwi crN, rI, MAX
            count = (w & 0xFFFF) + 1
            break
    entries = []
    o = table
    while len(entries) < (count or 512):
        e = word(o)
        if not (addr - 0x10000 < e < addr + 0x10000) or e & 3:
            break
        if o >= min([x for x in entries + [e] if x > table], default=0xFFFFFFFF):
            break  # the table cannot run into the code it jumps to
        entries.append(e)
        o += 4
    if count is None:
        count = len(entries)
    if len(entries) < count:  # the compare found belongs to something else
        count = len(entries)
    if count == 0:
        print(f"# skipped absolute table at {addr:08X}", file=sys.stderr)
        continue
    print("[[switch]]")
    print(f"base = 0x{addr:08X}")
    print(f"r = {index_reg}")
    print(f"default = 0x{entries[0]:08X}")
    print("labels = [")
    for l in entries[:count]:
        print(f"    0x{l:08X},")
    print("]")
    print()
    absolute += 1
print(f"# {found} offset jump tables, {absolute} unchecked absolute tables", file=sys.stderr)
