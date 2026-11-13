"""BF1943 texture pack tool: 2x upscaled textures with a full PBR map set.

  python texpack.py extract --game <original/v106> --out <work>
      Decodes every 2D texture in the game archives to <work>/source/*.png and
      writes <work>/source/index.json.
  python texpack.py build --work <work> [--filter text] [--limit N]
      Groups textures into materials (color / normal / spec / AO by name),
      upscales 2x with Real-ESRGAN (ncnn, Vulkan) and writes, per material, to
      <work>/pack/<name>/:
        albedo.png  normal.png (OpenGL, +Y)  roughness.png  metallic.png
        ao.png  height.png  orm.png (R=AO, G=roughness, B=metallic)
      Non-material textures (UI, masks, effects) are upscaled only.
  python texpack.py install --work <work> --game <original/v106> --out <dir>
      Writes the textures the game can use (albedo, normal and the upscaled
      non-material textures) as <dir>/rtex/*.rtex and <dir>/replacements.txt,
      for bf1943 --textures <dir>.
  python texpack.py make --game <original/v106> --work <work> [--out <dir>]
      All three in one (the launcher's button): what is already done is
      skipped, so a stopped run carries on. Fetches Real-ESRGAN for this
      system (Windows, Linux or macOS) into <work>/realesrgan when there is
      none, and numpy and Pillow with pip when they are missing. Prints
      "progress <step> <done> <total>" lines for the launcher. --out defaults
      to <work>/install.
  python texpack.py fetch --to <dir>
      Only fetches Real-ESRGAN (realesrgan-ncnn-vulkan, MIT, any Vulkan GPU).

The work folder holds game-derived data: keep it outside the public repository.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

import zipfile


def _need(modules):
    """numpy and Pillow when missing: with pip for this user, or, where the
    system's Python refuses that (PEP 668: Ubuntu, Debian, Fedora...), in a
    Python of texpack's own (a venv in the user's cache folder) that this
    script then runs again in."""
    missing = []
    for mod, pkg in modules:
        try:
            __import__(mod)
        except ImportError:
            missing.append(pkg)
    if not missing:
        return
    pip = ['-m', 'pip', 'install', '--disable-pip-version-check', '-q']
    managed = os.name != 'nt' and sys.prefix == sys.base_prefix and any(
        os.path.isfile(os.path.join(d, 'EXTERNALLY-MANAGED'))
        for d in {os.path.dirname(os.__file__), os.path.join(sys.base_prefix, 'lib', 'python%d.%d' % sys.version_info[:2])})
    if not managed:
        print('installing', ' '.join(missing), 'with pip', flush=True)
        if subprocess.run([sys.executable] + pip + ['--user'] + missing).returncode == 0:
            return
    if sys.prefix != sys.base_prefix:  # in a venv already (ours or the user's)
        subprocess.run([sys.executable] + pip + missing, check=True)
        return
    cache = os.environ.get('XDG_CACHE_HOME') or os.path.join(os.path.expanduser('~'), '.cache')
    venv = os.path.join(cache, 'bf1943-texpack', 'venv')
    py = os.path.join(venv, 'Scripts' if os.name == 'nt' else 'bin', 'python' + ('.exe' if os.name == 'nt' else ''))
    if not os.path.isfile(py):
        print('making a Python for texpack in', venv, flush=True)
        if subprocess.run([sys.executable, '-m', 'venv', venv], stderr=subprocess.DEVNULL).returncode != 0:
            # (Ubuntu and Debian leave pip out of the venv module, python3-venv:
            # a venv without it, and pip fetched into it, needs no root)
            import urllib.request
            ok = subprocess.run([sys.executable, '-m', 'venv', '--without-pip', '--clear', venv]).returncode == 0
            if ok:
                get_pip = os.path.join(cache, 'bf1943-texpack', 'get-pip.py')
                try:
                    urllib.request.urlretrieve('https://bootstrap.pypa.io/get-pip.py', get_pip)
                    ok = subprocess.run([py, get_pip, '-q', '--disable-pip-version-check']).returncode == 0
                except OSError:
                    ok = False
            if not ok:
                sys.exit('Could not make a Python for texpack: install the venv module '
                         '(Ubuntu and Debian: sudo apt install python3-venv), then try again.')
    print('installing', ' '.join(missing), 'into it', flush=True)
    subprocess.run([py] + pip + missing, check=True)
    os.execv(py, [py, os.path.abspath(__file__)] + sys.argv[1:])


_need([('numpy', 'numpy'), ('PIL', 'Pillow')])
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

import xtexture as xt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ESRGAN_EXE = 'realesrgan-ncnn-vulkan' + ('.exe' if os.name == 'nt' else '')
# Real-ESRGAN ncnn Vulkan 0.2.5.0 (MIT), per system: the zip, its SHA-256 and size.
ESRGAN_URL = 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-%s.zip'
ESRGAN_ZIPS = {
    'windows': ('abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d', 45474481),
    'ubuntu': ('e5aa6eb131234b87c0c51f82b89390f5e3e642b7b70f2b9bbe95b6a285a40c96', 46931474),
    'macos': ('e0ad05580abfeb25f8d8fb55aaf7bedf552c375b5b4d9bd3c8d59764d2cc333a', 51817124),
}
# Where an unpacked Real-ESRGAN is looked for (else --esrgan, else fetched).
ESRGAN_PLACES = [os.path.join(HERE, '..', 'realesrgan'), os.path.join(HERE, '..', '..', '..', 'tools', 'realesrgan')]
DEFAULT_ESRGAN = os.path.normpath(os.path.join(ESRGAN_PLACES[-1], ESRGAN_EXE))
PAD = 16  # wrap padding (source pixels) so tiling textures stay seamless


# ---------------------------------------------------------------- Real-ESRGAN

def esrgan_system():
    if sys.platform.startswith('win'):
        return 'windows'
    if sys.platform == 'darwin':
        return 'macos'
    return 'ubuntu'  # (the Linux build: it runs on any distribution with a Vulkan driver)


def find_esrgan(extra=()):
    for d in list(extra) + ESRGAN_PLACES:
        exe = os.path.normpath(os.path.join(d, ESRGAN_EXE))
        if os.path.isfile(exe):
            return exe
    return None


def fetch_esrgan(to):
    """Downloads and unpacks this system's Real-ESRGAN into `to`; returns the program."""
    import hashlib as _h
    import urllib.request
    system = esrgan_system()
    sha, size = ESRGAN_ZIPS[system]
    os.makedirs(to, exist_ok=True)
    zpath = os.path.join(to, 'realesrgan-%s.zip' % system)
    if not (os.path.isfile(zpath) and os.path.getsize(zpath) == size):
        print('fetching Real-ESRGAN for', system, '(%d MB)' % (size >> 20), flush=True)
        part = zpath + '.part'
        with urllib.request.urlopen(ESRGAN_URL % system, timeout=60) as resp, open(part, 'wb') as f:
            got = 0
            while True:
                chunk = resp.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
                print('progress fetch %d %d' % (got, size), flush=True)
        os.replace(part, zpath)
    digest = _h.sha256(open(zpath, 'rb').read()).hexdigest()
    if digest != sha:
        os.remove(zpath)
        sys.exit('Real-ESRGAN download is not the expected file (SHA-256 %s): try again' % digest)
    with zipfile.ZipFile(zpath) as z:
        for info in z.infolist():
            if info.filename.startswith(('models/', 'realesrgan-ncnn-vulkan', 'vcomp')) or info.filename.endswith('.dylib'):
                z.extract(info, to)
    exe = os.path.join(to, ESRGAN_EXE)
    if os.name != 'nt':
        os.chmod(exe, 0o755)  # (a zip keeps no execute bit)
    os.remove(zpath)
    print('Real-ESRGAN ready:', exe, flush=True)
    return exe


