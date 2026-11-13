"""Installs the bots' advisor (--bot-llm, runtime/bot_llm.h) for a dedicated
server: llama.cpp's llama-server (a release build from GitHub) and a small
vision model (Qwen3.5 4B, Q4_K_M, with its vision projector, from Hugging
Face), then writes bot_llm.ini for the server to start it from.

  python tools/setup_bot_llm.py [--dir llm] [--backend auto|vulkan|cpu|cuda]
                                [--quant Q4_K_M] [--repo unsloth/Qwen3.5-4B-GGUF]

About 3.5 GB. A stopped download carries on where it was; the model files are
checked against Hugging Face's SHA-256. Then run the server with
--bot-llm <dir> (start_server.bat / .sh: BOT_LLM=1).
"""
import argparse
import ctypes.util
import hashlib
import json
import os
import platform
import re
import shutil
import stat
import sys
import tarfile
import urllib.request
import zipfile

UA = {'User-Agent': 'bf-recomp-setup-bot-llm'}


def get_json(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
        return json.load(r)


def download(url, dest, size=None, sha256=None):
    """url to dest, resuming a .part file; checked against size and sha256."""
    if os.path.exists(dest) and (size is None or os.path.getsize(dest) == size):
        if not sha256 or sha_of(dest) == sha256:
            print(f'have {os.path.basename(dest)}')
            return
        os.remove(dest)
    part = dest + '.part'
    have = os.path.getsize(part) if os.path.exists(part) else 0
    headers = dict(UA)
    if have:
        headers['Range'] = f'bytes={have}-'
    print(f'downloading {os.path.basename(dest)}' + (f' ({size / 1e9:.2f} GB)' if size else ''), flush=True)
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=120) as r:
        if have and r.status != 206:  # (no resuming there: from the start)
            have = 0
        total = size or (int(r.headers.get('Content-Length', 0)) + have)
        with open(part, 'ab' if have else 'wb') as f:
            done, shown = have, -1
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                done += len(chunk)
                pct = done * 100 // total if total else 0
                if pct != shown:
                    shown = pct
                    print(f'progress {os.path.basename(dest)} {done} {total}', flush=True)
    if size and os.path.getsize(part) != size:
        sys.exit(f'error: {os.path.basename(dest)}: {os.path.getsize(part)} bytes, want {size} (run again to carry on)')
    if sha256 and sha_of(part) != sha256:
        os.remove(part)
        sys.exit(f'error: {os.path.basename(dest)}: the SHA-256 does not match (removed; run again)')
    os.replace(part, dest)


def sha_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 22), b''):
            h.update(chunk)
    return h.hexdigest()


def safe_extract(archive, out):
    """Unpacks a zip or tar.gz into out, nothing outside it."""
    root = os.path.realpath(out)

    def inside(name):
        p = os.path.realpath(os.path.join(root, name))
        return p == root or p.startswith(root + os.sep)

    if archive.endswith('.zip'):
        with zipfile.ZipFile(archive) as z:
            for m in z.infolist():
                if not inside(m.filename):
                    sys.exit(f'error: {archive}: {m.filename} is outside the folder')
            z.extractall(root)
    else:
        with tarfile.open(archive) as t:
            members = []
            for m in t.getmembers():
                if not inside(m.name) or m.isdev() or ((m.issym() or m.islnk()) and not inside(
                        os.path.join(os.path.dirname(m.name), m.linkname))):
                    sys.exit(f'error: {archive}: {m.name} is outside the folder')
                members.append(m)
            t.extractall(root, members=members)


def pick_backend(backend):
    if backend != 'auto':
        return backend
    if sys.platform == 'win32':
        return 'vulkan'  # (falls back to the CPU without a Vulkan driver)
    return 'vulkan' if ctypes.util.find_library('vulkan') else 'cpu'


def llama_asset(releases, backend):
    if platform.machine().lower() not in ('amd64', 'x86_64'):
        sys.exit('error: x86-64 only')
    if sys.platform == 'win32':
        kinds = {'cpu': r'win-cpu-x64\.zip', 'vulkan': r'win-vulkan-x64\.zip', 'cuda': r'win-cuda-12\.\d+-x64\.zip'}
    elif sys.platform.startswith('linux'):
        kinds = {'cpu': r'ubuntu-x64\.tar\.gz', 'vulkan': r'ubuntu-vulkan-x64\.tar\.gz',
                 'cuda': r'ubuntu-cuda-12\.\d+-x64\.tar\.gz'}
    else:
        sys.exit('error: Windows or Linux only')
    pat = re.compile(r'^llama-b\d+-bin-' + kinds[backend] + '$')
    # (The newest build that has one: "latest" can be a tag without builds.)
    for r in releases:
        for a in r['assets']:
            if pat.match(a['name']):
                return r, a
    sys.exit(f'error: no {backend} build for this system in llama.cpp\'s recent releases')


