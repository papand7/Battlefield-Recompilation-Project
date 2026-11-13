"""Makes the bots' navigation meshes on this PC from the player's own game, for
Battlefield 1943 and Battlefield: Bad Company (Windows and Linux; Python's
standard library only). The meshes come from the game, so they never ship:
each PC makes its own, once per map.

  python tools/make_navs.py --title bf1943|bfbc1 --game <game files (default.xex)>
         --exe <the game exe or the Linux server> --nav-dir <the folder the game reads nav files from>
         [--maps m1,m2,...] [--work <scratch folder>] [--force] [--timeout <seconds per map>]

Per map, only the files missing in --nav-dir are made (--force: all of them):
  bf1943  <map>.nav    the soldiers' mesh (runtime/bots/nav.cpp)
  bfbc1   <map>.nav    the soldiers' mesh (cells 0.2 m, radius 0.4 m)
          <map>.vnav   the vehicles' mesh (cells 0.4 m, radius 2 m, climb 0.6 m, slopes 35 degrees)
          <map>.hf     the terrain's heights (the bots' line of sight)
The game runs briefly as a dedicated server on the map (a new temporary --user
folder, no bots, BF_BOT_DUMP_COLLISION) until it has written the level's live
collision and logged "bot nav: play area x0 z0 x1 z1"; then that process (only
it, by its PID) is stopped. tools/fbrb.py and tools/terrain.py give the ground
from the level's terrain, tools/navgen makes the mesh from both, and the file
is moved into --nav-dir when complete (the game never reads half a file).

The game run has BF_MAKE_NAVS_CHILD=1 set (a server without a mesh starts this
tool by itself, runtime/nav_autogen.cpp; the run made here must not). A map
being made by another run of this tool (<nav-dir>/<map>.making, kept fresh
while it works) is waited for.

Output (for the launcher): "progress nav <done> <total>" (maps), "map <name> ok"
or "map <name> fail <why>" per map, other lines are information; the last line
is "ok" or "error: <why>". Exit code 0 only if every map is ok. Scratch files go
in --work (default: the temp folder) and are deleted after.
"""
import glob
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(TOOLS)  # bf1943-recomp (or a release's src)
WINDOWS = os.name == 'nt'
EXE_SUFFIX = '.exe' if WINDOWS else ''

TITLES = {
    'bf1943': {
        'maps': ['Wake_island_s', 'Iwo_Jima_s', 'Guadal_Canal'],
        'files': ['.nav'],
        # Coral Sea: air only (no flags, nothing to walk on): no mesh to make.
        'no_mesh': {'coral_sea'},
    },
    'bfbc1': {
        'maps': ['mp_harbour_day1', 'mp_harbour_day2', 'mp_ignition_day1', 'mp_ignition_day2',
                 'mp_monastery_day1', 'mp_monastery_day2', 'mp_beachhead_day1', 'mp_beachhead_day2'],
        'files': ['.nav', '.vnav', '.hf'],
        'no_mesh': set(),
    },
}
LOCK_STALE = 90  # seconds: a .making file not touched for this long is left over from a run that died
PLAY_AREA_AFTER_DUMP = 90  # seconds to wait for the play area once the collision is written


def say(line):
    print(line, flush=True)


class Fail(Exception):
    pass


# ---- finding things ----------------------------------------------------------------
def find_navgen(exe):
    """navgen: BF_NAVGEN, next to the game, this tree's build/navgen, a release's src/build/navgen, PATH."""
    name = 'navgen' + EXE_SUFFIX
    exe_dir = os.path.dirname(os.path.abspath(exe))
    cands = [os.environ.get('BF_NAVGEN', ''), os.path.join(exe_dir, name),
             os.path.join(SRC, 'build', 'navgen', name), os.path.join(SRC, 'build', 'navgen-linux', name)]
    d = exe_dir
    for _ in range(4):  # (<root>/game -> <root>/src/build/navgen; build_linux.sh's <out>/game -> <out>/navgen)
        cands += [os.path.join(d, 'navgen', name), os.path.join(d, 'src', 'build', 'navgen', name),
                  os.path.join(d, 'bf1943-recomp', 'build', 'navgen', name)]
        d = os.path.dirname(d)
    for c in cands:
        if c and os.path.isfile(c) and (WINDOWS or os.access(c, os.X_OK)):
            return c
    return shutil.which('navgen')


