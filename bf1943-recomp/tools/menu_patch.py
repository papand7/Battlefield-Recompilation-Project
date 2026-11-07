"""Main menu for the PC: leaves buttons out of the front end's main menu.

The menu is an EA APT movie (Menu/main_nav.res in menu/main/mainmenu-00.fbrb:
a BIGF archive of main_nav.apt, .const and .geo). Its ActionScript builds the
buttons as menuItems = [[flag, name, kind, clip], ...], one list per platform
branch. A left-out entry's instructions are replaced by ones that leave the
stack as it was (dup + pop), so no code moves, and the list's count is cut.

Writes <user>/override/dist/xenon/menu/main/mainmenu-00.fbrb (the payload
gzipped again as one member, as the game's: a raw one crashes it), which the
runtime opens instead of the game's file. Nothing of the game is shipped:
run it on your own copy.

  python tools/menu_patch.py --game original/v106 --user build/run/userdata
"""
import argparse
import gzip
import os
import struct
import zlib

# Buttons left out, by their action (the clip names, item00..., are screen
# slots that differ between the lists).
# Quick Match and Join Match with Friends stay: their labels read HOST GAME
# and JOIN GAME (runtime/pc_ui.cpp) and the runtime opens its own pages for
# them (xam.cpp, pc_settings.cpp), so the game's online sign-in never comes up.
REMOVE = {
    'cm': 'Create Private Match',
    'awards': 'Stats & Awards',
    'achieve': 'Achievements',
    'leaderboard': 'Leaderboards',
    'startCoralSea': 'Play Air Superiority',
}
ARCHIVE = 'dist/xenon/menu/main/mainmenu-00.fbrb'
MOVIE = 'Menu/main_nav.res'
# v106: the constant pool of the code that builds the lists, and that code.
POOL_OFFSET, POOL_COUNT = 190876, 152
CODE_START, CODE_END = 128336, 129400


def read_fbrb(data):
    n = struct.unpack('>I', data[4:8])[0]
    header = data[8:8 + n]
    rest = data[8 + n:]
    out = []
    while rest[:2] == b'\x1f\x8b':
        o = zlib.decompressobj(31)
        out.append(o.decompress(rest))
        rest = o.unused_data
    payload = b''.join(out) if out else rest
    h = gzip.decompress(header)
    _, sl = struct.unpack('>II', h[:8])
    st = h[8:8 + sl]
    pos = 8 + sl
    cnt = struct.unpack('>I', h[pos:pos + 4])[0]
    pos += 4
    entries = {}
    for _ in range(cnt):
        no, _, off, sz, _, ext = struct.unpack('>6I', h[pos:pos + 24])
        pos += 24
        entries[st[no:st.index(b'\0', no)].decode()] = (off, sz)
    return header, bytearray(payload), entries


def const_names(const):
    count, entries = struct.unpack('>II', const[24:32])
    names = {}
    for i in range(count):
        t, v = struct.unpack('>II', const[entries + 8 * i:entries + 8 * i + 8])
        if t == 1:
            names[i] = const[v:const.index(b'\0', v)].decode('latin1')
    return names


def ins_len(apt, o):
    op = apt[o]
    if op in (0xa2, 0xaf, 0xb5, 0xae, 0xa4, 0xa5, 0xa6, 0xa7, 0xb9):
        return 2
    if op in (0x96, 0x88):
        return ((o + 4) & ~3) + 8 - o
    if op in (0x99, 0x9d, 0xb8):
        return ((o + 4) & ~3) + 4 - o
    if op == 0xb6:
        return 3
    if op in (0xb4, 0xb7):
        return 5
    return 1


def filler(n):
    """n bytes that leave the stack as it was: dup + pop pairs, one push byte
    + pop for an odd count."""
    out = b''
    if n % 2:
        out += b'\xb5\x00\x17'
        n -= 3
    return out + b'\x4c\x17' * (n // 2)


def patch_apt(apt, names):
    pool = [struct.unpack('>I', apt[POOL_OFFSET + 4 * i:POOL_OFFSET + 4 * i + 4])[0]
            for i in range(POOL_COUNT)]
    local = lambda i: names.get(pool[i], '') if i < len(pool) else ''
    if local(6) != 'menuItems' or local(27) != 'item00':
        raise SystemExit('main_nav.apt is not the v106 one this patch knows')
    o, cur, start, lists = CODE_START, None, None, 0
    while o < CODE_END:
        op = apt[o]
        if op == 0xa2 and local(apt[o + 1]) == 'menuItems':
            cur, start = [], o + 2
        elif cur is not None and op == 0xb5 and apt[o + 2] == 0x42:
            if apt[o + 1] == 4:
                action, p = None, start
                while p < o:
                    if apt[p] in (0xa2, 0xaf) and local(apt[p + 1]) in REMOVE:
                        action = local(apt[p + 1])
                    p += ins_len(apt, p)
                cur.append((start, o + 3, action))
                start = o + 3
            else:
                removed = 0
                for s, e, action in cur:
                    if action in REMOVE:
                        apt[s:e] = filler(e - s)
                        removed += 1
                apt[o + 1] -= removed
                print('list of %d: %d left out' % (len(cur), removed))
                lists += 1
                cur = None
        o += ins_len(apt, o)
    if lists != 4:
        raise SystemExit('expected 4 menu lists, found %d' % lists)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', required=True, help='game folder (contains dist/xenon)')
    ap.add_argument('--user', required=True, help='the runtime\'s --user folder')
    args = ap.parse_args()
    data = open(os.path.join(args.game, ARCHIVE), 'rb').read()
    header, payload, entries = read_fbrb(data)
    off, size = entries[MOVIE]
    res = payload[off:off + size]
    # BIGF: count at 8, entries {offset, size, name} from 16 (big endian).
    count = struct.unpack('>I', res[8:12])[0]
    p, files = 16, {}
    for _ in range(count):
        fo, fs = struct.unpack('>II', res[p:p + 8])
        e = res.index(b'\0', p + 8)
        files[res[p + 8:e].decode()] = (fo, fs)
        p = e + 1
    ao, asz = files['main_nav.apt']
    co, csz = files['main_nav.const']
    apt = bytearray(res[ao:ao + asz])
    patch_apt(apt, const_names(bytes(res[co:co + csz])))
    payload[off + ao:off + ao + asz] = apt
    out = os.path.join(args.user, 'override', ARCHIVE)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'wb') as f:
        z = zlib.compressobj(6, zlib.DEFLATED, 31)
        f.write(data[:8] + header + z.compress(bytes(payload)) + z.flush())
    print('left out:', ', '.join(REMOVE.values()))
    print('wrote', out)


if __name__ == '__main__':
    main()
