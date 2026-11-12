"""What the bots learned on servers, into the next version (runtime/bot_knowledge.h).

Every server adds what its matches show (where players go, die and kill from,
per team, on 8 m cells) to <user>/bot_knowledge/<map>.txt. This merges those
files into data/bot_knowledge/, which ships with the game (copied next to the
exe), so every copy of the next version starts from what all of them learned.

  python tools/bot_knowledge.py merge [--decay F] [--clear] [--to DIR] <learned file or folder>...
      adds them to data/bot_knowledge/<map>.txt (made if missing). --decay F
      first scales what is shipped by F (0.5: the old matches count half), so
      recent play weighs more. --clear empties the merged learned files after
      (they are in the shipped data now; merging them again would count them twice;
      stop those servers first, or they save theirs again). --to DIR: into DIR instead.
  python tools/bot_knowledge.py info <file>...
      cells, totals per team, and the spots killed from most.

Only positions on the maps go in these files (no game data).
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHIPPED = os.path.join(ROOT, 'data', 'bot_knowledge')
CELL = 8.0
NAMES = ('visits US', 'visits Japan', 'deaths US', 'deaths Japan', 'kills US', 'kills Japan')


def read(path):
    cells, name = {}, None
    with open(path) as f:
        for line in f:
            p = line.split()
            if line.startswith('# bot knowledge') and len(p) >= 5:
                name = p[4]
            elif len(p) == 9 and p[0] == 'c':
                cells[(int(p[1]), int(p[2]))] = [float(v) for v in p[3:]]
    return name or os.path.splitext(os.path.basename(path))[0], cells


def write(path, name, cells):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', newline='\n') as f:
        f.write('# bot knowledge 1 %s\n' % name)
        for (ix, iz), v in sorted(cells.items()):
            if any(x >= 0.005 for x in v):
                f.write('c %d %d %s\n' % (ix, iz, ' '.join('%.2f' % x for x in v)))


def files(args):
    out = []
    for a in args:
        if not os.path.exists(a):
            print('(not there: %s)' % a)
        elif os.path.isdir(a):
            out += sorted(glob.glob(os.path.join(a, '*.txt')))
        else:
            out.append(a)
    return out


def merge(args):
    decay, clear, to = 1.0, False, SHIPPED
    rest = []
    i = 0
    while i < len(args):
        if args[i] == '--decay':
            decay = float(args[i + 1])
            i += 2
            continue
        if args[i] == '--to':
            to = args[i + 1]
            i += 2
            continue
        if args[i] == '--clear':
            clear = True
        else:
            rest.append(args[i])
        i += 1
    learned = files(rest)
    if not learned:
        sys.exit('nothing to merge')
    by_map = {}
    for path in learned:
        name, cells = read(path)
        by_map.setdefault(name, []).append((path, cells))
    for name, parts in sorted(by_map.items()):
        target = os.path.join(to, name + '.txt')
        cells = read(target)[1] if os.path.exists(target) else {}
        for v in cells.values():
            for k in range(6):
                v[k] *= decay
        added = 0
        for path, more in parts:
            for key, v in more.items():
                t = cells.setdefault(key, [0.0] * 6)
                for k in range(6):
                    t[k] += v[k]
                added += 1
        write(target, name, cells)
        print('%s: %d cells from %d file(s) -> %s (%d cells)' % (name, added, len(parts), target, len(cells)))
        if clear:
            for path, _ in parts:
                write(path, name, {})
                print('  cleared %s' % path)


def info(args):
    for path in files(args):
        name, cells = read(path)
        tot = [sum(v[k] for v in cells.values()) for k in range(6)]
        print('%s (%s): %d cells; %s' % (name, path, len(cells), ', '.join('%s %.0f' % (n, t) for n, t in zip(NAMES, tot))))
        for team, k in ((1, 4), (2, 5)):
            top = sorted(cells.items(), key=lambda kv: -kv[1][k])[:3]
            spots = ', '.join('%.0f,%.0f (%.0f)' % ((ix + 0.5) * CELL, (iz + 0.5) * CELL, v[k]) for (ix, iz), v in top if v[k] >= 0.5)
            if spots:
                print('  killed from most, team %d: %s' % (team, spots))


if __name__ == '__main__':
    if len(sys.argv) < 3 or sys.argv[1] not in ('merge', 'info'):
        sys.exit(__doc__)
    (merge if sys.argv[1] == 'merge' else info)(sys.argv[2:])