def cmd_fetch(args):
    fetch_esrgan(args.to)


# ---------------------------------------------------------------- extract

def fnv1a(b):
    h = 0xcbf29ce484222325
    for c in b:
        h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def cmd_extract(args):
    dist = os.path.join(args.game, 'dist', 'xenon')
    out = os.path.join(args.out, 'source')
    os.makedirs(out, exist_ok=True)
    index = {}
    for root, _, files in os.walk(dist):
        for fn in sorted(files):
            if not fn.endswith('.fbrb') or 'sound' in fn:
                continue
            path = os.path.join(root, fn)
            try:
                entries = xt.read_fbrb(path)
            except Exception as e:  # noqa: BLE001
                print('skip', fn, e)
                continue
            for name, ext, data in entries:
                if ext != 'XenonTexture' or len(data) < 92 or data[:4] != b'\0\0\0\3':
                    continue
                tex = xt.Texture(data)
                if tex.format not in xt.FORMATS or tex.cube or name in index:
                    continue
                try:
                    img = xt.decode(tex)
                except Exception as e:  # noqa: BLE001
                    print('fail', name, tex.format_name, e)
                    continue
                key = name.replace('/', '__')
                Image.fromarray(img, 'RGBA').save(os.path.join(out, key + '.png'))
                index[name] = {
                    'file': key + '.png', 'archive': os.path.relpath(path, dist).replace('\\', '/'),
                    'format': tex.format_name, 'width': tex.width, 'height': tex.height,
                    'mips': tex.mips, 'sha1': hashlib.sha1(data).hexdigest(),
                }
            print('%-48s %5d textures so far' % (fn, len(index)))
    with open(os.path.join(out, 'index.json'), 'w') as f:
        json.dump(index, f, indent=1, sort_keys=True)
    print('extracted', len(index), 'textures to', out)


# ---------------------------------------------------------------- grouping