def level_dir(game, title, m):
    """The level's folder in the game files (case as on disk: Linux file names are case-sensitive)."""
    levels = os.path.join(game, 'dist', 'xenon', 'levels')
    try:
        for n in os.listdir(levels):
            if n.lower() == m.lower() and os.path.isdir(os.path.join(levels, n)):
                return os.path.join(levels, n)
    except OSError:
        pass
    return None


def run_py(script, *args):
    r = subprocess.run([sys.executable, '-I', os.path.join(TOOLS, script), *args], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, errors='replace')
    if r.returncode != 0:
        tail = (r.stdout or '').strip().splitlines()[-1:] or ['exit %d' % r.returncode]
        raise Fail('%s failed: %s' % (script, tail[0][:200]))
    return r.stdout


def run_navgen(navgen, out, inputs, bounds, extra):
    args = [navgen, out, *inputs, '--bounds', *('%g' % v for v in bounds), *extra]
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace',
                       creationflags=subprocess.CREATE_NO_WINDOW if WINDOWS else 0)
    lines = [ln for ln in (r.stdout or '').splitlines() if 'too many' not in ln]
    if r.returncode != 0 or not os.path.isfile(out):
        raise Fail('navgen failed (exit %d): %s' % (r.returncode, (lines[-1:] or [''])[0][:200]))
    with open(out, 'rb') as f:
        if f.read(4) != b'BFNV' or os.path.getsize(out) < 64:
            raise Fail('navgen made no mesh (%s)' % (lines[-1:] or [''])[0][:200])
    for ln in lines[-2:]:
        say('  ' + ln)


def heightfield(game, title, m, work):
    """The level's detailed terrain tile (TerrainHeightfield *-000.res), extracted into work."""
    lvl = level_dir(game, title, m)
    if not lvl:
        raise Fail('no level folder for %s in %s' % (m, os.path.join(game, 'dist', 'xenon', 'levels')))
    arc = os.path.join(lvl, 'terrain-00.fbrb')
    if not os.path.isfile(arc):
        raise Fail('no terrain-00.fbrb in ' + lvl)
    out = os.path.join(work, 'terrain')
    run_py('fbrb.py', 'extract', arc, out, 'TerrainHeightfield')
    found = sorted(glob.glob(os.path.join(out, '**', '*Heightfield-000.res'), recursive=True))
    if not found:
        raise Fail('no terrain heightfield in ' + arc)
    return found[0]


def hf_box(path):
    """The heightfield's box (x0, z0, x1, z1) and cell size (as tools/terrain.py)."""
    with open(path, 'rb') as f:
        d = f.read(28)
    x0, _, z0, x1, _, z1 = struct.unpack('>6f', d[:24])
    n = struct.unpack('>I', d[24:28])[0]
    return (x0, z0, x1, z1), (x1 - x0) / n


def ground(hf, bounds, step, out):
    """The terrain over bounds as triangles (tools/terrain.py), on the heightfield's own grid."""
    (hx0, hz0, hx1, hz1), cell = hf_box(hf)
    if step is None:
        step = cell
    # Snapped outwards onto the grid and kept on the tile.
    def snap_lo(v, o):
        return o + max(0, int((v - o) / cell) - 1) * cell
    def snap_hi(v, o, e):
        return min(e - cell, o + (int((v - o) / cell) + 2) * cell)
    x0, z0 = snap_lo(bounds[0], hx0), snap_lo(bounds[1], hz0)
    x1, z1 = snap_hi(bounds[2], hx0, hx1), snap_hi(bounds[3], hz0, hz1)
    if x1 <= x0 or z1 <= z0:
        raise Fail('the play area is off the terrain')
    run_py('terrain.py', hf, out, '%g' % x0, '%g' % z0, '%g' % x1, '%g' % z1, '--step', '%g' % step)


