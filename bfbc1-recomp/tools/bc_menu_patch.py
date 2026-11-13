"""Main menu for the PC (Bad Company): a fifth row, PC SETTINGS.

The main menu (Menu/main_nav.res in menu/main/mainmenu-00.fbrb: a BIGF
archive of main_nav.apt, .const and .geo) has five rows, sp, mp, op, ext and
dev; retail drops dev from navArray and hides it unless extern.isLive and
extern.enableDevOpts. The navArray rebuild is jumped over and the hiding of
dev hides mp_dev (hidden there already) instead, so the row stays (dup + pop
fillers would not do: the stack is empty there). Its label, pushed as "DEV",
points instead at a debug trace string rewritten to PC SETTINGS. The runtime
(bfbc1_game.cpp) opens the PC settings page when the row is chosen.

Writes <user>/override/dist/xenon/menu/main/mainmenu-00.fbrb (the payload
gzipped again as one member), which the runtime opens instead of the game's
file. Nothing of the game is shipped: run it on your own copy.

  python tools/bc_menu_patch.py --game ../bfbc1/extracted --user userdata
"""
import argparse
import gzip
import os
import struct
import zlib

ARCHIVE = 'dist/xenon/menu/main/mainmenu-00.fbrb'
MOVIE = 'Menu/main_nav.res'
LABEL = b'PC SETTINGS'
# In main_nav.apt (the US disc's): the code that drops the dev row (navArray =
# [sp, mp, op, ext]; setVar), then hides mp_dev and dev (push variable, 0xae,
# with their pool indices 111 and 38); the DEV label's string pointer; the
# trace string that becomes the label.
CUT = (221692, 221706)
HIDE_DEV = 221712
LABEL_PTR, DEV_STRING, SPARE_STRING = 350724, 0x5bf44, 340436
SPARE_TEXT = b"APT messed up on us. Reached 'this page is removed'"
# Choosing a row: when signed in (extern.isPresenceConnected == true) and
# navPos != 1, getURL("FSCommand:presenceRequestOnline", "0") signs out. Never
# so offline; it becomes the PC SETTINGS row's: the sign-in test is made true
# (pop; push true; dup + pop) and navPos != 1 becomes navPos == "4" (its push
# of register 1 pushes "4", register 1, "navPos" from a list in the spare
# string's tail; getMember; equals; dup + pop; to number, where the first of
# the two nots was, which the sign-in test no longer jumps past), so it runs
# for navPos 4 only, not while navPos is unset as the menu comes up (the
# runtime opens the PC settings page on presenceRequestOnline("0")).
SIGNED_IN, PUSH, COMPARE = 221384, 221397, 221408
STRING_4, REGISTER_1, NAVPOS = 8338, 0x1c4f, 7040  # const entries


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


def patch_apt(apt):
    # The bytes expected: navArray rebuilt with 4 entries, dev hidden, the label.
    if (apt[CUT[1] - 4:CUT[1]] != b'\xb5\x04\x42\x1d' or apt[HIDE_DEV:HIDE_DEV + 2] != b'\xae\x26' or
            struct.unpack('>I', apt[LABEL_PTR:LABEL_PTR + 4])[0] != DEV_STRING or
            apt[DEV_STRING:DEV_STRING + 4] != b'DEV\0' or
            apt[SPARE_STRING:SPARE_STRING + len(SPARE_TEXT)] != SPARE_TEXT or
            apt[SIGNED_IN:SIGNED_IN + 4] != b'\xaf\x62\x73\x49' or
            apt[PUSH:PUSH + 11] != b'\x96\x00\x00\x00\x00\x00\x01\x00\x04\xe0\xe4' or
            apt[COMPARE:COMPARE + 6] != b'\xaf\x0f\x5a\x49\x12\x12'):
        raise SystemExit('main_nav.apt is not the one this patch knows (already patched?)')
    # A jump (0x99; its offset aligned, from the instruction's end) to CUT's end.
    end = ((CUT[0] + 4) & ~3) + 4
    apt[CUT[0]:end] = b'\x99' + bytes(end - CUT[0] - 5) + struct.pack('>i', CUT[1] - end)
    apt[HIDE_DEV + 1] = 111
    apt[SPARE_STRING:SPARE_STRING + len(SPARE_TEXT)] = LABEL.ljust(len(SPARE_TEXT), b'\0')
    apt[LABEL_PTR:LABEL_PTR + 4] = struct.pack('>I', SPARE_STRING)
    items = SPARE_STRING + 12  # after "PC SETTINGS\0", 4-aligned
    apt[items:items + 12] = struct.pack('>3I', STRING_4, REGISTER_1, NAVPOS)
    apt[SIGNED_IN:SIGNED_IN + 4] = b'\x17\x73\x4c\x17'
    apt[PUSH + 3:PUSH + 11] = struct.pack('>II', 3, items)
    apt[COMPARE:COMPARE + 5] = b'\x4e\x49\x4c\x17\x18'


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
    apt = bytearray(res[ao:ao + asz])
    patch_apt(apt)
    payload[off + ao:off + ao + asz] = apt
    out = os.path.join(args.user, 'override', ARCHIVE)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    # Written whole, then put in place: a partial file is never taken for done.
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        z = zlib.compressobj(6, zlib.DEFLATED, 31)
        f.write(data[:8] + header + z.compress(bytes(payload)) + z.flush())
    os.replace(tmp, out)
    print('wrote', out)


if __name__ == '__main__':
    main()