# Frostbite 1 suffixes: Df/Dm/Cm colour, Nm normal, Sp specular, Gm gloss and
# Px parallax height (one channel), Oc/Occlusion ambient occlusion.
ROLES = [
    ('normal', r'(normal|nrm|norm|nm|_n)$'),
    ('spec', r'(spec|specular|_sp|_sm|_s)$'),
    ('gloss', r'(gloss|_gm)$'),
    ('ao', r'(_ao|_oc|_occ|occlusion)$'),
    ('height', r'(_px|height|_hm)$'),
    ('color', r'(diffuse|color|colour|albedo|diff|_df|_dm|_cm|_dif|_col|_d|_c)$'),
]
MAP_ROLES = ('color', 'normal', 'spec', 'gloss', 'ao', 'height')


def classify(name):
    name = name[:-4] if name.lower().endswith(".res") else name
    base = name.rsplit("/", 1)[-1]
    low = base.lower()
    for role, pat in ROLES:
        m = re.search(pat, low)
        if m:
            stem = base[:m.start()].rstrip('_- ')
            return role, name[:len(name) - len(base)] + stem
    return None, name


def group(index):
    mats, other = {}, []
    for name, info in index.items():
        role, stem = classify(name)
        if role is None:
            other.append(name)
        else:
            mats.setdefault(stem.lower(), {'stem': stem})[role] = name
    # Colour variants (Bricks_t03v02) share the base material's other maps.
    for key, m in mats.items():
        base = re.sub(r'v\d+$', '', key)
        if base != key and base in mats:
            for role in MAP_ROLES[1:]:
                if role not in m and role in mats[base]:
                    m[role] = mats[base][role]
    for key in list(mats):
        if 'color' not in mats[key]:  # loose normal/spec maps: upscale only
            other += [v for k, v in mats[key].items() if k != 'stem']
            del mats[key]
    return mats, sorted(other)


# ---------------------------------------------------------------- upscaling

