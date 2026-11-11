"""Builds a BF1943 map's navigation mesh for the bots (runtime/bots/nav.cpp)
from the player's own game files: the level's placed collision (the
*Entity_HavokAsset resources of reduced-level-00.fbrb: static models, rigid
models, middle growth, forest) and its terrain (terrain-00.fbrb), with
tools/navgen (built from tools/navgen/CMakeLists.txt).

  python tools/make_navmesh.py <game dir> <level folder> <map name> <out.nav> [--work DIR] [--navgen EXE]
         [--live <collision.obj> --bounds x0 z0 x1 z1]

  e.g. python tools/make_navmesh.py original/v106 wake_island_s Wake_island_s build/game/nav/Wake_island_s.nav

The mesh covers the collision's extent and 150 m around it (beaches and the
sea floor near the shore), with the terrain in 2 m steps.

--live: the collision Bad Company's tool took from the running game
(BF_BOT_DUMP_COLLISION, runtime/bot_collision.cpp: every rigid body, carriers
and placed objects too) instead of the level files' groups, over the play area
it logs ("bot nav: play area x0 z0 x1 z1").
"""
import glob
import os
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)


def run(*args):
    subprocess.run([sys.executable, '-I', *args], check=True, stdout=subprocess.DEVNULL)


def obj_bounds(path):
    x0 = z0 = 1e9
    x1 = z1 = -1e9
    with open(path) as f:
        for line in f:
            if line.startswith('v '):
                _, x, _, z = line.split()
                x, z = float(x), float(z)
                x0, x1, z0, z1 = min(x0, x), max(x1, x), min(z0, z), max(z1, z)
    return x0, z0, x1, z1


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) < 4:
        sys.exit(__doc__)
    game, level, name, out = args[:4]
    opt = lambda k, d: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
    work = opt('--work', os.path.join(ROOT, 'build', 'navwork', level))
    navgen = opt('--navgen', os.path.join(ROOT, 'build', 'navgen', 'navgen.exe'))
    src = os.path.join(game, 'dist', 'xenon', 'levels', level)
    os.makedirs(work, exist_ok=True)
    run(os.path.join(TOOLS, 'fbrb.py'), 'extract', os.path.join(src, 'reduced-level-00.fbrb'), work, 'HavokPhysicsData')
    run(os.path.join(TOOLS, 'fbrb.py'), 'extract', os.path.join(src, 'terrain-00.fbrb'), work, 'TerrainHeightfield')
    objs = []
    live = opt('--live', None)
    if live:
        i = sys.argv.index('--bounds')
        x0, z0, x1, z1 = (float(v) for v in sys.argv[i + 1:i + 5])
        hf = glob.glob(os.path.join(work, 'Terrains', '*', '*.Heightfield-000.res'))[0]
        ground = os.path.join(work, 'ground.obj')
        run(os.path.join(TOOLS, 'terrain.py'), hf, ground, str(x0), str(z0), str(x1), str(z1), '--step', '2')
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        subprocess.run([navgen, out, ground, live, '--bounds', str(x0), str(z0), str(x1), str(z1)], check=True)
        print(f'{name}: {out} (live collision, {x0:.0f} {z0:.0f} .. {x1:.0f} {z1:.0f})')
        return
    for res in sorted(glob.glob(os.path.join(work, 'Levels', '*', '*Entity_HavokAsset.res'))):
        o = os.path.join(work, os.path.basename(res).replace('.res', '.obj'))
        run(os.path.join(TOOLS, 'havok.py'), res, '--obj', o)
        objs.append(o)
    if not objs:
        sys.exit('no level collision found in ' + src)
    b = [obj_bounds(o) for o in objs]
    x0, z0 = min(v[0] for v in b) - 150, min(v[1] for v in b) - 150
    x1, z1 = max(v[2] for v in b) + 150, max(v[3] for v in b) + 150
    hf = glob.glob(os.path.join(work, 'Terrains', '*', '*.Heightfield-000.res'))[0]
    ground = os.path.join(work, 'ground.obj')
    run(os.path.join(TOOLS, 'terrain.py'), hf, ground, str(x0), str(z0), str(x1), str(z1), '--step', '2')
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    subprocess.run([navgen, out, ground, *objs, '--bounds', str(x0), str(z0), str(x1), str(z1)], check=True)
    print(f'{name}: {out} ({x0:.0f} {z0:.0f} .. {x1:.0f} {z1:.0f}, {len(objs)} collision groups)')


if __name__ == '__main__':
    main()