# ---- the game run ------------------------------------------------------------------
class Job:
    """Windows: a job object that ends the game if this tool dies (kill-on-close)."""

    def __init__(self):
        self.h = None
        if not WINDOWS:
            return
        try:
            import ctypes
            from ctypes import wintypes
            k32 = ctypes.WinDLL('kernel32', use_last_error=True)
            k32.CreateJobObjectW.restype = wintypes.HANDLE
            h = k32.CreateJobObjectW(None, None)
            if not h:
                return

            class BASIC(ctypes.Structure):
                _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64),
                            ('LimitFlags', wintypes.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                            ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wintypes.DWORD),
                            ('Affinity', ctypes.c_size_t), ('PriorityClass', wintypes.DWORD),
                            ('SchedulingClass', wintypes.DWORD)]

            class IO(ctypes.Structure):
                _fields_ = [(n, ctypes.c_uint64) for n in ('r', 'w', 'o', 'rt', 'wt', 'ot')]

            class EXT(ctypes.Structure):
                _fields_ = [('Basic', BASIC), ('Io', IO), ('ProcessMemoryLimit', ctypes.c_size_t),
                            ('JobMemoryLimit', ctypes.c_size_t), ('PeakProcessMemoryUsed', ctypes.c_size_t),
                            ('PeakJobMemoryUsed', ctypes.c_size_t)]
            info = EXT()
            info.Basic.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            if k32.SetInformationJobObject(wintypes.HANDLE(h), 9, ctypes.byref(info), ctypes.sizeof(info)):
                self.h, self.k32, self.wintypes = h, k32, wintypes
        except Exception:  # (best effort: without it the game is still stopped below)
            self.h = None

    def add(self, proc):
        if self.h:
            self.k32.AssignProcessToJobObject(self.wintypes.HANDLE(self.h), self.wintypes.HANDLE(int(proc._handle)))


def linux_child_setup():
    # The game ends with this tool (PR_SET_PDEATHSIG, SIGKILL).
    try:
        import ctypes
        ctypes.CDLL(None, use_errno=True).prctl(1, signal.SIGKILL, 0, 0, 0)
    except Exception:
        pass


def stop(proc):
    """Stops the game run (this PID only)."""
    if proc.poll() is not None:
        return
    try:
        proc.kill()  # TerminateProcess / SIGKILL
    except OSError:
        pass
    try:
        proc.wait(30)
    except subprocess.TimeoutExpired:
        pass


def dump_collision(exe, game, m, work, timeout, job):
    """Hosts the map in the game until it logs the play area: (live.obj, (x0, z0, x1, z1))."""
    live = os.path.join(work, 'live.obj')
    user = os.path.join(work, 'user')
    os.makedirs(user, exist_ok=True)
    log_path = os.path.join(work, 'game.log')
    env = dict(os.environ, BF_BOT_DUMP_COLLISION=live, BF_MAKE_NAVS_CHILD='1')
    # Its own ports: a server may be running on this PC (it can be the one that asked for this mesh).
    port_offset = str(60 + os.getpid() % 40)
    args = [exe, '--game', os.path.abspath(game), '--user', user, '--dedicated', '--maps', m, '--bots', '0',
            '--port-offset', port_offset, '--name', 'navgen']
    t0 = time.time()
    with open(log_path, 'wb') as log:
        kw = {}
        if WINDOWS:
            kw['creationflags'] = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS
        else:
            kw['preexec_fn'] = linux_child_setup
        try:
            proc = subprocess.Popen(args, cwd=work, env=env, stdin=subprocess.DEVNULL, stdout=log,
                                    stderr=subprocess.STDOUT, **kw)
        except OSError as e:
            raise Fail('cannot start %s: %s' % (exe, e))
        job.add(proc)
        say('  the game (PID %d) hosts %s to read its collision' % (proc.pid, m))
        area, dumped_at, pos, rest = None, None, 0, b''
        try:
            while True:
                time.sleep(1)
                with open(log_path, 'rb') as f:
                    f.seek(pos)
                    chunk = f.read()
                pos += len(chunk)
                lines = (rest + chunk).split(b'\n')
                rest = lines.pop()
                for ln in lines:
                    s = ln.decode('utf-8', 'replace').rstrip('\r')
                    if 'bot nav: collision of' in s:
                        dumped_at = time.time()
                        say('  ' + s.split('] ', 1)[-1][:160])
                    elif 'bot nav: play area' in s:
                        try:
                            area = tuple(float(v) for v in s.split('play area', 1)[1].split()[:4])
                        except ValueError:
                            raise Fail('cannot read the play area: ' + s[-80:])
                if area and len(area) == 4:
                    break
                if proc.poll() is not None:
                    raise Fail('the game stopped (exit code %s) before the play area was logged' % proc.returncode)
                if dumped_at and time.time() - dumped_at > PLAY_AREA_AFTER_DUMP:
                    raise Fail('the level has no play area (no flags or crates found)')
                if time.time() - t0 > timeout:
                    raise Fail('timed out after %d s waiting for the level to load' % timeout)
        except Fail:
            tail = log_tail(log_path)
            if tail:
                say('  the game\'s last lines:')
                for ln in tail:
                    say('    ' + ln)
            raise
        finally:
            stop(proc)
    if not os.path.isfile(live) or os.path.getsize(live) < 1000:
        raise Fail('the game wrote no collision')
    say('  play area %g %g %g %g (%d s)' % (*area, time.time() - t0))
    return live, area


