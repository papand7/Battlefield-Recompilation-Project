"""Finds a function in the other game by what it calls, when its own shape
doesn't match well enough (tools/match_functions.py): its callees are matched
one by one, then the other game's functions are ranked by how many of those
matches they call.

  python tools/match_by_callees.py <from generated/ppc> <to generated/ppc> <sub_X> [--top N]
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import match_functions as mf  # noqa: E402

CALL = re.compile(r'\b(sub_[0-9A-F]{8})\(ctx, base\)')


def calls(folder):
    out = {}
    import glob
    for path in glob.glob(os.path.join(folder, 'ppc_recomp.*.cpp')):
        name = None
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = mf.FUNC.match(line)
                if m:
                    name = m.group(1)
                    out.setdefault(name, set())
                    continue
                if name:
                    for c in CALL.findall(line):
                        out[name].add(c)
    return out


def best(body, dst, by_len):
    lo, hi = int(len(body) * 0.75), int(len(body) * 1.33) + 2
    from collections import Counter
    import difflib
    bag = Counter(body)
    cands = []
    for ln, n in by_len:
        if ln < lo:
            continue
        if ln > hi:
            break
        cands.append((2 * sum((bag & Counter(dst[n])).values()) / (len(body) + len(dst[n])), n))
    cands.sort(reverse=True)
    scored = sorted(((difflib.SequenceMatcher(None, body, dst[n], autojunk=False).ratio(), n) for _, n in cands[:25]),
                    reverse=True)
    return scored[0] if scored else (0, None)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) < 3:
        sys.exit(__doc__)
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 5
    src, dst = mf.load(args[0]), mf.load(args[1])
    src_calls, dst_calls = calls(args[0]), calls(args[1])
    by_len = sorted((len(b), n) for n, b in dst.items())
    want = args[2]
    matched = {}
    for c in sorted(src_calls.get(want, ())):
        if c in src and len(src[c]) >= 12:  # tiny ones match anything
            r, n = best(src[c], dst, by_len)
            if n and r >= 0.6:
                matched[c] = (n, r)
                print(f'  callee {c} -> {n} {r:.2f}')
    targets = {n for n, _ in matched.values()}
    rank = sorted(((len(cs & targets), f) for f, cs in dst_calls.items() if cs & targets), reverse=True)
    print(f'{want}: {len(matched)} callees matched; callers of the most of them:')
    for k, f in rank[:top]:
        print(f'  {f}: calls {k} ({len(dst.get(f, []))} lines)')


if __name__ == '__main__':
    main()