def pick_device(server):
    """The dedicated graphics card llama-server lists ("Vulkan0: NVIDIA ..."), or None."""
    import subprocess
    try:
        out = subprocess.run([server, '--list-devices'], capture_output=True, text=True, timeout=60,
                             cwd=os.path.dirname(server)).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    devices = [l.strip() for l in out.splitlines() if re.match(r'\s*(Vulkan|CUDA)\d+:', l)]
    integrated = re.compile(r'Radeon\(TM\) Graphics|Intel\(R\) (UHD|Iris|HD) Graphics|Vega \d+ Graphics|llvmpipe', re.I)
    dedicated = [d for d in devices if not integrated.search(d)]
    return (dedicated or devices or [None])[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dir', default='llm', help='where to install (default: llm)')
    ap.add_argument('--backend', default='auto', choices=['auto', 'vulkan', 'cpu', 'cuda'])
    ap.add_argument('--repo', default='unsloth/Qwen3.5-4B-GGUF', help='the model on Hugging Face')
    ap.add_argument('--quant', default='Q4_K_M')
    ap.add_argument('--mmproj', default='mmproj-F16.gguf', help='the vision projector (empty: no vision)')
    a = ap.parse_args()
    out = os.path.abspath(a.dir)
    os.makedirs(os.path.join(out, 'models'), exist_ok=True)

    # 1. llama.cpp: the latest release's build for this system.
    backend = pick_backend(a.backend)
    releases = get_json('https://api.github.com/repos/ggml-org/llama.cpp/releases?per_page=10')
    release, asset = llama_asset(releases, backend)
    llama = os.path.join(out, 'llama')
    stamp = os.path.join(llama, '.release')
    if not (os.path.exists(stamp) and open(stamp).read() == asset['name']):
        archive = os.path.join(out, asset['name'])
        download(asset['browser_download_url'], archive, asset['size'])
        shutil.rmtree(llama, ignore_errors=True)
        os.makedirs(llama)
        print(f'unpacking {asset["name"]}')
        safe_extract(archive, llama)
        os.remove(archive)
        with open(stamp, 'w') as f:
            f.write(asset['name'])
    exe = 'llama-server.exe' if sys.platform == 'win32' else 'llama-server'
    server = next((os.path.join(d, exe) for d, _, files in os.walk(llama) if exe in files), None)
    if not server:
        sys.exit(f'error: no {exe} in {asset["name"]}')
    if sys.platform != 'win32':
        os.chmod(server, os.stat(server).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    print(f'llama.cpp {release["tag_name"]} ({backend}): {os.path.relpath(server, out)}')

    # 2. The model and its vision projector, checked against their SHA-256.
    info = get_json(f'https://huggingface.co/api/models/{a.repo}?blobs=true')
    files = {s['rfilename']: s for s in info.get('siblings', [])}
    model = next((n for n in files if n.endswith(f'-{a.quant}.gguf') and 'mmproj' not in n), None)
    if not model:
        sys.exit(f'error: no {a.quant} model in {a.repo}')
    wanted = [model] + ([a.mmproj] if a.mmproj else [])
    for name in wanted:
        if name not in files:
            sys.exit(f'error: no {name} in {a.repo}')
        lfs = files[name].get('lfs') or {}
        download(f'https://huggingface.co/{a.repo}/resolve/main/{name}', os.path.join(out, 'models', name),
                 lfs.get('size') or files[name].get('size'), lfs.get('sha256'))

    # The graphics card to run it on: with an integrated one as well, llama.cpp
    # splits the model over both and the integrated one slows it down a lot.
    device = pick_device(server)
    if device:
        print(f'graphics card: {device}')

    # 3. What the server starts (paths relative to this folder).
    with open(os.path.join(out, 'bot_llm.ini'), 'w', newline='\n') as f:
        f.write('# The bots\' advisor (--bot-llm): written by tools/setup_bot_llm.py\n')
        f.write(f'server={os.path.relpath(server, out)}\n')
        f.write(f'model=models/{model}\n')
        if a.mmproj:
            f.write(f'mmproj=models/{a.mmproj}\n')
        f.write('# more llama-server options (-ngl 99: all of it on the GPU, when there is one)\n')
        f.write('args=-ngl 99' + (f' --device {device.split(":")[0]}' if device else '') + '\n')
    print(f'ok {out}')


if __name__ == '__main__':
    main()