def log_tail(path, n=6):
    try:
        with open(path, 'rb') as f:
            f.seek(max(0, os.path.getsize(path) - 8192))
            lines = f.read().decode('utf-8', 'replace').splitlines()
        return [ln[:200] for ln in lines[-n:]]
    except OSError:
        return []


# ---- one map -----------------------------------------------------------------------
class Lock:
    """<nav-dir>/<map>.making while this run works on the map, touched every few seconds."""

    def __init__(self, nav_dir, m):
        self.path = os.path.join(nav_dir, m + '.making')
        self.stop = threading.Event()
        self.thread = None

    def wait_and_take(self, limit):
        t0 = time.time()
        said = False
        while True:
            try:
                fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(fd, b'%d\n' % os.getpid())
                os.close(fd)
                break
            except FileExistsError:
                try:
                    age = time.time() - os.path.getmtime(self.path)
                except OSError:
                    continue
                if age > LOCK_STALE:
                    try:
                        os.remove(self.path)
                    except OSError:
                        time.sleep(1)
                    continue
                if not said:
                    say('  another run is making this map: waiting for it')
                    said = True
                if time.time() - t0 > limit:
                    raise Fail('another run is still making this map')
                time.sleep(3)
        self.thread = threading.Thread(target=self.beat, daemon=True)
        self.thread.start()

    def beat(self):
        while not self.stop.wait(10):
            try:
                os.utime(self.path)
            except OSError:
                pass

    def release(self):
        self.stop.set()
        try:
            os.remove(self.path)
        except OSError:
            pass


def missing(nav_dir, title, m, force):
    return [ext for ext in TITLES[title]['files'] if force or not os.path.isfile(os.path.join(nav_dir, m + ext))]


def make_map(a, m, navgen, scratch, job):
    title = a['title']
    if m.lower() in TITLES[title]['no_mesh']:
        say('  %s: an air map, the bots need no mesh there' % m)
        return
    if not missing(a['nav_dir'], title, m, a['force']):
        say('  %s: already there' % m)
        return
    lock = Lock(a['nav_dir'], m)
    lock.wait_and_take(a['timeout'] * 3)
    try:
        todo = missing(a['nav_dir'], title, m, a['force'])  # (again: the other run may have made them)
        if not todo:
            say('  %s: made by the other run' % m)
            return
        say('  %s: making %s' % (m, ' '.join(m + e for e in todo)))
        work = os.path.join(scratch, m)
        os.makedirs(work, exist_ok=True)
        if not level_dir(a['game'], title, m):
            raise Fail('no such map in the game files')
        hf = heightfield(a['game'], title, m, work)
        made = []

        def put(tmp, ext):
            # Copied next to it first (the scratch can be on another drive), then renamed:
            # the game never sees half a file.
            dst = os.path.join(a['nav_dir'], m + ext)
            shutil.copyfile(tmp, dst + '.part')
            os.replace(dst + '.part', dst)
            made.append(m + ext)

        if '.hf' in todo:
            tmp = os.path.join(work, m + '.hf')
            run_py('terrain.py', hf, '--hf', tmp)
            put(tmp, '.hf')
        meshes = [e for e in todo if e in ('.nav', '.vnav')]
        if meshes:
            if not navgen:
                raise Fail('navgen is missing (build it: tools/navgen/CMakeLists.txt)')
            live, area = dump_collision(a['exe'], a['game'], m, work, a['timeout'], job)
            ground_obj = os.path.join(work, 'ground.obj')
            if title == 'bf1943':
                # As tools/make_navmesh.py --live: the terrain in 2 m steps, navgen's soldier.
                ground(hf, area, 2.0, ground_obj)
                tmp = os.path.join(work, m + '.nav')
                run_navgen(navgen, tmp, [ground_obj, live], area, [])
                put(tmp, '.nav')
            else:
                # Bad Company: the terrain at its own resolution. The vehicles' mesh over
                # the same play area (the crates' extent and 350 m: tools' "+150" on the
                # 200 m the game logged before).
                varea = area
                ground(hf, area, None, ground_obj)
                if '.nav' in meshes:
                    tmp = os.path.join(work, m + '.nav')
                    run_navgen(navgen, tmp, [ground_obj, live], area, ['--cell', '0.2', '--radius', '0.4'])
                    put(tmp, '.nav')
                if '.vnav' in meshes:
                    tmp = os.path.join(work, m + '.vnav')
                    run_navgen(navgen, tmp, [ground_obj, live], varea,
                               ['--cell', '0.4', '--radius', '2.0', '--climb', '0.6', '--slope', '35'])
                    put(tmp, '.vnav')
        say('  %s: %s' % (m, ', '.join(made)))
    finally:
        lock.release()
        shutil.rmtree(os.path.join(scratch, m), ignore_errors=True)