def esrgan_batch(images, exe):
    """images: {key: (h, w, 3) uint8} -> {key: (2h, 2w, 3) uint8}, seamless (wrap)."""
    if not images:
        return {}
    tmp = tempfile.mkdtemp(prefix='texpack_')
    src, dst = os.path.join(tmp, 'in'), os.path.join(tmp, 'out')
    os.makedirs(src)
    os.makedirs(dst)
    pads = {}
    for i, (key, img) in enumerate(images.items()):
        p = min(PAD, img.shape[0] // 2, img.shape[1] // 2)
        pads[key] = (i, p, img.shape)
        Image.fromarray(np.pad(img, ((p, p), (p, p), (0, 0)), mode='wrap')).save(
            os.path.join(src, '%05d.png' % i))
    subprocess.run([exe, '-i', src, '-o', dst, '-n', 'realesrgan-x4plus', '-f', 'png',
                    '-m', os.path.join(os.path.dirname(exe), 'models')],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    out = {}
    for key, (i, p, shape) in pads.items():
        big = Image.open(os.path.join(dst, '%05d.png' % i)).convert('RGB')
        h, w = shape[:2]
        big = big.crop((4 * p, 4 * p, 4 * (p + w), 4 * (p + h)))
        out[key] = np.asarray(big.resize((2 * w, 2 * h), Image.LANCZOS))
    shutil.rmtree(tmp, ignore_errors=True)
    return out


def lanczos2x(a):
    """2x Lanczos with wrap-around edges (seamless for tiling textures)."""
    h, w = a.shape[:2]
    p = min(8, h // 2, w // 2)
    padded = np.pad(a, ((p, p), (p, p)) + ((0, 0),) * (a.ndim - 2), mode='wrap')
    big = Image.fromarray(padded).resize((padded.shape[1] * 2, padded.shape[0] * 2), Image.LANCZOS)
    return np.asarray(big.crop((2 * p, 2 * p, 2 * (p + w), 2 * (p + h))))


# ---------------------------------------------------------------- PBR maps

def f32(a):
    return a.astype(np.float32) / 255.0


def u8(a):
    return (np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)


def lum(rgb):
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def blur(a, r):
    """Wrap-around box blur (3 passes ~ Gaussian), radius r pixels."""
    if r < 1:
        return a
    out = a
    for _ in range(3):
        for axis in (0, 1):
            rr = min(r, out.shape[axis] // 2 - 1)
            if rr < 1:
                continue
            c = np.cumsum(np.concatenate([out.take(range(-rr - 1, 0), axis), out,
                                          out.take(range(0, rr), axis)], axis), axis)
            n = out.shape[axis]
            out = (c.take(range(2 * rr + 1, 2 * rr + 1 + n), axis) - c.take(range(0, n), axis)) / (2 * rr + 1)
    return out


def normal_vectors(rgb):
    n = f32(rgb[..., :3]) * 2 - 1
    n[..., 2] = np.sqrt(np.clip(1 - n[..., 0] ** 2 - n[..., 1] ** 2, 0, 1))
    return n / np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)


def height_from_normals(n, y_down):
    """Frankot-Chellappa integration (periodic, so tiling stays seamless)."""
    nz = np.maximum(n[..., 2], 0.2)
    p = -n[..., 0] / nz                                # dh/dx
    q = (n[..., 1] if y_down else -n[..., 1]) / -nz    # dh/drow (image rows go down)
    h, w = p.shape
    wx = np.fft.fftfreq(w)[None, :] * 2 * np.pi
    wy = np.fft.fftfreq(h)[:, None] * 2 * np.pi
    d = wx ** 2 + wy ** 2
    d[0, 0] = 1
    z = (-1j * wx * np.fft.fft2(p) - 1j * wy * np.fft.fft2(q)) / d
    z[0, 0] = 0
    hm = np.real(np.fft.ifft2(z))
    lo, hi = np.percentile(hm, [1, 99])
    return np.clip((hm - lo) / max(hi - lo, 1e-6), 0, 1)


def normals_from_height(hm, strength):
    gx = (np.roll(hm, -1, 1) - np.roll(hm, 1, 1)) * 0.5 * strength
    gy = (np.roll(hm, -1, 0) - np.roll(hm, 1, 0)) * 0.5 * strength
    n = np.stack([-gx, gy, np.ones_like(hm)], -1)      # OpenGL: +Y up
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


def encode_normals(n):
    return u8(n * 0.5 + 0.5)


def ambient_occlusion(hm, scale):
    ao = np.ones_like(hm)
    for r, wgt in ((max(1, scale // 128), 0.3), (max(2, scale // 32), 0.25), (max(4, scale // 8), 0.15)):
        ao -= wgt * np.clip((blur(hm, r) - hm) * 3, 0, 1)
    return np.clip(ao, 0, 1)


METAL_WORDS = re.compile(r'(metal|steel|iron|gun|rifle|pistol|smg|bazooka|weapon|bolt|barrel|'
                         r'knife|bayonet|grenade|wrench|engine|propeller|turret|cannon|railing)')


def build_material(stem, color, normal, spec, gloss, ao_src, height_src):
    """All inputs are 2x upscaled uint8 arrays (or None). Returns {map: array}."""
    rgba = color
    albedo = f32(rgba[..., :3])
    l = lum(albedo)
    h, w = l.shape
    detail = l - blur(l, max(2, w // 128))

    # Normals and height. The game's normal maps are D3D-style; pick the green
    # direction whose height agrees best with the dark cavities of the albedo.
    if normal is not None:
        nv = normal_vectors(normal)
        if nv.shape[:2] != (h, w):
            nv = normal_vectors(np.asarray(Image.fromarray(normal).resize((w, h), Image.LANCZOS)))
        h_dn, h_up = height_from_normals(nv, True), height_from_normals(nv, False)
        y_down = np.corrcoef(h_dn.ravel(), detail.ravel())[0, 1] >= np.corrcoef(h_up.ravel(), detail.ravel())[0, 1]
        height = h_dn if y_down else h_up
        if y_down:
            nv[..., 1] = -nv[..., 1]  # to OpenGL
        # Fine surface detail from the upscaled albedo on top of the game's normals.
        fine = normals_from_height(detail * 0.6, w / 256)
        nv = nv + np.stack([fine[..., 0], fine[..., 1], np.zeros_like(l)], -1) * 0.5
        nv /= np.linalg.norm(nv, axis=-1, keepdims=True)
    else:
        height = np.clip(blur(l, 1) - blur(l, max(4, w // 32)) + 0.5, 0, 1)
        nv = normals_from_height(height, w / 96)
    if height_src is not None:  # the game's parallax map is the real height
        height = lum(f32(height_src[..., :3]))
        if height.shape != (h, w):
            height = np.asarray(Image.fromarray(u8(height)).resize((w, h), Image.LANCZOS)).astype(np.float32) / 255

    # Specular intensity -> roughness / metallic.
    metal_name = bool(METAL_WORDS.search(stem.lower()))
    if spec is not None:
        sp = f32(spec)
        if sp.shape[:2] != (h, w):
            sp = f32(np.asarray(Image.fromarray(spec).resize((w, h), Image.LANCZOS)))
        s = lum(sp[..., :3])
        s = s / max(np.percentile(s, 99.5), 0.05)
        roughness = np.clip(1.0 - 0.85 * np.sqrt(np.clip(s, 0, 1)), 0.08, 1.0)
        metallic = np.clip((s - 0.45) / 0.3, 0, 1) * (1.0 if metal_name else 0.0)
    else:
        roughness = np.clip(0.82 - detail * 1.5, 0.35, 1.0)
        metallic = np.full_like(l, 0.85 if metal_name else 0.0)
        if metal_name:
            roughness = np.clip(roughness - 0.3, 0.2, 1.0)

    if gloss is not None:  # the game's own gloss map wins over the spec guess
        g = lum(f32(gloss[..., :3]))
        if g.shape != (h, w):
            g = np.asarray(Image.fromarray(u8(g)).resize((w, h), Image.LANCZOS)).astype(np.float32) / 255
        roughness = np.clip(1.0 - 0.75 * g, 0.15, 1.0)

    if ao_src is not None:
        ao = lum(f32(ao_src[..., :3]))
        if ao.shape != (h, w):
            ao = np.asarray(Image.fromarray(u8(ao)).resize((w, h), Image.LANCZOS)).astype(np.float32) / 255
    else:
        ao = ambient_occlusion(height, w)
    roughness = np.clip(roughness + (1 - ao) * 0.15, 0, 1)  # crevices are duller

    return {
        'albedo': rgba,
        'normal': encode_normals(nv),
        'roughness': u8(roughness),
        'metallic': u8(metallic),
        'ao': u8(ao),
        'height': u8(height),
        'orm': np.stack([u8(ao), u8(roughness), u8(metallic)], -1),
    }


def load_rgba(work, index, name):
    return np.asarray(Image.open(os.path.join(work, 'source', index[name]['file'])).convert('RGBA'))


def upscale_rgba(imgs, esr):
    """{key: rgba} -> {key: 2x rgba}; alpha goes through the upscaler too when used."""
    rgb = {k: v[..., :3] for k, v in imgs.items()}
    alpha = {('#a', k): np.repeat(v[..., 3:], 3, -1) for k, v in imgs.items() if v[..., 3].min() < 250}
    up = esrgan_batch({**rgb, **alpha}, esr)
    out = {}
    for k, v in imgs.items():
        a = up[('#a', k)][..., :1] if ('#a', k) in up else np.full(up[k].shape[:2] + (1,), 255, np.uint8)
        out[k] = np.concatenate([up[k], a], -1)
    return out


# Data textures pack independent masks into the channels (CombinedAOs: one AO
# per channel). Real-ESRGAN treats RGB as a photo and bleeds and invents detail
# across channels, which smears them in game: these go through it one channel
# at a time.
PACKED_WORDS = re.compile(r'(_ao|ao_|aos|occ|mask|colorpick|combined|_gm|gloss|_sp$|spec|lut|blend|height|_px)')


def is_packed(name, img):
    if PACKED_WORDS.search(name.lower()):
        return True
    rgb = img[..., :3].reshape(-1, 3).astype(np.float32)
    if rgb.shape[0] > 65536:
        rgb = rgb[::rgb.shape[0] // 65536]
    std = rgb.std(0)
    if (std > 8).sum() < 2:
        return False
    c = np.corrcoef(rgb[:, std > 8].T)
    return float(c.min()) < 0.5


def upscale_channels(imgs, esr):
    """{key: rgba} -> {key: 2x rgba}, each varying channel upscaled on its own."""
    jobs = {}
    for k, v in imgs.items():
        for c in range(4):
            if v[..., c].std() > 1:
                jobs[(k, c)] = np.repeat(v[..., c:c + 1], 3, -1)
    up = esrgan_batch(jobs, esr)
    out = {}
    for k, v in imgs.items():
        h, w = v.shape[:2]
        chans = []
        for c in range(4):
            if (k, c) in up:
                chans.append(np.asarray(up[(k, c)])[..., 0])
            else:
                chans.append(np.full((2 * h, 2 * w), v[..., c].flat[0], np.uint8))
        out[k] = np.stack(chans, -1)
    return out


def save_manifest(path, manifest):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        json.dump(manifest, f, indent=1, sort_keys=True)


def cmd_build(args):
    work = args.work
    with open(os.path.join(work, 'source', 'index.json')) as f:
        index = json.load(f)
    mats, other = group(index)
    flt = (args.filter or '').lower()
    mat_keys = [k for k in sorted(mats) if flt in k]
    other = [n for n in other if flt in n.lower()] if not args.materials_only else []
    if args.packed_only:  # redo just the per-channel textures
        mat_keys = []
        other = [n for n in other if not index[n]['format'].startswith('Normal')
                 and is_packed(n, load_rgba(work, index, n))]
        print(len(other), 'packed data textures')
    if args.limit:
        mat_keys, other = mat_keys[:args.limit], other[:args.limit]
    esr = args.esrgan if args.esrgan else find_esrgan([os.path.join(work, 'realesrgan')])
    if not esr or not os.path.isfile(esr):
        sys.exit('Real-ESRGAN not found (--esrgan, or texpack.py fetch): ' + str(esr))
    pack = os.path.join(work, 'pack')
    manifest_path = os.path.join(pack, 'manifest.json')
    manifest = json.load(open(manifest_path)) if os.path.isfile(manifest_path) else {}
    if args.resume:  # skip what an earlier (interrupted) build already wrote
        mat_keys = [k for k in mat_keys if mats[k]['stem'] not in manifest]
        other = [n for n in other if n not in manifest]
        print('resume:', len(mat_keys), 'materials and', len(other), 'textures left')

    batch = max(1, args.batch)
    total, done_n = len(mat_keys) + len(other), 0
    print('progress build 0 %d' % total, flush=True)
    for i in range(0, len(mat_keys), batch):
        keys = mat_keys[i:i + batch]
        srcs = {}
        for k in keys:
            for role in MAP_ROLES:
                if role in mats[k]:
                    srcs[(k, role)] = load_rgba(work, index, mats[k][role])
        up = upscale_rgba({key: v for key, v in srcs.items() if key[1] != 'normal'}, esr)
        for key, v in srcs.items():  # vectors: smooth resample, renormalised later
            if key[1] == 'normal':
                up[key] = lanczos2x(v[..., :3])
        for k in keys:
            m = mats[k]
            maps = build_material(m['stem'], up[(k, 'color')], up.get((k, 'normal')),
                                  up.get((k, 'spec')), up.get((k, 'gloss')), up.get((k, 'ao')),
                                  up.get((k, 'height')))
            d = os.path.join(pack, m['stem'].replace('/', '__'))
            os.makedirs(d, exist_ok=True)
            for name, a in maps.items():
                Image.fromarray(a).save(os.path.join(d, name + '.png'))
            manifest[m['stem']] = {
                'type': 'material', 'folder': os.path.basename(d),
                'sources': {r: {'name': m[r], 'sha1': index[m[r]]['sha1'], 'width': index[m[r]]['width'],
                                'height': index[m[r]]['height']} for r in MAP_ROLES if r in m},
            }
            print('material', m['stem'], up[(k, 'color')].shape[1], 'x', up[(k, 'color')].shape[0])
        save_manifest(manifest_path, manifest)
        done_n += len(keys)
        print('progress build %d %d' % (done_n, total), flush=True)

    for i in range(0, len(other), batch):
        names = other[i:i + batch]
        srcs = {n: load_rgba(work, index, n) for n in names}
        normals = {n for n in names if index[n]['format'].startswith('Normal')}
        packed = {n for n in names if n not in normals and is_packed(n, srcs[n])}
        up = upscale_rgba({n: v for n, v in srcs.items() if n not in normals and n not in packed}, esr)
        up.update(upscale_channels({n: srcs[n] for n in packed}, esr))
        for n in normals:  # vectors: smooth resample + renormalise, no hallucinated detail
            nv = normal_vectors(lanczos2x(srcs[n][..., :3]))
            up[n] = np.concatenate([encode_normals(nv), np.full(nv.shape[:2] + (1,), 255, np.uint8)], -1)
        d = os.path.join(pack, '_textures')
        os.makedirs(d, exist_ok=True)
        for n in names:
            Image.fromarray(up[n]).save(os.path.join(d, index[n]['file']))
            manifest[n] = {'type': 'texture', 'file': '_textures/' + index[n]['file'],
                           'sha1': index[n]['sha1'], 'width': index[n]['width'], 'height': index[n]['height']}
            print('texture', n)
        save_manifest(manifest_path, manifest)
        done_n += len(names)
        print('progress build %d %d' % (done_n, total), flush=True)
    save_manifest(manifest_path, manifest)
    print('done:', len(mat_keys), 'materials,', len(other), 'textures ->', pack)


# ---------------------------------------------------------------- install

# Formats the runtime can replace (block-compressed 2D) and how the game's host
# format lays out the channels: the replacement must put them in the same place.
REPLACEABLE = {'DXT1', 'DXT3', 'DXT5', 'DXT5A', 'DXN', 'NormalDXN', 'NormalDXT1', 'NormalDXT5'}


def guest_hash(data):
    """Same as xe::gpu::texrep::Hash over the first 4 KB of the tiled base level."""
    return fnv1a(data[92:92 + 4096].ljust(4096, b'\0'))


def to_game_layout(img, fmt):
    """RGBA in the channel layout the game reads for this format."""
    if fmt == 'DXT5A':
        return np.repeat(img[..., :1], 4, -1)
    if fmt in ('DXN', 'NormalDXN'):
        out = img.copy()
        out[..., 2], out[..., 3] = 0, 255
        return out
    if fmt == 'NormalDXT5':  # DXT5nm: X in alpha, Y in green
        out = img.copy()
        out[..., 3] = img[..., 0]
        return out
    if fmt == 'NormalDXT1':
        out = img.copy()
        out[..., 3] = 255
        return out
    return img


def game_normal(pack_normal, source):
    """Pack normals are OpenGL; flip green back if the game's map is D3D-style."""
    n = pack_normal[..., :3].copy()
    small = np.asarray(Image.fromarray(n).resize(source.shape[1::-1], Image.BOX)).astype(np.float32)
    g_pack, g_src = small[..., 1].ravel(), source[..., 1].astype(np.float32).ravel()
    if g_pack.std() > 1 and g_src.std() > 1 and np.corrcoef(g_pack, g_src)[0, 1] < 0:
        n[..., 1] = 255 - n[..., 1]
    return np.concatenate([n, np.full(n.shape[:2] + (1,), 255, np.uint8)], -1)


# Normal maps whose Z the game's shaders rebuild from X and Y: these get the
# --pbr layout below whatever their format (the runtime swizzles X from red to
# alpha for DXT5nm, *.pbr.rtex).
PBR_NORMAL_FORMATS = {'NormalDXT1', 'NormalDXT5', 'NormalDXN', 'DXN'}


def pbr_blue(rough, metal):
    """Roughness in the upper half of blue (128-255), in the lower half where
    metallic (over half): the shader reads metal from blue < 0.5. Not metal
    keeps blue high, as before, for shaders that take it as the normal's Z."""
    return np.round(np.clip(rough, 0, 1) * 127 + np.where(metal > 0.5, 0, 128)).astype(np.uint8)


def add_pbr_channels(img, folder):
    """For bf1943 --pbr: X, Y in red, green; roughness and metal in blue
    (pbr_blue); the height (parallax) in alpha, 191-253.

    Alpha stays below 255 so the shader can tell these maps from all other
    textures, for which it uses its own fallback. Metallic is a per-material
    estimate, so it is kept off coloured parts (the wooden stock of a gun):
    saturated albedo is not metal.
    """
    out = img.copy()
    size = img.shape[1::-1]
    load = lambda name: np.asarray(Image.open(os.path.join(folder, name)).convert('L').resize(size, Image.LANCZOS)).astype(np.float32) / 255
    rough = load('roughness.png') if os.path.isfile(os.path.join(folder, 'roughness.png')) else np.full(size[::-1], 0.8, np.float32)
    metal = load('metallic.png') if os.path.isfile(os.path.join(folder, 'metallic.png')) else np.zeros(size[::-1], np.float32)
    if metal.max() > 0 and os.path.isfile(os.path.join(folder, 'albedo.png')):
        alb = np.asarray(Image.open(os.path.join(folder, 'albedo.png')).convert('RGB').resize(size, Image.LANCZOS)).astype(np.float32)
        sat = (alb.max(-1) - alb.min(-1)) / np.maximum(alb.max(-1), 1)
        metal *= np.clip((0.35 - sat) / 0.2, 0, 1)
    height = load('height.png') if os.path.isfile(os.path.join(folder, 'height.png')) else np.ones(size[::-1], np.float32)
    out[..., 2] = pbr_blue(rough, metal)
    # Height in the top quarter of alpha (191-253): shaders outside the --pbr
    # table that read alpha as a mask still see it near 1, as before.
    out[..., 3] = np.round(191 + height * 62).astype(np.uint8)
    return out


def loose_pbr_channels(img, name):
    """add_pbr_channels for normal maps without a colour map of their own (the
    game pairs them with other textures): roughness from the normal's own
    detail, metallic from the name."""
    n = normal_vectors(img[..., :3])
    flatness = np.clip(n[..., 2], 0, 1)
    detail = blur(1.0 - flatness, 2)
    rough = np.clip(0.72 - detail * 1.2, 0.3, 0.95)
    low = name.lower()
    metal = 0.0
    if METAL_WORDS.search(low) and not re.search(r'(paint|rust|grimy|dirt)', low):
        metal, rough = 0.85, np.clip(rough - 0.25, 0.2, 0.9)
    out = img.copy()
    out[..., 2] = pbr_blue(rough, np.full_like(rough, metal))
    out[..., 3] = 253  # no height: flat at the top, no parallax
    return out


def write_rtex(path, img):
    h, w = img.shape[:2]
    levels = [img]
    while max(levels[-1].shape[:2]) > 1:
        lh, lw = levels[-1].shape[:2]
        levels.append(np.asarray(Image.fromarray(levels[-1]).resize((max(lw // 2, 1), max(lh // 2, 1)), Image.BOX)))
    with open(path, 'wb') as f:
        f.write(b'RTX1' + struct.pack('<III', w, h, len(levels)))
        for lv in levels:
            f.write(np.ascontiguousarray(lv, np.uint8).tobytes())


def cmd_install(args):
    work, out = args.work, args.out
    pack = os.path.join(work, 'pack')
    with open(os.path.join(work, 'source', 'index.json')) as f:
        index = json.load(f)
    with open(os.path.join(pack, 'manifest.json')) as f:
        manifest = json.load(f)
    mats, _ = group(index)
    # texture name -> (role, pack image)
    wanted = {}
    for m in mats.values():
        folder = os.path.join(pack, m['stem'].replace('/', '__'))
        if not os.path.isdir(folder):
            continue
        if 'normal' in m and args.normals:
            wanted.setdefault(m['normal'], ('normal', os.path.join(folder, 'normal.png')))
        if 'color' in m:
            wanted.setdefault(m['color'], ('color', os.path.join(folder, 'albedo.png')))
    for n, e in manifest.items():
        if e.get('type') == 'texture' and (args.ui or not n.lower().startswith(('menu', 'awards', 'textures/ui', 'ui/'))):
            wanted.setdefault(n, ('texture', os.path.join(pack, e['file'])))
    flt = (args.filter or '').lower()
    wanted = {n: v for n, v in wanted.items() if flt in n.lower() and index[n]['format'] in REPLACEABLE}

    os.makedirs(os.path.join(out, 'rtex'), exist_ok=True)
    lines, done = [], set()
    dist = os.path.join(args.game, 'dist', 'xenon')
    for archive in sorted({index[n]['archive'] for n in wanted}):
        for name, ext, data in xt.read_fbrb(os.path.join(dist, archive)):
            if name not in wanted or name in done or ext != 'XenonTexture':
                continue
            done.add(name)
            role, src = wanted[name]
            fmt = index[name]['format']
            if not os.path.isfile(src):
                continue
            img = np.asarray(Image.open(src).convert('RGBA'))
            if role == 'normal':
                img = game_normal(img, load_rgba(work, index, name))
            pbr = args.pbr and fmt in PBR_NORMAL_FORMATS and role in ('normal', 'texture')
            if pbr and role == 'normal':
                img = add_pbr_channels(img, os.path.dirname(src))
            elif pbr:
                img = loose_pbr_channels(img, name)
            else:
                img = to_game_layout(img, fmt)
            fn = 'rtex/' + name.replace('/', '__').replace('.res', '') + ('.pbr.rtex' if pbr else '.rtex')
            write_rtex(os.path.join(out, fn), img)
            tex = xt.Texture(data)
            lines.append('%016X %d %d %s' % (guest_hash(data), tex.width, tex.height, fn))
            print(role, name, fmt, '%dx%d' % img.shape[1::-1])
    with open(os.path.join(out, 'replacements.txt'), 'w') as f:
        f.write('# texpack install: <hash of first 4 KB of the tiled base level> <guest width> <guest height> <file>\n')
        f.write('\n'.join(lines) + '\n')
    print('installed', len(lines), 'textures to', out)


# ---------------------------------------------------------------- make (all of it)

def cmd_make(args):
    work = args.work
    out = args.out or os.path.join(work, 'install')
    os.makedirs(work, exist_ok=True)
    esr = args.esrgan or find_esrgan([os.path.join(work, 'realesrgan')]) or fetch_esrgan(os.path.join(work, 'realesrgan'))
    if not os.path.isfile(os.path.join(work, 'source', 'index.json')):
        print('progress extract 0 1', flush=True)
        cmd_extract(argparse.Namespace(game=args.game, out=work))
    print('progress extract 1 1', flush=True)
    cmd_build(argparse.Namespace(work=work, filter=args.filter, limit=args.limit, resume=True, batch=args.batch,
                                 materials_only=False, packed_only=False, esrgan=esr))
    print('progress install 0 1', flush=True)
    cmd_install(argparse.Namespace(work=work, game=args.game, out=out, filter=None, ui=False, normals=True,
                                   pbr=not args.no_pbr))
    print('progress install 1 1', flush=True)
    print('pack ready:', out, flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    e = sub.add_parser('extract')
    e.add_argument('--game', required=True, help='game folder (contains dist/xenon)')
    e.add_argument('--out', required=True, help='work folder (outside the repo)')
    b = sub.add_parser('build')
    b.add_argument('--work', required=True)
    b.add_argument('--filter', help='only names containing this text')
    b.add_argument('--limit', type=int, default=0)
    b.add_argument('--resume', action='store_true', help='skip materials and textures already in the manifest')
    b.add_argument('--batch', type=int, default=16, help='textures per upscaler run')
    b.add_argument('--materials-only', action='store_true')
    b.add_argument('--packed-only', action='store_true',
                   help='only the data textures upscaled one channel at a time')
    b.add_argument('--esrgan', help='the realesrgan-ncnn-vulkan program (default: found or fetched)')
    i = sub.add_parser('install')
    i.add_argument('--work', required=True)
    i.add_argument('--game', required=True, help='game folder (contains dist/xenon)')
    i.add_argument('--out', required=True, help='folder for bf1943 --textures')
    i.add_argument('--filter', help='only names containing this text')
    i.add_argument('--ui', action='store_true', help='also replace menu/UI textures')
    i.add_argument('--no-normals', dest='normals', action='store_false')
    i.add_argument('--no-pbr', dest='pbr', action='store_false',
                   help='leave roughness/metallic out of the normal maps')
    m = sub.add_parser('make')
    m.add_argument('--game', required=True, help='game folder (contains dist/xenon)')
    m.add_argument('--work', required=True, help='work folder (outside the repo)')
    m.add_argument('--out', help='folder for bf1943 --textures (default <work>/install)')
    m.add_argument('--batch', type=int, default=16, help='textures per upscaler run')
    m.add_argument('--filter', help='only names containing this text (testing)')
    m.add_argument('--limit', type=int, default=0, help='at most this many to build (testing)')
    m.add_argument('--esrgan', help='the realesrgan-ncnn-vulkan program (default: found or fetched)')
    m.add_argument('--no-pbr', action='store_true', help='leave roughness/metallic out of the normal maps')
    f = sub.add_parser('fetch')
    f.add_argument('--to', required=True, help='folder for Real-ESRGAN')
    args = ap.parse_args()
    {'extract': cmd_extract, 'build': cmd_build, 'install': cmd_install, 'make': cmd_make,
     'fetch': cmd_fetch}[args.cmd](args)


if __name__ == '__main__':
    main()
