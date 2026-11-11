"""Finds one game's functions in the other (Bad Company's in BF1943 and the
other way round): both recompiled from builds of the same engine, a function
keeps its shape (the same operations in the same order) while its addresses,
offsets and constants change. Compares the recompiled C++ (generated/ppc) with
numbers, function names and labels blanked out.

  python tools/match_functions.py <from generated/ppc> <to generated/ppc> <sub_X>... [--top N]

For each function, the N most alike in the other game (similarity 0..1).
"""
import difflib
import glob
import os
import re
import sys
from collections import Counter

FUNC = re.compile(r'^(?:/\*strong\*/ )?PPC_FUNC(?:_IMPL)?\((?:__imp__)?(sub_[0-9A-F]{8})\)\s*\{')
NUM = re.compile(r'\b(?:0x[0-9A-Fa-f]+|\d+)\b')
NAME = re.compile(r'\bsub_[0-9A-F]{8}\b')
LABEL = re.compile(r'\bloc_[0-9A-F]{8}\b')


def norm(line):
    line = line.split('//')[0].strip()
    if not line:
        return None
    line = NAME.sub('F', line)
    line = LABEL.sub('L', line)
    return NUM.sub('N', line)


def load(folder):
    funcs = {}
    for path in glob.glob(os.path.join(folder, 'ppc_recomp.*.cpp')):
        name, body = None, None
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = FUNC.match(line)
                if m:
                    if name and body:
                        funcs[name] = body
                    name, body = m.group(1), []
                    continue
                if name is None:
                    continue
                if line.startswith('}'):
                    funcs[name] = body
                    name = None
                    continue
                n = norm(line)
                if n:
                    body.append(n)
        if name and body:
            funcs[name] = body
    return funcs


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) < 3:
        sys.exit(__doc__)
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 3
    src, dst = load(args[0]), load(args[1])
    print(f'{len(src)} functions in {args[0]}, {len(dst)} in {args[1]}')
    by_len = sorted((len(b), n) for n, b in dst.items())
    for want in args[2:]:
        body = src.get(want)
        if not body:
            print(f'{want}: not found')
            continue
        lo, hi = int(len(body) * 0.75), int(len(body) * 1.33) + 2
        bag = Counter(body)
        cands = []
        for ln, n in by_len:
            if ln < lo:
                continue
            if ln > hi:
                break
            other = dst[n]
            common = sum((bag & Counter(other)).values())
            cands.append((2 * common / (len(body) + len(other)), n))
        cands.sort(reverse=True)
        scored = []
        for _, n in cands[:40]:
            r = difflib.SequenceMatcher(None, body, dst[n], autojunk=False).ratio()
            scored.append((r, n))
        scored.sort(reverse=True)
        print(f'{want} ({len(body)} lines): ' + ', '.join(f'{n} {r:.2f} ({len(dst[n])})' for r, n in scored[:top]))


if __name__ == '__main__':
    main()