# ---- main --------------------------------------------------------------------------
def parse(argv):
    a = {'title': None, 'game': None, 'exe': None, 'nav_dir': None, 'maps': None, 'work': None, 'force': False,
         'timeout': 900}
    i = 0
    while i < len(argv):
        k = argv[i]
        if k == '--force':
            a['force'] = True
            i += 1
            continue
        if k in ('-h', '--help'):
            print(__doc__)
            sys.exit(0)
        key = {'--title': 'title', '--game': 'game', '--exe': 'exe', '--nav-dir': 'nav_dir', '--maps': 'maps',
               '--work': 'work', '--timeout': 'timeout'}.get(k)
        if not key or i + 1 >= len(argv):
            raise Fail('unknown option or missing value: ' + k)
        a[key] = argv[i + 1]
        i += 2
    if a['title'] not in TITLES:
        raise Fail('--title: bf1943 or bfbc1')
    for k in ('game', 'exe', 'nav_dir'):
        if not a[k]:
            raise Fail('--%s is required' % k.replace('_', '-'))
    a['game'] = os.path.abspath(a['game'])
    a['exe'] = os.path.abspath(a['exe'])
    a['nav_dir'] = os.path.abspath(a['nav_dir'])
    if not os.path.isfile(os.path.join(a['game'], 'default.xex')):
        raise Fail('no default.xex in ' + a['game'])
    if not os.path.isfile(a['exe']):
        raise Fail('no game program at ' + a['exe'])
    try:
        a['timeout'] = max(60, int(a['timeout']))
    except ValueError:
        raise Fail('--timeout: seconds')
    maps = [m.strip() for m in (a['maps'] or '').split(',') if m.strip()]
    a['maps'] = maps or TITLES[a['title']]['maps']
    return a


def clean_old_scratch(parent):
    """Scratch folders a run that was stopped left behind (untouched for 12 hours)."""
    for d in glob.glob(os.path.join(parent, 'bf_navs_*')):
        try:
            newest = max([os.path.getmtime(d)] + [os.path.getmtime(os.path.join(r, f))
                                                  for r, _, fs in os.walk(d) for f in fs])
            if time.time() - newest > 12 * 3600:
                shutil.rmtree(d, ignore_errors=True)
        except OSError:
            pass


def main():
    try:
        a = parse(sys.argv[1:])
        os.makedirs(a['nav_dir'], exist_ok=True)
        if a['work']:
            os.makedirs(a['work'], exist_ok=True)
        scratch = tempfile.mkdtemp(prefix='bf_navs_', dir=a['work'] or None)
        clean_old_scratch(os.path.dirname(scratch))
    except (Fail, OSError) as e:
        say('error: %s' % e)
        return 1
    navgen = find_navgen(a['exe'])
    job = Job()
    total = len(a['maps'])
    failed = []
    say('progress nav 0 %d' % total)
    try:
        for n, m in enumerate(a['maps']):
            say('== %s (%d of %d)' % (m, n + 1, total))
            t0 = time.time()
            try:
                make_map(a, m, navgen, scratch, job)
                say('map %s ok' % m)
            except Fail as e:
                failed.append(m)
                say('map %s fail %s' % (m, e))
            except Exception as e:  # (OSError and the like: this map fails, the others still go)
                failed.append(m)
                say('map %s fail %s' % (m, str(e).replace('\n', ' ') or type(e).__name__))
            if time.time() - t0 > 5:
                say('  (%d s)' % (time.time() - t0))
            say('progress nav %d %d' % (n + 1, total))
    except KeyboardInterrupt:
        say('error: stopped')
        return 1
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    if failed:
        say('error: %d of %d maps failed: %s' % (len(failed), total, ', '.join(failed)))
        return 1
    say('ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
